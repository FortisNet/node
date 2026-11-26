#include "network_registry.h"

#include <assert.h>
#include <string.h>

using namespace fortis::networks;

int main() {
  NativeNetworkRegistry registry;
  size_t count = 0;
  const NativeNetworkConfig* networks = registry.list(&count);
  assert(networks != nullptr && count == 43);
  for (size_t i = 0; i < count; ++i) {
    const NativeNetworkConfig& network = networks[i];
    assert(network.id && strlen(network.id) <= 64);
    assert(network.family && strlen(network.family) <= 23);
    assert(network.rpc_url_count <= 5 && network.graphql_url_count <= 3);
    for (size_t j = 0; j < network.rpc_url_count; ++j)
      assert(network.rpc_urls[j] && strlen(network.rpc_urls[j]) <= 255);
    for (size_t j = 0; j < network.graphql_url_count; ++j)
      assert(network.graphql_urls[j] && strlen(network.graphql_urls[j]) <= 255);
    if (network.native_symbol) assert(strlen(network.native_symbol) <= 8);
    if (network.native_coingecko_id)
      assert(strlen(network.native_coingecko_id) <= 64);
    for (size_t j = 0; j < kNetworkCapabilityCount; ++j) {
      assert(strlen(kNetworkCapabilityNames[j]) <= 15);
      assert(strlen(network.capabilities[j]) <= 4);
      assert(strcmp(network.capabilities[j], "none") == 0);
    }
  }
  assert(strcmp(kNetworkCapabilityNames[0], "balances") == 0);
  assert(strcmp(kNetworkCapabilityNames[kNetworkCapabilityCount - 1],
                "popularTokens") == 0);
  return 0;
}
