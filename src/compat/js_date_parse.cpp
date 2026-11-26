#include "js_date_parse.h"

#include <math.h>

// Port of v8/src/date/dateparser{.h,-inl.h,.cc} at tag 12.4.254.21
// (sha256 dateparser.h 8c4dd289…, dateparser-inl.h 9335a83e…,
// dateparser.cc 64ae6bae…, date.cc dfab5cd7…, builtins-date.cc read at the
// same tag). Names follow V8 so the two can be compared line by line.
namespace fortis::compat {
namespace {

constexpr int kNone = 0x7fffffff;  // kMaxInt
constexpr int kMaxSignificantDigits = 9;

bool between(int x, int lo, int hi) {
  return static_cast<unsigned>(x - lo) <= static_cast<unsigned>(hi - lo);
}

// V8 IsWhiteSpace (ES WhiteSpace) and IsWhiteSpaceOrLineTerminator.
bool is_white_space(uint32_t c) {
  return c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000 ||
         c == 0xFEFF;
}
bool is_white_space_or_line_terminator(uint32_t c) {
  return is_white_space(c) || c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

struct InputReader {
  const uint16_t* buffer;
  int length;
  int index = 0;
  uint32_t ch = 0;

  void Next() {
    ch = index < length ? buffer[index] : 0;
    ++index;
  }
  int position() const { return index; }
  int ReadUnsignedNumeral() {
    int n = 0, i = 0;
    while (ch == '0') Next();
    while (IsAsciiDigit()) {
      if (i < kMaxSignificantDigits) n = n * 10 + static_cast<int>(ch) - '0';
      ++i;
      Next();
    }
    return n;
  }
  int ReadWord(uint32_t* prefix, int prefix_size) {
    int len;
    for (len = 0; IsAsciiAlphaOrAbove() && !IsWhiteSpaceChar(); Next(), ++len) {
      if (len < prefix_size) prefix[len] = ch | 0x20;  // AsciiAlphaToLower
    }
    for (int i = len; i < prefix_size; ++i) prefix[i] = 0;
    return len;
  }
  bool Skip(uint32_t c) {
    if (ch == c) { Next(); return true; }
    return false;
  }
  bool SkipWhiteSpace() {
    if (is_white_space_or_line_terminator(ch)) { Next(); return true; }
    return false;
  }
  bool SkipParentheses() {
    if (ch != '(') return false;
    int balance = 0;
    do {
      if (ch == ')') --balance;
      else if (ch == '(') ++balance;
      Next();
    } while (balance > 0 && ch);
    return true;
  }
  bool IsEnd() const { return ch == 0; }
  bool IsAsciiDigit() const { return ch >= '0' && ch <= '9'; }
  bool IsAsciiAlphaOrAbove() const { return ch >= 'A'; }
  bool IsWhiteSpaceChar() const { return is_white_space(ch); }
};

enum KeywordType { INVALID, MONTH_NAME, TIME_ZONE_NAME, TIME_SEPARATOR, AM_PM };

enum TagType {
  kInvalidTokenTag = -6,
  kUnknownTokenTag = -5,
  kWhiteSpaceTag = -4,
  kNumberTag = -3,
  kSymbolTag = -2,
  kEndOfInputTag = -1,
  kKeywordTagStart = 0
};

struct DateToken {
  int tag;
  int length;
  int value;

  bool IsInvalid() const { return tag == kInvalidTokenTag; }
  bool IsNumber() const { return tag == kNumberTag; }
  bool IsSymbol() const { return tag == kSymbolTag; }
  bool IsWhiteSpace() const { return tag == kWhiteSpaceTag; }
  bool IsEndOfInput() const { return tag == kEndOfInputTag; }
  bool IsKeyword() const { return tag >= kKeywordTagStart; }
  int number() const { return value; }
  KeywordType keyword_type() const { return static_cast<KeywordType>(tag); }
  int keyword_value() const { return value; }
  char symbol() const { return static_cast<char>(value); }
  bool IsSymbol(char s) const { return IsSymbol() && symbol() == s; }
  bool IsKeywordType(KeywordType t) const { return tag == t; }
  bool IsFixedLengthNumber(int n) const { return IsNumber() && length == n; }
  bool IsAsciiSign() const { return tag == kSymbolTag && (value == '-' || value == '+'); }
  int ascii_sign() const { return 44 - value; }
  bool IsKeywordZ() const { return IsKeywordType(TIME_ZONE_NAME) && length == 1 && value == 0; }

  static DateToken Keyword(KeywordType t, int v, int len) { return DateToken{t, len, v}; }
  static DateToken Number(int v, int len) { return DateToken{kNumberTag, len, v}; }
  static DateToken Symbol(char s) { return DateToken{kSymbolTag, 1, s}; }
  static DateToken EndOfInput() { return DateToken{kEndOfInputTag, 0, -1}; }
  static DateToken WhiteSpace(int len) { return DateToken{kWhiteSpaceTag, len, -1}; }
  static DateToken Unknown() { return DateToken{kUnknownTokenTag, 1, -1}; }
  static DateToken Invalid() { return DateToken{kInvalidTokenTag, 0, -1}; }
};

// KeywordTable (dateparser.cc).
constexpr int kPrefixLength = 3;
const int8_t kKeywords[][5] = {
    {'j', 'a', 'n', MONTH_NAME, 1},      {'f', 'e', 'b', MONTH_NAME, 2},
    {'m', 'a', 'r', MONTH_NAME, 3},      {'a', 'p', 'r', MONTH_NAME, 4},
    {'m', 'a', 'y', MONTH_NAME, 5},      {'j', 'u', 'n', MONTH_NAME, 6},
    {'j', 'u', 'l', MONTH_NAME, 7},      {'a', 'u', 'g', MONTH_NAME, 8},
    {'s', 'e', 'p', MONTH_NAME, 9},      {'o', 'c', 't', MONTH_NAME, 10},
    {'n', 'o', 'v', MONTH_NAME, 11},     {'d', 'e', 'c', MONTH_NAME, 12},
    {'a', 'm', '\0', AM_PM, 0},          {'p', 'm', '\0', AM_PM, 12},
    {'u', 't', '\0', TIME_ZONE_NAME, 0}, {'u', 't', 'c', TIME_ZONE_NAME, 0},
    {'z', '\0', '\0', TIME_ZONE_NAME, 0}, {'g', 'm', 't', TIME_ZONE_NAME, 0},
    {'c', 'd', 't', TIME_ZONE_NAME, -5}, {'c', 's', 't', TIME_ZONE_NAME, -6},
    {'e', 'd', 't', TIME_ZONE_NAME, -4}, {'e', 's', 't', TIME_ZONE_NAME, -5},
    {'m', 'd', 't', TIME_ZONE_NAME, -6}, {'m', 's', 't', TIME_ZONE_NAME, -7},
    {'p', 'd', 't', TIME_ZONE_NAME, -7}, {'p', 's', 't', TIME_ZONE_NAME, -8},
    {'t', '\0', '\0', TIME_SEPARATOR, 0}, {'\0', '\0', '\0', INVALID, 0},
};

int keyword_lookup(const uint32_t* pre, int len) {
  int i;
  for (i = 0; kKeywords[i][3] != INVALID; ++i) {
    int j = 0;
    while (j < kPrefixLength && pre[j] == static_cast<uint32_t>(kKeywords[i][j])) ++j;
    if (j == kPrefixLength && (len <= kPrefixLength || kKeywords[i][3] == MONTH_NAME)) return i;
  }
  return i;
}

struct Tokenizer {
  InputReader* in;
  DateToken next;

  DateToken Scan() {
    const int pre_pos = in->position();
    if (in->IsEnd()) return DateToken::EndOfInput();
    if (in->IsAsciiDigit()) {
      const int n = in->ReadUnsignedNumeral();
      const int length = in->position() - pre_pos;
      return DateToken::Number(n, length);
    }
    if (in->Skip(':')) return DateToken::Symbol(':');
    if (in->Skip('-')) return DateToken::Symbol('-');
    if (in->Skip('+')) return DateToken::Symbol('+');
    if (in->Skip('.')) return DateToken::Symbol('.');
    if (in->Skip(')')) return DateToken::Symbol(')');
    if (in->IsAsciiAlphaOrAbove() && !in->IsWhiteSpaceChar()) {
      uint32_t buffer[3] = {0, 0, 0};
      const int length = in->ReadWord(buffer, 3);
      const int index = keyword_lookup(buffer, length);
      return DateToken::Keyword(static_cast<KeywordType>(kKeywords[index][3]), kKeywords[index][4],
                                length);
    }
    if (in->SkipWhiteSpace()) return DateToken::WhiteSpace(in->position() - pre_pos);
    if (in->SkipParentheses()) return DateToken::Unknown();
    in->Next();
    return DateToken::Unknown();
  }
  DateToken Next() {
    const DateToken result = next;
    next = Scan();
    return result;
  }
  DateToken Peek() const { return next; }
  bool SkipSymbol(char s) {
    if (next.IsSymbol(s)) { next = Scan(); return true; }
    return false;
  }
};

bool is_minute(int x) { return between(x, 0, 59); }
bool is_hour(int x) { return between(x, 0, 23); }
bool is_second(int x) { return between(x, 0, 59); }
bool is_hour12(int x) { return between(x, 0, 12); }
bool is_millisecond(int x) { return between(x, 0, 999); }
bool is_month(int x) { return between(x, 1, 12); }
bool is_day(int x) { return between(x, 1, 31); }

enum { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, MILLISECOND, UTC_OFFSET, OUTPUT_SIZE };

struct TimeZoneComposer {
  int sign = kNone, hour = kNone, minute = kNone;
  void Set(int offset_in_hours) {
    sign = offset_in_hours < 0 ? -1 : 1;
    hour = offset_in_hours * sign;
    minute = 0;
  }
  void SetSign(int s) { sign = s < 0 ? -1 : 1; }
  void SetAbsoluteHour(int h) { hour = h; }
  void SetAbsoluteMinute(int m) { minute = m; }
  bool IsExpecting(int n) const { return hour != kNone && minute == kNone && is_minute(n); }
  bool IsUTC() const { return hour == 0 && minute == 0; }
  bool IsEmpty() const { return hour == kNone; }
  bool Write(double* output) {
    if (sign != kNone) {
      if (hour == kNone) hour = 0;
      if (minute == kNone) minute = 0;
      const unsigned total_unsigned = static_cast<unsigned>(hour) * 3600U +
                                      static_cast<unsigned>(minute) * 60U;
      // Smi::kMaxValue (32-bit Smis: Node.js builds without pointer compression).
      if (total_unsigned > 0x7fffffffU) return false;
      int total = static_cast<int>(total_unsigned);
      if (sign < 0) total = -total;
      output[UTC_OFFSET] = total;
    } else {
      output[UTC_OFFSET] = NAN;
    }
    return true;
  }
};

struct TimeComposer {
  static constexpr int kSize = 4;
  int comp[kSize]{};
  int index = 0;
  int hour_offset = kNone;
  bool IsEmpty() const { return index == 0; }
  bool IsExpecting(int n) const {
    return (index == 1 && is_minute(n)) || (index == 2 && is_second(n)) ||
           (index == 3 && is_millisecond(n));
  }
  bool Add(int n) { return index < kSize ? (comp[index++] = n, true) : false; }
  bool AddFinal(int n) {
    if (!Add(n)) return false;
    while (index < kSize) comp[index++] = 0;
    return true;
  }
  void SetHourOffset(int n) { hour_offset = n; }
  bool Write(double* output) {
    while (index < kSize) comp[index++] = 0;
    int& hour = comp[0];
    int& minute = comp[1];
    int& second = comp[2];
    int& millisecond = comp[3];
    if (hour_offset != kNone) {
      if (!is_hour12(hour)) return false;
      hour %= 12;
      hour += hour_offset;
    }
    if (!is_hour(hour) || !is_minute(minute) || !is_second(second) ||
        !is_millisecond(millisecond)) {
      if (hour != 24 || minute != 0 || second != 0 || millisecond != 0) return false;
    }
    output[HOUR] = hour;
    output[MINUTE] = minute;
    output[SECOND] = second;
    output[MILLISECOND] = millisecond;
    return true;
  }
};

struct DayComposer {
  static constexpr int kSize = 3;
  int comp[kSize]{};
  int index = 0;
  int named_month = kNone;
  bool is_iso_date = false;
  bool IsEmpty() const { return index == 0; }
  bool Add(int n) {
    if (index < kSize) { comp[index++] = n; return true; }
    return false;
  }
  void SetNamedMonth(int n) { named_month = n; }
  bool Write(double* output) {
    if (index < 1) return false;
    while (index < kSize) comp[index++] = 1;
    int year = 0, month = kNone, day = kNone;
    if (named_month == kNone) {
      if (is_iso_date || (index == 3 && !is_day(comp[0]))) {
        year = comp[0]; month = comp[1]; day = comp[2];
      } else {
        month = comp[0]; day = comp[1];
        if (index == 3) year = comp[2];
      }
    } else {
      month = named_month;
      if (index == 1) {
        day = comp[0];
      } else if (!is_day(comp[0])) {
        year = comp[0]; day = comp[1];
      } else {
        day = comp[0]; year = comp[1];
      }
    }
    if (!is_iso_date) {
      if (between(year, 0, 49)) year += 2000;
      else if (between(year, 50, 99)) year += 1900;
    }
    // Smi::IsValid(year): every reachable year is a 32-bit integer.
    if (!is_month(month) || !is_day(day)) return false;
    output[YEAR] = year;
    output[MONTH] = month - 1;
    output[DAY] = day;
    return true;
  }
};

int read_milliseconds(DateToken token) {
  int number = token.number();
  int length = token.length;
  if (length < 3) {
    if (length == 1) number *= 100;
    else if (length == 2) number *= 10;
  } else if (length > 3) {
    if (length > kMaxSignificantDigits) length = kMaxSignificantDigits;
    int factor = 1;
    do {
      factor *= 10;
      --length;
    } while (length > 3);
    number /= factor;
  }
  return number;
}

DateToken parse_es5(Tokenizer* scanner, DayComposer* day, TimeComposer* time,
                    TimeZoneComposer* tz) {
  if (scanner->Peek().IsAsciiSign()) {
    const DateToken sign_token = scanner->Next();
    if (!scanner->Peek().IsFixedLengthNumber(6)) return sign_token;
    const int sign = sign_token.ascii_sign();
    const int year = scanner->Next().number();
    if (sign < 0 && year == 0) return sign_token;
    day->Add(sign * year);
  } else if (scanner->Peek().IsFixedLengthNumber(4)) {
    day->Add(scanner->Next().number());
  } else {
    return scanner->Next();
  }
  if (scanner->SkipSymbol('-')) {
    if (!scanner->Peek().IsFixedLengthNumber(2) || !is_month(scanner->Peek().number()))
      return scanner->Next();
    day->Add(scanner->Next().number());
    if (scanner->SkipSymbol('-')) {
      if (!scanner->Peek().IsFixedLengthNumber(2) || !is_day(scanner->Peek().number()))
        return scanner->Next();
      day->Add(scanner->Next().number());
    }
  }
  if (!scanner->Peek().IsKeywordType(TIME_SEPARATOR)) {
    if (!scanner->Peek().IsEndOfInput()) return scanner->Next();
  } else {
    scanner->Next();
    if (!scanner->Peek().IsFixedLengthNumber(2) || !between(scanner->Peek().number(), 0, 24))
      return DateToken::Invalid();
    const bool hour_is_24 = scanner->Peek().number() == 24;
    time->Add(scanner->Next().number());
    if (!scanner->SkipSymbol(':')) return DateToken::Invalid();
    if (!scanner->Peek().IsFixedLengthNumber(2) || !is_minute(scanner->Peek().number()) ||
        (hour_is_24 && scanner->Peek().number() > 0))
      return DateToken::Invalid();
    time->Add(scanner->Next().number());
    if (scanner->SkipSymbol(':')) {
      if (!scanner->Peek().IsFixedLengthNumber(2) || !is_second(scanner->Peek().number()) ||
          (hour_is_24 && scanner->Peek().number() > 0))
        return DateToken::Invalid();
      time->Add(scanner->Next().number());
      if (scanner->SkipSymbol('.')) {
        if (!scanner->Peek().IsNumber() || (hour_is_24 && scanner->Peek().number() > 0))
          return DateToken::Invalid();
        time->Add(read_milliseconds(scanner->Next()));
      }
    }
    if (scanner->Peek().IsKeywordZ()) {
      scanner->Next();
      tz->Set(0);
    } else if (scanner->Peek().IsSymbol('+') || scanner->Peek().IsSymbol('-')) {
      tz->SetSign(scanner->Next().symbol() == '+' ? 1 : -1);
      if (scanner->Peek().IsFixedLengthNumber(4)) {
        const int hourmin = scanner->Next().number();
        const int hour = hourmin / 100;
        const int min = hourmin % 100;
        if (!is_hour(hour) || !is_minute(min)) return DateToken::Invalid();
        tz->SetAbsoluteHour(hour);
        tz->SetAbsoluteMinute(min);
      } else {
        if (!scanner->Peek().IsFixedLengthNumber(2) || !is_hour(scanner->Peek().number()))
          return DateToken::Invalid();
        tz->SetAbsoluteHour(scanner->Next().number());
        if (!scanner->SkipSymbol(':')) return DateToken::Invalid();
        if (!scanner->Peek().IsFixedLengthNumber(2) || !is_minute(scanner->Peek().number()))
          return DateToken::Invalid();
        tz->SetAbsoluteMinute(scanner->Next().number());
      }
    }
    if (!scanner->Peek().IsEndOfInput()) return DateToken::Invalid();
  }
  if (tz->IsEmpty() && time->IsEmpty()) tz->Set(0);
  day->is_iso_date = true;
  return DateToken::EndOfInput();
}

bool parse(const uint16_t* text, int length, double* out) {
  InputReader in{text, length};
  in.Next();
  Tokenizer scanner{&in, DateToken::EndOfInput()};
  scanner.next = scanner.Scan();
  TimeZoneComposer tz;
  TimeComposer time;
  DayComposer day;
  const DateToken next_unhandled_token = parse_es5(&scanner, &day, &time, &tz);
  if (next_unhandled_token.IsInvalid()) return false;
  bool has_read_number = !day.IsEmpty();
  for (DateToken token = next_unhandled_token; !token.IsEndOfInput(); token = scanner.Next()) {
    if (token.IsNumber()) {
      has_read_number = true;
      const int n = token.number();
      if (scanner.SkipSymbol(':')) {
        if (scanner.SkipSymbol(':')) {
          if (!time.IsEmpty()) return false;
          time.Add(n);
          time.Add(0);
        } else {
          if (!time.Add(n)) return false;
          if (scanner.Peek().IsSymbol('.')) scanner.Next();
        }
      } else if (scanner.SkipSymbol('.') && time.IsExpecting(n)) {
        time.Add(n);
        if (!scanner.Peek().IsNumber()) return false;
        const int ms = read_milliseconds(scanner.Next());
        if (ms < 0) return false;
        time.AddFinal(ms);
      } else if (tz.IsExpecting(n)) {
        tz.SetAbsoluteMinute(n);
      } else if (time.IsExpecting(n)) {
        time.AddFinal(n);
        const DateToken peek = scanner.Peek();
        if (!peek.IsEndOfInput() && !peek.IsWhiteSpace() && !peek.IsKeywordZ() &&
            !peek.IsAsciiSign())
          return false;
      } else {
        if (!day.Add(n)) return false;
        scanner.SkipSymbol('-');
      }
    } else if (token.IsKeyword()) {
      const KeywordType type = token.keyword_type();
      const int value = token.keyword_value();
      if (type == AM_PM && !time.IsEmpty()) {
        time.SetHourOffset(value);
      } else if (type == MONTH_NAME) {
        day.SetNamedMonth(value);
        scanner.SkipSymbol('-');
      } else if (type == TIME_ZONE_NAME && has_read_number) {
        tz.Set(value);
      } else {
        if (has_read_number) return false;
        if (scanner.Peek().IsNumber()) return false;
      }
    } else if (token.IsAsciiSign() && (tz.IsUTC() || !time.IsEmpty())) {
      tz.SetSign(token.ascii_sign());
      int n = 0, length_digits = 0;
      if (scanner.Peek().IsNumber()) {
        const DateToken next_token = scanner.Next();
        length_digits = next_token.length;
        n = next_token.number();
      }
      has_read_number = true;
      if (scanner.Peek().IsSymbol(':')) {
        tz.SetAbsoluteHour(n);
        tz.SetAbsoluteMinute(kNone);
      } else if (length_digits == 2 || length_digits == 1) {
        tz.SetAbsoluteHour(n);
        tz.SetAbsoluteMinute(0);
      } else if (length_digits == 4 || length_digits == 3) {
        tz.SetAbsoluteHour(n / 100);
        tz.SetAbsoluteMinute(n % 100);
      } else {
        return false;
      }
    } else if ((token.IsAsciiSign() || token.IsSymbol(')')) && has_read_number) {
      return false;
    }
  }
  return day.Write(out) && time.Write(out) && tz.Write(out);
}

double double_to_integer(double x) {
  if (isnan(x)) return 0;
  if (!isfinite(x)) return x;
  return trunc(x) + 0.0;
}

double make_day(double year, double month, double date) {
  if (-1000000.0 <= year && year <= 1000000.0 && -10000000.0 <= month && month <= 10000000.0 &&
      isfinite(date)) {
    int y = static_cast<int>(year);
    int m = static_cast<int>(month);
    y += m / 12;
    m %= 12;
    if (m < 0) { m += 12; y -= 1; }
    static const int kYearDelta = 399999;
    static const int kBaseDay = 365 * (1970 + kYearDelta) + (1970 + kYearDelta) / 4 -
                                (1970 + kYearDelta) / 100 + (1970 + kYearDelta) / 400;
    int day_from_year = 365 * (y + kYearDelta) + (y + kYearDelta) / 4 - (y + kYearDelta) / 100 +
                        (y + kYearDelta) / 400 - kBaseDay;
    if ((y % 4 != 0) || (y % 100 == 0 && y % 400 != 0)) {
      static const int kDayFromMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
      day_from_year += kDayFromMonth[m];
    } else {
      static const int kDayFromMonth[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};
      day_from_year += kDayFromMonth[m];
    }
    return static_cast<double>(day_from_year - 1) + double_to_integer(date);
  }
  return NAN;
}

double make_time(double hour, double min, double sec, double ms) {
  if (isfinite(hour) && isfinite(min) && isfinite(sec) && isfinite(ms)) {
    return double_to_integer(hour) * 3600000.0 + double_to_integer(min) * 60000.0 +
           double_to_integer(sec) * 1000.0 + double_to_integer(ms);
  }
  return NAN;
}

double make_date(double day, double time) {
  if (isfinite(day) && isfinite(time)) return time + day * 86400000.0;
  return NAN;
}

constexpr double kMaxTimeInMs = 864000000.0 * 10000000.0;
constexpr double kMaxTimeBeforeUTCInMs = kMaxTimeInMs + 86400000.0 * 30;

double time_clip(double time) {
  if (-kMaxTimeInMs <= time && time <= kMaxTimeInMs) return double_to_integer(time);
  return NAN;
}

}  // namespace

bool JsDateParse(const uint16_t* text, size_t length, double* epoch_ms) {
  if (length > 0x7fffffff) return false;
  double out[OUTPUT_SIZE];
  if (!parse(text, static_cast<int>(length), out)) return false;
  const double day = make_day(out[YEAR], out[MONTH], out[DAY]);
  const double time = make_time(out[HOUR], out[MINUTE], out[SECOND], out[MILLISECOND]);
  double date = make_date(day, time);
  if (isnan(out[UTC_OFFSET])) {
    // DateCache::ToUTC with a UTC local zone is the identity.
    if (!(date >= -kMaxTimeBeforeUTCInMs && date <= kMaxTimeBeforeUTCInMs)) return false;
    date = static_cast<double>(static_cast<int64_t>(date));
  } else {
    date -= out[UTC_OFFSET] * 1000.0;
  }
  date = time_clip(date);
  if (isnan(date)) return false;
  *epoch_ms = date;
  return true;
}

}  // namespace fortis::compat
