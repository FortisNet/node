// SimpleSwap (port of the TypeScript adapter): a deposit-address
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
constexpr const char* kSimpleSwapBase = "https://api.simpleswap.io";
constexpr const char* kSimpleSwapWebsite = "https://simpleswap.io/";

struct SwapAsset {
  const char* chain;
  const char* ticker;
  const char* network;
  const char* token;
  uint32_t decimals;
  const char* upper;
};

const SwapAsset kSwapAssets[] = {
    {"monero", "xmr", "xmr", "monero", 12, "XMR"},
    {"bitcoin", "btc", "btc", "bitcoin", 8, "BTC"},
    {"1", "eth", "eth", kNativeEvm, 18, "ETH"},
    {"solana-mainnet-beta", "sol", "sol", "11111111111111111111111111111111", 9, "SOL"},
};

const SwapAsset* swap_asset_for(const Value* chain_id) {
  for (const SwapAsset& asset : kSwapAssets)
    if (Is(chain_id, asset.chain)) return &asset;
  return nullptr;
}

bool swap_supported_pair(const Value* from, const Value* to) {
  return !SameText(from, to) && swap_asset_for(from) && swap_asset_for(to) && (Is(from, "monero") || Is(to, "monero"));
}

bool swap_valid_address(const Value* chain, const Value* address) {
  if (Is(chain, "monero")) return ValidMoneroAddress(address);
  if (Is(chain, "bitcoin")) return ValidBitcoinAddress(address);
  if (Is(chain, "solana-mainnet-beta")) return ValidSolanaAddress(address);
  return Is(chain, "1") && ValidEvmAddress(address);
}

const SwapAsset* swap_asset(const Endpoint& endpoint) {
  const SwapAsset* asset = swap_asset_for(endpoint.chain_id);
  return asset && Is(endpoint.token, asset->token) && endpoint.decimals == asset->decimals &&
                 swap_valid_address(endpoint.chain_id, endpoint.address)
             ? asset
             : nullptr;
}

// toAtomic(value, decimals): a decimal string to base units (null on syntax
// or a non-zero digit beyond `decimals`).
const char* to_atomic(Arena& arena, const Value* v, uint32_t decimals) {
  if (!v || v == Undefined() || v->type != tj::Type::kString || Units(v) > 120) return nullptr;
  size_t i = 0;
  while (i < v->length && v->text[i] >= '0' && v->text[i] <= '9') ++i;
  if (!i) return nullptr;
  const size_t whole_end = i;
  size_t fraction_start = v->length, fraction_end = fraction_start;
  if (i < v->length) {
    if (v->text[i] != '.') return nullptr;
    fraction_start = ++i;
    while (i < v->length && v->text[i] >= '0' && v->text[i] <= '9') ++i;
    if (i != v->length || i == fraction_start) return nullptr;
    fraction_end = i;
  }
  const size_t fraction_length = fraction_end - fraction_start;
  for (size_t k = decimals; k < fraction_length; ++k)
    if (v->text[fraction_start + k] != '0') return nullptr;
  char* digits = static_cast<char*>(arena.Alloc(whole_end + decimals + 1, 1));
  if (!digits) return nullptr;
  memcpy(digits, v->text, whole_end);
  for (uint32_t k = 0; k < decimals; ++k)
    digits[whole_end + k] = k < fraction_length ? v->text[fraction_start + k] : '0';
  digits[whole_end + decimals] = '\0';
  // BigInt(...).toString(): leading zeros removed.
  size_t lead = 0;
  while (digits[lead] == '0' && digits[lead + 1]) ++lead;
  return digits + lead;
}

const char* positive_atomic(Arena& arena, const Value* v, uint32_t decimals) {
  const char* raw = to_atomic(arena, v, decimals);
  return raw && !IsZeroDecimal(raw) ? raw : nullptr;
}

// toDecimal(value, decimals) (decimals > 0).
const char* to_decimal(Arena& arena, const Value* amount, uint32_t decimals) {
  const size_t length = amount->length;
  const size_t padded_length = length > decimals ? length : decimals + 1;
  char* padded = static_cast<char*>(arena.Alloc(padded_length + 2, 1));
  if (!padded) return nullptr;
  const size_t pad = padded_length - length;
  memset(padded, '0', pad);
  memcpy(padded + pad, amount->text, length);
  size_t fraction_end = padded_length;
  while (fraction_end > padded_length - decimals && padded[fraction_end - 1] == '0') --fraction_end;
  const size_t whole = padded_length - decimals;
  char* out = static_cast<char*>(arena.Alloc(padded_length + 2, 1));
  if (!out) return nullptr;
  memcpy(out, padded, whole);
  size_t used = whole;
  if (fraction_end > whole) {
    out[used++] = '.';
    memcpy(out + used, padded + whole, fraction_end - whole);
    used += fraction_end - whole;
  }
  out[used] = '\0';
  return out;
}

bool has_value(const Value* v) { return !Nullish(v) && !(v->type == tj::Type::kString && v->length == 0); }

bool valid_order_id(const Value* v) {
  if (!v || v->type != tj::Type::kString || v->length < 1 || v->length > 128) return false;
  for (size_t i = 0; i < v->length; ++i) {
    const char c = v->text[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return true;
}

bool swap_matches_pair(const Value* order, const SwapAsset& from, const SwapAsset& to) {
  return Is(tj::Get(order, "tickerFrom"), from.ticker) && Is(tj::Get(order, "networkFrom"), from.network) &&
         Is(tj::Get(order, "tickerTo"), to.ticker) && Is(tj::Get(order, "networkTo"), to.network);
}

bool swap_same_address(Arena& arena, const Value* chain, const Value* actual, const Value* expected) {
  if (!actual || actual->type != tj::Type::kString) return false;
  return Is(chain, "1") ? same_lower(arena, actual, expected) : SameText(actual, expected);
}

const Value* swap_tx_hash(const Value* chain, const Value* value) {
  if (!value || value->type != tj::Type::kString) return nullptr;
  if (Is(chain, "solana-mainnet-beta")) {
    if (value->length < 64 || value->length > 88) return nullptr;
    static const char kBase58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    for (size_t i = 0; i < value->length; ++i)
      if (!value->text[i] || !strchr(kBase58, value->text[i])) return nullptr;
    return value;
  }
  const size_t start = Is(chain, "1") ? 2 : 0;
  if (start && !StartsWith(value, "0x")) return nullptr;
  if (value->length != 64 + start) return nullptr;
  for (size_t i = start; i < value->length; ++i) {
    const char c = value->text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return nullptr;
  }
  return value;
}

// unwrap(): asRecord(asRecord(payload)?.result).
const Value* unwrap(const Value* payload) { return AsRecord(Field(payload, "result")); }

// boundedExpiry(value, now, maxAge).
const char* bounded_expiry(Arena& arena, const Value* value, double now, double max_age) {
  if (Nullish(value)) {
    Validity validity;
    return QuoteValidity(arena, nullptr, now, max_age, &validity) ? validity.expires_at : nullptr;
  }
  const char* parsed = SafeExpiry(arena, value);
  double ms = 0;
  if (!parsed || !DateParse(parsed, strlen(parsed), &ms) || ms <= now) return nullptr;
  return iso_min(arena, ms, now + max_age);
}

Value* swap_route(Arena& arena, const QuoteRequest& request, const SwapAsset& from, const SwapAsset& to,
                  const char* output, double now, const char* expires_at, const Value* order_id,
                  const Value* order_address) {
  const char* scaled = MulSmallDecimal(arena, output, 10000 - request.slippage_bps);
  const char* minimum = scaled ? DivSmallDecimal(arena, scaled, 10000) : nullptr;
  if (!minimum) return nullptr;
  Value* steps = tj::Arr(arena);
  Value* step = tj::Obj(arena);
  if (!steps || !step || !SetStr(arena, step, "kind", "bridge") || !SetStr(arena, step, "tool", "SimpleSwap") ||
      !SetVal(arena, step, "fromChainId", request.from.chain_id) || !SetVal(arena, step, "toChainId", request.to.chain_id) ||
      !SetStr(arena, step, "fromToken", from.upper) || !SetStr(arena, step, "toToken", to.upper) ||
      !tj::Push(arena, steps, step))
    return nullptr;
  const bool evm = Is(request.from.chain_id, "1");
  ExecutionFields execution;
  execution.memo_key = true;
  execution.kind = !order_id ? "provider-order" : evm ? "evm-transfer" : "deposit";
  execution.to = order_address;
  execution.data = order_id && evm ? str(arena, "0x") : nullptr;
  execution.value = order_id ? request.amount_in : nullptr;
  RouteFields f;
  f.provider_id = kSimpleSwapProvider.Id();
  f.provider_name = kSimpleSwapProvider.Name();
  f.provider_url = kSimpleSwapProvider.Website();
  f.provider_quote_id = order_id;
  f.bridge_id = str(arena, "simpleswap");
  f.bridge = str(arena, "SimpleSwap");
  f.bridge_url = kSimpleSwapWebsite;
  f.risk_key = "simpleswap";
  f.custody = kSimpleSwapProvider.Custody();
  f.output_protection = "estimate-only";
  f.amount_in = request.amount_in;
  f.amount_out = output;
  f.amount_out_min = minimum;
  f.decimals_in = from.decimals;
  f.decimals_out = to.decimals;
  f.quoted_at = IsoText(arena, now);
  f.expires_at = expires_at;
  f.steps = steps;
  f.execution = Execution(arena, execution);
  if (!f.execution || !f.quoted_at) return nullptr;
  return Route(arena, f);
}

const char* swap_query(Arena& arena, const char* path, const SwapAsset& from, const SwapAsset& to,
                       const char* amount) {
  Text text;
  if (!text.Init(arena, 512 + (amount ? strlen(amount) * 3 : 0))) return nullptr;
  text.out.Text(path);
  text.out.Byte('?');
  QueryPair pairs[] = {{"tickerFrom", str(arena, from.ticker)}, {"networkFrom", str(arena, from.network)},
                       {"tickerTo", str(arena, to.ticker)},     {"networkTo", str(arena, to.network)},
                       {"fixed", str(arena, "false")},          {"reverse", str(arena, "false")},
                       {"amount", amount ? str(arena, amount) : nullptr}};
  FormEncode(pairs, amount ? 7 : 6, text.out);
  return text.Finish();
}

}  // namespace
}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome SimpleSwapProvider::Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  *routes = Routes(arena, nullptr);
  if (!*routes) return Error();
  if (!settings.simpleswap_configured || !swap_supported_pair(request.from.chain_id, request.to.chain_id))
    return Outcome::Ok();
  const SwapAsset* from = swap_asset(request.from);
  const SwapAsset* to = swap_asset(request.to);
  if (!from || !to || !request.amount_in || !PositiveIntegerText(request.amount_in->text, request.amount_in->length) ||
      request.slippage_bps > 5000)
    return Outcome::Ok();
  const char* amount_in = CText(arena, request.amount_in);
  if (Is(request.from.chain_id, "monero") && CompareDecimal(amount_in, "18446744073709551615") > 0)
    return Outcome::Ok();
  const char* amount = to_decimal(arena, request.amount_in, from->decimals);
  if (!amount) return Error();
  auto get = [&](const char* path, Fetch* fetch) {
    fetch->lane = "simpleswap";
    fetch->base = kSimpleSwapBase;
    fetch->path = path;
    fetch->header_name = "x-api-key";
    fetch->header_value = settings.simpleswap_api_key;
    fetch->timeout_ms = 15000;
    if (!path) { fetch->failure = Failure::kError; return; }
    RunFetch(context, arena, *fetch);
  };
  // catch (error): a 400/404/422 answer means "no route".
  auto failed = [&](const Fetch& fetch, Outcome* outcome) {
    const bool no_route = fetch.failure == Failure::kError &&
                          (fetch.http_status == 400 || fetch.http_status == 404 || fetch.http_status == 422);
    *outcome = no_route ? Outcome::Ok() : fetch.error();
    return true;
  };
  Outcome outcome;
  Fetch ranges_fetch;
  get(swap_query(arena, "/v3/ranges", *from, *to, nullptr), &ranges_fetch);
  if (!ranges_fetch.ok() && failed(ranges_fetch, &outcome)) return outcome;
  const Value* ranges = unwrap(ranges_fetch.value);
  const char* min = to_atomic(arena, Field(ranges, "min"), from->decimals);
  const Value* raw_max = Field(ranges, "max");
  const char* max = Nullish(raw_max) ? nullptr : to_atomic(arena, raw_max, from->decimals);
  if (!min || CompareDecimal(amount_in, min) < 0 || (!Nullish(raw_max) && !max) ||
      (max && !IsZeroDecimal(max) && CompareDecimal(amount_in, max) > 0))
    return Outcome::Ok();
  Fetch estimate_fetch;
  get(swap_query(arena, "/v3/estimates", *from, *to, amount), &estimate_fetch);
  if (!estimate_fetch.ok() && failed(estimate_fetch, &outcome)) return outcome;
  const Value* estimate = unwrap(estimate_fetch.value);
  const char* estimated = positive_atomic(arena, Field(estimate, "estimatedAmount"), to->decimals);
  if (!estimated) return Outcome::Ok();
  const double now = context.Now();
  if (!request.prepare) {
    const char* expiry = bounded_expiry(arena, Field(estimate, "validUntil"), now, 60000);
    if (!expiry) return Outcome::Ok();
    *routes = Routes(arena, swap_route(arena, request, *from, *to, estimated, now, expiry, nullptr, nullptr));
    return *routes ? Outcome::Ok() : Error();
  }
  Value* body = tj::Obj(arena);
  if (!body || !SetStr(arena, body, "tickerFrom", from->ticker) || !SetStr(arena, body, "networkFrom", from->network) ||
      !SetStr(arena, body, "tickerTo", to->ticker) || !SetStr(arena, body, "networkTo", to->network) ||
      !SetBool(arena, body, "fixed", false) || !SetBool(arena, body, "reverse", false) ||
      !SetStr(arena, body, "amount", amount) || !SetVal(arena, body, "addressTo", request.to.address) ||
      !SetVal(arena, body, "userRefundAddress", request.from.address) || !SetStr(arena, body, "extraIdTo", "") ||
      !SetStr(arena, body, "userRefundExtraId", "") || !SetVal(arena, body, "rateId", nullptr))
    return Error();
  Fetch create;
  create.lane = "simpleswap";
  create.base = kSimpleSwapBase;
  create.path = "/v3/exchanges";
  create.post = true;
  create.body = body;
  create.header_name = "x-api-key";
  create.header_value = settings.simpleswap_api_key;
  create.timeout_ms = 15000;
  RunFetch(context, arena, create);
  if (!create.ok() && failed(create, &outcome)) return outcome;
  const Value* order = unwrap(create.value);
  const Value* amount_from = Field(order, "amountFrom");
  const char* atomic_from = Nullish(amount_from) ? nullptr : to_atomic(arena, amount_from, from->decimals);
  const Value* amount_to = Field(order, "amountTo");
  const Value* address_from = Field(order, "addressFrom");
  const char* expected = to_atomic(arena, Field(order, "expectedAmount"), from->decimals);
  if (!order || !valid_order_id(tj::Get(order, "id")) || !Is(tj::Get(order, "type"), "floating") ||
      !Is(tj::Get(order, "status"), "waiting") || !swap_matches_pair(order, *from, *to) ||
      !swap_same_address(arena, request.to.chain_id, tj::Get(order, "addressTo"), request.to.address) ||
      !swap_same_address(arena, request.from.chain_id, tj::Get(order, "userRefundAddress"), request.from.address) ||
      !expected || !Is(request.amount_in, expected) ||
      (!Nullish(amount_from) && !(atomic_from && (!strcmp(atomic_from, "0") || Is(request.amount_in, atomic_from)))) ||
      (!Nullish(amount_to) && !to_atomic(arena, amount_to, to->decimals)) || has_value(tj::Get(order, "extraIdFrom")) ||
      has_value(tj::Get(order, "extraIdTo")) || has_value(tj::Get(order, "userRefundExtraId")) ||
      has_value(tj::Get(order, "redirectUrl")) || !address_from || address_from->type != tj::Type::kString ||
      !swap_valid_address(request.from.chain_id, address_from))
    return Outcome::Ok();
  const char* expiry = bounded_expiry(arena, tj::Get(order, "validUntil"), now, 5 * 60000);
  if (!expiry) return Outcome::Ok();
  const char* output = positive_atomic(arena, amount_to, to->decimals);
  if (!output) output = estimated;
  *routes = Routes(arena, swap_route(arena, request, *from, *to, output, now, expiry, tj::Get(order, "id"), address_from));
  return *routes ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome SimpleSwapProvider::Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  if (!settings.simpleswap_configured || !Is(request.provider_id, "simpleswap") ||
      !valid_order_id(request.provider_quote_id) || !swap_tx_hash(request.from_chain_id, request.tx_hash) ||
      !swap_supported_pair(request.from_chain_id, request.to_chain_id))
    return Error();
  const SwapAsset& from = *swap_asset_for(request.from_chain_id);
  const SwapAsset& to = *swap_asset_for(request.to_chain_id);
  if ((request.to_token && !Is(request.to_token, to.token)) || (request.has_to_decimals && request.to_decimals != to.decimals))
    return Error();
  Text path;
  if (!path.Init(arena, request.provider_quote_id->length * 3 + 64)) return Error();
  path.out.Text("/v3/exchanges/");
  if (!EncodeComponent(request.provider_quote_id, path.out)) return Error();
  Fetch fetch;
  fetch.lane = "simpleswap";
  fetch.base = kSimpleSwapBase;
  fetch.path = path.Finish();
  fetch.header_name = "x-api-key";
  fetch.header_value = settings.simpleswap_api_key;
  fetch.timeout_ms = 15000;
  if (!fetch.path) return Error();
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  const Value* order = unwrap(fetch.value);
  if (!order || !SameText(tj::Get(order, "id"), request.provider_quote_id) || !swap_matches_pair(order, from, to))
    return Error();
  const Value* source_hash = swap_tx_hash(request.from_chain_id, tj::Get(order, "txFrom"));
  if (has_value(tj::Get(order, "txFrom"))) {
    if (!source_hash) return Error();
    const bool same = Is(request.from_chain_id, "solana-mainnet-beta") ? SameText(source_hash, request.tx_hash)
                                                                       : same_lower(arena, source_hash, request.tx_hash);
    if (!same) return Error();
  }
  const Value* destination_hash = swap_tx_hash(request.to_chain_id, tj::Get(order, "txTo"));
  const Value* raw_status = tj::Get(order, "status");
  const Value* state_text = raw_status && raw_status->type == tj::Type::kString ? raw_status : str(arena, "unknown");
  const char* amount = positive_atomic(arena, tj::Get(order, "amountTo"), to.decimals);
  if (Is(state_text, "finished") && (!source_hash || !destination_hash || !amount)) return Error();
  const bool active = Is(state_text, "waiting") || Is(state_text, "confirming") || Is(state_text, "exchanging") ||
                      Is(state_text, "sending");
  const char* state = Is(state_text, "finished") ? "completed" : Is(state_text, "refunded") ? "refunded"
                      : Is(state_text, "failed") || Is(state_text, "expired") ? "failed"
                      : Is(state_text, "verifying") || !active ? "needs-action" : "pending";
  const bool needs_help = !strcmp(state, "failed") || !strcmp(state, "needs-action");
  const char* message = !strcmp(state, "completed") ? "SimpleSwap reports the destination payment sent."
                        : !strcmp(state, "refunded") ? "SimpleSwap reports the deposit refunded."
                        : Is(state_text, "verifying") ? "SimpleSwap requires verification. Contact the provider with your order ID."
                        : needs_help ? "Check this order with SimpleSwap before sending another deposit."
                                     : "Waiting for SimpleSwap to confirm the deposit and deliver the destination funds.";
  double elapsed = 0;
  const bool has_elapsed = SecondsBetween(SafeTimestamp(arena, tj::Get(order, "createdAt")),
                                          SafeTimestamp(arena, tj::Get(order, "updatedAt")), &elapsed);
  Value* received = tj::Null(arena);
  if (!strcmp(state, "completed")) {
    received = tj::Obj(arena);
    if (!received || !SetVal(arena, received, "chainId", request.to_chain_id) || !SetStr(arena, received, "amount", amount) ||
        !SetStr(arena, received, "tokenAddress", to.token) || !SetStr(arena, received, "symbol", to.upper) ||
        !SetNum(arena, received, "decimals", to.decimals))
      return Error();
  }
  char label[32];
  snprintf(label, sizeof(label), "%s received", to.upper);
  Value* progress = tj::Arr(arena);
  Value* source = ProgressStep(arena, "source", "source", str(arena, "Source deposit"), source_hash ? "completed" : "pending",
                               request.from_chain_id, source_hash ? source_hash : request.tx_hash);
  Value* bridge = ProgressStep(arena, "bridge", "bridge", str(arena, "SimpleSwap exchange"), state, nullptr, nullptr);
  Value* destination = ProgressStep(arena, "destination", "destination", str(arena, label),
                                    !strcmp(state, "completed") ? "completed" : "not-started", request.to_chain_id,
                                    destination_hash);
  if (!progress || !source || !bridge || !destination || !tj::Push(arena, progress, source) ||
      !tj::Push(arena, progress, bridge) || !tj::Push(arena, progress, destination))
    return Error();
  Value* recovery = tj::Null(arena);
  if (needs_help)
    recovery = Recovery(arena, "contact-support", str(arena, "Contact SimpleSwap"), str(arena, message),
                        str(arena, "Open SimpleSwap"), str(arena, kSimpleSwapWebsite));
  else if (!strcmp(state, "refunded"))
    recovery = Recovery(arena, "track-refund", str(arena, "Check your refund"),
                        str(arena, "Sync the source wallet to check the refunded funds."), nullptr, nullptr);
  Value* data = tj::Obj(arena);
  Value* out = tj::Obj(arena);
  if (!recovery || !received || !data || !out || !SetVal(arena, data, "id", tj::Get(order, "id")) ||
      !SetVal(arena, data, "status", state_text) || !SetStr(arena, out, "providerId", "simpleswap") ||
      !SetStr(arena, out, "state", state) || !SetVal(arena, out, "substatus", state_text) ||
      !SetStr(arena, out, "message", message) ||
      !SetVal(arena, out, "sourceHash", source_hash ? source_hash : request.tx_hash) ||
      !SetVal(arena, out, "destinationHash", destination_hash) || !SetVal(arena, out, "providerExplorerUrl", nullptr) ||
      !SetVal(arena, out, "bridgeExplorerUrl", nullptr) ||
      !SetNumOrNull(arena, out, "elapsedSeconds", has_elapsed, elapsed) ||
      !tj::Set(arena, out, "actualReceived", received) || !tj::Set(arena, out, "progress", progress) ||
      !tj::Set(arena, out, "recovery", recovery) || !tj::Set(arena, out, "providerData", data))
    return Error();
  *status = out;
  return Outcome::Ok();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
namespace {
bool simpleswap_configured(const ProviderSettings& settings) { return settings.simpleswap_configured; }
}  // namespace
const SimpleSwapProvider kSimpleSwapProvider{};

const char* SimpleSwapProvider::Website() const { return detail::kSimpleSwapWebsite; }
bool SimpleSwapProvider::Configured(const ProviderSettings& settings) const { return simpleswap_configured(settings); }
}  // namespace fortis::exchange::cross_chain
