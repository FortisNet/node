#include "address_store.h"
#include "fake_seams.h"
#include "sync_scope.h"
#include "unit_test_support.h"

#include <string>
#include <vector>
#include <new>
#include <utility>

namespace {
thread_local std::size_t fail_array_bytes = 0;
struct ArrayFailure {
  explicit ArrayFailure(std::size_t bytes) : previous(std::exchange(fail_array_bytes, bytes)) {}
  ~ArrayFailure() { fail_array_bytes = previous; }
  std::size_t previous;
};
}

// Isolated test-binary replacement: real nothrow storage failure, no production
// allocation switch. Other threads and other allocation sizes are unchanged.
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  if (fail_array_bytes && bytes == fail_array_bytes) return nullptr;
  try { return ::operator new[](bytes); }
  catch (...) { return nullptr; }
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  ::operator delete[](pointer);
}

namespace {
namespace address = fortis::wallet::address;
namespace sync = fortis::wallet::sync;
namespace tj = fortis::codec::tj;
namespace testing = fortis::testing;
using testing::Require;
constexpr int64_t kNow = 1'800'000'000'000LL;
constexpr char kDevice[] = "01ARZ3NDEKTSV4RRFFQ69G0000";
constexpr char kOtherDevice[] = "01ARZ3NDEKTSV4RRFFQ69G0001";

struct Fixture {
  testing::TemporaryKv database;
  testing::ManualClock clock{kNow};
  address::Store store{&database.kv};
  sync::Scope scope;
  address::Account first, second;

  Fixture() {
    Require(store.Touch("eth-mainnet", "account:first", clock.NowMs(), &first), "create first account");
    Require(store.Touch("bitcoin", "account:second", clock.NowMs(), &second), "create second account");
    Require(scope.Start(&store), "start scope");
  }

  void Watch(const char* device, const char* topic, const char* account, int64_t expiry) {
    testing::JsonArena arena;
    Require(store.WriteWatch(device, topic, account, expiry, clock.NowMs(), arena.get()), "write watch");
  }

  std::vector<std::string> Accounts(const char* device = kDevice) {
    testing::JsonArena arena;
    sync::ScopeAccount* accounts = nullptr;
    size_t count = 0;
    Require(scope.Accounts(device, clock.NowMs(), arena.get(), &accounts, &count), "resolve scope");
    std::vector<std::string> result;
    for (size_t i = 0; i < count; ++i) result.emplace_back(accounts[i].id);
    return result;
  }
};

void account_resolution_and_isolation() {
  Fixture fixture;
  const int64_t expires = kNow + 3600000;
  fixture.Watch(kDevice, "portfolio:first", fixture.first.id, expires);
  fixture.Watch(kDevice, "address:eth-mainnet:account:first", nullptr, expires);
  fixture.Watch(kDevice, "address:bitcoin:account:second", nullptr, expires);
  fixture.Watch(kDevice, "address:missing:account", nullptr, expires);
  fixture.Watch(kDevice, "unrelated:topic", nullptr, expires);
  fixture.Watch(kOtherDevice, "portfolio:second", fixture.second.id, expires);
  const auto accounts = fixture.Accounts();
  Require(accounts.size() == 2, "portfolio and address topics deduplicate the same account");
  Require((accounts[0] == fixture.first.id || accounts[1] == fixture.first.id) &&
              (accounts[0] == fixture.second.id || accounts[1] == fixture.second.id), "colons in addresses survive topic splitting");
  const auto other = fixture.Accounts(kOtherDevice);
  Require(other.size() == 1 && other[0] == fixture.second.id, "one device never inherits another device's cached accounts");
  Require(fixture.Accounts("unwatched-device").empty(), "existing accounts are private without watches");
}

void cache_expiry_and_topic_invalidation() {
  Fixture fixture;
  const int64_t expires = kNow + 3600000;
  Require(fixture.Accounts().empty(), "initial scope empty");
  fixture.Watch(kDevice, "portfolio:first", fixture.first.id, expires);
  Require(fixture.Accounts().size() == 1, "empty result is not cached");
  fixture.Watch(kDevice, "portfolio:second", fixture.second.id, expires);
  fixture.clock.Advance(sync::kScopeCacheMs - 1);
  Require(fixture.Accounts().size() == 1, "plain repository write retains non-empty scope until expiry");
  fixture.clock.Advance(1);
  Require(fixture.Accounts().size() == 2, "four-minute cache expires at exact boundary");

  Fixture listener;
  listener.Watch(kDevice, "portfolio:first", listener.first.id, expires);
  Require(listener.Accounts().size() == 1, "prime listener case");
  testing::JsonArena arena;
  Require(listener.store.RememberTopic(kDevice, "address:bitcoin:account:second", listener.clock.NowMs(), arena.get()),
          "write topic through service API");
  Require(listener.Accounts().size() == 2, "topic listener invalidates scope immediately without advancing clock");
}

double expiry_of(address::Store& store, const char* topic, int64_t now) {
  testing::JsonArena arena;
  tj::Value* rows = nullptr;
  Require(store.ListDeviceWatches(kDevice, now, arena.get(), &rows) && tj::IsArray(rows), "read watch records");
  for (uint32_t i = 0; i < rows->count; ++i) {
    if (!tj::StringEquals(tj::Get(rows->items[i], "topic"), topic)) continue;
    const auto* expires = tj::Get(rows->items[i], "expiresAt");
    Require(tj::IsNumber(expires), "watch has expiry");
    return expires->number;
  }
  throw std::runtime_error("watch missing");
}

void renewal_boundary_and_expired_watches() {
  Fixture fixture;
  constexpr char topic[] = "portfolio:first";
  const int64_t expiry = kNow + sync::kWatchRenewBeforeMs + 1;
  fixture.Watch(kDevice, topic, fixture.first.id, expiry);
  fixture.Watch(kDevice, "expired", fixture.second.id, kNow);
  Require(fixture.Accounts().size() == 1, "expired watch cannot grant an account");
  Require(expiry_of(fixture.store, topic, kNow) == expiry, "watch outside five-minute margin is not renewed");
  fixture.clock.Advance(1);
  fixture.scope.Invalidate(kDevice);
  Require(fixture.Accounts().size() == 1, "renewal preserves authorized scope");
  Require(expiry_of(fixture.store, topic, fixture.clock.NowMs()) == fixture.clock.NowMs() + 15 * 60000,
          "watch at renewal boundary receives a full fifteen-minute lease");
}

void ownership_failure_and_lifecycle() {
  Fixture fixture;
  fixture.Watch(kDevice, "portfolio:first", fixture.first.id, kNow + 3600000);
  {
    testing::JsonArena first_arena;
    sync::ScopeAccount* accounts = nullptr;
    size_t count = 0;
    Require(fixture.scope.Accounts(kDevice, kNow, first_arena.get(), &accounts, &count) && count == 1,
            "prime cached blob from temporary arena");
  }
  testing::JsonArena copied;
  sync::ScopeAccount* accounts = nullptr;
  size_t count = 0;
  Require(fixture.scope.Accounts(kDevice, kNow, copied.get(), &accounts, &count) && count == 1, "hit outlives first arena");
  fixture.scope.Invalidate(kDevice);
  Require(!strcmp(accounts[0].id, fixture.first.id) && !strcmp(accounts[0].address, "account:first"),
          "returned rows outlive invalidation of internal cached blob");
  Require(fixture.Accounts().size() == 1, "re-prime scope");
  uint8_t bytes[1];
  tj::Arena tiny{bytes, sizeof(bytes), 0, true};
  sync::ScopeAccount* failed = accounts;
  size_t failed_count = 99;
  Require(!fixture.scope.Accounts(kDevice, kNow, tiny, &failed, &failed_count) && !failed && !failed_count,
          "arena exhaustion yields no partial authorization result");
  Require(fixture.Accounts().size() == 1, "failed copy does not poison cached scope");
  Require(!fixture.scope.Start(&fixture.store), "double start refused");
  fixture.scope.Stop();
  fixture.scope.Stop();
  Require(!fixture.scope.Accounts(kDevice, kNow, copied.get(), &failed, &failed_count), "stopped scope refuses reads");
  Require(!fixture.scope.Start(nullptr) && fixture.scope.Start(&fixture.store), "scope can be restarted with a valid store");
  Require(fixture.Accounts().size() == 1, "restart resolves durable watches");
}

void account_scratch_exhaustion() {
  Fixture fixture;
  address::Account output{};
  snprintf(output.id, sizeof(output.id), "%s", "unchanged");
  for (const auto bytes : {72U * 1024, 86U * 1024}) {
    const ArrayFailure failure(bytes);
    Require(fixture.store.Find("eth-mainnet", "account:first", kNow, &output) == address::ReadStatus::kError,
            "both outer and nested account scratch failure are explicit errors");
    Require(!strcmp(output.id, "unchanged"), "failed lookup publishes no partial account");
  }
  {
    const ArrayFailure failure(86 * 1024);
    Require(fixture.store.ById(fixture.first.id, kNow, &output) == address::ReadStatus::kError,
            "direct account scratch failure is an explicit error");
  }
  Require(fixture.store.Find("eth-mainnet", "account:first", kNow, &output) == address::ReadStatus::kFound &&
              !strcmp(output.id, fixture.first.id), "subsequent lookup retains acknowledged account");
}

}  // namespace

int main() {
  testing::UnitSuite suite;
  suite.Run("account resolution/isolation", account_resolution_and_isolation);
  suite.Run("cache boundary/invalidation", cache_expiry_and_topic_invalidation);
  suite.Run("watch renewal boundary", renewal_boundary_and_expired_watches);
  suite.Run("arena ownership/failure/lifecycle", ownership_failure_and_lifecycle);
  suite.Run("account scratch exhaustion", account_scratch_exhaustion);
  return suite.Finish("sync_scope_unit_test");
}
