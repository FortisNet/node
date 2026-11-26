#include <string_view>
#include "spot_response_serializer.h"

#include "proto_wire.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <locale.h>
#include <math.h>

namespace fortis::prices {

namespace {
namespace pw = fortis::codec::proto_wire;

constexpr int64_t kMaxTimestampMs = 253402300799999LL;  // 9999-12-31T23:59:59.999Z

bool valid_utf8(std::string_view value) {
  if ((!value.data() && !value.empty()) || value.size() > kSpotResponseMaxStringBytes)
    return false;
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char c = static_cast<unsigned char>(value.data()[i++]);
    if (c < 0x80) continue;
    const unsigned count = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 :
                           (c & 0xf8) == 0xf0 ? 3 : 99;
    if (count == 99 || value.size() - i < count) return false;
    uint32_t cp = c & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
    for (unsigned j = 0; j < count; ++j) {
      const unsigned char next = static_cast<unsigned char>(value.data()[i++]);
      if ((next & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (next & 63);
    }
    if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
        (count == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}

SpotSerializeResult validate(const SpotResponseInput& in) {
  if (in.price_count > kSpotResponseMaxEntries ||
      in.missing_count > kSpotResponseMaxEntries ||
      in.invalid_count > kSpotResponseMaxEntries ||
      in.price_count + in.missing_count + in.invalid_count > kSpotResponseMaxEntries ||
      (in.price_count && !in.prices) || (in.missing_count && !in.missing) ||
      (in.invalid_count && !in.invalid) || in.fiat.size() < 3 || in.fiat.size() > 5)
    return SpotSerializeResult::kInvalidInput;
  if (!valid_utf8(in.fiat)) return SpotSerializeResult::kInvalidUtf8;
  for (size_t i = 0; i < in.fiat.size(); ++i) {
    const char c = in.fiat.data()[i];
    if (c < 'a' || c > 'z') return SpotSerializeResult::kInvalidInput;
  }
  for (size_t i = 0; i < in.price_count; ++i) {
    const SpotPriceInput& p = in.prices[i];
    if (p.raw_key.empty() || p.source_id.empty() || p.source.empty())
      return SpotSerializeResult::kInvalidInput;
    if (!valid_utf8(p.raw_key) || !valid_utf8(p.source_id) || !valid_utf8(p.source))
      return SpotSerializeResult::kInvalidUtf8;
    if (p.has_cache_timestamp && !p.cache_hit) return SpotSerializeResult::kInvalidInput;
    if (!isfinite(p.price) || (p.has_change_24h_pct && !isfinite(p.change_24h_pct)))
      return SpotSerializeResult::kNonFinite;
    if (p.as_of_ms < 0 || p.as_of_ms > kMaxTimestampMs ||
        (p.has_cache_timestamp && (p.cache_timestamp_ms < 0 ||
                                   p.cache_timestamp_ms > kMaxTimestampMs)))
      return SpotSerializeResult::kInvalidTimestamp;
    for (size_t j = 0; j < i; ++j) {
      const std::string_view prior = in.prices[j].raw_key;
      if (prior.size() == p.raw_key.size() &&
          memcmp(prior.data(), p.raw_key.data(), prior.size()) == 0)
        return SpotSerializeResult::kInvalidInput;
    }
  }
  for (size_t i = 0; i < in.missing_count; ++i) {
    if (!valid_utf8(in.missing[i])) return SpotSerializeResult::kInvalidUtf8;
  }
  for (size_t i = 0; i < in.invalid_count; ++i) {
    if (!valid_utf8(in.invalid[i])) return SpotSerializeResult::kInvalidUtf8;
  }
  return SpotSerializeResult::kOk;
}

bool iso_time(int64_t millis, char out[25]) {
  const time_t seconds = static_cast<time_t>(millis / 1000);
  struct tm value;
  if (!gmtime_r(&seconds, &value)) return false;
  char formatted[128];
  const int n = snprintf(formatted, sizeof(formatted), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                         value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                         value.tm_hour, value.tm_min, value.tm_sec,
                         static_cast<long long>(millis % 1000));
  if (n != 24) return false;
  memcpy(out, formatted, 25);
  return true;
}

struct Writer {
  uint8_t* out;
  size_t cap;
  size_t pos = 0;
  bool ok = true;
  void byte(uint8_t b) {
    if (pos == SIZE_MAX) { ok = false; return; }
    if (out && pos >= cap) { ok = false; return; }
    if (out) out[pos] = b;
    ++pos;
  }
  void bytes(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n && ok; ++i) byte(p[i]);
  }
  void text(std::string_view s) { bytes(s.data(), s.size()); }
  // The proto_wire sink interface.
  void Append(const void* data, size_t n) { bytes(data, n); }
};

template <size_t N> void literal(Writer& w, const char (&value)[N]) {
  w.bytes(value, N - 1);
}

void ordered_price_indices(const SpotResponseInput& in, size_t indices[kSpotResponseMaxEntries]) {
  for (size_t i = 0; i < in.price_count; ++i) indices[i] = i;
}

void json_string(Writer& w, std::string_view s) {
  w.byte('"');
  for (size_t i = 0; i < s.size() && w.ok; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data()[i]);
    switch (c) {
      case '"': w.bytes("\\\"", 2); break;
      case '\\': w.bytes("\\\\", 2); break;
      case '\b': w.bytes("\\b", 2); break;
      case '\f': w.bytes("\\f", 2); break;
      case '\n': w.bytes("\\n", 2); break;
      case '\r': w.bytes("\\r", 2); break;
      case '\t': w.bytes("\\t", 2); break;
      default:
        if (c < 0x20) {
          static const char hex[] = "0123456789abcdef";
          char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
          w.bytes(escaped, sizeof(escaped));
        } else w.byte(c);
    }
  }
  w.byte('"');
}

// std::to_chars supplies the shortest round-trip digits. Normalize its
// exponent choice to ECMAScript JSON.stringify's fixed/scientific thresholds.
bool json_number(double value, char out[64], size_t* length) {
  if (!isfinite(value)) return false;
  if (value == 0) { out[0] = '0'; *length = 1; return true; }
  char source[64];
  size_t source_length = 0;
  for (int precision = 1; precision <= 17; ++precision) {
    const int count = snprintf(source, sizeof(source), "%.*g", precision, value);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(source)) return false;
    char* parsed_end = nullptr;
    const double parsed = strtod(source, &parsed_end);
    if (parsed_end == source + count && memcmp(&parsed, &value, sizeof(value)) == 0) {  // NOLINT(bugprone-suspicious-memory-comparison): round-trip must preserve the double representation, including negative zero.
      source_length = static_cast<size_t>(count);
      break;
    }
  }
  if (!source_length) return false;
  const char* end = source + source_length;
  const char* p = source;
  const struct lconv* locale = localeconv();
  const char* decimal = locale && locale->decimal_point && locale->decimal_point[0]
                            ? locale->decimal_point : ".";
  const size_t decimal_length = strlen(decimal);
  bool negative = false;
  if (p < end && *p == '-') { negative = true; ++p; }
  char digits[32]; size_t digits_n = 0; int decimal_pos = 0;
  bool after_dot = false;
  while (p < end && *p != 'e' && *p != 'E') {
    if (!after_dot && static_cast<size_t>(end - p) >= decimal_length &&
        memcmp(p, decimal, decimal_length) == 0) {
      after_dot = true; p += decimal_length; continue;
    }
    if (digits_n >= sizeof(digits)) return false;
    digits[digits_n++] = *p++;
    if (!after_dot) ++decimal_pos;
  }
  int exponent = 0;
  if (p < end) {
    ++p; bool exp_negative = false;
    if (p < end && (*p == '+' || *p == '-')) { exp_negative = *p == '-'; ++p; }
    while (p < end) exponent = exponent * 10 + (*p++ - '0');
    if (exp_negative) exponent = -exponent;
  }
  decimal_pos += exponent;
  size_t n = 0;
  if (negative) out[n++] = '-';
  if (decimal_pos > -6 && decimal_pos <= 21) {
    if (decimal_pos <= 0) {
      out[n++] = '0'; out[n++] = '.';
      for (int z = 0; z < -decimal_pos; ++z) out[n++] = '0';
      memcpy(out + n, digits, digits_n); n += digits_n;
    } else if (static_cast<size_t>(decimal_pos) >= digits_n) {
      memcpy(out + n, digits, digits_n); n += digits_n;
      for (int z = static_cast<int>(digits_n); z < decimal_pos; ++z) out[n++] = '0';
    } else {
      memcpy(out + n, digits, static_cast<size_t>(decimal_pos)); n += decimal_pos;
      out[n++] = '.';
      memcpy(out + n, digits + decimal_pos, digits_n - decimal_pos);
      n += digits_n - decimal_pos;
    }
  } else {
    out[n++] = digits[0];
    if (digits_n > 1) { out[n++] = '.'; memcpy(out + n, digits + 1, digits_n - 1); n += digits_n - 1; }
    const int scientific = decimal_pos - 1;
    out[n++] = 'e';
    if (scientific >= 0) out[n++] = '+';
    char expbuf[16];
    const int count = snprintf(expbuf, sizeof(expbuf), "%d", scientific);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(expbuf)) return false;
    memcpy(out + n, expbuf, static_cast<size_t>(count)); n += static_cast<size_t>(count);
  }
  *length = n;
  return true;
}

void json_double(Writer& w, double value) {
  char number[64]; size_t n = 0;
  if (!json_number(value, number, &n)) { w.ok = false; return; }
  w.bytes(number, n);
}

void json_spot(Writer& w, const SpotPriceInput& p) {
  char as_of[25]; iso_time(p.as_of_ms, as_of);
  literal(w, "{\"sourceId\":"); json_string(w, p.source_id);
  literal(w, ",\"price\":"); json_double(w, p.price);
  literal(w, ",\"change24hPct\":");
  if (p.has_change_24h_pct) json_double(w, p.change_24h_pct);
  else literal(w, "null");
  literal(w, ",\"asOf\":"); json_string(w, {as_of, 24});
  literal(w, ",\"stale\":"); if (p.stale) literal(w, "true"); else literal(w, "false");
  literal(w, ",\"source\":"); json_string(w, p.source);
  literal(w, ",\"cache\":{\"hit\":"); if (p.cache_hit) literal(w, "true"); else literal(w, "false");
  literal(w, ",\"timestamp\":");
  if (p.has_cache_timestamp) { char timestamp[25]; iso_time(p.cache_timestamp_ms, timestamp); json_string(w, {timestamp, 24}); }
  else w.bytes("null", 4);
  w.bytes("}}", 2);
}

void json_document(Writer& w, const SpotResponseInput& in) {
  size_t indices[kSpotResponseMaxEntries]; ordered_price_indices(in, indices);
  literal(w, "{\"fiat\":"); json_string(w, in.fiat); literal(w, ",\"prices\":{");
  for (size_t i = 0; i < in.price_count; ++i) {
    if (i) w.byte(',');
    const SpotPriceInput& price = in.prices[indices[i]];
    json_string(w, price.raw_key); w.byte(':'); json_spot(w, price);
  }
  literal(w, "},\"missing\":[");
  for (size_t i = 0; i < in.missing_count; ++i) { if (i) w.byte(','); json_string(w, in.missing[i]); }
  literal(w, "],\"invalid\":[");
  for (size_t i = 0; i < in.invalid_count; ++i) { if (i) w.byte(','); json_string(w, in.invalid[i]); }
  w.bytes("]}", 2);
}

// Protobuf (protobuf-es toBinary(PriceSpotResponse)) on the shared proto_wire
// writer; implicit-presence omission is decided per field below.
void proto_string(Writer& w, uint32_t field, std::string_view value) {
  pw::PutBytes(w, field, value.data(), value.size());
}
void proto_spot(Writer& w, const SpotPriceInput& p) {
  char as_of[25]; iso_time(p.as_of_ms, as_of);
  proto_string(w, 1, p.source_id);
  pw::PutImplicitDouble(w, 2, p.price);
  if (p.has_change_24h_pct) pw::PutDouble(w, 3, p.change_24h_pct);
  proto_string(w, 4, {as_of, 24});
  if (p.stale) pw::PutVarintField(w, 5, 1);
  proto_string(w, 6, p.source);
  uint8_t cache_bytes[64]; Writer cache{cache_bytes, sizeof(cache_bytes)};
  if (p.cache_hit) pw::PutVarintField(cache, 1, 1);
  if (p.has_cache_timestamp) { char timestamp[25]; iso_time(p.cache_timestamp_ms, timestamp); proto_string(cache, 2, {timestamp, 24}); }
  if (!cache.ok) { w.ok = false; return; }
  pw::PutBytes(w, 7, cache_bytes, cache.pos);  // TS always constructs cache.
}
void proto_map_entry(Writer& w, const SpotPriceInput& p) {
  // The maximum nested spot payload is bounded by the string caps above.
  uint8_t entry_bytes[2304];
  Writer entry{entry_bytes, sizeof(entry_bytes)};
  proto_string(entry, 1, p.raw_key);
  uint8_t spot_bytes[2048]; Writer spot{spot_bytes, sizeof(spot_bytes)};
  proto_spot(spot, p);
  if (!entry.ok || !spot.ok) { w.ok = false; return; }
  pw::PutBytes(entry, 2, spot_bytes, spot.pos);
  if (!entry.ok) { w.ok = false; return; }
  pw::PutBytes(w, 2, entry_bytes, entry.pos);
}
void proto_document(Writer& w, const SpotResponseInput& in) {
  size_t indices[kSpotResponseMaxEntries]; ordered_price_indices(in, indices);
  proto_string(w, 1, in.fiat);
  for (size_t i = 0; i < in.price_count; ++i) proto_map_entry(w, in.prices[indices[i]]);
  for (size_t i = 0; i < in.missing_count; ++i) proto_string(w, 3, in.missing[i]);
  for (size_t i = 0; i < in.invalid_count; ++i) proto_string(w, 4, in.invalid[i]);
}
}  // namespace

SpotSerializeResult SerializeSpotResponseJson(const SpotResponseInput& input,
                                               char* output, size_t capacity,
                                               size_t* out_size) {
  const auto checked = validate(input); if (checked != SpotSerializeResult::kOk) return checked;
  Writer sizing{nullptr, 0}; json_document(sizing, input);
  if (!sizing.ok || !output || capacity < sizing.pos) return SpotSerializeResult::kOutputTooSmall;
  Writer writer{reinterpret_cast<uint8_t*>(output), capacity}; json_document(writer, input);
  if (!writer.ok || writer.pos != sizing.pos) return SpotSerializeResult::kOutputTooSmall;
  if (out_size) *out_size = writer.pos;
  return SpotSerializeResult::kOk;
}

SpotSerializeResult SerializeSpotResponseProtobuf(const SpotResponseInput& input,
                                                   uint8_t* output, size_t capacity,
                                                   size_t* out_size) {
  const auto checked = validate(input); if (checked != SpotSerializeResult::kOk) return checked;
  Writer sizing{nullptr, 0}; proto_document(sizing, input);
  if (!sizing.ok || !output || capacity < sizing.pos) return SpotSerializeResult::kOutputTooSmall;
  Writer writer{output, capacity}; proto_document(writer, input);
  if (!writer.ok || writer.pos != sizing.pos) return SpotSerializeResult::kOutputTooSmall;
  if (out_size) *out_size = writer.pos;
  return SpotSerializeResult::kOk;
}

}  // namespace fortis::prices
