// The pure steps of the three Robinhood POST handlers (robinhood_service.h):
// protobuf-es fromBinary of the request messages, the routes' zod schemas and
// the Alchemy request and reply shapes.
#include "robinhood_service_internal.h"

namespace pw = fortis::codec::proto_wire;

namespace rh = fortis::exchange::robinhood;

namespace tj = fortis::codec::tj;

namespace fortis::exchange {

namespace {

// ---- protobuf-es fromBinary ----------------------------------------------------------------
//
// protobuf-es 2.16's readMessage over one cursor (DiscoveryProtoReader, as
// R40/R41): a nested RobinhoodEvmCall's length bounds only its own read
// loop, so its last field may run past it; known fields are read by their
// declared type whatever the wire type says (a string by `bytes()`, the
// `sponsored` bool by `bool()`, a call by `uint32()` then its loop); unknown
// fields are skipped (varints of any length, groups to their end tag, 100
// levels); every `calls` occurrence is a new call and a repeated singular
// string keeps its last value; strings are strict UTF-8 with one leading
// byte-order mark dropped (TextDecoder). Any read past the body throws, and
// the route answers 400.

constexpr int kProtoSkipLimit = 100;

Value* read_string(discovery::DiscoveryProtoReader& reader, tj::Arena& arena) {
  const uint8_t* text = nullptr;
  size_t length = 0;
  if (!reader.String(&text, &length)) return nullptr;
  return tj::Str(arena, reinterpret_cast<const char*>(text), length);
}

// RobinhoodEvmCall mapped to the route's call object: to, data and the
// optional fields that were present (prepare maps only `value`).
Value* decode_call(discovery::DiscoveryProtoReader& reader, tj::Arena& arena, bool fees) {
  static const char* const kNames[] = {"", "to", "data", "value", "gas", "maxFeePerGas", "maxPriorityFeePerGas"};
  uint32_t length = 0;
  if (!reader.MessageLength(&length)) return nullptr;
  const size_t end = reader.pos() + length;
  discovery::DiscoveryProtoReader::Frame frame(reader, end);
  if (!frame.ok()) return nullptr;
  Value* values[7] = {};
  values[1] = tj::Str(arena, "");
  values[2] = tj::Str(arena, "");
  if (!values[1] || !values[2]) return nullptr;
  while (reader.pos() < end) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return nullptr;
    if (field >= 1 && field <= 6) {
      values[field] = read_string(reader, arena);
      if (!values[field]) return nullptr;
    } else if (!reader.Skip(wire, field, kProtoSkipLimit)) {
      return nullptr;
    }
  }
  Value* call = tj::Obj(arena);
  if (!call) return nullptr;
  const int last = fees ? 6 : 3;
  for (int i = 1; i <= last; ++i)
    if (values[i] && !tj::Set(arena, call, kNames[i], values[i])) return nullptr;
  return call;
}

// fromBinary(RobinhoodDiagnoseRequest | RobinhoodPrepareRequest) mapped as
// the route maps it: {from, calls[, sponsored]}.
Value* decode_calls_request(const uint8_t* body, size_t length, tj::Arena& arena, bool diagnose) {
  Value* from = tj::Str(arena, "");
  Value* calls = tj::Arr(arena);
  bool sponsored = false;
  if (!from || !calls) return nullptr;
  discovery::DiscoveryProtoReader reader(body, length);
  while (reader.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return nullptr;
    if (field == 1) {
      from = read_string(reader, arena);
      if (!from) return nullptr;
    } else if (field == 2) {
      Value* call = decode_call(reader, arena, diagnose);
      if (!call || !tj::Push(arena, calls, call)) return nullptr;
    } else if (field == 3 && diagnose) {
      if (!reader.Bool(&sponsored)) return nullptr;
    } else if (!reader.Skip(wire, field, kProtoSkipLimit)) {
      return nullptr;
    }
  }
  Value* input = tj::Obj(arena);
  if (!input || !tj::Set(arena, input, "from", from) || !tj::Set(arena, input, "calls", calls)) return nullptr;
  if (diagnose && !tj::Set(arena, input, "sponsored", tj::Bool(arena, sponsored))) return nullptr;
  return input;
}

// fromBinary(RobinhoodSubmitRequest) mapped as {ticket, signatures}.
Value* decode_submit(const uint8_t* body, size_t length, tj::Arena& arena) {
  Value* ticket = tj::Str(arena, "");
  Value* signatures = tj::Arr(arena);
  if (!ticket || !signatures) return nullptr;
  discovery::DiscoveryProtoReader reader(body, length);
  while (reader.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!reader.Tag(&field, &wire)) return nullptr;
    if (field == 1) {
      ticket = read_string(reader, arena);
      if (!ticket) return nullptr;
    } else if (field == 2) {
      Value* signature = read_string(reader, arena);
      if (!signature || !tj::Push(arena, signatures, signature)) return nullptr;
    } else if (!reader.Skip(wire, field, kProtoSkipLimit)) {
      return nullptr;
    }
  }
  Value* input = tj::Obj(arena);
  if (!input || !tj::Set(arena, input, "ticket", ticket) || !tj::Set(arena, input, "signatures", signatures))
    return nullptr;
  return input;
}

// ---- zod schemas -----------------------------------------------------------------------------

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

bool hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool address_value(const Value* v) { return tj::IsString(v) && address_text(v->text, v->length); }

// /^0x(?:[0-9a-fA-F]{2})*$/ and at most 65,538 characters.
bool hex_data_value(const Value* v) {
  if (!tj::IsString(v) || v->length < 2 || v->length > 65538 || v->length % 2 || v->text[0] != '0' || v->text[1] != 'x')
    return false;
  for (size_t i = 2; i < v->length; ++i)
    if (!hex_digit(v->text[i])) return false;
  return true;
}

// /^0x(?:0|[1-9a-fA-F][0-9a-fA-F]*)$/
bool quantity_value(const Value* v) {
  if (!tj::IsString(v) || v->length < 3 || v->text[0] != '0' || v->text[1] != 'x') return false;
  if (v->text[2] == '0') return v->length == 3;
  for (size_t i = 2; i < v->length; ++i)
    if (!hex_digit(v->text[i])) return false;
  return true;
}

// evmCall / diagnosticCall (strict).
bool valid_call(const Value* call, bool fees) {
  static const char* const kKeys[] = {"to", "data", "value", "gas", "maxFeePerGas", "maxPriorityFeePerGas"};
  if (!tj::IsObject(call) || !only_keys(call, kKeys, fees ? 6 : 3)) return false;
  if (!address_value(tj::Get(call, "to")) || !hex_data_value(tj::Get(call, "data"))) return false;
  for (size_t k = 2; k < (fees ? 6u : 3u); ++k) {
    const Value* v = tj::Get(call, kKeys[k]);
    if (v && !quantity_value(v)) return false;
  }
  return true;
}

bool valid_calls(const Value* calls, bool fees) {
  if (!tj::IsArray(calls) || calls->count < 1 || calls->count > rh::kMaxCalls) return false;
  for (uint32_t i = 0; i < calls->count; ++i)
    if (!valid_call(calls->items[i], fees)) return false;
  return true;
}

// diagnosticsBody: {from, calls, sponsored = false} (strict).
bool valid_diagnose(const Value* input, bool* sponsored) {
  static const char* const kKeys[] = {"from", "calls", "sponsored"};
  if (!tj::IsObject(input) || !only_keys(input, kKeys, 3) || !address_value(tj::Get(input, "from")) ||
      !valid_calls(tj::Get(input, "calls"), true))
    return false;
  const Value* flag = tj::Get(input, "sponsored");
  if (flag && !tj::IsBool(flag)) return false;
  *sponsored = flag && flag->type == tj::Type::kTrue;
  return true;
}

// prepareBody: {from, calls} (strict).
bool valid_prepare(const Value* input) {
  static const char* const kKeys[] = {"from", "calls"};
  return tj::IsObject(input) && only_keys(input, kKeys, 2) && address_value(tj::Get(input, "from")) &&
         valid_calls(tj::Get(input, "calls"), false);
}

// submitBody: {ticket: /^[A-Za-z0-9_-]{32}$/, signatures: 1..2 of /^0x[0-9a-fA-F]{130}$/}.
bool valid_submit(const Value* input) {
  static const char* const kKeys[] = {"ticket", "signatures"};
  if (!tj::IsObject(input) || !only_keys(input, kKeys, 2)) return false;
  const Value* ticket = tj::Get(input, "ticket");
  if (!tj::IsString(ticket) || ticket->length != rh::TicketStore::kTicketChars) return false;
  for (size_t i = 0; i < ticket->length; ++i) {
    const char c = ticket->text[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  const Value* signatures = tj::Get(input, "signatures");
  if (!tj::IsArray(signatures) || signatures->count < 1 || signatures->count > rh::kMaxCalls) return false;
  for (uint32_t i = 0; i < signatures->count; ++i) {
    const Value* s = signatures->items[i];
    if (!tj::IsString(s) || s->length != 132 || s->text[0] != '0' || s->text[1] != 'x') return false;
    for (size_t k = 2; k < 132; ++k)
      if (!hex_digit(s->text[k])) return false;
  }
  return true;
}

}  // namespace

namespace robinhood_service_internal {

// /^0x[0-9a-fA-F]{40}$/
bool address_text(const char* t, size_t length) {
  if (length != 42 || t[0] != '0' || t[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i)
    if (!hex_digit(t[i])) return false;
  return true;
}

}  // namespace robinhood_service_internal

// ---- the pure steps of the three POST handlers (robinhood_service.h) --------------------------

Value* RobinhoodDecodeWriteRequest(RobinhoodWriteKind kind, const uint8_t* body, size_t length, tj::Arena& arena) {
  return kind == RobinhoodWriteKind::kSubmit
             ? decode_submit(body, length, arena)
             : decode_calls_request(body, length, arena, kind == RobinhoodWriteKind::kDiagnose);
}

bool RobinhoodValidWriteRequest(RobinhoodWriteKind kind, const Value* input, bool* sponsored) {
  *sponsored = false;
  if (!input) return false;
  return kind == RobinhoodWriteKind::kDiagnose ? valid_diagnose(input, sponsored)
         : kind == RobinhoodWriteKind::kPrepare ? valid_prepare(input)
                                                 : valid_submit(input);
}

Value* RobinhoodRpcEnvelope(tj::Arena& arena, const char* method, Value* params) {
  Value* envelope = tj::Obj(arena);
  if (!envelope || !params || !tj::Set(arena, envelope, "jsonrpc", tj::Str(arena, "2.0")) ||
      !tj::Set(arena, envelope, "id", tj::Num(arena, 1)) || !tj::Set(arena, envelope, "method", tj::Str(arena, method)) ||
      !tj::Set(arena, envelope, "params", params))
    return nullptr;
  return envelope;
}

Value* RobinhoodPrepareParams(tj::Arena& arena, Value* from_lower, Value* executable, const char* policy_id) {
  Value* request = tj::Obj(arena);
  Value* capabilities = tj::Obj(arena);
  Value* paymaster = tj::Obj(arena);
  Value* params = tj::Arr(arena);
  const bool built = request && capabilities && paymaster && params && tj::Set(arena, request, "from", from_lower) &&
                     tj::Set(arena, request, "chainId", tj::Str(arena, rh::kChainIdHex)) &&
                     tj::Set(arena, request, "calls", executable) &&
                     tj::Set(arena, paymaster, "policyId", tj::Str(arena, policy_id)) &&
                     tj::Set(arena, capabilities, "paymasterService", paymaster) &&
                     tj::Set(arena, request, "capabilities", capabilities) && tj::Push(arena, params, request);
  return built ? params : nullptr;
}

Value* RobinhoodSimulateParams(tj::Arena& arena, Value* simulation_calls, bool sponsored) {
  Value* block = tj::Obj(arena);
  Value* blocks = tj::Arr(arena);
  Value* options = tj::Obj(arena);
  Value* params = tj::Arr(arena);
  const bool built = simulation_calls && block && blocks && options && params &&
                     tj::Set(arena, block, "calls", simulation_calls) && tj::Push(arena, blocks, block) &&
                     tj::Set(arena, options, "blockStateCalls", blocks) &&
                     tj::Set(arena, options, "validation", tj::Bool(arena, !sponsored)) &&
                     tj::Set(arena, options, "traceTransfers", tj::Bool(arena, true)) && tj::Push(arena, params, options);
  return built ? params : nullptr;
}

Value* RobinhoodTraceParams(tj::Arena& arena, Value* simulation_calls) {
  Value* params = tj::Arr(arena);
  Value* bundle = tj::Obj(arena);
  Value* bundles = tj::Arr(arena);
  Value* context = tj::Obj(arena);
  Value* tracer = tj::Obj(arena);
  const bool built = simulation_calls && params && bundle && bundles && context && tracer &&
                     tj::Set(arena, bundle, "transactions", simulation_calls) && tj::Push(arena, bundles, bundle) &&
                     tj::Push(arena, params, bundles) && tj::Set(arena, context, "blockNumber", tj::Str(arena, "latest")) &&
                     tj::Set(arena, context, "transactionIndex", tj::Num(arena, -1)) && tj::Push(arena, params, context) &&
                     tj::Set(arena, tracer, "tracer", tj::Str(arena, "callTracer")) && tj::Push(arena, params, tracer);
  return built ? params : nullptr;
}

Value* RobinhoodPrepareResult(tj::Arena& arena, const char* ticket, const char* expires_iso, Value* prepared,
                              Value* requests) {
  Value* result = tj::Obj(arena);
  if (!result || !tj::Set(arena, result, "ticket", tj::Str(arena, ticket)) ||
      !tj::Set(arena, result, "expiresAt", tj::Str(arena, expires_iso)) || !tj::Set(arena, result, "prepared", prepared) ||
      !tj::Set(arena, result, "signatureRequests", requests))
    return nullptr;
  return result;
}

bool RobinhoodTicketText(const uint8_t random[24], char out[33]) {
  char text[40];
  if (!fortis::chains::Base64Encode(random, 24, true, false, text, sizeof(text)) || strlen(text) != 32) return false;
  memcpy(out, text, 33);
  return true;
}

// sendProtobufOrJson(RobinhoodDynamicResponse, dynamicMessage(payload), payload).
bool RobinhoodEncodeDynamic(OutBuffer& out, const Value* payload) {
  if (!payload || payload->type == tj::Type::kNull) return true;  // payload unset
  if (!tj::IsObject(payload)) {
    // create() with a non-object Struct value: an empty Struct.
    pw::PutTag(out, 1, 2);
    out.Byte(0);
    return true;
  }
  return tj::AppendStructuredReadResponse(payload, out);
}

}  // namespace fortis::exchange
