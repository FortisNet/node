#include "tronscan_price.h"

#include "price_id_parser.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::providers {

namespace {
bool id_string(const codec::BoundedJsonDocument& document, uint32_t index,
               char* output, size_t capacity) {
  const codec::BoundedJsonToken* token = document.token(index);
  if (!token || !capacity) return false;
  if (token->type == codec::BoundedJsonType::kString)
    return document.DecodeString(index, output, capacity, nullptr);
  if (token->type != codec::BoundedJsonType::kNumber) return false;
  double value = 0;
  if (!document.Number(index, &value) || !isfinite(value) || value < 0 ||
      floor(value) != value || value > 9007199254740991.0) return false;
  const int written = snprintf(output, capacity, "%.0f", value);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool js_number_string(const codec::BoundedJsonDocument& document, uint32_t index,
                      double* output) {
  char value[256]{};
  size_t length = 0;
  if (!document.DecodeString(index, value, sizeof(value), &length)) return false;
  size_t begin = 0, end = length;
  while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
         value[begin] == '\n' || value[begin] == '\r' || value[begin] == '\f' ||
         value[begin] == '\v')) ++begin;
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
         value[end - 1] == '\n' || value[end - 1] == '\r' || value[end - 1] == '\f' ||
         value[end - 1] == '\v')) --end;
  if (begin == end) return false;  // The TS helper rejects blank strings before Number().
  if (end - begin >= sizeof(value)) return false;
  value[end] = '\0';
  const char* text = value + begin;
  // JavaScript Number accepts unsigned binary/octal/hex prefixes, while a
  // leading sign makes those strings invalid. Some libc strtod variants
  // accept signed hexadecimal input, so reject that spelling explicitly.
  if ((text[0] == '+' || text[0] == '-') && text[1] == '0' &&
      (text[2] == 'x' || text[2] == 'X' || text[2] == 'b' ||
       text[2] == 'B' || text[2] == 'o' || text[2] == 'O')) return false;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X' ||
      text[1] == 'b' || text[1] == 'B' || text[1] == 'o' || text[1] == 'O')) {
    const unsigned char prefix = static_cast<unsigned char>(text[1]);
    const unsigned base = prefix == 'x' || prefix == 'X' ? 16 :
        prefix == 'b' || prefix == 'B' ? 2 : 8;
    if (!text[2]) return false;
    double number = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text + 2); *p; ++p) {
      unsigned digit = *p >= '0' && *p <= '9' ? *p - '0' :
          *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
          *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : base;
      if (digit >= base) return false;
      number = number * base + digit;
    }
    *output = number;
    return true;
  }
  char* parsed = nullptr;
  errno = 0;
  const double number = strtod(text, &parsed);
  if (parsed == text || *parsed != '\0') return false;
  *output = number;
  return true;
}

bool positive_price(const codec::BoundedJsonDocument& document, uint32_t index,
                    double* output) {
  const codec::BoundedJsonToken* token = document.token(index);
  if (!token) return false;
  double value = 0;
  if (token->type == codec::BoundedJsonType::kNumber) {
    if (!document.Number(index, &value)) return false;
  } else if (token->type == codec::BoundedJsonType::kString) {
    if (!js_number_string(document, index, &value)) return false;
  } else return false;
  if (!isfinite(value) || value <= 0) return false;
  *output = value;
  return true;
}

size_t quote_index(const CoinGeckoSpotResult& result, const char* id) {
  for (size_t i = 0; i < result.count; ++i)
    if (strcmp(result.quotes[i].id, id) == 0) return i;
  return result.count;
}
}  // namespace

CoinGeckoSpotResult ExtractTronScanTrc10Prices(
    const unsigned char* input, size_t input_length,
    const char* const* requested_source_ids, size_t requested_count,
    codec::BoundedJsonToken* token_workspace, size_t token_workspace_capacity) {
  CoinGeckoSpotResult result{};
  if (!input || !input_length || !token_workspace || !token_workspace_capacity ||
      !requested_source_ids || !requested_count || requested_count > kCoinGeckoMaxSpotIds) {
    result.error = CoinGeckoSpotError::kInvalidArguments;
    return result;
  }
  if (input_length > codec::kBoundedJsonMaxInputBytes) {
    result.error = CoinGeckoSpotError::kResponseTooLarge;
    return result;
  }
  // Only the canonical numeric asset IDs survive validation. Keeping 100
  // complete ParsedPriceId records reserved over 155 KiB when GCC/LTO chose
  // not to inline this extractor. TRC-10 IDs have at most 20 decimal digits.
  char requested[kCoinGeckoMaxSpotIds][21]{};
  for (size_t i = 0; i < requested_count; ++i) {
    ParsedPriceId parsed{};
    if (!requested_source_ids[i] ||
        !ParsePriceId(requested_source_ids[i], strlen(requested_source_ids[i]), &parsed) ||
        parsed.kind != PriceIdKind::kTrc10) {
      result.error = CoinGeckoSpotError::kInvalidArguments;
      return result;
    }
    const size_t length = strnlen(parsed.address, sizeof(parsed.address));
    if (!length || length >= sizeof(requested[i])) {
      result.error = CoinGeckoSpotError::kInvalidArguments;
      return result;
    }
    memcpy(requested[i], parsed.address, length + 1);
  }
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError parse_error = codec::BoundedJsonError::kNone;
  if (!document.Parse(input, input_length, token_workspace, token_workspace_capacity, &parse_error)) {
    result.error = parse_error == codec::BoundedJsonError::kInputTooLarge ? CoinGeckoSpotError::kResponseTooLarge :
        parse_error == codec::BoundedJsonError::kTokenLimit ? CoinGeckoSpotError::kTokenLimit :
        parse_error == codec::BoundedJsonError::kDepthLimit ? CoinGeckoSpotError::kDepthLimit :
        CoinGeckoSpotError::kInvalidJson;
    return result;
  }
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (!root || root->type == codec::BoundedJsonType::kNull) {
    // TS reads body.data; null throws and the facet avoids a negative write.
    result.error = CoinGeckoSpotError::kInvalidJson;
    return result;
  }
  if (root->type != codec::BoundedJsonType::kObject) return result;
  const uint32_t data_index = document.FindObjectValue(document.root(), "data");
  const codec::BoundedJsonToken* data = document.token(data_index);
  if (!data || data->type != codec::BoundedJsonType::kArray) return result;

  for (uint32_t row_index = data->first_child; row_index != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* row = document.token(row_index);
    if (!row) { result.error = CoinGeckoSpotError::kInvalidJson; result.count = 0; return result; }
    const uint32_t next = row->next_sibling;
    if (row->type == codec::BoundedJsonType::kObject) {
      const uint32_t type_index = document.FindObjectValue(row_index, "type");
      const uint32_t id_index = document.FindObjectValue(row_index, "id");
      const uint32_t price_index = document.FindObjectValue(row_index, "priceInUsd");
      double price = 0;
      if (document.StringEquals(type_index, "trc10", true) && positive_price(document, price_index, &price)) {
        char id[32]{};
        if (id_string(document, id_index, id, sizeof(id))) {
          for (size_t i = 0; i < requested_count; ++i) {
            if (strcmp(id, requested[i]) != 0) continue;
            size_t quote = quote_index(result, requested_source_ids[i]);
            if (quote == result.count) {
              CoinGeckoSpotQuote& added = result.quotes[result.count++];
              memcpy(added.id, requested_source_ids[i], strlen(requested_source_ids[i]) + 1);
              added.price = price;
            } else result.quotes[quote].price = price;
          }
        }
      }
    }
    row_index = next;
  }
  return result;
}

}  // namespace fortis::providers
