// The network-scoped reads: GET /v1/networks/{network}/tx/{hash},
// /tx/{hash}/view and /overview.
#include "address_service_internal.h"

namespace fortis::wallet {

namespace address_service_internal {

// GET /v1/networks/{network}/tx/{hash}
//
// The normalized status of one transaction the wallet sent or is watching
// (the TypeScript getTransaction over TxDetailFacet and the
// family driver's txStatus). The wallet polls it after a send and keeps the
// `tx:` topic so /v1/sync can push the settlement.
//
// Request. GET, plus HEAD (same status, headers and Content-Length, no body;
// the topic, read and record writes still happen). Other methods, a trailing
// '/', '/TX/', '/tx//', an extra segment or '/VIEW' are the router's 404 (the
// lowercase '/view' is R27's route). The router answers 404 for an empty value,
// a decoded control character/DEL and a value over 512 bytes, so both values
// reach this handler percent-decoded. The query string is ignored entirely
// (even a malformed escape). In order:
//   1. A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}, no quota.
//   2. network or hash over 100 UTF-16 units once decoded (find-my-way's
//      maxParamLength; an escape counts once, an astral character twice):
//      414 {"error":"URI_TOO_LONG"}, no quota, no auth (so 101..128 units never reach zod's 128).
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN (also for HEAD and an unknown network);
//      missing/malformed/unknown bearer (a lowercase "bearer" scheme too)
//      401 UNAUTHORIZED; revoked or expired 401 SESSION_EXPIRED;
//      ATTESTATION_MODE=required and an unattested device 401
//      ATTESTATION_REQUIRED; then cost 1 against the wall-clock device
//      minute (development 240, standard 120, attested 600, premium 1,200):
//      over it 429 RATE_LIMITED, Retry-After 60, even for an empty hash or an
//      unknown network. Every later answer carries x-rate-limit-cost and
//      x-rate-limit-remaining.
//   4. zod: network 1..64 and hash 1..128 UTF-16 units; an empty segment
//      ("//tx/…", "…/tx/") is 400 {"error":"BAD_REQUEST"}. Control
//      characters, DEL, quotes, '/', '?', '#' and '%' (escaped) are kept and
//      reach the provider, the topic and the record key as decoded.
// Then on a foreground worker (runFacet: a RangeError is 400
// {"error":"BAD_REQUEST","detail":message}, a busy lane or open breaker 429
// RATE_LIMITED "Upstream data is temporarily busy; retained data remains
// available." with the lane's Retry-After, anything else 502
// {"error":"UPSTREAM_UNAVAILABLE"}):
//   5. rememberTxTopic first, for every request that passed zod, whatever
//      follows (an unknown network, an unsupported family, a failed read):
//      the device's watch on "tx:{network as typed}:{hash}" (decoded, not
//      canonicalized: "eip155:1" and "eth-mainnet" are two topics), expiring
//      15 min after the write. A (device, topic) renewed less than 10 min
//      ago is not rewritten (the renewal memo); a new one notifies the sync
//      topic listeners. A failed watch write is ignored (TypeScript logs
//      it) and never fails the read.
//   6. The network: a catalogue id, or eip155:{digits} (also %3A, leading
//      zeros) resolving to the catalogued chain; anything else is "unknown
//      network: {network}" (case-sensitive: "ETH-MAINNET" is unknown).
//   7. Served only where the family's driver implements txStatus (EVM and
//      Solana) and the network has RPC endpoints: every other family
//      (bitcoin and the UTXO family, cardano, xrpl, stellar, algorand,
//      hedera, tron, cosmos, aptos, sui, near, icp, vechain, bittensor) and
//      an eip155 id no catalogue entry has (eip155:0, eip155:999999, Solana's
//      eip155:101) is "tx status not served for {network as typed}".
// The hash is used as typed: never case-folded or format-checked ("0xAB…"
// and "0xab…" are different transactions; a short or prefixless hash simply
// is unknown to the chain).
//
// Cache. The durable record tx/detail/{enc(network id)}/{enc(hash)} (enc =
// encodeURIComponent) answers a settled read (confirmed or failed) while
// its fetchedAt > now - 120 s: {cache:{hit:true, timestamp:fetchedAt}}, no
// provider request. From 120,000 ms on the chain is asked again, so a reorg
// can move it back to pending. A pending or unknown record is never answered
// from the store: every such read asks the chain. Otherwise:
//  - EVM: eth_getTransactionByHash [hash] on the network's RPC endpoints in
//    order (a transport failure, 5xx or busy lane fails over; a JSON-RPC
//    error, a missing result or a non-JSON body is the call's failure). null
//    is "unknown". A transaction whose blockNumber is null or absent (also a
//    non-object answer: a string, number, false or array) is "pending" with
//    blockNumber null, detail {} and raw = the transaction. Otherwise
//    eth_getTransactionReceipt [hash] and eth_blockNumber [] in parallel
//    (Promise.all: the call that failed first decides), then blockNumber =
//    Number(BigInt(String(tx.blockNumber))) — hex/octal/binary literals,
//    trimmed signed decimals, "" and [] as 0, a one-element array as its
//    element; "1e+21", "240.5", "1,2", "true", "[object Object]", "0x" are
//    SyntaxErrors (502) — and tip = Number(BigInt(tip)), where a fractional
//    number is BigInt's RangeError (400 "The number 1.5 cannot be converted
//    to a BigInt because it is not an integer"), true is 1, null and junk
//    are 502. Past 2^53 both round, past 2^1024 they are Infinity (JSON
//    null). confirmations = tip >= block ? tip - block + 1 : 0;
//    requiredConfirmations = the catalogue's (cronos 1) or 3. status:
//    "failed" when receipt.status is exactly "0x0" (at any depth),
//    "confirmed" when a receipt is present (any non-null answer, even 'x' or
//    false) and confirmations >= required, else "pending". detail
//    {confirmations, requiredConfirmations, gasUsed, effectiveGasPrice} (the
//    two strings or null), raw {transaction, receipt}.
//  - Solana: getSignatureStatuses [[hash], {searchTransactionHistory: true}]
//    on Helius when keyed (mainnet), then the catalogue RPC (devnet: its
//    catalogue RPC only); helius and rpc:{host} lanes. A null result is 502;
//    value?.[0] (a list's first element, an object's "0" member, a string's
//    first UTF-16 unit) null or absent is "unknown". Else "failed" when err !=
//    null (false and 0 count), "confirmed" when confirmationStatus is
//    exactly "finalized", else "pending"; blockNumber = slot when a number;
//    detail {confirmationStatus or null}; raw = the status entry.
// A successful read writes, in one KV batch under the store lock, the
// record {network id, hash, status, blockNumber, confirmedAt: null, detail,
// raw, fetchedAt, pollUntil} expiring 7 days after the read, deletes the
// previous record's tx/pending index entry, and for pending/unknown sets
// pollUntil = now + 15 min with tx/pending/{pollUntil, 13 digits}/
// {enc(network id)}/{enc(hash)} = {network, hash} expiring at pollUntil. A
// failed read writes nothing (the topic was already remembered). Records are
// stored as their JSON text and a cached read answers them as stored (-0 is
// 0 and Infinity null in JSON); the live read answers the provider's values
// as parsed.
//
// The pending poller (TxDetailFacet.pollPendingOnce, every 5 s, the first
// 5 s after start, one loop for all devices): up to 50 live tx/pending
// entries from tx/pending/{now, 13 digits} in key order, each read with its
// driver (a failure, pending or unknown is skipped and retried next tick).
// A settled status rewrites the stored record ({...record, status,
// blockNumber, confirmedAt, detail, raw, fetchedAt: now, pollUntil: null},
// 7 days from the tick) and deletes the entry; a record that already
// expired is not recreated (its entry expires with its window). The tick's
// result is the number of transitions.
//
// The document. {status, blockNumber, detail, raw, fetchedAt (ISO),
// cache:{hit, timestamp}}: a live read is {hit:false, timestamp:null}, a
// stored one {hit:true, timestamp:fetchedAt}. JSON (application/json;
// charset=utf-8) or, when any Accept entry is application/protobuf (any
// case, q absent or > 0; application/x-protobuf and q=0 do not count), the
// StructuredReadResponse protobuf with the same document; Vary: Accept on
// 200 only; no ETag or validators. Errors are JSON whatever the Accept.
//
// Restart. Records, pending entries and watches are all in the durable KV: a
// new process answers a settled record within its 120 s window, rereads a
// pending one and resumes the open poll windows; only the renewal memo is
// lost (the first read rewrites the watch, as TypeScript's new process does).
//
// Concurrency and bounds. No coalescing (nor in TypeScript): concurrent cold
// reads of one hash each ask the chain and each write the record, the last
// batch winning; reads and the poller write under the same store lock.
// Three foreground workers, 32 queued tasks (a full queue or handoff: 429
// RATE_LIMITED, Retry-After 2), a 30 s handoff, 8 MiB task arenas, a 4 MiB
// response buffer per worker; three JSON-RPC calls per EVM read (one per
// Solana read), each tried on the network's endpoints in turn (xdc: up to
// six requests), each body at most 512 KiB; a record serialized to
// at most 1 MiB; the longest key the route can write (a 100-unit hash of
// three-byte characters) is under 1,000 bytes, inside the native KV's
// 1,024-byte bound. Five consecutive failures open a lane's breaker for
// 30 s (429 until it closes).
//
// Privacy. No client header, cookie, device id or token reaches a provider
// or a log line; the watch is keyed by the device id and holds only the
// network and hash.
//
// Intentional differences (asserted by name in
// tests/transaction_edges_test.py): a malformed path escape is
// {"error":"BAD_REQUEST"}; the request parser
// refuses a raw '#' (Node drops the fragment and serves the read), a raw
// byte that leaves the decoded path invalid UTF-8 and a target over 12,500
// bytes; the router's 404 body is {"error":"NOT_FOUND"}, and a decoded NUL or other control character in a path value is
// that 404 (TypeScript reads it);
// a cached protobuf read of a record holding Infinity carries null_value
// where TypeScript's msgpack record keeps number_value Infinity (both JSON
// answers say null); the breaker's Retry-After counts down on the monotonic
// clock; a record over 1 MiB is not stored (the read is still answered,
// and asks the chain again next time), where lmdb stores it; the renewal
// memo is 512 sets of 8 (device, topic) ways instead of an LRU of 50,000
// pairs, so a set overflow costs one extra watch rewrite.
}  // namespace address_service_internal

void AddressService::Impl::HandleTx(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], hash[520];
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool hash_ok = path_param(request, "hash", 128, hash, sizeof(hash));
  if (!network_ok || !hash_ok) {
    bad_request(client, head, policy);  // the schema's 1..64 / 1..128 units
    return;
  }
  AddressTask* task = new_task(AddressTask::kTx, request, policy);
  if (task && (!copy_into(task->network, sizeof(task->network), network) ||
               !copy_into(task->subject, sizeof(task->subject), hash))) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

// GET /v1/networks/{network}/tx/{hash}/view
//
// The app's OWN per-family transaction-detail JSON, passed through rather
// than normalized (the TypeScript getTransactionView over
// TxDetailViewCache and the family's
// transaction view; the chain_*_view / chain_{xrpl,stellar,algorand,
// hedera}.cpp TransactionView natively). The wallet (IntelligenceApi.txDetailView)
// asks for protobuf, sends the app network id and hash unencoded, parses
// `detail` with that family's own fromJson, and treats 404 and any 400 as
// "no server answer": it takes its direct-chain path.
//
// Request. GET, plus HEAD (same status, headers and Content-Length, no body;
// the topic and the read still happen). Other methods, a trailing '/',
// '/VIEW', '/views', '/TX/', an extra segment or '/tx//view/' are the router's
// 404, and so are an empty value, a decoded control character/DEL and a value
// over 512 bytes. Both values reach this handler percent-decoded. The query
// string is ignored entirely (even a malformed escape). In order:
//   1. A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}, no quota.
//   2. network or hash over 100 UTF-16 units once decoded (find-my-way's
//      maxParamLength; an escape counts once, an astral character twice):
//      414 {"error":"URI_TOO_LONG"}, no quota, no auth (so 101..128 units never reach zod's 128).
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN (also for HEAD and an unknown network or
//      unsupported family); missing/malformed/unknown bearer (a lowercase
//      "bearer" scheme too) 401 UNAUTHORIZED; revoked or expired 401
//      SESSION_EXPIRED; ATTESTATION_MODE=required and an unattested device
//      401 ATTESTATION_REQUIRED; then cost 1 against the wall-clock device
//      minute (development 240, standard 120, attested 600, premium 1,200):
//      over it 429 RATE_LIMITED, Retry-After 60, even for an unknown
//      network or an unsupported family. Every later answer carries
//      x-rate-limit-cost and x-rate-limit-remaining.
//   4. zod: network 1..64 and hash 1..128 UTF-16 units (an empty segment,
//      "//tx/…" or "…/tx//view", is the router's 404). Quotes, '/', '?',
//      '#', '%' and non-ASCII (escaped) are kept and reach the provider, the
//      topic and the envelope key as decoded.
//   5. registry.network(network as typed): a catalogue id, or eip155:{digits}
//      (also %3A, leading zeros) resolving to the catalogued chain or, for
//      an uncatalogued chain id (eip155:0, eip155:101, eip155:999999), a
//      generic EVM network without RPC endpoints. Anything else (case-
//      sensitive, "eip155:1a") is 400 {"error":"BAD_REQUEST",
//      "detail":"unknown network"} -- no "{network}" echo, unlike /tx/{hash}.
//   6. The family must have a view: evm, xrpl, algorand, solana, stellar,
//      cardano, hedera, bitcoin, dogecoin, litecoin, zcash. Every other
//      family (bitcoinCash, tron, cosmos, aptos, sui, near, icp, vechain,
//      bittensor) is 400 {"error":"UNSUPPORTED","detail":"no detail view
//      for {family}"} (the registry's family name, e.g. "bitcoinCash"): a
//      distinct code because it is permanent. Steps 5 and 6 answer before
//      runFacet, so neither remembers a topic.
// Then on a foreground worker (runFacet: a RangeError is 400 {"error":
// "BAD_REQUEST","detail":message}, a busy lane or open breaker 429
// RATE_LIMITED "Upstream data is temporarily busy; retained data remains
// available." with the lane's Retry-After, anything else 502 {"error":
// "UPSTREAM_UNAVAILABLE"}):
//   7. rememberTxTopic first, for every request that passed step 6, whatever
//      follows (404, 400, 502): the device's watch on "tx:{network as typed}:
//      {hash}" (not canonicalized: "eip155:1" and "eth-mainnet" are two
//      topics), expiring 15 min after the write. A (device, topic) renewed
//      less than 10 min ago is not rewritten (the renewal memo); a new one
//      notifies the sync topic listeners. A failed watch write is ignored
//      and never fails the read. This route does NOT enrol the pending
//      poller (no tx/detail record, no tx/pending entry): the app watches
//      /tx/{hash} for that.
//   8. TxDetailViewCache.get(network id, hash): SwrCache.read of
//      txd:{network id}:{hash} (both unencoded; every spelling of a network
//      shares one key) with TTL fresh 1 h (the coordinator's +-10 % jitter
//      at store time), stale 24 h more, KV expiry fresh + stale, and the
//      view's isFinal as shouldStore:
//       - a fresh envelope answers without a provider request;
//       - a stale one answers too and starts one background revalidation
//         (background lane priority, one per key): a final result is
//         stored, a non-final one (a reorg back to the mempool) is not and
//         leaves the stale envelope (the next stale read revalidates
//         again); a failure backs the key off 30 s (5 s for a busy lane);
//       - past the stale window (or absent): a live read, one flight per
//         key -- concurrent misses share the leader's answer, including a
//         pending view that is not stored and its 404/error;
//       - a live failure is held per key for 10 s (5 s for a busy lane):
//         reads inside the hold replay it without a provider request -- the
//         404 of an unknown hash, a 502, and a RangeError with its message.
//      isFinal: evm detail.blockNumber != null; the UTXO views
//      detail.detail.confirmed === true and Number(confirmations ?? 0) >=
//      requiredConfirmations (catalogue, else 1; zcash 10); xrpl, algorand,
//      solana, stellar, cardano, hedera always (they answer only settled
//      transactions). A view that answers null (the chain does not know
//      the hash yet, or it is not final where the view refuses unsettled
//      ones) is never stored: 404 {"error":"NOT_FOUND"}.
// The views (each provider call tried on the network's endpoints in turn;
// enrichments fail soft, dropping only their own field):
//  - evm: eth_getTransactionByHash [hash] (null: 404); when blockNumber is
//    set, eth_getTransactionReceipt and eth_getBlockByNumber [blockNumber,
//    false] (the block fails soft: no timestamp); the 4byte name of the
//    calldata selector through MethodSignatureResolver (its own envelope
//    4byte:{selector lowercased}, fresh 1 day, stale 30 days) and Blockscout
//    txlistinternal (chains 1, 11155111, 8453, 84532, 42161, 137, 4663, 14;
//    `txhash=` takes the hash unencoded, serialized as fetch() does: cut at
//    '#', space/quotes/<>/DEL/non-ASCII percent-encoded) for any found
//    transaction, pending included. EvmTxDetails.toJson(): blockNumber,
//    timestamp (ISO; a block time past Date's range is RangeError "Invalid
//    time value"), txType, nonce, positionInBlock (hexToInt: safe integers
//    only), valueWei, gasLimit, gasUsed, effectiveGasPriceWei, feeWei
//    (BigInt decimals), success (receipt.status !== "0x0", absent without a
//    string status), inputData, toAddress, inputMethod, internal, rawTx,
//    rawReceipt; null fields omitted. No RPC endpoint (a generic eip155 id):
//    RangeError "no endpoints configured".
//  - solana: getTransaction [hash, {maxSupportedTransactionVersion: 0,
//    encoding: "jsonParsed"}] (null or a non-number slot: 404): slot,
//    feeLamports, successful (no meta, or meta.err null), computeUnits,
//    blockTimeSecs, accounts (accountKeys zipped with pre/post balances).
//  - xrpl: tx [{transaction: hash}]: 404 unless validated with a number
//    ledger_index and Sequence and a string TransactionType; trustSet facts.
//  - stellar: Horizon /transactions/{enc(hash)} (any failure: 404; ledger
//    must be > 0): fee, sequence, successful, a trimmed non-"none" memo.
//  - algorand: indexer (-api. -> -idx.) /v2/transactions/{enc(hash)} (a
//    failure: 502; no confirmed-round: 404), printable note only.
//  - cardano: Koios POST /tx_info {"_tx_hashes":[hash]} (retried; empty: 404).
//  - hedera: mirror /api/v1/transactions/{enc(hash)} (any failure: 404) and
//    a fail-soft /api/v1/blocks?timestamp=gte:{enc(consensus)} for
//    blockNumber; transaction_hash base64 -> hashHex.
//  - bitcoin (Esplora): /tx/{enc(hash)} (failure: 404), then /blocks/tip/
//    height (failure: depth 1), /v1/block/{hash} (pool) and /block/{hash}
//    for a confirmed one, /v1/fees/mempool-blocks (inclusion projection)
//    for a pending one. dogecoin, litecoin, zcash (Blockbook): /tx, '/'
//    (bestHeight, else backend.blocks), /block/{hash}; no pool, no
//    projection; zcash reports feeIsReliable false for shielded spends.
// A GET to a 5xx is retried after 250 ms and 1 s (a POST only for Koios).
//
// The document. {network: the network id, family, detail, cache: {hit,
// timestamp}}: a live read is {hit: false, timestamp: null} (a follower of
// a shared flight too), an envelope {hit: true, timestamp: its asOf (ISO)},
// fresh or stale. JSON (application/json; charset=utf-8) or, when any
// Accept entry is application/protobuf (any case, q absent or > 0;
// application/x-protobuf and q=0 do not count), the StructuredReadResponse
// protobuf with the same document; Vary: Accept on 200 only; no ETag or
// validators. Errors (400, 404, 429, 502) are JSON whatever the Accept.
//
// Restart. Watches and the SWR envelopes are in the durable KV: a new
// process answers a stored view within its windows. The renewal memo, the
// miss holds, the revalidation backoffs and the flights are per process.
//
// Concurrency and bounds. One flight per envelope key in a 64-slot table
// (followers past a full table or after a 30 s wait proceed alone); a
// non-stored leader answer is kept for its followers in a 16-entry ring.
// Three foreground workers, 32 queued tasks (a full queue or handoff: 429
// RATE_LIMITED, Retry-After 2), a 30 s handoff, 8 MiB task arenas, a 4 MiB
// response buffer per worker; each provider body at most 512 KiB; an
// envelope serialized to at most 1 MiB (a larger view is answered but not
// stored); the longest key (a 100-unit hash of three-byte characters) is
// under 500 bytes. Five consecutive failures open a lane's breaker for 30 s.
//
// Privacy. No client header, cookie, device id or token reaches a provider
// or a log line; the watch is keyed by the device id and holds only the
// network and hash.
//
// Intentional differences (asserted by name in
// tests/transaction_view_edges_test.py): a malformed path escape is
// {"error":"BAD_REQUEST"}; the request parser
// refuses a raw '#' (Node drops the fragment, serves the read and
// remembers its topic) and a raw byte that leaves the decoded path invalid
// UTF-8; the router's 404 body is {"error":"NOT_FOUND"}, also for a decoded
// NUL or other control character in a path value (TypeScript reads it, 404,
// and remembers the topic); the envelopes are
// durable, so a restarted process answers a stored view from its envelope
// where TypeScript's new process (an in-memory Kv) reads live; a cached
// protobuf inside TypeScript's 30 s SwrCache L1 window carries the live
// object (Infinity, -0) where native answers the stored JSON (null, 0) --
// the JSON answers are equal; a provider body over 512 KiB fails the read
// (502) where TypeScript serves and stores it; the breaker's Retry-After
// counts down on the monotonic clock; the renewal memo is 512 sets of 8
// (device, topic) ways instead of an LRU of 50,000 pairs.
}  // namespace address_service_internal

void AddressService::Impl::HandleView(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], hash[520];
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool hash_ok = path_param(request, "hash", 128, hash, sizeof(hash));
  if (!network_ok || !hash_ok) {
    bad_request(client, head, policy);  // the schema's 1..64 / 1..128 units
    return;
  }
  // The registry and family checks answer before runFacet: no topic.
  fortis::chains::Network resolved;
  if (!this->catalog.Resolve(network, strlen(network), &resolved)) {
    bad_request(client, head, policy, "unknown network");
    return;
  }
  if (!fortis::chains::ViewFor(resolved.family)) {
    char detail[128];
    snprintf(detail, sizeof(detail), "no detail view for %s", fortis::chains::FamilyName(resolved.family));
    send_error(client, 400, "Bad Request", "UNSUPPORTED", detail, head, policy.rate_limit_cost,
               policy.rate_limit_remaining);
    return;
  }
  AddressTask* task = new_task(AddressTask::kView, request, policy);
  if (task && (!copy_into(task->network, sizeof(task->network), network) ||
               !copy_into(task->subject, sizeof(task->subject), hash))) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

// GET /v1/networks/{network}/overview
//
// The network page's header, news card and popular-token card in one read
// (the TypeScript getNetworkOverview over NetworkOverviewFacet). The wallet
// (IntelligenceApi.networkOverview) asks for protobuf, sends the app network
// id through Uri.encodeComponent and keeps its last model on any failure;
// GET /v1/networks advertises `news` / `popularTokens` per network from the
// same OverviewSupportsNews / OverviewSupportsPopularTokens rules.
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the read
// still happens). Other methods, a trailing '/', '/OVERVIEW', '/overviews' or
// an extra segment are the router's 404. The router answers 404 for an empty
// value, a decoded control character/DEL and a value over 512 bytes, so
// {network} reaches this handler percent-decoded. The query string is ignored
// entirely (even a malformed escape). In order:
//   1. A raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, no quota.
//   2. {network} over 100 UTF-16 units once decoded: 414
//      {"error":"URI_TOO_LONG"}, no quota.
//   3. The /v1 device policy: any Origin 403 BROWSER_ORIGIN_FORBIDDEN;
//      missing/malformed/unknown bearer 401 UNAUTHORIZED; revoked or expired
//      401 SESSION_EXPIRED; ATTESTATION_MODE=required and an unattested
//      device 401 ATTESTATION_REQUIRED; then cost 1 against the wall-clock
//      device minute (development 240, standard 120, attested 600, premium
//      1,200): over it 429 RATE_LIMITED, Retry-After 60. Refusals carry no
//      Vary. Every later answer is charged and carries x-rate-limit-cost: 1
//      and x-rate-limit-remaining.
//   4. zod: 1..64 UTF-16 units (65..100 units: 400 {"error":"BAD_REQUEST"},
//      no detail).
//   5. The facet's RangeErrors, 400 {"error":"BAD_REQUEST","detail":…}: a
//      network the registry does not resolve (catalog id, alias, eip155:N
//      with leading zeros; case, whitespace and "eip155:1.0" are not ids) is
//      "unknown network: {id as decoded}" (JSON-escaped); a testnet, a
//      generic eip155:N and a mainnet without a profile are "network
//      overview is unavailable: {id}".
// Errors are JSON whatever the Accept, with cache-control no-store.
//
// Document. {network:{id (canonical), name, description "{name} network",
// websiteUrl|null}, news:{available true, items, asOf, stale},
// popularTokens:{available, items, asOf, stale, metric "on_chain_holders",
// source "coingecko"|null}} in that order. 33 profiled mainnets
// (the TypeScript network overview profiles) serve it.
//  - news: NewsFacet.feed({topics chains/protocol/security, limit 100}) — the
//    newest 100 unexpired stories with one of those topics (7-day story
//    retention), cached as `news:chains+protocol+security|100` for 60 s and
//    served stale up to 3,600 s while it is recomputed — filtered by
//    isNetworkNews (a chain topic and an alias as an ASCII-case-insensitive
//    phrase between non-alphanumerics; non-ASCII characters are boundaries)
//    and cut to 4. asOf is the last ingest (null before any), stale when
//    there is none or it is older than 45 minutes. A failed feed read is
//    502 {"error":"UPSTREAM_UNAVAILABLE"}.
//  - popularTokens: for the 17 networks with a GeckoTerminal slug and a
//    popular family (EVM, Solana, Tron, Cardano, Aptos; Algorand has none),
//    otherwise {available false, items [], asOf null, stale false, source
//    null} with no provider request. Rows come from GET
//    https://api.geckoterminal.com/api/v2/networks/{slug}/trending_pools
//    ?include=base_token&duration=24h&page=1 (accept:
//    application/json;version=20230203, keyless, lane `geckoterminal`: 10
//    per minute, burst 2, two in flight; GET retried on transport/5xx/408/
//    unparseable 2xx up to three attempts, 10 s each; a 429 opens the
//    breaker 30 s). Projection: per pool (rank = pool index + 1) the last
//    included `token` resource with the trimmed base_token id; trimmed
//    address, sanitized name (64) and upper-cased sanitized symbol (16)
//    non-empty, integer decimals 0..255 (Number() coercion), market cap > 0,
//    first occurrence of an address; price >= 0 and h24 finite or null; an
//    https image URL in WHATWG form or null; the lower-cased coin id or null.
//    Then the family address grammar and token id (erc20:/spl:/trc20:/asa:/
//    cardano:/aptos:), the first five distinct token ids, each logo
//    registered as media (media/item/{key}, pending, 7 days, never
//    overwritten) and a holder count: Ethplorer (eth-mainnet, keyless
//    `freekey` or ETHPLORER_API_KEY in its URL), Etherscan V2
//    tokenholdercount (ETHERSCAN_API_KEY, chains 1/10/50/56/137/999/1329/
//    8453/42161/43114; "free api access is not supported" is null and stops
//    that chain's later overviews for the process lifetime — every token of
//    one overview is decided before the first answer), TronScan
//    token_trc20 (TRONSCAN_API_KEY) and Koios asset_summary, each its own
//    SWR entry `tf:{network}:{tokenId}:holders` (28 h fresh, 7 d stale). A
//    failed holder read is null for that row and is not cached (a JSON null
//    body, a NOTOK, an HTTP failure); an answer without a count is cached
//    as null. The rows are one SWR entry `netoverview:v3:{id}`: 300 s fresh
//    (the coordinator's ±10 % jitter), then served stale up to 3,600 s more
//    while one background revalidation refreshes it (a failed one backs off
//    30 s, 5 s when the lane shed it); past that a live miss, coalesced per
//    key. A failed miss (no floor) answers {available true, items [], asOf
//    null, stale true, source "coingecko"} with the news card intact and is
//    held 10 s (5 s for a refused lane) without asking GeckoTerminal. asOf
//    is the entry's write time, stale whether it was served stale.
//
// Response. JSON (application/json; charset=utf-8, no Vary) or, when an
// Accept entry is application/protobuf (q absent or > 0), the
// NetworkOverviewResponse message (networks.proto) with Vary: Accept. Both
// carry cache-control `private, max-age=60` and a weak validator
// W/"network-overview-{sha256(JSON)[0..24]}" or
// W/"network-overview-protobuf-{sha256(bytes)[0..24]}"; an If-None-Match
// equal to it (OWS trimmed; a list, '*' or a strong tag never matches) is
// 304 with the same headers and no body. A holderCount that
// protobuf-es refuses (a fraction or Infinity for BigInt(), a negative or
// >= 2^64 uint64) makes the protobuf answer 500 {"error":"INTERNAL"} with
// that cache-control and no Vary; its JSON is served.
//
// Bounds and concurrency. Three foreground workers, 32 queued tasks (full:
// 429 RATE_LIMITED, Retry-After 2), 8 MiB task arenas, a 4 MiB response
// buffer per worker, protobuf sub-messages sized from their payload (no fixed
// per-field cap), one revalidation worker (64 queued), at most two provider
// requests in flight process-wide. A provider body over 512 KiB is a failed
// read. The news reader is serialized by its own mutex over a 4 MiB arena
// and a 1 MiB feed buffer; a feed document over 1 MiB fails the read (502).
//
// Side effects and privacy. A read writes only SWR envelopes and media
// registrations: no address topic, watch, account or snapshot. Provider keys
// reach only their own provider (Etherscan and Ethplorer in the URL as
// TypeScript sends them, TronScan as TRON-PRO-API-KEY) and never a response,
// stored value or log line; no client header, cookie, device id or token
// reaches a provider.
//
// Intentional differences (asserted by tests/network_overview_edges_test.py):
// a malformed path escape is {"error":"BAD_REQUEST"}; the request parser refuses a raw '#' (Node drops the
// fragment and serves) and a raw byte leaving the path invalid UTF-8; 414/431
// carry the native security headers; the shared parser refuses a repeated
// If-None-Match line (Node joins them and serves 200) and the shared writer
// sends no Content-Length on a HEAD 304 (Node: 0); the SWR envelopes live in
// the durable KV, so within their window a read after a restart is served
// from them where TypeScript's new process (in-memory Kv) reads live;
// TypeScript answers a cached read from its in-process object for up to
// 30 s, so a holder count of Infinity (a JSON 1e400) still refuses protobuf
// and a -0 price is written as -0.0 there, while native has the stored
// JSON's null and 0 (what TypeScript answers after 30 s); a GeckoTerminal body over 512 KiB fails here
// (outage card, held 10 s) and is served there; an image_url over the
// WHATWG parser's 8,192-byte input bound has no logo here; a parse retry of
// an unparseable 2xx takes a lane token per attempt here (GeckoTerminal's
// burst of two refuses the third, TypeScript retries inside one slot); the
// single revalidation worker refreshes two stale holder entries of one read
// one after the other, where TypeScript's lane sheds the second until the
// next read; the Etherscan memo is this service's own (TypeScript shares it
// with the token page).
}  // namespace address_service_internal

void AddressService::Impl::HandleOverview(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520];
  if (!path_param(request, "network", 64, network, sizeof(network))) {
    bad_request(client, head, policy);
    return;
  }
  AddressTask* task = new_task(AddressTask::kOverview, request, policy);
  if (task && !copy_into(task->network, sizeof(task->network), network)) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

}  // namespace address_service_internal

}  // namespace fortis::wallet
