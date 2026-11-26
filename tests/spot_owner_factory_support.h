#include "native_config.h"
#pragma once

#include "function_ref.h"
#include "spot_provider_owner.h"

#include <stdio.h>
#include <stdlib.h>

namespace fortis::testing {

inline prices::SpotProviderOwner StartTestSpotOwner(
    prices::PriceSpotCache& cache, prices::PriceSeriesStore& series,
    const config::NativeConfig& config, providers::ProviderExecutionCoordinator* coordinator,
    base::Clock* wall_clock, base::FunctionRef<bool(prices::SpotProviderOwnerStartup&)> start,
    discovery::CoinPopularity* popularity = nullptr) {
  struct Starter final : base::Starter<prices::SpotProviderOwnerStartup> {
    explicit Starter(base::FunctionRef<bool(prices::SpotProviderOwnerStartup&)> callback) : callback(callback) {}
    bool Start(prices::SpotProviderOwnerStartup& startup) override { return callback(startup); }
    base::FunctionRef<bool(prices::SpotProviderOwnerStartup&)> callback;
  } starter(start);
  prices::SpotProviderOwnerOptions options;
  options.coordinator = coordinator;
  options.wall_clock = wall_clock;
  options.popularity = popularity;
  options.start_owner = &starter;
  auto made = prices::SpotProviderOwner::Create(cache, series, config, options);
  if (!made) {
    fprintf(stderr, "spot owner fixture initialization: %s\n", made.status().message());
    abort();
  }
  return made.TakeValue();
}

} // namespace fortis::testing
