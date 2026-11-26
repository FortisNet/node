#include "chain_family_drivers.h"

#include <math.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// kSolanaView: the Solana transaction view. getTransaction (jsonParsed) mapped
// to the app's SolanaTxInfo.toJson(); the per-account lamport deltas zip
// message.accountKeys with meta.preBalances/postBalances by index. Every
// returned view is final (getTransaction only answers confirmed slots).
namespace fortis::chains {
namespace {

bool nullish(const Value* v) { return !v || v->type == tj::Type::kNull; }

// typeof v === 'object' && v !== null (arrays included).
bool is_object(const Value* v) {
  return v && (v->type == tj::Type::kObject || v->type == tj::Type::kArray);
}

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

// String(value ?? 0).
Value* lamports_text(Arena& arena, const Value* v) {
  if (nullish(v)) return tj::Str(arena, "0");
  if (v->type == tj::Type::kString) return tj::Str(arena, v->text, v->length);
  uint8_t bytes[1024];
  OutBuffer out{bytes, sizeof(bytes), 0, true};
  append_js_string(v, out);
  return out.ok ? tj::Str(arena, reinterpret_cast<const char*>(bytes), out.used) : nullptr;
}

// solanaAccounts(result, meta).
bool solana_accounts(Arena& a, const Value* result, const Value* meta, Value* out) {
  const Value* pre = meta ? tj::Get(meta, "preBalances") : nullptr;
  const Value* post = meta ? tj::Get(meta, "postBalances") : nullptr;
  const Value* transaction = tj::Get(result, "transaction");
  if (!is_object(transaction)) transaction = nullptr;
  const Value* message = transaction ? tj::Get(transaction, "message") : nullptr;
  if (!is_object(message)) message = nullptr;
  const Value* keys = message ? tj::Get(message, "accountKeys") : nullptr;
  if (!tj::IsArray(pre) || !tj::IsArray(post) || !tj::IsArray(keys)) return true;
  for (uint32_t i = 0; i < keys->count && i < pre->count && i < post->count; ++i) {
    const Value* key = keys->items[i];
    // jsonParsed -> {pubkey, signer, ...}; legacy encoding -> a bare string.
    const bool keyed = is_object(key);
    const Value* pubkey = keyed ? tj::Get(key, "pubkey") : key;
    if (!tj::IsString(pubkey)) continue;
    Value* account = tj::Obj(a);
    if (!account || !tj::Set(a, account, "pubkey", tj::Str(a, pubkey->text, pubkey->length)) ||
        !tj::Set(a, account, "preLamports", lamports_text(a, pre->items[i])) ||
        !tj::Set(a, account, "postLamports", lamports_text(a, post->items[i])) ||
        !tj::Set(a, account, "signer",
                 tj::Bool(a, keyed && tj::Get(key, "signer") && tj::Get(key, "signer")->type == tj::Type::kTrue)) ||
        !tj::Push(a, out, account))
      return false;
  }
  return true;
}

}  // namespace

bool SolanaView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  Arena& a = *ctx.arena;
  Call call;
  call.kind = CallKind::kJsonRpc;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.method = "getTransaction";
  Value* params = tj::Arr(a);
  Value* options = tj::Obj(a);
  if (!params || !options || !tj::Push(a, params, tj::Str(a, hash)) ||
      !tj::Set(a, options, "maxSupportedTransactionVersion", tj::Num(a, 0)) ||
      !tj::Set(a, options, "encoding", tj::Str(a, "jsonParsed")) || !tj::Push(a, params, options)) {
    error->Upstream("arena exhausted");
    return false;
  }
  call.params = params;
  if (network.rpc_url_count == 0) { error->Range("no endpoints configured"); return false; }
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  const Value* result = call.value;
  if (nullish(result)) { *detail = nullptr; return true; }
  const Value* slot = tj::Get(result, "slot");
  if (!tj::IsNumber(slot)) { *detail = nullptr; return true; }  // not confirmed / not found yet

  const Value* meta = tj::Get(result, "meta");
  if (!is_object(meta)) meta = nullptr;
  const Value* fee = meta ? tj::Get(meta, "fee") : nullptr;
  const Value* err = meta ? tj::Get(meta, "err") : nullptr;
  Value* out = tj::Obj(a);
  bool ok = out && tj::Set(a, out, "slot", tj::Num(a, slot->number)) &&
            tj::Set(a, out, "feeLamports", tj::IsNumber(fee) ? lamports_text(a, fee) : tj::Str(a, "0")) &&
            // No meta: failure cannot be proved; err == null is success.
            tj::Set(a, out, "successful", tj::Bool(a, meta == nullptr || nullish(err))) &&
            tj::Set(a, out, "raw", const_cast<Value*>(result));
  const Value* units = meta ? tj::Get(meta, "computeUnitsConsumed") : nullptr;
  if (ok && tj::IsNumber(units)) ok = tj::Set(a, out, "computeUnits", tj::Num(a, units->number));
  const Value* block_time = tj::Get(result, "blockTime");
  if (ok && tj::IsNumber(block_time)) ok = tj::Set(a, out, "blockTimeSecs", tj::Num(a, block_time->number));
  Value* accounts = tj::Arr(a);
  ok = ok && accounts && solana_accounts(a, result, meta, accounts);
  if (ok && accounts->count > 0) ok = tj::Set(a, out, "accounts", accounts);
  if (!ok) { error->Upstream("arena exhausted"); return false; }
  *detail = out;
  return true;
}

namespace {

bool solana_view_final(const Value*, const Network&) { return true; }

}  // namespace

const SolanaView kSolanaView{};


bool SolanaView::IsFinal(const Value* detail, const Network& network) const { return solana_view_final(detail, network); }

}  // namespace fortis::chains
