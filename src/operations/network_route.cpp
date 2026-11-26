#include "network_route.h"

#include "address_overview.h"
#include "chain_core.h"
#include "device_enrollment.h"
#include "fortisnet/intelligence/v1/networks.pb.h"
#include <pb_encode.h>
#include <stdio.h>
#include <string.h>

// GET /v1/networks — the network capability catalog (TypeScript
// listNetworks over DriverRegistry.publicList).
//
// Request. Only GET (and the router's automatic HEAD, which sends the GET
// headers, including Content-Length, without a body). The query string and
// any request body or conditional header are ignored; the route has no
// parameters. Other methods, a trailing slash or another path case are the
// router's 404.
//
// Policy, in TypeScript's /v1 preHandler order (AuthorizeDeviceRequest):
//   1. any Origin header, even empty → 403 BROWSER_ORIGIN_FORBIDDEN (this
//      API is for installed clients, never browsers);
//   2. no "Bearer <token>" (case-sensitive, one space) carrying a well-formed
//      device token → 401 UNAUTHORIZED; an unknown token → 401 UNAUTHORIZED;
//      a revoked or expired device → 401 SESSION_EXPIRED; under
//      ATTESTATION_MODE=required a device not attested by App Attest or Play
//      Integrity → 401 ATTESTATION_REQUIRED;
//   3. the per-device minute quota at cost 1 → over it 429 RATE_LIMITED with
//      Retry-After: 60 and x-rate-limit-cost/-remaining.
// Refusals carry the JSON error body with no Vary (TypeScript answers them
// before the handler adds Vary: Accept). A 200 carries x-rate-limit-cost: 1,
// x-rate-limit-remaining, Vary: Accept, Cache-Control: no-store and the
// router's security baseline (nosniff, Referrer-Policy, Permissions-Policy).
//
// Negotiation (AcceptsProtobuf, shared with every /v1 route): an Accept
// item application/protobuf (case-insensitive) whose first q parameter is
// absent or > 0 selects application/protobuf; anything else, including no
// Accept, */* and application/x-protobuf, selects
// application/json; charset=utf-8.
//
// Body. JSON: {"networks":[NetworkCapabilityInfo…],"prices":{"spot":bool,
// "history":bool,"sources":{"coingecko":…,"jupiter":…,"vestige":…}}}, every
// network in catalogue order (the NETWORK_SEEDS order, 43 rows: mainnets
// first, then testnets). Each network: id, family, evmChainId (EVM only),
// testnet, requiredConfirmations (only where catalogued), rpcUrls (omitted
// when the seed has none), graphqlUrls (only when catalogued, i.e. Sui),
// native {symbol, decimals, coingeckoId (mainnets only)} and capabilities,
// an object of exactly the ten catalogue NONE keys in that order, each
// "ok" or "none". Protobuf: NetworksResponse (networks.proto) with the same
// values; optional fields (evm_chain_id, required_confirmations,
// coingecko_id) are present exactly where JSON has them, proto3 defaults
// (testnet false, spot/history false) are omitted, and both maps keep the
// JSON key order — byte-identical to protobuf-es for the same document.
//
// Provider-key labels (prices.sources) report configuration, never a key:
// coingecko "keyless" without COINGECKO_API_KEY, else COINGECKO_TIER
// ("demo"/"pro"); jupiter "keyed" with JUPITER_API_KEY, else "lite";
// vestige always "keyless".
//
// Public RPC URLs. rpcUrls/graphqlUrls are the generated catalogue
// literals (network_registry.cpp), never the deployment endpoints:
// the Alchemy-keyed Robinhood URL and DASH_BLOCKBOOK_URLS live only in the
// chain catalog the address service dials, so no key, token or private host
// can reach either representation.
//
// Capabilities describe what THIS process serves, derived once at startup
// from the planes main.cpp composed (NetworkLivePlanes) and, for the address
// plane, from the same chains::Catalog flags the address facets check before
// answering 400 "… not served for …":
//   balances, transfers, nfts, insight, txStatus, txDetail — address plane
//     live AND the catalog flag (driver/source/view present, endpoints
//     configured, per-key gates such as ALCHEMY/HELIUS/ETHERSCAN/TAOSTATS);
//   news — address plane (the overview route) AND news plane (its only
//     writer) AND OverviewSupportsNews (a profiled mainnet);
//   popularTokens — address plane AND OverviewSupportsPopularTokens;
//   token — token plane AND a mainnet with a native coingeckoId (registry.ts
//     rule; advertising only, the route itself answers more ids);
//   trustlines — always "none": no native or TypeScript route serves it;
//   prices.spot / prices.history — the spot / history plane is live.
// A normal image advertises the planes composed at startup. A route-only
// test image with no active planes publishes every capability "none" and
// both prices false.
//
// Cost and reads. An authorized response performs only the policy's KV
// reads (device record, quota counter); the catalog itself makes no provider
// call and no KV read. Both documents are generated once in
// RegisterNetworksRoute into static buffers that are never written again, so
// concurrent readers share them without locking (static_body: the response
// writer references them instead of copying), and a restart regenerates the
// same bytes from the same configuration. The router runs GET and HEAD
// without route_state_mutex (network_catalog_runs_without_state_lock in
// http_server.cpp): this handler must stay a read of those startup bytes
// plus the device policy; regenerating them after registration, or adding
// any other shared state, needs its own lock and a fresh audit there.
//
// Intentional differences from TypeScript: capability and prices values
// (TypeScript publishes its full feature set, including trustlines "ok" for
// XRPL/Stellar/Hedera/Algorand that no route serves); escaped paths such as
// /v1/%6Eetworks are the router's 404 (find-my-way decodes them); and the
// router-wide AcceptsProtobuf, 404 body and Connection: close differences.

namespace fortis::operations {

namespace {
constexpr size_t kJsonCapacity = 32768;
char json_body[kJsonCapacity];
uint8_t proto_body[fortisnet_intelligence_v1_NetworksResponse_size];
static_assert(sizeof(proto_body) < 1024 * 1024,
              "network protobuf response exceeds the HTTP copy limit");
size_t json_length = 0;
size_t proto_length = 0;
bool attestation_required = false;

constexpr size_t kMaxNetworks =
    sizeof(fortisnet_intelligence_v1_NetworksResponse{}.networks) /
    sizeof(fortisnet_intelligence_v1_NetworksResponse{}.networks[0]);
const char* capability_values[kMaxNetworks][networks::kNetworkCapabilityCount];
// The chain catalog for the derivation: the address service builds its own
// from the same configuration, so both hold the same flags.
fortis::chains::Catalog derivation_catalog;

struct Writer { char* p; size_t n; size_t used = 0; bool ok = true;
  void put(const char* s) { const size_t z = strlen(s); if (!ok || z >= n-used) { ok=false; return; } memcpy(p+used,s,z); used+=z; p[used]=0; }
  void string(const char* s) {
    put("\"");
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p && ok; ++p) {
      char escaped[7]{};
      if (*p == '"') put("\\\"");
      else if (*p == '\\') put("\\\\");
      else if (*p < 0x20) { snprintf(escaped, sizeof(escaped), "\\u%04x", *p); put(escaped); }
      else { escaped[0] = static_cast<char>(*p); put(escaped); }
    }
    put("\"");
  }
  void number(uint32_t v) { char b[16]; snprintf(b,sizeof(b),"%u",v); put(b); }
};

const char* coingecko_label(const config::ProviderRuntimeConfig& config) {
  if (!config.coingecko_api_key_configured) return "keyless";
  return config.coingecko_tier == config::CoinGeckoTier::kPro ? "pro" : "demo";
}
const char* jupiter_label(const config::ProviderRuntimeConfig& config) {
  return config.jupiter_api_key_configured ? "keyed" : "lite";
}

void append_network(Writer& w, const networks::NativeNetworkConfig& n,
                    const char* const (&capabilities)[networks::kNetworkCapabilityCount]) {
  w.put("{\"id\":"); w.string(n.id); w.put(",\"family\":"); w.string(n.family); w.put(",");
  if (n.has_evm_chain_id) { w.put("\"evmChainId\":"); w.number(n.evm_chain_id); w.put(","); }
  w.put("\"testnet\":"); w.put(n.testnet?"true":"false");
  if (n.has_required_confirmations) { w.put(",\"requiredConfirmations\":"); w.number(n.required_confirmations); }
  // publicList: `rpcUrls: seed.rpcUrls ? […] : undefined` — JSON omits a
  // seed without RPC URLs (protobuf sends no field 6 either way).
  if (n.rpc_url_count) {
    w.put(",\"rpcUrls\":[");
    for(size_t i=0;i<n.rpc_url_count;i++){ if(i)w.put(","); w.string(n.rpc_urls[i]); }
    w.put("]");
  }
  if(n.graphql_url_count){ w.put(",\"graphqlUrls\":["); for(size_t i=0;i<n.graphql_url_count;i++){if(i)w.put(",");w.string(n.graphql_urls[i]);} w.put("]"); }
  if(n.native_symbol){
    w.put(",\"native\":{\"symbol\":"); w.string(n.native_symbol); w.put(",\"decimals\":"); w.number(n.native_decimals);
    if(n.native_coingecko_id){w.put(",\"coingeckoId\":"); w.string(n.native_coingecko_id);} w.put("}");
  }
  w.put(",\"capabilities\":{");
  for(size_t i=0;i<networks::kNetworkCapabilityCount;i++){if(i)w.put(",");w.string(networks::kNetworkCapabilityNames[i]);w.put(":");w.string(capabilities[i]);}
  w.put("}}");
}
template <size_t N>
bool copy_proto_string(char (&destination)[N], const char* source) {
  if (!source) return false;
  const size_t length = strnlen(source, N);
  if (length >= N) return false;
  memcpy(destination, source, length + 1);
  return true;
}

bool build_proto(const networks::NativeNetworkConfig* rows, size_t count, const config::ProviderRuntimeConfig& cfg,
                 const NetworkLivePlanes& planes, uint8_t* output, size_t capacity,
                 size_t* length) {
  static fortisnet_intelligence_v1_NetworksResponse message;
  message = fortisnet_intelligence_v1_NetworksResponse_init_zero;
  if (!rows || !length || count > kMaxNetworks) return false;
  message.networks_count = static_cast<pb_size_t>(count);
  message.has_prices = true;
  message.prices.spot = planes.prices_spot;
  message.prices.history = planes.prices_history;
  message.prices.sources_count = 3;
  const char* source_keys[] = {"coingecko", "jupiter", "vestige"};
  const char* source_values[] = {coingecko_label(cfg), jupiter_label(cfg), "keyless"};
  for (size_t i = 0; i < 3; ++i) {
    auto& entry = message.prices.sources[i];
    if (!copy_proto_string(entry.key, source_keys[i]) ||
        !copy_proto_string(entry.value, source_values[i])) return false;
  }
  for (size_t i = 0; i < count; ++i) {
    auto& destination = message.networks[i];
    const networks::NativeNetworkConfig& source = rows[i];
    if (!source.id || !source.family ||
        source.rpc_url_count > sizeof(destination.rpc_urls) / sizeof(destination.rpc_urls[0]) ||
        source.graphql_url_count > sizeof(destination.graphql_urls) /
                                       sizeof(destination.graphql_urls[0]) ||
        networks::kNetworkCapabilityCount >
            sizeof(destination.capabilities) / sizeof(destination.capabilities[0]) ||
        !copy_proto_string(destination.id, source.id) ||
        !copy_proto_string(destination.family, source.family)) return false;
    destination.has_evm_chain_id = source.has_evm_chain_id;
    destination.evm_chain_id = source.evm_chain_id;
    destination.testnet = source.testnet;
    destination.has_required_confirmations = source.has_required_confirmations;
    destination.required_confirmations = source.required_confirmations;
    destination.rpc_urls_count = static_cast<pb_size_t>(source.rpc_url_count);
    for (size_t j = 0; j < source.rpc_url_count; ++j)
      if (!copy_proto_string(destination.rpc_urls[j], source.rpc_urls[j])) return false;
    destination.graphql_urls_count = static_cast<pb_size_t>(source.graphql_url_count);
    for (size_t j = 0; j < source.graphql_url_count; ++j)
      if (!copy_proto_string(destination.graphql_urls[j], source.graphql_urls[j])) return false;
    destination.has_native = source.native_symbol != nullptr;
    if (source.native_symbol) {
      if (!copy_proto_string(destination.native.symbol, source.native_symbol)) return false;
      destination.native.decimals = source.native_decimals;
      destination.native.has_coingecko_id = source.native_coingecko_id != nullptr;
      if (source.native_coingecko_id &&
          !copy_proto_string(destination.native.coingecko_id, source.native_coingecko_id))
        return false;
    }
    destination.capabilities_count = networks::kNetworkCapabilityCount;
    for (size_t j = 0; j < networks::kNetworkCapabilityCount; ++j) {
      if (!copy_proto_string(destination.capabilities[j].key,
                             networks::kNetworkCapabilityNames[j]) ||
          !copy_proto_string(destination.capabilities[j].value,
                             capability_values[i][j])) return false;
    }
  }
  pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
  if (!pb_encode(&stream, fortisnet_intelligence_v1_NetworksResponse_fields,
                 &message)) return false;
  *length = stream.bytes_written;
  return true;
}
bool build_json(const networks::NativeNetworkConfig* rows, size_t count, const config::ProviderRuntimeConfig& config,
                const NetworkLivePlanes& planes, char* output, size_t capacity,
                size_t* length) {
  Writer w{output, capacity};
  w.put("{\"networks\":[");
  for (size_t i = 0; i < count; ++i) {
    if (i) w.put(",");
    append_network(w, rows[i], capability_values[i]);
  }
  w.put("],\"prices\":{\"spot\":");
  w.put(planes.prices_spot ? "true" : "false");
  w.put(",\"history\":");
  w.put(planes.prices_history ? "true" : "false");
  w.put(",\"sources\":{\"coingecko\":\"");
  w.put(coingecko_label(config));
  w.put("\",\"jupiter\":\"");
  w.put(jupiter_label(config));
  w.put("\",\"vestige\":\"keyless\"}}}");
  if (!w.ok) return false;
  *length = w.used;
  return true;
}

void handle(int client, const http::HttpRequest& req, bool head, void*) {
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(req, 1, attestation_required);
  if (policy.status) {
    http::HttpResponse r{policy.status, policy.reason, "application/json; charset=utf-8",
                   reinterpret_cast<const uint8_t*>(policy.error_body), strlen(policy.error_body)};
    r.rate_limit_cost = policy.rate_limit_cost;
    r.rate_limit_remaining = policy.rate_limit_remaining;
    r.retry_after = policy.retry_after;
    WriteHttpResponse(client, r, head);
    return;
  }
  const bool proto = AcceptsProtobuf(req);
  http::HttpResponse r{200, "OK", proto ? "application/protobuf" : "application/json; charset=utf-8",
                 proto ? proto_body : reinterpret_cast<const uint8_t*>(json_body),
                 proto ? proto_length : json_length};
  r.static_body = true;
  r.vary_accept = true;
  r.rate_limit_cost = policy.rate_limit_cost;
  r.rate_limit_remaining = policy.rate_limit_remaining;
  WriteHttpResponse(client, r, head);
}
}  // namespace

bool DeriveNetworkCapabilities(const networks::NativeNetworkRegistry& registry, const config::ProviderRuntimeConfig& config,
                               const NetworkLivePlanes& planes,
                               const char* (*values)[networks::kNetworkCapabilityCount], size_t capacity,
                               size_t* count) {
  size_t rows_count = 0;
  const networks::NativeNetworkConfig* rows = registry.list(&rows_count);
  if (!rows || !values || !count || rows_count > capacity) return false;
  if (planes.address && !derivation_catalog.Init(config)) return false;
  for (size_t i = 0; i < rows_count; ++i) {
    const networks::NativeNetworkConfig& row = rows[i];
    bool known = false;
    fortis::chains::Network network;
    if (planes.address)
      known = derivation_catalog.Resolve(row.id, strlen(row.id), &network) && !network.generic;
    for (size_t j = 0; j < networks::kNetworkCapabilityCount; ++j) {
      const char* name = networks::kNetworkCapabilityNames[j];
      bool on = false;
      if (known && strcmp(name, "balances") == 0) on = network.caps.balances;
      else if (known && strcmp(name, "transfers") == 0) on = network.caps.transfers;
      else if (known && strcmp(name, "nfts") == 0) on = network.caps.nfts;
      else if (known && strcmp(name, "insight") == 0) on = network.caps.insight;
      else if (known && strcmp(name, "txStatus") == 0) on = network.caps.tx_status;
      else if (known && strcmp(name, "txDetail") == 0) on = network.caps.tx_detail;
      else if (known && strcmp(name, "news") == 0)
        on = planes.news && fortis::wallet::address::OverviewSupportsNews(network);
      else if (known && strcmp(name, "popularTokens") == 0)
        on = fortis::wallet::address::OverviewSupportsPopularTokens(network);
      else if (strcmp(name, "token") == 0)
        on = planes.token && !row.testnet && row.native_coingecko_id != nullptr;
      // "trustlines" and any capability added later stay "none".
      values[i][j] = on ? "ok" : "none";
    }
  }
  *count = rows_count;
  return true;
}

bool BuildNetworksDocuments(const networks::NativeNetworkRegistry& registry, const config::ProviderRuntimeConfig& config,
                            const NetworkLivePlanes& planes, char* json, size_t json_capacity,
                            size_t* json_out, uint8_t* proto, size_t proto_capacity,
                            size_t* proto_out) {
  size_t count = 0;
  const networks::NativeNetworkConfig* rows = registry.list(&count);
  size_t derived = 0;
  if (!rows || !json || !proto || !json_out || !proto_out ||
      !DeriveNetworkCapabilities(registry, config, planes, capability_values, kMaxNetworks,
                                 &derived) || derived != count)
    return false;
  return build_json(rows, count, config, planes, json, json_capacity, json_out) &&
         build_proto(rows, count, config, planes, proto, proto_capacity, proto_out);
}

bool RegisterNetworksRoute(http::HttpRouter& router, NetworkRouteContext* c) {
  if (!c || !c->registry || !c->config) return false;
  attestation_required = c->config->attestation_required;
  if (!BuildNetworksDocuments(*c->registry, *c->config, c->planes, json_body, sizeof(json_body),
                              &json_length, proto_body, sizeof(proto_body), &proto_length))
    return false;
  return router.Add("GET", "/v1/networks", handle, c);
}

const char* NetworkCatalogJson(size_t* length) {
  if (length) *length = json_length;
  return json_length ? json_body : nullptr;
}

}  // namespace fortis::operations
