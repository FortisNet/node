#pragma once

#include "network_registry.h"
#include "token_facts.h"
#include "token_provider.h"
#include "token_ref.h"

#include <stddef.h>
#include <stdint.h>

// The ledger reads of the TypeScript Stellar, Cardano, Algorand, XRPL, Hedera,
// Sui and Bittensor drivers for the token routes:
//  - Stellar (assetPage): Horizon
//    `/assets?asset_code=&asset_issuer=&limit=1` on the 'horizon' lane;
//  - Cardano (assetSummary): Koios
//    `/asset_summary?_asset_policy=&_asset_name=` on the 'koios' lane;
//  - Algorand (tokenInsight): algod
//    `/v2/assets/{id}` on the 'algod' lane;
//  - XRPL (tokenInsight): `gateway_balances` and
//    `account_info` of the issuer, two concurrent JSON-RPC POSTs on lane
//    'rpc:{host}';
//  - Hedera: Mirror Node `/api/v1/tokens/{id}`
//    (tokenInfo, shared by tokenMetadata and tokenInsight) and the distinct
//    positive-balance page `/api/v1/tokens/{id}/balances?account.balance=
//    gt:0&limit=100&order=asc` (tokenHolders), both on 'hedera-mirror';
//  - Sui (tokenMetadata): the GraphQL
//    `FortisCoinMetadata` POST for the canonical Coin<T> type to the
//    network's GraphQL endpoints (DriverTransport.postJson with
//    `retry: true`) on lane 'rpc:{host}';
//  - Bittensor (keyed only): the Taostats dTAO
//    pool `/api/dtao/pool/latest/v1?network=finney&netuid=N&limit=1`
//    (shared by tokenMetadata, tokenMarket and tokenInsight) and the
//    market's fail-soft TAO/USD row `/api/price/latest/v1?asset=tao`, both
//    on 'taostats' with the Authorization key (token_bittensor.h); the
//    driver's 15 s working set answers either without a request.
// Stellar and Cardano answer holders (tokenHolders, the
// DriverTokenHolderSource) and insight (tokenInsight) from one page; the
// Algorand and XRPL drivers have no tokenHolders, so their read answers
// insight only. The Sui driver has neither tokenHolders nor tokenInsight:
// its read answers chainMeta only (a registered coinMetadata row proves the
// type exists on the network, not that anyone reviewed it). The Bittensor
// pool answers chainMeta, insight and the market (no tokenHolders); a
// Taostats row is an index entry, not a verification. Hedera's and
// Bittensor's reads are independent parts: a page asks only for the parts
// its sub-facets need, and each part fails on its own. Besides
// the two Mirror reads, a Hedera HTS ref has the three SaucerSwap reads of
// SaucerSwapClient (token_saucerswap.h): the token row (`/tokens/{id}`,
// shared by market, profile and the insight listing section), the 25-hour
// recent prices and the associated pools, on the 'saucerswap' lane with
// HttpClient's GET retries; a SaucerSwap 404 is a JSON null page, not a
// failure.
//
// The GETs run across the network's configured endpoints with
// DriverTransport's getJson policy (token provider owner network GETs): a 2xx
// whose JSON does not parse is HttpClient's transport failure, three
// attempts per endpoint, then the next endpoint; the Sui GraphQL POST
// (retryPost) has the same policy. The XRPL POSTs follow
// DriverTransport.jsonRpc: single attempts; a non-2xx, transport failure,
// busy lane, unparseable body or JSON null body moves on to the next
// endpoint; a truthy `error` member or a missing `result` is the answer
// (UpstreamAnswerError) and fails the read. Every failure is thrown in
// TypeScript, so the facets fail (no value is cached for a read that did not
// answer).
//
// Concurrent readers of one read share the owner's in-flight request (same
// network, endpoint and path, plus body for a POST), which stands in for the
// drivers' 100-key FlightRegistry (Stellar, Cardano), the SWR single flight
// (Algorand, XRPL, Sui, Hedera holders), the Hedera driver's tokenInfo
// coordinator single flight and the Bittensor driver's subnet and TAO/USD
// single flights; nothing settled is kept except the Bittensor driver's own
// 15 s working set. Guest-safe.
namespace fortis::tokens::token_ledger {

enum class Family : uint8_t { kNone, kStellar, kCardano, kAlgorand, kXrpl, kHedera, kSui, kBittensor };

// The family whose driver reads this ref's ledger facts: an address ref on a
// Stellar, Cardano, Algorand, XRPL, Hedera or Sui network, or a subnet
// (bittensor-alpha) ref on a Bittensor network. Whether the Bittensor driver
// exists (a Taostats key) is the caller's check (token_bittensor::Keyed).
Family FamilyOf(const networks::TokenNetwork& network, const networks::TokenRef& ref);
// DriverTokenHolderSource.supports for these families (`ref.address !==
// null` and the driver's tokenHolders exists: Stellar, Cardano and Hedera).
inline bool HoldersSupported(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  const Family family = FamilyOf(network, ref);
  return family == Family::kStellar || family == Family::kCardano || family == Family::kHedera;
}
// The driver has tokenMetadata (chainMeta) among these families: Hedera, Sui
// and Bittensor.
inline bool MetadataSupported(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  const Family family = FamilyOf(network, ref);
  return family == Family::kHedera || family == Family::kSui || family == Family::kBittensor;
}
// The driver has tokenInsight: every family here but Sui.
inline bool InsightSupported(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  const Family family = FamilyOf(network, ref);
  return family != Family::kNone && family != Family::kSui;
}

// The independent reads of one ref. Every family but Hedera and Bittensor has
// one read (kIndexPart) answering all its facets; Hedera's holder count is
// the distinct balances page (kHolderPart), and its SaucerSwap token row,
// recent prices and pools are the kSaucer* parts. Bittensor's subnet pool is
// kIndexPart and the market's TAO/USD row kTaoPricePart.
enum Part : uint8_t {
  kIndexPart = 1, kHolderPart = 2, kSaucerTokenPart = 4, kSaucerPricesPart = 8, kSaucerPoolsPart = 16,
  kTaoPricePart = 32
};
constexpr size_t kParts = 6;
constexpr uint8_t kSaucerParts = kSaucerTokenPart | kSaucerPricesPart | kSaucerPoolsPart;
// The part's slot in Pages (its bit index).
inline size_t PartSlot(uint8_t part) {
  size_t slot = 0;
  while (part > 1) { part >>= 1; ++slot; }
  return slot;
}
// The order in which TypeScript's Token360 sub-facets reach the lanes
// (market, profile, liquidity, holders, chainMeta, insight): SaucerSwap
// token, recent prices, pools, then the Mirror balances and token-info
// reads; Bittensor's tokenMarket reads the pool before the TAO/USD row.
// Stages are submitted in this order.
constexpr uint8_t kPartOrder[kParts] = {kSaucerTokenPart, kSaucerPricesPart, kSaucerPoolsPart, kHolderPart,
                                        kIndexPart, kTaoPricePart};
// The provider lane a Hedera part runs on, as a bit: 'saucerswap' or
// 'hedera-mirror' (Bittensor's two parts share 'taostats').
inline uint8_t PartLane(uint8_t part) { return part & kSaucerParts ? 1 : 2; }
// The driver reads the ledger for this ref; false when it answers without a
// read (a bare Stellar issuer has no code; a Cardano unit that is not 56..120
// hex digits; an ASA id that is not all digits; an XRPL ref with no currency
// or an invalid issuer; a Hedera ref that is not an entity id; a Sui ref that
// is not a canonical coin type; a Bittensor ref that is not a subnet netuid):
// holders and chainMeta null, insight [].
bool Indexed(const networks::TokenNetwork& network, const networks::TokenRef& ref);

// Provider slots a read uses after the caller's requests.
constexpr size_t kStageSlots = kParts;

// One read's result per part slot (PartSlot): `page` (any JSON value,
// possibly null; for XRPL the array [gateway_balances result, account_info
// result]) lives in the work arena. A failed part sets `lane_busy` when a
// failed request's last endpoint was refused by its lane (DriverTransport
// and SaucerSwapClient rethrow that LaneBusyError). A part not read stays
// failed.
struct Pages {
  const codec::tj::Value* page[kParts] = {};
  bool failed[kParts] = {true, true, true, true, true, true};
  bool lane_busy[kParts] = {};
};

// The reads of `parts` (Part bits; families other than Hedera read their one
// page for kIndexPart or kHolderPart and ignore the other bits) for an
// Indexed ref. The SaucerSwap parts read `ref.address` on the network's
// SaucerSwap host (`seed->testnet`), the prices window ending at
// work.Now(). Hedera only: the `background` parts are sent at background
// lane priority after the others (a stale sub-facet's revalidation joining
// a foreground read), and the `refused` parts are answered as a lane's
// BACKGROUND_BUSY refusal without a request (failed, lane_busy). Bittensor:
// a part the driver's working set holds answers without a request; the
// others are sent in kPartOrder, and a `background` part is refused
// (BACKGROUND_BUSY) when an earlier part of this read is already on the
// 'taostats' lane (a revalidation's TAO/USD read behind its pool read). A
// pool or TAO/USD page that answered is kept in the working set. `batch`
// holds the caller's own `extra` provider requests and has room for
// kStageSlots more: those requests run in the reads' first provider batch;
// the caller interprets and releases them.
void Read(token_facts::Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed,
          const networks::TokenRef& ref, uint8_t parts, providers::TokenFetch* batch, size_t extra, Pages* out,
          uint8_t background = 0, uint8_t refused = 0);

// tokenHolders / tokenMetadata / tokenInsight over the part's page; nullptr
// where the driver throws (or lacks the capability).
codec::tj::Value* Holders(codec::tj::Arena& arena, Family family, const codec::tj::Value* page);
codec::tj::Value* Metadata(codec::tj::Arena& arena, Family family, const codec::tj::Value* page);
codec::tj::Value* Insight(codec::tj::Arena& arena, Family family, const networks::TokenRef& ref, const codec::tj::Value* page);
// Bittensor's tokenMarket + chainNativeMarketRow over the pool page and the
// TAO/USD page (nullptr: the fail-soft leg failed). A JSON null pool is
// JSON null; a null TAO/USD price falls back to Token360's ordinary
// CoinGecko quote of the network's native coin (`coinMarket(id, 'usd')`,
// failures logged and ignored), one provider request at `background`
// priority. nullptr on arena exhaustion.
codec::tj::Value* BittensorMarket(token_facts::Work& work, const networks::NativeNetworkConfig* seed, const codec::tj::Value* pool,
                           const codec::tj::Value* price, bool background);

}  // namespace fortis::tokens::token_ledger
