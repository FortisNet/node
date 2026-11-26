#include "price_history_service.h"
#include "history_query_service.h"
#include "ownership.h"

#include "price_history_view.h"
#include "price_id_parser.h"
#include "price_series_store.h"
#include "provider_execution_coordinator.h"
#include "threading.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

struct PriceHistoryService::Impl {
  Impl(const config::ProviderRuntimeConfig& config, fortis::storage::KvStore& kv,
       providers::ProviderExecutionCoordinator& coordinator, const PriceHistoryRouteSeams* seams)
      : series(kv), repository(series), backfill(series, config, &coordinator), ohlc(config, &coordinator), query(repository, backfill, seams) {
    route.series = &series;
    route.owner = &backfill;
    route.ohlc_owner = &ohlc;
    route.kv = &kv;
    route.attestation_required = config.attestation_required;
    route.seams = seams;
  }
  // Declaration order is construction order: the backfill owner borrows the store.
  fortis::prices::PriceSeriesStore series;
  StoredPriceSeriesRepository repository;
  PriceHistoryBackfillOwner backfill;
  OhlcProviderOwner ohlc;
  HistoryQueryService query;
  PriceHistoryRouteContext route{};
  bool driver_started = false;

  // The batch driver submits to the backfill owner, so it stops first and
  // answers its waiting batches; each owner then answers its own pending
  // requests and releases its coordinator lease. Every step is idempotent.
  void Stop() {
    if (driver_started) {
      StopPriceHistoryBatchDriver();
      driver_started = false;
    }
    ohlc.Stop();
    backfill.Stop();
  }
};

void PriceHistoryService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

PriceHistoryService::~PriceHistoryService() {
  if (!impl_) return;
  impl_->Stop();
  impl_.reset();
}

PriceHistoryService::PriceHistoryService(Impl* impl) : impl_(impl) {}
PriceHistoryService::PriceHistoryService(PriceHistoryService&&) noexcept = default;

base::Result<PriceHistoryService> PriceHistoryService::Create(
    const config::ProviderRuntimeConfig& config, PriceHistoryServiceDeps deps, const PriceHistoryServiceOptions& options) try {
  auto& kv = deps.kv;
  auto& coordinator = deps.coordinator;
  auto* start_owners = options.start_owners;
  auto* seams = options.seams;
  using fortis::base::Status;
  using fortis::base::StatusCode;
  if (!coordinator.valid()) return Status(StatusCode::kFailedPrecondition, "composition");
  Impl* impl = fortis::base::New<Impl>(config, kv, coordinator, seams);
  if (!impl) return Status(StatusCode::kResourceExhausted, "composition allocation");
  PriceHistoryService service(impl);
  Status status;
  if (start_owners) {
    if (!start_owners->Start(impl->backfill, impl->ohlc)) status = Status(StatusCode::kUnavailable, "test owners");
  } else if (!impl->backfill.Start()) {
    status = Status(StatusCode::kUnavailable, "backfill owner");
  } else if (!impl->ohlc.Start()) {
    status = Status(StatusCode::kUnavailable, "OHLC owner");
  }
  if (status.ok()) {
    if (StartPriceHistoryBatchDriver(&impl->route)) impl->driver_started = true;
    else status = Status(StatusCode::kUnavailable, "batch driver");
  }
  if (status.ok()) {
    return service;
  }
  // Stop whatever started; an owner whose Start failed has already
  // released any coordinator lease it took.
  return status;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "history allocation");
}

bool PriceHistoryService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (impl_->route.handoff) return false;
  impl_->route.handoff = &handoff;
  return RegisterPriceHistoryRoutes(router, &impl_->route);
}

void PriceHistoryService::Stop() {
  impl_->Stop();
}

bool PriceHistoryService::BackgroundHistory(const char* id, int64_t from, int64_t to, bool hourly) {
  return impl_->query.BackgroundHistory(id, from, to, hourly);
}
PriceHistorySeriesStatus PriceHistoryService::ReadSeries(const PriceHistorySeriesQuery& query, PriceHistorySeriesResult* result) {
  return impl_->query.ReadSeries(query, result);
}
size_t PriceHistoryService::SeriesCapacity(int64_t from, int64_t to, bool hourly) {
  return HistoryQueryService::SeriesCapacity(from, to, hourly);
}
}  // namespace fortis::prices
