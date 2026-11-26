#include <memory>
// The spot provider owner's Robinhood groups: issuer reference quotes for
// stock-token contracts, read with bounded concurrency and a short cache.
#include "spot_provider_owner_internal.h"

#include "threading.h"

namespace fortis::prices {

size_t RobinhoodSpotBatch::FreeRobinhoodGroupIndex() const {

  for (size_t i = 0; i < kMaxRobinhoodGroups; ++i)
    if (!robinhood_groups[i].used) return i;
  return kNoGroup;
}

size_t RobinhoodSpotBatch::FreeRobinhoodBatchIndex() const {
  auto& shared_batches = runtime_.batches.shared_batches;
  for (size_t i = 0; i < kSpotProviderOwnerMaxRequests; ++i)
    if (!shared_batches[i].used) return i;
  return kNoGroup;
}

const providers::RobinhoodAsset* RobinhoodSpotBatch::RobinhoodAssetForFlight(size_t flight_index) const {
  auto& flights = runtime_.flight_table.flights;
  if (flight_index >= kMaxSharedFlights) return nullptr;
  providers::ParsedPriceId parsed{};
  const SharedFlight& flight = flights[flight_index];
  if (!ParsePriceId(flight.id.data(), strlen(flight.id.data()), &parsed)) return nullptr;
  for (size_t i = 0; i < robinhood_catalog.count; ++i)
    if (strcmp(robinhood_catalog.assets[i].address, parsed.address) == 0)
      return &robinhood_catalog.assets[i];
  return nullptr;
}

bool RobinhoodSpotBatch::RobinhoodCachedQuote(const char* address, int64_t now, double* price) const {

  for (const RobinhoodQuoteCacheEntry& entry : robinhood_quote_cache)
    if (entry.used && entry.until_ms > now && strcmp(entry.address, address) == 0) {
      *price = entry.price; return true;
    }
  return false;
}

void RobinhoodSpotBatch::SaveRobinhoodQuote(const char* address, double price, int64_t now) {

  size_t target = providers::kRobinhoodMaxAssets;
  for (size_t i = 0; i < providers::kRobinhoodMaxAssets; ++i) {
    if (robinhood_quote_cache[i].used && strcmp(robinhood_quote_cache[i].address, address) == 0) {
      target = i; break;
    }
    if (target == providers::kRobinhoodMaxAssets &&
        (!robinhood_quote_cache[i].used || robinhood_quote_cache[i].until_ms <= now)) target = i;
  }
  if (target == providers::kRobinhoodMaxAssets) target = 0;
  RobinhoodQuoteCacheEntry& entry = robinhood_quote_cache[target];
  entry = RobinhoodQuoteCacheEntry{}; entry.used = true;
  memcpy(entry.address, address, strlen(address) + 1);
  entry.price = price; entry.until_ms = now + 15000;
}

void RobinhoodSpotBatch::SubmitRobinhoodQuote(size_t group_index, size_t position, size_t batch_index,
                                                   const char* path, bool catalog_request) {
  auto& config = runtime_.config;
  auto& scratch_request = runtime_.scratch_request;
  auto& redirect = runtime_.redirect;
  auto& slots = runtime_.requests.slots;
  auto& flights = runtime_.flight_table.flights;
  auto& shared_batches = runtime_.batches.shared_batches;
  RobinhoodGroup& group = robinhood_groups[group_index];
  SharedBatch& batch = shared_batches[batch_index];
  batch.used = true; batch.kind = providers::PriceIdKind::kRobinhood;
  batch.robinhood = true; batch.robinhood_catalog_request = catalog_request;
  batch.robinhood_group_index = static_cast<uint16_t>(group_index);
  batch.robinhood_group_generation = group.generation;
  batch.robinhood_position = position;
  if (!catalog_request) {
    batch.flight_count = 1;
    batch.flights[0] = group.flights[position];
  }
  uint32_t generation = batch.generation + 1; if (!generation) ++generation;
  batch.generation = generation;
  if (!providers::BuildRobinhoodRequest(config, path, &scratch_request)) {
    FailRobinhoodGroup(group_index, true); return;
  }
  redirect.Apply(scratch_request);
  providers::ProviderExecutionOptions options{}; options.max_attempts=3;
  bool background=true;
  for(size_t i=0;i<group.flight_count;++i) {
    const SharedFlight& flight=flights[group.flights[i]];
    background &= flight.background && flight.foreground_waiters==0 && !flight.stale_refresh;
    for(size_t w=0;w<flight.waiter_count;++w) {
      const FlightWaiter& waiter=flight.waiters[w];
      if(waiter.slot_index<kSpotProviderOwnerMaxRequests &&
         slots[waiter.slot_index].generation==waiter.generation &&
         slots[waiter.slot_index].deadline_ms>options.deadline_ms)
        options.deadline_ms=slots[waiter.slot_index].deadline_ms;
    }
  }
  options.background=background;
  if(!options.deadline_ms) options.deadline_ms=runtime_.MonotonicMs()+kAttemptTimeoutMs*3ULL;
  batch.background=background;
  const auto submitted=runtime_.ProviderSubmit("robinhood-assets",scratch_request,&batch,options);
  if(submitted.status!=providers::ProviderExecutionSubmitStatus::kAccepted) {
    runtime_.batches.ResetBatch(batch); FailRobinhoodGroup(group_index,true); return;
  }
  batch.job_id=submitted.job_id;
  if(!catalog_request) {
    ++group.active_quotes;
    flights[group.flights[position]].state=FlightState::kInFlight;
  }
}

void RobinhoodSpotBatch::FailRobinhoodGroup(size_t group_index, bool transport_failure) {
  auto& shared_batches = runtime_.batches.shared_batches;
  if(group_index>=kMaxRobinhoodGroups) return;
  RobinhoodGroup& group=robinhood_groups[group_index];
  if(!group.used) return;
  const uint32_t generation=group.generation;
  const size_t count=group.flight_count;
  uint16_t indices[kSpotResolutionMaxIds]{};
  memcpy(indices,group.flights,count*sizeof(uint16_t));
  for(SharedBatch& batch:shared_batches)
    if(batch.used&&batch.robinhood&&batch.robinhood_group_index==group_index&&
       batch.robinhood_group_generation==generation) { runtime_.ProviderCancel(batch.job_id); runtime_.batches.ResetBatch(batch); }
  std::destroy_at(&group); std::construct_at(&group); group.generation=generation;
  for(size_t i=0;i<count;++i) runtime_.flight_table.CompleteFlightFailure(indices[i],transport_failure);
}

void RobinhoodSpotBatch::CommitRobinhoodGroup(size_t group_index) {
  auto& cache = runtime_.cache;
  auto& series = runtime_.series;
  auto& flights = runtime_.flight_table.flights;
  RobinhoodGroup& group=robinhood_groups[group_index];
  const size_t count=group.flight_count;
  providers::CoinGeckoSpotResult extracted{};
  fortis::prices::PriceSpotQuoteInput quotes[kSpotResolutionMaxIds]{};
  const char* unknown[kSpotResolutionMaxIds]{};
  size_t quote_count=0,unknown_count=0;
  const int64_t answered_at=runtime_.wall_ms();
  for(size_t i=0;i<count;++i) {
    const char* id=flights[group.flights[i]].id.data();
    if(!group.values[i].has_price) {unknown[unknown_count++]=id;continue;}
    providers::CoinGeckoSpotQuote& q=extracted.quotes[extracted.count++];
    memcpy(q.id,id,strlen(id)+1);q.price=group.values[i].price;
    quotes[quote_count++]={id,q.price,0,0,false,false,answered_at,0};
  }
  extracted.error=providers::CoinGeckoSpotError::kNone;
  fortis::prices::PriceSeriesStoreStatus series_status=fortis::prices::PriceSeriesStoreStatus::kWriteFailure;
  const auto cache_status=cache.StoreProviderBatchWithSeries(series,"usd","robinhood-assets",
      quotes,quote_count,unknown,unknown_count,true,answered_at,&series_status);
  runtime_.RecordStorageFailures(cache_status!=fortis::prices::PriceSpotStoreStatus::kStored&&cache_status!=fortis::prices::PriceSpotStoreStatus::kNoItems,
      series_status!=fortis::prices::PriceSeriesStoreStatus::kStored&&series_status!=fortis::prices::PriceSeriesStoreStatus::kNoItems);
  uint16_t indices[kSpotResolutionMaxIds]{};memcpy(indices,group.flights,count*sizeof(uint16_t));
  const uint32_t generation=group.generation;std::destroy_at(&group); std::construct_at(&group);group.generation=generation;
  for(size_t i=0;i<count;++i) runtime_.flight_table.CompleteFlight(indices[i],true,&extracted,answered_at);
}

void RobinhoodSpotBatch::AdvanceRobinhoodGroup(size_t group_index) {
  if(group_index>=kMaxRobinhoodGroups) return;
  RobinhoodGroup& group=robinhood_groups[group_index];if(!group.used||!group.catalog_complete)return;
  while(group.active_quotes<kRobinhoodQuoteConcurrency&&group.next_position<group.flight_count) {
    const size_t position=group.next_position++;
    const size_t flight_index=group.flights[position];
    const providers::RobinhoodAsset* asset=RobinhoodAssetForFlight(flight_index);
    if(!asset) {++group.completed_count;continue;}
    double cached=0;
    if(RobinhoodCachedQuote(asset->address,runtime_.wall_ms(),&cached)) {
      group.values[position]={cached,true};++group.completed_count;continue;
    }
    const size_t batch_index=FreeRobinhoodBatchIndex();
    if(batch_index==kNoGroup) {--group.next_position;break;}
    char path[providers::kRobinhoodPricePathBytes]{};
    if(!providers::BuildRobinhoodPricePath(asset->symbol,path,sizeof(path))) {FailRobinhoodGroup(group_index,false);return;}
    SubmitRobinhoodQuote(group_index,position,batch_index,path,false);
    if(!group.used)return;
  }
  if(group.active_quotes==0&&group.completed_count==group.flight_count) CommitRobinhoodGroup(group_index);
}

void RobinhoodSpotBatch::CompleteRobinhoodBatch(SharedBatch& batch, size_t body_length, bool transport_ok) {
  auto& json_tokens = runtime_.json_tokens;
  auto& response_body = runtime_.response_body;
  const size_t group_index=batch.robinhood_group_index;
  if(group_index>=kMaxRobinhoodGroups) {runtime_.batches.ResetBatch(batch);return;}
  RobinhoodGroup& group=robinhood_groups[group_index];
  if(!group.used||group.generation!=batch.robinhood_group_generation) {runtime_.batches.ResetBatch(batch);return;}
  const bool catalog_request=batch.robinhood_catalog_request;
  const size_t position=batch.robinhood_position;
  const size_t flight_index=catalog_request?kMaxSharedFlights:group.flights[position];
  if(!transport_ok) {FailRobinhoodGroup(group_index,true);return;}
  if(catalog_request) {
    providers::RobinhoodAssetCatalog parsed{};
    // A shared catalog parses the body (parseAssets) and keeps it.
    fortis::base::UniqueLock robinhood_shared_lock(robinhood_shared_mutex);
    const SpotRobinhoodCatalog* shared=robinhood_shared;
    const bool adopted=shared&&shared->adopt(shared->context,response_body,body_length,&parsed);
    robinhood_shared_lock.Unlock();
    if(shared?!adopted:
       ExtractRobinhoodAssets(response_body,body_length,json_tokens,codec::kBoundedJsonMaxTokens,&parsed)!=providers::RobinhoodError::kNone) {
      FailRobinhoodGroup(group_index,false);return;
    }
    robinhood_catalog=parsed;robinhood_catalog_until_ms=runtime_.wall_ms()+15*60*1000;
    group.catalog_complete=true;runtime_.batches.ResetBatch(batch);AdvanceRobinhoodGroup(group_index);return;
  }
  const providers::RobinhoodAsset* asset=RobinhoodAssetForFlight(flight_index);double midpoint=0;
  const providers::RobinhoodError error=asset?ExtractRobinhoodQuote(response_body,body_length,*asset,json_tokens,codec::kBoundedJsonMaxTokens,&midpoint):providers::RobinhoodError::kInvalidQuote;
  if(error==providers::RobinhoodError::kInvalidJson||error==providers::RobinhoodError::kResponseTooLarge||error==providers::RobinhoodError::kTokenLimit) {
    FailRobinhoodGroup(group_index,false);return;
  }
  if(error==providers::RobinhoodError::kNone&&asset) {group.values[position]={midpoint,true};SaveRobinhoodQuote(asset->address,midpoint,runtime_.wall_ms());}
  if (group.active_quotes) --group.active_quotes;
  ++group.completed_count;
  runtime_.batches.ResetBatch(batch);AdvanceRobinhoodGroup(group_index);
}

bool RobinhoodSpotBatch::RobinhoodGroupHasWaiters(const RobinhoodGroup& group) const {
  auto& flights = runtime_.flight_table.flights;
  for (size_t i = 0; i < group.flight_count; ++i) {
    const size_t flight_index = group.flights[i];
    if (flight_index < kMaxSharedFlights &&
        (flights[flight_index].waiter_count || flights[flight_index].background)) return true;
  }
  return false;
}

}  // namespace fortis::prices
