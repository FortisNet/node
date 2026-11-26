#include "fixed_string.h"
#include "ownership.h"
#include <memory>
#include "cross_chain_service.h"

#include "chain_transport.h"
#include "cross_chain_providers.h"
#include "cross_chain_components.h"
#include "device_enrollment.h"
#include "device_policy_step.h"
#include "discovery_route_support.h"
#include "http_middleware.h"
#include "js_json.h"
#include "proto_wire.h"
#include "price_history_batch_codec.h"
#include "threading.h"
#include "token_json.h"
#include "token_text.h"
#include "whatwg_url.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace fortis::exchange {

namespace {

namespace cc = fortis::exchange::cross_chain;
namespace tj = fortis::codec::tj;
namespace hb = fortis::codec::history_batch;
namespace jj = fortis::compat::js_json;
namespace pw = fortis::codec::proto_wire;
using tj::OutBuffer;
using tj::Value;

constexpr const char* kJsonType = http::kHttpJsonUtf8;
constexpr const char* kProtobufType = "application/protobuf";
constexpr uint32_t kHandoffTimeoutMs = 30000;
constexpr uint32_t kProviderDeadlineMs = 25000;
constexpr size_t kResponseLimit = 2 * 1024 * 1024;  // http_server.cpp kCapturedResponseLimit
constexpr size_t kRequestArenaBytes = 4 * 1024 * 1024;
constexpr size_t kTaskArenaBytes = 64 * 1024 * 1024;
constexpr size_t kMaxRoutes = 5;

uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

// ---- replies --------------------------------------------------------------------------

using Policy = auth::DeviceQuota;

http::HttpResponse json_response(int status, const char* reason, const char* body, const Policy& policy) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = policy.cost;
  response.rate_limit_remaining = policy.remaining;
  return response;
}

void send(http::HttpReply& reply, int status, const char* reason, const char* body, const Policy& policy) {
  reply.Send(json_response(status, reason, body, policy));
}

void send_bad_request(http::HttpReply& reply, const Policy& policy) {
  send(reply, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", policy);
}

// replyBackPressure(): 429 RATE_LIMITED with retry-after (seconds, >= 1).
http::HttpResponse back_pressure(uint32_t retry_after_ms, const Policy& policy) {
  http::HttpResponse response = json_response(
      429, "Too Many Requests",
      "{\"error\":\"RATE_LIMITED\",\"detail\":\"Upstream data is temporarily busy; retained data remains available.\"}",
      policy);
  const uint32_t ms = retry_after_ms ? retry_after_ms : 2000;
  const uint32_t seconds = (ms + 999) / 1000;
  response.retry_after = seconds < 1 ? 1 : seconds;
  return response;
}

// ---- request bodies (content-type parsing, after the device hook) ---------------------

enum class BodyKind : uint8_t { kUndefined, kValue, kProtobuf };
struct Body {
  BodyKind kind = BodyKind::kUndefined;
  Value* value = nullptr;
  const uint8_t* bytes = nullptr;
  size_t length = 0;
};

// The parsers in the /v1 scope: JSON and protobuf; any other media type
// (text/plain and form bodies included) is 415.
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
      body->kind = BodyKind::kUndefined;
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

// ---- the shared steps of each route, in TypeScript's order --------------------------------

// GET /providers: the device hook at cost 1.
constexpr http::HttpStep<auth::DeviceStepState> kProvidersSteps[] = {auth::RequireDevice<auth::DeviceStepState, 1>};

// POST /quotes and POST /status: the device hook at cost 10, then the body
// parser (auth-first: an unauthenticated request is 401 whatever its body).
struct PostState {
  bool attestation_required = false;
  tj::Arena arena{};
  Body body;
  auth::DevicePolicyResult device{};
};
bool ParsePostBody(const http::HttpRequest& request, http::HttpReply& reply, PostState& state) {
  const auth::DeviceQuota quota = auth::DeviceQuotaOf(state.device);
  return parse_body(request, reply, http::HttpQuota{quota.cost, quota.remaining}, state.arena, &state.body);
}
constexpr http::HttpStep<PostState> kPostSteps[] = {auth::RequireDevice<PostState, 10>, ParsePostBody};

// ---- protobuf-es fromBinary ------------------------------------------------------------
//
// protobuf-es 2.16's readMessage over one cursor (DiscoveryProtoReader): a
// nested message's length bounds only its own read loop, so its last field
// may run past it; known fields are read by their declared type whatever
// the wire type says (a string by `bytes()`, a uint32 by `uint32()`, a
// message by `uint32()` then its loop); unknown fields are skipped (varints
// of any length, groups to their end tag, 100 levels); a repeated singular
// message merges into the first; strings are strict UTF-8 with one leading
// byte-order mark dropped (TextDecoder). Any read past the body throws, and
// the route answers 400.

constexpr int kProtoSkipLimit = 100;

Value* read_string(discovery::DiscoveryProtoReader& reader, tj::Arena& arena) {
  const uint8_t* text = nullptr;
  size_t length = 0;
  if (!reader.String(&text, &length)) return nullptr;
  return tj::Str(arena, reinterpret_cast<const char*>(text), length);
}

struct ProtoEndpoint {
  bool present = false;
  Value* chain_id = nullptr;
  Value* token = nullptr;
  uint32_t decimals = 0;
  Value* address = nullptr;
};

// CrossChainEndpoint {chain_id = 1, token = 2, decimals = 3, address = 4}.
bool decode_endpoint(discovery::DiscoveryProtoReader& reader, tj::Arena& arena, ProtoEndpoint* out) {
  uint32_t length = 0;
  if (!reader.MessageLength(&length)) return false;
  const size_t end = reader.pos() + length;
  discovery::DiscoveryProtoReader::Frame frame(reader, end);
  if (!frame.ok()) return false;
  if (!out->present) {
    out->present = true;
    out->chain_id = tj::Str(arena, "");
    out->token = tj::Str(arena, "");
    out->address = tj::Str(arena, "");
    if (!out->chain_id || !out->token || !out->address) return false;
  }
  while (reader.pos() < end) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return false;
    if (field == 1 || field == 2 || field == 4) {
      Value* text = read_string(reader, arena);
      if (!text) return false;
      (field == 1 ? out->chain_id : field == 2 ? out->token : out->address) = text;
    } else if (field == 3) {
      if (!reader.Uint32(&out->decimals)) return false;
    } else if (!reader.Skip(wire, field, kProtoSkipLimit)) {
      return false;
    }
  }
  return true;
}

Value* endpoint_object(tj::Arena& arena, const ProtoEndpoint& e) {
  Value* object = tj::Obj(arena);
  if (!object || !tj::Set(arena, object, "chainId", e.chain_id) || !tj::Set(arena, object, "token", e.token) ||
      !tj::Set(arena, object, "decimals", tj::Num(arena, e.decimals)) ||
      !tj::Set(arena, object, "address", e.address))
    return nullptr;
  return object;
}

}  // namespace

// fromBinary(CrossChainQuoteRequestSchema) mapped to the route's input
// object: {from?, to?, amountIn, slippageBps? (when present), prepare,
// providers? (when non-empty)}.
Value* CrossChainDecodeQuoteRequest(const uint8_t* body, size_t length, tj::Arena& arena) {
  ProtoEndpoint from, to;
  Value* amount = tj::Str(arena, "");
  bool has_slippage = false, prepare = false;
  uint32_t slippage = 0;
  Value* providers = tj::Arr(arena);
  if (!amount || !providers) return nullptr;
  discovery::DiscoveryProtoReader reader(body, length);
  while (reader.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return nullptr;
    if (field == 1 || field == 2) {
      if (!decode_endpoint(reader, arena, field == 1 ? &from : &to)) return nullptr;
    } else if (field == 3) {
      amount = read_string(reader, arena);
      if (!amount) return nullptr;
    } else if (field == 4) {
      if (!reader.Uint32(&slippage)) return nullptr;
      has_slippage = true;
    } else if (field == 5) {
      if (!reader.Bool(&prepare)) return nullptr;
    } else if (field == 6) {
      Value* text = read_string(reader, arena);
      if (!text || !tj::Push(arena, providers, text)) return nullptr;
    } else if (!reader.Skip(wire, field, kProtoSkipLimit)) {
      return nullptr;
    }
  }
  Value* input = tj::Obj(arena);
  if (!input) return nullptr;
  if (from.present) {
    Value* object = endpoint_object(arena, from);
    if (!object || !tj::Set(arena, input, "from", object)) return nullptr;
  }
  if (to.present) {
    Value* object = endpoint_object(arena, to);
    if (!object || !tj::Set(arena, input, "to", object)) return nullptr;
  }
  if (!tj::Set(arena, input, "amountIn", amount)) return nullptr;
  if (has_slippage && !tj::Set(arena, input, "slippageBps", tj::Num(arena, slippage))) return nullptr;
  if (!tj::Set(arena, input, "prepare", tj::Bool(arena, prepare))) return nullptr;
  if (providers->count && !tj::Set(arena, input, "providers", providers)) return nullptr;
  return input;
}

// fromBinary(CrossChainStatusRequestSchema) mapped to the route's input
// object: the four implicit strings always, each optional field when present.
Value* CrossChainDecodeStatusRequest(const uint8_t* body, size_t length, tj::Arena& arena) {
  static const char* kNames[] = {"", "providerId", "txHash", "fromChainId", "toChainId", "toToken", "toSymbol",
                                 "toDecimals", "providerQuoteId", "depositMemo", "bridgeId", "bridge"};
  Value* values[12] = {};
  bool has_decimals = false;
  uint32_t decimals = 0;
  for (int i = 1; i <= 4; ++i) {
    values[i] = tj::Str(arena, "");
    if (!values[i]) return nullptr;
  }
  discovery::DiscoveryProtoReader reader(body, length);
  while (reader.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return nullptr;
    if (field == 7) {
      if (!reader.Uint32(&decimals)) return nullptr;
      has_decimals = true;
    } else if (field >= 1 && field <= 11) {
      values[field] = read_string(reader, arena);
      if (!values[field]) return nullptr;
    } else if (!reader.Skip(wire, field, kProtoSkipLimit)) {
      return nullptr;
    }
  }
  Value* input = tj::Obj(arena);
  if (!input) return nullptr;
  for (int i = 1; i <= 11; ++i) {
    if (i == 7) {
      if (has_decimals && !tj::Set(arena, input, kNames[i], tj::Num(arena, decimals))) return nullptr;
      continue;
    }
    if (values[i] && !tj::Set(arena, input, kNames[i], values[i])) return nullptr;
  }
  return input;
}

namespace {

// ---- zod schemas -----------------------------------------------------------------------

// zod .strict(): every own key is a schema key.
bool only_keys(const Value* object, const char* const* keys, size_t count) {
  for (uint32_t i = 0; i < object->count; ++i) {
    bool known = false;
    for (size_t k = 0; k < count && !known; ++k)
      known = object->members[i].key_length == strlen(keys[k]) &&
              memcmp(object->members[i].key, keys[k], object->members[i].key_length) == 0;
    if (!known) return false;
  }
  return true;
}

// z.string().trim(): the trimmed copy (nullptr when not a string).
Value* trimmed(tj::Arena& arena, const Value* v) {
  if (!tj::IsString(v)) return nullptr;
  size_t begin = 0, end = 0;
  fortis::compat::token_text::Trim(v->text, v->length, &begin, &end);
  return tj::Str(arena, v->text + begin, end - begin);
}

// plainText: trim, 1..256 UTF-16 units, no C0 control or DEL.
Value* plain_text(tj::Arena& arena, const Value* v) {
  Value* text = trimmed(arena, v);
  if (!text) return nullptr;
  const size_t units = cc::Units(text);
  if (units < 1 || units > 256) return nullptr;
  for (uint32_t i = 0; i < text->length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text->text[i]);
    if (c < 0x20 || c == 0x7f) return nullptr;
  }
  return text;
}

// chainId: trim, /^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$/.
Value* chain_id(tj::Arena& arena, const Value* v) {
  Value* text = trimmed(arena, v);
  if (!text || text->length < 1 || text->length > 64) return nullptr;
  for (uint32_t i = 0; i < text->length; ++i) {
    const char c = text->text[i];
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == ':' || c == '-')) return nullptr;
  }
  return text;
}

// /^[a-z][a-z0-9-]{0,31}$/
bool provider_id(const Value* v) {
  if (!tj::IsString(v) || v->length < 1 || v->length > 32) return false;
  for (uint32_t i = 0; i < v->length; ++i) {
    const char c = v->text[i];
    const bool ok = (c >= 'a' && c <= 'z') || (i > 0 && ((c >= '0' && c <= '9') || c == '-'));
    if (!ok) return false;
  }
  return true;
}

// z.number().int().min(lo).max(hi).
bool integer_in(const Value* v, double lo, double hi, uint32_t* out) {
  if (!tj::IsNumber(v) || !isfinite(v->number) || trunc(v->number) != v->number || v->number < lo ||
      v->number > hi)
    return false;
  *out = static_cast<uint32_t>(v->number);
  return true;
}

bool parse_endpoint(tj::Arena& arena, const Value* v, cc::Endpoint* out) {
  static const char* const kKeys[] = {"chainId", "token", "decimals", "address"};
  if (!tj::IsObject(v) || !only_keys(v, kKeys, 4)) return false;
  out->chain_id = chain_id(arena, tj::Get(v, "chainId"));
  out->token = plain_text(arena, tj::Get(v, "token"));
  out->address = plain_text(arena, tj::Get(v, "address"));
  return out->chain_id && out->token && out->address && integer_in(tj::Get(v, "decimals"), 0, 36, &out->decimals);
}

bool ascii_lower_is(const Value* v, const char* text) {
  const size_t length = strlen(text);
  if (!v || v->length != length) return false;
  for (size_t i = 0; i < length; ++i) {
    char c = v->text[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != text[i]) return false;
  }
  return true;
}

struct QuoteInput {
  cc::QuoteRequest request;
  const Value* providers[10] = {};
  size_t provider_count = 0;
  bool has_providers = false;
};

// quoteBody.safeParse(input).
bool parse_quote(tj::Arena& arena, const Value* input, QuoteInput* out) {
  static const char* const kKeys[] = {"from", "to", "amountIn", "slippageBps", "prepare", "providers"};
  if (!tj::IsObject(input) || !only_keys(input, kKeys, 6)) return false;
  cc::QuoteRequest& q = out->request;
  if (!parse_endpoint(arena, tj::Get(input, "from"), &q.from) || !parse_endpoint(arena, tj::Get(input, "to"), &q.to))
    return false;
  const Value* amount = tj::Get(input, "amountIn");
  if (!tj::IsString(amount) || !cc::PositiveIntegerText(amount->text, amount->length)) return false;
  q.amount_in = amount;
  const Value* slippage = tj::Get(input, "slippageBps");
  q.slippage_bps = 50;
  if (slippage && !integer_in(slippage, 0, 5000, &q.slippage_bps)) return false;
  const Value* prepare = tj::Get(input, "prepare");
  q.prepare = false;
  if (prepare) {
    if (!tj::IsBool(prepare)) return false;
    q.prepare = prepare->type == tj::Type::kTrue;
  }
  const Value* providers = tj::Get(input, "providers");
  if (providers) {
    if (!tj::IsArray(providers) || providers->count < 1 || providers->count > 10) return false;
    for (uint32_t i = 0; i < providers->count; ++i) {
      if (!provider_id(providers->items[i])) return false;
      out->providers[i] = providers->items[i];
    }
    out->provider_count = providers->count;
    out->has_providers = true;
  }
  // The refinements, in order.
  if (cc::SameText(q.from.chain_id, q.to.chain_id)) return false;
  if (cc::Is(q.from.chain_id, "bitcoin-cash") || cc::Is(q.from.chain_id, "zcash")) return false;
  if (ascii_lower_is(q.from.chain_id, "ton")) return false;
  if (ascii_lower_is(q.to.chain_id, "ton") && !(cc::Is(q.to.chain_id, "ton") && cc::ValidTonFriendlyAddress(q.to.address)))
    return false;
  const cc::Endpoint* endpoints[] = {&q.from, &q.to};
  for (const cc::Endpoint* e : endpoints)
    if (ascii_lower_is(e->chain_id, "monero") &&
        !(cc::Is(e->chain_id, "monero") && cc::Is(e->token, "monero") && e->decimals == 12 &&
          cc::ValidMoneroAddress(e->address)))
      return false;
  if (q.prepare && !(out->has_providers && out->provider_count == 1)) return false;
  return true;
}

// statusBody.safeParse(input), with its transform.
bool parse_status(tj::Arena& arena, const Value* input, cc::StatusRequest* out) {
  static const char* const kKeys[] = {"providerId", "txHash", "fromChainId", "toChainId", "toToken", "toSymbol",
                                      "toDecimals", "providerQuoteId", "depositMemo", "bridgeId", "bridge"};
  if (!tj::IsObject(input) || !only_keys(input, kKeys, 11)) return false;
  const Value* provider = tj::Get(input, "providerId");
  if (!provider_id(provider)) return false;
  out->provider_id = provider;
  out->tx_hash = plain_text(arena, tj::Get(input, "txHash"));
  out->from_chain_id = chain_id(arena, tj::Get(input, "fromChainId"));
  out->to_chain_id = chain_id(arena, tj::Get(input, "toChainId"));
  if (!out->tx_hash || !out->from_chain_id || !out->to_chain_id) return false;
  auto optional_text = [&](const char* key, const Value** target) {
    const Value* v = tj::Get(input, key);
    if (!v) return true;
    *target = plain_text(arena, v);
    return *target != nullptr;
  };
  const Value* bridge = nullptr;
  if (!optional_text("toToken", &out->to_token) || !optional_text("providerQuoteId", &out->provider_quote_id) ||
      !optional_text("depositMemo", &out->deposit_memo) || !optional_text("bridgeId", &out->bridge_id) ||
      !optional_text("bridge", &bridge))
    return false;
  const Value* symbol = tj::Get(input, "toSymbol");
  if (symbol) {
    Value* text = trimmed(arena, symbol);
    if (!text || text->length < 1 || text->length > 32) return false;
    for (uint32_t i = 0; i < text->length; ++i) {
      const char c = text->text[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
            c == '+' || c == '-'))
        return false;
    }
    out->to_symbol = text;
  }
  const Value* decimals = tj::Get(input, "toDecimals");
  if (decimals) {
    if (!integer_in(decimals, 0, 36, &out->to_decimals)) return false;
    out->has_to_decimals = true;
  }
  // bridgeId ?? (providerId === 'squid' ? bridge : undefined).
  if (!out->bridge_id && cc::Is(provider, "squid")) out->bridge_id = bridge;
  return true;
}

// ---- protobuf-es toBinary ------------------------------------------------------------------

// Field writers over tj values. A lone surrogate in a string is written as
// U+FFFD (TextEncoder); a uint32 outside its range fails the encoding, as
// protobuf-es throws.
void put_string(OutBuffer& out, uint32_t field, const Value* v) {
  pw::PutBytes(out, field, v->text, v->length);
}

// Implicit proto3 string: written when it is a non-empty string.
void implicit_string(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (tj::IsString(v) && v->length) put_string(out, field, v);
}

// `...(x !== null ? {x} : {})` / `x != null`: written when it is a string.
void optional_string(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (tj::IsString(v)) put_string(out, field, v);
}

bool uint32_value(const Value* v, uint32_t* out) {
  if (!tj::IsNumber(v) || !isfinite(v->number) || trunc(v->number) != v->number || v->number < 0 ||
      v->number > 4294967295.0)
    return false;
  *out = static_cast<uint32_t>(v->number);
  return true;
}

bool implicit_uint32(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (!tj::IsNumber(v) || v->number == 0) return true;
  uint32_t value = 0;
  if (!uint32_value(v, &value)) return false;
  pw::PutTag(out, field, 0);
  pw::PutVarint(out, value);
  return true;
}

bool optional_uint32(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (!tj::IsNumber(v)) return true;
  uint32_t value = 0;
  if (!uint32_value(v, &value)) return false;
  pw::PutTag(out, field, 0);
  pw::PutVarint(out, value);
  return true;
}

void implicit_bool(OutBuffer& out, uint32_t field, bool value) {
  if (!value) return;
  pw::PutTag(out, field, 0);
  out.Byte(1);
}

using Encoder = bool (*)(OutBuffer& out, const Value* v);

bool embed(OutBuffer& out, uint32_t field, Encoder encode, const Value* v) {
  OutBuffer measure{nullptr, 0, 0, true};
  if (!encode(measure, v)) return false;
  pw::PutTag(out, field, 2);
  pw::PutVarint(out, measure.used);
  return encode(out, v);
}

bool encode_step(OutBuffer& out, const Value* s) {
  implicit_string(out, 1, s, "kind");
  implicit_string(out, 2, s, "tool");
  optional_string(out, 3, s, "fromChainId");
  optional_string(out, 4, s, "toChainId");
  optional_string(out, 5, s, "fromToken");
  optional_string(out, 6, s, "toToken");
  return true;
}

bool encode_execution(OutBuffer& out, const Value* e) {
  implicit_string(out, 1, e, "kind");
  optional_string(out, 2, e, "to");
  optional_string(out, 3, e, "memo");
  optional_string(out, 4, e, "data");
  optional_string(out, 5, e, "value");
  optional_string(out, 6, e, "gasLimit");
  optional_string(out, 7, e, "approvalAddress");
  return true;
}

bool encode_route(OutBuffer& out, const Value* r) {
  implicit_string(out, 1, r, "id");
  implicit_string(out, 2, r, "providerId");
  implicit_string(out, 3, r, "providerName");
  implicit_string(out, 4, r, "providerUrl");
  optional_string(out, 5, r, "providerQuoteId");
  optional_string(out, 6, r, "bridgeId");
  implicit_string(out, 7, r, "bridge");
  optional_string(out, 8, r, "bridgeUrl");
  implicit_string(out, 9, r, "riskKey");
  implicit_string(out, 10, r, "custody");
  optional_string(out, 11, r, "outputProtection");
  implicit_string(out, 12, r, "amountIn");
  implicit_string(out, 13, r, "amountOut");
  implicit_string(out, 14, r, "amountOutMin");
  if (!implicit_uint32(out, 15, r, "decimalsIn") || !implicit_uint32(out, 16, r, "decimalsOut")) return false;
  const Value* fee = tj::Get(r, "feeUsd");
  if (tj::IsNumber(fee)) pw::PutDouble(out, 17, fee->number);
  optional_string(out, 18, r, "sourceNetworkFee");
  if (!optional_uint32(out, 19, r, "etaSeconds")) return false;
  implicit_string(out, 20, r, "quotedAt");
  implicit_string(out, 21, r, "expiresAt");
  const Value* steps = tj::Get(r, "steps");
  if (tj::IsArray(steps))
    for (uint32_t i = 0; i < steps->count; ++i)
      if (!embed(out, 22, encode_step, steps->items[i])) return false;
  const Value* execution = tj::Get(r, "execution");
  if (tj::IsObject(execution) && !embed(out, 23, encode_execution, execution)) return false;
  return implicit_uint32(out, 24, r, "rank");
}

bool encode_attempt(OutBuffer& out, const Value* a) {
  implicit_string(out, 1, a, "providerId");
  implicit_string(out, 2, a, "status");
  return implicit_uint32(out, 3, a, "routeCount");
}

bool encode_quote(OutBuffer& out, const Value* q) {
  const Value* routes = tj::Get(q, "routes");
  for (uint32_t i = 0; i < routes->count; ++i)
    if (!embed(out, 1, encode_route, routes->items[i])) return false;
  optional_string(out, 2, q, "bestRouteId");
  const Value* attempts = tj::Get(q, "attempts");
  for (uint32_t i = 0; i < attempts->count; ++i)
    if (!embed(out, 3, encode_attempt, attempts->items[i])) return false;
  pw::PutTag(out, 4, 2);  // CacheInfo { hit: false }
  out.Byte(0);
  return true;
}

bool encode_received(OutBuffer& out, const Value* r) {
  optional_string(out, 1, r, "chainId");
  implicit_string(out, 2, r, "amount");
  optional_string(out, 3, r, "tokenAddress");
  optional_string(out, 4, r, "symbol");
  return optional_uint32(out, 5, r, "decimals");
}

bool encode_progress(OutBuffer& out, const Value* s) {
  implicit_string(out, 1, s, "id");
  implicit_string(out, 2, s, "kind");
  implicit_string(out, 3, s, "label");
  implicit_string(out, 4, s, "state");
  optional_string(out, 5, s, "chainId");
  optional_string(out, 6, s, "txHash");
  optional_string(out, 7, s, "explorerUrl");
  optional_string(out, 8, s, "startedAt");
  optional_string(out, 9, s, "completedAt");
  if (!optional_uint32(out, 10, s, "durationSeconds")) return false;
  optional_string(out, 11, s, "timingSource");
  optional_string(out, 12, s, "detail");
  optional_string(out, 13, s, "errorCode");
  optional_string(out, 14, s, "errorMessage");
  return true;
}

bool encode_recovery(OutBuffer& out, const Value* r) {
  implicit_string(out, 1, r, "kind");
  implicit_string(out, 2, r, "title");
  implicit_string(out, 3, r, "description");
  optional_string(out, 4, r, "actionLabel");
  optional_string(out, 5, r, "actionUrl");
  return true;
}

bool encode_struct(OutBuffer& out, const Value* object) { return tj::AppendProtoStruct(object, out); }

bool encode_status(OutBuffer& out, const Value* s) {
  implicit_string(out, 1, s, "providerId");
  implicit_string(out, 2, s, "state");
  optional_string(out, 3, s, "substatus");
  optional_string(out, 4, s, "message");
  implicit_string(out, 5, s, "sourceHash");
  optional_string(out, 6, s, "destinationHash");
  optional_string(out, 7, s, "providerExplorerUrl");
  optional_string(out, 8, s, "bridgeExplorerUrl");
  if (!optional_uint32(out, 9, s, "elapsedSeconds")) return false;
  const Value* received = tj::Get(s, "actualReceived");
  if (tj::IsObject(received) && !embed(out, 10, encode_received, received)) return false;
  const Value* progress = tj::Get(s, "progress");
  if (tj::IsArray(progress))
    for (uint32_t i = 0; i < progress->count; ++i)
      if (!embed(out, 11, encode_progress, progress->items[i])) return false;
  const Value* recovery = tj::Get(s, "recovery");
  if (tj::IsObject(recovery) && !embed(out, 12, encode_recovery, recovery)) return false;
  const Value* data = tj::Get(s, "providerData");
  if (tj::IsObject(data) && !embed(out, 13, encode_struct, data)) return false;
  return true;
}

// ---- route ranking -------------------------------------------------------------------------

// compareRoutes(): protected minimum, then expected output (both descending),
// then fee and ETA (ascending, unknown last). Array.prototype.sort is stable.
int compare_routes(const Value* a, const Value* b) {
  const char* min_a = tj::Get(a, "amountOutMin")->text;
  const char* min_b = tj::Get(b, "amountOutMin")->text;
  const int by_minimum = cc::CompareDecimal(min_b, min_a);
  if (by_minimum) return by_minimum;
  const int by_expected = cc::CompareDecimal(tj::Get(b, "amountOut")->text, tj::Get(a, "amountOut")->text);
  if (by_expected) return by_expected;
  const Value* fa = tj::Get(a, "feeUsd");
  const Value* fb = tj::Get(b, "feeUsd");
  const double fee_a = tj::IsNumber(fa) ? fa->number : INFINITY;
  const double fee_b = tj::IsNumber(fb) ? fb->number : INFINITY;
  if (fee_a != fee_b) {
    const double d = fee_a - fee_b;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
  }
  const Value* ea = tj::Get(a, "etaSeconds");
  const Value* eb = tj::Get(b, "etaSeconds");
  const double eta_a = tj::IsNumber(ea) ? ea->number : 9007199254740991.0;
  const double eta_b = tj::IsNumber(eb) ? eb->number : 9007199254740991.0;
  const double d = eta_a - eta_b;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}

}  // namespace

// ---- route plans (the handlers' pure steps; tests/cross_chain_post_test.cpp) -------------

const Value* CrossChainRequestInput(const uint8_t* bytes, size_t length, tj::Arena& arena) {
  return tj::Parse(arena, bytes, length);
}

void CrossChainPlanQuote(const Value* input, tj::Arena& arena, CrossChainQuotePlan* plan) {
  *plan = CrossChainQuotePlan{};
  QuoteInput parsed;
  if (!input || !parse_quote(arena, input, &parsed)) return;
  plan->request = parsed.request;
  // CrossChainRouter.quote: [...new Set(requested)] (or every registered
  // provider, in registration order), then UNKNOWN_CROSS_CHAIN_PROVIDER
  // listing the unregistered ids in request order.
  if (parsed.has_providers) {
    const Value* unique[10];
    size_t unique_count = 0;
    for (size_t i = 0; i < parsed.provider_count; ++i) {
      bool seen = false;
      for (size_t k = 0; k < unique_count && !seen; ++k) seen = cc::SameText(unique[k], parsed.providers[i]);
      if (!seen) unique[unique_count++] = parsed.providers[i];
    }
    for (size_t i = 0; i < unique_count; ++i) {
      const int index = cc::ProviderIndex(unique[i]->text, unique[i]->length);
      if (index < 0) plan->unknown[plan->unknown_count++] = unique[i];
      else plan->providers[plan->provider_count++] = static_cast<uint8_t>(index);
    }
  } else {
    for (size_t i = 0; i < cc::kProviderCount; ++i) plan->providers[plan->provider_count++] = static_cast<uint8_t>(i);
  }
  plan->kind = plan->unknown_count ? CrossChainPlanKind::kUnknownProviders : CrossChainPlanKind::kDispatch;
}

void CrossChainPlanStatus(const Value* input, tj::Arena& arena, CrossChainStatusPlan* plan) {
  *plan = CrossChainStatusPlan{};
  if (!input || !parse_status(arena, input, &plan->request)) return;
  plan->provider = cc::ProviderIndex(plan->request.provider_id->text, plan->request.provider_id->length);
  plan->kind = plan->provider < 0 ? CrossChainPlanKind::kUnknownProviders : CrossChainPlanKind::kDispatch;
}

bool CrossChainEncodeQuoteResponse(const Value* result, OutBuffer& out) { return encode_quote(out, result); }
bool CrossChainEncodeStatusResponse(const Value* result, OutBuffer& out) { return encode_status(out, result); }

}  // namespace fortis::exchange

// ---- the provider directory (GET /v1/exchange/cross-chain/providers) --------------------

namespace fortis::exchange::cross_chain {

bool LoadProviderSettings(const CrossChainConfig& config, ProviderSettings* out) {
  if (!out) return false;
  ProviderSettings& settings = *out;
  settings = ProviderSettings{};
  auto copy = [](char* target, size_t capacity, const char* source) {
    snprintf(target, capacity, "%s", source);
  };
  settings.lifi_configured = config.lifi_api_key_configured;
  copy(settings.lifi_api_key, sizeof(settings.lifi_api_key), config.lifi_api_key);
  settings.squid_configured = config.squid_integrator_id_configured;
  copy(settings.squid_integrator_id, sizeof(settings.squid_integrator_id), config.squid_integrator_id);
  settings.simpleswap_configured = config.simpleswap_api_key_configured;
  copy(settings.simpleswap_api_key, sizeof(settings.simpleswap_api_key), config.simpleswap_api_key);
  settings.near_jwt_configured = config.near_intents_jwt_configured;
  snprintf(settings.near_authorization, sizeof(settings.near_authorization), "Bearer %s", config.near_intents_jwt);
  settings.maya_execution_enabled = config.maya_dash_execution_enabled;
  // zod url() on every entry (loadConfig refuses the process otherwise), then
  // apiRoots(): trailing slashes removed, the first root of each WHATWG
  // origin kept. NativeConfig already holds only trimmed https:// entries.
  auto scratch_storage = base::AllocateStorage<fortis::compat::url::UrlScratch>(sizeof(fortis::compat::url::UrlScratch));
  auto* scratch = scratch_storage.get();
  auto parsed_storage = base::AllocateStorage<fortis::compat::url::Url>(sizeof(fortis::compat::url::Url));
  auto* parsed = parsed_storage.get();
  char origins[kMaxMayaRoots][512]{};
  bool ok = scratch && parsed && config.maya_node_url_count <= kMaxMayaRoots;
  for (size_t i = 0; ok && i < config.maya_node_url_count; ++i) {
    char root[512];
    snprintf(root, sizeof(root), "%s", config.maya_node_urls[i]);
    size_t length = strlen(root);
    ok = fortis::compat::url::Parse(root, length, nullptr, parsed, scratch);
    while (length && root[length - 1] == '/') root[--length] = '\0';
    if (!ok || !length || !fortis::compat::url::Parse(root, length, nullptr, parsed, scratch)) continue;
    const fortis::compat::url::View protocol = fortis::compat::url::Protocol(*parsed);
    const fortis::compat::url::View host = fortis::compat::url::Host(*parsed);
    const fortis::compat::url::View path = fortis::compat::url::Pathname(*parsed);
    if (protocol.length != 6 || memcmp(protocol.data, "https:", 6) != 0) continue;
    char origin[512];
    snprintf(origin, sizeof(origin), "https://%.*s", static_cast<int>(host.length), host.data);
    bool seen = false;
    for (size_t k = 0; k < settings.maya_root_count && !seen; ++k) seen = strcmp(origins[k], origin) == 0;
    if (seen) continue;
    const bool root_path = path.length == 1 && path.data[0] == '/';
    base::ConcatCString(settings.maya_roots[settings.maya_root_count], sizeof(settings.maya_roots[0]),
                        {origin, std::string_view(path.data, root_path ? 0 : path.length)});
    snprintf(origins[settings.maya_root_count], sizeof(origins[0]), "%s", origin);
    ++settings.maya_root_count;
  }
  scratch_storage.reset();
  parsed_storage.reset();
  // loadConfig's two-independent-origins rule, on WHATWG origins: aliases
  // the textual check in NativeConfig cannot see (0x7f.0.0.1, %61, :0443)
  // refuse the process as TypeScript does, never a silently disabled MAYA.
  if (ok && settings.maya_execution_enabled && settings.maya_root_count < 2) ok = false;
  return ok;
}

bool EncodeProviderDirectory(const ProviderSettings& settings, char* json, size_t json_capacity,
                             size_t* json_length, uint8_t* wire, size_t wire_capacity, size_t* wire_length) {
  if (!json || !wire || !json_length || !wire_length) return false;
  // CrossChainRouter.providers(): sorted by id.
  size_t sorted[kProviderCount];
  ProvidersById(sorted);
  OutBuffer text{reinterpret_cast<uint8_t*>(json), json_capacity, 0, true};
  OutBuffer proto{wire, wire_capacity, 0, true};
  text.Text("{\"providers\":[");
  for (size_t n = 0; n < kProviderCount; ++n) {
    const Descriptor d = Describe(settings, sorted[n]);
    char item[512];
    const int written =
        snprintf(item, sizeof(item),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"websiteUrl\":\"%s\",\"configured\":%s,\"custody\":\"%s\","
                 "\"capabilities\":{\"quote\":%s,\"execution\":%s,\"status\":%s}}",
                 n ? "," : "", d.id, d.name, d.website_url, d.configured ? "true" : "false", d.custody,
                 d.quote ? "true" : "false", d.execution ? "true" : "false", d.status ? "true" : "false");
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(item)) return false;
    text.Text(item);
    // CrossChainProvider: proto3 implicit presence, so false booleans are
    // absent; capabilities (6) is always present, possibly empty.
    uint8_t body[512];
    OutBuffer message{body, sizeof(body), 0, true};
    pw::PutBytes(message, 1, d.id, strlen(d.id));
    pw::PutBytes(message, 2, d.name, strlen(d.name));
    pw::PutBytes(message, 3, d.website_url, strlen(d.website_url));
    implicit_bool(message, 4, d.configured);
    pw::PutBytes(message, 5, d.custody, strlen(d.custody));
    uint8_t caps[16];
    OutBuffer capabilities{caps, sizeof(caps), 0, true};
    implicit_bool(capabilities, 1, d.quote);
    implicit_bool(capabilities, 2, d.execution);
    implicit_bool(capabilities, 3, d.status);
    pw::PutBytes(message, 6, caps, capabilities.used);
    if (!message.ok || !capabilities.ok) return false;
    pw::PutBytes(proto, 1, body, message.used);
  }
  text.Text("],\"cache\":{\"hit\":false,\"timestamp\":null}}");
  // CacheInfo{hit: false}: present and empty (no timestamp, no partial).
  pw::PutTag(proto, 2, 2);
  proto.Byte(0);
  if (!text.ok || !proto.ok) return false;
  *json_length = text.used;
  *wire_length = proto.used;
  return true;
}

}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain {
bool QuoteAggregator::Aggregate(Arena& arena, const ProviderSettings& settings,
    const QuoteInputs& inputs, Value** output) const {
    Value* routes = tj::Arr(arena);
    Value* attempts = tj::Arr(arena);
    bool ok = routes && attempts;
    for (size_t i = 0; ok && i < inputs.count; ++i) {
      const cc::Descriptor d = registry_.Describe(settings, inputs.providers[i]);
      const char* status = "not-configured";
      size_t route_count = 0;
      if (inputs.configured[i]) {
        const Value* list = inputs.routes[i];
        if (inputs.outcomes[i].failure != cc::Failure::kNone || !tj::IsArray(list)) {
          status = "failed";
        } else {
          route_count = list->count;
          status = route_count ? "quoted" : "no-route";
          for (uint32_t k = 0; ok && k < list->count; ++k) ok = tj::Push(arena, routes, list->items[k]);
        }
      }
      Value* attempt = tj::Obj(arena);
      ok = ok && attempt && tj::Set(arena, attempt, "providerId", tj::Str(arena, d.id)) &&
           tj::Set(arena, attempt, "status", tj::Str(arena, status)) &&
           tj::Set(arena, attempt, "routeCount", tj::Num(arena, static_cast<double>(route_count))) &&
           tj::Push(arena, attempts, attempt);
    }
    // Stable insertion sort by compareRoutes, then the first five ranked.
    if (ok)
      for (uint32_t i = 1; i < routes->count; ++i) {
        Value* item = routes->items[i];
        uint32_t k = i;
        while (k > 0 && compare_routes(routes->items[k - 1], item) > 0) {
          routes->items[k] = routes->items[k - 1];
          --k;
        }
        routes->items[k] = item;
      }
    Value* ranked = tj::Arr(arena);
    ok = ok && ranked;
    for (uint32_t i = 0; ok && i < routes->count && i < kMaxRoutes; ++i) {
      Value* route = routes->items[i];
      ok = tj::Set(arena, route, "rank", tj::Num(arena, i + 1)) && tj::Push(arena, ranked, route);
    }
    Value* result = tj::Obj(arena);
    Value* cache = tj::Obj(arena);
    ok = ok && result && cache && tj::Set(arena, result, "routes", ranked) &&
         tj::Set(arena, result, "bestRouteId",
                 ranked->count ? tj::Get(ranked->items[0], "id") : tj::Null(arena)) &&
         tj::Set(arena, result, "attempts", attempts) && tj::Set(arena, cache, "hit", tj::Bool(arena, false)) &&
         tj::Set(arena, cache, "timestamp", tj::Null(arena)) && tj::Set(arena, result, "cache", cache) && arena.ok;
    *output = result;
    return ok;
}
}  // namespace fortis::exchange::cross_chain

// ---- the service ------------------------------------------------------------------------

namespace fortis::exchange {

struct CrossChainService::Impl {
  struct Job {
    bool used = false;
    bool status = false;
    bool protobuf = false;
    bool head = false;
    uint64_t token = 0;
    uint64_t deadline = 0;
    Policy policy;
    tj::Arena arena{};
    cc::QuoteRequest quote;
    cc::StatusRequest status_request;
    size_t count = 0;            // selected providers
    uint8_t providers[cc::kProviderCount]{};
    bool configured[cc::kProviderCount]{};
    tj::Arena task_arena[cc::kProviderCount]{};
    cc::Outcome outcome[cc::kProviderCount]{};
    Value* result[cc::kProviderCount]{};
    size_t pending = 0;
  };
  struct Task {
    Job* job = nullptr;
    size_t slot = 0;
  };

  CrossChainConfig config{};
  cc::ProviderSettings settings{};
  CrossChainServiceSeams seams{};
  std::unique_ptr<fortis::chains::Transport, fortis::base::ObjectDeleter<fortis::chains::Transport>> transport;
  cc::CrossChainQuoteService quotes;
  cc::DepositTracker deposits;
  http::HttpResponseHandoff* handoff = nullptr;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::ConditionVariable idle;
  fortis::base::Thread workers[kWorkers];
  size_t started_workers = 0;
  bool stopping = false;
  Job jobs[kJobs]{};
  Task queue[kJobs * cc::kProviderCount]{};
  size_t queue_head = 0, queue_count = 0;
  char providers_json[cc::kDirectoryJsonBytes]{};
  size_t providers_json_length = 0;
  uint8_t providers_proto[cc::kDirectoryWireBytes]{};
  size_t providers_proto_length = 0;

  // The seam clock, else the system clock (a normal composition).
  fortis::base::Clock* Clock() const { return seams.clock ? seams.clock : &fortis::base::SystemClock(); }
  int64_t Now() const { return Clock()->NowMs(); }

  // ---- configuration ----

  bool LoadSettings() { return cc::LoadProviderSettings(config, &settings); }

  // GET /providers: static per process (descriptors read configuration only).
  bool BuildProviders() {
    return cc::EncodeProviderDirectory(settings, providers_json, sizeof(providers_json), &providers_json_length,
                                       providers_proto, sizeof(providers_proto), &providers_proto_length);
  }

  // ---- handlers ----


  void Providers(const http::HttpRequest& request, http::HttpReply& reply) {
    auth::DeviceStepState state;
    state.attestation_required = config.attestation_required;
    if (!RunHttpSteps(kProvidersSteps, request, reply, state)) return;
    const auth::DevicePolicyResult& authorized = state.device;
    const bool protobuf = AcceptsProtobuf(request);
    http::HttpResponse response{200, "OK", protobuf ? kProtobufType : kJsonType,
                          protobuf ? providers_proto : reinterpret_cast<const uint8_t*>(providers_json),
                          protobuf ? providers_proto_length : providers_json_length};
    response.vary_accept = true;
    response.rate_limit_cost = authorized.rate_limit_cost;
    response.rate_limit_remaining = authorized.rate_limit_remaining;
    reply.Send(response);
  }

  // Reserves a job; null after answering.
  Job* Reserve(http::HttpReply& reply, const Policy& policy) {
    fortis::base::UniqueLock lock(mutex);
    Job* job = nullptr;
    for (Job& candidate : jobs)
      if (!candidate.used) { job = &candidate; break; }
    if (!job || stopping) {
      lock.Unlock();
      // Local saturation: the same typed back-pressure answer a busy lane gives.
      reply.Send(back_pressure(2000, policy));
      return nullptr;
    }
    job->used = true;
    return job;
  }

  void Release(Job* job) {
    tj::FreeArena(job->arena);
    for (size_t i = 0; i < cc::kProviderCount; ++i) tj::FreeArena(job->task_arena[i]);
    fortis::base::LockGuard lock(mutex);
    *job = Job{};
    idle.Broadcast();
  }

  bool Enqueue(Job* job, size_t slots) {
    fortis::base::LockGuard lock(mutex);
    if (queue_count + slots > sizeof(queue) / sizeof(queue[0])) return false;
    job->pending = slots;
    for (size_t i = 0; i < slots; ++i) {
      const size_t tail = (queue_head + queue_count) % (sizeof(queue) / sizeof(queue[0]));
      queue[tail] = Task{job, i};
      ++queue_count;
    }
    changed.Broadcast();
    return true;
  }

  // The parsed body as the route sees it: the protobuf Buffer decoded, a JSON
  // value as parsed, no body as a value no schema accepts (nullptr).
  static const Value* route_input(const Body& body, tj::Arena& arena, bool status) {
    switch (body.kind) {
      case BodyKind::kProtobuf:
        return status ? CrossChainDecodeStatusRequest(body.bytes, body.length, arena)
                      : CrossChainDecodeQuoteRequest(body.bytes, body.length, arena);
      case BodyKind::kValue: return body.value;
      default: return nullptr;
    }
  }

  // ---- POST /v1/exchange/cross-chain/quotes -------------------------------------------
  //
  // What it is. Live cross-chain route quotes from every selected provider,
  // ranked (TypeScript: getCrossChainQuotes, CrossChainRouter.quote and
  // the six provider adapters). The Flutter app (IntelligenceApi
  // .crossChainQuotes) posts a protobuf CrossChainQuoteRequest with Accept:
  // application/protobuf. Nothing is cached or stored: every request asks
  // its providers again, and the answer is never a cache read.
  //
  // Method and path. POST exactly; GET, HEAD, PUT, a trailing slash or upper
  // case are router 404s ({"error":"NOT_FOUND"}). The query string is
  // ignored.
  //
  // Hooks first (auth-first), in the /v1 order: any Origin header (even
  // "null") → 403 BROWSER_ORIGIN_FORBIDDEN; a "Bearer " device token, else
  // 401 UNAUTHORIZED / SESSION_EXPIRED, or ATTESTATION_REQUIRED for a
  // development device when ATTESTATION_MODE=required; then the device quota
  // at cost 10 (premium 1200, attested 600, development 240 per minute) →
  // 429 RATE_LIMITED with retry-after (seconds left in the minute). Refusals
  // carry no vary header. The native request cap is 512 KiB (router-wide,
  // 413).
  //
  // Body parser, after the quota (each refusal is charged): a body without a
  // content type → 415 {"error":"UNSUPPORTED_MEDIA_TYPE"}; a JSON media type
  // (application/json with any parameters, any case) that is empty or
  // invalid → 400 {"error":"BAD_REQUEST"} (`__proto__` is an ordinary key);
  // a leading BOM is dropped and a repeated key keeps its last value;
  // application/protobuf (any parameters) → bytes; any other type
  // (text/plain, form bodies, vnd.api+json, x-protobuf, octet-stream, xml)
  // → 415.
  //
  // Validation, after the quota (each 400 is charged): a protobuf body is
  // decoded by protobuf-es fromBinary (above; a throw → 400 BAD_REQUEST),
  // then quoteBody.safeParse → 400 {"error":"BAD_REQUEST"}: a strict object
  // {from, to, amountIn, slippageBps = 50, prepare = false, providers?};
  // each endpoint strict {chainId: trimmed /^[A-Za-z0-9][A-Za-z0-9._:-]
  // {0,63}$/, token and address: trimmed 1..256 UTF-16 units with no C0
  // control or DEL, decimals: integer 0..36}; amountIn /^[1-9]\d{0,79}$/
  // (no trim, ASCII digits); slippageBps integer 0..5000; prepare boolean;
  // providers 1..10 ids /^[a-z][a-z0-9-]{0,31}$/; then, in order: the chains
  // differ (after trimming, case-sensitive), the source is not
  // bitcoin-cash or zcash (exact) or ton (any case), a ton destination
  // (any case) is exactly "ton" with a mainnet friendly address, a monero
  // endpoint (any case) is exactly "monero", token "monero", 12 decimals and
  // a checksum-valid address, and prepare needs exactly one listed provider
  // (before de-duplication: ["lifi","lifi"] is refused).
  //
  // Provider selection: the listed ids de-duplicated in order (absent: all
  // six in registration order simpleswap, maya, bridge-vet, lifi, squid,
  // near-intents); any unregistered id → 400 {"error":
  // "UNKNOWN_CROSS_CHAIN_PROVIDER","providers":[...]} (unknown ids, request
  // order, de-duplicated), before any provider work.
  //
  // Upstream. Each selected provider runs independently
  // (Promise.allSettled): an unconfigured one is reported without a call;
  // a configured one runs its adapter through its lane (lifi 100/min burst
  // 25, squid/simpleswap/near-intents/axelarscan 30/min, bridge-vet 20/min;
  // a 429 at once, or five consecutive faults — transport failures,
  // timeouts, 5xx, 401, 403, 408 — open its breaker for 30 s, then one
  // probe):
  // HttpClient.json with a 12 s (15 s for NEAR/SimpleSwap/bridge.vet
  // creates) abort per attempt; a GET is tried three times on a transport
  // failure, timeout, 5xx, 408 or an unparseable 2xx, after 250 ms then
  // 1 s (or retry-after, at most 5 s); a POST (Squid's route, NEAR's quote,
  // SimpleSwap's order) once; 4xx and 429 are not retried. A provider that
  // throws (including LaneBusyError: an open breaker or a full lane) is
  // "failed"; [] is "no-route"; else "quoted" with its routes. Provider
  // credentials go only to their own provider as request headers; no client
  // header, device id or token is forwarded. `prepare` asks NEAR for a real
  // deposit address and SimpleSwap to create an order (the only upstream
  // side effects).
  //
  // Ranking: every quoted route, stably sorted by compareRoutes (protected
  // minimum then expected output, both descending as big integers; then
  // feeUsd ascending, null last; then etaSeconds ascending, null last), the
  // first five numbered rank 1..5. Ties keep provider selection order.
  // Quote identity: a route id is "<provider>:<provider quote id>", or a
  // SHA-256 fingerprint of provider, bridge, minimum and calldata when the
  // provider gives none — so equal quote ids from two providers stay
  // distinct and a repeated request repeats the id; nothing de-duplicates
  // routes. expiresAt is min(the provider's expiry, now + 30 s): a provider
  // expiry in the past is reported as is.
  //
  // Answer. {"routes":[...],"bestRouteId":<first id or null>,"attempts":
  // [{"providerId","status","routeCount"} per selected provider, selection
  // order],"cache":{"hit":false,"timestamp":null}} as application/json;
  // charset=utf-8, or CrossChainQuoteResponse when acceptsProtobuf() (an
  // application/protobuf entry whose first q is absent or > 0). 200 carries
  // vary: Accept, cache-control: no-store and x-rate-limit-cost/remaining.
  // A protobuf encoding failure is 502 UPSTREAM_UNAVAILABLE with the vary
  // header (protobuf-es throws inside the route's try); a JSON body over the
  // 2 MiB response capture is 500 RESPONSE_TOO_LARGE (native bound).
  //
  // Concurrency and bounds. Validation runs on the HTTP worker; the providers
  // run as one task each on kWorkers (6) threads, at most kJobs (12) requests
  // in flight — the 13th is answered 429 RATE_LIMITED retry-after 2 (local
  // back-pressure; TypeScript has no such bound). Results go through the
  // response handoff (30 s), so a request's provider work has a 25 s
  // deadline; TypeScript has none (three 12 s attempts take 37 s), so a
  // provider that hangs on every attempt is "failed" after 25 s natively
  // and after ~37 s in TypeScript — the same answer, sooner. Provider bodies
  // are bounded at 512 KiB (native; a larger one fails the provider).
  //
  // Restart. No state: lanes, breakers, NEAR's five-minute token catalog
  // and quota counters are process memory in both, so a restart starts them
  // fresh; the persistent volume holds only the device store.
  //
  // Differences (all asserted by tests/cross_chain_post_edges_test.py): the
  // router 404 bodies; quota retry-after on the wall clock (TypeScript's
  // frozen-Date oracle says 60); the test transport skips the parse-retry
  // sleeps (production waits 250 ms / 1 s as HttpClient); the 25 s deadline,
  // the 12-job bound and the 512 KiB request/provider-body caps above.
  void Quotes(const http::HttpRequest& request, http::HttpReply& reply) {
    PostState state;
    state.attestation_required = config.attestation_required;
    state.arena = tj::GrowingArena(kRequestArenaBytes);
    tj::Arena& arena = state.arena;
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> request_arena_blocks(&arena);
    if (!RunHttpSteps(kPostSteps, request, reply, state)) { tj::FreeArena(arena); return; }
    const Body& body = state.body;
    const Policy policy = DeviceQuotaOf(state.device);
    CrossChainQuotePlan plan;
    CrossChainPlanQuote(route_input(body, arena, false), arena, &plan);
    if (plan.kind == CrossChainPlanKind::kBadRequest) {
      send_bad_request(reply, policy);
      tj::FreeArena(arena);
      return;
    }
    if (plan.kind == CrossChainPlanKind::kUnknownProviders) {
      Value* error = tj::Obj(arena);
      Value* list = tj::Arr(arena);
      bool ok = error && list && tj::Set(arena, error, "error", tj::Str(arena, "UNKNOWN_CROSS_CHAIN_PROVIDER"));
      for (size_t i = 0; ok && i < plan.unknown_count; ++i) ok = tj::Push(arena, list, const_cast<Value*>(plan.unknown[i]));
      ok = ok && tj::Set(arena, error, "providers", list);
      uint8_t text[1024];
      OutBuffer out{text, sizeof(text), 0, true};
      if (ok && tj::Stringify(error, out) && out.ok) {
        http::HttpResponse response{400, "Bad Request", kJsonType, text, out.used};
        response.rate_limit_cost = policy.cost;
        response.rate_limit_remaining = policy.remaining;
        reply.Send(response);
      } else {
        send(reply, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", policy);
      }
      tj::FreeArena(arena);
      return;
    }
    Job* job = Reserve(reply, policy);
    if (!job) { tj::FreeArena(arena); return; }
    job->status = false;
    job->protobuf = AcceptsProtobuf(request);
    job->head = reply.head();
    job->policy = policy;
    job->arena = *request_arena_blocks.release();
    job->quote = plan.request;
    job->count = plan.provider_count;
    size_t tasks = 0;
    for (size_t i = 0; i < plan.provider_count; ++i) {
      job->providers[i] = plan.providers[i];
      job->configured[i] = cc::Describe(settings, plan.providers[i]).configured;
      if (job->configured[i]) ++tasks;
    }
    uint64_t token = 0;
    if (!reply.Reserve(kHandoffTimeoutMs, &token)) {
      reply.Send(back_pressure(2000, policy));
      Release(job);
      return;
    }
    job->token = token;
    job->deadline = monotonic_ms() + (seams.provider_deadline_ms ? seams.provider_deadline_ms : kProviderDeadlineMs);
    if (!tasks) { FinishQuote(job); return; }
    if (!Enqueue(job, plan.provider_count)) {
      handoff->Complete(token, back_pressure(2000, policy));
      Release(job);
    }
  }

  // ---- POST /v1/exchange/cross-chain/status -------------------------------------------
  //
  // What it is. The progress of one cross-chain transfer, asked of the
  // provider that routed it (TypeScript: getCrossChainStatus,
  // CrossChainRouter.status, each adapter's
  // status()). The Flutter app (IntelligenceApi.crossChainStatus) posts a
  // protobuf CrossChainStatusRequest with Accept: application/protobuf and
  // polls while a transfer is open. Nothing is cached or stored: every
  // request asks the provider again.
  //
  // Method, path, body parser and hooks: as POST /quotes above (POST only,
  // Origin → 403, bearer → 401, quota cost 10 → 429, then the same media
  // types and parser errors).
  //
  // Validation, after the quota: a protobuf body is decoded by protobuf-es
  // (above: the four implicit strings always present, each optional field
  // when present; a throw → 400), then statusBody.safeParse → 400
  // {"error":"BAD_REQUEST"}: strict {providerId /^[a-z][a-z0-9-]{0,31}$/
  // (no trim), txHash plainText (trimmed 1..256 UTF-16 units, no C0/DEL),
  // fromChainId and toChainId as quote chain ids, toToken?, providerQuoteId?,
  // depositMemo?, bridgeId?, bridge? plainText, toSymbol? trimmed
  // /^[A-Za-z0-9._+-]{1,32}$/, toDecimals? integer 0..36}; null is never an
  // absent optional. The transform drops `bridge` except as Squid's bridgeId
  // fallback (bridgeId ?? (providerId === "squid" ? bridge : undefined)), so
  // a display label never reaches LI.FI.
  //
  // Provider: an unregistered id → 400 {"error":
  // "UNKNOWN_CROSS_CHAIN_PROVIDER"} (no list); an unconfigured one (lifi,
  // squid, simpleswap without their credential) → 503 {"error":
  // "PROVIDER_NOT_CONFIGURED","provider":"<id>"}; both after the quota and
  // before any provider work. maya, bridge-vet and near-intents are always
  // configured.
  //
  // Upstream: the provider's status() through its lane, as for quotes (a GET
  // tried three times on transport failures, timeouts, 5xx, 408 and
  // unparseable 2xx; a 4xx or 429 once): LI.FI GET /v1/status (a 400 with a
  // bridge hint is asked once more without it), Squid GET /v2/status (plus
  // AxelarScan's GMP search, best effort, 8 s per attempt), MAYA
  // /tx/status/<hash> on its first root (or the public node), NEAR GET
  // /v0/status by deposit address (and memo), SimpleSwap GET
  // /v3/exchanges/<id>, bridge.vet GET /api/bridge/status/<id>. A
  // LaneBusyError (an open breaker, 30 s after a 429 or five consecutive
  // faults; a full lane after 2 s) → 429 RATE_LIMITED with retry-after (its
  // retryAfterMs, else 30 s for a breaker, 2 s otherwise; at least 1 s);
  // any other failure — an upstream error, an unknown transfer the provider
  // answers 404, or a reply the adapter refuses (a different order id, a
  // finished order without its payout, a wrong token) → 502
  // {"error":"UPSTREAM_UNAVAILABLE"}.
  //
  // Projection. Each adapter maps the provider's states to the normalized
  // CrossChainStatus: state pending / completed / failed / refunded /
  // partial / needs-action / not-found (an unknown transfer the provider
  // answers 200), with progress steps, received amount, explorer links
  // (https only), timings and recovery guidance. Expired orders are failed
  // (SimpleSwap, bridge.vet); NEAR's unknown states stay pending.
  //
  // Answer. The status object as application/json; charset=utf-8, or
  // CrossChainStatusResponse (statusMessage, toBinary) when
  // acceptsProtobuf(); 200 carries vary: Accept, cache-control: no-store and
  // the quota headers; refusals carry no vary. A protobuf encoding failure
  // is 502 with vary.
  //
  // Concurrency, bounds and restart: one provider task on the quote workers
  // and job slots (the 13th simultaneous request is 429 retry-after 2), the
  // same 25 s deadline (TypeScript's lookup can run ~37 s before its 502),
  // 512 KiB request and provider-body caps; breakers, lanes and quotas are
  // process memory in both, so a restart closes an open breaker.
  //
  // Differences (asserted by tests/cross_chain_post_edges_test.py): the
  // router 404 bodies, quota retry-after on the wall clock, the test
  // transport's skipped parse-retry sleeps, and the bounds above.
  void StatusRoute(const http::HttpRequest& request, http::HttpReply& reply) {
    PostState state;
    state.attestation_required = config.attestation_required;
    state.arena = tj::GrowingArena(kRequestArenaBytes);
    tj::Arena& arena = state.arena;
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> request_arena_blocks(&arena);
    if (!RunHttpSteps(kPostSteps, request, reply, state)) { tj::FreeArena(arena); return; }
    const Body& body = state.body;
    const Policy policy = DeviceQuotaOf(state.device);
    CrossChainStatusPlan plan;
    CrossChainPlanStatus(route_input(body, arena, true), arena, &plan);
    if (plan.kind == CrossChainPlanKind::kBadRequest) {
      send_bad_request(reply, policy);
      tj::FreeArena(arena);
      return;
    }
    if (plan.kind == CrossChainPlanKind::kUnknownProviders) {
      send(reply, 400, "Bad Request", "{\"error\":\"UNKNOWN_CROSS_CHAIN_PROVIDER\"}", policy);
      tj::FreeArena(arena);
      return;
    }
    const cc::Descriptor descriptor = cc::Describe(settings, static_cast<size_t>(plan.provider));
    if (!descriptor.configured) {
      char text[128];
      snprintf(text, sizeof(text), "{\"error\":\"PROVIDER_NOT_CONFIGURED\",\"provider\":\"%s\"}", descriptor.id);
      send(reply, 503, "Service Unavailable", text, policy);
      tj::FreeArena(arena);
      return;
    }
    Job* job = Reserve(reply, policy);
    if (!job) { tj::FreeArena(arena); return; }
    job->status = true;
    job->protobuf = AcceptsProtobuf(request);
    job->head = reply.head();
    job->policy = policy;
    job->arena = *request_arena_blocks.release();
    job->status_request = plan.request;
    job->count = 1;
    job->providers[0] = static_cast<uint8_t>(plan.provider);
    job->configured[0] = true;
    uint64_t token = 0;
    if (!reply.Reserve(kHandoffTimeoutMs, &token)) {
      reply.Send(back_pressure(2000, policy));
      Release(job);
      return;
    }
    job->token = token;
    job->deadline = monotonic_ms() + (seams.provider_deadline_ms ? seams.provider_deadline_ms : kProviderDeadlineMs);
    if (!Enqueue(job, 1)) {
      handoff->Complete(token, back_pressure(2000, policy));
      Release(job);
    }
  }

  // ---- workers ----

  static void WorkerMain(Impl* self) { self->WorkerLoop(); }

  void WorkerLoop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      while (!queue_count && !stopping) changed.Wait(lock);
      if (!queue_count && stopping) return;
      const Task task = queue[queue_head];
      queue_head = (queue_head + 1) % (sizeof(queue) / sizeof(queue[0]));
      --queue_count;
      lock.Unlock();
      Run(task);
    }
  }

  void Run(const Task& task) {
    Job* job = task.job;
    const size_t slot = task.slot;
    cc::ProviderContext context;
    context.transport = transport.get();
    context.settings = &settings;
    context.clock = Clock();
    context.deadline_ms = job->deadline;
    context.near_catalog = &deposits.Catalog();
    if (job->configured[slot]) {
      job->task_arena[slot] = tj::GrowingArena(kTaskArenaBytes);
      tj::Arena& arena = job->task_arena[slot];
      const size_t provider = job->providers[slot];
      if (monotonic_ms() >= job->deadline) {
        job->outcome[slot].failure = cc::Failure::kError;
      } else if (job->status) {
        job->outcome[slot] = deposits.Track(provider, context, arena, job->status_request, &job->result[slot]);
      } else {
        job->outcome[slot] = quotes.Quote(provider, context, arena, job->quote, &job->result[slot]);
      }
      if (!arena.ok && job->outcome[slot].failure == cc::Failure::kNone) job->outcome[slot].failure = cc::Failure::kError;
    }
    fortis::base::UniqueLock lock(mutex);
    const bool last = --job->pending == 0;
    lock.Unlock();
    if (!last) return;
    if (job->status) FinishStatus(job);
    else FinishQuote(job);
  }

  // Encodes `value` as JSON or protobuf and completes the handoff.
  void Respond(Job* job, const Value* value, Encoder encode) {
    OutBuffer measure{nullptr, 0, 0, true};
    const bool measured = job->protobuf ? encode(measure, value) : tj::Stringify(value, measure);
    http::HttpResponse response;
    std::unique_ptr<uint8_t[], base::StorageDeleter> buffer_storage;
  uint8_t* buffer = nullptr;
    if (!measured || !measure.ok) {
      // protobuf-es throws inside the route's try block: a 502 that keeps the
      // Vary header sendProtobufOrJson already set.
      response = json_response(502, "Bad Gateway", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", job->policy);
      response.vary_accept = true;
    } else if (measure.used > kResponseLimit) {
      response = json_response(500, "Internal Server Error", "{\"error\":\"RESPONSE_TOO_LARGE\"}", job->policy);
    } else {
      buffer_storage = base::AllocateStorage<uint8_t[]>(measure.used ? measure.used : 1);
      buffer = buffer_storage.get();
      OutBuffer out{buffer, measure.used, 0, buffer != nullptr};
      const bool written = buffer && (job->protobuf ? encode(out, value) : tj::Stringify(value, out)) && out.ok;
      if (!written) {
        response = json_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", job->policy);
      } else {
        response = http::HttpResponse{200, "OK", job->protobuf ? kProtobufType : kJsonType, buffer, out.used};
        response.vary_accept = true;
        response.rate_limit_cost = job->policy.cost;
        response.rate_limit_remaining = job->policy.remaining;
      }
    }
    handoff->Complete(job->token, response);
    buffer_storage.reset();
  }

  void FinishQuote(Job* job) {
    cc::QuoteInputs inputs{job->count, job->providers, job->configured, job->outcome, job->result};
    Value* result = nullptr;
    if (!quotes.Aggregate(job->arena, settings, inputs, &result)) {
      handoff->Complete(job->token, json_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", job->policy));
    } else {
      Respond(job, result, encode_quote);
    }
    Release(job);
  }

  void FinishStatus(Job* job) {
    const cc::Outcome& outcome = job->outcome[0];
    if (outcome.failure == cc::Failure::kLaneBusy) {
      handoff->Complete(job->token, back_pressure(outcome.retry_after_ms, job->policy));
    } else if (outcome.failure != cc::Failure::kNone || !job->result[0]) {
      handoff->Complete(job->token,
                        json_response(502, "Bad Gateway", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", job->policy));
    } else {
      Respond(job, job->result[0], encode_status);
    }
    Release(job);
  }

  bool WaitIdle(uint32_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    fortis::base::UniqueLock lock(mutex);
    for (;;) {
      bool busy = queue_count > 0;
      for (const Job& job : jobs) busy = busy || job.used;
      if (!busy) return true;
      if (monotonic_ms() >= deadline) return false;
      timespec wake{};
      clock_gettime(CLOCK_REALTIME, &wake);
      wake.tv_nsec += 50 * 1000 * 1000;
      if (wake.tv_nsec >= 1000000000L) { ++wake.tv_sec; wake.tv_nsec -= 1000000000L; }
      idle.WaitUntil(lock, wake);
    }
  }
};

void CrossChainService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

CrossChainService::~CrossChainService() { Stop(); }

CrossChainService::CrossChainService(Impl* impl) : impl_(impl) {}
CrossChainService::CrossChainService(CrossChainService&&) noexcept = default;

base::Result<CrossChainService> CrossChainService::Create(
    const CrossChainConfig& config, CrossChainServiceDeps deps, const CrossChainServiceSeams* seams) try {
  auto& coordinator = deps.coordinator;
  Impl* impl = fortis::base::New<Impl>();
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "cross_chain allocation");
  CrossChainService service(impl);
  impl->config = config;
  if (seams) impl->seams = *seams;
  bool ok = impl->LoadSettings() && impl->BuildProviders();
  if (ok) {
    impl->transport.reset(fortis::base::New<fortis::chains::Transport>(config.transport, &coordinator));
    ok = impl->transport != nullptr;
  }
  if (ok) {
    ok = impl->seams.transport_starter ? impl->seams.transport_starter->Start(*impl->transport)
                                       : impl->transport->Start();
  }
  for (size_t i = 0; ok && i < kWorkers; ++i) {
    ok = impl->workers[i].Start(Impl::WorkerMain, impl, 256 * 1024);
    if (ok) ++impl->started_workers;
  }
  if (!ok) {
    return base::Status(base::StatusCode::kUnavailable, "cross_chain initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "cross_chain allocation");
}

bool CrossChainService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  impl_->handoff = &handoff;
  return router.Add<&Impl::Providers>("GET", "/v1/exchange/cross-chain/providers", *impl_) &&
         router.Add<&Impl::Quotes>("POST", "/v1/exchange/cross-chain/quotes", *impl_) &&
         router.Add<&Impl::StatusRoute>("POST", "/v1/exchange/cross-chain/status", *impl_);
}

void CrossChainService::Stop() {
  if (!impl_) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  for (size_t i = 0; i < impl_->started_workers; ++i) impl_->workers[i].Join();
  if (impl_->transport) {
    impl_->transport->Stop();
    impl_->transport.reset();
  }
  impl_->deposits.Reset();
  impl_.reset();
}

bool CrossChainService::WaitIdle(uint32_t timeout_ms) { return impl_->WaitIdle(timeout_ms); }

}  // namespace fortis::exchange
