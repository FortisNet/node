// Hedera family of the address, transaction and network routes: the Mirror
// Node account resolver, the balance driver, the transactions transfer
// source and the transaction view, as in the earlier TypeScript
// implementation. Associations belong to other routes and are not ported
// here; the token routes' Mirror reads (token metadata, holder count and
// insight) live in chain_asset_index.cpp / token_ledger.cpp, with the
// entity id canonicalization in token_ref.cpp. Guest-safe.
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

using tj::Type;

constexpr int kHbarDecimals = 8;
constexpr int kMaxPagesPerFetch = 10;
// MAX_TOKEN_LOOKUPS. TypeScript reads all of them at once; natively they run
// in waves of at most kTransportMaxCalls (one Run per wave).
constexpr uint32_t kMaxTokenLookups = 50;
constexpr const char* kLane = "hedera-mirror";

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

bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex_digit(char c) {
  return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool digits(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (!digit(text[i])) return false;
  return true;
}

Value* number_text(Arena& arena, double value) {
  char text[40];
  JsNumberText(value, text, sizeof(text));
  return tj::Str(arena, text);
}

// String(value) for the scalar shapes the Mirror Node sends.
Value* js_string(Arena& arena, const Value* v) {
  if (!v) return tj::Str(arena, "undefined");
  switch (v->type) {
    case Type::kString: return tj::Str(arena, v->text, v->length);
    case Type::kNumber: return number_text(arena, v->number);
    case Type::kTrue: return tj::Str(arena, "true");
    case Type::kFalse: return tj::Str(arena, "false");
    case Type::kNull: return tj::Str(arena, "null");
    case Type::kObject: return tj::Str(arena, "[object Object]");
    case Type::kArray: return tj::Str(arena, "");  // only scalars are expected here
  }
  return nullptr;
}

// String(value ?? fallback).
Value* string_or(Arena& arena, const Value* v, const char* fallback) {
  return nullish(v) ? tj::Str(arena, fallback) : js_string(arena, v);
}

bool finish(Arena& arena, Error* error) {
  if (arena.ok) return true;
  error->Upstream("hedera: work arena exhausted");
  return false;
}

const char* path_for(Arena& arena, const char* prefix, const char* component, const char* suffix) {
  const char* encoded = EncodeUri(arena, component);
  Value* path = Format(arena, "%s%s%s", prefix, encoded ? encoded : "", suffix);
  return path ? path->text : "/";
}

void mirror_get(Call& call, const Network& network, const char* path) {
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path;
  call.lane = kLane;
}

// ---- account resolver ------------------------------------------------------------------

// RAW_ED25519 /^(?:0x)?([0-9a-fA-F]{64})$/ or
// DER_ED25519 /^(?:0x)?302a300506032b6570032100([0-9a-fA-F]{64})$/ -> the
// lowercase key, or false.
bool public_key_of(const char* value, size_t length, char key[65]) {
  if (length >= 2 && value[0] == '0' && value[1] == 'x') { value += 2; length -= 2; }
  static const char kDer[] = "302a300506032b6570032100";
  if (length == 24 + 64 && memcmp(value, kDer, 24) == 0) { value += 24; length -= 24; }
  if (length != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!hex_digit(value[i])) return false;
    key[i] = (value[i] >= 'A' && value[i] <= 'F') ? static_cast<char>(value[i] - 'A' + 'a') : value[i];
  }
  key[64] = '\0';
  return true;
}

// /^\d+\.\d+\.\d+$/ and /^\d+\.\d+\.[A-Z2-7]{50,60}$/i.
bool entity_or_key_alias(const char* value, size_t length, bool* entity, bool* key_alias) {
  *entity = *key_alias = false;
  size_t i = 0;
  for (int part = 0; part < 2; ++part) {
    const size_t start = i;
    while (i < length && digit(value[i])) ++i;
    if (i == start || i >= length || value[i] != '.') return false;
    ++i;
  }
  const size_t start = i;
  if (start == length) return false;
  bool all_digits = true, all_base32 = true;
  for (size_t k = start; k < length; ++k) {
    const char c = value[k];
    all_digits = all_digits && digit(c);
    all_base32 = all_base32 && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'));
  }
  *entity = all_digits;
  *key_alias = all_base32 && length - start >= 50 && length - start <= 60;
  return true;
}

// /^(?:0x)?[0-9a-fA-F]{40}$/
bool evm_alias(const char* value, size_t length) {
  if (length >= 2 && value[0] == '0' && value[1] == 'x') { value += 2; length -= 2; }
  if (length != 40) return false;
  for (size_t i = 0; i < 40; ++i)
    if (!hex_digit(value[i])) return false;
  return true;
}

// resolveHederaMirrorAccount(): *account = the Mirror row, or nullptr for
// "not on the ledger" (404 / no match).
bool resolve_account(Context& ctx, const Network& network, const char* address, Value** account,
                     Error* error) {
  Arena& arena = *ctx.arena;
  *account = nullptr;
  size_t begin = 0, end = 0;
  fortis::codec::provider_text::Trim(address, strlen(address), &begin, &end);
  const char* value = arena.CopyText(address + begin, end - begin);
  if (!value) return finish(arena, error);
  const size_t length = end - begin;

  char key[65];
  Call call{};
  if (public_key_of(value, length, key)) {
    mirror_get(call, network, path_for(arena, "/api/v1/accounts?account.publickey=", key, "&limit=1"));
    ctx.transport->Run(call, arena);
    if (call.answer() && call.http_status == 404) return true;
    if (!call.ok()) { call.ToError(error); return false; }
    if (nullish(call.value)) { error->Upstream("mirror: null accounts page"); return false; }
    const Value* accounts = tj::Get(call.value, "accounts");
    if (tj::IsArray(accounts) && accounts->count && !nullish(accounts->items[0])) *account = accounts->items[0];
    return finish(arena, error);
  }

  bool entity = false, key_alias = false;
  entity_or_key_alias(value, length, &entity, &key_alias);
  if (!entity && !evm_alias(value, length) && !key_alias) {
    error->Range("not a Hedera account id or alias");
    return false;
  }
  mirror_get(call, network, path_for(arena, "/api/v1/accounts/", value, ""));
  ctx.transport->Run(call, arena);
  if (call.answer() && call.http_status == 404) return true;
  if (!call.ok()) { call.ToError(error); return false; }
  if (!nullish(call.value)) *account = call.value;
  return finish(arena, error);
}

// ---- driver --------------------------------------------------------------------------

Value* native_entry(Arena& arena, Value* raw, const Network& network) {
  Value* native = tj::Obj(arena);
  tj::Set(arena, native, "balanceRaw", raw);
  tj::Set(arena, native, "decimals",
          tj::Num(arena, network.has_native ? network.native_decimals : kHbarDecimals));
  tj::Set(arena, native, "symbol", tj::Str(arena, network.has_native ? network.native_symbol : "HBAR"));
  tj::Set(arena, native, "source", tj::Str(arena, "mirror"));
  return native;
}

}  // namespace

bool HederaDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
              Value** result, Error* error) const {
  Arena& arena = *ctx.arena;
  Value* account = nullptr;
  if (!resolve_account(ctx, network, address, &account, error)) return false;
  if (!account) {
    // Alias/key not on the ledger yet — a real zero.
    Value* out = tj::Obj(arena);
    tj::Set(arena, out, "native", native_entry(arena, tj::Str(arena, "0"), network));
    tj::Set(arena, out, "tokens", tj::Arr(arena));
    *result = out;
    return finish(arena, error);
  }

  const Value* balance = tj::Get(account, "balance");
  const Value* listed = tj::Get(balance, "tokens");
  if (!nullish(listed) && !tj::IsArray(listed)) {
    error->Upstream("mirror: tokens is not an array");
    return false;
  }
  const Value* held[kMaxTokenLookups];
  uint32_t held_count = 0;
  if (tj::IsArray(listed)) {
    for (uint32_t i = 0; i < listed->count && held_count < kMaxTokenLookups; ++i) {
      const Value* entry = listed->items[i];
      if (is_str(tj::Get(entry, "token_id")) && tj::IsNumber(tj::Get(entry, "balance")))
        held[held_count++] = entry;
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
      const char* id = tj::CString(arena, tj::Get(held[wave + i], "token_id"));
      mirror_get(calls[i], network, path_for(arena, "/api/v1/tokens/", id ? id : "", ""));
    }
    ctx.transport->Run(calls, count, arena);
    for (uint32_t i = 0; i < count; ++i) {
      // One token's metadata failing must not sink the rest.
      if (!calls[i].ok()) continue;
      const double decimals = JsNumber(tj::Get(calls[i].value, "decimals"));
      if (!isfinite(decimals) || trunc(decimals) != decimals || decimals < 0) continue;  // never mis-scale
      const Value* symbol = tj::Get(calls[i].value, "symbol");
      if (truthy(symbol) && !is_str(symbol)) continue;  // sanitizeSymbol would throw (caught -> null)
      const Value* entry = held[wave + i];
      const Value* id = tj::Get(entry, "token_id");
      Value* token = tj::Obj(arena);
      tj::Set(arena, token, "tokenId", Format(arena, "hts:%.*s", static_cast<int>(id->length), id->text));
      tj::Set(arena, token, "balanceRaw", number_text(arena, tj::Get(entry, "balance")->number));
      tj::Set(arena, token, "decimals", tj::Num(arena, decimals == 0 ? 0 : decimals));
      tj::Set(arena, token, "symbol",
              truthy(symbol) ? SanitizeSymbol(arena, symbol->text, symbol->length) : tj::Null(arena));
      tj::Set(arena, token, "source", tj::Str(arena, "mirror"));
      tj::Push(arena, tokens, token);
    }
  }

  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "native", native_entry(arena, string_or(arena, tj::Get(balance, "balance"), "0"), network));
  tj::Set(arena, out, "tokens", tokens);
  *result = out;
  return finish(arena, error);
}

namespace {

// ---- transfer source ------------------------------------------------------------------

// timestampNanos(value): /^\d+\.\d{1,9}$/ -> seconds * 1e9 + nanos.
bool timestamp_nanos(const Value* value, BigInt* out) {
  if (!is_str(value)) return false;
  const char* text = value->text;
  const size_t length = value->length;
  size_t dot = 0;
  while (dot < length && digit(text[dot])) ++dot;
  if (dot == 0 || dot >= length || text[dot] != '.') return false;
  const size_t fraction = length - dot - 1;
  if (fraction < 1 || fraction > 9 || !digits(text + dot + 1, fraction)) return false;
  BigInt seconds;
  if (!BigInt::ParseDecimal(text, dot, &seconds) || !seconds.MulSmall(1000000000u)) return false;
  char nanos[10];
  memcpy(nanos, text + dot + 1, fraction);
  for (size_t i = fraction; i < 9; ++i) nanos[i] = '0';
  BigInt part;
  if (!BigInt::ParseDecimal(nanos, 9, &part) || !seconds.Add(part)) return false;
  *out = seconds;
  return true;
}

bool text_nanos(Arena& arena, const char* text, BigInt* out) {
  const Value* value = tj::Str(arena, text);
  return value && timestamp_nanos(value, out);
}

// beforeTimestamp(value): one nanosecond earlier as "seconds.nanos".
const char* before_timestamp(Arena& arena, const BigInt& value) {
  BigInt previous = value;
  if (!previous.IsZero() && !previous.negative()) previous.Sub(BigInt::FromU64(1));
  else previous = BigInt::FromU64(0);
  uint32_t nanos = 0;
  BigInt seconds = previous;
  if (!seconds.DivSmall(1000000000u, &nanos)) return nullptr;
  char whole[200];
  if (!seconds.ToDecimal(whole, sizeof(whole))) return nullptr;
  Value* text = Format(arena, "%s.%09u", whole, nanos);
  return text ? text->text : nullptr;
}

// URL searchParams.get('timestamp') of a Mirror `links.next` (relative to
// https://mirror.invalid or absolute): application/x-www-form-urlencoded
// decoding of the first matching pair. Null when absent.
const char* continuation_timestamp(Arena& arena, const Value* next) {
  const char* text = next->text;
  const size_t length = next->length;
  size_t query = 0;
  while (query < length && text[query] != '?' && text[query] != '#') ++query;
  if (query >= length || text[query] != '?') return nullptr;
  size_t stop = query + 1;
  while (stop < length && text[stop] != '#') ++stop;
  size_t i = query + 1;
  auto decode = [&](size_t from, size_t to) -> char* {
    char* out = static_cast<char*>(arena.Alloc(to - from + 1, 1));
    if (!out) return nullptr;
    size_t n = 0;
    for (size_t k = from; k < to; ++k) {
      if (text[k] == '+') { out[n++] = ' '; continue; }
      if (text[k] == '%' && k + 2 < to && hex_digit(text[k + 1]) && hex_digit(text[k + 2])) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        out[n++] = static_cast<char>((nib(text[k + 1]) << 4) | nib(text[k + 2]));
        k += 2;
        continue;
      }
      out[n++] = text[k];
    }
    out[n] = '\0';
    return out;
  };
  while (i <= stop) {
    size_t amp = i;
    while (amp < stop && text[amp] != '&') ++amp;
    if (amp > i) {
      size_t eq = i;
      while (eq < amp && text[eq] != '=') ++eq;
      const char* name = decode(i, eq);
      if (name && strcmp(name, "timestamp") == 0) return decode(eq < amp ? eq + 1 : amp, amp);
    }
    i = amp + 1;
  }
  return nullptr;
}

// One transaction's legs (HederaTransferSource.legsFor). False on a value
// TypeScript would throw on (BigInt of a non-integral amount, an invalid
// consensus time that later fails serialization).
bool legs_for(Arena& arena, const Network& network, const char* account_id, const Value* tx, Value* records,
              Error* error) {
  Value* transaction_id = tj::Get(tx, "transaction_id");
  const Value* consensus = tj::Get(tx, "consensus_timestamp");
  if (!truthy(transaction_id) || !truthy(consensus)) return true;
  const double ts = JsNumber(consensus) * 1000;
  if (!isfinite(ts) || fabs(ts) > 8.64e15) {
    error->Upstream("mirror: invalid consensus timestamp");
    return false;
  }
  const Value* result = tj::Get(tx, "result");
  const bool failed = !Equals(result, "SUCCESS");
  Value* native = NativeAssetJson(arena, network);
  const Value* id_text = js_string(arena, transaction_id);
  const char* hash = id_text ? tj::CString(arena, id_text) : nullptr;
  if (!hash) return finish(arena, error);

  BigInt hbar_net;
  const Value* transfers = tj::Get(tx, "transfers");
  if (!nullish(transfers) && !tj::IsArray(transfers)) {
    error->Upstream("mirror: transfers is not an array");
    return false;
  }
  if (tj::IsArray(transfers)) {
    for (uint32_t i = 0; i < transfers->count; ++i) {
      const Value* t = transfers->items[i];
      const Value* amount = tj::Get(t, "amount");
      if (!Equals(tj::Get(t, "account"), account_id) || !tj::IsNumber(amount)) continue;
      BigInt value;
      if (!BigInt::FromDouble(amount->number, &value) || !hbar_net.Add(value)) {
        error->Upstream("mirror: non-integral transfer amount");
        return false;
      }
    }
  }
  // The payer's net already includes the fee; attach charged_tx_fee only
  // when this account visibly paid (net negative on a self-initiated tx).
  const Value* charged = tj::Get(tx, "charged_tx_fee");
  const bool paid_fee = hbar_net.negative() && tj::IsNumber(charged);
  const Value* memo_base64 = tj::Get(tx, "memo_base64");
  Value* memo = truthy(memo_base64) && is_str(memo_base64)
      ? PrintableMemoFromBase64(arena, memo_base64->text, memo_base64->length)
      : tj::Null(arena);
  Value* name = tj::Get(tx, "name");

  auto base = [&]() -> Value* {
    Value* r = NewTransfer(arena);
    tj::Set(arena, r, "status", tj::Str(arena, failed ? "failed" : "confirmed"));
    tj::Set(arena, r, "ts", tj::Num(arena, trunc(ts) == 0 ? 0 : trunc(ts)));
    tj::Set(arena, r, "hash", transaction_id);
    tj::Set(arena, r, "feeRaw", paid_fee ? number_text(arena, charged->number) : tj::Null(arena));
    tj::Set(arena, r, "feeDecimals", paid_fee ? tj::Num(arena, kHbarDecimals) : tj::Null(arena));
    tj::Set(arena, r, "feeSymbol", paid_fee ? tj::Get(native, "symbol") : tj::Null(arena));
    tj::Set(arena, r, "memo", memo);
    tj::Set(arena, r, "error",
            failed ? (nullish(result) ? tj::Str(arena, "failed") : const_cast<Value*>(result)) : tj::Null(arena));
    tj::Set(arena, r, "method", nullish(name) ? tj::Null(arena) : name);
    return r;
  };

  // TokenAssociate is an approval-like association, never a fake transfer.
  if (Equals(name, "TOKENASSOCIATE")) {
    Value* r = base();
    Value* record_id = TransferRecordId(arena, network.id, hash, nullptr);
    if (!record_id) { error->Upstream("mirror: empty transaction id"); return false; }
    tj::Set(arena, r, "recordId", record_id);
    tj::Set(arena, r, "txType", tj::Str(arena, "approve"));
    tj::Set(arena, r, "direction", tj::Str(arena, "outgoing"));
    tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
    tj::Push(arena, records, r);
    return finish(arena, error);
  }

  if (!hbar_net.IsZero()) {
    const bool outgoing = hbar_net.negative();
    const char* direction = DirectionFromFlow(!outgoing, outgoing);
    // The outgoing net includes the fee; the value moved is net minus fee.
    BigInt moved = hbar_net.Abs();
    if (outgoing && paid_fee) {
      BigInt fee;
      if (!BigInt::FromDouble(charged->number, &fee) || !moved.Sub(fee)) {
        error->Upstream("mirror: non-integral fee");
        return false;
      }
    }
    if (!moved.IsZero() && !moved.negative()) {
      Value* record_id = TransferRecordId(arena, network.id, hash, nullptr);
      if (!record_id) { error->Upstream("mirror: empty transaction id"); return false; }
      Value* r = base();
      tj::Set(arena, r, "recordId", record_id);
      tj::Set(arena, r, "txType", tj::Str(arena, TypeFromDirection(direction)));
      tj::Set(arena, r, "direction", tj::Str(arena, direction));
      tj::Set(arena, r, "tokenId", tj::Str(arena, "native"));
      tj::Set(arena, r, "asset", native);
      tj::Set(arena, r, "amountRaw", moved.ToValue(arena));
      tj::Set(arena, r, "amountDecimals", tj::Num(arena, kHbarDecimals));
      tj::Push(arena, records, r);
    }
  }

  // byToken: net per token id, in first-seen order.
  const Value* token_transfers = tj::Get(tx, "token_transfers");
  if (!nullish(token_transfers) && !tj::IsArray(token_transfers)) {
    error->Upstream("mirror: token_transfers is not an array");
    return false;
  }
  if (!tj::IsArray(token_transfers) || token_transfers->count == 0) return finish(arena, error);
  const uint32_t n = token_transfers->count;
  auto* ids = static_cast<const Value**>(arena.Alloc(sizeof(Value*) * n));
  auto* nets = static_cast<BigInt*>(arena.Alloc(sizeof(BigInt) * n));
  if (!ids || !nets) return finish(arena, error);
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Value* t = token_transfers->items[i];
    const Value* token_id = tj::Get(t, "token_id");
    const Value* amount = tj::Get(t, "amount");
    if (!Equals(tj::Get(t, "account"), account_id) || !truthy(token_id) || !is_str(token_id) ||
        !tj::IsNumber(amount))
      continue;
    BigInt value;
    if (!BigInt::FromDouble(amount->number, &value)) {
      error->Upstream("mirror: non-integral token amount");
      return false;
    }
    uint32_t slot = 0;
    while (slot < distinct && !(ids[slot]->length == token_id->length &&
                                memcmp(ids[slot]->text, token_id->text, token_id->length) == 0))
      ++slot;
    if (slot == distinct) {
      ids[distinct] = token_id;
      new (&nets[distinct]) BigInt();
      ++distinct;
    }
    if (!nets[slot].Add(value)) {
      error->Upstream("mirror: token amount overflow");
      return false;
    }
  }
  for (uint32_t k = 0; k < distinct; ++k) {
    if (nets[k].IsZero()) continue;
    const bool outgoing = nets[k].negative();
    const char* direction = DirectionFromFlow(!outgoing, outgoing);
    const char* token = tj::CString(arena, ids[k]);
    Value* record_id = token ? TransferRecordId(arena, network.id, hash, token) : nullptr;
    if (!record_id) { error->Upstream("mirror: empty transaction id"); return false; }
    Value* r = base();
    tj::Set(arena, r, "recordId", record_id);
    tj::Set(arena, r, "txType", tj::Str(arena, TypeFromDirection(direction)));
    tj::Set(arena, r, "direction", tj::Str(arena, direction));
    tj::Set(arena, r, "tokenId", Format(arena, "hts:%s", token));
    // HTS decimals are not in the transactions row — the raw amount without
    // an asset json, never mis-scaled.
    tj::Set(arena, r, "amountRaw", nets[k].Abs().ToValue(arena));
    tj::Push(arena, records, r);
  }
  return finish(arena, error);
}

}  // namespace

bool HederaSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
           double backfill_depth, SourceResult* out, Error* error) const {
  Arena& arena = *ctx.arena;
  // Resolve id/alias/public key once. A valid key without a ledger account is
  // an unfunded wallet, so its history is honestly empty and complete.
  Value* account = nullptr;
  if (!resolve_account(ctx, network, address, &account, error)) return false;
  if (!account) {
    out->records = tj::Arr(arena);
    out->state = state ? tj::Clone(arena, state) : tj::Obj(arena);
    out->backfill_complete = true;
    return finish(arena, error);
  }
  const Value* account_value = tj::Get(account, "account");
  if (!is_str(account_value)) {
    error->Upstream("mirror: no account id");
    return false;
  }
  const char* account_id = tj::CString(arena, account_value);
  if (!account_id) return finish(arena, error);

  const Value* stored = tj::Get(state, "gtTimestamp");
  BigInt probe;
  const char* gt = timestamp_nanos(stored, &probe) ? tj::CString(arena, stored) : "0.0";
  if (!gt) return finish(arena, error);
  // Bootstrap at creation, including its record, and repair old epoch-zero
  // checkpoints.
  BigInt created, current;
  if (timestamp_nanos(tj::Get(account, "created_timestamp"), &created) && text_nanos(arena, gt, &current) &&
      current.Compare(created) < 0) {
    gt = before_timestamp(arena, created);
    if (!gt) return finish(arena, error);
  }
  const double budget = fmax(1, floor(backfill_depth));
  double fetched = 0;
  bool complete = false;
  Value* records = tj::Arr(arena);
  for (int page = 0; page < kMaxPagesPerFetch && fetched < budget; ++page) {
    char limit[40];
    JsNumberText(fmin(budget - fetched, 100), limit, sizeof(limit));
    Value* path = Format(arena, "/api/v1/transactions?account.id=%s&timestamp=gt:%s&order=asc&limit=%s",
                         account_id, gt, limit);
    Call call{};
    mirror_get(call, network, path ? path->text : "/");
    ctx.transport->Run(call, arena);
    if (!call.ok()) { call.ToError(error); return false; }
    const Value* txs = tj::Get(call.value, "transactions");
    if (!tj::IsArray(txs)) {
      error->Upstream("mirror: missing transactions");
      return false;
    }
    for (uint32_t i = 0; i < txs->count; ++i)
      if (!legs_for(arena, network, account_id, txs->items[i], records, error)) return false;
    fetched += txs->count;

    const Value* last = txs->count ? tj::Get(txs->items[txs->count - 1], "consensus_timestamp") : nullptr;
    BigInt last_nanos, gt_nanos;
    const bool has_last = timestamp_nanos(last, &last_nanos);
    text_nanos(arena, gt, &gt_nanos);
    if (txs->count > 0 && (!has_last || last_nanos.Compare(gt_nanos) <= 0)) {
      error->Upstream("mirror: transaction cursor did not advance");
      return false;
    }
    const Value* next = tj::Get(tj::Get(call.value, "links"), "next");
    if (nullish(next)) {
      if (!nullish(last)) {
        const Value* text = js_string(arena, last);
        gt = text ? tj::CString(arena, text) : nullptr;
        if (!gt) return finish(arena, error);
      }
      complete = true;
      break;
    }
    // Read only the timestamp from the continuation; rebuild the request on
    // the configured endpoint with our account/order and remaining budget.
    const char* filter = is_str(next) ? continuation_timestamp(arena, next) : nullptr;
    const char* next_gt = filter && strncmp(filter, "gt:", 3) == 0 ? filter + 3 : nullptr;
    BigInt next_nanos;
    if (!next_gt || !text_nanos(arena, next_gt, &next_nanos) || next_nanos.Compare(gt_nanos) <= 0 ||
        (has_last && next_nanos.Compare(last_nanos) < 0)) {
      error->Upstream("mirror: continuation cursor did not advance");
      return false;
    }
    // Persist progress even for an empty page when this fetch hits its bound.
    gt = next_gt;
  }
  Value* next_state = tj::Obj(arena);
  tj::Set(arena, next_state, "gtTimestamp", tj::Str(arena, gt));
  out->records = records;
  out->state = next_state;
  out->backfill_complete = complete;
  return finish(arena, error);
}

namespace {

// ---- detail view ---------------------------------------------------------------------

}  // namespace

bool HederaView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  Arena& arena = *ctx.arena;
  *detail = nullptr;
  Call call{};
  mirror_get(call, network, path_for(arena, "/api/v1/transactions/", hash, ""));
  ctx.transport->Run(call, arena);
  // 404 = not indexed yet: TypeScript catches every failure as "no body".
  const Value* transactions = call.ok() ? tj::Get(call.value, "transactions") : nullptr;
  Value* records = tj::Arr(arena);
  if (tj::IsArray(transactions)) {
    for (uint32_t i = 0; i < transactions->count; ++i) {
      Value* tx = transactions->items[i];
      if (!tj::IsObject(tx) && !tj::IsArray(tx)) continue;
      Value* record = tj::Obj(arena);
      tj::Set(arena, record, "name", string_or(arena, tj::Get(tx, "name"), ""));
      tj::Set(arena, record, "result", string_or(arena, tj::Get(tx, "result"), ""));
      Value* fee = string_or(arena, tj::Get(tx, "charged_tx_fee"), "");
      tj::Set(arena, record, "feeTinybar", fee && digits(fee->text, fee->length) ? fee : tj::Str(arena, "0"));
      tj::Set(arena, record, "consensusRaw", string_or(arena, tj::Get(tx, "consensus_timestamp"), ""));
      tj::Set(arena, record, "raw", tx);
      // The mirror sends the 48-byte SHA-384 hash base64; the app shows hex.
      const Value* tx_hash = tj::Get(tx, "transaction_hash");
      if (is_str(tx_hash) && tx_hash->length) {
        const size_t capacity = tx_hash->length / 4 * 3 + 4;
        auto* bytes = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
        size_t written = 0;
        if (bytes && Base64Decode(tx_hash->text, tx_hash->length, bytes, capacity, &written)) {
          char* hex = static_cast<char*>(arena.Alloc(written * 2 + 3, 1));
          if (hex) {
            hex[0] = '0';
            hex[1] = 'x';
            HexEncode(bytes, written, hex + 2);
            tj::Set(arena, record, "hashHex", tj::Str(arena, hex));
          }
        }
      }
      Value* entity = tj::Get(tx, "entity_id");
      if (is_str(entity)) tj::Set(arena, record, "entityId", entity);
      tj::Push(arena, records, record);
    }
  }
  if (records->count == 0) return finish(arena, error);

  Value* out = tj::Obj(arena);
  tj::Set(arena, out, "records", records);
  const Value* consensus = tj::Get(records->items[0], "consensusRaw");
  if (consensus && consensus->length) {
    const char* text = tj::CString(arena, consensus);
    Call block{};
    mirror_get(block, network, path_for(arena, "/api/v1/blocks?timestamp=gte:", text ? text : "",
                                        "&limit=1&order=asc"));
    ctx.transport->Run(block, arena);
    // Best effort: a failure omits blockNumber rather than losing the records.
    if (block.ok()) {
      const Value* blocks = tj::Get(block.value, "blocks");
      Value* number = tj::IsArray(blocks) && blocks->count ? tj::Get(blocks->items[0], "number") : nullptr;
      if (tj::IsNumber(number)) tj::Set(arena, out, "blockNumber", number);
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

const HederaDriver kHederaDriver{};
const HederaSource kHederaSource{};
const HederaView kHederaView{};


bool HederaDriver::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool HederaSource::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool HederaSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return has_rpc(config, network); }
bool HederaView::IsFinal(const Value* detail, const Network& network) const { return final_view(detail, network); }

}  // namespace fortis::chains
