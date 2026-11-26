// XRPL family of the address, transaction and network routes: the balance
// driver (balances + displayCurrency), the account_tx transfer source and
// the `tx` detail view, as in the earlier TypeScript implementation.
// Trustlines and token insight belong to other routes and are not ported
// here. Guest-safe.
#include "chain_family_drivers.h"

#include "chain_asset_index.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

using tj::Type;

constexpr int kXrpDecimals = 6;
constexpr double kRippleEpochOffset = 946684800;

bool is_str(const Value* v) { return v && v->type == Type::kString; }
bool nullish(const Value* v) { return !v || v->type == Type::kNull; }

// JavaScript truthiness of a JSON value (absent = undefined).
bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case Type::kNull: case Type::kFalse: return false;
    case Type::kTrue: case Type::kArray: case Type::kObject: return true;
    case Type::kNumber: return v->number != 0 && !isnan(v->number);
    case Type::kString: return v->length != 0;
  }
  return false;
}

bool digits(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// `value ?? null`.
Value* or_null(Arena& arena, Value* v) { return nullish(v) ? tj::Null(arena) : v; }

// String(value) for the JSON shapes providers send.
Value* js_string(Arena& arena, const Value* v) {
  if (!v) return tj::Str(arena, "undefined");
  switch (v->type) {
    case Type::kString: return tj::Str(arena, v->text, v->length);
    case Type::kNumber: {
      char text[40];
      JsNumberText(v->number, text, sizeof(text));
      return tj::Str(arena, text);
    }
    case Type::kTrue: return tj::Str(arena, "true");
    case Type::kFalse: return tj::Str(arena, "false");
    case Type::kNull: return tj::Str(arena, "null");
    case Type::kObject: return tj::Str(arena, "[object Object]");
    case Type::kArray: {
      tj::OutBuffer out{static_cast<uint8_t*>(arena.Alloc(4096, 1)), 4096, 0, true};
      if (!out.data) return nullptr;
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        const Value* item = v->items[i];
        if (nullish(item)) continue;
        const Value* text = js_string(arena, item);
        if (text) out.Append(text->text, text->length);
      }
      return out.ok ? tj::Str(arena, reinterpret_cast<const char*>(out.data), out.used) : nullptr;
    }
  }
  return nullptr;
}

// One {account, ledger_index:'validated'} JSON-RPC parameter array.
Value* account_params(Arena& arena, const char* address) {
  Value* params = tj::Arr(arena);
  Value* request = tj::Obj(arena);
  tj::Set(arena, request, "account", tj::Str(arena, address));
  tj::Set(arena, request, "ledger_index", tj::Str(arena, "validated"));
  tj::Push(arena, params, request);
  return params;
}

// /^r[1-9A-HJ-NP-Za-km-z]{24,34}$/
bool xrpl_address(const char* address) {
  const size_t length = strlen(address);
  if (length < 25 || length > 35 || address[0] != 'r') return false;
  for (size_t i = 1; i < length; ++i) {
    const char c = address[i];
    const bool ok = (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') ||
                    (c >= 'P' && c <= 'Z') || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
    if (!ok) return false;
  }
  return true;
}

// displayCurrency(currency), shared with the token routes' issuer insight.
const char* display_currency(Arena& arena, const char* currency, size_t length) {
  return asset_index::XrplDisplayCurrency(arena, currency, length);
}

// result.error === 'actNotFound' / result.status === 'error' || result.error.
bool error_is(const Value* result, const char* code) { return Equals(tj::Get(result, "error"), code); }
bool result_failed(const Value* result) {
  return Equals(tj::Get(result, "status"), "error") || truthy(tj::Get(result, "error"));
}

void native_coin(const Network& network, const char** symbol, uint32_t* decimals) {
  *symbol = network.has_native ? network.native_symbol : "XRP";
  *decimals = network.has_native ? network.native_decimals : kXrpDecimals;
}

Value* native_entry(Arena& arena, const char* raw, size_t raw_length, const Network& network) {
  const char* symbol;
  uint32_t decimals;
  native_coin(network, &symbol, &decimals);
  Value* native = tj::Obj(arena);
  tj::Set(arena, native, "balanceRaw", tj::Str(arena, raw, raw_length));
  tj::Set(arena, native, "decimals", tj::Num(arena, decimals));
  tj::Set(arena, native, "symbol", tj::Str(arena, symbol));
  tj::Set(arena, native, "source", tj::Str(arena, "xrpl"));
  return native;
}

bool finish(Arena& arena, Error* error) {
  if (arena.ok) return true;
  error->Upstream("xrpl: work arena exhausted");
  return false;
}

// ---- driver --------------------------------------------------------------------------

}  // namespace

bool XrplDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const {
  if (!xrpl_address(address)) {
    error->Range("not an XRPL address: %s", address);
    return false;
  }
  Arena& arena = *ctx.arena;
  Call info{};
  info.kind = CallKind::kJsonRpc;
  info.urls = network.rpc_urls;
  info.url_count = network.rpc_url_count;
  info.method = "account_info";
  info.params = account_params(arena, address);
  ctx.transport->Run(info, arena);
  if (!info.ok()) { info.ToError(error); return false; }
  // A JSON null result is a TypeError on `info.error` in TypeScript.
  if (nullish(info.value)) { error->Upstream("account_info: null result"); return false; }
  if (error_is(info.value, "actNotFound")) {
    // Unfunded (below the base reserve) — a real zero.
    Value* out = tj::Obj(arena);
    tj::Set(arena, out, "native", native_entry(arena, "0", 1, network));
    tj::Set(arena, out, "tokens", tj::Arr(arena));
    *result = out;
    return finish(arena, error);
  }
  if (result_failed(info.value)) {
    error->Upstream("account_info failed");
    return false;
  }

  Call lines{};
  lines.kind = CallKind::kJsonRpc;
  lines.urls = network.rpc_urls;
  lines.url_count = network.rpc_url_count;
  lines.method = "account_lines";
  lines.params = account_params(arena, address);
  ctx.transport->Run(lines, arena);
  if (!lines.ok()) { lines.ToError(error); return false; }
  if (nullish(lines.value)) { error->Upstream("account_lines: null result"); return false; }
  if (result_failed(lines.value)) {
    error->Upstream("account_lines failed");
    return false;
  }

  Value* tokens = tj::Arr(arena);
  const Value* list = tj::Get(lines.value, "lines");
  if (tj::IsArray(list)) {
    for (uint32_t i = 0; i < list->count; ++i) {
      const Value* line = list->items[i];
      const Value* account = tj::Get(line, "account");
      const Value* currency = tj::Get(line, "currency");
      const Value* balance = tj::Get(line, "balance");
      // Non-string provider fields are treated as absent (TypeScript would
      // coerce or throw on them; rippled always sends strings).
      if (!truthy(account) || !truthy(currency) || !truthy(balance)) continue;
      if (!is_str(account) || !is_str(currency) || !is_str(balance)) continue;
      BigInt raw;
      int decimals = 0;
      // Negative = an obligation of THIS account (it is the issuer) — a debt,
      // not a holding. Zero lines are empty trustlines.
      if (!DecimalToRaw(balance->text, balance->length, -1, &raw, &decimals)) continue;
      if (raw.IsZero() || raw.negative()) continue;
      const char* code = display_currency(arena, currency->text, currency->length);
      if (!code) continue;
      Value* token = tj::Obj(arena);
      tj::Set(arena, token, "tokenId",
              Format(arena, "iou:%.*s:%s", static_cast<int>(account->length), account->text, code));
      tj::Set(arena, token, "balanceRaw", raw.ToValue(arena));
      tj::Set(arena, token, "decimals", tj::Num(arena, decimals));
      tj::Set(arena, token, "symbol", SanitizeSymbol(arena, code, strlen(code)));
      tj::Set(arena, token, "source", tj::Str(arena, "xrpl"));
      tj::Push(arena, tokens, token);
    }
  }

  const Value* drops = tj::Get(tj::Get(info.value, "account_data"), "Balance");
  const bool exact = is_str(drops) && digits(drops->text, drops->length);
  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "native",
          exact ? native_entry(arena, drops->text, drops->length, network) : native_entry(arena, "0", 1, network));
  tj::Set(arena, out, "tokens", tokens);
  *result = out;
  return finish(arena, error);
}

namespace {

// ---- transfer source ------------------------------------------------------------------

// XrplTransferSource.record(): one account_tx entry, or null.
Value* record(Arena& arena, const Network& network, const char* holder, const Value* entry) {
  const Value* tx = tj::Get(entry, "tx");
  const Value* hash = tj::Get(tx, "hash");
  const Value* date = tj::Get(tx, "date");
  if (!truthy(hash) || !tj::IsNumber(date)) return nullptr;
  const Value* meta = tj::Get(entry, "meta");
  const Value* result_code = tj::Get(meta, "TransactionResult");
  const bool failed = !Equals(result_code, "tesSUCCESS");
  const double ts_ms = (date->number + kRippleEpochOffset) * 1000;
  if (!isfinite(ts_ms)) return nullptr;
  Value* account = tj::Get(tx, "Account");
  Value* destination = tj::Get(tx, "Destination");
  const bool outgoing = is_str(account) && strlen(holder) == account->length &&
                        memcmp(account->text, holder, account->length) == 0;
  const bool incoming = is_str(destination) && strlen(holder) == destination->length &&
                        memcmp(destination->text, holder, destination->length) == 0;
  const char* direction = DirectionFromFlow(incoming, outgoing);
  Value* native = NativeAssetJson(arena, network);
  const Value* fee = tj::Get(tx, "Fee");
  const Value* fee_text = nullish(fee) ? tj::Str(arena, "") : js_string(arena, fee);
  const bool pays_fee = outgoing && fee_text && digits(fee_text->text, fee_text->length);
  const char* hash_text = nullptr;
  {
    const Value* text = js_string(arena, hash);
    hash_text = text ? tj::CString(arena, text) : nullptr;
  }
  if (!hash_text) return nullptr;

  Value* r = NewTransfer(arena);
  tj::Set(arena, r, "status", tj::Str(arena, failed ? "failed" : "confirmed"));
  tj::Set(arena, r, "direction", tj::Str(arena, direction));
  tj::Set(arena, r, "ts", tj::Num(arena, trunc(ts_ms)));
  tj::Set(arena, r, "hash", const_cast<Value*>(hash));
  tj::Set(arena, r, "blockNumber", or_null(arena, tj::Get(tx, "ledger_index")));
  tj::Set(arena, r, "fromAddress", or_null(arena, account));
  tj::Set(arena, r, "toAddress", or_null(arena, destination));
  tj::Set(arena, r, "counterparty", incoming ? or_null(arena, account) : or_null(arena, destination));
  tj::Set(arena, r, "feeRaw", pays_fee ? const_cast<Value*>(fee) : tj::Null(arena));
  tj::Set(arena, r, "feeDecimals", pays_fee ? tj::Num(arena, kXrpDecimals) : tj::Null(arena));
  tj::Set(arena, r, "feeSymbol", pays_fee ? tj::Get(native, "symbol") : tj::Null(arena));
  const Value* tag = tj::Get(tx, "DestinationTag");
  tj::Set(arena, r, "memo", tag ? js_string(arena, tag) : tj::Null(arena));
  tj::Set(arena, r, "error",
          failed ? (nullish(result_code) ? tj::Str(arena, "failed") : const_cast<Value*>(result_code))
                 : tj::Null(arena));

  const Value* type = tj::Get(tx, "TransactionType");
  if (!Equals(type, "Payment")) {
    // TrustSet / OfferCreate / … — visible as an interaction, typed honestly.
    tj::Set(arena, r, "recordId", Format(arena, "%s:%s", network.id, hash_text));
    tj::Set(arena, r, "txType", tj::Str(arena, "contractCall"));
    tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
    tj::Set(arena, r, "method", or_null(arena, const_cast<Value*>(type)));
    return r;
  }

  // delivered_amount is what actually moved (partial payments!).
  const Value* delivered = tj::Get(meta, "delivered_amount");
  if (nullish(delivered)) delivered = tj::Get(tx, "Amount");
  if (is_str(delivered)) {
    if (!digits(delivered->text, delivered->length)) return nullptr;
    tj::Set(arena, r, "recordId", Format(arena, "%s:%s", network.id, hash_text));
    tj::Set(arena, r, "txType", tj::Str(arena, TypeFromDirection(direction)));
    tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
    tj::Set(arena, r, "asset", native);
    tj::Set(arena, r, "amountRaw", const_cast<Value*>(delivered));
    tj::Set(arena, r, "amountDecimals", tj::Num(arena, kXrpDecimals));
    tj::Set(arena, r, "method", tj::Str(arena, "Payment"));
    return r;
  }
  const Value* value = tj::Get(delivered, "value");
  const Value* issuer = tj::Get(delivered, "issuer");
  const Value* currency = tj::Get(delivered, "currency");
  if (truthy(delivered) && is_str(value) && truthy(issuer) && truthy(currency) && is_str(issuer) &&
      is_str(currency)) {
    BigInt raw;
    int decimals = 0;
    if (!DecimalToRaw(value->text, value->length, -1, &raw, &decimals)) return nullptr;
    const char* code = display_currency(arena, currency->text, currency->length);
    if (!code) return nullptr;
    const int issuer_length = static_cast<int>(issuer->length);
    tj::Set(arena, r, "recordId",
            Format(arena, "%s:%s:%.*s:%s", network.id, hash_text, issuer_length, issuer->text, code));
    tj::Set(arena, r, "txType", tj::Str(arena, TypeFromDirection(direction)));
    tj::Set(arena, r, "tokenId", Format(arena, "iou:%.*s:%s", issuer_length, issuer->text, code));
    Value* asset = tj::Obj(arena);
    tj::Set(arena, asset, "chainId", tj::Str(arena, network.id));
    tj::Set(arena, asset, "symbol", SanitizeSymbol(arena, code, strlen(code)));
    tj::Set(arena, asset, "name", SanitizeSymbol(arena, code, strlen(code)));
    tj::Set(arena, asset, "decimals", tj::Num(arena, decimals));
    tj::Set(arena, asset, "contractAddress", const_cast<Value*>(issuer));
    tj::Set(arena, r, "asset", asset);
    tj::Set(arena, r, "amountRaw", raw.ToValue(arena));
    tj::Set(arena, r, "amountDecimals", tj::Num(arena, decimals));
    tj::Set(arena, r, "method", tj::Str(arena, "Payment"));
    return r;
  }
  return nullptr;
}

Value* min_ledger_state(Arena& arena, double min_ledger) {
  Value* state = tj::Obj(arena);
  tj::Set(arena, state, "minLedger", tj::Num(arena, min_ledger));
  return state;
}

}  // namespace

bool XrplSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const {
  Arena& arena = *ctx.arena;
  const Value* stored = tj::Get(state, "minLedger");
  const double min_ledger = tj::IsNumber(stored) ? stored->number : -1;
  const double limit = fmin(backfill_depth, 200);

  Value* params = tj::Arr(arena);
  Value* request = tj::Obj(arena);
  tj::Set(arena, request, "account", tj::Str(arena, address));
  tj::Set(arena, request, "ledger_index_min", tj::Num(arena, min_ledger));
  tj::Set(arena, request, "ledger_index_max", tj::Num(arena, -1));
  tj::Set(arena, request, "forward", tj::Bool(arena, true));
  tj::Set(arena, request, "limit", tj::Num(arena, limit));
  tj::Push(arena, params, request);
  Call call{};
  call.kind = CallKind::kJsonRpc;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.method = "account_tx";
  call.params = params;
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  if (nullish(call.value)) { error->Upstream("account_tx: null result"); return false; }
  if (error_is(call.value, "actNotFound")) {
    out->records = tj::Arr(arena);
    out->state = min_ledger_state(arena, min_ledger);
    out->backfill_complete = true;
    return finish(arena, error);
  }
  if (result_failed(call.value)) {
    // `account_tx: ${result.error ?? 'error'}`.
    const Value* code = tj::Get(call.value, "error");
    char text[64];
    if (nullish(code)) {
      error->Upstream("account_tx: error");
    } else if (tj::IsString(code)) {
      error->Upstream("account_tx: %.*s", static_cast<int>(code->length < 400 ? code->length : 400), code->text);
    } else if (tj::IsNumber(code)) {
      JsNumberText(code->number, text, sizeof(text));
      error->Upstream("account_tx: %s", text);
    } else if (code->type == Type::kTrue || code->type == Type::kFalse) {
      error->Upstream("account_tx: %s", code->type == Type::kTrue ? "true" : "false");
    } else {
      error->Upstream("account_tx: [object Object]");
    }
    return false;
  }
  const Value* transactions = tj::Get(call.value, "transactions");
  if (!nullish(transactions) && !tj::IsArray(transactions)) {
    error->Upstream("account_tx: transactions is not an array");
    return false;
  }
  Value* records = tj::Arr(arena);
  double max_ledger = min_ledger;
  double validated = 0;
  if (tj::IsArray(transactions)) {
    for (uint32_t i = 0; i < transactions->count; ++i) {
      const Value* entry = transactions->items[i];
      if (tj::Get(entry, "validated") && tj::Get(entry, "validated")->type == Type::kFalse) continue;
      ++validated;
      Value* r = record(arena, network, address, entry);
      if (r) tj::Push(arena, records, r);
      const Value* ledger = tj::Get(tj::Get(entry, "tx"), "ledger_index");
      if (tj::IsNumber(ledger) && ledger->number >= max_ledger) max_ledger = ledger->number + 1;
    }
  }
  out->records = records;
  out->state = min_ledger_state(arena, max_ledger);
  out->backfill_complete = validated < limit;
  return finish(arena, error);
}

namespace {

// ---- detail view ---------------------------------------------------------------------

}  // namespace

bool XrplView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  Arena& arena = *ctx.arena;
  *detail = nullptr;
  Value* params = tj::Arr(arena);
  Value* request = tj::Obj(arena);
  tj::Set(arena, request, "transaction", tj::Str(arena, hash));
  tj::Push(arena, params, request);
  Call call{};
  call.kind = CallKind::kJsonRpc;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.method = "tx";
  call.params = params;
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* result = call.value;
  if (nullish(result) || Equals(tj::Get(result, "status"), "error")) return finish(arena, error);
  const Value* validated = tj::Get(result, "validated");
  if (!validated || validated->type != Type::kTrue) return finish(arena, error);  // not final

  Value* ledger_index = tj::Get(result, "ledger_index");
  Value* sequence = tj::Get(result, "Sequence");
  Value* transaction_type = tj::Get(result, "TransactionType");
  if (!tj::IsNumber(ledger_index) || !tj::IsNumber(sequence) || !is_str(transaction_type))
    return finish(arena, error);

  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "ledgerIndex", ledger_index);
  tj::Set(arena, out, "sequence", sequence);
  const Value* fee = tj::Get(result, "Fee");
  const Value* fee_text = nullish(fee) ? tj::Str(arena, "") : js_string(arena, fee);
  tj::Set(arena, out, "feeDrops",
          fee_text && digits(fee_text->text, fee_text->length) ? const_cast<Value*>(fee_text)
                                                               : tj::Str(arena, "0"));
  tj::Set(arena, out, "transactionType", transaction_type);
  const Value* meta = tj::Get(result, "meta");
  Value* result_text = tj::Str(arena, "");
  if (tj::IsObject(meta) || tj::IsArray(meta)) {
    const Value* code = tj::Get(meta, "TransactionResult");
    if (!nullish(code)) result_text = js_string(arena, code);
  }
  tj::Set(arena, out, "result", result_text);
  tj::Set(arena, out, "raw", result);

  // TrustSet facts feed the app's Trustline section — present only for that
  // transaction type, exactly as the app's parser gates it.
  const Value* limit = tj::Get(result, "LimitAmount");
  if (Equals(transaction_type, "TrustSet") && (tj::IsObject(limit) || tj::IsArray(limit))) {
    Value* currency = tj::Get(limit, "currency");
    Value* issuer = tj::Get(limit, "issuer");
    if (is_str(currency) && is_str(issuer)) {
      const Value* value = tj::Get(limit, "value");
      Value* trust = tj::Obj(arena);
      tj::Set(arena, trust, "currencyRaw", currency);
      tj::Set(arena, trust, "issuer", issuer);
      tj::Set(arena, trust, "limit", nullish(value) ? tj::Str(arena, "") : js_string(arena, value));
      tj::Set(arena, out, "trustSet", trust);
    }
  }
  if (!finish(arena, error)) return false;
  *detail = out;
  return true;
}

namespace {

bool always(const config::ProviderRuntimeConfig&) { return true; }
bool has_rpc(const config::ProviderRuntimeConfig&, const Network& network) { return network.rpc_url_count > 0; }
bool final_view(const Value*, const Network&) { return true; }

}  // namespace

const XrplDriver kXrplDriver{};
const XrplSource kXrplSource{};
const XrplView kXrplView{};


bool XrplDriver::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool XrplSource::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool XrplSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return has_rpc(config, network); }
bool XrplView::IsFinal(const Value* detail, const Network& network) const { return final_view(detail, network); }

}  // namespace fortis::chains
