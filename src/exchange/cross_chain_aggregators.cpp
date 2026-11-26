// LI.FI and Squid (ports of the TypeScript adapters): calldata
// aggregators with server-held credentials, plus Squid's AxelarScan GMP
// enrichment and its reviewed Cosmos Hub -> Osmosis IBC reduction.
#include "cross_chain_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::cross_chain::detail {
namespace {

constexpr const char* kLifiBase = "https://li.quest";
constexpr const char* kSquidBase = "https://v2.api.squidrouter.com";
constexpr const char* kAxelarBase = "https://api.axelarscan.io";
constexpr const char* kSquidNativeToken = "0xEeeeeEeeeEeEeeEeEeEeeEEEeeeeEeeeeeeeEEeE";
constexpr const char* kFortisNativeToken = "0x0000000000000000000000000000000000000000";

// Arena text from an OutBuffer-built query/path.
struct Text {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  OutBuffer out{};
  bool Init(Arena& arena, size_t bytes) {
    data = static_cast<uint8_t*>(arena.Alloc(bytes, 1));
    capacity = bytes;
    out = OutBuffer{data, bytes, 0, data != nullptr};
    return data != nullptr;
  }
  const char* Finish() {
    if (!out.ok || out.used >= capacity) return nullptr;
    data[out.used] = '\0';
    return reinterpret_cast<const char*>(data);
  }
};

size_t value_bytes(const Value* v) { return v && v->type == tj::Type::kString ? v->length * 9 + 8 : 8; }

const Value* lifi_chain(Arena& arena, const Value* chain_id) {
  if (Is(chain_id, "solana-mainnet-beta")) return S(arena, "1151111081099710");
  if (Is(chain_id, "bitcoin")) return S(arena, "20000000000001");
  return chain_id;
}

bool lower_equals(Arena& arena, const Value* v, const char* text) {
  const Value* lower = Lower(arena, v);
  return lower && Is(lower, text);
}

const Value* squid_token(Arena& arena, const Value* token, const Value* chain_id) {
  if (Is(chain_id, "solana-mainnet-beta") && Is(token, "11111111111111111111111111111111"))
    return S(arena, kSquidNativeToken);
  if (Is(chain_id, "bitcoin") && lower_equals(arena, token, "bitcoin")) return S(arena, "satoshi");
  return lower_equals(arena, token, kFortisNativeToken) ? S(arena, kSquidNativeToken) : token;
}

Value* text_value(Arena& arena, const char* text) { return text ? tj::Str(arena, text) : nullptr; }

// execution.value: safeNonNegativeInteger(tx?.value) ?? stringValue(tx?.value).
const Value* execution_value(Arena& arena, const Value* raw) {
  const char* integer = SafeNonNegativeInteger(arena, raw);
  if (integer) return S(arena, integer);
  return StringValue(raw);
}

// ---- LI.FI ------------------------------------------------------------------------

Value* parse_lifi_route(Arena& arena, const Value* payload, const QuoteRequest& request, double now) {
  const Value* root = AsRecord(payload);
  const Value* estimate = AsRecord(Field(root, "estimate"));
  if (!root || !estimate) return nullptr;
  const char* amount_out = SafePositiveInteger(arena, tj::Get(estimate, "toAmount"));
  const char* amount_out_min = SafePositiveInteger(arena, tj::Get(estimate, "toAmountMin"));
  if (!amount_out || !amount_out_min || CompareDecimal(amount_out_min, amount_out) > 0) return nullptr;
  const Value* action = AsRecord(tj::Get(root, "action"));
  const Value* to_token = AsRecord(Field(action, "toToken"));
  const Value* from_token = AsRecord(Field(action, "fromToken"));
  if (!SameIdentifier(arena, Field(action, "fromChainId"), lifi_chain(arena, request.from.chain_id)) ||
      !SameIdentifier(arena, Field(action, "toChainId"), lifi_chain(arena, request.to.chain_id)) ||
      !SameIdentifier(arena, Field(from_token, "address"), request.from.token) ||
      !SameIdentifier(arena, Field(to_token, "address"), request.to.token) ||
      !SameIdentifier(arena, Field(action, "fromAmount"), request.amount_in))
    return nullptr;
  double reported_decimals = 0;
  if (IntegerNumber(Field(to_token, "decimals"), &reported_decimals) &&
      reported_decimals != static_cast<double>(request.to.decimals))
    return nullptr;
  Value* steps = ParseLifiSteps(arena, tj::Get(root, "includedSteps"));
  if (!steps) return nullptr;
  const Value* bridge_step_tool = nullptr;
  for (uint32_t i = 0; i < steps->count && !bridge_step_tool; ++i)
    if (Is(tj::Get(steps->items[i], "kind"), "bridge")) bridge_step_tool = tj::Get(steps->items[i], "tool");
  const Value* included = tj::Get(root, "includedSteps");
  const Value* raw_bridge = nullptr;
  if (included && included->type == tj::Type::kArray)
    for (uint32_t i = 0; i < included->count && !raw_bridge; ++i) {
      const Value* step = AsRecord(included->items[i]);
      if (step && Is(tj::Get(step, "type"), "cross")) raw_bridge = step;
    }
  const Value* bridge_id = StringValue(Field(tj::Get(raw_bridge, "toolDetails"), "key"));
  if (!bridge_id) bridge_id = StringValue(Field(raw_bridge, "tool"));
  if (!bridge_id) bridge_id = StringValue(Field(tj::Get(root, "toolDetails"), "key"));
  if (!bridge_id) bridge_id = StringValue(tj::Get(root, "tool"));
  const Value* bridge = bridge_step_tool;
  if (!bridge) bridge = StringValue(Field(tj::Get(root, "toolDetails"), "name"));
  if (!bridge) bridge = StringValue(tj::Get(root, "tool"));
  if (!bridge) bridge = S(arena, "Unknown bridge");
  const Value* tx = AsRecord(tj::Get(root, "transactionRequest"));
  const Value* target = StringValue(Field(tx, "to"));
  const Value* data = StringValue(Field(tx, "data"), 200000);
  ExecutionFields execution;
  execution.kind = ExecutionKind(data, target);
  execution.to = target;
  execution.data = data;
  execution.value = execution_value(arena, Field(tx, "value"));
  execution.gas_limit = text_value(arena, SafePositiveInteger(arena, Coalesce(Field(tx, "gasLimit"), Field(tx, "gas"))));
  execution.approval_address = StringValue(tj::Get(estimate, "approvalAddress"));
  Validity validity;
  if (!QuoteValidity(arena, Coalesce(tj::Get(root, "expiresAt"), tj::Get(estimate, "expiresAt")), now, 30000,
                     &validity))
    return nullptr;
  RouteFields f;
  f.provider_id = kLifiProvider.Id();
  f.provider_name = kLifiProvider.Name();
  f.provider_url = kLifiProvider.Website();
  f.provider_quote_id = StringValue(tj::Get(root, "id"));
  f.bridge_id = bridge_id;
  f.bridge = bridge;
  f.bridge_url = BridgeWebsiteUrl(arena, bridge);
  f.risk_key = NormalizedRiskKey(arena, bridge);
  f.custody = kLifiProvider.Custody();
  f.amount_in = request.amount_in;
  f.amount_out = amount_out;
  f.amount_out_min = amount_out_min;
  f.decimals_in = request.from.decimals;
  f.decimals_out = request.to.decimals;
  f.has_fee_usd = SumUsdCosts(estimate, &f.fee_usd);
  f.source_network_fee = SumRawNetworkCosts(arena, estimate);
  const Value* eta[] = {tj::Get(estimate, "executionDuration")};
  f.has_eta = SafeEta(eta, 1, &f.eta_seconds);
  f.quoted_at = validity.quoted_at;
  f.expires_at = validity.expires_at;
  f.steps = steps;
  f.execution = Execution(arena, execution);
  if (!f.execution) return nullptr;
  return Route(arena, f);
}

const Value* str(Arena& arena, const char* text) { return S(arena, text); }

// lifiRecovery().
Value* lifi_recovery(Arena& arena, const char* state, const Value* substatus, const Value* message,
                     const Value* url) {
  const Value* label = url ? str(arena, "Open LI.FI tracker") : nullptr;
  auto make = [&](const char* kind, const char* title, const Value* description) {
    return Recovery(arena, kind, str(arena, title), description, label, url);
  };
  if (!strcmp(state, "completed")) return tj::Null(arena);
  if (!strcmp(state, "partial"))
    return make("swap-received-asset", "Destination swap did not complete",
                str(arena, "The bridged asset reached the destination wallet. Review the actual received token, then "
                           "request a fresh on-chain swap quote if you still want the original asset."));
  if (!strcmp(state, "refunded"))
    return make("track-refund", "Funds were refunded",
                str(arena, "Verify the refund transaction and token balance. A refund can arrive on the source or "
                           "destination network depending on the bridge."));
  if (Is(substatus, "REFUND_IN_PROGRESS"))
    return make("track-refund", "Refund is in progress",
                str(arena, "Do not submit the same route again yet. Keep tracking until the refund transaction and "
                           "returned asset are visible."));
  if (Is(substatus, "NOT_PROCESSABLE_REFUND_NEEDED"))
    return make("track-refund", "This transfer requires recovery",
                str(arena, "The provider reports that normal delivery cannot finish. Refund behavior varies by bridge; "
                           "follow the provider tracker and contact support if no refund progress appears."));
  if (!strcmp(state, "not-found") || !strcmp(state, "pending")) {
    const bool unavailable = Is(substatus, "BRIDGE_NOT_AVAILABLE") || Is(substatus, "CHAIN_NOT_AVAILABLE");
    return make("wait", !strcmp(state, "not-found") ? "Waiting for provider indexing" : "Transfer is still progressing",
                str(arena, unavailable ? "A provider or chain data service is temporarily unavailable. Your "
                                         "transaction may still be progressing; keep polling rather than submitting it "
                                         "again."
                                       : "No recovery action is currently required. Keep this transaction open and "
                                         "wait for the next provider update."));
  }
  static const char* kDescriptions[][2] = {
      {"OUT_OF_GAS", "A transaction ran out of gas. Check the failed stage and provider tracker before retrying or "
                     "adding gas."},
      {"SLIPPAGE_EXCEEDED", "The protected minimum could not be met. If an intermediate asset arrived, use a fresh "
                            "destination-chain swap quote rather than repeating the bridge."},
      {"INSUFFICIENT_ALLOWANCE", "The source token approval was insufficient. Confirm whether the source transaction "
                                 "reverted before creating a new approval and quote."},
      {"INSUFFICIENT_BALANCE", "The wallet did not have enough token or network gas for the attempted step."},
      {"EXPIRED", "The quoted route expired before it could execute. Confirm the source transaction outcome before "
                  "requesting a fresh route."},
  };
  const Value* description = nullptr;
  for (const auto& entry : kDescriptions)
    if (Is(substatus, entry[0])) description = str(arena, entry[1]);
  if (!description) description = message;
  if (!description)
    description = str(arena, "The provider could not complete the route. Share the source hash, quote ID, networks and "
                             "failed step with support before retrying.");
  return make("contact-support", "Provider assistance may be required", description);
}

bool set_step(Arena& arena, Value* step, const char* key, const Value* value) {
  return SetVal(arena, step, key, value);
}

Value* parse_lifi_status(Arena& arena, const Value* payload, const StatusRequest& request) {
  const Value* root = AsRecord(payload);
  const Value* raw_status = StringValue(Field(root, "status"));
  const Value* status = raw_status ? Upper(arena, raw_status) : S(arena, "NOT_FOUND");
  const Value* raw_substatus = StringValue(Field(root, "substatus"));
  const Value* substatus = raw_substatus ? Upper(arena, raw_substatus) : nullptr;
  if (!status || (raw_substatus && !substatus)) return nullptr;
  const Value* sending = AsRecord(Field(root, "sending"));
  const Value* receiving = AsRecord(Field(root, "receiving"));
  const Value* reported_sending_hash = StringValue(Field(sending, "txHash"));
  const Value* sending_hash = reported_sending_hash ? reported_sending_hash : request.tx_hash;
  const Value* receiving_hash = StringValue(Field(receiving, "txHash"));
  const char* sending_at = SafeTimestamp(arena, Field(sending, "timestamp"));
  const char* receiving_at = SafeTimestamp(arena, Field(receiving, "timestamp"));
  const Value* provider_explorer = StringValue(Field(root, "lifiExplorerLink"));
  const Value* bridge_explorer = StringValue(Field(root, "bridgeExplorerLink"));
  const Value* message = StringValue(Field(root, "substatusMessage"));
  const bool wait_source = Is(substatus, "WAIT_SOURCE_CONFIRMATIONS");
  const bool not_found = Is(status, "NOT_FOUND");
  const char* state = Is(status, "PENDING") ? "pending"
                      : Is(status, "FAILED") && Is(substatus, "NOT_PROCESSABLE_REFUND_NEEDED") ? "needs-action"
                      : Is(status, "FAILED") || Is(status, "INVALID") ? "failed"
                      : Is(status, "DONE") && Is(substatus, "REFUNDED") ? "refunded"
                      : Is(status, "DONE") && Is(substatus, "PARTIAL") ? "partial"
                      : Is(status, "DONE") ? "completed"
                                           : "not-found";
  const char* source_state = not_found || wait_source ? "pending"
                             : Is(status, "INVALID") || (Is(status, "FAILED") && !reported_sending_hash) ? "failed"
                                                                                                         : "completed";
  auto is = [](const char* a, const char* b) { return strcmp(a, b) == 0; };
  const char* bridge_state =
      is(state, "completed") || is(state, "partial") ? "completed"
      : receiving_hash                               ? "completed"
      : is(state, "refunded")                        ? "refunded"
      : is(state, "needs-action")                    ? "needs-action"
      : is(state, "failed")                          ? (is(source_state, "failed") ? "not-started" : "failed")
      : wait_source || not_found                     ? "not-started"
                                                     : "pending";
  const char* destination_state = is(state, "completed")      ? "completed"
                                  : is(state, "partial")      ? "failed"
                                  : is(state, "refunded")     ? "not-started"
                                  : is(state, "failed")       ? (receiving_hash ? "failed" : "not-started")
                                  : is(state, "needs-action") ? "not-started"
                                  : wait_source || not_found  ? "not-started"
                                                              : "pending";
  const Value* receiving_token = AsRecord(Field(receiving, "token"));
  const char* received_amount = SafeNonNegativeInteger(arena, Field(receiving, "amount"));
  const Value* refund = AsRecord(Field(root, "refund"));
  const bool refund_in_progress = Is(substatus, "REFUND_IN_PROGRESS");

  Value* progress = tj::Arr(arena);
  if (!progress) return nullptr;
  // Source.
  Value* source = ProgressStep(arena, "source", "source", str(arena, "Source transaction"), source_state,
                               request.from_chain_id, sending_hash);
  if (!source || !set_step(arena, source, "explorerUrl", StringValue(Field(sending, "txLink"))) ||
      !set_step(arena, source, "completedAt",
                is(source_state, "completed") || is(source_state, "failed") ? text_value(arena, sending_at)
                                                                            : nullptr) ||
      !set_step(arena, source, "timingSource", sending_at ? str(arena, "provider") : nullptr) ||
      !set_step(arena, source, "detail",
                wait_source ? str(arena, "Waiting for enough source-network confirmations.") : nullptr) ||
      !set_step(arena, source, "errorCode", is(source_state, "failed") ? substatus : nullptr) ||
      !set_step(arena, source, "errorMessage", is(source_state, "failed") ? message : nullptr) ||
      !tj::Push(arena, progress, source))
    return nullptr;
  // Bridge.
  const Value* bridge_label = StringValue(Field(root, "tool"), 128);
  if (!bridge_label) bridge_label = request.bridge_id;
  if (!bridge_label) bridge_label = str(arena, "Bridge transfer");
  double bridge_duration = 0;
  const bool has_bridge_duration =
      is(bridge_state, "completed") && SecondsBetween(sending_at, receiving_at, &bridge_duration);
  const bool bridge_problem = is(bridge_state, "failed") || is(bridge_state, "needs-action");
  Value* bridge = ProgressStep(arena, "bridge", "bridge", bridge_label, bridge_state, nullptr, nullptr);
  if (!bridge || !set_step(arena, bridge, "explorerUrl", bridge_explorer) ||
      !set_step(arena, bridge, "startedAt", text_value(arena, sending_at)) ||
      !set_step(arena, bridge, "completedAt", is(bridge_state, "completed") ? text_value(arena, receiving_at) : nullptr) ||
      !SetNumOrNull(arena, bridge, "durationSeconds", has_bridge_duration, bridge_duration) ||
      !set_step(arena, bridge, "timingSource", sending_at || receiving_at ? str(arena, "inferred") : nullptr) ||
      !set_step(arena, bridge, "detail",
                is(bridge_state, "pending") ? str(arena, "The bridge is relaying funds to the destination network.")
                                            : nullptr) ||
      !set_step(arena, bridge, "errorCode", bridge_problem ? substatus : nullptr) ||
      !set_step(arena, bridge, "errorMessage", bridge_problem ? message : nullptr) ||
      !tj::Push(arena, progress, bridge))
    return nullptr;
  // Destination.
  const char* shown_destination = is(state, "refunded") || refund_in_progress ? "not-started" : destination_state;
  const bool destination_done = is(destination_state, "completed") || is(destination_state, "failed");
  Value* destination = ProgressStep(
      arena, "destination", "destination",
      str(arena, is(state, "partial") ? "Destination swap" : "Destination transaction"), shown_destination,
      request.to_chain_id, receiving_hash);
  const Value* destination_detail =
      is(state, "partial")
          ? str(arena, "The bridge delivered an intermediate asset, but the requested destination swap did not "
                       "complete.")
      : Is(substatus, "WAIT_DESTINATION_TRANSACTION")
          ? str(arena, "Waiting for the destination transaction to be created and confirmed.")
          : nullptr;
  if (!destination || !set_step(arena, destination, "explorerUrl", StringValue(Field(receiving, "txLink"))) ||
      !set_step(arena, destination, "completedAt", destination_done ? text_value(arena, receiving_at) : nullptr) ||
      !set_step(arena, destination, "timingSource", receiving_at ? str(arena, "provider") : nullptr) ||
      !set_step(arena, destination, "detail", destination_detail) ||
      !set_step(arena, destination, "errorCode", is(destination_state, "failed") ? substatus : nullptr) ||
      !set_step(arena, destination, "errorMessage", is(destination_state, "failed") ? message : nullptr) ||
      !tj::Push(arena, progress, destination))
    return nullptr;
  // Refund.
  if (is(state, "refunded") || refund_in_progress || is(state, "needs-action")) {
    const Value* refund_chain = ChainValue(arena, Field(refund, "chainId"));
    const char* refund_at = SafeTimestamp(arena, Field(refund, "timestamp"));
    const char* refund_started = refund_at ? refund_at : refund_in_progress ? receiving_at : nullptr;
    const char* refund_state = is(state, "refunded") ? "refunded" : is(state, "needs-action") ? "needs-action" : "pending";
    Value* step = ProgressStep(arena, "refund", "refund",
                               str(arena, is(state, "needs-action") ? "Refund requires action" : "Refund"), refund_state,
                               refund_chain ? refund_chain : request.from_chain_id, StringValue(Field(refund, "txHash")));
    const Value* detail =
        is(state, "refunded")
            ? str(arena, "The provider reports that funds were returned. Verify the refund transaction and asset in "
                         "your wallet.")
        : is(state, "needs-action")
            ? str(arena, "The transfer cannot complete normally. Refund handling depends on the bridge; follow the "
                         "provider tracker.")
            : str(arena, "The provider is processing a refund.");
    if (!step || !set_step(arena, step, "explorerUrl", StringValue(Field(refund, "transactionLink"))) ||
        !set_step(arena, step, "startedAt", text_value(arena, refund_started)) ||
        !set_step(arena, step, "completedAt", is(state, "refunded") ? text_value(arena, refund_at) : nullptr) ||
        !set_step(arena, step, "timingSource", refund_at ? str(arena, "provider") : nullptr) ||
        !set_step(arena, step, "detail", detail) ||
        !set_step(arena, step, "errorCode", is(state, "needs-action") ? substatus : nullptr) ||
        !set_step(arena, step, "errorMessage", is(state, "needs-action") ? message : nullptr) ||
        !tj::Push(arena, progress, step))
      return nullptr;
  }
  Value* recovery = lifi_recovery(arena, state, substatus, message,
                                  provider_explorer ? provider_explorer : bridge_explorer);
  Value* received = tj::Null(arena);
  if (received_amount) {
    received = tj::Obj(arena);
    double decimals = 0;
    const bool has_decimals = IntegerNumber(Field(receiving_token, "decimals"), &decimals);
    if (!received || !SetVal(arena, received, "chainId", request.to_chain_id) ||
        !SetStr(arena, received, "amount", received_amount) ||
        !SetVal(arena, received, "tokenAddress", StringValue(Field(receiving_token, "address"))) ||
        !SetVal(arena, received, "symbol", StringValue(Field(receiving_token, "symbol"), 64)) ||
        !SetNumOrNull(arena, received, "decimals", has_decimals, decimals))
      return nullptr;
  }
  Value* out = tj::Obj(arena);
  if (!recovery || !received || !out || !SetStr(arena, out, "providerId", "lifi") ||
      !SetStr(arena, out, "state", state) || !SetVal(arena, out, "substatus", substatus) ||
      !SetVal(arena, out, "message", message) || !SetVal(arena, out, "sourceHash", request.tx_hash) ||
      !SetVal(arena, out, "destinationHash", receiving_hash) ||
      !SetVal(arena, out, "providerExplorerUrl", provider_explorer) ||
      !SetVal(arena, out, "bridgeExplorerUrl", bridge_explorer) || !SetVal(arena, out, "elapsedSeconds", nullptr) ||
      !tj::Set(arena, out, "actualReceived", received) || !tj::Set(arena, out, "progress", progress) ||
      !tj::Set(arena, out, "recovery", recovery) ||
      !tj::Set(arena, out, "providerData", BoundedProviderSnapshot(arena, payload)))
    return nullptr;
  return out;
}

const char* lifi_status_path(Arena& arena, const StatusRequest& request, const Value* bridge_id) {
  Text text;
  if (!text.Init(arena, value_bytes(request.tx_hash) + value_bytes(bridge_id) + 256)) return nullptr;
  text.out.Text("/v1/status?");
  QueryPair pairs[] = {{"txHash", request.tx_hash},
                       {"fromChain", lifi_chain(arena, request.from_chain_id)},
                       {"toChain", lifi_chain(arena, request.to_chain_id)},
                       {"bridge", bridge_id}};
  FormEncode(pairs, bridge_id ? 4 : 3, text.out);
  return text.Finish();
}

// ---- Squid -----------------------------------------------------------------------

constexpr const char* kCosmosOsmosisChannel = "channel-141";
constexpr const char* kOsmosisAtomDenom = "ibc/27394FB092D2ECCD56123C74F36E4C1F926001CEADA9CA97EA622B25F41E5EB2";
constexpr const char* kOsmosisOutputDenom = "uosmo";
constexpr const char* kOsmosisSquidRouter = "osmo1n6ney9tsf55etz9nrmzyd8wa7e64qd3s06a74fqs30ka8pps6cvqtsycr6";

// JSON.parse(text) of a JavaScript string: no BOM is skipped.
const Value* parse_inner_json(Arena& arena, const Value* text) {
  if (!text || text->type != tj::Type::kString) return nullptr;
  if (text->length >= 3 && memcmp(text->text, "\xEF\xBB\xBF", 3) == 0) return nullptr;
  return tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
}

// exactRecord(value, keys).
const Value* exact_record(const Value* value, const char* const* keys, size_t count) {
  const Value* record = AsRecord(value);
  if (!record || record->count != count) return nullptr;
  for (size_t i = 0; i < count; ++i)
    if (!tj::Get(record, keys[i])) return nullptr;
  return record;
}
template <size_t N>
const Value* exact(const Value* value, const char* const (&keys)[N]) {
  return exact_record(value, keys, N);
}

bool validate_balance_actions(const Value* value, const char* denom, const char* replacer, bool include_proto) {
  if (!value || value->type != tj::Type::kArray || value->count != (include_proto ? 2u : 1u)) return false;
  static const char* const kFetchAction[] = {"native_balance_fetch"};
  static const char* const kFetch[] = {"denom", "replacer"};
  const Value* fetch_action = exact(value->items[0], kFetchAction);
  const Value* fetch = exact(Field(fetch_action, "native_balance_fetch"), kFetch);
  if (!fetch || !Is(tj::Get(fetch, "denom"), denom) || !Is(tj::Get(fetch, "replacer"), replacer)) return false;
  if (!include_proto) return true;
  static const char* const kProtoAction[] = {"field_to_proto_binary"};
  static const char* const kProto[] = {"replacer", "proto_msg_type"};
  const Value* proto_action = exact(value->items[1], kProtoAction);
  const Value* proto = exact(Field(proto_action, "field_to_proto_binary"), kProto);
  return proto && Is(tj::Get(proto, "replacer"), "/stargate/value") &&
         Is(tj::Get(proto, "proto_msg_type"), "osmosis_swap_exact_amt_in");
}

bool validate_cosmos_swap_memo(Arena& arena, const Value* memo, const Value* receiver, const Value* destination,
                               const char* route_minimum) {
  const Value* decoded = parse_inner_json(arena, memo);
  if (!decoded) return false;
  static const char* const kRoot[] = {"wasm"};
  static const char* const kWasm[] = {"squid_request_id", "contract", "msg"};
  const Value* root = exact(decoded, kRoot);
  const Value* wasm = exact(Field(root, "wasm"), kWasm);
  const Value* request_id = StringValue(Field(wasm, "squid_request_id"), 128);
  const Value* contract = StringValue(Field(wasm, "contract"), 256);
  if (!request_id || !contract || !SameText(receiver, contract) || !Is(contract, kOsmosisSquidRouter)) return false;
  static const char* const kMessage[] = {"multicall"};
  static const char* const kMulticall[] = {"calls", "fallback_address"};
  const Value* message = exact(Field(wasm, "msg"), kMessage);
  const Value* multicall = exact(Field(message, "multicall"), kMulticall);
  if (!multicall || !SameText(tj::Get(multicall, "fallback_address"), destination)) return false;
  const Value* calls = tj::Get(multicall, "calls");
  if (!calls || calls->type != tj::Type::kArray || calls->count != 2) return false;
  static const char* const kCall[] = {"msg", "actions"};
  static const char* const kStargateMessage[] = {"stargate"};
  static const char* const kStargate[] = {"type_url", "value"};
  static const char* const kSwap[] = {"sender", "routes", "token_in", "token_out_min_amount"};
  const Value* swap_call = exact(calls->items[0], kCall);
  const Value* swap_message = exact(Field(swap_call, "msg"), kStargateMessage);
  const Value* stargate = exact(Field(swap_message, "stargate"), kStargate);
  const Value* swap = exact(Field(stargate, "value"), kSwap);
  const Value* routes = Field(swap, "routes");
  if (!swap_call || !swap || !Is(Field(stargate, "type_url"), "/osmosis.gamm.v1beta1.MsgSwapExactAmountIn") ||
      !SameText(tj::Get(swap, "sender"), contract) || !routes || routes->type != tj::Type::kArray ||
      routes->count == 0 || routes->count > 4)
    return false;
  static const char* const kRoute[] = {"pool_id", "token_out_denom"};
  const Value* last_denom = nullptr;
  for (uint32_t i = 0; i < routes->count; ++i) {
    const Value* route = exact(routes->items[i], kRoute);
    const char* pool = SafePositiveInteger(arena, Field(route, "pool_id"));
    last_denom = StringValue(Field(route, "token_out_denom"), 256);
    if (!route || !pool || !last_denom) return false;
  }
  static const char* const kCoin[] = {"denom", "amount"};
  const Value* token_in = exact(tj::Get(swap, "token_in"), kCoin);
  const char* swap_minimum = SafePositiveInteger(arena, tj::Get(swap, "token_out_min_amount"));
  if (!Is(last_denom, kOsmosisOutputDenom) || !Is(Field(token_in, "denom"), kOsmosisAtomDenom) ||
      !Is(Field(token_in, "amount"), "0") || !swap_minimum || CompareDecimal(swap_minimum, route_minimum) < 0 ||
      !validate_balance_actions(tj::Get(swap_call, "actions"), kOsmosisAtomDenom, "/stargate/value/token_in/amount",
                                true))
    return false;
  static const char* const kBankMessage[] = {"bank"};
  static const char* const kBank[] = {"send"};
  static const char* const kSend[] = {"to_address", "amount"};
  const Value* send_call = exact(calls->items[1], kCall);
  const Value* send_message = exact(Field(send_call, "msg"), kBankMessage);
  const Value* bank = exact(Field(send_message, "bank"), kBank);
  const Value* send = exact(Field(bank, "send"), kSend);
  const Value* amounts = Field(send, "amount");
  if (!send_call || !send || !SameText(tj::Get(send, "to_address"), destination) || !amounts ||
      amounts->type != tj::Type::kArray || amounts->count != 1)
    return false;
  const Value* amount = exact(amounts->items[0], kCoin);
  return amount && Is(tj::Get(amount, "denom"), kOsmosisOutputDenom) && Is(tj::Get(amount, "amount"), "0") &&
         validate_balance_actions(tj::Get(send_call, "actions"), kOsmosisOutputDenom, "/bank/send/amount/0/amount",
                                  false);
}

// /^[a-z0-9]{1,83}1[0-9ac-hj-np-z]{20,90}$/
bool cosmos_receiver(const Value* v) {
  if (!v || v->type != tj::Type::kString) return false;
  auto word = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  auto data = [](char c) {
    return (c >= '0' && c <= '9') || c == 'a' || (c >= 'c' && c <= 'h') || (c >= 'j' && c <= 'n') ||
           (c >= 'p' && c <= 'z');
  };
  const size_t n = v->length;
  for (size_t i = 1; i <= 83 && i < n; ++i) {
    if (!word(v->text[i - 1])) return false;
    if (v->text[i] != '1') continue;
    const size_t rest = n - i - 1;
    if (rest < 20 || rest > 90) continue;
    bool ok = true;
    for (size_t k = i + 1; k < n && ok; ++k) ok = data(v->text[k]);
    if (ok) return true;
  }
  return false;
}

// normalizeCosmosIbcTransfer(): the rebuilt MsgTransfer JSON, or nullptr.
const Value* normalize_cosmos(Arena& arena, const Value* data, const QuoteRequest& request,
                              const char* amount_out_min) {
  if (!data || !Is(request.from.chain_id, "cosmoshub-4") || !Is(request.to.chain_id, "osmosis-1") ||
      !Is(request.from.token, "uatom") || !Is(request.to.token, "uosmo"))
    return nullptr;
  const Value* root = AsRecord(parse_inner_json(arena, data));
  const Value* value = AsRecord(Field(root, "value"));
  const Value* token = AsRecord(Field(value, "token"));
  const Value* timeout_height = AsRecord(Field(value, "timeoutHeight"));
  const char* token_amount = SafePositiveInteger(arena, Field(token, "amount"));
  if (!root || !value || !token || !Is(tj::Get(root, "typeUrl"), "/ibc.applications.transfer.v1.MsgTransfer") ||
      !Is(tj::Get(value, "sourcePort"), "transfer") || !Is(tj::Get(value, "sourceChannel"), kCosmosOsmosisChannel) ||
      !SameText(tj::Get(token, "denom"), request.from.token) || !token_amount ||
      !Is(request.amount_in, token_amount) || !SameText(tj::Get(value, "sender"), request.from.address))
    return nullptr;
  const Value* receiver = StringValue(tj::Get(value, "receiver"), 256);
  if (!receiver || !cosmos_receiver(receiver)) return nullptr;
  const Value* raw_memo = tj::Get(value, "memo");
  const Value* memo = Nullish(raw_memo) ? S(arena, "")
                      : raw_memo->type == tj::Type::kString && Units(raw_memo) <= 16384 ? raw_memo
                                                                                        : nullptr;
  if (!memo) return nullptr;
  if (!validate_cosmos_swap_memo(arena, memo, receiver, request.to.address, amount_out_min)) return nullptr;
  const char* revision_number = SafeNonNegativeInteger(arena, Field(timeout_height, "revisionNumber"));
  const char* revision_height = SafeNonNegativeInteger(arena, Field(timeout_height, "revisionHeight"));
  const char* timeout_timestamp = SafeNonNegativeInteger(arena, tj::Get(value, "timeoutTimestamp"));
  if (!revision_number) revision_number = "0";
  if (!revision_height) revision_height = "0";
  if (!timeout_timestamp) timeout_timestamp = "0";
  if (!strcmp(revision_height, "0") && !strcmp(timeout_timestamp, "0")) return nullptr;
  Value* message = tj::Obj(arena);
  Value* body = tj::Obj(arena);
  Value* coin = tj::Obj(arena);
  Value* height = tj::Obj(arena);
  if (!message || !body || !coin || !height || !SetStr(arena, message, "typeUrl", "/ibc.applications.transfer.v1.MsgTransfer") ||
      !SetStr(arena, body, "sourcePort", "transfer") || !SetVal(arena, body, "sourceChannel", tj::Get(value, "sourceChannel")) ||
      !SetVal(arena, coin, "denom", request.from.token) || !SetVal(arena, coin, "amount", request.amount_in) ||
      !tj::Set(arena, body, "token", coin) || !SetVal(arena, body, "sender", request.from.address) ||
      !SetVal(arena, body, "receiver", receiver) || !SetStr(arena, height, "revisionNumber", revision_number) ||
      !SetStr(arena, height, "revisionHeight", revision_height) || !tj::Set(arena, body, "timeoutHeight", height) ||
      !SetStr(arena, body, "timeoutTimestamp", timeout_timestamp) || !SetVal(arena, body, "memo", memo) ||
      !tj::Set(arena, message, "value", body))
    return nullptr;
  const size_t capacity = 64 * 1024 + memo->length * 7;
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
  if (!buffer) return nullptr;
  OutBuffer out{buffer, capacity, 0, true};
  if (!tj::Stringify(message, out) || !out.ok) return nullptr;
  return tj::Str(arena, reinterpret_cast<const char*>(buffer), out.used);
}

Value* parse_squid_route(Arena& arena, const Value* payload, const QuoteRequest& request, double now) {
  const Value* root = AsRecord(payload);
  const Value* route_root = AsRecord(Field(root, "route"));
  if (!route_root) route_root = root;
  const Value* estimate = AsRecord(Field(route_root, "estimate"));
  if (!root || !route_root || !estimate) return nullptr;
  const char* amount_out = SafePositiveInteger(arena, tj::Get(estimate, "toAmount"));
  const char* amount_out_min = SafePositiveInteger(arena, tj::Get(estimate, "toAmountMin"));
  if (!amount_out || !amount_out_min || CompareDecimal(amount_out_min, amount_out) > 0) return nullptr;
  const Value* to_token = AsRecord(tj::Get(estimate, "toToken"));
  const Value* from_token = AsRecord(tj::Get(estimate, "fromToken"));
  if (!SameIdentifier(arena, Coalesce(tj::Get(estimate, "fromChainId"), tj::Get(route_root, "fromChain")),
                      request.from.chain_id) ||
      !SameIdentifier(arena, Coalesce(tj::Get(estimate, "toChainId"), tj::Get(route_root, "toChain")),
                      request.to.chain_id) ||
      !SameIdentifier(arena, Field(from_token, "address"),
                      squid_token(arena, request.from.token, request.from.chain_id)) ||
      !SameIdentifier(arena, Field(to_token, "address"), squid_token(arena, request.to.token, request.to.chain_id)) ||
      !SameIdentifier(arena, tj::Get(estimate, "fromAmount"), request.amount_in))
    return nullptr;
  double reported_decimals = 0;
  if (IntegerNumber(Field(to_token, "decimals"), &reported_decimals) &&
      reported_decimals != static_cast<double>(request.to.decimals))
    return nullptr;
  Value* steps = ParseSquidSteps(arena, tj::Get(estimate, "actions"));
  if (!steps) return nullptr;
  const Value* bridge = nullptr;
  for (uint32_t i = 0; i < steps->count && !bridge; ++i)
    if (Is(tj::Get(steps->items[i], "kind"), "bridge")) bridge = tj::Get(steps->items[i], "tool");
  if (!bridge) bridge = S(arena, "Squid");
  const Value* tx = AsRecord(tj::Get(route_root, "transactionRequest"));
  const Value* target = StringValue(Field(tx, "target"));
  if (!target) target = StringValue(Field(tx, "to"));
  const Value* raw_data = StringValue(Field(tx, "data"), 200000);
  const Value* cosmos = normalize_cosmos(arena, raw_data, request, amount_out_min);
  if (Is(request.from.chain_id, "cosmoshub-4") && !cosmos) return nullptr;
  const Value* data = cosmos ? cosmos : raw_data;
  ExecutionFields execution;
  execution.kind = cosmos ? "cosmos-ibc-transfer" : ExecutionKind(data, target);
  execution.to = target;
  execution.data = data;
  execution.value = execution_value(arena, Field(tx, "value"));
  execution.gas_limit = text_value(arena, SafePositiveInteger(arena, Coalesce(Field(tx, "gasLimit"), Field(tx, "gas"))));
  if (!cosmos) {
    execution.approval_address = StringValue(tj::Get(estimate, "approvalAddress"));
    if (!execution.approval_address) execution.approval_address = target;
  }
  const Value* quote_id = StringValue(tj::Get(root, "quoteId"));
  if (!quote_id) quote_id = StringValue(tj::Get(route_root, "quoteId"));
  Validity validity;
  if (!QuoteValidity(arena, Coalesce(tj::Get(root, "expiresAt"), tj::Get(route_root, "expiresAt")), now, 30000,
                     &validity))
    return nullptr;
  RouteFields f;
  f.provider_id = kSquidProvider.Id();
  f.provider_name = kSquidProvider.Name();
  f.provider_url = kSquidProvider.Website();
  f.provider_quote_id = quote_id;
  f.bridge_id = bridge;
  f.bridge = bridge;
  f.bridge_url = BridgeWebsiteUrl(arena, bridge);
  f.risk_key = NormalizedRiskKey(arena, bridge);
  f.custody = kSquidProvider.Custody();
  f.amount_in = request.amount_in;
  f.amount_out = amount_out;
  f.amount_out_min = amount_out_min;
  f.decimals_in = request.from.decimals;
  f.decimals_out = request.to.decimals;
  f.has_fee_usd = SumUsdCosts(estimate, &f.fee_usd);
  f.source_network_fee = SumRawNetworkCosts(arena, estimate);
  const Value* eta[] = {tj::Get(estimate, "estimatedRouteDuration"),
                        tj::Get(estimate, "estimatedRouteDurationInSeconds")};
  f.has_eta = SafeEta(eta, 2, &f.eta_seconds);
  f.quoted_at = validity.quoted_at;
  f.expires_at = validity.expires_at;
  f.steps = steps;
  f.execution = Execution(arena, execution);
  if (!f.execution) return nullptr;
  return Route(arena, f);
}

// ---- Squid status and AxelarScan -----------------------------------------------------

bool axelar_mentioned(Arena& arena, const Value* v) {
  const Value* text = StringValue(v);
  const Value* lower = text ? Lower(arena, text) : nullptr;
  return lower && Contains(lower, "axelar");
}

bool should_fetch_axelar(Arena& arena, const Value* payload, const StatusRequest& request) {
  const Value* root = AsRecord(payload);
  const Value* gmp = Field(root, "isGMPTransaction");
  if ((gmp && gmp->type == tj::Type::kTrue) || StringValue(Field(root, "axelarTransactionUrl"))) return true;
  if (request.bridge_id) {
    const Value* lower = Lower(arena, request.bridge_id);
    if (lower && Contains(lower, "axelar")) return true;
  }
  const Value* rows = Field(root, "routeStatus");
  if (!rows || rows->type != tj::Type::kArray) return false;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const Value* row = AsRecord(rows->items[i]);
    static const char* kKeys[] = {"bridge", "provider", "tool", "protocol"};
    for (const char* key : kKeys)
      if (axelar_mentioned(arena, Field(row, key))) return true;
  }
  return false;
}

const Value* axelar_hash(const Value* event) {
  const Value* hash = StringValue(Field(event, "transactionHash"));
  if (!hash) hash = StringValue(Field(Field(event, "transaction"), "hash"));
  if (!hash) hash = StringValue(Field(Field(event, "receipt"), "transactionHash"));
  return hash;
}

const char* axelar_timestamp(Arena& arena, const Value* event) {
  return SafeTimestamp(arena, Coalesce(Coalesce(Field(event, "block_timestamp"), Field(event, "blockTimestamp")),
                                       Field(Field(event, "created_at"), "ms")));
}

const Value* axelar_chain_id(Arena& arena, const Value* event) {
  const Value* value = Field(Field(event, "transaction"), "chainId");
  if (!value || (value->type != tj::Type::kString && value->type != tj::Type::kNumber)) return nullptr;
  return JsString(arena, value);
}

bool axelar_event_failed(Arena& arena, const Value* event) {
  const Value* status = Field(Field(event, "receipt"), "status");
  if (status && ((status->type == tj::Type::kNumber && status->number == 0) || status->type == tj::Type::kFalse ||
                 Is(status, "0x0")))
    return true;
  const Value* text = StringValue(Field(event, "status"), 64);
  const Value* lower = text ? Lower(arena, text) : nullptr;
  return lower && Is(lower, "failed");
}

const Value* axelar_error_message(const Value* event, const Value* record) {
  const Value* receipt = AsRecord(Field(event, "receipt"));
  const Value* error = AsRecord(tj::Get(record, "error"));
  const Value* candidates[] = {StringValue(Field(event, "error")), StringValue(Field(event, "revertReason")),
                               StringValue(Field(receipt, "revertReason")), StringValue(Field(receipt, "error")),
                               StringValue(Coalesce(Field(error, "message"), Field(error, "reason")))};
  for (const Value* candidate : candidates)
    if (candidate) return candidate;
  return nullptr;
}

const Value* axelar_detail(Arena& arena, const Value* event, const char* fallback) {
  if (!event) return S(arena, fallback);
  const Value* values = AsRecord(tj::Get(event, "returnValues"));
  const size_t capacity = 64 * 1024;
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
  if (!buffer) return nullptr;
  OutBuffer out{buffer, capacity, 0, true};
  out.Text(fallback);
  size_t bits = 0;
  auto bit = [&](const char* prefix, const Value* text, const char* suffix) {
    out.Text(bits ? " \xC2\xB7 " : " ");
    ++bits;
    out.Text(prefix);
    out.Append(text->text, text->length);
    out.Text(suffix);
  };
  if (const Value* name = StringValue(tj::Get(event, "event"), 128)) bit("", name, "");
  const Value* block = tj::Get(event, "blockNumber");
  if (!Nullish(block)) { const Value* s = JsString(arena, block); if (!s) return nullptr; bit("block ", s, ""); }
  if (const Value* chain = StringValue(Field(values, "destinationChain"), 128)) bit("destination ", chain, "");
  if (const Value* symbol = StringValue(Field(values, "symbol"), 64)) bit("asset ", symbol, "");
  const Value* raw_amount = Field(values, "amount");
  const Value* amount = tj::Get(event, "amount");
  if (!Nullish(raw_amount)) {
    const Value* s = JsString(arena, raw_amount);
    if (!s) return nullptr;
    bit("raw amount ", s, "");
  } else if (!Nullish(amount)) {
    const Value* s = JsString(arena, amount);
    if (!s) return nullptr;
    bit("amount ", s, "");
  }
  const Value* gas_fee = Field(values, "gasFeeAmount");
  if (!Nullish(gas_fee)) { const Value* s = JsString(arena, gas_fee); if (!s) return nullptr; bit("gas fee ", s, " base units"); }
  const Value* poll = tj::Get(event, "poll_id");
  if (!Nullish(poll)) { const Value* s = JsString(arena, poll); if (!s) return nullptr; bit("poll ", s, ""); }
  if (bits) out.Byte('.');
  if (!out.ok) return nullptr;
  return tj::Str(arena, reinterpret_cast<const char*>(buffer), out.used);
}

// axelarExplorerUrl(); `*thrown` when encodeURIComponent throws.
const Value* axelar_explorer(Arena& arena, const char* type, const Value* hash, const Value* chain_id,
                             const Value* source_url, const Value* destination_url, bool* thrown) {
  if (!hash) return nullptr;
  const size_t encoded_capacity = hash->length * 9 + 8;
  uint8_t* encoded = static_cast<uint8_t*>(arena.Alloc(encoded_capacity, 1));
  if (!encoded) { *thrown = true; return nullptr; }
  OutBuffer enc{encoded, encoded_capacity, 0, true};
  if (!EncodeComponent(hash, enc)) { *thrown = true; return nullptr; }
  if (!strcmp(type, "axelar")) {
    Text text;
    if (!text.Init(arena, enc.used + 64)) { *thrown = true; return nullptr; }
    text.out.Text("https://axelarscan.io/tx/");
    text.out.Append(encoded, enc.used);
    const char* url = text.Finish();
    return url ? S(arena, url) : nullptr;
  }
  const Value* reported = !strcmp(type, "source") ? source_url : destination_url;
  if (reported && Contains(reported, "/tx/")) {
    // reported.replace(/\/tx\/[^/?#]+/, `/tx/${encoded}`): the first "/tx/"
    // followed by at least one character outside "/?#".
    for (size_t i = 0; i + 4 < reported->length + 1; ++i) {
      if (i + 4 > reported->length || memcmp(reported->text + i, "/tx/", 4) != 0) continue;
      size_t end = i + 4;
      while (end < reported->length && reported->text[end] != '/' && reported->text[end] != '?' &&
             reported->text[end] != '#')
        ++end;
      if (end == i + 4) continue;
      Text text;
      if (!text.Init(arena, reported->length + enc.used + 16)) { *thrown = true; return nullptr; }
      text.out.Append(reported->text, i);
      text.out.Text("/tx/");
      text.out.Append(encoded, enc.used);
      text.out.Append(reported->text + end, reported->length - end);
      if (!text.out.ok) { *thrown = true; return nullptr; }
      return tj::Str(arena, reinterpret_cast<const char*>(text.data), text.out.used);
    }
    return reported;
  }
  static const char* kBases[][2] = {{"1", "https://etherscan.io"},       {"10", "https://optimistic.etherscan.io"},
                                    {"56", "https://bscscan.com"},       {"137", "https://polygonscan.com"},
                                    {"8453", "https://basescan.org"},    {"42161", "https://arbiscan.io"},
                                    {"43114", "https://snowtrace.io"}};
  for (const auto& base : kBases) {
    if (!Is(chain_id, base[0])) continue;
    Text text;
    if (!text.Init(arena, enc.used + 64)) { *thrown = true; return nullptr; }
    text.out.Text(base[1]);
    text.out.Text("/tx/");
    text.out.Append(encoded, enc.used);
    const char* url = text.Finish();
    return url ? S(arena, url) : nullptr;
  }
  return nullptr;
}

const char* axelar_error_code(Arena& arena, const Value* record) {
  static const char* kFlags[] = {"is_insufficient_fee",
                                 "is_invalid_amount",
                                 "is_invalid_call",
                                 "is_invalid_contract_address",
                                 "is_invalid_destination_chain",
                                 "is_invalid_gas_paid",
                                 "is_invalid_payload_hash",
                                 "is_invalid_source_address",
                                 "is_invalid_symbol",
                                 "is_not_enough_gas",
                                 "not_enough_gas_to_execute"};
  for (const char* flag : kFlags) {
    const Value* v = tj::Get(record, flag);
    if (v && v->type == tj::Type::kTrue) {
      char upper[64];
      size_t i = 0;
      for (; flag[i] && i + 1 < sizeof(upper); ++i)
        upper[i] = flag[i] >= 'a' && flag[i] <= 'z' ? static_cast<char>(flag[i] - 'a' + 'A') : flag[i];
      upper[i] = '\0';
      return arena.CopyText(upper, i);
    }
  }
  return nullptr;
}

const Value* member_or_undefined(const Value* record, const char* key) {
  const Value* v = tj::Get(record, key);
  return v ? v : Undefined();
}

Value* axelar_support_data(Arena& arena, const Value* envelope, const Value* record) {
  static const char* kEventKeys[] = {"call", "gas_paid", "express_executed", "confirm", "approved", "executed",
                                     "refunded"};
  Value* events = tj::Obj(arena);
  if (!events) return nullptr;
  for (const char* key : kEventKeys) {
    const Value* event = AsRecord(tj::Get(record, key));
    if (!event) {
      if (!tj::Set(arena, events, key, tj::Null(arena))) return nullptr;
      continue;
    }
    const Value* receipt = AsRecord(tj::Get(event, "receipt"));
    const Value* raw_logs = Field(receipt, "logs");
    const bool logs_array = raw_logs && raw_logs->type == tj::Type::kArray;
    Value* copy = tj::Obj(arena);
    if (!copy) return nullptr;
    for (uint32_t i = 0; i < event->count; ++i) {
      const tj::Member& member = event->members[i];
      if (member.key_length == 7 && memcmp(member.key, "receipt", 7) == 0) continue;
      if (!tj::Set(arena, copy, member.key, member.key_length, member.value)) return nullptr;
    }
    Value* summary = tj::Null(arena);
    if (receipt) {
      summary = tj::Obj(arena);
      static const char* kReceiptKeys[] = {"transactionHash", "status", "blockNumber", "confirmations",
                                           "transactionIndex", "gasUsed", "cumulativeGasUsed",
                                           "effectiveGasPrice", "from", "error", "revertReason"};
      if (!summary) return nullptr;
      for (const char* name : kReceiptKeys)
        if (!tj::Set(arena, summary, name, const_cast<Value*>(member_or_undefined(receipt, name)))) return nullptr;
      Value* logs = tj::Arr(arena);
      if (!logs) return nullptr;
      if (logs_array)
        for (uint32_t i = 0; i < raw_logs->count && i < 128; ++i) {
          const Value* log = AsRecord(raw_logs->items[i]);
          Value* entry = tj::Obj(arena);
          static const char* kLogKeys[] = {"address", "topics", "data", "blockNumber", "transactionHash",
                                           "transactionIndex", "logIndex"};
          if (!entry) return nullptr;
          for (const char* name : kLogKeys)
            if (!tj::Set(arena, entry, name, const_cast<Value*>(log ? member_or_undefined(log, name) : Undefined())))
              return nullptr;
          if (!tj::Push(arena, logs, entry)) return nullptr;
        }
      if (!tj::Set(arena, summary, "logs", logs) ||
          !SetBool(arena, summary, "logsTruncated", logs_array && raw_logs->count > 128))
        return nullptr;
    }
    if (!tj::Set(arena, copy, "receipt", summary)) return nullptr;
    Value* bounded = BoundedProviderSnapshot(arena, copy);
    if (!bounded || !tj::Set(arena, events, key, bounded)) return nullptr;
  }
  static const char* kMetadataKeys[] = {"id",
                                        "message_id",
                                        "command_id",
                                        "status",
                                        "simplified_status",
                                        "time_spent",
                                        "amount",
                                        "value",
                                        "symbol",
                                        "fees",
                                        "gas",
                                        "to_refund",
                                        "no_gas_remain",
                                        "executing_at",
                                        "express_executing_at",
                                        "refunding_at",
                                        "is_insufficient_fee",
                                        "is_invalid_amount",
                                        "is_invalid_call",
                                        "is_invalid_contract_address",
                                        "is_invalid_destination_chain",
                                        "is_invalid_gas_paid",
                                        "is_invalid_gas_paid_mismatch_source_address",
                                        "is_invalid_payload_hash",
                                        "is_invalid_source_address",
                                        "is_invalid_symbol",
                                        "is_not_enough_gas",
                                        "not_enough_gas_to_execute"};
  Value* metadata = tj::Obj(arena);
  Value* support = tj::Obj(arena);
  if (!metadata || !support) return nullptr;
  for (const char* key : kMetadataKeys)
    if (!SetVal(arena, metadata, key, Coalesce(tj::Get(record, key), nullptr))) return nullptr;
  if (!SetStr(arena, support, "api", "axelarscan-gmp") ||
      !SetVal(arena, support, "total", Coalesce(tj::Get(envelope, "total"), nullptr)) ||
      !SetVal(arena, support, "time_spent", Coalesce(tj::Get(envelope, "time_spent"), nullptr)) ||
      !tj::Set(arena, support, "metadata", metadata) || !tj::Set(arena, support, "events", events))
    return nullptr;
  return BoundedProviderSnapshot(arena, support);
}

struct AxelarProgress {
  Value* steps = nullptr;  // array
  const Value* destination_hash = nullptr;
  Value* support = nullptr;  // nullptr: absent
};

bool parse_axelar(Arena& arena, const Value* payload, const StatusRequest& request, const Value* from_chain,
                  const Value* to_chain, const char* state, AxelarProgress* out, bool* thrown) {
  out->steps = tj::Arr(arena);
  if (!out->steps) return false;
  const Value* envelope = AsRecord(payload);
  const Value* data = Field(envelope, "data");
  const Value* record = data && data->type == tj::Type::kArray && data->count ? AsRecord(data->items[0]) : nullptr;
  if (!record) record = AsRecord(data);
  if (!record) return true;
  const Value* call = AsRecord(tj::Get(record, "call"));
  const Value* gas_paid = AsRecord(tj::Get(record, "gas_paid"));
  const Value* express = AsRecord(tj::Get(record, "express_executed"));
  const Value* confirm = AsRecord(tj::Get(record, "confirm"));
  const Value* approved = AsRecord(tj::Get(record, "approved"));
  const Value* executed = AsRecord(tj::Get(record, "executed"));
  const Value* refunded = AsRecord(tj::Get(record, "refunded"));
  const Value* gas_event_text = StringValue(Field(gas_paid, "event"), 128);
  const Value* gas_event = gas_event_text ? Lower(arena, gas_event_text) : nullptr;
  const bool include_express = express || (gas_event && Contains(gas_event, "express"));
  const Value* to_refund = tj::Get(record, "to_refund");
  const bool include_refund = refunded || (to_refund && to_refund->type == tj::Type::kTrue) ||
                              !Nullish(tj::Get(record, "refunding_at"));
  struct AxelarStage {
    const char* id;
    const char* label;
    const char* kind;
    const Value* event;
    const Value* chain_id;
    const Value* detail;
    const char* explorer;
    bool has_hash;
    const Value* hash;
  };
  AxelarStage stages[7];
  size_t count = 0;
  auto add = [&](const char* id, const char* label, const char* kind, const Value* event, const Value* chain_id,
                 const char* fallback, const char* explorer, bool has_hash = false, const Value* hash = nullptr) {
    stages[count++] = AxelarStage{id, label, kind, event, chain_id, axelar_detail(arena, event, fallback), explorer,
                                  has_hash, hash};
  };
  const Value* axelar_chain = S(arena, "axelar");
  add("sent", "Sent", "source", call, request.from_chain_id, "Source contract call emitted.", "source");
  add("gas-paid", "Gas paid", "source", gas_paid, request.from_chain_id,
      "Cross-chain execution gas paid on the source network.", "source");
  if (include_express)
    add("express-executed", "Express executed", "destination", express, request.to_chain_id,
        "Axelar express relayer executed the destination call early.", "destination");
  add("confirmed", "Confirmed", "bridge", confirm, axelar_chain, "Source event confirmed by the Axelar network.",
      "axelar", true, StringValue(Field(confirm, "confirmation_txhash")));
  add("approved", "Approved", "destination", approved, request.to_chain_id,
      "Axelar gateway approved the destination command.", "destination");
  add("executed", "Executed", "destination", executed, request.to_chain_id,
      "Destination contract execution completed.", "destination");
  if (include_refund)
    add("excess-gas-refunded", "Excess gas refunded", "source", refunded, request.from_chain_id,
        "Unused execution gas returned on the source network. This is not a swap refund.", "source");
  for (size_t i = 0; i < count; ++i)
    if (!stages[i].detail) return false;
  int first_missing = -1;
  for (size_t i = 0; i < count; ++i)
    if (!stages[i].event) { first_missing = static_cast<int>(i); break; }
  const char* previous = nullptr;
  const Value* source_url = StringValue(Field(from_chain, "transactionUrl"));
  const Value* destination_url = StringValue(Field(to_chain, "transactionUrl"));
  const char* error_code = axelar_error_code(arena, record);
  for (size_t i = 0; i < count; ++i) {
    const AxelarStage& stage = stages[i];
    const char* completed_at = axelar_timestamp(arena, stage.event);
    const Value* tx_hash = stage.has_hash && stage.hash ? stage.hash : axelar_hash(stage.event);
    const bool event_failed = axelar_event_failed(arena, stage.event);
    const bool awaiting = !stage.event && express && !strcmp(state, "completed") &&
                          (!strcmp(stage.id, "approved") || !strcmp(stage.id, "executed"));
    const bool first = static_cast<int>(i) == first_missing;
    const char* step_state = stage.event                         ? (event_failed ? "failed" : "completed")
                             : awaiting                          ? "finalizing"
                             : first && !strcmp(state, "failed") ? "failed"
                             : first && strcmp(state, "completed") != 0 && strcmp(state, "refunded") != 0
                                 ? "pending"
                                 : "not-started";
    if (first && !strcmp(state, "needs-action")) step_state = "needs-action";
    const char* started_at = nullptr;
    double previous_ms = 0, completed_ms = 0;
    if (previous && completed_at && DateParse(completed_at, strlen(completed_at), &completed_ms) &&
        DateParse(previous, strlen(previous), &previous_ms) && completed_ms >= previous_ms)
      started_at = previous;
    double duration = 0;
    const bool has_duration = started_at && completed_at;
    if (has_duration) {
      duration = floor((completed_ms - previous_ms) / 1000);
      if (duration == 0) duration = 0;
    }
    if (completed_at) previous = completed_at;
    const bool problem = !strcmp(step_state, "failed") || !strcmp(step_state, "needs-action");
    const Value* explorer = axelar_explorer(arena, stage.explorer, tx_hash, axelar_chain_id(arena, stage.event),
                                            source_url, destination_url, thrown);
    if (*thrown) return false;
    char id[64];
    snprintf(id, sizeof(id), "axelar-%s", stage.id);
    Value* step = ProgressStep(arena, id, stage.kind, S(arena, stage.label), step_state, stage.chain_id, tx_hash);
    const char* code = problem ? (error_code ? error_code : event_failed ? "TRANSACTION_REVERTED" : nullptr) : nullptr;
    if (!step || !SetVal(arena, step, "explorerUrl", explorer) || !SetStr(arena, step, "startedAt", started_at) ||
        !SetStr(arena, step, "completedAt", completed_at) ||
        !SetNumOrNull(arena, step, "durationSeconds", has_duration, duration) ||
        !SetStr(arena, step, "timingSource", completed_at ? "provider" : nullptr) ||
        !SetVal(arena, step, "detail",
                awaiting ? S(arena, "Funds were delivered by Express execution. Axelar canonical approval and "
                                    "execution are continuing in the background.")
                         : stage.detail) ||
        !SetStr(arena, step, "errorCode", code) ||
        !SetVal(arena, step, "errorMessage", problem ? axelar_error_message(stage.event, record) : nullptr) ||
        !tj::Push(arena, out->steps, step))
      return false;
  }
  out->destination_hash = axelar_hash(executed);
  if (!out->destination_hash) out->destination_hash = axelar_hash(express);
  Value* empty = nullptr;
  if (!envelope) {
    empty = tj::Obj(arena);
    if (!empty) return false;
  }
  out->support = axelar_support_data(arena, envelope ? envelope : empty, record);
  return out->support != nullptr;
}

const Value* squid_step_detail(Arena& arena, const Value* action, const char* kind, const Value* from_chain,
                               const Value* to_chain) {
  if (!strcmp(kind, "source")) {
    const Value* status = StringValue(Field(from_chain, "callEventStatus"));
    return status ? status : S(arena, "Submitted on the source network.");
  }
  if (!strcmp(kind, "destination")) {
    const Value* status = StringValue(Field(to_chain, "callEventStatus"));
    return status ? status : S(arena, "Executing the destination delivery or contract call.");
  }
  if (!strcmp(kind, "refund")) return S(arena, "Returning funds to the encoded sender address.");
  return Is(action, "gas_paid") ? S(arena, "Cross-chain execution gas was paid.") : nullptr;
}

Value* squid_recovery(Arena& arena, const char* state, const Value* url, const Value* error_message) {
  const Value* label = url ? S(arena, "Open Axelar tracker") : nullptr;
  auto make = [&](const char* kind, const char* title, const Value* description) {
    return Recovery(arena, kind, S(arena, title), description, label, url);
  };
  if (!strcmp(state, "completed")) return tj::Null(arena);
  if (!strcmp(state, "needs-action"))
    return make("add-gas", "Additional destination gas is required",
                S(arena, "The cross-chain message is paused. Open the Axelar tracker to inspect the gas status and add "
                         "gas using its supported recovery flow."));
  if (!strcmp(state, "partial"))
    return make("swap-received-asset", "Destination action reverted",
                S(arena, "The bridge completed but the destination call failed. Check the destination wallet for the "
                         "intermediate asset before requesting a fresh on-chain swap."));
  if (!strcmp(state, "refunded"))
    return make("track-refund", "Automatic refund reported",
                S(arena, "Squid reports a Coral refund to the encoded sender address. Verify the refund transaction and "
                         "source-chain balance before retrying."));
  if (!strcmp(state, "not-found") || !strcmp(state, "pending"))
    return make("wait", !strcmp(state, "not-found") ? "Waiting for provider indexing" : "Transfer is still progressing",
                S(arena, "No recovery action is required yet. If it remains unresolved for 15 minutes or twice the "
                         "quoted duration, contact support with the source hash and quote ID."));
  return make("contact-support", "Provider assistance may be required",
              error_message ? error_message
                            : S(arena, "The route failed without a completed refund. Share the source hash, quote ID, "
                                       "networks and failed step with support before retrying."));
}

// action.replaceAll('_', ' ').
const Value* spaced(Arena& arena, const Value* action) {
  char* copy = static_cast<char*>(arena.Alloc(action->length + 1, 1));
  if (!copy) return nullptr;
  for (size_t i = 0; i < action->length; ++i) copy[i] = action->text[i] == '_' ? ' ' : action->text[i];
  copy[action->length] = '\0';
  return tj::Str(arena, copy, action->length);
}

Value* replace_step(Arena& arena, const Value* existing, const char* state, const char* detail, const char* code,
                    const Value* code_value, const Value* message) {
  Value* copy = tj::Clone(arena, existing);
  if (!copy || !SetStr(arena, copy, "state", state) || !SetStr(arena, copy, "detail", detail) ||
      !SetVal(arena, copy, "errorCode", code_value ? code_value : S(arena, code)) ||
      !SetVal(arena, copy, "errorMessage", message))
    return nullptr;
  return copy;
}

Value* parse_squid_status(Arena& arena, const Value* payload, const StatusRequest& request,
                          const Value* axelar_payload, bool* thrown) {
  const Value* root = AsRecord(payload);
  const Value* raw_status = StringValue(Coalesce(Field(root, "squidTransactionStatus"), Field(root, "status")));
  const Value* status = raw_status ? Lower(arena, raw_status) : S(arena, "not_found");
  if (!status) return nullptr;
  const char* state = Is(status, "success")            ? "completed"
                      : Is(status, "ongoing")          ? "pending"
                      : Is(status, "partial_success")  ? "partial"
                      : Is(status, "refund") || Is(status, "refund_status") ? "refunded"
                      : Is(status, "needs_gas")        ? "needs-action"
                      : Is(status, "not_found")        ? "not-found"
                                                       : "failed";
  auto is = [&](const char* value) { return strcmp(state, value) == 0; };
  const Value* to_chain = AsRecord(Field(root, "toChain"));
  const Value* from_chain = AsRecord(Field(root, "fromChain"));
  const Value* error = AsRecord(Field(root, "error"));
  const Value* error_code = StringValue(Coalesce(Field(error, "code"), Field(root, "gasStatus")), 128);
  const Value* error_message = StringValue(Coalesce(Field(error, "message"), Field(error, "reason")));
  const Value* destination_hash = StringValue(Field(to_chain, "transactionId"));
  const Value* rows = Field(root, "routeStatus");
  const bool rows_array = rows && rows->type == tj::Type::kArray;
  if (!destination_hash && rows_array) {
    for (uint32_t k = rows->count; k-- > 0;) {
      const Value* row = AsRecord(rows->items[k]);
      const Value* action_text = StringValue(Field(row, "action"));
      const Value* action = action_text ? Lower(arena, action_text) : nullptr;
      if (!action || !Is(action, "send")) {
        const Value* hash = StringValue(Field(row, "txHash"));
        if (hash) destination_hash = hash;
      }
    }
  }
  const uint32_t row_count = rows_array ? (rows->count < 32 ? rows->count : 32) : 0;
  Value* squid = tj::Arr(arena);
  if (!squid) return nullptr;
  for (uint32_t index = 0; index < row_count; ++index) {
    const Value* row = AsRecord(rows->items[index]);
    if (!row) continue;
    const Value* action_text = StringValue(tj::Get(row, "action"), 64);
    const Value* action = action_text ? Lower(arena, action_text) : S(arena, "step");
    const Value* raw_state_text = StringValue(tj::Get(row, "status"), 64);
    const Value* raw_state = raw_state_text ? Lower(arena, raw_state_text) : nullptr;
    if (!action) return nullptr;
    const char* step_state = Is(raw_state, "success")    ? "completed"
                             : Is(raw_state, "refund")    ? "refunded"
                             : Is(raw_state, "needs_gas") ? "needs-action"
                             : Is(raw_state, "failed") || Is(raw_state, "error") ? "failed"
                                                                                 : "pending";
    const char* kind = Contains(action, "refund")    ? "refund"
                       : Is(action, "send")          ? "source"
                       : index == row_count - 1      ? "destination"
                                                     : "bridge";
    const char* started_at = SafeTimestamp(arena, Coalesce(tj::Get(row, "startedAt"), tj::Get(row, "startTime")));
    const bool settled = !strcmp(step_state, "completed") || !strcmp(step_state, "failed") ||
                         !strcmp(step_state, "refunded");
    const char* completed_at =
        settled ? SafeTimestamp(arena, Coalesce(Coalesce(tj::Get(row, "completedAt"), tj::Get(row, "timestamp")),
                                                tj::Get(row, "endTime")))
                : nullptr;
    const Value* durations[] = {tj::Get(row, "durationSeconds"), tj::Get(row, "duration"), tj::Get(row, "timeSpent")};
    double duration = 0;
    const bool has_duration = SafeEta(durations, 3, &duration);
    const Value* label = !strcmp(kind, "source")        ? S(arena, "Source transaction")
                         : !strcmp(kind, "refund")      ? S(arena, "Refund")
                         : !strcmp(kind, "destination") ? S(arena, "Destination transaction")
                                                        : spaced(arena, action);
    const Value* explorer = !strcmp(kind, "source")        ? StringValue(Field(from_chain, "transactionUrl"))
                            : !strcmp(kind, "destination") ? StringValue(Field(to_chain, "transactionUrl"))
                                                           : nullptr;
    const bool problem = !strcmp(step_state, "failed") || !strcmp(step_state, "needs-action");
    char id[48];
    snprintf(id, sizeof(id), "%s-%u", kind, index);
    Value* step = ProgressStep(arena, id, kind, label, step_state, StringValue(tj::Get(row, "chainId"), 64),
                               StringValue(tj::Get(row, "txHash")));
    if (!step || !label || !SetVal(arena, step, "explorerUrl", explorer) ||
        !SetStr(arena, step, "startedAt", started_at) || !SetStr(arena, step, "completedAt", completed_at) ||
        !SetNumOrNull(arena, step, "durationSeconds", has_duration, duration) ||
        !SetStr(arena, step, "timingSource", started_at || completed_at || has_duration ? "provider" : nullptr) ||
        !SetVal(arena, step, "detail", squid_step_detail(arena, action, kind, from_chain, to_chain)) ||
        !SetVal(arena, step, "errorCode", problem ? error_code : nullptr) ||
        !SetVal(arena, step, "errorMessage", problem ? error_message : nullptr) || !tj::Push(arena, squid, step))
      return nullptr;
  }
  if (squid->count == 0) {
    const char* terminal = is("completed")                    ? "completed"
                           : is("refunded")                   ? "refunded"
                           : is("failed") || is("partial")    ? "failed"
                           : is("needs-action")               ? "needs-action"
                                                              : "pending";
    Value* source = ProgressStep(arena, "source", "source", S(arena, "Source transaction"),
                                 is("not-found") ? "pending" : "completed", request.from_chain_id, request.tx_hash);
    if (!source || !SetVal(arena, source, "explorerUrl", StringValue(Field(from_chain, "transactionUrl"))) ||
        !SetStr(arena, source, "detail", "Submitted on the source network.") || !tj::Push(arena, squid, source))
      return nullptr;
    Value* bridge = ProgressStep(arena, "bridge", "bridge",
                                 request.bridge_id ? request.bridge_id : S(arena, "Cross-chain transfer"), terminal,
                                 nullptr, nullptr);
    if (!bridge ||
        !SetStr(arena, bridge, "detail",
                is("needs-action") ? "Destination execution is paused because additional gas is required." : nullptr) ||
        !SetVal(arena, bridge, "errorCode",
                is("needs-action") ? (error_code ? error_code : S(arena, "NEEDS_GAS")) : nullptr) ||
        !SetVal(arena, bridge, "errorMessage", is("failed") || is("needs-action") ? error_message : nullptr) ||
        !tj::Push(arena, squid, bridge))
      return nullptr;
    Value* destination = ProgressStep(arena, "destination", "destination", S(arena, "Destination transaction"),
                                      is("completed") ? "completed" : is("failed") ? "failed" : "not-started",
                                      request.to_chain_id, destination_hash);
    if (!destination || !SetVal(arena, destination, "explorerUrl", StringValue(Field(to_chain, "transactionUrl"))) ||
        !SetStr(arena, destination, "detail",
                is("partial") ? "The destination call reverted after the bridged asset arrived." : nullptr) ||
        !SetVal(arena, destination, "errorCode", is("partial") || is("failed") ? error_code : nullptr) ||
        !SetVal(arena, destination, "errorMessage", is("partial") || is("failed") ? error_message : nullptr) ||
        !tj::Push(arena, squid, destination))
      return nullptr;
  }
  if (is("needs-action")) {
    bool any = false;
    for (uint32_t i = 0; i < squid->count; ++i) any = any || Is(tj::Get(squid->items[i], "state"), "needs-action");
    if (!any) {
      int index = -1;
      for (uint32_t i = squid->count; i-- > 0;)
        if (!Is(tj::Get(squid->items[i], "state"), "completed")) { index = static_cast<int>(i); break; }
      const uint32_t target = index >= 0 ? static_cast<uint32_t>(index) : squid->count - 1;
      if (target < squid->count) {
        Value* replaced = replace_step(arena, squid->items[target], "needs-action",
                                       "Destination execution is paused because additional gas is required.",
                                       "NEEDS_GAS", error_code, error_message);
        if (!replaced) return nullptr;
        squid->items[target] = replaced;
      }
    }
  }
  if (is("partial") && squid->count > 0) {
    int index = -1;
    for (uint32_t i = squid->count; i-- > 0;)
      if (Is(tj::Get(squid->items[i], "kind"), "destination")) { index = static_cast<int>(i); break; }
    const uint32_t target = index >= 0 ? static_cast<uint32_t>(index) : squid->count - 1;
    Value* replaced = replace_step(arena, squid->items[target], "failed",
                                   "The destination call reverted after the bridged asset arrived. The intermediate "
                                   "asset should remain in the destination wallet.",
                                   "PARTIAL_SUCCESS", error_code, error_message);
    if (!replaced) return nullptr;
    squid->items[target] = replaced;
  }
  const Value* received_token = AsRecord(Field(to_chain, "token"));
  const char* received_amount = SafeNonNegativeInteger(arena, Field(to_chain, "amount"));
  double elapsed = 0;
  const bool has_elapsed = IntegerNumber(Field(Field(root, "timeSpent"), "total"), &elapsed) && elapsed >= 0;
  AxelarProgress axelar;
  if (!parse_axelar(arena, axelar_payload, request, from_chain, to_chain, state, &axelar, thrown)) return nullptr;
  Value* progress = nullptr;
  if (axelar.steps->count > 0) {
    progress = axelar.steps;
    for (uint32_t i = 0; i < squid->count; ++i) {
      const Value* step_state = tj::Get(squid->items[i], "state");
      if ((Is(step_state, "failed") || Is(step_state, "needs-action") || Is(step_state, "refunded")) &&
          !tj::Push(arena, progress, squid->items[i]))
        return nullptr;
    }
  } else {
    progress = squid;
  }
  if (axelar.destination_hash) destination_hash = axelar.destination_hash;
  const Value* explorer = StringValue(Field(root, "axelarTransactionUrl"));
  if (!explorer && axelar.steps->count > 0) {
    const size_t capacity = request.tx_hash->length * 9 + 64;
    uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
    if (!buffer) return nullptr;
    OutBuffer out{buffer, capacity, 0, true};
    out.Text("https://axelarscan.io/gmp/");
    if (!EncodeComponent(request.tx_hash, out)) { *thrown = true; return nullptr; }
    if (!out.ok) return nullptr;
    explorer = tj::Str(arena, reinterpret_cast<const char*>(buffer), out.used);
  }
  Value* received = tj::Null(arena);
  if (received_amount) {
    received = tj::Obj(arena);
    double decimals = 0;
    const bool has_decimals = IntegerNumber(Field(received_token, "decimals"), &decimals);
    if (!received || !SetVal(arena, received, "chainId", request.to_chain_id) ||
        !SetStr(arena, received, "amount", received_amount) ||
        !SetVal(arena, received, "tokenAddress", StringValue(Field(received_token, "address"))) ||
        !SetVal(arena, received, "symbol", StringValue(Field(received_token, "symbol"), 64)) ||
        !SetNumOrNull(arena, received, "decimals", has_decimals, decimals))
      return nullptr;
  }
  Value* provider_data = tj::Obj(arena);
  if (!provider_data || !tj::Set(arena, provider_data, "squid", BoundedProviderSnapshot(arena, payload)) ||
      (axelar.support && !tj::Set(arena, provider_data, "axelar", axelar.support)))
    return nullptr;
  Value* recovery = squid_recovery(arena, state, explorer, error_message);
  Value* out = tj::Obj(arena);
  if (!recovery || !received || !out || !SetStr(arena, out, "providerId", "squid") ||
      !SetStr(arena, out, "state", state) || !SetVal(arena, out, "substatus", status) ||
      !SetVal(arena, out, "message", error_message) || !SetVal(arena, out, "sourceHash", request.tx_hash) ||
      !SetVal(arena, out, "destinationHash", destination_hash) ||
      !SetVal(arena, out, "providerExplorerUrl", explorer) || !SetVal(arena, out, "bridgeExplorerUrl", explorer) ||
      !SetNumOrNull(arena, out, "elapsedSeconds", has_elapsed, elapsed == 0 ? 0 : elapsed) ||
      !tj::Set(arena, out, "actualReceived", received) || !tj::Set(arena, out, "progress", progress) ||
      !tj::Set(arena, out, "recovery", recovery) || !tj::Set(arena, out, "providerData", provider_data))
    return nullptr;
  return out;
}

}  // namespace

// ---- entry points ------------------------------------------------------------------

}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome LifiProvider::Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  if (!settings.lifi_configured || IsTon(request.from.chain_id) || IsTon(request.to.chain_id)) {
    *routes = Routes(arena, nullptr);
    return *routes ? Outcome::Ok() : Error();
  }
  char slippage[40];
  NumberText(static_cast<double>(request.slippage_bps) / 10000, slippage);
  Text path;
  if (!path.Init(arena, value_bytes(request.from.token) + value_bytes(request.to.token) +
                            value_bytes(request.from.address) + value_bytes(request.to.address) +
                            value_bytes(request.from.chain_id) + value_bytes(request.to.chain_id) + 512))
    return Error();
  path.out.Text("/v1/quote?");
  QueryPair pairs[] = {{"fromChain", lifi_chain(arena, request.from.chain_id)},
                       {"toChain", lifi_chain(arena, request.to.chain_id)},
                       {"fromToken", request.from.token},
                       {"toToken", request.to.token},
                       {"fromAmount", request.amount_in},
                       {"fromAddress", request.from.address},
                       {"toAddress", request.to.address},
                       {"slippage", S(arena, slippage)}};
  FormEncode(pairs, sizeof(pairs) / sizeof(pairs[0]), path.out);
  Fetch fetch;
  fetch.lane = "lifi";
  fetch.base = kLifiBase;
  fetch.path = path.Finish();
  fetch.header_name = "x-lifi-api-key";
  fetch.header_value = settings.lifi_api_key;
  fetch.timeout_ms = 12000;
  if (!fetch.path) return Error();
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  *routes = Routes(arena, parse_lifi_route(arena, fetch.value, request, context.Now()));
  return *routes ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome LifiProvider::Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  if (!settings.lifi_configured) return Error();
  auto load = [&](const Value* bridge_id, Fetch* fetch) {
    fetch->lane = "lifi";
    fetch->base = kLifiBase;
    fetch->path = lifi_status_path(arena, request, bridge_id);
    fetch->header_name = "x-lifi-api-key";
    fetch->header_value = settings.lifi_api_key;
    fetch->timeout_ms = 12000;
    if (!fetch->path) { fetch->failure = Failure::kError; return; }
    RunFetch(context, arena, *fetch);
  };
  Fetch first;
  load(request.bridge_id, &first);
  const Value* payload = first.value;
  if (!first.ok()) {
    // Retry only LI.FI's 400 for a stale or unknown bridge hint.
    if (!request.bridge_id || first.failure != Failure::kError || first.http_status != 400) return first.error();
    Fetch second;
    load(nullptr, &second);
    if (!second.ok()) return second.error();
    payload = second.value;
  }
  *status = parse_lifi_status(arena, payload, request);
  return *status ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome SquidProvider::Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  if (!settings.squid_configured || IsTon(request.from.chain_id) || IsTon(request.to.chain_id)) {
    *routes = Routes(arena, nullptr);
    return *routes ? Outcome::Ok() : Error();
  }
  Value* body = tj::Obj(arena);
  if (!body || !SetVal(arena, body, "fromChain", request.from.chain_id) ||
      !SetVal(arena, body, "toChain", request.to.chain_id) ||
      !SetVal(arena, body, "fromToken", squid_token(arena, request.from.token, request.from.chain_id)) ||
      !SetVal(arena, body, "toToken", squid_token(arena, request.to.token, request.to.chain_id)) ||
      !SetVal(arena, body, "fromAmount", request.amount_in) || !SetVal(arena, body, "fromAddress", request.from.address) ||
      !SetVal(arena, body, "toAddress", request.to.address) ||
      !SetNum(arena, body, "slippage", static_cast<double>(request.slippage_bps) / 100))
    return Error();
  Fetch fetch;
  fetch.lane = "squid";
  fetch.base = kSquidBase;
  fetch.path = "/v2/route";
  fetch.post = true;
  fetch.body = body;
  fetch.header_name = "x-integrator-id";
  fetch.header_value = settings.squid_integrator_id;
  fetch.timeout_ms = 12000;
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  *routes = Routes(arena, parse_squid_route(arena, fetch.value, request, context.Now()));
  return *routes ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome SquidProvider::Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const {
  using namespace detail;
  const ProviderSettings& settings = *context.settings;
  if (!settings.squid_configured) return Error();
  Text path;
  if (!path.Init(arena, value_bytes(request.tx_hash) + value_bytes(request.provider_quote_id) +
                            value_bytes(request.from_chain_id) + value_bytes(request.to_chain_id) + 256))
    return Error();
  path.out.Text("/v2/status?");
  QueryPair pairs[] = {{"transactionId", request.tx_hash},
                       {"fromChainId", request.from_chain_id},
                       {"toChainId", request.to_chain_id},
                       {"quoteId", request.provider_quote_id}};
  FormEncode(pairs, request.provider_quote_id ? 4 : 3, path.out);
  Fetch fetch;
  fetch.lane = "squid";
  fetch.base = kSquidBase;
  fetch.path = path.Finish();
  fetch.header_name = "x-integrator-id";
  fetch.header_value = settings.squid_integrator_id;
  fetch.timeout_ms = 12000;
  if (!fetch.path) return Error();
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  const Value* axelar = nullptr;
  if (should_fetch_axelar(arena, fetch.value, request)) {
    // AxelarScan is enrichment: any failure (including an unencodable hash)
    // leaves it out.
    Text gmp;
    if (gmp.Init(arena, request.tx_hash->length * 9 + 64)) {
      gmp.out.Text("/gmp/searchGMP?txHash=");
      if (EncodeComponent(request.tx_hash, gmp.out)) {
        Fetch enrich;
        enrich.lane = "axelarscan";
        enrich.base = kAxelarBase;
        enrich.path = gmp.Finish();
        enrich.timeout_ms = 8000;
        if (enrich.path) {
          RunFetch(context, arena, enrich);
          if (enrich.ok()) axelar = enrich.value;
        }
      }
    }
  }
  bool thrown = false;
  *status = parse_squid_status(arena, fetch.value, request, axelar, &thrown);
  return *status ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
namespace {
bool lifi_configured(const ProviderSettings& settings) { return settings.lifi_configured; }
bool squid_configured(const ProviderSettings& settings) { return settings.squid_configured; }
}  // namespace

const LifiProvider kLifiProvider{};
const SquidProvider kSquidProvider{};

bool LifiProvider::Configured(const ProviderSettings& settings) const { return lifi_configured(settings); }
bool SquidProvider::Configured(const ProviderSettings& settings) const { return squid_configured(settings); }
}  // namespace fortis::exchange::cross_chain
