#include "chain_family_drivers.h"
#include "chain_asset_index.h"
#include "token_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// Cardano over Koios (the TypeScript Cardano driver's balances and its
// Koios transfer source). Koios POSTs are idempotent reads, so
// they opt into the GET retry policy. Native assets are `cnt:{policy}{nameHex}`
// with on-chain default 0 decimals; a name decodes to a symbol only when it
// is printable ASCII. History is UTXO net-flow accounting: lovelace net of
// the fee the holder paid plus one `cnt:{unit}` leg per moved native asset.
// CardanoTokenHolders serves the network overview's holder counts over the
// asset-summary projections the token page shares (chain_asset_index).
namespace fortis::chains {
namespace utxo_detail {
// Defined in chain_utxo.cpp.
const char* JsStr(Arena& arena, const Value* v, size_t* length);
bool AllDigits(const char* text, size_t length);
bool DigitsValue(const Value* v);
bool BigIntOf(const Value* v, BigInt* out, Error* error);
bool Iterable(const Value* v, Value* const** items, size_t* count, Error* error);
bool Accessible(const Value* v, Error* error);
}  // namespace utxo_detail

namespace {

using utxo_detail::Accessible;
using utxo_detail::BigIntOf;
using utxo_detail::DigitsValue;
using utxo_detail::Iterable;
using utxo_detail::JsStr;

constexpr size_t kHydrateBatch = 50;
constexpr double kMaxTxsPerSync = 200;

bool ArenaFail(Error* error) {
  error->Upstream("arena exhausted");
  return false;
}

const char* Text(Arena& arena, const char* format, ...) __attribute__((format(printf, 2, 3)));
const char* Text(Arena& arena, const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int length = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  char* out = length < 0 ? nullptr : static_cast<char*>(arena.Alloc(static_cast<size_t>(length) + 1, 1));
  if (out) vsnprintf(out, static_cast<size_t>(length) + 1, format, args);
  va_end(args);
  return out;
}

bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// /^(addr1|addr_test1|stake1|Ae2|DdzFF)[0-9a-zA-Z]+$/
bool CardanoAddress(const char* address) {
  static const char* const kPrefixes[] = {"addr1", "addr_test1", "stake1", "Ae2", "DdzFF"};
  for (const char* prefix : kPrefixes) {
    const size_t length = strlen(prefix);
    if (strncmp(address, prefix, length) != 0) continue;
    const char* rest = address + length;
    if (!*rest) continue;
    bool alnum = true;
    for (const char* p = rest; *p && alnum; ++p) alnum = IsAlnum(*p);
    if (alnum) return true;
  }
  return false;
}

// {_addresses: [address]} (optionally with _after_block_height).
Value* AddressesBody(Arena& arena, const char* address, double after_block) {
  Value* body = tj::Obj(arena);
  Value* list = tj::Arr(arena);
  if (!body || !list || !tj::Push(arena, list, tj::Str(arena, address)) ||
      !tj::Set(arena, body, "_addresses", list))
    return nullptr;
  if (after_block > 0 && !tj::Set(arena, body, "_after_block_height", tj::Num(arena, after_block))) return nullptr;
  return body;
}

void KoiosPost(Call& call, const Network& network, const char* path, const Value* body) {
  call.kind = CallKind::kPost;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path;
  call.body = body;
  call.lane = "koios";
  call.retry_post = true;
}

bool Always(const config::ProviderRuntimeConfig&) { return true; }
bool SupportsRpc(const config::ProviderRuntimeConfig&, const Network& network) { return network.rpc_url_count > 0; }

// ---- CardanoDriver.balances -------------------------------------------------------

}  // namespace

bool CardanoDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                     Value** result, Error* error) const {
  Arena& arena = *ctx.arena;
  if (!CardanoAddress(address)) {
    error->Range("not a Cardano address: %s", address);
    return false;
  }
  Value* body = AddressesBody(arena, address, 0);
  if (!body) return ArenaFail(error);
  Call calls[2];
  KoiosPost(calls[0], network, "/address_info", body);
  KoiosPost(calls[1], network, "/address_assets", body);
  ctx.transport->Run(calls, 2, arena);
  for (Call& call : calls)
    if (!call.ok()) { call.ToError(error); return false; }
  const Value* info = calls[0].value;
  const Value* assets = calls[1].value;

  Value* tokens = tj::Arr(arena);
  if (!tokens) return ArenaFail(error);
  if (tj::IsArray(assets)) {
    for (uint32_t i = 0; i < assets->count; ++i) {
      const Value* asset = assets->items[i];
      if (!Accessible(asset, error)) return false;
      const Value* policy = tj::Get(asset, "policy_id");
      const Value* quantity = tj::Get(asset, "quantity");
      if (!tj::IsString(policy) || !tj::IsString(quantity)) continue;
      if (!utxo_detail::AllDigits(quantity->text, quantity->length)) continue;
      // nameHex = asset.asset_name ?? ''
      const Value* name = tj::Get(asset, "asset_name");
      size_t name_length = 0;
      const char* name_text =
          name && name->type != tj::Type::kNull ? JsStr(arena, name, &name_length) : "";
      const char* policy_text = tj::CString(arena, policy);
      const char* token_id = name_text && policy_text ? Text(arena, "cnt:%s%s", policy_text, name_text) : nullptr;
      // printableAsciiFromHex of a non-string name is null.
      Value* symbol = !name || name->type == tj::Type::kNull ? PrintableAsciiFromHex(arena, "", 0)
                      : tj::IsString(name) ? PrintableAsciiFromHex(arena, name->text, name->length)
                                           : tj::Null(arena);
      const Value* decimals = tj::Get(asset, "decimals");
      Value* token = tj::Obj(arena);
      if (!token_id || !token || !symbol || !tj::Set(arena, token, "tokenId", tj::Str(arena, token_id)) ||
          !tj::Set(arena, token, "balanceRaw", tj::Clone(arena, quantity)) ||
          !tj::Set(arena, token, "decimals",
                   decimals && decimals->type != tj::Type::kNull ? tj::Clone(arena, decimals) : tj::Num(arena, 0)) ||
          !tj::Set(arena, token, "symbol", symbol) || !tj::Set(arena, token, "source", tj::Str(arena, "koios")) ||
          !tj::Push(arena, tokens, token))
        return ArenaFail(error);
    }
  }
  // info[0]?.balance (a null body is the TypeError of `null[0]`).
  if (!Accessible(info, error)) return false;
  const Value* first = tj::IsArray(info) ? (info->count ? info->items[0] : nullptr)
                       : tj::IsObject(info) ? tj::Get(info, "0") : nullptr;
  const Value* lovelace = tj::Get(first, "balance");
  const bool valid = tj::IsString(lovelace) && utxo_detail::AllDigits(lovelace->text, lovelace->length);
  Value* native = tj::Obj(arena);
  Value* out = tj::Obj(arena);
  const char* symbol = network.has_native ? network.native_symbol : "ADA";
  const uint32_t decimals = network.has_native ? network.native_decimals : 6;
  if (!native || !out ||
      !tj::Set(arena, native, "balanceRaw", valid ? tj::Clone(arena, lovelace) : tj::Str(arena, "0")) ||
      !tj::Set(arena, native, "decimals", tj::Num(arena, decimals)) ||
      !tj::Set(arena, native, "symbol", tj::Str(arena, symbol)) ||
      !tj::Set(arena, native, "source", tj::Str(arena, "koios")) || !tj::Set(arena, out, "native", native) ||
      !tj::Set(arena, out, "tokens", tokens))
    return ArenaFail(error);
  *result = out;
  return true;
}

namespace {

// ---- CardanoTransferSource ----------------------------------------------------------

struct AssetNet {
  const char* unit;
  BigInt net;
};

struct Tally {
  BigInt lovelace;
  bool holder_funded = false;
  const Value* counterparty = nullptr;  // nullptr = JS null
  AssetNet* assets = nullptr;
  size_t count = 0, capacity = 0;
};

bool AddAsset(Arena& arena, Tally& tally, const char* unit, const BigInt& delta) {
  for (size_t i = 0; i < tally.count; ++i)
    if (strcmp(tally.assets[i].unit, unit) == 0) return tally.assets[i].net.Add(delta);
  if (tally.count == tally.capacity) {
    const size_t capacity = tally.capacity ? tally.capacity * 2 : 8;
    auto* grown = static_cast<AssetNet*>(arena.Alloc(sizeof(AssetNet) * capacity));
    if (!grown) return false;
    if (tally.count) memcpy(static_cast<void*>(grown), tally.assets, sizeof(AssetNet) * tally.count);
    tally.assets = grown;
    tally.capacity = capacity;
  }
  tally.assets[tally.count].unit = unit;
  tally.assets[tally.count].net = delta;
  ++tally.count;
  return true;
}

bool TallySides(Arena& arena, const Value* sides, bool outputs, const char* holder, Tally& tally, Error* error) {
  Value* const* items = nullptr;
  size_t count = 0;
  if (!Iterable(sides, &items, &count, error)) return false;
  for (size_t i = 0; i < count; ++i) {
    const Value* side = items[i];
    if (!Accessible(side, error)) return false;
    const Value* bech32 = tj::Get(tj::Get(side, "payment_addr"), "bech32");
    const bool is_holder = tj::IsString(bech32) && strlen(holder) == bech32->length &&
                           memcmp(bech32->text, holder, bech32->length) == 0;
    if (!is_holder) {
      if (outputs && !tally.counterparty) tally.counterparty = bech32 && bech32->type != tj::Type::kNull ? bech32 : nullptr;
      continue;
    }
    if (!outputs) tally.holder_funded = true;
    const Value* value = tj::Get(side, "value");
    if (DigitsValue(value)) {
      BigInt amount;
      if (!BigIntOf(value, &amount, error)) return false;
      if (!outputs) amount.Negate();
      if (!tally.lovelace.Add(amount)) { error->Upstream("amount overflow"); return false; }
    }
    Value* const* assets = nullptr;
    size_t asset_count = 0;
    if (!Iterable(tj::Get(side, "asset_list"), &assets, &asset_count, error)) return false;
    for (size_t j = 0; j < asset_count; ++j) {
      const Value* asset = assets[j];
      if (!Accessible(asset, error)) return false;
      const Value* policy = tj::Get(asset, "policy_id");
      const Value* quantity = tj::Get(asset, "quantity");
      if (!tj::IsString(policy) || !DigitsValue(quantity)) continue;
      const Value* name = tj::Get(asset, "asset_name");
      const char* name_text = name && name->type != tj::Type::kNull ? JsStr(arena, name, nullptr) : "";
      const char* policy_text = tj::CString(arena, policy);
      const char* unit = name_text && policy_text ? Text(arena, "%s%s", policy_text, name_text) : nullptr;
      BigInt amount;
      if (!unit) return ArenaFail(error);
      if (!BigIntOf(quantity, &amount, error)) return false;
      if (!outputs) amount.Negate();
      if (!AddAsset(arena, tally, unit, amount)) { error->Upstream("amount overflow"); return false; }
    }
  }
  return true;
}

// The shared `base` fields of every leg, then `metadata`.
bool SetBase(Arena& arena, Value* record, const char* direction, double ts, const Value* hash,
             const Value* block_height, const char* holder, const Value* counterparty, bool holder_funded,
             const BigInt& fee, const Network& network) {
  const bool incoming = strcmp(direction, "incoming") == 0;
  const uint32_t decimals = network.has_native ? network.native_decimals : 18;
  const char* symbol = network.has_native ? network.native_symbol : "";
  auto counter = [&]() { return counterparty ? tj::Clone(arena, counterparty) : tj::Null(arena); };
  return tj::Set(arena, record, "status", tj::Str(arena, "confirmed")) &&
         tj::Set(arena, record, "direction", tj::Str(arena, direction)) &&
         tj::Set(arena, record, "ts", tj::Num(arena, ts)) && tj::Set(arena, record, "hash", tj::Clone(arena, hash)) &&
         tj::Set(arena, record, "blockNumber",
                 block_height && block_height->type != tj::Type::kNull ? tj::Clone(arena, block_height)
                                                                       : tj::Null(arena)) &&
         tj::Set(arena, record, "fromAddress", incoming ? counter() : tj::Str(arena, holder)) &&
         tj::Set(arena, record, "toAddress", incoming ? tj::Str(arena, holder) : counter()) &&
         tj::Set(arena, record, "counterparty", counter()) &&
         tj::Set(arena, record, "feeRaw", holder_funded ? fee.ToValue(arena) : tj::Null(arena)) &&
         tj::Set(arena, record, "feeDecimals", holder_funded ? tj::Num(arena, decimals) : tj::Null(arena)) &&
         tj::Set(arena, record, "feeSymbol", holder_funded ? tj::Str(arena, symbol) : tj::Null(arena)) &&
         tj::Set(arena, record, "memo", tj::Null(arena)) && tj::Set(arena, record, "method", tj::Null(arena)) &&
         tj::Set(arena, record, "error", tj::Null(arena)) && tj::Set(arena, record, "metadata", tj::Obj(arena));
}

// legsFor(network, holder, tx): appends to `records`.
bool Legs(Context& ctx, const Network& network, const char* holder, const Value* tx, Value* records,
          Error* error) {
  Arena& arena = *ctx.arena;
  if (!Accessible(tx, error)) return false;
  const Value* hash = tj::Get(tx, "tx_hash");
  const Value* timestamp = tj::Get(tx, "tx_timestamp");
  size_t hash_length = 0;
  if (!tj::IsNumber(timestamp)) return true;
  const char* hash_text = JsStr(arena, hash, &hash_length);
  // `!tx.tx_hash`: undefined, null, '', 0, NaN and false are falsy.
  if (!hash || hash->type == tj::Type::kNull || hash->type == tj::Type::kFalse ||
      (hash->type == tj::Type::kString && hash->length == 0) ||
      (hash->type == tj::Type::kNumber && !(hash->number != 0)))
    return true;
  if (!hash_text) return ArenaFail(error);

  Tally tally;
  if (!TallySides(arena, tj::Get(tx, "outputs"), true, holder, tally, error) ||
      !TallySides(arena, tj::Get(tx, "inputs"), false, holder, tally, error))
    return false;
  const Value* fee_value = tj::Get(tx, "fee");
  BigInt fee;
  if (DigitsValue(fee_value) && !BigIntOf(fee_value, &fee, error)) return false;
  BigInt net_ex_fee = tally.lovelace;
  if (tally.holder_funded && !net_ex_fee.Add(fee)) { error->Upstream("amount overflow"); return false; }
  const char* direction = net_ex_fee.negative() ? "outgoing" : !net_ex_fee.IsZero() ? "incoming" : "selfTransfer";
  const double ts = timestamp->number * 1000;
  const Value* block_height = tj::Get(tx, "block_height");

  Value* native = NativeAssetJson(arena, network);
  Value* record = NewTransfer(arena);
  const char* record_id = Text(arena, "%s:%s", network.id, hash_text);
  const uint32_t decimals = network.has_native ? network.native_decimals : 18;
  if (!native || !record || !record_id ||
      !tj::Set(arena, record, "recordId", tj::Str(arena, record_id)) ||
      !tj::Set(arena, record, "txType", tj::Str(arena, strcmp(direction, "incoming") == 0 ? "receive" : "send")) ||
      !tj::Set(arena, record, "tokenId", tj::Str(arena, "native")) || !tj::Set(arena, record, "asset", native) ||
      !tj::Set(arena, record, "amountRaw", net_ex_fee.Abs().ToValue(arena)) ||
      !tj::Set(arena, record, "amountDecimals", tj::Num(arena, decimals)) ||
      !SetBase(arena, record, direction, ts, hash, block_height, holder, tally.counterparty, tally.holder_funded,
               fee, network) ||
      !tj::Push(arena, records, record))
    return ArenaFail(error);

  for (size_t i = 0; i < tally.count; ++i) {
    const AssetNet& entry = tally.assets[i];
    if (entry.net.IsZero()) continue;
    const char* leg_direction = entry.net.negative() ? "outgoing" : "incoming";
    // printableAsciiFromHex(unit.slice(56)) ?? ''
    Value* name_hex = compat::token_text::Slice(arena, entry.unit, strlen(entry.unit), 56, static_cast<size_t>(-1));
    Value* printable = name_hex ? PrintableAsciiFromHex(arena, name_hex->text, name_hex->length) : nullptr;
    Value* label = printable && printable->type == tj::Type::kString ? printable : tj::Str(arena, "");
    Value* asset = tj::Obj(arena);
    Value* leg = NewTransfer(arena);
    const char* leg_id = Text(arena, "%s:%s:%s", network.id, hash_text, entry.unit);
    const char* token_id = Text(arena, "cnt:%s", entry.unit);
    if (!printable || !label || !asset || !leg || !leg_id || !token_id ||
        !tj::Set(arena, asset, "chainId", tj::Str(arena, network.id)) ||
        !tj::Set(arena, asset, "symbol", label) || !tj::Set(arena, asset, "name", tj::Clone(arena, label)) ||
        !tj::Set(arena, asset, "decimals", tj::Num(arena, 0)) ||
        !tj::Set(arena, asset, "contractAddress", tj::Str(arena, entry.unit)) ||
        !tj::Set(arena, leg, "recordId", tj::Str(arena, leg_id)) ||
        !tj::Set(arena, leg, "txType",
                 tj::Str(arena, strcmp(leg_direction, "incoming") == 0 ? "receive" : "send")) ||
        !tj::Set(arena, leg, "tokenId", tj::Str(arena, token_id)) || !tj::Set(arena, leg, "asset", asset) ||
        !tj::Set(arena, leg, "amountRaw", entry.net.Abs().ToValue(arena)) ||
        !tj::Set(arena, leg, "amountDecimals", tj::Num(arena, 0)) ||
        !SetBase(arena, leg, leg_direction, ts, hash, block_height, holder, tally.counterparty,
                 tally.holder_funded, fee, network) ||
        !tj::Push(arena, records, leg))
      return ArenaFail(error);
  }
  return true;
}

}  // namespace

bool CardanoSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const {
  Arena& arena = *ctx.arena;
  const Value* stored = tj::Get(state, "afterBlock");
  const double after_block = tj::IsNumber(stored) ? stored->number : 0;
  Call list;
  Value* list_body = AddressesBody(arena, address, after_block);
  if (!list_body) return ArenaFail(error);
  KoiosPost(list, network, "/address_txs", list_body);
  ctx.transport->Run(list, arena);
  if (!list.ok()) { list.ToError(error); return false; }

  const double cap = backfill_depth < kMaxTxsPerSync ? backfill_depth : kMaxTxsPerSync;
  Value* hashes = tj::Arr(arena);
  if (!hashes) return ArenaFail(error);
  if (tj::IsArray(list.value)) {
    for (uint32_t i = 0; i < list.value->count; ++i) {
      const Value* row = list.value->items[i];
      if (!Accessible(row, error)) return false;
      const Value* hash = tj::Get(row, "tx_hash");
      if (!tj::IsString(hash)) continue;
      if (static_cast<double>(hashes->count) >= cap) continue;  // .slice(0, cap) after the filter
      if (!tj::Push(arena, hashes, tj::Clone(arena, hash))) return ArenaFail(error);
    }
  }

  Value* records = tj::Arr(arena);
  if (!records) return ArenaFail(error);
  double max_block = after_block;
  for (uint32_t start = 0; start < hashes->count; start += kHydrateBatch) {
    Value* batch = tj::Arr(arena);
    Value* body = tj::Obj(arena);
    if (!batch || !body) return ArenaFail(error);
    for (uint32_t i = start; i < hashes->count && i < start + kHydrateBatch; ++i)
      if (!tj::Push(arena, batch, hashes->items[i])) return ArenaFail(error);
    // No optional `_outputs`-style flags: Koios 404s on them.
    if (!tj::Set(arena, body, "_tx_hashes", batch)) return ArenaFail(error);
    Call hydrate;
    KoiosPost(hydrate, network, "/tx_info", body);
    ctx.transport->Run(hydrate, arena);
    if (!hydrate.ok()) { hydrate.ToError(error); return false; }
    if (!tj::IsArray(hydrate.value)) continue;
    for (uint32_t i = 0; i < hydrate.value->count; ++i) {
      const Value* info = hydrate.value->items[i];
      if (!Legs(ctx, network, address, info, records, error)) return false;
      const Value* height = tj::Get(info, "block_height");
      if (tj::IsNumber(height) && height->number >= max_block) max_block = height->number;
    }
  }
  Value* next = tj::Obj(arena);
  if (!next || !tj::Set(arena, next, "afterBlock", tj::Num(arena, max_block))) return ArenaFail(error);
  result->records = records;
  result->state = next;
  result->backfill_complete = static_cast<double>(hashes->count) < cap;
  return true;
}

// CardanoDriver.tokenHolders(network, {address}): cardanoAssetParts splits a
// 56..120-hex unit into lowercase policy + name, one Koios GET
// /asset_summary?_asset_policy=&_asset_name= on the koios lane, and
// koiosHolderCount over its first row (asset_index, shared with the token
// routes): staked_wallets + unstaked_addresses. TypeScript coalesces
// concurrent summary reads in a 100-key FlightRegistry; native issues each
// read.
bool CardanoTokenHolders(Context& ctx, const Network& network, const char* address, Value** count,
                         Error* error) {
  Arena& arena = *ctx.arena;
  *count = tj::Null(arena);
  if (!*count) return ArenaFail(error);
  char path[asset_index::kMaxPathBytes];
  if (!asset_index::CardanoSummaryPath(address, path, sizeof(path))) return true;
  Call call;
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path;
  call.lane = "koios";
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  // rows[0] (a null body is the TypeError of `null[0]`).
  const Value* rows = call.value;
  if (!Accessible(rows, error)) return false;
  Value* holders = asset_index::CardanoHolders(arena, rows);
  if (!holders) return ArenaFail(error);
  *count = holders;
  return true;
}

const CardanoDriver kCardanoDriver{};
const CardanoSource kCardanoSource{};


bool CardanoDriver::Registered(const config::ProviderRuntimeConfig& config) const { return Always(config); }
bool CardanoSource::Registered(const config::ProviderRuntimeConfig& config) const { return Always(config); }
bool CardanoSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return SupportsRpc(config, network); }

}  // namespace fortis::chains
