#include <algorithm>
#include "chain_family_drivers.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// The UTXO transaction views: the BitcoinTxInfo.toJson() views of Bitcoin
// (Esplora / mempool.space: witness-aware inputs, pool attribution, mempool
// inclusion projection) and Dogecoin / Litecoin / transparent Zcash
// (Blockbook: confirmations count, raw-hex SegWit marker, no pool and no
// projection). Everything after the transaction read itself fails soft: a
// missing tip, pool, block or projection drops that member, never the view.
namespace fortis::chains {
namespace utxo_detail {
// Defined in chain_utxo.cpp.
void JsString(const Value* v, OutBuffer& out);
const char* JsStr(Arena& arena, const Value* v, size_t* length);
bool AllDigits(const char* text, size_t length);
int ZcashHasShieldedComponents(Arena& arena, const Value* raw);
}  // namespace utxo_detail

namespace {

using utxo_detail::AllDigits;
using utxo_detail::JsStr;

// The TypeScript intOr(value, fallback).
double IntOr(const Value* v, double fallback) {
  return IntOrNull(v).value_or(fallback);
}

// `typeof v === 'object' && v !== null` (arrays included, as in JavaScript).
bool IsObjectLike(const Value* v) { return v && (v->type == tj::Type::kObject || v->type == tj::Type::kArray); }

bool StartsWithText(const Value* v, const char* prefix) {
  const size_t length = strlen(prefix);
  return tj::IsString(v) && v->length >= length && memcmp(v->text, prefix, length) == 0;
}

struct UtxoTx {
  const Value* txid = nullptr;
  bool confirmed = false;
  bool has_height = false;
  double height = 0;
  const Value* block_hash = nullptr;  // a string or nullptr
  bool has_time = false;
  double time = 0;
  const char* fee_sats = "0";
  bool fee_reliable = true;
  double vsize = 0;
  double weight = 0;
  bool segwit = false, taproot = false, rbf = false;
  const Value* raw = nullptr;
};

enum class Backend : uint8_t { kEsplora, kBlockbook };

const char* LaneOf(Backend backend) { return backend == Backend::kEsplora ? "esplora" : "blockbook"; }

// One GET on the network's own endpoints (the views never use GetBlock).
// False for any failure (every caller fails soft).
bool Get(Context& ctx, const Network& network, Backend backend, const char* path, Value** body) {
  Call call;
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path;
  call.lane = LaneOf(backend);
  if (!path) return false;
  ctx.transport->Run(call, *ctx.arena);
  if (!call.ok()) return false;
  *body = call.value;
  return true;
}

const char* EncodedPath(Arena& arena, const char* prefix, const char* value) {
  const char* encoded = EncodeUri(arena, value);
  if (!encoded) return nullptr;
  const size_t a = strlen(prefix), b = strlen(encoded);
  char* out = static_cast<char*>(arena.Alloc(a + b + 1, 1));
  if (!out) return nullptr;
  std::copy_n(prefix, a, out);
  memcpy(out + a, encoded, b + 1);
  return out;
}

// `/^\d+$/.test(String(value)) ? String(value) : '0'`.
const char* FeeSats(Arena& arena, const Value* value) {
  size_t length = 0;
  const char* text = JsStr(arena, value, &length);
  return text && AllDigits(text, length) ? text : "0";
}

bool EsploraTransaction(Context& ctx, const Network& network, const char* hash, UtxoTx* tx) {
  Arena& arena = *ctx.arena;
  Value* body = nullptr;
  if (!Get(ctx, network, Backend::kEsplora, EncodedPath(arena, "/tx/", hash), &body)) return false;
  if (!body || body->type == tj::Type::kNull || !tj::IsString(tj::Get(body, "txid"))) return false;
  const Value* status_value = tj::Get(body, "status");
  const Value* status = IsObjectLike(status_value) ? status_value : nullptr;
  const double weight = IntOr(tj::Get(body, "weight"), 0);
  const Value* vsize = tj::Get(body, "vsize");
  tx->vsize = tj::IsNumber(vsize) ? vsize->number : weight > 0 ? ceil(weight / 4) : 0;
  tx->weight = weight;
  const Value* vin = tj::Get(body, "vin");
  if (tj::IsArray(vin)) {
    for (uint32_t i = 0; i < vin->count; ++i) {
      const Value* input = vin->items[i];
      if (!IsObjectLike(input)) continue;
      // BIP-125: any sequence < 0xfffffffe signals replaceability.
      if (IntOr(tj::Get(input, "sequence"), 0xffffffff) < 0xfffffffe) tx->rbf = true;
      const Value* witness = tj::Get(input, "witness");
      if (tj::IsArray(witness) && witness->count > 0) tx->segwit = true;
      const Value* prevout = tj::Get(input, "prevout");
      const Value* type = IsObjectLike(prevout) ? tj::Get(prevout, "scriptpubkey_type") : nullptr;
      if (tj::IsString(type)) {
        if (StartsWithText(type, "v0_") || Equals(type, "p2sh")) tx->segwit = true;
        if (Equals(type, "v1_p2tr")) {
          tx->taproot = true;
          tx->segwit = true;
        }
      }
    }
  }
  // Outputs only flag Taproot.
  const Value* vout = tj::Get(body, "vout");
  if (tj::IsArray(vout)) {
    for (uint32_t i = 0; i < vout->count; ++i) {
      const Value* output = vout->items[i];
      if (IsObjectLike(output) && Equals(tj::Get(output, "scriptpubkey_type"), "v1_p2tr")) tx->taproot = true;
    }
  }
  tx->txid = tj::Get(body, "txid");
  const Value* confirmed = tj::Get(status, "confirmed");
  tx->confirmed = confirmed && confirmed->type == tj::Type::kTrue;
  if (status) {
    const auto height = IntOrNull(tj::Get(status, "block_height"));
    const auto time = IntOrNull(tj::Get(status, "block_time"));
    tx->has_height = height.has_value();
    if (height) tx->height = *height;
    tx->has_time = time.has_value();
    if (time) tx->time = *time;
  }
  const Value* block_hash = tj::Get(status, "block_hash");
  tx->block_hash = tj::IsString(block_hash) ? block_hash : nullptr;
  const Value* fee = tj::Get(body, "fee");
  Value zero;
  zero.type = tj::Type::kNumber;
  tx->fee_sats = FeeSats(arena, fee && fee->type != tj::Type::kNull ? fee : &zero);
  tx->fee_reliable = true;
  tx->raw = body;
  return true;
}

bool BlockbookTransaction(Context& ctx, const Network& network, const char* hash, UtxoTx* tx) {
  Arena& arena = *ctx.arena;
  Value* body = nullptr;
  if (!Get(ctx, network, Backend::kBlockbook, EncodedPath(arena, "/tx/", hash), &body)) return false;
  if (!body || body->type == tj::Type::kNull || !tj::IsString(tj::Get(body, "txid"))) return false;
  const double confirmations = IntOr(tj::Get(body, "confirmations"), 0);
  const double size = IntOr(tj::Get(body, "size"), 0);
  const Value* vsize_value = tj::Get(body, "vsize");
  const double vsize = tj::IsNumber(vsize_value) ? IntOr(vsize_value, 0) : size;
  const int shielded = network.family == Family::kZcash
                           ? utxo_detail::ZcashHasShieldedComponents(arena, tj::Get(body, "hex"))
                           : 0;
  // SegWit: the 0x00 marker + 0x01 flag right after the 4-byte version.
  const Value* hex = tj::Get(body, "hex");
  tx->segwit = tj::IsString(hex) && hex->length >= 12 && memcmp(hex->text + 8, "0001", 4) == 0;
  const Value* vin = tj::Get(body, "vin");
  if (tj::IsArray(vin)) {
    for (uint32_t i = 0; i < vin->count; ++i) {
      const Value* entry = vin->items[i];
      if (!IsObjectLike(entry)) continue;
      const auto sequence = IntOrNull(tj::Get(entry, "sequence"));
      if (sequence && *sequence < 0xfffffffe) {
        tx->rbf = true;
        break;
      }
    }
  }
  // Taproot: an output paying `OP_1 push-32 <32B>` ('5120…', 68 hex chars).
  const Value* vout = tj::Get(body, "vout");
  if (tj::IsArray(vout)) {
    for (uint32_t i = 0; i < vout->count; ++i) {
      const Value* entry = vout->items[i];
      const Value* script = IsObjectLike(entry) ? tj::Get(entry, "hex") : nullptr;
      if (tj::IsString(script) && script->length == 68 && memcmp(script->text, "5120", 4) == 0) {
        tx->taproot = true;
        break;
      }
    }
  }
  tx->txid = tj::Get(body, "txid");
  tx->confirmed = confirmations > 0;
  if (confirmations > 0) {
    const auto height = IntOrNull(tj::Get(body, "blockHeight"));
    tx->has_height = height.has_value();
    if (height) tx->height = *height;
  }
  const Value* block_hash = tj::Get(body, "blockHash");
  tx->block_hash = tj::IsString(block_hash) ? block_hash : nullptr;
  const auto time = IntOrNull(tj::Get(body, "blockTime"));
  tx->has_time = time.has_value();
  if (time) tx->time = *time;
  const Value* fees = tj::Get(body, "fees");
  Value empty;
  empty.type = tj::Type::kString;
  empty.text = "";
  tx->fee_sats = FeeSats(arena, fees && fees->type != tj::Type::kNull ? fees : &empty);
  tx->fee_reliable = network.family != Family::kZcash || shielded == 0;
  tx->vsize = vsize;
  tx->weight = vsize * 4;
  tx->raw = body;
  return true;
}

bool TipHeight(Context& ctx, const Network& network, Backend backend, double* height) {
  Value* body = nullptr;
  std::optional<double> parsed;
  if (backend == Backend::kEsplora) {
    if (Get(ctx, network, backend, "/blocks/tip/height", &body)) parsed = IntOrNull(body);
  } else {
    if (!Get(ctx, network, backend, "/", &body)) return false;
    const Value* blockbook = tj::Get(body, "blockbook");
    if (IsObjectLike(blockbook)) parsed = IntOrNull(tj::Get(blockbook, "bestHeight"));
    if (!parsed) {
      const Value* backend_info = tj::Get(body, "backend");
      if (IsObjectLike(backend_info)) parsed = IntOrNull(tj::Get(backend_info, "blocks"));
    }
  }
  if (!parsed) return false;
  *height = *parsed;
  return true;
}

// Mining-pool attribution (Esplora only); null on any failure.
const Value* PoolName(Context& ctx, const Network& network, const char* block_hash) {
  Value* body = nullptr;
  if (!Get(ctx, network, Backend::kEsplora, EncodedPath(*ctx.arena, "/v1/block/", block_hash), &body)) return nullptr;
  const Value* extras = tj::Get(body, "extras");
  if (!IsObjectLike(extras)) return nullptr;
  const Value* name = tj::Get(tj::Get(extras, "pool"), "name");
  return tj::IsString(name) && name->length ? name : nullptr;
}

Value* BlockInfo(Context& ctx, const Network& network, Backend backend, const char* block_hash) {
  Arena& arena = *ctx.arena;
  Value* body = nullptr;
  if (!Get(ctx, network, backend, EncodedPath(arena, "/block/", block_hash), &body)) return nullptr;
  if (!body || body->type == tj::Type::kNull) return nullptr;
  const bool esplora = backend == Backend::kEsplora;
  const Value* id = tj::Get(body, esplora ? "id" : "hash");
  const Value* difficulty = tj::Get(body, "difficulty");
  double difficulty_value = difficulty && difficulty->type != tj::Type::kNull ? JsNumber(difficulty) : 0;
  if (isnan(difficulty_value) || difficulty_value == 0) difficulty_value = 0;
  Value* info = tj::Obj(arena);
  if (!info ||
      !tj::Set(arena, info, "hash", tj::IsString(id) ? tj::Clone(arena, id) : tj::Str(arena, block_hash)) ||
      !tj::Set(arena, info, "height", tj::Num(arena, IntOr(tj::Get(body, "height"), 0))) ||
      !tj::Set(arena, info, "txCount", tj::Num(arena, IntOr(tj::Get(body, esplora ? "tx_count" : "txCount"), 0))) ||
      !tj::Set(arena, info, "difficulty", tj::Num(arena, difficulty_value)) ||
      !tj::Set(arena, info, "sizeBytes", tj::Num(arena, IntOr(tj::Get(body, "size"), 0))))
    return nullptr;
  const auto time = IntOrNull(tj::Get(body, esplora ? "timestamp" : "time"));
  if (time && !tj::Set(arena, info, "timeSecs", tj::Num(arena, *time)))
    return nullptr;
  return info;
}

// feeRanges(): the next mempool blocks' fee ranges (mempool.space only),
// then projectInclusion over them (ProjectInclusionOf).
void ProjectInclusion(Context& ctx, const Network& network, double fee_rate, bool* has_blocks, double* blocks,
                      bool* beyond) {
  Value* body = nullptr;
  if (!Get(ctx, network, Backend::kEsplora, "/v1/fees/mempool-blocks", &body)) body = nullptr;
  ProjectInclusionOf(fee_rate, body, has_blocks, blocks, beyond);
}

bool ViewFetch(Context& ctx, const Network& network, const char* hash, Backend backend, Value** out,
               Error* error) {
  Arena& arena = *ctx.arena;
  *out = nullptr;
  UtxoTx tx;
  const bool found = backend == Backend::kEsplora ? EsploraTransaction(ctx, network, hash, &tx)
                                                  : BlockbookTransaction(ctx, network, hash, &tx);
  if (!found) return true;  // the explorer does not know it (or failed): null
  const bool zcash = network.family == Family::kZcash;
  Value* inner = tj::Obj(arena);
  Value* detail = tj::Obj(arena);
  bool ok = inner && detail && tj::Set(arena, inner, "txid", tj::Clone(arena, tx.txid)) &&
            tj::Set(arena, inner, "confirmed", tj::Bool(arena, tx.confirmed));
  if (ok && tx.has_height) ok = tj::Set(arena, inner, "blockHeight", tj::Num(arena, tx.height));
  if (ok && tx.block_hash) ok = tj::Set(arena, inner, "blockHash", tj::Clone(arena, tx.block_hash));
  if (ok && tx.has_time) ok = tj::Set(arena, inner, "blockTimeSecs", tj::Num(arena, tx.time));
  ok = ok && tj::Set(arena, inner, "feeSats", tj::Str(arena, tx.fee_sats));
  if (ok && zcash && !tx.fee_reliable) ok = tj::Set(arena, inner, "feeIsReliable", tj::Bool(arena, false));
  ok = ok && tj::Set(arena, inner, "vsize", tj::Num(arena, tx.vsize));
  if (ok && !zcash)
    ok = tj::Set(arena, inner, "weight", tj::Num(arena, tx.weight)) &&
         tj::Set(arena, inner, "isSegWit", tj::Bool(arena, tx.segwit)) &&
         tj::Set(arena, inner, "isTaproot", tj::Bool(arena, tx.taproot)) &&
         tj::Set(arena, inner, "isRbf", tj::Bool(arena, tx.rbf));
  ok = ok && tj::Set(arena, inner, "raw", tj::Clone(arena, tx.raw)) && tj::Set(arena, detail, "detail", inner) &&
       tj::Set(arena, detail, "confirmations", tj::Num(arena, 0)) &&
       tj::Set(arena, detail, "expectedBeyond", tj::Bool(arena, false));
  if (!ok) { error->Upstream("arena exhausted"); return false; }

  if (tx.confirmed && tx.has_height) {
    double tip = 0;
    const bool has_tip = TipHeight(ctx, network, backend, &tip);
    // Depth needs the tip; without it the tx is still confirmed — depth 1.
    const double depth = has_tip && tip >= tx.height ? tip - tx.height + 1 : 1;
    if (!tj::Set(arena, detail, "confirmations", tj::Num(arena, depth))) {
      error->Upstream("arena exhausted");
      return false;
    }
    if (tx.block_hash) {
      // Promise.all(poolName, blockInfo): two independent fail-soft reads.
      const char* block_hash = tj::CString(arena, tx.block_hash);
      if (!block_hash) { error->Upstream("arena exhausted"); return false; }
      const Value* pool = backend == Backend::kEsplora ? PoolName(ctx, network, block_hash) : nullptr;
      Value* block = BlockInfo(ctx, network, backend, block_hash);
      if ((pool && !tj::Set(arena, detail, "miner", tj::Clone(arena, pool))) ||
          (block && !tj::Set(arena, detail, "block", block))) {
        error->Upstream("arena exhausted");
        return false;
      }
    }
    *out = detail;
    return true;
  }

  // Pending: project inclusion from the mempool's next blocks.
  const double fee_rate = tx.vsize > 0 ? strtod(tx.fee_sats, nullptr) / tx.vsize : 0;
  bool has_blocks = false, beyond = false;
  double blocks = 0;
  if (backend == Backend::kEsplora) ProjectInclusion(ctx, network, fee_rate, &has_blocks, &blocks, &beyond);
  if ((has_blocks && !tj::Set(arena, detail, "expectedBlocks", tj::Num(arena, blocks))) ||
      !tj::Set(arena, detail, "expectedBeyond", tj::Bool(arena, beyond))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *out = detail;
  return true;
}

}  // namespace

bool EsploraView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  return ViewFetch(ctx, network, hash, Backend::kEsplora, detail, error);
}

bool BlockbookView::Fetch(Context& ctx, const Network& network, const char* hash, Value** detail, Error* error) const {
  return ViewFetch(ctx, network, hash, Backend::kBlockbook, detail, error);
}

namespace {

// isFinal: confirmed with the network's required depth (default 1).
bool IsFinal(const Value* detail, const Network& network) {
  const Value* transaction = tj::Get(detail, "detail");
  if (!IsObjectLike(transaction)) return false;
  const Value* confirmed = tj::Get(transaction, "confirmed");
  if (!confirmed || confirmed->type != tj::Type::kTrue) return false;
  const Value* confirmations = tj::Get(detail, "confirmations");
  const double depth = confirmations && confirmations->type != tj::Type::kNull ? JsNumber(confirmations) : 0;
  const double required = network.has_required_confirmations ? network.required_confirmations : 1;
  return depth >= required;
}

}  // namespace

void ProjectInclusionOf(double fee_rate, const Value* body, bool* has_blocks, double* blocks, bool* beyond) {
  *has_blocks = false;
  *beyond = false;
  if (!tj::IsArray(body)) return;
  double count = 0;
  for (uint32_t i = 0; i < body->count; ++i) {
    const Value* entry = body->items[i];
    const Value* range = IsObjectLike(entry) ? tj::Get(entry, "feeRange") : nullptr;
    if (!tj::IsArray(range)) continue;
    // range.map(Number).filter(Number.isFinite)[0], dropping empty ranges.
    bool found = false;
    double min = 0;
    for (uint32_t j = 0; j < range->count && !found; ++j) {
      const double value = JsNumber(range->items[j]);
      if (isfinite(value)) { found = true; min = value; }
    }
    if (!found) continue;
    ++count;
    if (!*has_blocks && fee_rate >= min) {
      *has_blocks = true;
      *blocks = count;
    }
  }
  if (count == 0) return;  // no ranges: {blocks: null, beyond: false}
  if (!*has_blocks) {
    *has_blocks = true;
    *blocks = count;
    *beyond = true;
  }
}

const EsploraView kEsploraView{};
const BlockbookView kDogecoinView{Family::kDogecoin};
const BlockbookView kLitecoinView{Family::kLitecoin};
const BlockbookView kZcashView{Family::kZcash};


bool EsploraView::IsFinal(const Value* detail, const Network& network) const { return ::fortis::chains::IsFinal(detail, network); }
bool BlockbookView::IsFinal(const Value* detail, const Network& network) const { return ::fortis::chains::IsFinal(detail, network); }

}  // namespace fortis::chains
