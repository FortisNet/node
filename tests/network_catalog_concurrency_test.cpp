#include "test_support.h"
// GET and HEAD /v1/networks run without the router's route_state_mutex
// (network_catalog_runs_without_state_lock in src/http/http_server.cpp).
// Host-only, in process, over socketpairs and a real process KV, with the
// real network handler (RegisterNetworksRoute) and router; no provider,
// guest or Docker:
//  - held lock: while an unrelated locked route holds the lock, GET and HEAD
//    (JSON and protobuf) answer with the startup document's exact bytes and
//    headers while KV writes, sweeps and compactions race them; eight reads
//    in flight at once all answer; the device policy edge cases (missing,
//    malformed and unknown bearer, lowercase scheme, revoked device, Origin)
//    answer too. POST /v1/networks, GET|HEAD /v1/networks/:network/overview
//    and POST /v1/devices/rotate stay locked (negative controls);
//  - quota: a device at its 240/minute development quota is answered 200
//    with x-rate-limit-remaining 1 and 0, then 429 RATE_LIMITED with
//    Retry-After: 60, while the lock is held;
//  - churn: catalog reads from four threads race repeated rotations of
//    another device through the locked rotation route and KV
//    writes/sweeps/compactions; every read is the exact document;
//  - rotation: a GET paused inside device authentication while the locked
//    rotation route commits answers 401 SESSION_EXPIRED (it could not if the
//    GET held the lock); the old bearer is then refused and the new one reads;
//  - accounting: the route's Prometheus counter and the completion hook saw
//    every catalog dispatch, each 200 tagged with its device.
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <string>

#include "device_auth.h"
#include "device_enrollment.h"
#include "fortisnet/intelligence/v1/networks.pb.h"
#include "http_server.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "native_config.h"
#include "network_registry.h"
#include "network_route.h"
#include "token_json.h"

using namespace fortis::auth;
using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::networks;
using namespace fortis::operations;

namespace {

namespace tj = fortis::codec::tj;

// Development-tier fixture devices (SeedPriceRouteFixture, 240/minute).
constexpr char kTokenA[] = "fintel_dev_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  // reads
constexpr char kTokenB[] = "fintel_dev_BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";  // edges
constexpr char kTokenC[] = "fintel_dev_CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";  // quota
constexpr char kTokenD[] = "fintel_dev_DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD";  // rotation
constexpr char kTokenE[] = "fintel_dev_EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE";  // churned
constexpr char kTokenR[] = "fintel_dev_RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR";  // revoked
constexpr char kTokenF[] = "fintel_dev_FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";  // independent lock-free rotation
constexpr char kTokenU[] = "fintel_dev_UUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUU";  // unknown
constexpr char kIdA[] = "01JZ00000000000000000000NA";
constexpr char kIdB[] = "01JZ00000000000000000000NB";
constexpr char kIdC[] = "01JZ00000000000000000000NC";
constexpr char kIdD[] = "01JZ00000000000000000000ND";
constexpr char kIdE[] = "01JZ00000000000000000000NE";
constexpr char kIdF[] = "01JZ00000000000000000000NF";
constexpr char kIdR[] = "01JZ00000000000000000000NR";
constexpr uint32_t kDevelopmentQuota = 240;

void require(bool condition, const char* what) {
  if (condition) return;
  fprintf(stderr, "FAIL: %s\n", what);
  fflush(stderr);
  _exit(1);
}

int64_t wall_ms() {
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

double seconds_since(const timespec& start) {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<double>(now.tv_sec - start.tv_sec) +
         static_cast<double>(now.tv_nsec - start.tv_nsec) / 1e9;
}

void pause_ms(long milliseconds) {
  timespec pause{milliseconds / 1000, (milliseconds % 1000) * 1000 * 1000};
  nanosleep(&pause, nullptr);
}

void remove_directory(const char* directory) {
  DIR* entries = opendir(directory);
  if (!entries) return;
  while (dirent* entry = readdir(entries)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    const std::string path = std::string(directory) + "/" + entry->d_name;
    unlink(path.c_str());
  }
  closedir(entries);
  rmdir(directory);
}

// ---- HTTP plumbing ---------------------------------------------------------------

struct DispatchArgs {
  HttpRouter* router;
  int client;
};

void* dispatch_thread(void* opaque) {
  auto* args = static_cast<DispatchArgs*>(opaque);
  if (!args->router->Dispatch(args->client)) close(args->client);
  return nullptr;
}

struct Pending {
  DispatchArgs args{};
  int peer = -1;
  pthread_t thread{};
};

void start(HttpRouter* router, const char* method, const std::string& path, const std::string& headers,
           Pending* pending) {
  int pair[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
  const std::string request = std::string(method) + " " + path + " HTTP/1.1\r\nHost: test\r\n" + headers + "\r\n";
  require(send(pair[1], request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()),
          "request sent");
  shutdown(pair[1], SHUT_WR);
  pending->args = {router, pair[0]};
  pending->peer = pair[1];
  require(pthread_create(&pending->thread, nullptr, dispatch_thread, &pending->args) == 0, "dispatch thread");
}

bool readable_within(int fd, int milliseconds) {
  pollfd entry{fd, POLLIN, 0};
  return poll(&entry, 1, milliseconds) == 1;
}

struct Reply {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
  std::string header(const char* name) const {
    const auto found = headers.find(name);
    return found == headers.end() ? std::string("<absent>") : found->second;
  }
};

Reply finish(Pending* pending) {
  std::string raw;
  char buffer[65536];
  for (;;) {
    const ssize_t got = recv(pending->peer, buffer, sizeof(buffer), 0);
    if (got <= 0) break;
    raw.append(buffer, static_cast<size_t>(got));
  }
  pthread_join(pending->thread, nullptr);
  close(pending->peer);
  Reply reply;
  const size_t end = raw.find("\r\n\r\n");
  require(end != std::string::npos && raw.compare(0, 9, "HTTP/1.1 ") == 0, "well-formed response");
  reply.status = fortis::test::ParseInteger<int>(raw.c_str() + 9);
  size_t at = raw.find("\r\n") + 2;
  while (at < end) {
    const size_t line_end = raw.find("\r\n", at);
    const size_t colon = raw.find(':', at);
    require(colon != std::string::npos && colon < line_end, "header line");
    std::string name = raw.substr(at, colon - at);
    for (char& c : name) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    size_t value = colon + 1;
    while (value < line_end && raw[value] == ' ') ++value;
    require(reply.headers.emplace(name, raw.substr(value, line_end - value)).second, "no repeated header");
    at = line_end + 2;
  }
  reply.body = raw.substr(end + 4);
  return reply;
}

Reply call(HttpRouter* router, const char* method, const std::string& path, const std::string& headers) {
  Pending pending;
  start(router, method, path, headers, &pending);
  return finish(&pending);
}

// A request that must answer within `milliseconds`; fails fast otherwise
// (for example while it waits on a held route lock).
Reply call_within(HttpRouter* router, const char* method, const std::string& path, const std::string& headers,
                  int milliseconds, const char* what) {
  Pending pending;
  start(router, method, path, headers, &pending);
  require(readable_within(pending.peer, milliseconds), what);
  return finish(&pending);
}

// Holds until released (at most 20 s): the unrelated slow locked route.
struct Gate {
  std::atomic<int> entered{0};
  std::atomic<bool> release{false};
};

void gated_response(int client, const HttpRequest&, bool head, void* context) {
  auto* gate = static_cast<Gate*>(context);
  gate->entered.fetch_add(1);
  for (int i = 0; i < 20000 && !gate->release.load(); ++i) pause_ms(1);
  WriteHttpResponse(client, JsonResponse(200, "OK", "{}"), head);
}

// Stand-ins at the catalog's sibling registrations: they answer at once, so
// only the route lock can hold them back. 202 tells them apart in the
// completion hook, which is keyed by template only.
void immediate_response(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, JsonResponse(202, "Accepted", "{\"standIn\":true}"), head);
}

bool wait_for(const std::atomic<int>& value, int at_least, int milliseconds) {
  for (int i = 0; i < milliseconds && value.load() < at_least; ++i) pause_ms(1);
  return value.load() >= at_least;
}

// Unrelated KV traffic on the process store: expiring writes, sweeps of
// those keys and full compactions, racing every read below.
struct KvRacer {
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> writes{0};
  std::atomic<uint64_t> swept{0};
  std::atomic<int> compactions{0};
};

void* kv_race_thread(void* opaque) {
  auto* racer = static_cast<KvRacer*>(opaque);
  fortis::storage::KvStore* store = fortis::storage::RuntimeKvStore();
  require(store != nullptr, "racer: store");
  char value[96];
  memset(value, 'r', sizeof(value));
  for (uint64_t round = 0; !racer->stop.load(); ++round) {
    char key[32];
    snprintf(key, sizeof(key), "race/%llu", static_cast<unsigned long long>(round % 64));
    // Expiries far below every device record, so sweeping at them removes
    // only these keys.
    const fortis::storage::KvMutation write{key, value, sizeof(value), static_cast<int64_t>(1000 + round), false};
    require(store->Write(&write, 1), "racer: write");
    racer->writes.fetch_add(1);
    if (round % 8 == 7) racer->swept.fetch_add(store->Sweep(static_cast<int64_t>(1000 + round), 1024));
    if (round % 32 == 31) {
      require(store->Compact(), "racer: compaction");
      racer->compactions.fetch_add(1);
    }
    pause_ms(1);
  }
  return nullptr;
}

// Completion telemetry for the catalog template, from every worker thread.
struct Accounting {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  std::map<int, uint64_t> statuses;
  uint64_t untagged_ok = 0;
  uint64_t dispatched = 0;  // catalog requests this test sent
};
Accounting g_accounting;

void completion_hook(void*, const char* route, int status, const char* device_id, double) {
  if (strcmp(route, "/v1/networks") != 0) return;
  pthread_mutex_lock(&g_accounting.mutex);
  ++g_accounting.statuses[status];
  if (status == 200 && (!device_id || strlen(device_id) != 26)) ++g_accounting.untagged_ok;
  pthread_mutex_unlock(&g_accounting.mutex);
}

// ---- the catalog -----------------------------------------------------------------

struct Documents {
  std::string json;
  std::string protobuf;
};

HttpRouter* g_router = nullptr;
Documents g_documents;

std::string bearer(const char* token) { return std::string("Authorization: Bearer ") + token + "\r\n"; }

Reply catalog(const char* method, const std::string& headers) {
  {
    pthread_mutex_lock(&g_accounting.mutex);
    ++g_accounting.dispatched;
    pthread_mutex_unlock(&g_accounting.mutex);
  }
  return call(g_router, method, "/v1/networks", headers);
}

Reply catalog_within(const char* method, const std::string& headers, const char* what) {
  {
    pthread_mutex_lock(&g_accounting.mutex);
    ++g_accounting.dispatched;
    pthread_mutex_unlock(&g_accounting.mutex);
  }
  return call_within(g_router, method, "/v1/networks", headers, 2000, what);
}

// "app;dur=<digits>.<3 digits>" (http_server.cpp, kServerTimingLineBytes).
bool valid_server_timing(const std::string& value) {
  static const std::string kPrefix = "app;dur=";
  if (value.compare(0, kPrefix.size(), kPrefix) != 0) return false;
  const size_t dot = value.find('.', kPrefix.size());
  if (dot == std::string::npos || dot == kPrefix.size() || value.size() != dot + 4) return false;
  for (size_t i = kPrefix.size(); i < value.size(); ++i)
    if (i != dot && (value[i] < '0' || value[i] > '9')) return false;
  return true;
}

void require_baseline(const Reply& reply) {
  const std::string date = reply.header("date");
  tm utc{};
  const char* end = strptime(date.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &utc);
  require(date.size() == 29 && end && *end == 0 && date.ends_with(" GMT"), "Date: IMF-fixdate GMT");
  require(reply.header("connection") == "close", "Connection: close");
  require(reply.header("x-content-type-options") == "nosniff", "nosniff");
  require(reply.header("referrer-policy") == "no-referrer", "Referrer-Policy");
  require(reply.header("permissions-policy") == "camera=(), microphone=(), geolocation=()",
          "Permissions-Policy");
  require(reply.header("cache-control") == "no-store", "Cache-Control: no-store");
  require(valid_server_timing(reply.header("server-timing")), "one Server-Timing: app;dur=<ms>");
}

// `remaining` < 0 accepts any x-rate-limit-remaining below the quota.
void require_catalog(const Reply& reply, bool head, bool protobuf, int remaining = -1) {
  const std::string& expected = protobuf ? g_documents.protobuf : g_documents.json;
  if (reply.status != 200) fprintf(stderr, "got %d %s\n", reply.status, reply.body.c_str());
  require(reply.status == 200, "catalog 200");
  require_baseline(reply);
  require(reply.header("content-type") ==
              (protobuf ? "application/protobuf" : "application/json; charset=utf-8"),
          "catalog content type");
  require(reply.header("content-length") == std::to_string(expected.size()), "catalog Content-Length");
  require(reply.header("vary") == "Accept", "catalog Vary: Accept");
  require(reply.header("x-rate-limit-cost") == "1", "catalog x-rate-limit-cost");
  const std::string left = reply.header("x-rate-limit-remaining");
  require(left != "<absent>" && static_cast<uint32_t>(fortis::test::ParseInteger<int>(left)) < kDevelopmentQuota,
          "catalog x-rate-limit-remaining");
  if (remaining >= 0) require(left == std::to_string(remaining), "catalog exact x-rate-limit-remaining");
  require(reply.header("retry-after") == "<absent>", "catalog has no Retry-After");
  // The ten route headers plus the router's Server-Timing and HTTP Date.
  require(reply.headers.size() == 12, "catalog carries exactly the documented headers");
  require(reply.body == (head ? std::string() : expected), "catalog body is the startup document (HEAD: none)");
}

void require_refusal(const Reply& reply, int status, const char* body, bool quota_headers) {
  if (reply.status != status || reply.body != body) {
    fprintf(stderr, "got %d %s, want %d %s\n", reply.status, reply.body.c_str(), status, body);
    require(false, "refusal");
  }
  require_baseline(reply);
  require(reply.header("content-type") == "application/json; charset=utf-8", "refusal content type");
  require(reply.header("vary") == "<absent>", "refusal has no Vary");
  require((reply.header("x-rate-limit-cost") == "1") == quota_headers, "refusal rate-limit headers");
}

std::string read_headers(const char* token, const char* accept) {
  return bearer(token) + "Accept: " + accept + "\r\n";
}

struct Read {
  const char* method;
  const char* accept;
  bool protobuf;
};
constexpr Read kReads[] = {{"GET", "application/json", false},
                           {"HEAD", "application/json", false},
                           {"GET", "application/protobuf", true},
                           {"HEAD", "application/protobuf;q=0.5", true}};

// Waits, if needed, so the next few requests fall inside one quota minute.
void avoid_minute_boundary() {
  while (wall_ms() % 60000 > 55000) pause_ms(100);
}

// ---- held lock ---------------------------------------------------------------------

void test_reads_under_held_lock(KvRacer& racer, Gate& holder) {
  Pending held;
  start(g_router, "GET", "/v1/slow", "", &held);
  require(wait_for(holder.entered, 1, 5000), "slow locked route entered");

  // Sibling route controls stay locked. Device rotation uses its audited KV
  // transaction boundary and must answer independently with a separate bearer.
  Pending post, overview, overview_head, rotate;
  start(g_router, "POST", "/v1/networks", read_headers(kTokenB, "application/json"), &post);
  start(g_router, "GET", "/v1/networks/eth-mainnet/overview", read_headers(kTokenB, "application/json"),
        &overview);
  start(g_router, "HEAD", "/v1/networks/eth-mainnet/overview", read_headers(kTokenB, "application/json"),
        &overview_head);
  start(g_router, "POST", "/v1/devices/rotate", bearer(kTokenF), &rotate);

  timespec loop_start{};
  clock_gettime(CLOCK_MONOTONIC, &loop_start);
  int rounds = 0;
  // Until the racer has swept and compacted twice under these reads, but
  // bounded so device A stays under its 240/minute development quota.
  const int compactions_before = racer.compactions.load();
  const uint64_t swept_before = racer.swept.load();
  const uint64_t writes_before = racer.writes.load();
  while (rounds < 10 || racer.compactions.load() < compactions_before + 2 || racer.swept.load() == swept_before) {
    require(rounds < 40 && seconds_since(loop_start) < 15.0, "reads finished within bounds");
    // Exercise reads across actual writer/compactor progress, rather than
    // spending the device quota while another process temporarily has the CPU.
    while (racer.writes.load() < writes_before + static_cast<uint64_t>(rounds) * 8) {
      require(seconds_since(loop_start) < 15.0, "writer progresses within the original deadline");
      pause_ms(1);
    }
    for (const Read& read : kReads) {
      const Reply reply = catalog_within(read.method, read_headers(kTokenA, read.accept),
                                         "catalog read answers within 2 s while the lock is held");
      require_catalog(reply, strcmp(read.method, "HEAD") == 0, read.protobuf);
    }
    ++rounds;
    pause_ms(5);
  }
  const int reads = rounds * 4;

  // Eight reads in flight at once all answer, JSON and protobuf alternating.
  Pending parallel[8];
  for (int i = 0; i < 8; ++i) {
    pthread_mutex_lock(&g_accounting.mutex);
    ++g_accounting.dispatched;
    pthread_mutex_unlock(&g_accounting.mutex);
    start(g_router, i % 4 == 3 ? "HEAD" : "GET", "/v1/networks",
          read_headers(kTokenA, i % 2 ? "application/protobuf" : "*/*"), &parallel[i]);
  }
  for (int i = 0; i < 8; ++i) {
    require(readable_within(parallel[i].peer, 2000), "parallel read answers while the lock is held");
    require_catalog(finish(&parallel[i]), i % 4 == 3, i % 2 == 1);
  }

  // Device policy edge cases, still under the held lock.
  const char* json = "application/json";
  require_refusal(catalog("GET", "Accept: application/json\r\n"), 401, "{\"error\":\"UNAUTHORIZED\"}", false);
  require_refusal(catalog("GET", "Authorization: Bearer fintel_dev_short\r\n"), 401,
                  "{\"error\":\"UNAUTHORIZED\"}", false);
  require_refusal(catalog("GET", std::string("Authorization: bearer ") + kTokenB + "\r\n"), 401,
                  "{\"error\":\"UNAUTHORIZED\"}", false);
  require_refusal(catalog("GET", std::string("Authorization: Bearer  ") + kTokenB + "\r\n"), 401,
                  "{\"error\":\"UNAUTHORIZED\"}", false);
  require_refusal(catalog("GET", read_headers(kTokenU, json)), 401, "{\"error\":\"UNAUTHORIZED\"}", false);
  require_refusal(catalog("HEAD", read_headers(kTokenU, "application/protobuf")), 401, "", false);
  require_refusal(catalog("GET", read_headers(kTokenR, json)), 401, "{\"error\":\"SESSION_EXPIRED\"}", false);
  require_refusal(catalog("GET", read_headers(kTokenB, json) + "Origin: https://wallet.test\r\n"), 403,
                  "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}", false);
  require_refusal(catalog("GET", "Origin: https://wallet.test\r\n"), 403,
                  "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}", false);
  require_refusal(catalog("HEAD", read_headers(kTokenB, "application/protobuf") + "Origin: null\r\n"), 403, "",
                  false);
  // Ignored query string; protobuf with q=0 falls back to JSON.
  require_catalog(call(g_router, "GET", "/v1/networks?x=1", read_headers(kTokenB, "application/protobuf;q=0")),
                  false, false);
  {
    pthread_mutex_lock(&g_accounting.mutex);
    ++g_accounting.dispatched;
    pthread_mutex_unlock(&g_accounting.mutex);
  }

  // Quota: device C was pre-counted to two below its limit for this minute.
  avoid_minute_boundary();
  uint64_t used = 0;
  char key[96];
  snprintf(key, sizeof(key), "q:dev:%s:%lld", kIdC, static_cast<long long>(wall_ms() / 60000));
  require(fortis::storage::RuntimeKvIncrementByWindow(key, kDevelopmentQuota - 2, wall_ms(), 60000, &used) &&
              used == kDevelopmentQuota - 2,
          "device C pre-counted");
  require_catalog(catalog_within("GET", read_headers(kTokenC, json), "quota read 1 answers"), false,
                  false, 1);
  require_catalog(catalog_within("HEAD", read_headers(kTokenC, "application/protobuf"),
                                 "quota read 2 answers"),
                  true, true, 0);
  for (const char* method : {"GET", "HEAD"}) {
    const Reply limited = catalog_within(method, read_headers(kTokenC, json), "429 answers");
    require_refusal(limited, 429, strcmp(method, "HEAD") == 0 ? "" : "{\"error\":\"RATE_LIMITED\"}", true);
    require(limited.header("retry-after") == "60" && limited.header("x-rate-limit-remaining") == "0",
            "429 Retry-After: 60, remaining 0");
  }

  // Sibling controls are still waiting; device rotation is lock-free.
  require(!readable_within(post.peer, 300), "POST /v1/networks stays locked");
  require(!readable_within(overview.peer, 0), "GET /v1/networks/:network/overview stays locked");
  require(!readable_within(overview_head.peer, 0), "HEAD /v1/networks/:network/overview stays locked");
  require(readable_within(rotate.peer, 2000), "POST /v1/devices/rotate answers under the unrelated lock");
  const Reply rotated = finish(&rotate);
  require(rotated.status == 200 && rotated.body.find("\"deviceToken\":\"fintel_dev_") != std::string::npos,
          "independent device rotation completes while the route lock is held");
  require(holder.entered.load() == 1, "the slow route is still the only lock holder");

  holder.release.store(true);
  require(finish(&held).status == 200, "slow route completes");
  require(finish(&post).body == "{\"standIn\":true}", "POST /v1/networks completes after the lock is released");
  require(finish(&overview).body == "{\"standIn\":true}", "overview completes after the lock is released");
  require(finish(&overview_head).status == 202, "overview HEAD completes after the lock is released");
  printf("held lock: %d GET/HEAD JSON/protobuf reads, 8 parallel reads, 11 policy edge cases and 4 quota reads "
         "(remaining 1, 0, then 429) answered in %.2f s under an unrelated held lock (%llu racing KV writes, "
         "%d compactions); POST /v1/networks, GET|HEAD overview stayed locked; independent device rotation answered\n",
         reads, seconds_since(loop_start), static_cast<unsigned long long>(racer.writes.load()),
         racer.compactions.load());
}

// ---- reads during device and KV churn ---------------------------------------------

struct Churn {
  KvRacer* racer;
  int compactions_before;
  std::atomic<bool> done{false};
  std::atomic<int> rotations{0};
};

void* rotate_thread(void* opaque) {
  auto* churn = static_cast<Churn*>(opaque);
  std::string token = kTokenE;
  for (int i = 0; i < 30; ++i) {
    const Reply reply = call(g_router, "POST", "/v1/devices/rotate", bearer(token.c_str()));
    const size_t at = reply.body.find("\"deviceToken\":\"");
    require(reply.status == 200 && at != std::string::npos, "churn: rotation 200");
    token = reply.body.substr(at + 15, 54);
    churn->rotations.fetch_add(1);
    pause_ms(2);
  }
  // Keep the readers going until two compactions ran beside them.
  for (int i = 0; i < 5000 && churn->racer->compactions.load() < churn->compactions_before + 2; ++i) pause_ms(1);
  churn->done.store(true);
  return nullptr;
}

struct ChurnReader {
  Churn* churn;
  int index;
  int reads = 0;
};

void* churn_read_thread(void* opaque) {
  auto* reader = static_cast<ChurnReader*>(opaque);
  // Device A's quota: four readers, at most 12 reads each, paced so the
  // phase spans several racer compactions.
  while (reader->reads < 12 && (!reader->churn->done.load() || reader->reads < 3)) {
    const Read& read = kReads[(reader->index + reader->reads) % 4];
    require_catalog(catalog(read.method, read_headers(kTokenA, read.accept)), strcmp(read.method, "HEAD") == 0,
                    read.protobuf);
    ++reader->reads;
    pause_ms(10);
  }
  return nullptr;
}

void test_reads_during_churn(KvRacer& racer) {
  const int compactions_before = racer.compactions.load();
  Churn churn{&racer, compactions_before};
  ChurnReader readers[4];
  pthread_t reader_threads[4], rotator;
  require(pthread_create(&rotator, nullptr, rotate_thread, &churn) == 0, "rotator starts");
  for (int i = 0; i < 4; ++i) {
    readers[i] = {&churn, i};
    require(pthread_create(&reader_threads[i], nullptr, churn_read_thread, &readers[i]) == 0, "reader starts");
  }
  pthread_join(rotator, nullptr);
  for (pthread_t thread : reader_threads) pthread_join(thread, nullptr);
  int reads = 0;
  for (const ChurnReader& reader : readers) reads += reader.reads;
  require(churn.rotations.load() == 30, "churn: 30 rotations committed");
  require(racer.compactions.load() >= compactions_before + 2, "churn: compactions ran beside the reads");
  printf("churn: %d catalog reads on 4 threads beside 30 independent rotations of another device and %d KV "
         "compactions; every read was the exact document\n",
         reads, racer.compactions.load() - compactions_before);
}

// ---- rotation ---------------------------------------------------------------------

struct RotationHook {
  const char* device_id;
  std::atomic<bool> armed{false};
  std::atomic<int> paused{0};
  std::atomic<bool> resume{false};
};
RotationHook* g_hook = nullptr;

// Pauses the one armed GET between its bearer-hash lookup and its device row
// read (DeviceAuthStore::Authenticate's "authenticate" seam).
void rotation_hook(const char* operation, const char* id, void*) {
  if (!g_hook || strcmp(operation, "authenticate") != 0 || strcmp(id, g_hook->device_id) != 0) return;
  bool expected = true;
  if (!g_hook->armed.compare_exchange_strong(expected, false)) return;
  g_hook->paused.fetch_add(1);
  for (int i = 0; i < 5000 && !g_hook->resume.load(); ++i) pause_ms(1);
}

void test_rotation_during_read() {
  RotationHook hook;
  hook.device_id = kIdD;
  g_hook = &hook;
  fortis::auth::SetDeviceRowSnapshotHook(rotation_hook, nullptr);
  hook.armed.store(true);
  Pending read;
  {
    pthread_mutex_lock(&g_accounting.mutex);
    ++g_accounting.dispatched;
    pthread_mutex_unlock(&g_accounting.mutex);
  }
  start(g_router, "GET", "/v1/networks", read_headers(kTokenD, "application/json"), &read);
  require(wait_for(hook.paused, 1, 3000), "GET paused inside device authentication");
  // The rotation route commits D's new bearer while that GET is
  // inside its handler: it could not if the GET held the route lock.
  Pending rotate;
  start(g_router, "POST", "/v1/devices/rotate", bearer(kTokenD), &rotate);
  const bool rotation_answered = readable_within(rotate.peer, 3000);
  hook.resume.store(true);
  require(rotation_answered, "rotation completes while the GET is paused (the GET holds no route lock)");
  const Reply rotated = finish(&rotate);
  const size_t token_at = rotated.body.find("\"deviceToken\":\"");
  require(rotated.status == 200 && token_at != std::string::npos, "rotation 200");
  const std::string fresh = rotated.body.substr(token_at + 15, 54);
  require(fortis::auth::IsDeviceToken(fresh.c_str()) && fresh != kTokenD, "a new device bearer");
  require_refusal(finish(&read), 401, "{\"error\":\"SESSION_EXPIRED\"}", false);
  fortis::auth::SetDeviceRowSnapshotHook(nullptr, nullptr);
  g_hook = nullptr;

  require_refusal(catalog("GET", read_headers(kTokenD, "application/json")), 401, "{\"error\":\"UNAUTHORIZED\"}",
                  false);
  require_catalog(catalog("HEAD", read_headers(fresh.c_str(), "application/protobuf")), true, true);
  require_catalog(catalog("GET", read_headers(fresh.c_str(), "application/json")), false, false);
  puts("rotation: a GET paused in device auth while rotation committed answered 401 SESSION_EXPIRED; "
       "old bearer 401 UNAUTHORIZED, new bearer reads");
}

// ---- accounting -------------------------------------------------------------------

void test_accounting() {
  static char metrics[HttpRouter::kMetricsOutputCapacity];
  const size_t length = g_router->RenderPrometheus(metrics, sizeof(metrics));
  require(length > 0, "metrics render");
  const char* line = strstr(metrics, "fortis_native_http_requests_total{route=\"GET /v1/networks\"} ");
  require(line != nullptr, "catalog requests counter");
  const unsigned long long counted = strtoull(line + strlen("fortis_native_http_requests_total{route=\"GET "
                                                            "/v1/networks\"} "),
                                              nullptr, 10);
  const char* errors = strstr(metrics, "fortis_native_http_errors_total{route=\"GET /v1/networks\"} 0\n");
  pthread_mutex_lock(&g_accounting.mutex);
  uint64_t hooked = 0;
  for (const auto& entry : g_accounting.statuses) hooked += entry.second;
  const uint64_t dispatched = g_accounting.dispatched;
  const uint64_t ok = g_accounting.statuses[200];
  const uint64_t controls = g_accounting.statuses[202];
  const uint64_t untagged = g_accounting.untagged_ok;
  pthread_mutex_unlock(&g_accounting.mutex);
  require(counted == dispatched, "Prometheus counted every catalog dispatch (GET and HEAD)");
  require(errors != nullptr, "no catalog 5xx");
  // The hook is keyed by template only, so the POST /v1/networks control
  // (its 202) reports under "/v1/networks" too; Prometheus labels carry the
  // method and count only the GET registration.
  require(controls == 1 && hooked == dispatched + controls,
          "the completion hook saw every catalog dispatch plus the POST control");
  require(untagged == 0, "every catalog 200 carried its authenticated device");
  printf("accounting: %llu catalog dispatches counted by Prometheus and the completion hook (%llu 200s, each "
         "device-tagged; the POST control reported apart), no 5xx\n",
         counted, static_cast<unsigned long long>(ok));
}

}  // namespace

int main() try {
  char directory[] = "/tmp/fortis-network-catalog-lock.XXXXXX";
  require(mkdtemp(directory) != nullptr, "temporary directory");
  fortis::storage::RuntimeKvSetDirectory(directory);
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  require(kv != nullptr, "process KV opens");
  fortis::auth::DeviceAuthStore auth(kv);
  const int64_t now = wall_ms();
  require(auth.SeedPriceRouteFixture(kTokenA, kIdA, now) && auth.SeedPriceRouteFixture(kTokenB, kIdB, now) &&
              auth.SeedPriceRouteFixture(kTokenC, kIdC, now) && auth.SeedPriceRouteFixture(kTokenD, kIdD, now) &&
              auth.SeedPriceRouteFixture(kTokenE, kIdE, now) && auth.SeedPriceRouteFixture(kTokenF, kIdF, now) &&
              auth.SeedPriceRouteFixture(kTokenR, kIdR, now),
          "fixture devices seeded");
  require(auth.Revoke(kIdR, now), "device R revoked");

  NativeConfig config{};
  NativeNetworkRegistry registry;
  NetworkRouteContext context{&registry, &config};
  context.planes.token = true;
  context.planes.prices_spot = true;

  // The expected documents, generated independently of the route's buffers.
  static char json[32768];
  static uint8_t proto[fortisnet_intelligence_v1_NetworksResponse_size];
  size_t json_length = 0, proto_length = 0;
  require(BuildNetworksDocuments(registry, config, context.planes, json, sizeof(json), &json_length, proto,
                                 sizeof(proto), &proto_length),
          "documents build");
  g_documents.json.assign(json, json_length);
  g_documents.protobuf.assign(reinterpret_cast<const char*>(proto), proto_length);
  {
    tj::Arena arena = tj::GrowingArena(16u << 20);
    const tj::Value* document = tj::Parse(arena, reinterpret_cast<const uint8_t*>(json), json_length);
    const tj::Value* networks = tj::Get(document, "networks");
    require(networks && networks->type == tj::Type::kArray && networks->count == 43, "43 catalogued networks");
    require(g_documents.json.find("\"prices\":{\"spot\":true,\"history\":false,") != std::string::npos,
            "prices follow the planes");
    tj::FreeArena(arena);
  }
  require(proto_length > 1000, "protobuf document");

  HttpRouter router;
  g_router = &router;
  router.SetCompletionHook(completion_hook, nullptr);
  Gate holder;
  require(router.Add("GET", "/v1/slow", gated_response, &holder), "slow route");
  require(RegisterNetworksRoute(router, &context), "networks route");
  require(router.Add("POST", "/v1/networks", immediate_response) &&
              router.Add("GET", "/v1/networks/:network/overview", immediate_response),
          "sibling stand-ins");
  DeviceEnrollmentConfig devices{};
  require(RegisterDeviceRotationRoute(router, &devices), "rotation route");

  // Without contention first: the route answers the startup document.
  require_catalog(catalog("GET", read_headers(kTokenA, "application/json")), false, false);
  require_catalog(catalog("GET", read_headers(kTokenA, "application/protobuf")), false, true);

  KvRacer racer;
  pthread_t race_thread;
  require(pthread_create(&race_thread, nullptr, kv_race_thread, &racer) == 0, "racer starts");
  test_reads_under_held_lock(racer, holder);
  test_reads_during_churn(racer);
  racer.stop.store(true);
  pthread_join(race_thread, nullptr);
  test_rotation_during_read();
  test_accounting();

  g_router = nullptr;
  fortis::storage::RuntimeKvStoreIfOpen()->Close();
  remove_directory(directory);
  puts("network_catalog_concurrency_test: GET/HEAD /v1/networks run without the route lock; siblings keep it");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
