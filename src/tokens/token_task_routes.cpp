// The token routes answered by a queued task: GET
// /v1/networks/{network}/token/{tokenId}/page, /token/{tokenId} and
// /token/{tokenId}/liquidity/pools.
#include "token_service_internal.h"
#include "ownership.h"

namespace hb = fortis::codec::history_batch;

namespace fortis::tokens {

namespace {

// GET /v1/networks/{network}/token/{tokenId} — the legacy "Token 360"
// (TypeScript: getToken360 → runFacet('token360') → Token360Facet.get →
// sendStructuredOrJson). The shipped client parses the aggregator's own
// field names here; the token page (/page) is the app-vocabulary view over
// the same cached facts.
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the facts
// are still composed, providers included). Other methods, a trailing '/',
// '/TOKEN/', '/tokens/', an extra segment or no {tokenId} segment are the
// router's 404, and so are an empty value, a decoded control character/DEL
// and a value over 512 bytes; both values reach this handler
// percent-decoded. The query string is ignored entirely. In order:
//   1. A raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, no quota.
//   2. {network} or {tokenId} over 100 UTF-16 units once decoded: 414
//      {"error":"URI_TOO_LONG"}, no quota.
//   3. The /v1 device policy: any Origin 403 BROWSER_ORIGIN_FORBIDDEN, the
//      bearer 401 UNAUTHORIZED (ATTESTATION_REQUIRED for an unattested device
//      under ATTESTATION_MODE=required), then the device's per-minute quota
//      at cost 5 (requestCost's '/token/' rule; 429 RATE_LIMITED with
//      retry-after). Every later answer carries x-rate-limit-cost: 5.
//   4. networkTokenParams: network 1..64 and tokenId 1..160 UTF-16 units
//      (65..100 units): 400 {"error":"BAD_REQUEST"}.
//   5. facts()' synchronous resolve, before any provider work: registry.
//      network (exact id, alias such as eip155:1 or eip155:0001, a generic
//      EVM network for any other eip155:<digits>) else 400 {"error":
//      "BAD_REQUEST","detail":"unknown network: <network as decoded>"}; then
//      parseTokenRef over the JS-trimmed ref (NBSP, U+3000… are
//      trimmed) else "unsupported token ref: <tokenId as decoded>". The
//      detail is JSON-escaped.
//
// Response (200), JSON.stringify order: {network (the canonical id),
// tokenId (the canonical ref every cache entry is keyed by), meta:
// {coingeckoId} for a truthy resolved coin id else null, market, profile,
// liquidity (each the sub-facet value or null), holders: {count, floor:
// atLeast} or null, safety (scoreTokenSafety over the resolved facets),
// errors: {facet: "UPSTREAM_UNAVAILABLE"} in insertion order, cache:
// {market, profile, liquidity, holders: {hit, timestamp, partial?}}}. The
// facts are composed with only market, profile, liquidity, holders and
// chainMeta: no insight section and no contract section is answered, but a
// chainMeta failure is reported in `errors.chainMeta`, chainMeta's supply
// fills the market row, and GoPlus is still read (lazily, errors discarded)
// for a holder count when nothing else counted holders on its chains.
//
// Cache and providers (token_facts::ComputeFacts, shared with /page, /pools
// token pages): each sub-facet is a durable SWR entry tf:{network}:{tokenId}:
// {facet} (market 60 s fresh + 900 s stale, profile 6 h + 7 d, liquidity
// 6 min + 1 h, holders 28 h + 7 d, chainMeta 1 h + 24 h, the GoPlus probe
// 6 h + 7 d) plus tref:{network}:{tokenId} (the coin id, 6 h + 7 d), each
// freshness jittered +-10 % at store time. A stale entry is served and
// revalidated by the single background worker (market revalidations queued
// within 20 ms share one /coins/markets request; a failure backs off 30 s);
// a cold miss whose read fails is held 10 s, and answers the durable
// token_facts floor (market, profile, liquidity, holders) when there is one.
// Providers: CoinGecko (the contract lookup, /coins/markets, /coins/{id},
// /search), DexScreener, Ethplorer on eth-mainnet / keyed Etherscan V2 /
// the family holder sources, GoPlus, the EVM multicall, and each family's
// own index (Jupiter, TronScan/TronGrid, Horizon, Koios, Mirror and
// SaucerSwap, Sui GraphQL, Taostats, Robinhood). Every provider request
// carries accept: application/json and only its own provider's key
// (x-cg-demo-api-key / x-cg-pro-api-key on CoinGecko's own host, Ethplorer's
// apiKey in its URL), never a client header or a device credential. An
// unparseable 2xx from CoinGecko, DexScreener, GoPlus or Ethplorer is
// retried inside the same lane call (three attempts, 250 ms then 1 s), as
// HttpClient.json does; a 429 is not retried and opens the lane's breaker
// for 30 s. Live sub-facets write back: the token_facts change-set (market,
// profile, holders, contract, safety with their asOf and source), the
// registry row of a contract lookup, the holder observation, the liquidity
// snapshot (and the primed liquidity-detail entry /pools reads), fetch stats.
//
// Representations. application/json; charset=utf-8, or StructuredReadResponse
// {payload: google.protobuf.Struct} for an Accept naming application/
// protobuf (case-insensitive, any list position) with q > 0. Both carry
// Vary: Accept and the writer's baseline (cache-control: no-store, nosniff,
// no-referrer, permissions-policy); there is no ETag and If-None-Match is
// ignored (always 200). Errors are JSON with no Vary.
//
// Errors: 400/401/403/414/429 above; a full token queue or handoff answers
// 429 RATE_LIMITED retry-after 2 (runFacet's LaneBusy contract); a work
// arena that cannot be allocated 502 UPSTREAM_UNAVAILABLE; an encoding that
// does not fit 500 INTERNAL. A provider outage never fails the request: the
// section is null (or stale, or the floor) and named in `errors`.
//
// Concurrency and bounds: two foreground workers (queue of 16), one
// revalidation worker (64 slots), an 8 MiB task arena, a 2 MiB response, a
// 25 s task budget inside a 30 s handoff, 512 KiB provider bodies. The token
// owner shares identical in-flight GETs and kJsonRpc / kGraphql POSTs. A
// queued cold reader can arrive after that flight ends but before the SWR
// write, repeating a GET once and mixing live/cached sub-facet provenance.
// The EVM multicall POST is not shared (TypeScript sends identical eth_calls
// of two facets). Cross-worker cold-read single flight remains an open
// performance follow-up.
//
// Intentional differences (each a named rule in tests/token360_edges_test.py
// or tests/token360_test.cpp): the 414 and the raw-byte 400 carry the native
// baseline headers (parser_baseline); a malformed or non-UTF-8 path escape
// is the native parser's {"error":"BAD_REQUEST"} (bad_url); several failed
// sections list `errors` in facet order, not promise settlement order
// (errors_order); readers queued behind a concurrent cold read report the
// entries just written (hit: true), and the second worker repeats the
// eth_call (late_follower); the SWR entries survive
// a restart in the durable KV (restart_durable_swr, restart_state); a coin
// id that is not a string or is 1,100 bytes or longer leaves the token
// unlisted (unlisted_coin_id); a holders object without atLeast has no
// empty `floor` Value in protobuf (undefined_member, unreachable here).
// The path checks getToken360 and getTokenLiquidityPools share, after the
// device policy: networkTokenParams over the values the router decoded
// (both at most 100 UTF-16 units here, so 400 bytes of UTF-8) → bare 400;
// then the facet's synchronous resolve → RangeError 400 with the value as
// decoded ("unknown network: …" / "unsupported token ref: …"). False once
// answered.
bool facet_params(int client, const http::HttpRequest& request, bool head, int cost, uint32_t remaining,
                  char (&network)[520], size_t* network_length, char (&token)[520], size_t* token_length) {
  *network_length = path_param(request, "network", network, sizeof(network));
  *token_length = path_param(request, "tokenId", token, sizeof(token));
  const bool fits = *network_length != SIZE_MAX && *token_length != SIZE_MAX;
  const size_t network_units = fits ? hb::Utf16Units(network, *network_length) : 0;
  const size_t token_units = fits ? hb::Utf16Units(token, *token_length) : 0;
  if (!fits || network_units < 1 || network_units > 64 || token_units < 1 || token_units > 160) {
    send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return false;
  }
  return facet_resolve(client, head, cost, remaining, network, *network_length, token, *token_length);
}

}  // namespace

namespace token_service_internal {

// GET /v1/networks/{network}/token/{tokenId}/page — R24, the token page in
// one request (TypeScript: getTokenPage →
// validate(networkTokenParams + pageQuerySchema) → parseSections →
// runFacet('tokenPage', TokenPageFacet.get) →
// cache-control → sendConditionalStructured('token-page')). The app's caller
// is IntelligenceApi.tokenPage (sorted `include`, an optional coingeckoId
// hint, a conditional protobuf GET).
//
// Request. GET, plus HEAD (Fastify's exposed HEAD route: the page is still
// composed, providers included; same status and headers, no body). Other
// methods, a trailing '/', '/PAGE', an extra segment and ';' (not a query
// delimiter) are the router's 404, and so are an empty value, a decoded
// control character/DEL and a value over 512 bytes. Both values reach the
// handler percent-decoded. In order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"},
//      before any hook or quota;
//   2. {network} or {tokenId} over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"}, no quota;
//   3. the /v1 device policy: any Origin 403 BROWSER_ORIGIN_FORBIDDEN, the
//      bearer 401 UNAUTHORIZED (ATTESTATION_REQUIRED for an unattested device
//      under ATTESTATION_MODE=required), then the device's per-minute quota at
//      cost 5 (requestCost's '/token/' rule; 429 RATE_LIMITED, retry-after).
//      Every later answer carries x-rate-limit-cost: 5;
//   4. validate(): networkTokenParams (network 1..64, tokenId 1..160 UTF-16
//      units) and pageQuerySchema together, any issue the bare 400
//      {"error":"BAD_REQUEST"}. The query is parsed as fast-querystring does
//      ('+' a space, escapes decoded unless malformed or not UTF-8, keys
//      compared decoded, a repeated key an array → 400, other keys ignored)
//      in buffers as long as the target (TargetQuery), then ParsePageQuery:
//      include z.string().trim().max(200 units) optional; days
//      z.coerce.number().int().min(1).max(365).default(200) through
//      JavaScript Number() ("" and blanks are 0, JS whitespace trimmed,
//      0x/0o/0b, exponents, leading zeros of any length); interval exactly
//      "1h" or "1d" (default "1d"); coingeckoId trimmed, lower-cased
//      (String.prototype.toLowerCase: U+212A KELVIN SIGN becomes "k", U+0130
//      grows and fails) and matched against ^[a-z0-9][a-z0-9-]{0,99}$;
//   5. parseSections(include): absent or "" → the default sections (identity,
//      quote, market, profile, liquidity, holders, safety, contract,
//      comparison); "all" in any case → those plus insight and series;
//      otherwise the comma-separated names JS-trimmed and lower-cased, empty
//      names dropped, duplicates kept once in first-seen order (a list of
//      only commas is the default). Any unknown name: 400 {"error":
//      "BAD_REQUEST","detail":"unknown include: <names joined by ','>"},
//      lower-cased and trimmed as compared, a NUL kept (\u0000);
//   6. facts()' synchronous resolve (facet_resolve): "unknown network: <as
//      decoded>" / "unsupported token ref: <as decoded>" (RangeError 400).
//
// Response (200), JSON.stringify order: {network, ref (the canonical token
// id), input (the trimmed ref as written), asOf, included, identity, quote,
// market, profile, liquidity, holders, safety, contract, comparison,
// insight, series, errors, cache}. `included` is the Set of requested
// sections with identity appended when absent (a page without identity is
// not a page), in first-seen order; a section not included is null ([] for
// insight) and has no `cache` entry. `cache` is assigned in the facet's
// fixed order (identity, quote, market, profile, liquidity, holders, safety,
// contract, comparison, insight, series), each {hit, timestamp, partial?};
// identity combines chainMeta (and market when the market row was read),
// safety combines market, liquidity and holders. `asOf` is the oldest
// section timestamp, a section without one counting as now. Sections:
// identity (chain metadata, then the registry, the native coin, the ref, the
// market row; the symbol upper-cased); quote: the market price when truthy
// (source "market"), else PricesFacet.spot of the resolved coin id or the
// ref's price id ("spot"), else the deepest pool's positive finite price
// ("dex"), else null; market/profile the app's views; liquidity, holders,
// safety, contract, insight the facts as composed; comparison {fiat: "usd",
// items} from the comparison store over the token and the bitcoin/bnb
// native benchmarks (a failed compare is null with errors.comparison
// "UNAVAILABLE"); series {interval, points, volumes, window} from the price
// history service over [now - days*86400, now] for the resolved coin id
// (else the ref's price id; neither → null), a failed read null with
// errors.series "UPSTREAM_UNAVAILABLE". `errors` carries the facts' failed
// sub-facets in facet order, then comparison/series.
//
// Representations: JSON, or StructuredReadResponse {payload: Struct} for an
// Accept naming application/protobuf (case-insensitive, any list position)
// with q > 0 (then Vary: Accept). cache-control "private, max-age=15" on 200
// and 304 (the shortest section TTL is the 60 s market row). Weak ETag
// W/"token-page[-protobuf]-<first 24 hex of sha256(body)>": an
// If-None-Match equal to the selected representation's tag answers 304 with
// ETag and cache-control (a JSON tag never matches protobuf and vice versa;
// '*', lists, strong and padded forms never match). The ETag hashes the whole
// body, so a page whose asOf or provenance moved has a new tag. Errors are
// JSON with the writer's baseline (cache-control: no-store), no ETag, no Vary.
//
// Cache, durable effects and providers: the facts are token_facts::
// ComputeFacts with the sections' sub-facets only (FacetsForSections), so
// the page reads and writes exactly the durable SWR entries, token_facts
// floor, registry, holder observations and liquidity snapshots documented
// beside handle_token360 (a hinted scope neither reads nor writes the
// shared floor). The quote's spot read goes through the price canary's spot
// owner (/simple/price, /simple/token_price) only when the market row has no
// price; the series read goes through the price history service (its
// candle store, coverage markers and CoinGecko market_chart/range backfill
// of missing ranges); comparison reads the comparison store and the two
// benchmarks' market facts. No client header or device credential reaches a
// provider; each provider key only its own host.
//
// Errors: 400/401/403/414/429 above; a full token queue or handoff answers
// 429 RATE_LIMITED retry-after 2 (runFacet's LaneBusy contract); a work
// arena that cannot be allocated 502 UPSTREAM_UNAVAILABLE; an encoding over
// the 2 MiB response 500 INTERNAL. A provider outage never fails the page.
//
// Concurrency and bounds: queued to two foreground token workers (16 slots),
// an 8 MiB task arena (a 365-day hourly series holds 616 KiB of candles),
// a 2 MiB response, a 25 s task budget inside a 30 s handoff; one
// revalidation worker refreshes stale entries after the answer. Concurrent
// cold reads of one token share each provider GET in the token owner.
//
// Differences from TypeScript, each a named rule in
// tests/token_page_edges_test.py:
//  - parser_baseline / bad_url / router_404 / head_304: router- and
//    parser-wide (the 414 and raw-byte 400 carry the native security
//    headers; a malformed path escape is {"error":"BAD_REQUEST"}; the
//    router's 404 body; a HEAD 304 has no content-length: 0);
//  - errors_order_page: with several sub-facets failing, `errors` follows
//    facet order, not promise settlement (so the ETag is the native body's);
//  - late_follower: simultaneous cold reads queued behind the two workers
//    read, per sub-facet, either live or the entry a sibling just wrote (hit
//    at the write); a worker that misses just after a sibling's owner flight
//    completed but before its SWR write repeats that provider request once
//    (the open R20–R23 follow-up: no cross-worker single flight);
//  - restart_durable_swr / restart_floor_asof: the SWR entries, `tref:`
//    resolution included, are durable, so after a restart the page is served
//    from them (TypeScript's in-memory Kv composes live and re-reads its
//    registry row, coingeckoIdSource "registry").
}  // namespace token_service_internal

void TokenRoutes::HandlePage(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, attestation_required_);
  if (reply_policy(client, policy, head)) return;
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  static const char* const kNames[] = {"include", "days", "interval", "coingeckoId"};
  TargetQuery query(request.raw_target, kNames, 4);
  auto scratch_storage = query.ok() ? base::AllocateStorage<char[]>(query.capacity()) : nullptr;
  auto* scratch = scratch_storage.get();
  if (!scratch) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  char network[520], token[520];
  size_t network_length = 0, token_length = 0;
  const bool params = query.valid() && history_params(request, network, &network_length, token, &token_length);
  fortis::tokens::token_page::PageQueryValue values[4];
  for (size_t i = 0; i < 4; ++i) values[i] = {query.value(i), query.length(i), query.count(i)};
  fortis::tokens::token_page::PageOptions options{};
  char hint[128] = {};
  bool has_hint = false;
  char detail[fortis::tokens::token_page::kUnknownIncludeBytes];
  size_t detail_length = 0;
  const fortis::tokens::token_page::PageQueryStatus status =
      params ? fortis::tokens::token_page::ParsePageQuery(values[0], values[1], values[2], values[3], scratch,
                                                   query.capacity(), &options, hint, sizeof(hint), &has_hint, detail,
                                                   sizeof(detail), &detail_length)
             : fortis::tokens::token_page::PageQueryStatus::kInvalid;
  scratch_storage.reset();
  if (status == fortis::tokens::token_page::PageQueryStatus::kInvalid) {
    send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  if (status == fortis::tokens::token_page::PageQueryStatus::kUnknownInclude) {
    send_detail(client, "", detail, detail_length, head, cost, remaining);
    return;
  }
  if (!facet_resolve(client, head, cost, remaining, network, network_length, token, token_length)) return;
  TokenTask* task = new_task(TokenTask::kPage, request, policy);
  if (task && !copy_params(task, network, network_length, token, token_length)) { fortis::base::Delete(task); task = nullptr; }
  if (task) {
    task->page = options;
    task->has_hint = has_hint;
    memcpy(task->hint.data(), hint, task->hint.capacity());
  }
  dispatch(worker_, client, head, task);
}


void TokenRoutes::HandleToken360(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, attestation_required_);
  if (reply_policy(client, policy, head)) return;
  char network[520], token[520];
  size_t network_length = 0, token_length = 0;
  if (!facet_params(client, request, head, policy.rate_limit_cost, policy.rate_limit_remaining, network,
                    &network_length, token, &token_length))
    return;
  TokenTask* task = new_task(TokenTask::kToken360, request, policy);
  if (task && !copy_params(task, network, network_length, token, token_length)) { fortis::base::Delete(task); task = nullptr; }
  dispatch(worker_, client, head, task);
}

namespace token_service_internal {

// GET /v1/networks/{network}/token/{tokenId}/liquidity/pools — R22, the
// complete current pool rows a token card shows when expanded (TypeScript:
// getTokenLiquidityPools → runFacet('tokenLiquidityPools')
// → Token360Facet.liquidityDetails → sendConditionalStructured('token-
// liquidity-pools')). The token page carries only the six deepest pools.
//
// Request. GET, plus HEAD (same status and headers, the facet still runs).
// Other methods, '/' suffixes and spellings are the router's 404, and so are
// an empty value, a decoded control character/DEL and a value over 512
// bytes; both values reach the handler percent-decoded. The query string is
// ignored. In order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, no quota;
//   2. a param over 100 UTF-16 units: 414 {"error":"URI_TOO_LONG"}, no quota;
//   3. the /v1 device policy: Origin 403, bearer 401 (ATTESTATION_REQUIRED
//      under ATTESTATION_MODE=required), the per-minute quota at cost 5
//      (429 retry-after); every later answer carries x-rate-limit-cost: 5;
//   4. networkTokenParams (network 1..64, tokenId 1..160 units): bare 400;
//   5. liquidityDetails' synchronous resolve (facet_params): RangeError 400
//      "unknown network: <network as decoded>" / "unsupported token ref:
//      <tokenId as decoded>" (JSON-escaped; the ref is JS-trimmed for
//      parsing but echoed raw).
//
// Response (200), JSON.stringify order: {network (canonical id), tokenId
// (canonical ref), liquidity: calculateLiquiditySnapshot over every
// chain-filtered, deduplicated pool (Number.MAX_SAFE_INTEGER displayed), or
// null (no pools, no address — a native coin —, or a network with neither a
// DexScreener slug nor SaucerSwap), errors: {"liquidity-detail":
// "UPSTREAM_UNAVAILABLE"} when the read failed or is held, else {}, cache:
// {hit, timestamp}}. A provider failure never fails the request.
//
// Cache and providers: one SWR entry tf:{network}:{tokenId}:liquidity-detail
// (TTL.tokenLiquidityDetail: 60 s fresh, jittered at store time, no stale
// window, no durable floor), also primed by a live token-page read. A miss
// reads DexScreener /latest/dex/tokens/{address} (networks with a slug) or
// SaucerSwap's pools (Hedera HTS refs); a failed cold read is held 10 s
// (errors member, no provider request). Provider requests carry accept:
// application/json and no client header or device credential; an
// unparseable 2xx is retried inside the lane call (three attempts, 250 ms
// then 1 s, R19); a 429 opens the lane breaker for 30 s. A live non-null
// snapshot records its compact form (six pools) through RecordSuccess
// (facts, comparison and the raw observation in one KV frame), an empty one
// RecordNotFound — what /liquidity/history and /liquidity/summary read.
//
// Representations: JSON, or StructuredReadResponse {payload: Struct} for an
// Accept naming application/protobuf with q > 0 (Vary: Accept only then);
// weak ETag W/"token-liquidity-pools[-protobuf]-<24 hex of sha256(body)>"; an
// If-None-Match equal to it is a 304 with the ETag and cache-control
// "private, max-age=60". The cache block is part of the body, so a live
// answer (hit: false) and the cached revisit (hit: true) differ.
//
// Errors: 400/401/403/414/429 above; a full token queue or handoff answers
// 429 RATE_LIMITED retry-after 2 (runFacet's LaneBusy contract); a work arena
// that cannot be allocated 502 UPSTREAM_UNAVAILABLE; an encoding over 2 MiB
// 500 INTERNAL.
//
// Concurrency and bounds: the read runs on the token service's two
// foreground workers (16 queued, 30 s handoff, 25 s provider budget, 8 MiB
// arena, 512 KiB provider body); identical in-flight DexScreener GETs share
// one request.
//
// Intentional differences (named rules in tests/token_history_edges_test.py):
// the 414 and the raw-byte 400 carry the native security headers
// (parser_baseline); a malformed or non-UTF-8 path escape is
// {"error":"BAD_REQUEST"} (bad_url); router 404 bodies (router_404); a HEAD
// 304 has no content-length: 0 (head_304, router-wide); readers queued behind
// a simultaneous cold read report the entry just written, hit: true and that
// body's ETag (late_follower); the entry survives a restart in the durable
// KV, so within its 60 s a new process answers from it (restart_durable_swr).
}  // namespace token_service_internal

void TokenRoutes::HandlePools(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, attestation_required_);
  if (reply_policy(client, policy, head)) return;
  char network[520], token[520];
  size_t network_length = 0, token_length = 0;
  if (!facet_params(client, request, head, policy.rate_limit_cost, policy.rate_limit_remaining, network,
                    &network_length, token, &token_length))
    return;
  TokenTask* task = new_task(TokenTask::kPools, request, policy);
  if (task && !copy_params(task, network, network_length, token, token_length)) { fortis::base::Delete(task); task = nullptr; }
  dispatch(worker_, client, head, task);
}


}  // namespace fortis::tokens
