#include "ownership.h"
#include "cross_chain_internal.h"

#include "chain_transport.h"

#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::cross_chain {

const ExchangeProvider& ExchangeProviderRegistry::At(size_t index) const {
  return *providers_[index < count_ ? index : count_ - 1];
}
Descriptor ExchangeProviderRegistry::Describe(const ProviderSettings& settings, size_t index) const {
  const ExchangeProvider& provider = At(index);
  const bool trading = provider.Trading(settings);
  return {provider.Id(), provider.Name(), provider.Website(), provider.Configured(settings),
          provider.Custody(), trading, trading, true};
}
int ExchangeProviderRegistry::Index(const char* id, size_t length) const {
  for (size_t i = 0; i < count_; ++i)
    if (strlen(providers_[i]->Id()) == length && memcmp(providers_[i]->Id(), id, length) == 0)
      return static_cast<int>(i);
  return -1;
}
void ExchangeProviderRegistry::ById(size_t* order) const {
  for (size_t i = 0; i < count_; ++i) {
    size_t at = i;
    for (; at && strcmp(providers_[order[at - 1]]->Id(), providers_[i]->Id()) > 0; --at) order[at] = order[at - 1];
    order[at] = i;
  }
}
Descriptor Describe(const ProviderSettings& settings, size_t index) { return ExchangeProviderRegistry().Describe(settings, index); }
int ProviderIndex(const char* id, size_t length) { return ExchangeProviderRegistry().Index(id, length); }
void ProvidersById(size_t order[kProviderCount]) { ExchangeProviderRegistry().ById(order); }
Outcome Quote(size_t index, ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) {
  return ExchangeProviderRegistry().At(index).Quote(context, arena, request, routes);
}
Outcome Status(size_t index, ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) {
  return ExchangeProviderRegistry().At(index).Status(context, arena, request, status);
}

namespace detail {

void RunFetches(ProviderContext& context, Arena& arena, Fetch* fetches, size_t count) {
  if (!count) return;
  std::unique_ptr<chains::Call[]> calls_storage;
  try { calls_storage = std::make_unique<chains::Call[]>(count); } catch (const std::bad_alloc&) { calls_storage.reset(); }
  auto* calls = calls_storage.get();
  if (!calls) {
    for (size_t i = 0; i < count; ++i) fetches[i].failure = Failure::kError;
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    chains::Call& call = calls[i];
    const Fetch& fetch = fetches[i];
    call.kind = fetch.post ? chains::CallKind::kPost : chains::CallKind::kGet;
    call.urls = &fetches[i].base;
    call.url_count = 1;
    call.path = fetch.path;
    call.body = fetch.body;
    call.lane = fetch.lane;
    call.timeout_ms = fetch.timeout_ms;
    if (fetch.header_name && fetch.header_value) {
      call.headers[0].name = fetch.header_name;
      call.headers[0].value = fetch.header_value;
      call.header_count = 1;
    }
  }
  context.transport->Run(calls, count, arena, context.deadline_ms);
  for (size_t i = 0; i < count; ++i) {
    Fetch& fetch = fetches[i];
    const chains::Call& call = calls[i];
    fetch.value = nullptr;
    fetch.http_status = call.http_status;
    fetch.retry_after_ms = 0;
    switch (call.failure) {
      case chains::CallFailure::kNone:
        fetch.failure = Failure::kNone;
        fetch.value = call.value;
        break;
      case chains::CallFailure::kLaneBusy:
        fetch.failure = Failure::kLaneBusy;
        fetch.retry_after_ms = call.retry_after_ms;
        fetch.http_status = 0;
        break;
      case chains::CallFailure::kAnswer:
        // A 4xx answer: UpstreamError.status, the only status an adapter reads.
        fetch.failure = Failure::kError;
        break;
      default:
        // Transport failures, 5xx, an unparsable 2xx body, timeouts and the
        // operation deadline.
        fetch.failure = Failure::kError;
        fetch.http_status = 0;
        break;
    }
    calls[i].~Call();
  }
  calls_storage.reset();
}

Value* S(Arena& arena, const char* text) { return tj::Str(arena, text); }

bool SetStr(Arena& arena, Value* object, const char* key, const char* text) {
  return tj::Set(arena, object, key, text ? tj::Str(arena, text) : tj::Null(arena));
}

bool SetVal(Arena& arena, Value* object, const char* key, const Value* value) {
  return tj::Set(arena, object, key, value ? const_cast<Value*>(value) : tj::Null(arena));
}

bool SetNum(Arena& arena, Value* object, const char* key, double number) {
  return tj::Set(arena, object, key, tj::Num(arena, number));
}

bool SetNumOrNull(Arena& arena, Value* object, const char* key, bool present, double number) {
  return tj::Set(arena, object, key, present ? tj::Num(arena, number) : tj::Null(arena));
}

bool SetBool(Arena& arena, Value* object, const char* key, bool value) {
  return tj::Set(arena, object, key, tj::Bool(arena, value));
}

Value* ProgressStep(Arena& arena, const char* id, const char* kind, const Value* label, const char* state,
                    const Value* chain_id, const Value* tx_hash) {
  Value* step = tj::Obj(arena);
  if (!step || !SetStr(arena, step, "id", id) || !SetStr(arena, step, "kind", kind) ||
      !SetVal(arena, step, "label", label) || !SetStr(arena, step, "state", state) ||
      !SetVal(arena, step, "chainId", chain_id) || !SetVal(arena, step, "txHash", tx_hash))
    return nullptr;
  static const char* kNullKeys[] = {"explorerUrl", "startedAt", "completedAt", "durationSeconds", "timingSource",
                                    "detail", "errorCode", "errorMessage"};
  for (const char* key : kNullKeys)
    if (!tj::Set(arena, step, key, tj::Null(arena))) return nullptr;
  return step;
}

Value* Recovery(Arena& arena, const char* kind, const Value* title, const Value* description,
                const Value* action_label, const Value* action_url) {
  Value* recovery = tj::Obj(arena);
  if (!recovery || !SetStr(arena, recovery, "kind", kind) || !SetVal(arena, recovery, "title", title) ||
      !SetVal(arena, recovery, "description", description) || !SetVal(arena, recovery, "actionLabel", action_label) ||
      !SetVal(arena, recovery, "actionUrl", action_url))
    return nullptr;
  return recovery;
}

Value* Execution(Arena& arena, const ExecutionFields& fields) {
  Value* execution = tj::Obj(arena);
  if (!execution || !SetStr(arena, execution, "kind", fields.kind) || !SetVal(arena, execution, "to", fields.to))
    return nullptr;
  if (fields.memo_key && !SetVal(arena, execution, "memo", fields.memo)) return nullptr;
  if (!SetVal(arena, execution, "data", fields.data) || !SetVal(arena, execution, "value", fields.value) ||
      !SetVal(arena, execution, "gasLimit", fields.gas_limit) ||
      !SetVal(arena, execution, "approvalAddress", fields.approval_address))
    return nullptr;
  return execution;
}

Value* Route(Arena& arena, const RouteFields& f) {
  const Value* to = Field(f.execution, "to");
  const Value* data = Field(f.execution, "data");
  Value* id = RouteId(arena, f.provider_id, f.provider_quote_id, f.bridge, f.amount_out_min,
                      to && to->type == tj::Type::kString ? to : nullptr,
                      data && data->type == tj::Type::kString ? data : nullptr);
  Value* route = tj::Obj(arena);
  if (!id || !route || !tj::Set(arena, route, "id", id) || !SetStr(arena, route, "providerId", f.provider_id) ||
      !SetStr(arena, route, "providerName", f.provider_name) || !SetStr(arena, route, "providerUrl", f.provider_url) ||
      !SetVal(arena, route, "providerQuoteId", f.provider_quote_id) ||
      !SetVal(arena, route, "bridgeId", f.bridge_id) || !SetVal(arena, route, "bridge", f.bridge) ||
      !SetStr(arena, route, "bridgeUrl", f.bridge_url) || !SetStr(arena, route, "riskKey", f.risk_key) ||
      !SetStr(arena, route, "custody", f.custody))
    return nullptr;
  if (f.output_protection && !SetStr(arena, route, "outputProtection", f.output_protection)) return nullptr;
  if (!SetVal(arena, route, "amountIn", f.amount_in) || !SetStr(arena, route, "amountOut", f.amount_out) ||
      !SetStr(arena, route, "amountOutMin", f.amount_out_min) || !SetNum(arena, route, "decimalsIn", f.decimals_in) ||
      !SetNum(arena, route, "decimalsOut", f.decimals_out) ||
      !SetNumOrNull(arena, route, "feeUsd", f.has_fee_usd, f.fee_usd) ||
      !SetStr(arena, route, "sourceNetworkFee", f.source_network_fee) ||
      !SetNumOrNull(arena, route, "etaSeconds", f.has_eta, f.eta_seconds) ||
      !SetStr(arena, route, "quotedAt", f.quoted_at) || !SetStr(arena, route, "expiresAt", f.expires_at) ||
      !tj::Set(arena, route, "steps", f.steps ? f.steps : tj::Arr(arena)) ||
      !tj::Set(arena, route, "execution", f.execution))
    return nullptr;
  return route;
}

Value* Routes(Arena& arena, Value* route) {
  Value* routes = tj::Arr(arena);
  if (!routes) return nullptr;
  if (route && !tj::Push(arena, routes, route)) return nullptr;
  return routes;
}

bool IsTon(const Value* chain_id) {
  if (!chain_id || chain_id->type != tj::Type::kString || chain_id->length != 3) return false;
  const char* t = chain_id->text;
  return (t[0] | 0x20) == 't' && (t[1] | 0x20) == 'o' && (t[2] | 0x20) == 'n';
}

}  // namespace detail
}  // namespace fortis::exchange::cross_chain
