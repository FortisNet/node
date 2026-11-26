#include "ownership.h"
#include <string_view>
#include "token_jobs.h"

#include "provider_runtime_config.h"
#include "price_id_parser.h"
#include "threading.h"
#include "token_comparison_store.h"
#include "token_data.h"
#include "token_fetchers.h"
#include "token_history.h"
#include "token_liquidity.h"
#include "token_provider.h"
#include "token_swr.h"
#include "token_tron.h"
#include "token_views.h"
#include "token_writes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_jobs {
namespace {

using tj::Arena;
using tj::Value;
using token_facts::Work;

constexpr int64_t kHourMs = 3600000;
constexpr int64_t kDayMs = 24 * kHourMs;

int64_t hour_bucket(int64_t ms) {
  int64_t h = ms / kHourMs;
  if (ms % kHourMs < 0) --h;
  return h * kHourMs;
}

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

std::unique_ptr<char[], base::StorageDeleter> copy_string(const char* text) {
  if (!text) return nullptr;
  const size_t n = strlen(text);
  auto out = base::AllocateStorage<char[]>(n + 1);
  if (out) memcpy(out.get(), text, n + 1);
  return out;
}

}  // namespace

const char* FetchFacetName(FetchFacet facet) {
  switch (facet) {
    case FetchFacet::kSpot: return "spot";
    case FetchFacet::kHistory: return "history";
    case FetchFacet::kMarket: return "market";
    case FetchFacet::kProfile: return "profile";
    case FetchFacet::kLiquidity: return "liquidity";
    case FetchFacet::kHolders: return "holders";
  }
  return "market";
}

// ============================================================================
// TokenFetchStats
// ============================================================================

struct TokenStats::Bucket {
  int64_t bucket = 0;
  std::unique_ptr<char[], base::StorageDeleter> network;
  std::unique_ptr<char[], base::StorageDeleter> token_id;
  FetchFacet facet = FetchFacet::kMarket;
  std::unique_ptr<char[], base::StorageDeleter> source_id;
  double requests = 0, fresh = 0, stale = 0, floor = 0, successes = 0, failures = 0, negative = 0, latency = 0;
  bool has_last = false;
  int64_t last = 0;
};

namespace {
void release(TokenStats::Bucket& b) {
  b = TokenStats::Bucket{};
}
void merge_into(TokenStats::Bucket& target, const TokenStats::Bucket& older) {
  // mergeBuckets(older, newer=target).
  target.requests += older.requests; target.fresh += older.fresh; target.stale += older.stale;
  target.floor += older.floor; target.successes += older.successes; target.failures += older.failures;
  target.negative += older.negative; target.latency += older.latency;
  if (!target.source_id && older.source_id.get()) target.source_id = copy_string(older.source_id.get());
  if (older.has_last && (!target.has_last || older.last > target.last)) { target.has_last = true; target.last = older.last; }
}
}  // namespace

TokenStats::TokenStats() : mutex_(), buckets_(nullptr), count_(0), dropped_(0) {

  try { buckets_ = std::make_unique<Bucket[]>(kMaxPending); }
  catch (const std::bad_alloc&) { buckets_.reset(); }
}

TokenStats::~TokenStats() {
  if (buckets_) for (size_t i = 0; i < count_; ++i) release(buckets_[i]);
  buckets_.reset();

}

void TokenStats::Record(const FetchSample& sample) {
  if (!buckets_ || !sample.network || !sample.token_id) return;
  const int64_t bucket = hour_bucket(sample.at_ms);
  fortis::base::LockGuard lock(mutex_);
  Bucket* row = nullptr;
  for (size_t i = 0; i < count_ && !row; ++i) {
    Bucket& b = buckets_[i];
    if (b.bucket == bucket && b.facet == sample.facet && strcmp(b.network.get(), sample.network) == 0 &&
        strcmp(b.token_id.get(), sample.token_id) == 0)
      row = &b;
  }
  if (!row) {
    if (count_ >= kMaxPending) { ++dropped_; return; }
    Bucket fresh{};
    fresh.bucket = bucket;
    fresh.network = copy_string(sample.network);
    fresh.token_id = copy_string(sample.token_id);
    fresh.facet = sample.facet;
    fresh.source_id = copy_string(sample.source_id);
    if (!fresh.network || !fresh.token_id) { release(fresh); ++dropped_; return; }
    buckets_[count_] = std::move(fresh);
    row = &buckets_[count_++];
  }
  if (sample.foreground) {
    row->requests += 1;
    if (!row->has_last || sample.at_ms > row->last) { row->has_last = true; row->last = sample.at_ms; }
  }
  switch (sample.outcome) {
    case FetchOutcome::kFresh: row->fresh += 1; break;
    case FetchOutcome::kStale: row->stale += 1; break;
    case FetchOutcome::kFloor: row->floor += 1; break;
    case FetchOutcome::kLive: row->successes += 1; break;
    case FetchOutcome::kFailure: row->failures += 1; break;
  }
  if (sample.negative) row->negative += 1;
  if (isfinite(sample.latency_ms)) row->latency += sample.latency_ms > 0 ? floor(sample.latency_ms + 0.5) : 0;
  if (!row->source_id && sample.source_id) row->source_id = copy_string(sample.source_id);
}

size_t TokenStats::Flush(token_data::TokenDataStore* store, int64_t now_ms, bool* failed) {
  if (failed) *failed = false;
  if (!buckets_ || !store) return 0;
  fortis::base::UniqueLock lock(mutex_);
  const size_t count = count_;
  if (!count) return 0;
  std::unique_ptr<Bucket[]> detached;
  try { detached = std::make_unique<Bucket[]>(count); } catch (const std::bad_alloc&) { detached.reset(); }
  if (!detached) { lock.Unlock(); if (failed) *failed = true; return 0; }
  for (size_t i = 0; i < count; ++i) detached[i] = std::move(buckets_[i]);
  for (size_t i = 0; i < count; ++i) buckets_[i] = Bucket{};
  count_ = 0;
  lock.Unlock();

  // Each row's document (15 members: about 1.7 KiB of nodes, keys and
  // member arrays) plus its own strings. A flat 1.5 KiB per row failed every
  // flush of more than about a dozen buckets, which then merged back forever.
  size_t arena_bytes = 4096;
  for (size_t i = 0; i < count; ++i) {
    const Bucket& b = detached[i];
    arena_bytes += 3072 + (b.network ? strlen(b.network.get()) : 0) + (b.token_id ? strlen(b.token_id.get()) : 0) +
                   (b.source_id ? strlen(b.source_id.get()) : 0);
  }
  auto memory_storage = base::AllocateStorage<uint8_t[]>(arena_bytes);
  auto* memory = memory_storage.get();
  auto rows_storage = base::AllocateStorage<Value*[]>(sizeof(Value*) * count);
  auto* rows = rows_storage.get();
  bool ok = memory && rows;
  if (ok) {
    Arena a{memory, arena_bytes, 0, true};
    for (size_t i = 0; i < count && ok; ++i) {
      const Bucket& b = detached[i];
      Value* o = tj::Obj(a);
      ok = o && tj::Set(a, o, "bucket", tj::Num(a, static_cast<double>(b.bucket))) &&
           tj::Set(a, o, "network", tj::Str(a, b.network.get())) && tj::Set(a, o, "tokenId", tj::Str(a, b.token_id.get())) &&
           tj::Set(a, o, "facet", tj::Str(a, FetchFacetName(b.facet))) &&
           tj::Set(a, o, "sourceId", b.source_id ? tj::Str(a, b.source_id.get()) : tj::Null(a)) &&
           tj::Set(a, o, "requests", tj::Num(a, b.requests)) && tj::Set(a, o, "freshHits", tj::Num(a, b.fresh)) &&
           tj::Set(a, o, "staleHits", tj::Num(a, b.stale)) && tj::Set(a, o, "floorHits", tj::Num(a, b.floor)) &&
           tj::Set(a, o, "upstreamSuccesses", tj::Num(a, b.successes)) &&
           tj::Set(a, o, "upstreamFailures", tj::Num(a, b.failures)) &&
           tj::Set(a, o, "negativeResults", tj::Num(a, b.negative)) && tj::Set(a, o, "latencyMs", tj::Num(a, b.latency)) &&
           tj::Set(a, o, "lastRequestedAt", b.has_last ? tj::Num(a, static_cast<double>(b.last)) : tj::Null(a));
      rows[i] = o;
    }
    size_t dropped = 0;
    ok = ok && store->UpsertFetchRows(rows, count, now_ms, &dropped);
  }
  rows_storage.reset();
  memory_storage.reset();
  if (!ok) {
    // A failed flush merges the detached buckets back.
    lock.Lock();
    for (size_t i = 0; i < count; ++i) {
      Bucket& older = detached[i];
      Bucket* active = nullptr;
      for (size_t k = 0; k < count_ && !active; ++k) {
        Bucket& b = buckets_[k];
        if (b.bucket == older.bucket && b.facet == older.facet && strcmp(b.network.get(), older.network.get()) == 0 &&
            strcmp(b.token_id.get(), older.token_id.get()) == 0)
          active = &b;
      }
      if (active) { merge_into(*active, older); release(older); }
      else if (count_ < kMaxPending) { buckets_[count_++] = std::move(older); }
      else { release(older); ++dropped_; }
    }
    lock.Unlock();
    detached.reset();
    if (failed) *failed = true;
    return 0;
  }
  for (size_t i = 0; i < count; ++i) release(detached[i]);
  detached.reset();
  return count;
}

size_t TokenStats::pending() const {
  fortis::base::LockGuard lock(mutex_);
  const size_t n = count_;
  return n;
}

uint64_t TokenStats::dropped() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t n = dropped_;
  return n;
}

// ============================================================================
// TokenDemandService.refresh (fetch-request signals) + demand assessment
// ============================================================================
namespace {

constexpr size_t kMaxDemand = 4096;
constexpr double kPageTier0 = 100, kPageTier1 = 300, kPriceTier0 = 100, kPriceTier1 = 300, kHysteresis = 0.1;

struct DemandEntry {
  char network[128];
  char token_id[networks::kTokenRefMaxIdBytes + 1];
  // hourly accumulation
  int64_t hour_bucket;
  double hour_requests;
  char hour_source[providers::kPriceIdMaxSourceIdBytes + 1];
  bool hour_has_last;
  int64_t hour_last;
  bool in_hour;
  // requested
  bool requested;
  double requests24h, requests7d;
  char source[providers::kPriceIdMaxSourceIdBytes + 1];
  bool has_last;
  int64_t last;
  // projection
  char coingecko_id[providers::kPriceIdMaxSourceIdBytes + 1];
  int previous_page, previous_price;  // -1: none
  double page_score, price_score;
  bool page_observed, page_corroborated, price_observed;
  int page_tier, price_tier;
};

struct DemandState {
  DemandEntry* entries;
  size_t count;
  int64_t current_hour;
  int64_t now;
  bool overflow;
};

DemandEntry* find_entry(DemandState& s, const char* network, const char* token_id, bool create) {
  for (size_t i = 0; i < s.count; ++i)
    if (strcmp(s.entries[i].network, network) == 0 && strcmp(s.entries[i].token_id, token_id) == 0)
      return &s.entries[i];
  if (!create) return nullptr;
  if (s.count >= kMaxDemand || strlen(network) >= sizeof(DemandEntry::network) ||
      strlen(token_id) >= sizeof(DemandEntry::token_id)) { s.overflow = true; return nullptr; }
  DemandEntry& e = s.entries[s.count++];
  e = DemandEntry{};
  snprintf(e.network, sizeof(e.network), "%s", network);
  snprintf(e.token_id, sizeof(e.token_id), "%s", token_id);
  e.previous_page = e.previous_price = -1;
  return &e;
}

// [a, b].filter(truthy).sort().at(-1): the larger string by UTF-16 order
// (equal to byte order for the ASCII ids recorded here).
void max_source(char* target, size_t capacity, const char* candidate) {
  if (!candidate || !*candidate) return;
  if (!*target || strcmp(candidate, target) > 0) snprintf(target, capacity, "%s", candidate);
}

void flush_hour(DemandState& s) {
  for (size_t i = 0; i < s.count; ++i) {
    DemandEntry& e = s.entries[i];
    if (!e.in_hour) continue;
    e.in_hour = false;
    e.requested = true;
    if (e.hour_bucket >= s.now - 24 * kHourMs) e.requests24h += e.hour_requests;
    e.requests7d += e.hour_requests;
    max_source(e.source, sizeof(e.source), e.hour_source);
    if (e.hour_has_last && (!e.has_last || e.hour_last > e.last)) { e.has_last = true; e.last = e.hour_last; }
    e.hour_requests = 0;
    e.hour_source[0] = '\0';
    e.hour_has_last = false;
  }
}

bool fetch_row_visit(void* context, const Value* row) {
  auto* s = static_cast<DemandState*>(context);
  const Value* bucket = tj::Get(row, "bucket");
  const Value* network = tj::Get(row, "network");
  const Value* token = tj::Get(row, "tokenId");
  if (!tj::IsNumber(bucket) || !tj::IsString(network) || !tj::IsString(token)) return true;
  const int64_t at = static_cast<int64_t>(bucket->number);
  if (s->current_hour != -1 && at != s->current_hour) flush_hour(*s);
  s->current_hour = at;
  char net[128], id[networks::kTokenRefMaxIdBytes + 1];
  if (network->length >= sizeof(net) || token->length >= sizeof(id)) return true;
  memcpy(net, network->text, network->length); net[network->length] = '\0';
  memcpy(id, token->text, token->length); id[token->length] = '\0';
  DemandEntry* e = find_entry(*s, net, id, true);
  if (!e) return true;
  const Value* requests = tj::Get(row, "requests");
  const double count = tj::IsNumber(requests) ? requests->number : 0;
  const double prior = e->in_hour ? e->hour_requests : 0;
  e->in_hour = true;
  e->hour_bucket = at;
  e->hour_requests = count > prior ? count : prior;
  const Value* source = tj::Get(row, "sourceId");
  if (tj::IsString(source) && source->length && source->length < sizeof(e->hour_source)) {
    char text[providers::kPriceIdMaxSourceIdBytes + 1];
    memcpy(text, source->text, source->length); text[source->length] = '\0';
    max_source(e->hour_source, sizeof(e->hour_source), text);
  }
  const Value* last = tj::Get(row, "lastRequestedAt");
  if (tj::IsNumber(last) && (!e->hour_has_last || last->number > static_cast<double>(e->hour_last))) {
    e->hour_has_last = true;
    e->hour_last = static_cast<int64_t>(last->number);
  }
  return true;
}

double round6(double v) { return floor(v * 1000000 + 0.5) / 1000000; }

// Demand keys in byte order: network, then token id.
int key_compare(const DemandEntry& a, const DemandEntry& b) {
  const int n = strcmp(a.network, b.network);
  return n != 0 ? n : strcmp(a.token_id, b.token_id);
}

void allocate_plane(DemandEntry* entries, size_t count, bool page, double fast_cap, double slow_cap,
                    DemandEntry** sorted, int* assigned) {
  size_t n = 0;
  for (size_t i = 0; i < count; ++i)
    if (page ? entries[i].page_observed : entries[i].price_observed) sorted[n++] = &entries[i];
  auto score = [&](const DemandEntry* e) { return page ? e->page_score : e->price_score; };
  for (size_t i = 1; i < n; ++i) {
    DemandEntry* item = sorted[i];
    size_t j = i;
    while (j > 0 && (score(sorted[j - 1]) < score(item) ||
                     (score(sorted[j - 1]) == score(item) && key_compare(*sorted[j - 1], *item) > 0))) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = item;
  }
  for (size_t i = 0; i < count; ++i) assigned[i] = -1;
  size_t assigned_count = 0;
  auto index_of = [&](const DemandEntry* e) { return static_cast<size_t>(e - entries); };
  auto fill = [&](int tier, double cap, double boundary_before, bool (*eligible)(const DemandEntry*, bool),
                  bool (*incumbent)(const DemandEntry*, bool)) {
    if (cap <= 0) return;
    const double target = static_cast<double>(assigned_count) + cap;
    const double retention = ceil((boundary_before + cap) * (1 + kHysteresis));
    size_t retained = 0;
    for (size_t r = 0; r < n && static_cast<double>(retained) < cap; ++r) {
      DemandEntry* e = sorted[r];
      if (assigned[index_of(e)] >= 0 || !eligible(e, page) || !incumbent(e, page) ||
          static_cast<double>(r) >= retention)
        continue;
      assigned[index_of(e)] = tier;
      ++assigned_count;
      ++retained;
    }
    for (size_t r = 0; r < n; ++r) {
      if (static_cast<double>(assigned_count) >= target) break;
      DemandEntry* e = sorted[r];
      if (assigned[index_of(e)] < 0 && eligible(e, page)) { assigned[index_of(e)] = tier; ++assigned_count; }
    }
  };
  fill(0, fast_cap, 0,
       [](const DemandEntry* e, bool p) { return p ? e->page_corroborated : true; },
       [](const DemandEntry* e, bool p) { return (p ? e->previous_page : e->previous_price) == 0; });
  fill(1, slow_cap, fast_cap,
       [](const DemandEntry*, bool) { return true; },
       [](const DemandEntry* e, bool p) {
         const int previous = p ? e->previous_page : e->previous_price;
         return (previous < 0 ? 3 : previous) <= 1;
       });
}

// RegistryTokenSourceResolver.sourceId(network, tokenId, coingeckoId).
bool source_id(const char* network, const char* token_id, const char* coingecko_id, char* out, size_t capacity) {
  providers::ParsedPriceId parsed{};
  if (coingecko_id && *coingecko_id && ParsePriceId(coingecko_id, strlen(coingecko_id), &parsed)) {
    snprintf(out, capacity, "%s", coingecko_id);
    return true;
  }
  if (strcmp(token_id, "native") == 0) {
    networks::TokenNetwork resolved;
    if (!ResolveTokenNetwork(network, strlen(network), &resolved) || !resolved.native_coingecko_id) return false;
    snprintf(out, capacity, "%s", resolved.native_coingecko_id);
    return true;
  }
  char candidate[networks::kTokenRefMaxIdBytes + 160];
  const char* raw = token_id;
  if (strncmp(token_id, "erc20:", 6) == 0 || strncmp(token_id, "trc20:", 6) == 0 ||
      strncmp(token_id, "trc10:", 6) == 0) {
    snprintf(candidate, sizeof(candidate), "%s:%s", network, token_id);
    raw = candidate;
  }
  if (strlen(raw) > providers::kPriceIdMaxRawBytes || !ParsePriceId(raw, strlen(raw), &parsed)) return false;
  snprintf(out, capacity, "%s", parsed.source_id);
  return true;
}

}  // namespace

size_t RefreshDemand(Work& work, int64_t now) {
  Arena& a = work.arena;
  token_data::TokenDataStore* data = work.env->data;
  DemandState s{};
  auto entries_storage = base::AllocateStorage<DemandEntry[]>(sizeof(DemandEntry) * kMaxDemand);
  s.entries = entries_storage.get();
  if (!s.entries) return 0;
  s.current_hour = -1;
  s.now = now;
  const size_t mark = a.used;
  data->ForEachFetchRow(now - 7 * kDayMs, now, work.Now(), a, fetch_row_visit, &s);
  flush_hour(s);
  // Registry coin ids and the prior demand tiers of every requested token.
  TokenComparisonStore store(work.env->kv);
  for (size_t i = 0; i < s.count; ++i) {
    DemandEntry& e = s.entries[i];
    token_store::IdentityRecord identity{};
    if (store.ReadIdentity(e.network, e.token_id, work.Now(), &identity) == token_store::ReadStatus::kOk &&
        identity.found && identity.coingecko_id.present && identity.coingecko_id.length < sizeof(e.coingecko_id)) {
      memcpy(e.coingecko_id, identity.coingecko_id.text, identity.coingecko_id.length);
      e.coingecko_id[identity.coingecko_id.length] = '\0';
    }
    Value* prior = nullptr;
    if (data->ReadDemand(e.network, e.token_id, work.Now(), a, &prior) == token_data::DataOutcome::kOk &&
        tj::IsObject(prior)) {
      const Value* page = tj::Get(prior, "pageTier");
      const Value* price = tj::Get(prior, "priceTier");
      e.previous_page = tj::IsNumber(page) ? static_cast<int>(page->number) : 3;
      e.previous_price = tj::IsNumber(price) ? static_cast<int>(price->number) : 3;
      if (e.previous_page < 0 || e.previous_page > 3) e.previous_page = -1;
      if (e.previous_price < 0 || e.previous_price > 3) e.previous_price = -1;
    }
    // assessTokenDemand over the native signal set.
    const double r24 = e.requests24h < 50 ? e.requests24h : 50;
    const double r7 = e.requests7d < 250 ? e.requests7d : 250;
    e.page_score = round6(4 * log1p(floor(r24)) + 1.5 * log1p(floor(r7)));
    e.price_score = 0;
    e.page_observed = e.requests7d > 0;
    e.page_corroborated = false;
    e.price_observed = false;
  }
  auto sorted_storage = base::AllocateStorage<DemandEntry*[]>(sizeof(DemandEntry*) * (s.count + 1));
  auto* sorted = sorted_storage.get();
  auto page_storage = base::AllocateStorage<int[]>(sizeof(int) * (s.count + 1));
  auto* page = page_storage.get();
  auto price_storage = base::AllocateStorage<int[]>(sizeof(int) * (s.count + 1));
  auto* price = price_storage.get();
  size_t written = 0;
  if (sorted && page && price) {
    allocate_plane(s.entries, s.count, true, kPageTier0, kPageTier1, sorted, page);
    allocate_plane(s.entries, s.count, false, kPriceTier0, kPriceTier1, sorted, price);
    Value** rows = static_cast<Value**>(a.Alloc(sizeof(Value*) * (s.count + 1)));
    bool ok = rows != nullptr;
    for (size_t i = 0; ok && i < s.count; ++i) {
      DemandEntry& e = s.entries[i];
      e.page_tier = page[i] >= 0 ? page[i] : (e.page_observed ? 2 : 3);
      e.price_tier = price[i] >= 0 ? price[i] : (e.price_observed ? 2 : 3);
      char source[providers::kPriceIdMaxSourceIdBytes + 1] = {};
      bool has_source = e.source[0] != '\0';
      if (has_source) snprintf(source, sizeof(source), "%s", e.source);
      else has_source = source_id(e.network, e.token_id, e.coingecko_id[0] ? e.coingecko_id : nullptr, source,
                                  sizeof(source));
      uint8_t buffer[64];
      tj::OutBuffer page_score{buffer, sizeof(buffer), 0, true};
      codec::history_batch::AppendJsNumber(page_score, e.page_score);
      Value* o = tj::Obj(a);
      ok = o && tj::Set(a, o, "network", tj::Str(a, e.network)) && tj::Set(a, o, "tokenId", tj::Str(a, e.token_id)) &&
           tj::Set(a, o, "sourceId", has_source ? tj::Str(a, source) : tj::Null(a)) &&
           tj::Set(a, o, "activeHolderAccounts", tj::Num(a, 0)) && tj::Set(a, o, "unpricedHolderAccounts", tj::Num(a, 0)) &&
           tj::Set(a, o, "liveWatches", tj::Num(a, 0)) && tj::Set(a, o, "recentTransfers", tj::Num(a, 0)) &&
           tj::Set(a, o, "unpricedOperations", tj::Num(a, 0)) && tj::Set(a, o, "heldFiatUsd", tj::Null(a)) &&
           tj::Set(a, o, "foregroundRequests24h", tj::Num(a, floor(e.requests24h))) &&
           tj::Set(a, o, "foregroundRequests7d", tj::Num(a, floor(e.requests7d))) &&
           tj::Set(a, o, "lastRequestedAt", e.has_last ? tj::Num(a, static_cast<double>(e.last)) : tj::Null(a)) &&
           tj::Set(a, o, "pageScore", tj::Str(a, reinterpret_cast<const char*>(buffer), page_score.used)) &&
           tj::Set(a, o, "priceScore", tj::Str(a, "0")) && tj::Set(a, o, "pageTier", tj::Num(a, e.page_tier)) &&
           tj::Set(a, o, "priceTier", tj::Num(a, e.price_tier)) &&
           tj::Set(a, o, "computedAt", tj::Num(a, static_cast<double>(now)));
      rows[i] = o;
    }
    if (ok && data->ReplaceDemand(rows, s.count, now, work.Now())) written = s.count;
  }
  sorted_storage.reset();
  page_storage.reset();
  price_storage.reset();
  entries_storage.reset();
  a.used = mark;
  a.ok = true;
  return written;
}

// ============================================================================
// Warmers
// ============================================================================
namespace {

struct Candidate {
  char network[128];
  char token_id[networks::kTokenRefMaxIdBytes + 1];
  char source_id[providers::kPriceIdMaxSourceIdBytes + 1];
  bool has_source;
  int tier;
  double score;
  bool has_as_of;
  int64_t as_of;
};

constexpr size_t kMaxCandidates = 2048;

double number_or(const Value* v, double fallback) {
  if (tj::IsNumber(v)) return v->number;
  if (tj::IsString(v)) {
    char text[64];
    const size_t n = v->length < sizeof(text) - 1 ? v->length : sizeof(text) - 1;
    memcpy(text, v->text, n);
    text[n] = '\0';
    char* end = nullptr;
    const double parsed = strtod(text, &end);
    return end != text ? parsed : fallback;
  }
  return fallback;
}

struct CandidateScan {
  Candidate* items;
  size_t count;
  bool* has_last;
  int64_t* last;
};

// Copies one demand row (the KV scan holds the store lock, so the visitor
// must not read the store; facts are read after the scan).
bool candidate_visit(void* context, const Value* row) {
  auto* scan = static_cast<CandidateScan*>(context);
  if (scan->count >= kMaxCandidates) return false;
  const Value* network = tj::Get(row, "network");
  const Value* token = tj::Get(row, "tokenId");
  const Value* source = tj::Get(row, "sourceId");
  const Value* tier_value = tj::Get(row, "pageTier");
  if (!tj::IsString(network) || !tj::IsString(token) || network->length >= 128 ||
      token->length > networks::kTokenRefMaxIdBytes) return true;
  Candidate& c = scan->items[scan->count];
  c = Candidate{};
  memcpy(c.network, network->text, network->length);
  memcpy(c.token_id, token->text, token->length);
  c.has_source = tj::IsString(source) && source->length && source->length <= providers::kPriceIdMaxSourceIdBytes;
  if (c.has_source) memcpy(c.source_id, source->text, source->length);
  c.tier = tj::IsNumber(tier_value) ? static_cast<int>(tier_value->number) : 3;
  c.score = number_or(tj::Get(row, "pageScore"), NAN);
  const Value* last = tj::Get(row, "lastRequestedAt");
  scan->has_last[scan->count] = tj::IsNumber(last);
  scan->last[scan->count] = tj::IsNumber(last) ? static_cast<int64_t>(last->number) : 0;
  ++scan->count;
  return true;
}

// The per-warmer candidate filter over the stored facts.
bool keep_candidate(Work& work, int mode, int64_t now, Candidate& c, bool has_last, int64_t last, Arena& a) {
  const size_t mark = a.used;
  Value* facts = nullptr;
  work.env->data->ReadFacts(c.network, c.token_id, work.Now(), a, &facts);
  bool keep = true;
  switch (mode) {
    case 0:
    case 1: {
      if (!c.has_source) { keep = false; break; }
      if (mode == 1) {
        if (c.tier != 0 || !has_last || last < now - 10 * 60000) { keep = false; break; }
      } else if (c.tier > 1) { keep = false; break; }
      const Value* as_of = tj::Get(facts, "marketAsOf");
      c.has_as_of = tj::IsNumber(as_of);
      c.as_of = c.has_as_of ? static_cast<int64_t>(as_of->number) : 0;
      const int64_t stale = mode == 1 ? 60000 : 15 * 60000;
      const int64_t max_age = mode == 0 && c.tier == 1 ? stale * 4 : stale;
      if (c.has_as_of && c.as_of >= now - max_age) keep = false;
      break;
    }
    case 2: {
      if (c.tier != 0 && c.tier != 1) { keep = false; break; }
      if (tj::StringEquals(tj::Get(facts, "liquidityStatus"), "unsupported")) { keep = false; break; }
      const Value* checked = tj::Get(facts, "liquidityCheckedAt");
      c.has_as_of = tj::IsNumber(checked);
      c.as_of = c.has_as_of ? static_cast<int64_t>(checked->number) : 0;
      const int64_t due = c.tier == 0 ? 5 * 60000 : 15 * 60000;
      if (c.has_as_of && c.as_of > now - due) keep = false;
      break;
    }
    case 3: {
      if (c.tier > 1) { keep = false; break; }
      const Value* as_of = tj::Get(facts, "holdersAsOf");
      c.has_as_of = tj::IsNumber(as_of);
      c.as_of = c.has_as_of ? static_cast<int64_t>(as_of->number) : 0;
      if (c.has_as_of && c.as_of >= now - 20 * kHourMs) keep = false;
      break;
    }
  }
  a.used = mark;
  a.ok = true;
  return keep;
}

// tier asc, as-of asc (null = 0), score desc (Number(pageScore); NaN keeps order).
void sort_candidates(Candidate* items, size_t count) {
  auto before = [](const Candidate& x, const Candidate& y) {
    if (x.tier != y.tier) return x.tier < y.tier;
    const int64_t xa = x.has_as_of ? x.as_of : 0, ya = y.has_as_of ? y.as_of : 0;
    if (xa != ya) return xa < ya;
    const double d = y.score - x.score;
    return d < 0;  // a NaN difference compares as equal
  };
  for (size_t i = 1; i < count; ++i) {
    const Candidate item = items[i];
    size_t j = i;
    while (j > 0 && before(item, items[j - 1])) { items[j] = items[j - 1]; --j; }
    items[j] = item;
  }
}

size_t collect(Work& work, int mode, int64_t now, Candidate* items) {
  const size_t bytes = 2 * 1024 * 1024;
  auto memory_storage = base::AllocateStorage<uint8_t[]>(bytes);
  auto* memory = memory_storage.get();
  auto has_last_storage = base::AllocateStorage<bool[]>(sizeof(bool) * kMaxCandidates);
  auto* has_last = has_last_storage.get();
  auto last_storage = base::AllocateStorage<int64_t[]>(sizeof(int64_t) * kMaxCandidates);
  auto* last = last_storage.get();
  size_t kept = 0;
  if (memory && has_last && last) {
    Arena rows_arena{memory, bytes / 2, 0, true};
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    Arena facts_arena{memory + bytes / 2, bytes / 2, 0, true};
    CandidateScan scan{items, 0, has_last, last};
    work.env->data->ForEachDemand(work.Now(), rows_arena, candidate_visit, &scan);
    for (size_t i = 0; i < scan.count; ++i)
      if (keep_candidate(work, mode, now, items[i], has_last[i], last[i], facts_arena)) items[kept++] = items[i];
  }
  memory_storage.reset();
  has_last_storage.reset();
  last_storage.reset();
  sort_candidates(items, kept);
  return kept;
}

JobResult warm_markets(Work& work, int64_t now, bool active) {
  JobResult result{};
  Arena& a = work.arena;
  auto items_storage = base::AllocateStorage<Candidate[]>(sizeof(Candidate) * kMaxCandidates);
  auto* items = items_storage.get();
  if (!items) return result;
  size_t count = collect(work, active ? 1 : 0, now, items);
  const size_t cap = active ? 50 : 175;
  if (count > cap) count = cap;
  // eligible: CoinGecko ids only.
  size_t eligible = 0;
  for (size_t i = 0; i < count; ++i) {
    providers::ParsedPriceId parsed{};
    if (items[i].has_source && ParsePriceId(items[i].source_id, strlen(items[i].source_id), &parsed) &&
        parsed.kind == providers::PriceIdKind::kCoinGecko)
      items[eligible++] = items[i];
  }
  // Unique ids, in first-seen order.
  const char* ids[kMaxCandidates];
  size_t id_count = 0;
  for (size_t i = 0; i < eligible; ++i) {
    bool seen = false;
    for (size_t k = 0; k < id_count && !seen; ++k) seen = strcmp(ids[k], items[i].source_id) == 0;
    if (!seen) ids[id_count++] = items[i].source_id;
  }
  if (!id_count) { items_storage.reset(); return result; }
  // coinMarkets(ids) in path-bounded chunks (TypeScript sends one URL).
  const size_t mark = a.used;
  Value* rows = tj::Arr(a);
  bool lane_busy = false, failed = false;
  size_t offset = 0;
  while (rows && offset < id_count && !lane_busy && !failed) {
    size_t take = id_count - offset;
    if (take > 50) take = 50;
    auto fetch_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch))), 1);
  auto* fetch = fetch_storage.get();
    if (!fetch) { failed = true; break; }
    *fetch = providers::TokenFetch{};
    while (take > 0 && !token_fetch::CoinMarkets(fetch, ids + offset, take)) --take;
    if (!take) { failed = true; break; }
    fetch->background = true;
    work.env->executor->FetchAll(fetch, 1, work.deadline_ms);
    Value* chunk = nullptr;
    if (token_fetch::MarketRows(a, *fetch, &chunk) != token_fetch::Outcome::kValue) {
      lane_busy = fetch->lane_rejected;
      failed = !lane_busy;
    } else {
      for (uint32_t i = 0; i < chunk->count; ++i) tj::Push(a, rows, chunk->items[i]);
    }
    TokenFetchRelease(fetch);
    offset += take;
  }
  result.selected = eligible;
  if (lane_busy || failed || !rows) {
    a.used = mark;
    items_storage.reset();
    if (failed) result.failed = eligible;
    return result;
  }
  // TypeScript commits facts + comparison + demand of 64 tokens per
  // tokens.write, then primes the cache and records stats; each chunk is one
  // KV frame here (token_writes::RecordMarketRefreshes).
  struct Warmed {
    size_t candidate = 0;
    Value* market = nullptr;
    token_store::ComparisonPatch patch;
  };
  auto warmed_storage = base::AllocateStorage<Warmed[]>(sizeof(Warmed) * eligible);
  auto* warmed = warmed_storage.get();
  auto refreshes_storage = base::AllocateStorage<token_writes::MarketRefresh[]>(sizeof(token_writes::MarketRefresh) * eligible);
  auto* refreshes = refreshes_storage.get();
  auto found_storage = base::AllocateStorage<bool[]>(eligible);
  auto* found = found_storage.get();
  if (!warmed || !refreshes || !found) {
    warmed_storage.reset();
    refreshes_storage.reset();
    found_storage.reset();
    a.used = mark;
    items_storage.reset();
    result.failed = eligible;
    return result;
  }
  const int64_t written_at = work.Now();
  size_t warm_count = 0;
  for (size_t i = 0; i < eligible; ++i) {
    Candidate& c = items[i];
    Value* market = token_fetch::MarketRowFor(a, rows, c.source_id);
    found[i] = market && market->type != tj::Type::kNull;
    if (!found[i]) {
      ++result.unavailable;
      continue;
    }
    const Value* embedded = tj::Get(market, "source");
    const char* source = tj::IsString(embedded) ? tj::CString(a, embedded) : "coingecko";
    Warmed* w = new (&warmed[warm_count]) Warmed();
    w->candidate = i;
    w->market = market;
    token_views::MarketComparisonPatch(a, market, now, c.source_id, source, &w->patch);
    Value* fields = tj::Obj(a);
    Value* demand = tj::Obj(a);
    const bool built = fields && demand && tj::Set(a, fields, "market", market) &&
                       tj::Set(a, fields, "marketAsOf", tj::Num(a, static_cast<double>(now))) &&
                       tj::Set(a, fields, "marketSource", tj::Str(a, source)) &&
                       tj::Set(a, fields, "marketSourceId", tj::Str(a, c.source_id)) &&
                       tj::Set(a, demand, "lastRefreshedAt", tj::Num(a, static_cast<double>(now)));
    token_writes::MarketRefresh* r = new (&refreshes[warm_count]) token_writes::MarketRefresh();
    r->network = c.network;
    r->token_id = c.token_id;
    r->facts = built ? fields : nullptr;  // a null group is refused, not staged
    r->comparison = &w->patch;
    r->demand = demand;
    ++warm_count;
  }
  token_writes::RecordMarketRefreshes(*work.env->data, refreshes, warm_count, written_at);
  for (size_t k = 0; k < warm_count; ++k) {
    if (!refreshes[k].committed) {
      ++result.failed;
      continue;
    }
    Candidate& c = items[warmed[k].candidate];
    char key[128 + networks::kTokenRefMaxIdBytes + sizeof("tf:::market")];
    const int key_bytes = snprintf(key, sizeof(key), "tf:%s:%s:market", c.network, c.token_id);
    if (key_bytes >= 0 && static_cast<size_t>(key_bytes) < sizeof(key))
      work.env->swr->Write(key, active ? token_swr::kTokenMarketActiveWarm : token_swr::kTokenMarket, work.Now(),
                           warmed[k].market, work.scratch, work.scratch_capacity, nullptr);
    ++result.refreshed;
  }
  for (size_t i = 0; i < eligible && work.env->stats; ++i) {
    Candidate& c = items[i];
    FetchSample sample{};
    sample.network = c.network;
    sample.token_id = c.token_id;
    sample.source_id = c.source_id;
    sample.facet = FetchFacet::kMarket;
    sample.outcome = FetchOutcome::kLive;
    sample.negative = !found[i];
    sample.foreground = false;
    sample.at_ms = now;
    work.env->stats->Record(sample);
  }
  warmed_storage.reset();
  refreshes_storage.reset();
  found_storage.reset();
  a.used = mark;
  a.ok = true;
  items_storage.reset();
  return result;
}

const char* liquidity_slug(const char* network) {
  struct Slug { const char* network; const char* slug; };
  static constexpr Slug kSlugs[] = {
      {"eth-mainnet", "ethereum"}, {"bnb", "bsc"}, {"solana", "solana"}, {"cardano", "cardano"},
      {"base", "base"}, {"polygon", "polygon"}, {"avalanche", "avalanche"}, {"cronos", "cronos"},
      {"arbitrum", "arbitrum"}, {"optimism", "optimism"}, {"hyperevm", "hyperevm"},
      {"sei", "seiv2"}, {"flare", "flare"}, {"tron", "tron"},
  };
  for (const Slug& s : kSlugs) if (strcmp(s.network, network) == 0) return s.slug;
  return nullptr;
}

}  // namespace

JobResult WarmMarketsBroad(Work& work, int64_t now_ms) { return warm_markets(work, now_ms, false); }
JobResult WarmMarketsActive(Work& work, int64_t now_ms) { return warm_markets(work, now_ms, true); }

JobResult WarmLiquidity(Work& work, int64_t now) {
  JobResult result{};
  Arena& a = work.arena;
  auto items_storage = base::AllocateStorage<Candidate[]>(sizeof(Candidate) * kMaxCandidates);
  auto* items = items_storage.get();
  if (!items) return result;
  size_t count = collect(work, 2, now, items);
  if (count > 120) count = 120;
  result.selected = count;
  // Resolve targets; group by DexScreener slug in first-seen order.
  struct Target { size_t candidate; char address[networks::kTokenRefMaxInputBytes + 1]; const char* slug; };
  auto targets_storage = base::AllocateStorage<Target[]>(sizeof(Target) * (count + 1));
  auto* targets = targets_storage.get();
  if (!targets) { items_storage.reset(); return result; }
  size_t target_count = 0;
  for (size_t i = 0; i < count; ++i) {
    networks::TokenNetwork network;
    networks::TokenRef ref;
    const char* slug = nullptr;
    bool resolved = ResolveTokenNetwork(items[i].network, strlen(items[i].network), &network) &&
                    ParseTokenRef(&network, items[i].token_id, strlen(items[i].token_id), &ref);
    if (resolved) slug = liquidity_slug(network.id);
    if (!resolved || !ref.has_address || !slug) {
      if (token_history::RecordDiagnostic(work, items[i].network, items[i].token_id, "unsupported", now, nullptr))
        ++result.unsupported;
      else
        ++result.failed;
      continue;
    }
    Target& t = targets[target_count++];
    t.candidate = i;
    snprintf(t.address, sizeof(t.address), "%s", ref.address);
    t.slug = slug;
  }
  const char* order[32];
  size_t slugs = 0;
  for (size_t i = 0; i < target_count; ++i) {
    bool seen = false;
    for (size_t k = 0; k < slugs && !seen; ++k) seen = order[k] == targets[i].slug;
    if (!seen && slugs < 32) order[slugs++] = targets[i].slug;
  }
  bool foreground_owns_lane = false;
  for (size_t s = 0; s < slugs && !foreground_owns_lane; ++s) {
    for (size_t i = 0; i < target_count && !foreground_owns_lane; ++i) {
      if (targets[i].slug != order[s]) continue;
      Candidate& c = items[targets[i].candidate];
      const size_t mark = a.used;
      auto fetch_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch))), 1);
  auto* fetch = fetch_storage.get();
      if (!fetch) break;
      *fetch = providers::TokenFetch{};
      fetch->background = true;
      Value* pools = nullptr;
      bool ok = token_fetch::DexPools(fetch, targets[i].address);
      if (ok) {
        work.env->executor->FetchAll(fetch, 1, work.deadline_ms);
        ok = token_fetch::Pools(a, *fetch, targets[i].address, targets[i].slug, &pools) == token_fetch::Outcome::kValue;
        if (!ok && fetch->lane_rejected) { foreground_owns_lane = true; TokenFetchRelease(fetch); a.used = mark; break; }
        TokenFetchRelease(fetch);
      }
      FetchSample sample{};
      sample.network = c.network;
      sample.token_id = c.token_id;
      sample.source_id = c.has_source ? c.source_id : nullptr;
      sample.facet = FetchFacet::kLiquidity;
      sample.foreground = false;
      sample.at_ms = now;
      if (!ok) {
        token_history::RecordDiagnostic(work, c.network, c.token_id, "error", now, nullptr);
        sample.outcome = FetchOutcome::kFailure;
        if (work.env->stats) work.env->stats->Record(sample);
        ++result.failed;
        a.used = mark;
        continue;
      }
      Value* snapshot = nullptr;
      if (!token_liquidity::CalculateLiquiditySnapshot(a, pools, targets[i].address, token_liquidity::kDefaultPoolLimit,
                                                       &snapshot)) {
        ++result.failed;
        a.used = mark;
        continue;
      }
      token_history::ObservationContext context{};
      context.has_observed_at = true;
      context.observed_at = now;
      context.has_sampling = true;
      context.sampling_seconds = c.tier == 0 ? 300 : 900;
      sample.outcome = FetchOutcome::kLive;
      sample.negative = is_null(snapshot);
      bool persisted = true;
      if (is_null(snapshot)) {
        persisted = token_history::RecordNotFound(work, c.network, c.token_id, context);
        if (persisted) ++result.unavailable;
      } else {
        persisted = token_history::RecordSuccess(work, c.network, c.token_id, snapshot, context);
        if (persisted) {
          char key[128 + networks::kTokenRefMaxIdBytes + sizeof("tf:::liquidity-v2")];
          const int key_bytes = snprintf(key, sizeof(key), "tf:%s:%s:liquidity-v2", c.network, c.token_id);
          if (key_bytes >= 0 && static_cast<size_t>(key_bytes) < sizeof(key))
            work.env->swr->Write(key, c.tier == 0 ? token_swr::kTokenLiquidity : token_swr::kTokenLiquidityTier1,
                                 work.Now(), snapshot, work.scratch, work.scratch_capacity, nullptr);
          ++result.refreshed;
        }
      }
      if (!persisted) ++result.failed;
      if (work.env->stats) work.env->stats->Record(sample);
      a.used = mark;
      a.ok = true;
    }
  }
  targets_storage.reset();
  items_storage.reset();
  return result;
}

JobResult WarmHolders(Work& work, int64_t now) {
  JobResult result{};
  Arena& a = work.arena;
  auto items_storage = base::AllocateStorage<Candidate[]>(sizeof(Candidate) * kMaxCandidates);
  auto* items = items_storage.get();
  if (!items) return result;
  size_t count = collect(work, 3, now, items);
  const size_t cap = 50;
  if (count > cap * 4) count = cap * 4;
  // supported: the holder source, or a Solana SPL mint (chain metadata).
  size_t supported = 0;
  for (size_t i = 0; i < count; ++i) {
    networks::TokenNetwork network;
    networks::TokenRef ref;
    if (!ResolveTokenNetwork(items[i].network, strlen(items[i].network), &network) ||
        !ParseTokenRef(&network, items[i].token_id, strlen(items[i].token_id), &ref))
      continue;
    const bool evm = ref.kind == networks::TokenRefKind::kErc20 && strcmp(network.family, "evm") == 0 &&
                     (strcmp(network.id, "eth-mainnet") == 0 ||
                      (work.env->config && work.env->config->etherscan_api_key_configured));
    // DriverTokenHolderSource: Tron's tokenHolders exists only with a
    // TronScan key.
    const bool driver = ref.has_address && (strcmp(network.family, "stellar") == 0 ||
                                            strcmp(network.family, "hedera") == 0 ||
                                            strcmp(network.family, "cardano") == 0 ||
                                            token_tron::HoldersSupported(work.env->config, network, ref));
    const bool solana = strcmp(network.family, "solana") == 0 && ref.kind == networks::TokenRefKind::kSpl;
    if (evm || driver || solana) items[supported++] = items[i];
  }
  const size_t tier0_cap = 40;
  size_t tier0 = 0, tier1 = 0;
  for (size_t i = 0; i < supported; ++i) {
    Candidate& c = items[i];
    if (c.tier == 0) { if (tier0 >= tier0_cap) continue; ++tier0; }
    else if (c.tier == 1) { if (tier1 >= cap - tier0_cap) continue; ++tier1; }
    else continue;
    ++result.selected;
    const size_t mark = a.used;
    token_facts::Facts* facts = static_cast<token_facts::Facts*>(a.Alloc(sizeof(token_facts::Facts)));
    if (!facts) { ++result.failed; continue; }
    new (facts) token_facts::Facts();
    const token_facts::FactsOutcome status = token_facts::ComputeFacts(work, std::string_view{c.network, strlen(c.network)}, std::string_view{c.token_id, strlen(c.token_id)}, nullptr, token_facts::FacetBit(token_facts::kHolders) | token_facts::FacetBit(token_facts::kChainMeta), facts);
    if (status != token_facts::FactsOutcome::kOk) ++result.failed;
    else if (facts->holders) ++result.refreshed;
    else ++result.unavailable;
    a.used = mark;
    a.ok = true;
  }
  items_storage.reset();
  return result;
}

}  // namespace fortis::tokens::token_jobs
