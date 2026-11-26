#include "native_config.h"
#include "token_facts.h"
#include <string_view>
#include "chain_core.h"
#include "address_facets_internal.h"
#include "address_store.h"
#include "price_spot_cache.h"
#include "price_series_store.h"
#include "spot_provider_owner.h"
#include "unit_test_support.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>

namespace {
namespace chains = fortis::chains;
namespace address = fortis::wallet::address;
namespace prices = fortis::prices;
namespace tj = fortis::codec::tj;
namespace testing = fortis::testing;
using testing::Require;

void numeric_presence_and_values() {
  testing::JsonArena arena;
  Require(!chains::IntOrNull(nullptr) && !chains::NumOrNull(nullptr), "missing numeric value is absent");
  Require(chains::IntOrNull(tj::Null(arena.get())) == 0, "JSON null retains provider coercion to zero");
  Require(!chains::NumOrNull(tj::Bool(arena.get(), true)), "boolean is not a numeric value");
  Require(chains::NumOrNull(tj::Str(arena.get(), " 1.75 ")) == 1.75, "numeric string coercion");
  Require(chains::IntOrNull(tj::Num(arena.get(), -1.75)) == -1, "truncate toward zero");
  const auto truncated_zero = chains::IntOrNull(tj::Num(arena.get(), -0.5));
  Require(truncated_zero && *truncated_zero == 0 && !std::signbit(*truncated_zero), "integer zero is present and normalized positive");
  const auto negative_zero = chains::NumOrNull(tj::Num(arena.get(), -0.0));
  Require(negative_zero && std::signbit(*negative_zero), "number helper preserves negative zero");
  Require(!chains::NumOrNull(tj::Num(arena.get(), std::numeric_limits<double>::infinity())), "infinite is absent");
  Require(!chains::NumOrNull(tj::Num(arena.get(), std::numeric_limits<double>::quiet_NaN())), "NaN is absent");
  Require(chains::HexToInt(tj::Str(arena.get(), "0x0")) == 0, "hex zero is present");
  Require(chains::HexToInt(tj::Str(arena.get(), "0x1fffffffffffff")) == 9007199254740991.0, "safe integer boundary");
  Require(!chains::HexToInt(tj::Str(arena.get(), "0x20000000000000")) &&
              !chains::HexToInt(tj::Str(arena.get(), "0x")) && !chains::HexToInt(tj::Str(arena.get(), "0X1")), "invalid/unsafe hex is absent");
}
void high_water_outcomes() {
  testing::TemporaryKv fixture;
  address::Store store(&fixture.kv);
  auto missing = store.HighWater("account", 1000);
  Require(missing && !missing.value(), "empty history is successful absence");
  const fortis::storage::KvMutation rows[] = {
      {"transfer/cursor/account/001", nullptr, 0, 0, false},
      {"transfer/cursor/account/002", nullptr, 0, 0, false},
      {"transfer/cursor/account/999", nullptr, 0, 1000, false},
      {"transfer/cursor/empty/", nullptr, 0, 0, false}};
  Require(fixture.kv.Write(rows, 4), "seed high-water index");
  auto found = store.HighWater("account", 1000);
  Require(found && found.value() && !strcmp(found.value()->cursor, "002"), "highest live cursor is returned by value");
  auto empty = store.HighWater("empty", 1000);
  Require(empty && empty.value() && empty.value()->cursor[0] == '\0', "present empty suffix differs from absent record");
  const std::string suffix(100, 'x');
  const std::string key = "transfer/cursor/long/" + suffix;
  const fortis::storage::KvMutation long_row{key.c_str(), nullptr, 0, 0, false};
  Require(fixture.kv.Write(&long_row, 1), "seed oversized stored suffix");
  auto bounded = store.HighWater("long", 1000);
  Require(bounded && bounded.value() && strlen(bounded.value()->cursor) == 63, "retain historical 63-byte wire bound");
  const std::string too_long(2000, 'a');
  auto invalid = store.HighWater(too_long.c_str(), 1000);
  Require(!invalid && invalid.status().code() == fortis::base::StatusCode::kInvalidArgument, "invalid key is failure, not absence");
  fixture.kv.Close();
  auto failed = store.HighWater("account", 1000);
  Require(!failed && failed.status().code() == fortis::base::StatusCode::kUnavailable, "unreadable store is failure, not absence");
  Require(!strcmp(found.value()->cursor, "002"), "returned cursor outlives subsequent reads and store close");
}
void network_resolution_outcomes() {
  fortis::config::NativeConfig config;
  chains::Catalog catalog;
  Require(catalog.Init(config), "initialize network catalog");
  address::FacetDeps deps;
  deps.catalog = &catalog;
  auto impl = std::make_unique<address::WalletQueryContext>(deps);
  auto known = impl->Resolve("eth-mainnet");
  const auto* network = std::get_if<chains::Network>(&known);
  Require(network && !strcmp(network->id, "eth-mainnet"), "network is the success value");
  auto absent = impl->Resolve("unknown-network");
  const auto* failure = std::get_if<address::FacetReply>(&absent);
  Require(failure && failure->outcome == address::Outcome::kBadRequest &&
              !strcmp(failure->detail, "unknown network: unknown-network") && !failure->payload,
          "unknown network retains exact domain reply/detail");
  auto still_known = impl->Resolve("eth-mainnet");
  Require(std::holds_alternative<chains::Network>(still_known) && !strcmp(network->id, "eth-mainnet"), "failure does not poison other result values");
}
void token_fact_views() {
  namespace facts = fortis::tokens::token_facts;
  const char network_bytes[] = {'e','t','h','-','m','a','i','n','n','e','t'};
  const std::string token_bytes = "0x0000000000000000000000000000000000000001ignored";
  fortis::networks::TokenNetwork network;
  const fortis::networks::NativeNetworkConfig* seed = nullptr;
  fortis::networks::TokenRef ref;
  const auto status = facts::Resolve({network_bytes, sizeof(network_bytes)},
      {token_bytes.data(), 42}, &network, &seed, &ref);
  Require(status == facts::FactsOutcome::kOk && seed && !strcmp(network.id, "eth-mainnet") &&
      !strcmp(ref.token_id, "erc20:0x0000000000000000000000000000000000000001"),
      "network/ref views respect their bounds without a terminator");
  Require(facts::Resolve({}, {}, &network, &seed, &ref) == facts::FactsOutcome::kUnknownNetwork,
      "empty network view retains domain refusal");
  Require(facts::Resolve({network_bytes, sizeof(network_bytes)}, {}, &network, &seed, &ref) ==
      facts::FactsOutcome::kUnsupportedRef, "empty ref view retains domain refusal");
}
struct CompletionGate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool release = false;
  size_t count = 0;
  uint64_t ids[prices::kSpotProviderOwnerMaxRequests + 1]{};
  bool overflow = false;
};
void completed(void* raw, const prices::SpotProviderOwnerResult& result) {
  auto& gate = *static_cast<CompletionGate*>(raw);
  std::unique_lock<std::mutex> lock(gate.mutex);
  if (gate.count < sizeof(gate.ids) / sizeof(gate.ids[0])) gate.ids[gate.count] = result.request_id;
  else gate.overflow = true;
  ++gate.count;
  if (!gate.entered) {
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] { return gate.release; });
  }
}
uint64_t deadline() {
  timespec now{};
  Require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "monotonic deadline");
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000 + 10000;
}
void submission_outcomes() {
  static_assert(!std::is_default_constructible_v<prices::SpotProviderOwnerSubmission>, "no unclassified submission");
  static_assert(!std::is_constructible_v<prices::SpotProviderOwnerSubmission, prices::SpotSubmitTicket>, "only the owner issues tickets");
  testing::TemporaryKv fixture;
  prices::PriceSpotCache cache(fixture.kv);
  prices::PriceSeriesStore series(fixture.kv);
  fortis::config::NativeConfig config;
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/unused-no-network");
  CompletionGate gate;
  static_assert(!std::is_default_constructible_v<prices::SpotProviderOwner>);
  auto created = prices::SpotProviderOwner::Create(cache, series, config);
  Require(created.ok(), "create initialized bounded spot owner");
  auto owner = created.TakeValue();
  // Release before owner destruction even if an assertion throws.
  struct Release {
    CompletionGate& gate;
    ~Release() { std::lock_guard<std::mutex> lock(gate.mutex); gate.release = true; gate.changed.notify_all(); }
  } release{gate};
  const std::string_view empty_id{}; // invalid resolution, no outbound work
  auto submit = [&] { return owner.Submit(&empty_id, 1, {"usd", 3}, prices::SpotProviderOwnerFormat::kJson, deadline(), completed, &gate); };
  const auto invalid = owner.Submit(nullptr, 0, {"usd", 3}, prices::SpotProviderOwnerFormat::kJson, deadline(), completed, &gate);
  Require(!invalid.ticket() && invalid.status() == prices::SpotProviderOwnerSubmitStatus::kInvalidRequest, "invalid request is a rejection only");
  const auto first = submit();
  Require(first.ticket() && first.ticket()->request_id && first.status() == prices::SpotProviderOwnerSubmitStatus::kAccepted, "accepted request carries nonzero ticket");
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    Require(gate.changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate.entered; }), "first completion enters gate");
  }
  std::set<uint64_t> accepted{first.ticket()->request_id};
  for (size_t i = 0; i < prices::kSpotProviderOwnerMaxRequests; ++i) {
    const auto queued = submit();
    Require(queued.ticket() && accepted.insert(queued.ticket()->request_id).second, "queued tickets are unique");
  }
  const auto busy = submit();
  Require(!busy.ticket() && busy.status() == prices::SpotProviderOwnerSubmitStatus::kBusy, "capacity rejection cannot expose a stale ticket");
  Require(first.ticket()->request_id != 0, "rejections do not mutate earlier ticket values");
  {
    std::lock_guard<std::mutex> lock(gate.mutex); gate.release = true; gate.changed.notify_all();
  }
  owner.Stop();
  Require(!gate.overflow && gate.count == accepted.size(), "each accepted ticket completes exactly once through stop");
  Require(std::set<uint64_t>(gate.ids, gate.ids + gate.count) == accepted, "callback identities match returned tickets");
  const auto stopped = submit();
  Require(!stopped.ticket() && stopped.status() == prices::SpotProviderOwnerSubmitStatus::kNotRunning, "stopped owner rejects without a ticket");
}
} // namespace
int main() {
  testing::UnitSuite suite;
  suite.Run("numeric absence/value boundaries", numeric_presence_and_values);
  suite.Run("high-water absence/value/failure", high_water_outcomes);
  suite.Run("network value/domain failure", network_resolution_outcomes);
  suite.Run("submission rejection/ticket/lifecycle", submission_outcomes);
  suite.Run("bounded token-fact input views", token_fact_views);
  return suite.Finish("boundary_value_test");
}
