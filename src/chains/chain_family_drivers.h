#pragma once
#include "chain_driver.h"

namespace fortis::chains {
class VechainDriver final : public ChainDriver {
 public:
  constexpr VechainDriver() = default;
  Family family() const override { return Family::kVechain; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                      size_t token_count, Value** result, Error* error) const override;
};
class VechainSource final : public TransferSource {
 public:
  constexpr VechainSource() = default;
  Family family() const override { return Family::kVechain; }
  const char* Key() const override { return "thor-logs-v2"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                   double backfill_depth, SourceResult* result, Error* error) const override;
};
class HederaDriver final : public ChainDriver {
 public:
  constexpr HederaDriver() = default;
  Family family() const override { return Family::kHedera; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const override;
};
class HederaSource final : public TransferSource {
 public:
  constexpr HederaSource() = default;
  Family family() const override { return Family::kHedera; }
  const char* Key() const override { return "mirror"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const override;
};
class HederaView final : public TransactionView {
 public:
  constexpr HederaView() = default;
  Family family() const override { return Family::kHedera; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class BittensorDriver final : public ChainDriver {
 public:
  constexpr BittensorDriver() = default;
  Family family() const override { return Family::kBittensor; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                  Value** result, Error* error) const override;
};
class BittensorSource final : public TransferSource {
 public:
  constexpr BittensorSource() = default;
  Family family() const override { return Family::kBittensor; }
  const char* Key() const override { return "taostats"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state, double backfill_depth,
               SourceResult* result, Error* error) const override;
};
class StellarDriver final : public ChainDriver {
 public:
  constexpr StellarDriver() = default;
  Family family() const override { return Family::kStellar; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const override;
};
class StellarSource final : public TransferSource {
 public:
  constexpr StellarSource() = default;
  Family family() const override { return Family::kStellar; }
  const char* Key() const override { return "horizon"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const override;
};
class StellarView final : public TransactionView {
 public:
  constexpr StellarView() = default;
  Family family() const override { return Family::kStellar; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class SuiDriver final : public ChainDriver {
 public:
  constexpr SuiDriver() = default;
  Family family() const override { return Family::kSui; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                  Value** result, Error* error) const override;
};
class SuiSource final : public TransferSource {
 public:
  constexpr SuiSource() = default;
  Family family() const override { return Family::kSui; }
  const char* Key() const override { return "sui-graphql-v6"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
               double backfill_depth, SourceResult* result, Error* error) const override;
};
class NearDriver final : public ChainDriver {
 public:
  constexpr NearDriver() = default;
  Family family() const override { return Family::kNear; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                   size_t token_count, Value** result, Error* error) const override;
};
class NearSource final : public TransferSource {
 public:
  constexpr NearSource() = default;
  Family family() const override { return Family::kNear; }
  const char* Key() const override { return "nearblocks"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                double backfill_depth, SourceResult* result, Error* error) const override;
};
class EvmDriver final : public ChainDriver, public NftReader {
 public:
  constexpr EvmDriver() = default;
  Family family() const override { return Family::kEvm; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                  size_t token_count, Value** result, Error* error) const override;
  bool HasInsight() const override { return true; }
  bool Insight(Context& ctx, const Network& network, const char* address, Value** result, Error* error) const override;
  bool HasLatestHeight() const override { return true; }
  bool LatestHeight(Context& ctx, const Network& network, bool* has_height, double* height,
                       Error* error) const override;
  bool HasTxStatus() const override { return true; }
  bool TxStatus(Context& ctx, const Network& network, const char* hash, Value** result, Error* error) const override;
  const NftReader* Nfts(const config::ProviderRuntimeConfig& config) const override;
  bool ReadNfts(Context& ctx, const Network& network, const char* address, Value** items, Error* error) const override;
  bool SupportsNfts(const Network& network) const override;
};
class EvmSource final : public TransferSource {
 public:
  constexpr EvmSource() = default;
  Family family() const override { return Family::kEvm; }
  const char* Key() const override { return "etherscan"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const override;
};
class SolanaView final : public TransactionView {
 public:
  constexpr SolanaView() = default;
  Family family() const override { return Family::kSolana; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class SolanaDriver final : public ChainDriver, public NftReader {
 public:
  constexpr SolanaDriver() = default;
  Family family() const override { return Family::kSolana; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                     Value** result, Error* error) const override;
  bool HasInsight() const override { return true; }
  bool Insight(Context& ctx, const Network& network, const char* address, Value** result, Error* error) const override;
  bool HasTxStatus() const override { return true; }
  bool TxStatus(Context& ctx, const Network& network, const char* hash, Value** result, Error* error) const override;
  const NftReader* Nfts(const config::ProviderRuntimeConfig& config) const override;
  bool ReadNfts(Context& ctx, const Network& network, const char* address, Value** items, Error* error) const override;
  bool SupportsNfts(const Network& network) const override;
};
class SolanaSource final : public TransferSource {
 public:
  constexpr SolanaSource() = default;
  Family family() const override { return Family::kSolana; }
  const char* Key() const override { return "helius"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const override;
};
class IcpDriver final : public ChainDriver {
 public:
  constexpr IcpDriver() = default;
  Family family() const override { return Family::kIcp; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                  Value** result, Error* error) const override;
};
class IcpSource final : public TransferSource {
 public:
  constexpr IcpSource() = default;
  Family family() const override { return Family::kIcp; }
  const char* Key() const override { return "icp-ledger-api"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
               double backfill_depth, SourceResult* result, Error* error) const override;
};
class CardanoDriver final : public ChainDriver {
 public:
  constexpr CardanoDriver() = default;
  Family family() const override { return Family::kCardano; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                     Value** result, Error* error) const override;
};
class CardanoSource final : public TransferSource {
 public:
  constexpr CardanoSource() = default;
  Family family() const override { return Family::kCardano; }
  const char* Key() const override { return "koios"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const override;
};
class AlgorandDriver final : public ChainDriver {
 public:
  constexpr AlgorandDriver() = default;
  Family family() const override { return Family::kAlgorand; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const override;
};
class AlgorandSource final : public TransferSource {
 public:
  constexpr AlgorandSource() = default;
  Family family() const override { return Family::kAlgorand; }
  const char* Key() const override { return "indexer"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const override;
};
class AlgorandView final : public TransactionView {
 public:
  constexpr AlgorandView() = default;
  Family family() const override { return Family::kAlgorand; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class EvmView final : public TransactionView {
 public:
  constexpr EvmView() = default;
  Family family() const override { return Family::kEvm; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail_out,
                    Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class EsploraDriver final : public ChainDriver {
 public:
  constexpr EsploraDriver() = default;
  Family family() const override { return Family::kBitcoin; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                     Value** result, Error* error) const override;
};
class BlockbookDriver final : public ChainDriver {
 public:
  explicit constexpr BlockbookDriver(Family family) : family_(family) {}
  Family family() const override { return family_; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                       Value** result, Error* error) const override;
 private:
  Family family_;
};
class EsploraSource final : public TransferSource {
 public:
  constexpr EsploraSource() = default;
  Family family() const override { return Family::kBitcoin; }
  const char* Key() const override { return "esplora"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const override;
};
class BlockbookSource final : public TransferSource {
 public:
  explicit constexpr BlockbookSource(Family family) : family_(family) {}
  Family family() const override { return family_; }
  const char* Key() const override { return "blockbook"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                    double backfill_depth, SourceResult* result, Error* error) const override;
 private:
  Family family_;
};
class CosmosDriver final : public ChainDriver {
 public:
  constexpr CosmosDriver() = default;
  Family family() const override { return Family::kCosmos; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*,
                     size_t, Value** result, Error* error) const override;
};
class CosmosSource final : public TransferSource {
 public:
  constexpr CosmosSource() = default;
  Family family() const override { return Family::kCosmos; }
  const char* Key() const override { return "cosmos-lcd-v2"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const override;
};
class XrplDriver final : public ChainDriver {
 public:
  constexpr XrplDriver() = default;
  Family family() const override { return Family::kXrpl; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const override;
};
class XrplSource final : public TransferSource {
 public:
  constexpr XrplSource() = default;
  Family family() const override { return Family::kXrpl; }
  const char* Key() const override { return "xrpl"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const override;
};
class XrplView final : public TransactionView {
 public:
  constexpr XrplView() = default;
  Family family() const override { return Family::kXrpl; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class EsploraView final : public TransactionView {
 public:
  constexpr EsploraView() = default;
  Family family() const override { return Family::kBitcoin; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class BlockbookView final : public TransactionView {
 public:
  explicit constexpr BlockbookView(Family family) : family_(family) {}
  Family family() const override { return family_; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
 private:
  Family family_;
};
class CardanoView final : public TransactionView {
 public:
  constexpr CardanoView() = default;
  Family family() const override { return Family::kCardano; }
  bool Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const override;
  bool IsFinal(const Value* detail, const Network& network) const override;
};
class TronDriver final : public ChainDriver {
 public:
  constexpr TronDriver() = default;
  Family family() const override { return Family::kTron; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                   size_t token_count, Value** result, Error* error) const override;
};
class TronSource final : public TransferSource {
 public:
  constexpr TronSource() = default;
  Family family() const override { return Family::kTron; }
  const char* Key() const override { return "tronscan"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state, double backfill_depth,
                SourceResult* result, Error* error) const override;
};
class AptosDriver final : public ChainDriver {
 public:
  constexpr AptosDriver() = default;
  Family family() const override { return Family::kAptos; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                    Value** result, Error* error) const override;
};
class AptosSource final : public TransferSource {
 public:
  constexpr AptosSource() = default;
  Family family() const override { return Family::kAptos; }
  const char* Key() const override { return "aptos-indexer"; }
  bool Registered(const config::ProviderRuntimeConfig& config) const override;
  bool Supports(const config::ProviderRuntimeConfig& config, const Network& network) const override;
  bool Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                 double backfill_depth, SourceResult* result, Error* error) const override;
};

extern const VechainDriver kVechainDriver;
extern const VechainSource kVechainSource;
extern const HederaDriver kHederaDriver;
extern const HederaSource kHederaSource;
extern const HederaView kHederaView;
extern const BittensorDriver kBittensorDriver;
extern const BittensorSource kBittensorSource;
extern const StellarDriver kStellarDriver;
extern const StellarSource kStellarSource;
extern const StellarView kStellarView;
extern const SuiDriver kSuiDriver;
extern const SuiSource kSuiSource;
extern const NearDriver kNearDriver;
extern const NearSource kNearSource;
extern const EvmDriver kEvmDriver;
extern const EvmSource kEvmSource;
extern const SolanaView kSolanaView;
extern const SolanaDriver kSolanaDriver;
extern const SolanaSource kSolanaSource;
extern const IcpDriver kIcpDriver;
extern const IcpSource kIcpSource;
extern const CardanoDriver kCardanoDriver;
extern const CardanoSource kCardanoSource;
extern const AlgorandDriver kAlgorandDriver;
extern const AlgorandSource kAlgorandSource;
extern const AlgorandView kAlgorandView;
extern const EvmView kEvmView;
extern const EsploraDriver kEsploraDriver;
extern const BlockbookDriver kBitcoinCashDriver;
extern const BlockbookDriver kDashDriver;
extern const BlockbookDriver kDogecoinDriver;
extern const BlockbookDriver kLitecoinDriver;
extern const BlockbookDriver kZcashDriver;
extern const EsploraSource kEsploraSource;
extern const BlockbookSource kBitcoinCashSource;
extern const BlockbookSource kDashSource;
extern const BlockbookSource kDogecoinSource;
extern const BlockbookSource kLitecoinSource;
extern const BlockbookSource kZcashSource;
extern const CosmosDriver kCosmosDriver;
extern const CosmosSource kCosmosSource;
extern const XrplDriver kXrplDriver;
extern const XrplSource kXrplSource;
extern const XrplView kXrplView;
extern const EsploraView kEsploraView;
extern const BlockbookView kDogecoinView;
extern const BlockbookView kLitecoinView;
extern const BlockbookView kZcashView;
extern const CardanoView kCardanoView;
extern const TronDriver kTronDriver;
extern const TronSource kTronSource;
extern const AptosDriver kAptosDriver;
extern const AptosSource kAptosSource;

}  // namespace fortis::chains
