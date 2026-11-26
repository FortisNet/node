// Stellar family of the address, transaction and network routes: the
// Horizon balance driver, the payments transfer source and the transaction
// view, as in the earlier TypeScript implementation. Trustlines, holder
// counts and token insight belong to other routes and are not ported here.
// Guest-safe.
#include "chain_family_drivers.h"

#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

using tj::Type;

constexpr int kStellarDecimals = 7;

bool is_str(const Value* v) { return v && v->type == Type::kString; }
bool nullish(const Value* v) { return !v || v->type == Type::kNull; }

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

bool same(const Value* v, const char* text) {
  return is_str(v) && strlen(text) == v->length && memcmp(v->text, text, v->length) == 0;
}

// String(value) for the scalar shapes providers send.
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
    case Type::kArray: return tj::Str(arena, "");  // only scalars are expected here
  }
  return nullptr;
}

// `value ?? ''` then String().
Value* string_or_empty(Arena& arena, const Value* v) {
  return nullish(v) ? tj::Str(arena, "") : js_string(arena, v);
}

// /^G[A-Z2-7]{55}$/
bool stellar_address(const char* address) {
  if (strlen(address) != 56 || address[0] != 'G') return false;
  for (size_t i = 1; i < 56; ++i) {
    const char c = address[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
  }
  return true;
}

bool finish(Arena& arena, Error* error) {
  if (arena.ok) return true;
  error->Upstream("stellar: work arena exhausted");
  return false;
}

Value* native_entry(Arena& arena, Value* raw, const Network& network) {
  Value* native = tj::Obj(arena);
  tj::Set(arena, native, "balanceRaw", raw);
  tj::Set(arena, native, "decimals",
          tj::Num(arena, network.has_native ? network.native_decimals : kStellarDecimals));
  tj::Set(arena, native, "symbol", tj::Str(arena, network.has_native ? network.native_symbol : "XLM"));
  tj::Set(arena, native, "source", tj::Str(arena, "horizon"));
  return native;
}

// GET /accounts/{address} (or a sub-path) on the network's Horizon list.
void horizon_get(Call& call, const Network& network, const char* path) {
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path;
  call.lane = "horizon";
}

// ---- driver --------------------------------------------------------------------------

}  // namespace

bool StellarDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const {
  if (!stellar_address(address)) {
    error->Range("not a Stellar address: %s", address);
    return false;
  }
  Arena& arena = *ctx.arena;
  const char* encoded = EncodeUri(arena, address);
  Value* path = Format(arena, "/accounts/%s", encoded ? encoded : "");
  Call call{};
  horizon_get(call, network, path ? path->text : "/");
  ctx.transport->Run(call, arena);
  if (call.answer() && call.http_status == 404) {
    // Unfunded account — a real zero, the one case zero is honest.
    Value* out = tj::Obj(arena);
    tj::Set(arena, out, "native", native_entry(arena, tj::Str(arena, "0"), network));
    tj::Set(arena, out, "tokens", tj::Arr(arena));
    *result = out;
    return finish(arena, error);
  }
  if (!call.ok()) { call.ToError(error); return false; }
  if (nullish(call.value)) { error->Upstream("horizon: null account"); return false; }

  BigInt native_raw;
  Value* tokens = tj::Arr(arena);
  const Value* entries = tj::Get(call.value, "balances");
  if (!nullish(entries) && !tj::IsArray(entries)) {
    error->Upstream("horizon: balances is not an array");
    return false;
  }
  if (tj::IsArray(entries)) {
    for (uint32_t i = 0; i < entries->count; ++i) {
      const Value* entry = entries->items[i];
      const Value* balance = tj::Get(entry, "balance");
      BigInt raw;
      int decimals = 0;
      // A non-string balance is treated as unparseable (omitted).
      if (!is_str(balance) ||
          !DecimalToRaw(balance->text, balance->length, kStellarDecimals, &raw, &decimals))
        continue;  // unparseable — omit, never guess
      const Value* type = tj::Get(entry, "asset_type");
      if (Equals(type, "native")) {
        native_raw = raw;
        continue;
      }
      if (!Equals(type, "credit_alphanum4") && !Equals(type, "credit_alphanum12")) continue;
      const Value* code = tj::Get(entry, "asset_code");
      const Value* issuer = tj::Get(entry, "asset_issuer");
      if (!truthy(code) || !truthy(issuer) || !is_str(code) || !is_str(issuer)) continue;
      Value* token = tj::Obj(arena);
      tj::Set(arena, token, "tokenId",
              Format(arena, "iou:%.*s:%.*s", static_cast<int>(issuer->length), issuer->text,
                     static_cast<int>(code->length), code->text));
      tj::Set(arena, token, "balanceRaw", raw.ToValue(arena));
      tj::Set(arena, token, "decimals", tj::Num(arena, decimals));
      tj::Set(arena, token, "symbol", SanitizeSymbol(arena, code->text, code->length));
      tj::Set(arena, token, "source", tj::Str(arena, "horizon"));
      tj::Push(arena, tokens, token);
    }
  }
  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "native", native_entry(arena, native_raw.ToValue(arena), network));
  tj::Set(arena, out, "tokens", tokens);
  *result = out;
  return finish(arena, error);
}

namespace {

// ---- transfer source ------------------------------------------------------------------

// StellarTransferSource.record(): one payment row, or null. `*bad_time` is
// set for a created_at Date cannot parse: TypeScript stores an Invalid Date
// and its ingest then fails on serialization, so the fetch fails here.
Value* record(Arena& arena, const Network& network, const char* holder, const Value* p, bool* bad_time) {
  const Value* id = tj::Get(p, "id");
  const Value* hash = tj::Get(p, "transaction_hash");
  const Value* created_at = tj::Get(p, "created_at");
  if (!truthy(id) || !truthy(hash) || !truthy(created_at)) return nullptr;
  Value* native = NativeAssetJson(arena, network);

  // create_account IS a payment: the funder seeded starting_balance.
  const Value* type = tj::Get(p, "type");
  const bool is_create = Equals(type, "create_account");
  if (!is_create && !Equals(type, "payment")) return nullptr;  // path payments etc. = P2 tail
  Value* from = tj::Get(p, is_create ? "funder" : "from");
  Value* to = tj::Get(p, is_create ? "account" : "to");
  const Value* raw_amount = tj::Get(p, is_create ? "starting_balance" : "amount");
  BigInt amount;
  int amount_decimals = 0;
  // A non-string amount is refused (TypeScript would throw a TypeError).
  if (!nullish(raw_amount) && !is_str(raw_amount)) return nullptr;
  if (!is_str(raw_amount) ||
      !DecimalToRaw(raw_amount->text, raw_amount->length, kStellarDecimals, &amount, &amount_decimals))
    return nullptr;
  if (!truthy(from) || !truthy(to)) return nullptr;

  const char* direction = DirectionFromFlow(same(to, holder), same(from, holder));
  const Value* successful = tj::Get(p, "transaction_successful");
  const bool failed = successful && successful->type == Type::kFalse;
  const Value* transaction = tj::Get(p, "transaction");
  Value* fee_charged = string_or_empty(arena, tj::Get(transaction, "fee_charged"));
  const bool pays_fee = same(tj::Get(transaction, "source_account"), holder) && fee_charged &&
                        digits(fee_charged->text, fee_charged->length);

  const bool is_native = is_create || Equals(tj::Get(p, "asset_type"), "native");
  const Value* code = tj::Get(p, "asset_code");
  const Value* issuer = tj::Get(p, "asset_issuer");
  Value* asset = nullptr;
  if (is_native) {
    asset = native;
  } else if (truthy(code) && truthy(issuer) && is_str(code) && is_str(issuer)) {
    asset = tj::Obj(arena);
    tj::Set(arena, asset, "chainId", tj::Str(arena, network.id));
    tj::Set(arena, asset, "symbol", SanitizeSymbol(arena, code->text, code->length));
    tj::Set(arena, asset, "name", SanitizeSymbol(arena, code->text, code->length));
    tj::Set(arena, asset, "decimals", tj::Num(arena, kStellarDecimals));
    tj::Set(arena, asset, "contractAddress", const_cast<Value*>(issuer));
  }
  if (!asset) return nullptr;

  double ts = 0;
  if (!is_str(created_at) || !ParseTime(created_at->text, created_at->length, &ts)) {
    *bad_time = true;
    return nullptr;
  }
  const Value* hash_text = js_string(arena, hash);
  const Value* id_text = js_string(arena, id);
  Value* record_id = hash_text && id_text
      ? TransferRecordId(arena, network.id, tj::CString(arena, hash_text), tj::CString(arena, id_text))
      : nullptr;
  if (!record_id) return nullptr;

  Value* r = NewTransfer(arena);
  tj::Set(arena, r, "recordId", record_id);
  tj::Set(arena, r, "txType", tj::Str(arena, TypeFromDirection(direction)));
  tj::Set(arena, r, "status", tj::Str(arena, failed ? "failed" : "confirmed"));
  tj::Set(arena, r, "direction", tj::Str(arena, direction));
  tj::Set(arena, r, "ts", tj::Num(arena, ts));
  tj::Set(arena, r, "hash", const_cast<Value*>(hash));
  tj::Set(arena, r, "blockNumber", tj::Null(arena));
  tj::Set(arena, r, "fromAddress", from);
  tj::Set(arena, r, "toAddress", to);
  tj::Set(arena, r, "counterparty", strcmp(direction, "incoming") == 0 ? from : to);
  if (is_native) {
    tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
  } else {
    tj::Set(arena, r, "tokenId",
            Format(arena, "iou:%.*s:%.*s", static_cast<int>(issuer->length), issuer->text,
                   static_cast<int>(code->length), code->text));
  }
  tj::Set(arena, r, "asset", asset);
  tj::Set(arena, r, "amountRaw", amount.ToValue(arena));
  tj::Set(arena, r, "amountDecimals", tj::Num(arena, kStellarDecimals));
  tj::Set(arena, r, "feeRaw", pays_fee ? fee_charged : tj::Null(arena));
  tj::Set(arena, r, "feeDecimals", pays_fee ? tj::Num(arena, kStellarDecimals) : tj::Null(arena));
  tj::Set(arena, r, "feeSymbol", pays_fee ? tj::Get(native, "symbol") : tj::Null(arena));
  Value* memo = tj::Get(transaction, "memo");
  tj::Set(arena, r, "memo", nullish(memo) ? tj::Null(arena) : memo);
  tj::Set(arena, r, "method", nullish(type) ? tj::Null(arena) : const_cast<Value*>(type));
  tj::Set(arena, r, "error", failed ? tj::Str(arena, "transaction failed") : tj::Null(arena));
  return r;
}

Value* token_state(Arena& arena, Value* token) {
  Value* state = tj::Obj(arena);
  tj::Set(arena, state, "pagingToken", token);
  return state;
}

}  // namespace

bool StellarSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const {
  Arena& arena = *ctx.arena;
  const Value* stored = tj::Get(state, "pagingToken");
  const char* cursor = is_str(stored) ? tj::CString(arena, stored) : "";
  if (!cursor) cursor = "";
  const double limit = fmin(backfill_depth, 200);
  char limit_text[40];
  JsNumberText(limit, limit_text, sizeof(limit_text));
  const char* encoded = EncodeUri(arena, address);
  Value* path = Format(arena, "/accounts/%s/payments?order=asc&limit=%s&join=transactions%s%s",
                       encoded ? encoded : "", limit_text, cursor[0] ? "&cursor=" : "", cursor);
  Call call{};
  horizon_get(call, network, path ? path->text : "/");
  ctx.transport->Run(call, arena);
  if (call.answer() && call.http_status == 404) {
    out->records = tj::Arr(arena);
    out->state = token_state(arena, tj::Str(arena, cursor));
    out->backfill_complete = true;
    return finish(arena, error);
  }
  if (!call.ok()) { call.ToError(error); return false; }
  if (nullish(call.value)) { error->Upstream("horizon: null payments body"); return false; }
  const Value* payments = tj::Get(tj::Get(call.value, "_embedded"), "records");
  if (!nullish(payments) && !tj::IsArray(payments)) {
    error->Upstream("horizon: records is not an array");
    return false;
  }
  Value* records = tj::Arr(arena);
  double count = 0;
  Value* last_token = nullptr;
  if (tj::IsArray(payments)) {
    count = payments->count;
    for (uint32_t i = 0; i < payments->count; ++i) {
      bool bad_time = false;
      Value* r = record(arena, network, address, payments->items[i], &bad_time);
      if (bad_time) {
        error->Upstream("horizon: invalid created_at");
        return false;
      }
      if (r) tj::Push(arena, records, r);
    }
    if (payments->count) last_token = tj::Get(payments->items[payments->count - 1], "paging_token");
  }
  out->records = records;
  out->state = token_state(arena, nullish(last_token) ? tj::Str(arena, cursor) : last_token);
  out->backfill_complete = count < limit;
  return finish(arena, error);
}

namespace {

// ---- detail view ---------------------------------------------------------------------

}  // namespace

bool StellarView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  Arena& arena = *ctx.arena;
  *detail = nullptr;
  const char* encoded = EncodeUri(arena, hash);
  Value* path = Format(arena, "/transactions/%s", encoded ? encoded : "");
  Call call{};
  horizon_get(call, network, path ? path->text : "/");
  ctx.transport->Run(call, arena);
  // Horizon 404s an unknown hash; TypeScript catches EVERY failure here and
  // answers "not known yet".
  if (!call.ok() || nullish(call.value)) return finish(arena, error);
  Value* body = call.value;

  const double ledger = JsNumber(tj::Get(body, "ledger"));
  if (!isfinite(ledger) || ledger <= 0) return finish(arena, error);

  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "ledger", tj::Num(arena, ledger));
  Value* fee = string_or_empty(arena, tj::Get(body, "fee_charged"));
  tj::Set(arena, out, "feeChargedStroops",
          fee && digits(fee->text, fee->length) ? fee : tj::Str(arena, "0"));
  tj::Set(arena, out, "sourceSequence", string_or_empty(arena, tj::Get(body, "source_account_sequence")));
  const Value* successful = tj::Get(body, "successful");
  tj::Set(arena, out, "successful", tj::Bool(arena, successful && successful->type == Type::kTrue));
  tj::Set(arena, out, "raw", body);
  // `memo_type: 'none'` means there is no memo — not a memo of "none".
  Value* memo_type = string_or_empty(arena, tj::Get(body, "memo_type"));
  const Value* memo = tj::Get(body, "memo");
  if (memo_type && memo_type->length && !Equals(memo_type, "none") && !nullish(memo)) {
    const Value* text = js_string(arena, memo);
    if (text) {
      size_t begin = 0, end = 0;
      fortis::compat::token_text::Trim(text->text, text->length, &begin, &end);
      if (end > begin) tj::Set(arena, out, "memo", tj::Str(arena, text->text + begin, end - begin));
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

const StellarDriver kStellarDriver{};
const StellarSource kStellarSource{};
const StellarView kStellarView{};


bool StellarDriver::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool StellarSource::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool StellarSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return has_rpc(config, network); }
bool StellarView::IsFinal(const Value* detail, const Network& network) const { return final_view(detail, network); }

}  // namespace fortis::chains
