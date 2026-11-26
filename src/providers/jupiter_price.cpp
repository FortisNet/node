#include "jupiter_price.h"

#include "jupiter_request.h"

#include <math.h>
#include <string.h>

namespace fortis::providers {

JupiterPriceResult ExtractJupiterPriceV3(
    const unsigned char* input, size_t input_length,
    const char* const* requested_mints, size_t requested_count,
    codec::BoundedJsonToken* token_workspace, size_t token_workspace_capacity) {
  JupiterPriceResult result{};
  if (!input || !input_length || !token_workspace || !token_workspace_capacity ||
      requested_count > kJupiterPriceMaxQuotes ||
      (requested_count && !requested_mints)) {
    result.error = JupiterPriceError::kInvalidArguments;
    return result;
  }
  size_t lengths[kJupiterPriceMaxQuotes]{};
  for (size_t i = 0; i < requested_count; ++i) {
    if (!JupiterValidMint(requested_mints[i], &lengths[i])) {
      result.error = JupiterPriceError::kInvalidArguments;
      return result;
    }
    for (size_t j = 0; j < i; ++j) {
      if (lengths[j] == lengths[i] &&
          memcmp(requested_mints[j], requested_mints[i], lengths[i]) == 0) {
        result.error = JupiterPriceError::kInvalidArguments;
        return result;
      }
    }
  }
  if (input_length > codec::kBoundedJsonMaxInputBytes) {
    result.error = JupiterPriceError::kResponseTooLarge;
    return result;
  }

  codec::BoundedJsonDocument document;
  codec::BoundedJsonError parse_error = codec::BoundedJsonError::kNone;
  if (!document.Parse(input, input_length, token_workspace,
                      token_workspace_capacity, &parse_error)) {
    result.error = parse_error == codec::BoundedJsonError::kInputTooLarge ?
                       JupiterPriceError::kResponseTooLarge :
                   parse_error == codec::BoundedJsonError::kTokenLimit ?
                       JupiterPriceError::kTokenLimit :
                   parse_error == codec::BoundedJsonError::kDepthLimit ?
                       JupiterPriceError::kDepthLimit :
                       JupiterPriceError::kInvalidJson;
    return result;
  }
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (!root) {
    result.error = JupiterPriceError::kInvalidJson;
    return result;
  }
  if (root->type == codec::BoundedJsonType::kNull) {
    result.error = JupiterPriceError::kNullBody;
    return result;
  }
  // Arrays, strings, numbers and booleans enumerate to entries without a
  // usable `usdPrice`, so every requested mint is answered-but-unknown.
  if (root->type != codec::BoundedJsonType::kObject) return result;

  for (size_t i = 0; i < requested_count; ++i) {
    // FindObjectValue returns the last duplicate, matching JSON.parse.
    const uint32_t row_index = document.FindObjectValue(document.root(), requested_mints[i]);
    const codec::BoundedJsonToken* row = document.token(row_index);
    if (!row || row->type != codec::BoundedJsonType::kObject) continue;
    double price = 0;
    if (!document.Number(document.FindObjectValue(row_index, "usdPrice"), &price) ||
        !isfinite(price)) continue;
    JupiterPriceQuote& quote = result.quotes[result.count];
    quote = JupiterPriceQuote{};
    memcpy(quote.mint, requested_mints[i], lengths[i]);
    quote.mint[lengths[i]] = '\0';
    quote.price = price;
    double change = 0;
    if (document.Number(document.FindObjectValue(row_index, "priceChange24h"), &change) &&
        isfinite(change)) {
      quote.change24hPct = change;
      quote.hasChange24hPct = true;
    }
    ++result.count;
  }
  return result;
}

}  // namespace fortis::providers
