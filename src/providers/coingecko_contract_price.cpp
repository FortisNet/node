#include <cstdio>
#include "coingecko_contract_price.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fortis::providers {

namespace {
bool address_for(const char* source_id, char address[kPriceIdMaxSourceIdBytes + 1],
                 bool* fold_case) {
  ParsedPriceId parsed{};
  if (!source_id || !ParsePriceId(source_id, strlen(source_id), &parsed) ||
      parsed.kind != PriceIdKind::kContract) return false;
  std::snprintf(address, kPriceIdMaxSourceIdBytes + 1, "%s", parsed.address);
  // The TypeScript token-price response parser preserves provider key case;
  // its subsequent lookup uses the canonical (lowercase EVM) address.
  *fold_case = false;
  return true;
}
uint32_t hash_text(const char* text) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    h ^= *p; h *= 16777619u;
  }
  return h;
}
bool equal_address(const codec::BoundedJsonDocument& document, uint32_t key,
                   const char* address, bool fold) {
  return document.StringEquals(key, address, fold);
}
}

CoinGeckoSpotResult ExtractCoinGeckoTokenPrice(
    const unsigned char* input, size_t input_length,
    const char* const* requested_source_ids, size_t requested_count,
    const char* fiat, codec::BoundedJsonToken* token_workspace,
    size_t token_workspace_capacity, uint32_t* key_index_workspace,
    size_t key_index_workspace_capacity) {
  CoinGeckoSpotResult result{};
  if (!input || !input_length || !token_workspace || !token_workspace_capacity ||
      !key_index_workspace || key_index_workspace_capacity < codec::kBoundedJsonMaxTokens ||
      !requested_source_ids || !requested_count || requested_count > kCoinGeckoMaxSpotIds ||
      !fiat || strnlen(fiat, 6) < 3 || strnlen(fiat, 6) > 5) {
    result.error = CoinGeckoSpotError::kInvalidArguments; return result;
  }
  if (input_length > codec::kBoundedJsonMaxInputBytes) {
    result.error = CoinGeckoSpotError::kResponseTooLarge; return result;
  }
  char addresses[kCoinGeckoMaxSpotIds][kPriceIdMaxSourceIdBytes + 1]{};
  bool fold[kCoinGeckoMaxSpotIds]{};
  for (size_t i = 0; i < requested_count; ++i)
    if (!address_for(requested_source_ids[i], addresses[i], &fold[i])) {
      result.error = CoinGeckoSpotError::kInvalidArguments; return result;
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
  if (root->type == codec::BoundedJsonType::kNull) {
    // Object.entries(null) throws in the TypeScript provider extractor.
    result.error = CoinGeckoSpotError::kInvalidJson;
    return result;
  }
  if (root->type != codec::BoundedJsonType::kObject) return result;
  char change_key[20]{}; snprintf(change_key, sizeof(change_key), "%s_24h_change", fiat);
  for (size_t i = 0; i < codec::kBoundedJsonMaxTokens; ++i) key_index_workspace[i] = codec::kBoundedJsonNoToken;
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* kt = document.token(key);
    const codec::BoundedJsonToken* row = kt ? document.token(kt->next_sibling) : nullptr;
    if (!kt || !row) { result.error = CoinGeckoSpotError::kInvalidJson; result.count = 0; return result; }
    const uint32_t next = row->next_sibling;
    char decoded[kPriceIdMaxSourceIdBytes + 1]{};
    if (document.DecodeString(key, decoded, sizeof(decoded), nullptr)) {
      for (size_t i = 0; i < requested_count; ++i) if (equal_address(document, key, addresses[i], fold[i])) {
        size_t slot = hash_text(decoded) & (codec::kBoundedJsonMaxTokens - 1);
        for (size_t probes = 0; probes < codec::kBoundedJsonMaxTokens; ++probes) {
          const uint32_t prior = key_index_workspace[slot];
          if (prior == codec::kBoundedJsonNoToken) { key_index_workspace[slot] = key; token_workspace[key].auxiliary = kt->next_sibling; break; }
          char prior_text[kPriceIdMaxSourceIdBytes + 1]{};
          if (!document.DecodeString(prior, prior_text, sizeof(prior_text), nullptr)) break;
          if (strcmp(decoded, prior_text) == 0) { token_workspace[prior].auxiliary = kt->next_sibling; break; }
          slot = (slot + 1) & (codec::kBoundedJsonMaxTokens - 1);
        }
        break;
      }
    }
    key = next;
  }
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* kt = document.token(key);
    const codec::BoundedJsonToken* original = kt ? document.token(kt->next_sibling) : nullptr;
    if (!kt || !original) { result.error = CoinGeckoSpotError::kInvalidJson; result.count = 0; return result; }
    const uint32_t next = original->next_sibling;
    char decoded[kPriceIdMaxSourceIdBytes + 1]{};
    if (document.DecodeString(key, decoded, sizeof(decoded), nullptr)) {
      size_t slot = hash_text(decoded) & (codec::kBoundedJsonMaxTokens - 1);
      for (size_t probes = 0; probes < codec::kBoundedJsonMaxTokens; ++probes) {
        const uint32_t representative = key_index_workspace[slot];
        if (representative == codec::kBoundedJsonNoToken) break;
        char prior[kPriceIdMaxSourceIdBytes + 1]{};
        if (!document.DecodeString(representative, prior, sizeof(prior), nullptr)) break;
        if (strcmp(decoded, prior) == 0) {
          if (representative == key) {
            for (size_t i = 0; i < requested_count; ++i) if (equal_address(document, key, addresses[i], fold[i])) {
              const uint32_t effective = token_workspace[key].auxiliary;
              const codec::BoundedJsonToken* row = document.token(effective);
              if (!row || row->type != codec::BoundedJsonType::kObject) break;
              double price = 0;
              if (!document.Number(document.FindObjectValue(effective, fiat), &price)) break;
              CoinGeckoSpotQuote quote{};
              const size_t idlen = strlen(requested_source_ids[i]);
              memcpy(quote.id, requested_source_ids[i], idlen + 1);
              quote.price = price;
              quote.hasChange24hPct = document.Number(document.FindObjectValue(effective, change_key), &quote.change24hPct);
              double updated = 0;
              if (document.Number(document.FindObjectValue(effective, "last_updated_at"), &updated) &&
                  updated > 0 && updated <= 8640000000000.0 && floor(updated) == updated)
                quote.provider_observed_at_ms = static_cast<int64_t>(updated * 1000);
              result.quotes[result.count++] = quote;
            }
          }
          break;
        }
        slot = (slot + 1) & (codec::kBoundedJsonMaxTokens - 1);
      }
    }
    key = next;
  }
  return result;
}

}  // namespace fortis::providers
