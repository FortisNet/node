#include "price_diagnostics.h"
#include <string_view>
#include "exception_boundary.h"
// The spot provider owner's request slots: resolving a queued request
// against the cache, joining or creating its flights, and completing it.
#include "spot_provider_owner_internal.h"

#include "threading.h"

namespace fortis::prices {

void SpotRequestProcessor::Release(size_t index) {
  auto& mutex = runtime_.mutex;
  auto& changed = runtime_.changed;
  fortis::base::LockGuard lock(mutex);
  ResetSlotFields(slots[index], slots[index].generation);
  changed.Broadcast();
}

void SpotRequestProcessor::ResetSlotFields(Slot& slot, uint32_t generation) {

  slot.state = SlotState::kFree;
  slot.generation = generation;
  slot.request_id = 0;
  slot.deadline_ms = 0;
  slot.format = SpotProviderOwnerFormat::kJson;
  slot.completion = nullptr;
  slot.completion_context = nullptr;
  slot.cancel_requested = false;
  slot.callback_sent = false;
  slot.batch_initialized = false;
  slot.deadline_fallback = false;
  slot.background = false;
  slot.refresh_lead_ms = 0;
  slot.fiat[0] = '\0';
  memset(slot.raw_ids, 0, sizeof(slot.raw_ids));
  for (auto& view : slot.raw_views) view = std::string_view{};
  slot.raw_count = 0;
  slot.pending_flights = 0;
  memset(slot.flight_indices, 0, sizeof(slot.flight_indices));
  memset(slot.flight_registered, 0, sizeof(slot.flight_registered));
  slot.plan.id_count = 0;
  slot.plan.batch_count = 0;
}

bool SpotRequestProcessor::IsState(size_t index, SlotState expected) {
  auto& mutex = runtime_.mutex;
  fortis::base::LockGuard lock(mutex);
  const bool matches = slots[index].state == expected;
  return matches;
}

SpotTypes::CompletionHandle SpotRequestProcessor::CommitCompletion(size_t index,
                                                      SpotProviderOwnerResultStatus requested_status) {
  auto& mutex = runtime_.mutex;
  auto& stop_requested = runtime_.stop_requested;
  CompletionHandle handle{};
  fortis::base::LockGuard lock(mutex);
  Slot& slot = slots[index];
  if (slot.state == SlotState::kFree || slot.callback_sent) return handle;
  slot.callback_sent = true;
  handle.callback = slot.completion;
  handle.context = slot.completion_context;
  handle.request_id = slot.request_id;
  handle.status = stop_requested ? SpotProviderOwnerResultStatus::kShutdown
      : slot.cancel_requested ? SpotProviderOwnerResultStatus::kCancelled
      : !slot.deadline_fallback && runtime_.MonotonicMs() >= slot.deadline_ms
          ? SpotProviderOwnerResultStatus::kTimeout : requested_status;
  handle.valid = true;
  return handle;
}

void SpotRequestProcessor::EmitSerialized(size_t index, SpotResolutionCompletionStatus resolution) {

  bool ready = false;
  const SpotResponseInput* response = SpotResolutionResponse(
      &slots[index].workspace, &ready);
  if (!response || !ready) {
    FinishWithoutBody(index, SpotProviderOwnerResultStatus::kUnsupportedProvider,
                      resolution);
    return;
  }
  const CompletionHandle handle = CommitCompletion(
      index, SpotProviderOwnerResultStatus::kComplete);
  if (!handle.valid) return;
  if (handle.status != SpotProviderOwnerResultStatus::kComplete) {
    Release(index);
    if (handle.callback) {
      const SpotProviderOwnerResult result{handle.request_id, handle.status,
          resolution, nullptr, nullptr, 0};
      fortis::base::FatalBoundary("spot completion", [&] { handle.callback(handle.context, result); });
    }
    return;
  }
  size_t length = 0;
  SpotSerializeResult encoded = SpotSerializeResult::kInvalidInput;
  const char* content_type = nullptr;
  if (slots[index].format == SpotProviderOwnerFormat::kJson) {
    encoded = SerializeSpotResponseJson(*response,
        reinterpret_cast<char*>(serialized), serialized_capacity, &length);
    content_type = "application/json";
  } else {
    encoded = SerializeSpotResponseProtobuf(*response, serialized,
                                             serialized_capacity, &length);
    content_type = "application/protobuf";
  }
  if (encoded != SpotSerializeResult::kOk) {
    Release(index);
    if (handle.callback) {
      const SpotProviderOwnerResult result{handle.request_id,
          SpotProviderOwnerResultStatus::kInternalFailure, resolution,
          nullptr, nullptr, 0};
      fortis::base::FatalBoundary("spot completion", [&] { handle.callback(handle.context, result); });
    }
    return;
  }
  ReportMissingPrices(runtime_.errors, *response);
  Release(index);
  if (handle.callback) {
    const SpotProviderOwnerResult result{handle.request_id, handle.status,
        resolution, content_type, serialized, length};
    fortis::base::FatalBoundary("spot completion", [&] { handle.callback(handle.context, result); });
  }
}

void SpotRequestProcessor::FinishWithoutBody(size_t index, SpotProviderOwnerResultStatus status,
                                                SpotResolutionCompletionStatus resolution) {

  const CompletionHandle handle = CommitCompletion(index, status);
  Release(index);
  if (handle.valid && handle.callback) {
    const SpotProviderOwnerResult result{handle.request_id, handle.status,
        resolution, nullptr, nullptr, 0};
    fortis::base::FatalBoundary("spot completion", [&] { handle.callback(handle.context, result); });
  }
}

size_t FlightTable::FindOrCreateFlight(const char* fiat, const char* id, providers::PriceIdKind kind,
                                                   bool background) {

  for (size_t f = 0; f < kMaxSharedFlights; ++f) {
    const SharedFlight& candidate = flights[f];
    if (candidate.state != FlightState::kFree &&
        (candidate.waiter_count != 0 || candidate.background) &&
        strcmp(candidate.fiat.data(), fiat) == 0 && strcmp(candidate.id.data(), id) == 0)
    {
      if (background) flights[f].background = true;
      return f;
    }
  }
  if (background) {
    size_t background_only = 0;
    for (const SharedFlight& flight : flights)
      background_only += flight.state != FlightState::kFree &&
                         flight.background && flight.waiter_count == 0;
    if (background_only >= kMaxBackgroundOnlyFlights) return kMaxSharedFlights;
  }
  size_t index = kMaxSharedFlights;
  for (size_t f = 0; f < kMaxSharedFlights; ++f)
    if (flights[f].state == FlightState::kFree) { index = f; break; }
  if (index == kMaxSharedFlights) return index;
  SharedFlight& flight = flights[index];
  uint32_t generation = flight.generation + 1;
  if (!generation) ++generation;
  flight = SharedFlight{};
  flight.generation = generation;
  flight.state = FlightState::kWindow;
  flight.background = background;
  flight.kind = kind;
  memcpy(flight.fiat.data(), fiat, strlen(fiat) + 1);
  memcpy(flight.id.data(), id, strlen(id) + 1);
  const char* platform = flight_platform(kind, id);
  memcpy(flight.platform.data(), platform, strlen(platform) + 1);
  const uint64_t opened_at = runtime_.MonotonicMs();
  flight.window_deadline_ms = opened_at + kBatchWindowMs;
  for (const SharedFlight& peer : flights) {
    if (&peer != &flight && peer.state == FlightState::kWindow &&
        strcmp(peer.fiat.data(), fiat) == 0 && peer.window_deadline_ms > opened_at) {
      flight.window_deadline_ms = peer.window_deadline_ms;
      break;
    }
  }
  return index;
}

void SpotRequestProcessor::ScheduleStaleRevalidations(SpotResolutionWorkspace* workspace,
                                                         const char* fiat, int64_t refresh_lead_ms) {
  auto& contract_popularity = runtime_.contract_popularity;
  auto& flights = runtime_.flight_table.flights;
  size_t count = 0;
  const SpotResolutionIntent* intents = SpotResolutionIntents(workspace, &count);
  for (size_t i = 0; intents && i < count; ++i) {
    const SpotResolutionIntent& intent = intents[i];
    if (intent.kind != SpotResolutionIntentKind::kBackgroundRevalidation ||
        (!intent.stale_value_served && !refresh_lead_ms) || !intent.source_id.data() ||
        intent.source_id.empty() || intent.source_id.size() > providers::kPriceIdMaxSourceIdBytes) continue;
    char id[providers::kPriceIdMaxSourceIdBytes + 1];
    memcpy(id, intent.source_id.data(), intent.source_id.size());
    id[intent.source_id.size()] = '\0';
    // A full fixed registry sheds only the revalidation. The stale value is
    // already ready for the caller, and a later stale read can try again.
    const size_t flight = runtime_.flight_table.FindOrCreateFlight(fiat, id, intent.id_kind, true);
    if (flight >= kMaxSharedFlights) continue;
    if (!refresh_lead_ms) flights[flight].stale_refresh = true;
    // Record the admission attempt only when there is actual refresh work.
    // The next scheduler tick can retry after a provider refusal.
    if (refresh_lead_ms && contract_popularity) {
      const fortis::discovery::CoinIdView due{id, intent.source_id.size()};
      contract_popularity->MarkRefreshed(&due, 1);
    }
  }
}

void SpotRequestProcessor::ProcessQueued(size_t index) {
  auto& mutex = runtime_.mutex;
  auto& cache = runtime_.cache;
  auto& flights = runtime_.flight_table.flights;
  fortis::base::UniqueLock lock(mutex);
  Slot& slot = slots[index];
  if (slot.state != SlotState::kQueued) return;
  slot.state = SlotState::kWorking;
  lock.Unlock();

  const int64_t now = runtime_.wall_ms();
  if (!BeginSpotResolution(slot.raw_views, slot.raw_count,
        {slot.fiat, strlen(slot.fiat)}, cache, now, &slot.workspace, slot.refresh_lead_ms).ok()) {
    FinishWithoutBody(index, SpotProviderOwnerResultStatus::kInternalFailure,
                      SpotResolutionCompletionStatus::kInvalidWorkspace);
    return;
  }
  ScheduleStaleRevalidations(&slot.workspace, slot.fiat, slot.refresh_lead_ms);
  size_t fetch_count = 0;
  const char* const* fetch_ids = SpotResolutionFetchIds(&slot.workspace, &fetch_count);
  size_t kind_count = 0;
  const providers::PriceIdKind* fetch_kinds = SpotResolutionFetchKinds(&slot.workspace, &kind_count);
  if (fetch_count == 0) {
    EmitSerialized(index, SpotResolutionCompletionStatus::kNotNeeded);
    return;
  }
  // The request accumulator is per canonical ID. Shared upstream batches
  // may combine different request plans, so record each flight independently.
  // Fetch IDs are already canonical and unique; each becomes a one-ID batch
  // of its own kind with that provider's exact single-ID path length.
  bool plan_ok = kind_count == fetch_count && fetch_count <= providers::kCoinGeckoSpotMaxBatchCount;
  slot.plan.id_count = 0;
  slot.plan.batch_count = 0;
  for (size_t i = 0; plan_ok && i < fetch_count; ++i) {
    const size_t id_length = strnlen(fetch_ids[i], sizeof(slot.plan.ids[i]));
    const size_t path_length = SpotResolutionSingleIdPathLength(
        fetch_kinds[i], fetch_ids[i], slot.fiat);
    if (!id_length || id_length >= sizeof(slot.plan.ids[i]) || !path_length) {
      plan_ok = false;
      break;
    }
    memcpy(slot.plan.ids[i], fetch_ids[i], id_length + 1);
    slot.plan.batches[i] = {i, 1, path_length};
  }
  if (!plan_ok) {
    slot.plan.id_count = 0;
    slot.plan.batch_count = 0;
    const auto completed = CompleteSpotResolution(&slot.workspace, cache, nullptr, now);
    EmitSerialized(index, completed.status);
    return;
  }
  slot.plan.id_count = fetch_count;
  slot.plan.batch_count = fetch_count;
  if (!InitializeSpotResolutionBatchAccumulator(&slot.workspace, &slot.plan,
                                                &slot.accumulator)) {
    FinishWithoutBody(index, SpotProviderOwnerResultStatus::kInternalFailure,
                      SpotResolutionCompletionStatus::kInvalidWorkspace);
    return;
  }
  slot.batch_initialized = true;
  // Flight registration runs on the owner thread and is atomic with respect
  // to other registrations; Submit only publishes bounded request slots.
  for (size_t fetch_index = 0; fetch_index < slot.plan.id_count; ++fetch_index) {
    const size_t flight_index = runtime_.flight_table.FindOrCreateFlight(
        slot.fiat, slot.plan.ids[fetch_index], fetch_kinds[fetch_index], slot.background);
    if (flight_index == kMaxSharedFlights) {
      RecordSpotResolutionBatchFailure(&slot.accumulator, fetch_index, true);
      continue;
    }
    SharedFlight& flight = flights[flight_index];
    if (flight.waiter_count >= kSpotProviderOwnerMaxRequests) {
      RecordSpotResolutionBatchFailure(&slot.accumulator, fetch_index, true);
      continue;
    }
    flight.waiters[flight.waiter_count++] = {index, slot.generation, fetch_index};
    if (!slot.background) ++flight.foreground_waiters;
    slot.flight_indices[fetch_index] = static_cast<uint16_t>(flight_index);
    slot.flight_registered[fetch_index] = true;
    ++slot.pending_flights;
  }
  if (slot.pending_flights == 0) CompleteRequest(index);
}

void SpotRequestProcessor::CompleteRequest(size_t index) {
  auto& cache = runtime_.cache;
  Slot& slot = slots[index];
  if (!slot.batch_initialized || slot.pending_flights != 0 || slot.callback_sent ||
      slot.state == SlotState::kFree) return;
  const auto completed = FinalizeSpotResolutionBatches(
      &slot.workspace, cache, &slot.accumulator, runtime_.wall_ms(), false);
  if (completed.status == SpotResolutionCompletionStatus::kInvalidWorkspace)
    FinishWithoutBody(index, SpotProviderOwnerResultStatus::kInternalFailure,
                      completed.status);
  else
    EmitSerialized(index, completed.status);
}

}  // namespace fortis::prices
