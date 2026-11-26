# Compatibility decisions

This document lists the compatibility decisions of the node: which behaviours
are its contract with clients, which behaviours of the earlier implementation
it deliberately does not reproduce, which of them it still reproduces, and
which rules are security decisions that must never be weakened. Anyone who
changes request parsing, response encoding or anything under `src/compat/`
needs it.

The node was ported from an earlier TypeScript service (Node.js, Fastify,
zod, protobuf-es). It first reproduced that service byte for byte.
Compatibility with it is no longer required: **this server is the contract.**
A source comment that says "as TypeScript does" describes the current
behaviour of this server. It is not a requirement to match anything.

Codec-level detail (JSON readers, protobuf writer and readers) is in
[codec-semantics.md](codec-semantics.md). Its "JS parity only" column gives
the row-by-row detail for the codec entries below.

## Entries and source tags

Each decision has an entry name, which is also an anchor in this document.
Source and test comments refer to an entry as `compat-decisions.md#<entry>`.
Where the code still implements a behaviour only for parity with the earlier
implementation, the comment carries a greppable tag:

```
// parity-only, see compat-decisions.md#<entry>
```

`grep -rn 'compat-decisions.md#' src tests` lists the sites. The tag marks
the defining site of a mechanism, not every caller.

An entry is one behaviour, not one file. A `src/compat/` component whose
parts fall in different classes appears under each.

| Class | Meaning | Rule |
|---|---|---|
| Contract | A client depends on it, or it is this server's own design | Keep. Change it only as an API change |
| Replaced | A behaviour of the earlier implementation that nothing depended on | The native rule in the table applies. Do not reintroduce the old behaviour |
| Parity-only | A behaviour of the earlier implementation that the code still reproduces, although nothing is known to depend on it | May be replaced by the stated native rule, under the stated conditions |
| Security | Decides what the server may fetch, store or reveal | Keep, or replace with something at least as strict. Never weaken |

## What the clients depend on

These facts justify the contract entries. They describe the wallet
application (a Flutter app that uses Dio and the Dart `protobuf` package) and
the operator tools.

### The wallet application

| Area | Dependency |
|---|---|
| Request bodies | Requests carry `accept: application/protobuf`. A body, when present, is `content-type: application/protobuf`. The one exception is the authenticated diagnostic `POST /v1/prices/missing-identifiers`, which sends bounded JSON (`symbols`: 1 to 16 public ticker strings). No form or text body is ever sent |
| Empty bodies | Conversation retry (`POST …/messages/{id}/retry`) sends `content-type: application/protobuf` with an **empty** body. `POST /v1/devices/revoke` from the pending-revocation queue has no accept, no content type and no body |
| Error bodies | Only the JSON member `error` is read, only on a 401, only when the body is at most 1,024 bytes, and it is only compared with `UNAUTHORIZED`, `SESSION_EXPIRED` and `ATTESTATION_REQUIRED`. No `message`, `detail`, `issues`, `code` or `statusCode` is parsed or shown |
| Status codes | Branches on 200, 304, 401, 404, 429 and >= 500. A 400 or 404 on `tx/{hash}/view` means "no view". **A 400 on `POST …/robinhood/transactions/diagnose` means "rejected by the trade policy" and blocks signing**, so that route must not answer 400 to a request the application builds correctly. A 401 on `devices/rotate` starts a new enrollment. Anything else is one generic failure. There is no branch on 409, 413, 414 or 415 |
| Response headers | `etag` (opaque, echoed in `if-none-match`), `retry-after` as integer delta seconds (on attestation 503s and on every 429), `x-next-cursor`, `x-ai-reply-for`, `x-ai-reply-status`, `content-type` and `content-length` (media size cap) |
| JSON responses | `GET /v1/prices/snapshot` (`accept: application/json`), `GET /healthz`, `GET /statusz` |
| Snapshot rows | `cacheAsOfMs` and `freshUntilMs` must decode as a Dart `int`: an integer literal. `1.7e12` or `123.0` drops the row. `price` is any finite number > 0. `stale` is exactly `false`. A duplicate `id` voids the document |
| `/statusz` | `ok`, `version`, `node` (a value that starts with `native-cpp` is labelled "Runtime"), `uptimeSec`, `stats.window10m.clients/requests` |
| `google.protobuf.Struct` | Payloads go through `toProto3Json()`: every number becomes a double, **a `Value` with no kind set throws**, map order is not relied on, absent and null are treated alike, and `-0` is not special |
| Protobuf strings | Decoded with `allowMalformed: true`, so U+FFFD is acceptable and nothing depends on how a lone surrogate was encoded |
| Server order shown as received | Token search rows (appended after local results), network overview `popularTokens`, the first cross-chain route |
| Server order irrelevant | The Robinhood stock catalog (re-sorted by code unit), news (by `publishedAt`), markets (keyed map) |
| DashPay | The response `name` must equal the lower-case ASCII name the application sent, byte for byte |
| EVM addresses | Lower-case in sync rows |
| Dates sent | `DateTime.toUtc().toIso8601String()`: ISO 8601 with `Z` and three or six fractional digits (crash report `occurredAt`, sync `after`, AI `timestamp`). The AI conversation `timestamp` is embedded verbatim in the signed proof message |
| Dates received | Parsed with `DateTime.parse` / `tryParse`. A device `expiresAt` that does not parse fails enrollment and rotation. A sync change without a parseable `changedAt` is dropped |
| Query strings | Built by Dio with `Uri.encodeQueryComponent`: **a space is sent as `+`**, everything else outside `A-Za-z0-9-_.~*` as UTF-8 `%XX` (a comma is `%2C`, a colon `%3A`). Every value is a scalar, so no key is repeated. Numbers are decimal digits. The one boolean is the literal `refresh=true` |
| Path values | Network ids, token ids and refs, news ids and Robinhood contract addresses go through `Uri.encodeComponent`, so a `/` inside an id arrives as `%2F` and a `:` as `%3A`, and must decode **inside one segment**. Addresses, transaction hashes and conversation ids are sent raw |
| Field presence | Tested with `has…()`: token search `decimals` and `holderCount`, `change24hPct`, the `history` / `failure` oneof of a history batch entry, and the Struct `payload` of a structured read (absent means the response is refused as malformed). A zero that is present and a missing field are different answers |
| Search text | `q` is user text of any script. Ids that the application lower-cases itself are ASCII |

### Operator tools

| Dependency | Consequence |
|---|---|
| An administrative tool reads the token page as **JSON** (`accept: application/json`) with a device bearer. It needs a 2xx, `identity.tokenId`, `identity.network`, and `asOf` as a string that it compares **lexicographically** | `asOf` must stay a fixed-width UTC ISO 8601 string. The tool builds its query with `URLSearchParams` (a comma is `%2C`) and its path values with `encodeURIComponent` |
| Deployment checks probe a protected route without a bearer and require **401**. A POST probe may send `Content-Type: application/json` and the body `{}` | A protected POST with a JSON body and no bearer must answer 401 ([auth-first](#auth-first)) |
| The help-desk bridge (`/internal/support/v1/…`) is not served by this server | `tests/helpdesk_removed_test.py` |

### Consequences

1. The JSON **response** representation stays: snapshot, health, status, the
   token page and the operator pages.
2. JSON **request** bodies have few clients (the missing-identifiers
   diagnostic, deployment probes, the test suite and `curl`), but
   `application/json` stays an accepted request type.
3. The request **URL** grammar is contract: `+` and `%XX` in a query value,
   `%2F` inside a path value ([query-encoding](#query-encoding),
   [path-encoding](#path-encoding)). The parsers may change; the grammar may
   not.
4. The node terminates HTTPS itself. The code must not assume that anything
   in front of it normalises a query string, joins a repeated header or
   decodes a path. Every rule below applies to the client's own bytes.

## Inventory: `src/compat`

About 16,900 lines, of which about 12,500 are generated tables.

| Component | Lines | What it is | Callers (modules) | Entries |
|---|---:|---|---|---|
| `whatwg_url.{h,cpp}` | 2,050 | A WHATWG URL parser (the behaviour of `new URL()` in Node.js 22, Ada 2.9.2), plus `CanonicalArticleUrl` | news (feed and media fetch, clustering), discovery (`IsHttpUrl`), exchange (provider links, MAYA roots), tokens, wallet | [whatwg-url](#whatwg-url) |
| `idna.{h,cpp}` + `idna_tables.cpp` | 652 + 8,738 | `to_ascii`: UTS #46 mapping, NFC, punycode, bidi | `whatwg_url.cpp` only | [idna](#idna) |
| `js_regex.{h,cpp}` | 864 | Bounded backtracking matcher for a fixed ECMAScript subset over UTF-16 | news (`news_text.cpp`), ai (`ai_reply.cpp` secret-request patterns) | [js-regex](#js-regex) |
| `js_date_parse.{h,cpp}` | 595 | `new Date(string)` as V8 12.4 parses it: the ISO form, then the legacy free-form parser | prices (provider `last_updated`), news (feed dates), ai (proof timestamp), and through `provider_text::DateParse` exchange, tokens, wallet, chains | [date-parse](#date-parse) |
| `js_case.{h,cpp}` + `js_case_tables.cpp` | 240 + 3,715 | ASCII case mapping for identifiers, and Unicode `toUpperCase` / `toLowerCase` with full special casing for text | prices, providers, discovery, news, ai, tokens | [js-case](#js-case) |

`idna` has exactly one consumer (`whatwg_url.cpp`). `js_date_parse` is reached
directly and through `provider_text::DateParse` (`src/codec`) and
`cross_chain_support::DateParse`.

These modules once lived in `src/compat` and now sit with the component that
owns what they do. The entries below still describe their rules.

| Component | Lines | What it is | Callers (modules) | Entries |
|---|---:|---|---|---|
| `src/security/diagnostic_redaction.{h,cpp}` + `src/security/redaction_tables.{h,cpp}` | 861 + 8,376 | Secret scanner, NFKC folding, BIP-39 runs, address and path scrubbing | operations (crash reports), ai (stored and model-bound text) | [redaction](#redaction) |
| `src/codec/json_document_writer.{h,cpp}` | 487 | `JSON.stringify` and `google.protobuf.Struct` output over a bounded token document | discovery, exchange, news, prices, wallet | [js-property-order](#js-property-order), [number-format](#number-format) |
| `src/codec/provider_text.{h,cpp}` | 390 | String helpers over valid UTF-8: UTF-16 limits, safe slice and trim, sanitizers, `toFixed`, case, `Number()`, ISO dates | chains, exchange, tokens, wallet, ai | [lone-surrogates](#lone-surrogates), [number-format](#number-format) |
| `src/codec/text_sanitize.{h,cpp}` | 186 | `sanitizeSymbol` / `sanitizeName` for token search | discovery | [text-sanitizers](#text-sanitizers) |
| `src/ai/hourly_windows.h` | 66 | Fixed one-hour quota windows in a bounded in-memory table | ai | [hourly-windows](#hourly-windows) |
| `src/operations/sandbox_socket.{h,cpp}` | 213 | Client of the local sandbox broker socket | see [sandbox-control.md](sandbox-control.md) | - |

`redaction_tables` has exactly one consumer (`diagnostic_redaction.cpp`).

## Contract

Keep these. "Tests" names the files that assert the behaviour (`tests/`
prefix omitted).

| Entry | Behaviour | Why | Constraint | Tests |
|---|---|---|---|---|
| <a id="representation"></a>representation | Protobuf when `Accept` asks for it, JSON otherwise; both for every structured read | The application uses protobuf everywhere and JSON for snapshot, health and status. The administrative tool reads the token page as JSON | - | all route replays |
| <a id="error-code"></a>error-code | A refusal is a JSON object whose `error` member is an upper-snake code. A 401 carries `UNAUTHORIZED`, `SESSION_EXPIRED` or `ATTESTATION_REQUIRED` | The application reads `error` on a 401 only when the body is at most 1,024 bytes | Never grow a 401 body past 1,024 bytes | route replays |
| <a id="status-codes"></a>status-codes | 200, 304, 400, 401, 404, 429 and 5xx with their current meanings; 404 for "no such story / instrument / conversation"; 400 or 404 on `tx/{hash}/view`; on `POST …/robinhood/transactions/diagnose` a 400 is the trade-policy refusal and nothing else | The application shows the diagnose 400 as "blocked by policy" and disables signing | Never introduce a new 400 on the diagnose route for a canonical protobuf request (see [protobuf-es-reader](#protobuf-es-reader)) | route replays, `robinhood_writes_*` |
| <a id="auth-first"></a>auth-first | A protected route answers 401 to an unauthenticated request before it judges the body's schema, the quota or the path values | Deployment probes expect 401 for a JSON `{}` body without a bearer | See [fastify-errors](#fastify-errors) for the order of the framing refusals | `auth_before_body_test.py`, route edge tests |
| <a id="etag"></a>etag | Weak ETag over the selected representation; bodyless 304 on an exact `If-None-Match` | The application echoes the ETag | The value is opaque to clients, so its bytes may change whenever the body bytes do | conditional-read replays |
| <a id="retry-after"></a>retry-after | `Retry-After` is integer delta seconds (attestation 503s send 1 or 5; a 429 `RATE_LIMITED` sends 60, a busy refusal 2) | The application parses it as an integer | - | device and quota tests |
| <a id="ai-headers"></a>ai-headers | `x-next-cursor`, `x-ai-reply-for`, `x-ai-reply-status` and its value set; the `x-intelligence-authorization` request header | Read and sent by the conversation client | - | `ai_*` tests |
| <a id="ai-proof"></a>ai-proof | The proof message embeds the client's timestamp string verbatim; the snapshot hash is SHA-256 of the exact snapshot bytes; there is a timestamp tolerance window | The client signs the message | The timestamp **parser** is [date-parse](#date-parse) | `ai_open_*`, `ai_conversation_test.cpp` |
| <a id="empty-protobuf-body"></a>empty-protobuf-body | `content-type: application/protobuf` with an empty body is a valid empty message. A body-less POST with no content type is accepted | Conversation retry and pending revoke | - | `ai_messages_*`, device tests |
| <a id="query-encoding"></a>query-encoding | A query string is `application/x-www-form-urlencoded`: pairs split on `&`, name and value on the first `=`, `+` is a space, `%XX` is a UTF-8 byte | Dio sends every space as `+` (token search `q` is free user text) and every comma and colon as `%2C` / `%3A` (`ids`, `tokens`, `topics`, `include`) | One helper implements it: `src/http/query_string.h`. The strictness around the grammar is [query-string](#query-string) | `query_string_test.cpp`, `price_spot_query_test.cpp`, `token_search_*`, `*_edges_test.py` |
| <a id="path-encoding"></a>path-encoding | A path value is percent-decoded inside its segment (`%2F` does not split it), must be strict UTF-8 and at most 512 bytes | The application and the administrative tool percent-encode ids | Malformed values are refused by the router: [opaque-params](#opaque-params) | `http_server_test.cpp`, token and news replays |
| <a id="field-presence"></a>field-presence | Explicit-presence protobuf fields (proto3 `optional`, oneof members, sub-messages) are written whenever they are set, **including when the value is zero or the message is empty**. The Struct `payload` of a structured read is always written | The application's `has…()` tests: a present zero and an absent field are different answers, and an absent `payload` is a refused response | [protobuf-es-writer](#protobuf-es-writer) covers implicit-presence scalars only | `proto_wire_golden_test`, Struct replays |
| <a id="date-output"></a>date-output | The date strings that clients parse (device `expiresAt`, sync `asOf` / `changedAt`, cache `timestamp`, token page `asOf`) are UTC ISO 8601, `YYYY-MM-DDTHH:mm:ss.sssZ` | The application parses them with `DateTime.parse`; the administrative tool compares token page `asOf` values as strings | [date-parse](#date-parse) concerns dates the server **reads** | device, sync and token page replays |
| <a id="snapshot-json"></a>snapshot-json | The `/v1/prices/snapshot` JSON shape; millisecond fields are integer literals | The application drops a row whose millisecond field is not an integer literal | Any change to number output must keep integers as integer literals | `price_snapshot*` tests |
| <a id="health-status"></a>health-status | `/healthz` (`ok`, `supportedModels`) and `/statusz` (`ok`, `version`, `node` = `native-cpp…`, `uptimeSec`, `stats.window10m`) as `application/json` | Read by the application's health view | - | `smoke.py`, operations tests |
| <a id="struct-kind"></a>struct-kind | Every `google.protobuf.Value` written has a kind. An absent member is omitted from the Struct, never written as an empty `Value` | Dart `toProto3Json` throws on a `Value` without a kind | This differs from protobuf-es on purpose | Struct replays, `proto_wire_golden_test` |
| <a id="list-order"></a>list-order | Token search rows, `popularTokens` and cross-chain routes are returned in a deterministic, ranked order | The application shows server order | Keep the ranking keys. Ties are broken in byte order: [locale-compare](#locale-compare) | `token_search_*`, `network_overview_*`, `cross_chain_*` |
| <a id="dashpay-name"></a>dashpay-name | The resolved `name` is the lower-case ASCII canonical name | The application compares it byte for byte | - | `dashpay_*` |
| <a id="evm-lowercase"></a>evm-lowercase | EVM addresses are lower-case in sync rows and stored keys | The application matches them as lower-case | - | `sync_*` |
| <a id="bounds"></a>bounds | Request limits: body 512 KiB (413), path values, id counts, text lengths, quota costs. Lengths of text fields are counted in UTF-16 code units | The limits are this server's resource policy. The unit of counting is invisible for the ASCII and BMP text that clients send | Do not re-derive the limits in another unit. That would move the boundary by a few characters for astral text | query and edge tests |
| <a id="number-format"></a>number-format | JSON numbers: shortest round-trip digits, plain decimal for 1e-7 <= abs(x) < 1e21, exponent form outside, `-0` as `0`, non-finite as `null`. Display strings are built with fixed decimals (`ToFixed`) or grouping (`en-US`) | JSON responses are contract (snapshot, token page). The display strings are payload text that the application shows | Integers must stay integer literals | `json_document_writer_test.py`, `market_view_test.py`, replays |
| <a id="proto-field-order"></a>proto-field-order | Encoders write fields in ascending field-number order and omit proto3 defaults | Standard protobuf practice | - | `proto_wire_golden_test` |
| <a id="hourly-windows"></a>hourly-windows | AI conversation hourly allowances: a fixed window from the first hit, in memory, reset on restart, fail-open when the 4,096-entry table is full | Server policy. The header is `src/ai/hourly_windows.h` | - | `ai_open_test.cpp`, `ai_post_message_test.cpp` |
| <a id="text-sanitizers"></a>text-sanitizers | Provider-supplied symbols, names and descriptions are stripped of control and format characters, collapsed and truncated (16 / 64 / 2,000 units, with an ellipsis) before they reach a client | Display safety for third-party text | Truncation cuts at a code point boundary: [lone-surrogates](#lone-surrogates) | `token_ref_test` (`tests/corpus/token_ref_corpus.json`) |
| <a id="news-pipeline"></a>news-pipeline | Feed parsing, tag stripping, classification, clustering thresholds and `CanonicalArticleUrl` | The server's own product logic. The thresholds were measured | Do not re-tune the thresholds without a new measurement | `news_*` tests |

Smaller rules of the same class: quota counters are held in memory and reset
on restart, and a known path requested with another method answers 404.

## Replaced behaviours

The earlier implementation had each of these behaviours. Nothing depended on
them, and the node does not reproduce them. The "Current rule" column is the
contract. Do not reintroduce the earlier behaviour.

| Entry | Earlier behaviour | Current rule |
|---|---|---|
| <a id="fastify-errors"></a>fastify-errors | Body-framing refusals used Fastify's envelope (`statusCode`, `code: FST_ERR_…`, `error`, `message`) and were raised before authentication. A body was written even for HEAD | One envelope for every refusal: `{"error":"<CODE>"}` as `application/json; charset=utf-8`. See [Refusals and their order](#refusals-and-their-order) |
| <a id="zod-issues"></a>zod-issues | A schema refusal returned `{"error":"BAD_REQUEST","detail":[…]}` with zod issue objects | A schema refusal is `{"error":"BAD_REQUEST"}`. Every bound and every accept/refuse decision is unchanged. String `detail` values that are this server's own messages (for example `"1–100 ids"`, `"unrecognized id"`, `"invalid history range"`, `"invalid protobuf body"`) are kept |
| <a id="form-bodies"></a>form-bodies | `/v1` POST routes accepted `application/x-www-form-urlencoded` and `text/plain` bodies, then usually refused them at the schema | Both are 415 `UNSUPPORTED_MEDIA_TYPE`. Accepted request types: `application/protobuf`, `application/json`, none |
| <a id="request-coercion"></a>request-coercion | JSON and query values were coerced as ajv and `z.coerce.number()` do: `Number()` of a string (hex, octal, binary, `Infinity`, empty string is 0), scalars wrapped into arrays, booleans from strings | Strict types. See [Query strings and request types](#query-strings-and-request-types). `history_batch::JsStringToNumber` remains for **provider** payloads only (see [Parity-only behaviours that remain](#parity-only-behaviours-that-remain)) |
| <a id="query-string"></a>query-string | A repeated key became an array (then refused, or first or last by route); a malformed escape was kept verbatim; undecodable input fell back to the raw bytes | The first value wins for a repeated key. A malformed escape, or a component that is not UTF-8 after decoding, is 400. `+` as a space and `%XX` decoding stay ([query-encoding](#query-encoding)) |
| <a id="js-prototype-keys"></a>js-prototype-keys | `__proto__` keys were skipped, renamed or made the body invalid; a `constructor` object that owned `prototype` made the body invalid | They are ordinary keys and names. A strict schema refuses an unknown key (400 `BAD_REQUEST` after the quota); a non-strict schema ignores it. A `__proto__` map entry of a Bittensor query is an unsupported query, and a Bittensor path that names such a member is an unsupported path (400) |
| <a id="lone-surrogates"></a>lone-surrogates | Strings could hold lone UTF-16 surrogates: kept by the parser, produced by truncating in the middle of a pair, written as `\udXXX` in JSON and as U+FFFD in protobuf, with a separate rule for stored strings | Strings are valid UTF-8 everywhere. See [Unicode](#unicode) |
| <a id="js-property-order"></a>js-property-order | Objects were emitted in JavaScript property order (array-index keys first, ascending), in JSON and in Struct map entries | Source order for relayed documents, insertion order for built ones. A duplicate key keeps its first position and its last value. `history_batch::JsArrayIndexKey` remains as an index parser for provider arrays; it sorts nothing |
| <a id="protobuf-es-reader"></a>protobuf-es-reader | Request decoders mimicked protobuf-es 2.x: known fields read by declared type whatever the wire type, over-long 32-bit varints truncated, one cursor so that a field could run past its message, a leading BOM dropped from strings | One strict reader, `proto_wire::Reader`. See [Protobuf requests](#protobuf-requests) |
| <a id="protobuf-es-writer"></a>protobuf-es-writer | Encoders copied different zero tests from protobuf-es: one wrote `-0`, another omitted it | One rule, `proto_wire::PutImplicitDouble`: an implicit-presence double is omitted only when it is positive zero. Negative zero is written, as the [protobuf specification](https://protobuf.dev/programming-guides/proto3/) requires. `optional` fields, oneof members and sub-messages are written whenever set ([field-presence](#field-presence)) |
| <a id="locale-compare"></a>locale-compare | Sorts approximated `String.prototype.localeCompare` (ICU root collation) | Byte order. `RobinhoodSymbolCompare` orders by the bytes after ASCII lower-casing, a prefix first, then by the symbols' own bytes, which gives a total, deterministic order. The coin-popularity tie-break (`IdCompare`) is plain byte order. Ranking keys are unchanged ([list-order](#list-order)) |
| <a id="js-case"></a>js-case | Identifiers were case-mapped with the full Unicode tables (ß to SS, Final_Sigma, U+212A KELVIN SIGN to k) | Identifiers use ASCII case mapping (`compat::AsciiToLowerCase` / `AsciiToUpperCase`). Text keeps Unicode mapping. See [Case mapping](#case-mapping) |
| <a id="lmdb-msgpack"></a>lmdb-msgpack | Stored NFT and transaction records were rewritten as an lmdb/msgpack round trip left them (replacement of lone surrogates, `__proto__` renamed, a 1,978-byte key limit) | Records are stored as their JSON text, and a cached read answers what was written. An NFT item key over the native 1,024-byte bound is refused. A record written by an earlier version (with U+FFFD or a `__proto_` key) reads as stored; `address_nfts_test.cpp` and `transaction_test.cpp` pin this |
| <a id="opaque-params"></a>opaque-params | Some routes let a path value with a control character, an empty segment or an over-long value reach the handler, which authenticated first and then refused it | The router refuses such a path with 404 `{"error":"NOT_FOUND"}` for every route, before any handler, without authentication or quota. Percent-decoding of well-formed values, `%2F` inside a value and the 512-byte bound are unchanged ([path-encoding](#path-encoding)) |
| <a id="node-header-join"></a>node-header-join | A repeated `x-intelligence-authorization` was accepted by the parser and then authorized nothing | The request parser refuses it with 400, like every other repeated credential or framing header ([request-framing](#request-framing)) |

### Refusals and their order

Codes: `UNSUPPORTED_MEDIA_TYPE` (415), `BAD_REQUEST` (400), `URI_TOO_LONG`
(414), `BODY_TOO_LARGE` (413), `NOT_FOUND` (404).

Every protected POST route **authenticates first**. The device policy
(Origin, bearer, attestation, quota) answers before the body is judged. An
unauthenticated request is therefore 401 (or 403 with an Origin) whatever its
body, and a framing or schema refusal carries the quota headers. This applies
to `/v1/sync`, `/v1/prices/history`, `/v1/prices/markets`,
`/v1/bittensor/analytics`, `/v1/tokens/compare`, `/v1/crash-reports`, the
cross-chain quote and status routes, the Robinhood write routes and the AI
open and post-message routes.

- Where the quota cost depends on the body (history batch, markets,
  comparison), a body that cannot be framed is charged cost 1.
- The 414, the 400 for a raw non-ASCII byte in the request target and the
  router's 404 for a malformed path value come before every hook. They reveal
  nothing and need no credential.
- The parser's refusals of malformed HTTP framing
  ([request-framing](#request-framing)) also come before any handler.
- `POST /v1/devices` (enrollment) is unauthenticated by design. It answers
  415 for form and text bodies.
- The diagnose route answers 400 only for a body the application never
  builds.

The code is in `src/http/http_middleware.h` (`SendRefusal`,
`SelectBodyParser`).

### Query strings and request types

`src/http/query_string.h` is the one bounded, allocation-free query parser.
It serves the AI, DashPay, price (spot, history, OHLC), token, address and
news routes, the cache explorer and the operations token-price list.

- All components, including unknown keys and later duplicates, must have
  valid percent encoding and decode to UTF-8. Otherwise the route answers
  400.
- The first decoded name wins, including an empty first value.
- `+` is a space. `%XX` is decoded once; encoded separators (comma, colon,
  plus, equals) are component content.
- `refresh=true` is a literal comparison.
- Request query numbers are bounded decimal digits; leading zeros are
  allowed. Whitespace, signs, hexadecimal, exponents and `Infinity` are
  refused.
- JSON history-batch numbers and sync fields must have their declared types.
  Sync does not wrap scalars into arrays, unwrap arrays, or convert numbers
  and booleans into strings.
- Authentication and quota precede validation.

### Protobuf requests

`proto_wire::Reader` reads every request message that does not go through
nanopb. "Strict" means the protobuf specification, not stricter:

- A known field's wire type must match. A nested frame cannot consume bytes
  outside its message.
- `uint32` and length overflow, eleven-byte varints, malformed groups and
  invalid UTF-8 are refused with 400 `BAD_REQUEST`. A BOM at the start of a
  string is content.
- Valid unknown fields are skipped, because a newer client may send a field
  that this server does not know. Matching groups of bounded depth and
  unknown packed fields are skippable.
- A ten-byte varint is the canonical encoding of a negative `int32` or
  `int64` and is accepted.
- A repeated singular field keeps its last value.
- An empty body is the empty message
  ([empty-protobuf-body](#empty-protobuf-body)).

`tests/app_post_decode_test.cpp` replays POST bodies captured from the
client application through the request decoders, including the Robinhood
prepare and diagnose requests. A reader change must keep that test green: a
new 400 on the diagnose route blocks signing.

### Unicode

- The DOM and the bounded request decoder turn each lone JSON surrogate
  escape into one U+FFFD.
- UTF-8 builders, JSON output and protobuf output share the same scalar
  content. Malformed raw UTF-8 uses maximal-subpart replacement where a
  decoder permits recovery.
- Truncation counts the UTF-16 limits of [bounds](#bounds), but drops a code
  point that crosses the boundary.
- An HTML entity that spells a surrogate is replacement text. It cannot form
  a pair with a separate entity.
- New news records store one U+FFFD per lone surrogate, whatever the string
  length. Replacement characters in records written by earlier versions are
  data and are not collapsed.
- An NFT provider id that contains an escaped lone surrogate produces a valid
  key with a replacement character.
- The news text code works on UTF-16 units internally. That is separate from
  its wire and persistence representation, which is valid UTF-8.

### Case mapping

| Site | Kind | Mapping |
|---|---|---|
| `providers/price_id_parser.cpp`: network of a contract id, CoinGecko id | identifier | ASCII. A non-ASCII byte fails the id pattern |
| `prices/coin_price_scheduler.cpp`: provider row `id` | identifier (matched against requested ids) | ASCII |
| `discovery/price_markets_route.cpp`: requested ids | identifier | ASCII |
| `discovery/coin_popularity.cpp`: recorded ids | identifier | ASCII |
| `news/news_routes.cpp`: news id (ULID) | identifier | ASCII |
| `ai/ai_proof.cpp`: owner id and signer match | identifier (EVM address) | ASCII. The owner id is an HMAC over the lower-cased address, and ASCII mapping gives the same owner id for every `0x` + 40 hex address |
| `discovery/market_view.cpp`: provider `symbol` upper-casing | display text | Unicode |
| `tokens/token_comparison_store.cpp`: stored symbol upper-casing | display text | Unicode |
| `discovery/token_search_route.cpp`: search term, catalog symbol and name matching | user search text | Unicode |
| `news/news_text.cpp` | news text | Unicode |
| `codec/provider_text` `ToLowerCase` / `ToUpperCase` (callers in chains, tokens, wallet, exchange) | provider payload values | Unicode |

An id or network name with a non-ASCII letter keeps that letter, and a
Kelvin sign is not `k`. The Unicode tables in `js_case_tables.cpp` stay,
because search and news use them.

## Parity-only behaviours that remain

The code still reproduces these behaviours of the earlier implementation.
Nothing is known to depend on them. Each may be replaced by its native rule
under the stated condition.

| Entry | Behaviour | Native rule | Condition and risk | Code |
|---|---|---|---|---|
| <a id="date-parse"></a>date-parse | Date strings are parsed as V8 does: the ISO form, then the legacy free-form parser (month names, `GMT+hh`, parenthesised comments, two-digit years, local-time `YYYY-MM-DD HH:mm`) | A parser for exactly ISO 8601 and RFC 822/1123 dates; anything else is "no date". The ISO form must accept `Z` and `±hh:mm` offsets and one to nine fractional digits, and must keep reading a zone-less date-time as UTC | Clients send ISO 8601 only. News feeds send RFC 822 / RFC 1123 or ISO 8601. Whether every provider sends only those forms has **not** been verified, and many call sites read cross-chain and chain provider payloads. A provider with an unusual date format would lose its dates; for a cross-chain status that is a wrong answer, not a missing decoration. Sample real feed and provider dates first. Medium risk | `compat/js_date_parse.{h,cpp}`, `discovery/market_view.cpp` (`DateParseValid`), callers through `provider_text::DateParse` and `cross_chain_support::DateParse` |
| <a id="promise-all"></a>promise-all | Concurrent provider calls reproduce `Promise.all` timing: every request is issued before the first answer, and the failure that **settled first** decides the error | Deterministic: the first failure in call order decides | A client sees a failure code, not which sub-call lost the race. High risk: this is concurrency code with a sensitive shutdown order. Change it only when that code is reworked for another reason | `chains/chain_transport.{h,cpp}` (`FirstSettledFailure`) and its callers in chains, exchange, wallet and tokens |
| js-case (provider values) | `codec/provider_text` `ToLowerCase` / `ToUpperCase` map provider payload values with the Unicode tables | - | They are payload text, not identifiers. See [Case mapping](#case-mapping) | `codec/provider_text.{h,cpp}` |
| request-coercion (provider values) | `history_batch::JsStringToNumber` converts provider payload strings with ECMAScript `Number()` rules | - | It no longer applies to requests ([request-coercion](#request-coercion)) | callers in chains, tokens, wallet |
| js-prototype-keys (relayed data) | `exchange/cross_chain_support.cpp` drops a `__proto__` member when it copies a provider payload | - | Response side only. Requests treat the key as ordinary ([js-prototype-keys](#js-prototype-keys)) | `exchange/cross_chain_support.cpp` |

## Security

Keep these, or replace them with something at least as strict. Never weaken
them.

| Entry | Behaviour | Why it is security-relevant | Constraint | Tests |
|---|---|---|---|---|
| <a id="whatwg-url"></a>whatwg-url | Every URL the server fetches or republishes is parsed by one WHATWG-conformant parser: scheme, credentials, host type (domain / IPv4 / IPv6), port, percent-decoding of the host, IPv4 number forms (`0x7f.1`, `2130706433`) | `news/media_service.cpp` and `news/news_service.cpp` decide from the parsed host and port whether a feed, image or redirect target may be dialled (SSRF). The `https_url` / `safe_https` helpers in exchange, tokens and wallet and `market_view::IsHttpUrl` decide which provider links reach a client. `cross_chain_service.cpp` refuses to start on a bad MAYA root | Do not replace the parser with ad-hoc string checks. The IPv4 number forms and host percent-decoding are exactly what an SSRF filter must normalise | `whatwg_url_test.py`, `media_get_*`, `news_*` |
| <a id="idna"></a>idna | Non-ASCII and `xn--` hosts go through UTS #46 mapping, normalisation, bidi and joiner checks before the address policy sees them | A host that maps to a different ASCII name after normalisation must be judged by the name that will be resolved | A stricter alternative exists: refuse every host that is not already lower-case ASCII LDH, and every `xn--` label. It would remove `idna.{h,cpp}` and `idna_tables.cpp`, and would drop images and feeds served from internationalised domains. That is a product decision, not a clean-up | `whatwg_url_test.py` |
| <a id="redaction"></a>redaction | Crash reports and AI conversation text are scanned for secrets (private keys, seed phrases, tokens), with NFKC folding and confusable mapping so that disguised secrets are still caught. Wallet addresses and user paths are scrubbed | Decides what is stored and what is sent to the model provider | Keep the generated tables and their `check-redaction-tables` gate | `diagnostic_redaction_test` (`tests/corpus/crash_redaction_corpus.json`), `ai_conversation_test.cpp` |
| <a id="js-regex"></a>js-regex | A bounded backtracking matcher (step and frame budgets) runs the secret-request patterns of the AI reply filter and the patterns of the news pipeline | `ai/ai_reply.cpp` withholds a model reply that asks the user for a seed phrase or remote access. The budgets bound hostile input | Keep the engine and both pattern sets. "JavaScript" names the syntax it accepts | `ai_*` reply tests |
| <a id="request-framing"></a>request-framing | The request parser refuses (400) a repeated `Host`, `Content-Length`, `Content-Type`, `Authorization`, `x-intelligence-authorization`, `Accept`, `Origin` or `If-None-Match`, any `Transfer-Encoding`, a non-digit or empty `Content-Length`, a header name that is not a token, a control character in a header value and a NUL anywhere in the head. A body longer than the limit is 413 before it is read | Two parties that disagree on which `Content-Length`, `Host` or `Authorization` counts is the request-smuggling and credential-confusion pattern. This parser is the only place where repeated fields are judged | Never relax a duplicate refusal to "first value wins" | `http_server_test.cpp`, `*_edges_test.py` |
| <a id="json-bounds"></a>json-bounds | The bounded tokenizer's limits (256 KiB, 8,192 tokens, depth 16, strict UTF-8) and every native input bound | Resource exhaustion from untrusted bodies and provider replies | Listed as "no" in codec-semantics.md | codec tests |
| <a id="auth-order"></a>auth-order | Authentication, then quota, then validation, then work; attestation and bearer checks | Unauthenticated callers cause no validation work and learn nothing about stored state | Body-framing refusals come after authentication ([fastify-errors](#fastify-errors)) | route edge tests, `auth_before_body_test.py` |

## Invariants for any future change

Whatever else changes, keep:

- the JSON response representation;
- the `error` code and status of every refusal the application branches on
  (a 400 on the diagnose route means only "policy");
- 401 before body judgement on protected routes;
- ETag and 304, and integer `Retry-After`;
- a kind on every Struct `Value`, and explicit field presence;
- integer literals in the snapshot, and ISO 8601 date strings;
- `+` and `%XX` decoding of query values, and `%2F` decoding inside a path
  value;
- the AI headers and proof rules;
- the refusal of repeated framing and credential headers;
- the URL parser, redaction and the regex engine.

## Expectation mapping in the replay tests

Some expectations under `tests/corpus/` record the answers of the earlier
TypeScript implementation. The tests do not regenerate them. Where the node
deliberately answers differently (the [replaced behaviours](#replaced-behaviours)),
a replay maps the recorded answer to this server's answer through one helper
per behaviour:

- `tests/compat_expectations.h` for the C++ tests;
- `tests/compat_expectations.py` for the Python replays.

Each helper names the entry it implements and changes nothing else. A mapping
is keyed on the recorded request (its content type, a `__proto__` key, a
malformed path value) or on the recorded issue array, so the answer to any
other request is still compared byte for byte. Examples:

- `is_zod_refusal` / `zod_refusal` map a recorded issue list to
  `{"error":"BAD_REQUEST"}` ([zod-issues](#zod-issues)).
- `fastify_expected` / `fastify_case` (Python) and `FastifyExpected` (C++)
  map a recorded Fastify or Node.js envelope to this server's code. Given
  the request headers, a request without a usable credential expects the
  device policy's 401 or 403 ([fastify-errors](#fastify-errors),
  [form-bodies](#form-bodies)).
- `malformed_path_target` and `router_refused` expect the router's 404 for a
  malformed path value ([opaque-params](#opaque-params)).
- `byte_order_ties` re-breaks recorded equal-score ties in byte order
  ([locale-compare](#locale-compare)).
- `surrogate_text` maps both recorded forms of a lone surrogate to one
  U+FFFD ([lone-surrogates](#lone-surrogates)).
- `QuotaShift` moves later recorded `x-rate-limit-remaining` values by the
  cost that a changed refusal adds or removes.

Never change an expected value in `tests/corpus/` to make a test pass. Add or
extend a mapping helper, and name the entry.
