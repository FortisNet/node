#pragma once

#include "chain_core.h"
#include "chain_transport.h"

#include <stddef.h>
#include <stdint.h>

// Per-family contracts of the address, transaction and network routes (the
// TypeScript ChainDriver, TransferSource and TxDetailView) and the borrowed
// capabilities registered by the native composition.
//
// Every result is a token_json document in `ctx.arena`, shaped and key-ordered
// exactly as the TypeScript object, with these representation rules:
//  - Date fields are epoch-ms numbers (ts, confirmedAt, fiatSourceTs); the
//    facets/stores serialize them as TypeScript does;
//  - `undefined` members are absent; `null` members are JSON null;
//  - chain amounts are decimal strings, never doubles.
//
// Failure is a false return with `error` set (chain_core.h Error): a thrown
// RangeError is kRange, a LaneBusyError kLaneBusy, anything else kUpstream.
namespace fortis::chains {

// ---- drivers (ChainDriver) ----------------------------------------------------

// balances(network, address, tokens): a BalancesResult
//   {native:{balanceRaw, decimals, symbol, source}, tokens:[{tokenId,
//    balanceRaw, decimals, symbol, source, displayUnits?}], reference?:
//    {kind, value, hash?}, finality?}
// `address` is already canonicalAddress()'d; `tokens` is the tracked list.
// insight(network, address): {exists, kind, txCount, txCountFloor}.
// latestBlockHeight(network): `*has_height` false = null.
// txStatus(network, hash): a TxDetail {status, blockNumber, confirmedAt,
// detail, raw}, or *result = nullptr for null (the chain does not know it).
// nfts(network, address): NftItem[] {nftId, contractAddress, tokenId,
// standard, name, collectionName, imageUrl, flags}.

// EvmDriver.txStatus after its JSON-RPC answers (pure; exposed for the unit
// test): `tx` is a non-null eth_getTransactionByHash result; `receipt` and
// `tip` (eth_getTransactionReceipt / eth_blockNumber results) are read only
// when tx.blockNumber is set.
bool EvmTxStatusOf(Arena& arena, const Network& network, Value* tx, Value* receipt, const Value* tip,
                   Value** result, Error* error);
// SolanaDriver.txStatus after its getSignatureStatuses answer `res` (pure;
// exposed for the unit test). *result = nullptr when the cluster has not
// seen the signature.
bool SolanaTxStatusOf(Arena& arena, Value* res, Value** result, Error* error);


// ---- transfer sources (TransferSource) ----------------------------------------

// The outcome of one fetch: either `unavailable` (SourceUnavailable, a known
// coverage gap — not an error) or records + state + backfillComplete.
struct SourceResult {
  bool unavailable = false;
  char reason[512]{};
  // NormalizedTransfer[] (see chain_core.h NewTransfer for the record shape).
  Value* records = nullptr;
  // Provider-native cursor state persisted for the next delta sync.
  Value* state = nullptr;
  bool backfill_complete = false;
};

// Borrowed capabilities: composition owns concrete objects. Never delete through
// these bases. Optional reads are advertised explicitly, without RTTI.
class NftReader {
 public:
  virtual bool ReadNfts(Context&, const Network&, const char*, Value**, Error*) const = 0;
  virtual bool SupportsNfts(const Network&) const = 0;
 protected:
  ~NftReader() = default;
};
class ChainDriver {
 public:
  virtual Family family() const = 0;
  virtual bool Registered(const config::ProviderRuntimeConfig&) const = 0;
  virtual bool Balances(Context&, const Network&, const char*, const char* const*, size_t, Value**, Error*) const = 0;
  virtual bool HasInsight() const { return false; }
  virtual bool HasLatestHeight() const { return false; }
  virtual bool HasTxStatus() const { return false; }
  virtual const NftReader* Nfts(const config::ProviderRuntimeConfig&) const { return nullptr; }
  virtual bool Insight(Context&, const Network&, const char*, Value**, Error* error) const { error->Upstream("unsupported insight"); return false; }
  virtual bool LatestHeight(Context&, const Network&, bool*, double*, Error* error) const { error->Upstream("unsupported height"); return false; }
  virtual bool TxStatus(Context&, const Network&, const char*, Value**, Error* error) const { error->Upstream("unsupported transaction"); return false; }
 protected:
  ~ChainDriver() = default;
};
class TransferSource {
 public:
  virtual Family family() const = 0;
  virtual const char* Key() const = 0;
  virtual bool Registered(const config::ProviderRuntimeConfig&) const = 0;
  virtual bool Supports(const config::ProviderRuntimeConfig&, const Network&) const = 0;
  virtual bool Fetch(Context&, const Network&, const char*, const Value*, double, SourceResult*, Error*) const = 0;
 protected:
  ~TransferSource() = default;
};
class TransactionView {
 public:
  virtual Family family() const = 0;
  virtual bool Fetch(Context&, const Network&, const char*, Value**, Error*) const = 0;
  virtual bool IsFinal(const Value*, const Network&) const = 0;
 protected:
  ~TransactionView() = default;
};

// fetch(network, address, state, backfillDepth). `state` is this source's
// stored cursor blob (an object, {} when none).


// The request target fetch() sends for a URL string whose variable bytes are
// all in its query (Etherscan, Blockscout): WHATWG URL parsing ends the query
// at a raw '#' (the rest is a fragment, never sent) and percent-encodes the
// special-query set (C0 controls, space, '"', '\'', '<', '>', DEL and every
// non-ASCII byte) in uppercase hex. TypeScript interpolates addresses and
// hashes unencoded, so a value holding such a byte reaches the wire this way.
const char* FetchWirePath(Arena& a, const char* path);

// ---- transaction views -------------------------------------------------------

// fetch(network, hash): the app's per-family detail JSON, or *detail =
// nullptr when the chain does not know the hash (yet).
// isFinal(detail, network); null = conservative false.


// The TypeScript feeRanges + projectInclusion over one parsed
// /v1/fees/mempool-blocks body (pure; exposed for the unit test): the first
// next block whose fee floor `fee_rate` clears ({blocks, beyond: false}),
// else {blocks: the number of ranges, beyond: true}; *has_blocks false (no
// expectedBlocks) when no entry carries a finite feeRange value.
void ProjectInclusionOf(double fee_rate, const Value* body, bool* has_blocks, double* blocks, bool* beyond);

// ---- Tron token holdings (walletTokens) ---------------------------------------

// TronscanClient.walletTokens(address): the structured holdings array.
bool TronscanWalletTokens(Context& ctx, const char* address, Value** holdings, Error* error);
// Its projection of one parsed /api/account/wallet body (pure; exposed for
// the unit test).
bool TronWalletHoldings(Arena& arena, const Value* body, Value** holdings, Error* error);

// ---- token holder counts (driver tokenHolders, used by the overview) ----------

// TronDriver.tokenHolders (assigned only with a TronScan key; false
// `*present` otherwise) and CardanoDriver.tokenHolders: the
// ChainTokenHolderCount {count, atLeast, source} or JSON null.
bool TronTokenHoldersPresent(const config::ProviderRuntimeConfig& config);
bool TronTokenHolders(Context& ctx, const Network& network, const char* address, Value** count, Error* error);
bool CardanoTokenHolders(Context& ctx, const Network& network, const char* address, Value** count, Error* error);

// ---- dispatch -------------------------------------------------------------------

// The registered native drivers/sources/views (chain_registry.cpp). Lookups
// return null when the family has none in this deployment.
const ChainDriver* DriverFor(const config::ProviderRuntimeConfig& config, Family family);
const TransferSource* SourceFor(const config::ProviderRuntimeConfig& config, const Network& network);
const TransactionView* ViewFor(Family family);
// Whether any source is registered for the family (capability probe input).
bool AnySourceSupports(const config::ProviderRuntimeConfig& config, const Network& network);

}  // namespace fortis::chains
