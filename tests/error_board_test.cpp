#include "error_board.h"
#include "provider_diagnostics.h"
#include "log.h"
#include "unit_test_support.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <string>

namespace {
using fortis::operations::ErrorBoard;
using fortis::testing::Require;

class Clock final : public fortis::base::Clock {
 public:
  std::atomic<std::int64_t> now{1795000000000LL};
  std::int64_t NowMs() override { return now.load(); }
};

std::string document(ErrorBoard& board) {
  std::array<char, ErrorBoard::kJsonCapacity> bytes{};
  const auto length = board.RenderJson(bytes.data(), bytes.size());
  Require(length != 0, "error snapshot fits its documented bound");
  return {bytes.data(), length};
}

void PersistenceAndPrivacy() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ErrorBoard board(fixture.kv, &clock);
  Require(board.Load(), "empty board loads");
  const auto frames = fixture.kv.WriteFrames();
  board.Report("prices", "client_identifier_missing", "FLR");
  clock.now.fetch_add(1);
  board.Report("prices", "client_identifier_missing", "FLR");
  board.Report("ai", "rate_limited");
  board.Report("ai", "raw upstream text", "sk-secret\nnever-public");
  board.Report("ai", "failed", "<script>never-public</script>");
  Require(fixture.kv.WriteFrames() == frames, "reporting does no storage I/O");
  auto pending = document(board);
  Require(pending.find("\"persisted\":false") != std::string::npos &&
          pending.find("\"count\":2") != std::string::npos, "summaries aggregate in memory");
  Require(pending.find("never-public") == std::string::npos, "invalid public fields are rejected");
  Require(board.Flush() && fixture.kv.WriteFrames() == frames + 1, "one frame commits all dirty summaries");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load(), "persistent summaries reload");
  const auto saved = document(restored);
  Require(saved.find("\"persisted\":true") != std::string::npos &&
          saved.find("\"identifier\":\"FLR\",\"count\":2") != std::string::npos &&
          saved.find("rate_limited") != std::string::npos, "counts and public identifiers survive restart");
  char tiny[4];
  Require(board.RenderJson(tiny, sizeof(tiny)) == 0, "short output fails rather than publishing partial JSON");
}

void PersistenceFailureAndRetry() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ErrorBoard board(fixture.kv, &clock);
  board.Report("ai", "unavailable");
  fixture.kv.Close();
  Require(!board.Flush(), "closed storage fails the flush");
  const auto failed = document(board);
  Require(failed.find("\"writeFailed\":true") != std::string::npos &&
          failed.find("unavailable") != std::string::npos, "storage failure remains publicly visible in memory");
  Require(fixture.kv.Open(fixture.directory.path()), "storage reopens");
  Require(board.Flush(), "dirty summaries can be retried");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load() && document(restored).find("unavailable") != std::string::npos,
          "retry persists the original event");
}

void CorruptionAndBounds() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  const char bad[] = "1|ai|failed|<script>|1|1|2";
  const fortis::storage::KvMutation write{"ops/error/v1/0", bad, sizeof(bad) - 1, 0, false};
  Require(fixture.kv.Write(&write, 1), "seed corrupt metadata");
  ErrorBoard board(fixture.kv, &clock);
  Require(board.Load(), "malformed metadata does not disable the node");
  Require(document(board).find("invalid_error_record") != std::string::npos &&
          document(board).find("<script>") == std::string::npos, "corruption is classified without exposing its contents");
  for (std::size_t index = 0; index < ErrorBoard::kCapacity + 20; ++index) {
    clock.now.fetch_add(1);
    board.Report("prices", "quote_unavailable", "coin-" + std::to_string(index));
  }
  const auto bounded = document(board);
  std::size_t rows = 0;
  for (auto at = bounded.find("\"service\":"); at != std::string::npos; at = bounded.find("\"service\":", at + 1)) ++rows;
  Require(rows == ErrorBoard::kCapacity, "the board keeps its fixed memory/persistence bound");
  Require(board.Flush(), "bounded summaries persist");
}

void ConcurrentReportsAndFlush() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ErrorBoard board(fixture.kv, &clock);
  std::array<std::future<void>, 4> workers;
  for (auto& worker : workers) {
    worker = std::async(std::launch::async, [&] {
      for (int index = 0; index < 1000; ++index) board.Report("ai", "rate_limited");
    });
  }
  for (int index = 0; index < 8; ++index) Require(board.Flush(), "concurrent flush");
  for (auto& worker : workers) worker.get();
  Require(board.Flush(), "final generation flush");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load() && document(restored).find("\"count\":4000") != std::string::npos,
          "concurrent generations do not lose reports during persistence");
}
void OriginsLifetimeAndStatus() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ErrorBoard board(fixture.kv, &clock);
  using fortis::base::ErrorOrigin;
  std::string provider = "coingecko";
  board.Report("providers", "http_rate_limited", {}, {ErrorOrigin::kThirdParty, provider, 429});
  provider.assign("changed");
  board.Report("providers", "http_rate_limited", {}, {ErrorOrigin::kThirdParty, "coingecko", 429});
  board.Report("providers", "http_rate_limited", {}, {ErrorOrigin::kThirdParty, "coingecko", 503});
  board.Report("providers", "http_rate_limited");
  board.Report("prices", "quote_unavailable", {}, {ErrorOrigin::kUnclassified});
  const auto frames = fixture.kv.WriteFrames();
  board.Report("providers", "bad", {}, {ErrorOrigin::kThirdParty, "", 429});
  board.Report("providers", "bad", {}, {ErrorOrigin::kThirdParty, "bad/private?key=secret", 429});
  board.Report("providers", "bad", {}, {ErrorOrigin::kThirdParty, "coingecko", 600});
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): intentional non-enumerator in a fixed uint8_t enum; verifies metadata rejection.
  board.Report("providers", "bad", {}, {static_cast<ErrorOrigin>(255)});
  const auto json = document(board);
  Require(json.find("changed") == std::string::npos && json.find("secret") == std::string::npos &&
          json.find("bad") == std::string::npos, "borrowed labels copied and invalid metadata refused");
  Require(json.find("\"origin\":\"third_party\",\"provider\":\"coingecko\",\"httpStatus\":429") != std::string::npos &&
          json.find("\"count\":2") != std::string::npos && json.find("\"httpStatus\":503") != std::string::npos &&
          json.find("\"origin\":\"node\"") != std::string::npos &&
          json.find("\"origin\":\"unclassified\"") != std::string::npos, "origin, provider and status separate aggregation keys");
  Require(fixture.kv.WriteFrames() == frames && board.Flush(), "report remains memory-only");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load() && document(restored) == document(board), "typed incidents survive restart");
}

void LegacyUnknownAndSaturatedCounters() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  const char old[] = "1|prices|quote_unavailable|bitcoin|7|1|2";
  const char saturated[] = "2|providers|http_rate_limited||18446744073709551615|1|2|third_party|coingecko|429";
  const fortis::storage::KvMutation seeds[] = {
      {"ops/error/v1/0", old, sizeof(old) - 1, 0, false},
      {"ops/error/v1/1", saturated, sizeof(saturated) - 1, 0, false}};
  Require(fixture.kv.Write(seeds, 2), "seed historical and saturated records");
  ErrorBoard board(fixture.kv, &clock);
  Require(board.Load(), "validated record versions load");
  Require(document(board).find("\"origin\":\"unclassified\"") != std::string::npos,
          "historical records are not attributed without evidence");
  board.Report("providers", "http_rate_limited", {}, {fortis::base::ErrorOrigin::kThirdParty, "coingecko", 429});
  Require(document(board).find("18446744073709551615") != std::string::npos && board.Flush(),
          "counter saturates and historical evidence upgrades without loss");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load() && document(restored) == document(board), "upgraded evidence stays unclassified");
}

void LegacyRollbackProjection() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ErrorBoard board(fixture.kv, &clock);
  board.Report("providers", "http_rate_limited", {}, {fortis::base::ErrorOrigin::kThirdParty, "coingecko", 429});
  const auto before = fixture.kv.WriteFrames();
  Require(board.Flush() && fixture.kv.WriteFrames() == before + 1, "base and typed sidecar commit in one frame");
  char legacy[384], typed[384]; std::size_t base_size = 0, typed_size = 0;
  Require(fixture.kv.Get("ops/error/v1/0", clock.NowMs(), legacy, sizeof(legacy), &base_size), "base record is retained");
  Require(base_size > 2 && legacy[0] == '1', "fallback sees the exact seven-field legacy format");
  Require(fixture.kv.Get("ops/error-meta/v2/0", clock.NowMs(), typed, sizeof(typed), &typed_size), "typed provenance has a separate prefix");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load() && document(restored).find("third_party") != std::string::npos, "matching sidecar restores classification");
  const char replacement[] = "1|ai|unavailable||3|1|2";
  const fortis::storage::KvMutation changed{"ops/error/v1/0", replacement, sizeof(replacement) - 1, 0, false};
  Require(fixture.kv.Write(&changed, 1), "simulate an older release reusing a slot");
  ErrorBoard upgraded(fixture.kv, &clock);
  Require(upgraded.Load(), "rollback-written records remain readable");
  const auto result = document(upgraded);
  Require(result.find("unclassified") != std::string::npos && result.find("coingecko") == std::string::npos,
          "stale provenance is not applied to a different legacy record");
  Require(upgraded.Flush(), "new base and unclassified sidecar are repaired atomically");
}

void OriginRetentionAndMaximumFields() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ErrorBoard board(fixture.kv, &clock);
  board.Report("storage", "critical_write_failed");
  for (std::size_t i = 0; i < ErrorBoard::kCapacity * 2; ++i) {
    clock.now.fetch_add(1);
    board.Report("providers", "http_rate_limited", "coin-" + std::to_string(i),
        {fortis::base::ErrorOrigin::kThirdParty, "coingecko", 429});
  }
  Require(document(board).find("critical_write_failed") != std::string::npos,
          "dependency incident churn cannot erase the node category");
  for (std::size_t i = 0; i < ErrorBoard::kCapacity; ++i) {
    const std::string identifier = std::string(97, 'a') + std::to_string(100 + i);
    board.Report(std::string(24, 's'), std::string(48, 'c'), identifier,
        {fortis::base::ErrorOrigin::kThirdParty, std::string(32, 'p'), 599});
  }
  Require(!document(board).empty() && board.Flush(), "maximum-length fields fit JSON and persistence bounds");
  ErrorBoard restored(fixture.kv, &clock);
  Require(restored.Load() && document(restored) == document(board), "full typed board survives restart");
}

void TypedConsoleSummaries() {
  struct Logs final : fortis::base::LogSink {
    std::string text;
    bool Write(const fortis::base::LogRecord& record) override {
      text.append(record.text, record.length); text += '\n'; return true;
    }
  } logs;
  auto* previous = fortis::base::SetLogSink(&logs);
  const auto restore = [previous](fortis::base::LogSink*) noexcept { fortis::base::SetLogSink(previous); };
  std::unique_ptr<fortis::base::LogSink, decltype(restore)> scope(&logs, restore);
  fortis::testing::TemporaryKv fixture;
  ErrorBoard board(fixture.kv);
  board.Report("storage", "write_failed");
  board.Report("providers", "http_rate_limited", "PUBLICCOIN",
      {fortis::base::ErrorOrigin::kThirdParty, "coingecko", 429});
  Require(logs.text.empty(), "reporting does no synchronous console I/O");
  Require(board.Flush(), "console summaries follow successful async persistence");
  Require(logs.text.find("origin=node service=storage") != std::string::npos &&
          logs.text.find("origin=third_party service=providers provider=coingecko") != std::string::npos &&
          logs.text.find("http_status=429 total=1") != std::string::npos &&
          logs.text.find("PUBLICCOIN") == std::string::npos, "console origin tags exclude identifiers and payloads");
  const auto size = logs.text.size();
  Require(board.Flush() && logs.text.size() == size, "unchanged summaries are not repeatedly logged");
}

void CorruptTypedMetadata() {
  fortis::testing::TemporaryKv fixture;
  const char bad[] = "2|providers|http_rate_limited||1|1|2|third_party|coingecko|700";
  const fortis::storage::KvMutation write{"ops/error/v1/0", bad, sizeof(bad) - 1, 0, false};
  Require(fixture.kv.Write(&write, 1), "seed invalid typed metadata");
  ErrorBoard board(fixture.kv);
  Require(board.Load(), "invalid metadata does not disable service");
  const auto json = document(board);
  Require(json.find("invalid_error_record") != std::string::npos &&
          json.find("http_rate_limited") == std::string::npos, "invalid stored status is not published");
}

void CoinIdentityEvidence() {
  using fortis::providers::CoinGeckoProfileId;
  Require(CoinGeckoProfileId("/api/v3/coins/bitcoin?localization=false") == "bitcoin", "exact profile slug");
  Require(CoinGeckoProfileId("/api/v3/coins/the-open-network") == "the-open-network", "exact profile without query");
  for (const auto path : {"/api/v3/coins/", "/api/v3/coins/list", "/api/v3/coins/ethereum/contract/0x123",
       "/api/v3/coins/bitcoin/market_chart", "/api/v3/simple/price?ids=bitcoin", "/api/v3/coins/%0asecret",
       "/api/v3/coins/Bitcoin", "/api/v3/coins/-bad"}) {
    Require(CoinGeckoProfileId(path).empty(), "unrelated or malformed path is not coin identity evidence");
  }
  fortis::testing::TemporaryKv fixture;
  ErrorBoard board(fixture.kv);
  board.Report("prices", "provider_rejected_coin_id", "obsolete-coin", {fortis::base::ErrorOrigin::kThirdParty, "coingecko", 404});
  board.Report("prices", "provider_rejected_coin_id", "bitcoin", {fortis::base::ErrorOrigin::kThirdParty, "coingecko", 404});
  Require(document(board).find("obsolete-coin") == std::string::npos, "unrecognised identifiers are not public telemetry");
  Require(document(board).find("bitcoin") != std::string::npos, "catalogued public identity remains visible");
  Require(board.Flush(), "identity rejection persists");
  ErrorBoard restored(fixture.kv);
  Require(restored.Load() && document(restored).find("provider_rejected_coin_id") != std::string::npos,
          "identity evidence survives restart");
}

void DependencyClassification() {
  using namespace fortis;
  providers::ProviderExecutionResult result{};
  result.attempts = 1;
  result.response.status = 429;
  auto failure = providers::DescribeProviderFailure("coingecko", result);
  Require(failure.code && std::string_view(failure.code) == "http_rate_limited" &&
          failure.details.origin == base::ErrorOrigin::kThirdParty && failure.details.http_status == 429,
          "actual upstream 429 is explicitly third-party");
  for (int status : {400, 401, 403, 404, 500, 502, 503}) {
    result.response.status = status;
    failure = providers::DescribeProviderFailure("coingecko", result);
    Require(failure.code && failure.details.http_status == status &&
            failure.details.origin == base::ErrorOrigin::kThirdParty, "upstream refusal/status preserved");
  }
  for (int status : {0, 200, 204, 301, 304, 600}) {
    result.response.status = status;
    Require(!providers::DescribeProviderFailure("coingecko", result).code, "success/redirect is not an error");
  }
  result.response.status = 0;
  for (auto error : {http::OutboundError::kResolve, http::OutboundError::kTimeout,
                    http::OutboundError::kConnect, http::OutboundError::kTls,
                    http::OutboundError::kHttp, http::OutboundError::kTooLarge,
                    http::OutboundError::kUnsupportedFraming}) {
    result.response.error = error;
    failure = providers::DescribeProviderFailure("rpc:private-host-label", result);
    Require(failure.code && failure.details.origin == base::ErrorOrigin::kUnclassified &&
            failure.details.provider == "blockchain_rpc" && !failure.details.http_status,
            "communication failure preserves uncertainty and redacts dynamic lane suffixes");
  }
  result.response.error = http::OutboundError::kInvalidRequest;
  Require(providers::DescribeProviderFailure("coingecko", result).details.origin == base::ErrorOrigin::kNode,
          "local request refusal is a node failure");
  result.cancelled = true;
  Require(!providers::DescribeProviderFailure("coingecko", result).code, "shutdown/caller cancellation is not a provider error");
  result.cancelled = false; result.attempts = 0;
  Require(!providers::DescribeProviderFailure("coingecko", result).code, "unattempted local admission is not a provider error");
  result.attempts = 1; result.lane_rejected = true;
  Require(!providers::DescribeProviderFailure("coingecko", result).code, "lane rejection is not an upstream response");
}

}  // namespace

int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("durable summaries, nonblocking reporting and public-field validation", PersistenceAndPrivacy);
  suite.Run("persistence failure remains visible and retries", PersistenceFailureAndRetry);
  suite.Run("corrupt metadata and fixed capacity", CorruptionAndBounds);
  suite.Run("rollback-readable atomic projection", LegacyRollbackProjection);
  suite.Run("concurrent reporting and atomic persistence", ConcurrentReportsAndFlush);
  suite.Run("typed origins, status isolation and borrowed label lifetimes", OriginsLifetimeAndStatus);
  suite.Run("historical uncertainty and saturating counters", LegacyUnknownAndSaturatedCounters);
  suite.Run("origin retention and maximum field bounds", OriginRetentionAndMaximumFields);
  suite.Run("typed console summaries without payloads or polling spam", TypedConsoleSummaries);
  suite.Run("malformed typed persistence metadata", CorruptTypedMetadata);
  suite.Run("read-only incorrect coin identity evidence", CoinIdentityEvidence);
  suite.Run("upstream versus local/cancelled dependency classification", DependencyClassification);
  return suite.Finish("error_board_test");
}
