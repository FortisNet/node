# Codec semantics

This document lists the JSON and protobuf codecs in the node: what each one
guarantees, its bounds, and when to use which. Anyone who reads or writes a
request, a response, a provider reply or a stored document needs it.

The node was ported from an earlier TypeScript service, and some codec
behaviours match that implementation (JavaScript `JSON.parse` and
`JSON.stringify`, protobuf-es 2.x). The "JS parity only" column in the tables
marks them:

- `yes`: the behaviour exists only because the earlier implementation had it.
  Nobody would design it for its own sake, and it can be reconsidered where
  the wire contract allows.
- `no`: a deliberate native bound, a rule of this server's contract, or a
  property of the format itself.

The request readers, the Unicode rules and the member order are native
rules, not parity rules. [compat-decisions.md](compat-decisions.md) records
those decisions.

## Codec map

| Codec | Files | Model | Use it for |
|---|---|---|---|
| Bounded tokenizer (`BoundedJsonDocument`) | `src/codec/bounded_json.{h,cpp}` | Strict, allocation-free tokens over the caller's buffer | Untrusted request bodies and provider replies that are read field by field |
| JS serializer (`js_json`) | `src/compat/js_json.{h,cpp}` | Not a parser: `JSON.stringify` and `google.protobuf.Value/Struct` output over a bounded token document | Relaying a provider payload as JSON or as a Struct |
| Mutable DOM (`tj`) | `src/codec/token_json.{h,cpp}` | Arena DOM with JavaScript value semantics; parse, build, patch, stringify, Struct | Documents the server builds or patches itself and then writes as JSON and as a Struct |
| News DOM (`news::json`) | `src/news/news_json.{h,cpp}` | Read-only arena DOM, UTF-16 strings, source spans | The news store's own cached documents (re-emitting elements byte for byte) |
| Shared JSON lexer (`json_lex`) | `src/codec/json_lex.h` | Header-only: whitespace, number grammar, hex digits, UTF-8 encode | Internal to the three readers above |
| JS text helpers (`history_batch`) | `src/codec/price_history_batch_codec.{h,cpp}` | `OutBuffer`, `AppendJsNumber`, `AppendJsonString`, `JsStringToNumber`, ... | Hand-written JSON output and JavaScript string/number rules |
| Protobuf wire codec (`proto_wire`) | `src/codec/proto_wire.{h,cpp}` | The one hand-written writer (templates over a byte sink) and bounded typed request readers | Every hand-written protobuf encoder and the request decoders listed below |
| nanopb | `third_party/nanopb`, `proto/generated` | Generated fixed-shape messages | Device challenge / enrollment / rotation, `/v1/networks`, DashPay name response |

The three JSON readers are kept apart on purpose: none is a subset of
another (see the tables and
[Why the JSON readers stay separate](#why-the-json-readers-stay-separate)).
What they have in common, the lexical rules, lives once in `json_lex.h`.

## JSON readers: bounds and policies

| Property | Bounded tokenizer | Mutable DOM (`tj`) | News DOM (`news::json`) |
|---|---|---|---|
| Input bound | 256 KiB (`kBoundedJsonMaxInputBytes`); `ParseWithLimits` up to 1 MiB; empty input refused | None of its own; bounded by the arena | None of its own; bounded by the arena |
| Node bound | 8,192 tokens (`ParseWithLimit` raises it for a measured caller) | Arena capacity (fixed region, or `GrowingArena(limit)`) | Arena capacity |
| Depth | 16 nested containers (`kBoundedJsonMaxDepth`), error `kDepthLimit` | 64 (`tj::kMaxDepth`); stringify and Struct writers refuse past 256 | 32 |
| Number grammar | JSON (shared `json_lex::ScanNumber`) | same | same |
| Number value | Kept as text span; `Number()` is `strtod` to a double | `strtod` to a double at parse time | `strtod` to a double at parse time |
| Number bounds | Literal shorter than 64 bytes and finite, else the document is refused | None: any length; an out-of-range literal becomes +-Infinity | Literal shorter than 64 bytes; non-finite accepted |
| Exact / big integers | Not supported (callers that need digits read the text span) | Not supported (double only) | Not supported |
| Input encoding | Strict UTF-8: overlong, surrogate and > U+10FFFF sequences refuse the document | WHATWG decode: invalid UTF-8 becomes U+FFFD per maximal subpart; one leading BOM dropped | Structural UTF-8 check only (lead byte and continuation bytes) |
| Raw control characters in strings | Refused | Refused | Refused |
| `\uXXXX` surrogate pair | Combined to one code point | Combined to one code point | Kept as two UTF-16 units (the string type is UTF-16) |
| Lone surrogate escape | U+FFFD | U+FFFD | Kept as a UTF-16 unit |
| Decoded string form | Undecoded span; `DecodeString` writes strict UTF-8 | Valid UTF-8, NUL-terminated, arena copy | UTF-16 code units (`U16`), arena copy |
| Duplicate keys | Every member is kept as tokens; `FindObjectValue` returns the **last** | First position, **last** value (one member) | Every member kept in source order; `Find` returns the **last** |
| Member order | Source order | Source/insertion order | Source order |
| Trailing bytes after the value | Refused (whitespace allowed) | Refused (whitespace allowed) | Refused (whitespace allowed) |
| Allocation | None: caller-owned token array | Caller-owned bump arena (fixed, or growing by ~1 MiB blocks up to a limit); nothing freed individually | `news::Arena` bump allocation |
| Source spans kept | Yes (tokens index the input) | No | Yes (`raw`, `raw_length` per value) |
| Mutable | No | Yes (`Set`, `Remove`, `Push`, `Clone`) | No |
| JSON output | `js_json::Stringify` | `tj::Stringify` | None (elements are re-emitted from their source span) |
| Protobuf output | `js_json::AppendProtoValue` / `AppendProtoStruct` | `tj::AppendProtoStruct` / `AppendStructuredReadResponse` | `news_routes` message encoders on `proto_wire` |

## JSON behaviours and whether they are JS parity only

| Codec | Behaviour | JS parity only | Callers (modules) |
|---|---|---|---|
| Bounded tokenizer | 256 KiB / 8,192 tokens / depth 16 bounds; document refused past them | no (native resource bound; stricter than `JSON.parse`) | auth (Play Integrity), codec (crash report), discovery, news (media), operations (crash report route), prices, providers, tokens (comparison route), wallet (sync request) |
| Bounded tokenizer | Number literal must be < 64 bytes and finite | no (native bound; `JSON.parse` accepts both) | same |
| Bounded tokenizer | Strict raw UTF-8; lone-surrogate escapes become U+FFFD | no (one Unicode scalar representation) | same |
| Bounded tokenizer | `FindObjectValue` returns the last duplicate | yes (`JSON.parse` last-wins) | same |
| Bounded tokenizer | Numbers read through `strtod` as IEEE doubles (`9007199254740993` reads as `...992`) | yes (JavaScript has only doubles; a native API could keep integers exact) | same |
| `js_json::Stringify` | Duplicate keys: first position, last value | yes (`JSON.stringify(JSON.parse(text))`) | discovery (markets, token search, bittensor, route support), prices (coin price scheduler), news (price samples) |
| `js_json::Stringify` | Keys retain source order | no | same |
| `js_json::Stringify` | Numbers re-formatted with `Number.prototype.toString` (see number formatting below), so `1E3` becomes `1000` and `-0` becomes `0` | yes | same |
| `js_json::Stringify` | String escaping: `\"`, `\\`, `\b \f \n \r \t`, other controls as lowercase `\u00xx`; `/`, U+007F, U+2028 and U+2029 are not escaped; escapes in the input are decoded then re-escaped | yes (`JSON.stringify` well-formed output) | same |
| `js_json::AppendProtoValue/Struct` | Same member order and duplicate rule as `Stringify` for `Struct.fields` map entries | yes (protobuf-es `fromJson` then `toBinary` iterates the JS object) | discovery, exchange (Robinhood policy) |
| `js_json::AppendProtoValue/Struct` | `-0` is written as `number_value` -0 although `Stringify` writes `0` | yes (the double survives `JSON.parse`; only `toString` drops the sign) | same |
| `js_json::ForEachTopLevelArrayElement` | Splits a top-level array without tokenizing it | no (native memory optimisation) | discovery (markets), prices (coin price scheduler) |
| `tj::Parse` | Invalid UTF-8 replaced by U+FFFD (maximal subparts), leading BOM dropped | yes (the text `Response.json()` / `TextDecoder` would hand to `JSON.parse`) | ai, chains, exchange, tokens, wallet |
| `tj::Parse` | Lone surrogate escapes become one U+FFFD | no (valid UTF-8) | same |
| `tj::Parse` | Number literals of any length become the nearest double; overflow becomes +-Infinity | yes (`JSON.parse` number semantics) | same |
| `tj::Parse` | Depth 64 | no (native bound) | same |
| `tj` objects | Duplicate keys and `Set` on an existing key: first position, last value | yes (object spread / assignment) | same |
| `tj` objects | Source/insertion order | no | same |
| `tj::Stringify` | Lone surrogate written as `\udXXX` (lowercase hex) | yes (well-formed `JSON.stringify`) | same |
| `tj::Stringify` | Non-finite number written as `null`; `-0` written as `0` | yes (`JSON.stringify`) | same |
| `tj::AppendProtoStruct` | The same valid UTF-8 strings used by JSON | no | exchange (cross-chain, Robinhood), tokens, wallet (address, sync) |
| `tj::AppendProtoStruct` | `-0`, NaN and the infinities are written bit for bit as `number_value` | yes for `-0` (see above); NaN/Infinity only arise from documents built in code | same |
| `news::json::Parse` | Strings decoded to UTF-16 units; lone surrogates kept; source spans kept | partly: UTF-16 is what the ported news text code (JavaScript string indices, `.length`, regex) operates on | news (routes, reactions, reaction prices) |
| `news::json::Find` | Returns the last duplicate | yes (`JSON.parse` last-wins) | news |
| `news::json::Parse` | Depth 32; number literal < 64 bytes; structural UTF-8 check only | no (native bounds; the input is the server's own output) | news |
| `history_batch::AppendJsNumber` | Shortest round-trip digits, then `Number.prototype.toString` layout: plain decimal for 1e-7 <= abs(x) < 1e21, otherwise `d.ddde+x` / `d.ddde-x`; `-0` is `0`; non-finite is `null` | yes for the layout thresholds, the `e+21` form and the `-0` / `null` rules; shortest round-trip digits themselves are a sound native choice | chains, codec, compat (token text), discovery, news, operations, prices, tokens, wallet |
| `history_batch::AppendJsonString` | `JSON.stringify` escaping of valid UTF-8 (same set as above; no lone-surrogate handling, input must be UTF-8) | yes (which characters are escaped, and lowercase hex) | app, codec, compat, discovery, news, operations, prices, tokens, wallet |
| `history_batch::JsStringToNumber` | ECMAScript `Number("...")`: trims JS whitespace, accepts hex/octal/binary prefixes and `Infinity`, empty string is 0 | yes | chains, compat, discovery, news, prices, tokens, wallet |
| `history_batch::JsArrayIndexKey` | Canonical array-index key test (a decimal index without a leading zero), used to index into provider arrays | yes | chains (asset index) |

## Protobuf writer

There is one hand-written writer, `fortis::proto_wire` in
`src/codec/proto_wire.h`. Every encoder that does not go through nanopb
builds on it.

| Property | `proto_wire` writer |
|---|---|
| Sink | Any type with `Append(const void*, size_t)`: `history_batch::OutBuffer`, `proto_wire::Span` (fixed stack region), route-local growable buffers, the spot serializer's `Writer` |
| Varint | Base-128 little-endian, 1..10 bytes (`EncodeVarint`, `VarintSize`, `PutVarint`); a negative `int64` is passed sign-extended and takes ten bytes |
| Zigzag (`sint32/64`) | Not implemented: no schema here uses it |
| Fixed64 / double | Eight little-endian bytes, every bit preserved (`PutFixed64`, `PutDouble`) |
| Fixed32 / float | Tag constant only (`kWireFixed32`): no schema here writes one |
| Length prefix | Never back-patched. Either the nested message is encoded into a `Span` and copied (`PutBytes`), or it is measured with a counting `OutBuffer` (null data) and then written (`PutLengthPrefix`): two-pass. The two Struct emitters measure bottom-up once and cache the sizes |
| Packed repeated | Not used by any current message; no helper on purpose |
| Default-value omission | None in the writer. The caller decides per field (see the next table) |
| Field ordering | Caller's. Every encoder writes fields in ascending field-number order, the order protobuf-es `toBinary` uses |
| Overflow | Sink-defined: `OutBuffer` and `Span` drop the whole append and clear `ok`; nothing is written past capacity |
| Struct framing | `kValue*` field numbers, `DelimitedSize`, `StructEntrySize`, `PutNullValue`, `PutBoolValue`, `PutNumberValue`, `PutStructEntryKeyPrefix`, `PutStructEntryValuePrefix`, shared by `js_json` and `tj` |

### protobuf-es behaviours kept by the encoders

| Codec | Behaviour | JS parity only | Callers (modules) |
|---|---|---|---|
| all `proto_wire` encoders | Fields written in ascending field-number order | no for decoding (any order is valid protobuf); yes for byte equality with protobuf-es `toBinary` | ai, discovery, exchange, news, prices, tokens, wallet |
| all `proto_wire` encoders | proto3 implicit-presence fields at their default are omitted (empty string, 0, false); explicit `optional` fields, oneof members and set sub-messages are written even when empty | no (proto3 rule), but **which** sub-messages are always written matches the earlier TypeScript handlers (for example the spot and OHLC `cache` message is always present) | same |
| `proto_wire::PutImplicitDouble` (OHLC, history view, spot `price`, Robinhood markets) | An implicit double is omitted only when it is positive zero (all bits clear). `-0` is distinct under protobuf's presence rules and **is** written | no (protobuf rule) | prices, exchange |
| `prices` spot (`spot_response_serializer.cpp`) | `change_24h_pct` is `optional` and written when present, including 0 | no (explicit presence) | prices |
| `js_json` / `tj` Struct emitters | `google.protobuf.Struct` map entries preserve source/insertion order, one entry per unique key with the last value | no | discovery, exchange, tokens, wallet |
| `js_json` / `tj` Struct emitters | Every map entry carries both key and value; every `Value` oneof member is written, including `null_value` 0, `false`, `0` and `""` | no (oneof and map-entry rules of the format; protobuf-es writes both entry fields even at defaults, which this matches) | same |
| `tj::AppendProtoStruct` | Strings and keys share the DOM valid UTF-8 representation | no | exchange, tokens, wallet |
| `ai_service.cpp` message strings | Implicit strings omitted when empty; malformed raw UTF-8 uses Unicode maximal-subpart replacement | no | ai |
| `news` encoders (`news_routes_internal.h`) | UTF-16 strings re-encoded as UTF-8; implicit strings and zero varints omitted; optional doubles written when the JSON member is a number | no (proto3), apart from the UTF-16 source | news |
| `wallet/sync_route.cpp` | Records streamed with pre-measured sizes into a growable buffer; a record that does not fit ends the page | no (native paging bound) | wallet |
| `exchange/dashpay_proof.cpp` request | Plain protobuf for Dash Platform gRPC (`GetDocumentsRequest`), nested messages built in bounded spans | no (third-party schema, no JS involved) | exchange |

`exchange/dashpay_proof.cpp` also has `encode_leb128` / `leb128`: that is
GroveDB's Merk proof encoding, not protobuf, and stays with the proof code.

## Protobuf readers

| Codec | Behaviour | JS parity only | Callers (modules) |
|---|---|---|---|
| `proto_wire::ReadVarint32` | Bounded 64-bit varint, rejected above UINT32_MAX; no truncation | no | request decoders |
| `proto_wire::ReadVarint64` | At most ten bytes; tenth byte <=1; canonical ten-byte signed negative values remain valid | no | same |
| `proto_wire::ReadTag` | Reject field 0, invalid wire types, tags wider than 32 bits | no | same |
| `proto_wire::SkipField` | Skip valid unknown wire fields, including packed length-delimited fields; groups must match and have bounded depth | no | same |
| `proto_wire::Reader` | Known field wire types must match. `Frame` bounds nested messages. Strings are strict UTF-8 and preserve BOM content | no | discovery, cross-chain, Robinhood, comparison |
| history batch, crash reports, sync and AI decoders | Shared bounded primitives plus explicit known-field wire checks; last singular value wins | no | codec, wallet, AI |
| `DiscoveryProtoReader` and comparison `ProtoReader` | Aliases of the shared `Reader` | no | discovery, exchange, comparison |
| Bittensor Struct map entries | Valid unknown entry fields are skipped; missing key/value and repeated values retain protobuf merge semantics | no | discovery |
| `pb_varint` etc. in `exchange/dashpay_proof.cpp` | Plain protobuf reader for Tenderdash / Platform responses | no | exchange |
| nanopb `pb_decode` | Generated decoders | no | auth, operations |

## Why the JSON readers stay separate

- **The three JSON readers.** The bounded tokenizer is allocation-free and
  strict. `tj` allocates a mutable DOM and is lenient in the ways
  `JSON.parse` over `Response.json()` is. `news::json` decodes to UTF-16 and
  keeps source spans. Each row of "JSON readers: bounds and policies" where
  the columns differ is a difference visible on the wire or in memory, so
  folding one reader into another would change behaviour.
- **The `js_json` and `tj` Struct emitters.** They produce the same output
  format from different inputs: a token document with duplicate-member
  scratch, and a DOM that is already in insertion order. They share the
  framing in `proto_wire.h`; the traversals stay separate.
- **Presence helpers.** `price_history_view.h` (`PutImplicitString`,
  `PutInt64`, ...), `token_comparison.cpp` (`put_implicit`, `put_date`, ...),
  `news_routes_internal.h` (`Proto`) and `ai_service.cpp` (`string_field`)
  each wrap `proto_wire` with the presence rule of their message. They are
  thin wrappers and are not shared.

The request decoders are not separate: discovery, cross-chain, Robinhood and
comparison all use `proto_wire::Reader`.

## How this is tested

- `tests/proto_wire_golden_test.cpp` (`./build/proto_wire_golden_test`, part
  of `make test`) checks the writer over boundary values and 200,000
  pseudo-random values, and checks whole outputs against the golden byte
  strings in `tests/proto_wire_golden.inc`: `JSON.stringify` output, `Value`,
  `Struct` and `StructuredReadResponse` bytes for documents that cover
  duplicate keys, array-index keys, number forms, escapes, lone surrogates,
  `-0` and non-finite numbers, plus whole messages that use every writer
  entry point. Regenerate the goldens only for an intended wire change:
  `./build/proto_wire_golden_test --print`.
- `python3 tests/js_json_test.py build/js_json_test` compares the serializer
  with recorded reference answers: an independent source-order `Map`
  reference using Node.js scalar formatting and protobuf-es scalar encodings
  with independent Struct framing.
- `tests/proto_request_reader_test.cpp`, `tests/query_string_test.cpp` and
  `tests/unicode_contract_test.cpp` pin the shared request reader, the query
  parser and the Unicode rules.
- `tests/app_post_decode_test.cpp` replays POST bodies captured from the
  client application (`tests/app_post_capture_test.dart`) through the request
  decoders.
- The module replay tests compare whole HTTP responses, JSON and protobuf,
  with the expectations in `tests/corpus/`.
