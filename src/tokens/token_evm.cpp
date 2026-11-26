#include <algorithm>
#include "token_evm.h"

#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_evm {
namespace {

using tj::Arena;
using tj::Value;

constexpr char kMulticall[] = "0xcA11bde05977b3631167028862bE2a173976CA11";
constexpr int kMaxDecimals = 24;

struct U256 { uint32_t limb[8]; };  // little-endian 32-bit limbs

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// BigInt('0x' + 64 hex digits); false for a non-hex digit (SyntaxError).
bool parse_word(const char* hex, U256* out) {
  *out = U256{};
  for (int i = 0; i < 64; ++i) {
    const int v = hexv(hex[i]);
    if (v < 0) return false;
    const int nibble = 63 - i;  // position from the least significant nibble
    out->limb[nibble / 8] |= static_cast<uint32_t>(v) << ((nibble % 8) * 4);
  }
  return true;
}

bool is_zero(const U256& v) {
  for (uint32_t l : v.limb) if (l) return false;
  return true;
}

// BigInt.prototype.toString() (decimal).
void to_decimal(U256 v, char* out, size_t capacity) {
  char digits[96];
  size_t n = 0;
  if (is_zero(v)) { snprintf(out, capacity, "0"); return; }
  while (!is_zero(v)) {
    uint64_t rem = 0;
    for (int i = 7; i >= 0; --i) {
      const uint64_t cur = (rem << 32) | v.limb[i];
      v.limb[i] = static_cast<uint32_t>(cur / 1000000000ULL);
      rem = cur % 1000000000ULL;
    }
    for (int k = 0; k < 9; ++k) { digits[n++] = static_cast<char>('0' + rem % 10); rem /= 10; }
  }
  while (n > 1 && digits[n - 1] == '0') --n;
  size_t used = 0;
  for (size_t i = 0; i < n && used + 1 < capacity; ++i) out[used++] = digits[n - 1 - i];
  out[used] = '\0';
}

// Number(bigint): correctly rounded.
double to_double(const U256& v) {
  char text[96];
  to_decimal(v, text, sizeof(text));
  return strtod(text, nullptr);
}

struct Hex {
  const char* text;
  size_t length;
};

// wordAt(hex, byteOffset) as a double offset; false when truncated or not hex.
bool word_at(const Hex& hex, double byte_offset, U256* word) {
  if (!(byte_offset >= 0) || !isfinite(byte_offset)) return false;
  const double start = byte_offset * 2;
  if (start + 64 > static_cast<double>(hex.length)) return false;
  return parse_word(hex.text + static_cast<size_t>(start), word);
}

// hex.slice(start, start + count) with JavaScript clamping.
Hex slice(const Hex& hex, double start, double count) {
  if (!(start >= 0) || start >= static_cast<double>(hex.length)) return Hex{hex.text + hex.length, 0};
  const size_t begin = static_cast<size_t>(start);
  double end = start + count;
  if (end > static_cast<double>(hex.length)) end = static_cast<double>(hex.length);
  if (end <= start) return Hex{hex.text + begin, 0};
  return Hex{hex.text + begin, static_cast<size_t>(end) - begin};
}

// Buffer.from(hex, 'hex').toString('utf8') into a WTF-8 string value.
Value* hex_to_utf8(Arena& a, const Hex& hex) {
  uint8_t* bytes = static_cast<uint8_t*>(a.Alloc(hex.length / 2 + 1, 1));
  if (!bytes) return nullptr;
  size_t n = 0;
  for (size_t i = 0; i + 1 < hex.length; i += 2) {
    const int hi = hexv(hex.text[i]);
    const int lo = hexv(hex.text[i + 1]);
    if (hi < 0 || lo < 0) break;
    bytes[n++] = static_cast<uint8_t>(hi << 4 | lo);
  }
  // Decode with replacement (maximal subparts), as Buffer#toString does.
  char* text = static_cast<char*>(a.Alloc(n * 3 + 1, 1));
  if (!text) return nullptr;
  size_t used = 0, i = 0;
  auto put = [&](uint32_t cp) {
    tj::OutBuffer o{reinterpret_cast<uint8_t*>(text + used), 4, 0, true};
    compat::token_text::AppendCodePoint(o, cp);
    used += o.used;
  };
  while (i < n) {
    const uint8_t b = bytes[i];
    if (b < 0x80) { text[used++] = static_cast<char>(b); ++i; continue; }
    size_t need = 0; uint32_t cp = 0; uint8_t lower = 0x80, upper = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) { need = 2; cp = b & 0x0F; if (b == 0xE0) lower = 0xA0; if (b == 0xED) upper = 0x9F; }
    else if (b >= 0xF0 && b <= 0xF4) { need = 3; cp = b & 0x07; if (b == 0xF0) lower = 0x90; if (b == 0xF4) upper = 0x8F; }
    else { put(0xFFFD); ++i; continue; }
    size_t j = i + 1, seen = 0;
    bool bad = false;
    while (seen < need) {
      if (j >= n) { bad = true; break; }
      const uint8_t c = bytes[j];
      if (c < lower || c > upper) { bad = true; break; }
      lower = 0x80; upper = 0xBF;
      cp = (cp << 6) | (c & 0x3F);
      ++j; ++seen;
    }
    if (bad) { put(0xFFFD); i = j; continue; }
    put(cp);
    i = j;
  }
  return tj::Str(a, text, used);
}

// decodeString(returnData): null (JSON null value) when undecodable.
Value* decode_string(Arena& a, const Hex& hex) {
  if (hex.length == 0) return tj::Null(a);
  if (hex.length == 64) {
    // hex.replace(/(00)+$/, ''): the leftmost suffix of an even run of '0's.
    size_t cut = hex.length;
    for (size_t i = 0; i < hex.length; ++i) {
      bool zeros = true;
      for (size_t k = i; k < hex.length && zeros; ++k) zeros = hex.text[k] == '0';
      if (zeros && (hex.length - i) >= 2 && (hex.length - i) % 2 == 0) { cut = i; break; }
    }
    return hex_to_utf8(a, Hex{hex.text, cut});
  }
  U256 word{};
  if (!word_at(hex, 0, &word)) return tj::Null(a);
  const double offset = to_double(word);
  if (!word_at(hex, offset, &word)) return tj::Null(a);
  const double length = to_double(word);
  return hex_to_utf8(a, slice(hex, (offset + 32) * 2, length * 2));
}

// rawToDecimalString(raw, decimals).
Value* raw_to_decimal(Arena& a, const U256& raw, double decimals) {
  if (!(decimals >= 0) || decimals > kMaxDecimals || decimals != floor(decimals)) return tj::Null(a);
  const int places = static_cast<int>(decimals);
  char digits[128];
  to_decimal(raw, digits, sizeof(digits));
  size_t n = strlen(digits);
  char padded[160];
  size_t pad = n < (static_cast<size_t>(places) + 1) ? (static_cast<size_t>(places) + 1) - n : 0;
  memset(padded, '0', pad);
  memcpy(padded + pad, digits, n + 1);
  n += pad;
  if (places == 0) return tj::Str(a, padded, n);
  const size_t whole = n - static_cast<size_t>(places);
  size_t fraction_end = n;
  while (fraction_end > whole && padded[fraction_end - 1] == '0') --fraction_end;
  char out[200];
  size_t used = 0;
  memcpy(out, padded, whole); used = whole;
  if (fraction_end > whole) {
    out[used++] = '.';
    memcpy(out + used, padded + whole, fraction_end - whole);
    used += fraction_end - whole;
  }
  return tj::Str(a, out, used);
}

bool is_address(const char* address) {
  if (strlen(address) != 42 || address[0] != '0' || address[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i) if (hexv(address[i]) < 0) return false;
  return true;
}

}  // namespace

bool ContractFactsRequest(const char* address, char* output, size_t capacity, size_t* length) {
  if (!is_address(address)) return false;
  char target[65];
  memset(target, '0', 24);
  for (int i = 0; i < 40; ++i) {
    const char c = address[2 + i];
    target[24 + i] = (c >= 'A' && c <= 'F') ? static_cast<char>(c + 32) : c;
  }
  target[64] = '\0';
  auto uint_word = [](unsigned value, char out[65]) { snprintf(out, 65, "%064x", value); };
  const char* selectors[4] = {"18160ddd", "313ce567", "95d89b41", "06fdde03"};
  char data[2048];
  size_t used = 0;
  char word[65];
  used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "0x82ad56cb"));
  uint_word(32, word); used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", word));
  uint_word(4, word); used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", word));
  unsigned cursor = 4 * 32;
  for (int i = 0; i < 4; ++i) {
    uint_word(cursor, word);
    used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", word));
    cursor += 160;
  }
  for (int i = 0; i < 4; ++i) {
    used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", target));
    uint_word(1, word); used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", word));
    uint_word(96, word); used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", word));
    uint_word(4, word); used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s", word));
    used += static_cast<size_t>(snprintf(data + used, sizeof(data) - used, "%s%056d", selectors[i], 0));
  }
  const int n = snprintf(output, capacity,
                         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_call\",\"params\":"
                         "[{\"to\":\"%s\",\"data\":\"%s\"},\"latest\"]}",
                         kMulticall, data);
  if (n <= 0 || static_cast<size_t>(n) >= capacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}

FactsStatus ContractFacts(Arena& a, const Value* body, Value** facts) {
  *facts = nullptr;
  if (!body || body->type == tj::Type::kNull) return FactsStatus::kFailed;
  const Value* error = tj::Get(body, "error");
  if (error && error->type != tj::Type::kNull && !(error->type == tj::Type::kFalse) &&
      !(error->type == tj::Type::kNumber && (error->number == 0 || isnan(error->number))) &&
      !(error->type == tj::Type::kString && error->length == 0)) return FactsStatus::kFailed;
  const Value* result = tj::Get(body, "result");
  if (!result) return FactsStatus::kFailed;
  if (!tj::IsString(result)) return FactsStatus::kFailed;  // `.trim()` of a non-string throws
  size_t begin = 0, end = 0;
  compat::token_text::Trim(result->text, result->length, &begin, &end);
  Hex hex{result->text + begin, end - begin};
  if (hex.length >= 2 && hex.text[0] == '0' && (hex.text[1] == 'x' || hex.text[1] == 'X')) {
    hex.text += 2; hex.length -= 2;
  }
  // decodeAggregate3 (any malformed shape is a null decode).
  if (hex.length < 128) return FactsStatus::kNone;
  Hex returns[4];
  bool success[4] = {};
  {
    U256 word{};
    if (!word_at(hex, 0, &word)) return FactsStatus::kNone;
    const double array_offset = to_double(word);
    if (!word_at(hex, array_offset, &word)) return FactsStatus::kNone;
    const double count = to_double(word);
    if (count > 4096) return FactsStatus::kNone;
    // Any other length decodes (or not) to a result contractFacts rejects.
    if (count != 4) return FactsStatus::kNone;
    const double base = array_offset + 32;
    for (int i = 0; i < 4; ++i) {
      if (!word_at(hex, base + i * 32, &word)) return FactsStatus::kNone;
      const double tuple = base + to_double(word);
      if (!word_at(hex, tuple, &word)) return FactsStatus::kNone;
      success[i] = !is_zero(word);
      if (!word_at(hex, tuple + 32, &word)) return FactsStatus::kNone;
      const double data = tuple + to_double(word);
      if (!word_at(hex, data, &word)) return FactsStatus::kNone;
      const double length = to_double(word);
      returns[i] = slice(hex, (data + 32) * 2, length * 2);
    }
  }
  // decodeUint256: 0 when shorter than one word; BigInt throws on non-hex.
  auto uint256 = [&](const Hex& data, U256* value, bool* ok) {
    *value = U256{};
    *ok = true;
    if (data.length < 64) return;
    *ok = parse_word(data.text, value);
  };
  Value* decimals = tj::Null(a);
  U256 value{};
  bool ok = true;
  if (success[1]) {
    uint256(returns[1], &value, &ok);
    if (!ok) return FactsStatus::kFailed;
    decimals = tj::Num(a, to_double(value));
  }
  Value* supply = tj::Null(a);
  if (success[0] && decimals && decimals->type == tj::Type::kNumber) {
    uint256(returns[0], &value, &ok);
    if (!ok) return FactsStatus::kFailed;
    supply = raw_to_decimal(a, value, decimals->number);
  }
  Value* symbol = success[2] ? decode_string(a, returns[2]) : tj::Null(a);
  Value* name = success[3] ? decode_string(a, returns[3]) : tj::Null(a);
  if (!decimals || !supply || !symbol || !name) return FactsStatus::kFailed;
  if (tj::IsString(symbol))
    symbol = symbol->length ? compat::token_text::SanitizeSymbol(a, symbol->text, symbol->length) : tj::Null(a);
  if (tj::IsString(name))
    name = name->length ? compat::token_text::SanitizeName(a, name->text, name->length) : tj::Null(a);
  Value* out = tj::Obj(a);
  if (!out || !symbol || !name || !tj::Set(a, out, "supply", supply) || !tj::Set(a, out, "decimals", decimals) ||
      !tj::Set(a, out, "symbol", symbol) || !tj::Set(a, out, "name", name)) return FactsStatus::kFailed;
  *facts = out;
  return FactsStatus::kFacts;
}

Value* TokenMetadata(Arena& a, const Value* facts) {
  if (!tj::IsObject(facts)) return tj::Null(a);
  const Value* symbol = tj::Get(facts, "symbol");
  const Value* name = tj::Get(facts, "name");
  const Value* decimals = tj::Get(facts, "decimals");
  auto null = [](const Value* v) { return !v || v->type == tj::Type::kNull; };
  if (null(symbol) && null(name) && null(decimals)) return tj::Null(a);
  Value* out = tj::Obj(a);
  if (!out || !tj::Set(a, out, "symbol", tj::Clone(a, symbol)) || !tj::Set(a, out, "name", tj::Clone(a, name)) ||
      !tj::Set(a, out, "decimals", tj::Clone(a, decimals)) || !tj::Set(a, out, "verified", tj::Null(a)) ||
      !tj::Set(a, out, "holderCount", tj::Null(a)) || !tj::Set(a, out, "logoMediaKey", tj::Null(a)))
    return nullptr;
  return out;
}

Value* TokenInsight(Arena& a, const Value* facts) {
  Value* sections = tj::Arr(a);
  if (!sections || !tj::IsObject(facts)) return sections;
  const Value* supply = tj::Get(facts, "supply");
  const Value* decimals = tj::Get(facts, "decimals");
  const Value* symbol = tj::Get(facts, "symbol");
  const Value* name = tj::Get(facts, "name");
  Value* rows = tj::Arr(a);
  if (!rows) return nullptr;
  auto row = [&](const char* label, Value* value) {
    Value* r = tj::Obj(a);
    return r && value && tj::Set(a, r, "label", tj::Str(a, label)) && tj::Set(a, r, "value", value) &&
           tj::Push(a, rows, r);
  };
  bool ok = true;
  if (tj::IsString(supply)) ok = ok && row("Declared supply", tj::Clone(a, supply));
  if (tj::IsNumber(decimals)) {
    uint8_t buffer[64];
    tj::OutBuffer o{buffer, sizeof(buffer), 0, true};
    codec::history_batch::AppendJsNumber(o, decimals->number);
    ok = ok && row("Decimals", tj::Str(a, reinterpret_cast<const char*>(buffer), o.used));
  }
  if (tj::IsString(symbol) && symbol->length) ok = ok && row("Contract symbol", tj::Clone(a, symbol));
  if (tj::IsString(name) && name->length) ok = ok && row("Contract name", tj::Clone(a, name));
  if (!ok) return nullptr;
  if (rows->count == 0) return sections;
  Value* section = tj::Obj(a);
  Value* summary = nullptr;
  if (tj::IsString(supply)) {
    char* text = static_cast<char*>(a.Alloc(supply->length + 32, 1));
    if (!text) return nullptr;
    memcpy(text, supply->text, supply->length);
    std::copy_n(" declared supply", 16, text + supply->length);
    summary = tj::Str(a, text, supply->length + 16);
  } else {
    summary = tj::Str(a, "ERC-20 metadata");
  }
  if (!section || !tj::Set(a, section, "title", tj::Str(a, "Contract")) || !tj::Set(a, section, "summary", summary) ||
      !tj::Set(a, section, "ok", tj::Bool(a, true)) || !tj::Set(a, section, "rows", rows) ||
      !tj::Push(a, sections, section)) return nullptr;
  return sections;
}

}  // namespace fortis::tokens::token_evm
