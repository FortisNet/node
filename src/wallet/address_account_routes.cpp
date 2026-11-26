// The per-account reads: GET /v1/networks/{network}/address/{address}/insight,
// /nfts, /token-holdings and /balances.
#include "address_service_internal.h"

namespace fortis::wallet {

namespace address_service_internal {

// GET /v1/networks/{network}/address/{address}/insight
//
// What a wallet needs to know about a pasted recipient before sending:
// whether the account exists on chain, what kind it is and how many
// transactions it has made. The wallet (IntelligenceApi.insight) always asks
// for protobuf and puts the network id and address in the path as typed.
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the read
// still happens). Other methods, a trailing '/', and any other spelling of the
// path are the router's 404. The router answers 404 for an empty value, a
// decoded control character/DEL and a value over 512 bytes, so every path value
// reaches this handler percent-decoded. The query string is ignored entirely
// (even a malformed escape in it). In order:
//   1. A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}, no
//      quota.
//   2. network or address over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"},
//      no quota.
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN; malformed/unknown bearer 401 UNAUTHORIZED;
//      revoked or expired 401 SESSION_EXPIRED; ATTESTATION_MODE=required and
//      an unattested device 401 ATTESTATION_REQUIRED; then cost 1 against the
//      wall-clock device minute (development 240, standard 120, attested 600,
//      premium 1,200): over it 429 RATE_LIMITED, Retry-After 60. Every later
//      answer carries x-rate-limit-cost/-remaining.
//   4. zod: network 1..64 and address 1..128 units; an empty segment (e.g.
//      "/address//insight") is 400 {"error":"BAD_REQUEST"}. Control
//      characters and DEL are kept (re-decoded from the path) and reach the
//      messages below JSON-escaped.
// Then InsightFacet.forAddress on a foreground worker (runFacet's contract:
// a RangeError is 400 {"error":"BAD_REQUEST","detail":message}, anything
// else 502 {"error":"UPSTREAM_UNAVAILABLE"}, a busy lane 429):
//   5. The network: a catalogue id, or eip155:{digits} (also %3A, leading
//      zeros) resolving to the catalogued chain with that id. Anything else
//      is "unknown network: {network}". An eip155 id no catalogue entry has
//      is a generic EVM network with every capability none.
//   6. Insight is served only where the family's driver implements it (EVM
//      and Solana) and the network has RPC endpoints; otherwise "insight not
//      served for {network as typed}" (bitcoin, xrpl, aptos, tron, a generic
//      eip155 id, ...).
//   7. The canonical address: EVM folds with String.prototype.toLowerCase
//      (Unicode-aware: "0X" is valid, "É" becomes "é", "İ" becomes "i̇");
//      every other family keeps it byte for byte. The driver's own check is
//      a RangeError: "not an EVM address: {canonical}" (/^0x[0-9a-fA-F]{40}$/)
//      or "not a Solana address: {address}".
//
// Cache identity and freshness. One SWR entry per ins:{network id}:
// {canonical}, so a checksummed, "0X" or alias spelling shares the entry.
// Fresh 30 s (the coordinator's +-10 % jitter), then served stale for 300 s
// with stale:true while one background revalidation refreshes it at
// background lane priority: a lane with a call in flight sheds it
// (BACKGROUND_BUSY) and the call fails over to the next endpoint at once. A
// failed revalidation backs off 30 s, 5 s when it failed busy (Promise.all:
// whichever call failed first decides). On a one-endpoint EVM network the
// driver's second parallel call is always shed behind its first, so the
// entry is never refreshed in the background and stays stale until it
// expires; two endpoints (xdc) or Helius plus the catalogue RPC (Solana)
// refresh it. Past the stale window a miss reads live, coalesced per key
// (concurrent readers share one fetch). There is no durable floor: a failed
// miss answers its failure and is held 10 s (5 s for a busy lane), replayed
// without asking a provider (a RangeError replays its message). An open
// lane breaker or busy lane is 429 RATE_LIMITED with the lane's Retry-After.
//
// The document. {exists, kind, txCount, txCountFloor, asOf, stale,
// cache:{hit, timestamp}}; cache.hit is false only for a live read (then
// timestamp null), else timestamp = asOf.
//  - EVM: eth_getCode and eth_getTransactionCount at "latest", in parallel.
//    kind "contract" unless the code is exactly "0x" or "0x0" (a null or
//    non-string code counts as contract), else "eoa". txCount =
//    Number(BigInt(nonce)): hex/octal/binary literals or a signed decimal
//    (trimmed; "" is 0), an integral number, a boolean or a one-element
//    array; past 2^53 it rounds, past 2^1024 it is Infinity, which JSON
//    writes as the honest unknown null; -0 is 0. A fractional number is
//    BigInt's RangeError ("The number 1.5 cannot be converted to a BigInt
//    because it is not an integer", 400); null, an object, a malformed
//    string, a missing result or a JSON-RPC error are 502. exists =
//    contract || txCount > 0. txCountFloor false (the nonce counts outgoing
//    transactions exactly).
//  - Solana: getAccountInfo and getSignaturesForAddress (limit 1000), on
//    Helius when keyed, failing over to the catalogue RPC. kind "account";
//    txCount = the signature list's length (0 when not a list) and
//    txCountFloor = txCount >= 1000 (the RPC caps it: a floor, never exact);
//    exists = value !== null || txCount > 0; a null result is 502.
//  "unknown" kind is in the contract but neither driver produces it.
//
// Response. JSON (application/json; charset=utf-8) or, when any Accept
// entry is application/protobuf (any case, q absent or > 0;
// application/x-protobuf does not count), the StructuredReadResponse
// protobuf carrying the same document in its Struct; Vary: Accept on 200
// only. No ETag or validators; errors are JSON whatever the Accept.
//
// Bounds and concurrency. Three foreground workers, 32 queued tasks (a full
// queue or handoff: 429 RATE_LIMITED, Retry-After 2), 8 MiB task arenas, a
// 4 MiB response buffer per worker, one revalidation worker (64 queued), at
// most two provider requests per read. Readers queued behind the three
// workers start after a shared cold fetch finished and answer the entry it
// wrote (cache.hit true, same data).
//
// Side effects and privacy. A read writes only its SWR envelope: no
// address topic or watch for the device, no account row, no balance
// snapshot (unlike balances and transfers). No client header, cookie,
// device id or token reaches a provider or a log line.
//
// Intentional differences (asserted by tests/address_insight_edges_test.py):
// a malformed path escape is {"error":"BAD_REQUEST"}; the request parser refuses a raw '#' (Node drops the
// fragment and serves the read) and a target over 12,500 bytes; a decoded
// NUL in a path value is the router's 404; the SWR
// envelope lives in the durable KV, so within 330 s of being written a read
// after a restart can still be a cache hit where TypeScript's new process
// (an in-memory cache) reads live; TypeScript answers a cached protobuf
// read from its in-process object, so a txCount past 2^1024 travels as
// number_value Infinity there and as null_value here (its own KV copy is
// null too); the breaker's Retry-After counts down on the monotonic clock;
// 414/431 carry the native security headers; Solana reports its failing
// call in call order rather than in the order the calls failed.
}  // namespace address_service_internal

void AddressService::Impl::HandleInsight(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], address[520];
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool address_ok = path_param(request, "address", 128, address, sizeof(address));
  if (!network_ok || !address_ok) {
    // A value over its unit bound is the schema's 400.
    bad_request(client, head, policy);
    return;
  }
  AddressTask* task = new_task(AddressTask::kInsight, request, policy);
  if (task && (!copy_into(task->network, sizeof(task->network), network) ||
               !copy_into(task->subject, sizeof(task->subject), address))) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

// GET /v1/networks/{network}/address/{address}/nfts
//
// One account's NFT gallery from a durable server-side inventory. The wallet
// (IntelligenceApi.nfts) always asks for protobuf, puts the network id and
// address in the path as typed, keeps the item maps and reads `stale` (true
// = the last rescan failed and the stored gallery is shown).
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the read,
// account touch, scan and media registration still happen). Other methods, a
// trailing '/', and any other spelling of the path are the router's 404. The
// router answers 404 for an empty value, a decoded control character/DEL and a
// value over 512 bytes, so every path value reaches this handler
// percent-decoded. The query string is ignored. In order:
//   1. A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}, no
//      quota.
//   2. network or address over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"},
//      no quota.
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN; malformed/unknown bearer 401 UNAUTHORIZED;
//      revoked or expired 401 SESSION_EXPIRED; ATTESTATION_MODE=required and
//      an unattested device 401 ATTESTATION_REQUIRED; then cost 20 against
//      the wall-clock device minute (development 240, standard 120, attested
//      600, premium 1,200): over it 429 RATE_LIMITED, Retry-After 60. Every
//      later answer carries x-rate-limit-cost: 20 and -remaining.
//   4. zod: network 1..64 and address 1..128 units; an empty segment (e.g.
//      "/address//nfts") is 400 {"error":"BAD_REQUEST"}. Control characters
//      and DEL are kept (re-decoded from the path).
// Then NftsFacet.forAccount on a foreground worker (runFacet: a RangeError
// is 400 {"error":"BAD_REQUEST","detail":message}, anything else 502
// {"error":"UPSTREAM_UNAVAILABLE"}):
//   5. The network: a catalogue id, or eip155:{digits} (also %3A, leading
//      zeros) resolving to the catalogued chain; else "unknown network:
//      {network}". An eip155 id no entry has is a generic EVM network
//      without NFTs.
//   6. NFTs are served where the family's driver has an inventory source for
//      this deployment and network: EVM with ALCHEMY_API_KEY on eth-mainnet,
//      base, polygon and bnb; Solana mainnet with HELIUS_API_KEY. Otherwise
//      "nfts not served for {network as typed}" (arbitrum, devnet, bitcoin,
//      tron, a generic eip155 id, any network without the key, ...).
//   7. The canonical address (EVM: String.prototype.toLowerCase, so "0X..."
//      and checksummed spellings share one account; other families byte for
//      byte) and AccountStore.touch(network id, canonical): account/pair,
//      account/id and account/recent (read time renewed at most hourly).
//      This happens before the address is validated: an invalid address
//      gets an account, a failed scan and a 200.
//
// The scan (ensureScanned). One flight per account: concurrent reads join
// the leader's scan and then read what it stored. The scan record
// nft/scan/{enc(account)} {lastScanAt, lastFullScanAt, lastError,
// lastErrorAt} decides: a failure less than 60 s ago means no scan;
// otherwise no record, or a last successful scan more than 10 minutes ago
// (exactly 10 minutes is still fresh), means scan. The driver reads one
// provider page, never following a cursor:
//  - EVM: GET https://{eth-mainnet|base-mainnet|polygon-mainnet|bnb-
//    mainnet}.g.alchemy.com/nft/v3/{key}/getNFTsForOwner?owner={address}
//    &withMetadata=true&pageSize=100 (alchemy lane; a 5xx, a timeout or a
//    body that is not JSON is retried after 250 ms and 1 s). The address
//    must match /^0x[0-9a-fA-F]{40}$/ ("not an EVM address: {address}").
//    Per ownedNfts entry (absent: none; a string: none; another
//    non-array: failure): skipped without a contract address or a string
//    tokenId; id "{network id}:{lowercased contract}:{tokenId}", standard
//    = tokenType lowercased (null when absent), name / collection name
//    sanitizeName'd when truthy (non-strings fail the scan), image =
//    image.cachedUrl ?? image.originalUrl ?? null (any JSON value), flags
//    {spam: true} when contract.isSpam is truthy.
//  - Solana: POST https://mainnet.helius-rpc.com/?api-key={key}
//    getAssetsByOwner {ownerAddress, page: 1, limit: 200} (helius lane, no
//    retry). A base58 32-byte address ("not a Solana address: {address}").
//    Per item (a string list: none; null result or other non-array:
//    failure): skipped without a string id or with interface FungibleToken
//    / FungibleAsset; id "solana:{id}", contractAddress null, standard
//    "compressed" or "metaplex", name and the first collection grouping
//    sanitizeName'd, image a string or null, flags {compressed: true}.
// Reconcile on success: every item is written to nft/item/{enc(account)}/
// {enc(id)} in batches of 200 {accountId, nftId, contractAddress, tokenId,
// standard, name, collectionName, sourceUrls:{image}, flags, firstSeenAt,
// goneAt: null}, firstSeenAt kept from a live previous record (so a sold
// and returned item keeps it until its record expired); a duplicate id is
// written twice, the later entry wins. Then every live record the scan did
// not return gets goneAt = now. A batch with a lone-surrogate id
// (encodeURIComponent: "URIError: URI malformed") or an item key over the
// native KV's 1,024-byte key bound is not written and fails the scan;
// batches before it stay. Records are stored as written. Then the scan record:
// success {now, now, null, null}; failure keeps the last success times and
// stores "{class}: {message}" with lastErrorAt now. A provider failure
// leaves the stored gallery intact. A reconciliation failure can leave
// earlier completed 200-item batches committed, just as in TypeScript.
//
// Durability. Item and scan records expire 7 days after their last write
// (a gone record 7 days after it was marked; a failed scan rewrites the scan
// record but not the items, so items can expire under a live scan record).
// Everything is in the durable KV: a restart keeps the gallery, the scan
// times and the cooldown; only the in-process flight is lost.
//
// The document. {network (resolved id), address (canonical), items, stale,
// cache:{hit: true, timestamp: lastScanAt or null}}; items are the live
// records with goneAt null in key order (the byte order of the encoded id,
// e.g. "%" < "1" < "10" < "9" < "Z" < "a"), each {id, contractAddress,
// tokenId, standard, name, collectionName, media:{image, imageProxy},
// flags, firstSeenAt (ISO)}; stale = the scan record holds an error. Each
// string image that parses as an http(s) URL is registered with the media
// facet (media/item/{sha256(url)[:32]} {status: "pending", ...}, 7 days,
// an existing record is never overwritten) and answered as imageProxy
// "/v1/media/{key}"; the device fetches images only through that proxy,
// which re-validates and fetches on first use. No image host is contacted
// by a read. JSON (application/json; charset=utf-8) or, when any Accept
// entry is application/protobuf (any case, q absent or > 0), the
// StructuredReadResponse protobuf with the same document; Vary: Accept on
// 200 only; no validators; errors are JSON whatever the Accept.
//
// Bounds. Three foreground workers, 32 queued tasks (full: 429
// RATE_LIMITED, Retry-After 2), an 8 MiB task arena and a 4 MiB response
// buffer per worker; one provider request per scan (three with retries);
// pages of 200 keys read through an 8 MiB scratch arena, so stored gone
// records cost no response memory; records over 1 MiB fail the scan.
// Side effects and privacy: the account, scan, item and media records only
// (no topic, watch or balance snapshot). No client header, cookie, device id
// or token reaches a provider or a log line; the provider keys reach only
// the provider and never a response or a stored error.
//
// Intentional differences (asserted by tests/address_nfts_edges_test.py):
// a malformed path escape is {"error":"BAD_REQUEST"}; the request parser refuses a raw '#' (Node drops the
// fragment and serves the read) and a target over 12,500 bytes; a decoded
// NUL in a path value is the router's 404 without an account (TypeScript
// touches an account for the NUL address and answers a failed scan);
// lastError
// holds the native failure text (only its presence is observable); 414/431
// carry the native security headers.
}  // namespace address_service_internal

void AddressService::Impl::HandleNfts(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 20, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], address[520];
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool address_ok = path_param(request, "address", 128, address, sizeof(address));
  if (!network_ok || !address_ok) {
    // A value over its unit bound is the schema's 400.
    bad_request(client, head, policy);
    return;
  }
  AddressTask* task = new_task(AddressTask::kNfts, request, policy);
  if (task && (!copy_into(task->network, sizeof(task->network), network) ||
               !copy_into(task->subject, sizeof(task->subject), address))) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

// GET /v1/networks/{network}/address/{address}/token-holdings
//
// The TRC-10 and TRC-20 tokens one Tron address holds, read from TronScan's
// keyed wallet listing on the server so the provider key never reaches the
// app. The wallet (IntelligenceApi.tronWalletTokens, the recipient/account
// card) always asks for protobuf, sends network "tron" and the address
// through Uri.encodeComponent, and parses `data` with the same
// parseTronWalletTokens it uses for a direct TronScan answer.
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the read
// still happens). Other methods, a trailing '/', '/token-holding' or any other
// spelling are the router's 404; the static "token-holdings" segment sits
// beside the balances/insight/nfts/transfers siblings and shares no pattern
// with them. The router answers 404 for an empty value, a decoded control
// character/DEL and a value over 512 bytes, so every path value reaches this
// handler percent-decoded. The query string is ignored entirely (even a
// malformed escape in it). In order:
//   1. A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}, no
//      quota.
//   2. network or address over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"},
//      no quota.
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN; malformed/unknown bearer 401 UNAUTHORIZED;
//      revoked or expired 401 SESSION_EXPIRED; ATTESTATION_MODE=required and
//      an unattested device 401 ATTESTATION_REQUIRED; then cost 5 against the
//      wall-clock device minute (development 240, standard 120, attested 600,
//      premium 1,200): over it 429 RATE_LIMITED, Retry-After 60. Every later
//      answer, including the 400s and 503 below, is charged and carries
//      x-rate-limit-cost: 5 and x-rate-limit-remaining.
//   4. zod: network 1..64 and address 1..128 units; an empty segment (e.g.
//      "/address//token-holdings") or a 65..100-unit network is 400
//      {"error":"BAD_REQUEST"} without a detail.
//   5. The gate: network must be exactly "tron" (no alias, case or testnet:
//      "TRON", "eip155:728126428", "tron-nile" are refused; "tr%6Fn" decodes
//      to "tron" and passes) and the address must match
//      /^T[1-9A-HJ-NP-Za-km-z]{33}$/ (Base58 alphabet only, no checksum; a
//      hex 41.../0x41... form, a space or a non-ASCII character fails).
//      Otherwise 400 {"error":"BAD_REQUEST",
//      "detail":"valid Tron address required"}.
//   6. TRONSCAN_API_KEY unset or blank: 503 {"error":"UPSTREAM_UNAVAILABLE"}
//      (JSON whatever the Accept, no Vary), without any provider request.
// Then TronTokenHoldingsFacet on a foreground worker (runFacet's contract:
// anything that fails is 502 {"error":"UPSTREAM_UNAVAILABLE"}, a refused
// TronScan lane 429 RATE_LIMITED with Retry-After; this route never answers
// a RangeError 400).
//
// Provider request. GET https://apilist.tronscanapi.com/api/account/wallet
// ?address={encodeURIComponent(address)} with `accept: application/json`
// and the key in the TRON-PRO-API-KEY header only (never in the URL, never
// a query parameter). The key is required: a 401/403/429 is not retried
// anonymously. The shared HTTP client retries a transport failure, a 5xx,
// a 408 or an unparseable 2xx body up to three attempts (250 ms, 1 s; a
// Retry-After up to 5 s), all inside one 'tronscan' lane slot; a 4xx or 429
// fails at once. The lane is TronScan's single shared budget (20 per minute,
// burst 20, three in flight, also used by Tron history and TRC-10 prices).
// Five consecutive provider faults (no answer, 5xx, 408, 401, 403) or one
// 429 open its breaker for 30 s (a 429's Retry-After if longer); a 404 or a
// malformed body is an answer and resets the count.
//
// Projection. The body's `data` must be an array (a null body, a missing,
// null, object or string `data`, or a non-object body: 502). Each row that
// is an object and whose String(token_type) is exactly "10" or "20" (a
// number 10/20 written in any JSON spelling, the strings "10"/"20", or an
// array whose join gives them, e.g. [20], ["10"], [[20]]) becomes
// {token_type, token_id, token_abbr, token_name, token_url, balance,
// token_decimal, token_value_in_usd} in that order, each value copied as
// TronScan sent it (any JSON type); a member the row lacks is omitted, a
// null one kept. Everything else is dropped: TRX (0), NFTs (721/1155),
// "20 ", "020", -20, 10.5, null/boolean/object types, non-object rows and
// every other member (vip, token_price, ...). Provider order is kept. A
// duplicate member keeps its last value; numbers are written as
// JSON.stringify writes them (1e+21, 12345678901234567000, -0 as 0,
// Infinity as null); lone surrogates are kept and escaped. The response is
// {"data": [...]} (an empty array for a wallet with none) with no freshness
// fields.
//
// Cache identity and freshness. One SWR entry per tronhold:{address} (the
// decoded address verbatim: Base58 is case-significant, so escaped spellings
// share the entry and no other spelling exists). Fresh 20 s (the
// coordinator's +-10 % jitter), then served stale for 600 s while one
// background revalidation refreshes it at background lane priority: a
// TronScan lane with a call in flight, a queue, no token or a non-closed
// breaker sheds it. A failed revalidation backs off 30 s (5 s when shed) and
// keeps serving the stale entry. Past the stale window a miss reads live,
// coalesced per key (concurrent readers share one provider call). There is
// no durable floor: a failed miss answers its failure (502, or 429 for a
// refused lane) and is held 10 s (5 s for a refused lane), replayed without
// asking TronScan. Only successful projections are stored; a failure never
// replaces a cached entry.
//
// Response. JSON (application/json; charset=utf-8) or, when any Accept
// entry is application/protobuf (any case, q absent or > 0;
// application/x-protobuf does not count), the StructuredReadResponse
// protobuf carrying the same document in its Struct; Vary: Accept on 200
// only. No ETag or validators; errors are JSON whatever the Accept.
//
// Bounds and concurrency. Three foreground workers, 32 queued tasks (a full
// queue or handoff: 429 RATE_LIMITED, Retry-After 2), 8 MiB task arenas, a
// 4 MiB response buffer per worker, one revalidation worker (64 queued), one
// provider request per read (three with the HTTP retries), at most two
// provider requests in flight process-wide (the execution adapter's HTTPS
// workers). A TronScan answer over 512 KiB is a failed read (502 / held /
// stale kept), and a stored projection over the 1 MiB SWR scratch is served
// but not cached.
//
// Side effects and privacy. A read writes only its SWR envelope: no address
// topic or watch for the device, no account row, no balance snapshot. The
// TronScan key, client headers, cookies, device id and token never reach a
// response, a provider URL or a log line; the provider sees only the
// address.
//
// Intentional differences (asserted by
// tests/tron_token_holdings_edges_test.py): a malformed path escape is
// {"error":"BAD_REQUEST"}; the request parser
// refuses a raw '#' (Node drops the fragment and serves the read) and a
// target over 12,500 bytes; 414/431 carry the native security headers; the
// SWR envelope lives in the durable KV, so within 620 s of being written a
// read after a restart can still be served from it where TypeScript's new
// process (an in-memory cache) reads live; TypeScript answers a cached
// protobuf read from its in-process object for 20 s, so a -0 or an Infinity
// TronScan sent travels as that double there and as the stored JSON's 0 /
// null_value here (its own KV copy says the same); the breaker's Retry-After
// counts down on the monotonic clock (a held lane refusal reports the
// remaining cooldown, TypeScript the one it first saw); a TronScan body over
// 512 KiB fails here and is served there. Readers queued behind the three
// workers start after a shared cold fetch finished and answer the entry it
// wrote (same bytes).
}  // namespace address_service_internal

void AddressService::Impl::HandleHoldings(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 5, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], address[520];
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool address_ok = path_param(request, "address", 128, address, sizeof(address));
  if (!network_ok || !address_ok) {
    bad_request(client, head, policy);  // the schema's bounds
    return;
  }
  if (strcmp(network, "tron") != 0 || !fortis::wallet::address::ValidTronAddress(address)) {
    bad_request(client, head, policy, "valid Tron address required");
    return;
  }
  if (!this->config.tronscan_api_key_configured) {
    send_text(client, 503, "Service Unavailable", kUpstreamUnavailable, head, policy.rate_limit_cost,
              policy.rate_limit_remaining);
    return;
  }
  AddressTask* task = new_task(AddressTask::kHoldings, request, policy);
  if (task && (!copy_into(task->network, sizeof(task->network), network) ||
               !copy_into(task->subject, sizeof(task->subject), address))) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

// GET /v1/networks/{network}/address/{address}/balances?tokens=a,b,...
//
// One account's native balance plus the token balances the caller tracks,
// each priced in USD, for any chain family. The wallet
// (IntelligenceApi.balances) always asks for protobuf, puts the network id
// and address in the path as typed, and sends `tokens` (its tracked token
// ids joined with ',') only when it tracks any.
//
// Request. GET, plus HEAD (same status, headers and Content-Length; the read,
// topic and store writes still happen). The router 404s an empty value, a
// decoded control character/DEL and one over 512 bytes; every path value
// reaches this handler percent-decoded, in this order:
//   1. A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"} before
//      anything else, no quota.
//   2. network or address over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"}
//      (reserved escapes such as %2F kept), no quota.
//   3. The /v1 device policy (AuthorizeDeviceRequest): any Origin 403
//      BROWSER_ORIGIN_FORBIDDEN; malformed/unknown bearer 401 UNAUTHORIZED;
//      revoked or expired 401 SESSION_EXPIRED; ATTESTATION_MODE=required and
//      an unattested device 401 ATTESTATION_REQUIRED; then cost 1 against the
//      wall-clock device minute (development 240, standard 120, attested 600,
//      premium 1,200): over it 429 RATE_LIMITED, Retry-After 60. Every later
//      answer carries x-rate-limit-cost/-remaining.
//   4. zod: network 1..64 and address 1..128 units (an empty segment, e.g.
//      "/address//balances", is the router's 404); `tokens`, when
//      present, exactly one string of at most 8,000 UTF-16 units after
//      fast-querystring decoding ('+' is a space; a malformed or non-UTF-8
//      escape keeps the value as written; a second `tokens` makes an array:
//      400). Other keys, `TOKENS` and `tokens[]` are ignored.
//   5. tokens.split(',').map(trim).filter(t => t !== '') (SplitTrackedTokens;
//      String.prototype.trim's whitespace set, so NBSP, BOM and U+3000 go,
//      U+200B stays). More than 200 left: 400 {"error":"BAD_REQUEST",
//      "detail":"\u2264200 tokens"}. Duplicates are kept.
// Then the facet runs on a foreground worker (runFacet's error contract):
//   6. The topic `address:{network as typed}:{canonical address}` is
//      remembered for the device (15-minute watch, renewed at most once per
//      10 minutes), even when the read below fails (an unknown network
//      keeps the address as typed).
//   7. BalancesFacet.forAccount: an unknown network is 400
//      {"error":"BAD_REQUEST","detail":"unknown network: {network}"};
//      known EIP-155 aliases resolve to their catalogued network; one
//      whose balances capability is none, "balances not served for
//      {network}"; a family without a driver, "no driver for {family}". EVM
//      addresses fold to lowercase (so "0X..." is valid); every other family
//      keeps the address byte for byte. The driver's own RangeError is a 400
//      with its message (e.g. "not an EVM address: {canonical}").
//
// Cache identity and freshness. One SWR entry per bal:{network id}:
// {canonical}:{sha1(tokens sorted by UTF-16 code units, joined with ',')
// [:12]}, so a permutation or padding of one tracked set shares the entry,
// while a duplicate, a checksummed id or a different subset does not. Fresh
// 20 s (60 s for the UTXO families: bitcoin, bitcoin-cash, dash, dogecoin,
// litecoin, zcash; no jitter beyond the coordinator's +-10 %), then served
// stale for 600 s while one background revalidation refreshes it (a failed
// one backs off 30 s, 5 s when its lane shed it). Past that, a miss reads
// live with in-process coalescing (concurrent readers share one fetch). A
// failed miss is held 10 s (5 s for a busy lane): within the hold no
// provider is asked again.
//
// The durable floor. Every successful read (live or background) touches the
// account (account/pair, account/id, account/recent; read times renewed at
// most hourly) and merges the payload into balance/account/{id}: previous
// tokens by id, requested-but-omitted ids removed (a failed balanceOf is
// omitted, never zero), incoming entries replacing in place. When a miss
// fails, the floor answers instead: the snapshot's native, its tokens (all
// of them for a native-only request, else only those whose stored tokenId
// equals a requested id exactly), reference and finality, with stale true,
// cache {hit:true} and asOf = the snapshot's write. No floor: the failure,
// 502 {"error":"UPSTREAM_UNAVAILABLE"} or, when the provider lane's breaker
// is open or the lane shed the call, 429 RATE_LIMITED with Retry-After from
// the lane (30 s for an open breaker).
//
// The price join. native -> the network's CoinGecko id; erc20:{address} ->
// "{network}:erc20:{address}"; spl:/asa: ids -> themselves; other token ids
// are unpriced. One spot lookup (SWR 30 s fresh, 600 s stale, unknown ids
// negative for 300 s) through the price plane; a stale price is served and
// refreshed at foreground lane priority. price is the quote or null; fiat =
// (displayUnits ?? Number(balanceRaw) / 10^decimals) x price, null when
// unpriced or not finite. A price provider outage leaves prices null, never
// fails the read. Without the price canary every price is null.
//
// Response. {network (resolved id), address (canonical), native, tokens[],
// reference?, finality?, asOf, stale, cache:{hit, timestamp}}, entries
// {tokenId, balanceRaw, decimals, symbol, price, fiat, source,
// displayUnits?}. cache.hit is false only for a live read (then timestamp
// null), else timestamp = asOf. JSON (application/json; charset=utf-8) or,
// when any Accept entry is application/protobuf (any case, q absent or
// > 0; application/x-protobuf does not count), the StructuredReadResponse
// protobuf with the same document in its Struct; Vary: Accept on 200 only.
// No ETag, no validators; errors are JSON whatever the Accept.
//
// Bounds and concurrency. Three foreground workers, 32 queued tasks (a full
// queue or handoff: 429 RATE_LIMITED, Retry-After 2), 8 MiB task arenas, a
// 4 MiB response buffer per worker, one revalidation worker (64 queued).
// Readers beyond the three workers that queue behind a shared cold fetch
// answer the entry it wrote (cache.hit true, same data). Over 99 price ids
// the join is split into price-plane requests of at most 100 ids.
// Privacy: no client header, cookie, device id or token reaches a provider
// or a log line; the device id only keys its own watches.
//
// Intentional differences (asserted by tests/address_balances_edges_test.py):
// the router answers a malformed path escape with {"error":"BAD_REQUEST"}
//; the request parser refuses a raw '#' and a
// target over 12,500 bytes (400) where Node serves up to its 16 KiB head;
// a decoded NUL in a path value is the router's 404 without a topic; the SWR entry lives in the durable KV, so a read after a
// restart can be a cache hit where TypeScript's new process reads live; the
// 414 and 431 answers carry the native security headers.
}  // namespace address_service_internal

void AddressService::Impl::HandleBalances(int client, const http::HttpRequest& request, bool head) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, this->attestation_required);
  if (reply_policy(client, policy, head)) return;
  char network[520], address[520];
  QueryValue tokens;
  const bool network_ok = path_param(request, "network", 64, network, sizeof(network));
  const bool address_ok = path_param(request, "address", 128, address, sizeof(address));
  if (!network_ok || !address_ok ||
      !query_value(request.raw_target ? request.raw_target : "", "tokens", &tokens) ||
      (tokens.count && !string_value(tokens, 8000))) {
    bad_request(client, head, policy);
    return;
  }
  AddressTask* task = new_task(AddressTask::kBalances, request, policy);
  if (!task) { dispatch(*this, client, head, nullptr); return; }
  if (tokens.count && !fortis::wallet::address::SplitTrackedTokens(tokens.value, tokens.length, task->tokens,
                                                           &task->token_count)) {
    free_task(task);
    dispatch(*this, client, head, nullptr);
    return;
  }
  if (task->token_count > kMaxTokens) {
    free_task(task);
    bad_request(client, head, policy, "\xE2\x89\xA4" "200 tokens");
    return;
  }
  if (!copy_into(task->network, sizeof(task->network), network) ||
      !copy_into(task->subject, sizeof(task->subject), address)) {
    free_task(task);
    task = nullptr;
  }
  dispatch(*this, client, head, task);
}

namespace address_service_internal {

}  // namespace address_service_internal

}  // namespace fortis::wallet
