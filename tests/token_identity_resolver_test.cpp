#include "token_identity_resolver.h"

#include "token_comparison_store.h"
#include "token_facts_internal.h"
#include "unit_test_support.h"

#include <string>
#include <vector>

namespace {
namespace facts = fortis::tokens::token_facts;
namespace tj = fortis::codec::tj;
namespace swr = fortis::tokens::token_swr;
namespace store = fortis::tokens::token_store;
using fortis::testing::Require;
using Log = std::vector<std::string>;

std::string json(const tj::Value* value) {
  uint8_t data[8192];
  tj::OutBuffer out{data, sizeof(data)};
  Require(tj::Stringify(value, out) && out.ok, "serialize fixture value");
  return {reinterpret_cast<char*>(data), out.used};
}
tj::Value* parse(tj::Arena& arena, const std::string& text) {
  tj::Value* value = nullptr;
  value = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  Require(value != nullptr, "parse fixture");
  return value;
}
struct Registry final : facts::TokenIdentityRegistry {
  Log& log;
  std::string coin, symbol, name, patched_coin;
  store::FieldState symbol_state = store::FieldState::kUnchanged;
  store::FieldState name_state = store::FieldState::kUnchanged;
  int patches = 0;
  explicit Registry(Log& events) : log(events) {}
  void FindCoin(tj::Arena&, const fortis::networks::TokenNetwork&, const fortis::networks::TokenRef&,
                int64_t now, char (&out)[facts::kCoinIdBytes]) override {
    Require(now == 101 || now == 102, "live lookup samples its own current time");
    log.push_back("registry");
    snprintf(out, sizeof(out), "%s", coin.c_str());
  }
  void Patch(const fortis::networks::TokenNetwork&, const fortis::networks::TokenRef&,
             const store::IdentityPatch& patch, int64_t now) override {
    Require(now == 101 || now == 102, "patch retains lookup timestamp");
    log.push_back("patch"); ++patches;
    symbol_state = patch.symbol.state; name_state = patch.name.state;
    symbol.assign(patch.symbol.text ? patch.symbol.text : "", patch.symbol.length);
    name.assign(patch.name.text ? patch.name.text : "", patch.name.length);
    patched_coin.assign(patch.coingecko_id.text, patch.coingecko_id.length);
  }
};
struct Provider final : facts::TokenIdentityProvider {
  Log& log;
  bool begin_ok = true, ok = true;
  std::string body = "{}", requested;
  explicit Provider(Log& events) : log(events) {}
  bool BeginLookup() override { log.push_back("reserve"); return begin_ok; }
  bool Search(tj::Arena& arena, const char* symbol, tj::Value** hits) override {
    log.push_back("search"); requested = symbol;
    if (ok) *hits = parse(arena, body);
    return ok;
  }
  bool Contract(tj::Arena& arena, const char* platform, const char* address, tj::Value** info) override {
    log.push_back("contract"); requested = std::string(platform) + ":" + address;
    if (ok) *info = parse(arena, body);
    return ok;
  }
};
struct Cache final : facts::TokenIdentityCache {
  Log& log;
  mutable int64_t now = 100;
  swr::State state = swr::State::kMissing;
  bool held = false;
  std::string body = "null", stored, key;
  explicit Cache(Log& events) : log(events) {}
  int64_t Now() const override { log.push_back("now"); return ++now; }
  swr::State Read(const char* input, int64_t time, tj::Arena& arena, tj::Value** value) override {
    Require(time == 101, "cache read uses initial time");
    log.push_back("read"); key = input;
    if (state != swr::State::kMissing) *value = parse(arena, body);
    return state;
  }
  void RequestRefresh(const char*, const fortis::networks::TokenNetwork&, const fortis::networks::TokenRef&, int64_t time) override {
    Require(time == 101, "refresh admission uses initial time"); log.push_back("refresh");
  }
  bool MissHeld(const char*, int64_t time) override {
    Require(time == 101, "miss gate uses initial time"); log.push_back("held"); return held;
  }
  void Store(const char*, int64_t time, const tj::Value* value) override {
    Require(time == 103, "cache write samples time after lookup"); log.push_back("store"); stored = json(value);
  }
  void HoldMiss(const char*, int64_t time) override {
    Require(time == 103, "failure hold samples time after lookup"); log.push_back("hold");
  }
};
struct Fixture {
  fortis::testing::JsonArena arena;
  Log log;
  Registry registry{log};
  Provider provider{log};
  Cache cache{log};
  facts::TokenIdentityResolver resolver{arena.get(), registry, provider, cache};
  fortis::networks::TokenNetwork network;
  fortis::networks::TokenRef ref;
  Fixture() {
    network.id = "eth-mainnet"; network.family = "evm";
    ref.kind = fortis::networks::TokenRefKind::kErc20; ref.has_address = true;
    snprintf(ref.address, sizeof(ref.address), "0x0000000000000000000000000000000000000001");
    snprintf(ref.token_id, sizeof(ref.token_id), "erc20:%s", ref.address);
  }
  facts::Resolution Resolve(const char* hint = nullptr) {
    facts::Resolution out;
    resolver.Resolve(network, ref, hint, &out);
    return out;
  }
};
void precedence() {
  Fixture direct;
  direct.ref.has_coingecko_id = true;
  snprintf(direct.ref.coingecko_id, sizeof(direct.ref.coingecko_id), "direct");
  auto out = direct.Resolve("hint");
  Require(out.has_coin && !strcmp(out.coin_id, "direct") && out.source == facts::ResolutionSource::kRef && direct.log.empty(), "explicit ref skips every dependency");
  Fixture registry;
  registry.registry.coin = "stored";
  out = registry.Resolve("hint");
  Require(!strcmp(out.coin_id, "stored") && out.source == facts::ResolutionSource::kRegistry, "registry precedes provider and hint");
  Require(registry.log == Log{"now", "read", "held", "now", "registry", "now", "store"}, "registry hit skips provider reservation and preserves time/order");
  Require(registry.cache.stored == "{\"coinId\":\"stored\",\"source\":\"registry\",\"ambiguous\":false}", "cache wire shape/key order unchanged");
}
void cached_and_stale() {
  for (auto state : {swr::State::kFresh, swr::State::kStale}) {
    Fixture f; f.cache.state = state;
    f.cache.body = R"({"coinId":"cached","source":"search","ambiguous":true})";
    auto out = f.Resolve("hint");
    Require(out.has_coin && !strcmp(out.coin_id, "cached") && out.ambiguous, "cached answer takes precedence");
    Require(f.log == (state == swr::State::kStale ? Log{"now", "read", "refresh"} : Log{"now", "read"}), "only stale answers request refresh");
  }
  Fixture malformed; malformed.cache.state = swr::State::kFresh; malformed.cache.body = "{}";
  auto out = malformed.Resolve("  FALL-BACK  ");
  Require(out.has_coin && !strcmp(out.coin_id, "fall-back") && malformed.log == Log{"now", "read"}, "cached absence falls back to hint without live fetch");
}
void search_ambiguity() {
  Fixture f; f.ref.kind = fortis::networks::TokenRefKind::kSymbol;
  snprintf(f.ref.symbol, sizeof(f.ref.symbol), "SS");
  f.provider.body = R"([{"id":"prefix","symbol":"SSX"},{"id":"first","symbol":"ß"},{"id":"second","symbol":"ss"}])";
  auto out = f.Resolve();
  Require(out.has_coin && !strcmp(out.coin_id, "first") && out.source == facts::ResolutionSource::kSearch && out.ambiguous, "Unicode case folding/exact matches retain first result and ambiguity");
  Require(f.provider.requested == "SS" && f.registry.patches == 0, "search does not patch registry");
  Require(f.log == Log{"now", "read", "held", "now", "registry", "reserve", "search", "now", "store"}, "search flow and clock sequence");
  Fixture absent; absent.ref.kind = fortis::networks::TokenRefKind::kSymbol;
  snprintf(absent.ref.symbol, sizeof(absent.ref.symbol), "OTHER"); absent.provider.body = "[]";
  out = absent.Resolve("hint");
  Require(out.source == facts::ResolutionSource::kHint && absent.cache.stored == "{\"coinId\":null,\"source\":null,\"ambiguous\":false}", "successful unresolved search is cached separately from hint");
}
void contract_patching() {
  Fixture f; f.provider.body = R"({"id":"coin","symbol":"  C`OIN  ","name":"  Coin  "})";
  auto out = f.Resolve();
  Require(out.has_coin && !strcmp(out.coin_id, "coin") && out.source == facts::ResolutionSource::kContract, "contract resolves identity");
  Require(f.registry.patches == 1 && f.registry.patched_coin == "coin" && f.registry.symbol == "COIN" && f.registry.name == "Coin", "patch sanitizes strings and retains coin");
  Require(f.provider.requested == "ethereum:0x0000000000000000000000000000000000000001", "network platform and contract address forwarded");
  Fixture invalid; invalid.provider.body = R"({"id":"coin","symbol":true,"name":"Coin"})";
  out = invalid.Resolve();
  Require(out.has_coin && invalid.registry.patches == 0, "truthy non-string skips patch but preserves identity");
  Fixture nulls; nulls.provider.body = R"({"id":"coin","symbol":false,"name":null})";
  out = nulls.Resolve();
  Require(out.has_coin && nulls.registry.symbol_state == store::FieldState::kNull && nulls.registry.name_state == store::FieldState::kNull, "falsy metadata emits explicit null patches");
}
void failures_and_hint_bounds() {
  Fixture held; held.cache.held = true;
  auto out = held.Resolve("\xC2\xA0" "MiXeD-COIN" "\xC2\xA0");
  Require(out.source == facts::ResolutionSource::kHint && !strcmp(out.coin_id, "mixed-coin") && held.log == Log{"now", "read", "held"}, "held miss allows Unicode-trimmed hint without provider");
  Fixture failed; failed.provider.ok = false;
  out = failed.Resolve("hint");
  Require(out.has_coin && failed.cache.stored.empty() && failed.log.back() == "hold", "provider failure holds miss before applying hint");
  for (const auto& hint : {std::string("-bad"), std::string("bad_id"), std::string(101, 'a'), std::string()}) {
    Fixture f; f.cache.held = true;
    Require(!f.Resolve(hint.c_str()).has_coin, "reject invalid/oversized hints");
  }
  Fixture bound; bound.cache.held = true;
  Require(bound.Resolve(std::string(100, 'a').c_str()).has_coin, "100-byte hint is accepted");
}
void empty_platform_and_reservation() {
  Fixture f; f.network.id = "unknown";
  auto out = f.Resolve("hint");
  Require(out.source == facts::ResolutionSource::kHint && f.log == Log{"now", "read", "held", "now", "registry", "reserve", "now", "store"}, "no-platform lookup still reserves workspace before cached absence");
  Fixture oom; oom.network.id = "unknown"; oom.provider.begin_ok = false;
  out = oom.Resolve("hint");
  Require(out.has_coin && oom.log.back() == "hold" && oom.cache.stored.empty(), "failed reservation remains failure even with no platform");
  Fixture key; const std::string huge(1100, 'x'); key.network.id = huge.c_str();
  out = key.Resolve("hint");
  Require(out.source == facts::ResolutionSource::kHint && key.log.empty(), "oversized cache key skips dependencies and keeps hint fallback");
}

void live_and_absent_results() {
  Fixture live; live.registry.coin = "background";
  tj::Value* value = nullptr;
  Require(live.resolver.ResolveLive(live.network, live.ref, &value), "background lookup succeeds");
  Require(live.log == Log{"now", "registry"} && json(value) == "{\"coinId\":\"background\",\"source\":\"registry\",\"ambiguous\":false}", "live entry bypasses cache without recursing through foreground policy");
  Fixture absent; absent.provider.body = R"({"id":""})";
  auto out = absent.Resolve();
  Require(!out.has_coin && absent.registry.patches == 0 && absent.log.back() == "store", "empty provider id is successful cached absence, not failure hold");
  Fixture oversized;
  oversized.provider.body = std::string("{\"id\":\"") + std::string(facts::kCoinIdBytes, 'x') + "\"}";
  out = oversized.Resolve();
  Require(!out.has_coin && oversized.registry.patches == 0 && oversized.log.back() == "store", "oversized provider id is absent without truncation or patch");
}

struct Queue final : facts::TokenRevalidationQueue {
  int calls = 0;
  facts::Revalidation task;
  bool Enqueue(const facts::Revalidation& value) override {
    ++calls; task = value; return false;
  }
};
void production_cache_and_registry() {
  fortis::testing::TemporaryKv fixture;
  fortis::tokens::TokenComparisonStore registry(&fixture.kv);
  store::IdentityPatch patch;
  patch.symbol = {store::FieldState::kValue, "ß", strlen("ß")};
  patch.coingecko_id = {store::FieldState::kValue, "registered", 10};
  Require(registry.PatchIdentity("eth-mainnet", "aaa", patch, 1), "seed registry symbol");
  patch.coingecko_id = {store::FieldState::kValue, "second", 6};
  Require(registry.PatchIdentity("eth-mainnet", "bbb", patch, 1), "seed later matching symbol");
  swr::Cache cache(&fixture.kv, nullptr);
  std::vector<uint8_t> scratch(65536);
  facts::Env env; env.kv = &fixture.kv; env.swr = &cache;
  Queue queue; env.revalidation_queue = &queue;
  facts::Work work; work.env = &env; work.arena = tj::GrowingArena(8 * 1024 * 1024); work.scratch = scratch.data(); work.scratch_capacity = scratch.size();
  struct ArenaOwner { tj::Arena& arena; ~ArenaOwner() { tj::FreeArena(arena); } } owner{work.arena};
  fortis::networks::TokenNetwork network; network.id = "eth-mainnet"; network.family = "evm";
  fortis::networks::TokenRef ref; ref.kind = fortis::networks::TokenRefKind::kSymbol;
  snprintf(ref.token_id, sizeof(ref.token_id), "symbol:SS"); snprintf(ref.symbol, sizeof(ref.symbol), "SS");
  facts::Resolution resolution;
  facts::internal::ResolveTokenIdentity(work, network, ref, nullptr, &resolution);
  Require(resolution.has_coin && !strcmp(resolution.coin_id, "registered"), "production registry scan applies Unicode upper-case match");
  const char* key = "tref:eth-mainnet:symbol:SS";
  auto* value = parse(work.arena, R"({"coinId":"stale","source":"search","ambiguous":false})");
  const int64_t now = cache.Now();
  Require(cache.Write(key, {1, 60000}, now - 1000, value, scratch.data(), scratch.size(), nullptr), "seed stale resolution");
  facts::internal::ResolveTokenIdentity(work, network, ref, nullptr, &resolution);
  Require(!strcmp(resolution.coin_id, "stale") && queue.calls == 1 && queue.task.kind == facts::RevalidationKind::kResolution && !strcmp(queue.task.network.data(), network.id), "stale adapter preserves queued task and cached answer");
  Require(cache.BeginRevalidate(key, cache.Now()), "rejected queue releases revalidation claim without backoff");
  cache.EndRevalidate(key, cache.Now(), false, false);
}
}  // namespace
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("ref and registry precedence", precedence);
  suite.Run("cached answers and stale refresh", cached_and_stale);
  suite.Run("symbol exact matching and ambiguity", search_ambiguity);
  suite.Run("contract identity and patch semantics", contract_patching);
  suite.Run("failure holds and hint grammar", failures_and_hint_bounds);
  suite.Run("reservation and cache-key boundaries", empty_platform_and_reservation);
  suite.Run("live lookup and successful absence", live_and_absent_results);
  suite.Run("production registry and cache adapters", production_cache_and_registry);
  return suite.Finish("token_identity_resolver_test");
}
