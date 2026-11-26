// The TypeScript EvmTxDetailView: the app's EvmTxDetails.toJson()
// built from eth_getTransactionByHash / eth_getTransactionReceipt /
// eth_getBlockByNumber, the 4byte method name (through ctx.resolve_selector,
// the facet's cached MethodSignatureResolver) and Blockscout internal
// transfers (blockscoutBaseFor). Guest-safe.
#include "chain_family_drivers.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

using tj::Type;

struct Blockscout {
  double chain_id;
  const char* base;
};
// BLOCKSCOUT_BY_CHAIN_ID, mirrored verbatim from the app.
constexpr Blockscout kBlockscout[] = {
    {1, "https://eth.blockscout.com"},
    {11155111, "https://eth-sepolia.blockscout.com"},
    {8453, "https://base.blockscout.com"},
    {84532, "https://base-sepolia.blockscout.com"},
    {42161, "https://arbitrum.blockscout.com"},
    {137, "https://polygon.blockscout.com"},
    {4663, "https://robinhoodchain.blockscout.com"},
    {14, "https://flare-explorer.flare.network"},
};

const char* blockscout_base(const Network& network) {
  if (!network.has_evm_chain_id) return nullptr;
  for (const Blockscout& entry : kBlockscout)
    if (entry.chain_id == network.evm_chain_id) return entry.base;
  return nullptr;
}

bool nullish(const Value* v) { return !v || v->type == Type::kNull; }
bool is_string(const Value* v) { return v && v->type == Type::kString; }

bool oom(Error* error) {
  error->Upstream("arena exhausted");
  return false;
}

Value* hash_params(Arena& a, const char* hash) {
  Value* params = tj::Arr(a);
  return params && tj::Push(a, params, tj::Str(a, hash)) ? params : nullptr;
}

void rpc(Call& call, const Network& network, const char* method, const Value* params) {
  call = Call{};
  call.kind = CallKind::kJsonRpc;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.method = method;
  call.params = params;
}

// hexToDecimalString as a nullable value pointer (nullptr = null).
Value* hex_decimal(Arena& a, const Value* v) {
  Value* out = HexToDecimalString(a, v);
  return out && out->type == Type::kString ? out : nullptr;
}

Value* hex_int(Arena& a, const Value* v) {
  const auto number = HexToInt(v);
  return number ? tj::Num(a, *number) : nullptr;
}

// put(key, value): omitted when null/undefined.
bool put(Arena& a, Value* detail, const char* key, Value* value) {
  if (!value || value->type == Type::kNull) return true;
  return tj::Set(a, detail, key, value);
}

// internalTransfers(): fail-soft Blockscout txlistinternal legs.
bool internal_transfers(Context& ctx, const Network& network, const char* hash, Value** legs) {
  Arena& a = *ctx.arena;
  *legs = tj::Arr(a);
  if (!*legs) return false;
  const char* base = blockscout_base(network);
  if (!base) return true;
  // `txhash=${hash}`, unencoded: as fetch() serializes the URL string.
  Value* raw_path = Format(a, "/api?module=account&action=txlistinternal&txhash=%s", hash);
  const char* path = raw_path ? FetchWirePath(a, raw_path->text) : nullptr;
  if (!path) return false;
  const char* urls[1] = {base};
  Call call;
  call.kind = CallKind::kGet;
  call.urls = urls;
  call.url_count = 1;
  call.path = path;
  call.lane = "blockscout";
  ctx.transport->Run(call, a);
  if (!call.ok()) return true;
  const Value* rows = tj::Get(call.value, "result");
  if (!rows || rows->type != Type::kArray) return true;
  Value* out = tj::Arr(a);
  if (!out) return false;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const Value* row = rows->items[i];
    // `row.value` on a null element throws inside the try: no legs at all.
    if (row->type == Type::kNull) return true;
    const Value* value = tj::Get(row, "value");
    if (!is_string(value) || !value->length) continue;
    bool digits = true;
    for (uint32_t c = 0; c < value->length && digits; ++c) digits = value->text[c] >= '0' && value->text[c] <= '9';
    if (!digits) continue;
    Value* leg = tj::Obj(a);
    if (!leg || !tj::Set(a, leg, "valueWei", const_cast<Value*>(value))) return false;
    const Value* from = tj::Get(row, "from");
    const Value* to = tj::Get(row, "to");
    if (is_string(from) && !tj::Set(a, leg, "from", const_cast<Value*>(from))) return false;
    if (is_string(to) && !tj::Set(a, leg, "to", const_cast<Value*>(to))) return false;
    if (!tj::Push(a, out, leg)) return false;
  }
  *legs = out;
  return true;
}

}  // namespace

bool EvmView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail_out,
                    Error* error) const {
  Arena& a = *ctx.arena;
  Value* params = hash_params(a, hash);
  if (!params) return oom(error);
  Call call;
  rpc(call, network, "eth_getTransactionByHash", params);
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* tx = call.value;
  if (!tx || tx->type == Type::kNull) { *detail_out = nullptr; return true; }

  const Value* block_number = tj::Get(tx, "blockNumber");
  const bool mined = !nullish(block_number);
  Value* receipt = nullptr;  // null
  Value* block = nullptr;
  if (mined) {
    Value* receipt_params = hash_params(a, hash);
    if (!receipt_params) return oom(error);
    Call receipt_call;
    rpc(receipt_call, network, "eth_getTransactionReceipt", receipt_params);
    ctx.transport->Run(receipt_call, a);
    if (!receipt_call.ok()) { receipt_call.ToError(error); return false; }
    receipt = nullish(receipt_call.value) ? nullptr : receipt_call.value;
    Value* block_params = tj::Arr(a);
    if (!block_params || !tj::Push(a, block_params, tj::Clone(a, block_number)) ||
        !tj::Push(a, block_params, tj::Bool(a, false)))
      return oom(error);
    Call block_call;
    rpc(block_call, network, "eth_getBlockByNumber", block_params);
    ctx.transport->Run(block_call, a);
    // The timestamp is a nicety, not the answer.
    if (block_call.ok() && !nullish(block_call.value)) block = block_call.value;
  }

  Value* gas_used = hex_decimal(a, tj::Get(receipt, "gasUsed"));
  Value* gas_price = hex_decimal(a, tj::Get(receipt, "effectiveGasPrice"));
  if (!gas_price) gas_price = hex_decimal(a, tj::Get(tx, "gasPrice"));
  const Value* input = tj::Get(tx, "input");
  Value* input_data = is_string(input) ? const_cast<Value*>(input) : nullptr;
  const auto timestamp_sec = HexToInt(tj::Get(block, "timestamp"));

  // methodFor(inputData) then internalTransfers(): Promise.all in TypeScript;
  // both fail soft, so running them in turn answers the same.
  Value* method = nullptr;
  if (ctx.resolve_selector && input_data && input_data->length >= 10 &&
      !(input_data->length == 2 && memcmp(input_data->text, "0x", 2) == 0)) {
    char selector[11];
    memcpy(selector, input_data->text, 10);
    selector[10] = '\0';
    const char* name = ctx.resolve_selector(ctx.selector_context, selector, a);
    if (name) method = tj::Str(a, name);
  }
  Value* internal = nullptr;
  if (!internal_transfers(ctx, network, hash, &internal)) return oom(error);

  Value* detail = tj::Obj(a);
  if (!detail) return oom(error);
  Value* timestamp = nullptr;
  if (timestamp_sec) {
    char iso[32];
    // new Date(sec * 1000).toISOString() throws a RangeError out of range.
    if (fabs(*timestamp_sec * 1000) > 8.64e15 || !IsoTime(static_cast<int64_t>(*timestamp_sec * 1000), iso)) {
      error->Range("Invalid time value");
      return false;
    }
    timestamp = tj::Str(a, iso);
  }
  Value* fee = nullptr;
  if (gas_used && gas_price) {
    BigInt product, price;
    if (!BigInt::ParseDecimal(gas_used->text, gas_used->length, &product) ||
        !BigInt::ParseDecimal(gas_price->text, gas_price->length, &price) || !product.Mul(price))
      return oom(error);
    fee = product.ToValue(a);
  }
  const Value* to = tj::Get(tx, "to");
  bool ok = put(a, detail, "blockNumber", hex_int(a, block_number)) &&
            put(a, detail, "timestamp", timestamp) && put(a, detail, "txType", hex_int(a, tj::Get(tx, "type"))) &&
            put(a, detail, "nonce", hex_int(a, tj::Get(tx, "nonce"))) &&
            put(a, detail, "positionInBlock", hex_int(a, tj::Get(tx, "transactionIndex"))) &&
            put(a, detail, "valueWei", hex_decimal(a, tj::Get(tx, "value"))) &&
            put(a, detail, "gasLimit", hex_decimal(a, tj::Get(tx, "gas"))) && put(a, detail, "gasUsed", gas_used) &&
            put(a, detail, "effectiveGasPriceWei", gas_price) && put(a, detail, "feeWei", fee);
  // `status` is absent on pre-Byzantium receipts and on a pending tx.
  const Value* status = tj::Get(receipt, "status");
  if (ok && receipt && is_string(status)) {
    const bool reverted = status->length == 3 && memcmp(status->text, "0x0", 3) == 0;
    ok = put(a, detail, "success", tj::Bool(a, !reverted));
  }
  ok = ok && put(a, detail, "inputData", input_data) &&
       put(a, detail, "toAddress", is_string(to) ? const_cast<Value*>(to) : nullptr) &&
       put(a, detail, "inputMethod", method) &&
       (internal->count == 0 || put(a, detail, "internal", internal)) && put(a, detail, "rawTx", tx) &&
       put(a, detail, "rawReceipt", receipt);
  if (!ok) return oom(error);
  *detail_out = detail;
  return true;
}

namespace {

// isFinal: detail.blockNumber != null.
bool evm_view_final(const Value* detail, const Network&) { return !nullish(tj::Get(detail, "blockNumber")); }

}  // namespace

const EvmView kEvmView{};


bool EvmView::IsFinal(const Value* detail, const Network& network) const { return evm_view_final(detail, network); }

}  // namespace fortis::chains
