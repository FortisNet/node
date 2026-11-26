#pragma once
#include "token_facts.h"
namespace fortis::providers { struct TokenFetch; }
namespace fortis::tokens::token_facts {
namespace internal { struct FacetInputs; }
enum class FacetFetchPlan : uint8_t { kRequest, kValue, kFailed, kSnapshot };
// One resolved facet context throughout preparation, interpretation and failure
// recording. The work/inputs and their network/ref must outlive this view.
class TokenFacetFetcher {
 public:
  TokenFacetFetcher(Work& work, const internal::FacetInputs& inputs) : work_(work), inputs_(inputs) {}
  FacetFetchPlan Prepare(Facet facet, providers::TokenFetch* fetch, Value** value, bool* lane_busy);
  bool Interpret(Facet facet, const providers::TokenFetch& fetch, Value** value);
  bool LiquiditySuccess(Value* pools, Value** value);
  void RecordFailure(Facet facet);
 private:
  FacetFetchPlan Issuer(Facet facet, Value** value, bool* lane_busy);
  Work& work_;
  const internal::FacetInputs& inputs_;
};
}  // namespace fortis::tokens::token_facts
