#include <algorithm>
#include "chain_core.h"

#include "chain_driver.h"
#include "evm_proof.h"
#include "network_registry.h"
#include "price_history_batch_codec.h"
#include "provider_text.h"

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

constexpr const char* kFamilyNames[] = {
    "evm", "bitcoin", "bitcoinCash", "dash", "dogecoin", "litecoin", "zcash", "solana",
    "cardano", "xrpl", "stellar", "hedera", "algorand", "sui", "near", "ton", "aptos", "icp",
    "bittensor", "vechain", "tron", "cosmos"};
static_assert(sizeof(kFamilyNames) / sizeof(kFamilyNames[0]) == static_cast<size_t>(Family::kCount),
              "family table");

bool digits_only(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// Number(digits) for a decimal digit string (JavaScript rounds to nearest).
double digits_number(const char* text, size_t length) {
  char buffer[128];
  if (length >= sizeof(buffer)) return HUGE_VAL;
  memcpy(buffer, text, length);
  buffer[length] = '\0';
  return strtod(buffer, nullptr);
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

const char* strip0x(const char* hex, size_t* length) {
  if (*length >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    *length -= 2;
    return hex + 2;
  }
  return hex;
}

// Buffer.from(bytes).toString('utf8'): maximal-subpart replacement (U+FFFD),
// written as UTF-8 (never a lone surrogate).
Value* utf8_value(Arena& arena, const uint8_t* bytes, size_t length) {
  char* out = static_cast<char*>(arena.Alloc(length * 3 + 1, 1));
  if (!out) return nullptr;
  size_t used = 0;
  size_t i = 0;
  auto replacement = [&]() { out[used++] = '\xEF'; out[used++] = '\xBF'; out[used++] = '\xBD'; };
  while (i < length) {
    const uint8_t b = bytes[i];
    if (b < 0x80) { out[used++] = static_cast<char>(b); ++i; continue; }
    size_t need = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) need = 1;
    else if (b == 0xE0) { need = 2; lo = 0xA0; }
    else if (b >= 0xE1 && b <= 0xEC) need = 2;
    else if (b == 0xED) { need = 2; hi = 0x9F; }
    else if (b >= 0xEE && b <= 0xEF) need = 2;
    else if (b == 0xF0) { need = 3; lo = 0x90; }
    else if (b >= 0xF1 && b <= 0xF3) need = 3;
    else if (b == 0xF4) { need = 3; hi = 0x8F; }
    else { replacement(); ++i; continue; }
    size_t j = 1;
    for (; j <= need; ++j) {
      if (i + j >= length) break;
      const uint8_t c = bytes[i + j];
      const uint8_t low = j == 1 ? lo : 0x80, high = j == 1 ? hi : 0xBF;
      if (c < low || c > high) break;
    }
    if (j == need + 1) {
      memcpy(out + used, bytes + i, need + 1);
      used += need + 1;
      i += need + 1;
    } else {
      replacement();
      i += j;
    }
  }
  return tj::Str(arena, out, used);
}

}  // namespace

// ---- families -----------------------------------------------------------------

const char* FamilyName(Family family) {
  const size_t index = static_cast<size_t>(family);
  return index < static_cast<size_t>(Family::kCount) ? kFamilyNames[index] : "";
}

bool ParseFamily(const char* name, Family* out) {
  for (size_t i = 0; i < static_cast<size_t>(Family::kCount); ++i) {
    if (strcmp(name, kFamilyNames[i]) == 0) {
      *out = static_cast<Family>(i);
      return true;
    }
  }
  return false;
}

// ---- catalog --------------------------------------------------------------------

bool Catalog::Init(const config::ProviderRuntimeConfig& config) {
  networks::NativeNetworkRegistry registry;
  size_t seeds = 0;
  const networks::NativeNetworkConfig* list = registry.list(&seeds);
  count_ = 0;
  if (config.alchemy_api_key_configured) {
    const int written = snprintf(robinhood_url_, sizeof(robinhood_url_),
                                 "https://robinhood-mainnet.g.alchemy.com/v2/%s", config.alchemy_api_key);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(robinhood_url_)) return false;
  }
  for (size_t i = 0; i < config.dash_blockbook_url_count && i < kMaxNetworkUrls; ++i)
    snprintf(dash_urls_[i], sizeof(dash_urls_[i]), "%s", config.dash_blockbook_urls[i]);
  for (size_t i = 0; i < seeds; ++i) {
    if (count_ == kMax) return false;
    const networks::NativeNetworkConfig& seed = list[i];
    Network& n = networks_[count_];
    n = Network{};
    if (strlen(seed.id) > kMaxNetworkIdBytes) return false;
    snprintf(n.id, sizeof(n.id), "%s", seed.id);
    if (!ParseFamily(seed.family, &n.family)) return false;
    n.has_evm_chain_id = seed.has_evm_chain_id;
    n.evm_chain_id = seed.evm_chain_id;
    n.testnet = seed.testnet;
    n.has_required_confirmations = seed.has_required_confirmations;
    n.required_confirmations = seed.required_confirmations;
    // registry options.rpcUrls: deployment-owned replacements.
    if (strcmp(seed.id, "robinhood") == 0 && config.alchemy_api_key_configured) {
      n.rpc_urls[0] = robinhood_url_;
      n.rpc_url_count = 1;
    } else if (strcmp(seed.id, "dash") == 0 && config.dash_blockbook_url_count > 0) {
      for (size_t u = 0; u < config.dash_blockbook_url_count && u < kMaxNetworkUrls; ++u)
        n.rpc_urls[n.rpc_url_count++] = dash_urls_[u];
    } else {
      for (size_t u = 0; u < seed.rpc_url_count && u < kMaxNetworkUrls; ++u)
        n.rpc_urls[n.rpc_url_count++] = seed.rpc_urls[u];
    }
    for (size_t u = 0; u < seed.graphql_url_count && u < kMaxNetworkUrls; ++u)
      n.graphql_urls[n.graphql_url_count++] = seed.graphql_urls[u];
    n.has_native = seed.native_symbol != nullptr;
    n.native_symbol = seed.native_symbol;
    n.native_decimals = seed.native_decimals;
    n.native_coingecko_id = seed.native_coingecko_id;
    // Capability truth: per driver/source/view and endpoints.
    const ChainDriver* driver = DriverFor(config, n.family);
    const size_t balance_endpoints = n.family == Family::kSui ? n.graphql_url_count : n.rpc_url_count;
    n.caps.balances = driver && balance_endpoints > 0;
    n.caps.transfers = AnySourceSupports(config, n);
    n.caps.insight = driver && driver->HasInsight() && n.rpc_url_count > 0;
    n.caps.tx_status = driver && driver->HasTxStatus() && n.rpc_url_count > 0;
    n.caps.nfts = driver && driver->Nfts(config) && driver->Nfts(config)->SupportsNfts(n);
    n.caps.tx_detail = ViewFor(n.family) != nullptr;
    ++count_;
  }
  return true;
}

bool Catalog::Resolve(const char* id, size_t length, Network* out) const {
  for (size_t i = 0; i < count_; ++i) {
    if (strlen(networks_[i].id) == length && memcmp(networks_[i].id, id, length) == 0) {
      *out = networks_[i];
      return true;
    }
  }
  if (length <= 7 || memcmp(id, "eip155:", 7) != 0 || !digits_only(id + 7, length - 7)) return false;
  const double chain_id = digits_number(id + 7, length - 7);
  for (size_t i = 0; i < count_; ++i) {
    if (networks_[i].has_evm_chain_id && networks_[i].evm_chain_id == chain_id) {
      *out = networks_[i];
      return true;
    }
  }
  if (length > kMaxNetworkIdBytes) return false;
  *out = Network{};
  memcpy(out->id, id, length);
  out->id[length] = '\0';
  out->family = Family::kEvm;
  out->has_evm_chain_id = true;
  out->evm_chain_id = chain_id;
  out->generic = true;
  return true;
}

// ---- errors and context -------------------------------------------------------

void Error::Range(const char* format, ...) {
  kind = ErrorKind::kRange;
  retry_after_ms = 0;
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
}

void Error::Upstream(const char* format, ...) {
  kind = ErrorKind::kUpstream;
  retry_after_ms = 0;
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
}

void Error::LaneBusy(uint32_t retry_after) {
  kind = ErrorKind::kLaneBusy;
  retry_after_ms = retry_after;
  snprintf(message, sizeof(message), "lane busy");
}

int64_t Context::Now() const {
  if (clock) return clock->NowMs();
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// ---- JavaScript values ----------------------------------------------------------

const Value* Member(const Value* object, const char* key) { return tj::Get(object, key); }

const char* StringOrNull(Arena& arena, const Value* v, bool reject_empty) {
  if (!tj::IsString(v)) return nullptr;
  if (reject_empty && v->length == 0) return nullptr;
  return arena.CopyText(v->text, v->length);
}

double JsNumber(const Value* v) {
  if (!v) return NAN;
  switch (v->type) {
    case tj::Type::kNull: return 0;
    case tj::Type::kFalse: return 0;
    case tj::Type::kTrue: return 1;
    case tj::Type::kNumber: return v->number;
    case tj::Type::kString: return codec::provider_text::ToNumber(v->text, v->length);
    case tj::Type::kArray:
      // Number([]) = 0; Number([x]) = Number(String(x)); otherwise NaN.
      if (v->count == 0) return 0;
      if (v->count == 1) {
        const Value* item = v->items[0];
        if (!item || item->type == tj::Type::kNull) return 0;
        if (item->type == tj::Type::kString) return codec::provider_text::ToNumber(item->text, item->length);
        if (item->type == tj::Type::kNumber) return item->number;
        if (item->type == tj::Type::kArray) return JsNumber(item);
        return NAN;  // String(true) / String({}) are not numeric
      }
      return NAN;
    case tj::Type::kObject: return NAN;
  }
  return NAN;
}

std::optional<double> NumOrNull(const Value* v) {
  if (!v || v->type == tj::Type::kTrue || v->type == tj::Type::kFalse) return std::nullopt;
  const double parsed = JsNumber(v);
  return isfinite(parsed) ? std::optional<double>(parsed) : std::nullopt;
}

std::optional<double> IntOrNull(const Value* v) {
  const auto parsed = NumOrNull(v);
  if (!parsed) return std::nullopt;
  const double number = trunc(*parsed);
  return number == 0 ? 0.0 : number;  // Preserve normalization of negative zero.
}

static bool hex_quantity(const Value* v) {
  // /^0x[0-9a-fA-F]*$/ and not the bare "0x".
  if (!tj::IsString(v) || v->length < 3 || v->text[0] != '0' || v->text[1] != 'x') return false;
  return IsHex(v->text + 2, v->length - 2);
}

Value* HexToDecimalString(Arena& arena, const Value* v) {
  if (!hex_quantity(v)) return tj::Null(arena);
  BigInt value;
  if (!BigInt::ParseHex(v->text + 2, v->length - 2, &value)) return tj::Null(arena);
  return value.ToValue(arena);
}

std::optional<double> HexToInt(const Value* v) {
  if (!hex_quantity(v)) return std::nullopt;
  BigInt value;
  if (!BigInt::ParseHex(v->text + 2, v->length - 2, &value)) return std::nullopt;
  const double number = value.ToDouble();
  if (number > 9007199254740991.0) return std::nullopt;
  return number;
}

Value* AmountString(Arena& arena, const Value* v) {
  if (tj::IsString(v)) {
    return digits_only(v->text, v->length) ? tj::Str(arena, v->text, v->length) : tj::Null(arena);
  }
  if (tj::IsNumber(v) && isfinite(v->number) && trunc(v->number) == v->number && v->number >= 0) {
    char text[64];
    JsNumberText(v->number, text, sizeof(text));
    return tj::Str(arena, text);
  }
  return tj::Null(arena);
}

Value* IntegerString(Arena& arena, const Value* v) {
  if (tj::IsString(v)) {
    size_t begin = 0, end = 0;
    codec::provider_text::Trim(v->text, v->length, &begin, &end);
    const char* text = v->text + begin;
    const size_t length = end - begin;
    const size_t sign = length && text[0] == '-' ? 1 : 0;
    return digits_only(text + sign, length - sign) ? tj::Str(arena, text, length) : tj::Null(arena);
  }
  if (!tj::IsNumber(v) || !isfinite(v->number) || trunc(v->number) != v->number) return tj::Null(arena);
  // toLocaleString('fullwide', {useGrouping:false}): the shortest round-trip
  // digits of the double with any exponent expanded into zeros (2 ** 64 is
  // "18446744073709552000", not its exact binary value).
  if (v->number == 0) return tj::Str(arena, signbit(v->number) ? "-0" : "0");
  char text[64]{};
  JsNumberText(v->number, text, sizeof(text));
  const char* e = strchr(text, 'e');
  if (!e) return tj::Str(arena, text);
  const long exponent = strtol(e + 1, nullptr, 10);
  char digits[64];
  size_t count = 0;
  const bool negative = text[0] == '-';
  size_t point = 0;
  bool seen_point = false;
  for (const char* p = text + (negative ? 1 : 0); p < e; ++p) {
    if (*p == '.') { seen_point = true; continue; }
    digits[count++] = *p;
    if (!seen_point) ++point;
  }
  const long zeros = exponent - static_cast<long>(count - point);
  if (zeros < 0 || zeros > 400) return tj::Null(arena);
  char* out = static_cast<char*>(arena.Alloc(count + static_cast<size_t>(zeros) + 2, 1));
  if (!out) return nullptr;
  size_t used = 0;
  if (negative) out[used++] = '-';
  memcpy(out + used, digits, count);
  used += count;
  for (long i = 0; i < zeros; ++i) out[used++] = '0';
  return tj::Str(arena, out, used);
}

bool QuoteLongIntegers(const uint8_t* text, size_t length, OutBuffer& out) {
  size_t index = 0;
  bool in_string = false;
  while (index < length) {
    const uint8_t c = text[index];
    if (in_string) {
      out.Byte(c);
      if (c == '\\') {
        ++index;
        if (index < length) out.Byte(text[index]);
      } else if (c == '"') {
        in_string = false;
      }
      ++index;
      continue;
    }
    if (c == '"') {
      in_string = true;
      out.Byte(c);
      ++index;
      continue;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t end = index;
      if (text[end] == '-') ++end;
      while (end < length && text[end] >= '0' && text[end] <= '9') ++end;
      const uint8_t next = end < length ? text[end] : 0;
      const bool integer = next != '.' && next != 'e' && next != 'E';
      const size_t digits = end - index - (text[index] == '-' ? 1 : 0);
      if (integer && digits >= 16) {
        out.Byte('"');
        out.Append(text + index, end - index);
        out.Byte('"');
      } else {
        out.Append(text + index, end - index);
      }
      index = end;
      continue;
    }
    out.Byte(c);
    ++index;
  }
  return out.ok;
}

void JsNumberText(double value, char* output, size_t capacity) {
  if (!capacity) return;
  uint8_t buffer[64];
  OutBuffer out{buffer, sizeof(buffer), 0, true};
  if (isnan(value)) {
    snprintf(output, capacity, "NaN");
    return;
  }
  if (isinf(value)) {
    snprintf(output, capacity, value > 0 ? "Infinity" : "-Infinity");
    return;
  }
  codec::history_batch::AppendJsNumber(out, value);
  const size_t used = out.ok ? out.used : 0;
  const size_t copy = used < capacity - 1 ? used : capacity - 1;
  memcpy(output, buffer, copy);
  output[copy] = '\0';
}

namespace {

// Array.prototype.join's ToString of one element (null -> "").
bool join_text(const Value* v, OutBuffer& out, int depth) {
  if (!v || depth > 64) return false;
  switch (v->type) {
    case tj::Type::kNull: return true;
    case tj::Type::kTrue: out.Text("true"); return out.ok;
    case tj::Type::kFalse: out.Text("false"); return out.ok;
    case tj::Type::kNumber: {
      char text[64];
      JsNumberText(v->number, text, sizeof(text));
      out.Text(text);
      return out.ok;
    }
    case tj::Type::kString: out.Append(v->text, v->length); return out.ok;
    case tj::Type::kArray:
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        if (!join_text(v->items[i], out, depth + 1)) return false;
      }
      return out.ok;
    default: out.Text("[object Object]"); return out.ok;
  }
}

// Number(BigInt(text)) for a string: StringToBigInt (trimmed; "" is 0n; a
// 0x/0o/0b literal or a signed decimal integer, nothing else) without its
// 640-bit limit: the nearest double, Infinity past 2^1024, never -0.
bool string_bigint_number(Arena& a, const char* text, size_t length, double* out) {
  size_t begin = 0, end = 0;
  codec::history_batch::JsTrim(text, length, &begin, &end);
  const char* s = text + begin;
  const size_t n = end - begin;
  if (!n) { *out = 0; return true; }
  bool valid = true;
  if (n > 2 && s[0] == '0' && strchr("xXoObB", s[1])) {
    const int radix = s[1] == 'x' || s[1] == 'X' ? 16 : s[1] == 'o' || s[1] == 'O' ? 8 : 2;
    for (size_t i = 2; valid && i < n; ++i) {
      const char c = s[i];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
      valid = digit < radix;
    }
  } else {
    size_t i = s[0] == '+' || s[0] == '-' ? 1 : 0;
    valid = i < n;
    for (; valid && i < n; ++i) valid = s[i] >= '0' && s[i] <= '9';
  }
  if (!valid) return false;
  char* scratch = static_cast<char*>(a.Alloc(n + 64, 1));
  if (!scratch) return false;
  const double value = codec::history_batch::JsStringToNumber(s, n, scratch, n + 64);
  if (isnan(value)) return false;
  *out = value == 0 ? 0 : value;
  return true;
}

}  // namespace

bool BigIntNumber(Arena& a, const Value* v, double* out, Error* error) {
  if (v && v->type == tj::Type::kNumber) {
    if (!isfinite(v->number) || trunc(v->number) != v->number) {
      char text[64];
      JsNumberText(v->number, text, sizeof(text));
      error->Range("The number %s cannot be converted to a BigInt because it is not an integer", text);
      return false;
    }
    *out = v->number == 0 ? 0 : v->number;
    return true;
  }
  if (v && (v->type == tj::Type::kTrue || v->type == tj::Type::kFalse)) {
    *out = v->type == tj::Type::kTrue ? 1 : 0;
    return true;
  }
  if (v && v->type == tj::Type::kString) {
    if (string_bigint_number(a, v->text, v->length, out)) return true;
  } else if (v && v->type == tj::Type::kArray) {
    // A provider body is at most 512 KiB, so is its join.
    const size_t capacity = 512 * 1024 + 64;
    uint8_t* joined = static_cast<uint8_t*>(a.Alloc(capacity, 1));
    OutBuffer text{joined, capacity, 0, true};
    if (joined && join_text(v, text, 0) &&
        string_bigint_number(a, reinterpret_cast<const char*>(joined), text.used, out))
      return true;
  }
  error->Upstream("not a BigInt");
  return false;
}

bool StringBigIntNumber(Arena& a, const Value* v, double* out, Error* error) {
  // A provider body is at most 512 KiB, so is any String() of its values.
  const size_t capacity = 512 * 1024 + 64;
  uint8_t* text = static_cast<uint8_t*>(a.Alloc(capacity, 1));
  OutBuffer s{text, capacity, 0, true};
  bool built = text && v;
  if (built && v->type == tj::Type::kNull) s.Text("null");
  else if (built) built = join_text(v, s, 0);
  if (built && s.ok && string_bigint_number(a, reinterpret_cast<const char*>(text), s.used, out)) return true;
  const int shown = built && s.ok ? static_cast<int>(s.used < 400 ? s.used : 400) : 0;
  error->Upstream("Cannot convert %.*s to a BigInt", shown, text ? reinterpret_cast<const char*>(text) : "");
  return false;
}



Value* Format(Arena& arena, const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  const int needed = vsnprintf(nullptr, 0, format, args);
  va_end(args);
  if (needed < 0) { va_end(again); return nullptr; }
  char* text = static_cast<char*>(arena.Alloc(static_cast<size_t>(needed) + 1, 1));
  if (!text) { va_end(again); return nullptr; }
  vsnprintf(text, static_cast<size_t>(needed) + 1, format, again);
  va_end(again);
  return tj::Str(arena, text, static_cast<size_t>(needed));
}

bool Equals(const Value* v, const char* text) { return tj::StringEquals(v, text); }

bool StartsWith(const char* text, const char* prefix) {
  return text && strncmp(text, prefix, strlen(prefix)) == 0;
}

bool IsoTime(int64_t epoch_ms, char output[32]) { return codec::provider_text::IsoString(epoch_ms, output); }

bool ParseTime(const char* text, size_t length, double* epoch_ms) {
  return codec::provider_text::DateParse(text, length, epoch_ms);
}

Value* IsoValue(Arena& arena, int64_t epoch_ms) {
  char text[32];
  return IsoTime(epoch_ms, text) ? tj::Str(arena, text) : tj::Null(arena);
}

bool EncodeUriComponent(const char* text, size_t length, OutBuffer& out) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
                            c == '\'' || c == '(' || c == ')';
    if (unreserved) { out.Byte(c); continue; }
    // A lone surrogate (WTF-8 ED A0..BF) makes encodeURIComponent throw.
    if (c == 0xED && i + 1 < length && static_cast<uint8_t>(text[i + 1]) >= 0xA0) return false;
    out.Byte('%');
    out.Byte(static_cast<uint8_t>(kHex[c >> 4]));
    out.Byte(static_cast<uint8_t>(kHex[c & 15]));
  }
  return out.ok;
}

const char* EncodeUri(Arena& arena, const char* text) {
  const size_t length = strlen(text);
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(length * 3 + 1, 1));
  if (!buffer) return nullptr;
  OutBuffer out{buffer, length * 3 + 1, 0, true};
  if (!EncodeUriComponent(text, length, out) || !out.ok) return nullptr;
  buffer[out.used] = '\0';
  return reinterpret_cast<const char*>(buffer);
}

bool UrlHost(const char* url, char* output, size_t capacity) {
  const char* scheme_end = strstr(url, "://");
  if (!scheme_end) return false;
  const size_t scheme_length = static_cast<size_t>(scheme_end - url);
  const bool https = scheme_length == 5 && strncmp(url, "https", 5) == 0;
  const bool http = scheme_length == 4 && strncmp(url, "http", 4) == 0;
  if (!https && !http) return false;
  const char* authority = scheme_end + 3;
  size_t authority_length = strcspn(authority, "/?#");
  const char* at = nullptr;
  for (size_t i = 0; i < authority_length; ++i)
    if (authority[i] == '@') at = authority + i;
  if (at) {
    authority_length -= static_cast<size_t>(at + 1 - authority);
    authority = at + 1;
  }
  if (!authority_length || authority_length >= capacity) return false;
  size_t host_length = authority_length;
  const char* port = nullptr;
  size_t port_length = 0;
  if (authority[0] == '[') {
    const char* close = static_cast<const char*>(memchr(authority, ']', authority_length));
    if (!close) return false;
    host_length = static_cast<size_t>(close + 1 - authority);
  } else {
    const char* colon = static_cast<const char*>(memchr(authority, ':', authority_length));
    if (colon) host_length = static_cast<size_t>(colon - authority);
  }
  if (host_length < authority_length && authority[host_length] == ':') {
    port = authority + host_length + 1;
    port_length = authority_length - host_length - 1;
  }
  const bool default_port = !port_length || (https && port_length == 3 && memcmp(port, "443", 3) == 0) ||
                            (http && port_length == 2 && memcmp(port, "80", 2) == 0);
  size_t used = 0;
  for (size_t i = 0; i < host_length; ++i) {
    char c = authority[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    output[used++] = c;
  }
  if (!default_port) {
    output[used++] = ':';
    memcpy(output + used, port, port_length);
    used += port_length;
  }
  output[used] = '\0';
  return used > 0;
}

// ---- BigInt -----------------------------------------------------------------------

BigInt BigInt::FromU64(uint64_t value) {
  BigInt out;
  out.limbs_[0] = static_cast<uint32_t>(value);
  out.limbs_[1] = static_cast<uint32_t>(value >> 32);
  return out;
}

BigInt BigInt::FromI64(int64_t value) {
  BigInt out = FromU64(value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value));
  out.negative_ = value < 0;
  return out;
}

bool BigInt::IsZero() const {
  for (uint32_t limb : limbs_)
    if (limb) return false;
  return true;
}

bool BigInt::MulSmall(uint32_t factor) {
  uint64_t carry = 0;
  for (uint32_t& limb : limbs_) {
    const uint64_t product = static_cast<uint64_t>(limb) * factor + carry;
    limb = static_cast<uint32_t>(product);
    carry = product >> 32;
  }
  return carry == 0;
}

bool BigInt::AddSmall(uint32_t value) {
  uint64_t carry = value;
  for (uint32_t& limb : limbs_) {
    if (!carry) break;
    const uint64_t sum = static_cast<uint64_t>(limb) + carry;
    limb = static_cast<uint32_t>(sum);
    carry = sum >> 32;
  }
  return carry == 0;
}

bool BigInt::DivSmall(uint32_t divisor, uint32_t* remainder) {
  if (!divisor) return false;
  uint64_t rest = 0;
  for (size_t i = kLimbs; i-- > 0;) {
    const uint64_t current = (rest << 32) | limbs_[i];
    limbs_[i] = static_cast<uint32_t>(current / divisor);
    rest = current % divisor;
  }
  if (remainder) *remainder = static_cast<uint32_t>(rest);
  return true;
}

bool BigInt::ParseDecimal(const char* text, size_t length, BigInt* out) {
  BigInt value;
  if (!length) return false;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    if (!value.MulSmall(10) || !value.AddSmall(static_cast<uint32_t>(text[i] - '0'))) return false;
  }
  *out = value;
  return true;
}

bool BigInt::ParseHex(const char* text, size_t length, BigInt* out) {
  text = strip0x(text, &length);
  BigInt value;
  for (size_t i = 0; i < length; ++i) {
    const int digit = hex_value(text[i]);
    if (digit < 0) return false;
    if (!value.MulSmall(16) || !value.AddSmall(static_cast<uint32_t>(digit))) return false;
  }
  *out = value;
  return true;
}

bool BigInt::Parse(const char* text, size_t length, BigInt* out) {
  // StringToBigInt: StrWhiteSpaceChar trimmed; "" -> 0n.
  size_t begin = 0, end = length;
  codec::provider_text::Trim(text, length, &begin, &end);
  text += begin;
  length = end - begin;
  if (!length) { *out = BigInt(); return true; }
  if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    if (!IsHex(text + 2, length - 2)) return false;
    return ParseHex(text + 2, length - 2, out);
  }
  if (length > 2 && text[0] == '0' && (text[1] == 'o' || text[1] == 'O' || text[1] == 'b' || text[1] == 'B')) {
    const uint32_t radix = (text[1] == 'o' || text[1] == 'O') ? 8 : 2;
    BigInt value;
    for (size_t i = 2; i < length; ++i) {
      const int digit = text[i] - '0';
      if (digit < 0 || static_cast<uint32_t>(digit) >= radix) return false;
      if (!value.MulSmall(radix) || !value.AddSmall(static_cast<uint32_t>(digit))) return false;
    }
    *out = value;
    return true;
  }
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    ++text;
    --length;
  }
  if (!ParseDecimal(text, length, out)) return false;
  out->negative_ = negative;
  return true;
}

bool BigInt::FromDouble(double value, BigInt* out) {
  if (!isfinite(value) || trunc(value) != value) return false;
  BigInt result;
  const bool negative = value < 0;
  double magnitude = fabs(value);
  int exponent = 0;
  const double mantissa = frexp(magnitude, &exponent);  // magnitude = mantissa * 2^exponent
  if (exponent > static_cast<int>(kLimbs * 32)) return false;
  uint64_t bits = static_cast<uint64_t>(ldexp(mantissa, 53));
  int shift = exponent - 53;
  if (shift < 0) {
    bits >>= -shift;
    shift = 0;
  }
  result = FromU64(bits);
  for (int i = 0; i < shift; ++i)
    if (!result.MulSmall(2)) return false;
  result.negative_ = negative;
  *out = result;
  return true;
}

int BigInt::Compare(const BigInt& other) const {
  const bool a_negative = negative(), b_negative = other.negative();
  if (a_negative != b_negative) return a_negative ? -1 : 1;
  int magnitude = 0;
  for (size_t i = kLimbs; i-- > 0;) {
    if (limbs_[i] != other.limbs_[i]) {
      magnitude = limbs_[i] < other.limbs_[i] ? -1 : 1;
      break;
    }
  }
  return a_negative ? -magnitude : magnitude;
}

namespace {
int compare_magnitude(const uint32_t* a, const uint32_t* b, size_t n) {
  for (size_t i = n; i-- > 0;)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
bool add_magnitude(uint32_t* a, const uint32_t* b, size_t n) {
  uint64_t carry = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint64_t sum = static_cast<uint64_t>(a[i]) + b[i] + carry;
    a[i] = static_cast<uint32_t>(sum);
    carry = sum >> 32;
  }
  return carry == 0;
}
// a -= b where |a| >= |b|.
void sub_magnitude(uint32_t* a, const uint32_t* b, size_t n) {
  int64_t borrow = 0;
  for (size_t i = 0; i < n; ++i) {
    int64_t diff = static_cast<int64_t>(a[i]) - b[i] - borrow;
    borrow = diff < 0 ? 1 : 0;
    if (diff < 0) diff += (static_cast<int64_t>(1) << 32);
    a[i] = static_cast<uint32_t>(diff);
  }
}
}  // namespace

bool BigInt::Add(const BigInt& other) {
  if (negative() == other.negative() || other.IsZero()) {
    const bool sign = IsZero() ? other.negative() : negative();
    if (!add_magnitude(limbs_, other.limbs_, kLimbs)) return false;
    negative_ = sign;
    return true;
  }
  if (compare_magnitude(limbs_, other.limbs_, kLimbs) >= 0) {
    sub_magnitude(limbs_, other.limbs_, kLimbs);
  } else {
    uint32_t copy[kLimbs];
    memcpy(copy, other.limbs_, sizeof(copy));
    sub_magnitude(copy, limbs_, kLimbs);
    memcpy(limbs_, copy, sizeof(copy));
    negative_ = other.negative();
  }
  return true;
}

bool BigInt::Sub(const BigInt& other) {
  BigInt negated = other;
  negated.negative_ = !other.negative();
  if (other.IsZero()) return true;
  return Add(negated);
}

bool BigInt::Mul(const BigInt& other) {
  uint32_t result[kLimbs * 2] = {};
  for (size_t i = 0; i < kLimbs; ++i) {
    if (!limbs_[i]) continue;
    uint64_t carry = 0;
    for (size_t j = 0; j < kLimbs; ++j) {
      const uint64_t current = static_cast<uint64_t>(limbs_[i]) * other.limbs_[j] + result[i + j] + carry;
      result[i + j] = static_cast<uint32_t>(current);
      carry = current >> 32;
    }
    result[i + kLimbs] = static_cast<uint32_t>(carry);
  }
  for (size_t i = kLimbs; i < kLimbs * 2; ++i)
    if (result[i]) return false;
  const bool sign = negative() != other.negative();
  memcpy(limbs_, result, sizeof(limbs_));
  negative_ = sign;
  return true;
}

size_t BigInt::BitLength() const {
  for (size_t i = kLimbs; i-- > 0;) {
    if (limbs_[i]) {
      size_t bits = 32;
      while (bits && !(limbs_[i] & (1u << (bits - 1)))) --bits;
      return i * 32 + bits;
    }
  }
  return 0;
}

bool BigInt::Div(const BigInt& divisor, BigInt* remainder) {
  if (divisor.IsZero()) return false;
  // Schoolbook binary long division on magnitudes.
  BigInt quotient, rest;
  const size_t bits = BitLength();
  for (size_t i = bits; i-- > 0;) {
    rest.MulSmall(2);
    if (limbs_[i / 32] & (1u << (i % 32))) rest.limbs_[0] |= 1;
    if (compare_magnitude(rest.limbs_, divisor.limbs_, kLimbs) >= 0) {
      sub_magnitude(rest.limbs_, divisor.limbs_, kLimbs);
      quotient.limbs_[i / 32] |= 1u << (i % 32);
    }
  }
  const bool dividend_negative = negative();
  quotient.negative_ = dividend_negative != divisor.negative();
  rest.negative_ = dividend_negative;
  if (remainder) *remainder = rest;
  *this = quotient;
  return true;
}

double BigInt::ToDouble() const {
  // Round to nearest (ties to even) like Number(bigint): take the top 64
  // bits plus a sticky bit and let the double conversion round once.
  const size_t bits = BitLength();
  if (!bits) return 0;
  double value = 0;
  if (bits <= 64) {
    value = static_cast<double>(LowU64());
  } else {
    const size_t shift = bits - 64;
    uint64_t top = 0;
    for (size_t i = 0; i < 64; ++i) {
      const size_t bit = shift + i;
      if (limbs_[bit / 32] & (1u << (bit % 32))) top |= static_cast<uint64_t>(1) << i;
    }
    bool sticky = false;
    for (size_t bit = 0; bit < shift && !sticky; ++bit)
      if (limbs_[bit / 32] & (1u << (bit % 32))) sticky = true;
    // 64 -> 53 bits: round half to even with the sticky bit folded in.
    const uint64_t mantissa = top >> 11;
    const uint64_t rest = top & 0x7FF;
    uint64_t rounded = mantissa;
    if (rest > 0x400 || (rest == 0x400 && (sticky || (mantissa & 1)))) ++rounded;
    value = ldexp(static_cast<double>(rounded), static_cast<int>(shift + 11));
  }
  return negative() ? -value : value;
}

bool BigInt::FitsU64() const {
  for (size_t i = 2; i < kLimbs; ++i)
    if (limbs_[i]) return false;
  return true;
}

uint64_t BigInt::LowU64() const {
  return static_cast<uint64_t>(limbs_[0]) | (static_cast<uint64_t>(limbs_[1]) << 32);
}

bool BigInt::ToDecimal(char* output, size_t capacity) const {
  char digits[kLimbs * 10 + 2];
  size_t count = 0;
  BigInt value = Abs();
  if (value.IsZero()) digits[count++] = '0';
  while (!value.IsZero()) {
    uint32_t rest = 0;
    value.DivSmall(1000000000u, &rest);
    for (int i = 0; i < 9; ++i) {
      digits[count++] = static_cast<char>('0' + rest % 10);
      rest /= 10;
      if (value.IsZero() && rest == 0) break;
    }
  }
  const size_t sign = negative() ? 1 : 0;
  if (count + sign + 1 > capacity) return false;
  size_t used = 0;
  if (sign) output[used++] = '-';
  while (count) output[used++] = digits[--count];
  output[used] = '\0';
  return true;
}

bool BigInt::ToHex(char* output, size_t capacity) const {
  static const char kHex[] = "0123456789abcdef";
  char digits[kLimbs * 8 + 2];
  size_t count = 0;
  for (size_t i = 0; i < kLimbs; ++i)
    for (int nibble = 0; nibble < 8; ++nibble) digits[count++] = kHex[(limbs_[i] >> (nibble * 4)) & 15];
  while (count > 1 && digits[count - 1] == '0') --count;
  const size_t sign = negative() ? 1 : 0;
  if (count + sign + 1 > capacity) return false;
  size_t used = 0;
  if (sign) output[used++] = '-';
  while (count) output[used++] = digits[--count];
  output[used] = '\0';
  return true;
}

Value* BigInt::ToValue(Arena& arena) const {
  char text[kLimbs * 10 + 4];
  if (!ToDecimal(text, sizeof(text))) return nullptr;
  return tj::Str(arena, text);
}

// ---- amounts --------------------------------------------------------------------

bool DecimalToRaw(const char* input, size_t input_length, int fixed_decimals, BigInt* raw, int* decimals) {
  // /^([+-]?)(\d*)(?:\.(\d*))?(?:[eE]([+-]?\d+))?$/ over the trimmed value.
  size_t begin = 0, end = input_length;
  codec::provider_text::Trim(input, input_length, &begin, &end);
  const char* s = input + begin;
  const size_t n = end - begin;
  size_t i = 0;
  bool negative = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
  const size_t whole_begin = i;
  while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
  const size_t whole_end = i;
  size_t frac_begin = i, frac_end = i;
  if (i < n && s[i] == '.') {
    frac_begin = ++i;
    while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
    frac_end = i;
  }
  long exponent = 0;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool exp_negative = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) exp_negative = s[i++] == '-';
    const size_t exp_begin = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (exponent < 100000) exponent = exponent * 10 + (s[i] - '0');
      ++i;
    }
    if (i == exp_begin) return false;
    if (exp_negative) exponent = -exponent;
  }
  if (i != n) return false;
  if (whole_end == whole_begin && frac_end == frac_begin) return false;
  // digits = whole + frac with the point after pointPos digits.
  const size_t whole_length = whole_end - whole_begin, frac_length = frac_end - frac_begin;
  const size_t digit_count = whole_length + frac_length;
  long point = static_cast<long>(whole_length) + exponent;
  if (digit_count > 2048 || point > 4096 || point < -4096) return false;
  char digits[8192];
  size_t length = 0;
  if (point < 0) {
    for (long z = 0; z < -point; ++z) digits[length++] = '0';
    point = 0;
  }
  memcpy(digits + length, s + whole_begin, whole_length);
  length += whole_length;
  memcpy(digits + length, s + frac_begin, frac_length);
  length += frac_length;
  while (static_cast<size_t>(point) > length) digits[length++] = '0';
  // whole = digits[0, point) without leading zeros; frac = digits[point, …).
  const char* whole = digits;
  size_t whole_digits = static_cast<size_t>(point);
  while (whole_digits > 1 && whole[0] == '0') { ++whole; --whole_digits; }
  const char* frac = digits + point;
  size_t frac_digits = length - static_cast<size_t>(point);
  if (fixed_decimals < 0) {
    while (frac_digits && frac[frac_digits - 1] == '0') --frac_digits;
    if (frac_digits > 24) frac_digits = 24;
  } else {
    if (fixed_decimals > 24) return false;
    if (frac_digits > static_cast<size_t>(fixed_decimals)) frac_digits = static_cast<size_t>(fixed_decimals);
  }
  char combined[8300];
  size_t used = 0;
  if (whole_digits == 0) combined[used++] = '0';
  memcpy(combined + used, whole, whole_digits);
  used += whole_digits;
  memcpy(combined + used, frac, frac_digits);
  used += frac_digits;
  size_t scale = frac_digits;
  if (fixed_decimals >= 0) {
    while (scale < static_cast<size_t>(fixed_decimals)) { combined[used++] = '0'; ++scale; }
  }
  BigInt value;
  if (!BigInt::ParseDecimal(combined, used, &value)) return false;
  if (negative) value.Negate();
  *raw = value;
  *decimals = static_cast<int>(scale);
  return true;
}

Value* RawToDecimalString(Arena& arena, const BigInt& raw, int decimals) {
  if (decimals < 0 || decimals > 24) return tj::Null(arena);
  char magnitude[BigInt::kLimbs * 10 + 4];
  if (!raw.Abs().ToDecimal(magnitude, sizeof(magnitude))) return tj::Null(arena);
  size_t length = strlen(magnitude);
  char padded[BigInt::kLimbs * 10 + 32];
  size_t used = 0;
  // padStart(decimals + 1, '0').
  for (size_t i = length; i < static_cast<size_t>(decimals) + 1; ++i) padded[used++] = '0';
  std::copy_n(magnitude, length, padded + used);
  used += length;
  char out[BigInt::kLimbs * 10 + 40];
  size_t o = 0;
  if (raw.negative()) out[o++] = '-';
  if (decimals == 0) {
    memcpy(out + o, padded, used);
    o += used;
    return tj::Str(arena, out, o);
  }
  const size_t whole = used - static_cast<size_t>(decimals);
  memcpy(out + o, padded, whole);
  o += whole;
  size_t fraction = static_cast<size_t>(decimals);
  while (fraction && padded[whole + fraction - 1] == '0') --fraction;
  if (fraction) {
    out[o++] = '.';
    memcpy(out + o, padded + whole, fraction);
    o += fraction;
  }
  return tj::Str(arena, out, o);
}

// ---- encodings --------------------------------------------------------------------

bool IsHex(const char* text, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (hex_value(text[i]) < 0) return false;
  return true;
}

bool HexDecode(const char* hex, size_t length, uint8_t* output, size_t capacity, size_t* written) {
  if (length % 2) return false;
  if (length / 2 > capacity) return false;
  for (size_t i = 0; i < length; i += 2) {
    const int hi = hex_value(hex[i]), lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    output[i / 2] = static_cast<uint8_t>(hi * 16 + lo);
  }
  *written = length / 2;
  return true;
}

void HexEncode(const uint8_t* bytes, size_t length, char* output) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    output[i * 2] = kHex[bytes[i] >> 4];
    output[i * 2 + 1] = kHex[bytes[i] & 15];
  }
  output[length * 2] = '\0';
}

namespace {
constexpr char kBase58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
}

bool Base58Decode(const char* text, size_t length, uint8_t* output, size_t capacity, size_t* written) {
  // Big-endian base-256 accumulation (the BigInt loop of the TS helpers).
  uint8_t work[256];
  size_t size = 0;  // significant bytes, little-endian in work
  for (size_t i = 0; i < length; ++i) {
    const char* found = std::find(kBase58, kBase58 + 58, text[i]);
    if (found == kBase58 + 58 || !text[i]) return false;
    uint32_t carry = static_cast<uint32_t>(found - kBase58);
    for (size_t j = 0; j < size; ++j) {
      carry += static_cast<uint32_t>(work[j]) * 58;
      work[j] = static_cast<uint8_t>(carry);
      carry >>= 8;
    }
    while (carry) {
      if (size == sizeof(work)) return false;
      work[size++] = static_cast<uint8_t>(carry);
      carry >>= 8;
    }
  }
  size_t zeros = 0;
  while (zeros < length && text[zeros] == '1') ++zeros;
  if (zeros + size > capacity) return false;
  for (size_t i = 0; i < zeros; ++i) output[i] = 0;
  for (size_t i = 0; i < size; ++i) output[zeros + i] = work[size - 1 - i];
  *written = zeros + size;
  return true;
}

bool Base58Encode(const uint8_t* bytes, size_t length, char* output, size_t capacity) {
  uint8_t digits[512];
  size_t size = 0;
  for (size_t i = 0; i < length; ++i) {
    uint32_t carry = bytes[i];
    for (size_t j = 0; j < size; ++j) {
      carry += static_cast<uint32_t>(digits[j]) << 8;
      digits[j] = static_cast<uint8_t>(carry % 58);
      carry /= 58;
    }
    while (carry) {
      if (size == sizeof(digits)) return false;
      digits[size++] = static_cast<uint8_t>(carry % 58);
      carry /= 58;
    }
  }
  size_t zeros = 0;
  while (zeros < length && bytes[zeros] == 0) ++zeros;
  if (zeros + size + 1 > capacity) return false;
  size_t used = 0;
  for (size_t i = 0; i < zeros; ++i) output[used++] = '1';
  for (size_t i = 0; i < size; ++i) output[used++] = kBase58[digits[size - 1 - i]];
  output[used] = '\0';
  return true;
}

bool Base58CheckDecode(const char* text, size_t length, uint8_t* payload, size_t capacity, size_t* written) {
  uint8_t decoded[300];
  size_t size = 0;
  if (!Base58Decode(text, length, decoded, sizeof(decoded), &size) || size < 4) return false;
  uint8_t first[32], second[32];
  Sha256(decoded, size - 4, first);
  Sha256(first, sizeof(first), second);
  if (memcmp(second, decoded + size - 4, 4) != 0) return false;
  if (size - 4 > capacity) return false;
  memcpy(payload, decoded, size - 4);
  *written = size - 4;
  return true;
}

bool Base64Decode(const char* text, size_t length, uint8_t* output, size_t capacity, size_t* written) {
  uint32_t buffer = 0;
  int bits = 0;
  size_t used = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    int value = -1;
    if (c >= 'A' && c <= 'Z') value = c - 'A';
    else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
    else if (c >= '0' && c <= '9') value = c - '0' + 52;
    else if (c == '+' || c == '-') value = 62;
    else if (c == '/' || c == '_') value = 63;
    else if (c == '=') break;
    if (value < 0) continue;
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (used == capacity) return false;
      output[used++] = static_cast<uint8_t>(buffer >> bits);
    }
  }
  *written = used;
  return true;
}

bool Base64Encode(const uint8_t* bytes, size_t length, bool url, bool pad, char* output, size_t capacity) {
  const char* alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                             : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t used = 0;
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (i + 1 < length ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < length ? bytes[i + 2] : 0);
    const size_t produce = i + 2 < length ? 4 : i + 1 < length ? 3 : 2;
    for (size_t j = 0; j < 4; ++j) {
      if (used + 2 > capacity) return false;
      if (j < produce) output[used++] = alphabet[(chunk >> (18 - 6 * j)) & 63];
      else if (pad) output[used++] = '=';
    }
  }
  output[used] = '\0';
  return true;
}

void Sha256(const uint8_t* bytes, size_t length, uint8_t digest[32]) {
  mbedtls_sha256(bytes, length, digest, 0);
}

void Sha512(const uint8_t* bytes, size_t length, uint8_t digest[64]) {
  mbedtls_sha512(bytes, length, digest, 0);
}

void Sha1(const uint8_t* bytes, size_t length, uint8_t digest[20]) { mbedtls_sha1(bytes, length, digest); }

void Keccak256(const uint8_t* bytes, size_t length, uint8_t digest[32]) { auth::evm_keccak256(bytes, length, digest); }

// ---- sanitizers -----------------------------------------------------------------

Value* SanitizeSymbol(Arena& arena, const char* text, size_t length) {
  return codec::provider_text::SanitizeSymbol(arena, text, length);
}

Value* SanitizeName(Arena& arena, const char* text, size_t length) {
  return codec::provider_text::SanitizeName(arena, text, length);
}

Value* SanitizeText(Arena& arena, const char* text, size_t length) {
  return codec::provider_text::SanitizeText(arena, text, length);
}

Value* PrintableMemoFromBase64(Arena& arena, const char* base64, size_t length) {
  if (!base64 || !length) return tj::Null(arena);
  uint8_t bytes[1024];
  size_t size = 0;
  if (!Base64Decode(base64, length, bytes, sizeof(bytes), &size)) return tj::Null(arena);
  if (size == 0 || size > 512) return tj::Null(arena);
  for (size_t i = 0; i < size; ++i)
    if ((bytes[i] < 0x20 && bytes[i] != 0x0a) || bytes[i] > 0x7e) return tj::Null(arena);
  Value* text = codec::provider_text::SanitizeText(arena, reinterpret_cast<const char*>(bytes), size);
  if (!text) return nullptr;
  return text->length == 0 ? tj::Null(arena) : text;
}

Value* PrintableAsciiFromHex(Arena& arena, const char* hex, size_t length) {
  if (!length || length % 2 || !IsHex(hex, length)) return tj::Null(arena);
  uint8_t bytes[512]{};
  size_t size = 0;
  if (length / 2 > sizeof(bytes) || !HexDecode(hex, length, bytes, sizeof(bytes), &size)) return tj::Null(arena);
  for (size_t i = 0; i < size; ++i)
    if (bytes[i] < 0x20 || bytes[i] > 0x7e) return tj::Null(arena);
  Value* text = codec::provider_text::SanitizeSymbol(arena, reinterpret_cast<const char*>(bytes), size);
  if (!text) return nullptr;
  return text->length == 0 ? tj::Null(arena) : text;
}

// ---- EVM ABI ----------------------------------------------------------------------

namespace {

// strip0x(address).toLowerCase().padStart(64, '0') appended to `out`.
void append_address_word(char* out, size_t* used, const char* address) {
  size_t length = strlen(address);
  const char* hex = strip0x(address, &length);
  for (size_t i = length; i < 64; ++i) out[(*used)++] = '0';
  for (size_t i = 0; i < length; ++i) {
    char c = hex[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out[(*used)++] = c;
  }
}

void append_uint_word(char* out, size_t* used, uint64_t value) {
  char digits[17];
  snprintf(digits, sizeof(digits), "%llx", static_cast<unsigned long long>(value));
  const size_t length = strlen(digits);
  for (size_t i = length; i < 64; ++i) out[(*used)++] = '0';
  std::copy_n(digits, length, out + *used);
  *used += length;
}

const char* selector_call(Arena& arena, const char* selector, const char* address) {
  const size_t address_length = address ? strlen(address) : 0;
  char* out = static_cast<char*>(arena.Alloc(2 + 8 + 64 + address_length + 1, 1));
  if (!out) return nullptr;
  size_t used = 0;
  out[used++] = '0';
  out[used++] = 'x';
  memcpy(out + used, selector, 8);
  used += 8;
  if (address) append_address_word(out, &used, address);
  out[used] = '\0';
  return out;
}

// wordAt(hex, byteOffset): false where TypeScript throws (truncated word, or
// an offset no string could reach); the value must fit in 53 bits to be used
// as a later offset, otherwise `*big` is set and callers treat it as huge.
bool word_at(const char* hex, size_t length, double byte_offset, BigInt* value) {
  if (!(byte_offset >= 0) || byte_offset * 2 + 64 > static_cast<double>(length)) return false;
  const size_t start = static_cast<size_t>(byte_offset) * 2;
  return BigInt::ParseHex(hex + start, 64, value);
}

}  // namespace

const char* AbiBalanceOf(Arena& arena, const char* holder) { return selector_call(arena, "70a08231", holder); }
const char* AbiGetEthBalance(Arena& arena, const char* holder) { return selector_call(arena, "4d2301cc", holder); }
const char* AbiDecimals(Arena& arena) { return selector_call(arena, "313ce567", nullptr); }
const char* AbiSymbol(Arena& arena) { return selector_call(arena, "95d89b41", nullptr); }
const char* AbiName(Arena& arena) { return selector_call(arena, "06fdde03", nullptr); }
const char* AbiTotalSupply(Arena& arena) { return selector_call(arena, "18160ddd", nullptr); }

const char* AbiAddressTopic(Arena& arena, const char* address) {
  const size_t length = strlen(address);
  char* out = static_cast<char*>(arena.Alloc(2 + 64 + length + 1, 1));
  if (!out) return nullptr;
  size_t used = 0;
  out[used++] = '0';
  out[used++] = 'x';
  append_address_word(out, &used, address);
  out[used] = '\0';
  return out;
}

const char* AbiAggregate3(Arena& arena, const AbiCall3* calls, size_t count) {
  size_t total = 2 + 8 + 64 * 2 + count * 64 + 1;
  for (size_t i = 0; i < count; ++i) {
    size_t length = strlen(calls[i].call_data);
    strip0x(calls[i].call_data, &length);
    const size_t target_length = strlen(calls[i].target);
    total += (target_length > 64 ? target_length : 64) + 64 * 3 + ((length + 63) / 64) * 64 + 64;
  }
  char* out = static_cast<char*>(arena.Alloc(total, 1));
  if (!out) return nullptr;
  size_t used = 0;
  out[used++] = '0';
  out[used++] = 'x';
  memcpy(out + used, "82ad56cb", 8);
  used += 8;
  append_uint_word(out, &used, 32);
  append_uint_word(out, &used, count);
  // Element offsets from the first byte after the length word.
  uint64_t cursor = count * 32;
  for (size_t i = 0; i < count; ++i) {
    append_uint_word(out, &used, cursor);
    size_t length = strlen(calls[i].call_data);
    strip0x(calls[i].call_data, &length);
    size_t target_length = strlen(calls[i].target);
    strip0x(calls[i].target, &target_length);
    const size_t target_hex = target_length > 64 ? target_length : 64;
    const size_t tuple_hex = target_hex + 64 * 3 + ((length + 63) / 64) * 64;
    cursor += tuple_hex / 2;
  }
  for (size_t i = 0; i < count; ++i) {
    size_t length = strlen(calls[i].call_data);
    const char* data = strip0x(calls[i].call_data, &length);
    append_address_word(out, &used, calls[i].target);
    append_uint_word(out, &used, 1);
    append_uint_word(out, &used, 96);
    append_uint_word(out, &used, length / 2);
    memcpy(out + used, data, length);
    used += length;
    const size_t padded = ((length + 63) / 64) * 64;
    for (size_t p = length; p < padded; ++p) out[used++] = '0';
  }
  out[used] = '\0';
  return out;
}

bool AbiDecodeAggregate3(Arena& arena, const char* input, size_t input_length, AbiResult** results,
                         size_t* count) {
  size_t begin = 0, end = input_length;
  codec::provider_text::Trim(input, input_length, &begin, &end);
  size_t length = end - begin;
  const char* hex = strip0x(input + begin, &length);
  if (length < 128) return false;
  BigInt word;
  if (!word_at(hex, length, 0, &word)) return false;
  const double array_offset = word.ToDouble();
  if (!word_at(hex, length, array_offset, &word)) return false;
  const double total = word.ToDouble();
  if (total > 4096) return false;
  const size_t n = static_cast<size_t>(total);
  AbiResult* out = n ? static_cast<AbiResult*>(arena.Alloc(sizeof(AbiResult) * n)) : nullptr;
  if (n && !out) return false;
  const double base = array_offset + 32;
  for (size_t i = 0; i < n; ++i) {
    if (!word_at(hex, length, base + static_cast<double>(i) * 32, &word)) return false;
    const double tuple_offset = base + word.ToDouble();
    if (!word_at(hex, length, tuple_offset, &word)) return false;
    const bool success = !word.IsZero();
    if (!word_at(hex, length, tuple_offset + 32, &word)) return false;
    const double data_offset = tuple_offset + word.ToDouble();
    if (!word_at(hex, length, data_offset, &word)) return false;
    const double data_length = word.ToDouble();
    // hex.slice(start, start + len * 2) clamps at the end of the string.
    const double start = (data_offset + 32) * 2;
    const double stop = start + data_length * 2;
    const size_t s = start >= static_cast<double>(length) ? length : static_cast<size_t>(start);
    const size_t e = stop >= static_cast<double>(length) ? length : static_cast<size_t>(stop);
    out[i].success = success;
    out[i].data = hex + s;
    out[i].length = e > s ? e - s : 0;
  }
  *results = out;
  *count = n;
  return true;
}

BigInt AbiUint256(const char* hex, size_t length) {
  BigInt value;
  if (!AbiUint256Checked(hex, length, &value)) return BigInt();
  return value;
}

bool AbiUint256Checked(const char* hex, size_t length, BigInt* out) {
  hex = strip0x(hex, &length);
  if (length < 64) {
    *out = BigInt();
    return true;
  }
  if (!IsHex(hex, 64)) return false;
  return BigInt::ParseHex(hex, 64, out);
}

namespace {
// Buffer.from(hex, 'hex'): pairs decoded until the first invalid pair; a
// trailing odd digit is dropped.
Value* hex_utf8(Arena& arena, const char* hex, size_t length) {
  uint8_t* bytes = static_cast<uint8_t*>(arena.Alloc(length / 2 + 1, 1));
  if (!bytes) return nullptr;
  size_t size = 0;
  for (size_t i = 0; i + 1 < length; i += 2) {
    const int hi = hex_value(hex[i]), lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) break;
    bytes[size++] = static_cast<uint8_t>(hi * 16 + lo);
  }
  return utf8_value(arena, bytes, size);
}
}  // namespace

Value* AbiString(Arena& arena, const char* input, size_t input_length) {
  size_t length = input_length;
  const char* hex = strip0x(input, &length);
  if (length == 0) return tj::Null(arena);
  if (length == 64) {
    // bytes32 symbol: /(00)+$/ removes the longest even run of trailing '0'
    // characters that ends the string.
    size_t zeros = 0;
    while (zeros < length && hex[length - 1 - zeros] == '0') ++zeros;
    const size_t keep = length - (zeros / 2) * 2;
    return hex_utf8(arena, hex, keep);
  }
  BigInt word;
  if (!word_at(hex, length, 0, &word)) return tj::Null(arena);
  const double offset = word.ToDouble();
  if (!word_at(hex, length, offset, &word)) return tj::Null(arena);
  const double string_length = word.ToDouble();
  const double start = (offset + 32) * 2;
  const double stop = start + string_length * 2;
  const size_t s = start >= static_cast<double>(length) ? length : static_cast<size_t>(start);
  const size_t e = stop >= static_cast<double>(length) ? length : static_cast<size_t>(stop);
  return hex_utf8(arena, hex + s, e > s ? e - s : 0);
}

// ---- transfers --------------------------------------------------------------------

Value* TransferRecordId(Arena& arena, const char* chain, const char* hash, const char* unit) {
  size_t cb = 0, ce = 0, hb = 0, he = 0;
  codec::provider_text::Trim(chain, strlen(chain), &cb, &ce);
  codec::provider_text::Trim(hash, strlen(hash), &hb, &he);
  if (cb == ce || hb == he) return nullptr;
  if (unit) {
    size_t ub = 0, ue = 0;
    codec::provider_text::Trim(unit, strlen(unit), &ub, &ue);
    if (ue > ub) {
      return Format(arena, "%.*s:%.*s:%.*s", static_cast<int>(ce - cb), chain + cb, static_cast<int>(he - hb),
                    hash + hb, static_cast<int>(ue - ub), unit + ub);
    }
  }
  return Format(arena, "%.*s:%.*s", static_cast<int>(ce - cb), chain + cb, static_cast<int>(he - hb), hash + hb);
}

Value* NativeAssetJson(Arena& arena, const Network& network) {
  Value* asset = tj::Obj(arena);
  const char* symbol = network.has_native ? network.native_symbol : "";
  const double decimals = network.has_native ? network.native_decimals : 18;
  if (!asset || !tj::Set(arena, asset, "chainId", tj::Str(arena, network.id)) ||
      !tj::Set(arena, asset, "symbol", tj::Str(arena, symbol)) ||
      !tj::Set(arena, asset, "name", tj::Str(arena, symbol)) ||
      !tj::Set(arena, asset, "decimals", tj::Num(arena, decimals)) ||
      !tj::Set(arena, asset, "contractAddress", tj::Null(arena)))
    return nullptr;
  return asset;
}

const char* DirectionFromFlow(bool incoming, bool outgoing) {
  if (incoming && outgoing) return "selfTransfer";
  return incoming ? "incoming" : "outgoing";
}

const char* TypeFromDirection(const char* direction) {
  return strcmp(direction, "incoming") == 0 ? "receive" : "send";
}

Value* NewTransfer(Arena& arena) {
  static const char* const kKeys[] = {"recordId", "txType", "status", "direction", "ts", "hash",
                                      "blockNumber", "fromAddress", "toAddress", "counterparty",
                                      "tokenId", "asset", "amountRaw", "amountDecimals", "feeRaw",
                                      "feeDecimals", "feeSymbol", "memo", "method", "error"};
  Value* record = tj::Obj(arena);
  if (!record) return nullptr;
  for (const char* key : kKeys)
    if (!tj::Set(arena, record, key, tj::Null(arena))) return nullptr;
  if (!tj::Set(arena, record, "metadata", tj::Obj(arena))) return nullptr;
  return record;
}

bool WalkDescendingByHeight(double since_height, double budget, double max_pages,
                            bool (*fetch_page)(void*, double, const char*, HeightWalkPage*, Error*),
                            bool (*height_of)(void*, const Value*, double*),
                            void (*on_row)(void*, Value*), void* context, HeightWalkResult* result,
                            Error* error) {
  double high_water = since_height;
  bool complete = false;
  double rows_seen = 0;
  const char* cursor = nullptr;
  for (size_t page = 0; static_cast<double>(page) < max_pages && rows_seen < budget; ++page) {
    HeightWalkPage fetched;
    if (!fetch_page(context, page, cursor, &fetched, error)) return false;
    const size_t count = tj::IsArray(fetched.rows) ? fetched.rows->count : 0;
    if (count == 0) {
      complete = true;
      break;
    }
    bool caught_up = false;
    for (size_t i = 0; i < count; ++i) {
      Value* row = fetched.rows->items[i];
      double height = 0;
      const bool has_height = height_of(context, row, &height);
      if (has_height && height <= since_height) {
        caught_up = true;
        break;
      }
      ++rows_seen;
      if (has_height && height > high_water) high_water = height;
      on_row(context, row);
    }
    if (caught_up) {
      complete = true;
      break;
    }
    cursor = fetched.cursor;
    if (!fetched.more) {
      complete = true;
      break;
    }
  }
  result->high_water = high_water;
  result->complete = complete;
  result->rows_seen = rows_seen;
  return true;
}

}  // namespace fortis::chains
