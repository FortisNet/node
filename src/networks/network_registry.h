#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::networks {

constexpr size_t kNetworkCapabilityCount = 10;
struct NativeNetworkConfig {
  const char* id;
  const char* family;
  bool has_evm_chain_id;
  uint32_t evm_chain_id;
  bool testnet;
  bool has_required_confirmations;
  uint32_t required_confirmations;
  const char* const* rpc_urls;
  size_t rpc_url_count;
  const char* const* graphql_urls;
  size_t graphql_url_count;
  const char* native_symbol;
  uint32_t native_decimals;
  const char* native_coingecko_id;
  const char* capabilities[kNetworkCapabilityCount];
};

// Network seed metadata generated from contracts/network-catalog.json. The
// generated capability column is the NONE default ("none" everywhere); GET
// /v1/networks publishes values derived at startup from the planes this
// process serves (DeriveNetworkCapabilities, network_route.h).
class NativeNetworkRegistry {
 public:
  const NativeNetworkConfig* list(size_t* count) const;
};

extern const char* const kNetworkCapabilityNames[kNetworkCapabilityCount];

}  // namespace fortis::networks
