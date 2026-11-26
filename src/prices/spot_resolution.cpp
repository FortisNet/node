#include <string_view>
#include "spot_resolution.h"
#include "kraken_price.h"
#include "defillama_price.h"
#include "mexc_price.h"

#include <math.h>
#include <string.h>

namespace fortis::prices {

namespace {
constexpr uint16_t kNoUniqueId = UINT16_MAX;
constexpr char kCoinGeckoProvider[] = "coingecko";
constexpr char kJupiterProvider[] = "jupiter";
constexpr char kVestigeProvider[] = "vestige";
constexpr char kTronScanProvider[] = "tronscan";
constexpr char kRobinhoodProvider[] = "robinhood-assets";
constexpr char kSplPrefix[] = "spl:";
constexpr int64_t kMaxTimestampMs = 253402300799999LL;
constexpr size_t kCoinGeckoPathPrefixBytes = sizeof("/api/v3/simple/price?ids=") - 1;
constexpr size_t kCoinGeckoPathFiatBytes = sizeof("&vs_currencies=") - 1;
constexpr size_t kCoinGeckoPathFieldsBytes =
    sizeof("&include_24hr_change=true&include_24hr_vol=true") - 1;
constexpr size_t kJupiterPathPrefixBytes = sizeof("/price/v3?ids=") - 1;
constexpr size_t kVestigePathPrefixBytes = sizeof("/assets/price?network_id=0&asset_ids=") - 1;
constexpr size_t kVestigePathSuffixBytes = sizeof("&denominating_asset_id=31566704") - 1;
constexpr size_t kTronScanPathBytes = sizeof("/api/getAssetWithPriceList") - 1;
constexpr size_t kSeparatorBytes = 3;  // "%2C"

bool valid_fiat(std::string_view fiat) {
  if (!fiat.data() || fiat.size() < 3 || fiat.size() > 5) return false;
  for (size_t i = 0; i < fiat.size(); ++i)
    if (fiat.data()[i] < 'a' || fiat.data()[i] > 'z') return false;
  return true;
}

bool raw_equals(const char* left, size_t left_size, const char* right,
                size_t right_size) {
  return left_size == right_size && memcmp(left, right, left_size) == 0;
}

bool usd_fiat(const char* fiat) { return fiat && strcmp(fiat, "usd") == 0; }

bool valid_slug(const char* id, size_t length) {
  if (!length || length > 100) return false;
  for (size_t k = 0; k < length; ++k)
    if (!((id[k] >= 'a' && id[k] <= 'z') || (id[k] >= '0' && id[k] <= '9') ||
          (k && id[k] == '-')))
      return false;
  return true;
}

bool base58_char(unsigned char c) {
  if (c >= '1' && c <= '9') return true;
  if (c >= 'A' && c <= 'Z') return c != 'I' && c != 'O';
  if (c >= 'a' && c <= 'z') return c != 'l';
  return false;
}

// Canonical SPL source IDs are "spl:" + 32-44 base58 bytes, as in the
// TypeScript parsePriceId.
bool valid_spl_source_id(const char* id, size_t length) {
  const size_t prefix = sizeof(kSplPrefix) - 1;
  if (length <= prefix || memcmp(id, kSplPrefix, prefix) != 0) return false;
  const size_t mint = length - prefix;
  if (mint < 32 || mint > 44) return false;
  for (size_t k = prefix; k < length; ++k)
    if (!base58_char(static_cast<unsigned char>(id[k]))) return false;
  return true;
}

bool valid_asa_source_id(const char* id, size_t length) {
  if (length <= 4 || memcmp(id, "asa:", 4) != 0 || length > 24) return false;
  for (size_t i = 4; i < length; ++i)
    if (id[i] < '0' || id[i] > '9') return false;
  return true;
}

bool valid_trc10_source_id(const char* id, size_t length) {
  providers::ParsedPriceId parsed{};
  return ParsePriceId(id, length, &parsed) && parsed.kind == providers::PriceIdKind::kTrc10 &&
      strcmp(parsed.source_id, id) == 0;
}

bool valid_robinhood_source_id(const char* id, size_t length) {
  providers::ParsedPriceId parsed{};
  return ParsePriceId(id, length, &parsed) && parsed.kind == providers::PriceIdKind::kContract &&
      strcmp(parsed.standard, "erc20") == 0 &&
      (strcmp(parsed.network, "robinhood") == 0 || strcmp(parsed.network, "eip155:4663") == 0) &&
      strcmp(parsed.source_id, id) == 0;
}

bool valid_contract_source_id(const char* id, size_t length) {
  providers::ParsedPriceId parsed{};
  return ParsePriceId(id, length, &parsed) && parsed.kind == providers::PriceIdKind::kContract;
}

size_t coingecko_batch_path_length(size_t ids_path_length, const char* fiat) {
  return kCoinGeckoPathPrefixBytes + ids_path_length + kCoinGeckoPathFiatBytes +
         strlen(fiat) + kCoinGeckoPathFieldsBytes;
}
size_t jupiter_batch_path_length(size_t ids_path_length, const char*) {
  return kJupiterPathPrefixBytes + ids_path_length;
}
size_t vestige_batch_path_length(size_t ids_path_length, const char*) {
  return kVestigePathPrefixBytes + ids_path_length + kVestigePathSuffixBytes;
}
size_t tronscan_batch_path_length(size_t, const char*) { return kTronScanPathBytes; }
size_t robinhood_batch_path_length(size_t, const char*) { return sizeof("/rhj/assets") - 1; }

// /simple/token_price/{platform} for one indexed contract.
size_t contract_single_path_length(const char* source_id, size_t length, const char* fiat) {
  providers::ParsedPriceId parsed{};
  if (!ParsePriceId(source_id, length, &parsed)) return 0;
  const char* platform = providers::CoinGeckoPlatformForNetwork(parsed.network);
  if (!platform) return 0;
  size_t encoded = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(parsed.address); *p; ++p) {
    const bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
    encoded += safe ? 1 : 3;
  }
  return sizeof("/api/v3/simple/token_price/") - 1 + strlen(platform) +
      sizeof("?contract_addresses=") - 1 + encoded +
      sizeof("&vs_currencies=") - 1 + strlen(fiat) +
      sizeof("&include_24hr_change=true&include_last_updated_at=true") - 1;
}

// ---- the spot source table ----------------------------------------------------
//
// One row per fetchable PriceIdKind is everything resolution knows about the
// provider behind it: the name recorded as `source`/cache provenance (also
// the provider's lane), the fiats it quotes, its source-id grammar and its
// request-path arithmetic. Adding a kind is one row. The row order is the
// order completed batches are written to the cache, one frame per kind.
struct SpotSourceOps {
  providers::PriceIdKind kind;
  const char* provider;
  // The provider quotes USD only; other fiats stay missing without a call.
  bool usd_only;
  bool (*valid_source)(const char* id, size_t length);
  // Leading source-id bytes the request path does not carry ("spl:", "asa:").
  size_t unsent_prefix;
  // The path length of a batch whose joined ids take `ids_path_length` bytes.
  size_t (*batch_path_length)(size_t ids_path_length, const char* fiat);
  // Non-null: the kind is fetched one id per request, on this path.
  size_t (*single_path_length)(const char* source_id, size_t length, const char* fiat);
};

constexpr SpotSourceOps kSpotSources[] = {
    {providers::PriceIdKind::kCoinGecko, kCoinGeckoProvider, false, valid_slug, 0, coingecko_batch_path_length, nullptr},
    // The "spl:" prefix is not sent; Jupiter receives the bare mint.
    {providers::PriceIdKind::kSpl, kJupiterProvider, true, valid_spl_source_id, sizeof(kSplPrefix) - 1,
     jupiter_batch_path_length, nullptr},
    {providers::PriceIdKind::kContract, kCoinGeckoProvider, false, valid_contract_source_id, 0, nullptr,
     contract_single_path_length},
    {providers::PriceIdKind::kAsa, kVestigeProvider, true, valid_asa_source_id, sizeof("asa:") - 1,
     vestige_batch_path_length, nullptr},
    {providers::PriceIdKind::kTrc10, kTronScanProvider, true, valid_trc10_source_id, 0, tronscan_batch_path_length, nullptr},
    {providers::PriceIdKind::kRobinhood, kRobinhoodProvider, true, valid_robinhood_source_id, 0, robinhood_batch_path_length,
     nullptr},
};

const SpotSourceOps* source_of(providers::PriceIdKind kind) {
  for (const SpotSourceOps& source : kSpotSources)
    if (source.kind == kind) return &source;
  return nullptr;
}

// Store answered quotes and provider-answered unknowns, one atomic frame per
// provider. Reports the first failing provider store, else stored/no-items.
bool valid_origin(const providers::CoinGeckoSpotQuote& quote, const char* fiat) {
  if (quote.origin == providers::SpotQuoteOrigin::kDefault) return true;
  if (strcmp(fiat, "usd") != 0 || quote.price <= 0 || quote.provider_observed_at_ms <= 0) return false;
  if (quote.origin == providers::SpotQuoteOrigin::kKraken) return providers::KrakenUsdPair(quote.id) != nullptr;
  if (quote.origin == providers::SpotQuoteOrigin::kMexc) return providers::MexcUsdSupported(quote.id);
  char key[providers::kDefiLlamaKeyCapacity]{};
  return quote.origin == providers::SpotQuoteOrigin::kDefiLlama && providers::DefiLlamaPriceKey(quote.id, key, sizeof(key));
}

fortis::prices::PriceSpotStoreStatus persist_by_provider(
    fortis::prices::PriceSpotCache& cache, const char* fiat,
    const fortis::prices::PriceSpotQuoteInput* quotes, const providers::PriceIdKind* quote_kinds,
    size_t quote_count, const char* const* unknown, const providers::PriceIdKind* unknown_kinds,
    size_t unknown_count, int64_t now_ms,
    const providers::SpotQuoteOrigin* origins = nullptr) {
  fortis::prices::PriceSpotStoreStatus status = fortis::prices::PriceSpotStoreStatus::kNoItems;
  for (size_t group = 0; group < sizeof(kSpotSources) / sizeof(kSpotSources[0]) + 3; ++group) {
    const bool kraken = group == sizeof(kSpotSources) / sizeof(kSpotSources[0]);
    const bool defillama = group == sizeof(kSpotSources) / sizeof(kSpotSources[0]) + 1;
    const bool mexc = group == sizeof(kSpotSources) / sizeof(kSpotSources[0]) + 2;
    const SpotSourceOps& source = kSpotSources[(kraken || defillama || mexc) ? 0 : group];
    const providers::PriceIdKind kind = source.kind;
    fortis::prices::PriceSpotQuoteInput group_quotes[kSpotResolutionMaxIds]{};
    const char* group_unknown[kSpotResolutionMaxIds]{};
    size_t group_quote_count = 0, group_unknown_count = 0;
    for (size_t i = 0; i < quote_count; ++i)
      if (mexc ? (origins && origins[i] == providers::SpotQuoteOrigin::kMexc) :
          defillama ? (origins && origins[i] == providers::SpotQuoteOrigin::kDefiLlama) :
          kraken ? (origins && origins[i] == providers::SpotQuoteOrigin::kKraken) :
          (quote_kinds[i] == kind && (!origins || origins[i] == providers::SpotQuoteOrigin::kDefault)))
        group_quotes[group_quote_count++] = quotes[i];
    for (size_t i = 0; i < unknown_count; ++i)
      if (!kraken && !defillama && !mexc && unknown_kinds[i] == kind) group_unknown[group_unknown_count++] = unknown[i];
    if (!group_quote_count && !group_unknown_count) continue;
    const fortis::prices::PriceSpotStoreStatus stored = cache.StoreProviderBatch(
        fiat, mexc ? "mexc-defillama" : defillama ? "defillama" : kraken ? "kraken" : source.provider, group_quotes, group_quote_count,
        group_unknown, group_unknown_count, true, now_ms);
    if (stored == fortis::prices::PriceSpotStoreStatus::kStored) {
      if (status == fortis::prices::PriceSpotStoreStatus::kNoItems) status = stored;
    } else if (stored != fortis::prices::PriceSpotStoreStatus::kNoItems) {
      return stored;
    }
  }
  return status;
}
}  // namespace

bool SpotResolutionKindFetchable(providers::PriceIdKind kind, const char* fiat) {
  const SpotSourceOps* source = source_of(kind);
  return source && (!source->usd_only || usd_fiat(fiat));
}

const char* SpotResolutionProviderName(providers::PriceIdKind kind) {
  const SpotSourceOps* source = source_of(kind);
  return source ? source->provider : nullptr;
}

size_t SpotResolutionSingleIdPathLength(providers::PriceIdKind kind, const char* source_id,
                                        const char* fiat) {
  if (!source_id || !fiat) return 0;
  const size_t length = strnlen(source_id, providers::kPriceIdMaxSourceIdBytes + 1);
  const SpotSourceOps* source = source_of(kind);
  if (!length || length > providers::kPriceIdMaxSourceIdBytes || !source ||
      !source->valid_source(source_id, length)) return 0;
  if (source->single_path_length) return source->single_path_length(source_id, length, fiat);
  return source->batch_path_length(length - source->unsent_prefix, fiat);
}

void SpotResolutionWorkspace::Reset() {
  ++generation_;
  if (generation_ == 0) ++generation_;
  fiat_[0] = '\0';
  raw_count_ = 0;
  unique_count_ = 0;
  intent_count_ = 0;
  fetch_count_ = 0;
  started_at_ms_ = 0;
  provider_completed_ = false;
  batch_managed_ = false;
  response_price_count_ = 0;
  response_missing_count_ = 0;
  response_invalid_count_ = 0;
  response_ = SpotResponseInput{};
}

void SpotResolutionWorkspace::CopyValue(const fortis::prices::PriceSpotValue& source,
                                         bool stale, bool cache_hit,
                                         Value* output) {
  const size_t source_length = strnlen(source.provider, sizeof(source.provider));
  if (source_length >= sizeof(output->source)) return;
  memcpy(output->source, source.provider, source_length);
  output->source[source_length] = '\0';
  output->price = source.price;
  output->change24h_pct = source.change24h_pct;
  output->has_change24h_pct = source.has_change24h_pct;
  output->as_of_ms = source.as_of_ms;
  output->stale = stale;
  output->cache_hit = cache_hit;
  output->has_cache_timestamp = cache_hit;
  output->cache_timestamp_ms = cache_hit ? source.as_of_ms : 0;
}

void SpotResolutionWorkspace::BuildResponse() {
  response_price_count_ = 0;
  response_missing_count_ = 0;
  response_invalid_count_ = 0;
  // PricesFacet records invalid IDs during the first parse pass, before it
  // walks canonical IDs. Preserve that source order and duplicate behavior.
  for (size_t i = 0; i < raw_count_; ++i) {
    std::string_view raw{raw_ids_[i], raw_lengths_[i]};
    if (raw_invalid_[i]) response_invalid_[response_invalid_count_++] = raw;
  }
  // PricesFacet next walks canonical IDs in first-seen order, then their raw
  // aliases. That order controls JSON object insertion and missing[] order.
  for (size_t slot = 0; slot < unique_count_; ++slot) {
    const UniqueId& unique = unique_[slot];
    for (size_t i = 0; i < raw_count_; ++i) {
      if (raw_invalid_[i] || raw_to_unique_[i] != slot) continue;
      std::string_view raw{raw_ids_[i], raw_lengths_[i]};
      if (unique.state == ValueState::kMissing) {
        response_missing_[response_missing_count_++] = raw;
        continue;
      }
      if (unique.state != ValueState::kPositive) continue;

      // JS prices is an object: repeated exact raw keys collapse at their
      // first insertion position; missing[] retains duplicate occurrences.
      bool already_emitted = false;
      for (size_t j = 0; j < response_price_count_; ++j) {
        const std::string_view prior = response_prices_[j].raw_key;
        if (raw_equals(raw.data(), raw.size(), prior.data(), prior.size())) {
          already_emitted = true;
          break;
        }
      }
      if (already_emitted) continue;
      SpotPriceInput& out = response_prices_[response_price_count_++];
      out = SpotPriceInput{};
      out.raw_key = raw;
      out.source_id = {unique.source_id, strlen(unique.source_id)};
      out.price = unique.value.price;
      out.has_change_24h_pct = unique.value.has_change24h_pct;
      out.change_24h_pct = unique.value.change24h_pct;
      out.as_of_ms = unique.value.as_of_ms;
      out.stale = unique.value.stale;
      out.source = {unique.value.source, strlen(unique.value.source)};
      out.cache_hit = unique.value.cache_hit;
      out.has_cache_timestamp = unique.value.has_cache_timestamp;
      out.cache_timestamp_ms = unique.value.cache_timestamp_ms;
    }
  }
  response_ = {{fiat_, strlen(fiat_)}, response_prices_, response_price_count_,
               response_missing_, response_missing_count_, response_invalid_,
               response_invalid_count_};
}

bool SpotResolutionWorkspace::ResponseReady() const {
  if (raw_count_ == 0 || !provider_completed_) return false;
  for (size_t i = 0; i < unique_count_; ++i)
    if (unique_[i].state == ValueState::kUnsupported) return false;
  return true;
}

fortis::base::Status BeginSpotResolution(
    const std::string_view* raw_ids, size_t raw_count, std::string_view fiat,
    fortis::prices::PriceSpotCache& cache, int64_t now_ms,
    SpotResolutionWorkspace* workspace, int64_t refresh_lead_ms) {
  if (workspace) workspace->Reset();
  if (!workspace || !raw_ids || raw_count == 0 ||
      raw_count > kSpotResolutionMaxIds || !valid_fiat(fiat) || now_ms < 0 ||
      now_ms > kMaxTimestampMs)
    return fortis::base::Status(fortis::base::StatusCode::kInvalidArgument, "spot resolution input");
  for (size_t i = 0; i < raw_count; ++i)
    if ((!raw_ids[i].data() && !raw_ids[i].empty()) ||
        raw_ids[i].size() > providers::kPriceIdMaxRawBytes)
      return fortis::base::Status(fortis::base::StatusCode::kInvalidArgument, "spot resolution input");

  workspace->raw_count_ = raw_count;
  workspace->unique_count_ = 0;
  workspace->intent_count_ = 0;
  workspace->fetch_count_ = 0;
  workspace->started_at_ms_ = now_ms;
  workspace->provider_completed_ = false;
  memcpy(workspace->fiat_, fiat.data(), fiat.size());
  workspace->fiat_[fiat.size()] = '\0';

  for (size_t i = 0; i < raw_count; ++i) {
    const std::string_view raw = raw_ids[i];
    if (!raw.empty()) memcpy(workspace->raw_ids_[i], raw.data(), raw.size());
    workspace->raw_ids_[i][raw.size()] = '\0';
    workspace->raw_lengths_[i] = raw.size();
    providers::ParsedPriceId parsed{};
    if (!ParsePriceId(workspace->raw_ids_[i], raw.size(), &parsed)) {
      workspace->raw_invalid_[i] = true;
      workspace->raw_to_unique_[i] = kNoUniqueId;
      continue;
    }
    workspace->raw_invalid_[i] = false;
    size_t unique_index = 0;
    while (unique_index < workspace->unique_count_ &&
           strcmp(workspace->unique_[unique_index].source_id, parsed.source_id) != 0)
      ++unique_index;
    if (unique_index == workspace->unique_count_) {
      SpotResolutionWorkspace::UniqueId& unique = workspace->unique_[unique_index];
      unique = SpotResolutionWorkspace::UniqueId{};
      memcpy(unique.source_id, parsed.source_id, strlen(parsed.source_id) + 1);
      const bool robinhood_candidate = parsed.kind == providers::PriceIdKind::kContract &&
          strcmp(parsed.standard, "erc20") == 0 &&
          (strcmp(parsed.network, "robinhood") == 0 || strcmp(parsed.network, "eip155:4663") == 0);
      unique.kind = robinhood_candidate ? providers::PriceIdKind::kRobinhood : parsed.kind;
      const bool unindexed_contract = parsed.kind == providers::PriceIdKind::kContract && !robinhood_candidate &&
          providers::CoinGeckoPlatformForNetwork(parsed.network) == nullptr;
      const bool fetchable = SpotResolutionKindFetchable(unique.kind, workspace->fiat_) &&
          !unindexed_contract;
      const fortis::prices::PriceSpotLookup lookup =
          cache.Lookup(workspace->fiat_, unique.source_id, now_ms);
      switch (lookup.state) {
        case fortis::prices::PriceSpotLookupState::kFresh:
          unique.state = SpotResolutionWorkspace::ValueState::kPositive;
          workspace->CopyValue(lookup.value, false, true, &unique.value);
          break;
        case fortis::prices::PriceSpotLookupState::kNegative:
          unique.state = SpotResolutionWorkspace::ValueState::kMissing;
          break;
        case fortis::prices::PriceSpotLookupState::kStale:
          unique.state = SpotResolutionWorkspace::ValueState::kPositive;
          workspace->CopyValue(lookup.value, true, true, &unique.value);
          if (unindexed_contract) {
            // TypeScript serves the stale floor, then revalidation records a
            // short-lived `none` miss. The answer keeps its copied floor while
            // the next read observes that deterministic negative.
            const char* unknown_id = unique.source_id;
            cache.StoreProviderBatch(workspace->fiat_, "none", nullptr, 0,
                                     &unknown_id, 1, true, now_ms);
          } else if (parsed.kind == providers::PriceIdKind::kTrc10 && !usd_fiat(workspace->fiat_)) {
            const char* unknown_id = unique.source_id;
            cache.StoreProviderBatch(workspace->fiat_, "none", nullptr, 0,
                                     &unknown_id, 1, true, now_ms);
          }
          break;
        case fortis::prices::PriceSpotLookupState::kExpired:
          // Fall through: an expired envelope requires a provider attempt
          // before its durable floor can be served.
          [[fallthrough]];
        case fortis::prices::PriceSpotLookupState::kMissing:
        case fortis::prices::PriceSpotLookupState::kCorrupt:
        case fortis::prices::PriceSpotLookupState::kIoError:
        case fortis::prices::PriceSpotLookupState::kInvalidKey:
          if (unindexed_contract) {
            // PricesFacet records a durable "none" result for ordinary
            // contracts that have no CoinGecko platform, then continues
            // resolving other IDs in the same request.
            unique.state = SpotResolutionWorkspace::ValueState::kMissing;
            const char* unknown_id = unique.source_id;
            cache.StoreProviderBatch(workspace->fiat_, "none", nullptr, 0,
                                     &unknown_id, 1, true, now_ms);
          } else if (parsed.kind == providers::PriceIdKind::kTrc10 && !usd_fiat(workspace->fiat_)) {
            unique.state = SpotResolutionWorkspace::ValueState::kMissing;
            const char* unknown_id = unique.source_id;
            cache.StoreProviderBatch(workspace->fiat_, "none", nullptr, 0,
                                     &unknown_id, 1, true, now_ms);
          } else if (fetchable) {
            unique.state = SpotResolutionWorkspace::ValueState::kPending;
          } else if (parsed.kind == providers::PriceIdKind::kSpl || parsed.kind == providers::PriceIdKind::kAsa || unique.kind == providers::PriceIdKind::kRobinhood) {
            // PricesFacet never asks Jupiter for a non-USD fiat and writes
            // no negative; only a durable floor can answer, always stale.
            if (lookup.state == fortis::prices::PriceSpotLookupState::kExpired &&
                lookup.has_floor_candidate) {
              unique.state = SpotResolutionWorkspace::ValueState::kPositive;
              workspace->CopyValue(lookup.value, true, true, &unique.value);
            } else {
              unique.state = SpotResolutionWorkspace::ValueState::kMissing;
            }
          } else {
            unique.state = SpotResolutionWorkspace::ValueState::kUnsupported;
          }
          break;
      }
      ++workspace->unique_count_;

      if (unique.state == SpotResolutionWorkspace::ValueState::kPending) {
        workspace->intents_[workspace->intent_count_++] = {
            SpotResolutionIntentKind::kFetch, unique.kind,
            {unique.source_id, strlen(unique.source_id)}, false};
        workspace->fetch_kinds_[workspace->fetch_count_] = unique.kind;
        workspace->fetch_ids_[workspace->fetch_count_++] = unique.source_id;
      } else if (lookup.state == fortis::prices::PriceSpotLookupState::kStale ||
                 (refresh_lead_ms > 0 && lookup.state == fortis::prices::PriceSpotLookupState::kFresh &&
                  lookup.value.fresh_until_ms - now_ms <= refresh_lead_ms)) {
        if (fetchable) {
          workspace->intents_[workspace->intent_count_++] = {
              SpotResolutionIntentKind::kBackgroundRevalidation, unique.kind,
              {unique.source_id, strlen(unique.source_id)}, lookup.stale, true};
        } else if (parsed.kind != providers::PriceIdKind::kSpl && parsed.kind != providers::PriceIdKind::kAsa &&
                   parsed.kind != providers::PriceIdKind::kContract && parsed.kind != providers::PriceIdKind::kTrc10) {
          workspace->intents_[workspace->intent_count_++] = {
              SpotResolutionIntentKind::kUnsupportedProvider, unique.kind,
              {unique.source_id, strlen(unique.source_id)}, true, true};
        }
        // A stale non-USD SPL value is served as-is: TypeScript's revalidation
        // batch skips Jupiter for other fiats, so there is nothing to schedule.
      } else if (unique.state == SpotResolutionWorkspace::ValueState::kUnsupported) {
        workspace->intents_[workspace->intent_count_++] = {
            SpotResolutionIntentKind::kUnsupportedProvider, unique.kind,
            {unique.source_id, strlen(unique.source_id)}, false};
      }
    }
    workspace->raw_to_unique_[i] = static_cast<uint16_t>(unique_index);
  }
  workspace->provider_completed_ = workspace->fetch_count_ == 0;
  workspace->BuildResponse();
  return fortis::base::Status::Ok();
}

SpotResolutionCompletion CompleteSpotResolution(
    SpotResolutionWorkspace* workspace, fortis::prices::PriceSpotCache& cache,
    const providers::CoinGeckoSpotResult* provider_result, int64_t completed_at_ms) {
  SpotResolutionCompletion completion{};
  if (!workspace || workspace->raw_count_ == 0) {
    completion.status = SpotResolutionCompletionStatus::kInvalidWorkspace;
    return completion;
  }
  if (workspace->batch_managed_) {
    completion.status = SpotResolutionCompletionStatus::kInvalidWorkspace;
    return completion;
  }
  if (workspace->fetch_count_ == 0) {
    completion.status = SpotResolutionCompletionStatus::kNotNeeded;
    return completion;
  }
  if (workspace->provider_completed_ || completed_at_ms < workspace->started_at_ms_ ||
      completed_at_ms > kMaxTimestampMs) {
    completion.status = SpotResolutionCompletionStatus::kInvalidWorkspace;
    return completion;
  }

  size_t slot_for_quote[kSpotResolutionMaxIds];
  bool seen[kSpotResolutionMaxIds]{};
  size_t quote_count = 0;
  bool answered = provider_result && provider_result->error == providers::CoinGeckoSpotError::kNone &&
                  provider_result->count <= workspace->fetch_count_;
  if (answered) {
    for (size_t q = 0; q < provider_result->count; ++q) {
      const providers::CoinGeckoSpotQuote& quote = provider_result->quotes[q];
      const size_t id_length = strnlen(quote.id, sizeof(quote.id));
      size_t fetch_index = 0;
      while (fetch_index < workspace->fetch_count_ &&
             (strlen(workspace->fetch_ids_[fetch_index]) != id_length ||
              memcmp(workspace->fetch_ids_[fetch_index], quote.id, id_length) != 0))
        ++fetch_index;
      if (id_length == 0 || id_length >= sizeof(quote.id) ||
          fetch_index == workspace->fetch_count_ || seen[fetch_index] ||
          !isfinite(quote.price) ||
          !valid_origin(quote, workspace->fiat_) ||
          (quote.hasChange24hPct && !isfinite(quote.change24hPct)) ||
          (quote.hasVolume24h && !isfinite(quote.volume24h))) {
        answered = false;
        break;
      }
      seen[fetch_index] = true;
      slot_for_quote[quote_count++] = fetch_index;
    }
  }

  if (answered) {
    fortis::prices::PriceSpotQuoteInput quotes[kSpotResolutionMaxIds]{};
    providers::PriceIdKind quote_kinds[kSpotResolutionMaxIds]{};
    providers::SpotQuoteOrigin origins[kSpotResolutionMaxIds]{};
    const char* unknown[kSpotResolutionMaxIds]{};
    providers::PriceIdKind unknown_kinds[kSpotResolutionMaxIds]{};
    size_t unknown_count = 0;
    for (size_t i = 0; i < quote_count; ++i) {
      const size_t fetch_index = slot_for_quote[i];
      const providers::CoinGeckoSpotQuote& quote = provider_result->quotes[i];
      quotes[i] = {workspace->fetch_ids_[fetch_index], quote.price,
                   quote.change24hPct, quote.volume24h,
                   quote.hasChange24hPct, quote.hasVolume24h,
                   completed_at_ms, 0, quote.provider_observed_at_ms};
      origins[i] = quote.origin;
      quote_kinds[i] = workspace->fetch_kinds_[fetch_index];
    }
    for (size_t i = 0; i < workspace->fetch_count_; ++i) {
      if (seen[i]) continue;
      unknown_kinds[unknown_count] = workspace->fetch_kinds_[i];
      unknown[unknown_count++] = workspace->fetch_ids_[i];
    }
    completion.cache_store_status = persist_by_provider(
        cache, workspace->fiat_, quotes, quote_kinds, quote_count,
        unknown, unknown_kinds, unknown_count, completed_at_ms, origins);

    for (size_t i = 0; i < quote_count; ++i) {
      const size_t fetch_index = slot_for_quote[i];
      // Source IDs are not contiguous across UniqueId records; locate the
      // canonical slot by exact ID instead of retaining ephemeral result refs.
      size_t slot = 0;
      while (slot < workspace->unique_count_ &&
             strcmp(workspace->unique_[slot].source_id,
                    workspace->fetch_ids_[fetch_index]) != 0) ++slot;
      if (slot == workspace->unique_count_) continue;
      const providers::CoinGeckoSpotQuote& quote = provider_result->quotes[i];
      SpotResolutionWorkspace::UniqueId& unique = workspace->unique_[slot];
      const char* provider = quote.origin == providers::SpotQuoteOrigin::kMexc ? "mexc-defillama" : quote.origin == providers::SpotQuoteOrigin::kKraken
              ? "kraken" : quote.origin == providers::SpotQuoteOrigin::kDefiLlama ? "defillama" : SpotResolutionProviderName(unique.kind);
      unique.state = SpotResolutionWorkspace::ValueState::kPositive;
      unique.value = SpotResolutionWorkspace::Value{};
      if (provider) memcpy(unique.value.source, provider, strlen(provider) + 1);
      unique.value.price = quote.price;
      unique.value.change24h_pct = quote.change24hPct;
      unique.value.has_change24h_pct = quote.hasChange24hPct;
      unique.value.as_of_ms = completed_at_ms;
      unique.value.stale = false;
      unique.value.cache_hit = false;
      unique.value.has_cache_timestamp = false;
    }
    for (size_t i = 0; i < workspace->fetch_count_; ++i) {
      if (seen[i]) continue;
      size_t slot = 0;
      while (slot < workspace->unique_count_ &&
             strcmp(workspace->unique_[slot].source_id, workspace->fetch_ids_[i]) != 0) ++slot;
      if (slot < workspace->unique_count_)
        workspace->unique_[slot].state = SpotResolutionWorkspace::ValueState::kMissing;
    }
    completion.status = SpotResolutionCompletionStatus::kApplied;
  } else {
    completion.status = SpotResolutionCompletionStatus::kProviderFailed;
    completion.cache_store_status = fortis::prices::PriceSpotStoreStatus::kTransportFailureNoWrite;
    for (size_t i = 0; i < workspace->fetch_count_; ++i) {
      size_t slot = 0;
      while (slot < workspace->unique_count_ &&
             strcmp(workspace->unique_[slot].source_id, workspace->fetch_ids_[i]) != 0) ++slot;
      if (slot == workspace->unique_count_) continue;
      SpotResolutionWorkspace::UniqueId& unique = workspace->unique_[slot];
      // Provider work may cross floor expiry, or another request may have
      // filled/negatived a cold ID after Begin. Re-read on failure and use only
      // the cache state that is current at completion.
      const fortis::prices::PriceSpotLookup current = cache.Lookup(
          workspace->fiat_, unique.source_id, completed_at_ms);
      if (current.state == fortis::prices::PriceSpotLookupState::kFresh) {
        unique.state = SpotResolutionWorkspace::ValueState::kPositive;
        workspace->CopyValue(current.value, false, true, &unique.value);
      } else if (current.state == fortis::prices::PriceSpotLookupState::kStale) {
        unique.state = SpotResolutionWorkspace::ValueState::kPositive;
        workspace->CopyValue(current.value, true, true, &unique.value);
      } else if (current.state == fortis::prices::PriceSpotLookupState::kExpired &&
                 current.has_floor_candidate) {
        unique.state = SpotResolutionWorkspace::ValueState::kPositive;
        workspace->CopyValue(current.value, true, true, &unique.value);
      } else {
        unique.state = SpotResolutionWorkspace::ValueState::kMissing;
      }
    }
  }
  size_t remaining_intents = 0;
  for (size_t i = 0; i < workspace->intent_count_; ++i) {
    if (workspace->intents_[i].kind != SpotResolutionIntentKind::kFetch)
      workspace->intents_[remaining_intents++] = workspace->intents_[i];
  }
  workspace->intent_count_ = remaining_intents;
  workspace->fetch_count_ = 0;
  workspace->provider_completed_ = true;
  workspace->BuildResponse();
  return completion;
}

bool InitializeSpotResolutionBatchAccumulator(
    SpotResolutionWorkspace* workspace, const providers::CoinGeckoSpotBatchPlan* plan,
    SpotResolutionBatchAccumulator* accumulator) {
  if (!workspace || !plan || !accumulator || workspace->raw_count_ == 0 ||
      workspace->provider_completed_ || workspace->batch_managed_ ||
      workspace->fetch_count_ == 0 ||
      plan->id_count != workspace->fetch_count_ || plan->batch_count == 0 ||
      plan->batch_count > providers::kCoinGeckoSpotMaxBatchCount) return false;
  size_t next = 0;
  for (size_t b = 0; b < plan->batch_count; ++b) {
    const providers::CoinGeckoSpotBatch& batch = plan->batches[b];
    if (batch.first_id != next || batch.id_count == 0 ||
        batch.id_count > plan->id_count - next || batch.path_length == 0 ||
        batch.path_length > providers::kCoinGeckoTokenPriceMaxPathBytes) return false;
    // One provider per batch: every ID shares the first ID's kind.
    const providers::PriceIdKind kind = workspace->fetch_kinds_[next];
    const SpotSourceOps* source = source_of(kind);
    if (!source) return false;
    size_t ids_path_length = 0;
    for (size_t j = 0; j < batch.id_count; ++j) {
      const char* id = plan->ids[next + j];
      const size_t length = strnlen(id, sizeof(plan->ids[next + j]));
      if (!length || length >= sizeof(plan->ids[next + j])) return false;
      if (workspace->fetch_kinds_[next + j] != kind ||
          !source->valid_source(id, length)) return false;
      const size_t sent = length - source->unsent_prefix;
      if (sent > SIZE_MAX - ids_path_length - (j ? kSeparatorBytes : 0)) return false;
      ids_path_length += sent + (j ? kSeparatorBytes : 0);
      if (strcmp(id, workspace->fetch_ids_[next + j]) != 0)
        return false;
    }
    if (source->single_path_length) {
      if (batch.id_count != 1 || batch.path_length != SpotResolutionSingleIdPathLength(
              kind, plan->ids[next], workspace->fiat_)) return false;
    } else if (batch.path_length != source->batch_path_length(ids_path_length, workspace->fiat_))
      return false;
    next += batch.id_count;
  }
  if (next != plan->id_count) return false;
  accumulator->plan_ = *plan;
  memset(accumulator->states_, 0, sizeof(accumulator->states_));
  memset(accumulator->answered_at_ms_, 0, sizeof(accumulator->answered_at_ms_));
  for (size_t i = 0; i < kSpotResolutionMaxIds; ++i)
    accumulator->quotes_[i] = providers::CoinGeckoSpotQuote{};
  memset(accumulator->quote_fetch_index_, 0, sizeof(accumulator->quote_fetch_index_));
  memset(accumulator->quote_answered_at_ms_, 0, sizeof(accumulator->quote_answered_at_ms_));
  accumulator->quote_count_ = 0;
  accumulator->recorded_count_ = 0;
  accumulator->finalized_ = false;
  accumulator->workspace_generation_ = workspace->generation_;
  accumulator->started_at_ms_ = workspace->started_at_ms_;
  accumulator->initialized_ = true;
  workspace->batch_managed_ = true;
  return true;
}

SpotResolutionBatchReceipt RecordSpotResolutionBatchFailure(
    SpotResolutionBatchAccumulator* accumulator, size_t batch_index,
    bool transport_failure) {
  if (!accumulator || !accumulator->initialized_ || accumulator->finalized_)
    return SpotResolutionBatchReceipt::kInvalidAccumulator;
  if (batch_index >= accumulator->plan_.batch_count)
    return SpotResolutionBatchReceipt::kInvalidBatch;
  if (accumulator->states_[batch_index] != SpotResolutionBatchState::kPending)
    return SpotResolutionBatchReceipt::kDuplicateBatch;
  accumulator->states_[batch_index] = transport_failure
      ? SpotResolutionBatchState::kTransportFailure
      : SpotResolutionBatchState::kParseFailure;
  ++accumulator->recorded_count_;
  return SpotResolutionBatchReceipt::kRecordedFailure;
}
SpotResolutionBatchReceipt RecordSpotResolutionBatchResult(
    SpotResolutionBatchAccumulator* accumulator, size_t batch_index,
    const providers::CoinGeckoSpotResult* result, int64_t answered_at_ms) {
  if (!accumulator || !accumulator->initialized_ || accumulator->finalized_)
    return SpotResolutionBatchReceipt::kInvalidAccumulator;
  if (batch_index >= accumulator->plan_.batch_count)
    return SpotResolutionBatchReceipt::kInvalidBatch;
  if (accumulator->states_[batch_index] != SpotResolutionBatchState::kPending)
    return SpotResolutionBatchReceipt::kDuplicateBatch;
  const providers::CoinGeckoSpotBatch& batch = accumulator->plan_.batches[batch_index];
  if (!result || result->error != providers::CoinGeckoSpotError::kNone) {
    accumulator->states_[batch_index] = SpotResolutionBatchState::kParseFailure;
    ++accumulator->recorded_count_;
    return SpotResolutionBatchReceipt::kRecordedFailure;
  }
  if (answered_at_ms < accumulator->started_at_ms_ ||
      answered_at_ms > kMaxTimestampMs ||
      result->count > batch.id_count ||
      accumulator->quote_count_ + result->count > kSpotResolutionMaxIds) {
    accumulator->states_[batch_index] = SpotResolutionBatchState::kInvalidResult;
    ++accumulator->recorded_count_;
    return SpotResolutionBatchReceipt::kRecordedFailure;
  }
  bool seen[kSpotResolutionMaxIds]{};
  for (size_t q = 0; q < result->count; ++q) {
    const providers::CoinGeckoSpotQuote& quote = result->quotes[q];
    const size_t length = strnlen(quote.id, sizeof(quote.id));
    size_t local = 0;
    while (local < batch.id_count &&
           (strlen(accumulator->plan_.ids[batch.first_id + local]) != length ||
            memcmp(accumulator->plan_.ids[batch.first_id + local], quote.id, length) != 0))
      ++local;
    if (length == 0 || length >= sizeof(quote.id) || local == batch.id_count ||
        seen[local] || !isfinite(quote.price) ||
        (quote.origin != providers::SpotQuoteOrigin::kDefault &&
         quote.origin != providers::SpotQuoteOrigin::kKraken &&
         quote.origin != providers::SpotQuoteOrigin::kDefiLlama &&
         quote.origin != providers::SpotQuoteOrigin::kMexc) ||
        (quote.hasChange24hPct && !isfinite(quote.change24hPct)) ||
        (quote.hasVolume24h && !isfinite(quote.volume24h))) {
      accumulator->states_[batch_index] = SpotResolutionBatchState::kInvalidResult;
      ++accumulator->recorded_count_;
      return SpotResolutionBatchReceipt::kRecordedFailure;
    }
    seen[local] = true;
  }
  for (size_t q = 0; q < result->count; ++q) {
    const providers::CoinGeckoSpotQuote& quote = result->quotes[q];
    size_t fetch_index = batch.first_id;
    while (strcmp(accumulator->plan_.ids[fetch_index], quote.id) != 0) ++fetch_index;
    const size_t out = accumulator->quote_count_++;
    accumulator->quotes_[out] = quote;
    accumulator->quote_fetch_index_[out] = static_cast<uint16_t>(fetch_index);
    accumulator->quote_answered_at_ms_[out] = answered_at_ms;
  }
  accumulator->answered_at_ms_[batch_index] = answered_at_ms;
  accumulator->states_[batch_index] = SpotResolutionBatchState::kAnswered;
  ++accumulator->recorded_count_;
  return SpotResolutionBatchReceipt::kRecordedAnswer;
}

const SpotResolutionBatchState* SpotResolutionBatchStates(
    const SpotResolutionBatchAccumulator* accumulator, size_t* count) {
  if (count) *count = accumulator && accumulator->initialized_
                          ? accumulator->plan_.batch_count : 0;
  return accumulator && accumulator->initialized_ ? accumulator->states_ : nullptr;
}

SpotResolutionCompletion FinalizeSpotResolutionBatches(
    SpotResolutionWorkspace* workspace, fortis::prices::PriceSpotCache& cache,
    SpotResolutionBatchAccumulator* accumulator, int64_t completed_at_ms,
    bool persist_cache) {
  SpotResolutionCompletion completion{};
  if (!workspace || !accumulator || !accumulator->initialized_ ||
      accumulator->finalized_ || !workspace->batch_managed_ ||
      workspace->generation_ != accumulator->workspace_generation_ ||
      workspace->provider_completed_ || workspace->fetch_count_ == 0 ||
      accumulator->recorded_count_ != accumulator->plan_.batch_count ||
      completed_at_ms < accumulator->started_at_ms_ || completed_at_ms > kMaxTimestampMs) {
    completion.status = SpotResolutionCompletionStatus::kInvalidWorkspace;
    return completion;
  }
  for (size_t b = 0; b < accumulator->plan_.batch_count; ++b)
    if (accumulator->states_[b] == SpotResolutionBatchState::kPending ||
        (accumulator->answered_at_ms_[b] && accumulator->answered_at_ms_[b] > completed_at_ms)) {
      completion.status = SpotResolutionCompletionStatus::kInvalidWorkspace;
      return completion;
    }
  size_t answered_batches = 0;
  size_t failed_ids = 0;
  fortis::prices::PriceSpotQuoteInput quotes[kSpotResolutionMaxIds]{};
  providers::PriceIdKind quote_kinds[kSpotResolutionMaxIds]{};
  providers::SpotQuoteOrigin origins[kSpotResolutionMaxIds]{};
  const char* unknown[kSpotResolutionMaxIds]{};
  providers::PriceIdKind unknown_kinds[kSpotResolutionMaxIds]{};
  size_t unknown_count = 0;
  for (size_t b = 0; b < accumulator->plan_.batch_count; ++b) {
    if (accumulator->states_[b] == SpotResolutionBatchState::kAnswered) {
      ++answered_batches;
      const providers::CoinGeckoSpotBatch& batch = accumulator->plan_.batches[b];
      bool seen[kSpotResolutionMaxIds]{};
      for (size_t q = 0; q < accumulator->quote_count_; ++q) {
        if (accumulator->quote_fetch_index_[q] < batch.first_id ||
            accumulator->quote_fetch_index_[q] >= batch.first_id + batch.id_count) continue;
        const providers::CoinGeckoSpotQuote& quote = accumulator->quotes_[q];
        if (!valid_origin(quote, workspace->fiat_)) {
          completion.status = SpotResolutionCompletionStatus::kInvalidWorkspace;
          return completion;
        }
        quotes[q] = {accumulator->plan_.ids[accumulator->quote_fetch_index_[q]],
                     quote.price, quote.change24hPct, quote.volume24h,
                     quote.hasChange24hPct, quote.hasVolume24h,
                     accumulator->quote_answered_at_ms_[q], 0, quote.provider_observed_at_ms};
        origins[q] = quote.origin;
        quote_kinds[q] = workspace->fetch_kinds_[accumulator->quote_fetch_index_[q]];
        seen[accumulator->quote_fetch_index_[q] - batch.first_id] = true;
      }
      for (size_t i = 0; i < batch.id_count; ++i) {
        if (seen[i]) continue;
        unknown_kinds[unknown_count] = workspace->fetch_kinds_[batch.first_id + i];
        unknown[unknown_count++] = accumulator->plan_.ids[batch.first_id + i];
      }
    } else {
      failed_ids += accumulator->plan_.batches[b].id_count;
    }
  }
  completion.cache_store_status = fortis::prices::PriceSpotStoreStatus::kNoItems;
  if (answered_batches && persist_cache)
    completion.cache_store_status = persist_by_provider(
        cache, workspace->fiat_, quotes, quote_kinds, accumulator->quote_count_,
        unknown, unknown_kinds, unknown_count, completed_at_ms, origins);
  for (size_t b = 0; b < accumulator->plan_.batch_count; ++b) {
    const providers::CoinGeckoSpotBatch& batch = accumulator->plan_.batches[b];
    const bool answered = accumulator->states_[b] == SpotResolutionBatchState::kAnswered;
    for (size_t i = 0; i < batch.id_count; ++i) {
      const char* id = accumulator->plan_.ids[batch.first_id + i];
      size_t slot = 0;
      while (slot < workspace->unique_count_ && strcmp(workspace->unique_[slot].source_id, id) != 0) ++slot;
      if (slot == workspace->unique_count_) continue;
      auto& unique = workspace->unique_[slot];
      if (answered) {
        size_t q = 0;
        while (q < accumulator->quote_count_ && strcmp(accumulator->quotes_[q].id, id) != 0) ++q;
        if (q == accumulator->quote_count_) {
          unique.state = SpotResolutionWorkspace::ValueState::kMissing;
        } else {
          const auto& quote = accumulator->quotes_[q];
          const char* provider = quote.origin == providers::SpotQuoteOrigin::kMexc ? "mexc-defillama" : quote.origin == providers::SpotQuoteOrigin::kKraken
              ? "kraken" : quote.origin == providers::SpotQuoteOrigin::kDefiLlama ? "defillama" : SpotResolutionProviderName(unique.kind);
          unique.state = SpotResolutionWorkspace::ValueState::kPositive;
          unique.value = SpotResolutionWorkspace::Value{};
          if (provider) memcpy(unique.value.source, provider, strlen(provider) + 1);
          unique.value.price = quote.price;
          unique.value.change24h_pct = quote.change24hPct;
          unique.value.has_change24h_pct = quote.hasChange24hPct;
          unique.value.as_of_ms = accumulator->quote_answered_at_ms_[q];
        }
      } else {
        const auto current = cache.Lookup(workspace->fiat_, id, completed_at_ms);
        if (current.state == fortis::prices::PriceSpotLookupState::kFresh) {
          unique.state = SpotResolutionWorkspace::ValueState::kPositive;
          workspace->CopyValue(current.value, false, true, &unique.value);
        } else if (current.state == fortis::prices::PriceSpotLookupState::kStale ||
                   (current.state == fortis::prices::PriceSpotLookupState::kExpired && current.has_floor_candidate)) {
          unique.state = SpotResolutionWorkspace::ValueState::kPositive;
          workspace->CopyValue(current.value, true, true, &unique.value);
        } else if (current.state == fortis::prices::PriceSpotLookupState::kNegative) {
          unique.state = SpotResolutionWorkspace::ValueState::kMissing;
        } else {
          unique.state = SpotResolutionWorkspace::ValueState::kMissing;
        }
      }
    }
  }
  size_t remaining = 0;
  for (size_t i = 0; i < workspace->intent_count_; ++i)
    if (workspace->intents_[i].kind != SpotResolutionIntentKind::kFetch)
      workspace->intents_[remaining++] = workspace->intents_[i];
  workspace->intent_count_ = remaining;
  workspace->fetch_count_ = 0;
  workspace->provider_completed_ = true;
  workspace->BuildResponse();
  accumulator->finalized_ = true;
  completion.status = failed_ids == 0 ? SpotResolutionCompletionStatus::kApplied
      : (answered_batches ? SpotResolutionCompletionStatus::kPartiallyApplied
                          : SpotResolutionCompletionStatus::kProviderFailed);
  if (!answered_batches)
    completion.cache_store_status = fortis::prices::PriceSpotStoreStatus::kTransportFailureNoWrite;
  return completion;
}

const SpotResponseInput* SpotResolutionResponse(
    const SpotResolutionWorkspace* workspace, bool* ready) {
  if (ready) *ready = workspace && workspace->ResponseReady();
  return workspace && workspace->raw_count_ != 0 ? &workspace->response_ : nullptr;
}

const SpotResolutionIntent* SpotResolutionIntents(
    const SpotResolutionWorkspace* workspace, size_t* count) {
  if (count) *count = workspace ? workspace->intent_count_ : 0;
  return workspace ? workspace->intents_ : nullptr;
}

const char* const* SpotResolutionFetchIds(
    const SpotResolutionWorkspace* workspace, size_t* count) {
  if (count) *count = workspace ? workspace->fetch_count_ : 0;
  return workspace ? workspace->fetch_ids_ : nullptr;
}

const providers::PriceIdKind* SpotResolutionFetchKinds(
    const SpotResolutionWorkspace* workspace, size_t* count) {
  if (count) *count = workspace ? workspace->fetch_count_ : 0;
  return workspace ? workspace->fetch_kinds_ : nullptr;
}

size_t SpotResolutionUniqueIdCount(const SpotResolutionWorkspace* workspace) {
  return workspace ? workspace->unique_count_ : 0;
}

}  // namespace fortis::prices
