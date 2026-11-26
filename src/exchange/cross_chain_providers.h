#pragma once

#include "clock.h"
#include "cross_chain_support.h"
#include "threading.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::chains { class ChainTransport; }

// The six cross-chain provider adapters, ports of the TypeScript ones:
// LI.FI, Squid (+ AxelarScan GMP enrichment), MAYAChain, NEAR Intents 1Click,
// SimpleSwap v3 and bridge.vet. Each runs synchronously on a cross-chain
// worker thread and reaches its upstream only through chains::Transport, the
// process provider coordinator's lanes (pacing, breaker, bounded queue,
// HttpClient's GET retry policy and the 512 KiB response bound). Requests
// carry the deployment's credentials as headers; responses are parsed as
// JSON.parse would and normalized into the same objects, in the same key
// order, the TypeScript adapters build. The service never holds wallet keys
// and never signs or broadcasts.
namespace fortis::exchange::cross_chain {

constexpr size_t kMaxMayaRoots = 5;

// The wallet's request, after the API boundary's zod transforms (chain ids
// and plain-text fields trimmed). Strings are arena values.
struct Endpoint {
  const Value* chain_id = nullptr;
  const Value* token = nullptr;
  uint32_t decimals = 0;
  const Value* address = nullptr;
};
struct QuoteRequest {
  Endpoint from;
  Endpoint to;
  const Value* amount_in = nullptr;
  uint32_t slippage_bps = 50;
  bool prepare = false;
};
struct StatusRequest {
  const Value* provider_id = nullptr;
  const Value* tx_hash = nullptr;
  const Value* from_chain_id = nullptr;
  const Value* to_chain_id = nullptr;
  const Value* to_token = nullptr;
  const Value* to_symbol = nullptr;
  bool has_to_decimals = false;
  uint32_t to_decimals = 0;
  const Value* provider_quote_id = nullptr;
  const Value* deposit_memo = nullptr;
  const Value* bridge_id = nullptr;
};

// Deployment settings: the provider credentials, Maya's
// opt-in and its MAYANode roots (apiRoots(): deduplicated, trailing slashes
// removed, one per origin).
struct ProviderSettings {
  bool lifi_configured = false;
  char lifi_api_key[256]{};
  bool squid_configured = false;
  char squid_integrator_id[256]{};
  bool simpleswap_configured = false;
  char simpleswap_api_key[256]{};
  bool near_jwt_configured = false;
  char near_authorization[256]{};  // "Bearer <jwt>"
  bool maya_execution_enabled = false;
  size_t maya_root_count = 0;
  char maya_roots[kMaxMayaRoots][512]{};
};

// A provider's thrown error, as the facet distinguishes it.
enum class Failure : uint8_t {
  kNone,
  kError,     // any other rejection (UpstreamError, validation Error, ...)
  kLaneBusy,  // LaneBusyError: the 429 back-pressure answer
};
struct Outcome {
  Failure failure = Failure::kNone;
  uint32_t retry_after_ms = 0;
  bool breaker_open = false;
  static Outcome Ok() { return Outcome{}; }
};

// NEAR Intents' in-memory token catalog (five minutes on the service clock),
// held in its own growing arena; a refresh swaps in a new arena.
struct NearToken {
  const Value* asset_id;
  const Value* blockchain;  // lowercased
  const Value* symbol;
  uint32_t decimals;
  const Value* contract;  // nullptr: null contract address
};
struct NearCatalog {
  fortis::base::Mutex mutex;
  bool loaded = false;
  double expires_at = 0;
  codec::tj::Arena arena{};
  NearToken* tokens = nullptr;
  size_t count = 0;
};
constexpr size_t kNearCatalogMax = 5000;

struct ProviderContext {
  chains::ChainTransport* transport = nullptr;
  const ProviderSettings* settings = nullptr;
  // The service clock: NowMs (epoch ms) and SleepMs; tests replace it.
  fortis::base::Clock* clock = nullptr;
  // Monotonic deadline of the whole provider operation.
  uint64_t deadline_ms = 0;
  NearCatalog* near_catalog = nullptr;
  double Now() const { return static_cast<double>(clock->NowMs()); }
};

// ---- the provider registry ---------------------------------------------------
//
// One concrete ExchangeProvider object per provider, defined next to its adapter
// (cross_chain_aggregators.cpp and the per-provider deposit files), is everything the
// service knows about it: the directory row, when the deployment has it
// configured, and its quote/status entry points. Adding a provider is that
// object plus one line in kProviders below; the directory order, the id
// lookup and every per-provider array size derive from the table.
// Borrowed provider behavior; credentials and operation state stay in the
// explicit context. Composition owns concrete adapters, never a base pointer.
class ExchangeProvider {
 public:
  virtual const char* Id() const = 0;
  virtual const char* Name() const = 0;
  virtual const char* Website() const = 0;
  virtual const char* Custody() const = 0;
  virtual bool Configured(const ProviderSettings&) const { return true; }
  virtual bool Trading(const ProviderSettings&) const { return true; }
  virtual Outcome Quote(ProviderContext&, Arena&, const QuoteRequest&, Value**) const = 0;
  virtual Outcome Status(ProviderContext&, Arena&, const StatusRequest&, Value**) const = 0;
 protected:
  ~ExchangeProvider() = default;
};

class LifiProvider final : public ExchangeProvider {
 public:
  constexpr LifiProvider() = default;
  const char* Id() const override { return "lifi"; }
  const char* Name() const override { return "LI.FI"; }
  const char* Website() const override { return "https://li.fi/"; }
  const char* Custody() const override { return "non-custodial"; }
  bool Configured(const ProviderSettings& settings) const override;
  Outcome Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const override;
  Outcome Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const override;
};

class SquidProvider final : public ExchangeProvider {
 public:
  constexpr SquidProvider() = default;
  const char* Id() const override { return "squid"; }
  const char* Name() const override { return "Squid"; }
  const char* Website() const override { return "https://www.squidrouter.com/"; }
  const char* Custody() const override { return "non-custodial"; }
  bool Configured(const ProviderSettings& settings) const override;
  Outcome Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const override;
  Outcome Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const override;
};

class NearIntentsProvider final : public ExchangeProvider {
 public:
  constexpr NearIntentsProvider() = default;
  const char* Id() const override { return "near-intents"; }
  const char* Name() const override { return "NEAR Intents 1Click"; }
  const char* Website() const override { return "https://near-intents.org/"; }
  const char* Custody() const override { return "custodial"; }
  Outcome Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const override;
  Outcome Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const override;
};

class MayaProvider final : public ExchangeProvider {
 public:
  constexpr MayaProvider() = default;
  const char* Id() const override { return "maya"; }
  const char* Name() const override { return "MAYAChain"; }
  const char* Website() const override { return "https://www.mayaprotocol.com/"; }
  const char* Custody() const override { return "non-custodial"; }
  bool Trading(const ProviderSettings& settings) const override;
  Outcome Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const override;
  Outcome Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const override;
};

class SimpleSwapProvider final : public ExchangeProvider {
 public:
  constexpr SimpleSwapProvider() = default;
  const char* Id() const override { return "simpleswap"; }
  const char* Name() const override { return "SimpleSwap"; }
  const char* Website() const override;
  const char* Custody() const override { return "custodial"; }
  bool Configured(const ProviderSettings& settings) const override;
  Outcome Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const override;
  Outcome Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const override;
};

class BridgeVetProvider final : public ExchangeProvider {
 public:
  constexpr BridgeVetProvider() = default;
  const char* Id() const override { return "bridge-vet"; }
  const char* Name() const override { return "VeChain Bridge"; }
  const char* Website() const override { return "https://bridge.vet/"; }
  const char* Custody() const override { return "custodial"; }
  Outcome Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const override;
  Outcome Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const override;
};

extern const LifiProvider kLifiProvider;
extern const SquidProvider kSquidProvider;
extern const NearIntentsProvider kNearIntentsProvider;
extern const MayaProvider kMayaProvider;
extern const SimpleSwapProvider kSimpleSwapProvider;
extern const BridgeVetProvider kBridgeVetProvider;

// The TypeScript new CrossChainRouter([...]): registration order, which is
// the order a quote without `providers` asks them in. A provider's index
// here is its registration index.
inline constexpr const ExchangeProvider* kProviders[] = {
    &kSimpleSwapProvider, &kMayaProvider, &kBridgeVetProvider,
    &kLifiProvider,       &kSquidProvider, &kNearIntentsProvider,
};
constexpr size_t kProviderCount = sizeof(kProviders) / sizeof(kProviders[0]);

// descriptor(): one provider's directory row for this deployment.
struct Descriptor {
  const char* id;
  const char* name;
  const char* website_url;
  bool configured;
  const char* custody;
  bool quote;
  bool execution;
  bool status;
};
class ExchangeProviderRegistry {
 public:
  constexpr ExchangeProviderRegistry() : providers_(kProviders), count_(kProviderCount) {}
  // Ordered nonempty borrowed set; storage and providers outlive the registry.
  constexpr ExchangeProviderRegistry(const ExchangeProvider* const* providers, size_t count)
      : providers_(providers), count_(count) {}
  const ExchangeProvider& At(size_t index) const;
  Descriptor Describe(const ProviderSettings& settings, size_t index) const;
  int Index(const char* id, size_t length) const;
  void ById(size_t* order) const;
  size_t size() const { return count_; }
 private:
  const ExchangeProvider* const* providers_;
  size_t count_;
};

Descriptor Describe(const ProviderSettings& settings, size_t index);
// The registration index of `id`, or -1.
int ProviderIndex(const char* id, size_t length);
// CrossChainRouter.providers(): the registration indices sorted by id
// (localeCompare; ids are lower-case ASCII, so this is byte order).
void ProvidersById(size_t order[kProviderCount]);

Outcome Quote(size_t index, ProviderContext& context, Arena& arena, const QuoteRequest& request,
              Value** routes);
Outcome Status(size_t index, ProviderContext& context, Arena& arena, const StatusRequest& request,
               Value** status);

}  // namespace fortis::exchange::cross_chain
