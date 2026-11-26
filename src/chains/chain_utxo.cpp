#include "chain_family_drivers.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// The UTXO families of the address, transaction and network routes, ported
// from the earlier TypeScript implementation:
//  - the drivers: EsploraDriver (bitcoin) and BlockbookDriver
//    (bitcoinCash, dash, dogecoin, litecoin, zcash; dogecoin/litecoin/zcash
//    prepend the keyed GetBlock endpoint rebuilt from GETBLOCK_*_TOKEN);
//  - the transfer sources: EsploraTransferSource ('esplora') and
//    BlockbookTransferSource ('blockbook') with the shared net-flow rule;
//  - the Bitcoin Cash, Dash and Zcash address/transaction checks.
//
// `utxo_detail` also carries the JavaScript coercions (String(value),
// BigInt(value), truthiness) the UTXO and Cardano views/sources share; the
// other chain_utxo*/chain_cardano* files redeclare what they use.
namespace fortis::chains {
namespace utxo_detail {

// String(value) for a JSON value (nullptr = undefined), appended to `out`:
// numbers as Number.prototype.toString, objects "[object Object]", arrays
// joined with "," (null/undefined elements empty).
void JsString(const Value* v, OutBuffer& out) {
  if (!v) { out.Text("undefined"); return; }
  switch (v->type) {
    case tj::Type::kNull: out.Text("null"); return;
    case tj::Type::kTrue: out.Text("true"); return;
    case tj::Type::kFalse: out.Text("false"); return;
    case tj::Type::kNumber: {
      char text[40];
      JsNumberText(v->number, text, sizeof(text));
      out.Text(text);
      return;
    }
    case tj::Type::kString: out.Append(v->text, v->length); return;
    case tj::Type::kObject: out.Text("[object Object]"); return;
    case tj::Type::kArray:
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        const Value* item = v->items[i];
        if (item && item->type != tj::Type::kNull) JsString(item, out);
      }
      return;
  }
}

// String(value) as an arena string (null on arena exhaustion).
const char* JsStr(Arena& arena, const Value* v, size_t* length) {
  if (v && v->type == tj::Type::kString) {
    const char* copy = arena.CopyText(v->text, v->length);
    if (length) *length = v->length;
    return copy;
  }
  uint8_t buffer[4096];
  OutBuffer out{buffer, sizeof(buffer), 0, true};
  JsString(v, out);
  if (!out.ok) return nullptr;
  if (length) *length = out.used;
  return arena.CopyText(reinterpret_cast<const char*>(buffer), out.used);
}

bool AllDigits(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// /^\d+$/.test(value ?? '') (RegExp.test coerces with String()).
bool DigitsValue(const Value* v) {
  if (!v || v->type == tj::Type::kNull) return false;
  if (v->type == tj::Type::kString) return AllDigits(v->text, v->length);
  uint8_t buffer[512];
  OutBuffer out{buffer, sizeof(buffer), 0, true};
  JsString(v, out);
  return out.ok && AllDigits(reinterpret_cast<const char*>(buffer), out.used);
}

// JavaScript truthiness (nullptr = undefined).
bool Truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull: case tj::Type::kFalse: return false;
    case tj::Type::kNumber: return v->number != 0 && !isnan(v->number);
    case tj::Type::kString: return v->length != 0;
    default: return true;
  }
}

// BigInt(value): integral numbers, strings (blank = 0n), booleans. A
// non-integral number is the RangeError JavaScript throws; anything else a
// TypeError/SyntaxError (kUpstream).
bool BigIntOf(const Value* v, BigInt* out, Error* error) {
  if (!v || v->type == tj::Type::kNull) {
    error->Upstream("Cannot convert %s to a BigInt", v ? "null" : "undefined");
    return false;
  }
  if (v->type == tj::Type::kTrue || v->type == tj::Type::kFalse) {
    *out = BigInt::FromU64(v->type == tj::Type::kTrue ? 1 : 0);
    return true;
  }
  if (v->type == tj::Type::kNumber) {
    if (BigInt::FromDouble(v->number, out)) return true;
    char text[40];
    JsNumberText(v->number, text, sizeof(text));
    error->Range("The number %s cannot be converted to a BigInt because it is not an integer", text);
    return false;
  }
  if (v->type == tj::Type::kString) {
    size_t begin = 0, end = v->length;
    while (begin < end && (v->text[begin] == ' ' || v->text[begin] == '\t' || v->text[begin] == '\n' ||
                           v->text[begin] == '\r')) ++begin;
    while (end > begin && (v->text[end - 1] == ' ' || v->text[end - 1] == '\t' ||
                           v->text[end - 1] == '\n' || v->text[end - 1] == '\r')) --end;
    if (begin == end) { *out = BigInt(); return true; }
    if (BigInt::Parse(v->text, v->length, out)) return true;
    error->Upstream("Cannot convert string to a BigInt");
    return false;
  }
  error->Upstream("Cannot convert value to a BigInt");
  return false;
}

// `for (const x of value ?? [])`: arrays iterate, a string iterates its
// characters (no element of which carries the members the loops read), null
// and undefined iterate nothing; any other value is the TypeError "not
// iterable".
bool Iterable(const Value* v, Value* const** items, size_t* count, Error* error) {
  *items = nullptr;
  *count = 0;
  if (!v || v->type == tj::Type::kNull || v->type == tj::Type::kString) return true;
  if (v->type == tj::Type::kArray) {
    *items = v->items;
    *count = v->count;
    return true;
  }
  error->Upstream("value is not iterable");
  return false;
}

// Property access on an element (`x.member`): null/undefined throw TypeError.
bool Accessible(const Value* v, Error* error) {
  if (v && v->type != tj::Type::kNull) return true;
  error->Upstream("Cannot read properties of %s", v ? "null" : "undefined");
  return false;
}

// The TypeScript zcashHasShieldedComponents: 1 true, 0 false,
// -1 null.
namespace {
struct ZReader {
  const uint8_t* bytes;
  size_t length;
  size_t offset = 0;
  bool ok = true;
  void Skip(uint64_t count) {
    if (!ok || count > 9007199254740991ULL || count > length - offset) { ok = false; return; }
    offset += static_cast<size_t>(count);
  }
  uint8_t U8() {
    if (!ok || offset >= length) { ok = false; return 0; }
    return bytes[offset++];
  }
  uint32_t U16() { const uint32_t a = U8(); return a | (static_cast<uint32_t>(U8()) << 8); }
  uint32_t U32() {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(U8()) << (8 * i);
    return value;
  }
  uint64_t U64() {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(U8()) << (8 * i);
    return value;
  }
  uint64_t CompactSize() {
    const uint8_t first = U8();
    if (!ok) return 0;
    if (first < 0xfd) return first;
    if (first == 0xfd) {
      const uint32_t value = U16();
      if (value < 0xfd) ok = false;
      return value;
    }
    if (first == 0xfe) {
      const uint32_t value = U32();
      if (value < 0x10000) ok = false;
      return value;
    }
    const uint64_t value = U64();
    if (value < 0x100000000ULL || value > 9007199254740991ULL) ok = false;
    return value;
  }
  void TransparentBundle() {
    const uint64_t inputs = CompactSize();
    for (uint64_t i = 0; ok && i < inputs; ++i) {
      Skip(36);
      Skip(CompactSize());
      Skip(4);
    }
    const uint64_t outputs = CompactSize();
    for (uint64_t i = 0; ok && i < outputs; ++i) {
      Skip(8);
      Skip(CompactSize());
    }
  }
};
}  // namespace

int ZcashHasShieldedComponents(Arena& arena, const Value* raw) {
  if (!raw || raw->type != tj::Type::kString || raw->length % 2 != 0 || raw->length == 0 ||
      !IsHex(raw->text, raw->length))
    return -1;
  const size_t size = raw->length / 2;
  auto* bytes = static_cast<uint8_t*>(arena.Alloc(size, 1));
  size_t written = 0;
  if (!bytes || !HexDecode(raw->text, raw->length, bytes, size, &written) || written != size) return -1;
  ZReader r{bytes, size};
  const uint32_t version = r.U32() & 0x7fffffffU;
  if (!r.ok) return -1;
  if (version == 4) {
    r.Skip(4);
    r.TransparentBundle();
    r.Skip(8);
    const uint64_t value_balance = r.U64();
    if (!r.ok) return -1;
    if (value_balance != 0) return 1;
    for (int i = 0; i < 3; ++i) {
      const uint64_t count = r.CompactSize();
      if (!r.ok) return -1;
      if (count > 0) return 1;
    }
    return 0;
  }
  if (version == 5 || version == 6) {
    r.Skip(16);
    r.TransparentBundle();
    const int bundles = version == 6 ? 4 : 3;
    for (int i = 0; i < bundles; ++i) {
      const uint64_t count = r.CompactSize();
      if (!r.ok) return -1;
      if (count > 0) return 1;
    }
    return 0;
  }
  return -1;
}

}  // namespace utxo_detail

namespace {

using utxo_detail::Accessible;
using utxo_detail::BigIntOf;
using utxo_detail::DigitsValue;
using utxo_detail::Iterable;
using utxo_detail::JsStr;
using utxo_detail::Truthy;

constexpr const char* kBase58Alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool IsBase58Char(char c) { return c && strchr(kBase58Alphabet, c) != nullptr; }
bool IsLowerAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); }

bool Base58Run(const char* text, size_t length, size_t min, size_t max) {
  if (length < min || length > max) return false;
  for (size_t i = 0; i < length; ++i) if (!IsBase58Char(text[i])) return false;
  return true;
}

bool LowerRun(const char* text, size_t length, size_t min, size_t max) {
  if (length < min || length > max) return false;
  for (size_t i = 0; i < length; ++i) if (!IsLowerAlnum(text[i])) return false;
  return true;
}

// /^(bc1[0-9a-z]{20,90}|[13][1-9A-HJ-NP-Za-km-z]{25,39})$/
bool BitcoinAddress(const char* a, size_t n) {
  if (n > 3 && strncmp(a, "bc1", 3) == 0 && LowerRun(a + 3, n - 3, 20, 90)) return true;
  return n > 1 && (a[0] == '1' || a[0] == '3') && Base58Run(a + 1, n - 1, 25, 39);
}

// /^[DA9][1-9A-HJ-NP-Za-km-z]{25,39}$/
bool DogecoinAddress(const char* a, size_t n) {
  return n > 1 && (a[0] == 'D' || a[0] == 'A' || a[0] == '9') && Base58Run(a + 1, n - 1, 25, 39);
}

// /^(ltc1[0-9a-z]{20,90}|[LM3][1-9A-HJ-NP-Za-km-z]{25,39})$/
bool LitecoinAddress(const char* a, size_t n) {
  if (n > 4 && strncmp(a, "ltc1", 4) == 0 && LowerRun(a + 4, n - 4, 20, 90)) return true;
  return n > 1 && (a[0] == 'L' || a[0] == 'M' || a[0] == '3') && Base58Run(a + 1, n - 1, 25, 39);
}

// The TypeScript validBitcoinCashAddress (strict mainnet CashAddr).
constexpr const char* kCashAddrCharset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
constexpr const char* kCashAddrPrefix = "bitcoincash";

uint64_t CashAddrPolymod(const uint8_t* values, size_t count) {
  static const uint64_t kGenerators[5] = {0x98f2bc8e61ULL, 0x79b76d99e2ULL, 0xf33e5fb3c4ULL,
                                          0xae2eabe2a8ULL, 0x1e4f43e470ULL};
  uint64_t checksum = 1;
  for (size_t i = 0; i < count; ++i) {
    const uint64_t top = checksum >> 35;
    checksum = ((checksum & 0x07ffffffffULL) << 5) ^ values[i];
    for (int bit = 0; bit < 5; ++bit)
      if ((top >> bit) & 1) checksum ^= kGenerators[bit];
  }
  return checksum ^ 1;
}

bool BitcoinCashAddress(const char* address, size_t length) {
  if (length < 22 || length > 125) return false;
  bool has_lower = false, has_upper = false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(address[i]);
    if (c >= 0x80) return false;  // outside the CashAddr alphabet either way
    if (c >= 'a' && c <= 'z') has_lower = true;
    if (c >= 'A' && c <= 'Z') has_upper = true;
  }
  if (has_lower && has_upper) return false;
  char lower[128];
  for (size_t i = 0; i < length; ++i) {
    const char c = address[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  lower[length] = '\0';
  const char* first = strchr(lower, ':');
  const char* last = strrchr(lower, ':');
  const size_t prefix_length = strlen(kCashAddrPrefix);
  if (!first || first != last || static_cast<size_t>(first - lower) != prefix_length ||
      strncmp(lower, kCashAddrPrefix, prefix_length) != 0)
    return false;
  const char* body = first + 1;
  const size_t body_length = length - prefix_length - 1;
  if (body_length < 9) return false;
  uint8_t values[160];
  size_t count = 0;
  for (size_t i = 0; i < prefix_length; ++i) values[count++] = static_cast<uint8_t>(kCashAddrPrefix[i] & 31);
  values[count++] = 0;
  const size_t data_start = count;
  for (size_t i = 0; i < body_length; ++i) {
    const char* hit = strchr(kCashAddrCharset, body[i]);
    if (!body[i] || !hit) return false;
    values[count++] = static_cast<uint8_t>(hit - kCashAddrCharset);
  }
  if (CashAddrPolymod(values, count) != 0) return false;
  // convertBits(values.slice(0, -8), 5, 8) with strict padding.
  uint32_t accumulator = 0;
  int bits = 0;
  uint8_t decoded[128];
  size_t decoded_count = 0;
  for (size_t i = data_start; i < count - 8; ++i) {
    accumulator = (accumulator << 5) | values[i];
    bits += 5;
    while (bits >= 8) {
      bits -= 8;
      decoded[decoded_count++] = static_cast<uint8_t>((accumulator >> bits) & 0xff);
    }
  }
  if (bits >= 5 || ((accumulator << (8 - bits)) & 0xff) != 0) return false;
  if (!decoded_count) return false;
  static const size_t kHashSizes[8] = {20, 24, 28, 32, 40, 48, 56, 64};
  const uint8_t version = decoded[0];
  if ((version & 0x80) != 0 || ((version >> 3) & 0x0f) > 1) return false;
  return decoded_count - 1 == kHashSizes[version & 0x07];
}

// Base58Check with a double-SHA256 checksum over the first `payload` bytes.
bool Base58Checked(const char* address, size_t length, uint8_t* decoded, size_t capacity,
                   size_t expected, size_t payload) {
  size_t written = 0;
  if (!Base58Decode(address, length, decoded, capacity, &written) || written != expected) return false;
  uint8_t first[32], second[32];
  Sha256(decoded, payload, first);
  Sha256(first, sizeof(first), second);
  return memcmp(second, decoded + payload, 4) == 0;
}

// The TypeScript validDashAddress: mainnet P2PKH (0x4c) / P2SH (0x10).
bool DashAddress(const char* address, size_t length) {
  if (length < 25 || length > 35) return false;
  uint8_t decoded[64];
  if (!Base58Checked(address, length, decoded, sizeof(decoded), 25, 21)) return false;
  return decoded[0] == 0x4c || decoded[0] == 0x10;
}

// The TypeScript validZcashTransparentAddress: t1 (1cb8) / t3 (1cbd).
bool ZcashAddress(const char* address, size_t length) {
  if (length != 35 || address[0] != 't' || (address[1] != '1' && address[1] != '3')) return false;
  uint8_t decoded[64];
  if (!Base58Checked(address, length, decoded, sizeof(decoded), 26, 22)) return false;
  const uint8_t second = decoded[1] == 0xb8 ? 0xb8 : 0xbd;
  return decoded[0] == 0x1c && decoded[1] == second;
}

// A NUL-terminated printf arena string (null when the arena is exhausted).
const char* Text(Arena& arena, const char* format, ...) __attribute__((format(printf, 2, 3)));
const char* Text(Arena& arena, const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int length = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  char* out = length < 0 ? nullptr : static_cast<char*>(arena.Alloc(static_cast<size_t>(length) + 1, 1));
  if (out) vsnprintf(out, static_cast<size_t>(length) + 1, format, args);
  va_end(args);
  return out;
}

const char* PathFor(Arena& arena, const char* prefix, const char* address, const char* suffix) {
  const char* encoded = EncodeUri(arena, address);
  return encoded ? Text(arena, "%s%s%s", prefix, encoded, suffix) : nullptr;
}

bool ArenaFail(Error* error) {
  error->Upstream("arena exhausted");
  return false;
}

Value* NativeEntry(Arena& arena, const char* balance, const Network& network, const char* default_symbol,
                   uint32_t default_decimals, const char* source) {
  Value* native = tj::Obj(arena);
  const char* symbol = network.has_native ? network.native_symbol : default_symbol;
  const uint32_t decimals = network.has_native ? network.native_decimals : default_decimals;
  if (!native || !tj::Set(arena, native, "balanceRaw", tj::Str(arena, balance)) ||
      !tj::Set(arena, native, "decimals", tj::Num(arena, decimals)) ||
      !tj::Set(arena, native, "symbol", tj::Str(arena, symbol)) ||
      !tj::Set(arena, native, "source", tj::Str(arena, source)))
    return nullptr;
  Value* result = tj::Obj(arena);
  if (!result || !tj::Set(arena, result, "native", native) ||
      !tj::Set(arena, result, "tokens", tj::Arr(arena)))
    return nullptr;
  return result;
}

bool Always(const config::ProviderRuntimeConfig&) { return true; }

// ---- Esplora (bitcoin) ---------------------------------------------------------

}  // namespace

bool EsploraDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                     Value** result, Error* error) const {
  Arena& arena = *ctx.arena;
  const size_t length = strlen(address);
  if (!BitcoinAddress(address, length)) {
    error->Range("not a Bitcoin address: %s", address);
    return false;
  }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = PathFor(arena, "/address/", address, "");
  call.lane = "esplora";
  if (!call.path) return ArenaFail(error);
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  const Value* stats = call.value;
  if (!Accessible(stats, error)) return false;
  const Value* chain = tj::Get(stats, "chain_stats");
  const Value* funded_value = tj::Get(chain, "funded_txo_sum");
  const Value* spent_value = tj::Get(chain, "spent_txo_sum");
  Value zero;
  zero.type = tj::Type::kNumber;
  BigInt funded, spent;
  if (!BigIntOf(funded_value && funded_value->type != tj::Type::kNull ? funded_value : &zero, &funded, error) ||
      !BigIntOf(spent_value && spent_value->type != tj::Type::kNull ? spent_value : &zero, &spent, error))
    return false;
  if (!funded.Sub(spent)) { error->Upstream("balance overflow"); return false; }
  char balance[220];
  if (!funded.ToDecimal(balance, sizeof(balance))) { error->Upstream("balance overflow"); return false; }
  *result = NativeEntry(arena, balance, network, "BTC", 8, "esplora");
  return *result ? true : ArenaFail(error);
}

namespace {

// ---- Blockbook (bitcoinCash / dash / dogecoin / litecoin / zcash) ---------------

const char* DogecoinGetBlockToken(const config::ProviderRuntimeConfig& config) {
  return config.getblock_doge_token_configured ? config.getblock_doge_token : nullptr;
}
const char* LitecoinGetBlockToken(const config::ProviderRuntimeConfig& config) {
  return config.getblock_ltc_token_configured ? config.getblock_ltc_token : nullptr;
}
const char* ZcashGetBlockToken(const config::ProviderRuntimeConfig& config) {
  return config.getblock_zec_token_configured ? config.getblock_zec_token : nullptr;
}

// The coins served by the shared Blockbook driver, one row each: the
// address grammar and, where the deployment may carry one, the keyed
// GetBlock endpoint's token. A new Blockbook coin is one row here plus its
// ChainDriver/TransferSource lines at the end of this file.
struct BlockbookCoin {
  Family family;
  bool (*valid_address)(const char* address, size_t length);
  // nullptr: the coin has no GetBlock endpoint.
  const char* (*getblock_token)(const config::ProviderRuntimeConfig& config);
};
constexpr BlockbookCoin kBlockbookCoins[] = {
    {Family::kBitcoinCash, BitcoinCashAddress, nullptr},
    {Family::kDash, DashAddress, nullptr},
    {Family::kZcash, ZcashAddress, ZcashGetBlockToken},
    {Family::kDogecoin, DogecoinAddress, DogecoinGetBlockToken},
    {Family::kLitecoin, LitecoinAddress, LitecoinGetBlockToken},
};
const BlockbookCoin* BlockbookCoinOf(Family family) {
  for (const BlockbookCoin& coin : kBlockbookCoins)
    if (coin.family == family) return &coin;
  return nullptr;
}

const char* GetBlockToken(const config::ProviderRuntimeConfig& config, Family family) {
  const BlockbookCoin* coin = BlockbookCoinOf(family);
  return coin && coin->getblock_token ? coin->getblock_token(config) : nullptr;
}

// endpointsFor(network): the keyed GetBlock endpoint first, mirrors after.
bool BlockbookEndpoints(Context& ctx, const Network& network, const char* const** urls, size_t* count) {
  const char* token = GetBlockToken(*ctx.config, network.family);
  if (!token) {
    *urls = network.rpc_urls;
    *count = network.rpc_url_count;
    return true;
  }
  auto* list = static_cast<const char**>(ctx.arena->Alloc(sizeof(char*) * (network.rpc_url_count + 1)));
  const char* getblock = Text(*ctx.arena, "https://go.getblock.io/%s/api/v2", token);
  if (!list || !getblock) return false;
  // Cppcheck 2.13 misidentifies the Arena-allocated array of char pointers as integers.
  // cppcheck-suppress AssignmentAddressToInteger
  list[0] = getblock;
  // Cppcheck 2.13 misidentifies the Arena-allocated array of char pointers as integers.
  // cppcheck-suppress AssignmentAddressToInteger
  for (size_t i = 0; i < network.rpc_url_count; ++i) list[i + 1] = network.rpc_urls[i];
  *urls = list;
  *count = network.rpc_url_count + 1;
  return true;
}

bool BalanceValid(const Value* body) {
  const Value* balance = tj::Get(body, "balance");
  return tj::IsString(balance) && utxo_detail::AllDigits(balance->text, balance->length);
}

}  // namespace

bool BlockbookDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                       Value** result, Error* error) const {
  Arena& arena = *ctx.arena;
  const size_t length = strlen(address);
  // A family outside the table is read with the Litecoin grammar.
  const BlockbookCoin* coin = BlockbookCoinOf(network.family);
  const bool valid = coin ? coin->valid_address(address, length) : LitecoinAddress(address, length);
  if (!valid) {
    error->Range("not a %s address: %s", FamilyName(network.family), address);
    return false;
  }
  Call call;
  call.kind = CallKind::kGet;
  if (!BlockbookEndpoints(ctx, network, &call.urls, &call.url_count)) return ArenaFail(error);
  call.path = PathFor(arena, "/address/", address, "?details=basic");
  call.validate = BalanceValid;
  if (!call.path) return ArenaFail(error);
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  const char* balance = tj::CString(arena, tj::Get(call.value, "balance"));
  if (!balance) return ArenaFail(error);
  *result = NativeEntry(arena, balance, network, "", 8, "blockbook");
  return *result ? true : ArenaFail(error);
}

namespace {

// ---- transfer sources -------------------------------------------------------------

struct NetFlow {
  BigInt to_holder;
  BigInt from_holder;
  bool holder_funded = false;
};

struct Classified {
  const char* direction;
  const char* tx_type;
  BigInt amount;
};

// classify(flow, fee): net flow excluding the fee the holder paid.
bool Classify(const NetFlow& flow, const BigInt& fee, Classified* out) {
  BigInt net = flow.to_holder;
  if (!net.Sub(flow.from_holder)) return false;
  if (flow.holder_funded && !net.Add(fee)) return false;
  if (net.negative()) {
    out->direction = "outgoing";
    out->tx_type = "send";
    out->amount = net.Abs();
  } else if (!net.IsZero()) {
    out->direction = "incoming";
    out->tx_type = "receive";
    out->amount = net;
  } else {
    out->direction = "selfTransfer";
    out->tx_type = "send";
    out->amount = BigInt();
  }
  return true;
}

Value* NullIfAbsent(Arena& arena, const Value* v) {
  return v ? tj::Clone(arena, v) : tj::Null(arena);
}

// The shared tail of both record() builders.
bool FillRecord(Arena& arena, Value* record, const Network& network, const Value* txid, const char* tx_type,
                const char* status, const char* direction, double ts, const Value* block_number,
                const char* holder, const Value* counterparty, const BigInt& amount, bool fee_shown,
                const BigInt& fee, Value* metadata) {
  size_t txid_length = 0;
  const char* txid_text = JsStr(arena, txid, &txid_length);
  Value* native = NativeAssetJson(arena, network);
  const char* record_id = txid_text ? Text(arena, "%s:%s", network.id, txid_text) : nullptr;
  if (!record_id || !native) return false;
  const bool incoming = strcmp(direction, "incoming") == 0;
  Value* counter = counterparty ? tj::Clone(arena, counterparty) : tj::Null(arena);
  Value* holder_value = tj::Str(arena, holder);
  const uint32_t decimals = network.has_native ? network.native_decimals : 18;
  const char* symbol = network.has_native ? network.native_symbol : "";
  return tj::Set(arena, record, "recordId", tj::Str(arena, record_id)) &&
         tj::Set(arena, record, "txType", tj::Str(arena, tx_type)) &&
         tj::Set(arena, record, "status", tj::Str(arena, status)) &&
         tj::Set(arena, record, "direction", tj::Str(arena, direction)) &&
         tj::Set(arena, record, "ts", tj::Num(arena, ts)) &&
         tj::Set(arena, record, "hash", tj::Clone(arena, txid)) &&
         tj::Set(arena, record, "blockNumber", NullIfAbsent(arena, block_number)) &&
         tj::Set(arena, record, "fromAddress", incoming ? counter : holder_value) &&
         tj::Set(arena, record, "toAddress", incoming ? holder_value : counter) &&
         tj::Set(arena, record, "counterparty", counterparty ? tj::Clone(arena, counterparty) : tj::Null(arena)) &&
         tj::Set(arena, record, "tokenId", tj::Str(arena, "native")) &&
         tj::Set(arena, record, "asset", native) &&
         tj::Set(arena, record, "amountRaw", amount.ToValue(arena)) &&
         tj::Set(arena, record, "amountDecimals", tj::Num(arena, decimals)) &&
         tj::Set(arena, record, "feeRaw", fee_shown ? fee.ToValue(arena) : tj::Null(arena)) &&
         tj::Set(arena, record, "feeDecimals", fee_shown ? tj::Num(arena, decimals) : tj::Null(arena)) &&
         tj::Set(arena, record, "feeSymbol", fee_shown ? tj::Str(arena, symbol) : tj::Null(arena)) &&
         tj::Set(arena, record, "memo", tj::Null(arena)) &&
         tj::Set(arena, record, "method", tj::Null(arena)) &&
         tj::Set(arena, record, "error", tj::Null(arena)) &&
         tj::Set(arena, record, "metadata", metadata);
}

// `tx.status?.block_time ? new Date(block_time * 1000) : new Date()`.
double TimeOr(Context& ctx, const Value* seconds) {
  if (!Truthy(seconds)) return static_cast<double>(ctx.Now());
  return JsNumber(seconds) * 1000;
}

bool SupportsRpc(const config::ProviderRuntimeConfig&, const Network& network) { return network.rpc_url_count > 0; }

// ---- EsploraTransferSource ------------------------------------------------------

constexpr size_t kEsploraPage = 25;
constexpr int kEsploraMaxPages = 4;

bool EsploraRecord(Context& ctx, const Network& network, const char* holder, const Value* tx, Value** out,
                   Error* error) {
  Arena& arena = *ctx.arena;
  *out = nullptr;
  const Value* txid = tj::Get(tx, "txid");
  if (!Truthy(txid)) return true;
  NetFlow flow;
  Value* const* items = nullptr;
  size_t count = 0;
  if (!Iterable(tj::Get(tx, "vin"), &items, &count, error)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!Accessible(items[i], error)) return false;
    const Value* prevout = tj::Get(items[i], "prevout");
    const Value* address = tj::Get(prevout, "scriptpubkey_address");
    const Value* value = tj::Get(prevout, "value");
    if (tj::IsString(address) && strlen(holder) == address->length &&
        memcmp(address->text, holder, address->length) == 0 && tj::IsNumber(value)) {
      BigInt amount;
      if (!BigIntOf(value, &amount, error)) return false;
      if (!flow.from_holder.Add(amount)) { error->Upstream("amount overflow"); return false; }
      flow.holder_funded = true;
    }
  }
  const Value* counterparty = nullptr;  // JS null
  if (!Iterable(tj::Get(tx, "vout"), &items, &count, error)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!Accessible(items[i], error)) return false;
    const Value* value = tj::Get(items[i], "value");
    if (!tj::IsNumber(value)) continue;
    const Value* address = tj::Get(items[i], "scriptpubkey_address");
    if (tj::IsString(address) && strlen(holder) == address->length &&
        memcmp(address->text, holder, address->length) == 0) {
      BigInt amount;
      if (!BigIntOf(value, &amount, error)) return false;
      if (!flow.to_holder.Add(amount)) { error->Upstream("amount overflow"); return false; }
    } else if (!counterparty || counterparty->type == tj::Type::kNull) {
      counterparty = address ? address : nullptr;
    }
  }
  if (counterparty && counterparty->type == tj::Type::kNull) counterparty = nullptr;
  const Value* fee_value = tj::Get(tx, "fee");
  Value zero;
  zero.type = tj::Type::kNumber;
  BigInt fee;
  if (!BigIntOf(fee_value && fee_value->type != tj::Type::kNull ? fee_value : &zero, &fee, error)) return false;
  Classified classified{};
  if (!Classify(flow, fee, &classified)) { error->Upstream("amount overflow"); return false; }
  const Value* status = tj::Get(tx, "status");
  const Value* confirmed = tj::Get(status, "confirmed");
  Value* record = NewTransfer(arena);
  Value* metadata = tj::Obj(arena);
  if (!record || !metadata ||
      !FillRecord(arena, record, network, txid, classified.tx_type,
                  confirmed && confirmed->type == tj::Type::kTrue ? "confirmed" : "pending", classified.direction,
                  TimeOr(ctx, tj::Get(status, "block_time")), tj::Get(status, "block_height"), holder,
                  counterparty, classified.amount, flow.holder_funded, fee, metadata))
    return ArenaFail(error);
  *out = record;
  return true;
}

}  // namespace

bool EsploraSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const {
  Arena& arena = *ctx.arena;
  const Value* stored = tj::Get(state, "lastTxid");
  const Value* last_txid = tj::IsString(stored) ? stored : nullptr;
  Value* collected = tj::Arr(arena);
  if (!collected) return ArenaFail(error);
  const char* before = nullptr;
  bool saw_cursor = false, exhausted = false;
  for (int page = 0; page < kEsploraMaxPages && static_cast<double>(collected->count) < backfill_depth; ++page) {
    Call call;
    call.kind = CallKind::kGet;
    call.urls = network.rpc_urls;
    call.url_count = network.rpc_url_count;
    call.lane = "esplora";
    if (before) {
      const char* prefix = PathFor(arena, "/address/", address, "/txs/chain/");
      call.path = prefix ? Text(arena, "%s%s", prefix, before) : nullptr;
    } else {
      call.path = PathFor(arena, "/address/", address, "/txs");
    }
    if (!call.path) return ArenaFail(error);
    ctx.transport->Run(call, arena);
    if (!call.ok()) { call.ToError(error); return false; }
    const Value* txs = call.value;
    if (!tj::IsArray(txs) || txs->count == 0) {
      exhausted = true;
      break;
    }
    for (uint32_t i = 0; i < txs->count; ++i) {
      const Value* tx = txs->items[i];
      if (!Accessible(tx, error)) return false;
      const Value* txid = tj::Get(tx, "txid");
      if (last_txid && tj::IsString(txid) && txid->length == last_txid->length &&
          memcmp(txid->text, last_txid->text, txid->length) == 0) {
        saw_cursor = true;
        break;
      }
      if (!tj::Push(arena, collected, txs->items[i])) return ArenaFail(error);
    }
    if (saw_cursor || txs->count < kEsploraPage) {
      exhausted = exhausted || txs->count < kEsploraPage;
      break;
    }
    // before = txs.at(-1)?.txid ?? null
    const Value* tail = tj::Get(txs->items[txs->count - 1], "txid");
    if (!tail || tail->type == tj::Type::kNull) break;
    before = JsStr(arena, tail, nullptr);
    if (!before) return ArenaFail(error);
  }
  Value* records = tj::Arr(arena);
  if (!records) return ArenaFail(error);
  for (uint32_t i = 0; i < collected->count; ++i) {
    Value* record = nullptr;
    if (!EsploraRecord(ctx, network, address, collected->items[i], &record, error)) return false;
    if (record && !tj::Push(arena, records, record)) return ArenaFail(error);
  }
  // state: { lastTxid: collected[0]?.txid ?? lastTxid }
  const Value* head = collected->count ? tj::Get(collected->items[0], "txid") : nullptr;
  const Value* next = head && head->type != tj::Type::kNull ? head : last_txid;
  Value* next_state = tj::Obj(arena);
  if (!next_state || !tj::Set(arena, next_state, "lastTxid", next ? tj::Clone(arena, next) : tj::Null(arena)))
    return ArenaFail(error);
  result->records = records;
  result->state = next_state;
  result->backfill_complete = saw_cursor || exhausted;
  return true;
}

namespace {

// ---- BlockbookTransferSource ------------------------------------------------------

// `holder in entry.addresses` as `entry.addresses?.includes(holder)`: an array
// compares elements, a string tests a substring, null/undefined is false and
// any other value has no includes() (TypeError).
bool AddressesInclude(const Value* addresses, const char* holder, bool* includes, Error* error) {
  *includes = false;
  if (!addresses || addresses->type == tj::Type::kNull) return true;
  const size_t holder_length = strlen(holder);
  if (addresses->type == tj::Type::kArray) {
    for (uint32_t i = 0; i < addresses->count; ++i) {
      const Value* item = addresses->items[i];
      if (tj::IsString(item) && item->length == holder_length && memcmp(item->text, holder, holder_length) == 0) {
        *includes = true;
        return true;
      }
    }
    return true;
  }
  if (addresses->type == tj::Type::kString) {
    if (!holder_length) { *includes = true; return true; }
    for (size_t i = 0; i + holder_length <= addresses->length; ++i)
      if (memcmp(addresses->text + i, holder, holder_length) == 0) { *includes = true; return true; }
    return true;
  }
  error->Upstream("addresses.includes is not a function");
  return false;
}

// firstBlockbookAddress(entry).
const Value* FirstBlockbookAddress(Arena& arena, const Value* entry) {
  const Value* is_address = tj::Get(entry, "isAddress");
  if (is_address && is_address->type == tj::Type::kFalse) return nullptr;
  const Value* addresses = tj::Get(entry, "addresses");
  const Value* first = nullptr;
  if (tj::IsArray(addresses)) {
    first = addresses->count ? addresses->items[0] : nullptr;
  } else if (tj::IsString(addresses) && addresses->length) {
    // String.prototype[0]: the first UTF-16 unit (one UTF-8 character here).
    size_t width = 1;
    const unsigned char lead = static_cast<unsigned char>(addresses->text[0]);
    if (lead >= 0xf0) width = 4;
    else if (lead >= 0xe0) width = 3;
    else if (lead >= 0xc0) width = 2;
    if (width > addresses->length) width = addresses->length;
    first = tj::Str(arena, addresses->text, width);
  } else if (tj::IsObject(addresses)) {
    first = tj::Get(addresses, "0");
  }
  return tj::IsString(first) && first->length > 0 ? first : nullptr;
}

bool BlockbookRecord(Context& ctx, const Network& network, const char* holder, const Value* tx, Value** out,
                     Error* error) {
  Arena& arena = *ctx.arena;
  *out = nullptr;
  if (!Accessible(tx, error)) return false;
  const Value* txid = tj::Get(tx, "txid");
  if (!Truthy(txid)) return true;
  NetFlow flow;
  const Value* sender = nullptr;
  const Value* recipient = nullptr;
  Value* const* items = nullptr;
  size_t count = 0;
  if (!Iterable(tj::Get(tx, "vin"), &items, &count, error)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!Accessible(items[i], error)) return false;
    bool includes = false;
    if (!AddressesInclude(tj::Get(items[i], "addresses"), holder, &includes, error)) return false;
    const Value* value = tj::Get(items[i], "value");
    if (includes && DigitsValue(value)) {
      BigInt amount;
      if (!BigIntOf(value, &amount, error)) return false;
      if (!flow.from_holder.Add(amount)) { error->Upstream("amount overflow"); return false; }
      flow.holder_funded = true;
    } else if (!sender) {
      sender = FirstBlockbookAddress(arena, items[i]);
    }
  }
  if (!Iterable(tj::Get(tx, "vout"), &items, &count, error)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!Accessible(items[i], error)) return false;
    const Value* value = tj::Get(items[i], "value");
    if (!DigitsValue(value)) continue;
    bool includes = false;
    if (!AddressesInclude(tj::Get(items[i], "addresses"), holder, &includes, error)) return false;
    if (includes) {
      BigInt amount;
      if (!BigIntOf(value, &amount, error)) return false;
      if (!flow.to_holder.Add(amount)) { error->Upstream("amount overflow"); return false; }
    } else if (!recipient) {
      recipient = FirstBlockbookAddress(arena, items[i]);
    }
  }
  const Value* fees = tj::Get(tx, "fees");
  BigInt fee;
  if (DigitsValue(fees) && !BigIntOf(fees, &fee, error)) return false;
  const int shielded = network.family == Family::kZcash
                           ? utxo_detail::ZcashHasShieldedComponents(arena, tj::Get(tx, "hex"))
                           : 0;
  // A missing/unparseable raw Zcash transaction is untrusted too.
  const bool fee_reliable = network.family != Family::kZcash || shielded == 0;
  Classified classified{};
  if (!Classify(flow, fee_reliable ? fee : BigInt(), &classified)) {
    error->Upstream("amount overflow");
    return false;
  }
  const Value* counterparty = flow.holder_funded ? recipient : sender;
  const Value* confirmations = tj::Get(tx, "confirmations");
  const double depth = confirmations && confirmations->type != tj::Type::kNull ? JsNumber(confirmations) : 0;
  Value* metadata = tj::Obj(arena);
  if (!metadata) return ArenaFail(error);
  if (!fee_reliable && (!tj::Set(arena, metadata, "transparentPoolMovement", tj::Bool(arena, true)) ||
                        !tj::Set(arena, metadata, "feeUnavailable", tj::Bool(arena, true))))
    return ArenaFail(error);
  Value* record = NewTransfer(arena);
  if (!record ||
      !FillRecord(arena, record, network, txid, classified.tx_type, depth > 0 ? "confirmed" : "pending",
                  classified.direction, TimeOr(ctx, tj::Get(tx, "blockTime")), tj::Get(tx, "blockHeight"), holder,
                  counterparty, classified.amount, flow.holder_funded && fee_reliable, fee, metadata))
    return ArenaFail(error);
  *out = record;
  return true;
}

bool TransactionsValid(const Value* body) { return tj::IsArray(tj::Get(body, "transactions")); }

}  // namespace

bool BlockbookSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                    double backfill_depth, SourceResult* result, Error* error) const {
  Arena& arena = *ctx.arena;
  const double limit = backfill_depth < 50 ? backfill_depth : 50;
  const bool incremental = Equals(tj::Get(state, "mode"), "incremental");
  const Value* from_value = tj::Get(state, "fromHeight");
  const double from_height = tj::IsNumber(from_value) && from_value->number >= 0 ? from_value->number : 0;
  const Value* page_value = tj::Get(state, "page");
  const double page = !incremental && tj::IsNumber(page_value) && floor(page_value->number) == page_value->number &&
                              page_value->number > 0
                          ? page_value->number
                          : 1;
  char limit_text[40], from_text[40], page_text[40];
  JsNumberText(limit, limit_text, sizeof(limit_text));
  JsNumberText(from_height, from_text, sizeof(from_text));
  JsNumberText(page, page_text, sizeof(page_text));
  const char* encoded = EncodeUri(arena, address);
  if (!encoded) return ArenaFail(error);
  const char* path = incremental
                         ? (from_height > 0
                                ? Text(arena, "/address/%s?details=txs&pageSize=%s&from=%s", encoded, limit_text,
                                       from_text)
                                : Text(arena, "/address/%s?details=txs&pageSize=%s", encoded, limit_text))
                         : Text(arena, "/address/%s?details=txs&pageSize=%s&page=%s", encoded, limit_text,
                                page_text);
  Call call;
  call.kind = CallKind::kGet;
  if (!path || !BlockbookEndpoints(ctx, network, &call.urls, &call.url_count)) return ArenaFail(error);
  call.path = path;
  call.validate = TransactionsValid;
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  const Value* txs = tj::Get(call.value, "transactions");
  const uint32_t count = tj::IsArray(txs) ? txs->count : 0;
  Value* records = tj::Arr(arena);
  if (!records) return ArenaFail(error);
  for (uint32_t i = 0; i < count; ++i) {
    Value* record = nullptr;
    if (!BlockbookRecord(ctx, network, address, txs->items[i], &record, error)) return false;
    if (record && !tj::Push(arena, records, record)) return ArenaFail(error);
  }
  double max_height = from_height;
  for (uint32_t i = 0; i < count; ++i) {
    const Value* height = tj::Get(txs->items[i], "blockHeight");
    if (tj::IsNumber(height) && height->number >= max_height) max_height = height->number + 1;
  }
  Value* next = tj::Obj(arena);
  if (!next) return ArenaFail(error);
  bool complete = true;
  if (!incremental) {
    const bool reached_history_end = count < limit;
    const bool reached_requested_depth = page * limit >= backfill_depth;
    complete = reached_history_end || reached_requested_depth;
  }
  const bool ok = complete
                      ? tj::Set(arena, next, "mode", tj::Str(arena, "incremental")) &&
                            tj::Set(arena, next, "fromHeight", tj::Num(arena, max_height))
                      : tj::Set(arena, next, "mode", tj::Str(arena, "backfill")) &&
                            tj::Set(arena, next, "page", tj::Num(arena, page + 1)) &&
                            tj::Set(arena, next, "fromHeight", tj::Num(arena, max_height));
  if (!ok) return ArenaFail(error);
  result->records = records;
  result->state = next;
  result->backfill_complete = complete;
  return true;
}

const EsploraDriver kEsploraDriver{};
const BlockbookDriver kBitcoinCashDriver{Family::kBitcoinCash};
const BlockbookDriver kDashDriver{Family::kDash};
const BlockbookDriver kDogecoinDriver{Family::kDogecoin};
const BlockbookDriver kLitecoinDriver{Family::kLitecoin};
const BlockbookDriver kZcashDriver{Family::kZcash};

const EsploraSource kEsploraSource{};
const BlockbookSource kBitcoinCashSource{Family::kBitcoinCash};
const BlockbookSource kDashSource{Family::kDash};
const BlockbookSource kDogecoinSource{Family::kDogecoin};
const BlockbookSource kLitecoinSource{Family::kLitecoin};
const BlockbookSource kZcashSource{Family::kZcash};


bool EsploraDriver::Registered(const config::ProviderRuntimeConfig& config) const { return Always(config); }
bool BlockbookDriver::Registered(const config::ProviderRuntimeConfig& config) const { return Always(config); }
bool EsploraSource::Registered(const config::ProviderRuntimeConfig& config) const { return Always(config); }
bool EsploraSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return SupportsRpc(config, network); }
bool BlockbookSource::Registered(const config::ProviderRuntimeConfig& config) const { return Always(config); }
bool BlockbookSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return SupportsRpc(config, network); }

}  // namespace fortis::chains
