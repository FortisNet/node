#include "chain_family_drivers.h"

#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// The Solana family of the address, transaction and network routes:
//  - kSolanaDriver: the TypeScript Solana driver (getBalance + jsonParsed
//    getTokenAccountsByOwner over Token and Token-2022, insight, signature
//    status, Helius DAS NFTs) with its address validation;
//  - kSolanaSource: the Helius transfer source (Helius Enhanced
//    Transactions, one record per value leg).
// Endpoint order is Helius first when keyed on a mainnet network, then the
// catalogue RPC. TypeScript gives each endpoint its own lane ('helius' for the
// Helius URL, rpc:{host} otherwise); the transport takes one lane per call, so
// the failover is run here in two stages with DriverTransport's rule: only a
// transport failure or busy lane moves on, an answer error stops.
namespace fortis::chains {
namespace {

constexpr char kTokenProgram[] = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
constexpr char kToken2022Program[] = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb";
constexpr char kHeliusBase[] = "https://api.helius.xyz/v0/addresses";
constexpr double kHeliusPageSize = 100;
constexpr int kHeliusMaxPages = 5;
// The largest Promise.all this driver issues (insight: two calls).
constexpr size_t kMaxRpcBatch = 4;
constexpr char kBrowserUa[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0 Safari/537.36";

bool is_object(const Value* v) { return v && (v->type == tj::Type::kObject || v->type == tj::Type::kArray); }

// JavaScript truthiness (an absent member is undefined, falsy).
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

bool nullish(const Value* v) { return !v || v->type == tj::Type::kNull; }

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
      // Array.prototype.join(','): null and undefined elements are empty.
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
  if (!out.ok) return nullptr;
  return tj::Str(arena, reinterpret_cast<const char*>(bytes), out.used);
}

// `value ?? fallback` as a stored member (undefined -> fallback JSON null).
Value* or_null(Arena& arena, const Value* v) {
  return nullish(v) ? tj::Null(arena) : const_cast<Value*>(v);
}

// The TypeScript validSolanaAddress: 32..44 base58 characters decoding to
// exactly 32 bytes (leading '1's included).
bool valid_solana_address(const char* address) {
  const size_t length = strlen(address);
  if (length < 32 || length > 44) return false;
  uint8_t bytes[64];
  size_t written = 0;
  if (!Base58Decode(address, length, bytes, sizeof(bytes), &written)) return false;
  return written == 32;
}

const char* helius_rpc_url(Context& ctx) {
  if (!ctx.config->helius_api_key_configured) return nullptr;
  Value* url = Format(*ctx.arena, "https://mainnet.helius-rpc.com/?api-key=%s", ctx.config->helius_api_key);
  return url ? url->text : nullptr;
}

// endpointsFor(network): Helius-first on mainnet when keyed.
struct Endpoints {
  const char* helius = nullptr;
  size_t count = 0;  // helius (0/1) + catalogue
  const char* first = nullptr;
};

Endpoints endpoints_for(Context& ctx, const Network& network) {
  Endpoints e;
  if (!network.testnet) e.helius = helius_rpc_url(ctx);
  e.count = (e.helius ? 1 : 0) + network.rpc_url_count;
  e.first = e.helius ? e.helius : (network.rpc_url_count ? network.rpc_urls[0] : nullptr);
  return e;
}

void reset_results(Call& call) {
  call.failure = CallFailure::kNone;
  call.http_status = 0;
  call.value = nullptr;
  call.message[0] = '\0';
  call.has_code = false;
  call.code = 0;
  call.retry_after_ms = 0;
  call.endpoint = nullptr;
}

// DriverTransport.jsonRpc over endpointsFor(network) with per-endpoint lanes.
// `calls` carry method/params; urls/lane are set here. False only when no
// endpoint exists (RangeError 'no endpoints configured').
bool run_rpc(Context& ctx, const Network& network, Call* calls, size_t count, Error* error) {
  const Endpoints e = endpoints_for(ctx, network);
  if (e.count == 0) { error->Range("no endpoints configured"); return false; }
  const char* helius[1] = {e.helius};
  if (e.helius) {
    for (size_t i = 0; i < count; ++i) {
      calls[i].kind = CallKind::kJsonRpc;
      calls[i].urls = helius;
      calls[i].url_count = 1;
      calls[i].lane = "helius";
    }
    ctx.transport->Run(calls, count, *ctx.arena);
    if (network.rpc_url_count == 0) return true;
    // Transport failures and a busy lane fail over to the catalogue RPC.
    Call retry[kMaxRpcBatch];
    size_t index[kMaxRpcBatch];
    size_t n = 0;
    for (size_t i = 0; i < count && n < kMaxRpcBatch; ++i) {
      if (calls[i].ok() || calls[i].answer()) continue;
      retry[n] = calls[i];
      reset_results(retry[n]);
      retry[n].urls = network.rpc_urls;
      retry[n].url_count = network.rpc_url_count;
      retry[n].lane = nullptr;
      index[n++] = i;
    }
    if (n) {
      ctx.transport->Run(retry, n, *ctx.arena);
      for (size_t k = 0; k < n; ++k) calls[index[k]] = retry[k];
    }
    return true;
  }
  for (size_t i = 0; i < count; ++i) {
    calls[i].kind = CallKind::kJsonRpc;
    calls[i].urls = network.rpc_urls;
    calls[i].url_count = network.rpc_url_count;
    calls[i].lane = nullptr;
  }
  ctx.transport->Run(calls, count, *ctx.arena);
  return true;
}

// `rpc:${new URL(url).host}` or 'helius' for the Helius endpoint.
Value* source_name(Arena& arena, const Endpoints& e) {
  if (e.helius && e.first == e.helius) return tj::Str(arena, "helius");
  char host[300];
  if (!e.first || !UrlHost(e.first, host, sizeof(host))) return nullptr;
  return Format(arena, "rpc:%s", host);
}

Value* str_array(Arena& arena, const char* a) {
  Value* array = tj::Arr(arena);
  if (!array || !tj::Push(arena, array, tj::Str(arena, a))) return nullptr;
  return array;
}

// ---- balances ---------------------------------------------------------------

struct MintTotal {
  const Value* mint;
  BigInt amount;
  double decimals;
  double display;
};

}  // namespace

bool SolanaDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                     Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!valid_solana_address(address)) { error->Range("not a Solana address: %s", address); return false; }
  const Endpoints e = endpoints_for(ctx, network);
  if (e.count == 0) { error->Range("no RPC configured for %s", network.id); return false; }

  Call balance;
  balance.method = "getBalance";
  balance.params = str_array(a, address);
  if (!balance.params) { error->Upstream("arena exhausted"); return false; }
  if (!run_rpc(ctx, network, &balance, 1, error)) return false;
  if (!balance.ok()) { balance.ToError(error); return false; }
  // `lamports.value` (a null result throws a TypeError).
  if (!balance.value || balance.value->type == tj::Type::kNull) {
    error->Upstream("getBalance: null result");
    return false;
  }
  Value* lamports = js_string(a, tj::Get(balance.value, "value"));

  // Every parsed token account across both programs, summed per mint in
  // first-seen order (a JavaScript Map).
  size_t capacity = 0, count = 0;
  MintTotal* mints = nullptr;
  static const char* const kPrograms[] = {kTokenProgram, kToken2022Program};
  for (const char* program : kPrograms) {
    Call accounts;
    accounts.method = "getTokenAccountsByOwner";
    Value* params = tj::Arr(a);
    Value* filter = tj::Obj(a);
    Value* encoding = tj::Obj(a);
    if (!params || !filter || !encoding || !tj::Set(a, filter, "programId", tj::Str(a, program)) ||
        !tj::Set(a, encoding, "encoding", tj::Str(a, "jsonParsed")) ||
        !tj::Push(a, params, tj::Str(a, address)) || !tj::Push(a, params, filter) ||
        !tj::Push(a, params, encoding)) {
      error->Upstream("arena exhausted");
      return false;
    }
    accounts.params = params;
    if (!run_rpc(ctx, network, &accounts, 1, error)) return false;
    if (!accounts.ok()) { accounts.ToError(error); return false; }
    const Value* list = is_object(accounts.value) ? tj::Get(accounts.value, "value") : nullptr;
    if (!tj::IsArray(list)) { error->Upstream("getTokenAccountsByOwner: value is not iterable"); return false; }
    if (count + list->count > capacity) {
      const size_t next = count + list->count;
      auto* grown = static_cast<MintTotal*>(a.Alloc(sizeof(MintTotal) * (next ? next : 1)));
      if (!grown) { error->Upstream("arena exhausted"); return false; }
      if (count) memcpy(static_cast<void*>(grown), mints, sizeof(MintTotal) * count);
      mints = grown;
      capacity = next;
    }
    for (uint32_t i = 0; i < list->count; ++i) {
      const Value* entry = list->items[i];
      if (nullish(entry)) { error->Upstream("token account entry is null"); return false; }
      const Value* info = tj::Get(tj::Get(tj::Get(tj::Get(entry, "account"), "data"), "parsed"), "info");
      const Value* mint = tj::Get(info, "mint");
      const Value* token_amount = tj::Get(info, "tokenAmount");
      const Value* amount = tj::Get(token_amount, "amount");
      const Value* decimals = tj::Get(token_amount, "decimals");
      if (!tj::IsString(mint) || !tj::IsString(amount) || !tj::IsNumber(decimals)) continue;
      BigInt raw;
      if (!BigInt::Parse(amount->text, amount->length, &raw)) {
        error->Upstream("token amount is not an integer");  // BigInt() SyntaxError
        return false;
      }
      const double shown = JsNumber(tj::Get(token_amount, "uiAmountString"));
      const double fallback = compat::token_text::ToNumber(amount->text, amount->length) / pow(10, decimals->number);
      MintTotal* found = nullptr;
      for (size_t k = 0; k < count; ++k) {
        if (mints[k].mint->length == mint->length && memcmp(mints[k].mint->text, mint->text, mint->length) == 0) {
          found = &mints[k];
          break;
        }
      }
      if (!found) {
        found = &mints[count++];
        *found = MintTotal{mint, BigInt(), 0, 0};
      }
      if (!found->amount.Add(raw)) { error->Upstream("token amount overflow"); return false; }
      found->decimals = decimals->number;
      found->display += isfinite(shown) ? shown : fallback;
    }
  }

  Value* source = source_name(a, e);
  if (!source) { error->Upstream("invalid endpoint"); return false; }
  Value* tokens = tj::Arr(a);
  if (!tokens) { error->Upstream("arena exhausted"); return false; }
  for (size_t k = 0; k < count; ++k) {
    const MintTotal& m = mints[k];
    const double held = m.amount.ToDouble() / pow(10, m.decimals);
    Value* token = tj::Obj(a);
    bool ok = token && tj::Set(a, token, "tokenId", Format(a, "spl:%.*s", static_cast<int>(m.mint->length), m.mint->text)) &&
              tj::Set(a, token, "balanceRaw", m.amount.ToValue(a)) && tj::Set(a, token, "decimals", tj::Num(a, m.decimals)) &&
              tj::Set(a, token, "symbol", tj::Null(a)) && tj::Set(a, token, "source", source);
    // Only where the mint scales what it displays beyond the display's own
    // rounding of the raw amount.
    if (ok && fabs(m.display - held) > pow(10, -m.decimals)) ok = tj::Set(a, token, "displayUnits", tj::Num(a, m.display));
    if (!ok || !tj::Push(a, tokens, token)) { error->Upstream("arena exhausted"); return false; }
  }

  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  const char* symbol = network.has_native ? network.native_symbol : "SOL";
  const double decimals = network.has_native ? network.native_decimals : 9;
  if (!native || !out || !lamports || !tj::Set(a, native, "balanceRaw", lamports) ||
      !tj::Set(a, native, "decimals", tj::Num(a, decimals)) || !tj::Set(a, native, "symbol", tj::Str(a, symbol)) ||
      !tj::Set(a, native, "source", source) || !tj::Set(a, out, "native", native) || !tj::Set(a, out, "tokens", tokens)) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

// ---- insight / tx status ----------------------------------------------------

}  // namespace

bool SolanaDriver::Insight(Context& ctx, const Network& network, const char* address, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!valid_solana_address(address)) { error->Range("not a Solana address: %s", address); return false; }
  Call calls[2];
  calls[0].method = "getAccountInfo";
  calls[0].params = str_array(a, address);
  calls[1].method = "getSignaturesForAddress";
  Value* params = tj::Arr(a);
  Value* limit = tj::Obj(a);
  if (!calls[0].params || !params || !limit || !tj::Set(a, limit, "limit", tj::Num(a, 1000)) ||
      !tj::Push(a, params, tj::Str(a, address)) || !tj::Push(a, params, limit)) {
    error->Upstream("arena exhausted");
    return false;
  }
  calls[1].params = params;
  if (!run_rpc(ctx, network, calls, 2, error)) return false;
  for (Call& call : calls) if (!call.ok()) { call.ToError(error); return false; }
  if (!calls[0].value || calls[0].value->type == tj::Type::kNull) {
    error->Upstream("getAccountInfo: null result");  // account.value on null
    return false;
  }
  const Value* value = tj::Get(calls[0].value, "value");
  const double count = tj::IsArray(calls[1].value) ? calls[1].value->count : 0;
  const bool exists = !(value && value->type == tj::Type::kNull) || count > 0;
  Value* out = tj::Obj(a);
  if (!out || !tj::Set(a, out, "exists", tj::Bool(a, exists)) || !tj::Set(a, out, "kind", tj::Str(a, "account")) ||
      !tj::Set(a, out, "txCount", tj::Num(a, count)) || !tj::Set(a, out, "txCountFloor", tj::Bool(a, count >= 1000))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

bool SolanaDriver::TxStatus(Context& ctx, const Network& network, const char* hash, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  Call call;
  call.method = "getSignatureStatuses";
  Value* params = tj::Arr(a);
  Value* history = tj::Obj(a);
  if (!params || !history || !tj::Push(a, params, str_array(a, hash)) ||
      !tj::Set(a, history, "searchTransactionHistory", tj::Bool(a, true)) || !tj::Push(a, params, history)) {
    error->Upstream("arena exhausted");
    return false;
  }
  call.params = params;
  if (!run_rpc(ctx, network, &call, 1, error)) return false;
  if (!call.ok()) { call.ToError(error); return false; }
  return SolanaTxStatusOf(a, call.value, result, error);
}

namespace {

// ---- Helius DAS NFTs ----------------------------------------------------------

// sanitizeName of a truthy value (a non-string throws a TypeError in TS).
bool name_or_null(Arena& a, const Value* v, Value** out, Error* error) {
  if (!truthy(v)) { *out = tj::Null(a); return true; }
  if (!tj::IsString(v)) { error->Upstream("NFT name is not a string"); return false; }
  *out = SanitizeName(a, v->text, v->length);
  return *out != nullptr;
}

}  // namespace

bool SolanaDriver::ReadNfts(Context& ctx, const Network& network, const char* address, Value** items, Error* error) const {
  Arena& a = *ctx.arena;
  if (!valid_solana_address(address)) { error->Range("not a Solana address: %s", address); return false; }
  if (network.testnet) { error->Range("DAS serves mainnet only"); return false; }
  const char* helius[1] = {helius_rpc_url(ctx)};
  if (!helius[0]) { error->Upstream("Helius is not configured"); return false; }
  Call call;
  call.kind = CallKind::kJsonRpc;
  call.urls = helius;
  call.url_count = 1;
  call.lane = "helius";
  call.method = "getAssetsByOwner";
  Value* params = tj::Obj(a);
  if (!params || !tj::Set(a, params, "ownerAddress", tj::Str(a, address)) || !tj::Set(a, params, "page", tj::Num(a, 1)) ||
      !tj::Set(a, params, "limit", tj::Num(a, 200))) {
    error->Upstream("arena exhausted");
    return false;
  }
  call.params = params;
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  if (!call.value || call.value->type == tj::Type::kNull) { error->Upstream("getAssetsByOwner: null result"); return false; }
  const Value* list = tj::Get(call.value, "items");
  Value* out = tj::Arr(a);
  if (!out) { error->Upstream("arena exhausted"); return false; }
  // `?? []`; a string iterates characters that carry no `id`.
  if (nullish(list) || tj::IsString(list)) { *items = out; return true; }
  if (!tj::IsArray(list)) { error->Upstream("DAS items are not iterable"); return false; }
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* asset = list->items[i];
    if (nullish(asset)) { error->Upstream("DAS asset is null"); return false; }
    const Value* id = tj::Get(asset, "id");
    if (!tj::IsString(id)) continue;
    const Value* kind = tj::Get(asset, "interface");
    if (Equals(kind, "FungibleToken") || Equals(kind, "FungibleAsset")) continue;
    const Value* grouping = tj::Get(asset, "grouping");
    const Value* collection = nullptr;
    if (!nullish(grouping)) {
      if (!tj::IsArray(grouping)) { error->Upstream("DAS grouping has no find"); return false; }
      for (uint32_t g = 0; g < grouping->count; ++g) {
        const Value* group = grouping->items[g];
        if (nullish(group)) { error->Upstream("DAS group is null"); return false; }
        if (Equals(tj::Get(group, "group_key"), "collection")) { collection = tj::Get(group, "group_value"); break; }
      }
    }
    const Value* content = tj::Get(asset, "content");
    const bool compressed = truthy(tj::Get(tj::Get(asset, "compression"), "compressed"));
    const Value* image = tj::Get(tj::Get(content, "links"), "image");
    Value* name = nullptr;
    Value* collection_name = nullptr;
    if (!name_or_null(a, tj::Get(tj::Get(content, "metadata"), "name"), &name, error) ||
        !name_or_null(a, collection, &collection_name, error))
      return false;
    Value* flags = tj::Obj(a);
    Value* nft = tj::Obj(a);
    bool ok = flags && nft && (!compressed || tj::Set(a, flags, "compressed", tj::Bool(a, true))) &&
              tj::Set(a, nft, "nftId", Format(a, "solana:%.*s", static_cast<int>(id->length), id->text)) &&
              tj::Set(a, nft, "contractAddress", tj::Null(a)) && tj::Set(a, nft, "tokenId", tj::Str(a, id->text, id->length)) &&
              tj::Set(a, nft, "standard", tj::Str(a, compressed ? "compressed" : "metaplex")) &&
              tj::Set(a, nft, "name", name) && tj::Set(a, nft, "collectionName", collection_name) &&
              tj::Set(a, nft, "imageUrl", tj::IsString(image) ? tj::Str(a, image->text, image->length) : tj::Null(a)) &&
              tj::Set(a, nft, "flags", flags) && tj::Push(a, out, nft);
    if (!ok) { error->Upstream("arena exhausted"); return false; }
  }
  *items = out;
  return true;
}

namespace {

bool solana_present(const config::ProviderRuntimeConfig&) { return true; }
bool solana_nfts_present(const config::ProviderRuntimeConfig& config) { return config.helius_api_key_configured; }
bool solana_supports_nfts(const Network& network) { return !network.testnet; }

// ---- Helius transfer source -------------------------------------------------

// One newest-first page of /v0/addresses/{address}/transactions.
bool helius_page(Context& ctx, const char* address, const char* before, Value** txs, Error* error) {
  Arena& a = *ctx.arena;
  static const char* const kBase[] = {kHeliusBase};
  Value* path = before ? Format(a, "/%s/transactions?api-key=%s&limit=%d&before=%s", address,
                                ctx.config->helius_api_key, static_cast<int>(kHeliusPageSize), before)
                       : Format(a, "/%s/transactions?api-key=%s&limit=%d", address, ctx.config->helius_api_key,
                                static_cast<int>(kHeliusPageSize));
  if (!path) { error->Upstream("arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = kBase;
  call.url_count = 1;
  call.path = path->text;
  call.lane = "helius";
  // Cloudflare 1010-blocks default library user agents.
  call.headers[0] = CallHeader{"user-agent", kBrowserUa};
  call.header_count = 1;
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  *txs = tj::IsArray(call.value) ? call.value : nullptr;
  return true;
}

// The holder's legs of one Helius transaction (legsFor), appended to `out`.
bool helius_legs(Arena& a, const Network& network, const char* holder, const Value* tx, Value* out, Error* error) {
  if (nullish(tx)) { error->Upstream("Helius transaction is null"); return false; }
  const Value* signature = tj::Get(tx, "signature");
  const Value* timestamp = tj::Get(tx, "timestamp");
  if (!truthy(signature) || !tj::IsNumber(timestamp)) return true;
  Value* sig = js_string(a, signature);
  if (!sig) { error->Upstream("arena exhausted"); return false; }
  const double ts = timestamp->number * 1000;
  const bool failed = !nullish(tj::Get(tx, "transactionError"));
  Value* native = NativeAssetJson(a, network);
  const Value* fee = tj::Get(tx, "fee");
  const bool paid_fee = Equals(tj::Get(tx, "feePayer"), holder) && tj::IsNumber(fee);
  const double native_decimals = network.has_native ? network.native_decimals : 18;
  const char* native_symbol = network.has_native ? network.native_symbol : "";
  if (!native) { error->Upstream("arena exhausted"); return false; }

  auto common = [&](Value* record, const char* direction, Value* record_id) -> bool {
    return record && record_id && tj::Set(a, record, "recordId", record_id) &&
           tj::Set(a, record, "txType", tj::Str(a, TypeFromDirection(direction))) &&
           tj::Set(a, record, "status", tj::Str(a, failed ? "failed" : "confirmed")) &&
           tj::Set(a, record, "direction", tj::Str(a, direction)) && tj::Set(a, record, "ts", tj::Num(a, ts)) &&
           tj::Set(a, record, "hash", tj::Str(a, sig->text, sig->length)) &&
           tj::Set(a, record, "blockNumber", tj::Null(a));
  };
  auto tail = [&](Value* record) -> bool {
    bool ok = true;
    if (paid_fee) {
      ok = tj::Set(a, record, "feeRaw", js_string(a, fee)) && tj::Set(a, record, "feeDecimals", tj::Num(a, native_decimals)) &&
           tj::Set(a, record, "feeSymbol", tj::Str(a, native_symbol));
    } else {
      ok = tj::Set(a, record, "feeRaw", tj::Null(a)) && tj::Set(a, record, "feeDecimals", tj::Null(a)) &&
           tj::Set(a, record, "feeSymbol", tj::Null(a));
    }
    return ok && tj::Set(a, record, "memo", tj::Null(a)) && tj::Set(a, record, "method", tj::Null(a)) &&
           tj::Set(a, record, "error", failed ? tj::Str(a, "transaction failed") : tj::Null(a)) &&
           tj::Set(a, record, "metadata", tj::Obj(a));
  };

  // Native leg: every SOL movement touching the holder, netted.
  BigInt in, out_total;
  const Value* native_from = nullptr;
  const Value* native_to = nullptr;
  const Value* natives = tj::Get(tx, "nativeTransfers");
  if (!nullish(natives)) {
    if (!tj::IsArray(natives)) { error->Upstream("nativeTransfers is not iterable"); return false; }
    for (uint32_t i = 0; i < natives->count; ++i) {
      const Value* t = natives->items[i];
      if (nullish(t)) { error->Upstream("native transfer is null"); return false; }
      const Value* amount = tj::Get(t, "amount");
      if (!tj::IsNumber(amount)) continue;
      const Value* from = tj::Get(t, "fromUserAccount");
      const Value* to = tj::Get(t, "toUserAccount");
      BigInt value;
      if (Equals(to, holder) || Equals(from, holder)) {
        if (!BigInt::FromDouble(amount->number, &value)) {
          error->Upstream("lamport amount is not an integer");  // BigInt() RangeError in an ingest
          return false;
        }
      }
      if (Equals(to, holder)) {
        if (!in.Add(value)) { error->Upstream("overflow"); return false; }
        if (!nullish(from)) native_from = from;
      }
      if (Equals(from, holder)) {
        if (!out_total.Add(value)) { error->Upstream("overflow"); return false; }
        if (!nullish(to)) native_to = to;
      }
    }
  }
  const BigInt zero;
  const bool incoming_native = in.Compare(zero) > 0;
  const bool outgoing_native = out_total.Compare(zero) > 0;
  if (incoming_native || outgoing_native) {
    const char* direction = DirectionFromFlow(incoming_native, outgoing_native);
    const bool incoming = strcmp(direction, "incoming") == 0;
    BigInt net = in;
    if (in.Compare(out_total) > 0) net.Sub(out_total);
    else { net = out_total; net.Sub(in); }
    Value* record = NewTransfer(a);
    Value* holder_value = tj::Str(a, holder);
    Value* from_value = native_from ? const_cast<Value*>(native_from) : tj::Null(a);
    Value* to_value = native_to ? const_cast<Value*>(native_to) : tj::Null(a);
    const bool ok = common(record, direction, TransferRecordId(a, network.id, sig->text, nullptr)) &&
                    tj::Set(a, record, "fromAddress", incoming ? from_value : holder_value) &&
                    tj::Set(a, record, "toAddress", incoming ? holder_value : to_value) &&
                    tj::Set(a, record, "counterparty", incoming ? from_value : to_value) &&
                    tj::Set(a, record, "tokenId", tj::Str(a, "native")) && tj::Set(a, record, "asset", native) &&
                    tj::Set(a, record, "amountRaw", net.ToValue(a)) &&
                    tj::Set(a, record, "amountDecimals", tj::Num(a, native_decimals)) && tail(record) &&
                    tj::Push(a, out, record);
    if (!ok) { error->Upstream("arena exhausted"); return false; }
  }

  // Token legs: one record per mint the holder sent or received.
  const Value* tokens = tj::Get(tx, "tokenTransfers");
  if (!nullish(tokens)) {
    if (!tj::IsArray(tokens)) { error->Upstream("tokenTransfers is not iterable"); return false; }
    for (uint32_t i = 0; i < tokens->count; ++i) {
      const Value* t = tokens->items[i];
      if (nullish(t)) { error->Upstream("token transfer is null"); return false; }
      const Value* mint = tj::Get(t, "mint");
      const Value* raw = tj::Get(tj::Get(t, "rawTokenAmount"), "tokenAmount");
      const Value* decimals = tj::Get(tj::Get(t, "rawTokenAmount"), "decimals");
      if (!truthy(mint) || !tj::IsString(raw) || !tj::IsNumber(decimals)) continue;
      const Value* from = tj::Get(t, "fromUserAccount");
      const Value* to = tj::Get(t, "toUserAccount");
      const bool incoming = Equals(to, holder);
      const bool outgoing = Equals(from, holder);
      if (!incoming && !outgoing) continue;
      const char* direction = DirectionFromFlow(incoming, outgoing);
      Value* mint_text = js_string(a, mint);
      const Value* abbr = tj::Get(t, "tokenSymbol");
      Value* symbol = nullptr;
      if (truthy(abbr)) {
        if (!tj::IsString(abbr)) { error->Upstream("tokenSymbol is not a string"); return false; }
        symbol = SanitizeSymbol(a, abbr->text, abbr->length);
      } else {
        symbol = tj::Str(a, "");
      }
      // raw.replace('-', ''): the first '-' only.
      const char* dash = static_cast<const char*>(memchr(raw->text, '-', raw->length));
      Value* amount = nullptr;
      if (dash) {
        const size_t at = static_cast<size_t>(dash - raw->text);
        amount = Format(a, "%.*s%.*s", static_cast<int>(at), raw->text, static_cast<int>(raw->length - at - 1), dash + 1);
      } else {
        amount = tj::Str(a, raw->text, raw->length);
      }
      Value* asset = tj::Obj(a);
      Value* record = NewTransfer(a);
      const bool ok = mint_text && symbol && amount && asset &&
                      tj::Set(a, asset, "chainId", tj::Str(a, network.id)) && tj::Set(a, asset, "symbol", symbol) &&
                      tj::Set(a, asset, "name", tj::Str(a, symbol->text, symbol->length)) &&
                      tj::Set(a, asset, "decimals", tj::Num(a, decimals->number)) &&
                      tj::Set(a, asset, "contractAddress", const_cast<Value*>(mint)) &&
                      common(record, direction, TransferRecordId(a, network.id, sig->text, mint_text->text)) &&
                      tj::Set(a, record, "fromAddress", or_null(a, from)) && tj::Set(a, record, "toAddress", or_null(a, to)) &&
                      tj::Set(a, record, "counterparty", or_null(a, incoming ? from : to)) &&
                      tj::Set(a, record, "tokenId", Format(a, "spl:%s", mint_text->text)) &&
                      tj::Set(a, record, "asset", asset) && tj::Set(a, record, "amountRaw", amount) &&
                      tj::Set(a, record, "amountDecimals", tj::Num(a, decimals->number)) && tail(record) &&
                      tj::Push(a, out, record);
      if (!ok) { error->Upstream("arena exhausted"); return false; }
    }
  }
  return true;
}

}  // namespace

bool SolanaSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* stored = tj::Get(state, "lastSig");
  const Value* last_sig = tj::IsString(stored) ? stored : nullptr;
  Value* collected = tj::Arr(a);
  if (!collected) { error->Upstream("arena exhausted"); return false; }
  const char* before = nullptr;
  bool saw_cursor = false, exhausted = false;
  for (int page = 0; page < kHeliusMaxPages && collected->count < backfill_depth; ++page) {
    Value* txs = nullptr;
    if (!helius_page(ctx, address, before, &txs, error)) return false;
    const uint32_t length = txs ? txs->count : 0;
    if (length == 0) { exhausted = true; break; }
    for (uint32_t i = 0; i < length; ++i) {
      const Value* tx = txs->items[i];
      if (nullish(tx)) { error->Upstream("Helius transaction is null"); return false; }
      const Value* signature = tj::Get(tx, "signature");
      if (last_sig && tj::IsString(signature) && signature->length == last_sig->length &&
          memcmp(signature->text, last_sig->text, last_sig->length) == 0) {
        saw_cursor = true;
        break;
      }
      if (!tj::Push(a, collected, txs->items[i])) { error->Upstream("arena exhausted"); return false; }
    }
    if (saw_cursor || length < kHeliusPageSize) {
      exhausted = exhausted || length < kHeliusPageSize;
      break;
    }
    const Value* last = tj::Get(txs->items[length - 1], "signature");
    if (nullish(last)) break;
    Value* text = js_string(a, last);
    if (!text) { error->Upstream("arena exhausted"); return false; }
    before = text->text;
  }

  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("arena exhausted"); return false; }
  for (uint32_t i = 0; i < collected->count; ++i)
    if (!helius_legs(a, network, address, collected->items[i], records, error)) return false;

  // state: {lastSig: collected[0]?.signature ?? lastSig}
  const Value* newest = collected->count ? tj::Get(collected->items[0], "signature") : nullptr;
  Value* cursor = !nullish(newest) ? const_cast<Value*>(newest)
                                   : (last_sig ? const_cast<Value*>(last_sig) : tj::Null(a));
  Value* next = tj::Obj(a);
  if (!next || !tj::Set(a, next, "lastSig", cursor)) { error->Upstream("arena exhausted"); return false; }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = saw_cursor || exhausted;
  return true;
}

namespace {

bool helius_present(const config::ProviderRuntimeConfig& config) { return config.helius_api_key_configured; }
bool helius_supports(const config::ProviderRuntimeConfig&, const Network& network) { return !network.testnet; }

}  // namespace

bool SolanaTxStatusOf(Arena& a, Value* res, Value** result, Error* error) {
  if (!res || res->type == tj::Type::kNull) {
    error->Upstream("getSignatureStatuses: null result");  // res.value on null
    return false;
  }
  const Value* list = tj::Get(res, "value");
  // res.value?.[0]: an array's first element, an object's "0" member, or a
  // string's first UTF-16 unit (an astral character's high surrogate, as
  // WTF-8); undefined for anything else.
  const Value* status = nullptr;
  if (tj::IsArray(list)) {
    status = list->count ? list->items[0] : nullptr;
  } else if (tj::IsObject(list)) {
    status = tj::Get(list, "0");
  } else if (list && list->type == tj::Type::kString && list->length) {
    const uint8_t lead = static_cast<uint8_t>(list->text[0]);
    size_t bytes = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    if (bytes > list->length) bytes = list->length;
    if (bytes == 4) {
      uint32_t cp = (lead & 0x07u) << 18 | (static_cast<uint8_t>(list->text[1]) & 0x3Fu) << 12 |
                    (static_cast<uint8_t>(list->text[2]) & 0x3Fu) << 6 | (static_cast<uint8_t>(list->text[3]) & 0x3Fu);
      const uint32_t high = 0xD800 + ((cp - 0x10000) >> 10);
      const char wtf8[3] = {static_cast<char>(0xE0 | high >> 12), static_cast<char>(0x80 | ((high >> 6) & 0x3F)),
                            static_cast<char>(0x80 | (high & 0x3F))};
      status = tj::Str(a, wtf8, 3);
    } else {
      status = tj::Str(a, list->text, bytes);
    }
  }
  if (nullish(status)) { *result = nullptr; return true; }  // unseen (yet)
  // RPC `confirmed` is optimistic and can roll back; only `finalized` settles.
  const Value* confirmation = tj::Get(status, "confirmationStatus");
  const bool finalized = Equals(confirmation, "finalized");
  const Value* slot = tj::Get(status, "slot");
  Value* detail = tj::Obj(a);
  Value* out = tj::Obj(a);
  const char* state = !nullish(tj::Get(status, "err")) ? "failed" : finalized ? "confirmed" : "pending";
  if (!detail || !out || !tj::Set(a, detail, "confirmationStatus", or_null(a, confirmation)) ||
      !tj::Set(a, out, "status", tj::Str(a, state)) ||
      !tj::Set(a, out, "blockNumber", tj::IsNumber(slot) ? tj::Num(a, slot->number) : tj::Null(a)) ||
      !tj::Set(a, out, "confirmedAt", tj::Null(a)) || !tj::Set(a, out, "detail", detail) ||
      !tj::Set(a, out, "raw", const_cast<Value*>(status))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

const SolanaDriver kSolanaDriver{};

const SolanaSource kSolanaSource{};


bool SolanaDriver::Registered(const config::ProviderRuntimeConfig& config) const { return solana_present(config); }
const NftReader* SolanaDriver::Nfts(const config::ProviderRuntimeConfig& config) const { return solana_nfts_present(config) ? this : nullptr; }
bool SolanaDriver::SupportsNfts(const Network& network) const { return solana_supports_nfts(network); }
bool SolanaSource::Registered(const config::ProviderRuntimeConfig& config) const { return helius_present(config); }
bool SolanaSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return helius_supports(config, network); }

}  // namespace fortis::chains
