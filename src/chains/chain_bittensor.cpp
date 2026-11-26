// Bittensor: the TypeScript Bittensor driver's balances (Taostats account
// snapshot) and its transfer source (Taostats transfers), ported for the
// address routes. Both exist only when TAOSTATS_API_KEY is set.
// The driver's tokenMetadata/tokenMarket/tokenInsight belong to the token
// routes and are not part of this port.
#include "chain_family_drivers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

constexpr const char* kBase = "https://api.taostats.io";
const char* const kBaseUrls[] = {kBase};
constexpr double kPageLimit = 200;  // Taostats caps `limit` at 200
constexpr double kMaxPages = 3;

bool base58_char(char c) {
  return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') || (c >= 'P' && c <= 'Z') ||
         (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}

// /^[1-9A-HJ-NP-Za-km-z]{46,48}$/
bool ss58_address(const char* text, size_t length) {
  if (length < 46 || length > 48) return false;
  for (size_t i = 0; i < length; ++i)
    if (!base58_char(text[i])) return false;
  return true;
}

bool keyed(const config::ProviderRuntimeConfig& config) { return config.taostats_api_key_configured; }

bool nullish(const Value* v) { return !v || v->type == tj::Type::kNull; }

}  // namespace

bool BittensorDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                  Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!ss58_address(address, strlen(address))) {
    error->Range("not an SS58 address: %s", address);
    return false;
  }
  const char* symbol = network.has_native ? network.native_symbol : "TAO";
  const uint32_t decimals = network.has_native ? network.native_decimals : 9;
  const char* encoded = EncodeUri(a, address);
  Value* path = encoded ? Format(a, "/api/account/latest/v1?address=%s", encoded) : nullptr;
  if (!path) { error->Upstream("taostats: arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.path = tj::CString(a, path);
  call.lane = "taostats";
  call.headers[0] = CallHeader{"Authorization", ctx.config->taostats_api_key};
  call.header_count = 1;
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  // `body.data?.[0]`: a null body throws.
  if (nullish(call.value)) { error->Upstream("taostats: account body is null"); return false; }
  const Value* data = tj::Get(call.value, "data");
  const Value* row = nullptr;
  if (tj::IsArray(data)) row = data->count ? data->items[0] : nullptr;
  else if (tj::IsObject(data)) row = tj::Get(data, "0");
  // Portfolio snapshots include delegated/staked TAO.
  const Value* candidate = tj::Get(row, "balance_total");
  if (nullish(candidate)) candidate = tj::Get(row, "balance_free");
  if (nullish(candidate)) candidate = tj::Get(row, "balance");
  Value* raw = nullptr;
  if (tj::IsString(candidate) && candidate->length) {
    bool digits = true;
    for (uint32_t i = 0; i < candidate->length; ++i) digits = digits && candidate->text[i] >= '0' && candidate->text[i] <= '9';
    if (digits) raw = tj::Str(a, candidate->text, candidate->length);
  } else if (tj::IsNumber(candidate) && trunc(candidate->number) == candidate->number &&
             fabs(candidate->number) <= 9007199254740991.0) {
    char text[40];
    JsNumberText(candidate->number, text, sizeof(text));
    raw = tj::Str(a, text);
  }
  if (!raw) raw = tj::Str(a, "0");
  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!raw || !native || !out || !tj::Set(a, native, "balanceRaw", raw) ||
      !tj::Set(a, native, "decimals", tj::Num(a, decimals)) ||
      !tj::Set(a, native, "symbol", tj::Str(a, symbol ? symbol : "")) ||
      !tj::Set(a, native, "source", tj::Str(a, "taostats")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", tj::Arr(a))) {
    error->Upstream("taostats: arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

// ---- Taostats transfer source -----------------------------------------------------

struct Walk {
  Context* ctx;
  const Network* network;
  const char* address;
  const char* holder_hex;  // null when the state has none
  Value* records;
  bool failed;
};

bool page(void* opaque, double index, const char*, HeightWalkPage* out, Error* error) {
  auto* w = static_cast<Walk*>(opaque);
  Arena& a = *w->ctx->arena;
  const char* encoded = EncodeUri(a, w->address);
  char page_text[40];
  JsNumberText(index + 1, page_text, sizeof(page_text));  // Taostats pages are 1-based
  Value* path = encoded ? Format(a, "/api/transfer/v1?address=%s&network=finney&order=timestamp_desc&page=%s&limit=200",
                                 encoded, page_text)
                        : nullptr;
  if (!path) { error->Upstream("taostats: arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kGet;
  call.urls = kBaseUrls;
  call.url_count = 1;
  call.path = tj::CString(a, path);
  call.lane = "taostats";
  call.headers[0] = CallHeader{"authorization", w->ctx->config->taostats_api_key};
  call.header_count = 1;
  w->ctx->transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* data = tj::Get(call.value, "data");
  out->rows = tj::IsArray(data) ? data : tj::Arr(a);
  if (!out->rows) { error->Upstream("taostats: arena exhausted"); return false; }
  const Value* next = tj::Get(tj::Get(call.value, "pagination"), "next_page");
  out->more = out->rows->count == kPageLimit && !nullish(next);
  out->cursor = nullptr;
  return true;
}

bool height_of(void* opaque, const Value* row, double* height) {
  auto* w = static_cast<Walk*>(opaque);
  if (nullish(row)) { w->failed = true; return false; }  // a property read of null throws
  const double value = JsNumber(tj::Get(row, "block_number"));
  if (!isfinite(value)) return false;
  *height = value;
  return true;
}

// isHolder(party, ss58, hex). False in *throws for a null party.
bool is_holder(const Value* party, const char* ss58, const char* hex, bool* throws) {
  if (!party) return false;
  if (party->type == tj::Type::kNull) { *throws = true; return false; }
  const Value* id = tj::Get(party, "ss58");
  if (id && Equals(id, ss58)) return true;
  const Value* party_hex = tj::Get(party, "hex");
  if (!hex || !tj::IsString(party_hex) || party_hex->length != strlen(hex)) return false;
  for (uint32_t i = 0; i < party_hex->length; ++i) {
    char c = party_hex->text[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != hex[i]) return false;
  }
  return true;
}

// new Date(value).getTime() for the shapes a JSON timestamp can take.
double date_value(const Value* v) {
  if (v->type == tj::Type::kNull || v->type == tj::Type::kFalse) return 0;
  if (v->type == tj::Type::kTrue) return 1;
  if (v->type == tj::Type::kNumber) {
    if (!isfinite(v->number) || fabs(v->number) > 8.64e15) return NAN;
    return trunc(v->number) + 0.0;
  }
  if (v->type == tj::Type::kString) {
    double ms = 0;
    return ParseTime(v->text, v->length, &ms) ? ms : NAN;
  }
  return NAN;
}

// `row.<side>?.ss58 ?? null` as a record value.
Value* ss58_of(Arena& a, const Value* party) {
  const Value* id = tj::Get(party, "ss58");
  return nullish(id) ? tj::Null(a) : tj::Clone(a, id);
}

void on_row(void* opaque, Value* row) {
  auto* w = static_cast<Walk*>(opaque);
  Arena& a = *w->ctx->arena;
  if (w->failed) return;
  // Same identifier fallback chain the app uses: a row without any is unusable.
  const Value* hash = tj::Get(row, "transaction_hash");
  if (nullish(hash)) hash = tj::Get(row, "extrinsic_id");
  if (nullish(hash)) hash = tj::Get(row, "id");
  const Value* amount = IntegerString(a, tj::Get(row, "amount"));
  const Value* timestamp = tj::Get(row, "timestamp");
  if (!tj::IsString(hash) || !tj::IsString(amount) || !timestamp) return;
  const double when = date_value(timestamp);
  if (isnan(when)) return;

  bool throws = false;
  const Value* to = tj::Get(row, "to");
  const Value* from = tj::Get(row, "from");
  const bool incoming = is_holder(to, w->address, w->holder_hex, &throws);
  const bool outgoing = is_holder(from, w->address, w->holder_hex, &throws);
  if (throws) { w->failed = true; return; }
  if (!incoming && !outgoing) return;  // not this holder's row
  const char* direction = DirectionFromFlow(incoming, outgoing);
  const bool inbound = strcmp(direction, "incoming") == 0;
  const double height = JsNumber(tj::Get(row, "block_number"));
  Value* fee = inbound ? tj::Null(a) : IntegerString(a, tj::Get(row, "fee"));
  const bool has_fee = tj::IsString(fee);
  const Network& network = *w->network;
  const uint32_t decimals = network.has_native ? network.native_decimals : 9;
  const char* symbol = network.has_native && network.native_symbol ? network.native_symbol : "TAO";
  Value* r = tj::Obj(a);
  const bool ok = r && fee &&
      tj::Set(a, r, "recordId", Format(a, "%s:%.*s", network.id, static_cast<int>(hash->length), hash->text)) &&
      tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection(direction))) &&
      // Taostats only indexes included extrinsics.
      tj::Set(a, r, "status", tj::Str(a, "confirmed")) && tj::Set(a, r, "direction", tj::Str(a, direction)) &&
      tj::Set(a, r, "ts", tj::Num(a, when)) && tj::Set(a, r, "hash", tj::Clone(a, hash)) &&
      tj::Set(a, r, "blockNumber", isfinite(height) ? tj::Num(a, height) : tj::Null(a)) &&
      tj::Set(a, r, "fromAddress", ss58_of(a, from)) && tj::Set(a, r, "toAddress", ss58_of(a, to)) &&
      tj::Set(a, r, "counterparty", inbound ? ss58_of(a, from) : ss58_of(a, to)) &&
      tj::Set(a, r, "tokenId", tj::Str(a, "native")) && tj::Set(a, r, "asset", NativeAssetJson(a, network)) &&
      tj::Set(a, r, "amountRaw", tj::Clone(a, amount)) && tj::Set(a, r, "amountDecimals", tj::Num(a, decimals)) &&
      tj::Set(a, r, "feeRaw", fee) && tj::Set(a, r, "feeDecimals", has_fee ? tj::Num(a, decimals) : tj::Null(a)) &&
      tj::Set(a, r, "feeSymbol", has_fee ? tj::Str(a, symbol) : tj::Null(a)) &&
      tj::Set(a, r, "memo", tj::Null(a)) && tj::Set(a, r, "method", tj::Null(a)) &&
      tj::Set(a, r, "error", tj::Null(a)) && tj::Set(a, r, "metadata", tj::Obj(a)) && tj::Push(a, w->records, r);
  if (!ok) w->failed = true;
}

}  // namespace

bool BittensorSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state, double backfill_depth,
               SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* last = tj::Get(state, "lastHeight");
  const double since = tj::IsNumber(last) ? last->number : 0;
  const double budget = isnan(backfill_depth) ? NAN : fmax(1, fmin(backfill_depth, kPageLimit * kMaxPages));
  const Value* hex = tj::Get(state, "hex");
  char* holder_hex = nullptr;
  if (tj::IsString(hex)) {
    holder_hex = a.CopyText(hex->text, hex->length);
    if (!holder_hex) { error->Upstream("taostats: arena exhausted"); return false; }
    for (char* c = holder_hex; *c; ++c)
      if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c - 'A' + 'a');
  }
  Walk w{&ctx, &network, address, holder_hex, tj::Arr(a), false};
  if (!w.records) { error->Upstream("taostats: arena exhausted"); return false; }
  HeightWalkResult walked;
  if (!WalkDescendingByHeight(since, budget, kMaxPages, page, height_of, on_row, &w, &walked, error)) return false;
  if (w.failed) { error->Upstream("taostats: malformed rows or arena exhausted"); return false; }
  Value* next = tj::IsObject(state) ? tj::Clone(a, state) : tj::Obj(a);
  if (!next || !tj::Set(a, next, "lastHeight", tj::Num(a, walked.high_water))) {
    error->Upstream("taostats: arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = w.records;
  result->state = next;
  result->backfill_complete = walked.complete;
  return true;
}

namespace {

bool tao_supports(const config::ProviderRuntimeConfig&, const Network& network) { return !network.testnet; }

}  // namespace

// Keyed-only: the Substrate storage path is not implemented, so an unkeyed
// deployment truthfully reports balances:none for bittensor.
const BittensorDriver kBittensorDriver{};

// Taostats is the ONLY Bittensor history index: no key, no capability.
const BittensorSource kBittensorSource{};


bool BittensorDriver::Registered(const config::ProviderRuntimeConfig& config) const { return keyed(config); }
bool BittensorSource::Registered(const config::ProviderRuntimeConfig& config) const { return keyed(config); }
bool BittensorSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return tao_supports(config, network); }

}  // namespace fortis::chains
