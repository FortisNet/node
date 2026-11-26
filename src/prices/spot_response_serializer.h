#include <string_view>
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices {

// Bounded projection of PricesFacet's public SpotResult/PricesResponse shape.
// String views and the ordered arrays remain owned by the caller.
constexpr size_t kSpotResponseMaxEntries = 100;
constexpr size_t kSpotResponseMaxStringBytes = 512;


struct SpotPriceInput {
  std::string_view raw_key;
  std::string_view source_id;
  double price = 0;
  bool has_change_24h_pct = false;
  double change_24h_pct = 0;
  int64_t as_of_ms = 0;
  bool stale = false;
  std::string_view source;
  bool cache_hit = false;
  bool has_cache_timestamp = false;
  int64_t cache_timestamp_ms = 0;
};

struct SpotResponseInput {
  std::string_view fiat;
  const SpotPriceInput* prices = nullptr;
  size_t price_count = 0;
  const std::string_view* missing = nullptr;
  size_t missing_count = 0;
  const std::string_view* invalid = nullptr;
  size_t invalid_count = 0;
};

enum class SpotSerializeResult : uint8_t {
  kOk,
  kInvalidInput,
  kInvalidUtf8,
  kNonFinite,
  kOutputTooSmall,
  kInvalidTimestamp
};

// Both serializers preflight the complete output before writing, so errors
// leave output untouched. out_size is set only on success.
SpotSerializeResult SerializeSpotResponseJson(const SpotResponseInput& input,
                                               char* output, size_t capacity,
                                               size_t* out_size);
SpotSerializeResult SerializeSpotResponseProtobuf(const SpotResponseInput& input,
                                                   uint8_t* output, size_t capacity,
                                                   size_t* out_size);

}  // namespace fortis::prices
