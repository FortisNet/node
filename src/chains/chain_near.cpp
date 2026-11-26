// NEAR: the TypeScript NEAR driver (balances) and its NearBlocks transfer
// source (account history), ported for the address routes.
#include "chain_family_drivers.h"

#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

constexpr size_t kMaxTokens = 20;
constexpr const char* kBase = "https://api.nearblocks.io";
constexpr double kPageLimit = 25;
constexpr double kMaxPages = 4;
constexpr const char* kSystemAccount = "system";  // validator reward payer
const char* const kBaseUrls[] = {kBase};

// Arena storage for a batch of calls (too large for a worker stack).
Call* new_calls(Arena& arena, size_t count) {
  auto* calls = static_cast<Call*>(arena.Alloc(sizeof(Call) * count));
  if (!calls) return nullptr;
  for (size_t i = 0; i < count; ++i) new (&calls[i]) Call();
  return calls;
}

bool is_string(const Value* v, const char* text) { return Equals(v, text); }

// /^[a-z0-9._-]{2,64}$/
bool near_account(const char* text, size_t length) {
  if (length < 2 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// /^[0-9a-f]{64}$/ (an implicit account).
bool implicit_account(const char* text, size_t length) {
  if (length != 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// /^\d+$/ on a string value.
bool digit_string(const Value* v) {
  if (!tj::IsString(v) || v->length == 0) return false;
  for (uint32_t i = 0; i < v->length; ++i)
    if (v->text[i] < '0' || v->text[i] > '9') return false;
  return true;
}

// JavaScript truthiness of a JSON value (undefined = nullptr is falsy).
bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull: case tj::Type::kFalse: return false;
    case tj::Type::kTrue: return true;
    case tj::Type::kNumber: return v->number != 0 && !isnan(v->number);
    case tj::Type::kString: return v->length != 0;
    default: return true;
  }
}

// One call_function view: jsonRpc 'query' with args_base64 of
// JSON.stringify(args).
bool view_call(Arena& arena, const Network& network, const char* contract, const char* method,
               const char* address, Call* call) {
  Value* args = tj::Obj(arena);
  if (!args || (address && !tj::Set(arena, args, "account_id", tj::Str(arena, address)))) return false;
  uint8_t json[512];
  OutBuffer out{json, sizeof(json), 0, true};
  if (!tj::Stringify(args, out) || !out.ok) return false;
  char base64[720];
  if (!Base64Encode(json, out.used, false, true, base64, sizeof(base64))) return false;
  Value* params = tj::Obj(arena);
  if (!params || !tj::Set(arena, params, "request_type", tj::Str(arena, "call_function")) ||
      !tj::Set(arena, params, "finality", tj::Str(arena, "final")) ||
      !tj::Set(arena, params, "account_id", tj::Str(arena, contract)) ||
      !tj::Set(arena, params, "method_name", tj::Str(arena, method)) ||
      !tj::Set(arena, params, "args_base64", tj::Str(arena, base64)))
    return false;
  call->kind = CallKind::kJsonRpc;
  call->urls = network.rpc_urls;
  call->url_count = network.rpc_url_count;
  call->method = "query";
  call->params = params;
  return true;
}

// viewJson's decode: `res.result` (a byte array of a JSON payload) parsed;
// `*parsed` null when the payload is absent or not JSON. False where the
// TypeScript property read throws (a null result).
bool view_json(Arena& arena, const Call& call, Value** parsed) {
  *parsed = nullptr;
  const Value* res = call.value;
  if (!res || res->type == tj::Type::kNull) return false;
  const Value* bytes = tj::Get(res, "result");
  if (!tj::IsArray(bytes)) return true;
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(bytes->count ? bytes->count : 1, 1));
  if (!buffer) return false;
  for (uint32_t i = 0; i < bytes->count; ++i) {
    // Buffer.from(array): each element is ToNumber(x) & 255.
    const double d = JsNumber(bytes->items[i]);
    double t = isfinite(d) ? trunc(d) : 0;
    t = fmod(t, 256);
    if (t < 0) t += 256;
    buffer[i] = static_cast<uint8_t>(t);
  }
  *parsed = tj::Parse(arena, buffer, bytes->count);
  return true;
}

}  // namespace

bool NearDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                   size_t token_count, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  const size_t length = strlen(address);
  if (!near_account(address, length) && !implicit_account(address, length)) {
    error->Range("not a NEAR account: %s", address);
    return false;
  }
  const char* symbol = network.has_native ? network.native_symbol : "NEAR";
  const uint32_t decimals = network.has_native ? network.native_decimals : 24;

  const char* amount = "0";
  {
    Value* params = tj::Obj(a);
    if (!params || !tj::Set(a, params, "request_type", tj::Str(a, "view_account")) ||
        !tj::Set(a, params, "finality", tj::Str(a, "final")) ||
        !tj::Set(a, params, "account_id", tj::Str(a, address))) {
      error->Upstream("near: arena exhausted");
      return false;
    }
    Call call;
    call.kind = CallKind::kJsonRpc;
    call.urls = network.rpc_urls;
    call.url_count = network.rpc_url_count;
    call.method = "query";
    call.params = params;
    ctx.transport->Run(call, a);
    if (call.ok()) {
      const Value* value = tj::Get(call.value, "amount");
      if (digit_string(value)) amount = tj::CString(a, value);
    } else if (!(call.answer() && strstr(call.message, "UNKNOWN_ACCOUNT"))) {
      // A named account never created is a real zero, not an outage.
      call.ToError(error);
      return false;
    }
    if (!amount) { error->Upstream("near: arena exhausted"); return false; }
  }

  // nep141:{contract} ids, first 20.
  const char* contracts[kMaxTokens];
  size_t contract_count = 0;
  for (size_t i = 0; i < token_count && contract_count < kMaxTokens; ++i) {
    const char* t = tokens[i];
    if (strncmp(t, "nep141:", 7) != 0 || !near_account(t + 7, strlen(t + 7))) continue;
    contracts[contract_count++] = t + 7;
  }

  Value* list = tj::Arr(a);
  if (!list) { error->Upstream("near: arena exhausted"); return false; }
  if (contract_count) {
    Call* calls = new_calls(a, contract_count * 2);
    bool built = calls != nullptr;
    for (size_t i = 0; i < contract_count && built; ++i) {
      built = view_call(a, network, contracts[i], "ft_balance_of", address, &calls[i * 2]) &&
              view_call(a, network, contracts[i], "ft_metadata", nullptr, &calls[i * 2 + 1]);
    }
    if (!built) { error->Upstream("near: arena exhausted"); return false; }
    ctx.transport->Run(calls, contract_count * 2, a);
    for (size_t i = 0; i < contract_count; ++i) {
      // Any failure of the pair omits the token (unknown, never zero).
      if (!calls[i * 2].ok() || !calls[i * 2 + 1].ok()) continue;
      Value* balance = nullptr;
      Value* metadata = nullptr;
      if (!view_json(a, calls[i * 2], &balance) || !view_json(a, calls[i * 2 + 1], &metadata)) continue;
      if (!digit_string(balance)) continue;
      const Value* token_decimals = tj::Get(metadata, "decimals");
      if (!tj::IsNumber(token_decimals)) continue;  // never mis-scale
      const Value* raw_symbol = tj::Get(metadata, "symbol");
      Value* token_symbol = tj::Null(a);
      if (truthy(raw_symbol)) {
        // sanitizeSymbol on a non-string throws inside the token's try.
        if (!tj::IsString(raw_symbol)) continue;
        token_symbol = SanitizeSymbol(a, raw_symbol->text, raw_symbol->length);
      }
      Value* entry = tj::Obj(a);
      if (!entry || !tj::Set(a, entry, "tokenId", Format(a, "nep141:%s", contracts[i])) ||
          !tj::Set(a, entry, "balanceRaw", balance) ||
          !tj::Set(a, entry, "decimals", tj::Num(a, token_decimals->number)) ||
          !tj::Set(a, entry, "symbol", token_symbol) || !tj::Set(a, entry, "source", tj::Str(a, "near-rpc")) ||
          !tj::Push(a, list, entry)) {
        error->Upstream("near: arena exhausted");
        return false;
      }
    }
  }

  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!native || !out || !tj::Set(a, native, "balanceRaw", tj::Str(a, amount)) ||
      !tj::Set(a, native, "decimals", tj::Num(a, decimals)) ||
      !tj::Set(a, native, "symbol", tj::Str(a, symbol ? symbol : "")) ||
      !tj::Set(a, native, "source", tj::Str(a, "near-rpc")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", list)) {
    error->Upstream("near: arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

bool always(const config::ProviderRuntimeConfig&) { return true; }

// ---- NearBlocks transfer source ---------------------------------------------------

// One transaction's netted view, accumulated across its receipt rows.
struct Aggregate {
  const char* hash;
  BigInt net;
  Value* height;  // raw block_height or JSON null
  BigInt ts_ns;
  Value* counterparty;  // nullptr = null
  bool failed;
  Value* fee_raw;  // nullptr = null
  Value* method;   // nullptr = null
  bool reward;
};

struct Walk {
  Context* ctx;
  const Network* network;
  const char* address;
  Aggregate* aggregates;
  size_t count;
  size_t capacity;
  bool failed;
};

bool page(void* opaque, double, const char* cursor, HeightWalkPage* out, Error* error) {
  auto* w = static_cast<Walk*>(opaque);
  Arena& a = *w->ctx->arena;
  const char* encoded = EncodeUri(a, w->address);
  const char* encoded_cursor = cursor ? EncodeUri(a, cursor) : "";
  if (!encoded || !encoded_cursor) { error->Upstream("nearblocks: arena exhausted"); return false; }
  Value* path = cursor ? Format(a, "/v1/account/%s/txns?per_page=25&order=desc&cursor=%s", encoded, encoded_cursor)
                       : Format(a, "/v1/account/%s/txns?per_page=25&order=desc", encoded);
  if (!path) { error->Upstream("nearblocks: arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = kBaseUrls;
  call.url_count = 1;
  call.path = tj::CString(a, path);
  if (!call.path) { error->Upstream("nearblocks: arena exhausted"); return false; }
  call.lane = "nearblocks";
  // Yocto amounts arrive as JSON numbers: parse them exactly.
  call.exact_integers = true;
  if (w->ctx->config->nearblocks_api_key_configured) {
    Value* bearer = Format(a, "Bearer %s", w->ctx->config->nearblocks_api_key);
    if (!bearer) { error->Upstream("nearblocks: arena exhausted"); return false; }
    call.headers[0] = CallHeader{"authorization", bearer->text};
    call.header_count = 1;
  }
  w->ctx->transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* txns = tj::Get(call.value, "txns");
  out->rows = tj::IsArray(txns) ? txns : tj::Arr(a);
  const Value* next = tj::Get(call.value, "cursor");
  out->cursor = tj::IsString(next) ? tj::CString(a, next) : nullptr;
  out->more = tj::IsString(next);
  if (!out->rows || (tj::IsString(next) && !out->cursor)) {
    error->Upstream("nearblocks: arena exhausted");
    return false;
  }
  return true;
}

bool height_of(void* opaque, const Value* row, double* height) {
  auto* w = static_cast<Walk*>(opaque);
  // `txn.block?.block_height` reads a property of the row: a null row throws.
  if (!row || row->type == tj::Type::kNull) w->failed = true;
  const Value* value = tj::Get(tj::Get(row, "block"), "block_height");
  if (!value || value->type == tj::Type::kNull) return false;
  *height = JsNumber(value);
  return true;
}

// integerString(value) as a BigInt (false when null).
bool integer_big(Arena& a, const Value* value, BigInt* out) {
  const Value* text = IntegerString(a, value);
  return tj::IsString(text) && BigInt::Parse(text->text, text->length, out);
}

void on_row(void* opaque, Value* txn) {
  auto* w = static_cast<Walk*>(opaque);
  Arena& a = *w->ctx->arena;
  if (w->failed) return;
  const Value* hash = tj::Get(txn, "transaction_hash");
  if (!tj::IsString(hash)) return;
  Value* from = tj::Get(txn, "predecessor_account_id");
  Value* to = tj::Get(txn, "receiver_account_id");
  if (from && from->type == tj::Type::kNull) from = nullptr;
  if (to && to->type == tj::Type::kNull) to = nullptr;
  const bool received = is_string(to, w->address);
  const bool sent = is_string(from, w->address);
  if (!received && !sent) return;

  Aggregate* g = nullptr;
  for (size_t i = 0; i < w->count; ++i)
    if (strlen(w->aggregates[i].hash) == hash->length && memcmp(w->aggregates[i].hash, hash->text, hash->length) == 0) {
      g = &w->aggregates[i];
      break;
    }
  if (!g) {
    if (w->count == w->capacity) { w->failed = true; return; }
    g = &w->aggregates[w->count];
    *g = Aggregate{};
    g->hash = tj::CString(a, hash);
    Value* height = tj::Get(tj::Get(txn, "block"), "block_height");
    g->height = height ? height : tj::Null(a);
    BigInt ts;
    if (integer_big(a, tj::Get(txn, "block_timestamp"), &ts)) g->ts_ns = ts;
    if (!g->hash || !g->height) { w->failed = true; return; }
    ++w->count;
  }

  const Value* actions = tj::Get(txn, "actions");
  if (tj::IsArray(actions)) {
    for (uint32_t i = 0; i < actions->count; ++i) {
      const Value* action = actions->items[i];
      BigInt amount;
      const Value* deposit = IntegerString(a, tj::Get(action, "deposit"));
      if (!tj::IsString(deposit) || Equals(deposit, "0")) continue;
      if (!BigInt::Parse(deposit->text, deposit->length, &amount)) continue;
      // A receipt where the holder is BOTH sides (a refund to itself) nets out.
      if (received && !g->net.Add(amount)) w->failed = true;
      if (sent && !g->net.Sub(amount)) w->failed = true;
      if (!g->counterparty) g->counterparty = received ? from : to;
      if (received && is_string(from, kSystemAccount)) g->reward = true;
      Value* method = tj::Get(action, "method");
      if (truthy(method) && !g->method) g->method = method;
    }
  }
  // The fee belongs to the signer; a receipt the holder only received on was
  // paid by someone else.
  if (sent && !g->fee_raw) {
    Value* fee = IntegerString(a, tj::Get(tj::Get(txn, "outcomes_agg"), "transaction_fee"));
    if (tj::IsString(fee)) g->fee_raw = fee;
  }
  const Value* status = tj::Get(tj::Get(txn, "outcomes"), "status");
  if (status && status->type == tj::Type::kFalse) g->failed = true;
}

Value* or_null(Arena& a, Value* v) { return v ? v : tj::Null(a); }

// toRecord(): null for a call that moved no value.
bool to_record(Arena& a, const Network& network, const char* address, const Aggregate& g, Value** record) {
  *record = nullptr;
  if (g.net.IsZero()) return true;
  const bool incoming = !g.net.negative();
  const char* direction = DirectionFromFlow(incoming, !incoming);
  // Validator payouts are rewards in the app's vocabulary, not plain receives.
  const char* type = g.reward ? "reward" : TypeFromDirection(direction);
  BigInt ms = g.ts_ns;
  uint32_t remainder = 0;
  if (!ms.DivSmall(1000000, &remainder)) return false;
  const BigInt amount = g.net.Abs();
  const uint32_t decimals = network.has_native ? network.native_decimals : 24;
  const char* symbol = network.has_native && network.native_symbol ? network.native_symbol : "NEAR";
  Value* r = tj::Obj(a);
  Value* holder = tj::Str(a, address);
  Value* party = or_null(a, g.counterparty);
  const bool ok = r && holder && party &&
      tj::Set(a, r, "recordId", Format(a, "%s:%s", network.id, g.hash)) &&
      tj::Set(a, r, "txType", tj::Str(a, type)) &&
      tj::Set(a, r, "status", tj::Str(a, g.failed ? "failed" : "confirmed")) &&
      tj::Set(a, r, "direction", tj::Str(a, direction)) &&
      tj::Set(a, r, "ts", tj::Num(a, ms.ToDouble())) && tj::Set(a, r, "hash", tj::Str(a, g.hash)) &&
      tj::Set(a, r, "blockNumber", g.height) && tj::Set(a, r, "fromAddress", incoming ? party : holder) &&
      tj::Set(a, r, "toAddress", incoming ? holder : party) && tj::Set(a, r, "counterparty", party) &&
      tj::Set(a, r, "tokenId", tj::Str(a, "native")) && tj::Set(a, r, "asset", NativeAssetJson(a, network)) &&
      tj::Set(a, r, "amountRaw", amount.ToValue(a)) && tj::Set(a, r, "amountDecimals", tj::Num(a, decimals)) &&
      tj::Set(a, r, "feeRaw", or_null(a, g.fee_raw)) &&
      tj::Set(a, r, "feeDecimals", g.fee_raw ? tj::Num(a, decimals) : tj::Null(a)) &&
      tj::Set(a, r, "feeSymbol", g.fee_raw ? tj::Str(a, symbol) : tj::Null(a)) &&
      tj::Set(a, r, "memo", tj::Null(a)) && tj::Set(a, r, "method", or_null(a, g.method)) &&
      tj::Set(a, r, "error", g.failed ? tj::Str(a, "failed") : tj::Null(a)) &&
      tj::Set(a, r, "metadata", tj::Obj(a));
  if (!ok) return false;
  *record = r;
  return true;
}

}  // namespace

bool NearSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* last = tj::Get(state, "lastHeight");
  const double since = tj::IsNumber(last) ? last->number : 0;
  // Math.max(1, Math.min(backfillDepth, 100)); NaN stays NaN.
  const double budget = isnan(backfill_depth) ? NAN : fmax(1, fmin(backfill_depth, kPageLimit * kMaxPages));
  const size_t capacity = 128;  // at most 100 rows are consumed
  auto* aggregates = static_cast<Aggregate*>(a.Alloc(sizeof(Aggregate) * capacity));
  if (!aggregates) { error->Upstream("nearblocks: arena exhausted"); return false; }
  Walk w{&ctx, &network, address, aggregates, 0, capacity, false};
  HeightWalkResult walked;
  if (!WalkDescendingByHeight(since, budget, kMaxPages, page, height_of, on_row, &w, &walked, error))
    return false;
  if (w.failed) { error->Upstream("nearblocks: malformed rows or arena exhausted"); return false; }

  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("nearblocks: arena exhausted"); return false; }
  for (size_t i = 0; i < w.count; ++i) {
    Value* record = nullptr;
    if (!to_record(a, network, address, aggregates[i], &record)) {
      error->Upstream("nearblocks: arena exhausted");
      return false;
    }
    if (record && !tj::Push(a, records, record)) { error->Upstream("nearblocks: arena exhausted"); return false; }
  }
  Value* next = tj::IsObject(state) ? tj::Clone(a, state) : tj::Obj(a);
  if (!next || !tj::Set(a, next, "lastHeight", tj::Num(a, walked.high_water))) {
    error->Upstream("nearblocks: arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = walked.complete;
  return true;
}

namespace {

bool near_supports(const config::ProviderRuntimeConfig&, const Network& network) { return !network.testnet; }

}  // namespace

const NearDriver kNearDriver{};

const NearSource kNearSource{};


bool NearDriver::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool NearSource::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool NearSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return near_supports(config, network); }

}  // namespace fortis::chains
