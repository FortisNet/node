#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The Robinhood trade policy and transaction diagnostics (ports of the
// TypeScript sponsored-trade gateway and transaction diagnostics facets),
// as pure functions over the token_json DOM so the corpus can drive them
// directly. Guest-safe: no C++ standard-library headers.
namespace fortis::exchange::robinhood {

using codec::tj::Arena;
using codec::tj::Value;

constexpr uint32_t kChainId = 4663;
constexpr const char* kChainIdHex = "0x1237";
constexpr size_t kMaxCalls = 2;
// The one disabled reason RobinhoodSponsoredTradeGateway.capabilities() reports.
constexpr const char* kSponsorshipDisabledReason = "ALCHEMY_GAS_POLICY_NOT_CONFIGURED";

// capabilities(): sponsorship is enabled exactly when ALCHEMY_API_KEY and
// ALCHEMY_GAS_POLICY_ID are both configured (non-blank after trimming).
constexpr bool SponsoredTradesEnabled(bool alchemy_api_key, bool gas_policy_id) {
  return alchemy_api_key && gas_policy_id;
}

// The two RobinhoodSponsoredCapabilitiesResponse documents for one state:
// the JSON.stringify text (NUL-terminated, so `json_capacity` must exceed
// its length) and the protobuf-es toBinary bytes (proto3 defaults omitted,
// `reason` present only when disabled). False when either buffer is too
// small; nothing partial is reported as success.
bool EncodeSponsoredCapabilities(bool enabled, char* json, size_t json_capacity, size_t* json_length,
                                 uint8_t* proto, size_t proto_capacity, size_t* proto_length);

// The official routers (Uniswap V2 Router02, Uniswap SwapRouter02) and the
// funding tokens (WETH, USDG), lowercase.
bool IsRouter(const char* address, size_t length);
bool IsFundingToken(const char* address, size_t length);
// KNOWN_TOKENS (WETH 18, USDG 6); nullptr for any other contract.
const char* KnownTokenSymbol(const char* address, size_t length, int* decimals);

// The active stock tokens one request reads, in catalog order: lowercase
// contract addresses and their tokenSymbol values. Lookups follow `new
// Map(entries)`: a repeated contract keeps the last symbol.
struct StockToken {
  char address[43]{};
  const Value* symbol = nullptr;
};
struct StockSet {
  const StockToken* items = nullptr;
  size_t count = 0;
  bool Has(const char* lower, size_t length) const;
  const Value* Symbol(const char* lower, size_t length) const;
};

// stockLogoUrl(symbol): the Financial Modeling Prep artwork URL for a bounded
// ticker (trim, toUpperCase, /^[A-Z0-9][A-Z0-9.-]{0,31}$/), else nullptr.
const char* StockLogoUrl(Arena& arena, const char* symbol, size_t length);

// validateTradeCalls(calls, assets, from): `calls` is an array of {to, data,
// value?} objects; `from` is compared exactly as given (the diagnose route
// passes the request's spelling, prepare passes it lowercase). Returns the
// normalized [{to, data, value}] calls, or nullptr where TypeScript throws
// SponsoredTradeValidationError. `*exhausted` reports arena exhaustion.
Value* ValidateTradeCalls(Arena& arena, const Value* calls, const StockSet& stocks, const char* from,
                          size_t from_length, bool* exhausted);

// validatePrepared(prepared, from): false where TypeScript throws (the route
// answers 500). On success `*requests` is the array of signatureRequest
// values and `*node_count` the prepared node count (1 or 2).
bool ValidatePrepared(Arena& arena, const Value* prepared, const char* from_lower, Value** requests,
                      size_t* node_count);
// attachSignatures(prepared, signatures): each node without signatureRequest
// and with signature {type:'secp256k1', data}. Null on arena exhaustion or
// where TypeScript throws.
Value* AttachSignatures(Arena& arena, const Value* prepared, const Value* const* signatures, size_t count);

// A JSON-RPC reply as the Alchemy clients read it.
enum class RpcReply : uint8_t {
  kResult,    // body.result (may be JSON null)
  kRpcError,  // body.error: message (sliced to 240 units or the default) and data
  kInternal,  // the client throws a TypeError (null body/error, missing result)
};
RpcReply ReadRpcReply(Arena& arena, const Value* body, const char* method, Value** result, Value** message,
                      const Value** data);

// decodeRevertReason(value): Error(string) sliced to 240 units, or "Contract
// panic (0x…)."; nullptr where TypeScript returns null.
Value* DecodeRevertReason(Arena& arena, const Value* value);

// The diagnose request after validation.
struct DiagnosticsRequest {
  const Value* calls = nullptr;       // the request calls (gas fields included)
  const Value* executable = nullptr;  // ValidateTradeCalls output
  const char* from = nullptr;         // as requested
  size_t from_length = 0;
  bool sponsored = false;
  const StockSet* stocks = nullptr;
  int64_t now_ms = 0;
};
// The eth_simulateV1 / debug_traceCallMany call list (compactCall order:
// from, to, data, value, gas, maxFeePerGas, maxPriorityFeePerGas).
Value* SimulationCalls(Arena& arena, const DiagnosticsRequest& request);
// buildReport(...) with `trace` nullptr for a failed trace. Null on arena
// exhaustion or when an exact integer exceeds the native 640-bit bound.
Value* BuildReport(Arena& arena, const DiagnosticsRequest& request, const Value* simulation, const Value* trace);
// failedReport(...) for a JSON-RPC simulation error; `reason` is a string
// value or nullptr.
Value* FailedReport(Arena& arena, const DiagnosticsRequest& request, const Value* reason);

}  // namespace fortis::exchange::robinhood
