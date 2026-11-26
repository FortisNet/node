#include "chain_family_drivers.h"

#include "chain_tron_token.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

// The Tron family of the address, transaction and network routes:
//  - kTronDriver: the TypeScript Tron driver's balances — TronGrid getaccount
//    for TRX and assetV2, triggerconstantcontract views
//    (balanceOf/decimals/symbol)
//    for requested TRC-20s, and the TRC-10 token snapshot (TronScan when keyed,
//    else /wallet/getassetissuebyid) for requested TRC-10 decimals/symbol;
//  - TronscanWalletTokens: the TypeScript walletTokens (token-holdings);
//  - kTronSource: the TronScan transfer source (native and TRC-20 legs).
// TronScanClient.get: one 'tronscan' lane, the deployment key as
// TRON-PRO-API-KEY, re-issued once anonymously on 401/403/429 unless the
// endpoint requires the key. The token snapshot interpreters are shared with
// the token routes (chain_tron_token.h, token_tron.cpp), which own
// tokenMetadata/tokenInsight and the token page's tokenHolders.
//
// Native difference: TypeScript coalesces concurrent snapshot reads of one
// TRC-10 id across requests (a FlightRegistry shared by every driver
// caller); here duplicates are shared only within one balances call, and the
// token routes' flights (token_tron.cpp) are not joined by the address routes.
namespace fortis::chains {
namespace {

constexpr char kTronscanBase[] = "https://apilist.tronscanapi.com";
constexpr size_t kMaxTokens = 20;
constexpr double kPage = 50;  // TronScan's hard cap
constexpr int kMaxPagesPerLeg = 4;
// Concurrent calls per Transport::Run (TypeScript fires a Promise.all; the
// native transport takes at most kTransportMaxCalls at once).
constexpr size_t kChunk = 45;

bool nullish(const Value* v) { return !v || v->type == tj::Type::kNull; }

bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull:
    case tj::Type::kFalse: return false;
    case tj::Type::kTrue: return true;
    case tj::Type::kNumber: return v->number != 0 && !isnan(v->number);
    case tj::Type::kString: return v->length > 0;
    default: return true;
  }
}

bool is_record(const Value* v) { return v && v->type == tj::Type::kObject; }

void append_js_string(const Value* v, OutBuffer& out) {
  char number[40];
  if (!v) { out.Text("undefined"); return; }
  switch (v->type) {
    case tj::Type::kNull: out.Text("null"); return;
    case tj::Type::kTrue: out.Text("true"); return;
    case tj::Type::kFalse: out.Text("false"); return;
    case tj::Type::kNumber: JsNumberText(v->number, number, sizeof(number)); out.Text(number); return;
    case tj::Type::kString: out.Append(v->text, v->length); return;
    case tj::Type::kObject: out.Text("[object Object]"); return;
    case tj::Type::kArray:
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        if (!nullish(v->items[i])) append_js_string(v->items[i], out);
      }
      return;
  }
}

// String(value) / `${value}`.
Value* js_string(Arena& arena, const Value* v) {
  if (v && v->type == tj::Type::kString) return tj::Str(arena, v->text, v->length);
  uint8_t bytes[1024];
  OutBuffer out{bytes, sizeof(bytes), 0, true};
  append_js_string(v, out);
  return out.ok ? tj::Str(arena, reinterpret_cast<const char*>(bytes), out.used) : nullptr;
}

Value* or_null(Arena& arena, const Value* v) { return nullish(v) ? tj::Null(arena) : const_cast<Value*>(v); }

bool all_digits(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i) if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

bool tron_address(const char* text, size_t length) { return tron_token::IsAddress(text, length); }

// base58ToEvmHex: the 20-byte account hex of a Tron address (RangeError on a
// bad payload or checksum).
bool base58_to_evm_hex(const char* address, char hex[41], Error* error) {
  uint8_t bytes[64];
  size_t written = 0;
  if (!Base58Decode(address, strlen(address), bytes, sizeof(bytes), &written)) {
    error->Range("not base58");
    return false;
  }
  if (written != 25 || bytes[0] != 0x41) { error->Range("not a Tron payload"); return false; }
  uint8_t first[32], second[32];
  Sha256(bytes, 21, first);
  Sha256(first, 32, second);
  if (memcmp(second, bytes + 21, 4) != 0) { error->Range("bad checksum"); return false; }
  HexEncode(bytes + 1, 20, hex);
  return true;
}

void run_chunked(Context& ctx, Call* calls, size_t count) {
  for (size_t start = 0; start < count; start += kChunk)
    ctx.transport->Run(calls + start, count - start < kChunk ? count - start : kChunk, *ctx.arena);
}

// ---- TronScan client ------------------------------------------------------------

const char* const kTronscanUrls[] = {kTronscanBase};

void tronscan_call(Context& ctx, Call& call, const char* path, bool keyed) {
  call.kind = CallKind::kGet;
  call.urls = kTronscanUrls;
  call.url_count = 1;
  call.path = path;
  call.lane = "tronscan";
  call.header_count = 0;
  if (keyed) {
    call.headers[0] = CallHeader{"TRON-PRO-API-KEY", ctx.config->tronscan_api_key};
    call.header_count = 1;
  }
}

// TronscanClient.get for every path at once: keyed when configured; a
// 401/403/429 is re-issued anonymously unless `key_required`. Failures stay
// in the calls (the caller decides whether they throw).
bool tronscan_get_all(Context& ctx, const char* const* paths, size_t count, bool key_required, Call* calls,
                      Error* error) {
  const bool keyed = ctx.config->tronscan_api_key_configured;
  if (key_required && !keyed) { error->Upstream("TronScan key required"); return false; }
  for (size_t i = 0; i < count; ++i) tronscan_call(ctx, calls[i], paths[i], keyed);
  run_chunked(ctx, calls, count);
  if (key_required || !keyed) return true;
  for (size_t i = 0; i < count; ++i) {
    const Call& c = calls[i];
    if (c.ok() || c.failure == CallFailure::kLaneBusy || c.failure == CallFailure::kCancelled) continue;
    if (c.http_status != 401 && c.http_status != 403 && c.http_status != 429) continue;
    Call retry;
    tronscan_call(ctx, retry, paths[i], false);
    ctx.transport->Run(retry, *ctx.arena);
    calls[i] = retry;
  }
  return true;
}

bool tronscan_get(Context& ctx, const char* path, bool key_required, Value** body, Error* error) {
  Call call;
  if (!tronscan_get_all(ctx, &path, 1, key_required, &call, error)) return false;
  if (!call.ok()) { call.ToError(error); return false; }
  *body = call.value;
  return true;
}

// ---- driver ---------------------------------------------------------------------

void trongrid_post(Context& ctx, const Network& network, Call& call, const char* path, Value* body) {
  call.kind = CallKind::kPost;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path;
  call.body = body;
  call.lane = "trongrid";
  call.header_count = 0;
  if (ctx.config->trongrid_api_key_configured) {
    call.headers[0] = CallHeader{"TRON-PRO-API-KEY", ctx.config->trongrid_api_key};
    call.header_count = 1;
  }
}

// decodeUint256 parses the first word with BigInt: a non-hex word throws.
bool uint_word_ok(const Value* hex) {
  const char* text = hex->text;
  size_t length = hex->length;
  if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { text += 2; length -= 2; }
  return length < 64 || IsHex(text, 64);
}

// One triggerconstantcontract view's request body.
Value* view_body(Arena& a, const char* owner, const char* contract, const char* selector, const char* parameter) {
  Value* body = tj::Obj(a);
  if (!body || !tj::Set(a, body, "owner_address", tj::Str(a, owner)) ||
      !tj::Set(a, body, "contract_address", tj::Str(a, contract)) ||
      !tj::Set(a, body, "function_selector", tj::Str(a, selector)) ||
      !tj::Set(a, body, "parameter", tj::Str(a, parameter)) || !tj::Set(a, body, "visible", tj::Bool(a, true)))
    return nullptr;
  return body;
}

using tron_token::Read;
using tron_token::Snapshot;

}  // namespace

bool TronDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                   size_t token_count, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!tron_address(address, strlen(address))) { error->Range("not a Tron address: %s", address); return false; }

  Value* account_body = tj::Obj(a);
  if (!account_body || !tj::Set(a, account_body, "address", tj::Str(a, address)) ||
      !tj::Set(a, account_body, "visible", tj::Bool(a, true))) {
    error->Upstream("arena exhausted");
    return false;
  }
  if (network.rpc_url_count == 0) { error->Range("no endpoints configured"); return false; }
  Call account;
  trongrid_post(ctx, network, account, "/wallet/getaccount", account_body);
  ctx.transport->Run(account, a);
  if (!account.ok()) { account.ToError(error); return false; }
  if (nullish(account.value)) { error->Upstream("getaccount: null body"); return false; }

  // Requested TRC-20 contracts and TRC-10 ids (20 each, request order).
  const char* contracts[kMaxTokens];
  const char* trc10[kMaxTokens];
  size_t contract_count = 0, trc10_count = 0;
  for (size_t i = 0; i < token_count; ++i) {
    const char* t = tokens[i];
    const size_t length = strlen(t);
    if (contract_count < kMaxTokens && length == 6 + 34 && strncmp(t, "trc20:", 6) == 0 &&
        tron_address(t + 6, 34))
      contracts[contract_count++] = t + 6;
    else if (trc10_count < kMaxTokens && length > 6 && length <= 6 + 20 && strncmp(t, "trc10:", 6) == 0 &&
             all_digits(t + 6, length - 6))
      trc10[trc10_count++] = t + 6;
  }

  char holder_hex[41];
  if (!base58_to_evm_hex(address, holder_hex, error)) return false;
  // holderHex.padStart(64, '0')
  char parameter[65];
  memset(parameter, '0', 24);
  memcpy(parameter + 24, holder_hex, 41);

  Value* out_tokens = tj::Arr(a);
  if (!out_tokens) { error->Upstream("arena exhausted"); return false; }

  // TRC-20: [balanceOf, decimals, symbol] per contract, all concurrently. Any
  // failure omits that token only.
  if (contract_count) {
    Call* calls = static_cast<Call*>(a.Alloc(sizeof(Call) * contract_count * 3));
    if (!calls) { error->Upstream("arena exhausted"); return false; }
    static const char* const kSelectors[] = {"balanceOf(address)", "decimals()", "symbol()"};
    for (size_t i = 0; i < contract_count; ++i) {
      for (size_t k = 0; k < 3; ++k) {
        Call* call = new (&calls[i * 3 + k]) Call();
        Value* body = view_body(a, address, contracts[i], kSelectors[k], k == 0 ? parameter : "");
        if (!body) { error->Upstream("arena exhausted"); return false; }
        trongrid_post(ctx, network, *call, "/wallet/triggerconstantcontract", body);
      }
    }
    run_chunked(ctx, calls, contract_count * 3);
    Value* source = tj::Str(a, "trongrid");
    for (size_t i = 0; i < contract_count; ++i) {
      const Call* c = &calls[i * 3];
      if (!c[0].ok() || !c[1].ok() || !c[2].ok()) continue;  // one token failing must not sink the rest
      const Value* balance = nullptr;
      const Value* decimals = nullptr;
      const Value* symbol = nullptr;
      // view() on a null body throws: caught, omitted.
      if (tron_token::ViewResult(a, c[0].value, &balance) == Read::kThrow ||
          tron_token::ViewResult(a, c[1].value, &decimals) == Read::kThrow ||
          tron_token::ViewResult(a, c[2].value, &symbol) == Read::kThrow) {
        if (!a.ok) { error->Upstream("arena exhausted"); return false; }
        continue;
      }
      if (!balance || !decimals) continue;  // never mis-scale
      // decodeUint256/decodeString of a non-string throws: caught, omitted.
      if (!tj::IsString(balance) || !tj::IsString(decimals) || (symbol && !tj::IsString(symbol))) continue;
      // BigInt('0x' + word) throws on a non-hex word: caught, omitted.
      if (!uint_word_ok(balance) || !uint_word_ok(decimals)) continue;
      Value* raw_symbol = symbol ? AbiString(a, symbol->text, symbol->length) : nullptr;
      Value* sanitized = (raw_symbol && tj::IsString(raw_symbol) && raw_symbol->length)
                             ? SanitizeSymbol(a, raw_symbol->text, raw_symbol->length)
                             : tj::Null(a);
      Value* token = tj::Obj(a);
      const bool ok = token && sanitized &&
                      tj::Set(a, token, "tokenId", Format(a, "trc20:%s", contracts[i])) &&
                      tj::Set(a, token, "balanceRaw", AbiUint256(balance->text, balance->length).ToValue(a)) &&
                      tj::Set(a, token, "decimals",
                              tj::Num(a, AbiUint256(decimals->text, decimals->length).ToDouble())) &&
                      tj::Set(a, token, "symbol", sanitized) && tj::Set(a, token, "source", source) &&
                      tj::Push(a, out_tokens, token);
      if (!ok) { error->Upstream("arena exhausted"); return false; }
    }
  }

  // assetV2 ledger entries: a Map of id -> raw balance (last entry wins).
  const Value* assets = tj::Get(account.value, "assetV2");
  if (!nullish(assets) && !tj::IsArray(assets)) { error->Upstream("assetV2 is not an array"); return false; }
  if (assets) {
    for (uint32_t i = 0; i < assets->count; ++i)
      if (nullish(assets->items[i])) { error->Upstream("assetV2 entry is null"); return false; }
  }

  // TRC-10: the token snapshot per distinct id (TronScan, then the chain).
  if (trc10_count) {
    Snapshot snapshots[kMaxTokens];
    bool have[kMaxTokens] = {};
    size_t first_of[kMaxTokens];
    for (size_t i = 0; i < trc10_count; ++i) {
      first_of[i] = i;
      for (size_t j = 0; j < i; ++j) if (strcmp(trc10[j], trc10[i]) == 0) { first_of[i] = j; break; }
    }
    bool need_chain[kMaxTokens] = {};
    if (ctx.config->tronscan_api_key_configured) {
      const char* paths[kMaxTokens];
      size_t which[kMaxTokens];
      size_t n = 0;
      for (size_t i = 0; i < trc10_count; ++i) {
        if (first_of[i] != i) continue;
        const char* encoded = EncodeUri(a, trc10[i]);
        Value* path = encoded ? Format(a, "/api/token?id=%s&showAll=1&start=0&limit=1", encoded) : nullptr;
        if (!path) { error->Upstream("arena exhausted"); return false; }
        paths[n] = path->text;
        which[n++] = i;
      }
      Call* calls = static_cast<Call*>(a.Alloc(sizeof(Call) * kMaxTokens));
      if (!calls) { error->Upstream("arena exhausted"); return false; }
      for (size_t k = 0; k < n; ++k) new (&calls[k]) Call();
      if (!tronscan_get_all(ctx, paths, n, false, calls, error)) return false;
      for (size_t k = 0; k < n; ++k) {
        const size_t i = which[k];
        // The optional index is enrichment: a failure or no row reads the chain.
        have[i] = calls[k].ok() &&
                  tron_token::TronscanTrc10(a, calls[k].value, trc10[i], &snapshots[i]) == Read::kSnapshot;
        need_chain[i] = !have[i];
      }
    } else {
      for (size_t i = 0; i < trc10_count; ++i) need_chain[i] = first_of[i] == i;
    }
    Call* chain = static_cast<Call*>(a.Alloc(sizeof(Call) * kMaxTokens));
    if (!chain) { error->Upstream("arena exhausted"); return false; }
    size_t which[kMaxTokens];
    size_t n = 0;
    for (size_t i = 0; i < trc10_count; ++i) {
      if (!need_chain[i]) continue;
      Value* body = tj::Obj(a);
      if (!body || !tj::Set(a, body, "value", tj::Str(a, trc10[i])) || !tj::Set(a, body, "visible", tj::Bool(a, true))) {
        error->Upstream("arena exhausted");
        return false;
      }
      new (&chain[n]) Call();
      trongrid_post(ctx, network, chain[n], "/wallet/getassetissuebyid", body);
      which[n++] = i;
    }
    if (n) run_chunked(ctx, chain, n);
    for (size_t k = 0; k < n; ++k)
      have[which[k]] = chain[k].ok() &&
                       tron_token::OnChainTrc10(a, chain[k].value, trc10[which[k]], &snapshots[which[k]]) ==
                           Read::kSnapshot;
    Value* source = tj::Str(a, "trongrid");
    for (size_t i = 0; i < trc10_count; ++i) {
      const Snapshot& snapshot = snapshots[first_of[i]];
      if (!have[first_of[i]] || !snapshot.has_decimals) continue;
      // assetBalances.get(assetId) ?? '0'.
      Value* raw = nullptr;
      if (assets) {
        for (uint32_t k = 0; k < assets->count; ++k) {
          const Value* entry = assets->items[k];
          const Value* key = tj::Get(entry, "key");
          Value* value = tron_token::IntegerString(a, tj::Get(entry, "value"));
          if (!tj::IsString(key) || !value || !all_digits(key->text, key->length) || key->length > 20) continue;
          if (key->length == strlen(trc10[i]) && memcmp(key->text, trc10[i], key->length) == 0) raw = value;
        }
      }
      Value* token = tj::Obj(a);
      const bool ok = token && tj::Set(a, token, "tokenId", Format(a, "trc10:%s", trc10[i])) &&
                      tj::Set(a, token, "balanceRaw", raw ? raw : tj::Str(a, "0")) &&
                      tj::Set(a, token, "decimals", tj::Num(a, snapshot.decimals)) &&
                      tj::Set(a, token, "symbol", snapshot.symbol ? snapshot.symbol : tj::Null(a)) &&
                      tj::Set(a, token, "source", source) && tj::Push(a, out_tokens, token);
      if (!ok) { error->Upstream("arena exhausted"); return false; }
    }
  }

  // String(account.balance ?? 0): an absent balance is a new account's zero.
  const Value* balance = tj::Get(account.value, "balance");
  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  const bool ok = native && out &&
                  tj::Set(a, native, "balanceRaw", nullish(balance) ? tj::Str(a, "0") : js_string(a, balance)) &&
                  tj::Set(a, native, "decimals", tj::Num(a, network.has_native ? network.native_decimals : 6)) &&
                  tj::Set(a, native, "symbol", tj::Str(a, network.has_native ? network.native_symbol : "TRX")) &&
                  tj::Set(a, native, "source", tj::Str(a, "trongrid")) && tj::Set(a, out, "native", native) &&
                  tj::Set(a, out, "tokens", out_tokens);
  if (!ok) { error->Upstream("arena exhausted"); return false; }
  *result = out;
  return true;
}

namespace {

bool tron_present(const config::ProviderRuntimeConfig&) { return true; }

// ---- TronScan transfer source ------------------------------------------------

struct Leg {
  Value* items = nullptr;
  double newest_ts = 0;
  bool exhausted = false;
};

// walkPages(): newest-first until the stored timestamp cursor is met.
bool walk_pages(Context& ctx, bool token_transfers, const char* address, const char* list_key, const char* ts_key,
                double since_ts, double backfill_depth, Leg* leg, Error* error) {
  Arena& a = *ctx.arena;
  leg->items = tj::Arr(a);
  if (!leg->items) { error->Upstream("arena exhausted"); return false; }
  for (int page = 0; page < kMaxPagesPerLeg && leg->items->count < backfill_depth; ++page) {
    char path[512];
    if (token_transfers)
      snprintf(path, sizeof(path), "/api/token_trc20/transfers?sort=-timestamp&count=true&limit=%d&start=%d&relatedAddress=%s",
               static_cast<int>(kPage), static_cast<int>(page * kPage), address);
    else
      snprintf(path, sizeof(path), "/api/transfer?sort=-timestamp&count=true&limit=%d&start=%d&address=%s",
               static_cast<int>(kPage), static_cast<int>(page * kPage), address);
    Value* body = nullptr;
    if (!tronscan_get(ctx, path, false, &body, error)) return false;
    if (nullish(body)) { error->Upstream("TronScan body is null"); return false; }
    const Value* list = tj::Get(body, list_key);
    const uint32_t length = tj::IsArray(list) ? list->count : 0;
    if (length == 0) { leg->exhausted = true; break; }
    bool saw_cursor = false;
    for (uint32_t i = 0; i < length; ++i) {
      const Value* row = list->items[i];
      if (nullish(row)) { error->Upstream("TronScan row is null"); return false; }
      const Value* raw = tj::Get(row, ts_key);
      const double ts = nullish(raw) ? 0 : JsNumber(raw);
      if (ts > leg->newest_ts) leg->newest_ts = ts;
      if (since_ts > 0 && ts <= since_ts) { saw_cursor = true; break; }
      if (!tj::Push(a, leg->items, list->items[i])) { error->Upstream("arena exhausted"); return false; }
    }
    if (saw_cursor || length < kPage) { leg->exhausted = true; break; }
  }
  return true;
}

// Fields every TronScan record shares after its parties.
bool record_tail(Arena& a, Value* record) {
  return tj::Set(a, record, "feeRaw", tj::Null(a)) && tj::Set(a, record, "feeDecimals", tj::Null(a)) &&
         tj::Set(a, record, "feeSymbol", tj::Null(a)) && tj::Set(a, record, "memo", tj::Null(a)) &&
         tj::Set(a, record, "method", tj::Null(a)) && tj::Set(a, record, "error", tj::Null(a)) &&
         tj::Set(a, record, "metadata", tj::Obj(a));
}

// nativeRecord: false only on a TypeScript throw; *record null = skipped.
bool native_record(Arena& a, const Network& network, const char* holder, const Value* t, Value** record,
                   Error* error) {
  *record = nullptr;
  const Value* hash = tj::Get(t, "transactionHash");
  const Value* timestamp = tj::Get(t, "timestamp");
  if (!truthy(hash) || !tj::IsNumber(timestamp)) return true;
  const Value* token_name = tj::Get(t, "tokenName");
  if (token_name && !Equals(token_name, "_")) return true;  // TRC-10 legs: a separate pass later
  const Value* amount = tj::Get(t, "amount");
  if (!tj::IsNumber(amount)) return true;
  const Value* from = tj::Get(t, "transferFromAddress");
  const Value* to = tj::Get(t, "transferToAddress");
  const char* direction = DirectionFromFlow(Equals(to, holder), Equals(from, holder));
  const bool incoming = strcmp(direction, "incoming") == 0;
  Value* hash_text = js_string(a, hash);
  Value* record_id = hash_text ? TransferRecordId(a, network.id, hash_text->text, nullptr) : nullptr;
  if (hash_text && !record_id) { error->Upstream("chainId and hash must be non-empty"); return false; }
  const Value* confirmed = tj::Get(t, "confirmed");
  Value* r = NewTransfer(a);
  const bool ok = r && record_id && tj::Set(a, r, "recordId", record_id) &&
                  tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection(direction))) &&
                  tj::Set(a, r, "status", tj::Str(a, confirmed && confirmed->type == tj::Type::kFalse ? "pending" : "confirmed")) &&
                  tj::Set(a, r, "direction", tj::Str(a, direction)) && tj::Set(a, r, "ts", tj::Num(a, timestamp->number)) &&
                  tj::Set(a, r, "hash", const_cast<Value*>(hash)) && tj::Set(a, r, "blockNumber", tj::Null(a)) &&
                  tj::Set(a, r, "fromAddress", or_null(a, from)) && tj::Set(a, r, "toAddress", or_null(a, to)) &&
                  tj::Set(a, r, "counterparty", or_null(a, incoming ? from : to)) &&
                  tj::Set(a, r, "tokenId", tj::Str(a, "native")) && tj::Set(a, r, "asset", NativeAssetJson(a, network)) &&
                  tj::Set(a, r, "amountRaw", js_string(a, amount)) &&
                  tj::Set(a, r, "amountDecimals", tj::Num(a, network.has_native ? network.native_decimals : 18)) &&
                  // TronScan transfer rows carry no fee: an honest null.
                  record_tail(a, r);
  if (!ok) { error->Upstream("arena exhausted"); return false; }
  *record = r;
  return true;
}

bool trc20_record(Arena& a, const Network& network, const char* holder, const Value* t, Value** record, Error* error) {
  *record = nullptr;
  const Value* hash = tj::Get(t, "transaction_id");
  const Value* block_ts = tj::Get(t, "block_ts");
  if (!truthy(hash) || !tj::IsNumber(block_ts)) return true;
  const Value* info = tj::Get(t, "tokenInfo");
  const Value* contract = tj::Get(info, "tokenId");
  const Value* decimals = tj::Get(info, "tokenDecimal");
  const Value* quant = tj::Get(t, "quant");
  if (!truthy(contract) || !tj::IsNumber(decimals)) return true;
  // /^\d+$/.test(t.quant ?? ''): the test coerces with String().
  Value* quant_text = nullish(quant) ? tj::Str(a, "") : js_string(a, quant);
  if (!quant_text) { error->Upstream("arena exhausted"); return false; }
  if (!all_digits(quant_text->text, quant_text->length)) return true;
  const Value* from = tj::Get(t, "from_address");
  const Value* to = tj::Get(t, "to_address");
  const char* direction = DirectionFromFlow(Equals(to, holder), Equals(from, holder));
  const bool incoming = strcmp(direction, "incoming") == 0;
  const Value* abbr = tj::Get(info, "tokenAbbr");
  Value* symbol = nullptr;
  if (truthy(abbr)) {
    if (!tj::IsString(abbr)) { error->Upstream("tokenAbbr is not a string"); return false; }
    symbol = SanitizeSymbol(a, abbr->text, abbr->length);
  } else {
    symbol = tj::Str(a, "");
  }
  Value* hash_text = js_string(a, hash);
  Value* contract_text = js_string(a, contract);
  if (!hash_text || !contract_text || !symbol) { error->Upstream("arena exhausted"); return false; }
  Value* record_id = TransferRecordId(a, network.id, hash_text->text, contract_text->text);
  if (!record_id) { error->Upstream("chainId and hash must be non-empty"); return false; }
  Value* asset = tj::Obj(a);
  const Value* confirmed = tj::Get(t, "confirmed");
  Value* r = NewTransfer(a);
  const bool ok = asset && r && tj::Set(a, asset, "chainId", tj::Str(a, network.id)) && tj::Set(a, asset, "symbol", symbol) &&
                  tj::Set(a, asset, "name", tj::Str(a, symbol->text, symbol->length)) &&
                  tj::Set(a, asset, "decimals", tj::Num(a, decimals->number)) &&
                  tj::Set(a, asset, "contractAddress", const_cast<Value*>(contract)) &&
                  tj::Set(a, r, "recordId", record_id) && tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection(direction))) &&
                  tj::Set(a, r, "status", tj::Str(a, confirmed && confirmed->type == tj::Type::kFalse ? "pending" : "confirmed")) &&
                  tj::Set(a, r, "direction", tj::Str(a, direction)) && tj::Set(a, r, "ts", tj::Num(a, block_ts->number)) &&
                  tj::Set(a, r, "hash", const_cast<Value*>(hash)) && tj::Set(a, r, "blockNumber", tj::Null(a)) &&
                  tj::Set(a, r, "fromAddress", or_null(a, from)) && tj::Set(a, r, "toAddress", or_null(a, to)) &&
                  tj::Set(a, r, "counterparty", or_null(a, incoming ? from : to)) &&
                  tj::Set(a, r, "tokenId", Format(a, "trc20:%s", contract_text->text)) && tj::Set(a, r, "asset", asset) &&
                  tj::Set(a, r, "amountRaw", const_cast<Value*>(quant)) &&
                  tj::Set(a, r, "amountDecimals", tj::Num(a, decimals->number)) && record_tail(a, r);
  if (!ok) { error->Upstream("arena exhausted"); return false; }
  *record = r;
  return true;
}

}  // namespace

bool TronSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state, double backfill_depth,
                SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* native_cursor = tj::Get(state, "lastNativeTs");
  const Value* trc20_cursor = tj::Get(state, "lastTrc20Ts");
  const double last_native_ts = tj::IsNumber(native_cursor) ? native_cursor->number : 0;
  const double last_trc20_ts = tj::IsNumber(trc20_cursor) ? trc20_cursor->number : 0;

  // Sequential legs: TronScan rate-limits aggressively.
  Leg native, trc20;
  if (!walk_pages(ctx, false, address, "data",
                  "timestamp", last_native_ts, backfill_depth, &native, error))
    return false;
  if (!walk_pages(ctx, true,
                  address, "token_transfers", "block_ts", last_trc20_ts, backfill_depth, &trc20, error))
    return false;

  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("arena exhausted"); return false; }
  for (uint32_t i = 0; i < native.items->count; ++i) {
    Value* record = nullptr;
    if (!native_record(a, network, address, native.items->items[i], &record, error)) return false;
    if (record && !tj::Push(a, records, record)) { error->Upstream("arena exhausted"); return false; }
  }
  for (uint32_t i = 0; i < trc20.items->count; ++i) {
    Value* record = nullptr;
    if (!trc20_record(a, network, address, trc20.items->items[i], &record, error)) return false;
    if (record && !tj::Push(a, records, record)) { error->Upstream("arena exhausted"); return false; }
  }

  Value* next = tj::Obj(a);
  if (!next ||
      !tj::Set(a, next, "lastNativeTs", tj::Num(a, last_native_ts > native.newest_ts ? last_native_ts : native.newest_ts)) ||
      !tj::Set(a, next, "lastTrc20Ts", tj::Num(a, last_trc20_ts > trc20.newest_ts ? last_trc20_ts : trc20.newest_ts))) {
    error->Upstream("arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = native.exhausted && trc20.exhausted;
  return true;
}

namespace {

bool tronscan_present(const config::ProviderRuntimeConfig&) { return true; }
bool tronscan_supports(const config::ProviderRuntimeConfig&, const Network& network) { return !network.testnet; }

}  // namespace

// TronscanClient.walletTokens' projection: TRC-10/TRC-20 rows only (String(
// token_type) is exactly "10" or "20": a number, a numeric string or an
// array that joins to it), each reduced to the eight fields the wallet
// displays, in that order. A member the row lacks is absent, as
// JSON.stringify drops an undefined property; a null member stays null.
// Non-object rows are skipped. A null body is TypeScript's TypeError and a
// body without a `data` array its "Malformed TronScan wallet response": both
// upstream failures.
bool TronWalletHoldings(Arena& a, const Value* body, Value** holdings, Error* error) {
  const Value* data = nullish(body) ? nullptr : tj::Get(body, "data");
  if (nullish(body) || !tj::IsArray(data)) { error->Upstream("Malformed TronScan wallet response"); return false; }
  static const char* const kFields[] = {"token_type", "token_id", "token_abbr", "token_name", "token_url",
                                        "balance", "token_decimal", "token_value_in_usd"};
  Value* out = tj::Arr(a);
  if (!out) { error->Upstream("arena exhausted"); return false; }
  for (uint32_t i = 0; i < data->count; ++i) {
    const Value* row = data->items[i];
    if (!is_record(row)) continue;
    const Value* type = tj::Get(row, "token_type");
    if (!tron_token::JsStringEquals(type, "10") && !tron_token::JsStringEquals(type, "20")) continue;
    Value* item = tj::Obj(a);
    if (!item) { error->Upstream("arena exhausted"); return false; }
    for (const char* field : kFields) {
      Value* value = tj::Get(row, field);
      if (value && !tj::Set(a, item, field, value)) { error->Upstream("arena exhausted"); return false; }
    }
    if (!tj::Push(a, out, item)) { error->Upstream("arena exhausted"); return false; }
  }
  *holdings = out;
  return true;
}

// TronscanClient.walletTokens: the keyed /api/account/wallet listing (never
// sent without the key, never retried anonymously), then the projection.
bool TronscanWalletTokens(Context& ctx, const char* address, Value** holdings, Error* error) {
  Arena& a = *ctx.arena;
  const char* encoded = EncodeUri(a, address);
  Value* path = encoded ? Format(a, "/api/account/wallet?address=%s", encoded) : nullptr;
  if (!path) { error->Upstream("arena exhausted"); return false; }
  Value* body = nullptr;
  if (!tronscan_get(ctx, path->text, true, &body, error)) return false;
  return TronWalletHoldings(a, body, holdings, error);
}

bool TronTokenHoldersPresent(const config::ProviderRuntimeConfig& config) { return config.tronscan_api_key_configured; }

// TronDriver.readTokenHolders: tokenSnapshot(network, address) -> holders.
// TronScan first (TRC-20 /api/token_trc20, TRC-10 /api/token); a TronScan
// failure or null snapshot reads the chain, whose snapshot never carries
// holders. Every provider failure is caught (TypeScript answers null).
bool TronTokenHolders(Context& ctx, const Network& network, const char* address, Value** count, Error* error) {
  Arena& a = *ctx.arena;
  *count = tj::Null(a);
  if (!*count) { error->Upstream("arena exhausted"); return false; }
  const size_t length = strlen(address);
  const bool trc10 = tron_token::IsTrc10Id(address, length);
  if (!trc10 && !tron_address(address, length)) return true;
  if (!ctx.config->tronscan_api_key_configured) return true;  // no tokenHolders method

  Snapshot snapshot;
  bool have = false;
  const char* encoded = EncodeUri(a, address);
  Value* path = !encoded ? nullptr
                : trc10 ? Format(a, "/api/token?id=%s&showAll=1&start=0&limit=1", encoded)
                        : Format(a, "/api/token_trc20?contract=%s", encoded);
  if (!path) { error->Upstream("arena exhausted"); return false; }
  Call call;
  const char* text = path->text;
  if (!tronscan_get_all(ctx, &text, 1, false, &call, error)) return false;
  if (call.ok()) {
    have = (trc10 ? tron_token::TronscanTrc10(a, call.value, address, &snapshot)
                  : tron_token::TronscanTrc20(a, call.value, address, &snapshot)) == Read::kSnapshot;
  }
  if (!have) {
    // The on-chain snapshot: its requests still run, but it has no holders.
    if (network.rpc_url_count == 0) return true;  // RangeError, caught
    if (trc10) {
      Value* body = tj::Obj(a);
      if (!body || !tj::Set(a, body, "value", tj::Str(a, address)) || !tj::Set(a, body, "visible", tj::Bool(a, true))) {
        error->Upstream("arena exhausted");
        return false;
      }
      Call chain;
      trongrid_post(ctx, network, chain, "/wallet/getassetissuebyid", body);
      ctx.transport->Run(chain, a);
    } else {
      Call views[4];
      for (size_t k = 0; k < 4; ++k) {
        Value* body = view_body(a, address, address, tron_token::kOnChainSelectors[k], "");
        if (!body) { error->Upstream("arena exhausted"); return false; }
        trongrid_post(ctx, network, views[k], "/wallet/triggerconstantcontract", body);
      }
      ctx.transport->Run(views, 4, a);
    }
    return true;
  }
  if (!snapshot.has_holders) return true;
  Value* out = tj::Obj(a);
  if (!out || !tj::Set(a, out, "count", tj::Num(a, snapshot.holders)) ||
      !tj::Set(a, out, "atLeast", tj::Bool(a, false)) || !tj::Set(a, out, "source", tj::Str(a, "tronscan"))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *count = out;
  return true;
}

const TronDriver kTronDriver{};

const TronSource kTronSource{};


bool TronDriver::Registered(const config::ProviderRuntimeConfig& config) const { return tron_present(config); }
bool TronSource::Registered(const config::ProviderRuntimeConfig& config) const { return tronscan_present(config); }
bool TronSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return tronscan_supports(config, network); }

}  // namespace fortis::chains
