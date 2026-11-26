#include <algorithm>
// bridge.vet (port of the TypeScript adapter): a deposit-address
// provider. Passive pricing never creates an order; only a prepared quote
// allocates a quote-bound deposit target, which the adapter re-validates
// against the request before the wallet may sign anything.
#include "cross_chain_deposit_internal.h"

#include "whatwg_url.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::cross_chain::detail {
using namespace deposit;
namespace {
constexpr const char* kBridgeVetBase = "https://bridge.vet";
constexpr uint32_t kOrderReadinessDelaysMs[] = {250, 500, 1000};

struct VetSource {
  const char* id;
  const char* symbol;
  uint32_t decimals;
  const char* allowed[3];
  const char* status_id;
};

const VetSource kVetSources[] = {
    {"2:btc", "BTC", 8, {"1:btc", "2:btc", "3:btc"}, "btc"},
    {"2:eth", "ETH", 18, {"1:eth", "2:eth", "3:eth"}, "eth"},
    {"2:bnbbsc", "BNB", 18, {"1:bnbbsc", "2:bnbbsc", "3:bnbbsc"}, "bnb-bsc"},
};

const VetSource* vet_source(Arena& arena, const QuoteRequest& request) {
  const VetSource* source = Is(request.from.chain_id, "bitcoin") ? &kVetSources[0]
                            : Is(request.from.chain_id, "1")     ? &kVetSources[1]
                            : Is(request.from.chain_id, "56")    ? &kVetSources[2]
                                                                 : nullptr;
  if (!source || request.from.decimals != source->decimals) return nullptr;
  if (Is(request.from.chain_id, "bitcoin"))
    return lower_is(arena, request.from.token, "bitcoin") && ValidBitcoinAddress(request.from.address) ? source : nullptr;
  return lower_is(arena, request.from.token, kNativeEvm) && ValidEvmAddress(request.from.address) ? source : nullptr;
}

bool vet_destination(Arena& arena, const QuoteRequest& request) {
  return Is(request.to.chain_id, "vechain") && lower_is(arena, request.to.token, "vet") && request.to.decimals == 18 &&
         ValidEvmAddress(request.to.address);
}

// unitsToDecimal(raw, decimals).
const char* units_to_decimal(Arena& arena, const char* raw, uint32_t decimals) {
  if (!raw || !PositiveIntegerText(raw, strlen(raw))) return nullptr;
  const size_t length = strlen(raw);
  const size_t padded_length = length > decimals ? length : decimals + 1;
  char* padded = static_cast<char*>(arena.Alloc(padded_length + 1, 1));
  char* out = static_cast<char*>(arena.Alloc(padded_length + 2, 1));
  if (!padded || !out) return nullptr;
  memset(padded, '0', padded_length - length);
  std::copy_n(raw, length, padded + padded_length - length);
  const size_t whole = padded_length - decimals;
  size_t fraction_end = padded_length;
  while (decimals && fraction_end > whole && padded[fraction_end - 1] == '0') --fraction_end;
  memcpy(out, padded, whole);
  size_t used = whole;
  if (decimals && fraction_end > whole) {
    out[used++] = '.';
    memcpy(out + used, padded + whole, fraction_end - whole);
    used += fraction_end - whole;
  }
  out[used] = '\0';
  return out;
}

// decimalToUnits(value, decimals): String(value) of a string or number.
const char* decimal_to_units(Arena& arena, const Value* value, uint32_t decimals) {
  if (!value || (value->type != tj::Type::kString && value->type != tj::Type::kNumber)) return nullptr;
  const Value* text = JsString(arena, value);
  if (!text) return nullptr;
  size_t i = 0;
  while (i < text->length && text->text[i] >= '0' && text->text[i] <= '9') ++i;
  if (!i) return nullptr;
  const size_t whole_end = i;
  size_t fraction_start = text->length, fraction_end = fraction_start;
  if (i < text->length) {
    if (text->text[i] != '.') return nullptr;
    fraction_start = ++i;
    while (i < text->length && text->text[i] >= '0' && text->text[i] <= '9') ++i;
    if (i != text->length || i == fraction_start) return nullptr;
    fraction_end = i;
  }
  const size_t fraction_length = fraction_end - fraction_start;
  for (size_t k = decimals; k < fraction_length; ++k)
    if (text->text[fraction_start + k] != '0') return nullptr;
  char* digits = static_cast<char*>(arena.Alloc(whole_end + decimals + 1, 1));
  if (!digits) return nullptr;
  memcpy(digits, text->text, whole_end);
  for (uint32_t k = 0; k < decimals; ++k)
    digits[whole_end + k] = k < fraction_length ? text->text[fraction_start + k] : '0';
  const size_t length = whole_end + decimals;
  digits[length] = '\0';
  size_t lead = 0;
  while (lead < length && digits[lead] == '0') ++lead;
  return lead == length ? nullptr : digits + lead;
}

struct VetEstimate {
  const Value* extra_data = nullptr;
  double output = 0;
  const char* output_raw = nullptr;
};

bool vet_estimate(Arena& arena, const Value* payload, const VetSource& source, VetEstimate* out) {
  const Value* root = AsRecord(payload);
  const Value* extra = StringValue(Field(root, "extraData"), 64);
  const Value* raw_output = Field(root, "output");
  const double output = raw_output && raw_output->type == tj::Type::kNumber ? raw_output->number : JsNumber(raw_output);
  const char* output_raw = decimal_to_units(arena, raw_output, 18);
  if (!extra) return false;
  // extraData.split(':') must be [a, b, 'vet'] with `${a}:${b}` allowed.
  size_t colons = 0, second = 0;
  for (size_t i = 0; i < extra->length; ++i)
    if (extra->text[i] == ':') { if (++colons == 2) second = i; }
  if (colons != 2) return false;
  const Value* tail = tj::Str(arena, extra->text + second + 1, extra->length - second - 1);
  const Value* head = tj::Str(arena, extra->text, second);
  if (!tail || !head || !Is(tail, "vet")) return false;
  if (!Is(head, source.allowed[0]) && !Is(head, source.allowed[1]) && !Is(head, source.allowed[2])) return false;
  if (!isfinite(output) || output <= 0 || !output_raw) return false;
  out->extra_data = extra;
  out->output = output;
  out->output_raw = output_raw;
  return true;
}

// /^(?:[1-9]\d{0,5}:)?[A-Za-z0-9][A-Za-z0-9_-]{0,199}$/
bool valid_swap_id(const Value* v) {
  if (!v || v->type != tj::Type::kString) return false;
  auto rest_ok = [&](size_t start) {
    const size_t n = v->length - start;
    if (n < 1 || n > 200) return false;
    for (size_t i = start; i < v->length; ++i) {
      const char c = v->text[i];
      const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (i == start ? !alnum : !(alnum || c == '_' || c == '-')) return false;
    }
    return true;
  };
  size_t i = 0;
  if (v->length && v->text[0] >= '1' && v->text[0] <= '9') {
    i = 1;
    while (i < v->length && i < 6 && v->text[i] >= '0' && v->text[i] <= '9') ++i;
    // The prefix may stop earlier than the run of digits.
    for (size_t end = i; end >= 1; --end)
      if (end < v->length && v->text[end] == ':' && rest_ok(end + 1)) return true;
  }
  return rest_ok(0);
}

bool same_swap_id(const Value* payload_id, const Value* swap_id) {
  if (!payload_id || payload_id->type != tj::Type::kString) return false;
  size_t start = 0;
  for (size_t i = 0; i < swap_id->length; ++i)
    if (swap_id->text[i] == ':') { start = i + 1; break; }
  return payload_id->length == swap_id->length - start &&
         memcmp(payload_id->text, swap_id->text + start, payload_id->length) == 0;
}

// amountWithinRange(payload, amount).
bool vet_amount_within_range(const Value* payload, const Value* amount) {
  const Value* root = AsRecord(payload);
  const double value = JsNumber(amount);
  const double min = JsNumber(Field(root, "min"));
  const double max = JsNumber(Field(root, "max"));
  return isfinite(value) && isfinite(min) && min > 0 && value >= min && (!isfinite(max) || max <= 0 || value <= max);
}

Value* vet_route(Arena& arena, const QuoteRequest& request, const VetSource& source, const char* output_raw,
                 double now, const Value* order_id, const Value* order_address, const Value* order_memo) {
  const char* scaled = MulSmallDecimal(arena, output_raw, 10000 - request.slippage_bps);
  const char* minimum = scaled ? DivSmallDecimal(arena, scaled, 10000) : nullptr;
  if (!minimum) return nullptr;
  const bool bitcoin = Is(request.from.chain_id, "bitcoin");
  ExecutionFields execution;
  execution.memo_key = true;
  if (order_id) {
    execution.kind = bitcoin ? "deposit" : "evm-transfer";
    execution.to = order_address;
    execution.memo = order_memo;
    execution.data = bitcoin ? nullptr : str(arena, "0x");
    execution.value = request.amount_in;
    execution.gas_limit = bitcoin ? nullptr : str(arena, "21000");
  }
  Validity validity;
  if (!QuoteValidity(arena, nullptr, now, order_id ? 5 * 60000 : 60000, &validity)) return nullptr;
  Value* steps = tj::Arr(arena);
  Value* step = tj::Obj(arena);
  if (!steps || !step || !SetStr(arena, step, "kind", "bridge") || !SetStr(arena, step, "tool", "VeChain Bridge") ||
      !SetVal(arena, step, "fromChainId", request.from.chain_id) || !SetVal(arena, step, "toChainId", request.to.chain_id) ||
      !SetStr(arena, step, "fromToken", source.symbol) || !SetStr(arena, step, "toToken", "VET") ||
      !tj::Push(arena, steps, step))
    return nullptr;
  RouteFields f;
  f.provider_id = kBridgeVetProvider.Id();
  f.provider_name = kBridgeVetProvider.Name();
  f.provider_url = kBridgeVetProvider.Website();
  f.provider_quote_id = order_id;
  f.bridge_id = str(arena, "bridge-vet-swap");
  f.bridge = str(arena, "VeChain Bridge");
  f.bridge_url = BridgeWebsiteUrl(arena, f.bridge_id);
  f.risk_key = "bridge-vet-swap";
  f.custody = kBridgeVetProvider.Custody();
  f.output_protection = "estimate-only";
  f.amount_in = request.amount_in;
  f.amount_out = output_raw;
  f.amount_out_min = minimum;
  f.decimals_in = request.from.decimals;
  f.decimals_out = request.to.decimals;
  f.quoted_at = validity.quoted_at;
  f.expires_at = validity.expires_at;
  f.steps = steps;
  f.execution = Execution(arena, execution);
  if (!f.execution) return nullptr;
  return Route(arena, f);
}

Value* vet_prepared(Arena& arena, const Value* payload, const QuoteRequest& request, const VetSource& source,
                    const VetEstimate& estimate, const Value* swap_id, double now) {
  const Value* root = AsRecord(payload);
  const Value* input = AsRecord(Field(root, "input"));
  const Value* output = AsRecord(Field(root, "output"));
  const Value* input_token = AsRecord(Field(input, "token"));
  const Value* output_token = AsRecord(Field(output, "token"));
  const Value* address = StringValue(Field(input, "address"), 256);
  const Value* output_address = StringValue(Field(output, "address"), 256);
  const Value* input_id = StringValue(Field(input_token, "id"), 64);
  const Value* output_id = StringValue(Field(output_token, "id"), 64);
  const Value* from_id = StringValue(Field(root, "from"), 64);
  const Value* to_id = StringValue(Field(root, "to"), 64);
  const char* raw_input = decimal_to_units(arena, Coalesce(Field(input, "amount"), Field(root, "amount")),
                                           source.decimals);
  const char* expected_raw = decimal_to_units(arena, Field(output, "amountExpected"), 18);
  if (!expected_raw) expected_raw = estimate.output_raw;
  const Value* input_vechain = Field(input, "isVechain");
  const Value* output_vechain = Field(output, "isVechain");
  if (!root || !same_swap_id(tj::Get(root, "id"), swap_id) || !from_id || !lower_is(arena, from_id, source.status_id) ||
      !to_id || !lower_is(arena, to_id, "vet") || !address ||
      !(Is(request.from.chain_id, "bitcoin") ? ValidBitcoinAddress(address) : ValidEvmAddress(address)) ||
      !output_address || !same_lower(arena, output_address, request.to.address) ||
      (input_vechain && input_vechain->type == tj::Type::kTrue) ||
      !(output_vechain && output_vechain->type == tj::Type::kTrue) || !input_id ||
      !lower_is(arena, input_id, source.status_id) || !output_id || !lower_is(arena, output_id, "vet") || !raw_input ||
      !Is(request.amount_in, raw_input) || IsZeroDecimal(expected_raw))
    return nullptr;
  const Value* extra = Field(input, "extraId");
  const Value* memo = Nullish(extra) ? nullptr : StringValue(extra, 256);
  return vet_route(arena, request, source, expected_raw, now, swap_id, address, memo);
}

Value* vet_step(Arena& arena, const char* kind, const char* label, const char* state, const Value* chain_id,
                const Value* tx_hash) {
  char id[48];
  snprintf(id, sizeof(id), "bridge-vet-%s", kind);
  Value* step = ProgressStep(arena, id, kind, str(arena, label), state, chain_id, tx_hash);
  const bool problem = !strcmp(state, "failed") || !strcmp(state, "needs-action");
  if (!step || !SetStr(arena, step, "errorCode", problem ? state : nullptr)) return nullptr;
  return step;
}

// parseBridgeVetStatus(); nullptr with `*thrown` for a different swap id.
Value* parse_vet_status(Arena& arena, const Value* payload, const StatusRequest& request, const Value* swap_id) {
  const Value* root = AsRecord(payload);
  const Value* input = AsRecord(Field(root, "input"));
  const Value* output = AsRecord(Field(root, "output"));
  const Value* id = Field(root, "id");
  if (!Nullish(id) && !same_swap_id(id, swap_id)) return nullptr;
  const Value* raw_status = StringValue(Field(root, "status"), 64);
  const Value* status = Lower(arena, raw_status ? raw_status : str(arena, "waiting"));
  if (!status) return nullptr;
  const char* state = Is(status, "finished") ? "completed" : Is(status, "refunded") ? "refunded"
                      : Is(status, "failed") || Is(status, "expired") ? "failed"
                      : Is(status, "hold") ? "needs-action" : "pending";
  const Value* input_tx = StringValue(Field(input, "txId"), 256);
  const Value* output_tx = StringValue(Field(output, "txId"), 256);
  const Value* source_hash = input_tx ? input_tx : request.tx_hash;
  Text url;
  if (!url.Init(arena, swap_id->length * 3 + 64)) return nullptr;
  url.out.Text("https://bridge.vet/swap/");
  if (!EncodeComponent(swap_id, url.out)) return nullptr;
  const char* explorer_text = url.Finish();
  if (!explorer_text) return nullptr;
  Value* explorer = str(arena, explorer_text);
  const char* actual = !strcmp(state, "completed") ? decimal_to_units(arena, Field(output, "amount"), 18) : nullptr;
  double elapsed = 0;
  const bool has_elapsed = SecondsBetween(SafeTimestamp(arena, Field(root, "createdAt")),
                                          SafeTimestamp(arena, Field(root, "updatedAt")), &elapsed);
  const bool deposited = !Is(status, "waiting") || input_tx;
  const bool complete = Is(status, "finished");
  const char* bridge_state = complete ? "completed" : Is(status, "hold") ? "needs-action"
                             : Is(status, "failed") || Is(status, "expired") ? "failed"
                             : Is(status, "refunded") ? "refunded" : deposited ? "pending" : "not-started";
  Value* progress = tj::Arr(arena);
  Value* source_step = vet_step(arena, "source", "Deposit sent", deposited ? "completed" : "pending",
                                request.from_chain_id, input_tx ? input_tx : source_hash);
  Value* bridge_step = vet_step(arena, "bridge", "Swap to VET", bridge_state, nullptr, nullptr);
  Value* destination_step = vet_step(arena, "destination", "VET received", complete ? "completed" : "not-started",
                                     str(arena, "vechain"), output_tx);
  if (!progress || !source_step || !bridge_step || !destination_step || !tj::Push(arena, progress, source_step) ||
      !tj::Push(arena, progress, bridge_step) || !tj::Push(arena, progress, destination_step))
    return nullptr;
  static const char* kMessages[][2] = {
      {"waiting", "Waiting for the source-chain deposit."},
      {"confirming", "The deposit is being confirmed."},
      {"exchanging", "The provider is exchanging the deposit for VET."},
      {"sending", "VET is being sent to the VeChain wallet."},
      {"finished", "VET was delivered to the VeChain wallet."},
      {"refunded", "The source deposit was refunded."},
      {"hold", "The provider placed this swap on hold; support may be required."},
      {"expired", "The deposit order expired."},
      {"failed", "The provider could not complete this swap."}};
  const char* message = "The VeChain bridge is processing the deposit.";
  for (const auto& entry : kMessages)
    if (Is(status, entry[0])) message = entry[1];
  Value* recovery = tj::Null(arena);
  if (Is(status, "waiting") || Is(status, "confirming") || Is(status, "exchanging") || Is(status, "sending")) {
    recovery = Recovery(arena, "wait", str(arena, "Bridge in progress"),
                        str(arena, "Keep this transaction in history while the provider completes settlement."),
                        str(arena, "Track swap"), explorer);
  } else if (Is(status, "refunded")) {
    recovery = Recovery(arena, "track-refund", str(arena, "Deposit refunded"),
                        str(arena, "Check the source wallet for the returned native coin."), str(arena, "Track refund"),
                        explorer);
  } else if (Is(status, "hold") || Is(status, "failed") || Is(status, "expired")) {
    recovery = Recovery(arena, "contact-support", str(arena, "Provider assistance may be required"),
                        str(arena, "Open the provider swap page to inspect the order and contact its support."),
                        str(arena, "Open swap"), explorer);
  }
  Value* received = tj::Null(arena);
  if (actual) {
    received = tj::Obj(arena);
    if (!received || !SetStr(arena, received, "chainId", "vechain") || !SetStr(arena, received, "amount", actual) ||
        !SetStr(arena, received, "tokenAddress", "vet") || !SetStr(arena, received, "symbol", "VET") ||
        !SetNum(arena, received, "decimals", 18))
      return nullptr;
  }
  Value* out = tj::Obj(arena);
  if (!recovery || !received || !out || !SetStr(arena, out, "providerId", "bridge-vet") ||
      !SetStr(arena, out, "state", state) || !SetVal(arena, out, "substatus", Upper(arena, status)) ||
      !SetStr(arena, out, "message", message) || !SetVal(arena, out, "sourceHash", source_hash) ||
      !SetVal(arena, out, "destinationHash", output_tx) || !SetVal(arena, out, "providerExplorerUrl", explorer) ||
      !SetVal(arena, out, "bridgeExplorerUrl", explorer) ||
      !SetNumOrNull(arena, out, "elapsedSeconds", has_elapsed, elapsed) ||
      !tj::Set(arena, out, "actualReceived", received) || !tj::Set(arena, out, "progress", progress) ||
      !tj::Set(arena, out, "recovery", recovery) ||
      !tj::Set(arena, out, "providerData", BoundedProviderSnapshot(arena, root)))
    return nullptr;
  return out;
}

const char* vet_query(Arena& arena, const char* path, const VetSource& source, const char* amount) {
  Text text;
  if (!text.Init(arena, 256 + (amount ? strlen(amount) * 3 : 0))) return nullptr;
  text.out.Text(path);
  text.out.Byte('?');
  QueryPair pairs[] = {{"from", str(arena, source.id)}, {"to", str(arena, "vet:vet")},
                       {"amount", amount ? str(arena, amount) : nullptr}};
  FormEncode(pairs, amount ? 3 : 2, text.out);
  return text.Finish();
}

Fetch vet_get(const char* path) {
  Fetch fetch;
  fetch.lane = "bridge-vet";
  fetch.base = kBridgeVetBase;
  fetch.path = path;
  fetch.timeout_ms = 12000;
  return fetch;
}

const char* vet_status_path(Arena& arena, const Value* swap_id) {
  Text text;
  if (!text.Init(arena, swap_id->length * 3 + 64)) return nullptr;
  text.out.Text("/api/bridge/status/");
  if (!EncodeComponent(swap_id, text.out)) return nullptr;
  return text.Finish();
}

}  // namespace
}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome BridgeVetProvider::Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
  using namespace detail;
  *routes = Routes(arena, nullptr);
  if (!*routes) return Error();
  const VetSource* source = vet_source(arena, request);
  if (!source || !vet_destination(arena, request)) return Outcome::Ok();
  const double now = context.Now();
  const char* amount_in = CText(arena, request.amount_in);
  const char* amount = units_to_decimal(arena, amount_in, source->decimals);
  if (!amount) return Outcome::Ok();
  // decimalToUnits(Number(amount)) must give the exact request back.
  Value* amount_value = tj::Str(arena, amount);
  const double amount_number = JsNumber(amount_value);
  Value* number = tj::Num(arena, amount_number);
  const char* round_trip = number ? decimal_to_units(arena, number, source->decimals) : nullptr;
  if (!round_trip || strcmp(round_trip, amount_in) != 0) return Outcome::Ok();
  Fetch range = vet_get(vet_query(arena, "/api/bridge/range", *source, nullptr));
  if (!range.path) return Error();
  RunFetch(context, arena, range);
  if (!range.ok()) return range.error();
  if (!vet_amount_within_range(range.value, amount_value)) return Outcome::Ok();
  Fetch estimate_fetch = vet_get(vet_query(arena, "/api/bridge/estimate", *source, amount));
  if (!estimate_fetch.path) return Error();
  RunFetch(context, arena, estimate_fetch);
  if (!estimate_fetch.ok()) return estimate_fetch.error();
  VetEstimate estimate;
  if (!vet_estimate(arena, estimate_fetch.value, *source, &estimate)) return Outcome::Ok();
  if (!request.prepare) {
    *routes = Routes(arena, vet_route(arena, request, *source, estimate.output_raw, now, nullptr, nullptr, nullptr));
    return *routes ? Outcome::Ok() : Error();
  }
  Value* body = tj::Obj(arena);
  Value* estimate_body = tj::Obj(arena);
  if (!body || !estimate_body || !SetStr(arena, body, "direction", "to") || !SetStr(arena, body, "from", source->id) ||
      !SetStr(arena, body, "to", "vet:vet") || !SetNum(arena, body, "amount", amount_number) ||
      !SetVal(arena, body, "address", request.to.address) ||
      !SetVal(arena, estimate_body, "extraData", estimate.extra_data) ||
      !SetNum(arena, estimate_body, "output", estimate.output) || !tj::Set(arena, body, "estimate", estimate_body))
    return Error();
  Fetch swap;
  swap.lane = "bridge-vet";
  swap.base = kBridgeVetBase;
  swap.path = "/api/bridge/swap";
  swap.post = true;
  swap.body = body;
  swap.timeout_ms = 15000;
  RunFetch(context, arena, swap);
  if (!swap.ok()) return swap.error();
  const Value* swap_id = StringValue(Field(swap.value, "swapId"), 256);
  if (!swap_id || !valid_swap_id(swap_id)) return Error();
  const char* status_path = vet_status_path(arena, swap_id);
  if (!status_path) return Error();
  for (size_t attempt = 0; attempt <= sizeof(kOrderReadinessDelaysMs) / sizeof(kOrderReadinessDelaysMs[0]); ++attempt) {
    if (attempt > 0) context.clock->SleepMs(kOrderReadinessDelaysMs[attempt - 1]);
    Fetch status = vet_get(status_path);
    RunFetch(context, arena, status);
    if (!status.ok()) {
      const bool readiness = status.failure == Failure::kError &&
                             (status.http_status == 404 || status.http_status == 409 || status.http_status == 425);
      if (!readiness) return status.error();
      continue;
    }
    Value* prepared = vet_prepared(arena, status.value, request, *source, estimate, swap_id, now);
    if (prepared) {
      *routes = Routes(arena, prepared);
      return *routes ? Outcome::Ok() : Error();
    }
  }
  return Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome BridgeVetProvider::Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const {
  using namespace detail;
  const Value* swap_id = request.provider_quote_id;
  if (!swap_id || !valid_swap_id(swap_id)) return Error();
  Fetch fetch = vet_get(vet_status_path(arena, swap_id));
  if (!fetch.path) return Error();
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  *status = parse_vet_status(arena, fetch.value, request, swap_id);
  return *status ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
const BridgeVetProvider kBridgeVetProvider{};

}  // namespace fortis::exchange::cross_chain
