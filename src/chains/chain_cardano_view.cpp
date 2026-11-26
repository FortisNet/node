#include "chain_family_drivers.h"

#include <math.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// The Cardano transaction view: Koios `POST /tx_info` mapped to
// CardanoTxHeader.toJson(). The POST is a read, so it opts into idempotent
// retry; the optional `_outputs`-style flags stay off (Koios 404s on them).
// Koios only serves settled blocks, so every view is final.
namespace fortis::chains {
namespace utxo_detail {
// Defined in chain_utxo.cpp.
const char* JsStr(Arena& arena, const Value* v, size_t* length);
bool AllDigits(const char* text, size_t length);
}  // namespace utxo_detail

bool CardanoView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  Arena& arena = *ctx.arena;
  *detail = nullptr;
  Value* body = tj::Obj(arena);
  Value* hashes = tj::Arr(arena);
  if (!body || !hashes || !tj::Push(arena, hashes, tj::Str(arena, hash)) ||
      !tj::Set(arena, body, "_tx_hashes", hashes)) {
    error->Upstream("arena exhausted");
    return false;
  }
  Call call;
  call.kind = CallKind::kPost;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = "/tx_info";
  call.body = body;
  call.lane = "koios";
  call.retry_post = true;
  ctx.transport->Run(call, arena);
  if (!call.ok()) { call.ToError(error); return false; }
  const Value* rows = call.value;
  const Value* row = tj::IsArray(rows) && rows->count ? rows->items[0] : nullptr;
  if (!row || row->type == tj::Type::kNull) return true;  // not indexed yet

  const double seconds = JsNumber(tj::Get(row, "tx_timestamp"));
  const double height = JsNumber(tj::Get(row, "block_height"));
  const Value* fee = tj::Get(row, "fee");
  size_t fee_length = 0;
  const char* fee_text = fee && fee->type != tj::Type::kNull ? utxo_detail::JsStr(arena, fee, &fee_length) : "";
  if (!fee_text) { error->Upstream("arena exhausted"); return false; }
  const bool fee_ok = utxo_detail::AllDigits(fee_text, fee_length);
  // new Date(seconds * 1000).toISOString(): TimeClip, then the RangeError of
  // an invalid time value.
  const double ms = (isfinite(seconds) ? seconds : 0) * 1000;
  char iso[32];
  if (!(fabs(ms) <= 8.64e15) || !IsoTime(static_cast<int64_t>(trunc(ms)), iso)) {
    error->Range("Invalid time value");
    return false;
  }
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "blockHeight", tj::Num(arena, isfinite(height) ? height : 0)) ||
      !tj::Set(arena, out, "feeLovelace", fee_ok ? tj::Str(arena, fee_text, fee_length) : tj::Str(arena, "0")) ||
      !tj::Set(arena, out, "timestamp", tj::Str(arena, iso)) || !tj::Set(arena, out, "raw", tj::Clone(arena, row))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *detail = out;
  return true;
}

namespace {

bool IsFinal(const Value*, const Network&) { return true; }

}  // namespace

const CardanoView kCardanoView{};


bool CardanoView::IsFinal(const Value* detail, const Network& network) const { return ::fortis::chains::IsFinal(detail, network); }

}  // namespace fortis::chains
