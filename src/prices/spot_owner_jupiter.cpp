#include <memory>
// The spot provider owner's Jupiter groups: one request's SPL mints priced
// in pages, committed in one frame once every page has answered.
#include "spot_provider_owner_internal.h"

namespace fortis::prices {

size_t JupiterSpotBatch::FreeGroupIndex() const {

  for (size_t g = 0; g < kMaxJupiterGroups; ++g) if (!groups[g].used) return g;
  return kNoGroup;
}

void JupiterSpotBatch::FreeGroup(size_t g) {

  const uint32_t generation = groups[g].generation;
  // GCC 13 in the pinned Unikraft toolchain ICEs on aggregate assignment of
  // this large fixed buffer. Zero it and restore the nonzero sentinel.
  std::destroy_at(&groups[g]);
  std::construct_at(&groups[g]);
  groups[g].generation = generation;
  groups[g].active_batch = kNoBatch;
}

bool JupiterSpotBatch::GroupHasWaiters(const JupiterGroup& group) const {
  auto& flights = runtime_.flight_table.flights;
  for (size_t i = 0; i < group.flight_count; ++i) {
    const size_t flight_index = group.flights[i];
    if (flight_index < kMaxSharedFlights &&
        (flights[flight_index].waiter_count || flights[flight_index].background))
      return true;
  }
  return false;
}

// Nothing was written for any page: every flight of the group falls back
// to its current cache state (floor or missing), exactly as a thrown
// JupiterClient.prices() does for the TypeScript group.
void JupiterSpotBatch::FailGroup(size_t g, bool transport_failure) {
  if (g >= kMaxJupiterGroups || !groups[g].used) return;
  JupiterGroup& group = groups[g];
  for (size_t i = 0; i < group.flight_count; ++i)
    runtime_.flight_table.CompleteFlightFailure(group.flights[i], transport_failure);
  FreeGroup(g);
}

// All pages parsed: write every quote and provider-answered unknown in one
// frame, then complete each flight with the group's answer.
void JupiterSpotBatch::CommitGroup(size_t g) {
  auto& cache = runtime_.cache;
  auto& series = runtime_.series;
  auto& flights = runtime_.flight_table.flights;
  JupiterGroup& group = groups[g];
  const int64_t answered_at = group.answered_at_ms ? group.answered_at_ms : runtime_.wall_ms();
  providers::CoinGeckoSpotResult extracted{};
  fortis::prices::PriceSpotQuoteInput quotes[kJupiterGroupMaxMints]{};
  const char* unknown[kJupiterGroupMaxMints]{};
  bool quoted[kJupiterGroupMaxMints]{};
  size_t quote_count = 0, unknown_count = 0;
  for (size_t q = 0; q < group.quote_count && extracted.count < providers::kCoinGeckoMaxSpotIds; ++q) {
    const JupiterGroupQuote& quote = group.quotes[q];
    size_t position = group.flight_count;
    for (size_t i = 0; i < group.flight_count; ++i)
      if (group.flights[i] == quote.flight) { position = i; break; }
    if (position == group.flight_count || quoted[position] ||
        quote.flight >= kMaxSharedFlights) continue;
    quoted[position] = true;
    const char* id = flights[quote.flight].id.data();
    providers::CoinGeckoSpotQuote& out = extracted.quotes[extracted.count++];
    out = providers::CoinGeckoSpotQuote{};
    memcpy(out.id, id, strlen(id) + 1);
    out.price = quote.price;
    out.change24hPct = quote.change24h_pct;
    out.hasChange24hPct = quote.has_change24h_pct;
    quotes[quote_count++] = {id, quote.price, quote.change24h_pct, 0,
                             quote.has_change24h_pct, false, answered_at, 0};
  }
  for (size_t i = 0; i < group.flight_count; ++i)
    if (!quoted[i] && group.flights[i] < kMaxSharedFlights)
      unknown[unknown_count++] = flights[group.flights[i]].id.data();
  fortis::prices::PriceSeriesStoreStatus series_status =
      fortis::prices::PriceSeriesStoreStatus::kWriteFailure;
  const auto cache_status = cache.StoreProviderBatchWithSeries(series,
      group.fiat, SpotResolutionProviderName(providers::PriceIdKind::kSpl), quotes, quote_count,
      unknown, unknown_count, true, answered_at, &series_status);
  runtime_.RecordStorageFailures(
      cache_status != fortis::prices::PriceSpotStoreStatus::kStored &&
          cache_status != fortis::prices::PriceSpotStoreStatus::kNoItems,
      series_status != fortis::prices::PriceSeriesStoreStatus::kStored &&
          series_status != fortis::prices::PriceSeriesStoreStatus::kNoItems);
  for (size_t i = 0; i < group.flight_count; ++i)
    runtime_.flight_table.CompleteFlight(group.flights[i], true, &extracted, answered_at);
  FreeGroup(g);
}

SpotTypes::PageSubmit JupiterSpotBatch::SubmitGroupPage(size_t g, size_t batch_index, uint64_t now) {
  auto& config = runtime_.config;
  auto& scratch_request = runtime_.scratch_request;
  auto& redirect = runtime_.redirect;
  auto& slots = runtime_.requests.slots;
  auto& flights = runtime_.flight_table.flights;
  auto& shared_batches = runtime_.batches.shared_batches;
  JupiterGroup& group = groups[g];
  SharedBatch& shared = shared_batches[batch_index];
  const size_t start = group.next_page_start;
  size_t count = start < group.flight_count ? group.flight_count - start : 0;
  if (count > providers::kJupiterPriceMaxMints) count = providers::kJupiterPriceMaxMints;
  if (!count) { FailGroup(g, false); return PageSubmit::kFailed; }
  const char* mints[providers::kJupiterPriceMaxMints]{};
  for (size_t i = 0; i < count; ++i)
    mints[i] = flights[group.flights[start + i]].id.data() + kSplPrefixBytes;
  if (providers::BuildJupiterPriceRequest(config, mints, count, shared.path, sizeof(shared.path),
                               &scratch_request) != providers::JupiterPriceRequestError::kNone) {
    FailGroup(g, false);
    return PageSubmit::kFailed;
  }
  scratch_request.timeout_ms = kAttemptTimeoutMs;
  redirect.Apply(scratch_request);
  providers::ProviderExecutionOptions options{};
  options.max_attempts = group.page_attempts < kJupiterPageMaxAttempts
      ? kJupiterPageMaxAttempts - group.page_attempts : 1;
  options.deadline_ms = 0;
  // The whole group gates every waiter, so the page runs under the latest
  // waiter deadline of any group flight, not only its own page.
  bool background_only = true;
  bool stale_refresh = false;
  for (size_t i = 0; i < group.flight_count; ++i) {
    const SharedFlight& flight = flights[group.flights[i]];
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
  if (background_only) options.deadline_ms = now + kAttemptTimeoutMs * 3ULL;
  // A stale refresh has no waiter deadline of its own.
  else if (!options.deadline_ms && stale_refresh) options.deadline_ms = now + kAttemptTimeoutMs * 3ULL;
  if (!options.deadline_ms) { FailGroup(g, true); return PageSubmit::kFailed; }
  shared.used = true;
  shared.background = background_only;
  shared.kind = providers::PriceIdKind::kSpl;
  shared.group = g;
  shared.flight_count = count;
  for (size_t i = 0; i < count; ++i) shared.flights[i] = group.flights[start + i];
  uint32_t generation = shared.generation + 1;
  if (!generation) ++generation;
  shared.generation = generation;
  const auto submitted = runtime_.ProviderSubmit(lane_for_kind(providers::PriceIdKind::kSpl),
                                        scratch_request, &shared, options);
  if (submitted.status != providers::ProviderExecutionSubmitStatus::kAccepted) {
    runtime_.batches.ResetBatch(shared);
    if (submitted.status == providers::ProviderExecutionSubmitStatus::kBusy) {
      group.retry_at_ms = runtime_.MonotonicMs() + kWaitSliceMs;
      return PageSubmit::kBusy;
    }
    FailGroup(g, true);
    return PageSubmit::kFailed;
  }
  shared.job_id = submitted.job_id;
  group.active_batch = batch_index;
  group.background = background_only;
  group.retry_at_ms = 0;
  return PageSubmit::kSubmitted;
}

// One Jupiter page answered. The group buffers parsed quotes until its last
// page; any transport failure, null body, or exhausted malformed-body retry
// fails the whole group without writing.
void JupiterSpotBatch::CompleteGroupPage(size_t batch_index, const providers::ProviderExecutionResult& result,
                                                size_t body_length, bool transport_ok) {
  auto& json_tokens = runtime_.json_tokens;
  auto& response_body = runtime_.response_body;
  auto& flights = runtime_.flight_table.flights;
  auto& shared_batches = runtime_.batches.shared_batches;
  SharedBatch& batch = shared_batches[batch_index];
  const size_t g = batch.group;
  uint16_t page_flights[providers::kCoinGeckoSpotMaxBatchCount];
  memcpy(page_flights, batch.flights, sizeof(page_flights));
  const size_t page_count = batch.flight_count <= providers::kJupiterPriceMaxMints
      ? batch.flight_count : providers::kJupiterPriceMaxMints;
  const uint32_t attempts = result.attempts ? result.attempts : 1;
  runtime_.batches.ResetBatch(batch);
  if (g >= kMaxJupiterGroups || !groups[g].used || groups[g].active_batch != batch_index)
    return;  // An orphaned page; its group already failed and released its flights.
  JupiterGroup& group = groups[g];
  group.active_batch = kNoBatch;
  if (!transport_ok) { FailGroup(g, true); return; }
  const char* mints[providers::kJupiterPriceMaxMints]{};
  for (size_t i = 0; i < page_count; ++i) mints[i] = flights[page_flights[i]].id.data() + kSplPrefixBytes;
  const providers::JupiterPriceResult jupiter = providers::ExtractJupiterPriceV3(response_body, body_length,
      mints, page_count, json_tokens, codec::kBoundedJsonMaxTokens);
  if (jupiter.error == providers::JupiterPriceError::kNone) {
    for (size_t q = 0; q < jupiter.count; ++q) {
      const providers::JupiterPriceQuote& quote = jupiter.quotes[q];
      for (size_t i = 0; i < page_count; ++i) {
        if (strcmp(mints[i], quote.mint) != 0) continue;
        if (group.quote_count < kJupiterGroupMaxMints)
          group.quotes[group.quote_count++] = {page_flights[i], quote.price,
                                               quote.change24hPct, quote.hasChange24hPct};
        break;
      }
    }
    group.answered_at_ms = runtime_.wall_ms();
    group.next_page_start += page_count;
    group.page_attempts = 0;
    if (group.next_page_start >= group.flight_count) CommitGroup(g);
    return;  // Otherwise the next page is scheduled after this drain.
  }
  // Object.entries(null) throws outside the HTTP retry loop: one call, no retry.
  if (jupiter.error == providers::JupiterPriceError::kNullBody) { FailGroup(g, false); return; }
  // A body res.json() cannot parse is retried like a transport failure, up
  // to three attempts per page including those the adapter already made.
  group.page_attempts += attempts;
  if (group.page_attempts >= kJupiterPageMaxAttempts) { FailGroup(g, false); return; }
  group.retry_at_ms = runtime_.MonotonicMs() +
      kJupiterParseRetryDelaysMs[group.page_attempts <= 1 ? 0 : 1];
}

}  // namespace fortis::prices
