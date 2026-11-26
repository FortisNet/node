#include "token_service_internal.h"
namespace tj = fortis::codec::tj;
namespace fortis::tokens {
// ---- price series seam (PricesFacet.history for the page series) ---------------
// The shared price history service reads its store and fills gaps through
// its backfill owner on this worker thread, bounded by the task deadline.
// Stop cancels a pending gap, so a stopping service does not wait out the
// provider.
bool TokenPriceAccess::Cancelled(void* context) {
  return static_cast<TokenPriceAccess*>(context)->control_.IsStopping();
}
fortis::tokens::token_facts::SeriesStatus TokenPriceAccess::Read(Work& work, const char* price_id,
                                                                      int64_t from_seconds, int64_t to_seconds,
                                                                      bool hourly,
                                                                      fortis::tokens::token_facts::SeriesRead* out) {
  using fortis::tokens::token_facts::SeriesStatus;
  const size_t capacity = prices::PriceHistoryService::SeriesCapacity(from_seconds, to_seconds, hourly);
  if (!history_ || !capacity) return SeriesStatus::kUnavailable;
  // 72 bytes per bucket in the work arena: 26 KiB for 365 daily candles,
  // 616 KiB for 365 hourly days.
  auto* candles = static_cast<fortis::prices::PriceSeriesCandle*>(
      work.arena.Alloc(sizeof(fortis::prices::PriceSeriesCandle) * capacity, alignof(fortis::prices::PriceSeriesCandle)));
  if (!candles) return SeriesStatus::kUnavailable;
  prices::PriceHistorySeriesQuery query{};
  query.price_id = price_id;
  query.from_seconds = from_seconds;
  query.to_seconds = to_seconds;
  query.hourly = hourly;
  query.deadline_monotonic_ms = work.deadline_ms;
  query.cancelled = &TokenPriceAccess::Cancelled;
  query.cancel_context = this;
  query.candles = candles;
  query.capacity = capacity;
  prices::PriceHistorySeriesResult result{};
  const prices::PriceHistorySeriesStatus status = history_->ReadSeries(query, &result);
  if (status == prices::PriceHistorySeriesStatus::kNoSeries) return SeriesStatus::kNoSeries;
  if (status != prices::PriceHistorySeriesStatus::kOk) return SeriesStatus::kUnavailable;
  out->candles = candles;
  out->count = result.count;
  out->cache = fortis::tokens::token_facts::Provenance{};
  out->cache.hit = result.hit;
  out->cache.has_timestamp = result.has_timestamp;
  out->cache.timestamp = result.timestamp_seconds * 1000;
  return SeriesStatus::kOk;
}

void TokenPriceAccess::Done(void* context, const prices::SpotProviderOwnerResult& result) {
  auto* wait = static_cast<SpotWait*>(context);
  fortis::base::LockGuard lock(wait->mutex);
  wait->complete = result.status == prices::SpotProviderOwnerResultStatus::kComplete && result.body && result.body_length;
  if (wait->complete) {
    wait->body = base::AllocateStorage<uint8_t[]>(result.body_length);
    if (wait->body) { memcpy(wait->body.get(), result.body, result.body_length); wait->length = result.body_length; }
    else wait->complete = false;
  }
  wait->finished = true;
  wait->done.Signal();
}
bool TokenPriceAccess::Quote( const char* price_id, uint64_t deadline_ms,
                                   fortis::tokens::token_facts::SpotQuote* quote) {
  if (!spot_) return false;
  SpotWait wait;
  std::string_view id{price_id, strlen(price_id)};
  std::string_view fiat{"usd", 3};
  const uint64_t now = providers::TokenMonotonicMs();
  uint64_t deadline = deadline_ms > now ? deadline_ms : now + 1000;
  if (deadline - now > prices::kSpotProviderOwnerMaxDeadlineMs) deadline = now + prices::kSpotProviderOwnerMaxDeadlineMs;
  if (spot_->Submit(&id, 1, fiat, prices::SpotProviderOwnerFormat::kJson, deadline,
                         &TokenPriceAccess::Done, &wait).status() != prices::SpotProviderOwnerSubmitStatus::kAccepted)
    return false;
  fortis::base::UniqueLock lock(wait.mutex);
  while (!wait.finished) wait.done.WaitLocked(wait.mutex);
  lock.Unlock();


  bool hit = false;
  if (wait.complete) {
    const size_t bytes = wait.length * 4 + 4096;
    auto memory_storage = base::AllocateStorage<uint8_t[]>(bytes);
    auto* memory = memory_storage.get();
    if (memory) {
      tj::Arena a{memory, bytes, 0, true};
      Value* body = tj::Parse(a, wait.body.get(), wait.length);
      const Value* entry = tj::Get(tj::Get(body, "prices"), price_id);
      const Value* price = tj::Get(entry, "price");
      if (tj::IsObject(entry) && tj::IsNumber(price)) {
        hit = true;
        *quote = fortis::tokens::token_facts::SpotQuote{};
        quote->price = price->number;
        const Value* change = tj::Get(entry, "change24hPct");
        quote->has_change = tj::IsNumber(change);
        quote->change = quote->has_change ? change->number : 0;
        const Value* as_of = tj::Get(entry, "asOf");
        if (tj::IsString(as_of) && as_of->length < sizeof(quote->as_of)) {
          quote->has_as_of = true;
          memcpy(quote->as_of, as_of->text, as_of->length);
          quote->as_of[as_of->length] = '\0';
        }
        const Value* cache = tj::Get(entry, "cache");
        quote->cache.hit = tj::Get(cache, "hit") && tj::Get(cache, "hit")->type == tj::Type::kTrue;
        const Value* stamp = tj::Get(cache, "timestamp");
        double parsed = 0;
        if (tj::IsString(stamp) && fortis::codec::provider_text::DateParse(stamp->text, stamp->length, &parsed)) {
          quote->cache.has_timestamp = true;
          quote->cache.timestamp = static_cast<int64_t>(parsed);
        }
      }
      memory_storage.reset();
    }
  }
  wait.body.reset();
  return hit;
}

}  // namespace fortis::tokens
