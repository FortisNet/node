#include "discovery_cache.h"
#include "fake_seams.h"
#include "token_swr.h"
#include "unit_test_support.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace d = fortis::discovery;
namespace swr = fortis::tokens::token_swr;
namespace tj = fortis::codec::tj;
namespace testing = fortis::testing;
using testing::Require;
constexpr int64_t kNow = 1'800'000'000'000LL;

void discovery_expiry_and_restart() {
  testing::TemporaryKv fixture;
  testing::ManualClock clock(kNow);
  testing::FixedJitter jitter;
  d::DiscoveryCacheSeams seams{&clock, &jitter};
  unsigned char scratch[1024];
  d::DiscoveryCache cache(&fixture.kv, scratch, sizeof(scratch), &seams);
  Require(cache.Now() == kNow, "injected wall clock");
  Require(cache.Read("quote", clock.NowMs()).state == d::DiscoveryCacheState::kMissing, "cold miss");
  constexpr char body[] = "{\"price\":42}";
  int64_t as_of = 0;
  Require(cache.Write("quote", {1000, 2000}, clock.NowMs(), body, sizeof(body) - 1, &as_of), "write envelope");
  Require(as_of == kNow, "write provenance");
  auto value = cache.Read("quote", kNow + 999);
  Require(value.state == d::DiscoveryCacheState::kFresh && value.as_of_ms == kNow &&
              value.body_length == sizeof(body) - 1 && !memcmp(value.body, body, value.body_length),
          "fresh body and provenance");
  Require(cache.Read("quote", kNow + 1000).state == d::DiscoveryCacheState::kStale, "fresh boundary is exclusive");
  Require(cache.Read("quote", kNow + 2999).state == d::DiscoveryCacheState::kStale, "last stale millisecond");
  cache.RecordMissFailure("failure", kNow, false, 502);
  cache.RecordRevalidateFailure("quote", kNow, false);
  fixture.kv.Close();
  Require(fixture.kv.Open(fixture.directory.path()), "reopen durable cache");
  d::DiscoveryCache restarted(&fixture.kv, scratch, sizeof(scratch), &seams);
  value = restarted.Read("quote", kNow + 999);
  Require(value.state == d::DiscoveryCacheState::kFresh && value.as_of_ms == kNow, "envelope survives restart");
  Require(restarted.AllowRevalidate("quote", kNow) && !restarted.MissReplay("failure", kNow, nullptr),
          "process-local failure windows do not survive restart");
  Require(restarted.Read("quote", kNow + 3000).state == d::DiscoveryCacheState::kMissing, "stale boundary is exclusive");
}

void discovery_bounds_and_corruption() {
  testing::TemporaryKv fixture;
  testing::FixedJitter jitter;
  d::DiscoveryCacheSeams seams{nullptr, &jitter};
  unsigned char scratch[128];
  d::DiscoveryCache cache(&fixture.kv, scratch, sizeof(scratch), &seams);
  constexpr char body[] = "old";
  Require(cache.Write("value", {1000, 1000}, kNow, body, 3, nullptr), "seed retained value");
  unsigned char encoded[128];
  size_t size = 0;
  Require(fixture.kv.Get("value", kNow, encoded, sizeof(encoded), &size), "read opaque envelope");
  const uint64_t frames = fixture.kv.WriteFrames();
  for (size_t capacity : {size_t{0}, size_t{1}, size - 1}) {
    std::vector<unsigned char> guarded(size + 2, 0xa5);
    d::DiscoveryCache small(&fixture.kv, guarded.data() + 1, capacity, &seams);
    Require(!small.Write("value", {1000, 1000}, kNow, "new", 3, nullptr), "undersized scratch refuses write");
    for (unsigned char byte : guarded) Require(byte == 0xa5, "refusal does not touch scratch or its guards");
    Require(fixture.kv.WriteFrames() == frames, "refusal publishes no storage frame");
  }
  std::vector<unsigned char> guarded(size + 2, 0xa5);
  d::DiscoveryCache exact(&fixture.kv, guarded.data() + 1, size, &seams);
  Require(exact.Write("value", {1000, 1000}, kNow, body, 3, nullptr), "exact fit accepts envelope");
  Require(guarded.front() == 0xa5 && guarded.back() == 0xa5, "exact fit preserves guards");
  auto read = cache.Read("value", kNow);
  Require(read.body_length == 3 && !memcmp(read.body, body, 3), "rejected replacement preserves old value");
  // Re-store a borrowed read body into the same scratch (overlapping copy).
  Require(cache.Write("copy", {1000, 1000}, kNow, read.body, read.body_length, nullptr), "borrowed body can be re-stored");
  read = cache.Read("copy", kNow);
  Require(read.body_length == 3 && !memcmp(read.body, body, 3), "overlap does not corrupt payload");
  Require(!cache.Write("null", {1000, 1000}, kNow, nullptr, 1, nullptr), "non-empty body needs storage");
  Require(cache.Write("empty", {1000, 1000}, kNow, nullptr, 0, nullptr), "empty body needs no source pointer");
  Require(cache.Read("empty", kNow).state == d::DiscoveryCacheState::kFresh &&
              cache.Read("empty", kNow).body_length == 0, "empty envelope is a hit");
  const fortis::storage::KvMutation broken{"value", encoded, static_cast<uint32_t>(size - 1), 0, false};
  Require(fixture.kv.Write(&broken, 1), "seed truncated cache record");
  Require(cache.Read("value", kNow).state == d::DiscoveryCacheState::kError, "corrupt envelope is an error, not a hit");
  fixture.kv.Close();
  Require(!cache.Write("copy", {1000, 1000}, kNow, body, 3, nullptr), "closed KV write reports failure");
  Require(cache.Read("copy", kNow).state == d::DiscoveryCacheState::kError, "closed KV read reports error");
}

void jitter_and_retention() {
  testing::TemporaryKv fixture;
  for (const auto sample : {0.0, 0.5, 0.999}) {
    testing::FixedJitter jitter(sample);
    d::DiscoveryCacheSeams seams{nullptr, &jitter};
    unsigned char scratch[128];
    d::DiscoveryCache cache(&fixture.kv, scratch, sizeof(scratch), &seams);
    const int64_t expected = sample == 0 ? 900 : sample == 0.5 ? 1000 : 1100;
    Require(cache.Write("jitter", {1000, 1000}, kNow, "x", 1, nullptr), "jittered write");
    const auto read = cache.Read("jitter", kNow);
    Require(read.fresh_until_ms == kNow + expected && read.stale_until_ms == kNow + expected + 1000,
            "injected jitter controls freshness without altering stale duration");
  }
  testing::FixedJitter jitter;
  d::DiscoveryCacheSeams seams{nullptr, &jitter};
  unsigned char scratch[128];
  d::DiscoveryCache cache(&fixture.kv, scratch, sizeof(scratch), &seams);
  Require(cache.Write("long", {14LL * 86400000, 0}, kNow, "x", 1, nullptr), "long TTL write");
  Require(cache.Read("long", kNow + 7LL * 86400000 - 1).state == d::DiscoveryCacheState::kFresh,
          "long envelope remains readable up to retention cap");
  Require(cache.Read("long", kNow + 7LL * 86400000).state == d::DiscoveryCacheState::kMissing,
          "KV retention caps even an envelope still declaring freshness");
}

void token_values_and_invalidation() {
  testing::TemporaryKv fixture;
  testing::ManualClock clock(kNow);
  testing::FixedJitter jitter;
  swr::Seams seams{&clock, &jitter};
  swr::Cache cache(&fixture.kv, &seams);
  swr::CacheClock view(&cache);
  Require(view.NowMs() == kNow, "cache clock delegates to injected clock");
  testing::JsonArena arena;
  auto* original = tj::Obj(arena.get());
  Require(original && tj::Set(arena.get(), original, "price", tj::Num(arena.get(), 42)), "build value");
  Require(tj::Set(arena.get(), original, "symbol", tj::Str(arena.get(), "FORTIS")), "build string value");
  uint8_t scratch[512];
  int64_t as_of = 0;
  Require(cache.Write("token", {1000, 2000}, kNow, original, scratch, sizeof(scratch), &as_of), "write token envelope");
  Require(tj::Set(arena.get(), original, "price", tj::Num(arena.get(), 99)), "mutate caller's value");
  tj::Value* value = nullptr;
  Require(cache.Read("token", kNow + 999, scratch, sizeof(scratch), arena.get(), &value, &as_of) == swr::State::kFresh &&
              tj::Get(value, "price")->number == 42 && as_of == kNow,
          "stored value is independent of input arena mutations");
  Require(cache.Read("token", kNow + 1000, scratch, sizeof(scratch), arena.get(), &value, &as_of) == swr::State::kStale,
          "token freshness boundary");
  // A decoded result must not borrow the reusable byte scratch.
  memset(scratch, 0, sizeof(scratch));
  Require(tj::Get(value, "price")->number == 42 && tj::StringEquals(tj::Get(value, "symbol"), "FORTIS"),
          "decoded value owns its string/document storage");
  cache.HoldMiss("token", kNow, false, 503);
  cache.EndRevalidate("token", kNow, true, false);
  Require(cache.Invalidate("token"), "invalidate stored value and failure windows");
  Require(!cache.MissHeld("token", kNow) && cache.BeginRevalidate("token", kNow), "invalidation clears both policies");
  cache.EndRevalidate("token", kNow, false, false);
  Require(cache.Read("token", kNow, scratch, sizeof(scratch), arena.get(), &value, &as_of) == swr::State::kMissing && !value,
          "invalidated entry cannot be served");
}

void token_single_flight_and_failures() {
  testing::TemporaryKv fixture;
  testing::ManualClock clock(kNow);
  testing::FixedJitter jitter;
  swr::Seams seams{&clock, &jitter};
  swr::Cache cache(&fixture.kv, &seams);
  std::atomic<unsigned> winners{0};
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> callers;
  try {
    for (unsigned i = 0; i < 16; ++i) callers.emplace_back([&] {
      ++ready;
      while (!start.load()) std::this_thread::yield();
      if (cache.BeginRevalidate("one-flight", clock.NowMs())) ++winners;
    });
  } catch (...) {
    start = true;
    for (auto& caller : callers) caller.join();
    throw;
  }
  while (ready.load() != callers.size()) std::this_thread::yield();
  start = true;
  for (auto& caller : callers) caller.join();
  Require(winners == 1, "exactly one caller owns revalidation until completion");
  cache.EndRevalidate("one-flight", kNow, true, true);
  Require(!cache.BeginRevalidate("one-flight", kNow + 4999), "busy failure backs off");
  Require(cache.BeginRevalidate("one-flight", kNow + 5000), "backoff expires at exact boundary");
  cache.EndRevalidate("one-flight", kNow + 5000, false, false);
  const std::string first(300, 'a');
  const std::string second = first + "b";
  cache.HoldMiss(first.c_str(), kNow, false, 503);
  int status = 0;
  Require(cache.MissHeld(first.c_str(), kNow + 9999, &status) && status == 503, "miss replays its own status");
  Require(!cache.MissHeld(second.c_str(), kNow, &status), "long keys with common prefix have independent policy");
  Require(!cache.MissHeld(first.c_str(), kNow + 10000), "miss hold expires at exact boundary");
  std::vector<std::string> active;
  for (unsigned i = 0; i < 256; ++i) {
    const std::string key = "active-" + std::to_string(i);
    if (!cache.BeginRevalidate(key.c_str(), kNow)) break;
    active.push_back(key);
  }
  Require(!active.empty() && active.size() < 256, "revalidation work is bounded");
  Require(!cache.BeginRevalidate(active.front().c_str(), kNow), "capacity refusal does not release an existing owner");
  cache.EndRevalidate(active.front().c_str(), kNow, false, false);
  Require(cache.BeginRevalidate("replacement", kNow), "completing work frees capacity");
  for (const auto& key : active) cache.EndRevalidate(key.c_str(), kNow, false, false);
  cache.EndRevalidate("replacement", kNow, false, false);
}

void token_reopen_corruption_and_write_failure() {
  testing::TemporaryKv fixture;
  testing::FixedJitter jitter;
  swr::Seams seams{nullptr, &jitter};
  testing::JsonArena arena;
  const auto* document = tj::Str(arena.get(), "persisted");
  uint8_t scratch[256];
  swr::Cache cache(&fixture.kv, &seams);
  Require(cache.Write("durable", {1000, 2000}, kNow, document, scratch, sizeof(scratch), nullptr), "write durable JSON");
  cache.HoldMiss("failed", kNow, false, 503);
  fixture.kv.Close();
  Require(!cache.Write("durable", {1000, 2000}, kNow, tj::Null(arena.get()), scratch, sizeof(scratch), nullptr),
          "KV failure is returned instead of publishing a replacement");
  Require(fixture.kv.Open(fixture.directory.path()), "reopen token KV");
  swr::Cache restarted(&fixture.kv, &seams);
  tj::Value* value = nullptr;
  int64_t as_of = 0;
  Require(restarted.Read("durable", kNow + 1000, scratch, sizeof(scratch), arena.get(), &value, &as_of) == swr::State::kStale &&
              tj::StringEquals(value, "persisted") && as_of == kNow, "old value survives failed replacement and restart");
  Require(!restarted.MissHeld("failed", kNow), "token holds are process-local");
  Require(restarted.Read("durable", kNow + 3000, scratch, sizeof(scratch), arena.get(), &value, &as_of) == swr::State::kMissing && !value,
          "token stale boundary is exclusive");
  Require(cache.Write("corrupt", {1000, 1000}, kNow, document, scratch, sizeof(scratch), nullptr), "seed corruption case");
  size_t size = 0;
  Require(fixture.kv.Get("corrupt", kNow, scratch, sizeof(scratch), &size) && size > 0, "read stored envelope");
  scratch[size - 1] = '!';  // valid KV frame, invalid JSON payload of unchanged length
  const fortis::storage::KvMutation broken{"corrupt", scratch, static_cast<uint32_t>(size), 0, false};
  Require(fixture.kv.Write(&broken, 1), "store invalid JSON envelope");
  Require(cache.Read("corrupt", kNow, scratch, sizeof(scratch), arena.get(), &value, &as_of) == swr::State::kMissing && !value,
          "unparseable cache JSON is a miss, never a partial value");
  const uint64_t frames = fixture.kv.WriteFrames();
  Require(!cache.Write("short", {1000, 1000}, kNow, document, scratch, 1, nullptr) && fixture.kv.WriteFrames() == frames,
          "undersized token scratch cannot write storage");
}

}  // namespace

int main() {
  testing::UnitSuite suite;
  suite.Run("discovery expiry/restart", discovery_expiry_and_restart);
  suite.Run("discovery bounds/corruption", discovery_bounds_and_corruption);
  suite.Run("jitter/retention", jitter_and_retention);
  suite.Run("token value ownership/invalidation", token_values_and_invalidation);
  suite.Run("token revalidation/failure isolation", token_single_flight_and_failures);
  suite.Run("token persistence/corruption/failures", token_reopen_corruption_and_write_failure);
  return suite.Finish("cache_contract_test");
}
