#include "ownership.h"
#include "address_facets_internal.h"

#include "kv_store.h"
#include "price_id_parser.h"
#include "price_series_store.h"
#include "repair_candidates.h"
#include "threading.h"
#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <new>

namespace tj = fortis::codec::tj;

// Ports of the TypeScript TransfersFacet, TransferIngester, fiat
// enrichment (fiat_at_time), change detection, transfer retention and
// operation candle repair.
namespace fortis::wallet::address {
namespace {

constexpr int64_t kDefaultMaxAgeMs = 30000;
constexpr int64_t kErrorRetryCooldownMs = 60000;
constexpr uint64_t kDeltaIngestWaitMs = 1500;
constexpr int64_t kNearest1hToleranceMs = 2 * 3600000LL;
constexpr int64_t kNearest1dToleranceMs = 2 * 86400000LL;
constexpr int64_t kFiatRetryBackoffMs = 6 * 3600000LL;
constexpr int64_t kHintedFiatRetryBackoffMs = 60000;
constexpr int64_t kTransferRetentionMs = 730LL * 86400000;
constexpr size_t kRetentionBatch = 256;
constexpr size_t kRetentionMaxBatches = 80;
// The TypeScript change detection budgets.
constexpr int64_t kVerifyFreshMs = 3 * 60000;
constexpr int64_t kMaxCleanAgeMs = 60 * 60000;
constexpr int64_t kExplorerLagMs = 90000;
constexpr int64_t kBalancesOnlyCleanAgeMs = 15 * 60000;
constexpr size_t kIngestArenaBytes = 16 * 1024 * 1024;
// One read's page of at most 501 stored rows (about 4 KiB parsed each).
constexpr size_t kTransferPageArenaBytes = 8 * 1024 * 1024;
// pad13(ts) + '/' + encodeURIComponent(id) of a <= 600-unit cursor's id.
constexpr size_t kTransferBoundaryBytes = 8192;

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

const char* str(const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  return tj::IsString(v) ? v->text : nullptr;
}

constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10,
                             1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21,
                             1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29, 1e30, 1e31, 1e32,
                             1e33, 1e34, 1e35, 1e36, 1e37, 1e38, 1e39, 1e40};

double pow10(double decimals) {
  if (decimals >= 0 && decimals <= 40 && trunc(decimals) == decimals) return kPow10[static_cast<int>(decimals)];
  return pow(10, decimals);
}

// Number(amountRaw) / 10 ** amountDecimals.
double record_units(const Value* record) {
  const Value* raw = tj::Get(record, "amountRaw");
  const double number = tj::IsString(raw) ? fortis::compat::token_text::ToNumber(raw->text, raw->length) : chains::JsNumber(raw);
  return number / pow10(chains::JsNumber(tj::Get(record, "amountDecimals")));
}

}  // namespace

// ---- change tracker -------------------------------------------------------------

struct ChangeTracker::State {
  char id[kAccountIdBytes + 1];
  int64_t watched_since;
  int64_t last_observed_at;
  bool has_fingerprint;
  uint64_t fingerprint;
  bool full;
  bool has_change;
  int64_t last_change_at;
  bool has_ingest;
  int64_t ingest_requested_at;
};

ChangeTracker::ChangeTracker() {

  states_ = base::AllocateZeroedStorage<State[]>(kMaxAccounts, sizeof(State));
}

ChangeTracker::~ChangeTracker() {
  states_.reset();

}

void ChangeTracker::Observe(const char* account_id, const Observation& observation, int64_t at) {
  if (!states_) return;
  fortis::base::LockGuard lock(mutex_);
  State* previous = nullptr;
  for (size_t i = 0; i < count_; ++i)
    if (!strcmp(states_[i].id, account_id)) { previous = &states_[i]; break; }
  if (!observation.observed || !previous) {
    State next{};
    snprintf(next.id, sizeof(next.id), "%s", account_id);
    next.watched_since = at;
    next.last_observed_at = observation.observed ? at : 0;
    next.has_fingerprint = observation.has_fingerprint;
    next.fingerprint = observation.fingerprint;
    next.full = observation.full_coverage;
    next.has_change = previous && previous->has_change;
    next.last_change_at = previous ? previous->last_change_at : 0;
    next.has_ingest = previous && previous->has_ingest;
    next.ingest_requested_at = previous ? previous->ingest_requested_at : 0;
    if (previous) *previous = next;
    else if (count_ < kMaxAccounts) states_[count_++] = next;
    return;
  }
  const bool moved = observation.log_matched ||
                     (observation.has_fingerprint && previous->has_fingerprint &&
                      observation.fingerprint != previous->fingerprint);
  previous->last_observed_at = at;
  if (observation.has_fingerprint) {
    previous->has_fingerprint = true;
    previous->fingerprint = observation.fingerprint;
  }
  previous->full = observation.full_coverage;
  if (moved) {
    previous->has_change = true;
    previous->last_change_at = at;
  }
}

bool ChangeTracker::IsUnchangedSince(const char* account_id, int64_t synced_at, int64_t now) {
  if (!states_) return false;
  fortis::base::LockGuard lock(mutex_);
  bool unchanged = false;
  for (size_t i = 0; i < count_; ++i) {
    const State& s = states_[i];
    if (strcmp(s.id, account_id) != 0) continue;
    unchanged = s.watched_since <= synced_at && (!s.has_change || s.last_change_at + kExplorerLagMs <= synced_at) &&
                now - s.last_observed_at <= kVerifyFreshMs &&
                now - synced_at <= (s.full ? kMaxCleanAgeMs : kBalancesOnlyCleanAgeMs);
    break;
  }
  return unchanged;
}

size_t ChangeTracker::Dirty(int64_t now, char (*out)[kAccountIdBytes + 1], size_t capacity) {
  if (!states_) return 0;
  fortis::base::LockGuard lock(mutex_);
  size_t n = 0;
  for (size_t i = 0; i < count_ && n < capacity; ++i) {
    const State& s = states_[i];
    if (!s.has_change) continue;
    const int64_t indexed_by = s.last_change_at + kExplorerLagMs;
    if (indexed_by <= now && (!s.has_ingest || s.ingest_requested_at < indexed_by))
      snprintf(out[n++], kAccountIdBytes + 1, "%s", s.id);
  }
  return n;
}

void ChangeTracker::MarkIngestRequested(const char* account_id, int64_t at) {
  if (!states_) return;
  fortis::base::LockGuard lock(mutex_);
  for (size_t i = 0; i < count_; ++i) {
    if (strcmp(states_[i].id, account_id) != 0) continue;
    states_[i].has_ingest = true;
    states_[i].ingest_requested_at = at;
    break;
  }
}

void ChangeTracker::RetainOnly(const char (*ids)[kAccountIdBytes + 1], size_t count) {
  if (!states_) return;
  fortis::base::LockGuard lock(mutex_);
  size_t kept = 0;
  for (size_t i = 0; i < count_; ++i) {
    bool keep = false;
    for (size_t j = 0; j < count && !keep; ++j) keep = strcmp(states_[i].id, ids[j]) == 0;
    if (keep) states_[kept++] = states_[i];
  }
  count_ = kept;
}

size_t ChangeTracker::size() {
  fortis::base::LockGuard lock(mutex_);
  const size_t n = count_;
  return n;
}

// ---- fiat enrichment ------------------------------------------------------------------

namespace {

struct Point {
  int64_t ms;
  double price;
};

struct Series {
  const char* source_id;
  Point* hourly;
  size_t hourly_count;
  Point* daily;
  size_t daily_count;
};

// The candles of one (interval, source) over [from, to], read in windows
// the native store accepts.
bool read_series(WalletSeriesReader* store, prices::PriceSeriesInterval interval, const char* source_id, int64_t from,
                 int64_t to, int64_t now, Arena& arena, Point** points, size_t* count) {
  *points = nullptr;
  *count = 0;
  const size_t cap = PriceSeriesMaxRangeCandles(interval);
  const int64_t width = interval == prices::PriceSeriesInterval::k1h ? 3600000LL : 86400000LL;
  const int64_t window = static_cast<int64_t>(cap - 2) * width;
  if (!store || from < 0) return true;
  // Only retained candles can exist (1h: 90 days, 1d: 366 days).
  const int64_t retain = interval == prices::PriceSeriesInterval::k1h ? 90LL * 86400000 : 366LL * 86400000;
  if (from < now - retain - width) from = now - retain - width;
  if (from < 0) from = 0;
  if (to < from) return true;
  auto buffer_storage = base::AllocateStorage<prices::PriceSeriesCandle[]>(sizeof(prices::PriceSeriesCandle) * cap);
  auto* buffer = buffer_storage.get();
  if (!buffer) return false;
  size_t total = 0, capacity = 0;
  Point* out = nullptr;
  for (int64_t start = from; start <= to; start += window) {
    const int64_t end = start + window - 1 < to ? start + window - 1 : to;
    size_t n = 0;
    const prices::PriceSeriesReadStatus status = store->Range(interval, "usd", source_id, start, end, now, buffer, cap, &n);
    if (status != prices::PriceSeriesReadStatus::kFound && status != prices::PriceSeriesReadStatus::kMissing) continue;
    if (total + n > capacity) {
      const size_t next = (total + n) * 2 + 16;
      Point* grown = static_cast<Point*>(arena.Alloc(sizeof(Point) * next));
      if (!grown) { buffer_storage.reset(); return false; }
      if (total) memcpy(grown, out, sizeof(Point) * total);
      out = grown;
      capacity = next;
    }
    for (size_t i = 0; i < n; ++i) out[total++] = Point{buffer[i].ts, buffer[i].close};
  }
  buffer_storage.reset();
  *points = out;
  *count = total;
  return true;
}

// nearest(points, target, tolerance).
const Point* nearest(const Point* points, size_t count, int64_t target, int64_t tolerance) {
  if (!points || !count) return nullptr;
  size_t lo = 0, hi = count - 1;
  while (lo < hi) {
    const size_t mid = (lo + hi) >> 1;
    if (points[mid].ms < target) lo = mid + 1;
    else hi = mid;
  }
  const Point* candidates[2] = {&points[lo], &points[lo > 0 ? lo - 1 : 0]};
  const Point* best = nullptr;
  for (const Point* c : candidates) {
    if (!best || llabs(c->ms - target) < llabs(best->ms - target)) best = c;
  }
  return best && llabs(best->ms - target) <= tolerance ? best : nullptr;
}

// The registry coingeckoId of a token (TokenStore.get('registry', …)).
const char* registry_coingecko_id(Store* store, const char* network, const char* token_id, int64_t now, Arena& arena) {
  char n[300], t[600], key[1000];
  if (!Store::Component(network, n, sizeof(n)) || !Store::Component(token_id, t, sizeof(t))) return nullptr;
  if (snprintf(key, sizeof(key), "token/registry/%s/%s", n, t) >= static_cast<int>(sizeof(key))) return nullptr;
  Value* row = nullptr;
  if (store->GetJson(key, now, arena, &row) != ReadStatus::kFound) return nullptr;
  const Value* id = tj::Get(row, "coingeckoId");
  return tj::IsString(id) && id->length ? id->text : nullptr;
}

double demand_price_tier(Store* store, const char* network, const char* token_id, int64_t now, Arena& arena) {
  char n[300], t[600], key[1000];
  if (!Store::Component(network, n, sizeof(n)) || !Store::Component(token_id, t, sizeof(t))) return 3;
  if (snprintf(key, sizeof(key), "token/demand/%s/%s", n, t) >= static_cast<int>(sizeof(key))) return 3;
  Value* row = nullptr;
  if (store->GetJson(key, now, arena, &row) != ReadStatus::kFound) return 3;
  const Value* tier = tj::Get(row, "priceTier");
  return tj::IsNumber(tier) ? tier->number : 3;
}

// enrichPriceable(tokens, series, network, records, overrides): sets
// fiatAtTime / fiatSourceInterval / fiatSourceId / fiatSourceTs (ms) on the
// priceable records in place.
}  // namespace

void WalletValuation::Enrich(const Network& network, Value* const* records, size_t count,
                      const char* override_token, const char* override_source, int64_t now, Arena& arena) {
  // Candidates: amountRaw and amountDecimals present.
  struct TokenSource {
    const char* token_id;
    const char* source_id;  // null = unpriceable
  };
  TokenSource* tokens = static_cast<TokenSource*>(arena.Alloc(sizeof(TokenSource) * (count + 1)));
  size_t token_count = 0;
  if (!tokens) return;
  int64_t min_ts = INT64_MAX, max_ts = INT64_MIN;
  bool any = false;
  for (size_t i = 0; i < count; ++i) {
    const Value* r = records[i];
    if (is_null(tj::Get(r, "amountRaw")) || is_null(tj::Get(r, "amountDecimals"))) continue;
    const char* token = str(r, "tokenId");
    if (!token) continue;
    bool seen = false;
    for (size_t j = 0; j < token_count; ++j) if (!strcmp(tokens[j].token_id, token)) { seen = true; break; }
    if (!seen) {
      const char* canonical = registry_coingecko_id(deps_.store, network.id, token, now, arena);
      if (!canonical && override_token && !strcmp(override_token, token)) canonical = override_source;
      tokens[token_count++] = TokenSource{token, PriceIdForTransfer(arena, network, token, canonical)};
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const Value* r = records[i];
    if (is_null(tj::Get(r, "amountRaw")) || is_null(tj::Get(r, "amountDecimals"))) continue;
    const char* token = str(r, "tokenId");
    for (size_t j = 0; token && j < token_count; ++j) {
      if (strcmp(tokens[j].token_id, token) != 0 || !tokens[j].source_id) continue;
      const int64_t ts = RowMs(r, "ts");
      if (ts < min_ts) min_ts = ts;
      if (ts > max_ts) max_ts = ts;
      any = true;
    }
  }
  if (!any) return;
  const int64_t from = min_ts - kNearest1dToleranceMs, to = max_ts + kNearest1dToleranceMs;
  Series* series = static_cast<Series*>(arena.Alloc(sizeof(Series) * (token_count + 1)));
  size_t series_count = 0;
  if (!series) return;
  for (size_t j = 0; j < token_count; ++j) {
    if (!tokens[j].source_id) continue;
    bool seen = false;
    for (size_t k = 0; k < series_count; ++k) if (!strcmp(series[k].source_id, tokens[j].source_id)) { seen = true; break; }
    if (seen) continue;
    Series& s = series[series_count++];
    s = Series{tokens[j].source_id, nullptr, 0, nullptr, 0};
    read_series(deps_.series, prices::PriceSeriesInterval::k1h, s.source_id, from, to, now, arena, &s.hourly, &s.hourly_count);
    read_series(deps_.series, prices::PriceSeriesInterval::k1d, s.source_id, from, to, now, arena, &s.daily, &s.daily_count);
  }
  for (size_t i = 0; i < count; ++i) {
    Value* r = records[i];
    if (is_null(tj::Get(r, "amountRaw")) || is_null(tj::Get(r, "amountDecimals"))) continue;
    const char* token = str(r, "tokenId");
    const char* source = nullptr;
    for (size_t j = 0; token && j < token_count; ++j)
      if (!strcmp(tokens[j].token_id, token)) { source = tokens[j].source_id; break; }
    if (!source) continue;
    const Series* s = nullptr;
    for (size_t k = 0; k < series_count; ++k) if (!strcmp(series[k].source_id, source)) { s = &series[k]; break; }
    if (!s) continue;
    const int64_t ts = RowMs(r, "ts");
    const Point* hourly = nearest(s->hourly, s->hourly_count, ts, kNearest1hToleranceMs);
    const Point* daily = hourly ? nullptr : nearest(s->daily, s->daily_count, ts, kNearest1dToleranceMs);
    const Point* match = hourly ? hourly : daily;
    if (!match) continue;
    const double units = record_units(r);
    if (!isfinite(units)) continue;
    tj::Set(arena, r, "fiatAtTime", tj::Num(arena, units * match->price));
    tj::Set(arena, r, "fiatSourceInterval", tj::Str(arena, hourly ? "1h" : "1d"));
    tj::Set(arena, r, "fiatSourceId", tj::Str(arena, source));
    tj::Set(arena, r, "fiatSourceTs", tj::Num(arena, static_cast<double>(match->ms)));
  }
}

namespace {

struct EligibleContext {
  WalletQueryContext* impl;
  const Network* network;
  const char* token_id;
  bool hinted;
  int64_t retry_before;
  int64_t now;
  Arena* arena;
};

// backfillFiatAtTime's priceable(tokenId).
bool priceable_token(const EligibleContext& c, const char* token_id) {
  if (!strcmp(token_id, "native") && c.network->native_coingecko_id) return true;
  static const char* const kPrefixes[] = {"erc20:", "trc20:", "trc10:", "spl:", "asa:"};
  for (const char* prefix : kPrefixes)
    if (chains::StartsWith(token_id, prefix)) return true;
  if (registry_coingecko_id(c.impl->deps.store, c.network->id, token_id, c.now, *c.arena)) return true;
  return c.hinted && c.token_id && !strcmp(token_id, c.token_id);
}

bool eligible_row(EligibleContext* c, const Value* row) {
  if (!is_null(tj::Get(row, "fiatAtTime"))) return false;
  const Value* attempted = tj::Get(row, "fiatAttemptedAt");
  if (!is_null(attempted) && RowMs(row, "fiatAttemptedAt") > c->retry_before) return false;
  const char* token = str(row, "tokenId");
  const char* to_token = str(row, "toTokenId");
  if (c->token_id && !(token && !strcmp(token, c->token_id)) && !(to_token && !strcmp(to_token, c->token_id)))
    return false;
  return token && priceable_token(*c, token);
}

}  // namespace

size_t TransferRepairService::BackfillFiat(Arena& arena, const Network& network, const char* account_id, const char* token_id,
                            const char* price_source_id, int64_t now) {
  const bool hinted = token_id && price_source_id;
  const int64_t retry_before = now - (hinted ? kHintedFiatRetryBackoffMs : kFiatRetryBackoffMs);
  EligibleContext context{&context_, &network, token_id, hinted, retry_before, now, &arena};
  Value* rows = nullptr;
  Store* store = context_.deps.store;
  const bool read = token_id
      ? store->UnpricedForToken(account_id, token_id, 500 < 501 ? 500 : 501, retry_before, [&, visitor_state = &context](const fortis::codec::tj::Value* row) { return (eligible_row)(visitor_state, row); }, now, arena, &rows)
      : store->PageTransfers(account_id, false, 500, nullptr, PageOptions{}, [&, visitor_state = &context](const fortis::codec::tj::Value* row) { return (eligible_row)(visitor_state, row); }, now, arena, &rows);
  if (!read || !tj::IsArray(rows) || rows->count == 0) return 0;
  // candidates: copies with the valuation cleared.
  Value** candidates = static_cast<Value**>(arena.Alloc(sizeof(Value*) * rows->count));
  if (!candidates) return 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    candidates[i] = tj::Clone(arena, rows->items[i]);
    if (!candidates[i]) return 0;
    tj::Set(arena, candidates[i], "fiatAtTime", tj::Null(arena));
    tj::Set(arena, candidates[i], "fiatSourceInterval", tj::Null(arena));
    tj::Set(arena, candidates[i], "fiatSourceId", tj::Null(arena));
    tj::Set(arena, candidates[i], "fiatSourceTs", tj::Null(arena));
  }
  context_.valuation.Enrich(network, candidates, rows->count, hinted ? token_id : nullptr, price_source_id, now, arena);
  ValuationTarget* targets = static_cast<ValuationTarget*>(arena.Alloc(sizeof(ValuationTarget) * rows->count));
  if (!targets) return 0;
  size_t repaired = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const Value* c = candidates[i];
    ValuationTarget& t = targets[i];
    t = ValuationTarget{};
    t.account_id = account_id;
    t.record_id = str(c, "recordId");
    const Value* fiat = tj::Get(c, "fiatAtTime");
    const char* interval = str(c, "fiatSourceInterval");
    const char* source = str(c, "fiatSourceId");
    const Value* source_ts = tj::Get(c, "fiatSourceTs");
    if (tj::IsNumber(fiat) && interval && source && tj::IsNumber(source_ts)) {
      char* text = static_cast<char*>(arena.Alloc(160, 1));
      if (text) {
        JsToFixed(fiat->number, 24, text, 160);
        t.has_valuation = true;
        t.fiat_at_time = text;
        t.interval = interval;
        t.source_id = source;
        t.source_ts = static_cast<int64_t>(source_ts->number);
      }
    }
    if (tj::IsNumber(fiat)) ++repaired;
  }
  size_t changed = 0;
  store->RecordValuations(targets, rows->count, now, arena, &changed);
  return repaired;
}

// ---- ingester -------------------------------------------------------------------------

bool TransferIngestor::HasSource(const Network& network) const {
  return context_.Chains().SourceFor(*context_.deps.config, network) != nullptr;
}

void TransferIngestor::EnsureFresh(const char* account_id, const Network& network, const char* address, int64_t max_age_ms) {
  const chains::TransferSource* source = context_.Chains().SourceFor(*context_.deps.config, network);
  if (!source) return;
  char flight_key[96];
  snprintf(flight_key, sizeof(flight_key), "ingest:%s", account_id);
  size_t slot = kFlights;
  bool waited = false;
  if (!context_.BeginFlight(flight_key, &slot, &waited) || waited) return;  // joined the running ingest
  Arena arena = tj::GrowingArena(kIngestArenaBytes);
  std::unique_ptr<Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  Store* store = context_.deps.store;
  const int64_t now = context_.Now();
  auto record_failure = [&](const char* message) {
    Value* fields = tj::Obj(arena);
    if (fields && tj::Set(arena, fields, "lastError", tj::Str(arena, message)) &&
        tj::Set(arena, fields, "lastErrorAt", tj::Num(arena, static_cast<double>(context_.Now()))))
      store->PatchSyncState(account_id, fields, context_.Now(), arena);
  };
  Value* state = nullptr;
  const ReadStatus state_status = store->GetSyncState(account_id, now, arena, &state);
  if (state_status == ReadStatus::kError) {
    record_failure("Error: sync state unreadable");
    tj::FreeArena(arena);
    context_.EndFlight(slot);
    return;
  }
  if (state_status != ReadStatus::kFound) state = nullptr;
  const Value* last_error_at = state ? tj::Get(state, "lastErrorAt") : nullptr;
  const Value* last_sync_at = state ? tj::Get(state, "lastSyncAt") : nullptr;
  const Value* complete = state ? tj::Get(state, "backfillComplete") : nullptr;
  const bool backfill_complete = complete && complete->type == tj::Type::kTrue;
  bool skip = false;
  if (tj::IsNumber(last_error_at) && now - static_cast<int64_t>(last_error_at->number) < kErrorRetryCooldownMs) skip = true;
  if (!skip && backfill_complete && tj::IsNumber(last_sync_at) &&
      now - static_cast<int64_t>(last_sync_at->number) < max_age_ms)
    skip = true;
  if (!skip && max_age_ms > 0 && backfill_complete && tj::IsNumber(last_sync_at) && context_.deps.changes &&
      context_.deps.changes->IsUnchangedSince(account_id, static_cast<int64_t>(last_sync_at->number), now))
    skip = true;
  if (!skip) {
    const Value* stored_cursors = state ? tj::Get(state, "upstreamCursors") : nullptr;
    Value* cursors = tj::IsObject(stored_cursors) ? tj::Clone(arena, stored_cursors) : tj::Obj(arena);
    const Value* depth_value = state ? tj::Get(state, "backfillDepth") : nullptr;
    const double depth = tj::IsNumber(depth_value) ? depth_value->number : 500;
    const Value* source_state = cursors ? tj::Get(cursors, source->Key()) : nullptr;
    Value* empty = tj::Obj(arena);
    chains::Context ctx = context_.ChainContext(arena);
    chains::SourceResult fetched;
    chains::Error error;
    const bool ok = cursors && empty &&
                    source->Fetch(ctx, network, address, is_null(source_state) ? empty : source_state, depth, &fetched,
                                  &error);
    if (!ok) {
      char message[600];
      snprintf(message, sizeof(message), "%s: %s",
               error.kind == chains::ErrorKind::kRange ? "RangeError" : "Error", error.message);
      record_failure(message);
    } else if (fetched.unavailable) {
      record_failure(fetched.reason);
    } else {
      Value* records = tj::IsArray(fetched.records) ? fetched.records : tj::Arr(arena);
      if (records && records->count)
        context_.valuation.Enrich(network, records->items, records->count, nullptr, nullptr, context_.Now(), arena);
      size_t changed = 0;
      bool invalid_time = false;
      const bool upserted =
          records && store->UpsertTransfers(account_id, network, records, context_.Now(), arena, &changed, &invalid_time);
      if (!upserted) {
        record_failure(invalid_time ? "RangeError: Invalid time value" : "Error: transfer upsert failed");
      } else {
        if (records->count > 0) {
          // onActivity: drop the account's balance envelopes (tracked union
          // and native-only) so the next read is not the pre-transfer cache.
          Value* tokens = nullptr;
          if (store->TokensForAccount(account_id, context_.Now(), arena, &tokens)) {
            const size_t n = tj::IsArray(tokens) ? tokens->count : 0;
            const char** list = static_cast<const char**>(arena.Alloc(sizeof(char*) * (n + 1)));
            for (size_t i = 0; list && i < n; ++i) list[i] = tokens->items[i]->text;
            char key[1024];
            if (list && BalancesCacheKey(network.id, address, list, n, key, sizeof(key)))
              context_.deps.swr->Invalidate(key);
            if (BalancesCacheKey(network.id, address, nullptr, 0, key, sizeof(key))) context_.deps.swr->Invalidate(key);
          }
        }
        Value* fields = tj::Obj(arena);
        if (fields && cursors &&
            tj::Set(arena, cursors, source->Key(), fetched.state ? fetched.state : tj::Obj(arena)) &&
            tj::Set(arena, fields, "upstreamCursors", cursors) &&
            tj::Set(arena, fields, "backfillComplete", tj::Bool(arena, fetched.backfill_complete)) &&
            tj::Set(arena, fields, "backfillDepth", tj::Num(arena, depth)) &&
            tj::Set(arena, fields, "lastSyncAt", tj::Num(arena, static_cast<double>(now))) &&
            tj::Set(arena, fields, "lastError", tj::Null(arena)) && tj::Set(arena, fields, "lastErrorAt", tj::Null(arena)))
          store->PatchSyncState(account_id, fields, context_.Now(), arena);
      }
    }
  }
  tj::FreeArena(arena);
  context_.EndFlight(slot);
}

// ---- transfers facet ------------------------------------------------------------------------


FacetReply WalletQueryService::Transfers(Arena& arena, const char* network_id, const char* address, const TransferQuery& query,
                         const char* device_id) {
  FacetReply result;
  auto resolved_network = impl_->Resolve(network_id);
  if (const auto* failure = std::get_if<FacetReply>(&resolved_network)) return *failure;
  Network& network = std::get<Network>(resolved_network);
  if (!network.caps.transfers) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "transfers not served for %s", network_id);
    return result;
  }
  Store* store = impl_->deps.store;
  const char* normalized = Canonical(arena, network, address);
  Account account;
  if (!normalized || !store->Touch(network.id, normalized, Now(), &account)) return result;
  if (device_id && *device_id) {
    const char* tracked[1] = {query.token_id};
    store->Remember(device_id, account.id, network.id, normalized, tracked, query.token_id ? 1 : 0, Now(), arena);
  }
  const int64_t max_age = query.refresh ? 0 : kDefaultMaxAgeMs;
  // `since` selects the delta path only when non-empty (truthy).
  const bool delta = query.since && query.since_length > 0;
  if (delta) {
    // The delta poll waits a bounded 1.5 s for the explorer; the ingest keeps
    // running and its rows reach devices through the sync journal.
    if (!impl_->deps.ingest_async ||
        !impl_->deps.ingest_async(impl_->deps.ingest_context, account.id, network, normalized, max_age,
                                  kDeltaIngestWaitMs))
      EnsureFresh(account.id, network, normalized, max_age);
  } else {
    EnsureFresh(account.id, network, normalized, max_age);
  }
  const int64_t now = Now();
  if (query.token_id) {
    // Only side effects (valuations written): its allocations are dropped.
    const tj::Mark mark = tj::MarkOf(arena);
    BackfillFiat(arena, network, account.id, query.token_id, query.price_source_id, now);
    tj::Rewind(arena, mark);
  }
  // The page position: `since ?? boundary(before)`; an empty `since` is a
  // position too (the time order then starts at the bare index root and
  // reads nothing). decodeDisplayCursor runs after the ingest, as
  // TypeScript orders it.
  char boundary[kTransferBoundaryBytes];
  const char* after = query.since;
  if (!after && query.before) {
    int64_t ts = 0;
    const char* id = nullptr;
    size_t id_length = 0;
    if (!DecodeDisplayCursor(arena, query.before, query.before_length, &ts, &id, &id_length)) {
      result.outcome = Outcome::kBadRequest;
      snprintf(result.detail, sizeof(result.detail), "invalid display cursor");
      return result;
    }
    // transferTimeBoundary: pad13(ts)/encodeURIComponent(id); an id holding
    // a lone surrogate throws URIError there (not a RangeError: 502).
    char pad[24];
    Store::Pad13(ts, pad);
    const size_t pad_length = strlen(pad);
    memcpy(boundary, pad, pad_length);
    boundary[pad_length] = '/';
    chains::OutBuffer encoded{reinterpret_cast<uint8_t*>(boundary + pad_length + 1),
                             sizeof(boundary) - pad_length - 2, 0, true};
    if (!chains::EncodeUriComponent(id, id_length, encoded) || !encoded.ok) return result;
    boundary[pad_length + 1 + encoded.used] = '\0';
    after = boundary;
  }
  PageOptions options;
  if (!delta) {
    options.token_id = query.token_id;
    options.has_from = query.has_from;
    options.from_ms = query.from_sec * 1000;
    options.has_to = query.has_to;
    options.to_ms = query.to_sec * 1000;
  }
  // The page's rows live in their own arena: only the documents built from
  // them (copied) stay in the task arena, so a 501-row page fits.
  Arena page = tj::GrowingArena(kTransferPageArenaBytes);
  std::unique_ptr<Arena, tj::ArenaBlocksDeleter> page_blocks(&page);

  Value* rows = nullptr;
  const size_t limit = static_cast<size_t>(query.limit);
  if (!store->PageTransfers(account.id, delta, limit + 1, after, options, [&query](const Value* row) { return TransferMatches(row, query); }, now, page, &rows))
    return result;
  const bool has_more = rows->count > limit;
  const size_t page_count = has_more ? limit : rows->count;
  const auto high_water = store->HighWater(account.id, now);
  if (!high_water) return result;
  Value* sync_state = nullptr;
  const ReadStatus sync_status = store->GetSyncState(account.id, now, arena, &sync_state);
  const Value* complete = sync_status == ReadStatus::kFound ? tj::Get(sync_state, "backfillComplete") : nullptr;
  const Value* last_sync = sync_status == ReadStatus::kFound ? tj::Get(sync_state, "lastSyncAt") : nullptr;
  // Confirmation depth from the tip, read once for the page (fail soft).
  bool has_tip = false;
  double tip = 0;
  const chains::ChainDriver* driver = impl_->Chains().DriverFor(*impl_->deps.config, network.family);
  if (driver && driver->HasLatestHeight()) {
    bool needed = false;
    for (size_t i = 0; i < page_count && !needed; ++i) {
      const Value* row = rows->items[i];
      needed = chains::Equals(tj::Get(row, "status"), "confirmed") && !is_null(tj::Get(row, "blockNumber"));
    }
    if (needed) {
      chains::Context ctx = impl_->ChainContext(page);
      chains::Error error;
      if (!driver->LatestHeight(ctx, network, &has_tip, &tip, &error)) has_tip = false;
    }
  }
  Value* items = tj::Arr(arena);
  Value* operations = tj::Arr(arena);
  if (!items || !operations) return result;
  int64_t latest_update = 0;
  bool has_update = false;
  for (size_t i = 0; i < page_count; ++i) {
    const Value* row = rows->items[i];
    if (!tj::Push(arena, items, TransferAppJson(arena, row, network, has_tip, tip)) ||
        !tj::Push(arena, operations, OperationAppJson(arena, row)))
      return result;
    const int64_t updated = RowMs(row, "updatedAt");
    if (!has_update || updated > latest_update) { latest_update = updated; has_update = true; }
  }
  Value* cursor = nullptr;
  if (delta && has_more && page_count > 0) {
    const Value* last = tj::Get(rows->items[page_count - 1], "cursor");
    cursor = last ? tj::Clone(arena, last) : tj::Null(arena);
  } else {
    cursor = high_water.value() ? tj::Str(arena, high_water.value()->cursor) : tj::Null(arena);
  }
  Value* next_before = tj::Null(arena);
  if (has_more && page_count > 0) {
    const Value* last = rows->items[page_count - 1];
    char encoded[2048];
    const char* record_id = str(last, "recordId");
    if (record_id && EncodeDisplayCursor(RowMs(last, "ts"), record_id, encoded, sizeof(encoded)))
      next_before = tj::Str(arena, encoded);
  }
  Value* window = tj::Obj(arena);
  const bool has_last_sync = tj::IsNumber(last_sync);
  Value* out = tj::Obj(arena);
  bool ok = out && window &&
            tj::Set(arena, window, "from", query.has_from ? tj::Num(arena, query.from_sec) : tj::Null(arena)) &&
            tj::Set(arena, window, "to", query.has_to ? tj::Num(arena, query.to_sec) : tj::Null(arena)) &&
            tj::Set(arena, window, "flow", tj::Str(arena, query.flow ? query.flow : "all")) &&
            tj::Set(arena, out, "network", tj::Str(arena, network.id)) &&
            tj::Set(arena, out, "address", tj::Str(arena, normalized)) && tj::Set(arena, out, "items", items) &&
            tj::Set(arena, out, "operations", operations) && tj::Set(arena, out, "cursor", cursor) &&
            tj::Set(arena, out, "hasMore", tj::Bool(arena, has_more)) &&
            tj::Set(arena, out, "nextBefore", next_before) &&
            tj::Set(arena, out, "historyComplete",
                    tj::Bool(arena, complete && complete->type == tj::Type::kTrue)) &&
            tj::Set(arena, out, "window", window) &&
            tj::Set(arena, out, "cache",
                    CacheJson(arena, true, has_last_sync ? static_cast<int64_t>(last_sync->number) : latest_update,
                              has_last_sync || has_update));
  if (!ok) return result;
  result.payload = out;
  result.outcome = Outcome::kOk;
  return result;
}

// ---- retention and candle repair --------------------------------------------------------

size_t TransferRepairService::PruneTransfers(Arena& arena) {
  const int64_t now = context_.Now();
  const int64_t cutoff = now - kTransferRetentionMs;
  size_t pruned = 0;
  for (size_t batch = 0; batch < kRetentionMaxBatches; ++batch) {
    const size_t count = context_.deps.store->PruneTerminalBefore(cutoff, kRetentionBatch, now, arena);
    pruned += count;
    if (count < kRetentionBatch) break;
  }
  return pruned;
}

namespace {

using repair::Candidate;
struct RepairScan {
  WalletQueryContext* impl;
  repair::RepairCandidates& candidates;
  int64_t retry_before;
  int64_t now;
};

bool repair_visit(RepairScan* scan, const Value* transfer, Arena& arena) {
  if (!is_null(tj::Get(transfer, "fiatAtTime")) || !chains::Equals(tj::Get(transfer, "status"), "confirmed") ||
      is_null(tj::Get(transfer, "amountRaw")) || is_null(tj::Get(transfer, "amountDecimals")))
    return true;
  const Value* attempted = tj::Get(transfer, "fiatAttemptedAt");
  if (!is_null(attempted) && RowMs(transfer, "fiatAttemptedAt") > scan->retry_before) return true;
  const char* account_id = str(transfer, "accountId");
  const char* record_id = str(transfer, "recordId");
  const char* token_id = str(transfer, "tokenId");
  if (!account_id || !record_id || !token_id) return true;
  Account account;
  if (scan->impl->deps.store->ById(account_id, scan->now, &account) != ReadStatus::kFound) return true;
  Candidate candidate{};
  candidate.account_id.Assign(account_id);
  candidate.record_id.Assign(record_id);
  candidate.network.Assign(account.network);
  candidate.token_id.Assign(token_id);
  candidate.ts = RowMs(transfer, "ts");
  const char* coingecko = registry_coingecko_id(scan->impl->deps.store, account.network, token_id, scan->now, arena);
  candidate.has_coingecko = coingecko != nullptr;
  if (coingecko) snprintf(candidate.coingecko_id.data(), candidate.coingecko_id.capacity(), "%s", coingecko);
  candidate.price_tier = demand_price_tier(scan->impl->deps.store, account.network, token_id, scan->now, arena);
  const bool priceable = !strcmp(token_id, "native") || chains::StartsWith(token_id, "erc20:") ||
                         chains::StartsWith(token_id, "spl:") || chains::StartsWith(token_id, "asa:") ||
                         candidate.has_coingecko;
  if (!priceable) scan->candidates.MarkUnpriceable(account_id, record_id);
  else scan->candidates.Offer(candidate);
  return true;
}

}  // namespace

WalletQueryService::RepairResult TransferRepairService::RepairOperationCandles(Arena& arena,
    base::FunctionRef<bool(const char*, int64_t, int64_t, bool)> history) {
  WalletQueryService::RepairResult out;
  const int64_t now = context_.Now();
  const int64_t retry_before = now - 6 * 3600000LL;
  const size_t cap = 500;
  auto created = repair::RepairCandidates::Create(cap);
  if (!created) return out;
  auto candidates = created.TakeValue();
  RepairScan scan{&context_, candidates, retry_before, now};
  context_.deps.store->ForEachUnpriced([&](const Value* row, Arena& row_arena) {
    return repair_visit(&scan, row, row_arena);
  }, true, retry_before, now, arena);
  const auto rows = candidates.TakeSorted();
  struct Group {
    char source_id[300];
    int64_t from_ms, to_ms;
    size_t first, count;  // indices into `members`
  };
  Group groups[4];
  size_t group_count = 0;
  // Keep grouping allocation failure soft, as with the old paired mallocs.
  std::vector<size_t> members, member_group;
  try {
    members.resize(rows.size() + 1);
    member_group.resize(rows.size() + 1);
  } catch (const std::bad_alloc&) {
    members.clear();
    member_group.clear();
  }
  size_t member_count = 0;
  for (size_t i = 0; !members.empty() && !member_group.empty() && i < rows.size(); ++i) {
    const Candidate& row = rows[i];
    Network network;
    if (!context_.deps.catalog->Resolve(row.network.data(), strlen(row.network.data()), &network)) continue;
    const char* source = PriceIdForTransfer(arena, network, row.token_id.data(), row.has_coingecko ? row.coingecko_id.data() : nullptr);
    providers::ParsedPriceId parsed{};
    const bool parses = source && ParsePriceId(source, strlen(source), &parsed);
    if (!source || !parses) {
      candidates.MarkUnpriceable(row.account_id.data(), row.record_id.data());
      continue;
    }
    size_t g = group_count;
    for (size_t k = 0; k < group_count; ++k) if (!strcmp(groups[k].source_id, source)) { g = k; break; }
    if (g == group_count) {
      if (group_count >= 4) continue;
      snprintf(groups[g].source_id, sizeof(groups[g].source_id), "%s", source);
      groups[g].from_ms = groups[g].to_ms = row.ts;
      ++group_count;
    }
    if (row.ts < groups[g].from_ms) groups[g].from_ms = row.ts;
    if (row.ts > groups[g].to_ms) groups[g].to_ms = row.ts;
    members[member_count] = i;
    member_group[member_count] = g;
    ++member_count;
  }
  out.groups = group_count;
  for (size_t g = 0; history && g < group_count; ++g) {
    const int64_t padding = 2 * 86400000LL;
    const int64_t from_sec = static_cast<int64_t>(floor(static_cast<double>(groups[g].from_ms - padding) / 1000));
    const int64_t to_limit = groups[g].to_ms + padding < now ? groups[g].to_ms + padding : now;
    const int64_t to_sec = static_cast<int64_t>(ceil(static_cast<double>(to_limit + 1) / 1000));
    if (to_sec <= from_sec) continue;
    const bool hourly = to_sec - from_sec <= 90 * 86400;
    if (!history(groups[g].source_id, from_sec, to_sec, hourly)) continue;
    ++out.histories_ready;
    // Each distinct (network, token, account) of the group.
    for (size_t m = 0; m < member_count; ++m) {
      if (member_group[m] != g) continue;
      const Candidate& row = rows[members[m]];
      bool duplicate = false;
      for (size_t p = 0; p < m && !duplicate; ++p) {
        if (member_group[p] != g) continue;
        const Candidate& other = rows[members[p]];
        duplicate = !strcmp(other.network.data(), row.network.data()) && !strcmp(other.token_id.data(), row.token_id.data()) &&
                    !strcmp(other.account_id.data(), row.account_id.data());
      }
      if (duplicate) continue;
      Network network;
      if (!context_.deps.catalog->Resolve(row.network.data(), strlen(row.network.data()), &network)) continue;
      const tj::Mark mark = tj::MarkOf(arena);
      out.transfers_repaired += BackfillFiat(arena, network, row.account_id.data(), row.token_id.data(), groups[g].source_id, now);
      tj::Rewind(arena, mark);
    }
  }
  const auto& unpriceable = candidates.unpriceable();
  if (!unpriceable.empty()) {
    std::vector<ValuationTarget> targets;
    try {
      targets.resize(unpriceable.size());
    } catch (const std::bad_alloc&) {
      return out;
    }
    for (size_t i = 0; i < unpriceable.size(); ++i) {
      targets[i].account_id = unpriceable[i].account.data();
      targets[i].record_id = unpriceable[i].record.data();
    }
    size_t changed = 0;
    context_.deps.store->RecordValuations(targets.data(), targets.size(), now, arena, &changed);
  }
  return out;
}

bool WalletQueryService::HasSource(const Network& network) const { return impl_->ingestor.HasSource(network); }
void WalletQueryService::EnsureFresh(const char* id, const Network& network, const char* address, int64_t max_age) {
  impl_->ingestor.EnsureFresh(id, network, address, max_age);
}
size_t WalletQueryService::BackfillFiat(Arena& arena, const Network& network, const char* id,
    const char* token, const char* price_id, int64_t now) {
  return impl_->repair.BackfillFiat(arena, network, id, token, price_id, now);
}
size_t WalletQueryService::PruneTransfers(Arena& arena) { return impl_->repair.PruneTransfers(arena); }
WalletQueryService::RepairResult WalletQueryService::RepairOperationCandles(Arena& arena,
    base::FunctionRef<bool(const char*, int64_t, int64_t, bool)> history) {
  return impl_->repair.RepairOperationCandles(arena, history);
}

}  // namespace fortis::wallet::address
