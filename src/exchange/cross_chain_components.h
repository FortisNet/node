#pragma once
#include "cross_chain_providers.h"
namespace fortis::exchange::cross_chain {
struct QuoteInputs {
  size_t count;
  const uint8_t* providers;
  const bool* configured;
  const Outcome* outcomes;
  Value* const* routes;
};
class QuoteAggregator {
 public:
  explicit QuoteAggregator(ExchangeProviderRegistry registry = {}) : registry_(registry) {}
  bool Aggregate(Arena& arena, const ProviderSettings& settings, const QuoteInputs& inputs, Value** output) const;
 private:
  ExchangeProviderRegistry registry_;
};
class CrossChainQuoteService {
 public:
  explicit CrossChainQuoteService(ExchangeProviderRegistry registry = {}) : registry_(registry), aggregator_(registry) {}
  Outcome Quote(size_t provider, ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
    return registry_.At(provider).Quote(context, arena, request, routes);
  }
  bool Aggregate(Arena& arena, const ProviderSettings& settings, const QuoteInputs& inputs, Value** output) const {
    return aggregator_.Aggregate(arena, settings, inputs, output);
  }
 private:
  ExchangeProviderRegistry registry_;
  QuoteAggregator aggregator_;
};
// Reset/destruction follow worker joins; adapter catalog reads synchronize on
// the existing catalog mutex. Tracking never starts a polling thread or order.
class DepositTracker {
 public:
  explicit DepositTracker(ExchangeProviderRegistry registry = {}) : registry_(registry) {}
  ~DepositTracker() { Reset(); }
  DepositTracker(const DepositTracker&) = delete;
  DepositTracker& operator=(const DepositTracker&) = delete;
  NearCatalog& Catalog() { return catalog_; }
  Outcome Track(size_t provider, ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) {
    context.near_catalog = &catalog_;
    return registry_.At(provider).Status(context, arena, request, status);
  }
  void Reset() {
    if (catalog_.loaded) codec::tj::FreeArena(catalog_.arena);
    catalog_.loaded = false; catalog_.tokens = nullptr; catalog_.count = 0;
  }
 private:
  ExchangeProviderRegistry registry_;
  NearCatalog catalog_;
};
}  // namespace fortis::exchange::cross_chain
