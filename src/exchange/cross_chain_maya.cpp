// MAYAChain (port of the TypeScript adapter): a deposit-address provider.
// Passive pricing never creates an order; only a prepared quote allocates a
// quote-bound deposit target, which the adapter re-validates against the
// request before the wallet may sign anything.
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
constexpr const char* kMayaPublicApi = "https://mayanode.mayachain.info/mayachain";

struct MayaAsset {
  const char* asset;
  const char* symbol;
};

bool maya_valid_address(const Value* chain_id, const Value* address) {
  return Is(chain_id, "dash") ? ValidDashAddress(address) : Is(chain_id, "1") && ValidEvmAddress(address);
}

bool maya_assets(Arena& arena, const QuoteRequest& request, MayaAsset* source, MayaAsset* destination) {
  if (!Is(request.from.chain_id, "dash") || !Is(request.to.chain_id, "1")) return false;
  if (!(lower_is(arena, request.from.token, "dash") && request.from.decimals == 8)) return false;
  if (!(lower_is(arena, request.to.token, kNativeEvm) && request.to.decimals == 18)) return false;
  if (!maya_valid_address(request.from.chain_id, request.from.address) ||
      !maya_valid_address(request.to.chain_id, request.to.address))
    return false;
  *source = MayaAsset{"DASH.DASH", "DASH"};
  *destination = MayaAsset{"ETH.ETH", "ETH"};
  return true;
}

// toMayaAmount / fromMayaAmount: rescale to/from MAYA's 8 decimals.
const char* to_maya(Arena& arena, const Value* amount, uint32_t decimals) {
  if (!amount || !PositiveIntegerText(amount->text, amount->length)) return nullptr;
  const char* raw = CText(arena, amount);
  const char* scaled = decimals == 8 ? raw : decimals > 8 ? ScaleDown(arena, raw, decimals - 8) : ScaleUp(arena, raw, 8 - decimals);
  // safePositiveInteger(toMayaAmount(...)): at most 80 digits.
  return scaled && !IsZeroDecimal(scaled) && PositiveIntegerText(scaled, strlen(scaled)) ? scaled : nullptr;
}
const char* from_maya(Arena& arena, const char* amount, uint32_t decimals) {
  if (!amount || !PositiveIntegerText(amount, strlen(amount))) return nullptr;
  const char* scaled = decimals == 8 ? amount : decimals > 8 ? ScaleUp(arena, amount, decimals - 8) : ScaleDown(arena, amount, 8 - decimals);
  return scaled && !IsZeroDecimal(scaled) ? scaled : nullptr;
}

// mayaInteger(value, allowZero): /^(\d{1,40})(?:[eE]\+?(\d{1,2}))?$/.
const char* maya_integer(Arena& arena, const char* text, size_t length, bool allow_zero) {
  size_t digits = 0;
  while (digits < length && text[digits] >= '0' && text[digits] <= '9') ++digits;
  if (digits < 1 || digits > 40) return nullptr;
  uint32_t exponent = 0;
  if (digits < length) {
    size_t i = digits;
    if (text[i] != 'e' && text[i] != 'E') return nullptr;
    ++i;
    if (i < length && text[i] == '+') ++i;
    const size_t start = i;
    while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
    if (i != length || i - start < 1 || i - start > 2) return nullptr;
    for (size_t k = start; k < i; ++k) exponent = exponent * 10 + static_cast<uint32_t>(text[k] - '0');
  }
  if (exponent > 32) return nullptr;
  const char* base = arena.CopyText(text, digits);
  const char* value = base ? ScaleUp(arena, base, exponent) : nullptr;
  if (!value) return nullptr;
  const bool zero = IsZeroDecimal(value);
  return !zero || allow_zero ? value : nullptr;
}

struct MayaMemo {
  const char* asset = nullptr;
  const Value* destination = nullptr;
  const char* minimum = nullptr;
  const char* interval = nullptr;  // nullptr: null
  const char* quantity = nullptr;
};

bool parse_maya_memo(Arena& arena, const Value* memo, MayaMemo* out) {
  // memo.split(':').
  const Value* fields[16];
  size_t count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= memo->length; ++i) {
    if (i < memo->length && memo->text[i] != ':') continue;
    if (count == 16) return false;
    fields[count++] = tj::Str(arena, memo->text + start, i - start);
    if (!fields[count - 1]) return false;
    start = i + 1;
  }
  if (count < 4 || count > 9) return false;
  const Value* verb = Lower(arena, fields[0]);
  if (!verb || !(Is(verb, "=") || Is(verb, "swap") || Is(verb, "s"))) return false;
  const Value* asset = Upper(arena, fields[1]);
  if (!asset || !(Is(asset, "ETH.ETH") || Is(asset, "E"))) return false;
  const Value* destination = fields[2];
  if (!destination->length || Contains(destination, "/")) return false;
  for (size_t i = 4; i < count; ++i)
    if (fields[i]->length) return false;
  const Value* limit = fields[3];
  size_t slash[3];
  size_t slashes = 0;
  for (size_t i = 0; i < limit->length; ++i)
    if (limit->text[i] == '/') {
      if (slashes == 3) return false;
      slash[slashes++] = i;
    }
  if (slashes != 0 && slashes != 2) return false;
  const size_t first_end = slashes ? slash[0] : limit->length;
  const char* minimum = maya_integer(arena, limit->text, first_end, false);
  if (!minimum) return false;
  if (slashes == 2) {
    const char* interval = maya_integer(arena, limit->text + slash[0] + 1, slash[1] - slash[0] - 1, false);
    const char* quantity = maya_integer(arena, limit->text + slash[1] + 1, limit->length - slash[1] - 1, true);
    if (!interval || !quantity) return false;
    out->interval = interval;
    out->quantity = quantity;
  }
  out->asset = "ETH.ETH";
  out->destination = destination;
  out->minimum = minimum;
  return true;
}

bool same_destination(Arena& arena, const Value* first, const Value* second) {
  const Value* a = Lower(arena, first);
  const Value* b = Lower(arena, second);
  if (StartsWith(a, "0x") || StartsWith(b, "0x")) return SameText(a, b);
  return SameText(first, second);
}

bool within_bps(Arena& arena, const char* first, const char* second, uint32_t tolerance) {
  if (IsZeroDecimal(first) || IsZeroDecimal(second) || tolerance >= 10000) return false;
  const bool first_larger = CompareDecimal(first, second) > 0;
  const char* larger = first_larger ? first : second;
  const char* difference = first_larger ? SubDecimal(arena, first, second) : SubDecimal(arena, second, first);
  if (!difference) return false;
  const char* left = MulSmallDecimal(arena, difference, 10000);
  const char* right = MulSmallDecimal(arena, larger, tolerance);
  return left && right && CompareDecimal(left, right) <= 0;
}

// optionalPositiveInteger(): absent/null -> none; else safePositiveInteger.
bool optional_positive(Arena& arena, const Value* v, const char** out) {
  *out = nullptr;
  if (Nullish(v)) return true;
  *out = SafePositiveInteger(arena, v);
  return true;
}

// expiryIso(value, now): at most ten minutes ahead, never already past.
const char* maya_expiry(Arena& arena, const Value* value, double now) {
  const char* parsed = SafeTimestamp(arena, value);
  double ms = 0;
  if (!parsed || !DateParse(parsed, strlen(parsed), &ms) || ms <= now) return nullptr;
  return iso_min(arena, ms, now + 10 * 60000);
}

Value* parse_maya_route(Arena& arena, const Value* payload, const QuoteRequest& request, double now) {
  MayaAsset source{}, destination{};
  const Value* root = AsRecord(payload);
  if (!maya_assets(arena, request, &source, &destination) || !root) return nullptr;
  const char* expected = SafePositiveInteger(arena, tj::Get(root, "expected_amount_out"));
  const Value* inbound = StringValue(tj::Get(root, "inbound_address"));
  const Value* memo = StringValue(tj::Get(root, "memo"), 512);
  if (!expected || !inbound || !memo) return nullptr;
  if (!maya_valid_address(request.from.chain_id, inbound)) return nullptr;
  const char* scaled = MulSmallDecimal(arena, expected, 10000 - request.slippage_bps);
  const char* requested_minimum = scaled ? DivSmallDecimal(arena, scaled, 10000) : nullptr;
  MayaMemo parsed;
  if (!requested_minimum || IsZeroDecimal(requested_minimum) || !parse_maya_memo(arena, memo, &parsed) ||
      strcmp(parsed.asset, destination.asset) != 0 ||
      !same_destination(arena, parsed.destination, request.to.address) ||
      CompareDecimal(parsed.minimum, requested_minimum) < 0 || CompareDecimal(parsed.minimum, expected) > 0)
    return nullptr;
  const char* input = to_maya(arena, request.amount_in, request.from.decimals);
  const char* dust = nullptr;
  const char* recommended = nullptr;
  optional_positive(arena, tj::Get(root, "dust_threshold"), &dust);
  const Value* raw_recommended = tj::Get(root, "recommended_min_amount_in");
  optional_positive(arena, raw_recommended, &recommended);
  if (!input || (dust && CompareDecimal(input, dust) < 0) || (!Nullish(raw_recommended) && !recommended) ||
      (recommended && CompareDecimal(input, recommended) < 0))
    return nullptr;
  // Dash transports the routing instruction in a standard OP_RETURN output.
  if (Is(request.from.chain_id, "dash") && memo->length > 80) return nullptr;
  const char* amount_out = from_maya(arena, expected, request.to.decimals);
  const char* minimum = from_maya(arena, parsed.minimum, request.to.decimals);
  const char* expiry = maya_expiry(arena, tj::Get(root, "expiry"), now);
  if (!amount_out || !minimum || !expiry) return nullptr;
  // safeEta(total_swap_seconds, sumEta(inbound_confirmation_seconds, outbound_delay_seconds)).
  double a = 0, b = 0;
  const Value* first[] = {tj::Get(root, "inbound_confirmation_seconds")};
  const Value* second[] = {tj::Get(root, "outbound_delay_seconds")};
  const bool has_a = SafeEta(first, 1, &a), has_b = SafeEta(second, 1, &b);
  const Value* sum = has_a || has_b ? num_value(arena, (has_a ? a : 0) + (has_b ? b : 0)) : nullptr;
  const Value* eta_values[] = {tj::Get(root, "total_swap_seconds"), sum};
  RouteFields f;
  f.has_eta = SafeEta(eta_values, 2, &f.eta_seconds);
  const bool prepared = request.prepare;
  const bool evm = Is(request.from.chain_id, "1");
  ExecutionFields execution;
  execution.memo_key = true;
  if (prepared) {
    execution.kind = evm ? "evm-transfer" : "deposit";
    execution.to = inbound;
    execution.memo = memo;
    execution.data = evm ? str(arena, "0x") : nullptr;
    execution.value = request.amount_in;
    execution.gas_limit = evm ? str(arena, "21000") : nullptr;
  }
  Value* steps = tj::Arr(arena);
  Value* step = tj::Obj(arena);
  if (!steps || !step || !SetStr(arena, step, "kind", "bridge") || !SetStr(arena, step, "tool", "MAYAChain") ||
      !SetVal(arena, step, "fromChainId", request.from.chain_id) || !SetVal(arena, step, "toChainId", request.to.chain_id) ||
      !SetStr(arena, step, "fromToken", source.symbol) || !SetStr(arena, step, "toToken", destination.symbol) ||
      !tj::Push(arena, steps, step))
    return nullptr;
  f.provider_id = kMayaProvider.Id();
  f.provider_name = kMayaProvider.Name();
  f.provider_url = kMayaProvider.Website();
  f.bridge_id = str(arena, "mayachain");
  f.bridge = str(arena, "MAYAChain");
  f.bridge_url = BridgeWebsiteUrl(arena, f.bridge_id);
  f.risk_key = "mayachain";
  f.custody = kMayaProvider.Custody();
  f.output_protection = "minimum";
  f.amount_in = request.amount_in;
  f.amount_out = amount_out;
  f.amount_out_min = minimum;
  f.decimals_in = request.from.decimals;
  f.decimals_out = request.to.decimals;
  f.quoted_at = IsoText(arena, now);
  f.expires_at = expiry;
  f.steps = steps;
  f.execution = Execution(arena, execution);
  if (!f.execution || !f.quoted_at) return nullptr;
  return Route(arena, f);
}

struct DashInbound {
  bool ok = false;
  const Value* address = nullptr;
  bool halted = false;
  bool paused = false;
  const char* dust = nullptr;
};

// optionalBoolean(): absent/null -> false; a boolean; anything else fails.
bool optional_boolean(const Value* v, bool* out) {
  if (Nullish(v)) { *out = false; return true; }
  if (v->type == tj::Type::kTrue || v->type == tj::Type::kFalse) { *out = v->type == tj::Type::kTrue; return true; }
  return false;
}

DashInbound dash_inbound(Arena& arena, const Value* payload) {
  DashInbound state;
  if (!payload || payload->type != tj::Type::kArray) return state;
  for (uint32_t i = 0; i < payload->count; ++i) {
    const Value* record = AsRecord(payload->items[i]);
    const Value* chain = StringValue(Field(record, "chain"));
    const Value* upper = chain ? Upper(arena, chain) : nullptr;
    if (!upper || !Is(upper, "DASH")) continue;
    const Value* address = StringValue(Field(record, "address"));
    const char* dust = SafePositiveInteger(arena, Field(record, "dust_threshold"));
    if (!address || !ValidDashAddress(address) || !dust) return state;
    const Value* halted = tj::Get(record, "halted");
    if (!halted || (halted->type != tj::Type::kTrue && halted->type != tj::Type::kFalse)) return state;
    bool chain_paused = false, global_paused = false;
    if (!optional_boolean(tj::Get(record, "chain_trading_paused"), &chain_paused) ||
        !optional_boolean(tj::Get(record, "global_trading_paused"), &global_paused))
      return state;
    state.ok = true;
    state.address = address;
    state.halted = halted->type == tj::Type::kTrue;
    state.paused = chain_paused || global_paused;
    state.dust = dust;
    return state;
  }
  return state;
}

struct MayaObservation {
  Value* route = nullptr;
  DashInbound inbound;
  const Value* quoted_inbound = nullptr;
  const Value* memo = nullptr;
};

bool same_intent(Arena& arena, const MayaObservation& first, const MayaObservation& candidate, uint32_t tolerance) {
  if (!first.route || !candidate.route || !first.memo || !candidate.memo) return false;
  const bool both_null = !first.quoted_inbound && !candidate.quoted_inbound;
  if (!both_null && !SameText(first.quoted_inbound, candidate.quoted_inbound)) return false;
  MayaMemo a, b;
  if (!parse_maya_memo(arena, first.memo, &a) || !parse_maya_memo(arena, candidate.memo, &b)) return false;
  auto same_optional = [](const char* x, const char* y) { return (!x && !y) || (x && y && CompareDecimal(x, y) == 0); };
  const Value* first_min = tj::Get(first.route, "amountOutMin");
  const Value* second_min = tj::Get(candidate.route, "amountOutMin");
  return SameText(tj::Get(first.route, "amountIn"), tj::Get(candidate.route, "amountIn")) &&
         strcmp(a.asset, b.asset) == 0 && same_destination(arena, a.destination, b.destination) &&
         same_optional(a.interval, b.interval) && same_optional(a.quantity, b.quantity) &&
         within_bps(arena, CText(arena, first_min), CText(arena, second_min), tolerance);
}

// progressStep() of the TypeScript MAYAChain adapter.
Value* maya_step(Arena& arena, const char* id, const char* kind, const char* label, const char* state,
                 const Value* chain_id, const Value* tx_hash, const char* detail = nullptr) {
  Value* step = ProgressStep(arena, id, kind, str(arena, label), state, chain_id, tx_hash);
  if (!step || !SetStr(arena, step, "detail", detail)) return nullptr;
  return step;
}

bool completed_stage(const Value* stages, const char* key) {
  const Value* v = Field(tj::Get(stages, key), "completed");
  return v && v->type == tj::Type::kTrue;
}

Value* parse_maya_status(Arena& arena, const Value* payload, const StatusRequest& request) {
  const Value* root = AsRecord(payload);
  const Value* stages = AsRecord(Field(root, "stages"));
  const bool observed = completed_stage(stages, "inbound_observed");
  const bool finalised = completed_stage(stages, "inbound_finalised") ||
                         completed_stage(stages, "inbound_confirmation_counted");
  const bool swap_finalised = completed_stage(stages, "swap_finalised");
  const bool outbound_signed = completed_stage(stages, "outbound_signed");
  const Value* out_txs = Field(root, "out_txs");
  const bool txs_array = out_txs && out_txs->type == tj::Type::kArray;
  const Value* first_tx = txs_array && out_txs->count ? AsRecord(out_txs->items[0]) : nullptr;
  const Value* destination_hash =
      StringValue(Coalesce(Coalesce(Field(first_tx, "id"), Field(first_tx, "tx_id")), Field(first_tx, "hash")));
  const Value* status_raw = StringValue(Field(root, "status"), 128);
  const Value* status_text = status_raw ? Lower(arena, status_raw) : str(arena, "");
  const Value* message = StringValue(Coalesce(Field(root, "error"), Field(root, "message")), 1024);
  bool refund = Contains(status_text, "refund");
  if (txs_array)
    for (uint32_t i = 0; i < out_txs->count && !refund; ++i) {
      const Value* memo = StringValue(Field(out_txs->items[i], "memo"));
      const Value* upper = memo ? Upper(arena, memo) : nullptr;
      refund = upper && StartsWith(upper, "REFUND:");
    }
  const bool failed = Contains(status_text, "fail") || Contains(status_text, "error");
  const bool not_found = !truthy(Field(root, "tx")) && (!stages || stages->count == 0) && (!txs_array || out_txs->count == 0);
  const char* state = refund ? "refunded" : failed ? "failed" : outbound_signed || destination_hash ? "completed"
                      : not_found ? "not-found" : "pending";
  const char* bridge_state = refund ? "refunded" : failed ? "failed" : swap_finalised ? "completed"
                             : observed ? "finalizing" : "pending";
  Value* progress = tj::Arr(arena);
  Value* source = maya_step(arena, "source", "source", "Source transaction",
                            observed ? "completed" : !strcmp(state, "not-found") ? "pending" : "finalizing",
                            request.from_chain_id, request.tx_hash);
  Value* bridge = maya_step(arena, "bridge", "bridge", "MAYAChain swap", bridge_state, nullptr, nullptr,
                            finalised ? "Inbound finality reached." : nullptr);
  Value* destination = maya_step(arena, "destination", refund ? "refund" : "destination",
                                 refund ? "Refund" : "Destination transaction",
                                 refund ? "refunded" : destination_hash || outbound_signed ? "completed"
                                                       : failed ? "failed" : "not-started",
                                 refund ? request.from_chain_id : request.to_chain_id, destination_hash);
  if (!progress || !source || !bridge || !destination || !tj::Push(arena, progress, source) ||
      !tj::Push(arena, progress, bridge) || !tj::Push(arena, progress, destination))
    return nullptr;
  Text url;
  if (!url.Init(arena, request.tx_hash->length * 3 + 64)) return nullptr;
  url.out.Text("https://www.mayascan.org/tx/");
  if (!EncodeComponent(request.tx_hash, url.out)) return nullptr;
  const char* explorer_text = url.Finish();
  if (!explorer_text) return nullptr;
  Value* explorer = str(arena, explorer_text);
  Value* recovery = tj::Null(arena);
  if (refund)
    recovery = Recovery(arena, "track-refund", str(arena, "Refund is being returned"),
                        str(arena, "MAYAChain could not complete the swap and is returning the inbound asset."),
                        str(arena, "Track on MAYAScan"), explorer);
  else if (failed)
    recovery = Recovery(arena, "contact-support", str(arena, "MAYAChain assistance may be required"),
                        message ? message : str(arena, "Share the source transaction hash with MAYAChain support before retrying."),
                        str(arena, "Track on MAYAScan"), explorer);
  Value* data = tj::Obj(arena);
  Value* out = tj::Obj(arena);
  if (!recovery || !data || !out || !tj::Set(arena, data, "maya", BoundedProviderSnapshot(arena, payload)) ||
      !SetStr(arena, out, "providerId", "maya") || !SetStr(arena, out, "state", state) ||
      !SetVal(arena, out, "substatus", status_text->length ? status_text : nullptr) ||
      !SetVal(arena, out, "message", message) || !SetVal(arena, out, "sourceHash", request.tx_hash) ||
      !SetVal(arena, out, "destinationHash", destination_hash) || !SetVal(arena, out, "providerExplorerUrl", explorer) ||
      !SetVal(arena, out, "bridgeExplorerUrl", explorer) || !SetVal(arena, out, "elapsedSeconds", nullptr) ||
      !SetVal(arena, out, "actualReceived", nullptr) || !tj::Set(arena, out, "progress", progress) ||
      !tj::Set(arena, out, "recovery", recovery) || !tj::Set(arena, out, "providerData", data))
    return nullptr;
  return out;
}

// /^(?:0x)?[0-9a-fA-F]{64}$/
bool maya_tx_hash(const Value* v) {
  if (!v || v->type != tj::Type::kString) return false;
  size_t start = StartsWith(v, "0x") && v->length == 66 ? 2 : 0;
  if (v->length - start != 64) return false;
  for (size_t i = start; i < v->length; ++i) {
    const char c = v->text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

}  // namespace
}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome MayaProvider::Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  *routes = Routes(arena, nullptr);
  if (!*routes) return Error();
  if (!settings.maya_execution_enabled || settings.maya_root_count < 2) return Outcome::Ok();
  MayaAsset source{}, destination{};
  if (!maya_assets(arena, request, &source, &destination)) return Outcome::Ok();
  const char* maya_amount = to_maya(arena, request.amount_in, request.from.decimals);
  if (!maya_amount) return Outcome::Ok();
  char tolerance[16];
  snprintf(tolerance, sizeof(tolerance), "%u", request.slippage_bps);
  Text query;
  if (!query.Init(arena, value_bytes(request.to.address) + 256)) return Error();
  QueryPair pairs[] = {{"from_asset", str(arena, source.asset)},
                       {"to_asset", str(arena, destination.asset)},
                       {"amount", str(arena, maya_amount)},
                       {"destination", request.to.address},
                       {"liquidity_tolerance_bps", str(arena, tolerance)}};
  FormEncode(pairs, 5, query.out);
  const char* query_text = query.Finish();
  if (!query_text) return Error();
  const double now = context.Now();
  const size_t roots = settings.maya_root_count;
  Fetch fetches[2 * kMaxMayaRoots];
  for (size_t i = 0; i < roots; ++i) {
    Text quote_path, inbound_path;
    // A root is "https://host[:port][/prefix]"; the prefix joins the path.
    const char* root = settings.maya_roots[i];
    const char* authority = root + 8;
    const char* slash = strchr(authority, '/');
    const size_t base_length = slash ? static_cast<size_t>(slash - root) : strlen(root);
    const char* prefix = slash ? slash : "";
    char* base = static_cast<char*>(arena.Alloc(base_length + 1, 1));
    if (!base || !quote_path.Init(arena, strlen(prefix) + strlen(query_text) + 64) ||
        !inbound_path.Init(arena, strlen(prefix) + 64))
      return Error();
    memcpy(base, root, base_length);
    base[base_length] = '\0';
    quote_path.out.Text(prefix);
    quote_path.out.Text("/quote/swap?");
    quote_path.out.Text(query_text);
    inbound_path.out.Text(prefix);
    inbound_path.out.Text("/inbound_addresses");
    Fetch& quote = fetches[2 * i];
    Fetch& inbound = fetches[2 * i + 1];
    quote.lane = inbound.lane = "maya";
    quote.base = inbound.base = base;
    quote.path = quote_path.Finish();
    inbound.path = inbound_path.Finish();
    quote.timeout_ms = inbound.timeout_ms = 12000;
    if (!quote.path || !inbound.path) return Error();
  }
  RunFetches(context, arena, fetches, 2 * roots);
  for (size_t i = 0; i < 2 * roots; ++i)
    if (!fetches[i].ok()) return fetches[i].error();
  MayaObservation observations[kMaxMayaRoots];
  for (size_t i = 0; i < roots; ++i) {
    const Value* quote_payload = fetches[2 * i].value;
    const Value* quote_record = AsRecord(quote_payload);
    observations[i].route = parse_maya_route(arena, quote_payload, request, now);
    observations[i].inbound = dash_inbound(arena, fetches[2 * i + 1].value);
    observations[i].quoted_inbound = StringValue(Field(quote_record, "inbound_address"));
    observations[i].memo = StringValue(Field(quote_record, "memo"), 512);
  }
  const MayaObservation& first = observations[0];
  if (!first.route) return Outcome::Ok();
  for (size_t i = 0; i < roots; ++i) {
    const MayaObservation& o = observations[i];
    if (!o.route || !o.inbound.ok || !SameText(o.quoted_inbound, o.inbound.address) || o.inbound.halted ||
        o.inbound.paused || CompareDecimal(maya_amount, o.inbound.dust) < 0 ||
        !same_intent(arena, first, o, request.slippage_bps))
      return Outcome::Ok();
  }
  size_t selected = 0;
  for (size_t i = 1; i < roots; ++i) {
    const char* candidate = CText(arena, tj::Get(observations[i].route, "amountOutMin"));
    const char* current = CText(arena, tj::Get(observations[selected].route, "amountOutMin"));
    if (CompareDecimal(candidate, current) < 0) selected = i;
  }
  *routes = Routes(arena, observations[selected].route);
  return *routes ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome MayaProvider::Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const {
  using namespace detail;
  if (!maya_tx_hash(request.tx_hash)) return Error();
  const ProviderSettings& settings = *context.settings;
  const char* root = settings.maya_root_count ? settings.maya_roots[0] : kMayaPublicApi;
  const char* authority = root + 8;
  const char* slash = strchr(authority, '/');
  const size_t base_length = slash ? static_cast<size_t>(slash - root) : strlen(root);
  char* base = static_cast<char*>(arena.Alloc(base_length + 1, 1));
  Text path;
  if (!base || !path.Init(arena, (slash ? strlen(slash) : 0) + request.tx_hash->length * 3 + 64)) return Error();
  memcpy(base, root, base_length);
  base[base_length] = '\0';
  path.out.Text(slash ? slash : "");
  path.out.Text("/tx/status/");
  if (!EncodeComponent(request.tx_hash, path.out)) return Error();
  Fetch fetch;
  fetch.lane = "maya";
  fetch.base = base;
  fetch.path = path.Finish();
  fetch.timeout_ms = 12000;
  if (!fetch.path) return Error();
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  *status = parse_maya_status(arena, fetch.value, request);
  return *status ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
namespace {
// MAYA quotes and executes only with the opt-in and two independent MAYANode roots.
bool maya_trading(const ProviderSettings& settings) {
  return settings.maya_execution_enabled && settings.maya_root_count >= 2;
}
}  // namespace

const MayaProvider kMayaProvider{};

bool MayaProvider::Trading(const ProviderSettings& settings) const { return maya_trading(settings); }
}  // namespace fortis::exchange::cross_chain
