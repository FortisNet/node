#include "chain_family_drivers.h"
#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

// Aptos (the TypeScript AptosDriver and AptosTransferSource): the fullnode
// `0x1::coin::balance` view (4xx = no coin store = zero), and the indexer's
// ascending version list hydrated through `/transactions/by_version/{v}` at
// most eight at a time (as the TypeScript mapWithConcurrency did), with
// per-sync coin / fungible-asset metadata reads. Guest-safe.
namespace fortis::chains {
namespace {

constexpr const char* kGraphqlUrl = "https://api.mainnet.aptoslabs.com/v1/graphql";
constexpr size_t kHydrationConcurrency = 8;
constexpr double kMaxVersionsPerSync = 100;
constexpr const char* kAptTransfer = "0x1::aptos_account::transfer";
constexpr const char* kAptCoin = "0x1::aptos_coin::AptosCoin";

bool digits_only(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

bool hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

// /^\d+$/.test(value ?? '') with String() coercion.
bool regex_digits_or_empty(const Value* v) {
  if (!v || v->type == tj::Type::kNull) return false;  // '' fails \d+
  if (tj::IsString(v)) return digits_only(v->text, v->length);
  if (tj::IsNumber(v)) {
    char text[64];
    JsNumberText(v->number, text, sizeof(text));
    return digits_only(text, strlen(text));
  }
  return false;
}

// BigInt(value) for a value that passed regex_digits_or_empty.
bool to_big(const Value* v, BigInt* out) {
  if (tj::IsString(v)) return BigInt::ParseDecimal(v->text, v->length, out);
  char text[64];
  JsNumberText(v->number, text, sizeof(text));
  return BigInt::ParseDecimal(text, strlen(text), out);
}

// "0x" + hex without leading zeroes, lowercase ("0x0" when all zero).
const char* canonical_address(Arena& a, const char* hex, size_t length) {
  size_t i = 0;
  while (i < length && hex[i] == '0') ++i;
  char buffer[80];
  size_t used = 0;
  buffer[used++] = '0';
  buffer[used++] = 'x';
  if (i == length) buffer[used++] = '0';
  for (; i < length; ++i) {
    const char c = hex[i];
    buffer[used++] = (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  return a.CopyText(buffer, used);
}

enum class IdKind : uint8_t { kNone, kCoin, kFungibleAsset };

// canonicalAptosTokenIdentifier(raw).
IdKind canonical_identifier(Arena& a, const char* raw, size_t raw_length, const char** value) {
  size_t begin = 0, end = 0;
  compat::token_text::Trim(raw, raw_length, &begin, &end);
  const char* s = raw + begin;
  const size_t n = end - begin;
  *value = nullptr;
  if (n < 3 || s[0] != '0' || s[1] != 'x') return IdKind::kNone;
  size_t h = 2;
  while (h < n && hex_char(s[h])) ++h;
  const size_t hex_length = h - 2;
  if (hex_length < 1 || hex_length > 64) return IdKind::kNone;
  if (h == n) {
    *value = canonical_address(a, s + 2, hex_length);
    return *value ? IdKind::kFungibleAsset : IdKind::kNone;
  }
  // ::module::Struct
  if (h + 2 > n || s[h] != ':' || s[h + 1] != ':') return IdKind::kNone;
  size_t m = h + 2;
  if (m >= n || !ident_start(s[m])) return IdKind::kNone;
  size_t me = m + 1;
  while (me < n && ident_char(s[me])) ++me;
  if (me + 2 > n || s[me] != ':' || s[me + 1] != ':') return IdKind::kNone;
  size_t t = me + 2;
  if (t >= n || !ident_start(s[t])) return IdKind::kNone;
  size_t te = t + 1;
  while (te < n && ident_char(s[te])) ++te;
  if (te != n) return IdKind::kNone;
  const char* address = canonical_address(a, s + 2, hex_length);
  const Value* joined = address ? Format(a, "%s::%.*s::%.*s", address, static_cast<int>(me - m), s + m,
                                         static_cast<int>(te - t), s + t)
                                : nullptr;
  if (!joined) return IdKind::kNone;
  *value = joined->text;
  return IdKind::kCoin;
}

// sameAptosAddress(left, right); `*failed` where TypeScript throws
// (a null or non-string left reaches String.prototype.trim).
bool same_address(Arena& a, const Value* left, const char* right, bool* failed) {
  if (!left) return false;
  if (!tj::IsString(left)) { *failed = true; return false; }
  const char* l = nullptr;
  const char* r = nullptr;
  const IdKind lk = canonical_identifier(a, left->text, left->length, &l);
  const IdKind rk = canonical_identifier(a, right, strlen(right), &r);
  return lk == IdKind::kFungibleAsset && rk == IdKind::kFungibleAsset && strcmp(l, r) == 0;
}

// args[i] for any JSON value (arrays by index, objects by the index key,
// strings by UTF-16 code unit — ASCII here).
const Value* index_of(Arena& a, const Value* v, uint32_t i) {
  if (tj::IsArray(v)) return i < v->count ? v->items[i] : nullptr;
  if (tj::IsObject(v)) {
    char key[16];
    snprintf(key, sizeof(key), "%u", i);
    return tj::Get(v, key);
  }
  if (tj::IsString(v)) return i < v->length ? tj::Str(a, v->text + i, 1) : nullptr;
  return nullptr;
}

bool aptos_present(const config::ProviderRuntimeConfig&) { return true; }

}  // namespace

bool AptosDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                    Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  // /^0x[0-9a-fA-F]{1,64}$/
  const size_t length = strlen(address);
  bool valid = length >= 3 && length <= 66 && address[0] == '0' && address[1] == 'x';
  for (size_t i = 2; valid && i < length; ++i) valid = hex_char(address[i]);
  if (!valid) {
    error->Range("not an Aptos address: %s", address);
    return false;
  }
  Value* body = tj::Obj(a);
  Value* type_arguments = tj::Arr(a);
  Value* arguments = tj::Arr(a);
  if (!body || !type_arguments || !arguments || !tj::Push(a, type_arguments, tj::Str(a, kAptCoin)) ||
      !tj::Push(a, arguments, tj::Str(a, address)) ||
      !tj::Set(a, body, "function", tj::Str(a, "0x1::coin::balance")) ||
      !tj::Set(a, body, "type_arguments", type_arguments) || !tj::Set(a, body, "arguments", arguments)) {
    error->Upstream("arena exhausted");
    return false;
  }
  Call call;
  call.kind = CallKind::kPost;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = "/view";
  call.body = body;
  call.retry_post = true;  // a Move view call is an idempotent read
  ctx.transport->Run(call, a);
  Value* raw = tj::Str(a, "0");
  if (call.ok()) {
    if (!call.value || call.value->type == tj::Type::kNull) {
      error->Upstream("aptos view: null result");  // result[0] throws
      return false;
    }
    const Value* value = index_of(a, call.value, 0);
    if (tj::IsString(value) && digits_only(value->text, value->length)) raw = tj::Str(a, value->text, value->length);
  } else if (!call.answer()) {
    call.ToError(error);
    return false;
  }  // 4xx family: the account or its coin store doesn't exist -> zero

  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!raw || !native || !out || !tj::Set(a, native, "balanceRaw", raw) ||
      !tj::Set(a, native, "decimals", tj::Num(a, network.has_native ? network.native_decimals : 8)) ||
      !tj::Set(a, native, "symbol", tj::Str(a, network.has_native ? network.native_symbol : "APT")) ||
      !tj::Set(a, native, "source", tj::Str(a, "aptos-fullnode")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", tj::Arr(a))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

bool aptos_supports(const config::ProviderRuntimeConfig&, const Network& network) {
  return !network.testnet && network.rpc_url_count > 0;
}

bool in_set(const Value* fn, const char* const* names, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (tj::StringEquals(fn, names[i])) return true;
  return false;
}
const char* const kCoinTransfer[] = {"0x1::coin::transfer", "0x1::aptos_account::transfer_coins"};
const char* const kFaTransfer[] = {"0x1::primary_fungible_store::transfer",
                                   "0x1::aptos_account::transfer_fungible_assets"};

// One metadata identifier the sync needs (the TypeScript per-sync cache).
struct Meta {
  const char* identifier;
  bool coin;
  bool fetched;
  bool found;
  const char* symbol;
  const char* name;
  double decimals;
};

// What record() would request: the token identifier of a non-APT coin or a
// fungible-asset transfer, or null.
const char* metadata_target(Arena& a, const Value* tx, bool* coin) {
  const Value* hash = tj::Get(tx, "hash");
  const Value* timestamp = tj::Get(tx, "timestamp");
  const bool truthy_hash = hash && !(hash->type == tj::Type::kNull || hash->type == tj::Type::kFalse ||
                                     (hash->type == tj::Type::kString && hash->length == 0) ||
                                     (hash->type == tj::Type::kNumber && (hash->number == 0 || isnan(hash->number))));
  if (!truthy_hash || !tj::IsString(timestamp)) return nullptr;
  const Value* payload = tj::Get(tx, "payload");
  const Value* fn = tj::Get(payload, "function");
  const Value* args = tj::Get(payload, "arguments");
  if (in_set(fn, kCoinTransfer, 2) && tj::IsString(index_of(a, args, 0))) {
    const Value* type_arg = index_of(a, tj::Get(payload, "type_arguments"), 0);
    const char* value = nullptr;
    if (tj::IsString(type_arg) &&
        canonical_identifier(a, type_arg->text, type_arg->length, &value) == IdKind::kCoin &&
        strcmp(value, kAptCoin) != 0) {
      *coin = true;
      return value;
    }
    return nullptr;
  }
  if (in_set(fn, kFaTransfer, 2)) {
    const Value* metadata_arg = index_of(a, args, 0);
    const char* value = nullptr;
    if (tj::IsString(metadata_arg) &&
        canonical_identifier(a, metadata_arg->text, metadata_arg->length, &value) == IdKind::kFungibleAsset &&
        tj::IsString(index_of(a, args, 1))) {
      *coin = false;
      return value;
    }
  }
  return nullptr;
}

// fetchTokenMetadata's interpretation of one resource body.
void read_metadata(Arena& a, const Call& call, Meta* meta) {
  meta->found = false;
  if (!call.ok() || !call.value || call.value->type == tj::Type::kNull) return;  // throws -> null
  const Value* data = tj::Get(call.value, "data");
  const Value* symbol = tj::Get(data, "symbol");
  const Value* name = tj::Get(data, "name");
  const Value* raw = tj::Get(data, "decimals");
  size_t b = 0, e = 0;
  const char* symbol_text = "";
  size_t symbol_length = 0;
  if (tj::IsString(symbol)) {
    compat::token_text::Trim(symbol->text, symbol->length, &b, &e);
    symbol_text = symbol->text + b;
    symbol_length = e - b;
  }
  const char* name_text = "";
  size_t name_length = 0;
  if (tj::IsString(name)) {
    compat::token_text::Trim(name->text, name->length, &b, &e);
    name_text = name->text + b;
    name_length = e - b;
  }
  double decimals = NAN;
  if (tj::IsNumber(raw)) decimals = raw->number;
  else if (tj::IsString(raw)) decimals = JsNumber(raw);
  if (symbol_length == 0 || !isfinite(decimals) || trunc(decimals) != decimals || decimals < 0 || decimals > 255)
    return;
  meta->symbol = a.CopyText(symbol_text, symbol_length);
  meta->name = name_length ? a.CopyText(name_text, name_length) : meta->symbol;
  meta->decimals = decimals;
  meta->found = meta->symbol && meta->name;
}

Value* asset_of(Arena& a, const Network& network, const Meta& meta) {
  Value* asset = tj::Obj(a);
  if (!asset || !tj::Set(a, asset, "chainId", tj::Str(a, network.id)) ||
      !tj::Set(a, asset, "symbol", tj::Str(a, meta.symbol)) || !tj::Set(a, asset, "name", tj::Str(a, meta.name)) ||
      !tj::Set(a, asset, "decimals", tj::Num(a, meta.decimals)) ||
      !tj::Set(a, asset, "contractAddress", tj::Str(a, meta.identifier)))
    return nullptr;
  return asset;
}

// record(network, holder, tx): false with `error` where TypeScript throws.
bool build_record(Context& ctx, const Network& network, const char* holder, const Value* tx, Meta* metas,
                  size_t meta_count, Value* records, Error* error) {
  Arena& a = *ctx.arena;
  const Value* hash = tj::Get(tx, "hash");
  const Value* timestamp = tj::Get(tx, "timestamp");
  const bool truthy_hash = hash && !(hash->type == tj::Type::kNull || hash->type == tj::Type::kFalse ||
                                     (hash->type == tj::Type::kString && hash->length == 0) ||
                                     (hash->type == tj::Type::kNumber && (hash->number == 0 || isnan(hash->number))));
  if (!truthy_hash || !tj::IsString(timestamp)) return true;
  // new Date(Number(timestamp) / 1000): microseconds -> ms (TimeClip).
  const double ms = JsNumber(timestamp) / 1000;
  if (!isfinite(ms) || fabs(ms) > 8.64e15) {
    error->Upstream("aptos tx: invalid timestamp");  // toISOString throws at upsert
    return false;
  }
  const Value* success = tj::Get(tx, "success");
  const bool failed = success && success->type == tj::Type::kFalse;
  const Value* sender = tj::Get(tx, "sender");
  bool threw = false;
  const bool outgoing = same_address(a, sender, holder, &threw);
  if (threw) { error->Upstream("aptos tx: malformed sender"); return false; }
  const bool pays_fee = outgoing && regex_digits_or_empty(tj::Get(tx, "gas_used")) &&
                        regex_digits_or_empty(tj::Get(tx, "gas_unit_price"));
  char fee_text[200] = {};
  if (pays_fee) {
    BigInt used, price;
    if (!to_big(tj::Get(tx, "gas_used"), &used) || !to_big(tj::Get(tx, "gas_unit_price"), &price) ||
        !used.Mul(price) || !used.ToDecimal(fee_text, sizeof(fee_text))) {
      error->Upstream("aptos fee overflow");
      return false;
    }
  }
  const double native_decimals = network.has_native ? network.native_decimals : 18;
  const char* native_symbol = network.has_native ? network.native_symbol : "";
  const Value* version = tj::Get(tx, "version");
  const Value* payload = tj::Get(tx, "payload");
  const Value* fn_value = tj::Get(payload, "function");
  const Value* args_value = tj::Get(payload, "arguments");
  const Value* fn = fn_value && fn_value->type != tj::Type::kNull ? fn_value : nullptr;
  const Value* args = args_value && args_value->type != tj::Type::kNull ? args_value : nullptr;
  const Value* vm_status = tj::Get(tx, "vm_status");

  Value* r = NewTransfer(a);
  const Value* record_id = Format(a, "%s:%s", network.id, tj::IsString(hash) ? tj::CString(a, hash) : "");
  if (!tj::IsString(hash)) {
    // `${tx.hash}` of a truthy non-string (a number): String(value).
    char text[64];
    JsNumberText(hash->type == tj::Type::kNumber ? hash->number : 0, text, sizeof(text));
    record_id = Format(a, "%s:%s", network.id, hash->type == tj::Type::kNumber ? text : "true");
  }
  auto base = [&]() -> bool {
    return tj::Set(a, r, "status", tj::Str(a, failed ? "failed" : "confirmed")) &&
           tj::Set(a, r, "ts", tj::Num(a, trunc(ms))) && tj::Set(a, r, "hash", tj::Clone(a, hash)) &&
           tj::Set(a, r, "blockNumber", version ? tj::Num(a, JsNumber(version)) : tj::Null(a)) &&
           tj::Set(a, r, "fromAddress", sender ? tj::Clone(a, sender) : tj::Null(a)) &&
           tj::Set(a, r, "feeRaw", pays_fee ? tj::Str(a, fee_text) : tj::Null(a)) &&
           tj::Set(a, r, "feeDecimals", pays_fee ? tj::Num(a, native_decimals) : tj::Null(a)) &&
           tj::Set(a, r, "feeSymbol", pays_fee ? tj::Str(a, native_symbol) : tj::Null(a)) &&
           tj::Set(a, r, "memo", tj::Null(a)) &&
           tj::Set(a, r, "error", failed ? (vm_status && vm_status->type != tj::Type::kNull ? tj::Clone(a, vm_status)
                                                                                           : tj::Str(a, "failed"))
                                         : tj::Null(a)) &&
           tj::Set(a, r, "metadata", tj::Obj(a));
  };
  // transferRecord(): a holder-relative transfer of one token.
  auto transfer = [&](const Value* to, const Value* raw_amount, const char* token_id, Value* asset,
                      const Value* decimals, const char* method) -> bool {
    bool bad = false;
    const bool incoming = same_address(a, to, holder, &bad);
    if (bad) { error->Upstream("aptos tx: malformed recipient"); return false; }
    const char* direction = DirectionFromFlow(incoming, outgoing);
    const bool amount_ok = tj::IsString(raw_amount) && digits_only(raw_amount->text, raw_amount->length);
    const bool is_incoming = strcmp(direction, "incoming") == 0;
    const bool ok =
        r && record_id && tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) &&
        tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection(direction))) &&
        tj::Set(a, r, "direction", tj::Str(a, direction)) && tj::Set(a, r, "toAddress", tj::Clone(a, to)) &&
        tj::Set(a, r, "counterparty",
                is_incoming ? (sender ? tj::Clone(a, sender) : tj::Null(a)) : tj::Clone(a, to)) &&
        tj::Set(a, r, "tokenId", tj::Str(a, token_id)) && tj::Set(a, r, "asset", asset ? asset : tj::Null(a)) &&
        tj::Set(a, r, "amountRaw", amount_ok ? tj::Clone(a, raw_amount) : tj::Null(a)) &&
        tj::Set(a, r, "amountDecimals",
                amount_ok && decimals ? tj::Clone(a, decimals) : tj::Null(a)) &&
        tj::Set(a, r, "method", tj::Str(a, method)) && base() && tj::Push(a, records, r);
    if (!ok) error->Upstream("arena exhausted");
    return ok;
  };
  auto meta_for = [&](const char* identifier) -> const Meta* {
    for (size_t i = 0; i < meta_count; ++i)
      if (strcmp(metas[i].identifier, identifier) == 0) return metas[i].found ? &metas[i] : nullptr;
    return nullptr;
  };

  const char* fn_text = tj::IsString(fn) ? tj::CString(a, fn) : nullptr;
  const Value* arg0 = index_of(a, args, 0);
  if (tj::StringEquals(fn, kAptTransfer) && tj::IsString(arg0)) {
    return transfer(arg0, index_of(a, args, 1), "native", NativeAssetJson(a, network),
                    tj::Num(a, native_decimals), fn_text);
  }
  if (in_set(fn, kCoinTransfer, 2) && tj::IsString(arg0)) {
    const Value* type_arg = index_of(a, tj::Get(payload, "type_arguments"), 0);
    const char* value = nullptr;
    if (tj::IsString(type_arg) &&
        canonical_identifier(a, type_arg->text, type_arg->length, &value) == IdKind::kCoin) {
      if (strcmp(value, kAptCoin) == 0)
        return transfer(arg0, index_of(a, args, 1), "native", NativeAssetJson(a, network),
                        tj::Num(a, native_decimals), fn_text);
      const Meta* meta = meta_for(value);
      const Value* token_id = Format(a, "aptos:%s", value);
      return transfer(arg0, index_of(a, args, 1), token_id ? token_id->text : "",
                      meta ? asset_of(a, network, *meta) : nullptr,
                      meta ? tj::Num(a, meta->decimals) : nullptr, fn_text);
    }
  }
  if (in_set(fn, kFaTransfer, 2)) {
    const Value* metadata_arg = arg0;
    const Value* to = index_of(a, args, 1);
    const char* value = nullptr;
    if (tj::IsString(metadata_arg) &&
        canonical_identifier(a, metadata_arg->text, metadata_arg->length, &value) == IdKind::kFungibleAsset &&
        tj::IsString(to)) {
      const Meta* meta = meta_for(value);
      const Value* token_id = Format(a, "aptos:%s", value);
      return transfer(to, index_of(a, args, 2), token_id ? token_id->text : "",
                      meta ? asset_of(a, network, *meta) : nullptr,
                      meta ? tj::Num(a, meta->decimals) : nullptr, fn_text);
    }
  }
  // Anything else the account touched — visible, honestly typed.
  const bool ok = r && record_id && tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) &&
                  tj::Set(a, r, "txType", tj::Str(a, "contractCall")) &&
                  tj::Set(a, r, "direction", tj::Str(a, outgoing ? "outgoing" : "incoming")) &&
                  tj::Set(a, r, "toAddress", tj::Null(a)) && tj::Set(a, r, "counterparty", tj::Null(a)) &&
                  tj::Set(a, r, "tokenId", tj::Str(a, "native")) && tj::Set(a, r, "asset", tj::Null(a)) &&
                  tj::Set(a, r, "amountRaw", tj::Null(a)) && tj::Set(a, r, "amountDecimals", tj::Null(a)) &&
                  tj::Set(a, r, "method", fn ? tj::Clone(a, fn) : tj::Null(a)) && base() &&
                  tj::Push(a, records, r);
  if (!ok) error->Upstream("arena exhausted");
  return ok;
}

}  // namespace

bool AptosSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                 double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* min_value = tj::Get(state, "minVersion");
  const double min_version = tj::IsNumber(min_value) ? min_value->number : 0;
  const double limit = backfill_depth < kMaxVersionsPerSync ? backfill_depth : kMaxVersionsPerSync;

  Value* body = tj::Obj(a);
  Value* variables = tj::Obj(a);
  static const char kQuery[] =
      "query ($addr: String!, $gt: bigint!, $limit: Int!) {"
      " account_transactions(where: {account_address: {_eq: $addr},"
      " transaction_version: {_gt: $gt}},"
      " order_by: {transaction_version: asc}, limit: $limit)"
      " { transaction_version } }";
  if (!body || !variables || !tj::Set(a, variables, "addr", tj::Str(a, address)) ||
      !tj::Set(a, variables, "gt", tj::Num(a, min_version)) || !tj::Set(a, variables, "limit", tj::Num(a, limit)) ||
      !tj::Set(a, body, "query", tj::Str(a, kQuery)) || !tj::Set(a, body, "variables", variables)) {
    error->Upstream("arena exhausted");
    return false;
  }
  const char* const graphql[] = {kGraphqlUrl};
  Call gql;
  gql.kind = CallKind::kPost;
  gql.urls = graphql;
  gql.url_count = 1;
  gql.path = "";
  gql.body = body;
  gql.lane = "rpc:api.mainnet.aptoslabs.com";
  ctx.transport->Run(gql, a);
  if (!gql.ok()) { gql.ToError(error); return false; }
  if (!gql.value || gql.value->type == tj::Type::kNull) {
    error->Upstream("aptos indexer: null body");  // gql.data throws
    return false;
  }
  const Value* rows = tj::Get(tj::Get(gql.value, "data"), "account_transactions");
  if (rows && rows->type != tj::Type::kNull && !tj::IsArray(rows)) {
    error->Upstream("aptos indexer: account_transactions is not an array");  // .map throws
    return false;
  }
  double versions[256];
  size_t version_count = 0;
  if (tj::IsArray(rows)) {
    for (uint32_t i = 0; i < rows->count; ++i) {
      const Value* row = rows->items[i];
      if (!row || row->type == tj::Type::kNull) {
        error->Upstream("aptos indexer: null row");
        return false;
      }
      const Value* v = tj::Get(row, "transaction_version");
      if (!tj::IsNumber(v)) continue;
      if (version_count == sizeof(versions) / sizeof(versions[0])) {
        // Bounded by the request's limit (<= 100); a provider ignoring it is
        // refused instead of truncated.
        error->Upstream("aptos indexer: too many versions");
        return false;
      }
      versions[version_count++] = v->number;
    }
  }

  // The bounded hydration: eight in flight, never more.
  const char* fullnode = network.rpc_urls[0];
  const size_t fullnode_length = strlen(fullnode);
  const bool trailing_slash = fullnode_length && fullnode[fullnode_length - 1] == '/';
  const char* const bases[] = {fullnode};
  Value** txs = static_cast<Value**>(a.Alloc(sizeof(Value*) * (version_count ? version_count : 1)));
  if (!txs) { error->Upstream("arena exhausted"); return false; }
  for (size_t start = 0; start < version_count; start += kHydrationConcurrency) {
    const size_t n = version_count - start < kHydrationConcurrency ? version_count - start : kHydrationConcurrency;
    Call calls[kHydrationConcurrency];
    for (size_t i = 0; i < n; ++i) {
      char number[48];
      JsNumberText(versions[start + i], number, sizeof(number));
      // `${fullnode}/transactions/...`: joinUrl would drop a trailing "/" the
      // TypeScript template keeps, so put it back into the path.
      const Value* path = Format(a, "%s/transactions/by_version/%s", trailing_slash ? "/" : "", number);
      if (!path) { error->Upstream("arena exhausted"); return false; }
      calls[i].kind = CallKind::kGet;
      calls[i].urls = bases;
      calls[i].url_count = 1;
      calls[i].path = path->text;
    }
    ctx.transport->Run(calls, n, a);
    for (size_t i = 0; i < n; ++i) {
      // One version failing must not sink the sync.
      txs[start + i] = calls[i].ok() && calls[i].value && calls[i].value->type != tj::Type::kNull
                           ? calls[i].value : nullptr;
    }
  }

  // The per-sync metadata cache: each identifier is read once, concurrently.
  Meta* metas = static_cast<Meta*>(a.Alloc(sizeof(Meta) * (version_count ? version_count : 1)));
  size_t meta_count = 0;
  if (!metas) { error->Upstream("arena exhausted"); return false; }
  for (size_t i = 0; i < version_count; ++i) {
    if (!txs[i]) continue;
    bool coin = false;
    const char* identifier = metadata_target(a, txs[i], &coin);
    if (!identifier) continue;
    bool seen = false;
    for (size_t m = 0; m < meta_count && !seen; ++m) seen = strcmp(metas[m].identifier, identifier) == 0;
    if (seen) continue;
    metas[meta_count++] = Meta{identifier, coin, false, false, nullptr, nullptr, 0};
  }
  // fullnode.replace(/\/$/, '') — the base the transport joins equals it.
  for (size_t start = 0; start < meta_count; start += kTransportMaxCalls) {
    const size_t n = meta_count - start < kTransportMaxCalls ? meta_count - start : kTransportMaxCalls;
    Call* calls = static_cast<Call*>(a.Alloc(sizeof(Call) * n));
    if (!calls) { error->Upstream("arena exhausted"); return false; }
    for (size_t i = 0; i < n; ++i) {
      new (&calls[i]) Call();
      Meta& meta = metas[start + i];
      const char* split = strstr(meta.identifier, "::");
      const size_t owner_length = split ? static_cast<size_t>(split - meta.identifier) : strlen(meta.identifier);
      const Value* resource = meta.coin ? Format(a, "0x1::coin::CoinInfo<%s>", meta.identifier)
                                        : Format(a, "0x1::fungible_asset::Metadata");
      const char* escaped = resource ? EncodeUri(a, resource->text) : nullptr;
      const Value* path = escaped ? Format(a, "/accounts/%.*s/resource/%s", static_cast<int>(owner_length),
                                           meta.identifier, escaped)
                                  : nullptr;
      if (!path) { error->Upstream("arena exhausted"); return false; }
      calls[i].kind = CallKind::kGet;
      calls[i].urls = bases;
      calls[i].url_count = 1;
      calls[i].path = path->text;
    }
    ctx.transport->Run(calls, n, a);
    for (size_t i = 0; i < n; ++i) {
      metas[start + i].fetched = true;
      read_metadata(a, calls[i], &metas[start + i]);
    }
  }

  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("arena exhausted"); return false; }
  for (size_t i = 0; i < version_count; ++i) {
    if (!txs[i]) continue;
    if (!build_record(ctx, network, address, txs[i], metas, meta_count, records, error)) return false;
  }

  double max_version = min_version;
  for (size_t i = 0; i < version_count; ++i)
    if (i == 0 || versions[i] > max_version) max_version = versions[i];
  if (version_count == 0) max_version = min_version;
  Value* next = tj::Obj(a);
  if (!next || !tj::Set(a, next, "minVersion", tj::Num(a, max_version))) {
    error->Upstream("arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = version_count < limit;
  return true;
}

const AptosDriver kAptosDriver{};

const AptosSource kAptosSource{};


bool AptosDriver::Registered(const config::ProviderRuntimeConfig& config) const { return aptos_present(config); }
bool AptosSource::Registered(const config::ProviderRuntimeConfig& config) const { return aptos_present(config); }
bool AptosSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return aptos_supports(config, network); }

}  // namespace fortis::chains
