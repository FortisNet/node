#include "ownership.h"
// GET /v1/networks/{network}/address/{address}/transfers and its query rules.
#include "address_service_internal.h"

namespace hb = fortis::codec::history_batch;

namespace tj = fortis::codec::tj;

namespace fortis::wallet {

namespace address_service_internal {

// GET /v1/networks/{network}/address/{address}/transfers
//
// The durable, server-side transaction history of one address, with real
// delta sync. The wallet (IntelligenceApi.transfers) always asks for
// protobuf, puts the network id and address in the path as typed, and sends
// `since` (its stored server cursor), `before` (a display cursor), `token`,
// `priceId`, `from`/`to` (UTC epoch seconds), `flow`, `refresh=true` and
// `limit` as query parameters. The stored rows reach devices twice: here, and
// as `transfer` events of the account's sync journal (POST /v1/sync).
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the read
// and its writes still happen). Other methods, a trailing '/' and other
// spellings are the router's 404. The router answers 404 for an empty value, a
// decoded control character/DEL and a value over 512 bytes, so every path value
// reaches this handler percent-decoded. In order:
//   1. A raw byte >= 0x80 anywhere in the target: 400 {"error":"BAD_REQUEST"},
//      no quota.
//   2. network or address over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"},
//      no quota.
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN; malformed/unknown bearer 401 UNAUTHORIZED;
//      revoked or expired 401 SESSION_EXPIRED; ATTESTATION_MODE=required and
//      an unattested device 401 ATTESTATION_REQUIRED; then cost 10 against
//      the wall-clock device minute (development 240, standard 120, attested
//      600, premium 1,200): over it 429 RATE_LIMITED, Retry-After 60. Every
//      later answer carries x-rate-limit-cost: 10 and x-rate-limit-remaining.
//   4. zod (400 {"error":"BAD_REQUEST"}, no detail, nothing written):
//      network 1..64 and address 1..128 units, then
//      the query as fast-querystring parses it ('+' is a space; a malformed
//      or non-UTF-8 escape keeps the value as written; a repeated key makes
//      an array, which every field refuses; other keys, `LIMIT` and
//      `limit[]` are ignored; escaped key spellings such as `li%6Dit` count):
//        since    string, at most 40 UTF-16 units (any text: a cursor is
//                 only a position, "abc" or "-1" simply page from there);
//        before   string, at most 600 units (decoded later, see below);
//        refresh  exactly "true" (`tru%65` decodes to it);
//        limit    Number(value) (JS: "" and " " are 0, "0x3", "0b11", "2e0",
//                 " 3 " and "+3" coerce; NaN, Infinity and fractions fail),
//                 an integer 1..500, default 100;
//        token    string, at most 120 units, kept as typed (not trimmed);
//        priceId  String.prototype.trim (NBSP, BOM, U+3000 go; U+200B
//                 stays), full Unicode toLowerCase (U+212A becomes "k"),
//                 then /^[a-z0-9][a-z0-9-]{0,99}$/;
//        from/to  Number(value) as limit, integers 0..253402300799 ("" is 0,
//                 "-0" stays -0 and is echoed as -0);
//        flow     "in" | "out" | "neutral".
//      Refinements: a non-empty `since` with a non-empty `before`; `from`
//      after `to`; `flow` without a `token` key (`token=` counts as present).
//   5. Native only: a decoded NUL in `token` is 400 {"error":"BAD_REQUEST"}
//      here (a NUL in a path value is the router's 404).
// Then on a foreground worker (runFacet's contract: a RangeError is 400
// {"error":"BAD_REQUEST","detail":message}, anything else 502
// {"error":"UPSTREAM_UNAVAILABLE"}):
//   6. The device remembers the topic `address:{network as typed}:{canonical
//      address}` (a 15-minute watch; rewritten at most once per 10 minutes
//      per process), even when the read below fails; an unknown network
//      keeps the address as typed.
//   7. The network: a catalogue id or eip155:{digits} (%3A, leading zeros)
//      of a catalogued chain; else "unknown network: {network}". Transfers
//      are served only where the family's source is configured and covers
//      the network, else "transfers not served for {network as typed}"
//      (testnets, a generic eip155 id, ton; EVM without ETHERSCAN_API_KEY or
//      ALCHEMY_API_KEY, Solana without HELIUS_API_KEY, Bittensor without
//      TAOSTATS_API_KEY).
//   8. The account: EVM addresses fold to lowercase, every other family is
//      kept byte for byte; touch() creates it (read times renewed at most
//      hourly) with no address validation (an invalid address is the
//      provider's to refuse, which fails the ingest, not the read).
//   9. The device's portfolio watch `portfolio:{network id}:{canonical}`
//      tracks [token trimmed] (or none): rewritten when the account or the
//      tracked set changed, else renewed only within 5 minutes of its
//      15-minute expiry.
//  10. The ingest (TransferIngester.ensureFresh), single-flight per account
//      in this process (a joining reader waits for the running one). It
//      does nothing within 60 s of a recorded failure (lastErrorAt), or
//      when the history is complete and lastSyncAt is under 30 s old
//      (refresh=true makes that 0), or when EVM change detection watched the
//      account since lastSyncAt and saw nothing move. Otherwise the source
//      reads one page from its stored provider cursor (EVM: Etherscan v2
//      txlist + tokentx at chainid, 500 rows from startblock/page, the
//      address interpolated as typed and sent in fetch()'s WHATWG wire form;
//      a "free API access" or "unsupported chainid" answer marks the network
//      uncovered for the process and falls over, with independent cursors,
//      to Alchemy alchemy_getAssetTransfers; Etherscan failing falls over
//      too). Rows are upserted in commits of 200 keyed by recordId: a new
//      row or a changed app view mints the next journal sequence as its
//      `cursor` (so a pending->confirmed or re-org patch re-appears to
//      `since` pollers), unchanged rows write nothing; a new row whose
//      timestamp is not a valid Date aborts its commit ("RangeError: Invalid
//      time value"). Success patches the sync record (provider cursors,
//      backfillComplete, lastSyncAt = the ingest's start, errors cleared)
//      and, when rows arrived, drops the account's balance envelopes. A
//      failure (or a coverage gap with no fallback) stores lastError (the
//      failure's text) and lastErrorAt and serves the stored rows. The sync
//      record never expires; journal events expire after 7 days.
//      With a non-empty `since` (a delta poll) the read waits at most 1.5 s:
//      the ingest finishes on the ingest worker and its rows reach the device
//      through the journal; every other read waits for it.
//  11. With `token`: fiat valuation repair of that token's unpriced rows
//      (no provider request; candles already in the price-series store),
//      recording fiatAttemptedAt; retried after 6 h, or 60 s with `priceId`
//      (the hint used when the registry has no CoinGecko id).
//  12. The page. `before` decodes as decodeDisplayCursor: Buffer base64url
//      (both alphabets, other characters skipped, stops at '='), JSON.parse
//      (no BOM; duplicate keys keep the last), then a string `ts` Date.parse
//      accepts (ISO with any offset, date-only, legacy "Oct 26 2026 ...
//      GMT"; a zone-less date-time as UTC, TypeScript's local time in its
//      UTC production) and a non-empty string `id`; otherwise
//      400 "invalid display cursor" (after steps 6-11 ran). An id with a lone
//      surrogate throws URIError building the boundary: 502.
//      A non-empty `since` pages the cursor index ascending from that text
//      (exclusive), applying token/from/to/flow as row filters. Otherwise
//      the time index (the token's own index when `token` is non-empty)
//      newest first from `since` when present (an empty `since` reads
//      nothing), else from the boundary {pad13(ts)}/{encodeURIComponent(id)}
//      (exclusive; ties by encoded recordId descending; pad13 of a negative
//      instant is "00000000-1000", so such rows order by that text), capped
//      at `to`, stopping below `from`. flow with a token: in = toTokenId is
//      the token or a receive/mint/reward/unstake (or incoming unknown) leg
//      of it; out = a send/swap/bridge/burn/stake/fee (or outgoing unknown)
//      leg; neutral = a send/receive selfTransfer leg; flow with `token=`
//      matches direction only. limit + 1 rows are read for hasMore.
//  13. The confirmation tip: when a confirmed row with a block number is on
//      the page, the driver's latest height (EVM eth_blockNumber,
//      Number(BigInt(result)), safe and >= 0) read once; failing soft to 1
//      confirmation.
//
// Response. {network (resolved id), address (canonical), items (the app's
// TransactionHistory JSON per row: confirmations tip - block + 1, else 1 for
// confirmed, 0 otherwise; timestamps by toISOString, six-digit years
// included), operations (one economic operation per row), cursor (with a
// non-empty `since` and more rows: the last row's cursor; else the account's
// high-water cursor or null), hasMore, nextBefore (the last row's display
// cursor while hasMore), historyComplete (the sync record's
// backfillComplete), window {from, to, flow ("all" by default)}, cache {hit:
// true, timestamp: lastSyncAt, else the newest row's updatedAt, else null}}.
// JSON (application/json; charset=utf-8) or, when any Accept entry is
// application/protobuf (any case, q absent or > 0; application/x-protobuf
// does not count), the StructuredReadResponse protobuf with the same
// document; Vary: Accept on 200 only. Errors are JSON whatever the Accept.
//
// Bounds and concurrency. Three foreground workers, 32 queued tasks (a full
// queue or handoff: 429 RATE_LIMITED, Retry-After 2), an 8 MiB task arena
// for the documents plus an 8 MiB page arena for the rows (a 501-row page
// fits), a 4 MiB response buffer; one ingest worker with 16 queued delta
// ingests (a full queue ingests on the reader's worker), 16 MiB ingest
// arenas; at most two provider requests in flight process-wide. Provider
// lanes: Etherscan 3/s (burst 2), Alchemy 100/s (burst 25), each RPC host
// 20/s (burst 5); GET retries (transport failure, 5xx, 408, unparseable
// 2xx) up to three attempts inside one lane slot; five consecutive faults
// open a lane's breaker for 30 s. A provider body over 512 KiB fails the
// ingest. Retention: a daily job deletes terminal rows older than 730 days.
//
// Privacy. Provider keys travel only to their own provider (Etherscan's in
// its query, Alchemy's in its path) and never reach a response, the sync
// record or a log line; no client header, cookie, device id or token
// reaches a provider; the device id only keys its own watches.
//
// Intentional differences (asserted by tests/address_transfers_edges_test.py):
// a malformed path escape is {"error":"BAD_REQUEST"}; the request parser refuses a raw '#' (Node drops a path
// fragment and serves the read; in the query TypeScript's zod refuses the
// value, charged), a raw byte leaving the path invalid UTF-8, and a target
// over 12,500 bytes; 414/431 carry the native security headers; a decoded
// NUL in a path value is the router's 404 with no account, topic or
// provider request (TypeScript ingests for that address); a decoded NUL in
// `token` is 400
// (TypeScript reads an empty token page and tracks the token); for
// transport-class failures (TypeScript UpstreamError, UpstreamAnswerError,
// TypeError, LaneBusyError) the stored lastError is native text. A display
// cursor boundary may exceed the KV key bound; Page compares the full text
// so a stored key equal to its prefix is included on a reverse scan.
}  // namespace address_service_internal

void AddressService::Impl::HandleTransfers(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 10, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], address[520];
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool address_ok = path_param(request, "address", 128, address, sizeof(address));
  auto input = base::MakeUnique<fortis::wallet::address::TransfersQueryInput>();
  if (!input) { dispatch(*this, client, head, nullptr); return; }
  const bool query_ok = fortis::wallet::address::ParseTransfersQuery(request.raw_target ? request.raw_target : "", input.get());
  if (!network_ok || !address_ok || !query_ok) {
    input.reset();
    bad_request(client, head, policy);
    return;
  }
  // A decoded NUL in `token` cannot be carried by the C-string account,
  // watch and index keys: 400 without the facet (no topic, account or
  // provider request).
  if (input->has_token && strlen(input->token) != input->token_length) {
    input.reset();
    bad_request(client, head, policy);
    return;
  }
  AddressTask* task = new_task(AddressTask::kTransfers, request, policy);
  if (!task || !copy_into(task->network, sizeof(task->network), network) ||
      !copy_into(task->subject, sizeof(task->subject), address)) {
    input.reset();
    free_task(task);
    dispatch(*this, client, head, nullptr);
    return;
  }
  static const char* const kFlows[] = {"in", "out", "neutral"};
  fortis::wallet::address::TransferQuery& q = task->query;
  if (input->has_since) {
    memcpy(task->since, input->since, input->since_length + 1);
    q.since = task->since;
    q.since_length = input->since_length;
  }
  if (input->has_before && input->before_length) {
    memcpy(task->before, input->before, input->before_length + 1);
    q.before = task->before;
    q.before_length = input->before_length;
  }
  q.refresh = input->refresh;
  q.limit = input->limit;
  if (input->has_token && input->token_length) {
    memcpy(task->token_id, input->token, input->token_length + 1);
    q.token_id = task->token_id;
  }
  if (input->has_price) {
    memcpy(task->price_id, input->price_id, strlen(input->price_id) + 1);
    q.price_source_id = task->price_id;
  }
  q.has_from = input->has_from;
  q.from_sec = input->from;
  q.has_to = input->has_to;
  q.to_sec = input->to;
  if (input->flow >= 0) {
    snprintf(task->flow, sizeof(task->flow), "%s", kFlows[input->flow]);
    q.flow = task->flow;
  }
  input.reset();
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

}  // namespace address_service_internal

}  // namespace fortis::wallet

namespace fortis::wallet::address {

namespace {

// One present string field: exactly one value, decoded, valid UTF-8, at most
// `max_units` UTF-16 units (z.string().max), copied with its length.
bool copy_string(const QueryValue& q, size_t max_units, char* out, size_t capacity, size_t* length) {
  if (!string_value(q, max_units) || q.length >= capacity) return false;
  memcpy(out, q.value, q.length);
  out[q.length] = '\0';
  *length = q.length;
  return true;
}

}  // namespace

bool ParseTransfersQuery(const char* target, TransfersQueryInput* out) {
  QueryValue since, before, refresh, limit, token, price_id, from, to, flow;
  if (!query_value(target, "since", &since) || !query_value(target, "before", &before) ||
      !query_value(target, "refresh", &refresh) || !query_value(target, "limit", &limit) ||
      !query_value(target, "token", &token) || !query_value(target, "priceId", &price_id) ||
      !query_value(target, "from", &from) || !query_value(target, "to", &to) || !query_value(target, "flow", &flow))
    return false;
  if (since.count && !copy_string(since, 40, out->since, sizeof(out->since), &out->since_length)) return false;
  out->has_since = since.count > 0;
  if (before.count && !copy_string(before, 600, out->before, sizeof(out->before), &out->before_length)) return false;
  out->has_before = before.count > 0;
  if (refresh.count && !(refresh.count == 1 && refresh.value && refresh.length == 4 && !memcmp(refresh.value, "true", 4)))
    return false;
  out->refresh = refresh.count > 0;
  out->limit = 100;
  if (limit.count && !coerce_int(limit, 1, 500, &out->limit)) return false;
  if (token.count && !copy_string(token, 120, out->token, sizeof(out->token), &out->token_length)) return false;
  out->has_token = token.count > 0;
  if (price_id.count) {
    // z.string().trim().toLowerCase().regex(/^[a-z0-9][a-z0-9-]{0,99}$/):
    // String.prototype.trim's whitespace, then the full Unicode lowercase
    // (U+212A KELVIN SIGN becomes "k" and passes; U+0130 becomes "i̇" and
    // fails).
    if (price_id.count != 1 || price_id.too_long || !price_id.value ||
        !hb::ValidUtf8(reinterpret_cast<const uint8_t*>(price_id.value), price_id.length))
      return false;
    size_t b = 0, e = 0;
    fortis::compat::token_text::Trim(price_id.value, price_id.length, &b, &e);
    uint8_t arena_bytes[4096];
    tj::Arena a{arena_bytes, sizeof(arena_bytes), 0, true};
    Value* lowered = e - b < 1000 ? fortis::compat::token_text::ToLowerCase(a, price_id.value + b, e - b) : nullptr;
    bool valid = lowered && lowered->length >= 1 && lowered->length <= 100;
    for (uint32_t i = 0; valid && i < lowered->length; ++i) {
      const char c = lowered->text[i];
      const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      valid = alnum || (i > 0 && c == '-');
    }
    if (!valid) return false;
    memcpy(out->price_id, lowered->text, lowered->length);
    out->price_id[lowered->length] = '\0';
    out->has_price = true;
  }
  if (from.count && !coerce_int(from, 0, 253402300799.0, &out->from)) return false;
  out->has_from = from.count > 0;
  if (to.count && !coerce_int(to, 0, 253402300799.0, &out->to)) return false;
  out->has_to = to.count > 0;
  out->flow = -1;
  if (flow.count) {
    if (flow.count != 1 || !flow.value) return false;
    if (flow.length == 2 && !memcmp(flow.value, "in", 2)) out->flow = 0;
    else if (flow.length == 3 && !memcmp(flow.value, "out", 3)) out->flow = 1;
    else if (flow.length == 7 && !memcmp(flow.value, "neutral", 7)) out->flow = 2;
    else return false;
  }
  // .refine: `since` and `before` both truthy; from > to; flow without token.
  if (out->since_length && out->before_length) return false;
  if (out->has_from && out->has_to && out->from > out->to) return false;
  if (out->flow >= 0 && !out->has_token) return false;
  return true;
}

}  // namespace fortis::wallet::address
