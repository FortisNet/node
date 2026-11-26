#include "chain_registry.h"
#include "chain_family_drivers.h"
#include <iterator>

// The native composition of the driver registry, transfer sources and tx
// views (the TypeScript service's buildApp): one driver, at most one transfer
// source and at most one detail view per family, each present only when the
// deployment carries the key TypeScript requires for it.
namespace fortis::chains {
namespace {

const ChainDriver* const kDrivers[] = {
    &kEvmDriver, &kSolanaDriver, &kStellarDriver, &kAlgorandDriver, &kHederaDriver,
    &kCardanoDriver, &kXrplDriver, &kEsploraDriver, &kBitcoinCashDriver, &kDashDriver,
    &kDogecoinDriver, &kLitecoinDriver, &kZcashDriver, &kTronDriver, &kVechainDriver,
    &kSuiDriver, &kNearDriver, &kCosmosDriver, &kAptosDriver, &kIcpDriver, &kBittensorDriver,
};

const TransferSource* const kSources[] = {
    &kEvmSource, &kSolanaSource, &kXrplSource, &kStellarSource, &kAlgorandSource,
    &kHederaSource, &kEsploraSource, &kBitcoinCashSource, &kDashSource, &kDogecoinSource,
    &kLitecoinSource, &kZcashSource, &kCardanoSource, &kTronSource, &kAptosSource,
    &kSuiSource, &kVechainSource, &kCosmosSource, &kNearSource, &kIcpSource,
    &kBittensorSource,
};

const TransactionView* const kViews[] = {
    &kEvmView, &kXrplView, &kAlgorandView, &kSolanaView, &kStellarView, &kCardanoView,
    &kHederaView, &kEsploraView, &kDogecoinView, &kLitecoinView, &kZcashView,
};

}  // namespace

const ChainDriver* ChainRegistry::DriverFor(const config::ProviderRuntimeConfig& config, Family family) const {
  for (size_t i = 0; i < driver_count_; ++i) {
    const ChainDriver* driver = drivers_[i];
    if (driver->family() == family && driver->Registered(config)) return driver;
  }
  return nullptr;
}

const TransferSource* ChainRegistry::SourceFor(const config::ProviderRuntimeConfig& config, const Network& network) const {
  for (size_t i = 0; i < source_count_; ++i) {
    const TransferSource* source = sources_[i];
    if (source->family() == network.family && source->Registered(config))
      return source->Supports(config, network) ? source : nullptr;
  }
  return nullptr;
}

bool AnySourceSupports(const config::ProviderRuntimeConfig& config, const Network& network) {
  return SourceFor(config, network) != nullptr;
}

const TransactionView* ChainRegistry::ViewFor(Family family) const {
  for (size_t i = 0; i < view_count_; ++i) {
    const TransactionView* view = views_[i];
    if (view->family() == family) return view;
  }
  return nullptr;
}

ChainRegistry::ChainRegistry()
    : ChainRegistry(kDrivers, std::size(kDrivers),
                    kSources, std::size(kSources),
                    kViews, std::size(kViews)) {}

// Compatibility for catalog/capability probes. All dispatch uses one policy.
const ChainDriver* DriverFor(const config::ProviderRuntimeConfig& config, Family family) {
  return ChainRegistry().DriverFor(config, family);
}
const TransferSource* SourceFor(const config::ProviderRuntimeConfig& config, const Network& network) {
  return ChainRegistry().SourceFor(config, network);
}
const TransactionView* ViewFor(Family family) { return ChainRegistry().ViewFor(family); }

}  // namespace fortis::chains
