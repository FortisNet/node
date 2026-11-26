// VeChain: the TypeScript VeChain driver (Thor balances + VIP-180 view calls)
// and its transfer source (Thor transfer/event logs), ported for the address
// routes.
#include "chain_family_drivers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

constexpr size_t kMaxTokens = 20;
// Three view calls per token; a Run holds at most kTransportMaxCalls.
constexpr size_t kTokensPerRun = kTransportMaxCalls / 3;
constexpr double kPageLimit = 100;
constexpr int kMaxPages = 3;
constexpr double kMaxBlock = 2000000000;  // Thor wants a bounded range; ~far future

Call* new_calls(Arena& arena, size_t count) {
  auto* calls = static_cast<Call*>(arena.Alloc(sizeof(Call) * count));
  if (!calls) return nullptr;
  for (size_t i = 0; i < count; ++i) new (&calls[i]) Call();
  return calls;
}

// /^0x[0-9a-fA-F]{40}$/
bool vet_address(const char* text, size_t length) {
  return length == 42 && text[0] == '0' && text[1] == 'x' && IsHex(text + 2, 40);
}

void lower(char* text) {
  for (; *text; ++text)
    if (*text >= 'A' && *text <= 'Z') *text = static_cast<char>(*text - 'A' + 'a');
}

const char* lower_copy(Arena& a, const char* text) {
  const size_t length = strlen(text);
  char* copy = a.CopyText(text, length);
  if (copy) lower(copy);
  return copy;
}

// ---- balances -----------------------------------------------------------------

// One Thor view call: POST /accounts/{contract} {value:'0x0', data}.
bool view_call(Arena& a, const Network& network, const char* contract, const char* data, Call* call) {
  Value* body = tj::Obj(a);
  Value* path = Format(a, "/accounts/%s", contract);
  if (!body || !path || !tj::Set(a, body, "value", tj::Str(a, "0x0")) || !tj::Set(a, body, "data", tj::Str(a, data)))
    return false;
  call->kind = CallKind::kPost;
  call->urls = network.rpc_urls;
  call->url_count = network.rpc_url_count;
  call->path = tj::CString(a, path);
  call->body = body;
  call->retry_post = true;  // a Thor view call is an idempotent read
  return call->path != nullptr;
}

enum class View : uint8_t { kData, kNull, kThrow };

// view(): the returned data hex, null (reverted or empty), or a throw (the
// request failed, or a null body whose property read throws).
View view_result(const Call& call, const char** data, size_t* length) {
  if (!call.ok()) return View::kThrow;
  const Value* body = call.value;
  if (!body || body->type == tj::Type::kNull) return View::kThrow;
  const Value* reverted = tj::Get(body, "reverted");
  if (reverted && reverted->type == tj::Type::kTrue) return View::kNull;
  const Value* hex = tj::Get(body, "data");
  if (!tj::IsString(hex) || Equals(hex, "0x")) return View::kNull;
  *data = hex->text;
  *length = hex->length;
  return View::kData;
}

}  // namespace

bool VechainDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                      size_t token_count, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!vet_address(address, strlen(address))) {
    error->Range("not a VeChain address: %s", address);
    return false;
  }
  const char* symbol = network.has_native ? network.native_symbol : "VET";
  const uint32_t decimals = network.has_native ? network.native_decimals : 18;

  const char* holder = lower_copy(a, address);
  Value* account_path = holder ? Format(a, "/accounts/%s", holder) : nullptr;
  if (!account_path) { error->Upstream("thor: arena exhausted"); return false; }
  Call account;
  account.kind = CallKind::kGet;
  account.urls = network.rpc_urls;
  account.url_count = network.rpc_url_count;
  account.path = tj::CString(a, account_path);
  ctx.transport->Run(account, a);
  if (!account.ok()) { account.ToError(error); return false; }

  // erc20:0x… ids, first 20 (contract spelling kept for the request path).
  const char* contracts[kMaxTokens];
  size_t contract_count = 0;
  for (size_t i = 0; i < token_count && contract_count < kMaxTokens; ++i) {
    const char* t = tokens[i];
    if (strncmp(t, "erc20:", 6) != 0 || !vet_address(t + 6, strlen(t + 6))) continue;
    contracts[contract_count++] = t + 6;
  }

  Value* list = tj::Arr(a);
  const char* balance_of = AbiBalanceOf(a, address);
  const char* decimals_call = AbiDecimals(a);
  const char* symbol_call = AbiSymbol(a);
  if (!list || !balance_of || !decimals_call || !symbol_call) {
    error->Upstream("thor: arena exhausted");
    return false;
  }
  for (size_t start = 0; start < contract_count; start += kTokensPerRun) {
    const size_t n = contract_count - start < kTokensPerRun ? contract_count - start : kTokensPerRun;
    Call* calls = new_calls(a, n * 3);
    bool built = calls != nullptr;
    for (size_t i = 0; built && i < n; ++i) {
      const char* contract = lower_copy(a, contracts[start + i]);
      built = contract && view_call(a, network, contract, balance_of, &calls[i * 3]) &&
              view_call(a, network, contract, decimals_call, &calls[i * 3 + 1]) &&
              view_call(a, network, contract, symbol_call, &calls[i * 3 + 2]);
    }
    if (!built) { error->Upstream("thor: arena exhausted"); return false; }
    ctx.transport->Run(calls, n * 3, a);
    for (size_t i = 0; i < n; ++i) {
      const char* hex[3] = {};
      size_t length[3] = {};
      View views[3];
      bool thrown = false;
      for (int k = 0; k < 3; ++k) {
        views[k] = view_result(calls[i * 3 + k], &hex[k], &length[k]);
        thrown = thrown || views[k] == View::kThrow;
      }
      // One token failing must not sink the rest; never mis-scale.
      if (thrown || views[0] != View::kData || views[1] != View::kData) continue;
      Value* token_symbol = tj::Null(a);
      if (views[2] == View::kData) {
        const Value* raw = AbiString(a, hex[2], length[2]);
        if (tj::IsString(raw) && raw->length) token_symbol = SanitizeSymbol(a, raw->text, raw->length);
      }
      const BigInt balance = AbiUint256(hex[0], length[0]);
      const BigInt token_decimals = AbiUint256(hex[1], length[1]);
      char* contract = a.CopyText(contracts[start + i], strlen(contracts[start + i]));
      if (contract) lower(contract);
      Value* entry = tj::Obj(a);
      if (!contract || !entry || !tj::Set(a, entry, "tokenId", Format(a, "erc20:%s", contract)) ||
          !tj::Set(a, entry, "balanceRaw", balance.ToValue(a)) ||
          !tj::Set(a, entry, "decimals", tj::Num(a, token_decimals.ToDouble())) ||
          !tj::Set(a, entry, "symbol", token_symbol) || !tj::Set(a, entry, "source", tj::Str(a, "thor")) ||
          !tj::Push(a, list, entry)) {
        error->Upstream("thor: arena exhausted");
        return false;
      }
    }
  }

  // `account.balance` is read last: a null body throws only here.
  if (!account.value || account.value->type == tj::Type::kNull) {
    error->Upstream("thor: account body is null");
    return false;
  }
  const Value* hex = tj::Get(account.value, "balance");
  Value* raw = nullptr;
  BigInt wei;
  if (tj::IsString(hex) && hex->length > 2 && hex->text[0] == '0' && hex->text[1] == 'x' &&
      IsHex(hex->text + 2, hex->length - 2) && BigInt::ParseHex(hex->text + 2, hex->length - 2, &wei))
    raw = wei.ToValue(a);
  else
    raw = tj::Str(a, "0");
  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!raw || !native || !out || !tj::Set(a, native, "balanceRaw", raw) ||
      !tj::Set(a, native, "decimals", tj::Num(a, decimals)) ||
      !tj::Set(a, native, "symbol", tj::Str(a, symbol ? symbol : "")) ||
      !tj::Set(a, native, "source", tj::Str(a, "thor")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", list)) {
    error->Upstream("thor: arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

bool always(const config::ProviderRuntimeConfig&) { return true; }

// ---- Thor log transfer source ---------------------------------------------------

// `${value}` of a JSON value (undefined = nullptr).
void append_template(OutBuffer& out, const Value* v) {
  if (!v) { out.Text("undefined"); return; }
  switch (v->type) {
    case tj::Type::kNull: out.Text("null"); return;
    case tj::Type::kTrue: out.Text("true"); return;
    case tj::Type::kFalse: out.Text("false"); return;
    case tj::Type::kNumber: {
      char text[40];
      JsNumberText(v->number, text, sizeof(text));
      out.Text(text);
      return;
    }
    case tj::Type::kString: out.Append(v->text, v->length); return;
    case tj::Type::kArray:
      // Array.prototype.join(','): null/undefined elements are empty.
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Text(",");
        const Value* item = v->items[i];
        if (item && item->type != tj::Type::kNull) append_template(out, item);
      }
      return;
    case tj::Type::kObject: out.Text("[object Object]"); return;
  }
}

// The holder's movements of one asset in one transaction.
struct Leg {
  const char* record_id;
  const char* hash;
  const Value* meta;     // log.meta ?? {}
  const char* contract;  // null for native VET
  BigInt net;            // in minus out, excluding transfers to itself
  BigInt self;           // moved from the holder to the holder
  const char* counterparty;
  BigInt largest;
};

struct State {
  Arena* arena;
  const char* holder;
  Leg* legs;
  size_t leg_count;
  size_t leg_capacity;
  const char** seen;
  size_t seen_count;
  size_t seen_capacity;
  bool have_resume;
  double resume_at;
  bool complete;
  bool failed;
};

// walked(): a walk cut by its budget resumes at its last block.
void walked(State& s, const Value* logs, bool exhausted) {
  if (exhausted) return;
  s.complete = false;
  double last = -INFINITY;
  for (uint32_t i = 0; i < logs->count; ++i) {
    const Value* block = tj::Get(tj::Get(logs->items[i], "meta"), "blockNumber");
    const double value = !block || block->type == tj::Type::kNull ? 0 : JsNumber(block);
    last = isnan(value) || isnan(last) ? NAN : fmax(last, value);
  }
  if (!s.have_resume || last < s.resume_at) { s.have_resume = true; s.resume_at = last; }
}

// `${kind}:${meta.txID}:${meta.clauseIndex ?? ''}:${position}`.
const char* log_identity(Arena& a, const char* kind, const Value* log, const char* fallback) {
  const Value* meta = tj::Get(log, "meta");
  uint8_t bytes[1024];
  OutBuffer out{bytes, sizeof(bytes), 0, true};
  out.Text(kind);
  out.Text(":");
  append_template(out, tj::Get(meta, "txID"));
  out.Text(":");
  const Value* clause = tj::Get(meta, "clauseIndex");
  if (clause && clause->type != tj::Type::kNull) append_template(out, clause);
  out.Text(":");
  const Value* log_index = tj::Get(meta, "logIndex");
  if (tj::IsNumber(log_index)) {
    const Value* tx_index = tj::Get(meta, "txIndex");
    if (tx_index && tx_index->type != tj::Type::kNull) append_template(out, tx_index);
    out.Text(":");
    append_template(out, log_index);
  } else {
    out.Text(fallback);
  }
  if (!out.ok) return nullptr;
  return a.CopyText(reinterpret_cast<const char*>(bytes), out.used);
}

// add(): fold one log into its transaction/asset leg.
void add(State& s, const char* record_id, const Value* log, const char* contract, const char* from, const char* to,
         const BigInt& amount, const char* identity) {
  Arena& a = *s.arena;
  if (!record_id || !identity) { s.failed = true; return; }
  // The sender and recipient walks both return a self-transfer.
  for (size_t i = 0; i < s.seen_count; ++i)
    if (strcmp(s.seen[i], identity) == 0) return;
  if (s.seen_count == s.seen_capacity) { s.failed = true; return; }
  s.seen[s.seen_count++] = identity;
  const Value* meta = tj::Get(log, "meta");
  const Value* hash = tj::Get(meta, "txID");
  if (!tj::IsString(hash)) return;
  const bool into = to && strcmp(to, s.holder) == 0;
  const bool out_of = from && strcmp(from, s.holder) == 0;
  if (!into && !out_of) return;
  Leg* leg = nullptr;
  for (size_t i = 0; i < s.leg_count; ++i)
    if (strcmp(s.legs[i].record_id, record_id) == 0) { leg = &s.legs[i]; break; }
  if (!leg) {
    if (s.leg_count == s.leg_capacity) { s.failed = true; return; }
    leg = &s.legs[s.leg_count++];
    *leg = Leg{};
    leg->record_id = record_id;
    leg->hash = tj::CString(a, hash);
    leg->meta = meta && meta->type != tj::Type::kNull ? meta : tj::Obj(a);
    leg->contract = contract;
    if (!leg->hash || !leg->meta) { s.failed = true; return; }
  }
  if (into && out_of) {
    if (!leg->self.Add(amount)) s.failed = true;
    return;
  }
  if (!(into ? leg->net.Add(amount) : leg->net.Sub(amount))) s.failed = true;
  if (amount.Compare(leg->largest) > 0) {
    leg->largest = amount;
    leg->counterparty = into ? from : to;
  }
}

// hexToDecimalString(value) as a BigInt.
bool hex_big(Arena& a, const Value* value, BigInt* out) {
  const Value* text = HexToDecimalString(a, value);
  return tj::IsString(text) && BigInt::ParseDecimal(text->text, text->length, out);
}

// `value?.toLowerCase() ?? null`; false where the call throws (a non-string).
bool lower_or_null(Arena& a, const Value* value, const char** out) {
  *out = nullptr;
  if (!value || value->type == tj::Type::kNull) return true;
  if (!tj::IsString(value)) return false;
  char* copy = a.CopyText(value->text, value->length);
  if (!copy) return false;
  lower(copy);
  *out = copy;
  return true;
}

// topicToAddress: a 32-byte topic as its 20-byte address, or null.
const char* topic_address(Arena& a, const Value* topic) {
  if (!tj::IsString(topic) || topic->length != 66 || topic->text[0] != '0' || topic->text[1] != 'x' ||
      !IsHex(topic->text + 2, 64))
    return nullptr;
  char text[43];
  text[0] = '0';
  text[1] = 'x';
  memcpy(text + 2, topic->text + 26, 40);
  text[42] = '\0';
  lower(text);
  return a.CopyText(text, 42);
}

// The offset-paged log walk; `*exhausted` false only when the budget cut it.
bool walk(Context& ctx, const Network& network, const char* path, Value* range, Value* criterion, double budget,
          Value** logs, bool* exhausted, Error* error) {
  Arena& a = *ctx.arena;
  Value* all = tj::Arr(a);
  Value* criteria = tj::Arr(a);
  if (!all || !criteria || !tj::Push(a, criteria, criterion)) { error->Upstream("thor: arena exhausted"); return false; }
  *logs = all;
  for (int page = 0; page < kMaxPages; ++page) {
    Value* options = tj::Obj(a);
    Value* body = tj::Obj(a);
    if (!options || !body || !tj::Set(a, body, "range", range) || !tj::Set(a, body, "criteriaSet", criteria) ||
        !tj::Set(a, options, "offset", tj::Num(a, page * kPageLimit)) ||
        !tj::Set(a, options, "limit", tj::Num(a, kPageLimit)) ||
        !tj::Set(a, options, "includeIndexes", tj::Bool(a, true)) || !tj::Set(a, body, "options", options) ||
        !tj::Set(a, body, "order", tj::Str(a, "asc"))) {
      error->Upstream("thor: arena exhausted");
      return false;
    }
    Call call;
    call.kind = CallKind::kPost;
    call.urls = network.rpc_urls;
    call.url_count = network.rpc_url_count;
    call.path = path;
    call.body = body;
    // A log query is a read expressed as a POST: safe to retry.
    call.retry_post = true;
    ctx.transport->Run(call, a);
    if (!call.ok()) { call.ToError(error); return false; }
    const Value* batch = call.value;
    const uint32_t rows = tj::IsArray(batch) ? batch->count : 0;
    for (uint32_t i = 0; i < rows; ++i)
      if (!tj::Push(a, all, batch->items[i])) { error->Upstream("thor: arena exhausted"); return false; }
    if (rows < kPageLimit) { *exhausted = true; return true; }
    if (all->count >= budget) { *exhausted = false; return true; }
  }
  *exhausted = false;
  return true;
}

// legFlow + the record body shared by native and token rows.
bool leg_record(Arena& a, const Network& network, const char* holder, const Leg& leg, Value** record) {
  *record = nullptr;
  const char* direction;
  const char* from;
  const char* to;
  const char* counterparty;
  BigInt amount;
  if (leg.net.IsZero()) {
    if (leg.self.IsZero()) return true;  // in and out cancelled: nothing moved
    direction = DirectionFromFlow(true, true);
    from = to = counterparty = holder;
    amount = leg.self;
  } else {
    const bool incoming = !leg.net.negative();
    direction = DirectionFromFlow(incoming, !incoming);
    from = incoming ? leg.counterparty : holder;
    to = incoming ? holder : leg.counterparty;
    counterparty = leg.counterparty;
    amount = leg.net.Abs();
  }
  auto text_or_null = [&](const char* text) { return text ? tj::Str(a, text) : tj::Null(a); };
  const Value* block = tj::Get(leg.meta, "blockNumber");
  const Value* seconds = tj::Get(leg.meta, "blockTimestamp");
  const double ts = (!seconds || seconds->type == tj::Type::kNull ? 0 : JsNumber(seconds)) * 1000;
  const bool native = leg.contract == nullptr;
  Value* r = tj::Obj(a);
  // A reverted tx emits no logs, so anything here executed. VeChain bills gas
  // in VTHO, which the log does not carry: no fee rather than a wrong one.
  // Token decimals would need a per-contract view call the ingest path does
  // not make: a null asset shows the row without a mis-scaled amount.
  const bool ok = r && tj::Set(a, r, "recordId", tj::Str(a, leg.record_id)) &&
      tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection(direction))) &&
      tj::Set(a, r, "status", tj::Str(a, "confirmed")) && tj::Set(a, r, "direction", tj::Str(a, direction)) &&
      tj::Set(a, r, "ts", tj::Num(a, ts)) && tj::Set(a, r, "hash", tj::Str(a, leg.hash)) &&
      tj::Set(a, r, "blockNumber", block && block->type != tj::Type::kNull ? tj::Clone(a, block) : tj::Null(a)) &&
      tj::Set(a, r, "fromAddress", text_or_null(from)) && tj::Set(a, r, "toAddress", text_or_null(to)) &&
      tj::Set(a, r, "counterparty", text_or_null(counterparty)) &&
      tj::Set(a, r, "tokenId", native ? tj::Str(a, "native") : Format(a, "erc20:%s", leg.contract)) &&
      tj::Set(a, r, "asset", native ? NativeAssetJson(a, network) : tj::Null(a)) &&
      tj::Set(a, r, "amountRaw", amount.ToValue(a)) &&
      tj::Set(a, r, "amountDecimals",
              native ? tj::Num(a, network.has_native ? network.native_decimals : 18) : tj::Null(a)) &&
      tj::Set(a, r, "feeRaw", tj::Null(a)) && tj::Set(a, r, "feeDecimals", tj::Null(a)) &&
      tj::Set(a, r, "feeSymbol", tj::Null(a)) && tj::Set(a, r, "memo", tj::Null(a)) &&
      tj::Set(a, r, "method", native ? tj::Null(a) : tj::Str(a, "transfer")) &&
      tj::Set(a, r, "error", tj::Null(a)) && tj::Set(a, r, "metadata", tj::Obj(a)) &&
      tj::Set(a, r, "restatable", tj::Bool(a, true));
  if (!ok) return false;
  *record = r;
  return true;
}

}  // namespace

bool VechainSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                   double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const char* holder = lower_copy(a, address);
  const Value* last = tj::Get(state, "lastBlock");
  const double from_block = tj::IsNumber(last) ? last->number : 0;
  const double budget = isnan(backfill_depth) ? NAN : fmax(1, fmin(backfill_depth, kPageLimit * kMaxPages));
  Value* range = tj::Obj(a);
  if (!holder || !range || !tj::Set(a, range, "unit", tj::Str(a, "block")) ||
      !tj::Set(a, range, "from", tj::Num(a, from_block)) || !tj::Set(a, range, "to", tj::Num(a, kMaxBlock))) {
    error->Upstream("thor: arena exhausted");
    return false;
  }
  // Four walks of at most three 100-row pages each.
  const size_t capacity = 4 * kMaxPages * static_cast<size_t>(kPageLimit);
  State s{};
  s.arena = &a;
  s.holder = holder;
  s.legs = static_cast<Leg*>(a.Alloc(sizeof(Leg) * capacity));
  s.leg_capacity = capacity;
  s.seen = static_cast<const char**>(a.Alloc(sizeof(char*) * capacity));
  s.seen_capacity = capacity;
  s.complete = true;
  if (!s.legs || !s.seen) { error->Upstream("thor: arena exhausted"); return false; }

  // ── Native VET (two sides) ──
  static const char* const kSides[] = {"sender", "recipient"};
  for (const char* side : kSides) {
    Value* criterion = tj::Obj(a);
    if (!criterion || !tj::Set(a, criterion, side, tj::Str(a, holder))) {
      error->Upstream("thor: arena exhausted");
      return false;
    }
    Value* logs = nullptr;
    bool exhausted = false;
    if (!walk(ctx, network, "/logs/transfer", range, criterion, budget, &logs, &exhausted, error)) return false;
    walked(s, logs, exhausted);
    for (uint32_t i = 0; i < logs->count; ++i) {
      const Value* log = logs->items[i];
      if (!log || log->type == tj::Type::kNull) { error->Upstream("thor: null log"); return false; }
      BigInt amount;
      const Value* hash = tj::Get(tj::Get(log, "meta"), "txID");
      if (!hex_big(a, tj::Get(log, "amount"), &amount) || !tj::IsString(hash)) continue;
      const char* from = nullptr;
      const char* to = nullptr;
      if (!lower_or_null(a, tj::Get(log, "sender"), &from) || !lower_or_null(a, tj::Get(log, "recipient"), &to)) {
        error->Upstream("thor: non-string party");
        return false;
      }
      char decimal[200];
      if (!amount.ToDecimal(decimal, sizeof(decimal))) { error->Upstream("thor: amount"); return false; }
      Value* fallback = Format(a, "%s:%s:%s", from ? from : "null", to ? to : "null", decimal);
      Value* record_id = Format(a, "%s:%.*s", network.id, static_cast<int>(hash->length), hash->text);
      if (!fallback || !record_id) { error->Upstream("thor: arena exhausted"); return false; }
      add(s, record_id->text, log, nullptr, from, to, amount, log_identity(a, "transfer", log, fallback->text));
    }
  }

  // ── VIP-180 tokens (two topic positions) ──
  static const char* const kPositions[] = {"topic1", "topic2"};
  for (const char* position : kPositions) {
    Value* criterion = tj::Obj(a);
    if (!criterion || !tj::Set(a, criterion, "topic0", tj::Str(a, kTransferEventTopic)) ||
        !tj::Set(a, criterion, position, Format(a, "0x000000000000000000000000%s", holder + 2))) {
      error->Upstream("thor: arena exhausted");
      return false;
    }
    Value* logs = nullptr;
    bool exhausted = false;
    if (!walk(ctx, network, "/logs/event", range, criterion, budget, &logs, &exhausted, error)) return false;
    walked(s, logs, exhausted);
    for (uint32_t i = 0; i < logs->count; ++i) {
      const Value* log = logs->items[i];
      if (!log || log->type == tj::Type::kNull) { error->Upstream("thor: null log"); return false; }
      const Value* hash = tj::Get(tj::Get(log, "meta"), "txID");
      const char* contract = nullptr;
      if (!lower_or_null(a, tj::Get(log, "address"), &contract)) {
        error->Upstream("thor: non-string contract");
        return false;
      }
      BigInt amount;
      const bool has_amount = hex_big(a, tj::Get(log, "data"), &amount);
      const Value* topics = tj::Get(log, "topics");
      const Value* t1 = tj::IsArray(topics) && topics->count > 1 ? topics->items[1] : nullptr;
      const Value* t2 = tj::IsArray(topics) && topics->count > 2 ? topics->items[2] : nullptr;
      const char* from = topic_address(a, t1);
      const char* to = topic_address(a, t2);
      if (!tj::IsString(hash) || !contract || !has_amount) continue;
      if (!from && !to) continue;
      char decimal[200];
      if (!amount.ToDecimal(decimal, sizeof(decimal))) { error->Upstream("thor: amount"); return false; }
      // One tx can move several tokens: the contract makes each leg unique.
      Value* fallback =
          Format(a, "%s:%s:%s:%s", contract, from ? from : "null", to ? to : "null", decimal);
      Value* record_id = Format(a, "%s:%.*s:%s", network.id, static_cast<int>(hash->length), hash->text, contract);
      if (!fallback || !record_id) { error->Upstream("thor: arena exhausted"); return false; }
      add(s, record_id->text, log, contract, from, to, amount, log_identity(a, "event", log, fallback->text));
    }
  }
  if (s.failed) { error->Upstream("thor: leg bookkeeping exhausted"); return false; }

  // One block holding more of this address's logs than a whole poll can read
  // cannot be waited out; keep what was read rather than stall.
  if (s.have_resume && s.resume_at <= from_block) s.have_resume = false;

  double high_water = from_block;
  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("thor: arena exhausted"); return false; }
  for (size_t i = 0; i < s.leg_count; ++i) {
    const Leg& leg = s.legs[i];
    const Value* block = tj::Get(leg.meta, "blockNumber");
    const bool has_block = block && block->type != tj::Type::kNull;
    const double block_number = has_block ? JsNumber(block) : 0;
    if (s.have_resume && (!has_block || block_number >= s.resume_at)) continue;
    Value* record = nullptr;
    if (!leg_record(a, network, holder, leg, &record)) { error->Upstream("thor: arena exhausted"); return false; }
    if (!record) continue;
    if (has_block) high_water = isnan(block_number) || isnan(high_water) ? NAN : fmax(high_water, block_number);
    if (!tj::Push(a, records, record)) { error->Upstream("thor: arena exhausted"); return false; }
  }

  // Re-scan FROM the high-water block (Thor ranges are inclusive); a cut walk
  // resumes at the block it may have cut.
  Value* next = tj::IsObject(state) ? tj::Clone(a, state) : tj::Obj(a);
  if (!next || !tj::Set(a, next, "lastBlock", tj::Num(a, s.have_resume ? s.resume_at : high_water))) {
    error->Upstream("thor: arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = s.complete;
  return true;
}

namespace {

bool vechain_supports(const config::ProviderRuntimeConfig&, const Network& network) {
  return !network.testnet && network.rpc_url_count > 0;
}

}  // namespace

const VechainDriver kVechainDriver{};

// v2: one netted record per transaction and asset; records are restatable.
const VechainSource kVechainSource{};


bool VechainDriver::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool VechainSource::Registered(const config::ProviderRuntimeConfig& config) const { return always(config); }
bool VechainSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return vechain_supports(config, network); }

}  // namespace fortis::chains
