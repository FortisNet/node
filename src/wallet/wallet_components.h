#pragma once
#include "address_facets.h"
#include "chain_id_set.h"

namespace fortis::wallet::address {
class WalletValuation {
 public:
  explicit WalletValuation(const FacetDeps& deps) : deps_(deps) {}
  Value* SpotPrices(Arena& arena, const char* const* ids, size_t count);
  void Enrich(const Network& network, Value* const* records, size_t count,
              const char* override_token, const char* override_source, int64_t now, Arena& arena);
 private:
  const FacetDeps& deps_;
};
class TransferIngestor {
 public:
  explicit TransferIngestor(WalletQueryContext& context) : context_(context) {}
  bool HasSource(const Network& network) const;
  void EnsureFresh(const char* account_id, const Network& network, const char* address, int64_t max_age_ms);
 private:
  WalletQueryContext& context_;
};
class TransferRepairService {
 public:
  explicit TransferRepairService(WalletQueryContext& context) : context_(context) {}
  size_t BackfillFiat(Arena& arena, const Network& network, const char* account_id,
                      const char* token_id, const char* price_source_id, int64_t now_ms);
  WalletQueryService::RepairResult RepairOperationCandles(Arena& arena,
      base::FunctionRef<bool(const char*, int64_t, int64_t, bool)> history);
  size_t PruneTransfers(Arena& arena);
 private:
  WalletQueryContext& context_;
};
class WalletOverviewBuilder {
 public:
  explicit WalletOverviewBuilder(WalletQueryContext& context) : context_(context) {}
  FacetReply Build(Arena& arena, const char* network_id);
  bool OverviewTokens(const Network& network, Arena& arena, Value** rows, chains::Error* error);
  Value* OverviewHolderCount(const Network& network, const char* token_id, const char* address,
                             Arena& arena, bool etherscan_uncovered);
  bool HolderFetch(const Network& network, const char* token_id, const char* address, Arena& arena,
                    Value** value, chains::Error* error, bool check_memo);
  bool EtherscanHolderUnsupported(double chain_id);
  void RememberEtherscanHolderUnsupported(double chain_id);
 private:
  WalletQueryContext& context_;
  chains::BoundedChainIdSet<16> etherscan_unsupported_;
};
}  // namespace fortis::wallet::address
