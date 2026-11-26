#include "token_history.h"

#include "token_comparison_store.h"
#include "token_contract.h"
#include "token_data.h"
#include "token_liquidity.h"
#include "token_swr.h"
#include "token_text.h"
#include "token_views.h"
#include "token_writes.h"

#include <math.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_history {
namespace {

using tj::Arena;
using token_data::Observation;
using token_data::Candle;

constexpr int64_t kHourMs = 3600000;
constexpr int64_t kDayMs = 24 * kHourMs;
constexpr int64_t kTrendLookbackDays = 390;

struct Window {
  const char* key;
  const char* label;
  int64_t days;
  int64_t tolerance_ms;
};
constexpr Window kWindows[] = {
    {"change24hPct", "24h", 1, 8 * kHourMs},
    {"change7dPct", "7d", 7, 36 * kHourMs},
    {"change30dPct", "30d", 30, 3 * 24 * kHourMs},
    {"change90dPct", "90d", 90, 7 * 24 * kHourMs},
    {"change365dPct", "12m", 365, 21 * 24 * kHourMs},
};

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }
Value* num_or_null(Arena& a, bool has, double v) { return has ? tj::Num(a, v) : tj::Null(a); }
Value* iso(Arena& a, int64_t ms) { return token_facts::IsoValue(a, ms); }

int64_t utc_day(int64_t ms) {
  int64_t day = ms / kDayMs;
  if (ms % kDayMs < 0) --day;
  return day * kDayMs;
}

// number(value) of the TypeScript holder history: a number, or a numeric string.
bool number_of(const Value* v, double* out) {
  if (tj::IsNumber(v)) { if (!isfinite(v->number)) return false; *out = v->number; return true; }
  if (!tj::IsString(v)) return false;
  const double parsed = compat::token_text::ToNumber(v->text, v->length);
  if (!isfinite(parsed)) return false;
  *out = parsed;
  return true;
}

// String(n) of a finite number (numeric()).
Value* numeric_string(Arena& a, bool has, double v) {
  if (!has) return tj::Null(a);
  uint8_t buffer[64];
  tj::OutBuffer out{buffer, sizeof(buffer), 0, true};
  codec::history_batch::AppendJsNumber(out, v);
  return tj::Str(a, reinterpret_cast<const char*>(buffer), out.used);
}

double date_of(const Value* row, const char* key, bool* has) {
  const Value* v = tj::Get(row, key);
  *has = tj::IsNumber(v);
  return *has ? v->number : 0;
}

Value* empty_snapshot(Arena& a, const char* network, const char* token_id, const Value* source, int64_t ts,
                      int64_t observed) {
  Value* o = tj::Obj(a);
  if (!o) return nullptr;
  const bool ok = tj::Set(a, o, "network", tj::Str(a, network)) && tj::Set(a, o, "tokenId", tj::Str(a, token_id)) &&
                  tj::Set(a, o, "source", tj::Clone(a, source)) &&
                  tj::Set(a, o, "ts", tj::Num(a, static_cast<double>(ts))) &&
                  tj::Set(a, o, "observedAt", tj::Num(a, static_cast<double>(observed))) &&
                  tj::Set(a, o, "holderCount", tj::Null(a)) && tj::Set(a, o, "countObservedAt", tj::Null(a)) &&
                  tj::Set(a, o, "atLeast", tj::Bool(a, false)) && tj::Set(a, o, "topHoldersPercent", tj::Null(a)) &&
                  tj::Set(a, o, "topHoldersPercentExcludingLocked", tj::Null(a)) &&
                  tj::Set(a, o, "burnedPercent", tj::Null(a)) && tj::Set(a, o, "distributionHash", tj::Null(a)) &&
                  tj::Set(a, o, "distributionObservedAt", tj::Null(a)) &&
                  tj::Set(a, o, "positionCount", tj::Num(a, 0)) && tj::Set(a, o, "positions", tj::Arr(a));
  return ok ? o : nullptr;
}

// source.trim() (a non-string source throws inside the caller's try).
Value* trimmed_source(Arena& a, const Value* source) {
  if (!tj::IsString(source)) return nullptr;
  size_t begin = 0, end = 0;
  compat::token_text::Trim(source->text, source->length, &begin, &end);
  if (begin == end) return nullptr;
  return tj::Str(a, source->text + begin, end - begin);
}

const char* cstring(Arena& a, const Value* v) { return tj::CString(a, v); }

bool finite_percent(const Value* v, double* out) {
  return tj::IsNumber(v) && isfinite(v->number) && v->number >= 0 && v->number <= 100 && ((*out = v->number), true);
}
bool finite_nonnegative(const Value* v, double* out) {
  return tj::IsNumber(v) && isfinite(v->number) && v->number >= 0 && ((*out = v->number), true);
}

}  // namespace

// ============================================================================
// HolderSnapshotStore writes
// ============================================================================

bool RecordCount(Work& work, const char* network, const char* token_id, double count, bool at_least,
                 const Value* source, int64_t observed_at) {
  Arena& a = work.arena;
  const size_t mark = a.used;
  if (!isfinite(count) || count < 0 || count > 9007199254740991.0 || floor(count) != count) return false;
  Value* src = trimmed_source(a, source);
  const char* src_text = src ? cstring(a, src) : nullptr;
  if (!src_text) { a.used = mark; return false; }
  const int64_t ts = utc_day(observed_at);
  const int64_t now = work.Now();
  Value* old = nullptr;
  work.env->data->HolderRow(network, token_id, src_text, ts, now, a, &old);
  bool has = false;
  const double old_count_at = date_of(old, "countObservedAt", &has);
  if (has && old_count_at > static_cast<double>(observed_at)) { a.used = mark; return true; }
  Value* row = old ? tj::Clone(a, old) : empty_snapshot(a, network, token_id, src, ts, observed_at);
  bool ok = row != nullptr;
  const double row_observed = date_of(row, "observedAt", &has);
  ok = ok && tj::Set(a, row, "holderCount", tj::Num(a, count)) && tj::Set(a, row, "atLeast", tj::Bool(a, at_least)) &&
       tj::Set(a, row, "countObservedAt", tj::Num(a, static_cast<double>(observed_at))) &&
       tj::Set(a, row, "observedAt",
               tj::Num(a, row_observed > static_cast<double>(observed_at) ? row_observed
                                                                         : static_cast<double>(observed_at)));
  ok = ok && work.env->data->PutHolderRow(network, token_id, src_text, ts, row, now);
  a.used = mark;
  a.ok = true;
  return ok;
}

namespace {

// normalizePositions(holders).
Value* normalize_positions(Arena& a, const Value* holders) {
  Value* out = tj::Arr(a);
  if (!out) return nullptr;
  for (uint32_t i = 0; tj::IsArray(holders) && i < holders->count; ++i) {
    const Value* holder = holders->items[i];
    const Value* address = tj::Get(holder, "address");
    if (!tj::IsString(address)) continue;
    size_t begin = 0, end = 0;
    compat::token_text::Trim(address->text, address->length, &begin, &end);
    Value* canonical = tj::Str(a, address->text + begin, end - begin);
    if (!canonical) return nullptr;
    bool hex = canonical->length > 2 && canonical->text[0] == '0' &&
               (canonical->text[1] == 'x' || canonical->text[1] == 'X');
    for (uint32_t k = 2; hex && k < canonical->length; ++k) {
      const char c = canonical->text[k];
      hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    if (hex) canonical = compat::token_text::ToLowerCase(a, canonical->text, canonical->length);
    if (!canonical) return nullptr;
    if (canonical->length == 0 || token_contract::IsBurnAddress(a, canonical->text, canonical->length)) continue;
    bool seen = false;
    for (uint32_t k = 0; k < out->count && !seen; ++k) {
      const Value* prior = tj::Get(out->items[k], "address");
      seen = prior->length == canonical->length && memcmp(prior->text, canonical->text, canonical->length) == 0;
    }
    if (seen) continue;
    const Value* tag = tj::Get(holder, "tag");
    Value* clean_tag = tj::Null(a);
    if (tj::IsString(tag)) {
      compat::token_text::Trim(tag->text, tag->length, &begin, &end);
      if (end > begin) clean_tag = compat::token_text::Slice(a, tag->text + begin, end - begin, 0, 200);
    }
    double balance = 0, percent = 0;
    const bool has_balance = finite_nonnegative(tj::Get(holder, "balance"), &balance);
    const bool has_percent = finite_percent(tj::Get(holder, "percent"), &percent);
    const Value* is_contract = tj::Get(holder, "isContract");
    const Value* is_locked = tj::Get(holder, "isLocked");
    Value* row = tj::Obj(a);
    if (!row || !clean_tag || !tj::Set(a, row, "rank", tj::Num(a, out->count + 1)) ||
        !tj::Set(a, row, "address", canonical) || !tj::Set(a, row, "tag", clean_tag) ||
        !tj::Set(a, row, "isContract", tj::IsBool(is_contract) ? tj::Clone(a, is_contract) : tj::Null(a)) ||
        !tj::Set(a, row, "isLocked", tj::IsBool(is_locked) ? tj::Clone(a, is_locked) : tj::Null(a)) ||
        !tj::Set(a, row, "balance", num_or_null(a, has_balance, balance)) ||
        !tj::Set(a, row, "percent", num_or_null(a, has_percent, percent)) || !tj::Push(a, out, row))
      return nullptr;
  }
  return out;
}

}  // namespace

bool RecordDistribution(Work& work, const char* network, const char* token_id, const Value* contract,
                        int64_t observed_at) {
  Arena& a = work.arena;
  const size_t mark = a.used;
  Value* src = trimmed_source(a, tj::Get(contract, "source"));
  const char* src_text = src ? cstring(a, src) : nullptr;
  if (!src_text) { a.used = mark; return false; }
  Value* positions = normalize_positions(a, tj::Get(contract, "topHolders"));
  double v = 0;
  const bool has_top = finite_percent(tj::Get(contract, "topHoldersPercent"), &v);
  const double top = v;
  const bool has_live = finite_percent(tj::Get(contract, "topHoldersPercentExcludingLocked"), &v);
  const double live = v;
  const bool has_burned = finite_percent(tj::Get(contract, "burnedPercent"), &v);
  const double burned = v;
  Value* summary = tj::Obj(a);
  Value* hashed = tj::Obj(a);
  bool ok = positions && summary && hashed &&
            tj::Set(a, summary, "topHoldersPercent", num_or_null(a, has_top, top)) &&
            tj::Set(a, summary, "topHoldersPercentExcludingLocked", num_or_null(a, has_live, live)) &&
            tj::Set(a, summary, "burnedPercent", num_or_null(a, has_burned, burned)) &&
            tj::Set(a, hashed, "positions", positions) && tj::Set(a, hashed, "summary", summary);
  char hash_hex[65] = {};
  if (ok) {
    uint8_t buffer[64 * 1024];
    tj::OutBuffer text{buffer, sizeof(buffer), 0, true};
    ok = tj::Stringify(hashed, text) && text.ok;
    unsigned char digest[32];
    if (ok) {
      mbedtls_sha256(buffer, text.used, digest, 0);
      static const char kHex[] = "0123456789abcdef";
      for (size_t i = 0; i < 32; ++i) { hash_hex[i * 2] = kHex[digest[i] >> 4]; hash_hex[i * 2 + 1] = kHex[digest[i] & 15]; }
    }
  }
  if (!ok) { a.used = mark; a.ok = true; return false; }
  const int64_t ts = utc_day(observed_at);
  const int64_t now = work.Now();
  Value* old = nullptr;
  work.env->data->HolderRow(network, token_id, src_text, ts, now, a, &old);
  bool has = false;
  const double old_at = date_of(old, "distributionObservedAt", &has);
  if (has && old_at > static_cast<double>(observed_at)) { a.used = mark; return true; }
  Value* row = old ? tj::Clone(a, old) : empty_snapshot(a, network, token_id, src, ts, observed_at);
  ok = row != nullptr;
  const double row_observed = date_of(row, "observedAt", &has);
  const Value* old_hash = tj::Get(old, "distributionHash");
  const bool same = tj::IsString(old_hash) && old_hash->length == 64 && memcmp(old_hash->text, hash_hex, 64) == 0;
  Value* stored_positions = nullptr;
  if (ok && same) {
    stored_positions = tj::Get(row, "positions");
  } else if (ok) {
    stored_positions = tj::Arr(a);
    for (uint32_t i = 0; stored_positions && i < positions->count && ok; ++i) {
      Value* p = tj::Clone(a, positions->items[i]);
      const Value* balance = tj::Get(p, "balance");
      const Value* percent = tj::Get(p, "percent");
      ok = p && tj::Set(a, p, "balance", numeric_string(a, tj::IsNumber(balance), tj::IsNumber(balance) ? balance->number : 0)) &&
           tj::Set(a, p, "percent", numeric_string(a, tj::IsNumber(percent), tj::IsNumber(percent) ? percent->number : 0)) &&
           tj::Push(a, stored_positions, p);
    }
  }
  ok = ok && stored_positions &&
       tj::Set(a, row, "observedAt",
               tj::Num(a, row_observed > static_cast<double>(observed_at) ? row_observed
                                                                         : static_cast<double>(observed_at))) &&
       tj::Set(a, row, "distributionObservedAt", tj::Num(a, static_cast<double>(observed_at))) &&
       tj::Set(a, row, "distributionHash", tj::Str(a, hash_hex, 64)) &&
       tj::Set(a, row, "positionCount", tj::Num(a, positions->count)) &&
       tj::Set(a, row, "topHoldersPercent", numeric_string(a, has_top, top)) &&
       tj::Set(a, row, "topHoldersPercentExcludingLocked", numeric_string(a, has_live, live)) &&
       tj::Set(a, row, "burnedPercent", numeric_string(a, has_burned, burned)) &&
       tj::Set(a, row, "positions", stored_positions);
  ok = ok && work.env->data->PutHolderRow(network, token_id, src_text, ts, row, now);
  a.used = mark;
  a.ok = true;
  return ok;
}

// ============================================================================
// HolderSnapshotStore reads
// ============================================================================
namespace {

struct CountObservation {
  double observed_at;
  double count;
  const Value* source;
};

// countObservations(network, tokenId, now, days, source).
size_t count_observations(const Value* rows, int64_t now, int64_t days, const char* source, CountObservation* out,
                          size_t capacity) {
  const double cutoff = static_cast<double>(now - days * kDayMs);
  // Rows passing the filter, sorted by countObservedAt (stable).
  size_t n = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count && n < capacity; ++i) {
    const Value* row = rows->items[i];
    const Value* row_source = tj::Get(row, "source");
    if (source && !tj::StringEquals(row_source, source)) continue;
    const Value* count = tj::Get(row, "holderCount");
    bool has = false;
    const double at = date_of(row, "countObservedAt", &has);
    if (is_null(count) || !has || at < cutoff || at > static_cast<double>(now)) continue;
    const Value* at_least = tj::Get(row, "atLeast");
    if (at_least && at_least->type == tj::Type::kTrue) continue;
    if (!tj::IsNumber(count)) continue;
    // insertion sort keeps equal timestamps in key order
    size_t pos = n;
    while (pos > 0 && out[pos - 1].observed_at > at) { out[pos] = out[pos - 1]; --pos; }
    out[pos] = CountObservation{at, count->number, row_source};
    ++n;
  }
  if (source || n == 0) return n;
  const Value* latest_source = out[n - 1].source;
  size_t kept = 0;
  for (size_t i = 0; i < n; ++i) {
    const Value* s = out[i].source;
    const bool same = tj::IsString(s) && tj::IsString(latest_source) && s->length == latest_source->length &&
                      memcmp(s->text, latest_source->text, s->length) == 0;
    if (same) out[kept++] = out[i];
  }
  return kept;
}

// nearestByObservedAt.
const CountObservation* nearest_count(const CountObservation* rows, size_t n, size_t latest, int64_t days,
                                      int64_t tolerance) {
  const double target = rows[latest].observed_at - static_cast<double>(days * kDayMs);
  const CountObservation* best = nullptr;
  for (size_t i = 0; i < n; ++i) {
    if (i == latest) continue;
    const double distance = fabs(rows[i].observed_at - target);
    if (distance > static_cast<double>(tolerance)) continue;
    if (!best || distance < fabs(best->observed_at - target)) best = &rows[i];
  }
  return best;
}

}  // namespace

namespace {

// trends() over rows already read (HolderRows of one token).
bool trends_of(Arena& a, const Value* rows, const char* source, int64_t now_ms, Value** trends) {
  *trends = tj::Null(a);
  const size_t capacity = tj::IsArray(rows) ? rows->count + 1 : 1;
  auto* observations = static_cast<CountObservation*>(a.Alloc(sizeof(CountObservation) * capacity));
  if (!observations) return false;
  const size_t n = count_observations(rows, now_ms, kTrendLookbackDays, source, observations, capacity);
  if (!n || static_cast<double>(now_ms) - observations[n - 1].observed_at > static_cast<double>(36 * kHourMs))
    return true;
  Value* o = tj::Obj(a);
  if (!o) return false;
  for (const Window& window : kWindows) {
    const CountObservation* baseline = nearest_count(observations, n, n - 1, window.days, window.tolerance_ms);
    Value* change = tj::Null(a);
    if (baseline && baseline->count > 0)
      change = tj::Num(a, (observations[n - 1].count - baseline->count) / baseline->count * 100);
    if (!tj::Set(a, o, window.key, change)) return false;
  }
  if (!tj::Set(a, o, "latestAt", tj::Num(a, observations[n - 1].observed_at))) return false;
  *trends = o;
  return true;
}

}  // namespace

bool Trends(Work& work, const char* network, const char* token_id, const char* source, int64_t now_ms,
            Value** trends) {
  Arena& a = work.arena;
  *trends = tj::Null(a);
  Value* rows = nullptr;
  if (work.env->data->HolderRows(network, token_id, work.Now(), a, &rows) != token_data::DataOutcome::kOk) return false;
  return trends_of(a, rows, source, now_ms, trends);
}

namespace {

struct DistributionRow {
  const Value* row;
  double at;
};

// distributionRows(network, tokenId, days, now, source).
size_t distribution_rows(const Value* rows, int64_t now, int64_t days, DistributionRow* out, size_t capacity) {
  const double cutoff = static_cast<double>(now - (days < 1 ? 1 : days) * kDayMs);
  size_t n = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count && n < capacity; ++i) {
    const Value* row = rows->items[i];
    bool has = false;
    const double at = date_of(row, "distributionObservedAt", &has);
    if (is_null(tj::Get(row, "distributionHash")) || !has || at < cutoff || at > static_cast<double>(now)) continue;
    size_t pos = n;
    while (pos > 0 && out[pos - 1].at > at) { out[pos] = out[pos - 1]; --pos; }
    out[pos] = DistributionRow{row, at};
    ++n;
  }
  if (!n) return 0;
  const Value* latest_source = tj::Get(out[n - 1].row, "source");
  size_t kept = 0;
  for (size_t i = 0; i < n; ++i) {
    const Value* s = tj::Get(out[i].row, "source");
    if (tj::IsString(s) && tj::IsString(latest_source) && s->length == latest_source->length &&
        memcmp(s->text, latest_source->text, s->length) == 0)
      out[kept++] = out[i];
  }
  return kept;
}

Value* number_value(Arena& a, const Value* v) {
  double n = 0;
  return number_of(v, &n) ? tj::Num(a, n) : tj::Null(a);
}

Value* difference(Arena& a, const Value* x, const Value* y) {
  double p = 0, q = 0;
  return number_of(x, &p) && number_of(y, &q) ? tj::Num(a, p - q) : tj::Null(a);
}

Value* summary_json(Arena& a, const Value* row) {
  bool has = false;
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "at", iso(a, static_cast<int64_t>(date_of(row, "distributionObservedAt", &has)))) ||
      !tj::Set(a, o, "source", tj::Clone(a, tj::Get(row, "source"))) ||
      !tj::Set(a, o, "positionCount", tj::Clone(a, tj::Get(row, "positionCount"))) ||
      !tj::Set(a, o, "topHoldersPercent", number_value(a, tj::Get(row, "topHoldersPercent"))) ||
      !tj::Set(a, o, "topHoldersPercentExcludingLocked",
               number_value(a, tj::Get(row, "topHoldersPercentExcludingLocked"))) ||
      !tj::Set(a, o, "burnedPercent", number_value(a, tj::Get(row, "burnedPercent"))))
    return nullptr;
  return o;
}

const DistributionRow* nearest_baseline(const DistributionRow* rows, size_t n, int64_t days, int64_t tolerance) {
  const double target = rows[n - 1].at - static_cast<double>(days * kDayMs);
  const DistributionRow* best = nullptr;
  for (size_t i = 0; i + 1 < n; ++i) {
    const double distance = fabs(rows[i].at - target);
    if (distance > static_cast<double>(tolerance)) continue;
    if (!best || distance < fabs(best->at - target)) best = &rows[i];
  }
  return best;
}

struct Position {
  double rank;
  const Value* address;
  const Value* tag;
  Value* percent;
  Value* balance;
  const Value* is_contract;
  const Value* is_locked;
};

size_t positions_of(Arena& a, const Value* row, Position* out, size_t capacity) {
  const Value* list = tj::Get(row, "positions");
  size_t n = 0;
  for (uint32_t i = 0; tj::IsArray(list) && i < list->count && n < capacity; ++i) {
    const Value* p = list->items[i];
    const Value* rank = tj::Get(p, "rank");
    Position position{tj::IsNumber(rank) ? rank->number : 0, tj::Get(p, "address"), tj::Get(p, "tag"),
                      number_value(a, tj::Get(p, "percent")), number_value(a, tj::Get(p, "balance")),
                      tj::Get(p, "isContract"), tj::Get(p, "isLocked")};
    // positionRows are sorted by rank (stable).
    size_t pos = n;
    while (pos > 0 && out[pos - 1].rank > position.rank) { out[pos] = out[pos - 1]; --pos; }
    out[pos] = position;
    ++n;
  }
  return n;
}

bool same_text(const Value* x, const Value* y) {
  return tj::IsString(x) && tj::IsString(y) && x->length == y->length && memcmp(x->text, y->text, x->length) == 0;
}

// Map semantics: the last row for an address wins.
const Position* find_position(const Position* rows, size_t n, const Value* address) {
  const Position* found = nullptr;
  for (size_t i = 0; i < n; ++i) if (same_text(rows[i].address, address)) found = &rows[i];
  return found;
}

Value* clone_or_null(Arena& a, const Value* v) { return v ? tj::Clone(a, v) : tj::Null(a); }

bool nonzero(const Value* v) { return tj::IsNumber(v) && v->number != 0; }

bool flag_equal(const Value* x, const Value* y) {
  const auto kind = [](const Value* v) { return !v ? 0 : v->type == tj::Type::kTrue ? 1 : v->type == tj::Type::kFalse ? 2 : 3; };
  return kind(x) == kind(y);
}

struct Change {
  Value* object;
  double current_rank;
  double previous_rank;
  const Value* address;
};

// comparePositions(current, previous).
Value* compare_positions(Arena& a, const Position* current, size_t current_count, const Position* previous,
                         size_t previous_count) {
  const size_t capacity = current_count + previous_count + 1;
  auto* addresses = static_cast<const Value**>(a.Alloc(sizeof(Value*) * capacity));
  auto* changes = static_cast<Change*>(a.Alloc(sizeof(Change) * capacity));
  if (!addresses || !changes) return nullptr;
  size_t count = 0;
  auto add = [&](const Value* address) {
    for (size_t i = 0; i < count; ++i) if (same_text(addresses[i], address)) return;
    addresses[count++] = address;
  };
  for (size_t i = 0; i < current_count; ++i) add(current[i].address);
  for (size_t i = 0; i < previous_count; ++i) add(previous[i].address);
  const double kMax = 9007199254740991.0;
  for (size_t i = 0; i < count; ++i) {
    const Position* now = find_position(current, current_count, addresses[i]);
    const Position* before = find_position(previous, previous_count, addresses[i]);
    Value* rank_change = now && before ? tj::Num(a, before->rank - now->rank) : tj::Null(a);
    Value* pp_change = difference(a, now ? now->percent : nullptr, before ? before->percent : nullptr);
    Value* balance_change = difference(a, now ? now->balance : nullptr, before ? before->balance : nullptr);
    const char* kind = !now ? "exited"
                     : !before ? "entered"
                     : nonzero(rank_change) || nonzero(pp_change) || nonzero(balance_change) ||
                               !flag_equal(now->is_contract, before->is_contract) ||
                               !flag_equal(now->is_locked, before->is_locked)
                         ? "changed"
                         : "unchanged";
    const Value* tag = now && !is_null(now->tag) ? now->tag : before && !is_null(before->tag) ? before->tag : nullptr;
    Value* o = tj::Obj(a);
    auto flag = [&](const Position* p, bool contract) -> Value* {
      if (!p) return tj::Null(a);
      const Value* v = contract ? p->is_contract : p->is_locked;
      return v ? tj::Clone(a, v) : tj::Null(a);
    };
    if (!o || !tj::Set(a, o, "address", tj::Clone(a, addresses[i])) ||
        !tj::Set(a, o, "tag", clone_or_null(a, tag)) || !tj::Set(a, o, "kind", tj::Str(a, kind)) ||
        !tj::Set(a, o, "currentRank", now ? tj::Num(a, now->rank) : tj::Null(a)) ||
        !tj::Set(a, o, "previousRank", before ? tj::Num(a, before->rank) : tj::Null(a)) ||
        !tj::Set(a, o, "rankChange", rank_change) ||
        !tj::Set(a, o, "currentPercent", now ? tj::Clone(a, now->percent) : tj::Null(a)) ||
        !tj::Set(a, o, "previousPercent", before ? tj::Clone(a, before->percent) : tj::Null(a)) ||
        !tj::Set(a, o, "percentagePointChange", pp_change) ||
        !tj::Set(a, o, "currentBalance", now ? tj::Clone(a, now->balance) : tj::Null(a)) ||
        !tj::Set(a, o, "previousBalance", before ? tj::Clone(a, before->balance) : tj::Null(a)) ||
        !tj::Set(a, o, "balanceChange", balance_change) ||
        !tj::Set(a, o, "currentIsContract", flag(now, true)) || !tj::Set(a, o, "previousIsContract", flag(before, true)) ||
        !tj::Set(a, o, "currentIsLocked", flag(now, false)) || !tj::Set(a, o, "previousIsLocked", flag(before, false)))
      return nullptr;
    changes[i] = Change{o, now ? now->rank : kMax, before ? before->rank : kMax, addresses[i]};
  }
  // Stable sort: currentRank, previousRank, address in byte order (the ranks
  // are unique per day, so the address tie-break never decides).
  for (size_t i = 1; i < count; ++i) {
    const Change item = changes[i];
    size_t j = i;
    auto before = [&](const Change& x, const Change& y) {
      if (x.current_rank != y.current_rank) return x.current_rank < y.current_rank;
      if (x.previous_rank != y.previous_rank) return x.previous_rank < y.previous_rank;
      const size_t n = x.address->length < y.address->length ? x.address->length : y.address->length;
      const int c = memcmp(x.address->text, y.address->text, n);
      return c < 0 || (c == 0 && x.address->length < y.address->length);
    };
    while (j > 0 && before(item, changes[j - 1])) { changes[j] = changes[j - 1]; --j; }
    changes[j] = item;
  }
  Value* out = tj::Arr(a);
  for (size_t i = 0; out && i < count; ++i) if (!tj::Push(a, out, changes[i].object)) return nullptr;
  return out;
}

}  // namespace

bool HolderHistoryDocument(Work& work, const char* network, const char* token_id, double days, const char* source,
                           int64_t now_ms, Value** document) {
  Arena& a = work.arena;
  *document = nullptr;
  Value* rows = nullptr;
  if (work.env->data->HolderRows(network, token_id, work.Now(), a, &rows) != token_data::DataOutcome::kOk) return false;
  const size_t capacity = tj::IsArray(rows) ? rows->count + 1 : 1;
  const int64_t whole_days = static_cast<int64_t>(days);
  // history(network, tokenId, days): count evidence, newest first.
  Value* observations = tj::Arr(a);
  if (!observations) return false;
  {
    const double cutoff = static_cast<double>(now_ms - (whole_days < 1 ? 1 : whole_days) * kDayMs);
    auto* picked = static_cast<DistributionRow*>(a.Alloc(sizeof(DistributionRow) * capacity));
    if (!picked) return false;
    size_t n = 0;
    for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i) {
      const Value* row = rows->items[i];
      bool has = false;
      const double at = date_of(row, "countObservedAt", &has);
      if (is_null(tj::Get(row, "holderCount")) || !has || at < cutoff || at > static_cast<double>(now_ms)) continue;
      // sort by countObservedAt descending (stable)
      size_t pos = n;
      while (pos > 0 && picked[pos - 1].at < at) { picked[pos] = picked[pos - 1]; --pos; }
      picked[pos] = DistributionRow{row, at};
      ++n;
    }
    for (size_t i = 0; i < n; ++i) {
      const Value* row = picked[i].row;
      Value* o = tj::Obj(a);
      if (!o || !tj::Set(a, o, "at", iso(a, static_cast<int64_t>(picked[i].at))) ||
          !tj::Set(a, o, "count", tj::Clone(a, tj::Get(row, "holderCount"))) ||
          !tj::Set(a, o, "atLeast", clone_or_null(a, tj::Get(row, "atLeast"))) ||
          !tj::Set(a, o, "source", clone_or_null(a, tj::Get(row, "source"))) || !tj::Push(a, observations, o))
        return false;
    }
  }
  // trends(network, tokenId, {source}) over the same rows: TypeScript's three
  // reads run synchronously against one LMDB view.
  Value* trends = nullptr;
  if (!trends_of(a, rows, source, now_ms, &trends)) return false;
  Value* trends_json = tj::Null(a);
  if (tj::IsObject(trends)) {
    trends_json = tj::Obj(a);
    if (!trends_json) return false;
    for (const Window& window : kWindows)
      if (!tj::Set(a, trends_json, window.key, tj::Clone(a, tj::Get(trends, window.key)))) return false;
    if (!tj::Set(a, trends_json, "latestAt",
                 iso(a, static_cast<int64_t>(tj::Get(trends, "latestAt")->number))))
      return false;
  }
  // distributionReport(network, tokenId, days).
  auto* dist = static_cast<DistributionRow*>(a.Alloc(sizeof(DistributionRow) * capacity));
  if (!dist) return false;
  const int64_t requested = whole_days < 1 ? 1 : whole_days;
  const size_t n = distribution_rows(rows, now_ms, requested > kTrendLookbackDays ? requested : kTrendLookbackDays,
                                     dist, capacity);
  Value* snapshots = tj::Arr(a);
  Value* comparisons = tj::Arr(a);
  if (!snapshots || !comparisons) return false;
  const double cutoff = static_cast<double>(now_ms - requested * kDayMs);
  for (size_t i = n; i-- > 0;) {
    if (dist[i].at < cutoff) continue;
    Value* summary = summary_json(a, dist[i].row);
    if (!summary || !tj::Push(a, snapshots, summary)) return false;
  }
  if (n) {
    const DistributionRow& latest = dist[n - 1];
    // Every stored position of each day (normalizePositions keeps the
    // provider's whole list; TypeScript compares them all).
    const auto stored = [](const Value* row) {
      const Value* list = tj::Get(row, "positions");
      return static_cast<size_t>(tj::IsArray(list) ? list->count : 0) + 1;
    };
    size_t widest = 1;
    for (size_t i = 0; i < n; ++i) widest = stored(dist[i].row) > widest ? stored(dist[i].row) : widest;
    auto* current = static_cast<Position*>(a.Alloc(sizeof(Position) * stored(latest.row)));
    auto* previous = static_cast<Position*>(a.Alloc(sizeof(Position) * widest));
    if (!current || !previous) return false;
    const size_t current_count = positions_of(a, latest.row, current, stored(latest.row));
    for (const Window& window : kWindows) {
      const DistributionRow* baseline = nearest_baseline(dist, n, window.days, window.tolerance_ms);
      Value* o = tj::Obj(a);
      Value* positions = nullptr;
      if (baseline) {
        const size_t previous_count = positions_of(a, baseline->row, previous, widest);
        positions = compare_positions(a, current, current_count, previous, previous_count);
      } else {
        positions = tj::Arr(a);
      }
      const Value* br = baseline ? baseline->row : nullptr;
      if (!o || !positions || !tj::Set(a, o, "window", tj::Str(a, window.label)) ||
          !tj::Set(a, o, "latestAt", iso(a, static_cast<int64_t>(latest.at))) ||
          !tj::Set(a, o, "baselineAt", baseline ? iso(a, static_cast<int64_t>(baseline->at)) : tj::Null(a)) ||
          !tj::Set(a, o, "topHoldersPercentPointChange",
                   difference(a, tj::Get(latest.row, "topHoldersPercent"), tj::Get(br, "topHoldersPercent"))) ||
          !tj::Set(a, o, "liveWalletPercentPointChange",
                   difference(a, tj::Get(latest.row, "topHoldersPercentExcludingLocked"),
                              tj::Get(br, "topHoldersPercentExcludingLocked"))) ||
          !tj::Set(a, o, "burnedPercentPointChange",
                   difference(a, tj::Get(latest.row, "burnedPercent"), tj::Get(br, "burnedPercent"))) ||
          !tj::Set(a, o, "positions", positions) || !tj::Push(a, comparisons, o))
        return false;
    }
  }
  Value* distribution = tj::Obj(a);
  Value* doc = tj::Obj(a);
  if (!distribution || !doc || !tj::Set(a, distribution, "snapshots", snapshots) ||
      !tj::Set(a, distribution, "comparisons", comparisons) || !tj::Set(a, doc, "network", tj::Str(a, network)) ||
      !tj::Set(a, doc, "tokenId", tj::Str(a, token_id)) ||
      !tj::Set(a, doc, "trendSource", source ? tj::Str(a, source) : tj::Null(a)) ||
      !tj::Set(a, doc, "observations", observations) || !tj::Set(a, doc, "trends", trends_json) ||
      !tj::Set(a, doc, "distribution", distribution))
    return false;
  *document = doc;
  return a.ok;
}

// ============================================================================
// LiquiditySnapshotStore
// ============================================================================
namespace {

int64_t observation_bucket(int64_t at) {
  int64_t bucket = at / 300000;
  if (at % 300000 < 0) --bucket;
  return bucket * 300000;
}

bool observation_from_snapshot(const Value* snapshot, Observation* o) {
  double v = 0;
  auto field = [&](const char* key, bool* has, double* out) {
    *has = token_views::FiniteNumber(tj::Get(snapshot, key), &v);
    *out = *has ? v : 0;
  };
  field("totalUsd", &o->has_liquidity, &o->liquidity);
  field("weightedPriceUsd", &o->has_price, &o->price);
  field("priceSpreadPct", &o->has_spread, &o->spread);
  field("priceSpreadPools", &o->has_spread_pools, &o->spread_pools);
  field("topPoolShare", &o->has_top_share, &o->top_share);
  field("fdv", &o->has_fdv, &o->fdv);
  field("marketCap", &o->has_market_cap, &o->market_cap);
  const Value* pools = tj::Get(snapshot, "poolCount");
  o->pool_count = tj::IsNumber(pools) ? pools->number : 0;
  // lastWindow(volumeUsd, 'h24') and lastWindowObject(txns, 'h24').
  o->has_volume_h24 = token_views::FiniteNumberOrString(tj::Get(tj::Get(snapshot, "volumeUsd"), "h24"), &v);
  o->volume_h24 = o->has_volume_h24 ? v : 0;
  const Value* trades = tj::Get(tj::Get(snapshot, "txns"), "h24");
  if (tj::IsObject(trades) || tj::IsArray(trades)) {
    o->has_buys_h24 = token_views::FiniteNumberOrString(tj::Get(trades, "buys"), &v);
    o->buys_h24 = o->has_buys_h24 ? v : 0;
    o->has_sells_h24 = token_views::FiniteNumberOrString(tj::Get(trades, "sells"), &v);
    o->sells_h24 = o->has_sells_h24 ? v : 0;
  }
  return true;
}

void copy_source(char (&out)[24], const char* source) {
  snprintf(out, sizeof(out), "%s", source ? source : "dexscreener");
}

// The comparison patch of a not-found or diagnostic outcome.
void liquidity_status_patch(const char* status, int64_t checked_at, const char* source,
                            token_store::ComparisonPatch* patch) {
  using token_store::FieldState;
  *patch = token_store::ComparisonPatch{};
  patch->fiat = {FieldState::kValue, "usd", 3};
  comparison::LiquidityStatus value = comparison::LiquidityStatus::kNone;
  if (strcmp(status, "not_found") == 0) value = comparison::LiquidityStatus::kNotFound;
  else if (strcmp(status, "unsupported") == 0) value = comparison::LiquidityStatus::kUnsupported;
  else if (strcmp(status, "error") == 0) value = comparison::LiquidityStatus::kError;
  else if (strcmp(status, "ok") == 0) value = comparison::LiquidityStatus::kOk;
  patch->liquidity_status = {FieldState::kValue, value};
  patch->liquidity_checked_at = {FieldState::kValue, checked_at};
  patch->liquidity_source = {FieldState::kValue, source, strlen(source)};
  patch->updated_at = {FieldState::kValue, checked_at};
}

}  // namespace

// Each outcome is one tokens.write transaction in TypeScript and one KV
// frame here (token_writes::RecordLiquidity): facts, comparison and, for ok
// and not_found, the raw observation with its pair row.
bool RecordSuccess(Work& work, const char* network, const char* token_id, const Value* snapshot,
                   const ObservationContext& context) {
  Arena& a = work.arena;
  const size_t mark = a.used;
  const int64_t observed = context.has_observed_at ? context.observed_at : work.Now();
  const char* source = context.source ? context.source : "dexscreener";
  Value* fields = tj::Obj(a);
  bool ok = fields && tj::Set(a, fields, "liquidity", tj::Clone(a, snapshot)) &&
            tj::Set(a, fields, "liquidityAsOf", tj::Num(a, static_cast<double>(observed))) &&
            tj::Set(a, fields, "liquidityStatus", tj::Str(a, "ok")) &&
            tj::Set(a, fields, "liquidityCheckedAt", tj::Num(a, static_cast<double>(observed))) &&
            tj::Set(a, fields, "liquiditySource", tj::Str(a, source));
  token_store::ComparisonPatch patch{};
  token_views::LiquidityComparisonPatch(snapshot, observed, source, &patch);
  Observation o{};
  o.ts = observation_bucket(observed);
  o.ok = true;
  copy_source(o.source, source);
  o.has_sampling = context.has_sampling;
  o.sampling_seconds = context.sampling_seconds;
  observation_from_snapshot(snapshot, &o);
  token_writes::LiquidityOutcome outcome{};
  outcome.network = network;
  outcome.token_id = token_id;
  outcome.facts = fields;
  outcome.comparison = &patch;
  outcome.observation = &o;
  ok = ok && token_writes::RecordLiquidity(*work.env->data, outcome, work.Now());
  a.used = mark;
  a.ok = true;
  return ok;
}

bool RecordNotFound(Work& work, const char* network, const char* token_id, const ObservationContext& context) {
  Arena& a = work.arena;
  const size_t mark = a.used;
  const int64_t checked = context.has_observed_at ? context.observed_at : work.Now();
  const char* source = context.source ? context.source : "dexscreener";
  Value* fields = tj::Obj(a);
  bool ok = fields && tj::Set(a, fields, "liquidityStatus", tj::Str(a, "not_found")) &&
            tj::Set(a, fields, "liquidityCheckedAt", tj::Num(a, static_cast<double>(checked))) &&
            tj::Set(a, fields, "liquiditySource", tj::Str(a, source));
  token_store::ComparisonPatch patch{};
  liquidity_status_patch("not_found", checked, source, &patch);
  Observation o{};
  o.ts = observation_bucket(checked);
  o.ok = false;
  copy_source(o.source, source);
  o.has_sampling = context.has_sampling;
  o.sampling_seconds = context.sampling_seconds;
  o.pool_count = 0;
  token_writes::LiquidityOutcome outcome{};
  outcome.network = network;
  outcome.token_id = token_id;
  outcome.facts = fields;
  outcome.comparison = &patch;
  outcome.observation = &o;
  ok = ok && token_writes::RecordLiquidity(*work.env->data, outcome, work.Now());
  a.used = mark;
  a.ok = true;
  return ok;
}

bool RecordDiagnostic(Work& work, const char* network, const char* token_id, const char* status,
                      int64_t checked_at, const char* source) {
  Arena& a = work.arena;
  const size_t mark = a.used;
  const char* provider = source ? source : "dexscreener";
  Value* fields = tj::Obj(a);
  bool ok = fields && tj::Set(a, fields, "liquidityStatus", tj::Str(a, status)) &&
            tj::Set(a, fields, "liquidityCheckedAt", tj::Num(a, static_cast<double>(checked_at))) &&
            tj::Set(a, fields, "liquiditySource", tj::Str(a, provider));
  token_store::ComparisonPatch patch{};
  liquidity_status_patch(status, checked_at, provider, &patch);
  token_writes::LiquidityOutcome outcome{};
  outcome.network = network;
  outcome.token_id = token_id;
  outcome.facts = fields;
  outcome.comparison = &patch;
  ok = ok && token_writes::RecordLiquidity(*work.env->data, outcome, work.Now());
  a.used = mark;
  a.ok = true;
  return ok;
}

// ============================================================================
// LiquidityHistoryFacet
// ============================================================================
namespace {

constexpr int64_t kRollupLagMs = 2 * kHourMs;
constexpr size_t kMaxObservations = 4096;  // 7 days of 5-minute buckets is 2,016
constexpr size_t kMaxCandles = 4096;       // 90 days of hourly candles is 2,160

int64_t range_days(Range range) {
  switch (range) {
    case Range::k1d: return 1;
    case Range::k7d: return 7;
    case Range::k30d: return 30;
    case Range::k90d: return 90;
    case Range::k1y: return 365;
  }
  return 30;
}
const char* range_name(Range range) {
  switch (range) {
    case Range::k1d: return "1d";
    case Range::k7d: return "7d";
    case Range::k30d: return "30d";
    case Range::k90d: return "90d";
    case Range::k1y: return "1y";
  }
  return "30d";
}
const char* interval_name(Interval interval) {
  return interval == Interval::k1h ? "1h" : interval == Interval::k1d ? "1d" : "1w";
}
token_data::Interval data_interval(Interval interval) {
  return interval == Interval::k1h ? token_data::Interval::k1h
       : interval == Interval::k1d ? token_data::Interval::k1d : token_data::Interval::k1w;
}

// liquidityBucket(at, interval).
int64_t bucket(int64_t at, Interval interval) {
  if (interval == Interval::k1h) {
    int64_t h = at / kHourMs; if (at % kHourMs < 0) --h; return h * kHourMs;
  }
  const int64_t day = utc_day(at);
  if (interval == Interval::k1d) return day;
  // getUTCDay(): 1970-01-01 was a Thursday (4).
  int64_t days = day / kDayMs;
  int64_t weekday = (days + 4) % 7; if (weekday < 0) weekday += 7;
  const int64_t monday_offset = (weekday + 6) % 7;
  return day - monday_offset * kDayMs;
}

Value* seconds(Arena& a, int64_t ms) {
  int64_t s = ms / 1000;
  if (ms % 1000 < 0) --s;
  return tj::Num(a, static_cast<double>(s));
}

Value* percent_change(Arena& a, bool has_current, double current, bool has_baseline, double baseline) {
  if (!has_current || !has_baseline || baseline == 0) return tj::Null(a);
  return tj::Num(a, (current - baseline) / baseline * 100);
}

struct Freshness {
  bool has_age = false;
  double age_seconds = 0;
  bool stale = true;
};

Freshness snapshot_freshness(const Observation* latest, int64_t now) {
  Freshness f{};
  if (latest) {
    f.has_age = true;
    const double age = floor(static_cast<double>(now - latest->ts) / 1000.0);
    f.age_seconds = age > 0 ? age : 0;
  }
  const double cadence = latest && latest->has_sampling ? latest->sampling_seconds : 300;
  const double limit = 1800 > cadence * 2 ? 1800 : cadence * 2;
  f.stale = !f.has_age || f.age_seconds > limit;
  return f;
}

Value* change_at(Arena& a, const Observation* rows, size_t count, const Observation* latest, int64_t ago,
                 int64_t tolerance) {
  if (!latest) return tj::Null(a);
  const double target = static_cast<double>(latest->ts - ago);
  const Observation* baseline = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const double distance = fabs(static_cast<double>(rows[i].ts) - target);
    if (distance > static_cast<double>(tolerance)) continue;
    if (!baseline || distance < fabs(static_cast<double>(baseline->ts) - target)) baseline = &rows[i];
  }
  if (!baseline) return tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "baselineAt", seconds(a, baseline->ts)) ||
      !tj::Set(a, o, "liquidityPct", percent_change(a, latest->has_liquidity, latest->liquidity,
                                                    baseline->has_liquidity, baseline->liquidity)) ||
      !tj::Set(a, o, "pricePct", percent_change(a, latest->has_price, latest->price, baseline->has_price,
                                                baseline->price)))
    return nullptr;
  return o;
}

struct Availability {
  bool found = false;
  Value* status = nullptr;
  Value* checked_at = nullptr;
  Value* last_success = nullptr;
  Value* source = nullptr;
};

bool availability_facts(Work& work, const char* network, const char* token_id, Availability* out) {
  Arena& a = work.arena;
  Value* row = nullptr;
  const token_data::DataOutcome status = work.env->data->ReadFacts(network, token_id, work.Now(), a, &row);
  if (status == token_data::DataOutcome::kMissing || (status == token_data::DataOutcome::kOk && !tj::IsObject(row))) {
    *out = Availability{};
    return true;
  }
  if (status != token_data::DataOutcome::kOk) return false;
  out->found = true;
  const Value* s = tj::Get(row, "liquidityStatus");
  out->status = is_null(s) ? tj::Null(a) : tj::Clone(a, s);
  const Value* checked = tj::Get(row, "liquidityCheckedAt");
  out->checked_at = tj::IsNumber(checked) ? iso(a, static_cast<int64_t>(checked->number)) : tj::Null(a);
  const Value* success = tj::Get(row, "liquidityAsOf");
  out->last_success = tj::IsNumber(success) ? iso(a, static_cast<int64_t>(success->number)) : tj::Null(a);
  const Value* source = tj::Get(row, "liquiditySource");
  out->source = is_null(source) ? tj::Null(a) : tj::Clone(a, source);
  return true;
}

bool set_availability(Arena& a, Value* o, const Availability& f) {
  return tj::Set(a, o, "status", f.found && !is_null(f.status) ? f.status : tj::Str(a, "unknown")) &&
         tj::Set(a, o, "checkedAt", f.found ? f.checked_at : tj::Null(a)) &&
         tj::Set(a, o, "lastSuccessfulAt", f.found ? f.last_success : tj::Null(a)) &&
         tj::Set(a, o, "source", f.found ? f.source : tj::Null(a));
}

// Row storage sized by the window: one raw row per five-minute bucket, one
// candle per interval bucket. A raw row lives seven days from its arrival,
// not from its timestamp (a backfilled row is still read), so the window
// alone bounds it. Every caller's window is bounded: at most three days of
// five-minute buckets (the rollup), a week (a 1w chart's open bucket).
size_t window_rows(int64_t from, int64_t to, int64_t span, size_t cap) {
  if (to < from) return 1;
  const double rows = static_cast<double>(to - from) / static_cast<double>(span) + 2;
  return rows >= static_cast<double>(cap) ? cap : static_cast<size_t>(rows);
}

bool read_observations(Work& work, const char* network, const char* token_id, int64_t from, int64_t to,
                       token_data::Rows<Observation>* rows) {
  const size_t capacity = window_rows(from, to, 300000, kMaxObservations);
  rows->items = static_cast<Observation*>(work.arena.Alloc(sizeof(Observation) * capacity));
  rows->capacity = rows->items ? capacity : 0;
  if (!rows->items) return false;
  return work.env->data->Observations(network, token_id, from, to, work.Now(), rows) == token_data::DataOutcome::kOk &&
         !rows->truncated;
}

// latestSuccessful's predicate: ok && (liquidity !== null || price !== null).
bool usable_latest(const Observation& o) { return o.ok && (o.has_liquidity || o.has_price); }

bool read_candles(Work& work, const char* network, const char* token_id, token_data::Interval interval, int64_t from,
                  int64_t to, token_data::Rows<Candle>* rows) {
  const int64_t span = interval == token_data::Interval::k1h ? kHourMs
                     : interval == token_data::Interval::k1d ? kDayMs : 7 * kDayMs;
  const size_t capacity = window_rows(from, to, span, kMaxCandles);
  rows->items = static_cast<Candle*>(work.arena.Alloc(sizeof(Candle) * capacity));
  rows->capacity = rows->items ? capacity : 0;
  if (!rows->items) return false;
  return work.env->data->Candles(network, token_id, interval, from, to, work.Now(), rows) == token_data::DataOutcome::kOk;
}

// buildLiquidityCandles(rows, interval). `rows` are in ts order.
size_t build_candles(const Observation* rows, size_t count, Interval interval, Candle* out, size_t capacity) {
  size_t n = 0;
  size_t i = 0;
  while (i < count && n < capacity) {
    const int64_t start = bucket(rows[i].ts, interval);
    size_t end = i;
    while (end < count && bucket(rows[end].ts, interval) == start) ++end;
    Candle c{};
    c.ts = start;
    const Observation& first = rows[i];
    const Observation& last = rows[end - 1];
    auto ohlc = [&](size_t base, bool Observation::*has, double Observation::*value) {
      c.has[base] = first.*has; c.value[base] = first.*value;
      bool any = false; double hi = 0, lo = 0;
      for (size_t k = i; k < end; ++k) {
        if (!(rows[k].*has)) continue;
        const double v = rows[k].*value;
        if (!any || v > hi) hi = v;
        if (!any || v < lo) lo = v;
        any = true;
      }
      c.has[base + 1] = any; c.value[base + 1] = hi;
      c.has[base + 2] = any; c.value[base + 2] = lo;
      c.has[base + 3] = last.*has; c.value[base + 3] = last.*value;
    };
    ohlc(0, &Observation::has_liquidity, &Observation::liquidity);
    ohlc(4, &Observation::has_price, &Observation::price);
    auto close_max = [&](size_t base, bool Observation::*has, double Observation::*value) {
      c.has[base] = last.*has; c.value[base] = last.*value;
      bool any = false; double hi = 0;
      for (size_t k = i; k < end; ++k) {
        if (!(rows[k].*has)) continue;
        if (!any || rows[k].*value > hi) hi = rows[k].*value;
        any = true;
      }
      c.has[base + 1] = any; c.value[base + 1] = hi;
    };
    close_max(8, &Observation::has_spread, &Observation::spread);
    close_max(10, &Observation::has_top_share, &Observation::top_share);
    c.has[12] = true; c.value[12] = first.pool_count;
    c.has[13] = true; c.value[13] = last.pool_count;
    c.has[14] = last.has_volume_h24; c.value[14] = last.volume_h24;
    c.has[15] = last.has_buys_h24; c.value[15] = last.buys_h24;
    c.has[16] = last.has_sells_h24; c.value[16] = last.sells_h24;
    c.sample_count = static_cast<double>(end - i);
    double cadence = 300;
    for (size_t k = end; k-- > i;)
      if (rows[k].has_sampling && rows[k].sampling_seconds > 0) { cadence = rows[k].sampling_seconds; break; }
    const double interval_seconds = interval == Interval::k1h ? 3600 : interval == Interval::k1d ? 86400 : 604800;
    const double expected = floor(interval_seconds / cadence + 0.5);
    c.expected_samples = expected > 1 ? expected : 1;
    snprintf(c.source, sizeof(c.source), "%s", last.source);
    out[n++] = c;
    i = end;
  }
  return n;
}

Value* candle_number(Arena& a, const Candle& c, size_t field) {
  return c.has[field] ? tj::Num(a, c.value[field]) : tj::Null(a);
}

Value* candle_view(Arena& a, const Candle& c) {
  Value* o = tj::Obj(a);
  Value* liquidity = tj::Obj(a);
  Value* price = tj::Obj(a);
  Value* spread = tj::Obj(a);
  Value* concentration = tj::Obj(a);
  Value* pools = tj::Obj(a);
  Value* rolling = tj::Obj(a);
  Value* coverage = tj::Obj(a);
  if (!o || !liquidity || !price || !spread || !concentration || !pools || !rolling || !coverage) return nullptr;
  bool ok =
      tj::Set(a, liquidity, "open", candle_number(a, c, 0)) && tj::Set(a, liquidity, "high", candle_number(a, c, 1)) &&
      tj::Set(a, liquidity, "low", candle_number(a, c, 2)) && tj::Set(a, liquidity, "close", candle_number(a, c, 3)) &&
      tj::Set(a, liquidity, "changePct", percent_change(a, c.has[3], c.value[3], c.has[0], c.value[0])) &&
      tj::Set(a, price, "open", candle_number(a, c, 4)) && tj::Set(a, price, "high", candle_number(a, c, 5)) &&
      tj::Set(a, price, "low", candle_number(a, c, 6)) && tj::Set(a, price, "close", candle_number(a, c, 7)) &&
      tj::Set(a, price, "changePct", percent_change(a, c.has[7], c.value[7], c.has[4], c.value[4])) &&
      tj::Set(a, spread, "closePct", candle_number(a, c, 8)) && tj::Set(a, spread, "maxPct", candle_number(a, c, 9)) &&
      tj::Set(a, concentration, "close", candle_number(a, c, 10)) &&
      tj::Set(a, concentration, "max", candle_number(a, c, 11)) &&
      tj::Set(a, pools, "open", candle_number(a, c, 12)) && tj::Set(a, pools, "close", candle_number(a, c, 13)) &&
      tj::Set(a, rolling, "volumeUsd", candle_number(a, c, 14)) && tj::Set(a, rolling, "buys", candle_number(a, c, 15)) &&
      tj::Set(a, rolling, "sells", candle_number(a, c, 16)) &&
      tj::Set(a, coverage, "samples", tj::Num(a, isfinite(c.sample_count) ? c.sample_count : 0)) &&
      tj::Set(a, coverage, "expected", tj::Num(a, isfinite(c.expected_samples) ? c.expected_samples : 0)) &&
      tj::Set(a, coverage, "ratio",
              isfinite(c.sample_count) && isfinite(c.expected_samples) && c.expected_samples > 0
                  ? tj::Num(a, fmin(1, c.sample_count / c.expected_samples))
                  : tj::Null(a));
  ok = ok && tj::Set(a, o, "ts", seconds(a, c.ts)) && tj::Set(a, o, "liquidity", liquidity) &&
       tj::Set(a, o, "price", price) && tj::Set(a, o, "spread", spread) &&
       tj::Set(a, o, "concentration", concentration) && tj::Set(a, o, "pools", pools) &&
       tj::Set(a, o, "rolling24h", rolling) && tj::Set(a, o, "coverage", coverage) &&
       tj::Set(a, o, "source", tj::Str(a, c.source));
  return ok ? o : nullptr;
}

struct ChangeWindow {
  const char* key;
  int64_t ago;
  int64_t tolerance;
};
constexpr ChangeWindow kChangeWindows[] = {
    {"h1", kHourMs, 30 * 60000},
    {"h24", kDayMs, 2 * kHourMs},
    {"d7", 7 * kDayMs, 12 * kHourMs},
    {"d30", 30 * kDayMs, kDayMs},
};

}  // namespace

bool ParseRange(const char* text, size_t length, Range* range) {
  struct Name { const char* name; Range value; };
  static constexpr Name kNames[] = {{"1d", Range::k1d}, {"7d", Range::k7d}, {"30d", Range::k30d},
                                    {"90d", Range::k90d}, {"1y", Range::k1y}};
  for (const Name& n : kNames)
    if (strlen(n.name) == length && memcmp(n.name, text, length) == 0) { *range = n.value; return true; }
  return false;
}

bool ParseInterval(const char* text, size_t length, Interval* interval) {
  struct Name { const char* name; Interval value; };
  static constexpr Name kNames[] = {{"1h", Interval::k1h}, {"1d", Interval::k1d}, {"1w", Interval::k1w}};
  for (const Name& n : kNames)
    if (strlen(n.name) == length && memcmp(n.name, text, length) == 0) { *interval = n.value; return true; }
  return false;
}

bool ResolutionAllowed(Range range, Interval interval) {
  return interval != Interval::k1h || range_days(range) <= 90;
}

bool LiquidityHistoryDocument(Work& work, const char* network, const char* token_id, Range range,
                              Interval interval, int64_t now, Value** document, Value** validator) {
  Arena& a = work.arena;
  *document = *validator = nullptr;
  const int64_t from = now - range_days(range) * kDayMs;
  const int64_t lag_start = bucket(now - kRollupLagMs, interval);
  const int64_t raw_from = lag_start > from ? lag_start : from;
  Availability facts{};
  if (!availability_facts(work, network, token_id, &facts)) return false;
  // latest(network, tokenId, now, ok && (liquidity || price)): newest first.
  Observation newest{};
  bool found = false;
  if (work.env->data->LatestObservation(network, token_id, now, work.Now(), usable_latest, &newest, &found) !=
      token_data::DataOutcome::kOk)
    return false;
  const Observation* latest = found ? &newest : nullptr;
  token_data::Rows<Observation> observations{};
  if (!read_observations(work, network, token_id, raw_from, now, &observations)) return false;
  token_data::Rows<Candle> stored{};
  if (!read_candles(work, network, token_id, data_interval(interval), from, now, &stored)) return false;
  auto* rebuilt = static_cast<Candle*>(a.Alloc(sizeof(Candle) * (observations.count + 1)));
  if (!rebuilt) return false;
  const size_t rebuilt_count = build_candles(observations.items, observations.count, interval, rebuilt,
                                             observations.count + 1);
  // merged: stored candles, then rebuilt ones replacing equal timestamps,
  // sorted by ts (insertion order breaks no ties: timestamps are unique).
  const size_t merged_capacity = stored.count + rebuilt_count + 1;
  auto* merged = static_cast<const Candle**>(a.Alloc(sizeof(Candle*) * merged_capacity));
  if (!merged) return false;
  size_t merged_count = 0;
  for (size_t i = 0; i < stored.count; ++i) merged[merged_count++] = &stored.items[i];
  for (size_t i = 0; i < rebuilt_count; ++i) {
    bool replaced = false;
    for (size_t k = 0; k < merged_count && !replaced; ++k)
      if (merged[k]->ts == rebuilt[i].ts) { merged[k] = &rebuilt[i]; replaced = true; }
    if (!replaced) merged[merged_count++] = &rebuilt[i];
  }
  for (size_t i = 1; i < merged_count; ++i) {
    const Candle* item = merged[i];
    size_t j = i;
    while (j > 0 && merged[j - 1]->ts > item->ts) { merged[j] = merged[j - 1]; --j; }
    merged[j] = item;
  }
  Value* series = tj::Arr(a);
  if (!series) return false;
  for (size_t i = 0; i < merged_count; ++i) {
    Value* view = candle_view(a, *merged[i]);
    if (!view || !tj::Push(a, series, view)) return false;
  }
  const Freshness freshness = snapshot_freshness(latest, now);
  Value* changes = tj::Obj(a);
  if (!changes) return false;
  for (const ChangeWindow& window : kChangeWindows) {
    Value* change = tj::Null(a);
    if (!freshness.stale && latest) {
      const int64_t target = latest->ts - window.ago;
      token_data::Rows<Observation> rows{};
      if (!read_observations(work, network, token_id, target - window.tolerance, target + window.tolerance, &rows))
        return false;
      size_t usable = 0;
      for (size_t i = 0; i < rows.count; ++i)
        if (rows.items[i].ok && (rows.items[i].has_liquidity || rows.items[i].has_price))
          rows.items[usable++] = rows.items[i];
      change = change_at(a, rows.items, usable, latest, window.ago, window.tolerance);
      if (!change) return false;
      if (change->type == tj::Type::kNull && window.ago >= 7 * kDayMs) {
        token_data::Rows<Candle> daily{};
        if (!read_candles(work, network, token_id, token_data::Interval::k1d, target - window.tolerance,
                          target + window.tolerance, &daily))
          return false;
        const Candle* baseline = nullptr;
        for (size_t i = 0; i < daily.count; ++i)
          if (!baseline || llabs(daily.items[i].ts - target) < llabs(baseline->ts - target)) baseline = &daily.items[i];
        if (baseline) {
          change = tj::Obj(a);
          if (!change || !tj::Set(a, change, "baselineAt", seconds(a, baseline->ts)) ||
              !tj::Set(a, change, "liquidityPct",
                       percent_change(a, latest->has_liquidity, latest->liquidity, baseline->has[3],
                                      baseline->value[3])) ||
              !tj::Set(a, change, "pricePct",
                       percent_change(a, latest->has_price, latest->price, baseline->has[7], baseline->value[7])))
            return false;
        }
      }
    }
    if (!tj::Set(a, changes, window.key, change)) return false;
  }
  Value* availability = tj::Obj(a);
  Value* coverage = tj::Obj(a);
  Value* latest_json = tj::Null(a);
  if (!availability || !coverage || !set_availability(a, availability, facts) ||
      !tj::Set(a, availability, "stale", tj::Bool(a, freshness.stale)) ||
      !tj::Set(a, availability, "latestAgeSeconds",
               freshness.has_age ? tj::Num(a, freshness.age_seconds) : tj::Null(a)))
    return false;
  if (latest) {
    latest_json = tj::Obj(a);
    if (!latest_json || !tj::Set(a, latest_json, "ts", seconds(a, latest->ts)) ||
        !tj::Set(a, latest_json, "liquidityUsd", num_or_null(a, latest->has_liquidity, latest->liquidity)) ||
        !tj::Set(a, latest_json, "priceUsd", num_or_null(a, latest->has_price, latest->price)) ||
        !tj::Set(a, latest_json, "spreadPct", num_or_null(a, latest->has_spread, latest->spread)) ||
        !tj::Set(a, latest_json, "topPoolShare", num_or_null(a, latest->has_top_share, latest->top_share)) ||
        !tj::Set(a, latest_json, "poolCount", tj::Num(a, latest->pool_count)))
      return false;
  }
  if (!tj::Set(a, coverage, "from", seconds(a, from)) || !tj::Set(a, coverage, "to", seconds(a, now)) ||
      !tj::Set(a, coverage, "rawSamples", tj::Num(a, static_cast<double>(observations.count))) ||
      !tj::Set(a, coverage, "candleCount", tj::Num(a, static_cast<double>(merged_count))))
    return false;
  Value* doc = tj::Obj(a);
  if (!doc || !tj::Set(a, doc, "network", tj::Str(a, network)) || !tj::Set(a, doc, "tokenId", tj::Str(a, token_id)) ||
      !tj::Set(a, doc, "range", tj::Str(a, range_name(range))) ||
      !tj::Set(a, doc, "interval", tj::Str(a, interval_name(interval))) ||
      !tj::Set(a, doc, "availability", availability) || !tj::Set(a, doc, "latest", latest_json) ||
      !tj::Set(a, doc, "changes", changes) || !tj::Set(a, doc, "series", series) ||
      !tj::Set(a, doc, "coverage", coverage))
    return false;
  // liquidityHistoryValidator: {...result, availability, coverage} without the
  // clock-moving fields. A shallow copy: the series (the bulk) is shared.
  const auto shallow = [&](const Value* from, const char* const* dropped, size_t count) -> Value* {
    Value* copy = tj::Obj(a);
    for (uint32_t i = 0; copy && i < from->count; ++i) {
      const tj::Member& member = from->members[i];
      bool drop = false;
      for (size_t k = 0; k < count && !drop; ++k)
        drop = member.key_length == strlen(dropped[k]) && memcmp(member.key, dropped[k], member.key_length) == 0;
      if (!drop && !tj::Set(a, copy, member.key, member.key_length, member.value)) return nullptr;
    }
    return copy;
  };
  static const char* const kAge[] = {"latestAgeSeconds"};
  static const char* const kEdges[] = {"from", "to"};
  Value* stable_availability = shallow(availability, kAge, 1);
  Value* stable_coverage = shallow(coverage, kEdges, 2);
  Value* stable = shallow(doc, nullptr, 0);
  if (!stable_availability || !stable_coverage || !stable ||
      !tj::Set(a, stable, "availability", stable_availability) || !tj::Set(a, stable, "coverage", stable_coverage))
    return false;
  *document = doc;
  *validator = stable;
  return a.ok;
}

bool LiquiditySummaryDocument(Work& work, const char* network, const char* token_id, int64_t now,
                              Value** document) {
  Arena& a = work.arena;
  *document = nullptr;
  const int64_t from = now - 27 * kHourMs;
  Availability facts{};
  if (!availability_facts(work, network, token_id, &facts)) return false;
  token_data::Rows<Observation> rows{};
  if (!read_observations(work, network, token_id, from, now, &rows)) return false;
  size_t successful = 0;
  for (size_t i = 0; i < rows.count; ++i)
    if (rows.items[i].ok && rows.items[i].has_liquidity) rows.items[successful++] = rows.items[i];
  const Observation* latest = successful ? &rows.items[successful - 1] : nullptr;
  const Freshness freshness = snapshot_freshness(latest, now);
  Value* change = freshness.stale ? tj::Null(a) : change_at(a, rows.items, successful, latest, kDayMs, 2 * kHourMs);
  Value* availability = tj::Obj(a);
  Value* doc = tj::Obj(a);
  if (!change || !availability || !doc || !set_availability(a, availability, facts) ||
      !tj::Set(a, availability, "latestAgeSeconds",
               freshness.has_age ? tj::Num(a, freshness.age_seconds) : tj::Null(a)) ||
      !tj::Set(a, availability, "stale", tj::Bool(a, freshness.stale)) ||
      !tj::Set(a, doc, "network", tj::Str(a, network)) || !tj::Set(a, doc, "tokenId", tj::Str(a, token_id)) ||
      !tj::Set(a, doc, "availability", availability) ||
      !tj::Set(a, doc, "latestAt", latest ? seconds(a, latest->ts) : tj::Null(a)) ||
      !tj::Set(a, doc, "change24h", change))
    return false;
  *document = doc;
  return a.ok;
}

// ============================================================================
// rollupLiquidityCandles
// ============================================================================

size_t RollupLiquidityCandles(Work& work, int64_t now) {
  Arena& a = work.arena;
  const int64_t start_1h = bucket(now - 2 * kDayMs, Interval::k1h);
  const int64_t start_1d = bucket(now - 2 * kDayMs, Interval::k1d);
  const int64_t start_1w = bucket(now - 7 * kDayMs, Interval::k1w);
  const int64_t since = start_1h < start_1d ? start_1h : start_1d;
  token_data::Rows<token_data::TokenDataStore::Pair> pairs{};
  constexpr size_t kMaxPairs = 4096;
  pairs.items = static_cast<token_data::TokenDataStore::Pair*>(
      a.Alloc(sizeof(token_data::TokenDataStore::Pair) * kMaxPairs));
  pairs.capacity = pairs.items ? kMaxPairs : 0;
  if (!pairs.items || work.env->data->RecentPairs(since, work.Now(), &pairs) != token_data::DataOutcome::kOk) return 0;
  size_t written = 0;
  for (size_t p = 0; p < pairs.count; ++p) {
    const size_t mark = a.used;
    const char* network = pairs.items[p].network;
    const char* token_id = pairs.items[p].token_id;
    token_data::Rows<Observation> rows{};
    if (!read_observations(work, network, token_id, since, now, &rows)) { a.used = mark; continue; }
    Candle* new_daily = nullptr;
    size_t new_daily_count = 0;
    static constexpr Interval kRawIntervals[] = {Interval::k1h, Interval::k1d};
    for (Interval interval : kRawIntervals) {
      const int64_t start = interval == Interval::k1h ? start_1h : start_1d;
      size_t first = 0;
      while (first < rows.count && rows.items[first].ts < start) ++first;
      if (first == rows.count) continue;
      const size_t window = rows.count - first;
      auto* candles = static_cast<Candle*>(a.Alloc(sizeof(Candle) * (window + 1)));
      if (!candles) break;
      const size_t built = build_candles(rows.items + first, window, interval, candles, window + 1);
      work.env->data->PutCandles(network, token_id, data_interval(interval), candles, built, work.Now(), nullptr);
      if (interval == Interval::k1d) { new_daily = candles; new_daily_count = built; }
      written += built;
    }
    // Weekly from durable daily candles plus the fresh ones.
    token_data::Rows<Candle> saved{};
    if (read_candles(work, network, token_id, token_data::Interval::k1d, start_1w, now, &saved)) {
      const size_t capacity = saved.count + new_daily_count + 1;
      auto* daily = static_cast<Candle*>(a.Alloc(sizeof(Candle) * capacity));
      auto* weekly = static_cast<Candle*>(a.Alloc(sizeof(Candle) * capacity));
      if (daily && weekly) {
        size_t count = 0;
        for (size_t i = 0; i < saved.count; ++i) daily[count++] = saved.items[i];
        for (size_t i = 0; i < new_daily_count; ++i) {
          bool replaced = false;
          for (size_t k = 0; k < count && !replaced; ++k)
            if (daily[k].ts == new_daily[i].ts) { daily[k] = new_daily[i]; replaced = true; }
          if (!replaced) daily[count++] = new_daily[i];
        }
        for (size_t i = 1; i < count; ++i) {
          const Candle item = daily[i];
          size_t j = i;
          while (j > 0 && daily[j - 1].ts > item.ts) { daily[j] = daily[j - 1]; --j; }
          daily[j] = item;
        }
        size_t weeks = 0;
        size_t i = 0;
        while (i < count) {
          const int64_t week = bucket(daily[i].ts, Interval::k1w);
          size_t end = i;
          while (end < count && bucket(daily[end].ts, Interval::k1w) == week) ++end;
          Candle w{};
          w.ts = week;
          const Candle& first = daily[i];
          const Candle& last = daily[end - 1];
          auto extreme = [&](size_t field, bool max) {
            bool any = false; double best = 0;
            for (size_t k = i; k < end; ++k) {
              if (!daily[k].has[field]) continue;
              const double v = daily[k].value[field];
              if (!any || (max ? v > best : v < best)) best = v;
              any = true;
            }
            w.has[field] = any; w.value[field] = best;
          };
          auto copy = [&](size_t field, const Candle& from) { w.has[field] = from.has[field]; w.value[field] = from.value[field]; };
          copy(0, first); extreme(1, true); extreme(2, false); copy(3, last);
          copy(4, first); extreme(5, true); extreme(6, false); copy(7, last);
          copy(8, last); extreme(9, true); copy(10, last); extreme(11, true);
          copy(12, first); copy(13, last); copy(14, last); copy(15, last); copy(16, last);
          for (size_t k = i; k < end; ++k) { w.sample_count += daily[k].sample_count; w.expected_samples += daily[k].expected_samples; }
          snprintf(w.source, sizeof(w.source), "%s", last.source);
          weekly[weeks++] = w;
          i = end;
        }
        work.env->data->PutCandles(network, token_id, token_data::Interval::k1w, weekly, weeks, work.Now(), nullptr);
        written += weeks;
      }
    }
    a.used = mark;
    a.ok = true;
  }
  return written;
}

}  // namespace fortis::tokens::token_history
