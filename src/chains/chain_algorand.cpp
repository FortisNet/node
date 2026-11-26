// Algorand family of the address, transaction and network routes: the algod
// balance driver, the indexer transfer source and the indexer transaction
// view, as in the earlier TypeScript implementation. Opt-ins and token
// insight belong to other routes and are not ported here. Guest-safe.
#include "chain_family_drivers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

using tj::Type;

constexpr int kAlgoDecimals = 6;
// MAX_ASSET_LOOKUPS. TypeScript reads all of them at once; natively they run
// in waves of at most kTransportMaxCalls (one Run per wave).
constexpr uint32_t kMaxAssetLookups = 50;

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

bool same(const Value* v, const char* text) {
  return is_str(v) && strlen(text) == v->length && memcmp(v->text, text, v->length) == 0;
}

bool same_value(const Value* a, const Value* b) {
  if (!a || !b) return !a && !b;
  if (a->type != b->type) return false;
  if (a->type == Type::kString) return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
  if (a->type == Type::kNumber) return a->number == b->number;
  return a->type == Type::kNull || a->type == Type::kTrue || a->type == Type::kFalse;
}

// String(number).
Value* number_text(Arena& arena, double value) {
  char text[40];
  JsNumberText(value, text, sizeof(text));
  return tj::Str(arena, text);
}

// String(value) for `String(account.amount ?? 0)`.
Value* js_string(Arena& arena, const Value* v) {
  if (nullish(v)) return tj::Str(arena, "0");
  switch (v->type) {
    case Type::kString: return tj::Str(arena, v->text, v->length);
    case Type::kNumber: return number_text(arena, v->number);
    case Type::kTrue: return tj::Str(arena, "true");
    case Type::kFalse: return tj::Str(arena, "false");
    case Type::kObject: return tj::Str(arena, "[object Object]");
    default: return tj::Str(arena, "");
  }
}

Value* or_null(Arena& arena, Value* v) { return nullish(v) ? tj::Null(arena) : v; }

// /^[A-Z2-7]{58}$/
bool algorand_address(const char* address) {
  if (strlen(address) != 58) return false;
  for (size_t i = 0; i < 58; ++i) {
    const char c = address[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
  }
  return true;
}

bool finish(Arena& arena, Error* error) {
  if (arena.ok) return true;
  error->Upstream("algorand: work arena exhausted");
  return false;
}

Value* native_entry(Arena& arena, Value* raw, const Network& network) {
  Value* native = tj::Obj(arena);
  tj::Set(arena, native, "balanceRaw", raw);
  tj::Set(arena, native, "decimals",
          tj::Num(arena, network.has_native ? network.native_decimals : kAlgoDecimals));
  tj::Set(arena, native, "symbol", tj::Str(arena, network.has_native ? network.native_symbol : "ALGO"));
  tj::Set(arena, native, "source", tj::Str(arena, "algod"));
  return native;
}

// The app's host derivation: mainnet-api.algonode.cloud → mainnet-idx…
// (String.prototype.replace: the first occurrence only).
const char* const* indexer_urls(Arena& arena, const Network& network) {
  auto* urls = static_cast<const char**>(arena.Alloc(sizeof(char*) * (network.rpc_url_count + 1)));
  if (!urls) return nullptr;
  for (size_t i = 0; i < network.rpc_url_count; ++i) {
    const char* url = network.rpc_urls[i];
    const char* at = strstr(url, "-api.");
    // Cppcheck 2.13 misidentifies the Arena-allocated array of char pointers as integers.
    // cppcheck-suppress AssignmentAddressToInteger
    if (!at) { urls[i] = url; continue; }
    Value* replaced = Format(arena, "%.*s-idx.%s", static_cast<int>(at - url), url, at + 5);
    urls[i] = replaced ? replaced->text : url;
  }
  return urls;
}

// ---- driver --------------------------------------------------------------------------

}  // namespace

bool AlgorandDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const {
  if (!algorand_address(address)) {
    error->Range("not an Algorand address: %s", address);
    return false;
  }
  Arena& arena = *ctx.arena;
  const char* encoded = EncodeUri(arena, address);
  Value* path = Format(arena, "/v2/accounts/%s", encoded ? encoded : "");
  Call call{};
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path ? path->text : "/";
  call.lane = "algod";
  ctx.transport->Run(call, arena);
  if (call.answer() && call.http_status == 404) {
    Value* out = tj::Obj(arena);
    tj::Set(arena, out, "native", native_entry(arena, tj::Str(arena, "0"), network));
    tj::Set(arena, out, "tokens", tj::Arr(arena));
    *result = out;
    return finish(arena, error);
  }
  if (!call.ok()) { call.ToError(error); return false; }
  const Value* account = call.value;
  if (nullish(account)) { error->Upstream("algod: null account"); return false; }

  // held: assets with a numeric asset-id and amount, capped.
  const Value* assets = tj::Get(account, "assets");
  if (!nullish(assets) && !tj::IsArray(assets)) {
    error->Upstream("algod: assets is not an array");
    return false;
  }
  const Value* held[kMaxAssetLookups];
  uint32_t held_count = 0;
  if (tj::IsArray(assets)) {
    for (uint32_t i = 0; i < assets->count && held_count < kMaxAssetLookups; ++i) {
      const Value* asset = assets->items[i];
      if (tj::IsNumber(tj::Get(asset, "asset-id")) && tj::IsNumber(tj::Get(asset, "amount")))
        held[held_count++] = asset;
    }
  }

  Value* tokens = tj::Arr(arena);
  for (uint32_t wave = 0; wave < held_count; wave += kTransportMaxCalls) {
    const uint32_t count = held_count - wave < kTransportMaxCalls ? held_count - wave
                                                                  : static_cast<uint32_t>(kTransportMaxCalls);
    auto* calls = static_cast<Call*>(arena.Alloc(sizeof(Call) * count));
    if (!calls) return finish(arena, error);
    for (uint32_t i = 0; i < count; ++i) {
      new (&calls[i]) Call();
      char id[40];
      JsNumberText(tj::Get(held[wave + i], "asset-id")->number, id, sizeof(id));
      Value* asset_path = Format(arena, "/v2/assets/%s", id);
      calls[i].kind = CallKind::kGet;
      calls[i].urls = network.rpc_urls;
      calls[i].url_count = network.rpc_url_count;
      calls[i].path = asset_path ? asset_path->text : "/";
      calls[i].lane = "algod";
    }
    ctx.transport->Run(calls, count, arena);
    for (uint32_t i = 0; i < count; ++i) {
      // One asset's metadata failing must not sink the rest.
      if (!calls[i].ok()) continue;
      const Value* params = tj::Get(calls[i].value, "params");
      const Value* decimals = tj::Get(params, "decimals");
      if (!tj::IsNumber(decimals)) continue;  // never mis-scale
      const Value* unit = tj::Get(params, "unit-name");
      if (truthy(unit) && !is_str(unit)) continue;  // sanitizeSymbol would throw (caught -> null)
      const Value* asset = held[wave + i];
      Value* token = tj::Obj(arena);
      char id[40];
      JsNumberText(tj::Get(asset, "asset-id")->number, id, sizeof(id));
      tj::Set(arena, token, "tokenId", Format(arena, "asa:%s", id));
      tj::Set(arena, token, "balanceRaw", number_text(arena, tj::Get(asset, "amount")->number));
      tj::Set(arena, token, "decimals", tj::Num(arena, decimals->number));
      tj::Set(arena, token, "symbol",
              truthy(unit) ? SanitizeSymbol(arena, unit->text, unit->length) : tj::Null(arena));
      tj::Set(arena, token, "source", tj::Str(arena, "algod"));
      tj::Push(arena, tokens, token);
    }
  }

  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "native", native_entry(arena, js_string(arena, tj::Get(account, "amount")), network));
  tj::Set(arena, out, "tokens", tokens);
  *result = out;
  return finish(arena, error);
}

namespace {

// ---- transfer source ------------------------------------------------------------------

Value* record(Arena& arena, const Network& network, const char* holder, const Value* tx) {
  Value* id = tj::Get(tx, "id");
  const Value* round_time = tj::Get(tx, "round-time");
  if (!truthy(id) || !tj::IsNumber(round_time) || !is_str(id)) return nullptr;
  Value* native = NativeAssetJson(arena, network);
  Value* sender = tj::Get(tx, "sender");
  const Value* fee = tj::Get(tx, "fee");
  const bool pays_fee = same(sender, holder) && tj::IsNumber(fee);
  const Value* note = tj::Get(tx, "note");
  Value* memo = truthy(note) && is_str(note) ? PrintableMemoFromBase64(arena, note->text, note->length)
                                             : tj::Null(arena);
  const char* id_text = tj::CString(arena, id);

  Value* r = NewTransfer(arena);
  tj::Set(arena, r, "status", tj::Str(arena, "confirmed"));  // the indexer only serves confirmed rounds
  tj::Set(arena, r, "ts", tj::Num(arena, trunc(round_time->number * 1000)));
  tj::Set(arena, r, "hash", id);
  tj::Set(arena, r, "blockNumber", or_null(arena, tj::Get(tx, "confirmed-round")));
  tj::Set(arena, r, "fromAddress", or_null(arena, sender));
  tj::Set(arena, r, "feeRaw", pays_fee ? number_text(arena, fee->number) : tj::Null(arena));
  tj::Set(arena, r, "feeDecimals", pays_fee ? tj::Num(arena, kAlgoDecimals) : tj::Null(arena));
  tj::Set(arena, r, "feeSymbol", pays_fee ? tj::Get(native, "symbol") : tj::Null(arena));
  tj::Set(arena, r, "memo", memo);
  tj::Set(arena, r, "error", tj::Null(arena));

  const Value* type = tj::Get(tx, "tx-type");
  const Value* pay = tj::Get(tx, "payment-transaction");
  if (Equals(type, "pay") && truthy(pay)) {
    const Value* amount = tj::Get(pay, "amount");
    Value* receiver = tj::Get(pay, "receiver");
    if (!tj::IsNumber(amount) || !truthy(receiver)) return nullptr;
    const char* direction = DirectionFromFlow(same_value(receiver, tj::Str(arena, holder)), same(sender, holder));
    tj::Set(arena, r, "recordId", TransferRecordId(arena, network.id, id_text, nullptr));
    tj::Set(arena, r, "txType", tj::Str(arena, TypeFromDirection(direction)));
    tj::Set(arena, r, "direction", tj::Str(arena, direction));
    tj::Set(arena, r, "toAddress", receiver);
    tj::Set(arena, r, "counterparty", strcmp(direction, "incoming") == 0 ? or_null(arena, sender) : receiver);
    tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
    tj::Set(arena, r, "asset", native);
    tj::Set(arena, r, "amountRaw", number_text(arena, amount->number));
    tj::Set(arena, r, "amountDecimals", tj::Num(arena, kAlgoDecimals));
    tj::Set(arena, r, "method", tj::Str(arena, "pay"));
    return r;
  }

  const Value* axfer = tj::Get(tx, "asset-transfer-transaction");
  if (Equals(type, "axfer") && truthy(axfer)) {
    const Value* amount = tj::Get(axfer, "amount");
    Value* receiver = tj::Get(axfer, "receiver");
    const Value* asset_id = tj::Get(axfer, "asset-id");
    if (!tj::IsNumber(amount) || !truthy(receiver) || !tj::IsNumber(asset_id)) return nullptr;
    const char* direction = DirectionFromFlow(same_value(receiver, tj::Str(arena, holder)), same(sender, holder));
    // A 0-amount self-axfer is the opt-in idiom — visible as its own kind.
    const bool opt_in = amount->number == 0 && same_value(receiver, sender);
    char asset_text[40];
    JsNumberText(asset_id->number, asset_text, sizeof(asset_text));
    tj::Set(arena, r, "recordId", TransferRecordId(arena, network.id, id_text, asset_text));
    tj::Set(arena, r, "txType", tj::Str(arena, opt_in ? "approve" : TypeFromDirection(direction)));
    tj::Set(arena, r, "direction", tj::Str(arena, direction));
    tj::Set(arena, r, "toAddress", receiver);
    tj::Set(arena, r, "counterparty", strcmp(direction, "incoming") == 0 ? or_null(arena, sender) : receiver);
    tj::Set(arena, r, "tokenId", Format(arena, "asa:%s", asset_text));
    // Decimals are not in the indexer row: the raw amount without an asset
    // json, never mis-scaled.
    tj::Set(arena, r, "amountRaw", number_text(arena, amount->number));
    tj::Set(arena, r, "method", tj::Str(arena, "axfer"));
    return r;
  }

  // appl / keyreg / afrz / acfg — visible, honestly typed.
  tj::Set(arena, r, "recordId", TransferRecordId(arena, network.id, id_text, nullptr));
  tj::Set(arena, r, "txType", tj::Str(arena, "contractCall"));
  tj::Set(arena, r, "direction", tj::Str(arena, same(sender, holder) ? "outgoing" : "incoming"));
  tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
  tj::Set(arena, r, "method", or_null(arena, const_cast<Value*>(type)));
  return r;
}

}  // namespace

bool AlgorandSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const {
  Arena& arena = *ctx.arena;
  const Value* stored = tj::Get(state, "minRound");
  const double min_round = tj::IsNumber(stored) ? stored->number : 0;
  const double limit = fmin(backfill_depth, 500);
  char limit_text[40], round_text[40];
  JsNumberText(limit, limit_text, sizeof(limit_text));
  JsNumberText(min_round, round_text, sizeof(round_text));
  const char* encoded = EncodeUri(arena, address);
  Value* path = Format(arena, "/v2/accounts/%s/transactions?limit=%s%s%s", encoded ? encoded : "",
                       limit_text, min_round > 0 ? "&min-round=" : "", min_round > 0 ? round_text : "");
  Call call{};
  call.kind = CallKind::kGet;
  call.urls = indexer_urls(arena, network);
  call.url_count = network.rpc_url_count;
  call.path = path ? path->text : "/";
  call.lane = "algorand-indexer";
  if (!call.urls) return finish(arena, error);
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  if (nullish(call.value)) { error->Upstream("indexer: null body"); return false; }
  const Value* txs = tj::Get(call.value, "transactions");
  if (!nullish(txs) && !tj::IsArray(txs)) {
    error->Upstream("indexer: transactions is not an array");
    return false;
  }
  Value* records = tj::Arr(arena);
  double max_round = min_round;
  double count = 0;
  if (tj::IsArray(txs)) {
    count = txs->count;
    for (uint32_t i = 0; i < txs->count; ++i) {
      Value* r = record(arena, network, address, txs->items[i]);
      if (r) tj::Push(arena, records, r);
    }
    for (uint32_t i = 0; i < txs->count; ++i) {
      const Value* round = tj::Get(txs->items[i], "confirmed-round");
      if (tj::IsNumber(round) && round->number >= max_round) max_round = round->number + 1;
    }
  }
  Value* next = tj::Obj(arena);
  tj::Set(arena, next, "minRound", tj::Num(arena, max_round));
  out->records = records;
  out->state = next;
  out->backfill_complete = count < limit;
  return finish(arena, error);
}

namespace {

// ---- detail view ---------------------------------------------------------------------

}  // namespace

bool AlgorandView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  Arena& arena = *ctx.arena;
  *detail = nullptr;
  if (network.rpc_url_count == 0) return true;
  const char* encoded = EncodeUri(arena, hash);
  Value* path = Format(arena, "/v2/transactions/%s", encoded ? encoded : "");
  Call call{};
  call.kind = CallKind::kGet;
  call.urls = indexer_urls(arena, network);
  call.url_count = network.rpc_url_count;
  call.path = path ? path->text : "/";
  call.lane = "algorand-indexer";
  if (!call.urls) return finish(arena, error);
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* txn = tj::Get(call.value, "transaction");
  if (nullish(txn)) return finish(arena, error);
  Value* round = tj::Get(txn, "confirmed-round");
  if (!tj::IsNumber(round) || round->number == 0) return finish(arena, error);  // not indexed yet

  const Value* payment = tj::Get(txn, "payment-transaction");
  const Value* transfer = tj::Get(txn, "asset-transfer-transaction");
  Value* out = tj::Obj(arena);
  Value* id = tj::Get(txn, "id");
  tj::Set(arena, out, "id", is_str(id) ? id : tj::Str(arena, hash));
  tj::Set(arena, out, "confirmedRound", round);
  const Value* fee = tj::Get(txn, "fee");
  tj::Set(arena, out, "feeMicroAlgos", tj::IsNumber(fee) ? number_text(arena, fee->number) : tj::Str(arena, "0"));
  tj::Set(arena, out, "raw", txn);
  // put(): only non-null values become members.
  Value* round_time = tj::Get(txn, "round-time");
  if (tj::IsNumber(round_time)) tj::Set(arena, out, "roundTime", round_time);
  Value* type = tj::Get(txn, "tx-type");
  if (is_str(type)) tj::Set(arena, out, "txType", type);
  Value* sender = tj::Get(txn, "sender");
  if (is_str(sender)) tj::Set(arena, out, "sender", sender);
  Value* receiver = tj::Get(payment, "receiver");
  if (!is_str(receiver)) receiver = tj::Get(transfer, "receiver");
  if (is_str(receiver)) tj::Set(arena, out, "receiver", receiver);
  Value* asset_id = tj::Get(transfer, "asset-id");
  if (tj::IsNumber(asset_id)) tj::Set(arena, out, "assetId", asset_id);
  // The note is base64; only PRINTABLE text is surfaced (the app's rule).
  const Value* note = tj::Get(txn, "note");
  if (is_str(note) && note->length) {
    Value* memo = PrintableMemoFromBase64(arena, note->text, note->length);
    if (!nullish(memo)) tj::Set(arena, out, "note", memo);
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

const AlgorandDriver kAlgorandDriver{};
const AlgorandSource kAlgorandSource{};
const AlgorandView kAlgorandView{};


bool AlgorandDriver::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool AlgorandSource::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool AlgorandSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return has_rpc(config, network); }
bool AlgorandView::IsFinal(const Value* detail, const Network& network) const { return final_view(detail, network); }

}  // namespace fortis::chains
