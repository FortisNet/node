#pragma once
#include "chain_core.h"
#include "price_series_repository.h"

namespace fortis::wallet::address {
class WalletSpotReader {
 public:
  virtual chains::Value* Read(chains::Arena& arena, const char* const* ids, size_t count) = 0;
 protected:
  ~WalletSpotReader() = default;
};
using WalletSeriesReader = prices::PriceSeriesReader;
}  // namespace fortis::wallet::address
