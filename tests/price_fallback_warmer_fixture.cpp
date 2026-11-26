#include "price_spot_service.h"
#include "clock.h"
#include "price_core_ids.h"
#include "price_spot_cache.h"
#include "provider_execution_coordinator.h"
#include "unit_test_support.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }
struct Starter final : fortis::prices::PriceSpotOwnerStarter {
  const char* port = nullptr;
  const char* ca = nullptr;
  bool Start(fortis::prices::SpotProviderOwnerStartup& target) override {
    return target.StartRedirected("localhost", port, ca, allow_loopback, nullptr);
  }
};
template <class Predicate>
void until(Predicate predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (!predicate() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(predicate());
}
}  // namespace
int main(int argc, char** argv) try {
  assert(argc == 3);
  using namespace fortis::prices;
  fortis::testing::TemporaryKv fixture;
  fortis::config::ProviderRuntimeConfig config{};
  config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", argv[2]);
  config.price_snapshot_enabled = true;
  config.split_price_store = true;
  fortis::providers::ProviderExecutionCoordinator coordinator(false);
  assert(coordinator.Start());
  Starter starter;
  starter.port = argv[1];
  starter.ca = argv[2];
  PriceSpotServiceOptions options;
  options.start_owner = &starter;
  auto made = PriceSpotService::Create(config, {fixture.kv, coordinator}, options);
  assert(made.ok());
  auto service = made.TakeValue();
  auto* cache = service.cache();
  auto fresh = [&](const char* id, int64_t after) {
    const auto row = cache->Lookup("usd", id, now_ms(), true);
    return row.state == PriceSpotLookupState::kFresh && row.value.as_of_ms >= after && row.value.price == 2.5 &&
           strcmp(row.value.provider, "defillama") == 0 && row.value.provider_observed_at_ms > 0;
  };
  until([&] {
    for (const auto* id : kPriceCoreIds)
      if (!fresh(id, 1)) return false;
    return true;
  });
  const std::string_view contract = "eip155:1:erc20:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48";
  std::atomic<unsigned> callbacks{0};
  const auto submitted = service.route_context()->owner->Submit(
      &contract, 1, "usd", SpotProviderOwnerFormat::kJson, fortis::base::SystemClock().MonotonicMs() + 10000,
      [](void* raw, const SpotProviderOwnerResult& result) {
        assert(result.status == SpotProviderOwnerResultStatus::kComplete);
        static_cast<std::atomic<unsigned>*>(raw)->fetch_add(1);
      },
      &callbacks);
  assert(submitted.ticket());
  until([&] { return callbacks.load() == 1 && fresh(contract.data(), 1); });
  // Advance only the entries toward expiry. No foreground request follows:
  // both warmers must refresh independently while CoinGecko is still paused.
  const int64_t threshold = now_ms();
  PriceSpotQuoteInput aged[kPriceCoreCount + 1]{};
  for (size_t i = 0; i < kPriceCoreCount; ++i)
    aged[i] = {kPriceCoreIds[i], 1, 0, 0, false, false, threshold - 15000, 0};
  aged[kPriceCoreCount] = {contract.data(), 1, 0, 0, false, false, threshold - 15000, 0};
  assert(cache->StoreProviderBatch("usd", "defillama", aged, kPriceCoreCount + 1, nullptr, 0, true,
                                   threshold - 15000) == PriceSpotStoreStatus::kStored);
  until([&] {
    for (const auto& row : aged)
      if (!fresh(row.source_id, threshold)) return false;
    return true;
  });
  service.Stop();
  assert(coordinator.in_flight_count() == 0 && coordinator.Stop());
  puts("Core and contract warmers used fresh fallback during CoinGecko cooldown, then drained cleanly.");
} catch (...) {
  fputs("Price fallback warmer fixture failed with an exception.\n", stderr);
  return 1;
}
