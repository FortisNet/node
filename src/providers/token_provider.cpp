#include <algorithm>
#include "exception_boundary.h"
#include "token_provider.h"
#include "ownership.h"

#include "outbound_https.h"
#include "provider_execution_coordinator.h"
#include "threading.h"
#include "upstream_access.h"
#include "upstream_redirect.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::providers {

namespace {
constexpr size_t kClients = 64;
constexpr size_t kFlights = 24;
constexpr uint64_t kWakeMs = 40;
constexpr uint32_t kTimeoutMs = 10000;
constexpr uint64_t kMaxDeadlineMs = 60000;
constexpr size_t kHostBytes = 254;
constexpr size_t kPortBytes = 6;
constexpr size_t kLaneBytes = 96;

timespec wake_at(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

// ---- the upstream table -------------------------------------------------------
//
// One row per TokenUpstream, in enum order, is everything the owner knows
// about an upstream: its lane, how its requests travel, the host and the
// credential rule. Adding an upstream is its enum value plus its row here
// (and, for a new credential rule, one function in upstream_access.h).

// How a request travels.
enum class Send : uint8_t {
  kGet,          // GET to the upstream's own host
  kNetworkGet,   // GET with the path appended to a network endpoint (joinUrl)
  kPostOnce,     // DriverTransport POST: one endpoint at a time, failed over, never retried in place
  kPostRetried,  // DriverTransport.postJson with retryPost: retried in place like a GET
};

enum Trait : uint8_t {
  kNone = 0,
  // The lane is "{lane}:{endpoint host}" (rpcLane).
  kHostLane = 1 << 0,
  // The POST goes to the endpoint URL itself (DriverTransport.jsonRpc);
  // every other endpoint request appends its path (joinUrl).
  kUrlIsTarget = 1 << 1,
  // Identical in-flight requests never share one job.
  kUnshared = 1 << 2,
  // HttpClient.json retries an unparseable 2xx: the keyless REST clients
  // whose callers take the body as their answer.
  kRetryUnparseable = 1 << 3,
  // An `anonymous` fetch is sent without the deployment's key.
  kAnonymousKeyless = 1 << 4,
};

struct UpstreamOps {
  TokenUpstream upstream;
  const char* lane;
  Send send;
  uint8_t traits;
  // The upstream's fixed host; nullptr: the endpoint's, or `access` picks it.
  const char* host;
  // The deployment's host choice and credential; nullptr: keyless.
  UpstreamAccessFn access;

  bool has(Trait trait) const { return (traits & trait) != 0; }
  // Requests sent as a POST with a JSON body.
  bool posts() const { return send == Send::kPostOnce || send == Send::kPostRetried; }
  // Requests sent to one of the fetch's rpc_urls.
  bool endpoint() const { return send != Send::kGet; }
  // The requests identical in-flight copies may share (a POST's key
  // includes its body).
  bool shareable() const { return !has(kUnshared); }
};

constexpr UpstreamOps kUpstreams[] = {
    {TokenUpstream::kCoinGecko, "coingecko", Send::kGet, kRetryUnparseable, nullptr, CoinGeckoAccess},
    {TokenUpstream::kDexScreener, "dexscreener", Send::kGet, kRetryUnparseable, "api.dexscreener.com", nullptr},
    {TokenUpstream::kGoPlus, "goplus", Send::kGet, kRetryUnparseable, "api.gopluslabs.io", nullptr},
    {TokenUpstream::kEthplorer, "ethplorer", Send::kGet, kRetryUnparseable, "api.ethplorer.io", nullptr},
    {TokenUpstream::kEtherscan, "etherscan", Send::kGet, kNone, "api.etherscan.io", nullptr},
    {TokenUpstream::kJupiter, "jupiter", Send::kGet, kNone, nullptr, JupiterAccess},
    {TokenUpstream::kTronScan, "tronscan", Send::kGet, kAnonymousKeyless, "apilist.tronscanapi.com", TronScanAccess},
    {TokenUpstream::kTronGrid, "trongrid", Send::kPostOnce, kUnshared, nullptr, TronGridAccess},
    {TokenUpstream::kRpc, "rpc", Send::kPostOnce, kHostLane | kUrlIsTarget | kUnshared, nullptr, nullptr},
    {TokenUpstream::kHorizon, "horizon", Send::kNetworkGet, kNone, nullptr, nullptr},
    {TokenUpstream::kKoios, "koios", Send::kNetworkGet, kNone, nullptr, nullptr},
    {TokenUpstream::kAlgod, "algod", Send::kNetworkGet, kNone, nullptr, nullptr},
    {TokenUpstream::kJsonRpc, "rpc", Send::kPostOnce, kHostLane | kUrlIsTarget, nullptr, nullptr},
    {TokenUpstream::kHederaMirror, "hedera-mirror", Send::kNetworkGet, kNone, nullptr, nullptr},
    {TokenUpstream::kSaucerSwap, "saucerswap", Send::kGet, kNone, "api.saucerswap.finance", SaucerSwapAccess},
    {TokenUpstream::kSaucerSwapTestnet, "saucerswap", Send::kGet, kNone, "test-api.saucerswap.finance",
     SaucerSwapAccess},
    {TokenUpstream::kGraphql, "rpc", Send::kPostRetried, kHostLane, nullptr, nullptr},
    // The Bittensor driver exists only with a key.
    {TokenUpstream::kTaostats, "taostats", Send::kNetworkGet, kNone, nullptr, TaostatsAccess},
    {TokenUpstream::kRobinhood, "robinhood-assets", Send::kGet, kNone, "api.robinhood.com", nullptr},
};
constexpr size_t kUpstreamCount = sizeof(kUpstreams) / sizeof(kUpstreams[0]);

constexpr bool upstreams_in_enum_order() {
  for (size_t i = 0; i < kUpstreamCount; ++i)
    if (static_cast<size_t>(kUpstreams[i].upstream) != i) return false;
  return true;
}
static_assert(kUpstreamCount == static_cast<size_t>(TokenUpstream::kRobinhood) + 1 && upstreams_in_enum_order(),
              "kUpstreams needs one row per TokenUpstream, in enum order");

const UpstreamOps& ops_of(TokenUpstream upstream) {
  const size_t index = static_cast<size_t>(upstream);
  return kUpstreams[index < kUpstreamCount ? index : static_cast<size_t>(TokenUpstream::kRpc)];
}

// One worker's FetchAll: results land in the TokenFetch entries.
struct Batch {
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  size_t outstanding = 0;
};

struct Delivery {
  Batch* batch;
  TokenFetch* fetch;
};
}  // namespace

uint64_t TokenMonotonicMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

void TokenFetchRelease(TokenFetch* fetch) {
  if (!fetch) return;
  fetch->body.reset();
  fetch->body_length = 0;
}

bool TokenSplitHttpsUrl(const char* url, char* host, size_t host_capacity, char* port,
                        size_t port_capacity, char* path, size_t path_capacity, char* lane_host,
                        size_t lane_host_capacity) {
  if (!url || strncmp(url, "https://", 8) != 0) return false;
  const char* authority = url + 8;
  size_t authority_length = strcspn(authority, "/?#");
  if (!authority_length || memchr(authority, '@', authority_length)) return false;
  const char* colon = static_cast<const char*>(memchr(authority, ':', authority_length));
  const size_t host_length = colon ? static_cast<size_t>(colon - authority) : authority_length;
  if (!host_length || host_length >= host_capacity) return false;
  memcpy(host, authority, host_length);
  host[host_length] = '\0';
  for (size_t i = 0; i < host_length; ++i) {
    const char c = host[i];
    if (c >= 'A' && c <= 'Z') host[i] = static_cast<char>(c - 'A' + 'a');
  }
  if (colon) {
    const size_t port_length = authority_length - host_length - 1;
    if (!port_length || port_length >= port_capacity) return false;
    for (size_t i = 0; i < port_length; ++i)
      if (colon[1 + i] < '0' || colon[1 + i] > '9') return false;
    memcpy(port, colon + 1, port_length);
    port[port_length] = '\0';
  } else {
    if (port_capacity < 4) return false;
    memcpy(port, "443", 4);
  }
  // new URL(url).host omits the default port.
  const bool default_port = strcmp(port, "443") == 0;
  const int lane_length = default_port
      ? snprintf(lane_host, lane_host_capacity, "%s", host)
      : snprintf(lane_host, lane_host_capacity, "%s:%s", host, port);
  if (lane_length < 0 || static_cast<size_t>(lane_length) >= lane_host_capacity) return false;
  const char* rest = authority + authority_length;
  const size_t rest_length = strcspn(rest, "#");
  const char* source = rest;
  size_t length = rest_length;
  if (!length || rest[0] != '/') {
    // "https://host?x" keeps its query on the root path.
    if (length + 2 > path_capacity) return false;
    path[0] = '/';
    memcpy(path + 1, source, length);
    path[length + 1] = '\0';
    return true;
  }
  if (length + 1 > path_capacity) return false;
  memcpy(path, source, length);
  path[length] = '\0';
  return true;
}

struct TokenProviderOwner::Impl {
  struct Client {
    bool used = false, queued = true, cancelled = false, background = false, anonymous = false;
    uint64_t id = 0, deadline = 0;
    size_t flight = kFlights;
    TokenUpstream upstream = TokenUpstream::kCoinGecko;
    Delivery delivery{};
    char path[kTokenMaxPathBytes]{};
    char host[kHostBytes]{}, port[kPortBytes]{}, lane[kLaneBytes]{};
    char network[kTokenMaxNetworkBytes]{};
    char body[kTokenMaxRpcBodyBytes]{};
    size_t body_length = 0;
  };
  struct Flight {
    bool used = false, active = false, orphaned = false, background = true, shareable = true;
    TokenUpstream upstream = TokenUpstream::kCoinGecko;
    uint64_t job = 0, deadline = 0;
    size_t waiter_count = 0;
    uint8_t waiters[kClients]{};
    size_t client = kClients;  // the client whose request the flight sends
  };
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::Thread thread;
  bool started = false, stopping = false;
  uint64_t next_id = 1, owner_id = 0;
  config::ProviderRuntimeConfig config{};
  ProviderExecutionCoordinator* coordinator = nullptr;
  Client clients[kClients]{};
  Flight flights[kFlights]{};
  unsigned char response[kTokenMaxResponseBytes]{};
  char request_path[kTokenMaxPathBytes + 512]{};
  // Off in production (see upstream_redirect.h).
  http::UpstreamRedirect redirect;
  // SetNetworkEndpoints("primary=url,url;..."): a network GET whose first
  // endpoint is `primary` tries the listed endpoints instead (a replay lists
  // a failing endpoint first, as the TypeScript fixture's failover app does).
  // None in production.
  struct EndpointOverride {
    char primary[256]{};
    char urls[kTokenMaxRpcUrls][256]{};
    const char* list[kTokenMaxRpcUrls]{};
    size_t count = 0;
  };
  EndpointOverride endpoint_overrides[4]{};
  size_t endpoint_override_count = 0;

  bool ParseEndpointOverrides(const char* spec) {
    for (const char* entry = spec; entry && *entry;) {
      const char* end = strchr(entry, ';');
      const size_t length = end ? static_cast<size_t>(end - entry) : strlen(entry);
      const char* equals = std::find(entry, entry + length, '=');
      if (equals == entry + length || endpoint_override_count == 4) return false;
      EndpointOverride& o = endpoint_overrides[endpoint_override_count++];
      const size_t primary = static_cast<size_t>(equals - entry);
      if (!primary || primary >= sizeof(o.primary)) return false;
      memcpy(o.primary, entry, primary);
      for (const char* url = equals + 1; url < entry + length;) {
        const char* comma = static_cast<const char*>(memchr(url, ',', static_cast<size_t>(entry + length - url)));
        const size_t url_length = static_cast<size_t>((comma ? comma : entry + length) - url);
        if (!url_length || url_length >= sizeof(o.urls[0]) || o.count == kTokenMaxRpcUrls) return false;
        memcpy(o.urls[o.count], url, url_length);
        o.list[o.count] = o.urls[o.count];
        ++o.count;
        url += url_length + (comma ? 1 : 0);
      }
      if (!o.count) return false;
      entry = end ? end + 1 : nullptr;
    }
    return true;
  }

  void OverrideEndpoints(TokenFetch& fetch) const {
    if ((ops_of(fetch.upstream).send != Send::kNetworkGet && fetch.upstream != TokenUpstream::kJsonRpc &&
         fetch.upstream != TokenUpstream::kGraphql) ||
        !fetch.rpc_urls || !fetch.rpc_url_count)
      return;
    for (size_t i = 0; i < endpoint_override_count; ++i) {
      const EndpointOverride& o = endpoint_overrides[i];
      if (strcmp(fetch.rpc_urls[0], o.primary) != 0) continue;
      fetch.rpc_urls = o.list;
      fetch.rpc_url_count = o.count;
      return;
    }
  }

  Impl(const config::ProviderRuntimeConfig& cfg, ProviderExecutionCoordinator* shared)
      : config(cfg), coordinator(shared) {}
  ~Impl() = default;

  static void Wake(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    fortis::base::LockGuard lock(self->mutex);
    self->changed.Signal();
  }
  static void* Entry(void* opaque) { static_cast<Impl*>(opaque)->Loop(); return nullptr; }

  bool Start() {
    if (started || !coordinator || !coordinator->valid() || !config.ca_bundle_configured ||
        !config.ca_bundle_path[0]) return false;
    owner_id = coordinator->RegisterOwner(Wake, this);
    if (!owner_id) return false;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      coordinator->UnregisterOwner(owner_id); owner_id = 0; return false;
    }
    if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
      pthread_attr_destroy(&attr); coordinator->UnregisterOwner(owner_id); owner_id = 0;
      return false;
    }
    const int rc = (thread.StartEntry<Entry>(this, &attr) ? 0 : -1);
    pthread_attr_destroy(&attr);
    if (rc != 0) { coordinator->UnregisterOwner(owner_id); owner_id = 0; return false; }
    started = true;
    return true;
  }

  // Fills the client's target from the fetch (kRpc / kTronGrid / kJsonRpc /
  // kGraphql / network GETs: the next endpoint).
  bool Target(Client& c, const TokenFetch& fetch) {
    c.body_length = 0;
    c.host[0] = c.port[0] = c.lane[0] = c.network[0] = '\0';
    const UpstreamOps& ops = ops_of(fetch.upstream);
    if (!ops.endpoint()) {
      const size_t length = strnlen(fetch.path, kTokenMaxPathBytes);
      if (!length || length >= kTokenMaxPathBytes || fetch.path[0] != '/') return false;
      memcpy(c.path, fetch.path, length + 1);
      snprintf(c.lane, sizeof(c.lane), "%s", ops.lane);
      return true;
    }
    if (fetch.next_url >= fetch.rpc_url_count || !fetch.rpc_urls ||
        fetch.rpc_body_length > kTokenMaxRpcBodyBytes) return false;
    if (fetch.network) {
      const size_t length = strnlen(fetch.network, kTokenMaxNetworkBytes);
      if (length >= kTokenMaxNetworkBytes) return false;
      memcpy(c.network, fetch.network, length + 1);
    }
    char lane_host[kLaneBytes - 4];
    const char* url = fetch.rpc_urls[fetch.next_url];
    char joined[kTokenMaxPathBytes + 512];
    if (!ops.has(kUrlIsTarget)) {
      // joinUrl(base, path): the base without its trailing '/', then the path.
      size_t base = strnlen(url, sizeof(joined));
      if (base && url[base - 1] == '/') --base;
      const size_t path = strnlen(fetch.path, kTokenMaxPathBytes);
      if (base + path + 1 > sizeof(joined) || path >= kTokenMaxPathBytes) return false;
      memcpy(joined, url, base);
      memcpy(joined + base, fetch.path, path + 1);
      url = joined;
    }
    if (!TokenSplitHttpsUrl(url, c.host, sizeof(c.host), c.port, sizeof(c.port), c.path, sizeof(c.path),
                            lane_host, sizeof(lane_host)))
      return false;
    // rpcLane(new URL(url).host) of the endpoint itself; joinUrl only
    // appends a path, so the joined URL's host is the same.
    if (ops.has(kHostLane)) snprintf(c.lane, sizeof(c.lane), "%s:%s", ops.lane, lane_host);
    else snprintf(c.lane, sizeof(c.lane), "%s", ops.lane);
    if (!ops.posts()) return true;
    memcpy(c.body, fetch.rpc_body, fetch.rpc_body_length);
    c.body_length = fetch.rpc_body_length;
    return true;
  }

  bool Submit(Batch* batch, TokenFetch* fetch, uint64_t deadline) {
    const uint64_t now = TokenMonotonicMs();
    if (deadline <= now || deadline - now > kMaxDeadlineMs) return false;
    fortis::base::LockGuard lock(mutex);
    if (!started || stopping) return false;
    size_t ci = kClients;
    for (size_t i = 0; i < kClients; ++i) if (!clients[i].used) { ci = i; break; }
    if (ci == kClients) return false;
    Client& c = clients[ci];
    if (!Target(c, *fetch)) return false;
    uint64_t request_id = next_id++;
    if (!request_id) request_id = next_id++;
    c.used = true; c.queued = true; c.cancelled = false; c.background = fetch->background;
    c.anonymous = fetch->anonymous;
    c.id = request_id; c.deadline = deadline; c.flight = kFlights; c.upstream = fetch->upstream;
    c.delivery = Delivery{batch, fetch};
    fetch->request_id = request_id;
    changed.Signal();
    return true;
  }

  bool Build(const Flight& f, http::OutboundRequest* out) {
    const Client& c = clients[f.client];
    http::OutboundRequest request{};
    const UpstreamOps& ops = ops_of(f.upstream);
    request.method = ops.posts() ? http::OutboundMethod::kPostJson : http::OutboundMethod::kGet;
    request.port = "443";
    request.path = c.path;
    request.ca_file = config.ca_bundle_path;
    request.timeout_ms = kTimeoutMs;
    request.max_response_bytes = kTokenMaxResponseBytes;
    UpstreamAccess access{};
    access.host = ops.host;
    if (ops.access && !ops.access(config, &access)) return false;
    if (c.anonymous && ops.has(kAnonymousKeyless)) access.header = access.value = nullptr;
    if (ops.endpoint()) {
      access.host = c.host;
      request.port = c.port;
    }
    if (ops.posts()) {
      request.body = reinterpret_cast<const unsigned char*>(c.body);
      request.body_length = c.body_length;
    }
    const char* header = access.header;
    const char* value = access.value;
    const char* real_host = access.host;
    request.host = real_host;
    // HttpClient.once sends `accept: application/json` on every request (no
    // token upstream names its own accept).
    size_t headers = 0;
    snprintf(request.headers[headers].name, sizeof(request.headers[0].name), "accept");
    snprintf(request.headers[headers].value, sizeof(request.headers[0].value), "application/json");
    ++headers;
    if (header) {
      const size_t length = strnlen(value, 256);
      if (!length || length >= sizeof(request.headers[0].value)) return false;
      snprintf(request.headers[headers].name, sizeof(request.headers[0].name), "%s", header);
      memcpy(request.headers[headers].value, value, length + 1);
      ++headers;
    }
    if (redirect.enabled && redirect.real_host_header[0]) {
      snprintf(request.headers[headers].name, sizeof(request.headers[0].name), "%s",
               redirect.real_host_header);
      if (ops.endpoint() && strcmp(c.port, "443") != 0)
        snprintf(request.headers[headers].value, sizeof(request.headers[0].value), "%s:%s",
                 real_host, c.port);
      else
        snprintf(request.headers[headers].value, sizeof(request.headers[0].value), "%s",
                 real_host);
      ++headers;
    }
    redirect.Apply(request);
    request.header_count = headers;
    if (!OutboundHttpsHeadersValid(request)) return false;
    *out = request;
    return true;
  }

  // Copies the result into the waiting fetch and wakes its batch. Runs on
  // the owner thread without the owner mutex.
  static void Complete(const Delivery& delivery, bool answered, int status, bool transport_error,
                       bool cancelled, bool lane_rejected, uint32_t retry_after_ms,
                       const unsigned char* body, size_t length) {
    TokenFetch* fetch = delivery.fetch;
    fetch->answered = answered;
    fetch->status = status;
    fetch->transport_error = transport_error;
    fetch->cancelled = cancelled;
    fetch->lane_rejected = lane_rejected;
    fetch->retry_after_ms = retry_after_ms;
    fetch->body.reset();
    fetch->body_length = 0;
    if (answered && body && length) {
      fetch->body = base::AllocateStorage<uint8_t[]>(length);
      if (fetch->body) {
        memcpy(fetch->body.get(), body, length);
        fetch->body_length = length;
      } else {
        // Out of memory reads as a transport failure, never as an empty answer.
        fetch->answered = false;
        fetch->transport_error = true;
      }
    }
    fortis::base::LockGuard lock(delivery.batch->mutex);
    fetch->pending = false;
    --delivery.batch->outstanding;
    delivery.batch->done.Broadcast();
  }

  void Deliver(size_t ci, bool answered, int status, bool transport_error, bool cancelled,
               bool lane_rejected, uint32_t retry_after_ms,
               const unsigned char* body = nullptr, size_t length = 0) {
    fortis::base::UniqueLock lock(mutex);
    if (ci >= kClients || !clients[ci].used) return;
    const Delivery delivery = clients[ci].delivery;
    clients[ci].used = false;
    clients[ci].flight = kFlights;
    lock.Unlock();
    Complete(delivery, answered, status, transport_error, cancelled, lane_rejected,
             retry_after_ms, body, length);
  }

  void Finish(size_t fi, bool answered, int status, bool transport_error, bool cancelled,
              bool lane_rejected, uint32_t retry_after_ms,
              const unsigned char* body = nullptr, size_t length = 0) {
    Flight& f = flights[fi];
    const size_t count = f.waiter_count;
    uint8_t waiters[kClients]{};
    memcpy(waiters, f.waiters, count);
    f.used = f.active = f.orphaned = false;
    f.waiter_count = 0;
    f.client = kClients;
    for (size_t i = 0; i < count; ++i)
      Deliver(waiters[i], answered, status, transport_error, cancelled, lane_rejected,
              retry_after_ms, body, length);
  }

  // The in-flight key: upstream (which fixes the method), path and the
  // anonymous flag, plus the network, endpoint host and port for a network
  // GET or a kJsonRpc / kGraphql POST (two networks, or two endpoints of
  // one, never share an answer), and the body for those POSTs.
  bool SameRequest(const Flight& f, const Client& c) const {
    const UpstreamOps& ops = ops_of(c.upstream);
    if (!f.shareable || f.upstream != c.upstream || !ops.shareable()) return false;
    if (f.client >= kClients) return false;
    const Client& source = clients[f.client];
    if (ops.endpoint() &&
        (strcmp(source.network, c.network) != 0 || strcmp(source.host, c.host) != 0 ||
         strcmp(source.port, c.port) != 0))
      return false;
    if (ops.posts() &&
        (source.body_length != c.body_length || memcmp(source.body, c.body, c.body_length) != 0))
      return false;
    return source.anonymous == c.anonymous && strcmp(source.path, c.path) == 0;
  }

  void Admit() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex);
      Client& c = clients[ci];
      if (!c.used || !c.queued) continue;
      if (c.cancelled || TokenMonotonicMs() >= c.deadline) {
        lock.Unlock();
        Deliver(ci, false, 0, false, true, false, 0);
        continue;
      }
      size_t fi = kFlights;
      for (size_t i = 0; i < kFlights; ++i)
        if (flights[i].used && !flights[i].orphaned && SameRequest(flights[i], c)) { fi = i; break; }
      if (fi == kFlights) {
        for (size_t i = 0; i < kFlights; ++i) if (!flights[i].used) { fi = i; break; }
        if (fi == kFlights) continue;  // retry next pass
        Flight& f = flights[fi];
        f.used = true; f.active = false; f.orphaned = false; f.background = c.background;
        f.shareable = ops_of(c.upstream).shareable();
        f.upstream = c.upstream; f.job = 0; f.deadline = c.deadline; f.waiter_count = 0;
        f.client = ci;
      } else {
        Flight& f = flights[fi];
        f.background = f.background && c.background;
        if (c.deadline > f.deadline) f.deadline = c.deadline;
      }
      Flight& f = flights[fi];
      f.waiters[f.waiter_count++] = static_cast<uint8_t>(ci);
      c.queued = false;
      c.flight = fi;
    }
  }

  void CancelWaiters() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex);
      const bool cancel = clients[ci].used && !clients[ci].queued &&
          (clients[ci].cancelled || stopping || TokenMonotonicMs() >= clients[ci].deadline);
      const size_t fi = clients[ci].flight;
      lock.Unlock();
      if (!cancel) continue;
      if (fi < kFlights) {
        Flight& f = flights[fi];
        for (size_t i = 0; i < f.waiter_count; ++i) if (f.waiters[i] == ci) {
          for (size_t j = i + 1; j < f.waiter_count; ++j) f.waiters[j - 1] = f.waiters[j];
          --f.waiter_count;
          break;
        }
        if (f.client == ci && f.waiter_count) {
          // The flight's request text lives in its first client: keep a live
          // waiter's identical copy as the source instead.
          f.client = f.waiters[0];
        }
        if (!f.waiter_count) {
          if (f.active) { f.orphaned = true; coordinator->Cancel(owner_id, f.job); }
          else { f.used = false; f.client = kClients; }
        }
      }
      Deliver(ci, false, 0, false, true, false, 0);
    }
  }

  void Schedule() {
    for (size_t i = 0; i < kFlights; ++i) {
      Flight& f = flights[i];
      if (!f.used || f.active || !f.waiter_count) continue;
      if (coordinator->live_job_count(owner_id) >= kProviderExecutionMaxJobs) return;
      http::OutboundRequest request{};
      if (!Build(f, &request)) {
        Finish(i, false, 0, true, false, false, 0);
        continue;
      }
      ProviderExecutionOptions options{};
      // DriverTransport POSTs are not retried in place (failover moves on to
      // the next endpoint); GETs and kGraphql's retryPost keep HttpClient's
      // three attempts.
      const UpstreamOps& ops = ops_of(f.upstream);
      options.max_attempts = ops.send == Send::kPostOnce ? 1 : kProviderExecutionMaxAttempts;
      options.post_is_idempotent = ops.send == Send::kPostRetried;
      // HttpClient.json retries an unparseable 2xx for the keyless REST
      // clients whose callers take the body as their answer.
      options.retry_unparseable_json = ops.has(kRetryUnparseable);
      options.deadline_ms = f.deadline;
      options.background = f.background;
      const auto submitted = coordinator->Submit(owner_id, clients[f.client].lane, request, &f,
                                                 options);
      if (submitted.status == ProviderExecutionSubmitStatus::kBusy) continue;
      if (submitted.status != ProviderExecutionSubmitStatus::kAccepted) {
        Finish(i, false, 0, true, false,
               submitted.status == ProviderExecutionSubmitStatus::kLaneRejected,
               static_cast<uint32_t>(submitted.retry_after_ms));
        continue;
      }
      f.active = true;
      f.job = submitted.job_id;
    }
  }

  void Drain() {
    for (;;) {
      ProviderExecutionResult r{};
      const auto taken = coordinator->TryTakeResult(owner_id, &r, response, sizeof(response));
      if (taken == ProviderExecutionTakeStatus::kNone) return;
      if (taken != ProviderExecutionTakeStatus::kTaken) continue;
      auto* f = static_cast<Flight*>(r.context);
      if (!f || f < &flights[0] || f >= &flights[kFlights] || !f->active || f->job != r.job_id)
        continue;
      const bool transport = r.response.error != http::OutboundError::kNone;
      const bool cancelled = r.cancelled || r.deadline_exceeded;
      Finish(static_cast<size_t>(f - flights), !transport && !cancelled && r.response.status > 0,
             r.response.status, transport, cancelled, r.lane_rejected, r.retry_after_ms, response,
             r.body_length);
    }
  }

  void Loop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      const bool stop = stopping;
      lock.Unlock();
      CancelWaiters();
      for (Flight& f : flights) if (f.used && !f.active && !f.waiter_count) { f.used = false; f.client = kClients; }
      if (stop) {
        for (size_t ci = 0; ci < kClients; ++ci) {
          lock.Lock();
          const bool queued = clients[ci].used && clients[ci].queued;
          lock.Unlock();
          if (queued) Deliver(ci, false, 0, false, true, false, 0);
        }
      } else {
        Admit();
      }
      Drain();
      if (!stop) { Admit(); Schedule(); }
      bool clients_live = false, flights_live = false;
      lock.Lock();
      for (const Client& c : clients) clients_live = clients_live || c.used;
      lock.Unlock();
      for (const Flight& f : flights) flights_live = flights_live || f.used;
      if (stop && !clients_live && !flights_live && !coordinator->live_job_count(owner_id)) break;
      lock.Lock();
      const timespec wake = wake_at(kWakeMs);
      changed.WaitUntilLocked(mutex, wake);
    }
    if (!coordinator->UnregisterOwner(owner_id)) abort();
    owner_id = 0;
  }

  void FetchAll(TokenFetch* fetches, size_t count, uint64_t deadline) {
    Batch batch;
    auto fail_unsubmitted = [](TokenFetch& fetch) {
      fetch.answered = false; fetch.status = 0; fetch.transport_error = true;
      fetch.cancelled = false; fetch.lane_rejected = false; fetch.pending = false;
    };
    fortis::base::UniqueLock lock(batch.mutex);
    for (size_t i = 0; i < count; ++i) {
      TokenFetch& fetch = fetches[i];
      if (endpoint_override_count) OverrideEndpoints(fetch);
      fetch.next_url = fetch.first_url;
      fetch.pending = true;
      ++batch.outstanding;
      lock.Unlock();
      const bool submitted = Submit(&batch, &fetch, deadline);
      lock.Lock();
      if (!submitted) { --batch.outstanding; fail_unsubmitted(fetch); }
    }
    for (;;) {
      while (batch.outstanding) batch.done.WaitLocked(batch.mutex);
      // DriverTransport failover: a transport failure or 5xx (and a busy
      // lane) moves on to the network's next endpoint; so does a 429 for
      // the single-attempt POSTs and any non-2xx for kJsonRpc (jsonRpc has
      // no rethrowAnswer), while getJson's and postJson's rethrowAnswer make
      // any GET or kGraphql 4xx the answer.
      bool resubmitted = false;
      for (size_t i = 0; i < count; ++i) {
        TokenFetch& fetch = fetches[i];
        const bool outage = !fetch.answered ? !fetch.cancelled
                          : fetch.upstream == TokenUpstream::kJsonRpc ? fetch.status < 200 || fetch.status >= 300
                                                                      : (ops_of(fetch.upstream).send == Send::kPostOnce && fetch.status == 429) ||
                                                                            fetch.status >= 500;
        if (!ops_of(fetch.upstream).endpoint() || !outage ||
            fetch.next_url + 1 >= fetch.rpc_url_count) continue;
        ++fetch.next_url;
        fetch.body.reset(); fetch.body_length = 0;
        fetch.pending = true;
        ++batch.outstanding;
        lock.Unlock();
        const bool submitted = Submit(&batch, &fetch, deadline);
        lock.Lock();
        if (!submitted) { --batch.outstanding; fail_unsubmitted(fetch); }
        else resubmitted = true;
      }
      if (!resubmitted) break;
    }
    lock.Unlock();


  }
};

TokenProviderOwner::TokenProviderOwner(const config::ProviderRuntimeConfig& config,
                                       ProviderExecutionCoordinator* coordinator)
    : impl_(fortis::base::New<Impl>(config, coordinator)) {}

TokenProviderOwner::~TokenProviderOwner() {
  if (impl_ && impl_->started && pthread_equal(pthread_self(), impl_->thread.native())) abort();
  Stop();
  impl_.reset();
}

bool TokenProviderOwner::Start() { return impl_ && impl_->Start(); }

void TokenProviderOwner::FetchAll(TokenFetch* fetches, size_t count, uint64_t deadline_ms) {
  if (!impl_) {
    for (size_t i = 0; i < count; ++i) {
      fetches[i].answered = false; fetches[i].transport_error = true;
    }
    return;
  }
  impl_->FetchAll(fetches, count, deadline_ms);
}

bool TokenProviderOwner::running() const {
  if (!impl_) return false;
  fortis::base::LockGuard lock(impl_->mutex);
  const bool running = impl_->started && !impl_->stopping;
  return running;
}

bool TokenProviderOwner::etherscan_configured() const {
  return impl_ && impl_->config.etherscan_api_key_configured;
}

void TokenProviderOwner::Stop() {
  if (!impl_ || !impl_->started) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  if (pthread_equal(pthread_self(), impl_->thread.native())) return;
  impl_->thread.Join();
  impl_->started = false;
}

bool TokenProviderOwner::SetNetworkEndpoints(const char* spec) {
  if (!impl_ || impl_->endpoint_override_count) return false;
  if (impl_->ParseEndpointOverrides(spec)) return true;
  impl_->endpoint_override_count = 0;
  return false;
}

bool TokenProviderOwner::StartRedirected(const char* host, const char* port, const char* ca,
                                         bool (*allow)(void*, uint32_t), void* context,
                                         const char* real_host_header) {
  if (!impl_ || !impl_->redirect.Accepts(host, port, ca, allow, false) ||
      (real_host_header && !impl_->redirect.SetRealHostHeader(real_host_header)))
    return false;
  impl_->redirect.Set(host, port, ca, allow, context);
  impl_->config.ca_bundle_configured = true;
  snprintf(impl_->config.ca_bundle_path, sizeof(impl_->config.ca_bundle_path), "%s", ca);
  return impl_->Start();
}

}  // namespace fortis::providers
