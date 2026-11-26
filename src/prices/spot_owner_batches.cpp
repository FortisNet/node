#include <memory>
// The spot provider owner's shared flights and provider batches: scheduling
// the batches of each lane, and completing flights and their waiters from a
// provider answer.
#include "spot_provider_owner_internal.h"
#include "threading.h"

namespace fortis::prices {

size_t SpotBatchScheduler::ActiveBackgroundBatches() const {

  size_t active = 0;
  for (const SharedBatch& batch : shared_batches) active += batch.used && batch.background;
  return active;
}

void SpotBatchScheduler::ResetBatch(SharedBatch& batch) {

  batch.used = false;
  batch.background = false;
  batch.tronscan_anonymous_retry = false;
  batch.kraken_fallback = false;
  batch.defillama_fallback = false;
  batch.mexc_fallback = false;
  batch.usdt_usd = 0;
  batch.fx_observed_at_ms = 0;
  batch.robinhood = false;
  batch.robinhood_catalog_request = false;
  batch.robinhood_group_index = UINT16_MAX;
  batch.robinhood_group_generation = 0;
  batch.robinhood_position = 0;
  batch.kind = providers::PriceIdKind::kInvalid;
  batch.job_id = 0;
  batch.flight_count = 0;
  batch.group = kNoGroup;
}

void SpotBatchScheduler::ScheduleSharedBatches() {
  auto& mutex = runtime_.mutex;
  auto& stop_requested = runtime_.stop_requested;
  auto& config = runtime_.config;
  auto& scratch_request = runtime_.scratch_request;
  auto& redirect = runtime_.redirect;
  auto& slots = runtime_.requests.slots;
  auto& flights = runtime_.flight_table.flights;
  auto& groups = runtime_.jupiter.groups;
  auto& robinhood_catalog = runtime_.robinhood.robinhood_catalog;
  auto& robinhood_catalog_until_ms = runtime_.robinhood.robinhood_catalog_until_ms;
  auto& robinhood_shared_mutex = runtime_.robinhood.robinhood_shared_mutex;
  auto& robinhood_shared = runtime_.robinhood.robinhood_shared;
  auto& robinhood_shared_scratch = runtime_.robinhood.robinhood_shared_scratch;
  auto& robinhood_groups = runtime_.robinhood.robinhood_groups;
  fortis::base::UniqueLock lock(mutex);
  const bool stopping = stop_requested;
  lock.Unlock();
  if (stopping) return;
  const uint64_t now = runtime_.MonotonicMs();
  for (size_t batch_index = 0; batch_index < kSpotProviderOwnerMaxRequests;
       ++batch_index) {
    SharedBatch& shared = shared_batches[batch_index];
    if (shared.used) continue;
    // A group between pages, or retrying a malformed page, keeps its place
    // ahead of new cohorts so the requests already waiting on it complete.
    size_t due_group = kNoGroup;
    for (size_t g = 0; g < kMaxJupiterGroups; ++g) {
      const JupiterGroup& group = groups[g];
      if (!group.used || group.active_batch != kNoBatch || group.retry_at_ms > now) continue;
      if (group.background && ActiveBackgroundBatches() >= kMaxBackgroundBatchesInFlight)
        continue;
      due_group = g;
      break;
    }
    if (due_group != kNoGroup) {
      if (runtime_.jupiter.SubmitGroupPage(due_group, batch_index, now) == PageSubmit::kBusy) return;
      continue;
    }
    bool active_robinhood = false;
    for (const SharedBatch& pending : shared_batches)
      active_robinhood |= pending.used && pending.robinhood;
    for (const RobinhoodGroup& pending : robinhood_groups)
      active_robinhood |= pending.used;
    size_t first = kMaxSharedFlights;
    // Cold foreground misses always take an available adapter slot before
    // stale refresh work, even when the stale window matured first.
    for (size_t f = 0; f < kMaxSharedFlights; ++f)
      if (flights[f].state == FlightState::kWindow &&
          !(active_robinhood && flights[f].kind == providers::PriceIdKind::kRobinhood) &&
          flights[f].window_deadline_ms <= now &&
          (flights[f].foreground_waiters || flights[f].stale_refresh)) {
        first = f;
        break;
      }
    if (first == kMaxSharedFlights) {
      size_t active_background = 0;
      for (const SharedBatch& active : shared_batches)
        active_background += active.used && active.background;
      if (active_background >= kMaxBackgroundBatchesInFlight) return;
      for (size_t f = 0; f < kMaxSharedFlights; ++f)
        if (flights[f].state == FlightState::kWindow &&
            !(active_robinhood && flights[f].kind == providers::PriceIdKind::kRobinhood) &&
            flights[f].window_deadline_ms <= now && flights[f].background) {
          first = f;
          break;
        }
    }
    if (first == kMaxSharedFlights) return;
    const char* ids[providers::kCoinGeckoSpotMaxBatchCount]{};
    size_t selected[providers::kCoinGeckoSpotMaxBatchCount]{};
    size_t count = 0;
    const char* fiat = flights[first].fiat.data();
    const providers::PriceIdKind kind = flights[first].kind;
    const uint64_t cohort_deadline = flights[first].window_deadline_ms;
    // A cohort is one fiat, one provider kind, one collection window.
    const size_t cohort_limit = flights[first].defillama_fallback ? providers::kDefiLlamaMaxBatch : (flights[first].kraken_fallback || flights[first].mexc_fallback) ? 1 : kind == providers::PriceIdKind::kSpl ? kJupiterGroupMaxMints :
        kind == providers::PriceIdKind::kRobinhood ? 50 :
        kind == providers::PriceIdKind::kContract && !config.coingecko_api_key_configured &&
            anonymous_contract_singletons ? 1 : providers::kCoinGeckoSpotMaxBatchCount;
    for (size_t f = first; f < kMaxSharedFlights && count < cohort_limit; ++f) {
      if (flights[f].state != FlightState::kWindow ||
          flights[f].window_deadline_ms != cohort_deadline ||
          flights[f].kraken_fallback != flights[first].kraken_fallback ||
          flights[f].defillama_fallback != flights[first].defillama_fallback ||
          flights[f].mexc_fallback != flights[first].mexc_fallback ||
          flights[f].kind != kind || strcmp(flights[f].fiat.data(), fiat) != 0 ||
          strcmp(flights[f].platform.data(), flights[first].platform.data()) != 0)
        continue;
      ids[count] = flights[f].id.data();
      selected[count++] = f;
    }
    if (flights[first].mexc_fallback) {
      if (!providers::BuildMexcPriceRequest(config, ids[0], shared.path, sizeof(shared.path), &scratch_request)) {
        runtime_.flight_table.CompleteFlightFailure(first, false);
        continue;
      }
      shared.mexc_fallback = true;
      shared.usdt_usd = flights[first].usdt_usd;
      shared.fx_observed_at_ms = flights[first].fx_observed_at_ms;
      shared.flight_count = 1;
      shared.flights[0] = static_cast<uint16_t>(first);
    } else if (flights[first].defillama_fallback) {
      if (!providers::BuildDefiLlamaPriceRequest(config, ids, count, shared.path, sizeof(shared.path), &scratch_request)) {
        for (size_t i = 0; i < count; ++i) runtime_.flight_table.CompleteFlightFailure(selected[i], false);
        continue;
      }
      shared.defillama_fallback = true;
      shared.flight_count = count;
      for (size_t i = 0; i < count; ++i) shared.flights[i] = static_cast<uint16_t>(selected[i]);
    } else if (flights[first].kraken_fallback) {
      if (!providers::BuildKrakenPriceRequest(config, ids[0], shared.path,
              sizeof(shared.path), &scratch_request)) {
        runtime_.flight_table.CompleteFlightFailure(first, false);
        continue;
      }
      shared.kraken_fallback = true;
      shared.flight_count = 1;
      shared.flights[0] = static_cast<uint16_t>(first);
    } else if (kind == providers::PriceIdKind::kSpl) {
      // The cohort becomes one commit group; its 50-mint pages are
      // requested in order and nothing is served or written until the
      // last page parses. Without a free group the cohort waits.
      const size_t g = runtime_.jupiter.FreeGroupIndex();
      if (g == kNoGroup) return;
      JupiterGroup& group = groups[g];
      uint32_t group_generation = group.generation + 1;
      if (!group_generation) ++group_generation;
      std::destroy_at(&group);
      std::construct_at(&group);
      group.generation = group_generation;
      group.used = true;
      group.active_batch = kNoBatch;
      memcpy(group.fiat, fiat, strlen(fiat) + 1);
      group.flight_count = count;
      for (size_t i = 0; i < count; ++i) {
        group.flights[i] = static_cast<uint16_t>(selected[i]);
        flights[selected[i]].state = FlightState::kInFlight;
      }
      if (runtime_.jupiter.SubmitGroupPage(g, batch_index, now) == PageSubmit::kBusy) return;
      continue;
    } else if (kind == providers::PriceIdKind::kRobinhood) {
      const size_t group_index = runtime_.robinhood.FreeRobinhoodGroupIndex();
      if (group_index == kNoGroup) return;
      RobinhoodGroup& group = robinhood_groups[group_index];
      uint32_t group_generation = group.generation + 1;
      if (!group_generation) ++group_generation;
      std::destroy_at(&group);
      std::construct_at(&group);
      group.used = true; group.generation = group_generation; group.flight_count = count;
      shared.robinhood = true;
      shared.robinhood_group_index = static_cast<uint16_t>(group_index);
      shared.robinhood_group_generation = group_generation;
      for (size_t i = 0; i < count; ++i) {
        group.flights[i] = static_cast<uint16_t>(selected[i]);
        shared.flights[i] = static_cast<uint16_t>(selected[i]);
        flights[selected[i]].state = FlightState::kInFlight;
      }
      shared.flight_count = count;
    } else if (kind == providers::PriceIdKind::kContract) {
      const char* source_ids[providers::kCoinGeckoSpotMaxBatchCount]{};
      for (size_t i = 0; i < count; ++i) source_ids[i] = flights[selected[i]].id.data();
      if (providers::BuildCoinGeckoTokenPriceRequest(config, flights[first].platform.data(),
              source_ids, count, fiat, shared.path, sizeof(shared.path),
              &scratch_request) != providers::CoinGeckoSpotRequestError::kNone) {
        for (size_t i = 0; i < count; ++i) runtime_.flight_table.CompleteFlightFailure(selected[i], false);
        continue;
      }
      shared.flight_count = count;
      for (size_t i = 0; i < count; ++i)
        shared.flights[i] = static_cast<uint16_t>(selected[i]);
    } else if (kind == providers::PriceIdKind::kAsa) {
      const char* source_ids[providers::kVestigePriceMaxIds]{};
      for (size_t i = 0; i < count; ++i) source_ids[i] = flights[selected[i]].id.data();
      if (providers::BuildVestigePriceRequest(config, source_ids, count, shared.path,
              sizeof(shared.path), &scratch_request) != providers::VestigePriceRequestError::kNone) {
        for (size_t i = 0; i < count; ++i) runtime_.flight_table.CompleteFlightFailure(selected[i], false);
        continue;
      }
      shared.flight_count = count;
      for (size_t i = 0; i < count; ++i)
        shared.flights[i] = static_cast<uint16_t>(selected[i]);
    } else if (kind == providers::PriceIdKind::kTrc10) {
      const char* source_ids[providers::kTronScanPriceMaxIds]{};
      for (size_t i = 0; i < count; ++i) source_ids[i] = flights[selected[i]].id.data();
      if (providers::BuildTronScanPriceRequest(config, source_ids, count, false,
              shared.path, sizeof(shared.path), &scratch_request) !=
              providers::TronScanPriceRequestError::kNone) {
        for (size_t i = 0; i < count; ++i) runtime_.flight_table.CompleteFlightFailure(selected[i], false);
        continue;
      }
      shared.flight_count = count;
      for (size_t i = 0; i < count; ++i)
        shared.flights[i] = static_cast<uint16_t>(selected[i]);
    } else {
      providers::CoinGeckoSpotBatchPlan plan{};
      if (BuildCoinGeckoSpotBatchPlan(config, ids, count, fiat, &plan) !=
              providers::CoinGeckoSpotPlanError::kNone || !plan.batch_count) {
        runtime_.flight_table.CompleteFlightFailure(first, true);
        continue;
      }
      const providers::CoinGeckoSpotBatch& planned = plan.batches[0];
      const char* batch_ids[providers::kCoinGeckoSpotMaxBatchCount]{};
      for (size_t i = 0; i < planned.id_count; ++i)
        batch_ids[i] = plan.ids[planned.first_id + i];
      const auto built = providers::BuildCoinGeckoSpotRequest(config, batch_ids,
          planned.id_count, fiat, shared.path, sizeof(shared.path),
          &scratch_request);
      if (built != providers::CoinGeckoSpotRequestError::kNone) {
        runtime_.flight_table.CompleteFlightFailure(first, false);
        continue;
      }
      shared.flight_count = planned.id_count;
      for (size_t i = 0; i < planned.id_count; ++i) {
        const char* selected_id = plan.ids[planned.first_id + i];
        size_t match = count;
        for (size_t j = 0; j < count; ++j)
          if (strcmp(flights[selected[j]].id.data(), selected_id) == 0) { match = j; break; }
        if (match == count) { shared.flight_count = 0; break; }
        shared.flights[i] = static_cast<uint16_t>(selected[match]);
      }
    }
    if (!shared.kraken_fallback && !shared.defillama_fallback && !shared.mexc_fallback) scratch_request.timeout_ms = kAttemptTimeoutMs;
    redirect.Apply(scratch_request);
    if (!shared.flight_count) {
      runtime_.flight_table.CompleteFlightFailure(first, true);
      continue;
    }
    if (kind == providers::PriceIdKind::kRobinhood) {
      const size_t selected_group = shared.robinhood_group_index;
      if (selected_group == kNoGroup) { for(size_t i=0;i<count;++i) runtime_.flight_table.CompleteFlightFailure(selected[i],true); continue; }
      // A shared catalog decides freshness on its own clock; the owner's
      // copy is used only without one.
      fortis::base::UniqueLock robinhood_shared_lock(robinhood_shared_mutex);
      const bool shared_catalog = robinhood_shared != nullptr;
      const bool shared_fresh =
          shared_catalog && robinhood_shared->fresh(robinhood_shared->context, &robinhood_shared_scratch);
      robinhood_shared_lock.Unlock();
      if (shared_fresh) robinhood_catalog = robinhood_shared_scratch;
      if (shared_catalog ? shared_fresh : robinhood_catalog_until_ms > runtime_.wall_ms()) {
        robinhood_groups[selected_group].catalog_complete = true;
        ResetBatch(shared);
        runtime_.robinhood.AdvanceRobinhoodGroup(selected_group);
      } else {
        runtime_.robinhood.SubmitRobinhoodQuote(selected_group, 0, batch_index, "/rhj/assets", true);
      }
      continue;
    }
    providers::ProviderExecutionOptions options{};
    options.max_attempts = (shared.kraken_fallback || shared.defillama_fallback || shared.mexc_fallback) ? 1 : 3;
    options.deadline_ms = 0;
    bool background_only = true;
    bool stale_refresh = false;
    for (size_t i = 0; i < shared.flight_count; ++i) {
      const SharedFlight& flight = flights[shared.flights[i]];
      background_only &= flight.background && flight.foreground_waiters == 0 && !flight.stale_refresh;
      stale_refresh |= flight.stale_refresh;
      for (size_t w = 0; w < flight.waiter_count; ++w) {
        const FlightWaiter& waiter = flight.waiters[w];
        if (waiter.slot_index < kSpotProviderOwnerMaxRequests &&
            slots[waiter.slot_index].generation == waiter.generation &&
            slots[waiter.slot_index].deadline_ms > options.deadline_ms)
          options.deadline_ms = slots[waiter.slot_index].deadline_ms;
      }
    }
    options.background = background_only;
    if (background_only)
      options.deadline_ms = now + kAttemptTimeoutMs * 3ULL;
    // A stale refresh has no waiter deadline of its own.
    else if (!options.deadline_ms && stale_refresh)
      options.deadline_ms = now + kAttemptTimeoutMs * 3ULL;
    if (!options.deadline_ms) {
      for (size_t i = 0; i < shared.flight_count; ++i)
        runtime_.flight_table.CompleteFlightFailure(shared.flights[i], true);
      shared.flight_count = 0;
      continue;
    }
    shared.used = true;
    shared.background = background_only;
    shared.kind = kind;
    uint32_t generation = shared.generation + 1;
    if (!generation) ++generation;
    shared.generation = generation;
    const auto submitted = runtime_.ProviderSubmit(shared.mexc_fallback ? "mexc" : shared.defillama_fallback ? "defillama" : shared.kraken_fallback ? "kraken" : lane_for_kind(kind), scratch_request,
                                          &shared, options);
    if (submitted.status != providers::ProviderExecutionSubmitStatus::kAccepted) {
      const size_t rejected_count = shared.flight_count;
      uint16_t rejected[providers::kCoinGeckoSpotMaxBatchCount];
      memcpy(rejected, shared.flights, sizeof(rejected));
      ResetBatch(shared);
      shared.flight_count = rejected_count;
      memcpy(shared.flights, rejected, sizeof(rejected));
      if (submitted.status == providers::ProviderExecutionSubmitStatus::kBusy) {
        // In shared mode another owner's jobs may occupy all coordinator
        // slots. Keep these flights queued and retry after a bounded delay;
        // leaving their window deadline in the past would make Wait spin.
        const uint64_t retry_at = runtime_.MonotonicMs() + kWaitSliceMs;
        for (size_t i = 0; i < shared.flight_count; ++i)
          flights[shared.flights[i]].window_deadline_ms = retry_at;
        return;
      }
      for (size_t i = 0; i < shared.flight_count; ++i)
        if (!runtime_.flight_table.TryPriceFallback(shared.flights[i]))
          runtime_.flight_table.CompleteFlightFailure(shared.flights[i], true);
      continue;
    }
    shared.job_id = submitted.job_id;
    for (size_t i = 0; i < shared.flight_count; ++i)
      flights[shared.flights[i]].state = FlightState::kInFlight;
  }
}

void FlightTable::CompleteFlight(size_t flight_index, bool success,
                                             const providers::CoinGeckoSpotResult* extracted,
                                             int64_t answered_at_ms) {
  auto& slots = runtime_.requests.slots;
  if (flight_index >= kMaxSharedFlights) return;
  SharedFlight& flight = flights[flight_index];
  if (flight.state == FlightState::kFree) return;
  const uint32_t generation = flight.generation;
  const char* id = flight.id.data();
  for (size_t w = 0; w < flight.waiter_count; ++w) {
    const FlightWaiter waiter = flight.waiters[w];
    if (waiter.slot_index >= kSpotProviderOwnerMaxRequests) continue;
    Slot& slot = slots[waiter.slot_index];
    if (slot.generation != waiter.generation || slot.state == SlotState::kFree ||
        waiter.fetch_index >= slot.plan.id_count) continue;
    if (success) {
      providers::CoinGeckoSpotResult one{};
      one.error = providers::CoinGeckoSpotError::kNone;
      if (extracted && extracted->error == providers::CoinGeckoSpotError::kNone) {
        for (size_t q = 0; q < extracted->count; ++q) {
          if (strcmp(extracted->quotes[q].id, id) == 0) {
            one.quotes[0] = extracted->quotes[q];
            one.count = 1;
            break;
          }
        }
      }
      RecordSpotResolutionBatchResult(&slot.accumulator, waiter.fetch_index,
                                       &one, answered_at_ms);
    } else {
      RecordSpotResolutionBatchFailure(&slot.accumulator, waiter.fetch_index, true);
    }
    if (slot.pending_flights) --slot.pending_flights;
    if (slot.callback_sent && slot.pending_flights == 0) {
      runtime_.requests.Release(waiter.slot_index);
    } else if (!slot.callback_sent && slot.pending_flights == 0) {
      runtime_.requests.CompleteRequest(waiter.slot_index);
    }
  }
  // A flight can be completed only once. Keep its generation so stale batch
  // callbacks cannot alias a subsequently reused registry entry.
  flight = SharedFlight{};
  flight.generation = generation;
}

bool FlightTable::TryPriceFallback(size_t flight_index) {
  fortis::base::UniqueLock lock(runtime_.mutex);
  const bool stopping = runtime_.stop_requested;
  lock.Unlock();
  if (stopping) return false;
  if (flight_index >= kMaxSharedFlights) return false;
  auto& flight = flights[flight_index];
  if (flight.state == FlightState::kFree || flight.mexc_fallback ||
      (!flight.waiter_count && !flight.background && !flight.stale_refresh) ||
      flight.fiat.view() != "usd") return false;
  if (flight.defillama_fallback) {
    const int64_t now = runtime_.wall_ms();
    if (!providers::MexcUsdSupported(flight.id.view()) || flight.usdt_usd <= 0 || now < 300000 ||
        flight.fx_observed_at_ms > now || flight.fx_observed_at_ms < now - 300000) return false;
    flight.defillama_fallback = false;
    flight.mexc_fallback = true;
  } else if (!flight.kraken_fallback && flight.kind == providers::PriceIdKind::kCoinGecko && providers::KrakenUsdPair(flight.id.view())) {
    flight.kraken_fallback = true;
  } else {
    char key[providers::kDefiLlamaKeyCapacity]{};
    if (!providers::DefiLlamaPriceKey(flight.id.view(), key, sizeof(key))) return false;
    flight.kraken_fallback = false;
    flight.defillama_fallback = true;
  }
  flight.state = FlightState::kWindow;
  flight.window_deadline_ms = runtime_.MonotonicMs();
  return true;
}

void FlightTable::CompleteFlightFailure(size_t flight_index, bool transport_failure) {
  auto& slots = runtime_.requests.slots;
  if (flight_index >= kMaxSharedFlights ||
      flights[flight_index].state == FlightState::kFree) return;
  SharedFlight& flight = flights[flight_index];
  const uint32_t generation = flight.generation;
  for (size_t w = 0; w < flight.waiter_count; ++w) {
    const FlightWaiter waiter = flight.waiters[w];
    if (waiter.slot_index >= kSpotProviderOwnerMaxRequests) continue;
    Slot& slot = slots[waiter.slot_index];
    if (slot.generation != waiter.generation || slot.state == SlotState::kFree ||
        waiter.fetch_index >= slot.plan.id_count) continue;
    RecordSpotResolutionBatchFailure(&slot.accumulator, waiter.fetch_index,
                                     transport_failure);
    if (slot.pending_flights) --slot.pending_flights;
    if (slot.callback_sent && slot.pending_flights == 0)
      runtime_.requests.Release(waiter.slot_index);
    else if (!slot.callback_sent && slot.pending_flights == 0)
      runtime_.requests.CompleteRequest(waiter.slot_index);
  }
  flight = SharedFlight{};
  flight.generation = generation;
}

void FlightTable::DetachSlotWaiters(size_t slot_index, uint32_t generation) {
  auto& slots = runtime_.requests.slots;
  auto& shared_batches = runtime_.batches.shared_batches;
  auto& groups = runtime_.jupiter.groups;
  auto& robinhood_groups = runtime_.robinhood.robinhood_groups;
  if (slot_index >= kSpotProviderOwnerMaxRequests) return;
  Slot& slot = slots[slot_index];
  if (slot.generation != generation) return;
  for (size_t i = 0; i < slot.plan.id_count; ++i) {
    if (!slot.flight_registered[i]) continue;
    const size_t flight_index = slot.flight_indices[i];
    if (flight_index >= kMaxSharedFlights) continue;
    SharedFlight& flight = flights[flight_index];
    for (size_t w = 0; w < flight.waiter_count; ++w) {
      const FlightWaiter& waiter = flight.waiters[w];
      if (waiter.slot_index == slot_index && waiter.generation == generation &&
          waiter.fetch_index == i) {
        for (size_t move = w + 1; move < flight.waiter_count; ++move)
          flight.waiters[move - 1] = flight.waiters[move];
        --flight.waiter_count;
        flight.waiters[flight.waiter_count] = FlightWaiter{};
        if (!slot.background && flight.foreground_waiters) --flight.foreground_waiters;
        break;
      }
    }
    if (flight.waiter_count == 0 && flight.background) {
      size_t background_only = 0;
      for (const SharedFlight& candidate : flights)
        background_only += candidate.state != FlightState::kFree &&
                           candidate.background && candidate.waiter_count == 0;
      if (background_only > kMaxBackgroundOnlyFlights)
        flight.background = false;
    }
    if (flight.state == FlightState::kWindow && flight.waiter_count == 0 &&
        !flight.background) {
      const uint32_t flight_generation = flight.generation;
      flight = SharedFlight{};
      flight.generation = flight_generation;
    }
    slot.flight_registered[i] = false;
  }
  slot.pending_flights = 0;
  for (auto& batch : shared_batches) {
    if (!batch.used) continue;
    bool has_waiter = false;
    if (batch.group != kNoGroup && batch.group < kMaxJupiterGroups &&
        groups[batch.group].used) {
      // A page is kept alive by any waiter in its group: the group's answer
      // is shared, so a request waiting on a later page still needs it.
      has_waiter = runtime_.jupiter.GroupHasWaiters(groups[batch.group]);
    } else if (batch.robinhood && batch.robinhood_group_index < kMaxRobinhoodGroups &&
               robinhood_groups[batch.robinhood_group_index].used &&
               robinhood_groups[batch.robinhood_group_index].generation == batch.robinhood_group_generation) {
      has_waiter = runtime_.robinhood.RobinhoodGroupHasWaiters(robinhood_groups[batch.robinhood_group_index]);
    } else {
      for (size_t b = 0; b < batch.flight_count; ++b) {
        const size_t flight_index = batch.flights[b];
        if (flight_index < kMaxSharedFlights &&
            (flights[flight_index].waiter_count || flights[flight_index].background))
          has_waiter = true;
      }
    }
    if (!has_waiter) runtime_.ProviderCancel(batch.job_id);
  }
}

void SpotBatchScheduler::CompleteBatch(const providers::ProviderExecutionResult& result, size_t body_length) {
  auto& config = runtime_.config;
  auto& cache = runtime_.cache;
  auto& series = runtime_.series;
  auto& json_tokens = runtime_.json_tokens;
  auto& key_index = runtime_.key_index;
  auto& response_body = runtime_.response_body;
  auto& scratch_request = runtime_.scratch_request;
  auto& redirect = runtime_.redirect;
  auto& slots = runtime_.requests.slots;
  auto& flights = runtime_.flight_table.flights;
  auto* batch = static_cast<SharedBatch*>(result.context);
  if (!batch || batch < &shared_batches[0] ||
      batch >= &shared_batches[kSpotProviderOwnerMaxRequests] ||
      !batch->used || batch->job_id != result.job_id) return;
  if (!batch->defillama_fallback && !batch->mexc_fallback && batch->kind == providers::PriceIdKind::kContract &&
      !config.coingecko_api_key_configured && batch->flight_count > 1 &&
      !result.cancelled && !result.deadline_exceeded &&
      result.response.error == http::OutboundError::kNone &&
      result.response.status == 400) {
    // Anonymous CoinGecko now limits /simple/token_price to one contract
    // address. Requeue the rejected cohort without writing negative cache
    // rows; the next scheduling pass sends each address separately.
    anonymous_contract_singletons = true;
    const size_t count = batch->flight_count;
    uint16_t retry[providers::kCoinGeckoSpotMaxBatchCount]{};
    memcpy(retry, batch->flights, count * sizeof(retry[0]));
    ResetBatch(*batch);
    const uint64_t now = runtime_.MonotonicMs();
    for (size_t i = 0; i < count; ++i) {
      SharedFlight& flight = flights[retry[i]];
      if (flight.state == FlightState::kInFlight) {
        flight.state = FlightState::kWindow;
        flight.window_deadline_ms = now;
      }
    }
    return;
  }
  const bool rejected_key = result.response.error == http::OutboundError::kNone &&
      (result.response.status == 401 || result.response.status == 403 ||
       result.response.status == 429);
  if (batch->kind == providers::PriceIdKind::kTrc10 && config.tronscan_api_key_configured &&
      !batch->tronscan_anonymous_retry && rejected_key) {
    const size_t flight_count = batch->flight_count;
    uint16_t flight_indices[providers::kCoinGeckoSpotMaxBatchCount]{};
    memcpy(flight_indices, batch->flights, sizeof(flight_indices));
    const char* source_ids[providers::kTronScanPriceMaxIds]{};
    for (size_t i = 0; i < flight_count; ++i)
      source_ids[i] = flights[flight_indices[i]].id.data();
    if (providers::BuildTronScanPriceRequest(config, source_ids, flight_count, true,
            batch->path, sizeof(batch->path), &scratch_request) ==
            providers::TronScanPriceRequestError::kNone) {
      scratch_request.timeout_ms = kAttemptTimeoutMs;
      redirect.Apply(scratch_request);
      providers::ProviderExecutionOptions retry_options{};
      retry_options.max_attempts = 3;
      retry_options.deadline_ms = 0;
      bool background_only = true;
      for (size_t i = 0; i < flight_count; ++i) {
        const SharedFlight& flight = flights[flight_indices[i]];
        background_only &= flight.background && flight.foreground_waiters == 0 && !flight.stale_refresh;
        for (size_t w = 0; w < flight.waiter_count; ++w) {
          const FlightWaiter& waiter = flight.waiters[w];
          if (waiter.slot_index < kSpotProviderOwnerMaxRequests &&
              slots[waiter.slot_index].generation == waiter.generation &&
              slots[waiter.slot_index].deadline_ms > retry_options.deadline_ms)
            retry_options.deadline_ms = slots[waiter.slot_index].deadline_ms;
        }
      }
      retry_options.background = background_only;
      if (!retry_options.deadline_ms)
        retry_options.deadline_ms = runtime_.MonotonicMs() + kAttemptTimeoutMs * 3ULL;
      batch->tronscan_anonymous_retry = true;
      const providers::ProviderExecutionSubmission retry = runtime_.ProviderSubmit(
          "tronscan", scratch_request, batch, retry_options);
      if (retry.status == providers::ProviderExecutionSubmitStatus::kAccepted) {
        batch->job_id = retry.job_id;
        return;
      }
    }
    ResetBatch(*batch);
    for (size_t i = 0; i < flight_count; ++i)
      runtime_.flight_table.CompleteFlightFailure(flight_indices[i], true);
    return;
  }
  const bool transport_ok = !result.cancelled && !result.deadline_exceeded &&
      result.response.error == http::OutboundError::kNone &&
      result.response.status == 200 && body_length <= sizeof(response_body);
  if (batch->robinhood) {
    runtime_.robinhood.CompleteRobinhoodBatch(*batch, body_length, transport_ok);
    return;
  }
  if (batch->kind == providers::PriceIdKind::kSpl) {
    runtime_.jupiter.CompleteGroupPage(static_cast<size_t>(batch - &shared_batches[0]), result,
                      body_length, transport_ok);
    return;
  }
  const char* provider = batch->mexc_fallback ? "mexc-defillama" : batch->defillama_fallback ? "defillama" : batch->kraken_fallback ? "kraken" : SpotResolutionProviderName(batch->kind);
  const char* requested[providers::kCoinGeckoSpotMaxBatchCount]{};
  for (size_t i = 0; i < batch->flight_count; ++i)
    requested[i] = flights[batch->flights[i]].id.data();
  // Construct the bounded result directly in its final slot. Assigning four
  // branch-specific return temporaries reserved 574,752 bytes under GCC/LTO.
  const auto extracted = [&]() -> providers::CoinGeckoSpotResult {
    if (!transport_ok) return providers::CoinGeckoSpotResult();
    if (batch->mexc_fallback)
      return providers::ExtractMexcPrice(response_body, body_length, requested[0], runtime_.wall_ms(),
          batch->usdt_usd, batch->fx_observed_at_ms, json_tokens, codec::kBoundedJsonMaxTokens);
    if (batch->defillama_fallback)
      return providers::ExtractDefiLlamaPrices(response_body, body_length, requested, batch->flight_count,
          runtime_.wall_ms(), json_tokens, codec::kBoundedJsonMaxTokens);
    if (batch->kraken_fallback)
      return providers::ExtractKrakenPrice(response_body, body_length, requested[0],
          runtime_.wall_ms(), json_tokens, codec::kBoundedJsonMaxTokens);
    if (batch->kind == providers::PriceIdKind::kContract)
      return providers::ExtractCoinGeckoTokenPrice(response_body, body_length,
          requested, batch->flight_count, flights[batch->flights[0]].fiat.data(),
          json_tokens, codec::kBoundedJsonMaxTokens, key_index, codec::kBoundedJsonMaxTokens);
    if (batch->kind == providers::PriceIdKind::kAsa)
      return providers::ExtractVestigePrices(response_body, body_length, requested,
          batch->flight_count, json_tokens, codec::kBoundedJsonMaxTokens);
    if (batch->kind == providers::PriceIdKind::kTrc10)
      return providers::ExtractTronScanTrc10Prices(response_body, body_length, requested,
          batch->flight_count, json_tokens, codec::kBoundedJsonMaxTokens);
    return providers::ExtractCoinGeckoSimplePrice(response_body, body_length,
        requested, batch->flight_count, flights[batch->flights[0]].fiat.data(),
        json_tokens, codec::kBoundedJsonMaxTokens, key_index, codec::kBoundedJsonMaxTokens);
  }();
  const bool parse_ok = transport_ok && extracted.error == providers::CoinGeckoSpotError::kNone &&
      provider != nullptr;
  const int64_t answered_at = runtime_.wall_ms();
  if (parse_ok && batch->defillama_fallback) {
    double rate = 0;
    int64_t observed = 0;
    if (providers::ExtractDefiLlamaTetherUsd(response_body, body_length, answered_at, json_tokens,
          codec::kBoundedJsonMaxTokens, &rate, &observed)) {
      for (size_t i = 0; i < batch->flight_count; ++i) {
        auto& flight = flights[batch->flights[i]];
        if (providers::MexcUsdSupported(flight.id.view())) {
          flight.usdt_usd = rate;
          flight.fx_observed_at_ms = observed;
        }
      }
    }
  }
  if (parse_ok) {
    fortis::prices::PriceSpotQuoteInput quotes[providers::kCoinGeckoSpotMaxBatchCount]{};
    const char* unknown[providers::kCoinGeckoSpotMaxBatchCount]{};
    size_t quote_count = 0;
    size_t unknown_count = 0;
    for (size_t i = 0; i < batch->flight_count; ++i) {
      const char* id = flights[batch->flights[i]].id.data();
      size_t q = 0;
      while (q < extracted.count && strcmp(extracted.quotes[q].id, id) != 0) ++q;
      if (q == extracted.count) {
        char key[providers::kDefiLlamaKeyCapacity]{};
        if (!batch->defillama_fallback && !batch->kraken_fallback && !batch->mexc_fallback &&
            (strcmp(flights[batch->flights[i]].fiat.data(), "usd") != 0 ||
             !providers::DefiLlamaPriceKey(id, key, sizeof(key)))) unknown[unknown_count++] = id;
        continue;
      }
      const providers::CoinGeckoSpotQuote& quote = extracted.quotes[q];
      quotes[quote_count++] = {id, quote.price, quote.change24hPct,
          quote.volume24h, quote.hasChange24hPct, quote.hasVolume24h,
          answered_at, 0, quote.provider_observed_at_ms};
    }
    const char* fiat = flights[batch->flights[0]].fiat.data();
    // Commit positive/negative cache rows and positive series rows in one
    // frame. Preparation or commit failure leaves both planes unchanged;
    // the parsed provider reply still completes normally.
    fortis::prices::PriceSeriesStoreStatus series_status =
        fortis::prices::PriceSeriesStoreStatus::kWriteFailure;
    const auto cache_status = cache.StoreProviderBatchWithSeries(series,
        fiat, provider, quotes, quote_count, unknown, unknown_count,
        true, answered_at, &series_status);
    const bool cache_failed = cache_status != fortis::prices::PriceSpotStoreStatus::kStored &&
        cache_status != fortis::prices::PriceSpotStoreStatus::kNoItems;
    const bool series_failed = series_status != fortis::prices::PriceSeriesStoreStatus::kStored &&
        series_status != fortis::prices::PriceSeriesStoreStatus::kNoItems;
    runtime_.RecordStorageFailures(cache_failed, series_failed);
  }
  for (size_t i = 0; i < batch->flight_count; ++i) {
    const size_t flight_index = batch->flights[i];
    bool has_quote = false;
    for (size_t q = 0; parse_ok && q < extracted.count; ++q)
      has_quote |= strcmp(extracted.quotes[q].id, flights[flight_index].id.data()) == 0;
    if (!has_quote && !result.cancelled && !result.deadline_exceeded &&
        runtime_.flight_table.TryPriceFallback(flight_index)) continue;
    if (parse_ok && ((!batch->defillama_fallback && !batch->mexc_fallback) || has_quote)) runtime_.flight_table.CompleteFlight(flight_index, true, &extracted, answered_at);
    else runtime_.flight_table.CompleteFlightFailure(flight_index, !transport_ok);
  }
  ResetBatch(*batch);
}

}  // namespace fortis::prices
