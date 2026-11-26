#pragma once
#include "chain_driver.h"

namespace fortis::chains {
// Ordered borrowed capability sets. The default set is the deployment's native
// families; tests can supply concrete fakes without starting provider threads.
class ChainRegistry {
 public:
  ChainRegistry();
  constexpr ChainRegistry(const ChainDriver* const* drivers, size_t driver_count,
                          const TransferSource* const* sources, size_t source_count,
                          const TransactionView* const* views, size_t view_count)
      : drivers_(drivers), driver_count_(driver_count), sources_(sources),
        source_count_(source_count), views_(views), view_count_(view_count) {}
  const ChainDriver* DriverFor(const config::ProviderRuntimeConfig& config, Family family) const;
  const TransferSource* SourceFor(const config::ProviderRuntimeConfig& config, const Network& network) const;
  const TransactionView* ViewFor(Family family) const;
 private:
  const ChainDriver* const* drivers_;
  size_t driver_count_;
  const TransferSource* const* sources_;
  size_t source_count_;
  const TransactionView* const* views_;
  size_t view_count_;
};
}  // namespace fortis::chains
