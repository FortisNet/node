// The KV-only token history routes, answered on the HTTP worker: GET
// /v1/networks/{network}/token/{tokenId}/holders/history, /liquidity/history
// and /liquidity/summary.
#include "query_string.h"
#include "token_service_internal.h"

namespace hb = fortis::codec::history_batch;

namespace tj = fortis::codec::tj;

namespace fortis::tokens {

namespace {

// ---- KV-only history routes (answered on the HTTP worker) ------------------------

void send_structured(int client, bool head, const http::HttpRequest& request, Work& work, const char* name_space,
                     const Value* document, const Value* validator, const char* cache_control, int cost,
                     uint32_t remaining) {
  (void)work;
  const bool protobuf = AcceptsProtobuf(request);
  // Response buffers live outside the work arena (freed below).
  auto body_storage = base::AllocateStorage<uint8_t[]>(kResponseBytes);
  auto stable_storage = validator ? base::AllocateStorage<uint8_t[]>(kResponseBytes) : nullptr;
  auto* body = body_storage.get();
  auto* stable = stable_storage.get();
  bool ok = body && (!validator || stable);
  tj::OutBuffer out{body, body ? kResponseBytes : 0, 0, true};
  tj::OutBuffer stable_out{stable, stable ? kResponseBytes : 0, 0, true};
  if (ok) {
    ok = protobuf ? tj::AppendStructuredReadResponse(document, out) && out.ok : tj::Stringify(document, out) && out.ok;
    if (ok && validator)
      ok = protobuf ? tj::AppendStructuredReadResponse(validator, stable_out) && stable_out.ok
                    : tj::Stringify(validator, stable_out) && stable_out.ok;
  }
  http::HttpResponse selected{200, "OK", protobuf ? "application/protobuf" : kJsonType, body, out.used};
  http::HttpResponse response{};
  if (!ok || !ConditionalStructuredResponse(request, name_space, selected, selected, &response,
                                            validator ? stable : nullptr, validator ? stable_out.used : 0,
                                            validator ? stable : nullptr, validator ? stable_out.used : 0)) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
  } else {
    response.cache_control = cache_control;
    response.rate_limit_cost = cost;
    response.rate_limit_remaining = remaining;
    WriteHttpResponse(client, response, head);
  }
  body_storage.reset();
  stable_storage.reset();
}

// registry.network(...) + parseTokenRef(...) with the route's own details
// (no value echoed).
bool route_ref(int client, bool head, const char* network, size_t network_length, const char* token,
               size_t token_length, networks::TokenNetwork* resolved, networks::TokenRef* ref, int cost, uint32_t remaining) {
  if (!ResolveTokenNetwork(network, network_length, resolved)) {
    send_bad_request(client, "unknown network", head, cost, remaining);
    return false;
  }
  if (!ParseTokenRef(resolved, token, token_length, ref)) {
    send_bad_request(client, "unsupported token ref", head, cost, remaining);
    return false;
  }
  return true;
}

}  // namespace

namespace token_service_internal {

// GET /v1/networks/{network}/token/{tokenId}/holders/history — R20, the
// holder observations behind the trend percentages (TypeScript:
// getTokenHolderHistory over HolderSnapshotStore →
// sendConditionalStructured('token-holder-history')). Answered on the HTTP
// worker from the KV; no provider.
//
// Request. GET, plus HEAD (same status and headers, no body). Other methods,
// '/' suffixes and spellings are the router's 404, and so are an empty value,
// a decoded control character/DEL and a value over 512 bytes; both values
// reach the handler percent-decoded (history_params). In order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, no quota;
//   2. a param over 100 UTF-16 units: 414 {"error":"URI_TOO_LONG"}, no quota;
//   3. the /v1 device policy: Origin 403, bearer 401 (ATTESTATION_REQUIRED
//      under ATTESTATION_MODE=required), the per-minute quota at cost 5
//      (requestCost's '/token/' rule; 429 retry-after); every later answer
//      carries x-rate-limit-cost: 5;
//   4. validate(): networkTokenParams (network 1..64, tokenId 1..160 units)
//      and the query together, any issue a bare 400 {"error":"BAD_REQUEST"}:
//      days = z.coerce.number().int().min(1).max(400).default(30) — the value
//      through JavaScript Number() (JS whitespace trimmed, "" and blanks 0,
//      0x/0o/0b, exponents, leading zeros of any length) — and source =
//      z.string().trim().min(1).max(80) in UTF-16 units, optional; the query
//      parsed as fast-querystring does ('+' a space, escapes decoded unless
//      malformed, keys compared decoded, a repeated key an array → 400,
//      other keys ignored) in buffers as long as the target (TargetQuery);
//   5. registry.network: exact id or alias (eip155:1, eip155:0001, a generic
//      EVM network) else 400 detail "unknown network" (no value echoed;
//      case is not folded); parseTokenRef over the JS-trimmed ref
//      else "unsupported token ref".
//
// Response (200), JSON.stringify order: {network (canonical id), tokenId
// (canonical ref), trendSource: the trimmed source or null, observations,
// trends, distribution: {snapshots, comparisons}}. The three parts are
// computed from ONE read of the token's holder rows (TypeScript's reads are
// synchronous against one LMDB view), and independently:
//  - observations (history(days)): every row with a count whose
//    countObservedAt is within [now - days * 86400000, now] (a future count
//    is not shown), newest first, equal times in key order (source, day):
//    {at: ISO, count, atLeast (true for a lower bound), source}. Every source
//    and lower-bound rows included; ?source does not filter them.
//  - trends (trends({source})): exact (atLeast false) counts of the last 390
//    days from `source`, or — without one — of the source of the newest exact
//    count; null when there is none or the newest is over 36 h old. Else
//    {change24hPct, change7dPct, change30dPct, change90dPct, change365dPct,
//    latestAt}: each window's baseline is the count nearest latest - N days
//    within 8 h / 36 h / 3 d / 7 d / 21 d (inclusive; the earlier on a tie),
//    null without one or for a baseline of 0.
//  - distribution (distributionReport(days)): ranked-holder rows of the
//    source of the NEWEST DISTRIBUTION — deliberately not the count source
//    nor ?source (an Ethplorer count beside GoPlus holders); snapshots: that
//    source's rows within `days`, newest first {at, source, positionCount,
//    topHoldersPercent, topHoldersPercentExcludingLocked, burnedPercent}
//    (stored decimals as numbers, else null); comparisons: [] without a
//    distribution in 390 days, else the five windows 24h/7d/30d/90d/12m
//    (same targets and tolerances, over all 390 days whatever `days` is)
//    {window, latestAt, baselineAt|null, topHoldersPercentPointChange,
//    liveWalletPercentPointChange, burnedPercentPointChange, positions: []
//    without a baseline, else every address of either day: {address, tag,
//    kind entered|exited|changed|unchanged, current/previous rank, percent,
//    balance, isContract, isLocked and the rank/percentage-point/balance
//    changes}, ordered by current rank, previous rank, address}.
//
// Representations: JSON, or StructuredReadResponse {payload: Struct} for an
// Accept naming application/protobuf with q > 0 (Vary: Accept only then);
// cache-control "private, max-age=60"; weak ETag W/"token-holder-history
// [-protobuf]-<24 hex of sha256(body)>"; an If-None-Match equal to it gives
// 304 with ETag and cache-control (lists, '*' and strong forms never match).
// Errors are JSON with the writer's baseline (cache-control: no-store), no ETag.
//
// Persistence and effects: read-only. Rows live under token/holder/{net}/
// {id}/{source}/{start} (32-day blocks, one row per source and UTC day,
// retained 400 days from that day), written by RecordCount (the token facts'
// holder counts and the holder job) and RecordDistribution (contract
// facets); a later observation of a day replaces an earlier one, an older
// one is ignored, and counts and distributions of a day share one row.
// Errors: a store read or arena failure is 502 UPSTREAM_UNAVAILABLE (runFacet's
// non-RangeError contract); a failed buffer allocation or an encoding over
// 2 MiB 500 INTERNAL.
//
// Concurrency and bounds: any HTTP worker; each write is one KV frame, so a
// concurrent read sees a row before or after it, and observations, trends
// and distribution always describe the same rows. The document is built in a
// growing arena of at most 16 MiB (400 days x three sources with 300 ranked
// holders on six days measured at 9.4 MB held); no positions bound.
// A preceding stale token page may queue holder revalidation. Its completed
// write can become visible before this read, so callers may see the old or
// newly observed count with a matching ETag (the Hedera replay checks both
// atomic states rather than tying the answer to worker scheduling).
//
// Intentional differences (named rules in tests/token_history_edges_test.py):
// parser_baseline (414 and raw-byte 400 with the native security headers),
// bad_url (a malformed path escape is {"error":"BAD_REQUEST"}), router_404
// (status only), head_304 (no content-length: 0 on a HEAD 304; router-wide).
}  // namespace token_service_internal

void TokenRoutes::HandleHolders(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, attestation_required_);
  if (reply_policy(client, policy, head)) return;
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  char network[520], token[520];
  size_t network_length = 0, token_length = 0;
  static const char* const kNames[] = {"days", "source"};
  TargetQuery query(request.raw_target, kNames, 2);
  if (!query.ok()) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  bool valid = query.valid() && history_params(request, network, &network_length, token, &token_length);
  // days: z.coerce.number().int().min(1).max(400).default(30) over Number().
  double day_count = 30;
  if (valid && query.count(0)) {
    uint64_t value = 0;
    valid = http::query_string::ParseUnsigned(query.value(0), query.length(0), 400, &value) && value > 0;
    day_count = static_cast<double>(value);
  }
  // source: z.string().trim().min(1).max(80).optional() in UTF-16 units.
  size_t source_begin = 0, source_end = 0;
  if (valid && query.count(1)) {
    valid = query.count(1) == 1 &&
            hb::ValidUtf8(reinterpret_cast<const uint8_t*>(query.value(1)), query.length(1));
    if (valid) {
      hb::JsTrim(query.value(1), query.length(1), &source_begin, &source_end);
      const size_t units = hb::Utf16Units(query.value(1) + source_begin, source_end - source_begin);
      valid = units >= 1 && units <= 80;
    }
  }
  if (!valid) {
    send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  networks::TokenNetwork resolved;
  networks::TokenRef ref;
  if (!route_ref(client, head, network, network_length, token, token_length, &resolved, &ref, cost, remaining)) return;
  char source_value[400] = {};  // 80 UTF-16 units are at most 320 bytes
  if (query.count(1)) {
    memcpy(source_value, query.value(1) + source_begin, source_end - source_begin);
    source_value[source_end - source_begin] = '\0';
  }
  Work work{};
  if (!workspaces_.MakeHistory(&work)) {
    send_text(client, 502, "Bad Gateway", kUpstreamUnavailable, head, cost, remaining);
    return;
  }
  Value* document = nullptr;
  if (!fortis::tokens::token_history::HolderHistoryDocument(work, resolved.id, ref.token_id, day_count,
                                                    query.count(1) ? source_value : nullptr, env_.swr->Now(), &document)) {
    TokenWorkspaces::FreeHistory(&work);
    send_text(client, 502, "Bad Gateway", kUpstreamUnavailable, head, cost, remaining);
    return;
  }
  send_structured(client, head, request, work, "token-holder-history", document, nullptr, kMinuteCacheControl, cost,
                  remaining);
  TokenWorkspaces::FreeHistory(&work);
}

namespace token_service_internal {

// GET /v1/networks/{network}/token/{tokenId}/liquidity/history — R21, the
// time series of a token's DEX liquidity and price (TypeScript:
// getTokenLiquidityHistory → LiquidityHistoryFacet.get →
// sendConditionalStructured('token-liquidity-history', result,
// liquidityHistoryValidator(result))). No runFacet: a failure is the global
// handler's 500. Answered on the HTTP worker from the KV; no provider.
//
// Request. GET, plus HEAD (same status and headers, no body). Other methods,
// '/' suffixes and spellings are the router's 404, and so are an empty value,
// a decoded control character/DEL and a value over 512 bytes; both values
// reach the handler percent-decoded (history_params). In order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, no quota;
//   2. a param over 100 UTF-16 units: 414 {"error":"URI_TOO_LONG"}, no quota;
//   3. the /v1 device policy: Origin 403, bearer 401 (ATTESTATION_REQUIRED
//      under ATTESTATION_MODE=required), the per-minute quota at cost 5
//      (requestCost's '/token/' rule; 429 retry-after); every later answer
//      carries x-rate-limit-cost: 5;
//   4. validate(): networkTokenParams and the query together, any issue a
//      bare 400: range = z.enum(['1d','7d','30d','90d','1y']).default('30d'),
//      interval = z.enum(['1h','1d','1w']).default('1h') — exact values (no
//      trim, case-sensitive; escapes decoded first; a repeated key 400),
//      fast-querystring over the whole target (TargetQuery, other keys of any
//      length ignored);
//   5. 1h beyond 90 days (range 1y with interval 1h, including the default
//      interval): 400 detail "1h liquidity history caps at 90 days", before
//      the network is looked at;
//   6. registry.network / parseTokenRef: "unknown network" / "unsupported
//      token ref", as R20.
//
// Response (200), JSON.stringify order: {network, tokenId, range, interval,
// availability: {status (the facts' liquidityStatus ok|not_found|unsupported
// |error, or "unknown" without a facts row), checkedAt, lastSuccessfulAt
// (ISO or null), source, stale, latestAgeSeconds}, latest, changes, series,
// coverage: {from, to (epoch seconds of now - range and now), rawSamples,
// candleCount}}:
//  - latest: the newest raw observation at or before now with status ok and
//    a liquidity or price (any age: a raw row lives seven days from its
//    arrival, so a backfilled one is read; LatestObservation scans blocks
//    newest first), {ts (s), liquidityUsd, priceUsd, spreadPct, topPoolShare,
//    poolCount}, or null.
//  - stale: no latest, or its age over max(1800, 2 x samplingIntervalSeconds
//    (default 300)) s; latestAgeSeconds = max(0, floor(age)) or null.
//  - changes {h1, h24, d7, d30}: all null when stale; else each the ok row
//    with a liquidity or price nearest latest - 1 h / 24 h / 7 d / 30 d within
//    30 min / 2 h / 12 h / 1 d (inclusive; the earlier on a tie) — for d7 and
//    d30 without one, the daily candle nearest that target in the same band
//    (its closes) — {baselineAt (s), liquidityPct, pricePct} (null for a
//    missing value or a zero baseline), or null.
//  - series: interval candles with ts in [now - range, now], ascending:
//    the stored rollups, each replaced by the candle rebuilt from raw rows
//    (not_found rows included) from max(now - range, the interval bucket of
//    now - 2 h) on; a bucket without data is absent (gaps are not filled).
//    Candle {ts (s), liquidity/price {open, high, low, close, changePct},
//    spread {closePct, maxPct}, concentration {close, max}, pools {open,
//    close}, rolling24h {volumeUsd, buys, sells}, coverage {samples,
//    expected, ratio}, source (the bucket's last row)}.
//
// Representations: JSON or StructuredReadResponse protobuf (Vary: Accept
// only then); cache-control "private, max-age=60". The weak ETag
// W/"token-liquidity-history[-protobuf]-<24 hex>" hashes the VALIDATOR: the
// same encoding of the response without availability.latestAgeSeconds and
// coverage.from/to, so a revisit whose data did not change is a 304 while the
// clock moves (a new observation, a stale flip or a candle changes it).
//
// Persistence and effects: read-only. It reads the facts document
// (token/facts), raw observations (token/liqraw 6-hour blocks; a row expires
// seven days after max(arrival, ts)) and candles (token/liqcandle; 1h kept
// 92 days, 1d 367, 1w 371) written by RecordSuccess / RecordNotFound /
// RecordDiagnostic (token facts, /liquidity/pools, the liquidity job) and
// the hourly rollup (RollupLiquidityCandles: 1h/1d from two days of raw rows,
// 1w from the stored dailies). A store read or arena failure is 500 INTERNAL.
//
// Concurrency and bounds: any HTTP worker; each write is one KV frame (an
// observation with its facts and comparison rows). Reads page through blocks
// (Observations, Candles) under the store mutex; a raw window is at most a
// week of five-minute buckets, a candle read at most 4,096 rows. The document
// is built in a growing arena of at most 16 MiB: a full 90-day hourly chart
// (2,160 candles) measured at 9.4 MB held, 1.39 MB JSON, 1.32 MB protobuf
// (the response bound is 2 MiB).
//
// Intentional differences: parser_baseline, bad_url, router_404, head_304
// (as R20).
}  // namespace token_service_internal

void TokenRoutes::HandleLiquidityHistory(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, attestation_required_);
  if (reply_policy(client, policy, head)) return;
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  char network[520], token[520];
  size_t network_length = 0, token_length = 0;
  static const char* const kNames[] = {"range", "interval"};
  TargetQuery query(request.raw_target, kNames, 2);
  if (!query.ok()) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  bool valid = query.valid() && history_params(request, network, &network_length, token, &token_length);
  // z.enum([...]).default(...): one exact value, no trim.
  fortis::tokens::token_history::Range range = fortis::tokens::token_history::Range::k30d;
  fortis::tokens::token_history::Interval interval = fortis::tokens::token_history::Interval::k1h;
  if (valid && query.count(0))
    valid = query.count(0) == 1 && fortis::tokens::token_history::ParseRange(query.value(0), query.length(0), &range);
  if (valid && query.count(1))
    valid = query.count(1) == 1 && fortis::tokens::token_history::ParseInterval(query.value(1), query.length(1), &interval);
  if (!valid) {
    send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  if (!fortis::tokens::token_history::ResolutionAllowed(range, interval)) {
    send_bad_request(client, "1h liquidity history caps at 90 days", head, cost, remaining);
    return;
  }
  networks::TokenNetwork resolved;
  networks::TokenRef ref;
  if (!route_ref(client, head, network, network_length, token, token_length, &resolved, &ref, cost, remaining)) return;
  Work work{};
  if (!workspaces_.MakeHistory(&work)) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  Value* document = nullptr;
  Value* validator = nullptr;
  if (!fortis::tokens::token_history::LiquidityHistoryDocument(work, resolved.id, ref.token_id, range, interval, env_.swr->Now(),
                                                       &document, &validator)) {
    TokenWorkspaces::FreeHistory(&work);
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  send_structured(client, head, request, work, "token-liquidity-history", document, validator, kMinuteCacheControl,
                  cost, remaining);
  TokenWorkspaces::FreeHistory(&work);
}

namespace token_service_internal {

// GET /v1/networks/{network}/token/{tokenId}/liquidity/summary — R23, the
// compact 24-hour liquidity read of the token card (TypeScript:
// getTokenLiquiditySummary → LiquidityHistoryFacet.
// summary24h → sendConditionalStructured('token-liquidity-summary')). No
// runFacet: a failure is the global handler's 500. Answered on the HTTP
// worker from the KV; no provider.
//
// Request. GET, plus HEAD (same status and headers, no body). Other methods,
// '/' suffixes and spellings are the router's 404, and so are an empty value,
// a decoded control character/DEL and a value over 512 bytes; both values
// reach the handler percent-decoded (history_params). In order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, no quota;
//   2. a param over 100 UTF-16 units: 414 {"error":"URI_TOO_LONG"}, no quota;
//   3. the /v1 device policy: Origin 403, bearer 401 (ATTESTATION_REQUIRED
//      under ATTESTATION_MODE=required), the per-minute quota at cost 5
//      (requestCost's '/token/' rule; 429 retry-after); every later answer
//      carries x-rate-limit-cost: 5;
//   4. networkTokenParams (network 1..64, tokenId 1..160 units): bare 400.
//      The query string is ignored entirely;
//   5. registry.network / parseTokenRef: "unknown network" / "unsupported
//      token ref", as R20.
//
// Response (200), JSON.stringify order: {network, tokenId, availability:
// {status ("unknown" without a facts row), checkedAt, lastSuccessfulAt,
// source, latestAgeSeconds, stale} (note: age before stale, unlike
// /liquidity/history), latestAt, change24h}. Only raw rows with ts in
// [now - 27 h, now] are read; the successful ones (status ok and a non-null
// liquidity — a price-only row does not count, a not_found row never) give
// latest = the newest, latestAt its ts in epoch seconds or null; stale as
// R21 (age over max(1800, 2 x its sampling interval) s, or no latest);
// change24h = null when stale, else the successful row nearest latest - 24 h
// within 2 h either side (inclusive; the earlier on a tie) as {baselineAt
// (s), liquidityPct, pricePct} (null for a missing value or a zero baseline),
// or null without one. Sources are not distinguished.
//
// Representations: JSON or StructuredReadResponse protobuf (Vary: Accept
// only then); cache-control "private, max-age=60"; weak ETag
// W/"token-liquidity-summary[-protobuf]-<24 hex of sha256(body)>" over the
// whole body — latestAgeSeconds moves with the clock, so a revisit a second
// later is a 200 with a new ETag; If-None-Match equality gives 304.
//
// Persistence and effects: read-only over the facts document and raw
// observations (see R21). A store read or arena failure is 500 INTERNAL.
// Concurrency and bounds: any HTTP worker; at most 27 h of five-minute rows
// (326) and a growing arena of at most 16 MiB.
//
// Intentional differences: parser_baseline, bad_url, router_404, head_304
// (as R20).
}  // namespace token_service_internal

void TokenRoutes::HandleLiquiditySummary(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, attestation_required_);
  if (reply_policy(client, policy, head)) return;
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  char network[520], token[520];
  size_t network_length = 0, token_length = 0;
  if (!history_params(request, network, &network_length, token, &token_length)) {
    send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  networks::TokenNetwork resolved;
  networks::TokenRef ref;
  if (!route_ref(client, head, network, network_length, token, token_length, &resolved, &ref, cost, remaining)) return;
  Work work{};
  if (!workspaces_.MakeHistory(&work)) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  Value* document = nullptr;
  if (!fortis::tokens::token_history::LiquiditySummaryDocument(work, resolved.id, ref.token_id, env_.swr->Now(), &document)) {
    TokenWorkspaces::FreeHistory(&work);
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  send_structured(client, head, request, work, "token-liquidity-summary", document, nullptr, kMinuteCacheControl,
                  cost, remaining);
  TokenWorkspaces::FreeHistory(&work);
}


}  // namespace fortis::tokens
