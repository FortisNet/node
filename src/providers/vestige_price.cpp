#include "vestige_price.h"

#include "price_id_parser.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fortis::providers {

namespace {
bool asset_id_for(const char* source_id, char* digits, size_t capacity, size_t* length) {
  ParsedPriceId parsed{};
  if (!source_id || !ParsePriceId(source_id, strlen(source_id), &parsed) ||
      parsed.kind != PriceIdKind::kAsa) return false;
  *length = strlen(parsed.address);
  if (*length + 1 > capacity) return false;
  memcpy(digits, parsed.address, *length + 1);
  return true;
}

bool id_text(double value, char* output, size_t capacity) {
  if (!isfinite(value) || value < 0 || floor(value) != value ||
      value >= 1e21 || !capacity) return false;
  if (value == 0) {
    if (capacity < 2) return false;
    output[0] = '0';
    output[1] = '\0';
    return true;
  }
  const int count = snprintf(output, capacity, "%.0f", value);
  return count > 0 && static_cast<size_t>(count) < capacity;
}
}

CoinGeckoSpotResult ExtractVestigePrices(
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
  char requested[kCoinGeckoMaxSpotIds][21]{};
  size_t requested_lengths[kCoinGeckoMaxSpotIds]{};
  for (size_t i = 0; i < requested_count; ++i) {
    if (!asset_id_for(requested_source_ids[i], requested[i], sizeof(requested[i]),
                      &requested_lengths[i])) {
      result.error = CoinGeckoSpotError::kInvalidArguments;
      return result;
    }
  }
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError parse_error = codec::BoundedJsonError::kNone;
  if (!document.Parse(input, input_length, token_workspace, token_workspace_capacity, &parse_error)) {
    result.error = parse_error == codec::BoundedJsonError::kInputTooLarge ? CoinGeckoSpotError::kResponseTooLarge :
        parse_error == codec::BoundedJsonError::kTokenLimit ? CoinGeckoSpotError::kTokenLimit :
        parse_error == codec::BoundedJsonError::kDepthLimit ? CoinGeckoSpotError::kDepthLimit : CoinGeckoSpotError::kInvalidJson;
    return result;
  }
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (!root) { result.error = CoinGeckoSpotError::kInvalidJson; return result; }
  if (root->type != codec::BoundedJsonType::kArray) return result;

  // Model Map.set in VestigeClient: later valid rows replace earlier rows,
  // while malformed rows are skipped and cannot erase an earlier valid row.
  for (uint32_t row_index = root->first_child; row_index != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* row = document.token(row_index);
    if (!row) { result.error = CoinGeckoSpotError::kInvalidJson; result.count = 0; return result; }
    const uint32_t next = row->next_sibling;
    if (row->type == codec::BoundedJsonType::kObject) {
      double asset_id = 0, price = 0;
      if (document.Number(document.FindObjectValue(row_index, "asset_id"), &asset_id) &&
          document.Number(document.FindObjectValue(row_index, "price"), &price)) {
        char key[32]{};
        if (id_text(asset_id, key, sizeof(key))) {
          for (size_t i = 0; i < requested_count; ++i) {
            if (strlen(key) != requested_lengths[i] || memcmp(key, requested[i], requested_lengths[i]) != 0) continue;
            size_t found = result.count;
            for (size_t q = 0; q < result.count; ++q)
              if (strcmp(result.quotes[q].id, requested_source_ids[i]) == 0) { found = q; break; }
            if (found == result.count) {
              CoinGeckoSpotQuote& quote = result.quotes[result.count++];
              quote = CoinGeckoSpotQuote{};
              memcpy(quote.id, requested_source_ids[i], strlen(requested_source_ids[i]) + 1);
              quote.price = price;
            } else result.quotes[found].price = price;
          }
        }
      }
    }
    row_index = next;
  }
  return result;
}

}  // namespace fortis::providers
