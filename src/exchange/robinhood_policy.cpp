#include <algorithm>
#include "robinhood_policy.h"

#include "chain_core.h"
#include "cross_chain_support.h"
#include "js_json.h"
#include "proto_wire.h"
#include "price_history_batch_codec.h"
#include "token_text.h"

#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::robinhood {
namespace {

namespace text = fortis::compat::token_text;
namespace hb = fortis::codec::history_batch;
using chains::BigInt;

constexpr const char* kRouterV2 = "0x89e5db8b5aa49aa85ac63f691524311aeb649eba";
constexpr const char* kRouterV3 = "0xcaf681a66d020601342297493863e78c959e5cb2";
constexpr const char* kWeth = "0x0bd7d308f8e1639fab988df18a8011f41eacad73";
constexpr const char* kUsdg = "0x5fc5360d0400a0fd4f2af552add042d716f1d168";
constexpr const char* kZero = "0x0000000000000000000000000000000000000000";
constexpr const char* kSentinelOne = "0x0000000000000000000000000000000000000001";
constexpr const char* kSentinelTwo = "0x0000000000000000000000000000000000000002";
constexpr const char* kNative = "0xeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr const char* kTransferTopic = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
constexpr const char* kApprove = "095ea7b3";
// 100 * 10^18 wei, the per-call value ceiling.
constexpr const char* kMaxValueHex = "56bc75e2d63100000";
constexpr size_t kMaxDataChars = 65538;
constexpr size_t kMaxPath = 4;

bool hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool lower_hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool equals(const char* a, size_t a_length, const char* b) {
  return strlen(b) == a_length && memcmp(a, b, a_length) == 0;
}
bool equals(const Value* v, const char* text) {
  return tj::IsString(v) && equals(v->text, v->length, text);
}

bool is_record(const Value* v) { return tj::IsObject(v); }

// Copies `text` into the arena with ASCII letters lowered.
char* ascii_lower(Arena& arena, const char* source, size_t length) {
  char* out = static_cast<char*>(arena.Alloc(length + 1, 1));
  if (!out) return nullptr;
  for (size_t i = 0; i < length; ++i) {
    const char c = source[i];
    out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
  }
  out[length] = '\0';
  return out;
}

// /^0x[0-9a-f]{40}$/ (an ASCII-lowered address).
bool lower_address(const char* t, size_t length) {
  if (length != 42 || t[0] != '0' || t[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i)
    if (!lower_hex_digit(t[i])) return false;
  return true;
}

// `value.toLowerCase() === constant` for an ASCII constant: a non-ASCII
// character never lowercases into the hex alphabet, so ASCII lowering decides.
bool lower_equals(const char* t, size_t length, const char* constant) {
  if (length != strlen(constant)) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = t[i];
    const char l = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
    if (l != constant[i]) return false;
  }
  return true;
}

bool zeros(const char* t, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (t[i] != '0') return false;
  return true;
}

// One 64-hex-digit ABI word read as a small integer (offsets, lengths).
bool small_word(const char* w, uint64_t* out) {
  if (!zeros(w, 48)) return false;
  uint64_t value = 0;
  for (size_t i = 48; i < 64; ++i) {
    const char c = w[i];
    value = value * 16 + static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  *out = value;
  return true;
}

bool word_equals(const char* w, uint64_t expected) {
  uint64_t value = 0;
  return small_word(w, &value) && value == expected;
}

// An address word: twelve zero bytes, then the address.
bool address_word(const char* w) { return zeros(w, 24); }

// ---- router calldata (decodeRouterTrade) ---------------------------------------------------
//
// ethers decodes the calldata and re-encodes it; the two must match. That is
// exactly "the calldata is the canonical ABI encoding of some valid value",
// which these readers check directly: every head offset at its canonical
// position, every padding and high-order byte zero, nothing trailing.

struct Trade {
  const char* router = nullptr;
  char path[kMaxPath][43]{};
  size_t path_count = 0;
  bool has_amount = false;
  char amount[65]{};  // 64 hex digits
};

void copy_address(char out[43], const char* word) {
  out[0] = '0';
  out[1] = 'x';
  memcpy(out + 2, word + 24, 40);
  out[42] = '\0';
}

bool recipient_allowed(const char* recipient, const char* from, size_t from_length, bool sentinels) {
  if (equals(from, from_length, kZero)) return true;
  if (equals(from, from_length, recipient)) return true;
  return sentinels && (strcmp(recipient, kSentinelOne) == 0 || strcmp(recipient, kSentinelTwo) == 0);
}

// The lowercase hex body after "0x" + selector, as words.
struct Words {
  const char* data = nullptr;
  size_t count = 0;
  const char* at(size_t i) const { return data + i * 64; }
};

bool words_of(const char* hex, size_t length, Words* out) {
  if (length % 64) return false;
  out->data = hex;
  out->count = length / 64;
  return true;
}

bool decode_v2(const char* data, size_t length, const char* from, size_t from_length, Trade* trade) {
  const char* selector = data + 2;
  const bool eth_in = memcmp(selector, "b6f9de95", 8) == 0;
  if (!eth_in && memcmp(selector, "791ac947", 8) != 0 && memcmp(selector, "5c11d795", 8) != 0) return false;
  Words w;
  if (!words_of(data + 10, length - 10, &w)) return false;
  const size_t head = eth_in ? 4 : 5;
  const size_t path_word = eth_in ? 1 : 2;
  const size_t recipient_word = eth_in ? 2 : 3;
  if (w.count < head + 1 || !word_equals(w.at(path_word), head * 32) || !address_word(w.at(recipient_word)))
    return false;
  uint64_t count = 0;
  if (!small_word(w.at(head), &count) || count > w.count || w.count != head + 1 + count) return false;
  for (size_t i = 0; i < count; ++i)
    if (!address_word(w.at(head + 1 + i))) return false;
  char recipient[43];
  copy_address(recipient, w.at(recipient_word));
  if (!recipient_allowed(recipient, from, from_length, false)) return false;
  if (count < 2 || count > kMaxPath) return false;
  trade->path_count = count;
  for (size_t i = 0; i < count; ++i) copy_address(trade->path[i], w.at(head + 1 + i));
  trade->has_amount = !eth_in;
  if (!eth_in) {
    memcpy(trade->amount, w.at(0), 64);
    trade->amount[64] = '\0';
  }
  return true;
}

// decodeV3Path: 20-byte token, then (3-byte fee, 20-byte token) hops.
bool decode_v3_path(const char* hex, size_t length, Trade* trade) {
  if (length < 86 || (length - 40) % 46 != 0) return false;
  const size_t tokens = 1 + (length - 40) / 46;
  if (tokens > kMaxPath) return false;
  trade->path_count = tokens;
  for (size_t i = 0; i < tokens; ++i) {
    const char* token = i == 0 ? hex : hex + 40 + (i - 1) * 46 + 6;
    trade->path[i][0] = '0';
    trade->path[i][1] = 'x';
    memcpy(trade->path[i] + 2, token, 40);
    trade->path[i][42] = '\0';
  }
  return true;
}

// One SwapRouter02 inner call (lowercase hex without "0x").
bool decode_v3_inner(const char* call, size_t length, const char* from, size_t from_length, Trade* trade,
                     bool* swapped) {
  if (length < 8) return false;
  Words w;
  if (memcmp(call, "04e45aaf", 8) == 0) {  // exactInputSingle
    if (*swapped || !words_of(call + 8, length - 8, &w) || w.count != 7) return false;
    if (!address_word(w.at(0)) || !address_word(w.at(1)) || !zeros(w.at(2), 58) || !address_word(w.at(3)) ||
        !zeros(w.at(6), 24))
      return false;
    char recipient[43];
    copy_address(recipient, w.at(3));
    if (!recipient_allowed(recipient, from, from_length, true)) return false;
    trade->path_count = 2;
    copy_address(trade->path[0], w.at(0));
    copy_address(trade->path[1], w.at(1));
    trade->has_amount = true;
    memcpy(trade->amount, w.at(4), 64);
    trade->amount[64] = '\0';
    *swapped = true;
    return true;
  }
  if (memcmp(call, "b858183f", 8) == 0) {  // exactInput
    if (*swapped || !words_of(call + 8, length - 8, &w) || w.count < 6) return false;
    uint64_t bytes = 0;
    if (!word_equals(w.at(0), 32) || !word_equals(w.at(1), 128) || !address_word(w.at(2)) ||
        !small_word(w.at(5), &bytes) || bytes > w.count * 32)
      return false;
    const size_t data_words = (bytes + 31) / 32;
    if (w.count != 6 + data_words || !zeros(w.at(6) + bytes * 2, data_words * 64 - bytes * 2)) return false;
    char recipient[43];
    copy_address(recipient, w.at(2));
    if (!recipient_allowed(recipient, from, from_length, true)) return false;
    if (!decode_v3_path(w.at(6), bytes * 2, trade)) return false;
    trade->has_amount = true;
    memcpy(trade->amount, w.at(3), 64);
    trade->amount[64] = '\0';
    *swapped = true;
    return true;
  }
  if (memcmp(call, "49404b7c", 8) == 0) {  // unwrapWETH9
    if (!words_of(call + 8, length - 8, &w) || w.count != 2 || !address_word(w.at(1))) return false;
    char recipient[43];
    copy_address(recipient, w.at(1));
    return equals(from, from_length, kZero) || equals(from, from_length, recipient);
  }
  return false;
}

bool decode_v3(const char* data, size_t length, const char* from, size_t from_length, Trade* trade) {
  if (memcmp(data + 2, "5ae401dc", 8) != 0) return false;  // multicall(uint256,bytes[])
  Words w;
  if (!words_of(data + 10, length - 10, &w) || w.count < 3 || !word_equals(w.at(1), 64)) return false;
  uint64_t count = 0;
  if (!small_word(w.at(2), &count) || count < 1 || count > 2 || w.count < 3 + count) return false;
  size_t position = 3 + count;  // the first element's length word
  for (size_t i = 0; i < count; ++i) {
    uint64_t bytes = 0;
    if (!word_equals(w.at(3 + i), (position - 3) * 32) || position >= w.count ||
        !small_word(w.at(position), &bytes) || bytes > (w.count - position) * 32)
      return false;
    const size_t data_words = (bytes + 31) / 32;
    if (position + 1 + data_words > w.count ||
        !zeros(w.at(position + 1) + bytes * 2, data_words * 64 - bytes * 2))
      return false;
    position += 1 + data_words;
  }
  if (position != w.count) return false;
  bool swapped = false;
  position = 3 + count;
  for (size_t i = 0; i < count; ++i) {
    uint64_t bytes = 0;
    small_word(w.at(position), &bytes);
    if (!decode_v3_inner(w.at(position + 1), bytes * 2, from, from_length, trade, &swapped)) return false;
    position += 1 + (bytes + 31) / 32;
  }
  return swapped;
}

bool decode_router_trade(const char* router, const char* data, size_t length, const char* from, size_t from_length,
                         Trade* trade) {
  if (length < 10) return false;
  trade->router = router;
  return strcmp(router, kRouterV2) == 0 ? decode_v2(data, length, from, from_length, trade)
                                        : decode_v3(data, length, from, from_length, trade);
}

// BigInt(value) > 100n * 10n ** 18n for /^0x(?:0|[1-9a-f][0-9a-f]*)$/.
bool value_over_limit(const char* value, size_t length) {
  const size_t digits = length - 2;
  const size_t limit = strlen(kMaxValueHex);
  if (digits != limit) return digits > limit;
  return memcmp(value + 2, kMaxValueHex, limit) > 0;
}

// /^0x(?:0|[1-9a-f][0-9a-f]*)$/
bool quantity_text(const char* t, size_t length) {
  if (length < 3 || t[0] != '0' || t[1] != 'x') return false;
  if (t[2] == '0') return length == 3;
  for (size_t i = 2; i < length; ++i)
    if (!lower_hex_digit(t[i])) return false;
  return true;
}

// /^0x(?:[0-9a-f]{2})*$/
bool hex_data(const char* t, size_t length) {
  if (length < 2 || t[0] != '0' || t[1] != 'x' || length % 2) return false;
  for (size_t i = 2; i < length; ++i)
    if (!lower_hex_digit(t[i])) return false;
  return true;
}

// ---- diagnostics helpers ----------------------------------------------------------------

// quantity(value): BigInt of /^0x[0-9a-fA-F]+$/, else 0n. False when the
// value exceeds the native 640-bit integer.
bool quantity(const Value* v, BigInt* out) {
  *out = BigInt();
  if (!tj::IsString(v) || v->length < 3 || v->text[0] != '0' || v->text[1] != 'x') return true;
  for (size_t i = 2; i < v->length; ++i)
    if (!hex_digit(v->text[i])) return true;
  return BigInt::ParseHex(v->text + 2, v->length - 2, out);
}

Value* decimal_value(Arena& arena, const BigInt& value) {
  char digits[256];
  if (!value.ToDecimal(digits, sizeof(digits))) return nullptr;
  return tj::Str(arena, digits);
}

// text(value): a string of at most 256 UTF-16 units, else null.
Value* short_text(Arena& arena, const Value* v) {
  if (tj::IsString(v) && text::Utf16Length(v->text, v->length) <= 256) return const_cast<Value*>(v);
  return tj::Null(arena);
}

Value* str(Arena& arena, const char* t) { return tj::Str(arena, t); }

bool set(Arena& arena, Value* object, const char* key, Value* value) {
  return value && tj::Set(arena, object, key, value);
}

const char* call_text(const Value* call, const char* key, size_t* length) {
  const Value* v = tj::Get(call, key);
  if (!tj::IsString(v)) return nullptr;
  *length = v->length;
  return v->text;
}

// The symbol a stock or known token shows, as a Value (nullptr when unknown).
Value* token_symbol(Arena& arena, const StockSet& stocks, const char* lower, int* decimals) {
  if (const Value* symbol = stocks.Symbol(lower, 42)) {
    if (decimals) *decimals = 18;
    return const_cast<Value*>(symbol);
  }
  int known = 0;
  if (const char* symbol = KnownTokenSymbol(lower, 42, &known)) {
    if (decimals) *decimals = known;
    return str(arena, symbol);
  }
  return nullptr;
}

const char* call_kind(const char* to_lower, const char* data, size_t data_length) {
  if (data_length >= 10 && lower_equals(data + 2, 8, kApprove)) return "approval";
  if (IsRouter(to_lower, strlen(to_lower))) return "swap";
  return "contract-call";
}

Value* call_explanation(Arena& arena, const StockSet& stocks, const char* kind, const char* to_lower) {
  if (strcmp(kind, "approval") == 0) {
    const Value* symbol = token_symbol(arena, stocks, to_lower, nullptr);
    const char* prefix = "Allow the selected DEX router to spend ";
    const size_t length = strlen(prefix) + (symbol ? symbol->length : 5) + 1;
    char* out = static_cast<char*>(arena.Alloc(length + 1, 1));
    if (!out) return nullptr;
    size_t at = strlen(prefix);
    std::copy_n(prefix, at, out);
    if (symbol) { memcpy(out + at, symbol->text, symbol->length); at += symbol->length; }
    else { std::copy_n("token", 5, out + at); at += 5; }
    out[at++] = '.';
    return tj::Str(arena, out, at);
  }
  if (strcmp(kind, "swap") == 0) return str(arena, "Swap through a registered Robinhood Chain DEX router.");
  char buffer[80];
  snprintf(buffer, sizeof(buffer), "Call contract %s.", to_lower);
  return str(arena, buffer);
}

// extractApprovals(calls, stocks).
Value* extract_approvals(Arena& arena, const Value* calls, const StockSet& stocks) {
  Value* out = tj::Arr(arena);
  if (!out) return nullptr;
  for (uint32_t i = 0; i < calls->count; ++i) {
    const Value* call = calls->items[i];
    size_t data_length = 0, to_length = 0;
    const char* data = call_text(call, "data", &data_length);
    const char* to = call_text(call, "to", &to_length);
    if (!data || !to || data_length != 138 || !lower_equals(data + 2, 8, kApprove)) continue;
    char* token = ascii_lower(arena, to, to_length);
    char* lowered = ascii_lower(arena, data, data_length);
    if (!token || !lowered) return nullptr;
    char spender[43];
    spender[0] = '0';
    spender[1] = 'x';
    memcpy(spender + 2, lowered + 34, 40);
    spender[42] = '\0';
    BigInt amount;
    if (!BigInt::ParseHex(lowered + 74, 64, &amount)) return nullptr;
    const Value* symbol = token_symbol(arena, stocks, token, nullptr);
    // amount >= 2^255 (which includes MAX_UINT256): the top hex digit is 8-f.
    const bool unlimited = lowered[74] >= '8';
    Value* approval = tj::Obj(arena);
    if (!approval || !set(arena, approval, "callIndex", tj::Num(arena, i)) ||
        !set(arena, approval, "tokenAddress", str(arena, token)) ||
        !set(arena, approval, "tokenSymbol", symbol ? const_cast<Value*>(symbol) : tj::Null(arena)) ||
        !set(arena, approval, "spender", str(arena, spender)) ||
        !set(arena, approval, "rawAmount", decimal_value(arena, amount)) ||
        !set(arena, approval, "unlimited", tj::Bool(arena, unlimited)) ||
        !set(arena, approval, "trustedSpender", tj::Bool(arena, IsRouter(spender, 42))) || !tj::Push(arena, out, approval))
      return nullptr;
  }
  return out;
}

// topicAddress(value): the low 20 bytes of a 32-byte topic, lowercase.
bool topic_address(const Value* v, char out[43]) {
  if (!tj::IsString(v) || v->length != 66 || v->text[0] != '0' || v->text[1] != 'x') return false;
  for (size_t i = 2; i < 66; ++i)
    if (!hex_digit(v->text[i])) return false;
  out[0] = '0';
  out[1] = 'x';
  for (size_t i = 0; i < 40; ++i) {
    const char c = v->text[26 + i];
    out[2 + i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
  }
  out[42] = '\0';
  return true;
}

struct Change {
  char contract[43];
  BigInt amount;
};

// extractBalanceChanges(logs, owner, stocks). `*overflow` reports an amount
// beyond the native integer bound.
Value* extract_balance_changes(Arena& arena, const Value* const* logs, size_t log_count, const char* owner,
                               const StockSet& stocks, bool* overflow) {
  Change* changes = log_count ? static_cast<Change*>(arena.Alloc(sizeof(Change) * log_count)) : nullptr;
  if (log_count && !changes) return nullptr;
  size_t change_count = 0;
  for (size_t n = 0; n < log_count; ++n) {
    const Value* log = logs[n];
    const Value* topics = tj::Get(log, "topics");
    if (!is_record(log) || !tj::IsArray(topics) || topics->count < 3) continue;
    // String(topics[0]).toLowerCase() !== TRANSFER_TOPIC.
    Value* first = cross_chain::JsString(arena, topics->items[0]);
    if (!first) return nullptr;
    if (!lower_equals(first->text, first->length, kTransferTopic)) continue;
    char source[43], destination[43];
    const bool has_source = topic_address(topics->items[1], source);
    const bool has_destination = topic_address(topics->items[2], destination);
    BigInt amount;
    // An amount beyond the native bound is nonzero; it fails the report
    // only when the log would change the owner's balance.
    const bool exact = quantity(tj::Get(log, "data"), &amount);
    const Value* address = tj::Get(log, "address");
    char contract[43];
    if (!tj::IsString(address) || address->length != 42) continue;
    for (size_t i = 0; i < 42; ++i) {
      const char c = address->text[i];
      contract[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
    }
    contract[42] = '\0';
    if (!lower_address(contract, 42) || (exact && amount.IsZero())) continue;
    const bool debit = has_source && strcmp(source, owner) == 0;
    const bool credit = has_destination && strcmp(destination, owner) == 0;
    if (!debit && !credit) continue;
    if (!exact) { *overflow = true; return nullptr; }
    size_t index = change_count;
    for (size_t k = 0; k < change_count; ++k)
      if (strcmp(changes[k].contract, contract) == 0) { index = k; break; }
    if (index == change_count) {
      memcpy(changes[index].contract, contract, 43);
      changes[index].amount = BigInt();
      ++change_count;
    }
    if (debit && !changes[index].amount.Sub(amount)) { *overflow = true; return nullptr; }
    if (credit && !changes[index].amount.Add(amount)) { *overflow = true; return nullptr; }
  }
  Value* out = tj::Arr(arena);
  if (!out) return nullptr;
  for (size_t k = 0; k < change_count; ++k) {
    const Change& change = changes[k];
    if (change.amount.IsZero()) continue;
    const bool native = strcmp(change.contract, kNative) == 0;
    int decimals = 0;
    const Value* symbol = native ? str(arena, "ETH") : token_symbol(arena, stocks, change.contract, &decimals);
    if (native) decimals = 18;
    Value* entry = tj::Obj(arena);
    if (!entry || !set(arena, entry, "assetType", str(arena, native ? "native" : "erc20")) ||
        !set(arena, entry, "contractAddress", native ? tj::Null(arena) : str(arena, change.contract)) ||
        !set(arena, entry, "symbol", symbol ? const_cast<Value*>(symbol) : tj::Null(arena)) ||
        !set(arena, entry, "decimals", symbol ? tj::Num(arena, decimals) : tj::Null(arena)) ||
        !set(arena, entry, "rawAmount", decimal_value(arena, change.amount.Abs())) ||
        !set(arena, entry, "direction", str(arena, change.amount.negative() ? "debit" : "credit")) ||
        !set(arena, entry, "stockToken", tj::Bool(arena, stocks.Has(change.contract, 42))) || !tj::Push(arena, out, entry))
      return nullptr;
  }
  return out;
}

struct TraceState {
  Arena& arena;
  Value* out;
  bool ok = true;
  bool overflow = false;
};

// extractInternalCalls: depth-first, at most 40 frames below the roots.
void visit_frame(TraceState& state, const Value* value, uint32_t call_index, uint32_t depth) {
  if (!state.ok || !is_record(value) || state.out->count >= 40) return;
  Arena& arena = state.arena;
  if (depth > 0) {
    const Value* type = tj::Get(value, "type");
    BigInt amount, gas;
    if (!quantity(tj::Get(value, "value"), &amount) || !quantity(tj::Get(value, "gasUsed"), &gas)) {
      state.ok = false;
      state.overflow = true;
      return;
    }
    Value* frame = tj::Obj(arena);
    state.ok = frame && set(arena, frame, "callIndex", tj::Num(arena, call_index)) &&
               set(arena, frame, "depth", tj::Num(arena, depth)) &&
               set(arena, frame, "type", tj::IsString(type) ? const_cast<Value*>(type) : str(arena, "CALL")) &&
               set(arena, frame, "from", short_text(arena, tj::Get(value, "from"))) &&
               set(arena, frame, "to", short_text(arena, tj::Get(value, "to"))) &&
               set(arena, frame, "value", decimal_value(arena, amount)) &&
               set(arena, frame, "gasUsed", decimal_value(arena, gas)) &&
               set(arena, frame, "error", short_text(arena, tj::Get(value, "error"))) && tj::Push(arena, state.out, frame);
    if (!state.ok) return;
  }
  const Value* calls = tj::Get(value, "calls");
  if (tj::IsArray(calls))
    for (uint32_t i = 0; i < calls->count; ++i) visit_frame(state, calls->items[i], call_index, depth + 1);
}

Value* extract_internal_calls(Arena& arena, const Value* trace, bool* overflow) {
  Value* out = tj::Arr(arena);
  if (!out) return nullptr;
  if (!tj::IsArray(trace) || !trace->count || !tj::IsArray(trace->items[0])) return out;
  const Value* roots = trace->items[0];
  TraceState state{arena, out};
  for (uint32_t i = 0; i < roots->count && state.ok; ++i) visit_frame(state, roots->items[i], i, 0);
  if (state.overflow) *overflow = true;
  return state.ok ? out : nullptr;
}

Value* report_head(Arena& arena, const DiagnosticsRequest& request) {
  Value* report = tj::Obj(arena);
  char iso[40];
  const bool ok = report && text::IsoString(request.now_ms, iso) && set(arena, report, "available", tj::Bool(arena, true)) &&
                  set(arena, report, "network", str(arena, "robinhood")) &&
                  set(arena, report, "chainId", tj::Num(arena, kChainId)) &&
                  set(arena, report, "provider", str(arena, "alchemy")) && set(arena, report, "simulatedAt", str(arena, iso));
  return ok ? report : nullptr;
}

}  // namespace

// ---- tables -------------------------------------------------------------------------------

bool IsRouter(const char* address, size_t length) {
  return equals(address, length, kRouterV2) || equals(address, length, kRouterV3);
}

bool IsFundingToken(const char* address, size_t length) {
  return equals(address, length, kWeth) || equals(address, length, kUsdg);
}

const char* KnownTokenSymbol(const char* address, size_t length, int* decimals) {
  if (equals(address, length, kWeth)) { *decimals = 18; return "WETH"; }
  if (equals(address, length, kUsdg)) { *decimals = 6; return "USDG"; }
  return nullptr;
}

bool StockSet::Has(const char* lower, size_t length) const {
  for (size_t i = 0; i < count; ++i)
    if (equals(lower, length, items[i].address)) return true;
  return false;
}

const Value* StockSet::Symbol(const char* lower, size_t length) const {
  for (size_t i = count; i-- > 0;)
    if (equals(lower, length, items[i].address)) return items[i].symbol;
  return nullptr;
}

const char* StockLogoUrl(Arena& arena, const char* symbol, size_t length) {
  size_t begin = 0, end = 0;
  text::Trim(symbol, length, &begin, &end);
  const Value* upper = text::ToUpperCase(arena, symbol + begin, end - begin);
  if (!upper || upper->length < 1 || upper->length > 32) return nullptr;
  const char* t = upper->text;
  for (size_t i = 0; i < upper->length; ++i) {
    const char c = t[i];
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!(alnum || (i > 0 && (c == '.' || c == '-')))) return nullptr;
  }
  char url[128];
  snprintf(url, sizeof(url), "https://financialmodelingprep.com/image-stock/%.*s.png", static_cast<int>(upper->length), t);
  return arena.CopyText(url, strlen(url));
}

// ---- validateTradeCalls ----------------------------------------------------------------------

Value* ValidateTradeCalls(Arena& arena, const Value* calls, const StockSet& stocks, const char* from,
                          size_t from_length, bool* exhausted) {
  *exhausted = false;
  if (!tj::IsArray(calls) || calls->count < 1 || calls->count > kMaxCalls || stocks.count == 0) return nullptr;
  size_t router_calls = 0;
  bool selected_stock = false;
  Trade trade;
  bool has_trade = false;
  struct Approval {
    const char* token;
    char spender[43];
    char amount[65];
  } approvals[kMaxCalls];
  size_t approval_count = 0;
  Value* normalized = tj::Arr(arena);
  if (!normalized) { *exhausted = true; return nullptr; }
  for (uint32_t i = 0; i < calls->count; ++i) {
    const Value* call = calls->items[i];
    size_t to_length = 0, data_length = 0, value_length = 0;
    const char* to_raw = call_text(call, "to", &to_length);
    const char* data_raw = call_text(call, "data", &data_length);
    const Value* value_raw = tj::Get(call, "value");
    if (!to_raw || !data_raw) return nullptr;
    char* to = ascii_lower(arena, to_raw, to_length);
    char* data = ascii_lower(arena, data_raw, data_length);
    char* value = tj::IsString(value_raw) ? ascii_lower(arena, value_raw->text, value_raw->length)
                                          : arena.CopyText("0x0", 3);
    if (!to || !data || !value) { *exhausted = true; return nullptr; }
    value_length = strlen(value);
    if (!lower_address(to, to_length) || !hex_data(data, data_length) || data_length > kMaxDataChars ||
        !quantity_text(value, value_length))
      return nullptr;
    if (value_over_limit(value, value_length)) return nullptr;
    if (IsRouter(to, to_length)) {
      ++router_calls;
      Trade decoded;
      if (!decode_router_trade(strcmp(to, kRouterV2) == 0 ? kRouterV2 : kRouterV3, data, data_length, from,
                               from_length, &decoded))
        return nullptr;
      trade = decoded;
      has_trade = true;
      for (size_t k = 0; k < decoded.path_count; ++k)
        if (stocks.Has(decoded.path[k], 42)) selected_stock = true;
    } else if (stocks.Has(to, to_length) || IsFundingToken(to, to_length)) {
      if (strcmp(value, "0x0") != 0) return nullptr;
      if (data_length != 138 || memcmp(data + 2, kApprove, 8) != 0) return nullptr;
      Approval& approval = approvals[approval_count++];
      approval.token = to;
      approval.spender[0] = '0';
      approval.spender[1] = 'x';
      memcpy(approval.spender + 2, data + 34, 40);
      approval.spender[42] = '\0';
      if (!IsRouter(approval.spender, 42)) return nullptr;
      memcpy(approval.amount, data + 74, 64);
      approval.amount[64] = '\0';
      if (stocks.Has(to, to_length)) selected_stock = true;
    } else {
      return nullptr;
    }
    Value* entry = tj::Obj(arena);
    if (!entry || !set(arena, entry, "to", tj::Str(arena, to, to_length)) ||
        !set(arena, entry, "data", tj::Str(arena, data, data_length)) ||
        !set(arena, entry, "value", tj::Str(arena, value, value_length)) || !tj::Push(arena, normalized, entry)) {
      *exhausted = true;
      return nullptr;
    }
  }
  if (router_calls != 1 || !selected_stock) return nullptr;
  for (size_t i = 0; i < approval_count; ++i) {
    const Approval& approval = approvals[i];
    if (!has_trade || !trade.has_amount || strcmp(approval.token, trade.path[0]) != 0 ||
        strcmp(approval.spender, trade.router) != 0 || strcmp(approval.amount, trade.amount) != 0)
      return nullptr;
  }
  return normalized;
}

// ---- prepared calls -----------------------------------------------------------------------

bool ValidatePrepared(Arena& arena, const Value* prepared, const char* from_lower, Value** requests,
                      size_t* node_count) {
  if (!is_record(prepared)) return false;
  const Value* const* nodes = &prepared;
  size_t count = 1;
  if (equals(tj::Get(prepared, "type"), "array")) {
    const Value* data = tj::Get(prepared, "data");
    if (!tj::IsArray(data)) return false;
    nodes = data->items;
    count = data->count;
  }
  if (count < 1 || count > kMaxCalls) return false;
  Value* out = tj::Arr(arena);
  if (!out) return false;
  for (size_t i = 0; i < count; ++i) {
    const Value* node = nodes[i];
    const Value* request = tj::Get(node, "signatureRequest");
    if (!is_record(node) || !equals(tj::Get(node, "chainId"), kChainIdHex) || !is_record(request)) return false;
    const Value* type = tj::Get(request, "type");
    if (!equals(type, "personal_sign") && !equals(type, "eth_signAuthorization")) return false;
    const Value* raw = tj::Get(request, "rawPayload");
    if (!tj::IsString(raw) || raw->length != 66 || raw->text[0] != '0' || raw->text[1] != 'x') return false;
    for (size_t k = 2; k < 66; ++k)
      if (!hex_digit(raw->text[k])) return false;
    if (equals(tj::Get(node, "type"), "user-operation-v070")) {
      const Value* data = tj::Get(node, "data");
      const Value* sender = tj::Get(data, "sender");
      if (!is_record(data) || !tj::IsString(sender) || !lower_equals(sender->text, sender->length, from_lower))
        return false;
    }
    if (!tj::Push(arena, out, const_cast<Value*>(request))) return false;
  }
  *requests = out;
  *node_count = count;
  return true;
}

namespace {

// attachNode: { ...rest-without-signatureRequest, signature }.
Value* attach_node(Arena& arena, const Value* node, const Value* signature) {
  if (!is_record(node)) return nullptr;
  Value* out = tj::Obj(arena);
  if (!out) return nullptr;
  for (uint32_t i = 0; i < node->count; ++i) {
    const tj::Member& member = node->members[i];
    if (member.key_length == 16 && memcmp(member.key, "signatureRequest", 16) == 0) continue;
    if (!tj::Set(arena, out, member.key, member.key_length, member.value)) return nullptr;
  }
  Value* value = tj::Obj(arena);
  if (!value || !set(arena, value, "type", str(arena, "secp256k1")) ||
      !set(arena, value, "data", const_cast<Value*>(signature)) || !tj::Set(arena, out, "signature", value))
    return nullptr;
  return out;
}

}  // namespace

Value* AttachSignatures(Arena& arena, const Value* prepared, const Value* const* signatures, size_t count) {
  if (!is_record(prepared) || count == 0) return nullptr;
  if (!equals(tj::Get(prepared, "type"), "array")) return attach_node(arena, prepared, signatures[0]);
  const Value* data = tj::Get(prepared, "data");
  if (!tj::IsArray(data) || data->count > count) return nullptr;
  Value* list = tj::Arr(arena);
  Value* out = tj::Obj(arena);
  if (!list || !out) return nullptr;
  for (uint32_t i = 0; i < data->count; ++i) {
    Value* node = attach_node(arena, data->items[i], signatures[i]);
    if (!node || !tj::Push(arena, list, node)) return nullptr;
  }
  for (uint32_t i = 0; i < prepared->count; ++i) {
    const tj::Member& member = prepared->members[i];
    if (!tj::Set(arena, out, member.key, member.key_length, member.value)) return nullptr;
  }
  return tj::Set(arena, out, "data", list) ? out : nullptr;
}

// ---- JSON-RPC replies ------------------------------------------------------------------------

RpcReply ReadRpcReply(Arena& arena, const Value* body, const char* method, Value** result, Value** message,
                      const Value** data) {
  *result = nullptr;
  *message = nullptr;
  *data = nullptr;
  // `body.error` / `body.result` on a primitive or an array read undefined;
  // on null they throw.
  if (!body || body->type == tj::Type::kNull) return RpcReply::kInternal;
  if (!is_record(body)) return RpcReply::kInternal;
  bool has_error = false;
  for (uint32_t i = 0; i < body->count; ++i)
    if (body->members[i].key_length == 5 && memcmp(body->members[i].key, "error", 5) == 0) has_error = true;
  if (has_error) {
    const Value* error = tj::Get(body, "error");
    if (!error || error->type == tj::Type::kNull) return RpcReply::kInternal;
    const Value* text_value = is_record(error) ? tj::Get(error, "message") : nullptr;
    if (tj::IsString(text_value)) {
      *message = text::Slice(arena, text_value->text, text_value->length, 0, 240);
    } else {
      char fallback[96];
      snprintf(fallback, sizeof(fallback), "%s rejected the request", method);
      *message = str(arena, fallback);
    }
    if (!*message) return RpcReply::kInternal;
    *data = is_record(error) ? tj::Get(error, "data") : nullptr;
    return RpcReply::kRpcError;
  }
  bool has_result = false;
  for (uint32_t i = 0; i < body->count; ++i)
    if (body->members[i].key_length == 6 && memcmp(body->members[i].key, "result", 6) == 0) has_result = true;
  if (!has_result) return RpcReply::kInternal;
  *result = tj::Get(body, "result");
  return RpcReply::kResult;
}

// ---- revert reasons --------------------------------------------------------------------------

Value* DecodeRevertReason(Arena& arena, const Value* value) {
  if (!tj::IsString(value) || value->length < 2 || value->text[0] != '0' || value->text[1] != 'x') return nullptr;
  for (size_t i = 2; i < value->length; ++i)
    if (!hex_digit(value->text[i])) return nullptr;
  char* data = ascii_lower(arena, value->text, value->length);
  if (!data) return nullptr;
  const size_t length = value->length;
  const bool error = length >= 10 && memcmp(data, "0x08c379a0", 10) == 0;
  const bool panic = length >= 10 && memcmp(data, "0x4e487b71", 10) == 0;
  if (!error && !panic) return nullptr;
  const char* body = data + 10;
  const size_t hex = length - 10;
  if (hex % 2) return nullptr;
  const size_t bytes = hex / 2;
  if (bytes < 32) return nullptr;
  if (panic) {
    BigInt code;
    char digits[80];
    if (!BigInt::ParseHex(body, 64, &code) || !code.ToHex(digits, sizeof(digits))) return nullptr;
    char message[128];
    snprintf(message, sizeof(message), "Contract panic (0x%s).", digits);
    return str(arena, message);
  }
  // AbiCoder.decode(['string'], …): the offset word, then the length word and
  // the padded bytes at that offset (strict: no loose padding), then strict
  // UTF-8 (toUtf8String throws on invalid sequences).
  constexpr uint64_t kMaxSafe = 9007199254740991ULL;
  uint64_t offset = 0, size = 0;
  if (!small_word(body, &offset) || offset > kMaxSafe || offset > bytes || bytes - offset < 32) return nullptr;
  const char* sub = body + offset * 2;
  const size_t sub_bytes = bytes - offset;
  if (!small_word(sub, &size) || size > kMaxSafe || 32 + ((size + 31) / 32) * 32 > sub_bytes) return nullptr;
  uint8_t* raw = static_cast<uint8_t*>(arena.Alloc(size + 1, 1));
  if (!raw) return nullptr;
  for (size_t i = 0; i < size; ++i) {
    const char hi = sub[64 + i * 2], lo = sub[65 + i * 2];
    raw[i] = static_cast<uint8_t>(((hi <= '9' ? hi - '0' : hi - 'a' + 10) << 4) | (lo <= '9' ? lo - '0' : lo - 'a' + 10));
  }
  if (!hb::ValidUtf8(raw, size)) return nullptr;
  return text::Slice(arena, reinterpret_cast<const char*>(raw), size, 0, 240);
}

// ---- reports ----------------------------------------------------------------------------------

Value* SimulationCalls(Arena& arena, const DiagnosticsRequest& request) {
  Value* out = tj::Arr(arena);
  char* from = ascii_lower(arena, request.from, request.from_length);
  if (!out || !from) return nullptr;
  for (uint32_t i = 0; i < request.executable->count; ++i) {
    const Value* call = request.executable->items[i];
    const Value* input = i < request.calls->count ? request.calls->items[i] : nullptr;
    Value* entry = tj::Obj(arena);
    if (!entry || !set(arena, entry, "from", str(arena, from)) || !set(arena, entry, "to", tj::Get(call, "to")) ||
        !set(arena, entry, "data", tj::Get(call, "data")) || !set(arena, entry, "value", tj::Get(call, "value")))
      return nullptr;
    static const char* const kFees[] = {"gas", "maxFeePerGas", "maxPriorityFeePerGas"};
    for (const char* key : kFees) {
      Value* fee = tj::Get(input, key);
      if (tj::IsString(fee) && !tj::Set(arena, entry, key, fee)) return nullptr;
    }
    if (!tj::Push(arena, out, entry)) return nullptr;
  }
  return out;
}

Value* BuildReport(Arena& arena, const DiagnosticsRequest& request, const Value* simulation, const Value* trace) {
  const StockSet& stocks = *request.stocks;
  const Value* block = tj::IsArray(simulation) && simulation->count && is_record(simulation->items[0])
                           ? simulation->items[0]
                           : nullptr;
  const Value* simulated = block ? tj::Get(block, "calls") : nullptr;
  if (!tj::IsArray(simulated)) simulated = nullptr;
  Value* calls = tj::Arr(arena);
  if (!calls) return nullptr;
  BigInt gas_total, fee_total;
  BigInt* gas_used = request.calls->count
                         ? static_cast<BigInt*>(arena.Alloc(sizeof(BigInt) * request.calls->count))
                         : nullptr;
  if (request.calls->count && !gas_used) return nullptr;
  const Value* first_failed_reason = nullptr;
  bool would_revert = false;
  for (uint32_t i = 0; i < request.calls->count; ++i) {
    const Value* call = request.calls->items[i];
    const Value* result = simulated && i < simulated->count && is_record(simulated->items[i]) ? simulated->items[i]
                                                                                             : nullptr;
    BigInt status;
    gas_used[i] = BigInt();
    // A status beyond the native bound is not 1n.
    const bool success = quantity(tj::Get(result, "status"), &status) && status.Compare(BigInt::FromU64(1)) == 0;
    if (!quantity(tj::Get(result, "gasUsed"), &gas_used[i])) return nullptr;
    Value* reason = nullptr;
    if (!success) {
      reason = DecodeRevertReason(arena, tj::Get(result, "returnData"));
      if (!reason) reason = short_text(arena, tj::Get(result, "error"));
    } else {
      reason = tj::Null(arena);
    }
    size_t to_length = 0, data_length = 0;
    const char* to_raw = call_text(call, "to", &to_length);
    const char* data = call_text(call, "data", &data_length);
    char* to = to_raw ? ascii_lower(arena, to_raw, to_length) : nullptr;
    if (!to || !data) return nullptr;
    const char* kind = call_kind(to, data, data_length);
    Value* entry = tj::Obj(arena);
    if (!entry || !set(arena, entry, "index", tj::Num(arena, i)) || !set(arena, entry, "kind", str(arena, kind)) ||
        !set(arena, entry, "target", str(arena, to)) ||
        !set(arena, entry, "explanation", call_explanation(arena, stocks, kind, to)) ||
        !set(arena, entry, "success", tj::Bool(arena, success)) ||
        !set(arena, entry, "gasUsed", decimal_value(arena, gas_used[i])) || !set(arena, entry, "revertReason", reason) ||
        !tj::Push(arena, calls, entry))
      return nullptr;
    if (!success && !would_revert) {
      would_revert = true;
      first_failed_reason = reason;
    }
    BigInt fee;
    if (!quantity(tj::Get(call, "maxFeePerGas"), &fee) || !fee.Mul(gas_used[i]) || !gas_total.Add(gas_used[i]) ||
        !fee_total.Add(fee))
      return nullptr;
  }
  // logs: every simulated call's logs, in order.
  size_t log_count = 0;
  if (simulated)
    for (uint32_t i = 0; i < simulated->count; ++i) {
      const Value* logs = tj::Get(simulated->items[i], "logs");
      if (is_record(simulated->items[i]) && tj::IsArray(logs)) log_count += logs->count;
    }
  const Value** logs = log_count ? static_cast<const Value**>(arena.Alloc(sizeof(Value*) * log_count)) : nullptr;
  if (log_count && !logs) return nullptr;
  size_t at = 0;
  if (simulated)
    for (uint32_t i = 0; i < simulated->count; ++i) {
      const Value* list = tj::Get(simulated->items[i], "logs");
      if (!is_record(simulated->items[i]) || !tj::IsArray(list)) continue;
      for (uint32_t k = 0; k < list->count; ++k) {
        if (at >= log_count) return nullptr;
        logs[at++] = list->items[k];
      }
    }
  char owner[43];
  if (request.from_length != 42) return nullptr;
  for (size_t i = 0; i < 42; ++i) {
    const char c = request.from[i];
    owner[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
  }
  owner[42] = '\0';
  bool overflow = false;
  Value* changes = extract_balance_changes(arena, logs, log_count, owner, stocks, &overflow);
  Value* approvals = changes ? extract_approvals(arena, request.calls, stocks) : nullptr;
  Value* warnings = tj::Arr(arena);
  if (!changes || !approvals || !warnings) return nullptr;
  for (uint32_t i = 0; i < approvals->count; ++i) {
    const Value* approval = approvals->items[i];
    const bool trusted = tj::Get(approval, "trustedSpender")->type == tj::Type::kTrue;
    const bool unlimited = tj::Get(approval, "unlimited")->type == tj::Type::kTrue;
    if (trusted && !unlimited) continue;
    Value* warning = tj::Obj(arena);
    char message[160]{};
    if (!trusted) {
      const Value* spender = tj::Get(approval, "spender");
      snprintf(message, sizeof(message), "Approval gives %.*s permission to spend this token.",
               static_cast<int>(spender->length), spender->text);
    }
    if (!warning || !set(arena, warning, "severity", str(arena, "danger")) ||
        !set(arena, warning, "code", str(arena, trusted ? "UNLIMITED_TOKEN_APPROVAL" : "UNTRUSTED_APPROVAL_SPENDER")) ||
        !set(arena, warning, "message",
             str(arena, trusted ? "This call grants an unlimited token approval. Use an exact amount instead." : message)) ||
        !tj::Push(arena, warnings, warning))
      return nullptr;
  }
  Value* internal = extract_internal_calls(arena, trace, &overflow);
  Value* received = tj::Arr(arena);
  if (!internal || !received) return nullptr;
  for (uint32_t i = 0; i < changes->count; ++i) {
    Value* change = changes->items[i];
    if (tj::Get(change, "stockToken")->type == tj::Type::kTrue && equals(tj::Get(change, "direction"), "credit") &&
        !tj::Push(arena, received, change))
      return nullptr;
  }
  const bool dangerous = warnings->count > 0;
  Value* report = report_head(arena, request);
  const Value* number = block ? tj::Get(block, "number") : nullptr;
  if (!report || !set(arena, report, "blockNumber", tj::IsString(number) ? const_cast<Value*>(number) : tj::Null(arena)) ||
      !set(arena, report, "wouldRevert", tj::Bool(arena, would_revert)) ||
      !set(arena, report, "canSign", tj::Bool(arena, !would_revert && !dangerous)) ||
      !set(arena, report, "revertReason", would_revert ? const_cast<Value*>(first_failed_reason) : tj::Null(arena)) ||
      !set(arena, report, "calls", calls) || !set(arena, report, "balanceChanges", changes) ||
      !set(arena, report, "approvals", approvals) || !set(arena, report, "estimatedStockTokensReceived", received) ||
      !set(arena, report, "warnings", warnings) || !set(arena, report, "traceAvailable", tj::Bool(arena, trace != nullptr)) ||
      !set(arena, report, "internalCalls", internal))
    return nullptr;
  Value* fee = tj::Obj(arena);
  Value* per_call = tj::Arr(arena);
  if (!fee || !per_call || !set(arena, fee, "gasUsed", decimal_value(arena, gas_total)) ||
      !set(arena, fee, "estimatedMaxFeeWei", decimal_value(arena, fee_total)) ||
      !set(arena, fee, "paidBy", str(arena, request.sponsored ? "sponsor" : "wallet")))
    return nullptr;
  for (uint32_t i = 0; i < request.calls->count; ++i) {
    BigInt product;
    if (!quantity(tj::Get(request.calls->items[i], "maxFeePerGas"), &product) || !product.Mul(gas_used[i])) return nullptr;
    Value* entry = tj::Obj(arena);
    if (!entry || !set(arena, entry, "index", tj::Num(arena, i)) ||
        !set(arena, entry, "gasUsed", decimal_value(arena, gas_used[i])) ||
        !set(arena, entry, "estimatedMaxFeeWei", decimal_value(arena, product)) || !tj::Push(arena, per_call, entry))
      return nullptr;
  }
  if (!set(arena, fee, "perCall", per_call) || !set(arena, report, "fee", fee)) return nullptr;
  return overflow ? nullptr : report;
}

Value* FailedReport(Arena& arena, const DiagnosticsRequest& request, const Value* reason) {
  const StockSet& stocks = *request.stocks;
  Value* approvals = extract_approvals(arena, request.calls, stocks);
  Value* calls = tj::Arr(arena);
  Value* report = report_head(arena, request);
  if (!approvals || !calls || !report) return nullptr;
  for (uint32_t i = 0; i < request.calls->count; ++i) {
    const Value* call = request.calls->items[i];
    size_t to_length = 0, data_length = 0;
    const char* to_raw = call_text(call, "to", &to_length);
    const char* data = call_text(call, "data", &data_length);
    char* to = to_raw ? ascii_lower(arena, to_raw, to_length) : nullptr;
    if (!to || !data) return nullptr;
    const char* kind = call_kind(to, data, data_length);
    Value* entry = tj::Obj(arena);
    if (!entry || !set(arena, entry, "index", tj::Num(arena, i)) || !set(arena, entry, "kind", str(arena, kind)) ||
        !set(arena, entry, "target", str(arena, to)) ||
        !set(arena, entry, "explanation", call_explanation(arena, stocks, kind, to)) ||
        !set(arena, entry, "success", tj::Bool(arena, false)) || !set(arena, entry, "gasUsed", str(arena, "0")) ||
        !set(arena, entry, "revertReason", i == 0 && reason ? const_cast<Value*>(reason) : tj::Null(arena)) ||
        !tj::Push(arena, calls, entry))
      return nullptr;
  }
  Value* fee = tj::Obj(arena);
  if (!fee || !set(arena, report, "blockNumber", tj::Null(arena)) || !set(arena, report, "wouldRevert", tj::Bool(arena, true)) ||
      !set(arena, report, "canSign", tj::Bool(arena, false)) ||
      !set(arena, report, "revertReason",
           reason ? const_cast<Value*>(reason) : str(arena, "Simulation failed before broadcast.")) ||
      !set(arena, report, "calls", calls) || !set(arena, report, "balanceChanges", tj::Arr(arena)) ||
      !set(arena, report, "approvals", approvals) || !set(arena, report, "estimatedStockTokensReceived", tj::Arr(arena)) ||
      !set(arena, report, "warnings", tj::Arr(arena)) || !set(arena, report, "traceAvailable", tj::Bool(arena, false)) ||
      !set(arena, report, "internalCalls", tj::Arr(arena)) || !set(arena, fee, "gasUsed", str(arena, "0")) ||
      !set(arena, fee, "estimatedMaxFeeWei", str(arena, "0")) ||
      !set(arena, fee, "paidBy", str(arena, request.sponsored ? "sponsor" : "wallet")) ||
      !set(arena, fee, "perCall", tj::Arr(arena)) || !set(arena, report, "fee", fee))
    return nullptr;
  return report;
}

bool EncodeSponsoredCapabilities(bool enabled, char* json, size_t json_capacity, size_t* json_length,
                                 uint8_t* proto, size_t proto_capacity, size_t* proto_length) {
  if (!json || !json_length || !proto || !proto_length) return false;
  const char* flag = enabled ? "true" : "false";
  // JSON.stringify of the capabilities() object, in its key order; `reason`
  // is spread in only when disabled.
  const int written = snprintf(json, json_capacity,
                               "{\"network\":\"robinhood\",\"chainId\":%u,\"enabled\":%s,\"gasless\":%s,"
                               "\"sponsoredGas\":%s,\"batchedCalls\":%s,\"maxCalls\":%u,"
                               "\"accountStandard\":\"eip-7702\"%s%s%s}",
                               static_cast<unsigned>(kChainId), flag, flag, flag, flag,
                               static_cast<unsigned>(kMaxCalls), enabled ? "" : ",\"reason\":\"",
                               enabled ? "" : kSponsorshipDisabledReason, enabled ? "" : "\"");
  if (written < 0 || static_cast<size_t>(written) >= json_capacity) return false;
  // toBinary(create(RobinhoodSponsoredCapabilitiesResponseSchema, ...)):
  // field order; the four bools are proto3 implicit (false is omitted);
  // `optional string reason = 9` is written only when set.
  compat::js_json::OutBuffer out{proto, proto_capacity, 0, true};
  codec::proto_wire::PutBytes(out, 1, "robinhood", 9);
  codec::proto_wire::PutTag(out, 2, 0);
  codec::proto_wire::PutVarint(out, kChainId);
  for (uint32_t field = 3; enabled && field <= 6; ++field) {
    codec::proto_wire::PutTag(out, field, 0);
    out.Byte(1);
  }
  codec::proto_wire::PutTag(out, 7, 0);
  codec::proto_wire::PutVarint(out, kMaxCalls);
  codec::proto_wire::PutBytes(out, 8, "eip-7702", 8);
  if (!enabled) codec::proto_wire::PutBytes(out, 9, kSponsorshipDisabledReason, strlen(kSponsorshipDisabledReason));
  if (!out.ok) return false;
  *json_length = static_cast<size_t>(written);
  *proto_length = out.used;
  return true;
}

}  // namespace fortis::exchange::robinhood
