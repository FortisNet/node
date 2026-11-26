#pragma once

#include "http_server.h"
#include "spot_provider_owner.h"

namespace fortis::prices {

struct PriceSpotRouteContext {
  SpotProviderOwner* owner = nullptr;
  http::HttpResponseHandoff* handoff = nullptr;
  // NativeConfig::attestation_required, applied by the shared device policy.
  bool attestation_required = false;
  fortis::prices::PriceSpotCache* fast_cache = nullptr;
  bool shared_price_auth = false;
  base::ErrorReporter* errors = nullptr;
};

// Registers GET /v1/prices (HEAD uses the router fallback). The owner must be
// running and the handoff started before dispatch begins. Stop the owner
// before the handoff: pending requests complete through the handoff.
bool RegisterPriceSpotRoute(http::HttpRouter& router, PriceSpotRouteContext* context);

#ifdef FORTIS_SPOT_ROUTE_CONCURRENCY_TEST_ONLY
// Test-only: shortens the handoff reservation and owner answer deadlines
// (production: 30 s and 29 s). Read only by the handler: call while no
// handle_prices runs; requests already in the owner keep their deadlines.
void SetPriceSpotRouteDeadlinesForTest(uint32_t reservation_ms, uint32_t owner_answer_ms);
// Test-only: the number of RequestSlots in use; the first `capacity` of
// their reference counts are written to `references` in table order.
size_t PriceSpotRouteSlotsForTest(unsigned* references, size_t capacity);
#endif

}  // namespace fortis::prices
