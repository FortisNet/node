#include "price_diagnostics.h"
#include <memory>
#include <string_view>
#include "price_spot_route.h"
#include "ownership.h"

#include "device_enrollment.h"
#include "price_spot_query.h"
#include "threading.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

namespace {
// Interactive spot requests have a shorter budget than background jobs.
// Let the owner return its cache floor or explicit missing IDs before the
// handoff times out, even when an upstream attempt stalls.
constexpr uint32_t kInteractiveReservationMs = 5000;
constexpr uint32_t kOwnerAnswerMarginMs = 1000;
constexpr size_t kRequestSlots = http::HttpResponseHandoff::kCapacity;

#ifdef FORTIS_SPOT_ROUTE_CONCURRENCY_TEST_ONLY
uint32_t reservation_ms = kInteractiveReservationMs;
uint32_t owner_answer_ms = kInteractiveReservationMs - kOwnerAnswerMarginMs;
#else
constexpr uint32_t reservation_ms = kInteractiveReservationMs;
constexpr uint32_t owner_answer_ms = kInteractiveReservationMs - kOwnerAnswerMarginMs;
#endif

struct RequestSlot {
  bool used = false;
  unsigned references = 0;
  uint64_t handoff_token = 0;
  PriceSpotRouteContext* route = nullptr;
  int rate_remaining = 0;
  uint64_t owner_started_ms = 0;
};
RequestSlot slots[kRequestSlots]{};
fortis::base::Mutex slots_mutex;

uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void release_slot(RequestSlot* slot) {
  fortis::base::LockGuard slots_lock(slots_mutex);
  if (slot->references) --slot->references;
  if (!slot->references) *slot = RequestSlot{};
}

RequestSlot* acquire_slot(PriceSpotRouteContext* route) {
  fortis::base::LockGuard slots_lock(slots_mutex);
  RequestSlot* found = nullptr;
  for (auto& slot : slots) if (!slot.used) { found = &slot; break; }
  if (found) { *found = RequestSlot{}; found->used = true; found->route = route; found->references = 1; }
  return found;
}

void respond(int client, const http::HttpResponse& response, bool head) { WriteHttpResponse(client, response, head); }
// Every non-200 reply: JSON, no Vary (TypeScript adds `Vary: Accept` only in
// sendProtobufOrJson, which error replies never reach).
http::HttpResponse error_response(int status, const char* reason, const char* body,
                            int rate_cost = -1, uint32_t remaining = 0,
                            size_t length = static_cast<size_t>(-1)) {
  http::HttpResponse out{status, reason, "application/json; charset=utf-8",
                   reinterpret_cast<const uint8_t*>(body),
                   length == static_cast<size_t>(-1) ? strlen(body) : length};
  out.rate_limit_cost = rate_cost; out.rate_limit_remaining = remaining;
  if (status == 503) out.retry_after = 1;
  return out;
}

void handoff_terminal(void* opaque, bool cancelled) {
  auto* slot = static_cast<RequestSlot*>(opaque);
  // The TypeScript prices facet does not propagate route disconnects into the
  // shared provider work; let it finish and populate cache for the next caller.
  (void)cancelled;
  release_slot(slot);
}

void owner_complete(void* opaque, const SpotProviderOwnerResult& result) {
  auto* slot = static_cast<RequestSlot*>(opaque);
  const uint32_t remaining = static_cast<uint32_t>(slot->rate_remaining);
  http::HttpResponse response{};
  switch (result.status) {
    case SpotProviderOwnerResultStatus::kComplete:
      response = {200, "OK", result.content_type, result.body, result.body_length};
      if (result.content_type && strcmp(result.content_type, "application/json") == 0)
        response.content_type = "application/json; charset=utf-8";
      response.vary_accept = true; response.rate_limit_cost = 1;
      response.rate_limit_remaining = remaining; break;
    case SpotProviderOwnerResultStatus::kUnsupportedProvider:
      response = error_response(501, "Not Implemented", "{\"error\":\"UNSUPPORTED_PROVIDER\"}", 1, remaining); break;
    case SpotProviderOwnerResultStatus::kTimeout:
      response = error_response(504, "Gateway Timeout", "{\"error\":\"TIMEOUT\"}", 1, remaining); break;
    case SpotProviderOwnerResultStatus::kShutdown:
      response = error_response(503, "Service Unavailable", "{\"error\":\"SHUTDOWN\"}", 1, remaining); break;
    default:
      response = error_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", 1, remaining); break;
  }
  const uint64_t finished = monotonic_ms();
  if (slot->owner_started_ms && finished >= slot->owner_started_ms)
    response.owner_ms = static_cast<double>(finished - slot->owner_started_ms);
  const bool delivered = slot->route->handoff->Complete(slot->handoff_token, response);
  (void)delivered;
  release_slot(slot);
}

// GET /v1/prices (operation getSpotPrices) — reproduces the earlier
// TypeScript implementation: its prices route, PricesFacet.spot,
// parsePriceId and the /v1 preHandler.
//
// Route. GET only; HEAD runs this handler and writes the same status and
// headers (Content-Length of the would-be body) without a body. Exact path:
// `/v1/prices/`, `/V1/prices` and `/v1/prices/history` (GET) are not this
// route. No request body is read.
//
// Order and device policy (AuthorizeDeviceRequest, shared by /v1): Origin
// present → 403 BROWSER_ORIGIN_FORBIDDEN; missing/malformed/unknown bearer →
// 401 UNAUTHORIZED; revoked/expired → 401 SESSION_EXPIRED; attestation
// required and not attested → 401 ATTESTATION_REQUIRED (none carry rate
// headers). Then the per-device minute quota at cost 1 (development 240,
// default 120, attested 600, premium 1200): every later reply, including 400s,
// carries x-rate-limit-cost: 1 and x-rate-limit-remaining; over quota → 429
// RATE_LIMITED with Retry-After: 60. Validation runs only after all of that.
//
// Query (fortis::prices::spot_query::ParseQuery). fast-querystring semantics: '&'
// pairs, the first '=' splits, '+' is a space, percent escapes decoded as
// strict UTF-8 — a malformed escape or invalid/truncated UTF-8 keeps the raw
// component text (so `ids=%FF` is the id "%FF"); a pair without '=' is an
// empty value; other keys are ignored; a repeated key is an array. zod then
// checks `ids` (string, 1–4000 UTF-16 code units) and `fiat` (string matching
// /^[a-z]{3,5}$/, default "usd") and answers 400 {"error":"BAD_REQUEST"}
// when either fails. Next
// `ids.split(',').map(trim).filter(nonEmpty)` with JavaScript's trim set
// (incl. U+00A0, U+2028, U+3000, U+FEFF; not U+0085 or U+200B); zero or more
// than 100 pieces → 400 {"error":"BAD_REQUEST","detail":"1–100 ids"}. Ids are
// not validated here: each piece is answered under the exact (trimmed) string
// the client sent.
//
// Result (SpotProviderOwner + spot_resolution + spot_response_serializer).
// 200 `{"fiat","prices":{raw: {sourceId, price, change24hPct, asOf, stale,
// source, cache:{hit,timestamp}}},"missing":[raw…],"invalid":[raw…]}` with
// Content-Type application/json; charset=utf-8 and Vary: Accept, or the
// SpotPriceResponse protobuf (application/protobuf, same Vary) when Accept
// names application/protobuf with q > 0 (AcceptsProtobuf). Ids are parsed
// by ParsePriceId (CoinGecko slug, spl:, asa:, aptos:, tron:trc10:,
// {network}:erc20|trc20:{address}); grammar failures go to `invalid` in
// request order; aliases sharing a canonical source id are fetched once and
// each keyed as sent (`prices` keeps JavaScript object order: integer-like
// keys first, ascending); `missing` lists unpriced ids grouped by canonical
// id in first-appearance order. Cache-Control: no-store plus the security
// header baseline on every reply.
//
// Cache, TTL and provider (per canonical source id and fiat): a fresh
// envelope (`price/spot/v1/{fiat}/{sourceId}`, 30 s) answers cache.hit true,
// stale false; a stale one (≤ 10 min past fresh) answers stale true with
// cache.hit true and starts one background revalidation; a negative envelope
// (provider answered without the id, 5 min) is `missing` with no call.
// Misses are grouped per provider into windowed batches (20 ms, ≤ 250 ids):
// CoinGecko /simple/price for slugs (any fiat), /simple/token_price for
// indexed contracts, Jupiter (spl), Vestige (asa), TronScan (trc10) and
// Robinhood (its stock tokens) in USD only — those, and contracts on
// unindexed networks, become negatives without a call. A failed provider
// call (3 attempts, 250/1000 ms backoff, 10 s each) writes nothing; the
// durable floor (`price/series/v1/…` last spot, kept 7 days) then answers
// stale true, cache {hit:true, timestamp: its asOf}, else the id is
// `missing`. Positive answers write the envelope and the series spot and
// open candles in one KV frame (durable across restart and SIGKILL).
//
// Concurrency: one in-flight fetch per (fiat, source id) is shared by every
// request (and by revalidations); a request completes from the owner thread
// through the response handoff, never pinning an HTTP worker. The router
// runs GET and HEAD without route_state_mutex
// (spot_prices_run_without_state_lock in http_server.cpp): this handler may
// run on every HTTP worker at once, so the RequestSlot table is guarded by
// slots_mutex alone, and anything added here needs its own lock and a fresh
// audit there. A slot holds one reference per owner: this handler (dropped
// before it returns), the handoff reservation (handoff_terminal, exactly
// once: completion, client disconnect, the handoff deadline or Stop) and the
// owner (owner_complete, exactly once, or dropped here when Submit
// refuses). A client that disconnects does not cancel shared provider work;
// the result still fills the cache. Client demand for CoinGecko slugs is
// recorded for the coin price scheduler with the device id.
//
// Bounded native differences (TypeScript has no such limits):
// - an id longer than 512 bytes → 400 {"error":"BAD_REQUEST","detail":"ids
//   are limited to 512 bytes each"} (TypeScript: 200 with it in `invalid`, or
//   in `missing` for an erc20 id on a long unindexed network name);
// - at most 8 requests in the owner at once and 16 pending handoffs → 503
//   {"error":"UNAVAILABLE"}; shutdown → 503 {"error":"SHUTDOWN"}; an owner
//   fault → 500 {"error":"INTERNAL"};
// - a provider still unanswered four seconds after submission is given up for this
//   request: its ids answer from the durable floor or as `missing` (200),
//   and a batch no other request waits for is cancelled without a write.
//   TypeScript reaches the same answer when its third 10 s attempt fails
//   (~31.25 s) and lets the shared fetch run on. A request the owner never
//   planned by then gets 504 {"error":"TIMEOUT"};
// - the on-demand fresh TTL is exactly 30 s (TypeScript jitters ±10 %);
// - request targets over 12,500 bytes are refused by the HTTP parser and raw
//   (unescaped) non-ASCII target bytes are UTF-8, not Latin-1 (router-wide).
// Privacy: replies never echo the bearer or device id; error bodies are fixed
// strings without the input value.
// The established resolver keeps alias ordering, validation and wire parity.
// Keep its large scratch off the request stack and bound this fast path to
// ordinary wallet batches. Revalidate stale quotes through the owner.
bool serve_cached(PriceSpotRouteContext& route, int client, bool head,
                  const std::string_view* ids, size_t count, std::string_view fiat,
                  SpotProviderOwnerFormat format, const auth::DevicePolicyResult& policy,
                  http::HttpAdmission fallback = http::HttpAdmission::None, uint64_t token = 0) {
  constexpr size_t kFastCacheMaxIds = 32;
  if (!route.fast_cache || count > kFastCacheMaxIds) return false;
  std::unique_ptr<SpotResolutionWorkspace, fortis::base::ObjectDeleter<SpotResolutionWorkspace>> scratch(fortis::base::NewDefaultInit<SpotResolutionWorkspace>());
  if (!scratch) return false;
  auto* work = scratch.get();
  timespec wall{};
  if (clock_gettime(CLOCK_REALTIME, &wall) != 0) return false;
  const int64_t now = static_cast<int64_t>(wall.tv_sec) * 1000 + wall.tv_nsec / 1000000;
  if (!BeginSpotResolution(ids, count, fiat, *route.fast_cache, now, work).ok()) return false;
  size_t fetches = 0;
  SpotResolutionFetchIds(work, &fetches);
  if (fetches && fallback == http::HttpAdmission::None) return false;
  if (fallback != http::HttpAdmission::None) {
    // Use the resolver's normal provider-failure rules without storing a
    // negative. A genuinely cold batch still returns explicit overload.
    CompleteSpotResolution(work, *route.fast_cache, nullptr, now);
  }
  bool ready = false;
  const auto* result = SpotResolutionResponse(work, &ready);
  if (!result || !ready ||
      (fallback != http::HttpAdmission::None && !result->price_count)) return false;
  uint8_t buffer[24 * 1024];
  size_t used = 0;
  const auto encoded = format == SpotProviderOwnerFormat::kProtobuf
      ? SerializeSpotResponseProtobuf(*result, buffer, sizeof(buffer), &used)
      : SerializeSpotResponseJson(*result, reinterpret_cast<char*>(buffer), sizeof(buffer), &used);
  if (encoded != SpotSerializeResult::kOk) return false;
  ReportMissingPrices(route.errors, *result);
  const char* type = format == SpotProviderOwnerFormat::kProtobuf
      ? "application/protobuf" : "application/json; charset=utf-8";
  http::HttpResponse response{200, "OK", type, buffer, used};
  response.vary_accept = true;
  response.write_timeout_ms = 100;
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  response.admission = fallback;
  if (fallback == http::HttpAdmission::None) {
    size_t intents = 0;
    const auto* work_intents = SpotResolutionIntents(work, &intents);
    std::string_view stale_ids[kFastCacheMaxIds];
    size_t stale_count = 0;
    for (size_t i = 0; i < intents; ++i) {
      if (work_intents[i].kind == SpotResolutionIntentKind::kBackgroundRevalidation)
        stale_ids[stale_count++] = work_intents[i].source_id;
    }
    if (stale_count) {
      // Stale-only batches already have a wire response. Give revalidation
      // to the bounded owner using only the canonical IDs that need work.
      // Resolution emits at most one revalidation per unique input.
      (void)route.owner->Submit(stale_ids, stale_count, fiat, SpotProviderOwnerFormat::kJson,
          monotonic_ms() + kSpotProviderOwnerMaxDeadlineMs, [](void*, const SpotProviderOwnerResult&) {},
          nullptr, SpotSubmitMode::kBackground);
    }
  }
  if (!token) HttpNoteAdmission(fallback);
  return token ? route.handoff->Complete(token, response)
               : WriteHttpResponse(client, response, head);
}

void handle_prices(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* route = static_cast<PriceSpotRouteContext*>(opaque);
  const auth::DevicePolicyResult policy = route->shared_price_auth
      ? auth::AuthorizeSnapshotDeviceRequest(request, route->attestation_required)
      : auth::AuthorizeDeviceRequest(request, 1, route->attestation_required);
  if (policy.status) {
    http::HttpResponse response = error_response(policy.status, policy.reason,
        policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}",
        policy.rate_limit_cost, policy.rate_limit_remaining);
    response.retry_after = policy.retry_after; respond(client, response, head); return;
  }
  fortis::prices::spot_query::Query query;
  const char* mark = request.raw_target ? strchr(request.raw_target, '?') : nullptr;
  fortis::prices::spot_query::ParseQuery(mark ? mark + 1 : nullptr, mark ? strlen(mark + 1) : 0, &query);
  if (query.status != fortis::prices::spot_query::SpotQueryOutcome::kOk) {
    respond(client, error_response(400, "Bad Request", query.error_body, 1,
                                   policy.rate_limit_remaining, query.error_body_length), head);
    return;
  }
  std::string_view raw[kSpotResolutionMaxIds]{};
  const size_t count = query.id_count;
  for (size_t i = 0; i < count; ++i)
    raw[i] = {query.ids + query.id_offset[i], query.id_length[i]};
  const std::string_view fiat{query.fiat, strlen(query.fiat)};
  const auto format = request.accept && AcceptsProtobuf(request)
      ? SpotProviderOwnerFormat::kProtobuf : SpotProviderOwnerFormat::kJson;
  if (serve_cached(*route, client, head, raw, count, fiat, format, policy)) {
    route->owner->RecordDemand(raw, count, policy.device.id);
    return;
  }
  RequestSlot* slot = acquire_slot(route);
  if (!slot) {
    if (serve_cached(*route, client, head, raw, count, fiat, format, policy, http::HttpAdmission::SpotSlots)) {
      route->owner->RecordDemand(raw, count, policy.device.id);
      return;
    }
    HttpNoteAdmission(http::HttpAdmission::SpotSlots);
    respond(client, error_response(503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}",
                                   1, policy.rate_limit_remaining), head);
    return;
  }
  slot->rate_remaining = static_cast<int>(policy.rate_limit_remaining);
  fortis::base::UniqueLock slots_lock(slots_mutex); ++slot->references; slots_lock.Unlock(); // reservation callback ref
  if (!http::ReserveHttpResponse(client, head, reservation_ms,
                           &slot->handoff_token, handoff_terminal, slot)) {
    release_slot(slot);
    release_slot(slot);
    if (serve_cached(*route, client, head, raw, count, fiat, format, policy, http::HttpAdmission::ResponseSlots)) {
      route->owner->RecordDemand(raw, count, policy.device.id);
      return;
    }
    HttpNoteAdmission(http::HttpAdmission::ResponseSlots);
    respond(client, error_response(503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}", 1, policy.rate_limit_remaining), head); return;
  }
  slots_lock.Lock(); ++slot->references; slots_lock.Unlock(); // owner callback ref
  slot->owner_started_ms = monotonic_ms();
  const auto submitted = route->owner->Submit(raw, count, fiat, format,
      slot->owner_started_ms + owner_answer_ms, owner_complete, slot,
      SpotSubmitMode::kForeground, policy.device.id[0] ? policy.device.id : nullptr).status();
  if (submitted != SpotProviderOwnerSubmitStatus::kAccepted) {
    release_slot(slot); // No owner callback will arrive.
    if (serve_cached(*route, client, head, raw, count, fiat, format, policy, http::HttpAdmission::SpotOwner,
                     slot->handoff_token)) {
      release_slot(slot);
      return;
    }
    http::HttpResponse unavailable = error_response(503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}", 1, policy.rate_limit_remaining);
    unavailable.admission = http::HttpAdmission::SpotOwner;
    route->handoff->Complete(slot->handoff_token, unavailable);
    release_slot(slot); // route reference
    return;
  }
  // Drop the route's reference; owner and handoff each own one now.
  release_slot(slot);
}
} // namespace

#ifdef FORTIS_SPOT_ROUTE_CONCURRENCY_TEST_ONLY
void SetPriceSpotRouteDeadlinesForTest(uint32_t reservation, uint32_t owner_answer) {
  reservation_ms = reservation;
  owner_answer_ms = owner_answer;
}

size_t PriceSpotRouteSlotsForTest(unsigned* references, size_t capacity) {
  size_t used = 0;
  fortis::base::LockGuard slots_lock(slots_mutex);
  for (const auto& slot : slots) {
    if (!slot.used) continue;
    if (used < capacity) references[used] = slot.references;
    ++used;
  }
  return used;
}
#endif

bool RegisterPriceSpotRoute(http::HttpRouter& router, PriceSpotRouteContext* context) {
  return context && context->owner && context->handoff &&
      router.Add("GET", "/v1/prices", handle_prices, context);
}

}  // namespace fortis::prices
