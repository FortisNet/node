#include "ownership.h"
// The Robinhood routes on the HTTP worker: the parameter and body rules,
// the shared steps of each route, the handlers and handing a job to the
// service's workers.
#include "robinhood_service_internal.h"
#include "threading.h"

namespace rh = fortis::exchange::robinhood;

namespace tj = fortis::codec::tj;

namespace fortis::exchange {

namespace {

// A parameter over 100 UTF-16 units once decoded: 414 URI_TOO_LONG before
// any hook (no auth, no quota).
bool reject_long_param(const http::HttpRequest&, http::HttpReply& reply, const http::HttpPathParam* param) {
  if (!param || param->utf16_units <= 100) return false;
  http::SendRefusal(reply, 414, "URI Too Long", "URI_TOO_LONG");
  return true;
}

// ---- request bodies (content-type parsing, after the device hook) ------------------------

enum class BodyKind : uint8_t { kUndefined, kValue, kProtobuf };
struct Body {
  BodyKind kind = BodyKind::kUndefined;
  Value* value = nullptr;
  const uint8_t* bytes = nullptr;
  size_t length = 0;
};

// The /v1 parsers: JSON and protobuf; any other media type is 415.
//
// The media type selection and its refusals are the shared SelectBodyParser
// (after the device hook, with its quota headers); what each parser's body
// becomes is this family's.
bool parse_body(const http::HttpRequest& request, http::HttpReply& reply, http::HttpQuota quota, tj::Arena& arena,
                Body* body) {
  body->bytes = reinterpret_cast<const uint8_t*>(request.body);
  body->length = request.body_length;
  http::HttpBodyParser parser = http::HttpBodyParser::kNone;
  if (!SelectBodyParser(request, reply, quota, &parser)) return false;
  switch (parser) {
    case http::HttpBodyParser::kNone:
      return true;
    case http::HttpBodyParser::kJson:
      body->value = tj::Parse(arena, body->bytes, body->length);
      if (!body->value) {
        SendInvalidJsonBody(reply, quota);
        return false;
      }
      body->kind = BodyKind::kValue;
      return true;
    case http::HttpBodyParser::kProtobuf:
      body->kind = BodyKind::kProtobuf;
      return true;
  }
  return false;
}

// ---- the shared steps of each route, in TypeScript's order -----------------------------------

// GET capabilities: the device hook at cost 1.
constexpr http::HttpStep<auth::DeviceStepState> kCapabilitiesSteps[] = {auth::RequireDevice<auth::DeviceStepState, 1>};

// GET stock-tokens and stock-tokens/{address}: the parameter length limit
// (before any hook), then the device hook at cost 1, then the address.
struct ReadState {
  bool attestation_required = false;
  const http::HttpPathParam* param = nullptr;  // the instrument's address; null for the catalog
  auth::DevicePolicyResult device{};
};
bool RejectLongParam(const http::HttpRequest& request, http::HttpReply& reply, ReadState& state) {
  return !reject_long_param(request, reply, state.param);
}
constexpr http::HttpStep<ReadState> kReadSteps[] = {RejectLongParam, auth::RequireDevice<ReadState, 1>};

// POST diagnose, prepare and submit: the device hook at cost 20, then the
// body parser (auth-first: an unauthenticated request is 401 whatever its
// body).
struct WriteState {
  bool attestation_required = false;
  tj::Arena arena{};
  Body body;
  auth::DevicePolicyResult device{};
};
bool ParseWriteBody(const http::HttpRequest& request, http::HttpReply& reply, WriteState& state) {
  const auth::DeviceQuota quota = auth::DeviceQuotaOf(state.device);
  return parse_body(request, reply, http::HttpQuota{quota.cost, quota.remaining}, state.arena, &state.body);
}
constexpr http::HttpStep<WriteState> kWriteSteps[] = {auth::RequireDevice<WriteState, 20>, ParseWriteBody};

}  // namespace

// ---- handlers ----

// GET /v1/networks/robinhood/stock-tokens
// (TypeScript: the stock-tokens handler -> RobinhoodMarketsFacet.catalog()
// -> RobinhoodStockTokenClient.assets() / parseAssets, MediaFacet.register,
// catalogMessage.)
//
// What it answers. Robinhood's issuer list of tokenized US stocks on
// Robinhood Chain (chain id 4663), proxied so the wallet never talks to
// Robinhood. The wallet (IntelligenceApi.robinhoodStockTokens) always asks
// for protobuf, and uses the list to recognise stock-token contracts and
// their logos and to gate sponsored trades.
//
// Request. GET, plus HEAD (the same headers and Content-Length, no body;
// a HEAD still runs the read and may fetch). The query string, request
// body, conditional headers, cookies and every other header are ignored and
// never forwarded. Other methods and spellings of the path are the router's
// 404. "stock-tokens/" is the instrument route with an empty address.
//
// Policy, in this order (AuthorizeDeviceRequest, as the /v1 preHandler):
// any Origin header 403 BROWSER_ORIGIN_FORBIDDEN; a malformed or unknown
// bearer 401 UNAUTHORIZED; a revoked device or expired token 401
// SESSION_EXPIRED; under ATTESTATION_MODE=required an unattested device 401
// ATTESTATION_REQUIRED; then quota cost 1 per wall-clock device minute
// (development 240, standard 120, attested 600, premium 1,200), over it 429
// RATE_LIMITED with Retry-After: 60. These refusals carry no Vary, and
// only the quota 429 carries x-rate-limit-cost/-remaining. A refused
// request never reaches the provider or the cache.
//
// The provider read. GET https://api.robinhood.com/rhj/assets on the
// "robinhood-assets" lane (60 requests/s, 8 concurrent) with only
// `accept: application/json`. Each attempt has HttpClient's 10 s timeout. A
// transport failure, 5xx, 408 or non-JSON 2xx is tried three times in all
// (250 ms, then 1 s apart); a 4xx or 429 is not retried. A 429 also opens
// the lane's breaker for max(Retry-After, 30 s), and five transport-class
// failures in a row (no answer, timeout, 5xx, 408, 401, 403) open it for
// 30 s. The whole read stops at the 25 s provider
// deadline. The body must be an object whose `assets` is an array,
// otherwise the read fails.
//
// The catalog (parseAssets). A row needs: `id` (0x + 64 hex, lower-cased);
// `tokenSymbol` (at most 32 UTF-16 units) and `tokenName` (at most 160),
// both trimmed and non-blank; a positive decimal `currentMultiplier`
// (at most 18 fraction digits); and a deployment with Number(chainId) ===
// 4663 and a 0x + 40 hex contract (the first such one). Other rows are
// dropped silently. A repeated contract is kept, because TypeScript's
// `!seen.add()` is never true. Rows are stable-sorted by
// tokenSymbol: byte order after ASCII lower-casing, a prefix first, then the
// symbols' own bytes (discovery::RobinhoodSymbolCompare).
//
// Dynamic fields, per row:
//   - logoUrl is stockLogoUrl(symbol), the Financial Modeling Prep URL for
//     a trimmed, upper-cased ticker matching ^[A-Z0-9][A-Z0-9.-]{0,31}$.
//     Otherwise it is the issuer logo.
//   - issuerLogoUrl is the issuer logo: its WHATWG href, only for https:
//     without credentials, otherwise null.
//   - logoMediaKey is the first 32 hex digits of sha256(logoUrl) (null
//     without a logo). The URL is registered as a pending media record.
//   - pendingMultiplier is "" without a valid pending multiplier.
//     pendingMultiplierEffectiveTime is an ISO time only with one.
//   - status is ASSET_STATUS_ACTIVE, _INACTIVE or _UNKNOWN.
//   - tradingCapabilities has lower-cased statuses without the
//     "trading_status_" prefix, with the facet's fallbacks.
//
// The cache. One parsed catalog per process, kept 15 minutes from the
// moment its request finished (the route clock). Until then no request is
// made. At exactly 15 minutes the next read refetches. There is no stale
// serving: a failed refetch answers its error, keeps nothing and leaves
// the expired catalog unserved. Simultaneous reads share one request and
// its result. A read that waited in the job queue while that request
// failed takes the same failure instead of starting another request, as a
// TypeScript caller awaiting the in-flight promise would
// (StockClient::Assets' issued generation). A restart starts cold.
//
// Errors. A busy lane, full lane queue or open breaker: 429 RATE_LIMITED
// {"error":"RATE_LIMITED","detail":"Upstream data is temporarily busy;
// retained data remains available."} with Retry-After in whole seconds
// (at least 1). Any other failure: 502 {"error":"UPSTREAM_UNAVAILABLE"}.
// Both carry x-rate-limit-cost: 1 and -remaining, and no Vary.
//
// Encoding. Success is 200 with Vary: Accept, x-rate-limit-cost: 1 and
// x-rate-limit-remaining. Accept negotiation is AcceptsProtobuf (see
// Capabilities).
//   JSON: {"assets":[{id, tokenSymbol, tokenName,
//     deployments:[{chainId:4663, contractAddress}], currentMultiplier,
//     pendingMultiplier, pendingMultiplierEffectiveTime, logoUrl,
//     issuerLogoUrl, logoMediaKey, status, tradingCapabilities:
//     {fractionalTradability, allDayTradability,
//     extendedHoursFractionalTradability, market, extended, overnight
//     (each {whole, fractional})}}...],"source":"robinhood-assets"}.
//     Absent values are JSON null. A lone surrogate in provider text is
//     written as \udXXX.
//   Protobuf (RobinhoodStockCatalogResponse): assets=1, source=2.
//     - Implicit strings are omitted when empty, notably pendingMultiplier.
//     - The optional strings and the optional bool are omitted when null.
//     - The three sessions are always present, even when empty.
//     - A lone surrogate is written as U+FFFD.
//   An empty catalog is {"assets":[],"source":"robinhood-assets"}, or the
//   18-byte protobuf with only `source`.
//
// Concurrency and bounds.
//   - The read runs as a job: 8 job slots over 4 workers, answered through
//     the 30 s response handoff. With every slot taken: 429 RATE_LIMITED
//     with Retry-After: 2.
//   - Provider bodies are at most 512 KiB (depth 64). A larger declared
//     body fails that one request: 502, not retried.
//   - An answer over 2 MiB is 500 {"error":"RESPONSE_TOO_LARGE"}.
//   - A full 512 KiB catalog of short rows (1,978 instruments) answers
//     1.4 MB of JSON. Only rows whose display logos expand (for example,
//     percent-encoded spaces) can pass 2 MiB.
//
// Durable effects. Each display logo not yet registered writes one KV
// record, media/item/{key} {"key","sourceUrl","status":"pending",...},
// which expires after 7 days. An existing record, readable or not, is
// never overwritten. The record is what makes GET /v1/media/{key} serve
// that URL. The device policy writes its quota counter and the throttled
// last-seen presence. Nothing else is stored: the catalog cache is process
// memory.
//
// Privacy. The provider request carries no client header, token, device id
// or address. The answer contains issuer data only.
//
// Intentional differences from TypeScript:
//   - The canary gate. Without FORTIS_NATIVE_ROBINHOOD_CANARY=1 the route
//     is 404 (TypeScript always serves it).
//   - The bounds above. TypeScript parses any provider body and sends any
//     answer.
//   - The deadline. When every attempt times out, native answers its 502
//     at the 25 s provider deadline; TypeScript answers after about 31 s.
//   - Collation beyond ASCII is by code point. Robinhood tickers are ASCII;
//     ICU would place, for example, "É" beside "E".
//   - The router.
//     - A percent-encoded path segment is not decoded: 404, where
//       TypeScript answers the catalog.
//     - 404 bodies differ.
//     - The security headers and Connection: close are on every response.
//   - Quota minutes are KV windows that survive a restart. The job-slot
//     back-pressure above has no TypeScript equivalent.
//   - The token canary's StockClient keeps its own catalog cache.
void RobinhoodService::Impl::HandleCatalog(const http::HttpRequest& request, http::HttpReply& reply) {
  this->Read(request, reply, Kind::kCatalog);
}
// GET /v1/networks/robinhood/stock-tokens/{address}
// (TypeScript: the stock-tokens/:address handler ->
// RobinhoodMarketsFacet.instrument() ->
// RobinhoodStockTokenClient.assetByAddress / referenceQuote /
// corporateActionsFor, MediaFacet.register, instrumentMessage.)
//
// What it answers. One Robinhood stock token: its catalog row, the
// issuer's reference quote and its corporate actions (splits, dividends),
// proxied so the wallet never talks to Robinhood. The wallet
// (IntelligenceApi.robinhoodStockTokenInstrument) always asks for
// protobuf, sends the contract through Uri.encodeComponent, and reads 404
// as "not a stock token" (null); any other non-200 is an error.
//
// Request. GET, plus HEAD (the same headers and Content-Length, no body;
// a HEAD still runs the reads). The query string, body, conditional
// headers, cookies and every other header are ignored and never forwarded.
// The router has already refused (404) an empty segment, a decoded control
// character/DEL and a value over 512 bytes; the address reaches this
// handler percent-decoded:
//   1. More than 100 UTF-16 units of decoded address: 414
//      {"error":"URI_TOO_LONG"} before any hook (no quota).
//   2. The device policy, in this order (AuthorizeDeviceRequest, the /v1
//      preHandler): any Origin header 403 BROWSER_ORIGIN_FORBIDDEN; a
//      malformed or unknown bearer 401 UNAUTHORIZED; a revoked device or
//      expired token 401 SESSION_EXPIRED; under ATTESTATION_MODE=required
//      an unattested device 401 ATTESTATION_REQUIRED; then quota cost 1 per
//      wall-clock device minute (development 240, standard 120, attested
//      600, premium 1,200), over it 429 RATE_LIMITED, Retry-After: 60.
//   3. The decoded address must be exactly /^0x[0-9a-fA-F]{40}$/:
//      otherwise 400 {"error":"BAD_REQUEST"}. So "%30x…" is a valid
//      address, while "0X…", a leading or trailing escaped space,
//      full-width digits or 41/43 hex digits are 400. Validation never
//      trims.
// A refused request reaches neither the provider nor a cache.
//
// The asset (assetByAddress). The issuer catalog, GET /rhj/assets, read
// and cached exactly as the catalog route's (HandleCatalog: parseAssets,
// 15 minutes, no stale serving), shared with it. The asset is the FIRST
// row of the sorted catalog whose contract equals the address
// case-insensitively. A contract the issuer lists more than once
// therefore resolves to the row whose tokenSymbol sorts first, whatever
// the issuer's order or the case of either spelling. No row: 404
// {"error":"NOT_FOUND"} (JSON even for a protobuf Accept), and no quote
// or corporate-action request is made.
//
// The two reads (Promise.all). The quote and the corporate actions start
// together: this worker reads the quote while its companion thread reads
// the corporate actions. The first one to fail answers at once with its
// error; the other still runs to its end (filling its cache if it
// succeeds), and the job slot is released only then. Both on the
// "robinhood-assets" lane (60 requests/s, 8 concurrent), with only
// `accept: application/json`, each attempt under HttpClient's 10 s timeout:
// a transport failure, 5xx, 408 or non-JSON 2xx is tried three times in
// all (250 ms, then 1 s apart), a 4xx or 429 once. A provider 429 opens the
// lane's breaker for max(Retry-After, 30 s); five transport-class failures
// in a row open it for 30 s. The whole read stops at the 25 s provider
// deadline.
//   - The quote: GET /rhj/prices/{encodeURIComponent(tokenSymbol)}
//     ("A&B/C" is A%26B%2FC; a symbol holding a lone surrogate cannot be
//     encoded and fails the read without a request: 502). From an object
//     whose `quotes` is an array, the first row whose tokenSymbol equals
//     the asset's exactly and that has a deployment with Number(chainId)
//     === 4663 and the asset's contract (any case). That row must have
//     currency "USD", positive bid and ask under Number() (so " 100 ",
//     "0x10", true and [2] count, "", "abc", 0 and Infinity do not), a
//     Date.parse-able generatedAt (answered as an ISO string), and
//     bidUsd = bid x currentMultiplier <= askUsd = ask x currentMultiplier,
//     both finite. Then midpointUsd = (bidUsd + askUsd) / 2, rawBidUsd and
//     rawAskUsd the unscaled prices, multiplier the asset's
//     currentMultiplier, dailyTradingVolume a finite number >= 0 from a
//     number or non-blank string (else null), tradingHalted
//     `isTradingHalt === true`, currency "USD". Anything else, the body
//     included, is a null quote, not an error. A usable quote is cached
//     per contract for 15 s from the moment its request finished; a null
//     quote and a failure are not cached. At most 512 contracts are
//     cached (the stalest idle one is replaced).
//   - The corporate actions: GET /rhj/corporate-actions. From `corpActions`
//     (anything else is an empty list) every row with trimmed, non-blank
//     id (<= 68 UTF-16 units), type (<= 80), status (<= 80) and tokenSymbol
//     (<= 32), and whose FIRST deployment with Number(chainId) === 4663 and
//     a 0x + 40 hex contract supplies the contract. processDate is
//     {year 1900..9999, month 1..12, day 1..31}, each an integer under
//     Number() ("2026", "0x7EA", true and [9] count), otherwise null.
//     details is the provider's object re-serialized as JSON does
//     (±Infinity becomes null, -0 becomes 0) when its JSON text is at most
//     8,000 UTF-16 units, otherwise {} (also for a non-object). The parsed
//     list is cached for one hour, an empty list from an unusable body
//     included; a failure is not cached.
//   - corporateActionsFor: the actions whose contract equals the asset's
//     (any case), newest first by Date.UTC(year, month - 1, day) (so
//     February 31 is March 3), ties in provider order, undated ones last.
// Simultaneous reads share one request per document. A read accepted
// while a request for the same document was running, or before a request
// that has since finished, takes that request's result (failure or null
// quote) instead of asking again, as a TypeScript caller awaiting the
// in-flight promise would (the issued generations recorded in Read).
//
// Errors. A busy lane, full lane queue or open breaker: 429 RATE_LIMITED
// {"error":"RATE_LIMITED","detail":"Upstream data is temporarily busy;
// retained data remains available."} with Retry-After in whole seconds
// (at least 1). Any other failure: 502 {"error":"UPSTREAM_UNAVAILABLE"}.
// Both, and the 400 and 404, carry x-rate-limit-cost: 1 and -remaining and
// no Vary.
//
// Encoding. Success is 200 with Vary: Accept, x-rate-limit-cost: 1 and
// x-rate-limit-remaining; Accept negotiation is AcceptsProtobuf (see
// Capabilities).
//   JSON: {"asset":{the catalog row: id, tokenSymbol, tokenName,
//     deployments, currentMultiplier, pendingMultiplier,
//     pendingMultiplierEffectiveTime, logoUrl, issuerLogoUrl, logoMediaKey,
//     status, tradingCapabilities},"quote":{symbol, contractAddress,
//     bidUsd, askUsd, midpointUsd, rawBidUsd, rawAskUsd, multiplier,
//     dailyTradingVolume, tradingHalted, generatedAt, currency} or null,
//     "corporateActions":[{id, type, status, tokenSymbol, contractAddress,
//     processDate, details}...],"source":"robinhood-assets",
//     "disclosures":{"instrument":"Tokenized debt security","ownership":
//     "Economic exposure; not direct share ownership or voting rights",
//     "history":"Chart history is built prospectively from observed issuer
//     reference quotes"}}. Numbers are JavaScript's shortest round-trip
//     form (1e+21, 1e-7); -0 is written 0.
//   Protobuf (RobinhoodStockInstrumentResponse): asset=1, quote=2 (absent
//     for a null quote), corporate_actions=3, source=4, disclosures=5 (map
//     entries in the order above). In the quote the doubles and strings
//     are implicit (omitted when 0 or empty), daily_trading_volume is
//     optional (written when non-null, a -0 keeping its sign) and
//     trading_halted is written only when true. process_date is absent
//     when null; details is a google.protobuf.Struct, always written (an
//     empty one for {}). A lone surrogate is written as U+FFFD.
//
// Concurrency and bounds.
//   - The read runs as a job: 8 job slots over 4 workers, each with one
//     companion thread, answered through the 30 s response handoff. With
//     every slot taken: 429 RATE_LIMITED with Retry-After: 2.
//   - Provider bodies are at most 512 KiB (depth 64); a larger declared
//     body fails that request, not retried. An answer over 2 MiB is 500
//     {"error":"RESPONSE_TOO_LARGE"}. The largest relevant document, a
//     512 KiB actions list of ~3,700 actions for one contract, answers
//     474 KB of JSON (196 KB protobuf); the newest-first sort is a merge
//     sort over arena scratch (n log n).
//   - The companion touches only the StockClient (its own scratch and
//     snapshot arenas, under the client's lock) and the job's immutable
//     token, policy and deadline. The job arena stays this worker's.
//
// Durable effects. After both reads succeed, the display logo (as in the
// catalog: the Financial Modeling Prep logo of a ticker symbol, else the
// issuer's https logo) is registered: if absent, one KV record
// media/item/{first 32 hex of sha256(logo)} {"key","sourceUrl","status":
// "pending",...} that expires after 7 days; an existing record is never
// overwritten. logoMediaKey is that key, even when the KV write fails
// (MediaFacet.register logs the failure and still returns the key); it is
// null without a display logo. A 404 or a failed read registers nothing.
// The device policy writes its quota counter and throttled presence.
// Nothing else is stored: the caches are process memory, so a restart
// starts cold.
//
// Privacy. Provider requests carry no client header, token or device id;
// the address reaches the provider only as the asset's symbol and never
// in a URL of its own. The answer contains issuer data only.
//
// Intentional differences from TypeScript:
//   - The canary gate. Without FORTIS_NATIVE_ROBINHOOD_CANARY=1 the route
//     is 404 (TypeScript always serves it).
//   - The bounds above. TypeScript parses any provider body (a 512 KiB + 1
//     actions document answers 200 there, 502 here) and sends any answer.
//   - The 25 s deadline. When every attempt of a read times out, native
//     answers 502 at 25 s; TypeScript after about 31 s.
//   - Concurrency. Reads beyond the 4 workers wait in the job queue, so
//     with more than four simultaneous cold reads of different
//     instruments a quote can start one provider round late (TypeScript
//     starts every read at once). The job-slot 429 has no TypeScript
//     equivalent, and a job holds its slot until both reads end even after
//     the first failure answered. The quote cache keeps at most 512
//     contracts (TypeScript's map is unbounded).
//   - The router: other methods and spellings are
//     the router's 404, whose bodies differ; native sends
//     Connection: close.
//   - Quota minutes are KV windows that survive a restart.
//   - A symbol encodeURIComponent cannot encode fails before the lane
//     (TypeScript throws inside the lane); both answer 502 without a
//     request.
//   - Test hosts only: the test transport skips the parse-retry sleeps, so
//     a non-JSON 2xx retried three times answers sooner there.
void RobinhoodService::Impl::HandleInstrument(const http::HttpRequest& request, http::HttpReply& reply) {
  this->Read(request, reply, Kind::kInstrument);
}
void RobinhoodService::Impl::HandleCapabilities(const http::HttpRequest& request, http::HttpReply& reply) {
  this->Capabilities(request, reply);
}
// R45 — POST /v1/networks/robinhood/transactions/diagnose
// (TypeScript: the diagnose handler →
// RobinhoodTransactionDiagnostics.diagnose → validateTradeCalls →
// AlchemyTransactionDiagnosticsClient; dynamicMessage.)
//
// What it answers. A pre-sign report for one or two Robinhood Chain calls
// the wallet is about to sign (IntelligenceApi.diagnoseRobinhoodTransaction,
// protobuf both ways): whether they would revert and why, balance changes,
// approvals and their warnings, internal calls and the fee. It reads; it
// never prepares, signs or sends a transaction, and the report never alters
// the calls.
//
// Request. POST only; GET/HEAD/PUT, a trailing slash or another case are the
// router's 404 (native body {"error":"NOT_FOUND"}, no hook). The query string
// is ignored. First the /v1 hook, in order (AuthorizeDeviceRequest, cost 20): any Origin
// header (also empty or "null") 403 BROWSER_ORIGIN_FORBIDDEN; a missing or
// malformed bearer and an unknown token 401 UNAUTHORIZED; an expired or
// revoked device 401 SESSION_EXPIRED; with ATTESTATION_MODE=required a
// development device 401 ATTESTATION_REQUIRED; the quota (requestCost: 20
// for /transactions/diagnose, charged before validation; development 240,
// standard 120, attested 600, premium 1,200 a minute) → 429
// {"error":"RATE_LIMITED"} with Retry-After. Steps before the quota carry no
// x-rate-limit headers; every later answer carries x-rate-limit-cost: 20
// and x-rate-limit-remaining. Then the body parser (Write → parse_body):
//   - no Content-Type: with a body 415 {"error":"UNSUPPORTED_MEDIA_TYPE"};
//     without one the body is undefined (a 400 at validation);
//   - JSON (any case, with parameters): empty or unparsable → 400
//     {"error":"BAD_REQUEST"}; one leading BOM is dropped and a repeated
//     key keeps its last value; `__proto__` is an ordinary key;
//   - application/protobuf (any case, with parameters): the raw bytes;
//   - any other media type (text/plain, form bodies, x-protobuf, ...): 415.
//
// Validation (the HTTP worker). Protobuf: RobinhoodDiagnoseRequest {from = 1,
// repeated RobinhoodEvmCall calls = 2 {to 1, data 2, optional value 3, gas 4,
// max_fee_per_gas 5, max_priority_fee_per_gas 6}, bool sponsored = 3} read by
// protobuf-es's rules (RobinhoodDecodeWriteRequest: one cursor, declared
// types at any wire type, unknown fields and groups skipped, strict UTF-8
// with one leading BOM dropped, a repeated `from` keeps the last) and mapped
// to {from, calls (to, data and the present optional fields), sponsored}.
// Then diagnosticsBody .strict(): from /^0x[0-9a-fA-F]{40}$/; calls 1..2 of
// {to: address, data: /^0x(?:[0-9a-fA-F]{2})*$/ of at most 65,538 units,
// value?, gas?, maxFeePerGas?, maxPriorityFeePerGas?: /^0x(?:0|[1-9a-fA-F]
// [0-9a-fA-F]*)$/}; sponsored a boolean (default false; null is refused).
// Any failure: 400 {"error":"BAD_REQUEST"}, no provider request. Without
// ALCHEMY_API_KEY: 503 {"error":"TRANSACTION_DIAGNOSTICS_UNAVAILABLE"} (the
// gas policy is not needed).
//
// Work (one job; 8 jobs, 4 workers; 25 s provider deadline under the 30 s
// handoff). The active catalog (StockClient, 15 minutes on the route clock;
// a failure is 500 INTERNAL, a busy lane 429 RATE_LIMITED with Retry-After).
// validateTradeCalls with `from` exactly as sent (a mixed-case sender fails
// every recipient check) → 400 BAD_REQUEST. Then eth_simulateV1, POST
// https://robinhood-mainnet.g.alchemy.com/v2/<key>, body
//   {"jsonrpc":"2.0","id":1,"method":"eth_simulateV1","params":[{"blockStateCalls":
//    [{"calls":[{from (lowercase), to, data (lowercase), value (default "0x0"),
//    gas?, maxFeePerGas?, maxPriorityFeePerGas? (as sent)}]}],
//    "validation":!sponsored,"traceTransfers":true}]}
// on lane alchemy-simulation (10/s, burst 3, 2 concurrent, 30 queued; a 429
// or five consecutive faults open it for 30 s), one attempt, 10 s abort,
// Content-Type and Accept application/json. A JSON-RPC error is a report:
// failedReport with decodeRevertReason(error.data) (Error(string) sliced to
// 240 UTF-16 units, or "Contract panic (0x…).") else the message (sliced to
// 240 units; "eth_simulateV1 rejected the request" when not a string; a
// blank one is null → "Simulation failed before broadcast."). A null
// error, a missing result, a non-2xx, an unparsable body or a timeout: 500
// {"error":"INTERNAL"}; a busy or open lane: 429 RATE_LIMITED. After a
// result, debug_traceCallMany on the same lane: params [[{"transactions":
// calls}],{"blockNumber":"latest","transactionIndex":-1},{"tracer":
// "callTracer"}]; any failure (also a 429, which opens the lane, or a
// timeout) or a null result leaves traceAvailable false. buildReport: per
// call status (quantity === 1n), gasUsed and revert reason, transfer-log
// balance changes, approvals and danger warnings, at most 40 internal calls,
// fee gasUsed × maxFeePerGas; simulatedAt is the route clock.
//
// Answer. 200 with Vary: Accept: the report as JSON, or with Accept
// application/protobuf RobinhoodDynamicResponse {payload: Struct} (numbers
// as doubles, a lone surrogate as U+FFFD). Errors are JSON without Vary.
// Every answer: Cache-Control: no-store, X-Content-Type-Options: nosniff,
// Referrer-Policy: no-referrer, Permissions-Policy.
//
// State, concurrency, bounds. No report cache, KV entry or ticket; the
// state touched is the catalog cache, the lanes and breakers (process
// memory: a restart refetches the catalog and closes the breakers) and the
// device quota. Concurrent requests share the catalog read and the lane.
// Request bodies at most 512 KiB; answers at most 2 MiB (500
// RESPONSE_TOO_LARGE); a provider body at most 512 KiB.
//
// Differences (asserted by tests/robinhood_writes_edges_test.py):
//   - bigint_640_bit: any quantity or fee product past 640 bits (e.g. a
//     160-hex-digit maxFeePerGas) is 500 INTERNAL; TypeScript's BigInt
//     reports it. Fail closed; no EVM quantity is that wide.
//   - router_404 bodies; quota Retry-After from the wall-clock minute
//     (TypeScript's frozen minute says 60); quota minutes are KV windows that
//     survive a restart; the 8-job pool answers a 9th simultaneous request
//     with 429 RATE_LIMITED Retry-After 2 (TypeScript queues); bodies over
//     512 KiB are 413 (router-wide); the canary gate
//     (FORTIS_NATIVE_ROBINHOOD_CANARY=1, otherwise 404).
void RobinhoodService::Impl::HandleDiagnose(const http::HttpRequest& request, http::HttpReply& reply) {
  this->Write(request, reply, Kind::kDiagnose);
}

// R43 — POST /v1/networks/robinhood/sponsored-trades/prepare
// (TypeScript: the prepare handler →
// RobinhoodSponsoredTradeGateway.prepare and validateTradeCalls /
// validatePrepared → AlchemyWalletClient.prepareCalls.)
//
// What it answers. A gas-sponsored stock-token trade prepared through
// Alchemy's EIP-7702 Wallet API under this deployment's gas policy, bound to
// a one-time ticket of the calling device. The wallet
// (RobinhoodSponsoredTradeService) signs the returned signature requests and
// submits only signatures with the ticket; the server never holds a key.
//
// Request. POST only; other methods and spellings are the router's 404
// (native body {"error":"NOT_FOUND"}). The query string is ignored. First
// the /v1 hook (cost 20, as above for diagnose): Origin 403
// BROWSER_ORIGIN_FORBIDDEN, bearer 401 UNAUTHORIZED, expired or revoked 401
// SESSION_EXPIRED, ATTESTATION_MODE=required 401 ATTESTATION_REQUIRED, quota
// 429 RATE_LIMITED (requestCost: 20 for /sponsored-trades/ other than
// capabilities, charged before validation). Then the body parser, as for
// diagnose: no Content-Type with a body 415; JSON (any case, parameters)
// empty or unparsable → 400 {"error":"BAD_REQUEST"}, a leading BOM dropped,
// a repeated key's last value; application/protobuf (any case) is the raw
// bytes; other types (text/plain and form bodies included) 415.
//
// Validation (the HTTP worker). Protobuf RobinhoodPrepareRequest {from = 1,
// repeated RobinhoodEvmCall calls = 2} by protobuf-es's rules
// (RobinhoodDecodeWriteRequest), mapped to {from, calls: [{to, data,
// value?}]} — a call's gas fields are dropped. prepareBody .strict(): from
// an address; calls 1..2 of {to: address, data: even hex of at most 65,538
// units, value?: a canonical 0x quantity} with no other key. Failure: 400
// {"error":"BAD_REQUEST"}. Without both ALCHEMY_API_KEY and
// ALCHEMY_GAS_POLICY_ID: 503 {"error":"SPONSORED_TRADES_UNAVAILABLE"}.
//
// Work (one job). The active catalog (as diagnose; failure 500, busy 429).
// validateTradeCalls with `from` lowercased: one official router call
// (Uniswap V2 Router02 or SwapRouter02; canonical calldata of the allowed
// methods, recipient = sender or a V3 sentinel, path of 2–4 tokens) and at
// most one exact ERC-20 approval of a stock or funding token to that router
// for exactly the swap input; value at most 100 ETH and none on an
// approval; the batch must touch an active stock token → else 400. Then
// wallet_prepareCalls, POST https://api.g.alchemy.com/v2/<key>, body
//   {"jsonrpc":"2.0","id":1,"method":"wallet_prepareCalls","params":[{"from":<lowercase>,
//    "chainId":"0x1237","calls":[{"to","data","value"} lowercase, value "0x0" by
//    default],"capabilities":{"paymasterService":{"policyId":<gas policy>}}}]}
// on lane alchemy-wallet (20/s, burst 5, 4 concurrent, 40 queued; 429 or
// five consecutive faults open it for 30 s), one attempt, 10 s abort. A
// JSON-RPC error, a missing result, a non-2xx, an unparsable body, a
// timeout, or an operation validatePrepared refuses (1–2 nodes, chainId
// "0x1237", signatureRequest personal_sign or eth_signAuthorization with a
// 32-byte rawPayload, a user-operation-v070 sender equal to `from`): 500
// {"error":"INTERNAL"}; a busy or open lane: 429 RATE_LIMITED Retry-After.
// Then the ticket store prunes expired tickets; at 1,000 pending (in flight
// or not) the operation is discarded with 503 SPONSORED_TRADES_UNAVAILABLE
// (after Alchemy, as TypeScript). Otherwise ticket = base64url of 24
// CSPRNG bytes (32 characters), stored with the device id, the operation's
// JSON and node count, expiring 120,000 ms after the route clock.
//
// Answer. 200 with Vary: Accept: {"ticket","expiresAt" (ISO, ms),
// "prepared" (Alchemy's operation as JSON.stringify writes it),
// "signatureRequests" (each node's request)} as JSON, or a
// RobinhoodDynamicResponse Struct (numbers as doubles, a 1e400 as an IEEE
// infinity, a lone surrogate as U+FFFD). Errors are JSON without Vary;
// the baseline security headers on every answer. The gas policy id appears
// only in the wallet_prepareCalls body and the key only in its URL path.
//
// State, concurrency, bounds. Tickets are process memory (TicketStore,
// mutex-guarded): a restart drops them as TypeScript's Map does. Concurrent
// prepares draw distinct tickets; the store never exceeds 1,000. Request
// bodies at most 512 KiB, answers 2 MiB, provider bodies 512 KiB.
//
// Differences (asserted by tests/robinhood_writes_edges_test.py, or named):
//   - the stored operations of all pending tickets are bounded to 16 MiB;
//     past it the prepare is 503 SPONSORED_TRADES_UNAVAILABLE (TypeScript
//     keeps any size);
//   - router_404 bodies; quota Retry-After from the wall clock; quota
//     minutes survive a restart; the 8-job 429 (TypeScript queues); 413
//     past 512 KiB (router-wide); the canary gate.
void RobinhoodService::Impl::HandlePrepare(const http::HttpRequest& request, http::HttpReply& reply) {
  this->Write(request, reply, Kind::kPrepare);
}

// R44 — POST /v1/networks/robinhood/sponsored-trades/submit
// (TypeScript: the submit handler →
// RobinhoodSponsoredTradeGateway.submit and attachSignatures →
// AlchemyWalletClient.sendPreparedCalls.)
//
// What it answers. Sends the wallet's signatures for the exact operation a
// prepare stored under a ticket of the same device. Only signatures travel
// from the wallet; the server cannot alter the operation or reuse a ticket.
//
// Request. POST only; other methods and spellings are the router's 404.
// Body parser, then the /v1 hook (cost 20), exactly as for prepare above.
//
// Validation (the HTTP worker). Protobuf RobinhoodSubmitRequest {ticket =
// 1, repeated signatures = 2} by protobuf-es's rules (a BOM-led ticket or
// signature loses its BOM; a repeated ticket keeps the last). submitBody
// .strict(): ticket /^[A-Za-z0-9_-]{32}$/, signatures 1..2 of
// /^0x[0-9a-fA-F]{130}$/ → else 400 {"error":"BAD_REQUEST"}. Without
// ALCHEMY_API_KEY: 503 {"error":"SPONSORED_TRADES_UNAVAILABLE"} (the gas
// policy is not needed; without it no ticket can exist, so a known-format
// ticket is 404). Then the ticket store, under its mutex: prune expired;
// no such ticket, another device's, expiresAt <= now (route clock) or in
// flight → 404 {"error":"SPONSORED_TRADE_NOT_FOUND"}; a signature count
// other than the operation's node count → 400 BAD_REQUEST (the ticket is
// untouched); otherwise the ticket is claimed (in flight) and its
// operation copied. No catalog read.
//
// Work (one job). attachSignatures: each node without its signatureRequest
// and with signature {"type":"secp256k1","data":<as sent>} (a node's
// existing signature key keeps its position; an "array" operation maps
// its data in order). wallet_sendPreparedCalls, POST
// https://api.g.alchemy.com/v2/<key>, body {"jsonrpc":"2.0","id":1,"method":
// "wallet_sendPreparedCalls","params":[<signed operation>]} on lane
// alchemy-wallet (as prepare), one attempt, 10 s abort. A result: the
// ticket is deleted, then the answer is sent. A JSON-RPC error, a missing
// result, a non-2xx, an unparsable body, a timeout: 500 {"error":"INTERNAL"};
// a busy or open lane: 429 RATE_LIMITED Retry-After; the job pool full: 429
// Retry-After 2. Each failure releases the claim, so the wallet may retry
// the same ticket until it expires (an expired one is not resurrected).
//
// Answer. 200 with Vary: Accept: Alchemy's result passed through
// (sendProtobufOrJson: an object or array as JSON, a string as text/plain,
// null as "null"; protobuf: the Struct, empty for a non-object, unset for
// null). Errors are JSON without Vary; baseline security headers always.
//
// State, concurrency, bounds. A ticket is single use: of simultaneous
// submits exactly one claims it and reaches Alchemy, the others are 404
// until it fails. Tickets are process memory: after a restart — also a
// SIGKILL while a send is at Alchemy, which may still have accepted the
// operation — the ticket is unknown (404), as in TypeScript.
//
// Differences: router_404 bodies; quota Retry-After from the wall clock;
// quota minutes survive a restart; the 8-job 429 (the claim is released);
// 413 past 512 KiB; the canary gate.
void RobinhoodService::Impl::HandleSubmit(const http::HttpRequest& request, http::HttpReply& reply) {
  this->Write(request, reply, Kind::kSubmit);
}

// GET /v1/networks/robinhood/sponsored-trades/capabilities
// (TypeScript: getRobinhoodSponsoredTradeCapabilities
// -> RobinhoodSponsoredTradeGateway.capabilities().)
//
// What it answers. Whether this deployment can run gas-sponsored Robinhood
// Chain trades (EIP-7702 accounts through Alchemy's Wallet APIs and a gas
// policy). The wallet's RobinhoodSponsoredTradeService.canSponsor reads it
// before offering sponsorship: it needs `enabled` and `sponsoredGas` true,
// and treats any failure (404, 401, non-200, decode error) as "not
// sponsored", so it keeps its normal self-paid EOA path.
//
// Request. GET, plus HEAD (the same headers and Content-Length, no body).
// The query string, request body, conditional headers (If-None-Match,
// If-Modified-Since), Content-Type and cookies are ignored. Other methods
// and other spellings of the path are the router's 404.
//
// Policy, in this order (AuthorizeDeviceRequest; TypeScript's /v1
// preHandler). Every refusal is JSON with no
// Vary header, because TypeScript refuses before sendProtobufOrJson runs:
//   1. Any Origin header, even an empty one: 403 BROWSER_ORIGIN_FORBIDDEN.
//   2. The Authorization header must be exactly "Bearer " followed by a
//      fintel_dev_ token of 43 base64url characters; otherwise
//      401 UNAUTHORIZED. This also covers a missing header, lowercase
//      "bearer", Basic auth, a double space and trailing characters.
//   3. An unknown token: 401 UNAUTHORIZED. A revoked device or an expired
//      token: 401 SESSION_EXPIRED.
//   4. With ATTESTATION_MODE=required (production), a device that is not
//      App Attest or Play Integrity attested: 401 ATTESTATION_REQUIRED.
//   5. Quota: cost 1 against the device's wall-clock minute (development
//      240, standard 120, attested 600, premium 1,200). Once it is over:
//      429 RATE_LIMITED, Retry-After: 60. The KV counter fails open.
//      Steps 5 and 6 send x-rate-limit-cost: 1 and
//      x-rate-limit-remaining; steps 1–4 send neither.
//   6. Success: 200 with Vary: Accept.
// Every response carries Cache-Control: no-store,
// X-Content-Type-Options: nosniff, Referrer-Policy: no-referrer and
// Permissions-Policy: camera=(), microphone=(), geolocation=().
//
// Encoding. AcceptsProtobuf (TypeScript acceptsProtobuf): an Accept list
// element whose media type is application/protobuf (any case) with no q
// parameter, or whose first q parameter is a number > 0. That selects
// `application/protobuf`; anything else gets
// `application/json; charset=utf-8`.
//   JSON, in this key order:
//     {"network":"robinhood","chainId":4663,"enabled":B,"gasless":B,
//      "sponsoredGas":B,"batchedCalls":B,"maxCalls":2,
//      "accountStandard":"eip-7702"[,"reason":"ALCHEMY_GAS_POLICY_NOT_CONFIGURED"]}
//     The four B flags are all equal to `enabled`. The `reason` key is
//     present only when sponsorship is disabled.
//   Protobuf (RobinhoodSponsoredCapabilitiesResponse):
//     network=1, chain_id=2, enabled=3, gasless=4, sponsored_gas=5,
//     batched_calls=6, max_calls=7, account_standard=8, and
//     `optional string reason = 9`.
//     The bools are proto3 implicit, so a false flag is not on the wire.
//     `reason` has explicit presence: it is written only when disabled,
//     and Flutter maps it only when hasReason().
//   Sizes: 150 JSON / 34 protobuf bytes enabled, 199 / 61 disabled.
//
// Enabled versus disabled. Sponsorship is enabled exactly when both
// ALCHEMY_API_KEY and ALCHEMY_GAS_POLICY_ID are configured, that is,
// present and non-blank after trimming. Every other combination is
// disabled with the single reason "ALCHEMY_GAS_POLICY_NOT_CONFIGURED":
// no key, no policy, neither, or a blank or whitespace-only value. A key
// alone still enables diagnose (and submit), but not sponsorship. There is
// no other reason string. The state is fixed at startup, so a change needs
// a restart.
//
// Startup configuration (LoadNativeConfig). The route exists only with
// FORTIS_NATIVE_ROBINHOOD_CANARY=1, which requires FORTIS_CA_BUNDLE_PATH.
// When unset, empty or 0, the route is absent (404). Each credential is
// trimmed of space, tab, CR and LF. Startup is refused, before any route
// binds, when a value:
//   - has any other control, space or non-ASCII byte after trimming;
//   - is longer than 255 bytes after trimming;
//   - is longer than 4,096 bytes raw.
//
// Side effects. None for trade state: no ticket, no upstream request, no
// provider lease and no job slot. The bodies are static buffers that this
// HTTP worker reads directly; they are written only before Register, so
// concurrent reads need no lock. The only writes are the device policy's
// own: the quota counter and the throttled last-seen presence.
//
// Privacy. The documents contain only the constants above and one bit
// derived from configuration. The Alchemy key and the gas policy id are
// never in a response or a log line: the key is used only in Alchemy URL
// paths, and the policy id only in wallet_prepareCalls bodies.
//
// Intentional differences from TypeScript:
//   - The canary gate. A TypeScript deployment always serves this route.
//     Natively it is 404 without FORTIS_NATIVE_ROBINHOOD_CANARY=1, and
//     the wallet reads 404 as "not sponsored".
//   - Startup refusals. TypeScript trims every ECMAScript whitespace
//     character, accepts any length and any remaining character, and so
//     would report enabled for, say, a "\f"-padded, 256-byte, non-ASCII
//     or space-containing policy id. Native refuses to start instead.
//     Fail closed: it never serves a different capability answer.
//   - Router-wide behaviour. A percent-encoded path segment is not
//     decoded (404; find-my-way decodes it and answers 200). 404 bodies
//     differ. Native always sends the security headers and
//     Connection: close.
//   - Quota after a restart. Native minute counters live in KV, so they
//     survive a restart within the minute. TypeScript keeps them in
//     process memory.
void RobinhoodService::Impl::Capabilities(const http::HttpRequest& request, http::HttpReply& reply) {
  auth::DeviceStepState state;
  state.attestation_required = config.attestation_required;
  if (!RunHttpSteps(kCapabilitiesSteps, request, reply, state)) return;
  const auth::DevicePolicyResult& authorized = state.device;
  const bool protobuf = AcceptsProtobuf(request);
  http::HttpResponse response{200, "OK", protobuf ? kProtobufType : kJsonType,
                        protobuf ? capabilities_proto : reinterpret_cast<const uint8_t*>(capabilities_json),
                        protobuf ? capabilities_proto_length : capabilities_json_length};
  response.vary_accept = true;
  response.rate_limit_cost = authorized.rate_limit_cost;
  response.rate_limit_remaining = authorized.rate_limit_remaining;
  reply.Send(response);
}

// GET stock-tokens and stock-tokens/{address}.
void RobinhoodService::Impl::Read(const http::HttpRequest& request, http::HttpReply& reply, Kind kind) {
  ReadState state;
  state.attestation_required = config.attestation_required;
  state.param = kind == Kind::kInstrument ? HttpPathParamFind(request, "address") : nullptr;
  if (!RunHttpSteps(kReadSteps, request, reply, state)) return;
  const http::HttpPathParam* param = state.param;
  const Policy policy = DeviceQuotaOf(state.device);
  const char* address = param ? param->value : nullptr;
  if (kind == Kind::kInstrument && (!address || !address_text(address, strlen(address)))) {
    reply.Send(bad_request(policy));
    return;
  }
  // TypeScript issues the provider reads as the request arrives; a job
  // queued behind a request that fails shares that failure.
  const uint64_t assets_issued = this->client.AssetsGeneration();
  const uint64_t quote_issued =
      kind == Kind::kInstrument ? this->client.QuoteGeneration() : rh::StockClient::kNotIssued;
  const uint64_t actions_issued =
      kind == Kind::kInstrument ? this->client.ActionsGeneration() : rh::StockClient::kNotIssued;
  Job* job = Reserve(reply, policy);
  if (!job) return;
  job->assets_issued = assets_issued;
  job->quote_issued = quote_issued;
  job->actions_issued = actions_issued;
  job->kind = kind;
  job->protobuf = AcceptsProtobuf(request);
  job->head = reply.head();
  job->policy = policy;
  job->arena = tj::GrowingArena(kJobArenaBytes);
  if (address) memcpy(job->address, address, 43);
  Dispatch(reply, job);
}

// POST diagnose, prepare and submit.
void RobinhoodService::Impl::Write(const http::HttpRequest& request, http::HttpReply& reply, Kind kind) {
  WriteState state;
  state.attestation_required = config.attestation_required;
  state.arena = tj::GrowingArena(kJobArenaBytes);
  tj::Arena& arena = state.arena;
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> request_arena_blocks(&arena);
  if (!RunHttpSteps(kWriteSteps, request, reply, state)) { tj::FreeArena(arena); return; }
  const Body& body = state.body;
  const auth::DevicePolicyResult& authorized = state.device;
  const Policy policy = DeviceQuotaOf(authorized);
  const RobinhoodWriteKind write = kind == Kind::kDiagnose  ? RobinhoodWriteKind::kDiagnose
                                   : kind == Kind::kPrepare ? RobinhoodWriteKind::kPrepare
                                                            : RobinhoodWriteKind::kSubmit;
  const Value* input = nullptr;
  if (body.kind == BodyKind::kValue) input = body.value;
  else if (body.kind == BodyKind::kProtobuf) input = RobinhoodDecodeWriteRequest(write, body.bytes, body.length, arena);
  bool sponsored = false;
  const bool valid = RobinhoodValidWriteRequest(write, input, &sponsored);
  auto answer = [&](const http::HttpResponse& response) {
    reply.Send(response);
    tj::FreeArena(arena);
  };
  if (!valid) return answer(bad_request(policy));
  if (kind == Kind::kDiagnose && !diagnostics_enabled)
    return answer(json_response(503, "Service Unavailable", "{\"error\":\"TRANSACTION_DIAGNOSTICS_UNAVAILABLE\"}",
                                policy));
  if (kind == Kind::kPrepare && !sponsored_enabled) return answer(sponsored_unavailable(policy));
  rh::TicketStore::Claim claim;
  bool claimed = false;
  if (kind == Kind::kSubmit) {
    // submit() needs only the Alchemy key (prepared tickets need both).
    if (!diagnostics_enabled) return answer(sponsored_unavailable(policy));
    const Value* ticket = tj::Get(input, "ticket");
    const Value* signatures = tj::Get(input, "signatures");
    switch (tickets.Begin(ticket->text, authorized.device.id, Now(), signatures->count, &claim)) {
      case rh::TicketStore::kNotFound:
        return answer(json_response(404, "Not Found", "{\"error\":\"SPONSORED_TRADE_NOT_FOUND\"}", policy));
      case rh::TicketStore::kWrongCount:
        return answer(bad_request(policy));
      case rh::TicketStore::kClaimed:
        claimed = true;
        break;
    }
  }
  Job* job = Reserve(reply, policy);
  if (!job) {
    if (claimed) {
      tickets.Finish(claim, false);
      claim.prepared.reset();
    }
    tj::FreeArena(arena);
    return;
  }
  job->kind = kind;
  job->protobuf = AcceptsProtobuf(request);
  job->head = reply.head();
  job->policy = policy;
  job->arena = *request_arena_blocks.release();
  job->input = input;
  job->sponsored = sponsored;
  memcpy(job->device_id, authorized.device.id, sizeof(job->device_id));
  job->claim = std::move(claim);
  job->claimed = claimed;
  Dispatch(reply, job);
}

// Reserves a job; on saturation answers the typed back-pressure 429.
RobinhoodService::Impl::Job* RobinhoodService::Impl::Reserve(http::HttpReply& reply, const Policy& policy) {
  fortis::base::UniqueLock lock(mutex);
  Job* job = nullptr;
  for (Job& candidate : jobs)
    if (!candidate.used) { job = &candidate; break; }
  if (!job || stopping) {
    lock.Unlock();
    reply.Send(back_pressure(2000, policy));
    return nullptr;
  }
  job->used = true;
  return job;
}

// Transfers the socket to the handoff and queues the job.
void RobinhoodService::Impl::Dispatch(http::HttpReply& reply, Job* job) {
  uint64_t token = 0;
  if (!reply.Reserve(kHandoffTimeoutMs, &token)) {
    reply.Send(back_pressure(2000, job->policy));
    Release(job);
    return;
  }
  job->token = token;
  job->deadline = monotonic_ms() + (seams.provider_deadline_ms ? seams.provider_deadline_ms : kProviderDeadlineMs);
  fortis::base::LockGuard lock(mutex);
  queue[(queue_head + queue_count) % kJobs] = job;
  ++queue_count;
  changed.Broadcast();
}

}  // namespace fortis::exchange
