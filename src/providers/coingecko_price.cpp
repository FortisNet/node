#include "coingecko_price.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fortis::providers {

namespace {
constexpr size_t kCoinGeckoSlugCapacity = 101;
bool valid_id(const char* input, char output[kCoinGeckoSlugCapacity]) {
  if (!input) return false;
  const size_t length = strnlen(input, kCoinGeckoSlugCapacity);
  if (!length || length >= kCoinGeckoSlugCapacity) return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (i && c == '-'))) return false;
    output[i] = static_cast<char>(c);
  }
  output[length] = '\0';
  return true;
}

bool valid_fiat(const char* fiat) {
  if (!fiat) return false;
  const size_t length = strnlen(fiat, 6);
  if (length < 3 || length > 5) return false;
  for (size_t i = 0; i < length; ++i)
    if (fiat[i] < 'a' || fiat[i] > 'z') return false;
  return true;
}

uint32_t field(const codec::BoundedJsonDocument& document, uint32_t object,
               const char* key) {
  return document.FindObjectValue(object, key);
}

uint32_t string_hash(const char* value) {
  uint32_t hash = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    hash ^= *p;
    hash *= 16777619u;
  }
  return hash;
}

int requested_index(const codec::BoundedJsonDocument& document, uint32_t key,
                   char normalized[kCoinGeckoMaxSpotIds][kCoinGeckoSlugCapacity],
                   size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (document.StringEquals(key, normalized[i], true)) return static_cast<int>(i);
  return -1;
}

size_t result_index(const CoinGeckoSpotResult& result, const char* id) {
  for (size_t i = 0; i < result.count; ++i)
    if (strcmp(result.quotes[i].id, id) == 0) return i;
  return result.count;
}
}  // namespace

CoinGeckoSpotResult ExtractCoinGeckoSimplePrice(
    const unsigned char* input, size_t input_length,
    const char* const* requested_ids, size_t requested_id_count,
    const char* fiat, codec::BoundedJsonToken* token_workspace,
    size_t token_workspace_capacity, uint32_t* key_index_workspace,
    size_t key_index_workspace_capacity) {
  CoinGeckoSpotResult result{};
  if (!input || !input_length || !token_workspace || !token_workspace_capacity ||
      !key_index_workspace || key_index_workspace_capacity < codec::kBoundedJsonMaxTokens ||
      requested_id_count > kCoinGeckoMaxSpotIds ||
      (requested_id_count && !requested_ids) || !valid_fiat(fiat)) {
    result.error = CoinGeckoSpotError::kInvalidArguments;
    return result;
  }
  if (input_length > codec::kBoundedJsonMaxInputBytes) {
    result.error = CoinGeckoSpotError::kResponseTooLarge;
    return result;
  }

  char normalized[kCoinGeckoMaxSpotIds][kCoinGeckoSlugCapacity]{};
  for (size_t i = 0; i < requested_id_count; ++i) {
    if (!valid_id(requested_ids[i], normalized[i])) {
      result.error = CoinGeckoSpotError::kInvalidArguments;
      return result;
    }
  }

  codec::BoundedJsonDocument document;
  codec::BoundedJsonError parse_error = codec::BoundedJsonError::kNone;
  if (!document.Parse(input, input_length, token_workspace,
                      token_workspace_capacity, &parse_error)) {
    result.error = parse_error == codec::BoundedJsonError::kInputTooLarge ?
                       CoinGeckoSpotError::kResponseTooLarge :
                   parse_error == codec::BoundedJsonError::kTokenLimit ?
                       CoinGeckoSpotError::kTokenLimit :
                   parse_error == codec::BoundedJsonError::kDepthLimit ?
                       CoinGeckoSpotError::kDepthLimit :
                       CoinGeckoSpotError::kInvalidJson;
    return result;
  }
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) {
    result.error = CoinGeckoSpotError::kInvalidJson;
    return result;
  }

  char change_key[20]{};
  char volume_key[20]{};
  snprintf(change_key, sizeof(change_key), "%s_24h_change", fiat);
  snprintf(volume_key, sizeof(volume_key), "%s_24h_vol", fiat);

  for (size_t i = 0; i < codec::kBoundedJsonMaxTokens; ++i)
    key_index_workspace[i] = codec::kBoundedJsonNoToken;
  // Collapse exact duplicate JSON properties in expected O(n) time. The
  // representative remains the first key token; auxiliary points at its last
  // value, matching JSON.parse/Object.entries insertion and overwrite rules.
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* key_token = document.token(key);
    const codec::BoundedJsonToken* row_token = key_token ? document.token(key_token->next_sibling) : nullptr;
    if (!key_token || !row_token) {
      result.error = CoinGeckoSpotError::kInvalidJson;
      result.count = 0;
      return result;
    }
    const uint32_t next_key = row_token->next_sibling;
    if (requested_index(document, key, normalized, requested_id_count) >= 0) {
      char decoded_key[kCoinGeckoIdCapacity * 4]{};
      if (!document.DecodeString(key, decoded_key, sizeof(decoded_key), nullptr)) {
        result.error = CoinGeckoSpotError::kInvalidJson;
        result.count = 0;
        return result;
      }
      const uint32_t hash = string_hash(decoded_key);
      size_t slot = hash & (codec::kBoundedJsonMaxTokens - 1);
      size_t probes = 0;
      for (; probes < codec::kBoundedJsonMaxTokens; ++probes) {
        const uint32_t representative = key_index_workspace[slot];
        if (representative == codec::kBoundedJsonNoToken) {
          key_index_workspace[slot] = key;
          token_workspace[key].auxiliary = key_token->next_sibling;
          break;
        }
        char prior_key[kCoinGeckoIdCapacity * 4]{};
        if (!document.DecodeString(representative, prior_key, sizeof(prior_key), nullptr)) {
          result.error = CoinGeckoSpotError::kInvalidJson;
          result.count = 0;
          return result;
        }
        if (strcmp(decoded_key, prior_key) == 0) {
          token_workspace[representative].auxiliary = key_token->next_sibling;
          break;
        }
        slot = (slot + 1) & (codec::kBoundedJsonMaxTokens - 1);
      }
      if (probes == codec::kBoundedJsonMaxTokens) {
        result.error = CoinGeckoSpotError::kTokenLimit;
        result.count = 0;
        return result;
      }
    }
    key = next_key;
  }

  // Follow first insertion order. Different casings remain distinct keys and
  // therefore retain the same last-write behavior as the TS Map conversion.
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* key_token = document.token(key);
    const codec::BoundedJsonToken* source_value = key_token ? document.token(key_token->next_sibling) : nullptr;
    if (!key_token || !source_value) {
      result.error = CoinGeckoSpotError::kInvalidJson;
      result.count = 0;
      return result;
    }
    const uint32_t next_key = source_value->next_sibling;
    const int requested = requested_index(document, key, normalized, requested_id_count);
    if (requested >= 0) {
      char decoded_key[kCoinGeckoIdCapacity * 4]{};
      document.DecodeString(key, decoded_key, sizeof(decoded_key), nullptr);
      size_t slot = string_hash(decoded_key) & (codec::kBoundedJsonMaxTokens - 1);
      for (size_t probes = 0; probes < codec::kBoundedJsonMaxTokens; ++probes) {
        const uint32_t representative = key_index_workspace[slot];
        if (representative == codec::kBoundedJsonNoToken) break;
        if (document.StringEquals(representative, decoded_key)) {
          if (representative == key) {
            const uint32_t effective_value = token_workspace[key].auxiliary;
            const codec::BoundedJsonToken* row = document.token(effective_value);
            if (row && row->type == codec::BoundedJsonType::kObject) {
              const uint32_t price_token = field(document, effective_value, fiat);
              double price = 0;
              if (document.Number(price_token, &price)) {
                CoinGeckoSpotQuote quote{};
                memcpy(quote.id, normalized[requested], strlen(normalized[requested]) + 1);
                quote.price = price;
                const uint32_t change_token = field(document, effective_value, change_key);
                const uint32_t volume_token = field(document, effective_value, volume_key);
                quote.hasChange24hPct = document.Number(change_token, &quote.change24hPct);
                quote.hasVolume24h = document.Number(volume_token, &quote.volume24h);
                double updated = 0;
                if (document.Number(field(document, effective_value, "last_updated_at"), &updated) &&
                    updated > 0 && updated <= 8640000000000.0 && floor(updated) == updated)
                  quote.provider_observed_at_ms = static_cast<int64_t>(updated * 1000);
                const size_t existing = result_index(result, quote.id);
                if (existing == result.count && result.count < kCoinGeckoMaxSpotIds)
                  result.quotes[result.count++] = quote;
                else if (existing < result.count)
                  result.quotes[existing] = quote;
              }
            }
          }
          break;
        }
        slot = (slot + 1) & (codec::kBoundedJsonMaxTokens - 1);
      }
    }
    key = next_key;
  }
  return result;
}

}  // namespace fortis::providers

