#pragma once

#include "cross_chain_providers.h"

#include <stddef.h>
#include <stdint.h>

// Internal to the cross-chain adapters: the upstream fetch over
// chains::Transport and small builders shared by the provider files.
namespace fortis::exchange::cross_chain::detail {

// One HttpClient.json() call inside lanes.run(lane, ...).
struct Fetch {
  // ---- inputs ----
  const char* lane = nullptr;
  const char* base = nullptr;  // "https://host[:port][/prefix]" (no trailing '/')
  const char* path = nullptr;  // "/path?query"
  bool post = false;
  const Value* body = nullptr;  // JSON.stringify(body) for a POST
  const char* header_name = nullptr;
  const char* header_value = nullptr;
  uint32_t timeout_ms = 10000;
  // ---- results ----
  Failure failure = Failure::kError;
  int http_status = 0;          // UpstreamError.status (0: none)
  uint32_t retry_after_ms = 0;  // LaneBusyError.retryAfterMs
  Value* value = nullptr;       // the parsed body on success

  bool ok() const { return failure == Failure::kNone; }
  Outcome error() const {
    Outcome outcome;
    outcome.failure = failure == Failure::kNone ? Failure::kError : failure;
    outcome.retry_after_ms = retry_after_ms;
    return outcome;
  }
};

// Runs every fetch concurrently (Promise.all over independent calls) and
// waits for all of them, bounded by the context's deadline.
void RunFetches(ProviderContext& context, Arena& arena, Fetch* fetches, size_t count);
inline void RunFetch(ProviderContext& context, Arena& arena, Fetch& fetch) { RunFetches(context, arena, &fetch, 1); }

inline Outcome Error() {
  Outcome outcome;
  outcome.failure = Failure::kError;
  return outcome;
}

// Builders over tj objects (each returns false on arena exhaustion).
bool SetStr(Arena& arena, Value* object, const char* key, const char* text);
bool SetVal(Arena& arena, Value* object, const char* key, const Value* value);  // nullptr -> null
bool SetNum(Arena& arena, Value* object, const char* key, double number);
bool SetNumOrNull(Arena& arena, Value* object, const char* key, bool present, double number);
bool SetBool(Arena& arena, Value* object, const char* key, bool value);
Value* S(Arena& arena, const char* text);

// A progress step with every optional member null; callers set the rest.
Value* ProgressStep(Arena& arena, const char* id, const char* kind, const Value* label, const char* state,
                    const Value* chain_id, const Value* tx_hash);
// A recovery object (actionLabel/actionUrl may be nullptr -> null).
Value* Recovery(Arena& arena, const char* kind, const Value* title, const Value* description,
                const Value* action_label, const Value* action_url);
// An execution object; `memo_key` adds the memo member (deposit flows).
struct ExecutionFields {
  const char* kind = "provider-order";
  const Value* to = nullptr;
  bool memo_key = false;
  const Value* memo = nullptr;
  const Value* data = nullptr;
  const Value* value = nullptr;
  const Value* gas_limit = nullptr;
  const Value* approval_address = nullptr;
};
Value* Execution(Arena& arena, const ExecutionFields& fields);

// Route assembly in the TypeScript `partial` key order.
struct RouteFields {
  const char* provider_id = nullptr;
  const char* provider_name = nullptr;
  const char* provider_url = nullptr;
  const Value* provider_quote_id = nullptr;  // nullptr -> null
  const Value* bridge_id = nullptr;          // nullptr -> null
  const Value* bridge = nullptr;
  const char* bridge_url = nullptr;          // nullptr -> null
  const char* risk_key = nullptr;
  const char* custody = nullptr;
  const char* output_protection = nullptr;   // nullptr -> member absent
  const Value* amount_in = nullptr;
  const char* amount_out = nullptr;
  const char* amount_out_min = nullptr;
  uint32_t decimals_in = 0;
  uint32_t decimals_out = 0;
  bool has_fee_usd = false;
  double fee_usd = 0;
  const char* source_network_fee = nullptr;  // nullptr -> null
  bool has_eta = false;
  double eta_seconds = 0;
  const char* quoted_at = nullptr;
  const char* expires_at = nullptr;
  Value* steps = nullptr;
  Value* execution = nullptr;
};
// { id: routeId(providerId, providerQuoteId, partial), ...partial }.
Value* Route(Arena& arena, const RouteFields& fields);

// A one-element routes array, or an empty one for nullptr.
Value* Routes(Arena& arena, Value* route);

// chainId.toLowerCase() === 'ton' for an ASCII chain id.
bool IsTon(const Value* chain_id);
// The JavaScript `===` of a request string against a literal.
inline bool Eq(const Value* v, const char* text) { return Is(v, text); }

}  // namespace fortis::exchange::cross_chain::detail
