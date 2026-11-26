#pragma once

#include "clock.h"
#include "http_server.h"
#include "price_series_store.h"
#include "price_history_backfill_owner.h"
#include "ohlc_provider_owner.h"
#include "kv_store.h"
#include "seams.h"

namespace fortis::prices {

// Optional clock, jitter and read-failure substitutions. A normal composition
// leaves every member null and the routes use the wall clock, CSPRNG jitter
// and real store reads. Only a test binary's main supplies them.
struct PriceHistoryRouteSeams {
  // The OHLC cache clock (NowMs in epoch milliseconds).
  fortis::base::Clock* ohlc_clock = nullptr;
  // The OHLC freshness jitter sample in [0, 1]; false draws from the CSPRNG.
  fortis::base::JitterSource* ohlc_jitter = nullptr;
  // True makes the batch route treat this source ID's series reads as failed.
  bool (*fail_source_read)(void* context, const char* source_id) = nullptr;
  void* context = nullptr;
};

struct PriceHistoryRouteContext {
  fortis::prices::PriceSeriesStore* series = nullptr;
  PriceHistoryBackfillOwner* owner = nullptr;
  OhlcProviderOwner* ohlc_owner = nullptr;
  fortis::storage::KvStore* kv = nullptr;
  http::HttpResponseHandoff* handoff = nullptr;
  // NativeConfig::attestation_required, applied by the shared device policy.
  bool attestation_required = false;
  const PriceHistoryRouteSeams* seams = nullptr;
};

// The POST /v1/prices/history driver thread (price_history_batch_route.cpp).
// Start it once both owners run; it is idle until a batch is deferred. Stop
// answers waiting batches through the handoff and joins the thread; it must
// run before the backfill owner stops, and is idempotent.
bool StartPriceHistoryBatchDriver(PriceHistoryRouteContext* context);
void StopPriceHistoryBatchDriver();

// Registers GET /v1/prices/history, GET /v1/prices/ohlc and the batch
// POST /v1/prices/history (HEAD uses the router fallback). The batch driver
// must be running and context->handoff set. On false the router may hold
// some of the three routes, so the caller must not serve with it.
bool RegisterPriceHistoryRoutes(http::HttpRouter& router, PriceHistoryRouteContext* context);
// The POST route alone; RegisterPriceHistoryRoutes calls it.
bool RegisterPriceHistoryBatchRoute(http::HttpRouter& router, PriceHistoryRouteContext* context);

}  // namespace fortis::prices
