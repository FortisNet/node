#include "application.h"
#include "http_session.h"
#include "module_configs.h"
#include "native_config.h"
#include "service_composition.h"
#include "upstream_access.h"
#include "unit_test_support.h"
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using fortis::testing::Require;
namespace app = fortis::app;
namespace http = fortis::http;
namespace storage = fortis::storage;
namespace config = fortis::config;

void Transactions() {
  for (bool grouped : {false, true}) {
    for (auto durability : {storage::KvDurability::RollbackMarker, storage::KvDurability::CommitSlots}) {
      fortis::testing::TemporaryDirectory directory;
      storage::KvStore store(grouped, false, durability);
      Require(store.Open(directory.path()), "open durability mode");
      const char first[] = "one", second[] = "two";
      const storage::KvMutation initial[] = {{"a", first, 3, 0, false}, {"b", second, 3, 0, false}};
      Require(store.Write(initial, 2), "one atomic frame");
      const storage::KvExpectation expected{"a", first, 3};
      const storage::KvMutation change[] = {{"a", second, 3, 0, false}, {"b", nullptr, 0, 0, true}};
      Require(store.CompareAndWrite(expected, 1, change, 2) == storage::KvCompareStatus::Committed, "conditional transaction");
      Require(store.CompareAndWrite(expected, 1, initial, 2) == storage::KvCompareStatus::ValueMismatch, "refusal appends nothing");
      Require(store.Compact(), "compact committed state");
      store.Close();
      Require(store.Open(directory.path()), "recovery/replay after compaction");
      char value[8]{}; size_t size = 0;
      Require(store.Get("a", 1, value, sizeof(value), &size) && size == 3 && !memcmp(value, second, 3), "published value recovered");
      Require(store.GetStatus("b", 1, value, sizeof(value), &size) == storage::KvReadStatus::Missing, "erase recovered atomically");
    }
  }
}
struct Pair {
  int fd[2]{};
  Pair() { Require(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0, "socket pair"); }
  ~Pair() { for (int value : fd) if (value >= 0) close(value); }
  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;
};
void ParserOwnership() {
  Pair pair;
  constexpr char raw[] = "POST /component?x=1 HTTP/1.1\r\nHost: unit\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
  Require(write(pair.fd[0], raw, sizeof(raw) - 1) == sizeof(raw) - 1, "request bytes");
  http::ParsedRequest parsed;
  auto& request = parsed.get();
  Require(http::RequestParser::Read(pair.fd[1], &request) == http::HttpReadResult::Ok, "parse owned request");
  Require(!strcmp(request.method, "POST") && !strcmp(request.raw_target, "/component?x=1") && request.body_length == 2 && !memcmp(request.body, "{}", 2), "borrowed request views");
  Pair malformed;
  constexpr char bad[] = "GET / HTTP/1.1\r\nAuthorization: one\r\nAuthorization: two\r\n\r\n";
  Require(write(malformed.fd[0], bad, sizeof(bad) - 1) == sizeof(bad) - 1, "malformed bytes");
  http::ParsedRequest refused;
  Require(http::RequestParser::Read(malformed.fd[1], &refused.get()) == http::HttpReadResult::BadRequest, "partial request cleanup after refusal");
}
std::string Receive(int fd) {
  std::string result; char buffer[4096];
  for (;;) { const auto n = read(fd, buffer, sizeof(buffer)); if (n <= 0) break; result.append(buffer, static_cast<size_t>(n)); }
  return result;
}
void ResponsesAndReservations() {
  Pair pair;
  timespec started{}; clock_gettime(CLOCK_MONOTONIC, &started);
  const auto response = http::JsonResponse(200, "OK", "{\"ok\":true}");
  Require(http::ResponseWriter::Write(pair.fd[1], response, true, started), "HEAD response writer");
  shutdown(pair.fd[1], SHUT_WR);
  const auto head = Receive(pair.fd[0]);
  Require(head.find("HTTP/1.1 200 OK\r\n") == 0 && head.find("Content-Length: 11\r\n") != std::string::npos && head.substr(head.find("\r\n\r\n") + 4).empty(), "HEAD retains length without body");
  http::ResponseHandoff handoff;
  Require(handoff.Start(), "bounded writer owner");
  Pair reserved; uint64_t token = 0;
  struct Completion {
    std::atomic<size_t> count{0}; std::atomic<bool> cancelled{true};
    static void Done(void* context, bool cancelled) { auto& self = *static_cast<Completion*>(context); self.cancelled.store(cancelled); self.count.fetch_add(1); }
  } completed;
  Require(handoff.Reserve(reserved.fd[1], false, 5000, &token, Completion::Done, &completed), "reserve transfers socket");
  reserved.fd[1] = -1;
  char bytes[] = "copied";
  http::HttpResponse body{200, "OK", "text/plain", reinterpret_cast<uint8_t*>(bytes), 6};
  Require(handoff.Complete(token, body), "one completion");
  memset(bytes, 'x', 6);
  Require(!handoff.Complete(token, body), "duplicate completion refused");
  const auto answer = Receive(reserved.fd[0]);
  handoff.Stop();
  Require(answer.substr(answer.find("\r\n\r\n") + 4) == "copied" && completed.count.load() == 1 && !completed.cancelled.load(), "copied bytes and exactly-one terminal callback");
}
void NarrowConfiguration() {
  config::NativeConfig startup;
  startup.port = 9999;
  startup.coingecko_api_key_configured = true;
  snprintf(startup.coingecko_api_key, sizeof(startup.coingecko_api_key), "fixture-key");
  app::ModuleConfigs modules;
  app::BuildModuleConfigs(startup, &modules);
  fortis::providers::UpstreamAccess borrowed;
  Require(fortis::providers::CoinGeckoAccess(startup, &borrowed) && borrowed.value == startup.coingecko_api_key,
      "startup base view never creates temporary credential storage");
  startup.coingecko_api_key[0] = 'X';
  fortis::providers::UpstreamAccess access;
  Require(fortis::providers::CoinGeckoAccess(modules.providers, &access) && access.value && !strcmp(access.value, "fixture-key"), "module configuration owns credential bytes");
  Require(modules.probes.ai == &modules.ai, "composition dependency points to owned slice");
}
struct Actions final : app::ShutdownActions {
  std::vector<app::ShutdownStage> calls;
  void Execute(app::ShutdownStage stage) override { calls.push_back(stage); }
};
void ShutdownOrdering() {
  using Stage = app::ShutdownStage;
  for (unsigned flags = 0; flags < 8; ++flags) {
    Actions actions; app::ApplicationShutdown shutdown(actions);
    if (flags & 1) shutdown.WorkersStarting();
    if (flags & 2) shutdown.SamplerStarted();
    if (flags & 4) shutdown.SweepersStarting();
    shutdown.Fail();
    std::vector<Stage> expected{Stage::Listener};
    if (flags & 1) expected.push_back(Stage::Workers);
    if (flags & 2) expected.push_back(Stage::Sampler);
    if (flags & 4) expected.push_back(Stage::Sweepers);
    expected.insert(expected.end(), {Stage::Owners, Stage::Coordinator, Stage::Hooks, Stage::Handoff});
    Require(actions.calls == expected, "startup rollback follows attained phases");
    shutdown.Fail(); shutdown.Complete();
    Require(actions.calls == expected, "terminal shutdown cannot run twice");
  }
  Actions actions; app::ApplicationShutdown shutdown(actions);
  shutdown.Complete();
  Require(actions.calls == std::vector<Stage>({Stage::Listener, Stage::MarkShutdown, Stage::Health,
      Stage::Workers, Stage::Sampler, Stage::Attestation, Stage::Sweepers, Stage::Owners,
      Stage::Coordinator, Stage::Hooks, Stage::Handoff, Stage::Store}), "successful shutdown drains owners before handoff and store");
}

void ServiceConstruction() {
  using Value = std::unique_ptr<int>;
  using Result = fortis::base::Result<Value>;
  using Code = fortis::base::StatusCode;
  std::optional<Value> service;
  unsigned calls = 0;
  auto create = [&]() -> Result { ++calls; return std::make_unique<int>(7); };
  Require(app::detail::EmplaceService(service, create, false) == Code::kFailedPrecondition &&
          !service && calls == 0, "missing dependency never invokes the factory");
  Require(app::detail::EmplaceService(service, create).ok() && **service == 7 && calls == 1,
          "move-only service is constructed once");
  Require(app::detail::EmplaceService(service, create) == Code::kFailedPrecondition &&
          **service == 7 && calls == 1, "occupied service retains its identity without another factory call");
  service.reset();
  const auto failed = app::detail::EmplaceService(service, []() -> Result {
    return fortis::base::Status(Code::kUnavailable, "fixture failure");
  });
  Require(failed == Code::kUnavailable && !strcmp(failed.message(), "fixture failure") && !service,
          "factory status is preserved and failed construction publishes no service");
  bool caught = false;
  try {
    (void)app::detail::EmplaceService(service, []() -> Result { throw std::runtime_error("factory throw"); });
  } catch (const std::runtime_error&) { caught = true; }
  Require(caught && !service, "factory exceptions propagate without publishing a partial service");
  Require(app::detail::EmplaceService(service, create).ok() && **service == 7 && calls == 2,
          "failed construction leaves the slot reusable");
}

void ServiceFailureLogging() {
  struct Capture final : fortis::base::LogSink {
    fortis::base::LogSink* previous = fortis::base::SetLogSink(this);
    std::vector<std::string> lines;
    ~Capture() { fortis::base::SetLogSink(previous); }
    bool Write(const fortis::base::LogRecord& record) override {
      Require(record.level == fortis::base::LogLevel::kError && !strcmp(record.component, "app"),
              "composition failure keeps the error level and app component");
      lines.emplace_back(record.text, record.length);
      return true;
    }
  } capture;
  std::optional<std::unique_ptr<int>> service;
  auto create = []() -> fortis::base::Result<std::unique_ptr<int>> { return std::make_unique<int>(7); };
  Require(!app::detail::StartService(service, "service failed", create, false) &&
          capture.lines == std::vector<std::string>{"service failed"}, "one failure logs one unchanged message");
  Require(app::detail::StartService(service, "service failed", create) && **service == 7 &&
          capture.lines.size() == 1, "successful composition adds no failure log");
}

template <int Index>
struct RegisteredService {
  explicit RegisteredService(bool result) : succeeds(result) {}
  RegisteredService(const RegisteredService&) = delete;
  bool succeeds;
  bool Register(std::vector<int>& calls, unsigned& handoffs) {
    calls.push_back(Index); ++handoffs; return succeeds;
  }
};
void RegistrationOrdering() {
  std::optional<RegisteredService<1>> first(std::in_place, true);
  std::optional<RegisteredService<2>> absent;
  std::optional<RegisteredService<3>> third(std::in_place, false);
  std::optional<RegisteredService<4>> fourth(std::in_place, true);
  std::vector<int> calls;
  unsigned handoffs = 0;
  Require(!app::detail::RegisterServices(calls, handoffs, first, absent, third, fourth) &&
          calls == std::vector<int>({1, 3}) && handoffs == 2,
          "heterogeneous registration skips absent services and stops at the first failure");
  calls.clear(); handoffs = 0; third->succeeds = true;
  Require(app::detail::RegisterServices(calls, handoffs, first, absent, third, fourth) &&
          calls == std::vector<int>({1, 3, 4}) && handoffs == 3, "successful registration preserves argument order");
  Require(app::detail::RegisterServices(calls, handoffs) &&
          app::detail::RegisterServices(calls, handoffs, absent) && handoffs == 3,
          "empty and disabled groups register nothing and succeed");
}
static_assert(!std::is_copy_constructible<http::ParsedRequest>::value, "parsed storage keeps one lifetime");
static_assert(!std::is_copy_constructible<http::ResponseHandoff>::value, "reservation owner identity");
static_assert(!std::is_copy_constructible<app::ApplicationShutdown>::value, "shutdown state identity");
template <typename T, typename = void> struct HasProcessPort : std::false_type {};
template <typename T> struct HasProcessPort<T, std::void_t<decltype(std::declval<T>().port)>> : std::true_type {};
static_assert(!HasProcessPort<config::ProviderRuntimeConfig>::value, "provider owners cannot read listener ports");
static_assert(sizeof(config::ProviderRuntimeConfig) < sizeof(config::NativeConfig), "modules receive narrower value records");
}  // namespace
int main() try {
  Transactions(); ParserOwnership(); ResponsesAndReservations(); NarrowConfiguration(); ShutdownOrdering();
  ServiceConstruction(); ServiceFailureLogging(); RegistrationOrdering();
  puts("infrastructure components: 8 cases passed");
} catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
