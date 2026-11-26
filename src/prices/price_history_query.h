#pragma once

#include <stddef.h>
#include <stdint.h>

// GET history: strict decimal from/to, first repeated value, valid UTF-8.
// Positive safe integers; to > from and at most 90 days (1h) or ten years
// (1d). IDs are bounded to 200 UTF-16 units; intervals are 1h and 1d.
// Shared query decoding preserves +, percent escapes and encoded separators.
namespace fortis::prices {
namespace history_query {

constexpr size_t kMaxIdUnits = 200;  // zod max(200), in UTF-16 code units
// A UTF-16 code unit is at most three UTF-8 bytes, so a longer value is
// already over the zod maximum and is never needed verbatim.
constexpr size_t kIdBufferBytes = kMaxIdUnits * 3;
constexpr int64_t kMaxHourlyRangeSeconds = 90 * 86400LL;
constexpr int64_t kMaxDailyRangeSeconds = 10 * 366 * 86400LL;
constexpr int64_t kMaxSafeInteger = 9007199254740991LL;
// The longest body is the kRange refusal (about 80 bytes).
constexpr size_t kErrorBodyBytes = 256;

enum class HistoryQueryOutcome : uint8_t {
  kOk,
  kSchema,  // 400 {"error":"BAD_REQUEST"}
  kRange,   // 400 {"error":"BAD_REQUEST","detail":"range for 1h must be 1s–7776000s"}
};

struct Query {
  HistoryQueryOutcome status = HistoryQueryOutcome::kSchema;
  // The decoded `id` value (UTF-8, untrimmed: parsePriceId trims it).
  char id[kIdBufferBytes + 1]{};
  size_t id_length = 0;
  int64_t from = 0;
  int64_t to = 0;
  bool daily = false;  // interval 1d; 1h otherwise
  // Complete 400 body for every status other than kOk.
  char error_body[kErrorBodyBytes]{};
  size_t error_body_length = 0;
};

// Parses the query string (the text after the first '?', not including it;
// null or empty when the target has none).
void ParseQuery(const char* query, size_t length, Query* out);

// JavaScript Number(string) (ECMA-262 StringToNumber) over valid UTF-8 fed
// in any number of pieces, without buffering the text: whitespace and line
// terminators are trimmed, the empty string is 0, decimal literals (sign,
// fraction, exponent, Infinity) and unsigned 0x/0o/0b literals are rounded
// to the nearest double, and anything else is NaN. At most 800 significant
// decimal digits are kept, plus a sticky digit for the rest, which is
// enough for correct rounding.
class JsNumberParser {
 public:
  void Feed(const char* text, size_t length);
  void FeedByte(unsigned char byte);
  // NaN, ±Infinity or the rounded value.
  double Finish();

 private:
  enum class State : uint8_t {
    kLead, kSign, kZero, kInt, kDotOnly, kFrac, kExpMark, kExpSign, kExpDigits,
    kInfinity, kRadixPrefix, kRadixDigits, kTrail, kBad,
  };
  static constexpr size_t kMaxDigits = 800;
  void CodePoint(uint32_t code_point);
  bool LiteralComplete() const;
  void Digit(unsigned digit, bool fraction);
  void RadixDigit(unsigned digit);
  double Value() const;

  State state_ = State::kLead;
  uint32_t pending_ = 0;
  unsigned need_ = 0;
  bool negative_ = false;
  // Decimal: significant digits, a sticky flag for dropped non-zero digits,
  // the power-of-ten adjustment and the explicit exponent.
  char digits_[kMaxDigits]{};
  size_t digit_count_ = 0;
  bool any_digit_ = false;
  bool sticky_ = false;
  int64_t decimal_exponent_ = 0;
  int64_t exponent_ = 0;
  bool exponent_negative_ = false;
  unsigned infinity_matched_ = 0;
  // 0x/0o/0b: bits per digit, significant bit count, the first 64 bits.
  unsigned radix_bits_ = 0;
  uint64_t radix_top_ = 0;
  uint64_t radix_bit_count_ = 0;
  bool radix_sticky_ = false;
};

double JsStringToNumber(const char* text, size_t length);

// ECMA-262 WhiteSpace or LineTerminator: the code points Number() and
// String.prototype.trim() strip.
bool IsJsWhitespace(uint32_t code_point);

}  // namespace history_query
}  // namespace fortis::prices
