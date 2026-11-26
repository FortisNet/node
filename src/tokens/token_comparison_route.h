#pragma once

#include "clock.h"
#include "http_server.h"

#include <stdint.h>

namespace fortis::storage { class KvStore; }
namespace fortis::tokens::token_jobs { class TokenStats; }

namespace fortis::tokens {

// POST /v1/tokens/compare (R53). The full contract (hook order,
// validation, statuses, headers, differences) is the comment above
// handle_compare in token_comparison_route.cpp. The oracle is the earlier
// TypeScript implementation's comparison route, frozen in
// tests/corpus/token_comparison_route.json and
// token_comparison_edges.json. One bounded read of
// the native TokenComparisonStore and one pure projection: it never calls a
// provider. Memory is one work area allocated when the route is first
// registered and kept for the process lifetime (an image that never
// registers the route pays nothing), serialized by one route mutex: the JSON
// token arena and string scratch, records and keys, and two 192 KiB
// response/validator buffers (about 1 MiB in all). A response over 192 KiB
// (only possible with control-character-heavy stored strings) answers 500.
struct TokenComparisonRouteContext {
  fortis::storage::KvStore* kv = nullptr;
  // NativeConfig::attestation_required, applied by the shared device policy.
  bool attestation_required = false;
  // The comparison clock in epoch ms (computedAt and freshness). Null uses
  // the wall clock; only a test main fills it. A negative value also falls
  // back to the wall clock.
  fortis::base::Clock* clock = nullptr;
  // TokenFetchComparisonDemandObserver: one foreground market
  // sample per compared token feeds the demand projection. Null records none.
  fortis::tokens::token_jobs::TokenStats* stats = nullptr;
};

// Registers POST /v1/tokens/compare. `context` must outlive the router.
bool RegisterTokenComparisonRoute(http::HttpRouter& router, TokenComparisonRouteContext* context);

}  // namespace fortis::tokens
