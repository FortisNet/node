#pragma once
#include "token_facts.h"
namespace fortis::tokens::token_facts {
/** Synchronous facts facade borrowing Work for the entire facade lifetime.
 * Input string_views borrow caller storage only until each call returns; parsed
 * identifiers use result-owned records and Work arena storage rather than
 * retaining those input views. Returned Value trees borrow the Work arena;
 * resetting it invalidates them. Work must not be concurrently
 * mutated; callers serialize calls and retain dependencies through completion.
 * Domain failures use FactsOutcome; allocation/callback exceptions retain the
 * enclosing request boundary's existing policy. This facade never queues a view.
 */
class TokenFactsService {
 public:
  explicit TokenFactsService(Work& work) : work_(work) {}
  FactsOutcome Compute(std::string_view network, std::string_view ref, const char* hint,
                       uint32_t only_mask, Facts* facts);
  FactsOutcome LiquidityDetails(std::string_view network, std::string_view ref, Value** document);
  void Revalidate(const Revalidation& task);
  void RevalidateSnapshots(const Revalidation* const* tasks, size_t count);
  void RevalidateMarkets(const Revalidation* const* tasks, size_t count);
 private:
  Value* HolderTrends(const networks::TokenNetwork& network, const networks::TokenRef& ref,
                      const Value* holders, const FacetResult* selected);
  Work& work_;
};
}  // namespace fortis::tokens::token_facts
