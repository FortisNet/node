#pragma once

#include <optional>

#include "clock.h"
#include "provider_runtime_config.h"
#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// Shared vocabulary of the chain drivers, transfer sources and transaction
// views, ported from the earlier TypeScript implementation: the network
// catalog, JavaScript value coercions, exact integer arithmetic for chain
// amounts (BigInt in TypeScript), amount strings, encodings, sanitizers, the
// EVM ABI helpers and the transfer record helpers.
//
// Every document is a token_json DOM in a caller-owned arena: the
// TypeScript code builds and spreads plain objects, and the port builds the
// same objects in the same key order so JSON and protobuf Struct output
// match byte for byte. Host and guest use the same C++17 contracts.
namespace fortis::chains {

using codec::tj::Arena;
using codec::tj::OutBuffer;
using codec::tj::Value;

// ---- families and networks ---------------------------------------------------

enum class Family : uint8_t {
  kEvm, kBitcoin, kBitcoinCash, kDash, kDogecoin, kLitecoin, kZcash, kSolana, kCardano, kXrpl,
  kStellar, kHedera, kAlgorand, kSui, kNear, kTon, kAptos, kIcp, kBittensor, kVechain, kTron,
  kCosmos, kCount
};
// The TypeScript ChainFamily spelling ("bitcoinCash", "evm", ...).
const char* FamilyName(Family family);
bool ParseFamily(const char* name, Family* out);

constexpr size_t kMaxNetworkUrls = 5;
constexpr size_t kMaxNetworkIdBytes = 72;

// NetworkCapabilities as this native deployment serves them (the registry's
// capability truth: a facet is "ok" only when its driver/source exists and the
// network carries the configuration it needs).
struct Capabilities {
  bool balances = false;
  bool transfers = false;
  bool nfts = false;
  bool insight = false;
  bool tx_status = false;
  // A tx detail view exists for the family (GET /v1/networks txDetail).
  bool tx_detail = false;
};

// NetworkInfo. `rpc_urls` are the DEPLOYMENT endpoints (registry options
// override: Robinhood through Alchemy when keyed, DASH_BLOCKBOOK_URLS),
// never echoed to clients. Copyable: every string is either a static
// catalogue literal, catalog-owned storage, or inline.
struct Network {
  char id[kMaxNetworkIdBytes + 1]{};
  Family family = Family::kEvm;
  bool has_evm_chain_id = false;
  double evm_chain_id = 0;  // Number(digits) for a generic eip155 id
  bool testnet = false;
  bool has_required_confirmations = false;
  uint32_t required_confirmations = 0;
  const char* rpc_urls[kMaxNetworkUrls]{};
  size_t rpc_url_count = 0;
  const char* graphql_urls[kMaxNetworkUrls]{};
  size_t graphql_url_count = 0;
  bool has_native = false;
  const char* native_symbol = nullptr;
  uint32_t native_decimals = 0;
  const char* native_coingecko_id = nullptr;  // null: unpriced (testnets)
  Capabilities caps{};
  bool generic = false;  // an uncatalogued eip155:N network (no RPC, NONE caps)
};

// The registry for one process: catalogued seeds (network_registry.cpp,
// generated), deployment RPC overrides and capability truth computed from
// the native driver/source/view tables and configured keys.
class Catalog {
 public:
  bool Init(const config::ProviderRuntimeConfig& config);
  // registry.network(id): exact id, then eip155:N matched against catalogued
  // EVM chain ids by Number(N), then a generic EVM network. False = null.
  bool Resolve(const char* id, size_t length, Network* out) const;
  size_t count() const { return count_; }
  const Network& at(size_t index) const { return networks_[index]; }

 private:
  static constexpr size_t kMax = 64;
  Network networks_[kMax]{};
  size_t count_ = 0;
  char robinhood_url_[400]{};
  char dash_urls_[kMaxNetworkUrls][512]{};
};

// ---- operation context and errors ------------------------------------------

class ChainTransport;

// The TypeScript exception classes a driver/source/view can surface:
//  - kRange: RangeError (the facet's 400 with an authored message);
//  - kUpstream: any other failure (UpstreamError, UpstreamAnswerError,
//    malformed payloads) -> 502, never upstream text on the wire;
//  - kLaneBusy: LaneBusyError -> the typed 429 with retry-after.
enum class ErrorKind : uint8_t { kNone, kRange, kUpstream, kLaneBusy };
struct Error {
  ErrorKind kind = ErrorKind::kNone;
  uint32_t retry_after_ms = 0;
  // RangeError: the client-visible detail. Otherwise a log-only description.
  char message[512]{};
  bool ok() const { return kind == ErrorKind::kNone; }
  void Range(const char* format, ...) __attribute__((format(printf, 2, 3)));
  void Upstream(const char* format, ...) __attribute__((format(printf, 2, 3)));
  void LaneBusy(uint32_t retry_after);
  void Clear() { kind = ErrorKind::kNone; retry_after_ms = 0; message[0] = '\0'; }
};

// What a driver, source or view runs with. One per request/job; never shared
// between threads.
struct Context {
  ChainTransport* transport = nullptr;
  Arena* arena = nullptr;  // results and parsed provider bodies
  const config::ProviderRuntimeConfig* config = nullptr;
  // Date.now() for this operation (NowMs; null is the wall clock).
  base::Clock* clock = nullptr;
  // The TypeScript resolve(selector): the cached 4byte name of
  // a 0x-prefixed 4-byte selector, or null (unknown/outage). EVM view only.
  const char* (*resolve_selector)(void* context, const char* selector, Arena& arena) = nullptr;
  void* selector_context = nullptr;
  int64_t Now() const;
};

// ---- JavaScript value helpers -----------------------------------------------

// object[key] (null when absent or `object` is not an object).
const Value* Member(const Value* object, const char* key);
// typeof v === 'string' (optionally non-empty) -> NUL-terminated arena copy.
const char* StringOrNull(Arena& arena, const Value* v, bool reject_empty = false);
// Number(value) for a JSON value (undefined -> NaN, null -> 0, booleans 0/1,
// strings ECMAScript StringToNumber, arrays/objects as JavaScript does for
// the shapes providers send: [] -> 0, [x] -> Number(x), others NaN).
double JsNumber(const Value* v);
// The TypeScript intOrNull / numOrNull (booleans are null).
std::optional<double> IntOrNull(const Value* v);
std::optional<double> NumOrNull(const Value* v);
// The TypeScript hexToDecimalString / hexToInt / amountString /
// amountStringOrZero.
Value* HexToDecimalString(Arena& arena, const Value* v);  // JSON null when unusable
std::optional<double> HexToInt(const Value* v);
Value* AmountString(Arena& arena, const Value* v);  // JSON null when unusable
// The TypeScript integerString (strings trimmed /^-?\d+$/, integer doubles
// expanded exactly). JSON null when unusable.
Value* IntegerString(Arena& arena, const Value* v);
// The TypeScript quoteLongIntegers: quotes integer literals of 16+ digits in
// value position so JSON.parse keeps their digits. Output into `out`.
bool QuoteLongIntegers(const uint8_t* text, size_t length, OutBuffer& out);
// `${value}` / String(value) for a finite double (Number.prototype.toString).
void JsNumberText(double value, char* output, size_t capacity);
// Number(BigInt(v)) for a JSON value: a string by StringToBigInt (trimmed;
// "" is 0n; a 0x/0o/0b literal or a signed decimal integer) without a bit
// limit (the nearest double, Infinity past 2^1024), an integral number as
// itself, a boolean as 0/1, an array as its join(','); never -0. A
// non-integral number (JSON 1e400 parses to Infinity) is BigInt's
// RangeError, `error` kRange with V8's message; null, an object or a
// malformed string (TypeError/SyntaxError) are kUpstream.
bool BigIntNumber(Arena& arena, const Value* v, double* out, Error* error);
// Number(BigInt(String(v))) for a JSON value (EvmDriver.txStatus's block
// number): String() first (an array joined with ',', null elements empty; an
// object "[object Object]"; a number by Number::toString, so 1e21 is
// "1e+21"; true "true"), then StringToBigInt as above. A string that is not
// an integer literal is V8's SyntaxError, `error` kUpstream with its message
// "Cannot convert {text} to a BigInt" (no other failure exists).
bool StringBigIntNumber(Arena& arena, const Value* v, double* out, Error* error);
// An arena string from printf.
Value* Format(Arena& arena, const char* format, ...) __attribute__((format(printf, 2, 3)));
// Exact string equality / prefix tests on a string value.
bool Equals(const Value* v, const char* text);
bool StartsWith(const char* text, const char* prefix);

// Date.prototype.toISOString of epoch ms (false out of range).
bool IsoTime(int64_t epoch_ms, char output[32]);
// Date.parse / new Date(text).getTime() (false for NaN).
bool ParseTime(const char* text, size_t length, double* epoch_ms);
// An ISO string Value, or JSON null when out of range.
Value* IsoValue(Arena& arena, int64_t epoch_ms);

// encodeURIComponent (UTF-8, lone surrogates throw in JS: false here).
bool EncodeUriComponent(const char* text, size_t length, OutBuffer& out);
// encodeURIComponent into an arena string (null on failure).
const char* EncodeUri(Arena& arena, const char* text);
// new URL(url).host (lowercase host, non-default port kept). False if invalid.
bool UrlHost(const char* url, char* output, size_t capacity);

// ---- exact integers (JavaScript BigInt for chain amounts) -------------------

// Signed integer with a 640-bit magnitude: large enough for every chain
// amount the drivers handle (uint256 words, 10^24 NEAR yocto sums) and for
// intermediate products. Operations report overflow instead of wrapping.
class BigInt {
 public:
  static constexpr size_t kLimbs = 20;
  BigInt() = default;
  static BigInt FromU64(uint64_t value);
  static BigInt FromI64(int64_t value);
  // BigInt(text): optional sign, decimal digits, or 0x/0X hex. Surrounding
  // ASCII whitespace is accepted as JavaScript does. False on syntax/overflow.
  static bool Parse(const char* text, size_t length, BigInt* out);
  static bool ParseDecimal(const char* text, size_t length, BigInt* out);
  // Hex digits without a prefix (an optional 0x is skipped). Empty -> 0.
  static bool ParseHex(const char* text, size_t length, BigInt* out);
  // BigInt(number) for an integral finite double.
  static bool FromDouble(double value, BigInt* out);
  bool negative() const { return negative_ && !IsZero(); }
  bool IsZero() const;
  int Compare(const BigInt& other) const;
  bool Add(const BigInt& other);
  bool Sub(const BigInt& other);
  bool Mul(const BigInt& other);
  bool MulSmall(uint32_t factor);
  bool AddSmall(uint32_t value);
  // Truncating division (BigInt `/`); remainder has the dividend's sign.
  bool DivSmall(uint32_t divisor, uint32_t* remainder);
  bool Div(const BigInt& divisor, BigInt* remainder);
  void Negate() { negative_ = !negative_; }
  BigInt Abs() const { BigInt c = *this; c.negative_ = false; return c; }
  // Number(bigint): nearest double.
  double ToDouble() const;
  bool FitsU64() const;
  uint64_t LowU64() const;
  // .toString() / .toString(16) (lowercase, no prefix, "-" for negatives).
  bool ToDecimal(char* output, size_t capacity) const;
  bool ToHex(char* output, size_t capacity) const;
  Value* ToValue(Arena& arena) const;  // decimal string value
  size_t BitLength() const;

 private:
  uint32_t limbs_[kLimbs]{};
  bool negative_ = false;
};

// ---- amounts ------------------------------------------------------------------

// decimalToRaw(value, fixedDecimals?): -1 for "no fixed decimals". False
// where TypeScript returns null.
bool DecimalToRaw(const char* input, size_t input_length, int fixed_decimals, BigInt* raw, int* decimals);
// rawToDecimalString(raw, decimals); JSON null where TypeScript returns null.
Value* RawToDecimalString(Arena& arena, const BigInt& raw, int decimals);

// ---- encodings ----------------------------------------------------------------

bool HexDecode(const char* hex, size_t length, uint8_t* output, size_t capacity, size_t* written);
void HexEncode(const uint8_t* bytes, size_t length, char* output);  // 2*length + NUL
bool IsHex(const char* text, size_t length);  // /^[0-9a-fA-F]*$/
// Bitcoin base58 (leading '1' = zero byte). False on an invalid character or
// overflow of `capacity`.
bool Base58Decode(const char* text, size_t length, uint8_t* output, size_t capacity, size_t* written);
bool Base58Encode(const uint8_t* bytes, size_t length, char* output, size_t capacity);
// Base58Check: payload + 4-byte double-SHA256 checksum.
bool Base58CheckDecode(const char* text, size_t length, uint8_t* payload, size_t capacity, size_t* written);
// Buffer.from(text, 'base64') semantics (standard and URL alphabets accepted,
// whitespace and invalid characters skipped, padding optional).
bool Base64Decode(const char* text, size_t length, uint8_t* output, size_t capacity, size_t* written);
bool Base64Encode(const uint8_t* bytes, size_t length, bool url, bool pad, char* output, size_t capacity);
void Sha256(const uint8_t* bytes, size_t length, uint8_t digest[32]);
void Sha512(const uint8_t* bytes, size_t length, uint8_t digest[64]);
void Sha1(const uint8_t* bytes, size_t length, uint8_t digest[20]);
void Keccak256(const uint8_t* bytes, size_t length, uint8_t digest[32]);

// ---- sanitizers --------------------------------------------------------------

Value* SanitizeSymbol(Arena& arena, const char* text, size_t length);
Value* SanitizeName(Arena& arena, const char* text, size_t length);
Value* SanitizeText(Arena& arena, const char* text, size_t length);
// printableMemoFromBase64 / printableAsciiFromHex: JSON null when refused.
Value* PrintableMemoFromBase64(Arena& arena, const char* base64, size_t length);
Value* PrintableAsciiFromHex(Arena& arena, const char* hex, size_t length);

// ---- EVM ABI ------------------------------------------------------------------

constexpr const char* kMulticall3Address = "0xcA11bde05977b3631167028862bE2a173976CA11";
constexpr const char* kTransferEventTopic =
    "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";
// Each returns a 0x-prefixed arena string.
const char* AbiBalanceOf(Arena& arena, const char* holder);
const char* AbiGetEthBalance(Arena& arena, const char* holder);
const char* AbiAddressTopic(Arena& arena, const char* address);
const char* AbiDecimals(Arena& arena);
const char* AbiSymbol(Arena& arena);
const char* AbiName(Arena& arena);
const char* AbiTotalSupply(Arena& arena);
struct AbiCall3 {
  const char* target;
  const char* call_data;  // with or without 0x
};
// encodeAggregate3(calls): allowFailure=true on every sub-call.
const char* AbiAggregate3(Arena& arena, const AbiCall3* calls, size_t count);
struct AbiResult {
  bool success = false;
  const char* data = nullptr;  // hex, no 0x (points into the decoded input)
  size_t length = 0;           // hex characters
};
// decodeAggregate3: false where TypeScript returns null. `results` is an
// arena array of *count entries.
bool AbiDecodeAggregate3(Arena& arena, const char* input, size_t input_length, AbiResult** results, size_t* count);
// decodeUint256 (0 when shorter than a word). A non-hex word is 0 here;
// AbiUint256Checked is false where BigInt() would throw.
BigInt AbiUint256(const char* hex, size_t length);
bool AbiUint256Checked(const char* hex, size_t length, BigInt* out);
// decodeString: JSON null when undecodable; UTF-8 decoding as Buffer
// toString('utf8') (invalid sequences become U+FFFD).
Value* AbiString(Arena& arena, const char* input, size_t input_length);

// ---- transfers ------------------------------------------------------------------

// transferRecordId(chainId, hash, unit?): `unit` may be null. Null value when
// TypeScript throws (empty chain/hash after trim).
Value* TransferRecordId(Arena& arena, const char* chain, const char* hash, const char* unit);
// nativeAssetJson(network): {chainId, symbol, name, decimals, contractAddress:null}.
Value* NativeAssetJson(Arena& arena, const Network& network);
// directionFromFlow / typeFromDirection.
const char* DirectionFromFlow(bool incoming, bool outgoing);
const char* TypeFromDirection(const char* direction);

// A NormalizedTransfer as an object with every required TypeScript field in
// the order sources usually write it, all null except `metadata` ({}). Sources
// fill it with codec::tj::Set (replacing keeps position). `ts` is epoch ms (number).
Value* NewTransfer(Arena& arena);

// The TypeScript walkDescendingByHeight. `fetch_page` fills
// rows (an array value), `more` and an optional next cursor; returning false
// propagates the source failure in `error`.
struct HeightWalkPage {
  Value* rows = nullptr;
  bool more = false;
  const char* cursor = nullptr;  // null when absent
};
struct HeightWalkResult {
  double high_water = 0;
  bool complete = false;
  double rows_seen = 0;
};
bool WalkDescendingByHeight(double since_height, double budget, double max_pages,
                            bool (*fetch_page)(void* context, double index, const char* cursor,
                                               HeightWalkPage* page, Error* error),
                            bool (*height_of)(void* context, const Value* row, double* height),
                            void (*on_row)(void* context, Value* row), void* context,
                            HeightWalkResult* result, Error* error);

}  // namespace fortis::chains
