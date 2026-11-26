#pragma once
#include "wallet_readers.h"
namespace fortis::prices { class SpotProviderOwner; }
namespace fortis::wallet::address {
// Borrowed price-plane adapters. Price request processing remains in src/prices.
class WalletPriceAccess final : public WalletSpotReader, public WalletSeriesReader {
 public:
  WalletPriceAccess(prices::SpotProviderOwner* spot, prices::PriceSeriesStore& series)
      : spot_(spot), series_(series) {}
  chains::Value* Read(chains::Arena& arena, const char* const* ids, size_t count) override;
  prices::PriceSeriesReadStatus Range(prices::PriceSeriesInterval interval,
      const char* fiat, const char* source, int64_t from, int64_t to, int64_t now,
      prices::PriceSeriesCandle* output, size_t capacity, size_t* count) const override;
 private:
  prices::SpotProviderOwner* spot_;
  prices::StoredPriceSeriesRepository series_;
};
}  // namespace fortis::wallet::address
