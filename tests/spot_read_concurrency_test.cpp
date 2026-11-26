#include "test_support.h"
// GET and HEAD /v1/prices run without the router's route_state_mutex
// (spot_prices_run_without_state_lock in src/http/http_server.cpp).
// Host-only, in process, over socketpairs and a real process KV, with the
// real spot handler (RegisterPriceSpotRoute), router, response handoff and a
// real SpotProviderOwner (StartRedirected). The owner's provider target is a
// loopback "black hole": it accepts TLS connections and never answers, so a
// cold id stays in flight until the test closes them. No live provider,
// guest or Docker:
//  - churn: cached reads (GET/HEAD, JSON/protobuf) on four threads race
//    rotations of another device through the locked rotation route and KV
//    writes/sweeps/compactions; every reply is the exact cached document;
//  - rotation: a GET paused inside device authentication while the locked
//    rotation route commits answers 401 SESSION_EXPIRED (it could not if the
//    GET held the lock); the old bearer is refused and the new one reads;
//  - held lock: while an unrelated locked route holds the lock, cached reads
//    answer byte-exact within 2 s, eight in parallel, beside KV churn, and
//    each is recorded once as demand; device policy, query 400s and the
//    device quota (remaining 1, 0, then 429) answer too;
//  - owner lifecycle, still under the held lock: cold reads reserve their
//    socket and leave the handler while the provider is in flight; an owner
//    deadline answers 200 from the (empty) floor; a handoff deadline answers
//    504 and the owner keeps its reference; a full owner and a full handoff
//    answer 503 UNAVAILABLE; a client disconnect drops only the handoff
//    reference; owner Stop answers 503 SHUTDOWN and a stopped owner 503
//    UNAVAILABLE. RequestSlot references are checked at every step and all
//    return to zero (each terminal path released exactly once);
//  - negative controls: POST /v1/prices, GET|HEAD|POST /v1/prices/history,
//    GET /v1/prices/ohlc, POST /v1/prices/markets and POST
//    /v1/devices/rotate wait for the held lock throughout;
//  - accounting: Prometheus and the completion hook saw every spot dispatch
//    exactly once (the disconnected one is never reported), each 200 tagged
//    with its device.
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
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
#include <vector>

#include "env_clock.h"
#include "coin_popularity.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "fake_seams.h"
#include "http_server.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "native_config.h"
#include "price_series_store.h"
#include "price_spot_cache.h"
#include "price_spot_route.h"
#include "spot_provider_owner.h"
#include "spot_owner_factory_support.h"

using namespace fortis::auth;
using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::prices;

namespace {

// Development-tier fixture devices (SeedPriceRouteFixture, 240/minute).
constexpr char kTokenA[] = "fintel_dev_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  // cached reads
constexpr char kTokenB[] = "fintel_dev_BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";  // edges, owner
constexpr char kTokenC[] = "fintel_dev_CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";  // quota
constexpr char kTokenD[] = "fintel_dev_DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD";  // rotation
constexpr char kTokenE[] = "fintel_dev_EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE";  // churned
constexpr char kTokenR[] = "fintel_dev_RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR";  // revoked
constexpr char kTokenU[] = "fintel_dev_UUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUU";  // unknown
constexpr char kIdA[] = "01JZ00000000000000000000SA";
constexpr char kIdB[] = "01JZ00000000000000000000SB";
constexpr char kIdC[] = "01JZ00000000000000000000SC";
constexpr char kIdD[] = "01JZ00000000000000000000SD";
constexpr char kIdE[] = "01JZ00000000000000000000SE";
constexpr char kIdR[] = "01JZ00000000000000000000SR";
constexpr uint32_t kDevelopmentQuota = 240;
// The cached read: two seeded CoinGecko ids, an alias of one, and an id the
// price-id grammar refuses (answered in `invalid` without any provider work).
constexpr char kCachedPath[] = "/v1/prices?ids=bitcoin,ethereum,%20ETHEREUM%20,eth:erc20:nope";

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

template <typename Predicate>
bool eventually(Predicate predicate, int milliseconds) {
  for (int i = 0; i < milliseconds; ++i) {
    if (predicate()) return true;
    pause_ms(1);
  }
  return predicate();
}

// ---- HTTP plumbing ---------------------------------------------------------------

struct DispatchArgs {
  HttpRouter* router;
  int client;
  std::atomic<bool> returned{false};
};

void* dispatch_thread(void* opaque) {
  auto* args = static_cast<DispatchArgs*>(opaque);
  // A reserved socket belongs to the handoff once Dispatch returns true.
  if (!args->router->Dispatch(args->client)) close(args->client);
  args->returned.store(true);
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
  // The write side stays open: a half-closed peer would read as a
  // disconnect to the handoff's reserved-socket check.
  pending->args.router = router;
  pending->args.client = pair[0];
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

Reply parse(const std::string& raw) {
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

std::string read_all(int fd) {
  std::string raw;
  char buffer[65536];
  for (;;) {
    const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) break;
    raw.append(buffer, static_cast<size_t>(got));
  }
  return raw;
}

Reply finish(Pending* pending) {
  const std::string raw = read_all(pending->peer);
  pthread_join(pending->thread, nullptr);
  close(pending->peer);
  return parse(raw);
}

Reply finish_within(Pending* pending, int milliseconds, const char* what) {
  require(readable_within(pending->peer, milliseconds), what);
  return finish(pending);
}

// Waits until the handler has returned (the socket is reserved or answered).
bool returned_within(Pending* pending, int milliseconds) {
  return eventually([&] { return pending->args.returned.load(); }, milliseconds);
}

// Holds until released (at most 30 s): the unrelated slow locked route.
struct Gate {
  std::atomic<int> entered{0};
  std::atomic<bool> release{false};
};

void gated_response(int client, const HttpRequest&, bool head, void* context) {
  auto* gate = static_cast<Gate*>(context);
  gate->entered.fetch_add(1);
  for (int i = 0; i < 30000 && !gate->release.load(); ++i) pause_ms(1);
  WriteHttpResponse(client, JsonResponse(200, "OK", "{}"), head);
}

// Stand-ins at the spot route's sibling registrations: they answer at once,
// so only the route lock can hold them back. 202 tells them apart in the
// completion hook, which is keyed by template only.
void immediate_response(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, JsonResponse(202, "Accepted", "{\"standIn\":true}"), head);
}

// ---- the provider black hole ---------------------------------------------------------

// A loopback listener the owner's provider requests connect to. While
// holding, accepted connections stay open and silent (the TLS handshake
// waits); released, every held and later connection is closed at once.
struct Blackhole {
  int listener = -1;
  char port[6]{};
  pthread_t thread{};
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  std::vector<int> held;
  bool holding = true;
  std::atomic<bool> stop{false};
  std::atomic<int> accepted{0};

  void Hold(bool hold) {
    pthread_mutex_lock(&mutex);
    holding = hold;
    if (!hold) {
      for (int fd : held) close(fd);
      held.clear();
    }
    pthread_mutex_unlock(&mutex);
  }
};

void* blackhole_thread(void* opaque) {
  auto* hole = static_cast<Blackhole*>(opaque);
  while (!hole->stop.load()) {
    if (!readable_within(hole->listener, 20)) continue;
    const int fd = accept(hole->listener, nullptr, nullptr);
    if (fd < 0) continue;
    hole->accepted.fetch_add(1);
    pthread_mutex_lock(&hole->mutex);
    if (hole->holding) hole->held.push_back(fd);
    else close(fd);
    pthread_mutex_unlock(&hole->mutex);
  }
  return nullptr;
}

void start_blackhole(Blackhole* hole) {
  hole->listener = socket(AF_INET, SOCK_STREAM, 0);
  require(hole->listener >= 0, "black hole socket");
  const int on = 1;
  setsockopt(hole->listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(hole->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
              listen(hole->listener, 16) == 0,
          "black hole listens");
  socklen_t length = sizeof(address);
  require(getsockname(hole->listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "black hole port");
  snprintf(hole->port, sizeof(hole->port), "%u", static_cast<unsigned>(ntohs(address.sin_port)));
  require(pthread_create(&hole->thread, nullptr, blackhole_thread, hole) == 0, "black hole thread");
}

void stop_blackhole(Blackhole* hole) {
  hole->Hold(false);
  hole->stop.store(true);
  pthread_join(hole->thread, nullptr);
  close(hole->listener);
}

bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }

// ---- KV churn --------------------------------------------------------------------------

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
    // Expiries far below every device and price record, so sweeping at them
    // removes only these keys.
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

// ---- accounting -------------------------------------------------------------------------

// Completion telemetry for the spot template, from dispatch and handoff
// writer threads alike.
struct Accounting {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  std::map<int, uint64_t> statuses;
  uint64_t untagged_ok = 0;
  uint64_t dispatched = 0;   // spot requests this test sent
  uint64_t unreported = 0;   // spot requests whose client disconnected first
};
Accounting g_accounting;

void completion_hook(void*, const char* route, int status, const char* device_id, double) {
  if (strcmp(route, "/v1/prices") != 0) return;
  pthread_mutex_lock(&g_accounting.mutex);
  ++g_accounting.statuses[status];
  if (status == 200 && (!device_id || strlen(device_id) != 26)) ++g_accounting.untagged_ok;
  pthread_mutex_unlock(&g_accounting.mutex);
}

void count_dispatch() {
  pthread_mutex_lock(&g_accounting.mutex);
  ++g_accounting.dispatched;
  pthread_mutex_unlock(&g_accounting.mutex);
}

// ---- the spot route -------------------------------------------------------------------

struct World {
  HttpRouter* router = nullptr;
  SpotProviderOwner* owner = nullptr;
  HttpResponseHandoff* handoff = nullptr;
  fortis::discovery::CoinPopularity* popularity = nullptr;
  Blackhole* hole = nullptr;
  std::string json;      // the cached read's exact JSON body
  std::string protobuf;  // and its protobuf body
};
World g;

std::string bearer(const char* token) { return std::string("Authorization: Bearer ") + token + "\r\n"; }

std::string read_headers(const char* token, const char* accept) {
  return bearer(token) + "Accept: " + accept + "\r\n";
}

void start_spot(const char* method, const std::string& path, const std::string& headers, Pending* pending) {
  count_dispatch();
  start(g.router, method, path, headers, pending);
}

Reply spot(const char* method, const std::string& path, const std::string& headers) {
  Pending pending;
  start_spot(method, path, headers, &pending);
  return finish(&pending);
}

// A request that must answer within 2 s; fails fast otherwise (for example
// while it waits on a held route lock).
Reply spot_within(const char* method, const std::string& path, const std::string& headers, const char* what) {
  Pending pending;
  start_spot(method, path, headers, &pending);
  return finish_within(&pending, 2000, what);
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

void require_remaining(const Reply& reply, int remaining) {
  const std::string left = reply.header("x-rate-limit-remaining");
  require(left != "<absent>" && static_cast<uint32_t>(fortis::test::ParseInteger<int>(left)) < kDevelopmentQuota,
          "x-rate-limit-remaining below the quota");
  if (remaining >= 0) require(left == std::to_string(remaining), "exact x-rate-limit-remaining");
}

// A 200: `expected` is the full body (HEAD: none is sent, Content-Length
// still names it). `remaining` < 0 accepts any value below the quota.
void require_prices(const Reply& reply, bool head, bool protobuf, const std::string& expected, int remaining = -1) {
  if (reply.status != 200 || (!head && reply.body != expected))
    fprintf(stderr, "got %d %s\nwant %s\n", reply.status, reply.body.c_str(), expected.c_str());
  require(reply.status == 200, "prices 200");
  require_baseline(reply);
  require(reply.header("content-type") ==
              (protobuf ? "application/protobuf" : "application/json; charset=utf-8"),
          "prices content type");
  require(reply.header("content-length") == std::to_string(expected.size()), "prices Content-Length");
  require(reply.header("vary") == "Accept", "prices Vary: Accept");
  require(reply.header("x-rate-limit-cost") == "1", "prices x-rate-limit-cost");
  require_remaining(reply, remaining);
  require(reply.header("retry-after") == "<absent>", "prices has no Retry-After");
  // The ten route headers plus the router's Server-Timing and HTTP Date.
  require(reply.headers.size() == 12, "prices carries exactly the documented headers");
  require(reply.body == (head ? std::string() : expected), "prices body (HEAD: none)");
}

void require_cached(const Reply& reply, bool head, bool protobuf, int remaining = -1) {
  require_prices(reply, head, protobuf, protobuf ? g.protobuf : g.json, remaining);
}

// Any JSON refusal. `quota` < 0: no rate-limit headers (device policy and
// handoff replies); otherwise x-rate-limit-cost 1 and that remaining (or any
// remaining below the quota for quota == kDevelopmentQuota).
void require_refusal(const Reply& reply, int status, const char* body, bool head, int quota) {
  if (reply.status != status || reply.body != (head ? "" : body)) {
    fprintf(stderr, "got %d %s, want %d %s\n", reply.status, reply.body.c_str(), status, body);
    require(false, "refusal");
  }
  require_baseline(reply);
  require(reply.header("content-type") == "application/json; charset=utf-8", "refusal content type");
  require(reply.header("content-length") == std::to_string(strlen(body)), "refusal Content-Length");
  require(reply.header("vary") == "<absent>", "refusal has no Vary");
  if (quota < 0) {
    require(reply.header("x-rate-limit-cost") == "<absent>" &&
                reply.header("x-rate-limit-remaining") == "<absent>",
            "refusal carries no rate-limit headers");
  } else {
    require(reply.header("x-rate-limit-cost") == "1", "refusal x-rate-limit-cost");
    require_remaining(reply, quota == static_cast<int>(kDevelopmentQuota) ? -1 : quota);
  }
}

constexpr int kAnyRemaining = static_cast<int>(kDevelopmentQuota);

std::string missing_body(const char* id) {
  return std::string("{\"fiat\":\"usd\",\"prices\":{},\"missing\":[\"") + id + "\"],\"invalid\":[]}";
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

// RequestSlot references in table order (price_spot_route.cpp test seam).
std::vector<unsigned> slot_references() {
  unsigned references[HttpResponseHandoff::kCapacity]{};
  const size_t used = PriceSpotRouteSlotsForTest(references, HttpResponseHandoff::kCapacity);
  require(used <= HttpResponseHandoff::kCapacity, "slot count bounded");
  return std::vector<unsigned>(references, references + used);
}

// `ones` slots with one reference and `twos` with two, nothing else.
bool slots_are(size_t ones, size_t twos) {
  size_t one = 0, two = 0, other = 0;
  for (unsigned references : slot_references())
    (references == 1 ? one : references == 2 ? two : other)++;
  return one == ones && two == twos && other == 0;
}

double popularity_hits(const char* id) {
  fortis::discovery::PopularityRowList rows;
  require(g.popularity->Snapshot(&rows), "popularity snapshot");
  double hits = 0;
  for (size_t i = 0; i < rows.count; ++i)
    if (rows.items[i].id_length == strlen(id) && memcmp(rows.items[i].id, id, strlen(id)) == 0)
      hits = rows.items[i].state.hits;
  rows.Release();
  return hits;
}

fortis::testing::ManualClock g_fixed_clock(1'800'000'000'000LL);

// Waits, if needed, so the next few requests fall inside one quota minute.
void avoid_minute_boundary() {
  while (wall_ms() % 60000 > 55000) pause_ms(100);
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
    Pending pending;
    start(g.router, "POST", "/v1/devices/rotate", bearer(token.c_str()), &pending);
    const Reply reply = finish(&pending);
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
  // Device A's quota: four readers, at most 12 reads each.
  while (reader->reads < 12 && (!reader->churn->done.load() || reader->reads < 3)) {
    const Read& read = kReads[(reader->index + reader->reads) % 4];
    require_cached(spot(read.method, kCachedPath, read_headers(kTokenA, read.accept)),
                   strcmp(read.method, "HEAD") == 0, read.protobuf);
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
  require(eventually([] { return slot_references().empty(); }, 2000), "churn: every RequestSlot released");
  printf("churn: %d cached spot reads on 4 threads beside 30 rotations of another device and %d KV "
         "compactions; every reply was the exact document\n",
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
  start_spot("GET", kCachedPath, read_headers(kTokenD, "application/json"), &read);
  require(eventually([&] { return hook.paused.load() >= 1; }, 3000), "GET paused inside device authentication");
  // Rotation commits D's new bearer while the GET is inside authentication.
  Pending rotate;
  start(g.router, "POST", "/v1/devices/rotate", bearer(kTokenD), &rotate);
  const bool rotation_answered = readable_within(rotate.peer, 3000);
  hook.resume.store(true);
  require(rotation_answered, "rotation completes while the GET is paused (the GET holds no route lock)");
  const Reply rotated = finish(&rotate);
  const size_t token_at = rotated.body.find("\"deviceToken\":\"");
  require(rotated.status == 200 && token_at != std::string::npos, "rotation 200");
  const std::string fresh = rotated.body.substr(token_at + 15, 54);
  require(fortis::auth::IsDeviceToken(fresh.c_str()) && fresh != kTokenD, "a new device bearer");
  require_refusal(finish(&read), 401, "{\"error\":\"SESSION_EXPIRED\"}", false, -1);
  fortis::auth::SetDeviceRowSnapshotHook(nullptr, nullptr);
  g_hook = nullptr;

  require_refusal(spot("GET", kCachedPath, read_headers(kTokenD, "application/json")), 401,
                  "{\"error\":\"UNAUTHORIZED\"}", false, -1);
  require_cached(spot("HEAD", kCachedPath, read_headers(fresh.c_str(), "application/protobuf")), true, true);
  require_cached(spot("GET", kCachedPath, read_headers(fresh.c_str(), "application/json")), false, false);
  puts("rotation: a GET paused in device auth while rotation committed answered 401 SESSION_EXPIRED; "
       "old bearer 401 UNAUTHORIZED, new bearer reads");
}

// ---- held lock: cached reads, policy and quota -------------------------------------

void cached_reads_under_held_lock(KvRacer& racer) {
  const double hits_before = popularity_hits("bitcoin");
  timespec loop_start{};
  clock_gettime(CLOCK_MONOTONIC, &loop_start);
  int rounds = 0;
  // Until the racer has swept and compacted twice under these reads, but
  // bounded so device A stays under its 240/minute development quota.
  const int compactions_before = racer.compactions.load();
  const uint64_t swept_before = racer.swept.load();
  const uint64_t writes_before = racer.writes.load();
  while (rounds < 10 || racer.compactions.load() < compactions_before + 2 || racer.swept.load() == swept_before) {
    require(rounds < 30 && seconds_since(loop_start) < 15.0, "reads finished within bounds");
    // Keep quota-bounded reads spread across the writer's compaction epochs.
    while (racer.writes.load() < writes_before + static_cast<uint64_t>(rounds) * 8) {
      require(seconds_since(loop_start) < 15.0, "writer progresses within the original deadline");
      pause_ms(1);
    }
    for (const Read& read : kReads) {
      const Reply reply = spot_within(read.method, kCachedPath, read_headers(kTokenA, read.accept),
                                      "spot read answers within 2 s while the lock is held");
      require_cached(reply, strcmp(read.method, "HEAD") == 0, read.protobuf);
    }
    ++rounds;
    pause_ms(5);
  }
  const int reads = rounds * 4;
  // Each read is one foreground Submit naming bitcoin once: one demand hit
  // each (the tracker's clock is fixed, so nothing decays).
  require(popularity_hits("bitcoin") == hits_before + reads, "every unlocked read recorded its demand once");

  // Eight reads in flight at once all answer, JSON and protobuf alternating.
  Pending parallel[8];
  for (int i = 0; i < 8; ++i)
    start_spot(i % 4 == 3 ? "HEAD" : "GET", kCachedPath,
               read_headers(kTokenA, i % 2 ? "application/protobuf" : "*/*"), &parallel[i]);
  for (int i = 0; i < 8; ++i)
    require_cached(finish_within(&parallel[i], 2000, "parallel read answers while the lock is held"), i % 4 == 3,
                   i % 2 == 1);
  printf("held lock: %d cached GET/HEAD JSON/protobuf reads (each recorded once as demand) and 8 parallel reads "
         "answered in %.2f s beside %llu racing KV writes and %d compactions\n",
         reads, seconds_since(loop_start), static_cast<unsigned long long>(racer.writes.load()),
         racer.compactions.load() - compactions_before);
}

void policy_under_held_lock() {
  const char* json = "application/json";
  const std::string unauthorized = "{\"error\":\"UNAUTHORIZED\"}";
  require_refusal(spot_within("GET", kCachedPath, "Accept: application/json\r\n", "no bearer answers"), 401,
                  unauthorized.c_str(), false, -1);
  require_refusal(spot("GET", kCachedPath, "Authorization: Bearer fintel_dev_short\r\n"), 401,
                  unauthorized.c_str(), false, -1);
  require_refusal(spot("GET", kCachedPath, std::string("Authorization: bearer ") + kTokenB + "\r\n"), 401,
                  unauthorized.c_str(), false, -1);
  require_refusal(spot("GET", kCachedPath, read_headers(kTokenU, json)), 401, unauthorized.c_str(), false, -1);
  require_refusal(spot("HEAD", kCachedPath, read_headers(kTokenU, "application/protobuf")), 401,
                  unauthorized.c_str(), true, -1);
  require_refusal(spot("GET", kCachedPath, read_headers(kTokenR, json)), 401, "{\"error\":\"SESSION_EXPIRED\"}",
                  false, -1);
  require_refusal(spot("GET", kCachedPath, read_headers(kTokenB, json) + "Origin: https://wallet.test\r\n"), 403,
                  "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}", false, -1);
  require_refusal(spot("HEAD", kCachedPath, read_headers(kTokenB, json) + "Origin: null\r\n"), 403,
                  "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}", true, -1);
  // Query refusals come after the device quota: they carry its headers.
  const Reply no_ids = spot("GET", "/v1/prices", read_headers(kTokenB, json));
  require(no_ids.status == 400 && no_ids.body == "{\"error\":\"BAD_REQUEST\"}" &&
              no_ids.header("x-rate-limit-cost") == "1",
          "no ids: 400 BAD_REQUEST with quota headers");
  require_refusal(no_ids, 400, no_ids.body.c_str(), false, kAnyRemaining);
  std::string many = "/v1/prices?ids=a0";
  for (int i = 1; i < 101; ++i) many += ",a" + std::to_string(i);
  require_refusal(spot("GET", many, read_headers(kTokenB, json)), 400,
                  "{\"error\":\"BAD_REQUEST\",\"detail\":\"1\xE2\x80\x93" "100 ids\"}", false, kAnyRemaining);
  require_refusal(spot("HEAD", "/v1/prices?ids=" + std::string(513, 'x'), read_headers(kTokenB, json)), 400,
                  "{\"error\":\"BAD_REQUEST\",\"detail\":\"ids are limited to 512 bytes each\"}", true,
                  kAnyRemaining);
  // protobuf with q=0 falls back to JSON.
  require_cached(spot("GET", kCachedPath, read_headers(kTokenB, "application/protobuf;q=0")), false, false);

  // Quota: device C was pre-counted to two below its limit for this minute.
  avoid_minute_boundary();
  uint64_t used = 0;
  char key[96];
  snprintf(key, sizeof(key), "q:dev:%s:%lld", kIdC, static_cast<long long>(wall_ms() / 60000));
  require(fortis::storage::RuntimeKvIncrementByWindow(key, kDevelopmentQuota - 2, wall_ms(), 60000, &used) &&
              used == kDevelopmentQuota - 2,
          "device C pre-counted");
  require_cached(spot_within("GET", kCachedPath, read_headers(kTokenC, json), "quota read 1 answers"), false, false,
                 1);
  require_cached(spot_within("HEAD", kCachedPath, read_headers(kTokenC, "application/protobuf"),
                             "quota read 2 answers"),
                 true, true, 0);
  for (const char* method : {"GET", "HEAD"}) {
    const Reply limited = spot_within(method, kCachedPath, read_headers(kTokenC, json), "429 answers");
    require_refusal(limited, 429, "{\"error\":\"RATE_LIMITED\"}", strcmp(method, "HEAD") == 0, 0);
    require(limited.header("retry-after") == "60", "429 Retry-After: 60");
  }
  puts("held lock: 8 device-policy refusals, 3 query 400s with quota headers, q=0 fallback and the quota "
       "boundary (remaining 1, 0, then 429 with Retry-After: 60) answered");
}

// ---- held lock: the owner and handoff lifecycle ------------------------------------

void* stop_owner_thread(void* opaque) {
  static_cast<SpotProviderOwner*>(opaque)->Stop();
  return nullptr;
}

std::string cold_path(int index) { return "/v1/prices?ids=cold-" + std::to_string(index); }


void owner_lifecycle_under_held_lock() {
  const char* json = "application/json";
  const std::string unavailable = "{\"error\":\"UNAVAILABLE\"}";
  require(slot_references().empty() && g.owner->active_request_count() == 0, "owner window starts idle");

  // Six cold reads: each handler reserves its socket, submits and returns
  // while the provider call sits in the black hole and the lock is held.
  Pending held[6];
  for (int i = 0; i < 6; ++i)
    start_spot(i % 2 ? "HEAD" : "GET", cold_path(i), read_headers(kTokenB, i % 3 ? "application/protobuf" : json),
               &held[i]);
  for (Pending& pending : held) {
    require(returned_within(&pending, 2000), "cold read handler returns (socket reserved) while the lock is held");
    require(!readable_within(pending.peer, 0), "cold read is still waiting on its provider");
  }
  require(eventually([] { return g.owner->active_request_count() == 6; }, 2000), "six requests in the owner");
  require(eventually([] { return g.hole->accepted.load() >= 1; }, 3000), "the provider call is in flight");
  require(slots_are(0, 6), "each held slot: an owner and a handoff reference");

  // An owner deadline answers through the handoff from the cache and
  // durable floor (empty here): 200 with the id missing, while the provider
  // is still in flight.
  SetPriceSpotRouteDeadlinesForTest(5000, 800);
  Pending fallback;
  start_spot("GET", cold_path(6), read_headers(kTokenB, json), &fallback);
  timespec fallback_start{};
  clock_gettime(CLOCK_MONOTONIC, &fallback_start);
  const Reply fallback_reply = finish_within(&fallback, 3000, "owner deadline answers");
  require(seconds_since(fallback_start) >= 0.7, "the owner answered at its deadline, not before");
  require_prices(fallback_reply, false, false, missing_body("cold-6"));

  // A handoff deadline shorter than the owner's: the writer answers its
  // generic 504 and drops the handoff reference; the owner keeps its own.
  SetPriceSpotRouteDeadlinesForTest(600, 20000);
  Pending expired;
  start_spot("HEAD", cold_path(7), read_headers(kTokenB, json), &expired);
  const Reply timeout = finish_within(&expired, 3000, "handoff deadline answers");
  require(timeout.status == 504 && timeout.body.empty() && timeout.header("content-length") == "19" &&
              timeout.header("x-rate-limit-cost") == "<absent>",
          "handoff deadline: generic 504 TIMEOUT (HEAD)");
  require(eventually([] { return slots_are(1, 6); }, 2000), "timed-out slot keeps only the owner reference");
  // Keep the remaining cold requests pending while testing slot exhaustion.
  SetPriceSpotRouteDeadlinesForTest(kSpotProviderOwnerMaxDeadlineMs,
                                   kSpotProviderOwnerMaxDeadlineMs - 1000);

  // The eighth owner request fills the owner; the next read (even a cached
  // one) is refused 503 UNAVAILABLE through its reservation.
  Pending eighth;
  start_spot("GET", cold_path(8), read_headers(kTokenB, json), &eighth);
  require(returned_within(&eighth, 2000) &&
              eventually([] { return g.owner->active_request_count() == kSpotProviderOwnerMaxRequests; }, 2000),
          "the owner is full");
  require_refusal(spot_within("GET", kCachedPath, read_headers(kTokenB, json), "owner-full read answers"), 503,
                  unavailable.c_str(), false, kAnyRemaining);
  require(slots_are(1, 7), "the refused read released its slot");

  // Fill the rest of the 16-entry handoff directly (seven held reads own
  // seven entries): the next read cannot reserve and is refused at once.
  int fillers[HttpResponseHandoff::kCapacity][2];
  uint64_t tokens[HttpResponseHandoff::kCapacity]{};
  size_t filled = 0;
  for (; filled < HttpResponseHandoff::kCapacity; ++filled) {
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, fillers[filled]) == 0, "filler socketpair");
    if (!g.handoff->Reserve(fillers[filled][0], false, 30000, &tokens[filled])) {
      close(fillers[filled][0]);
      close(fillers[filled][1]);
      break;
    }
  }
  require(filled == HttpResponseHandoff::kCapacity - 7, "nine filler reservations fill the handoff");
  require_refusal(spot_within("HEAD", kCachedPath, read_headers(kTokenB, json), "handoff-full read answers"), 503,
                  unavailable.c_str(), true, kAnyRemaining);
  require(slots_are(1, 7), "the unreserved read released both its references");
  for (size_t i = 0; i < filled; ++i) {
    require(g.handoff->Complete(tokens[i], JsonResponse(200, "OK", "{\"filler\":true}")), "filler completes");
    const Reply reply = parse(read_all(fillers[i][1]));
    require(reply.status == 200 && reply.body == "{\"filler\":true}", "filler reply");
    close(fillers[i][1]);
  }

  // A client disconnect: the handoff's terminal drops only its reference.
  close(held[0].peer);
  held[0].peer = -1;
  pthread_join(held[0].thread, nullptr);
  require(eventually([] { return slots_are(2, 6); }, 2000), "the disconnected read keeps only the owner reference");
  pthread_mutex_lock(&g_accounting.mutex);
  ++g_accounting.unreported;
  pthread_mutex_unlock(&g_accounting.mutex);

  // Owner shutdown while the provider is still in flight: every waiting
  // read answers 503 SHUTDOWN through the handoff; the disconnected and the
  // timed-out slots release their last (owner) reference without a reply.
  pthread_t stopper;
  require(pthread_create(&stopper, nullptr, stop_owner_thread, g.owner) == 0, "owner stop starts");
  for (int i = 1; i < 6; ++i)
    require_refusal(finish_within(&held[i], 3000, "owner shutdown answers the held read"), 503,
                    "{\"error\":\"SHUTDOWN\"}", i % 2 == 1, kAnyRemaining);
  require_refusal(finish_within(&eighth, 3000, "owner shutdown answers the eighth read"), 503,
                  "{\"error\":\"SHUTDOWN\"}", false, kAnyRemaining);
  // The provider attempts are cancelled; release them so Stop's join cannot
  // wait for their 10 s attempt timeout.
  g.hole->Hold(false);
  pthread_join(stopper, nullptr);
  require(eventually([] { return slot_references().empty(); }, 2000),
          "every RequestSlot released exactly once its last terminal ran");

  // A stopped owner refuses the submission: 503 UNAVAILABLE through the
  // reservation, and the slot is released by both paths.
  require_refusal(spot_within("GET", kCachedPath, read_headers(kTokenB, json), "stopped-owner read answers"), 503,
                  unavailable.c_str(), false, kAnyRemaining);
  require(eventually([] { return slot_references().empty(); }, 2000), "stopped-owner slot released");
  printf("owner lifecycle under the held lock: 8 cold reads reserved and left their handlers; owner deadline 200 "
         "(missing), handoff deadline 504, owner-full and handoff-full 503 UNAVAILABLE, disconnect, owner Stop "
         "503 SHUTDOWN x6 and stopped-owner 503; RequestSlot references 2/1/0 as expected (%d provider "
         "connections held)\n",
         g.hole->accepted.load());
}

void test_under_held_lock(KvRacer& racer, Gate& holder) {
  Pending held;
  start(g.router, "GET", "/v1/slow", "", &held);
  require(eventually([&] { return holder.entered.load() >= 1; }, 5000), "slow locked route entered");

  // Negative controls, started while the lock is held: none may answer.
  struct Control {
    const char* method;
    const char* path;
    Pending pending;
  };
  Control controls[] = {
      {"POST", "/v1/prices", {}},          {"GET", "/v1/prices/history?id=bitcoin", {}},
      {"HEAD", "/v1/prices/history", {}},  {"POST", "/v1/prices/history", {}},
      {"GET", "/v1/prices/ohlc", {}},      {"POST", "/v1/prices/markets", {}},
  };
  for (Control& control : controls)
    start(g.router, control.method, control.path, bearer(kTokenB), &control.pending);

  cached_reads_under_held_lock(racer);
  policy_under_held_lock();
  owner_lifecycle_under_held_lock();

  // The controls are still waiting on the lock the slow route holds.
  require(!readable_within(controls[0].pending.peer, 300), "POST /v1/prices stays locked");
  for (Control& control : controls) {
    if (readable_within(control.pending.peer, 0)) fprintf(stderr, "%s %s answered\n", control.method, control.path);
    require(!readable_within(control.pending.peer, 0), "sibling price routes stay locked");
  }
  require(holder.entered.load() == 1, "the slow route is still the only lock holder");

  holder.release.store(true);
  require(finish(&held).status == 200, "slow route completes");
  for (Control& control : controls) {
    const Reply reply = finish(&control.pending);
    require(reply.status == 202 && reply.body == (strcmp(control.method, "HEAD") ? "{\"standIn\":true}" : ""),
            "sibling stand-in completes after the lock is released");
  }
  // POST /v1/prices reports under the "/v1/prices" template (202).
  puts("held lock: POST /v1/prices, GET|HEAD|POST /v1/prices/history, GET /v1/prices/ohlc, POST "
       "/v1/prices/markets stayed locked until the slow route released it");
}

// ---- handoff shutdown and accounting ------------------------------------------------

void test_handoff_stopped() {
  // A stopped handoff cannot reserve: the handler answers 503 itself.
  g.handoff->Stop();
  require_refusal(spot_within("GET", kCachedPath, read_headers(kTokenA, "application/json"),
                              "stopped-handoff read answers"),
                  503, "{\"error\":\"UNAVAILABLE\"}", false, kAnyRemaining);
  require(slot_references().empty(), "stopped-handoff slot released");
}

void test_accounting() {
  static char metrics[HttpRouter::kMetricsOutputCapacity];
  const size_t length = g.router->RenderPrometheus(metrics, sizeof(metrics));
  require(length > 0, "metrics render");
  const char* prefix = "fortis_native_http_requests_total{route=\"GET /v1/prices\"} ";
  const char* line = strstr(metrics, prefix);
  require(line != nullptr, "spot requests counter");
  const unsigned long long counted = strtoull(line + strlen(prefix), nullptr, 10);
  const char* error_prefix = "fortis_native_http_errors_total{route=\"GET /v1/prices\"} ";
  const char* errors_line = strstr(metrics, error_prefix);
  require(errors_line != nullptr, "spot errors counter");
  const unsigned long long errors = strtoull(errors_line + strlen(error_prefix), nullptr, 10);
  pthread_mutex_lock(&g_accounting.mutex);
  uint64_t hooked = 0;
  for (const auto& entry : g_accounting.statuses) hooked += entry.second;
  const uint64_t dispatched = g_accounting.dispatched;
  const uint64_t unreported = g_accounting.unreported;
  std::map<int, uint64_t> statuses = g_accounting.statuses;
  const uint64_t untagged = g_accounting.untagged_ok;
  pthread_mutex_unlock(&g_accounting.mutex);
  require(counted == dispatched, "Prometheus counted every spot dispatch (GET and HEAD)");
  // Reserved replies are recorded at dispatch as accepted (no status); only
  // the handoff-full and stopped-handoff refusals are 5xx at dispatch.
  require(errors == 2, "the two unreserved 503s are the only dispatch-time spot errors");
  // The hook is keyed by template only, so the POST /v1/prices control (its
  // 202) reports under "/v1/prices" too. The disconnected read is never
  // reported; every other dispatch exactly once, reserved ones by the writer.
  require(statuses[202] == 1 && hooked == dispatched - unreported + 1,
          "the completion hook saw every spot dispatch once, plus the POST control");
  require(statuses[504] == 1 && statuses[503] == 10 && statuses[429] == 2,
          "terminal statuses: one 504, ten 503 (2 unavailable + 6 shutdown + stopped owner + stopped handoff), "
          "two 429");
  require(untagged == 0, "every spot 200 carried its authenticated device");
  printf("accounting: %llu spot dispatches counted by Prometheus; the completion hook saw each once except the "
         "disconnected one (%llu 200s, each device-tagged; %llu 503, %llu 504), 2 dispatch-time 5xx\n",
         counted, static_cast<unsigned long long>(statuses[200]), static_cast<unsigned long long>(statuses[503]),
         static_cast<unsigned long long>(statuses[504]));
}

}  // namespace

int main(int argc, char** argv) try {
  // Any parseable CA: the black hole never completes a handshake.
  const char* ca = argc > 1 ? argv[1] : ".deps/mbedtls-3.6.7/framework/data_files/test-ca.crt";
  require(access(ca, R_OK) == 0, "CA bundle readable (run from the repository root or pass its path)");
  // The reads' round and time bounds assume the racing KV thread's fsyncs are
  // cheap. Where they are not (a container disk), FORTIS_TEST_TMPDIR names a
  // memory-backed directory (scripts/sanitize.py sets it).
  const char* base = getenv("FORTIS_TEST_TMPDIR");
  char directory[256];
  require(snprintf(directory, sizeof(directory), "%s/fortis-spot-read-lock.XXXXXX", base && base[0] ? base : "/tmp") <
              static_cast<int>(sizeof(directory)),
          "temporary directory name");
  require(mkdtemp(directory) != nullptr, "temporary directory");
  fortis::storage::RuntimeKvSetDirectory(directory);
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  require(kv != nullptr, "process KV opens");
  fortis::auth::DeviceAuthStore auth(kv);
  const int64_t now = wall_ms();
  require(auth.SeedPriceRouteFixture(kTokenA, kIdA, now) && auth.SeedPriceRouteFixture(kTokenB, kIdB, now) &&
              auth.SeedPriceRouteFixture(kTokenC, kIdC, now) && auth.SeedPriceRouteFixture(kTokenD, kIdD, now) &&
              auth.SeedPriceRouteFixture(kTokenE, kIdE, now) && auth.SeedPriceRouteFixture(kTokenR, kIdR, now),
          "fixture devices seeded");
  require(auth.Revoke(kIdR, now), "device R revoked");

  // Fresh for ten minutes: every cached read below is the same document.
  fortis::prices::PriceSpotCache cache(*kv);
  fortis::prices::PriceSeriesStore series(*kv);
  const fortis::prices::PriceSpotQuoteInput quotes[] = {
      {"bitcoin", 65000.5, 1.25, 0, true, false, now, 600000},
      {"ethereum", 3200.25, -0.5, 0, true, false, now, 600000},
  };
  require(cache.StoreProviderBatch("usd", "coingecko", quotes, 2, nullptr, 0, true, now) ==
              fortis::prices::PriceSpotStoreStatus::kStored,
          "cache seeded");

  Blackhole hole;
  start_blackhole(&hole);
  fortis::discovery::CoinPopularityOptions popularity_options;
  popularity_options.clock = &g_fixed_clock;
  fortis::discovery::CoinPopularity* popularity = fortis::discovery::CoinPopularity::Create(popularity_options).ValueOr(nullptr).release();
  require(popularity != nullptr, "popularity tracker");
  NativeConfig config{};
  config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", ca);
  auto owner = fortis::testing::StartTestSpotOwner(cache, series, config, nullptr,
      &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
        return startup.StartRedirected("localhost", hole.port, ca, allow_loopback, nullptr);
      }, popularity);
  HttpResponseHandoff handoff;
  require(handoff.Start(), "handoff starts");

  HttpRouter router;
  router.SetResponseHandoff(&handoff);
  router.SetCompletionHook(completion_hook, nullptr);
  Gate holder;
  PriceSpotRouteContext route{&owner, &handoff, false};
  require(router.Add("GET", "/v1/slow", gated_response, &holder), "slow route");
  require(RegisterPriceSpotRoute(router, &route), "spot route");
  require(router.Add("POST", "/v1/prices", immediate_response) &&
              router.Add("GET", "/v1/prices/history", immediate_response) &&
              router.Add("POST", "/v1/prices/history", immediate_response) &&
              router.Add("GET", "/v1/prices/ohlc", immediate_response) &&
              router.Add("POST", "/v1/prices/markets", immediate_response),
          "sibling stand-ins");
  DeviceEnrollmentConfig devices{};
  require(RegisterDeviceRotationRoute(router, &devices), "rotation route");
  g = World{&router, &owner, &handoff, popularity, &hole, {}, {}};

  // Without contention first: the cached read's two documents.
  {
    const Reply json = spot("GET", kCachedPath, read_headers(kTokenA, "application/json"));
    const Reply protobuf = spot("GET", kCachedPath, read_headers(kTokenA, "application/protobuf"));
    require(json.status == 200 && protobuf.status == 200, "baseline reads");
    g.json = json.body;
    g.protobuf = protobuf.body;
    require_cached(json, false, false);
    require_cached(protobuf, false, true);
    char as_of[32];
    snprintf(as_of, sizeof(as_of), "%lld", static_cast<long long>(now));
    require(g.json.compare(0, 23, "{\"fiat\":\"usd\",\"prices\":") == 0 &&
                g.json.find("\"bitcoin\":{\"sourceId\":\"bitcoin\",\"price\":65000.5,\"change24hPct\":1.25") !=
                    std::string::npos &&
                g.json.find("\"ethereum\":{\"sourceId\":\"ethereum\",\"price\":3200.25") != std::string::npos &&
                g.json.find("\"ETHEREUM\":{\"sourceId\":\"ethereum\"") != std::string::npos &&
                g.json.find("\"stale\":false") != std::string::npos &&
                g.json.find("\"missing\":[],\"invalid\":[\"eth:erc20:nope\"]}") != std::string::npos,
            "cached JSON document: both seeded prices, the alias keyed as sent, the invalid id");
    require(g.protobuf.size() > 40 && g.protobuf.find("bitcoin") != std::string::npos &&
                g.protobuf.find("ETHEREUM") != std::string::npos,
            "cached protobuf document");
    require(hole.accepted.load() == 0, "cached reads made no provider call");
  }

  KvRacer racer;
  pthread_t race_thread;
  require(pthread_create(&race_thread, nullptr, kv_race_thread, &racer) == 0, "racer starts");
  test_reads_during_churn(racer);
  test_rotation_during_read();
  test_under_held_lock(racer, holder);
  racer.stop.store(true);
  pthread_join(race_thread, nullptr);
  test_handoff_stopped();
  test_accounting();

  stop_blackhole(&hole);
  fortis::discovery::CoinPopularity::Destroy(popularity);
  fortis::storage::RuntimeKvStoreIfOpen()->Close();
  remove_directory(directory);
  puts("spot_read_concurrency_test: GET/HEAD /v1/prices run without the route lock; siblings keep it");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
