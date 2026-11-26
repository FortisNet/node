#include "chain_family_drivers.h"
#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// Internet Computer (the TypeScript IcpDriver and IcpTransferSource): the
// keyless dashboard Ledger API balance read (404 = never funded) and the
// descending stop-at-height walk over its offset pages. Native ICP only.
// Guest-safe.
namespace fortis::chains {
namespace {

constexpr double kPageLimit = 100;
constexpr double kMaxPages = 3;

bool digits_only(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// /^[0-9a-f]{64}$/
bool valid_account(const char* address) {
  if (strlen(address) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    const char c = address[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// `/^\d+$/.test(value)` coerces a non-string with String(value).
bool regex_digits(const Value* v) {
  if (!v) return false;  // String(undefined) = "undefined"
  if (tj::IsString(v)) return digits_only(v->text, v->length);
  if (tj::IsNumber(v)) {
    char text[64];
    JsNumberText(v->number, text, sizeof(text));
    return digits_only(text, strlen(text));
  }
  return false;  // null / booleans / objects never stringify to digits
}

bool icp_present(const config::ProviderRuntimeConfig&) { return true; }

}  // namespace

bool IcpDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                  Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!valid_account(address)) {
    error->Range("not an ICP account identifier: %s", address);
    return false;
  }
  const Value* path = Format(a, "/accounts/%s", address);
  if (!path) { error->Upstream("arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = path->text;
  ctx.transport->Run(call, a);
  Value* raw = tj::Str(a, "0");
  if (call.ok()) {
    if (!call.value || call.value->type == tj::Type::kNull) {
      error->Upstream("icp ledger: null body");  // account.balance throws
      return false;
    }
    const Value* balance = tj::Get(call.value, "balance");
    if (tj::IsString(balance) && digits_only(balance->text, balance->length)) {
      raw = tj::Str(a, balance->text, balance->length);
    } else if (tj::IsNumber(balance) && isfinite(balance->number) && trunc(balance->number) == balance->number &&
               fabs(balance->number) <= 9007199254740991.0) {
      char text[64];
      JsNumberText(balance->number, text, sizeof(text));
      raw = tj::Str(a, text);
    }
  } else if (!(call.answer() && call.http_status == 404)) {
    call.ToError(error);
    return false;
  }  // 404: never funded, a real zero

  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!raw || !native || !out || !tj::Set(a, native, "balanceRaw", raw) ||
      !tj::Set(a, native, "decimals", tj::Num(a, network.has_native ? network.native_decimals : 8)) ||
      !tj::Set(a, native, "symbol", tj::Str(a, network.has_native ? network.native_symbol : "ICP")) ||
      !tj::Set(a, native, "source", tj::Str(a, "ledger-api")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", tj::Arr(a))) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

bool icp_supports(const config::ProviderRuntimeConfig&, const Network& network) {
  return !network.testnet && network.rpc_url_count > 0;
}

struct Walk {
  Context* ctx;
  const Network* network;
  const char* account;
  Value* records;
  bool failed;
  Error* error;
};

bool fetch_page(void* opaque, double index, const char*, HeightWalkPage* page, Error* error) {
  Walk& w = *static_cast<Walk*>(opaque);
  Arena& a = *w.ctx->arena;
  char offset[32];
  JsNumberText(index * kPageLimit, offset, sizeof(offset));
  const Value* path = Format(a, "/accounts/%s/transactions?limit=100&offset=%s", w.account, offset);
  if (!path) { error->Upstream("arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = w.network->rpc_urls;
  call.url_count = w.network->rpc_url_count;
  call.path = path->text;
  w.ctx->transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* blocks = tj::Get(call.value, "blocks");
  if (!tj::IsArray(blocks)) blocks = tj::Arr(a);
  if (!blocks) { error->Upstream("arena exhausted"); return false; }
  for (uint32_t i = 0; i < blocks->count; ++i) {
    if (!blocks->items[i] || blocks->items[i]->type == tj::Type::kNull) {
      error->Upstream("icp ledger: null block");  // block.block_height throws
      return false;
    }
  }
  page->rows = blocks;
  // A short page IS the end of this account's history.
  page->more = blocks->count == kPageLimit;
  page->cursor = nullptr;
  return true;
}

bool height_of(void*, const Value* row, double* height) {
  *height = JsNumber(tj::Get(row, "block_height"));
  return isfinite(*height);
}

// toLowerCase of an optional string member (?.toLowerCase() ?? null).
Value* lower_or_null(Arena& a, const Value* v, bool* failed) {
  if (!v || v->type == tj::Type::kNull) return tj::Null(a);
  if (!tj::IsString(v)) { *failed = true; return nullptr; }  // .toLowerCase is not a function
  return compat::token_text::ToLowerCase(a, v->text, v->length);
}

// The ledger's transfer_type in the app's vocabulary.
const char* type_for(const Value* transfer_type, const char* direction) {
  if (tj::StringEquals(transfer_type, "mint")) return "mint";
  if (tj::StringEquals(transfer_type, "burn")) return "burn";
  if (tj::StringEquals(transfer_type, "approve")) return "approve";
  return TypeFromDirection(direction);
}

void on_row(void* opaque, Value* block) {
  Walk& w = *static_cast<Walk*>(opaque);
  if (w.failed) return;
  Arena& a = *w.ctx->arena;
  const Network& network = *w.network;
  const Value* hash = tj::Get(block, "transaction_hash");
  const Value* amount = tj::Get(block, "amount");
  if (!tj::IsString(hash) || !amount || !regex_digits(amount)) return;
  bool failed = false;
  Value* from = lower_or_null(a, tj::Get(block, "from_account_identifier"), &failed);
  Value* to = lower_or_null(a, tj::Get(block, "to_account_identifier"), &failed);
  if (failed) {
    w.failed = true;
    w.error->Upstream("icp ledger: non-string account identifier");
    return;
  }
  if (!from || !to) { w.failed = true; w.error->Upstream("arena exhausted"); return; }
  const bool incoming = tj::StringEquals(to, w.account);
  const bool outgoing = tj::StringEquals(from, w.account);
  const char* direction = DirectionFromFlow(incoming, outgoing);
  const double height = JsNumber(tj::Get(block, "block_height"));
  const bool is_incoming = strcmp(direction, "incoming") == 0;
  // The ledger bills the fee to the sender; a received transfer paid none.
  const Value* fee = tj::Get(block, "fee");
  const bool has_fee = !is_incoming && fee && regex_digits(fee);
  // `memo: '0'` is the ledger's "no memo" default.
  const Value* memo = tj::Get(block, "memo");
  const bool no_memo = !memo || tj::StringEquals(memo, "0");
  const Value* created = tj::Get(block, "created_at");
  // new Date((created_at ?? 0) * 1000): JavaScript multiplication coercion.
  const double seconds = !created || created->type == tj::Type::kNull ? 0 : JsNumber(created);
  const double ms = seconds * 1000;
  if (!isfinite(ms) || fabs(ms) > 8.64e15) {
    w.failed = true;  // an Invalid Date throws when the row is serialized
    w.error->Upstream("icp ledger: invalid created_at");
    return;
  }
  const double decimals = network.has_native ? network.native_decimals : 8;
  const char* symbol = network.has_native ? network.native_symbol : "ICP";
  Value* r = NewTransfer(a);
  const Value* record_id = Format(a, "%s:%.*s", network.id, static_cast<int>(hash->length), hash->text);
  const bool ok =
      r && record_id && tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) &&
      tj::Set(a, r, "txType", tj::Str(a, type_for(tj::Get(block, "transfer_type"), direction))) &&
      tj::Set(a, r, "status", tj::Str(a, "confirmed")) && tj::Set(a, r, "direction", tj::Str(a, direction)) &&
      tj::Set(a, r, "ts", tj::Num(a, trunc(ms))) && tj::Set(a, r, "hash", tj::Clone(a, hash)) &&
      tj::Set(a, r, "blockNumber", isfinite(height) ? tj::Num(a, height) : tj::Null(a)) &&
      tj::Set(a, r, "fromAddress", from) && tj::Set(a, r, "toAddress", to) &&
      tj::Set(a, r, "counterparty", tj::Clone(a, is_incoming ? from : to)) &&
      tj::Set(a, r, "tokenId", tj::Str(a, "native")) && tj::Set(a, r, "asset", NativeAssetJson(a, network)) &&
      tj::Set(a, r, "amountRaw", tj::Clone(a, amount)) && tj::Set(a, r, "amountDecimals", tj::Num(a, decimals)) &&
      tj::Set(a, r, "feeRaw", has_fee ? tj::Clone(a, fee) : tj::Null(a)) &&
      tj::Set(a, r, "feeDecimals", has_fee ? tj::Num(a, decimals) : tj::Null(a)) &&
      tj::Set(a, r, "feeSymbol", has_fee ? tj::Str(a, symbol) : tj::Null(a)) &&
      tj::Set(a, r, "memo", no_memo ? tj::Null(a) : tj::Clone(a, memo)) &&
      tj::Set(a, r, "method", tj::Null(a)) && tj::Set(a, r, "error", tj::Null(a)) &&
      tj::Set(a, r, "metadata", tj::Obj(a)) && tj::Push(a, w.records, r);
  if (!ok) { w.failed = true; w.error->Upstream("arena exhausted"); }
}

}  // namespace

bool IcpSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
               double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  Value* account = compat::token_text::ToLowerCase(a, address, strlen(address));
  const char* account_text = account ? tj::CString(a, account) : nullptr;
  const Value* last = tj::Get(state, "lastHeight");
  const double since_height = tj::IsNumber(last) ? last->number : 0;
  double budget = backfill_depth < kPageLimit * kMaxPages ? backfill_depth : kPageLimit * kMaxPages;
  if (!(budget >= 1)) budget = 1;
  Value* records = tj::Arr(a);
  if (!account_text || !records) { error->Upstream("arena exhausted"); return false; }
  Walk walk{&ctx, &network, account_text, records, false, error};
  HeightWalkResult walked;
  if (!WalkDescendingByHeight(since_height, budget, kMaxPages, fetch_page, height_of, on_row, &walk, &walked,
                              error))
    return false;
  if (walk.failed) return false;
  Value* next = tj::IsObject(state) ? tj::Clone(a, state) : tj::Obj(a);
  if (!next || !tj::Set(a, next, "lastHeight", tj::Num(a, walked.high_water))) {
    error->Upstream("arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = walked.complete;
  return true;
}

const IcpDriver kIcpDriver{};

const IcpSource kIcpSource{};


bool IcpDriver::Registered(const config::ProviderRuntimeConfig& config) const { return icp_present(config); }
bool IcpSource::Registered(const config::ProviderRuntimeConfig& config) const { return icp_present(config); }
bool IcpSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return icp_supports(config, network); }

}  // namespace fortis::chains
