#include "threading.h"
#include "proto_wire.h"
#include "token_comparison_route.h"
#include "ownership.h"
#include "token_jobs.h"

// POST /v1/tokens/compare: the contract is the comment above handle_compare.

#include "bounded_json.h"
#include "device_enrollment.h"
#include "price_history_batch_codec.h"
#include "token_comparison.h"
#include "token_comparison_store.h"
#include "token_ref.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::tokens {

namespace {

using fortis::tokens::comparison::ErrorEntry;
using fortis::tokens::comparison::Item;
using fortis::tokens::comparison::Record;
using fortis::tokens::comparison::kMaxTokens;
using fortis::codec::history_batch::OutBuffer;

constexpr size_t kMaxNetworkUnits = 64;
constexpr size_t kMaxTokenIdUnits = 200;
// A UTF-16 unit is at most three UTF-8 bytes (a surrogate pair is four bytes
// for two units).
constexpr size_t kMaxNetworkBytes = 3 * kMaxNetworkUnits;
constexpr size_t kMaxTokenIdBytes = 3 * kMaxTokenIdUnits;
constexpr size_t kResponseCapacity = 192 * 1024;
const char kCacheControl[] = "private, max-age=30";
const char kBadRequest[] = "{\"error\":\"BAD_REQUEST\",\"detail\":\"invalid comparison request\"}";
const char kInternal[] = "{\"error\":\"INTERNAL\"}";

struct InputToken {
  char network[kMaxNetworkBytes + 1]{};
  size_t network_length = 0;
  char token_id[kMaxTokenIdBytes + 1]{};
  size_t token_id_length = 0;
};

enum class BodyKind : uint8_t { kUndefined, kJson, kProtobuf };

// work_mutex owns the work buffers. They are allocated when the route is
// registered (about 0.95 MiB), so an image that never registers the route
// pays nothing for them.
fortis::base::Mutex work_mutex;
struct CompareWork {
  codec::BoundedJsonToken json_tokens[codec::kBoundedJsonMaxTokens];
  char decode_scratch[codec::kBoundedJsonMaxInputBytes + 8];
  InputToken inputs[kMaxTokens];
  networks::TokenNetwork networks[kMaxTokens];
  networks::TokenRef parsed_ref;
  fortis::tokens::token_store::Key keys[kMaxTokens];
  const char* key_token_ids[kMaxTokens];
  char key_token_storage[kMaxTokens][networks::kTokenRefMaxIdBytes + 1];
  Record records[kMaxTokens];
  Item items[kMaxTokens];
  ErrorEntry errors[kMaxTokens];
  uint8_t response_body[kResponseCapacity];
  uint8_t validator_body[kResponseCapacity];
};
CompareWork* work = nullptr;

void send_body(int client, int status, const char* reason, const char* body,
               int rate_cost = -1, uint32_t remaining = 0, const char* cache_control = nullptr) {
  http::HttpResponse response{status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = rate_cost;
  response.rate_limit_remaining = remaining;
  response.cache_control = cache_control;
  WriteHttpResponse(client, response);
}

const codec::BoundedJsonToken* token(const codec::BoundedJsonDocument& json, uint32_t index) {
  return index == codec::kBoundedJsonNoToken ? nullptr : json.token(index);
}

// TypeScript requestCost: the raw `tokens` array length of a JSON object body.
uint32_t request_cost(BodyKind kind, const codec::BoundedJsonDocument& json) {
  if (kind != BodyKind::kJson) return 1;
  const codec::BoundedJsonToken* root = token(json, json.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) return 1;
  const codec::BoundedJsonToken* tokens = token(json, json.FindObjectValue(json.root(), "tokens"));
  if (!tokens || tokens->type != codec::BoundedJsonType::kArray) return 1;
  uint32_t count = 0;
  for (uint32_t child = tokens->first_child; child != codec::kBoundedJsonNoToken;
       child = json.token(child)->next_sibling) ++count;
  return count ? count : 1;
}

// z.string().trim().min(1).max(max_units), copied into `out`.
bool trimmed_string(const char* text, size_t length, size_t max_units, size_t max_bytes,
                    char* out, size_t* out_length) {
  size_t begin = 0, end = 0;
  fortis::codec::history_batch::JsTrim(text, length, &begin, &end);
  const size_t units = fortis::codec::history_batch::Utf16Units(text + begin, end - begin);
  if (!units || units > max_units || end - begin > max_bytes) return false;
  memcpy(out, text + begin, end - begin);
  out[end - begin] = '\0';
  *out_length = end - begin;
  return true;
}

bool json_string_field(const codec::BoundedJsonDocument& json, uint32_t index, size_t max_units,
                       size_t max_bytes, char* out, size_t* out_length) {
  const codec::BoundedJsonToken* item = token(json, index);
  size_t length = 0;
  return item && item->type == codec::BoundedJsonType::kString &&
      json.DecodeString(index, work->decode_scratch, sizeof(work->decode_scratch), &length) &&
      trimmed_string(work->decode_scratch, length, max_units, max_bytes, out, out_length);
}

// Every own key is one of `allowed` (z.object(...).strict()).
bool strict_keys(const codec::BoundedJsonDocument& json, uint32_t object, const char* a, const char* b) {
  const codec::BoundedJsonToken* item = token(json, object);
  if (!item || item->type != codec::BoundedJsonType::kObject) return false;
  for (uint32_t key = item->first_child; key != codec::kBoundedJsonNoToken;) {
    if (!json.StringEquals(key, a) && !json.StringEquals(key, b)) return false;
    const uint32_t value = json.token(key)->next_sibling;
    key = json.token(value)->next_sibling;
  }
  return true;
}

bool validate_json(const codec::BoundedJsonDocument& json, size_t* count) {
  const uint32_t root = json.root();
  if (!strict_keys(json, root, "tokens", "fiat")) return false;
  const uint32_t fiat = json.FindObjectValue(root, "fiat");
  if (fiat != codec::kBoundedJsonNoToken && !json.StringEquals(fiat, "usd")) return false;
  const codec::BoundedJsonToken* tokens = token(json, json.FindObjectValue(root, "tokens"));
  if (!tokens || tokens->type != codec::BoundedJsonType::kArray) return false;
  size_t n = 0;
  for (uint32_t child = tokens->first_child; child != codec::kBoundedJsonNoToken;
       child = json.token(child)->next_sibling) {
    if (n == kMaxTokens || !strict_keys(json, child, "network", "tokenId")) return false;
    InputToken& input = work->inputs[n++];
    if (!json_string_field(json, json.FindObjectValue(child, "network"), kMaxNetworkUnits,
                           kMaxNetworkBytes, input.network, &input.network_length) ||
        !json_string_field(json, json.FindObjectValue(child, "tokenId"), kMaxTokenIdUnits,
                           kMaxTokenIdBytes, input.token_id, &input.token_id_length))
      return false;
  }
  *count = n;
  return n >= 1;
}

using ProtoReader = fortis::codec::proto_wire::Reader;

constexpr int kRecursionLimit = 100;

bool read_token_key(ProtoReader& reader, uint64_t end, InputToken* out, bool* store) {
  ProtoReader::Frame frame(reader, end);
  if (!frame.ok()) return false;
  const uint8_t* network = reinterpret_cast<const uint8_t*>("");
  const uint8_t* token_id = network;
  size_t network_length = 0, token_id_length = 0;
  while (reader.pos() < end) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return false;
    if (field == 1) { if (!reader.String(&network, &network_length)) return false; }
    else if (field == 2) { if (!reader.String(&token_id, &token_id_length)) return false; }
    else if (!reader.Skip(wire, field, kRecursionLimit - 2)) return false;
  }
  if (!*store) return true;
  // Zod runs after decoding; a decode failure later in the body still wins,
  // and both answer the same 400, so a failed field check only marks it.
  if (!trimmed_string(reinterpret_cast<const char*>(network), network_length, kMaxNetworkUnits,
                      kMaxNetworkBytes, out->network, &out->network_length) ||
      !trimmed_string(reinterpret_cast<const char*>(token_id), token_id_length, kMaxTokenIdUnits,
                      kMaxTokenIdBytes, out->token_id, &out->token_id_length))
    *store = false;
  return true;
}

// Decodes and validates; false for a decode failure or a Zod failure.
bool decode_protobuf(const uint8_t* body, size_t length, size_t* count) {
  ProtoReader reader{body, length};
  size_t n = 0;
  bool valid = true;
  // A repeated `optional string fiat` keeps its last value (protobuf-es), so
  // only the last one meets z.literal('usd').
  bool fiat_usd = true;
  while (reader.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return false;
    if (field == 1) {
      uint32_t size = 0;
      if (!reader.MessageLength(&size)) return false;
      const uint64_t end = static_cast<uint64_t>(reader.pos()) + size;
      InputToken scratch;
      bool store = valid && n < kMaxTokens;
      if (!read_token_key(reader, end, store ? &work->inputs[n] : &scratch, &store)) return false;
      if (!store) valid = false;
      ++n;
    } else if (field == 2) {
      const uint8_t* fiat = nullptr;
      size_t fiat_length = 0;
      if (!reader.String(&fiat, &fiat_length)) return false;
      // Zod: z.literal('usd') once present (an empty string included).
      fiat_usd = fiat_length == 3 && memcmp(fiat, "usd", 3) == 0;
    } else if (!reader.Skip(wire, field, kRecursionLimit - 1)) {
      return false;
    }
  }
  *count = n;
  return valid && fiat_usd && n >= 1 && n <= kMaxTokens;
}

int64_t route_now_ms(const TokenComparisonRouteContext* route) {
  if (route->clock) {
    const int64_t fixed = route->clock->NowMs();
    if (fixed >= 0) return fixed;
  }
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void copy_native(fortis::tokens::comparison::OptString* field, const char* text) {
  if (field->present || !text) return;
  const size_t n = strlen(text);
  if (n > fortis::tokens::comparison::kMaxStoredString) return;
  field->present = true;
  field->length = n;
  memcpy(field->text, text, n + 1);
}

// POST /v1/tokens/compare — compares up to 20 network tokens in USD.
//
// TypeScript: the handler (registerComparisonRoutes),
// TokenComparisonService.compare and EmbeddedTokenComparisonRepository.read,
// compareTokenRecords, comparisonMessage, sendConditionalJson /
// sendConditionalProtobuf, the /v1 hooks and requestCost; schema
// proto/schema/fortisnet/intelligence/v1/comparison.proto. Frozen in
// tests/corpus/token_comparison_route.json and
// token_comparison_edges.json (R53). Caller: the Flutter
// IntelligenceApi._conditionalComparisonPost sends a protobuf
// TokenComparisonRequest{tokens (at most 9), fiat:"usd"} with
// Accept: application/protobuf and its last protobuf ETag as If-None-Match.
//
// Method and path: POST /v1/tokens/compare only; no query or path parameters.
//
// Check order, each step answering before the next runs:
//  1. Body parsing, decided first but answered after step 2 (auth-first:
//     an unauthenticated request is 401 whatever its body; a refusal is
//     charged cost 1 and carries the quota headers): a body without
//     Content-Type is 415 {"error":"UNSUPPORTED_MEDIA_TYPE"} (an empty one is
//     an undefined body); application/json (any case, any parameters) must
//     be non-empty and valid JSON after one leading BOM (400
//     {"error":"BAD_REQUEST"}); `__proto__` is an ordinary key;
//     application/protobuf is bytes; anything else (text/plain, form
//     bodies, vnd.api+json, x-protobuf, octet-stream, xml) is 415.
//     Duplicate JSON keys: last wins.
//  2. preHandler: an Origin header is 403 BROWSER_ORIGIN_FORBIDDEN; then the
//     bearer (`Bearer ` exactly; trailing header whitespace is stripped by the
//     HTTP parser) must name a live device: 401 UNAUTHORIZED, 401
//     SESSION_EXPIRED, then 401 ATTESTATION_REQUIRED when attestation is
//     required (all without rate headers). Then the per-minute device quota
//     (240 development, 120 default, 600 attested, 1,200 premium) is charged
//     the request cost: the length of a JSON object body's `tokens` array
//     (raw, before validation, 300 entries cost 300), else 1 (protobuf, no
//     body, a non-array `tokens`). The counter is incremented first;
//     over the quota is 429 {"error":"RATE_LIMITED"} with retry-after: 60.
//     Every later answer carries x-rate-limit-cost and x-rate-limit-remaining.
//  3. A protobuf body is decoded with protobuf-es fromBinary (any failure:
//     truncation, a bad tag, invalid UTF-8, an end group without a start) and
//     mapped to {tokens, fiat?}; a repeated field keeps its last value.
//  4. Zod, strict: {tokens: [{network, tokenId}] (1..20), fiat?: 'usd'};
//     network and tokenId are strings trimmed (JavaScript trim) to 1..64 and
//     1..200 UTF-16 units; no other keys at either level; fiat is the exact
//     string "usd" when present (not "USD", null, "" or padded). Steps 3 and 4
//     answer one body: 400 {"error":"BAD_REQUEST","detail":"invalid comparison
//     request"}. So zero tokens and 21 tokens are 400, after the quota.
//  5. TokenComparisonService.compare: in input order, each token resolves to
//     a canonical (network id, token id) through the network registry (ids
//     and catalogued EIP-155 aliases; an unknown `eip155:<digits>` is a
//     generic EVM network whose id is the input) and parseTokenRef; a token
//     that does not resolve is an INVALID_TOKEN error, one whose canonical
//     key was already seen a DUPLICATE_TOKEN error, both echoing the input
//     index and the trimmed input strings. With no valid key nothing is read.
//  6. One read of the 'registry' and 'comparison' rows per key
//     (TokenComparisonStore::Read). A storage fault answers 500
//     {"error":"INTERNAL"} with rate headers (TypeScript throws).
//  7. Items keep the order of the valid keys. Partial data is honest:
//     a missing row or field is null; metrics from another fiat are ignored;
//     a native coin borrows the network's symbol (as symbol and name) and
//     CoinGecko id. Each metric is {value, asOf, source, status}: market
//     values are fresh 15 minutes after marketAsOf, DEX/liquidity 30 minutes
//     after liquidityAsOf (a future or missing time is never fresh); a stale
//     market value yields to a fresh DEX one; liquidity is `stale` unless its
//     status is ok, else unsupported/error/unavailable with
//     liquidityCheckedAt; `<metric>:stale` warnings; ratios rounded to 6
//     places; competition ranks (ties share, then input order).
//  8. Answer: `fiat`, `computedAt` (the clock now, ISO), `items`, `errors`,
//     cache-control: private, max-age=30. JSON unless AcceptsProtobuf (the
//     shared Accept negotiation: q=0 refuses, x-protobuf is not protobuf).
//     JSON: application/json; charset=utf-8, ETag W/"token-comparison-<24
//     hex>" over the JSON without computedAt. Protobuf: application/protobuf,
//     Vary: Accept, ETag W/"token-comparison-protobuf-<24 hex>" over the
//     message with computedAt "" (absent optional fields are not written).
//     An If-None-Match equal to the selected ETag (one exact value, not a
//     list) is a bodyless 304 with the ETag, cache-control, Vary for
//     protobuf and the rate headers. A pool count that is not a uint32 makes
//     protobuf-es throw: 500 INTERNAL with the private cache-control.
//     Every answer also carries nosniff, no-referrer and the permissions
//     policy; refusals carry cache-control: no-store.
//
// Cache and providers: the comparison rows are a read model written by the
// market warmer and the liquidity history (token_writes.h); this route never
// calls a provider, never writes, and never refreshes a row (TypeScript's
// demand observer sample is recorded through `stats` when set). "Cold" is a
// missing row (all unavailable), "stale" is an old asOf, an upstream failure
// is a liquidity `error`/`unsupported`/`not_found` status written by the
// liquidity job, kept beside any remembered value. The ETag includes the
// statuses, so a stale transition changes it even when storage did not.
//
// Persistence and concurrency: rows live in the runtime KV and survive a
// restart (TypeScript: LMDB); nothing about a request is stored. Requests
// are serialized by one route mutex over the static work buffers; a read
// sees each row as last committed.
//
// Native bounds and differences: the JSON reader's 256 KiB / 8,192 tokens /
// depth 16 bound answers 413 {"error":"BODY_TOO_LARGE"} after the device
// policy, at cost 1, and it refuses a lone surrogate escape or a number out
// of double range as invalid JSON (TypeScript reads both); stored strings are
// at most 256 bytes (the native store refuses longer writes); an answer over
// 192 KiB is 500 INTERNAL.
void handle_compare(int client, const http::HttpRequest& request, bool head, void* opaque) {
  static_cast<void>(head);
  auto* route = static_cast<TokenComparisonRouteContext*>(opaque);
  fortis::base::UniqueLock work_lock(work_mutex);
  // 1. Content-type parsing. A refusal is decided here but answered after
  // the device policy (auth-first), at cost 1.
  BodyKind kind = BodyKind::kUndefined;
  codec::BoundedJsonDocument json;
  int framing_status = 0;
  const char* framing_body = nullptr;
  if (!request.has_content_type) {
    if (request.body_length) {
      framing_status = 415;
      framing_body = "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}";
    }
  } else if (request.json_content_type) {
    const unsigned char* text = reinterpret_cast<const unsigned char*>(request.body);
    size_t text_length = request.body_length;
    // One leading byte-order mark is dropped.
    if (text_length >= 3 && text[0] == 0xef && text[1] == 0xbb && text[2] == 0xbf) {
      text += 3; text_length -= 3;
    }
    codec::BoundedJsonError error = codec::BoundedJsonError::kNone;
    if (!request.body_length) {
      framing_status = 400;
      framing_body = "{\"error\":\"BAD_REQUEST\"}";
    } else if (!json.Parse(text, text_length, work->json_tokens, codec::kBoundedJsonMaxTokens, &error)) {
      // Invalid JSON is 400; the native bound (256 KiB, 8,192 tokens, depth
      // 16) is 413.
      framing_status = error == codec::BoundedJsonError::kInvalidInput ? 400 : 413;
      framing_body = framing_status == 400 ? "{\"error\":\"BAD_REQUEST\"}" : "{\"error\":\"BODY_TOO_LARGE\"}";
    } else {
      kind = BodyKind::kJson;
    }
  } else if (request.protobuf_content_type) {
    kind = BodyKind::kProtobuf;
  } else {
    framing_status = 415;
    framing_body = "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}";
  }

  // 2. preHandler: Origin, device auth, quota charged at the request cost.
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(
      request, framing_status ? 1 : request_cost(kind, json), route->attestation_required);
  if (policy.status) {
    work_lock.Unlock();
    http::HttpResponse response{policy.status, policy.reason, "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t*>(policy.error_body ? policy.error_body : kInternal),
        strlen(policy.error_body ? policy.error_body : kInternal)};
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    WriteHttpResponse(client, response);
    return;
  }
  if (framing_status) {
    work_lock.Unlock();
    send_body(client, framing_status,
              framing_status == 415   ? "Unsupported Media Type"
              : framing_status == 413 ? "Payload Too Large"
                                      : "Bad Request",
              framing_body, policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  auto reply = [&](int status, const char* reason, const char* text,
                   const char* cache_control = nullptr) {
    send_body(client, status, reason, text, cost, remaining, cache_control);
    work_lock.Unlock();
  };

  // 3. protobuf decode and Zod.
  size_t count = 0;
  const bool valid = kind == BodyKind::kJson ? validate_json(json, &count)
      : kind == BodyKind::kProtobuf
          ? decode_protobuf(reinterpret_cast<const uint8_t*>(request.body), request.body_length,
                            &count)
          : false;
  if (!valid) { reply(400, "Bad Request", kBadRequest); return; }

  // 4. TokenComparisonService.compare: canonical keys, duplicates, one read.
  size_t key_count = 0, error_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const InputToken& input = work->inputs[i];
    networks::TokenNetwork& network = work->networks[key_count];
    bool canonical = ResolveTokenNetwork(input.network, input.network_length, &network) &&
        ParseTokenRef(&network, input.token_id, input.token_id_length, &work->parsed_ref);
    bool duplicate = false;
    if (canonical) {
      for (size_t k = 0; k < key_count && !duplicate; ++k)
        duplicate = strcmp(work->keys[k].network, network.id) == 0 &&
            strcmp(work->key_token_ids[k], work->parsed_ref.token_id) == 0;
    }
    if (!canonical || duplicate) {
      ErrorEntry& error = work->errors[error_count++];
      error = ErrorEntry{};
      error.input_index = static_cast<uint32_t>(i);
      error.network = input.network; error.network_length = input.network_length;
      error.token_id = input.token_id; error.token_id_length = input.token_id_length;
      error.duplicate = canonical;
      continue;
    }
    memcpy(work->key_token_storage[key_count], work->parsed_ref.token_id,
           strlen(work->parsed_ref.token_id) + 1);
    work->key_token_ids[key_count] = work->key_token_storage[key_count];
    work->keys[key_count].network = network.id;
    work->keys[key_count].token_id = work->key_token_ids[key_count];
    ++key_count;
  }
  const int64_t now = route_now_ms(route);
  if (key_count) {
    fortis::tokens::TokenComparisonStore store(route->kv);
    if (store.Read(work->keys, key_count, "usd", now, work->records) !=
        fortis::tokens::token_store::ReadStatus::kOk) {
      reply(500, "Internal Server Error", kInternal);
      return;
    }
  }
  // RegistryTokenComparisonIdentityResolver.metadata: a native coin borrows
  // the network's symbol (as symbol and name) and CoinGecko id.
  for (size_t k = 0; k < key_count; ++k) {
    if (strcmp(work->records[k].token_id, "native") != 0) continue;
    copy_native(&work->records[k].symbol, work->networks[k].native_symbol);
    copy_native(&work->records[k].name, work->networks[k].native_symbol);
    copy_native(&work->records[k].market_source_id, work->networks[k].native_coingecko_id);
  }
  // TokenFetchComparisonDemandObserver: telemetry never fails the read.
  if (route->stats) {
    for (size_t k = 0; k < key_count; ++k) {
      const fortis::tokens::comparison::Record& record = work->records[k];
      fortis::tokens::token_jobs::FetchSample sample{};
      sample.network = record.network;
      sample.token_id = record.token_id;
      sample.facet = fortis::tokens::token_jobs::FetchFacet::kMarket;
      sample.source_id = record.market_source_id.present ? record.market_source_id.text : nullptr;
      sample.outcome = fortis::tokens::token_jobs::FetchOutcome::kFloor;
      sample.negative = !record.market_price_usd.present && !record.market_cap_usd.present &&
                        !record.volume_24h_usd.present;
      sample.foreground = true;
      sample.at_ms = now;
      route->stats->Record(sample);
    }
  }
  fortis::tokens::comparison::CompareTokenRecords(work->records, key_count, now, work->items);
  fortis::tokens::comparison::Response result{};
  result.computed_at_ms = now;
  result.items = work->items; result.item_count = key_count;
  result.errors = work->errors; result.error_count = error_count;

  // 5. sendConditionalJson / sendConditionalProtobuf; computedAt is not
  // part of either validator.
  const bool protobuf = AcceptsProtobuf(request);
  OutBuffer body{work->response_body, sizeof(work->response_body)};
  OutBuffer validator{work->validator_body, sizeof(work->validator_body)};
  if (protobuf) {
    if (!fortis::tokens::comparison::AppendResponseProto(body, result, true) ||
        !fortis::tokens::comparison::AppendResponseProto(validator, result, false)) {
      // protobuf-es threw in toBinary after the cache-control header was set.
      reply(500, "Internal Server Error", kInternal, kCacheControl);
      return;
    }
  } else {
    fortis::tokens::comparison::AppendResponseJson(body, result, true);
    fortis::tokens::comparison::AppendResponseJson(validator, result, false);
  }
  if (!body.ok || !validator.ok) { reply(500, "Internal Server Error", kInternal); return; }
  http::HttpResponse selected{200, "OK",
      protobuf ? "application/protobuf" : "application/json; charset=utf-8",
      work->response_body, body.used};
  http::HttpResponse response{};
  if (!ConditionalStructuredResponse(request, "token-comparison", selected, selected, &response,
                                     work->validator_body, validator.used, work->validator_body,
                                     validator.used)) {
    reply(500, "Internal Server Error", kInternal);
    return;
  }
  response.cache_control = kCacheControl;
  response.rate_limit_cost = cost;
  response.rate_limit_remaining = remaining;
  WriteHttpResponse(client, response);
  work_lock.Unlock();
}

}  // namespace

bool RegisterTokenComparisonRoute(http::HttpRouter& router, TokenComparisonRouteContext* context) {
  if (!context || !context->kv) return false;
  if (!work) {
    // Registered before the HTTP workers start; kept for the process lifetime.
    work = fortis::base::New<CompareWork>();
    if (!work) return false;
  }
  return router.Add("POST", "/v1/tokens/compare", handle_compare, context);
}

}  // namespace fortis::tokens
