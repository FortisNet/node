#pragma once
#include <stddef.h>
#include <stdint.h>
namespace fortis::config {
enum class CoinGeckoTier : uint8_t { kDemo, kPro };
// Value-owned provider/module configuration. No process ports, data directory,
// AI credentials, metrics bearer or exchange-only keys cross this boundary.
// NativeConfig extends this data record; the implicit base reference is a view
// of existing storage, never a temporary credential buffer. Module owners copy
// this record at startup, so they do not borrow the caller's NativeConfig.
struct ProviderRuntimeConfig {
  bool attestation_required = false;
  bool ca_bundle_configured = false;
  char ca_bundle_path[1024]{};
  bool price_snapshot_enabled = false;
  bool split_price_store = false;
  CoinGeckoTier coingecko_tier = CoinGeckoTier::kDemo;
  bool coingecko_api_key_configured = false;
  bool jupiter_api_key_configured = false;
  bool tronscan_api_key_configured = false;
  // TAOSTATS_API_KEY enables POST /v1/bittensor/analytics (sent verbatim as
  // the Authorization header); unset answers the TypeScript "not configured".
  bool taostats_api_key_configured = false;
  // Provider headers allow at most 255 bytes, plus a terminator here.
  char coingecko_api_key[256]{};
  // JUPITER_API_KEY selects api.jup.ag with x-api-key; unset uses lite-api.jup.ag.
  char jupiter_api_key[256]{};
  char tronscan_api_key[256]{};
  char taostats_api_key[256]{};
  // ETHERSCAN_API_KEY enables the Etherscan V2 holder counts of non-Ethereum
  // EVM chains; ETHPLORER_API_KEY replaces Ethplorer's public "freekey"
  // (token routes). Both are optional and trimmed.
  bool etherscan_api_key_configured = false;
  bool ethplorer_api_key_configured = false;
  char etherscan_api_key[256]{};
  char ethplorer_api_key[256]{};
  // SAUCERSWAP_API_KEY (optional, trimmed) is sent as x-api-key on the
  // Hedera SaucerSwap token, price and pool reads; keyless works.
  bool saucerswap_api_key_configured = false;
  char saucerswap_api_key[256]{};
  // Chain provider keys (all optional, trimmed like optionalKey):
  // ALCHEMY_API_KEY (EVM NFTs, Alchemy transfers, the Robinhood RPC),
  // HELIUS_API_KEY (Solana history/NFTs/DAS), TRONGRID_API_KEY,
  // NEARBLOCKS_API_KEY and the GetBlock Blockbook tokens.
  bool alchemy_api_key_configured = false;
  bool helius_api_key_configured = false;
  bool trongrid_api_key_configured = false;
  bool nearblocks_api_key_configured = false;
  bool getblock_doge_token_configured = false;
  bool getblock_ltc_token_configured = false;
  bool getblock_zec_token_configured = false;
  char alchemy_api_key[256]{};
  char helius_api_key[256]{};
  char trongrid_api_key[256]{};
  char nearblocks_api_key[256]{};
  char getblock_doge_token[256]{};
  char getblock_ltc_token[256]{};
  char getblock_zec_token[256]{};
  // DASH_BLOCKBOOK_URLS: comma-separated, trimmed, at most five URLs (zod
  // url() each); they replace the Dash catalogue endpoints. Native accepts
  // https:// URLs only (the transport has no plain-HTTP path).
  size_t dash_blockbook_url_count = 0;
  char dash_blockbook_urls[5][512]{};
};
}  // namespace fortis::config
