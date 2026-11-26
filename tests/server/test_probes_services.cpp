// Test probes of the AI, cross-chain, Robinhood, DashPay, operations and
// networks surfaces. Each section compiles only with its own flag.
#include "test_probes.h"

#if defined(FORTIS_AI_ROUTE_TEST_ONLY) || defined(FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY) || \
    defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY) || defined(FORTIS_DASHPAY_ROUTE_TEST_ONLY) || \
    defined(FORTIS_OPERATIONS_TEST_ONLY) || defined(FORTIS_NETWORK_ROUTE_TEST_ONLY)
#include "app_support.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "outbound_https.h"
#include "service_wiring.h"
#include "test_hook_env.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif
#if defined(FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY) || defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DASHPAY_ROUTE_TEST_ONLY)
#include "chain_transport.h"
#endif
#ifdef FORTIS_ROBINHOOD_ROUTE_TEST_ONLY
#include <mbedtls/sha256.h>
#endif
#ifdef FORTIS_OPERATIONS_TEST_ONLY
#include "api_telemetry.h"
#include "crash_report_route.h"
#include "operations_routes.h"
#include "price_series_store.h"
#endif

// ---- AI conversations ---------------------------------------------
#ifdef FORTIS_AI_ROUTE_TEST_ONLY
namespace fortis::app {
namespace {

bool allow_ai_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// The AI routes' clock: FORTIS_TEST_AI_NOW_FILE lets one process move it
// between requests; otherwise the wall clock.
int64_t ai_test_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_AI_NOW_FILE");
  if (now_file && *now_file) {
    if (FILE* handle = fopen(now_file, "r")) {
      char buffer[32]{};
      const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
      fclose(handle);
      buffer[length] = '\0';
      char* end = nullptr;
      const long long parsed = strtoll(buffer, &end, 10);
      if (end && end != buffer && parsed >= 0 && (!*end || *end == '\n'))
        return static_cast<int64_t>(parsed);
    }
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// The contract fixture's ulid mock: "01JZ" + a 22-digit counter, continued
// across restarts through FORTIS_TEST_AI_ID_FILE.
pthread_mutex_t g_ai_test_id_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned long long g_ai_test_id_next = 0;
bool g_ai_test_id_loaded = false;
bool ai_test_new_id(char* output) {
  pthread_mutex_lock(&g_ai_test_id_mutex);
  const char* file = TestHookEnv("FORTIS_TEST_AI_ID_FILE");
  if (!g_ai_test_id_loaded) {
    if (file && *file) {
      if (FILE* handle = fopen(file, "r")) {
        if (fscanf(handle, "%llu", &g_ai_test_id_next) != 1) g_ai_test_id_next = 0;
        fclose(handle);
      }
    }
    g_ai_test_id_loaded = true;
  }
  const unsigned long long id = g_ai_test_id_next++;
  if (file && *file) {
    if (FILE* handle = fopen(file, "w")) {
      fprintf(handle, "%llu\n", g_ai_test_id_next);
      fflush(handle);
      fsync(fileno(handle));
      fclose(handle);
    }
  }
  pthread_mutex_unlock(&g_ai_test_id_mutex);
  snprintf(output, 27, "01JZ%022llu", id);
  return true;
}

TestFnClock<ai_test_now_ms> ai_test_clock;
TestFnIds<ai_test_new_id> ai_test_ids;

// The AI fixture devices: premium
// tier, the A and B tokens, the ids the signed proofs name.
bool seed_ai_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{};
  gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  char token_a[64], token_b[64];
  snprintf(token_a, sizeof(token_a), "fintel_dev_%s", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  snprintf(token_b, sizeof(token_b), "fintel_dev_%s", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
  return auth.SeedAddressRouteFixture(token_a, "01ARZ3NDEKTSV4RRFFQ69G5FAV", now_ms) &&
         auth.SeedAddressRouteFixture(token_b, "01ARZ3NDEKTSV4RRFFQ69G5FB2", now_ms);
}

// GET /probe/test-ai-idle: waits until no generation is queued or running.
void handle_ai_test_idle(int client, const http::HttpRequest&, bool head, void* opaque) {
  const bool idle = !opaque || static_cast<ai::AiConversationService*>(opaque)->WaitIdle(30000);
  respond(client, idle ? 200 : 503, idle ? "OK" : "Service Unavailable",
          idle ? "{\"ok\":true}" : "{\"error\":\"BUSY\"}", head);
}

bool ai_test_route = false;
char ai_upstream_port[8]{};
ai::AiServiceSeams ai_test_seams{};

}  // namespace

bool ConfigureAiTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* ai_flag = TestHookEnv("FORTIS_ENABLE_TEST_AI_ROUTES");
  ai_test_route = ai_flag && strcmp(ai_flag, "1") == 0;
  if (ai_test_route) {
    const int upstream_port = parse_port(TestHookEnv("FORTIS_TEST_AI_UPSTREAM_PORT"));
    if (!config.native_ai_canary || upstream_port < 0) {
      fprintf(stderr, "FORTIS_ENABLE_TEST_AI_ROUTES needs FORTIS_NATIVE_AI_CANARY=1 and "
                      "FORTIS_TEST_AI_UPSTREAM_PORT\n");
      return false;
    }
    snprintf(ai_upstream_port, sizeof(ai_upstream_port), "%d", upstream_port);
    ai_test_seams.clock = &ai_test_clock;
    ai_test_seams.new_id = &ai_test_ids;
    ai_test_seams.upstream_host = "localhost";
    ai_test_seams.upstream_port = ai_upstream_port;
    ai_test_seams.upstream_ca = config.ca_bundle_path;
    ai_test_seams.allow_address = allow_ai_test_address;
    const char* timeout = TestHookEnv("FORTIS_TEST_AI_PROVIDER_TIMEOUT_MS");
    if (timeout && *timeout) ai_test_seams.provider_timeout_ms = static_cast<uint32_t>(strtoul(timeout, nullptr, 10));
    options.ai_seams = &ai_test_seams;
  }
  return true;
}

bool SeedAiTest(fortis::storage::KvStore* kv) {
  if (ai_test_route && !seed_ai_test_devices(kv)) {
    fprintf(stderr, "Failed to seed test AI fixtures\n");
    return false;
  }
  return true;
}

bool RegisterAiTestRoutes(http::HttpRouter& router, Planes& planes) {
  return !ai_test_route || router.Add("GET", "/probe/test-ai-idle", handle_ai_test_idle, planes.ai ? &*planes.ai : nullptr);
}

void AiTestSweepClock(fortis::base::Clock** clock) {
  if (ai_test_route) *clock = &ai_test_clock;
}

}  // namespace fortis::app
#endif

// ---- cross-chain exchange -----------------------------------------
#ifdef FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY
namespace fortis::app {
namespace {

// The cross-chain clock: FORTIS_TEST_CROSS_CHAIN_NOW_FILE lets one process
// move it between requests (the fixture freezes Date per case); otherwise
// the wall clock.
class CrossChainTestClock final : public fortis::base::Clock {
 public:
  int64_t NowMs() override;
};
int64_t CrossChainTestClock::NowMs() {
  const char* now_file = TestHookEnv("FORTIS_TEST_CROSS_CHAIN_NOW_FILE");
  if (now_file && *now_file) {
    if (FILE* handle = fopen(now_file, "r")) {
      char buffer[32]{};
      const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
      fclose(handle);
      buffer[length] = '\0';
      char* end = nullptr;
      const long long value = strtoll(buffer, &end, 10);
      if (end != buffer && value > 0) return value;
    }
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

bool allow_cross_chain_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// Every provider host goes to one loopback upstream; each request names its
// real host in x-fortis-test-host.
struct CrossChainTestUpstream final : fortis::base::Starter<fortis::chains::Transport> {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(fortis::chains::Transport& transport) override {
    return transport.StartRedirected("localhost", port, ca, allow_cross_chain_test_address, nullptr,
                                     "x-fortis-test-host");
  }
};

// The cross-chain fixture devices: premium tier,
// one per letter A..D (the replay spreads requests over them).
bool seed_cross_chain_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{}; gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  static const char* kIds[] = {"01ARZ3NDEKTSV4RRFFQ69G5FAV", "01ARZ3NDEKTSV4RRFFQ69G5FB2",
                               "01ARZ3NDEKTSV4RRFFQ69G5FC3", "01ARZ3NDEKTSV4RRFFQ69G5FD4"};
  for (size_t i = 0; i < 4; ++i) {
    char token[64];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = static_cast<char>('A' + i);
    token[54] = '\0';
    if (!auth.SeedAddressRouteFixture(token, kIds[i], now_ms)) return false;
  }
  return true;
}

// GET /probe/test-cross-chain-idle: waits until no job is queued or running.
void handle_cross_chain_test_idle(int client, const http::HttpRequest&, bool head, void* opaque) {
  auto* service = static_cast<exchange::CrossChainService*>(opaque);
  const bool idle = service && service->WaitIdle(30000);
  WriteHttpResponse(client, http::JsonResponse(idle ? 200 : 503, idle ? "OK" : "Service Unavailable",
                                         idle ? "{\"idle\":true}" : "{\"idle\":false}"),
                    head);
}

bool cross_chain_test_route = false;
CrossChainTestUpstream cross_chain_test_upstream{};
exchange::CrossChainServiceSeams cross_chain_test_seams{};
CrossChainTestClock cross_chain_test_clock;

}  // namespace

bool ConfigureCrossChainTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* cross_chain_flag = TestHookEnv("FORTIS_ENABLE_TEST_CROSS_CHAIN_ROUTES");
  cross_chain_test_route = cross_chain_flag && strcmp(cross_chain_flag, "1") == 0;
  if (cross_chain_test_route) {
    const char* upstream_port = TestHookEnv("FORTIS_TEST_CROSS_CHAIN_UPSTREAM_PORT");
    const char* upstream_ca = TestHookEnv("FORTIS_TEST_CROSS_CHAIN_UPSTREAM_CA");
    if (!config.native_cross_chain_canary || parse_port(upstream_port) < 0 || !upstream_ca || !*upstream_ca ||
        !http::OutboundHttpsValidateCaBundle(upstream_ca)) {
      fprintf(stderr, "FORTIS_ENABLE_TEST_CROSS_CHAIN_ROUTES needs FORTIS_NATIVE_CROSS_CHAIN_CANARY=1, "
                      "FORTIS_TEST_CROSS_CHAIN_UPSTREAM_PORT and FORTIS_TEST_CROSS_CHAIN_UPSTREAM_CA\n");
      return false;
    }
    snprintf(cross_chain_test_upstream.port, sizeof(cross_chain_test_upstream.port), "%d", parse_port(upstream_port));
    cross_chain_test_upstream.ca = upstream_ca;
    cross_chain_test_seams.clock = &cross_chain_test_clock;
    cross_chain_test_seams.transport_starter = &cross_chain_test_upstream;
    const char* deadline = TestHookEnv("FORTIS_TEST_CROSS_CHAIN_DEADLINE_MS");
    if (deadline && *deadline) cross_chain_test_seams.provider_deadline_ms = static_cast<uint32_t>(strtoul(deadline, nullptr, 10));
    options.cross_chain_seams = &cross_chain_test_seams;
  }
  return true;
}

bool SeedCrossChainTest(fortis::storage::KvStore* kv) {
  if (cross_chain_test_route && !seed_cross_chain_test_devices(kv)) {
    fprintf(stderr, "Failed to seed test cross-chain fixtures\n");
    return false;
  }
  return true;
}

bool RegisterCrossChainTestRoutes(http::HttpRouter& router, Planes& planes) {
  return !cross_chain_test_route ||
         router.Add("GET", "/probe/test-cross-chain-idle", handle_cross_chain_test_idle, planes.cross_chain ? &*planes.cross_chain : nullptr);
}

}  // namespace fortis::app
#endif

// ---- Robinhood -----------------------------------------------------
#ifdef FORTIS_ROBINHOOD_ROUTE_TEST_ONLY
namespace fortis::app {
namespace {

// A millisecond instant or counter kept in a file the replay rewrites per
// case; `fallback` when the file is missing or unreadable.
long long robinhood_test_file_value(const char* file, long long fallback) {
  if (!file || !*file) return fallback;
  FILE* handle = fopen(file, "r");
  if (!handle) return fallback;
  char buffer[32]{};
  const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
  fclose(handle);
  buffer[length] = '\0';
  char* end = nullptr;
  const long long value = strtoll(buffer, &end, 10);
  return end != buffer && value >= 0 ? value : fallback;
}

// The Robinhood clock: FORTIS_TEST_ROBINHOOD_NOW_FILE (the fixture freezes
// Date per case); otherwise the wall clock.
class RobinhoodTestClock final : public fortis::base::Clock {
 public:
  int64_t NowMs() override;
};
int64_t RobinhoodTestClock::NowMs() {
  timeval now{};
  gettimeofday(&now, nullptr);
  const int64_t wall = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  const long long value = robinhood_test_file_value(TestHookEnv("FORTIS_TEST_ROBINHOOD_NOW_FILE"), 0);
  return value > 0 ? value : wall;
}

// The fixture's randomBytes(24) mock: sha256(`robinhood-ticket-${n}`)[0:24],
// n read from FORTIS_TEST_ROBINHOOD_TICKET_FILE and written back as n + 1.
pthread_mutex_t g_robinhood_ticket_mutex = PTHREAD_MUTEX_INITIALIZER;
class RobinhoodTestTickets final : public fortis::base::RandomSource {
 public:
  bool Bytes(uint8_t* output, size_t size) override;
};
bool RobinhoodTestTickets::Bytes(uint8_t* output, size_t size) {
  if (size != 24) return false;
  const char* file = TestHookEnv("FORTIS_TEST_ROBINHOOD_TICKET_FILE");
  pthread_mutex_lock(&g_robinhood_ticket_mutex);
  const long long n = robinhood_test_file_value(file, 0);
  if (file && *file) {
    if (FILE* handle = fopen(file, "w")) {
      fprintf(handle, "%lld\n", n + 1);
      fclose(handle);
    }
  }
  pthread_mutex_unlock(&g_robinhood_ticket_mutex);
  char seed[64];
  const int length = snprintf(seed, sizeof(seed), "robinhood-ticket-%lld", n);
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(seed), static_cast<size_t>(length), digest, 0);
  memcpy(output, digest, 24);
  return true;
}

bool allow_robinhood_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// Every upstream host goes to one loopback fixture; each request names its
// real host in x-fortis-test-host.
struct RobinhoodTestUpstream final : fortis::base::Starter<fortis::chains::Transport> {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(fortis::chains::Transport& transport) override {
    return transport.StartRedirected("localhost", port, ca, allow_robinhood_test_address, nullptr,
                                     "x-fortis-test-host");
  }
};

// The Robinhood fixture devices: premium tier, one
// per letter A..H, plus I..T for the native ticket-cap test (1,000 prepares
// within one wall-clock quota minute).
bool seed_robinhood_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{};
  gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  static const char* kIds[] = {"01ARZ3NDEKTSV4RRFFQ69G5FAV", "01ARZ3NDEKTSV4RRFFQ69G5FB2", "01ARZ3NDEKTSV4RRFFQ69G5FC3",
                               "01ARZ3NDEKTSV4RRFFQ69G5FD4", "01ARZ3NDEKTSV4RRFFQ69G5FE5", "01ARZ3NDEKTSV4RRFFQ69G5FF6",
                               "01ARZ3NDEKTSV4RRFFQ69G5FG7", "01ARZ3NDEKTSV4RRFFQ69G5FH8"};
  static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  for (size_t i = 0; i < 20; ++i) {
    char token[64], id[27];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = static_cast<char>('A' + i);
    token[54] = '\0';
    if (i < 8) snprintf(id, sizeof(id), "%s", kIds[i]);
    else snprintf(id, sizeof(id), "01ARZ3NDEKTSV4RRFFQ69G5G%cZ", kCrockford[i]);
    if (!auth.SeedAddressRouteFixture(token, id, now_ms)) return false;
  }
  // The R10 edge devices (robinhood_capabilities_edges.json):
  // a–d valid development devices, e expired one second ago, r revoked.
  static const char* kEdgeIds[] = {"01ARZ3NDEKTSV4RRFFQ69G5HA1", "01ARZ3NDEKTSV4RRFFQ69G5HB2",
                                   "01ARZ3NDEKTSV4RRFFQ69G5HC3", "01ARZ3NDEKTSV4RRFFQ69G5HD4",
                                   "01ARZ3NDEKTSV4RRFFQ69G5HE5", "01ARZ3NDEKTSV4RRFFQ69G5HR6"};
  static const char kEdgeLetters[] = "abcder";
  for (size_t i = 0; i < 6; ++i) {
    char token[55];
    memcpy(token, "fintel_dev_", 11);
    memset(token + 11, kEdgeLetters[i], 43);
    token[54] = '\0';
    const int64_t seeded_ms = kEdgeLetters[i] == 'e' ? now_ms - 3601000 : now_ms;
    if (!auth.SeedPriceRouteFixture(token, kEdgeIds[i], seeded_ms) ||
        (kEdgeLetters[i] == 'r' && !auth.Revoke(kEdgeIds[i], now_ms - 1000)))
      return false;
  }
  return true;
}

// GET /probe/test-robinhood-idle: waits until no job is queued or running.
void handle_robinhood_test_idle(int client, const http::HttpRequest&, bool head, void* opaque) {
  const bool idle = static_cast<exchange::RobinhoodService*>(opaque)->WaitIdle(30000);
  WriteHttpResponse(client, http::JsonResponse(idle ? 200 : 503, idle ? "OK" : "Service Unavailable",
                                         idle ? "{\"idle\":true}" : "{\"idle\":false}"),
                    head);
}

bool robinhood_test_route = false;
RobinhoodTestUpstream robinhood_test_upstream{};
exchange::RobinhoodServiceSeams robinhood_test_seams{};
RobinhoodTestClock robinhood_test_clock;
RobinhoodTestTickets robinhood_test_tickets;

}  // namespace

bool ConfigureRobinhoodTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* robinhood_flag = TestHookEnv("FORTIS_ENABLE_TEST_ROBINHOOD_ROUTES");
  robinhood_test_route = robinhood_flag && strcmp(robinhood_flag, "1") == 0;
  if (robinhood_test_route) {
    const char* upstream_port = TestHookEnv("FORTIS_TEST_ROBINHOOD_UPSTREAM_PORT");
    const char* upstream_ca = TestHookEnv("FORTIS_TEST_ROBINHOOD_UPSTREAM_CA");
    if (!config.native_robinhood_canary || parse_port(upstream_port) < 0 || !upstream_ca || !*upstream_ca ||
        !http::OutboundHttpsValidateCaBundle(upstream_ca)) {
      fprintf(stderr, "FORTIS_ENABLE_TEST_ROBINHOOD_ROUTES needs FORTIS_NATIVE_ROBINHOOD_CANARY=1, "
                      "FORTIS_TEST_ROBINHOOD_UPSTREAM_PORT and FORTIS_TEST_ROBINHOOD_UPSTREAM_CA\n");
      return false;
    }
    snprintf(robinhood_test_upstream.port, sizeof(robinhood_test_upstream.port), "%d", parse_port(upstream_port));
    robinhood_test_upstream.ca = upstream_ca;
    robinhood_test_seams.clock = &robinhood_test_clock;
    robinhood_test_seams.transport_starter = &robinhood_test_upstream;
    robinhood_test_seams.ticket_random = &robinhood_test_tickets;
    const char* deadline = TestHookEnv("FORTIS_TEST_ROBINHOOD_DEADLINE_MS");
    if (deadline && *deadline) robinhood_test_seams.provider_deadline_ms = static_cast<uint32_t>(strtoul(deadline, nullptr, 10));
    options.robinhood_seams = &robinhood_test_seams;
  }
  return true;
}

bool SeedRobinhoodTest(fortis::storage::KvStore* kv) {
  if (robinhood_test_route && !seed_robinhood_test_devices(kv)) {
    fprintf(stderr, "Failed to seed test Robinhood fixtures\n");
    return false;
  }
  return true;
}

bool RegisterRobinhoodTestRoutes(http::HttpRouter& router, Planes& planes) {
  return !robinhood_test_route ||
         router.Add("GET", "/probe/test-robinhood-idle", handle_robinhood_test_idle, planes.robinhood ? &*planes.robinhood : nullptr);
}

}  // namespace fortis::app
#endif

// ---- DashPay name resolution --------------------------------------
#ifdef FORTIS_DASHPAY_ROUTE_TEST_ONLY
namespace fortis::app {
namespace {

// A text file the replay rewrites between cases; empty when missing.
size_t dashpay_test_file(const char* variable, char* buffer, size_t capacity) {
  const char* file = TestHookEnv(variable);
  buffer[0] = '\0';
  if (!file || !*file) return 0;
  FILE* handle = fopen(file, "r");
  if (!handle) return 0;
  size_t length = fread(buffer, 1, capacity - 1, handle);
  fclose(handle);
  while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) --length;
  buffer[length] = '\0';
  return length;
}

// The freshness clock: FORTIS_TEST_DASHPAY_NOW_FILE (the captured replies'
// signed time); otherwise the wall clock.
class DashPayTestClock final : public fortis::base::Clock {
 public:
  int64_t NowMs() override;
};
int64_t DashPayTestClock::NowMs() {
  char text[32];
  if (dashpay_test_file("FORTIS_TEST_DASHPAY_NOW_FILE", text, sizeof(text))) {
    char* end = nullptr;
    const long long value = strtoll(text, &end, 10);
    if (end != text && value > 0) return value;
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// Deterministic evonode choice: the first live node in the trusted list.
class DashPayTestNodeChoice final : public fortis::exchange::dashpay::NodeChoice {
 public:
  uint32_t Next() override { return 0; }
};

// The TypeScript contract's malformed-identity case: when
// FORTIS_TEST_DASHPAY_IDENTITY_FILE names text, a found identity becomes it.
class DashPayTestLookupHook final : public exchange::DashPayLookupHook {
 public:
  void AfterLookup(fortis::exchange::dashpay::Outcome* outcome, char* identity, size_t capacity) override {
    char text[128];
    if (*outcome != fortis::exchange::dashpay::Outcome::kFound ||
        !dashpay_test_file("FORTIS_TEST_DASHPAY_IDENTITY_FILE", text, sizeof(text)))
      return;
    snprintf(identity, capacity, "%s", text);
  }
};

bool allow_dashpay_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// The trusted quorum service and every evonode go to one loopback fixture;
// each request names its real host in x-fortis-test-host.
struct DashPayTestUpstream final : fortis::base::Starter<fortis::chains::Transport> {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(fortis::chains::Transport& transport) override {
    return transport.StartRedirected("localhost", port, ca, allow_dashpay_test_address, nullptr,
                                     "x-fortis-test-host");
  }
};

// The DashPay fixture devices: premium tier,
// letters A..D.
bool seed_dashpay_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{};
  gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  static const char* kIds[] = {"01ARZ3NDEKTSV4RRFFQ69G5FAV", "01ARZ3NDEKTSV4RRFFQ69G5FB2", "01ARZ3NDEKTSV4RRFFQ69G5FC3",
                               "01ARZ3NDEKTSV4RRFFQ69G5FD4"};
  for (size_t i = 0; i < 4; ++i) {
    char token[64];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = static_cast<char>('A' + i);
    token[54] = '\0';
    if (!auth.SeedAddressRouteFixture(token, kIds[i], now_ms)) return false;
  }
  return true;
}

// GET /probe/test-dashpay-idle: waits until no job is queued or running.
void handle_dashpay_test_idle(int client, const http::HttpRequest&, bool head, void* opaque) {
  const bool idle = static_cast<exchange::DashPayService*>(opaque)->WaitIdle(30000);
  WriteHttpResponse(client, http::JsonResponse(idle ? 200 : 503, idle ? "OK" : "Service Unavailable",
                                         idle ? "{\"idle\":true}" : "{\"idle\":false}"),
                    head);
}

// GET /probe/test-dashpay-generation: the trusted connection generation.
void handle_dashpay_test_generation(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* service = static_cast<exchange::DashPayService*>(opaque);
  char reason[96], body[192];
  if (service) service->last_failure(reason, sizeof(reason));
  else reason[0] = '\0';
  for (char* p = reason; *p; ++p)
    if (*p == '"' || *p == '\\' || static_cast<unsigned char>(*p) < 0x20) *p = '?';
  snprintf(body, sizeof(body), "{\"generation\":%llu,\"lastFailure\":\"%s\"}",
           static_cast<unsigned long long>(service ? service->generation() : 0), reason);
  WriteHttpResponse(client, http::JsonResponse(200, "OK", body), head);
}

bool dashpay_test_route = false;
DashPayTestUpstream dashpay_test_upstream{};
exchange::DashPayServiceSeams dashpay_test_seams{};
DashPayTestClock dashpay_test_clock;
DashPayTestNodeChoice dashpay_test_node_choice;
DashPayTestLookupHook dashpay_test_lookup_hook;

}  // namespace

bool ConfigureDashPayTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* dashpay_flag = TestHookEnv("FORTIS_ENABLE_TEST_DASHPAY_ROUTES");
  dashpay_test_route = dashpay_flag && strcmp(dashpay_flag, "1") == 0;
  if (dashpay_test_route) {
    const char* upstream_port = TestHookEnv("FORTIS_TEST_DASHPAY_UPSTREAM_PORT");
    const char* upstream_ca = TestHookEnv("FORTIS_TEST_DASHPAY_UPSTREAM_CA");
    if (!config.native_dashpay_canary || parse_port(upstream_port) < 0 || !upstream_ca || !*upstream_ca ||
        !http::OutboundHttpsValidateCaBundle(upstream_ca)) {
      fprintf(stderr, "FORTIS_ENABLE_TEST_DASHPAY_ROUTES needs FORTIS_NATIVE_DASHPAY_CANARY=1, "
                      "FORTIS_TEST_DASHPAY_UPSTREAM_PORT and FORTIS_TEST_DASHPAY_UPSTREAM_CA\n");
      return false;
    }
    snprintf(dashpay_test_upstream.port, sizeof(dashpay_test_upstream.port), "%d", parse_port(upstream_port));
    dashpay_test_upstream.ca = upstream_ca;
    dashpay_test_seams.clock = &dashpay_test_clock;
    dashpay_test_seams.node_choice = &dashpay_test_node_choice;
    dashpay_test_seams.transport_starter = &dashpay_test_upstream;
    dashpay_test_seams.lookup_hook = &dashpay_test_lookup_hook;
    const char* connect_ms = TestHookEnv("FORTIS_TEST_DASHPAY_CONNECT_MS");
    const char* lookup_ms = TestHookEnv("FORTIS_TEST_DASHPAY_LOOKUP_MS");
    const char* request_ms = TestHookEnv("FORTIS_TEST_DASHPAY_REQUEST_MS");
    if (connect_ms && *connect_ms) dashpay_test_seams.connect_ms = static_cast<uint32_t>(strtoul(connect_ms, nullptr, 10));
    if (lookup_ms && *lookup_ms) dashpay_test_seams.lookup_ms = static_cast<uint32_t>(strtoul(lookup_ms, nullptr, 10));
    if (request_ms && *request_ms) dashpay_test_seams.request_ms = static_cast<uint32_t>(strtoul(request_ms, nullptr, 10));
    options.dashpay_seams = &dashpay_test_seams;
  }
  return true;
}

bool SeedDashPayTest() {
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (dashpay_test_route && (!kv || !seed_dashpay_test_devices(kv))) {
    fprintf(stderr, "Failed to seed test DashPay fixtures\n");
    return false;
  }
  return true;
}

bool RegisterDashPayTestRoutes(http::HttpRouter& router, Planes& planes) {
  return !dashpay_test_route ||
         (router.Add("GET", "/probe/test-dashpay-idle", handle_dashpay_test_idle, planes.dashpay ? &*planes.dashpay : nullptr) &&
          router.Add("GET", "/probe/test-dashpay-generation", handle_dashpay_test_generation, planes.dashpay ? &*planes.dashpay : nullptr));
}

}  // namespace fortis::app
#endif

// ---- operations (/statusz, crash reports, KV sweeps and compaction) ---------
#ifdef FORTIS_OPERATIONS_TEST_ONLY
namespace fortis::app {
namespace {

// Test-only operations seams. The crash report and expiry sweeper clock reads
// FORTIS_TEST_OPERATIONS_NOW_FILE when present (so one process can move it),
// otherwise the wall clock.
int64_t operations_test_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_OPERATIONS_NOW_FILE");
  if (now_file && *now_file) {
    if (FILE* handle = fopen(now_file, "r")) {
      char buffer[32]{};
      const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
      fclose(handle);
      buffer[length] = '\0';
      char* end = nullptr;
      const long long parsed = strtoll(buffer, &end, 10);
      if (end && end != buffer && parsed >= 0 && (!*end || *end == '\n'))
        return static_cast<int64_t>(parsed);
    }
  }
  timeval now{}; gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

TestFnClock<operations_test_now_ms> operations_test_clock;

// The 64 device credentials of the operations route fixture.
bool seed_operations_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{}; gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  for (int index = 0; index < 64; ++index) {
    char id[27], token[55];
    snprintf(id, sizeof(id), "01ARZ3NDEKTSV4RRFFQ69G5F%02d", index);
    snprintf(token, sizeof(token), "fintel_dev_%02dKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK", index);
    if (!auth.SeedPriceRouteFixture(token, id, now_ms)) return false;
  }
  return true;
}

// Cached prices for the public operator chart, with no provider requests.
// Seeded into the store the price planes use: the split price store when
// FORTIS_SPLIT_PRICE_STORE=1, so the token pages must read from it.
bool seed_operations_test_prices(fortis::storage::KvStore* kv) {
  if (!kv) return false;
  timeval now{}; gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  fortis::prices::PriceSeriesStore series(*kv);
  const int64_t bucket = now_ms / 300000 * 300000;
  const fortis::prices::PriceSeriesCandleInput candles[] = {
      {bucket - 600000, 100, 101, 99, 100, 0, false},
      {bucket - 300000, 100, 111, 100, 110, 0, false},
      {bucket, 110, 121, 109, 120, 0, false},
  };
  const fortis::prices::PriceSpotQuoteInput quote{"ethereum", 120, 0, 0, false, false,
                                          now_ms - 1000, 0};
  return series.UpsertCandles(fortis::prices::PriceSeriesInterval::k5m, "usd", "ethereum",
                             candles, 3, now_ms) == fortis::prices::PriceSeriesStoreStatus::kStored &&
         series.RecordSpots("usd", "test", &quote, 1, now_ms) ==
             fortis::prices::PriceSeriesStoreStatus::kStored;
}

// GET /v1/test-operations/status/:status answers that status. With a bearer
// it first runs the device preHandler, so telemetry sees the client.
void handle_operations_test_status(int client, const http::HttpRequest& request, bool head, void*) {
  const char* raw = HttpPathParamValue(request, "status");
  char* end = nullptr;
  const long status = raw ? strtol(raw, &end, 10) : 0;
  if (!raw || !end || *end || status < 200 || status > 599) {
    respond(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head);
    return;
  }
  if (request.authorization) {
    const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1);
    if (policy.status) {
      respond(client, policy.status, policy.reason, policy.error_body, head);
      return;
    }
  }
  respond(client, static_cast<int>(status), status >= 500 ? "Test Failure" : "OK",
          "{\"error\":\"TEST\"}", head);
}

// GET /v1/test-operations/reserve holds the socket until the handoff times
// out (504), so telemetry records a reserved response at completion.
void handle_operations_test_reserve(int client, const http::HttpRequest& request, bool head, void*) {
  const unsigned timeout_ms = query_unsigned(request.raw_target, "timeout_ms", 200, 5000);
  uint64_t token = 0;
  if (!http::ReserveHttpResponse(client, head, timeout_ms ? timeout_ms : 1, &token))
    respond(client, 503, "Service Unavailable", "{\"error\":\"OVERLOADED\"}", head);
}

// GET /probe/test-crash-report/:id: the facet's internal find().
void handle_operations_test_crash(int client, const http::HttpRequest& request, bool head, void*) {
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  static char record[1024 * 1024];
  size_t length = 0;
  if (!kv || !operations::FindCrashReport(*kv, HttpPathParamValue(request, "id"),
                              operations_test_now_ms(), record, sizeof(record) - 1,
                              &length)) {
    respond(client, 404, "Not Found", "{\"error\":\"NOT_FOUND\"}", head);
    return;
  }
  respond_bytes(client, 200, "OK", "application/json", reinterpret_cast<uint8_t*>(record),
                length, head);
}

void handle_operations_test_sweeps(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* sweeper = static_cast<const storage::KvExpirySweeper*>(opaque);
  char body[64];
  snprintf(body, sizeof(body), "{\"swept\":%llu}",
           static_cast<unsigned long long>(sweeper->swept()));
  respond(client, 200, "OK", body, head);
}

// Deterministic churn records for compaction tests: value i of `set` at
// `version` is its key repeated to `size` bytes followed by the version.
void churn_value(const char* key, unsigned version, unsigned size, char* value) {
  const size_t key_length = strlen(key);
  for (unsigned i = 0; i < size; ++i) value[i] = key[i % key_length];
  snprintf(value + size - 12, 12, "%011u", version);
}

// GET /probe/test-kv-churn?set=&count=&size=&ttl_ms=&version= writes `count`
// records under probe/churn/<set>/ in 64-record frames, expiring `ttl_ms`
// after the operations clock (0 = permanent).
void handle_operations_test_churn(int client, const http::HttpRequest& request, bool head, void*) {
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  const unsigned set = query_unsigned(request.raw_target, "set", 0, 9);
  const unsigned count = query_unsigned(request.raw_target, "count", 0, 100000);
  const unsigned size = query_unsigned(request.raw_target, "size", 64, 65536);
  const unsigned ttl_ms = query_unsigned(request.raw_target, "ttl_ms", 0, 2000000000u);
  const unsigned version = query_unsigned(request.raw_target, "version", 0, 1000000000u);
  const int64_t now = operations_test_now_ms();
  static char keys[64][48];
  static char values[64][65536];
  static pthread_mutex_t churn_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&churn_mutex);
  bool ok = kv && size >= 16;
  for (unsigned start = 0; ok && start < count; start += 64) {
    fortis::storage::KvMutation mutations[64];
    unsigned batch = 0;
    for (unsigned i = start; i < count && batch < 64; ++i, ++batch) {
      snprintf(keys[batch], sizeof(keys[batch]), "probe/churn/%u/%06u", set, i);
      churn_value(keys[batch], version, size, values[batch]);
      mutations[batch] = {keys[batch], values[batch], size, ttl_ms ? now + ttl_ms : 0, false};
    }
    ok = kv->Write(mutations, batch);
  }
  pthread_mutex_unlock(&churn_mutex);
  respond(client, ok ? 200 : 500, ok ? "OK" : "Internal Server Error",
          ok ? "{\"ok\":true}" : "{\"ok\":false}", head);
}

// GET /probe/test-kv-churn-check?set=&count=&size=&version= counts present
// records and those whose bytes match exactly.
void handle_operations_test_churn_check(int client, const http::HttpRequest& request, bool head,
                                        void*) {
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  const unsigned set = query_unsigned(request.raw_target, "set", 0, 9);
  const unsigned count = query_unsigned(request.raw_target, "count", 0, 100000);
  const unsigned size = query_unsigned(request.raw_target, "size", 64, 65536);
  const unsigned version = query_unsigned(request.raw_target, "version", 0, 1000000000u);
  const int64_t now = operations_test_now_ms();
  static char expected[65536], actual[65536];
  static pthread_mutex_t check_mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&check_mutex);
  unsigned present = 0, valid = 0;
  for (unsigned i = 0; kv && size >= 16 && i < count; ++i) {
    char key[48];
    snprintf(key, sizeof(key), "probe/churn/%u/%06u", set, i);
    size_t length = 0;
    if (!kv->Get(key, now, actual, sizeof(actual), &length)) continue;
    ++present;
    churn_value(key, version, size, expected);
    if (length == size && memcmp(actual, expected, size) == 0) ++valid;
  }
  pthread_mutex_unlock(&check_mutex);
  char body[96];
  snprintf(body, sizeof(body), "{\"present\":%u,\"valid\":%u}", present, valid);
  respond(client, 200, "OK", body, head);
}

bool operations_test = false;

}  // namespace

bool ConfigureOperationsTestPlane(operations::CrashReportRouteContext& crash_reports,
                                  operations::OperationsRouteContext& operations, operations::ApiTelemetry& telemetry) {
  const char* operations_flag = TestHookEnv("FORTIS_ENABLE_TEST_OPERATIONS");
  operations_test = operations_flag && strcmp(operations_flag, "1") == 0;
  if (operations_test) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    if (!kv || !seed_operations_test_devices(kv)) {
      fprintf(stderr, "Failed to seed test operations devices\n");
      return false;
    }
    crash_reports.clock = &operations_test_clock;
    // One movable clock for the telemetry samples and the pages that read
    // them, so tests can cross window and memo boundaries deterministically.
    operations.clock = &operations_test_clock;
    telemetry.SetClock(&operations_test_clock);
  }
  return true;
}

bool RegisterOperationsTestPlaneRoutes(http::HttpRouter& router, Planes& planes) {
  if (operations_test && !seed_operations_test_prices(
          planes.price_kv ? planes.price_kv : fortis::storage::RuntimeKvStore())) {
    fprintf(stderr, "Failed to seed test operations prices\n");
    return false;
  }
  if (operations_test &&
      (!router.Add("GET", "/v1/test-operations/status/:status", handle_operations_test_status) ||
       !router.Add("GET", "/v1/test-operations/reserve", handle_operations_test_reserve) ||
       !router.Add("GET", "/probe/test-crash-report/:id", handle_operations_test_crash) ||
       !router.Add("GET", "/probe/test-kv-sweeps", handle_operations_test_sweeps,
                   &planes.kv_sweeper) ||
       !router.Add("GET", "/probe/test-kv-churn", handle_operations_test_churn) ||
       !router.Add("GET", "/probe/test-kv-churn-check", handle_operations_test_churn_check))) {
    fprintf(stderr, "Failed to register test operations routes\n");
    return false;
  }
  return true;
}

void OperationsTestSweep(Planes& planes, uint32_t* interval_ms, fortis::base::Clock** clock) {
  if (operations_test) {
    *clock = &operations_test_clock;
    const char* interval = TestHookEnv("FORTIS_TEST_KV_SWEEP_INTERVAL_MS");
    if (interval && *interval) *interval_ms = static_cast<uint32_t>(strtoul(interval, nullptr, 10));
    // A small policy so compaction tests reach the triggers in seconds.
    const char* small = TestHookEnv("FORTIS_TEST_KV_COMPACTION_SMALL");
    if (small && strcmp(small, "1") == 0) {
      fortis::storage::KvCompactionPolicy policy;
      policy.min_file_bytes = 256 * 1024;
      policy.startup_delay_ms = 0;
      policy.min_interval_ms = 0;
      policy.retry_backoff_ms = 100;
      policy.max_backoff_ms = 1000;
      policy.reserve_bytes = 0;
      policy.slice_records = 64;
      policy.slice_bytes = 32 * 1024;
      policy.swap_tail_bytes = 32 * 1024;
      policy.tick_max_slices = 8;
      planes.kv_sweeper.SetCompactionPolicy(policy);
    }
  }
}

}  // namespace fortis::app
#endif

// ---- the networks route (R09) -----------------------------------------------
#ifdef FORTIS_NETWORK_ROUTE_TEST_ONLY
namespace fortis::app {

bool SeedNetworkTest() {
  const char* network_route_flag = TestHookEnv("FORTIS_ENABLE_TEST_NETWORK_ROUTE");
  const bool network_route_enabled = network_route_flag && strcmp(network_route_flag, "1") == 0;
  if (network_route_enabled) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    if (!kv) { fprintf(stderr, "Failed to open test networks route KV\n"); return false; }
    const char* fixture_token = "fintel_dev_NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN";
    const char* fixture_id = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    fortis::auth::DeviceAuthStore auth(kv);
    timeval auth_now{}; gettimeofday(&auth_now, nullptr);
    const int64_t auth_ms = static_cast<int64_t>(auth_now.tv_sec) * 1000 + auth_now.tv_usec / 1000;
    if (!auth.SeedPriceRouteFixture(fixture_token, fixture_id, auth_ms)) {
      fprintf(stderr, "Failed to seed test networks route fixture auth\n"); return false;
    }
    // The R09 edge devices (networks_edges.json): A–D valid
    // development devices, E expired one second ago, R revoked.
    static const char* kEdgeIds[] = {"01ARZ3NDEKTSV4RRFFQ69G5FA1", "01ARZ3NDEKTSV4RRFFQ69G5FB2",
                                     "01ARZ3NDEKTSV4RRFFQ69G5FC3", "01ARZ3NDEKTSV4RRFFQ69G5FD4",
                                     "01ARZ3NDEKTSV4RRFFQ69G5FE5", "01ARZ3NDEKTSV4RRFFQ69G5FR6"};
    static const char kEdgeLetters[] = "ABCDER";
    for (size_t i = 0; i < 6; ++i) {
      char edge_token[55];
      memcpy(edge_token, "fintel_dev_", 11);
      memset(edge_token + 11, kEdgeLetters[i], 43);
      edge_token[54] = '\0';
      const int64_t seeded_ms = kEdgeLetters[i] == 'E' ? auth_ms - 3601000 : auth_ms;
      if (!auth.SeedPriceRouteFixture(edge_token, kEdgeIds[i], seeded_ms) ||
          (kEdgeLetters[i] == 'R' && !auth.Revoke(kEdgeIds[i], auth_ms - 1000))) {
        fprintf(stderr, "Failed to seed test networks edge devices\n"); return false;
      }
    }
  }
  return true;
}

}  // namespace fortis::app
#endif
