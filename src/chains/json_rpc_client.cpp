#include "chain_transport_policy.h"
#include <stdio.h>

namespace tj = fortis::codec::tj;
namespace fortis::chains {
namespace {
bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull:
    case tj::Type::kFalse: return false;
    case tj::Type::kTrue: return true;
    case tj::Type::kNumber: return v->number != 0 && v->number == v->number;
    case tj::Type::kString: return v->length > 0;
    case tj::Type::kArray:
    case tj::Type::kObject: return true;
  }
  return false;
}

}  // namespace

Value* JsonRpcClient::Envelope(Arena& arena, const Call& call) {
  Value* document = tj::Obj(arena);
  Value* params = call.params ? const_cast<Value*>(call.params) : tj::Arr(arena);
  if (!document || !params || !tj::Set(arena, document, "jsonrpc", tj::Str(arena, "2.0")) ||
      !tj::Set(arena, document, "id", tj::Num(arena, 1)) ||
      !tj::Set(arena, document, "method", tj::Str(arena, call.method ? call.method : "")) ||
      !tj::Set(arena, document, "params", params))
    return nullptr;
  return document;
}
JsonRpcClient::Outcome JsonRpcClient::Interpret(Call& call, Value* body) {

  if (body->type == tj::Type::kNull) {
    // `body.error` on null is a TypeError: a transport-class failure.
    call.failure = CallFailure::kTransport;
    snprintf(call.message, sizeof(call.message), "null JSON-RPC body");
    return Outcome::kTransport;
  }
  const Value* error = tj::IsObject(body) ? tj::Get(body, "error") : nullptr;
  if (truthy(error)) {
    call.failure = CallFailure::kAnswer;
    uint8_t text[512];
    OutBuffer out{text, sizeof(text), 0, true};
    tj::Stringify(error, out);
    snprintf(call.message, sizeof(call.message), "%s: %.*s", call.method ? call.method : "",
         static_cast<int>(out.used), reinterpret_cast<const char*>(text));
    const Value* code = tj::IsObject(error) ? tj::Get(error, "code") : nullptr;
    if (tj::IsNumber(code)) { call.has_code = true; call.code = code->number; }
    return Outcome::kAnswer;
  }
  Value* result = tj::IsObject(body) ? tj::Get(body, "result") : nullptr;
  if (!result) {
    call.failure = CallFailure::kAnswer;
    snprintf(call.message, sizeof(call.message), "%s: empty result", call.method ? call.method : "");
    return Outcome::kAnswer;
  }
  call.failure = CallFailure::kNone;
  call.message[0] = '\0';
  call.value = result;
  return Outcome::kSuccess;
}
}  // namespace fortis::chains
