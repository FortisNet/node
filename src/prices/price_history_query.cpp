#include "price_history_query.h"

#include "price_spot_query.h"
#include "query_string.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::prices {
namespace history_query {
namespace {

int radix_digit(uint32_t c, unsigned bits) {
  int value = -1;
  if (c >= '0' && c <= '9') value = static_cast<int>(c - '0');
  else if (c >= 'a' && c <= 'f') value = static_cast<int>(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F') value = static_cast<int>(c - 'A' + 10);
  return value >= 0 && value < (1 << bits) ? value : -1;
}

}  // namespace

// ECMA-262 StrWhiteSpaceChar: WhiteSpace and LineTerminator.
bool IsJsWhitespace(uint32_t c) {
  return c == 0x09 || c == 0x0a || c == 0x0b || c == 0x0c || c == 0x0d || c == 0x20 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
         c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

void JsNumberParser::Feed(const char* text, size_t length) {
  for (size_t i = 0; i < length; ++i) FeedByte(static_cast<unsigned char>(text[i]));
}

// The input is valid UTF-8 (DecodeComponent's output always is).
void JsNumberParser::FeedByte(unsigned char byte) {
  if (need_) {
    pending_ = (pending_ << 6) | (byte & 0x3f);
    if (--need_ == 0) CodePoint(pending_);
    return;
  }
  if (byte < 0x80) { CodePoint(byte); return; }
  if (byte >= 0xf0) { pending_ = byte & 0x07; need_ = 3; }
  else if (byte >= 0xe0) { pending_ = byte & 0x0f; need_ = 2; }
  else { pending_ = byte & 0x1f; need_ = 1; }
}

bool JsNumberParser::LiteralComplete() const {
  switch (state_) {
    case State::kZero: case State::kInt: case State::kFrac:
    case State::kExpDigits: case State::kRadixDigits:
      return true;
    case State::kInfinity:
      return infinity_matched_ == 8;
    default:
      return false;
  }
}

void JsNumberParser::Digit(unsigned digit, bool fraction) {
  any_digit_ = true;
  if (digit_count_ == 0 && digit == 0) {
    if (fraction) --decimal_exponent_;
    return;
  }
  if (digit_count_ < kMaxDigits) {
    digits_[digit_count_++] = static_cast<char>('0' + digit);
    if (fraction) --decimal_exponent_;
    return;
  }
  if (digit) sticky_ = true;
  if (!fraction) ++decimal_exponent_;
}

void JsNumberParser::RadixDigit(unsigned digit) {
  for (int bit = static_cast<int>(radix_bits_) - 1; bit >= 0; --bit) {
    const unsigned value = (digit >> bit) & 1u;
    if (!radix_bit_count_ && !value) continue;
    if (radix_bit_count_ < 64) radix_top_ = (radix_top_ << 1) | value;
    else if (value) radix_sticky_ = true;
    ++radix_bit_count_;
  }
}

void JsNumberParser::CodePoint(uint32_t c) {
  if (state_ == State::kBad) return;
  if (IsJsWhitespace(c)) {
    if (state_ == State::kLead || state_ == State::kTrail) return;
    state_ = LiteralComplete() ? State::kTrail : State::kBad;
    return;
  }
  const bool digit = c >= '0' && c <= '9';
  const bool exponent = c == 'e' || c == 'E';
  switch (state_) {
    case State::kLead:
      if (c == '+' || c == '-') { negative_ = c == '-'; state_ = State::kSign; return; }
      // Only an unsigned literal may take a 0x/0o/0b prefix.
      if (c == '0') { Digit(0, false); state_ = State::kZero; return; }
      [[fallthrough]];
    case State::kSign:
      if (digit) { Digit(c - '0', false); state_ = State::kInt; return; }
      if (c == '.') { state_ = State::kDotOnly; return; }
      if (c == 'I') { infinity_matched_ = 1; state_ = State::kInfinity; return; }
      break;
    case State::kZero:
      if (c == 'x' || c == 'X') { radix_bits_ = 4; state_ = State::kRadixPrefix; return; }
      if (c == 'o' || c == 'O') { radix_bits_ = 3; state_ = State::kRadixPrefix; return; }
      if (c == 'b' || c == 'B') { radix_bits_ = 1; state_ = State::kRadixPrefix; return; }
      [[fallthrough]];
    case State::kInt:
      if (digit) { Digit(c - '0', false); state_ = State::kInt; return; }
      if (c == '.') { state_ = State::kFrac; return; }
      if (exponent) { state_ = State::kExpMark; return; }
      break;
    case State::kDotOnly:
      if (digit) { Digit(c - '0', true); state_ = State::kFrac; return; }
      break;
    case State::kFrac:
      if (digit) { Digit(c - '0', true); return; }
      if (exponent) { state_ = State::kExpMark; return; }
      break;
    case State::kExpMark:
      if (c == '+' || c == '-') {
        exponent_negative_ = c == '-'; state_ = State::kExpSign; return;
      }
      [[fallthrough]];
    case State::kExpSign:
    case State::kExpDigits:
      if (digit) {
        // Saturates far beyond any finite or non-zero double.
        exponent_ = exponent_ > 100000000 ? exponent_ : exponent_ * 10 + (c - '0');
        state_ = State::kExpDigits;
        return;
      }
      break;
    case State::kInfinity:
      if (infinity_matched_ < 8 && c == static_cast<unsigned char>("Infinity"[infinity_matched_])) {
        ++infinity_matched_;
        return;
      }
      break;
    case State::kRadixPrefix:
    case State::kRadixDigits: {
      const int value = radix_digit(c, radix_bits_);
      if (value >= 0) {
        RadixDigit(static_cast<unsigned>(value)); state_ = State::kRadixDigits; return;
      }
      break;
    }
    case State::kTrail:
    case State::kBad:
      break;
  }
  state_ = State::kBad;
}

double JsNumberParser::Value() const {
  if (infinity_matched_ == 8) return negative_ ? -INFINITY : INFINITY;
  if (radix_bits_) {
    // Round the exact integer to 53 bits, ties to even (V8's
    // InternalStringToIntDouble for power-of-two radixes).
    const uint64_t bits = radix_bit_count_;
    if (bits <= 53) return static_cast<double>(radix_top_);
    const unsigned shift = bits <= 64 ? static_cast<unsigned>(bits - 53) : 11u;
    uint64_t mantissa = radix_top_ >> shift;
    const uint64_t rest = radix_top_ & ((uint64_t{1} << shift) - 1);
    const uint64_t half = uint64_t{1} << (shift - 1);
    if (rest > half || (rest == half && (radix_sticky_ || (mantissa & 1)))) ++mantissa;
    const uint64_t scale = bits - 53;
    return scale > 2000 ? INFINITY : ldexp(static_cast<double>(mantissa), static_cast<int>(scale));
  }
  if (!digit_count_) return negative_ ? -0.0 : 0.0;
  // digits × 10^exponent, with a trailing 1 standing for dropped non-zero
  // digits; strtod rounds the decimal string correctly.
  char text[kMaxDigits + 32];
  memcpy(text, digits_, digit_count_);
  size_t used = digit_count_;
  int64_t power = decimal_exponent_ + (exponent_negative_ ? -exponent_ : exponent_);
  if (sticky_) { text[used++] = '1'; --power; }
  if (power > 200000) power = 200000;
  if (power < -200000) power = -200000;
  snprintf(text + used, sizeof(text) - used, "e%lld", static_cast<long long>(power));
  const double value = strtod(text, nullptr);
  return negative_ ? -value : value;
}

double JsNumberParser::Finish() {
  if (need_) state_ = State::kBad;
  switch (state_) {
    case State::kLead: return 0.0;  // Number("") and all-whitespace strings
    case State::kTrail: return Value();
    case State::kBad: return NAN;
    default: return LiteralComplete() ? Value() : NAN;
  }
}

double JsStringToNumber(const char* text, size_t length) {
  JsNumberParser parser;
  parser.Feed(text, length);
  return parser.Finish();
}

void ParseQuery(const char* query, size_t length, Query* out) {
  out->status = HistoryQueryOutcome::kSchema;
  out->id_length = 0;
  out->from = out->to = 0;
  out->daily = false;
  out->error_body[0] = '\0';
  out->error_body_length = 0;
  const http::query_string::Query parsed(query, length);
  http::query_string::Component value;
  const bool encoding_ok = parsed.Valid();
  const bool id_seen = encoding_ok && parsed.First("id", &value);
  const size_t id_total = id_seen ? value.Decode(out->id, kIdBufferBytes) : 0;
  uint64_t from = 0, to = 0;
  const bool from_ok = encoding_ok && parsed.First("from", &value) &&
      value.Unsigned(kMaxSafeInteger, &from) && from > 0;
  const bool to_ok = encoding_ok && parsed.First("to", &value) &&
      value.Unsigned(kMaxSafeInteger, &to) && to > 0;
  char interval[4]{};
  const bool interval_seen = encoding_ok && parsed.First("interval", &value);
  const size_t interval_total = interval_seen ? value.Decode(interval, sizeof(interval)) : 0;

  // Every field is checked; any failure is one 400 BAD_REQUEST.
  bool invalid = !encoding_ok;
  if (id_seen != 1) {
    invalid = true;
  } else {
    // Beyond the buffer the value is necessarily over 200 units.
    const size_t units = id_total > kIdBufferBytes ? kMaxIdUnits + 1
                                                   : spot_query::Utf16Units(out->id, id_total);
    if (units < 1 || units > kMaxIdUnits) {
      invalid = true;
    } else {
      out->id_length = id_total;
      out->id[id_total] = '\0';
    }
  }
  if (!from_ok || !to_ok) invalid = true;
  out->from = static_cast<int64_t>(from);
  out->to = static_cast<int64_t>(to);
  if (interval_seen &&
      !(interval_total == 2 && (memcmp(interval, "1h", 2) == 0 ||
                                memcmp(interval, "1d", 2) == 0))) {
    invalid = true;
  } else if (interval_seen == 1) {
    out->daily = interval[1] == 'd';
  }
  if (invalid) {
    out->error_body_length = static_cast<size_t>(
        snprintf(out->error_body, kErrorBodyBytes, "{\"error\":\"BAD_REQUEST\"}"));
    out->id_length = 0;
    out->id[0] = '\0';
    return;
  }

  const int64_t max_range = out->daily ? kMaxDailyRangeSeconds : kMaxHourlyRangeSeconds;
  if (!(out->to > out->from && out->to - out->from <= max_range)) {
    out->status = HistoryQueryOutcome::kRange;
    out->error_body_length = static_cast<size_t>(snprintf(out->error_body, kErrorBodyBytes,
        "{\"error\":\"BAD_REQUEST\",\"detail\":\"range for %s must be 1s\xe2\x80\x93%llds\"}",
        out->daily ? "1d" : "1h", static_cast<long long>(max_range)));
    return;
  }
  out->status = HistoryQueryOutcome::kOk;
}

}  // namespace history_query
}  // namespace fortis::prices
