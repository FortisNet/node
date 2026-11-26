#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// Shared vocabulary of the cross-chain port (the TypeScript cross-chain domain
// rules and shared adapter helpers): JavaScript value coercions over the
// token_json DOM (provider bodies parsed as JSON.parse would), the shared
// route/status helpers, the trusted provider and bridge directories, the
// address validators the adapters and the API boundary use, and the
// request encodings (URLSearchParams, encodeURIComponent).
//
// Strings are WTF-8 (token_json.h): a lone UTF-16 surrogate from a JSON
// escape keeps its generalized form and may contain NUL, so provider text
// travels as string Values (pointer + length), never as C strings. Every
// length is counted in UTF-16 code units, as String.prototype does.
// Guest-safe: no C++ standard-library headers.
namespace fortis::exchange::cross_chain {

using codec::tj::Arena;
using codec::tj::OutBuffer;
using codec::tj::Value;

// ---- JavaScript values -------------------------------------------------------

// `value ?? fallback`: absent (nullptr, `undefined`) or JSON null.
inline bool Nullish(const Value* v) { return !v || v->type == codec::tj::Type::kNull; }
inline const Value* Coalesce(const Value* a, const Value* b) { return Nullish(a) ? b : a; }
// asRecord(): a non-array object, else nullptr.
const Value* AsRecord(const Value* v);
// record?.[key] (nullptr when `record` is not an object or the key is absent).
const Value* Field(const Value* record, const char* key);
// `value === text` for a string value.
bool Is(const Value* v, const char* text);
bool SameText(const Value* a, const Value* b);  // two string values, ===
// stringValue(value, maxLength): the string itself when it is a string,
// not blank after trim() and at most maxLength UTF-16 units.
const Value* StringValue(const Value* v, size_t max_units = 512);
// String(value) for any JSON value (Number.prototype.toString, Array join,
// "[object Object]"); `undefined` reads "undefined".
Value* JsString(Arena& arena, const Value* v);
// Number(value) (ECMAScript ToNumber); `undefined` is NaN.
double JsNumber(const Value* v);
// finiteNumber(): `typeof value === 'number' ? value : Number(value)`.
bool FiniteNumber(const Value* v, double* out);
// integerNumber(): a finite integral number.
bool IntegerNumber(const Value* v, double* out);
// chainValue(): String(value) of a string or number that is not blank.
const Value* ChainValue(Arena& arena, const Value* v);
// sameIdentifier(reported, expected): an absent echo agrees.
bool SameIdentifier(Arena& arena, const Value* reported, const Value* expected);
// safePositiveInteger / safeNonNegativeInteger over String(value) of a
// string or number: /^[1-9]\d{0,79}$/ and /^\d{1,80}$/. Arena C strings.
const char* SafePositiveInteger(Arena& arena, const Value* v);
const char* SafeNonNegativeInteger(Arena& arena, const Value* v);
bool PositiveIntegerText(const char* text, size_t length);   // /^[1-9]\d{0,79}$/
bool NonNegativeIntegerText(const char* text, size_t length);  // /^\d{1,80}$/
// safeEta(...values): the first integerNumber >= 0.
bool SafeEta(const Value* const* values, size_t count, double* out);

// String.prototype.length / trim() === '' / toLowerCase / toUpperCase.
size_t Units(const Value* v);
size_t Units(const char* text, size_t length);
bool Blank(const char* text, size_t length);
Value* Lower(Arena& arena, const Value* v);
Value* Upper(Arena& arena, const Value* v);
// `.startsWith(prefix)` of a string value (ASCII prefix).
bool StartsWith(const Value* v, const char* prefix);
bool Contains(const Value* v, const char* needle);
// The text with each lone surrogate as U+FFFD (Node's UTF-8 encoder).
const char* WellFormed(Arena& arena, const char* text, size_t length, size_t* out_length);
// A NUL-terminated copy of a string value (nullptr when not a string).
const char* CText(Arena& arena, const Value* v);

// ---- time ----------------------------------------------------------------------

// new Date(ms).toISOString() for any valid time value (|ms| <= 8.64e15,
// truncated toward zero as TimeClip does); false where it throws.
bool IsoTime(double ms, char output[40]);
const char* IsoText(Arena& arena, double ms);
// Date.parse(text); false for NaN.
bool DateParse(const char* text, size_t length, double* ms);
// safeExpiry(), safeTimestamp(): arena ISO strings or nullptr.
const char* SafeExpiry(Arena& arena, const Value* v);
const char* SafeTimestamp(Arena& arena, const Value* v);
// secondsBetween(): false for null.
bool SecondsBetween(const char* started, const char* completed, double* seconds);
// quoteValidity(providerExpiry, now, localValidityMs).
struct Validity {
  const char* quoted_at = nullptr;
  const char* expires_at = nullptr;
};
bool QuoteValidity(Arena& arena, const Value* provider_expiry, double now_ms, double local_validity_ms,
                   Validity* out);

// ---- route helpers ---------------------------------------------------------------

// normalizedRiskKey(): trim, toLowerCase, /[^a-z0-9]+/g -> '-', strip one
// leading and one trailing '-', '' -> 'unknown'.
const char* NormalizedRiskKey(Arena& arena, const Value* value);
// bridgeWebsiteUrl(bridge): the trusted directory; nullptr for an unknown
// key. (crossChainProviderWebsiteUrl(id) is ExchangeProvider::Website().)
const char* BridgeWebsiteUrl(Arena& arena, const Value* bridge);
// routeId(providerId, quoteId, route): `${providerId}:${quoteId}` or the
// first 24 hex digits of sha256(providerId|bridge|amountOutMin|to|data).
// `quote_id`, `execution_to` and `execution_data` may be nullptr.
Value* RouteId(Arena& arena, const char* provider_id, const Value* quote_id, const Value* bridge,
               const char* amount_out_min, const Value* execution_to, const Value* execution_data);
// executionKind(data, target).
const char* ExecutionKind(const Value* data, const Value* target);
// sumUsdCosts(estimate) / sumRawNetworkCosts(estimate).
bool SumUsdCosts(const Value* estimate, double* out);
const char* SumRawNetworkCosts(Arena& arena, const Value* estimate);
// parseLifiSteps / parseSquidSteps: an array of step objects.
Value* ParseLifiSteps(Arena& arena, const Value* value);
Value* ParseSquidSteps(Arena& arena, const Value* value);
// boundedProviderSnapshot(value): the bounded, redacted copy (an object;
// {} for anything else). `Undefined()` stands for a JavaScript `undefined`
// member in objects the adapters build before bounding them.
Value* BoundedProviderSnapshot(Arena& arena, const Value* value);
const Value* Undefined();
// A string Value, or JSON null for nullptr.
Value* StrOrNull(Arena& arena, const char* text);
Value* OrNull(Arena& arena, const Value* v);

// ---- addresses ---------------------------------------------------------------------

bool ValidEvmAddress(const Value* v);         // /^0x[0-9a-fA-F]{40}$/
bool ValidAptosAddress(const Value* v);       // /^0x[0-9a-fA-F]{64}$/
bool ValidNearAccount(const Value* v);
bool ValidBitcoinAddress(const Value* v);     // native SegWit bc1 only
bool ValidBitcoinCashAddress(const Value* v); // mainnet CashAddr
bool ValidDashAddress(const Value* v);
bool ValidZcashTransparentAddress(const Value* v);
bool ValidSolanaAddress(const Value* v);
bool ValidTonFriendlyAddress(const Value* v);
bool ValidMoneroAddress(const Value* v);

// ---- request encodings ---------------------------------------------------------------

// URLSearchParams.toString() of name/value pairs (USVString: a lone
// surrogate is encoded as U+FFFD).
struct QueryPair {
  const char* name;
  const Value* value;
};
bool FormEncode(const QueryPair* pairs, size_t count, OutBuffer& out);
// encodeURIComponent; false where JavaScript throws (a lone surrogate).
bool EncodeComponent(const Value* v, OutBuffer& out);
// Number.prototype.toString.
void NumberText(double value, char output[40]);

// ---- exact decimal integers (BigInt) -------------------------------------------------

// Each takes and returns /^\d+$/ arena strings (no sign); false on syntax or
// overflow (640 bits). Division truncates, as BigInt `/` does.
int CompareDecimal(const char* a, const char* b);
const char* AddDecimal(Arena& arena, const char* a, const char* b);
const char* SubDecimal(Arena& arena, const char* a, const char* b);  // a >= b
const char* MulDecimal(Arena& arena, const char* a, const char* b);
const char* MulSmallDecimal(Arena& arena, const char* a, uint32_t factor);
const char* DivSmallDecimal(Arena& arena, const char* a, uint32_t divisor);
// a * 10^exponent, a / 10^exponent (truncating).
const char* ScaleUp(Arena& arena, const char* a, uint32_t exponent);
const char* ScaleDown(Arena& arena, const char* a, uint32_t exponent);
bool IsZeroDecimal(const char* a);

}  // namespace fortis::exchange::cross_chain
