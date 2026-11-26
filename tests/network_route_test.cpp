#include <cstdio>
#include "native_config.h"
// R09 unit test of GET /v1/networks generation (src/operations/network_route.cpp):
// DeriveNetworkCapabilities over every plane combination and the provider
// keys that gate a facet, the trustlines/testnet/ton/bittensor rows, the
// document builder's prices and provider labels, privacy of deployment RPC
// endpoints, and the exact buffer boundaries. The byte contract against the
// TypeScript oracle is tests/network_route_edges_test.py.
// Usage: network_route_test
#include "network_route.h"

#include <stdio.h>
#include <string.h>

using namespace fortis::config;
using namespace fortis::networks;
using namespace fortis::operations;

namespace {
int failures = 0;
int checks = 0;
void check(bool condition, const char* what) {
  ++checks;
  if (!condition) { ++failures; fprintf(stderr, "FAIL: %s\n", what); }
}

struct Env { const char* pairs[32][2]; size_t count = 0; };
const char* lookup(void* context, const char* name) {
  const Env* env = static_cast<const Env*>(context);
  for (size_t i = 0; i < env->count; ++i)
    if (strcmp(env->pairs[i][0], name) == 0) return env->pairs[i][1];
  return nullptr;
}
NativeConfig config_for(Env env) {
  NativeConfig config{};
  char error[256] = {};
  if (!LoadNativeConfig(lookup, &env, &config, error, sizeof(error)))
    fprintf(stderr, "config refused: %s\n", error);
  return config;
}

constexpr size_t kRows = 64;
const char* values[kRows][kNetworkCapabilityCount];
size_t rows = 0;
NativeNetworkRegistry registry;

int capability_index(const char* name) {
  for (size_t i = 0; i < kNetworkCapabilityCount; ++i)
    if (strcmp(kNetworkCapabilityNames[i], name) == 0) return static_cast<int>(i);
  return -1;
}
int row_index(const char* id) {
  size_t count = 0;
  const NativeNetworkConfig* list = registry.list(&count);
  for (size_t i = 0; i < count; ++i) if (strcmp(list[i].id, id) == 0) return static_cast<int>(i);
  return -1;
}
// The capabilities of `id` that are "ok", as "a,b,c" in catalogue key order.
const char* served(const char* id) {
  static char out[256];
  out[0] = '\0';
  const int row = row_index(id);
  if (row < 0) return "?";
  for (size_t j = 0; j < kNetworkCapabilityCount; ++j) {
    if (strcmp(values[row][j], "ok") == 0) {
      const size_t used = strlen(out);
      snprintf(out + used, sizeof(out) - used, "%s%s", used ? "," : "", kNetworkCapabilityNames[j]);
    } else if (strcmp(values[row][j], "none") != 0) {
      return "invalid value";
    }
  }
  return out;
}
bool derive(const NativeConfig& config, const NetworkLivePlanes& planes) {
  return DeriveNetworkCapabilities(registry, config, planes, values, kRows, &rows) && rows == 43;
}
void expect(const char* id, const char* want, const char* label) {
  char what[512];
  snprintf(what, sizeof(what), "%s: %s serves [%s], want [%s]", label, id, served(id), want);
  check(strcmp(served(id), want) == 0, what);
}
size_t count_ok(const char* capability) {
  const int j = capability_index(capability);
  size_t total = 0;
  for (size_t i = 0; i < rows; ++i) total += strcmp(values[i][j], "ok") == 0;
  return total;
}

char json[40000];
uint8_t proto[200000];
}  // namespace

int main() {
  const NativeConfig keyless = config_for(Env{});
  check(capability_index("trustlines") >= 0 && capability_index("popularTokens") == 9,
        "the ten registry.ts NONE capability names");

  // No plane: the normal image without canaries serves nothing per network.
  NetworkLivePlanes none{};
  check(derive(keyless, none), "derive without planes");
  for (size_t i = 0; i < rows; ++i)
    for (size_t j = 0; j < kNetworkCapabilityCount; ++j)
      check(strcmp(values[i][j], "none") == 0, "no plane: every capability none");

  // The address plane alone (keyless): the chain catalog flags. Overview
  // news stays off without the news ingest; token off without its plane.
  NetworkLivePlanes address{};
  address.address = true;
  check(derive(keyless, address), "derive address plane");
  expect("bitcoin", "balances,transfers,txDetail", "address keyless");
  expect("bitcoin-cash", "balances,transfers", "address keyless");
  expect("eth-mainnet", "balances,insight,txStatus,txDetail,popularTokens", "address keyless");
  expect("hyperevm", "balances,insight,txStatus,txDetail,popularTokens", "address keyless");
  expect("solana", "balances,insight,txStatus,txDetail,popularTokens", "address keyless");
  expect("xrpl", "balances,transfers,txDetail", "address keyless");
  expect("cardano", "balances,transfers,txDetail,popularTokens", "address keyless");
  expect("sui", "balances,transfers", "address keyless");
  expect("tron", "balances,transfers,popularTokens", "address keyless");
  expect("dash", "balances,transfers", "address keyless");
  expect("ton", "", "address keyless");
  expect("bittensor", "", "address keyless");
  expect("eth-sepolia", "balances,insight,txStatus,txDetail", "address keyless");
  expect("solana-devnet", "balances,insight,txStatus,txDetail", "address keyless");
  expect("xrpl-testnet", "balances,transfers,txDetail", "address keyless");
  check(count_ok("trustlines") == 0 && count_ok("token") == 0 && count_ok("news") == 0 &&
        count_ok("nfts") == 0, "address keyless: no trustlines, token, news or nfts");

  // Address + news: news on every profiled mainnet, never on a testnet or ton.
  NetworkLivePlanes overview = address;
  overview.news = true;
  check(derive(keyless, overview), "derive address+news");
  expect("bitcoin", "balances,transfers,txDetail,news", "address+news");
  expect("eth-mainnet", "balances,insight,txStatus,txDetail,news,popularTokens", "address+news");
  expect("bittensor", "news", "address+news");
  expect("ton", "", "address+news");
  expect("eth-sepolia", "balances,insight,txStatus,txDetail", "address+news");
  check(count_ok("news") == 33, "address+news: 33 profiled mainnets");
  // News without the address plane: no overview route, so no news.
  NetworkLivePlanes news_only{};
  news_only.news = true;
  check(derive(keyless, news_only) && count_ok("news") == 0, "news plane alone serves no overview");

  // Per-key gates (the same keys TypeScript reads).
  Env alchemy{}; alchemy.pairs[0][0] = "ALCHEMY_API_KEY"; alchemy.pairs[0][1] = "unit-alchemy-secret";
  alchemy.count = 1;
  const NativeConfig alchemy_config = config_for(alchemy);
  check(derive(alchemy_config, address), "derive alchemy");
  expect("eth-mainnet", "balances,transfers,nfts,insight,txStatus,txDetail,popularTokens", "alchemy");
  expect("arbitrum", "balances,transfers,insight,txStatus,txDetail,popularTokens", "alchemy");
  expect("avalanche", "balances,insight,txStatus,txDetail,popularTokens", "alchemy");
  expect("robinhood", "balances,insight,txStatus,txDetail,popularTokens", "alchemy");
  Env etherscan{}; etherscan.pairs[0][0] = "ETHERSCAN_API_KEY"; etherscan.pairs[0][1] = "unit-etherscan";
  etherscan.count = 1;
  check(derive(config_for(etherscan), address), "derive etherscan");
  expect("avalanche", "balances,transfers,insight,txStatus,txDetail,popularTokens", "etherscan");
  expect("eth-sepolia", "balances,insight,txStatus,txDetail", "etherscan");
  check(count_ok("transfers") == 13 + 22, "etherscan: 13 EVM mainnets + 22 keyless rows (TypeScript: 35)");
  Env helius{}; helius.pairs[0][0] = "HELIUS_API_KEY"; helius.pairs[0][1] = "unit-helius";
  helius.count = 1;
  check(derive(config_for(helius), address), "derive helius");
  expect("solana", "balances,transfers,nfts,insight,txStatus,txDetail,popularTokens", "helius");
  expect("solana-devnet", "balances,insight,txStatus,txDetail", "helius");
  Env taostats{}; taostats.pairs[0][0] = "TAOSTATS_API_KEY"; taostats.pairs[0][1] = "unit-taostats";
  taostats.count = 1;
  check(derive(config_for(taostats), address), "derive taostats");
  expect("bittensor", "balances,transfers", "taostats");

  // The token plane: TypeScript's `!testnet && native.coingeckoId`.
  NetworkLivePlanes token{};
  token.token = true;
  check(derive(keyless, token), "derive token");
  check(count_ok("token") == 34, "token: the 34 mainnets with a CoinGecko id");
  expect("ton", "token", "token");
  expect("bittensor", "token", "token");
  expect("eth-sepolia", "", "token");
  expect("algorand-testnet", "", "token");

  // Every plane and every key: trustlines is never served.
  Env all{};
  const char* keys[][2] = {{"ALCHEMY_API_KEY", "unit-alchemy-secret"}, {"HELIUS_API_KEY", "h"},
      {"ETHERSCAN_API_KEY", "e"}, {"TAOSTATS_API_KEY", "t"}, {"TRONSCAN_API_KEY", "s"},
      {"COINGECKO_API_KEY", "c"}, {"COINGECKO_TIER", "pro"}, {"JUPITER_API_KEY", "j"},
      {"DASH_BLOCKBOOK_URLS", "https://dash-unit-one.example/api/v2?token=unit-dash-secret,"
                              "https://dash-unit-two.example/api/v2?token=unit-dash-secret"}};
  for (const auto& pair : keys) { all.pairs[all.count][0] = pair[0]; all.pairs[all.count][1] = pair[1]; ++all.count; }
  const NativeConfig all_config = config_for(all);
  NetworkLivePlanes every{};
  every.address = every.token = every.news = every.prices_spot = every.prices_history = true;
  check(derive(all_config, every), "derive everything");
  check(count_ok("trustlines") == 0, "trustlines never served");
  expect("eth-mainnet", "balances,transfers,nfts,insight,txStatus,txDetail,token,news,popularTokens",
         "everything");
  expect("xrpl", "balances,transfers,txDetail,token,news", "everything");
  expect("dash", "balances,transfers,token,news", "everything");
  expect("hedera-testnet", "balances,transfers,txDetail", "everything");
  expect("ton", "token", "everything");

  // Documents: prices follow the planes, labels follow the keys, and no
  // deployment endpoint or key reaches either representation.
  size_t json_length = 0, proto_length = 0;
  check(BuildNetworksDocuments(registry, all_config, every, json, sizeof(json), &json_length, proto,
                               sizeof(proto), &proto_length), "build everything");
  check(strstr(json, "\"prices\":{\"spot\":true,\"history\":true,\"sources\":{\"coingecko\":\"pro\","
                     "\"jupiter\":\"keyed\",\"vestige\":\"keyless\"}}}") != nullptr, "prices, keyed labels");
  check(strstr(json, "unit-alchemy-secret") == nullptr && strstr(json, "unit-dash-secret") == nullptr &&
        strstr(json, "dash-unit-one") == nullptr && strstr(json, "alchemy.com") == nullptr,
        "no deployment endpoint in JSON");
  check(strstr(json, "\"id\":\"robinhood\",\"family\":\"evm\",\"evmChainId\":4663,\"testnet\":false,"
                     "\"rpcUrls\":[\"https://rpc.mainnet.chain.robinhood.com\"]") != nullptr &&
        strstr(json, "\"rpcUrls\":[\"https://dash.atomicwallet.io/api/v2\"]") != nullptr,
        "catalogue literals for overridden networks");
  bool proto_clean = true;
  static const char* const kSecrets[] = {"unit-alchemy-secret", "unit-dash-secret", "dash-unit-one",
                                         "alchemy.com"};
  for (const char* secret : kSecrets)
    for (size_t i = 0; i + strlen(secret) <= proto_length; ++i)
      if (memcmp(proto + i, secret, strlen(secret)) == 0) proto_clean = false;
  check(proto_clean, "no deployment endpoint in protobuf");

  NetworkLivePlanes spot_only{};
  spot_only.prices_spot = true;
  check(BuildNetworksDocuments(registry, keyless, spot_only, json, sizeof(json), &json_length, proto,
                               sizeof(proto), &proto_length) &&
        strstr(json, "\"prices\":{\"spot\":true,\"history\":false,\"sources\":{\"coingecko\":\"keyless\","
                     "\"jupiter\":\"lite\",\"vestige\":\"keyless\"}}}") != nullptr,
        "spot without history, keyless labels");
  // Protobuf: PriceCapabilityInfo is field 2 and is the document's tail;
  // spot=true is "08 01" and history=false is omitted.
  const uint8_t tail[] = {0x12, 0x08, 0x01, 0x1a};
  bool found_tail = false;
  for (size_t i = 0; i + 5 <= proto_length; ++i)
    if (proto[i] == tail[0] && proto[i + 2] == 0x08 && proto[i + 3] == 0x01 && proto[i + 4] == tail[3])
      found_tail = true;
  check(found_tail, "protobuf prices: spot true, history omitted");

  // Exact buffer boundaries: JSON needs its length + 1 (the NUL), protobuf
  // exactly its length.
  check(BuildNetworksDocuments(registry, keyless, none, json, sizeof(json), &json_length, proto,
                               sizeof(proto), &proto_length), "build keyless");
  const size_t exact_json = json_length, exact_proto = proto_length;
  check(exact_json > 15000 && exact_json < 32767, "keyless JSON size inside the 32 KiB buffer");
  check(BuildNetworksDocuments(registry, keyless, none, json, exact_json + 1, &json_length, proto,
                               exact_proto, &proto_length) && json_length == exact_json &&
        proto_length == exact_proto, "exact capacities succeed");
  check(!BuildNetworksDocuments(registry, keyless, none, json, exact_json, &json_length, proto,
                                sizeof(proto), &proto_length), "JSON one byte short refused");
  check(!BuildNetworksDocuments(registry, keyless, none, json, sizeof(json), &json_length, proto,
                                exact_proto - 1, &proto_length), "protobuf one byte short refused");
  check(!DeriveNetworkCapabilities(registry, keyless, none, values, 42, &rows),
        "derivation capacity below the catalogue refused");

  printf("network route unit: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
