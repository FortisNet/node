#include "price_spot_query.h"

#include "price_id_parser.h"
#include "query_string.h"

#include <string.h>

namespace fortis::prices {
namespace spot_query {
namespace {

void set_error(Query* out, SpotQueryOutcome status, const char* body) {
  out->status = status;
  const size_t length = strlen(body);
  memcpy(out->error_body, body, length + 1);
  out->error_body_length = length;
}

}  // namespace

size_t DecodeComponent(const char* text, size_t length, char* out, size_t capacity) {
  return http::query_string::Component{text, length}.Decode(out, capacity);
}

size_t DecodeComponentTo(const char* text, size_t length, DecodeSink sink, void* context) {
  return http::query_string::Component{text, length}.DecodeTo(sink, context);
}

size_t Utf16Units(const char* text, size_t length) {
  size_t units = 0;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if ((c & 0xc0) == 0x80) continue;
    units += c >= 0xf0 ? 2 : 1;
  }
  return units;
}

void ParseQuery(const char* query, size_t length, Query* out) {
  out->ids_length = 0;
  out->id_count = 0;
  out->error_body[0] = '\0';
  out->error_body_length = 0;
  memcpy(out->fiat, "usd", 4);
  const http::query_string::Query parsed(query, length);
  http::query_string::Component value;
  const bool encoding_ok = parsed.Valid();
  const bool ids_seen = encoding_ok && parsed.First("ids", &value);
  const size_t ids_total = ids_seen ? value.Decode(out->ids, kIdsBufferBytes) : 0;
  char fiat[8];
  const bool fiat_seen = encoding_ok && parsed.First("fiat", &value);
  const size_t fiat_total = fiat_seen ? value.Decode(fiat, sizeof(fiat)) : 0;

  // Every field is checked; any failure is one 400 BAD_REQUEST.
  bool invalid = !encoding_ok;
  if (ids_seen != 1) {
    invalid = true;
  } else {
    // Beyond the buffer the value is necessarily over 4000 units.
    const size_t units = ids_total > kIdsBufferBytes ? kMaxIdsUnits + 1
                                                     : Utf16Units(out->ids, ids_total);
    if (units < 1 || units > kMaxIdsUnits) invalid = true;
    else out->ids_length = ids_total;
  }
  if (fiat_seen) {
    bool valid = fiat_total >= 3 && fiat_total <= 5;
    for (size_t i = 0; valid && i < fiat_total; ++i)
      valid = fiat[i] >= 'a' && fiat[i] <= 'z';
    if (valid) {
      memcpy(out->fiat, fiat, fiat_total);
      out->fiat[fiat_total] = '\0';
    } else {
      invalid = true;
    }
  }
  if (invalid) {
    set_error(out, SpotQueryOutcome::kSchema, "{\"error\":\"BAD_REQUEST\"}");
    out->ids_length = 0;
    return;
  }

  // ids.split(',').map((s) => s.trim()).filter((s) => s !== '')
  size_t piece = 0;
  for (size_t i = 0; i <= out->ids_length; ++i) {
    if (i < out->ids_length && out->ids[i] != ',') continue;
    size_t begin = piece, end = i;
    piece = i + 1;
    while (size_t width = fortis::providers::JsWhitespaceAtStart(out->ids + begin, end - begin)) begin += width;
    while (size_t width = fortis::providers::JsWhitespaceAtEnd(out->ids + begin, end - begin)) end -= width;
    if (begin == end) continue;
    if (out->id_count == kMaxIds) {
      set_error(out, SpotQueryOutcome::kIdCount, "{\"error\":\"BAD_REQUEST\",\"detail\":\"1\xe2\x80\x93" "100 ids\"}");
      out->id_count = 0;
      return;
    }
    out->id_offset[out->id_count] = begin;
    out->id_length[out->id_count] = end - begin;
    ++out->id_count;
  }
  if (out->id_count == 0) {
    set_error(out, SpotQueryOutcome::kIdCount, "{\"error\":\"BAD_REQUEST\",\"detail\":\"1\xe2\x80\x93" "100 ids\"}");
    return;
  }
  for (size_t i = 0; i < out->id_count; ++i) {
    if (out->id_length[i] > kMaxIdBytes) {
      set_error(out, SpotQueryOutcome::kIdTooLong,
                "{\"error\":\"BAD_REQUEST\",\"detail\":\"ids are limited to 512 bytes each\"}");
      out->id_count = 0;
      return;
    }
  }
  out->status = SpotQueryOutcome::kOk;
}

}  // namespace spot_query
}  // namespace fortis::prices
