#pragma once

#include "cross_chain_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Internal to the deposit-address providers (MAYAChain, NEAR Intents,
// SimpleSwap and bridge.vet): passive pricing never
// creates an order; only a prepared quote for exactly one provider allocates
// a quote-bound deposit target, which every adapter re-validates against the
// request before the wallet may sign anything. These are the small text and
// value helpers the four adapters (cross_chain_maya.cpp,
// cross_chain_near_intents.cpp, cross_chain_simpleswap.cpp,
// cross_chain_bridge_vet.cpp) share.
namespace fortis::exchange::cross_chain::detail::deposit {
constexpr const char* kNativeEvm = "0x0000000000000000000000000000000000000000";

struct Text {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  OutBuffer out{};
  bool Init(Arena& arena, size_t bytes) {
    data = static_cast<uint8_t*>(arena.Alloc(bytes, 1));
    capacity = bytes;
    out = OutBuffer{data, bytes, 0, data != nullptr};
    return data != nullptr;
  }
  const char* Finish() {
    if (!out.ok || out.used >= capacity) return nullptr;
    data[out.used] = '\0';
    return reinterpret_cast<const char*>(data);
  }
};

inline size_t value_bytes(const Value* v) { return v && v->type == codec::tj::Type::kString ? v->length * 9 + 8 : 8; }

inline bool lower_is(Arena& arena, const Value* v, const char* text) {
  const Value* lower = Lower(arena, v);
  return lower && Is(lower, text);
}

inline bool same_lower(Arena& arena, const Value* a, const Value* b) {
  return SameText(Lower(arena, a), Lower(arena, b));
}

// JavaScript truthiness of a JSON value (`undefined` is falsy).
inline bool truthy(const Value* v) {
  if (!v || v == Undefined()) return false;
  switch (v->type) {
    case codec::tj::Type::kNull:
    case codec::tj::Type::kFalse: return false;
    case codec::tj::Type::kTrue: return true;
    case codec::tj::Type::kNumber: return v->number != 0 && v->number == v->number;
    case codec::tj::Type::kString: return v->length > 0;
    default: return true;
  }
}

inline Value* num_value(Arena& arena, double number) { return codec::tj::Num(arena, number); }

inline Value* str(Arena& arena, const char* text) { return S(arena, text); }

inline Value* concat(Arena& arena, const char* a, const Value* b, const char* c = "") {
  const size_t la = strlen(a), lc = strlen(c), lb = b ? b->length : 0;
  char* out = static_cast<char*>(arena.Alloc(la + lb + lc + 1, 1));
  if (!out) return nullptr;
  memcpy(out, a, la);
  if (lb) memcpy(out + la, b->text, lb);
  memcpy(out + la + lb, c, lc);
  out[la + lb + lc] = '\0';
  return codec::tj::Str(arena, out, la + lb + lc);
}

inline const char* iso_min(Arena& arena, double a, double b) { return IsoText(arena, a < b ? a : b); }

}  // namespace fortis::exchange::cross_chain::detail::deposit
