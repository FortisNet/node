#include "ownership.h"
#include "wallet_price_access.h"
#include "chain_transport.h"
#include "spot_provider_owner.h"
#include "threading.h"
#include <string_view>
#include <stdlib.h>
#include <string.h>
namespace tj = fortis::codec::tj;
namespace fortis::wallet::address {
using chains::Value;
using chains::Arena;
namespace { constexpr uint64_t kSpotDeadlineMs = 25000; }
// ---- spot prices (the TypeScript PricesFacet.spot) -----------------------------

namespace {
struct SpotWait {
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  bool finished = false;
  bool complete = false;
  std::unique_ptr<uint8_t[], base::StorageDeleter> body;
  size_t length = 0;
};
void spot_done(void* context, const prices::SpotProviderOwnerResult& result) {
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
}  // namespace

Value* WalletPriceAccess::Read(Arena& arena, const char* const* ids, size_t count) {
  Value* prices = tj::Obj(arena);
  if (!prices || !spot_ || !count) return prices;
  for (size_t offset = 0; offset < count; offset += prices::kPriceSpotMaxBatchItems) {
    const size_t n = count - offset < prices::kPriceSpotMaxBatchItems ? count - offset : prices::kPriceSpotMaxBatchItems;
    std::string_view views[prices::kPriceSpotMaxBatchItems];
    for (size_t i = 0; i < n; ++i) views[i] = std::string_view{ids[offset + i], strlen(ids[offset + i])};
    SpotWait wait;
    const uint64_t deadline = chains::ChainMonotonicMs() + kSpotDeadlineMs;
    if (spot_->Submit(views, n, std::string_view{"usd", 3}, prices::SpotProviderOwnerFormat::kJson,
                          deadline, spot_done, &wait).status() != prices::SpotProviderOwnerSubmitStatus::kAccepted) {
      // prices.spot(...).catch(() => ({prices: {}})): every quote unpriced.
      return tj::Obj(arena);
    }
    fortis::base::UniqueLock lock(wait.mutex);
    while (!wait.finished) wait.done.WaitLocked(wait.mutex);
    lock.Unlock();


    if (!wait.complete) {
      wait.body.reset();
      return tj::Obj(arena);
    }
    Value* body = tj::Parse(arena, wait.body.get(), wait.length);
    wait.body.reset();
    const Value* quotes = tj::Get(body, "prices");
    if (!tj::IsObject(quotes)) return tj::Obj(arena);
    for (uint32_t i = 0; i < quotes->count; ++i) {
      const Value* price = tj::Get(quotes->members[i].value, "price");
      if (tj::IsNumber(price))
        tj::Set(arena, prices, quotes->members[i].key, quotes->members[i].key_length, tj::Num(arena, price->number));
    }
  }
  return prices;
}


prices::PriceSeriesReadStatus WalletPriceAccess::Range(prices::PriceSeriesInterval interval,
    const char* fiat, const char* source, int64_t from, int64_t to, int64_t now,
    prices::PriceSeriesCandle* output, size_t capacity, size_t* count) const {
  return series_.Range(interval, fiat, source, from, to, now, output, capacity, count);
}
}  // namespace fortis::wallet::address
