#include "ownership.h"
#include <string_view>
#include <new>
#include "price_jobs.h"
#include "log.h"

#include "price_series_store.h"
#include "spot_provider_owner.h"
#include "spot_resolution.h"
#include "threading.h"
#include "token_data.h"
#include "token_json.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::price_jobs {
namespace {

using tj::Arena;
using tj::Value;

constexpr size_t kIdBytes = providers::kPriceIdMaxSourceIdBytes + 1;
constexpr size_t kRowArenaBytes = 1024 * 1024;
constexpr size_t kTouchBufferBytes = 64 * 1024;

uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL + static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

void pause_us(uint32_t us) {
  if (!us) return;
  timespec delay{static_cast<time_t>(us / 1000000U), static_cast<long>(us % 1000000U) * 1000L};
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

bool js_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// Number(value) for the JSON values a demand row can hold.
double js_number(const Value* v) {
  if (!v) return NAN;  // undefined
  switch (v->type) {
    case tj::Type::kNull: case tj::Type::kFalse: return 0;
    case tj::Type::kTrue: return 1;
    case tj::Type::kNumber: return v->number;
    case tj::Type::kString: break;
    default: return NAN;
  }
  const char* text = v->text;
  size_t start = 0, end = v->length;
  while (start < end && js_space(text[start])) ++start;
  while (end > start && js_space(text[end - 1])) --end;
  if (start == end) return 0;
  char buffer[128];
  if (end - start >= sizeof(buffer)) return NAN;
  memcpy(buffer, text + start, end - start);
  buffer[end - start] = '\0';
  const char* body = buffer[0] == '+' || buffer[0] == '-' ? buffer + 1 : buffer;
  if (strcmp(body, "Infinity") == 0) return buffer[0] == '-' ? -INFINITY : INFINITY;
  // strtod also accepts inf/nan/hex floats, which Number() does not.
  for (const char* c = body; *c; ++c)
    if (!((*c >= '0' && *c <= '9') || *c == '.' || *c == 'e' || *c == 'E' || *c == '+' || *c == '-')) return NAN;
  char* parsed_end = nullptr;
  const double value = strtod(buffer, &parsed_end);
  return parsed_end && *parsed_end == '\0' && parsed_end != buffer ? value : NAN;
}

struct Ranked {
  char source_id[kIdBytes];
  double tier, last, score;
};

// ranked.sort((a, b) => a.priceTier - b.priceTier || (a.last ?? 0) - (b.last ?? 0) ||
//   Number(b.priceScore) - Number(a.priceScore)): NaN and 0 fall through.
bool ranked_before(const Ranked& x, const Ranked& y) {
  double d = x.tier - y.tier;
  if (d != 0 && !isnan(d)) return d < 0;
  d = x.last - y.last;
  if (d != 0 && !isnan(d)) return d < 0;
  d = y.score - x.score;
  if (d != 0 && !isnan(d)) return d < 0;
  return false;
}

// A stable sort truncated to `capacity` rows, kept incrementally: a row
// enters behind every row it does not precede, as an insertion sort places
// it, and the row past the cap falls off.
struct RankScan {
  Ranked* items;
  size_t capacity, count;
  size_t kept;
  int64_t now;
  Ranked candidate;
};

bool rank_visit(void* context, const Value* row) {
  auto* scan = static_cast<RankScan*>(context);
  const Value* source = tj::Get(row, "sourceId");
  if (!tj::IsString(source) || !source->length || source->length >= kIdBytes) return true;
  const Value* tier = tj::Get(row, "priceTier");
  const double tier_value = tj::IsNumber(tier) ? tier->number : NAN;
  if (tier_value > 1) return true;
  const Value* last = tj::Get(row, "lastRefreshedAt");
  const bool has_last = tj::IsNumber(last);
  if (tier_value == 1 && has_last && last->number > static_cast<double>(scan->now - kTier1DueMs)) return true;
  ++scan->kept;
  Ranked& c = scan->candidate;
  memcpy(c.source_id, source->text, source->length);
  c.source_id[source->length] = '\0';
  c.tier = tier_value;
  c.last = has_last ? last->number : 0;
  c.score = js_number(tj::Get(row, "priceScore"));
  if (!scan->capacity) return true;
  size_t j = scan->count;
  if (j == scan->capacity) {
    if (!ranked_before(c, scan->items[j - 1])) return true;
    --j;
  } else {
    ++scan->count;
  }
  while (j > 0 && ranked_before(c, scan->items[j - 1])) {
    scan->items[j] = scan->items[j - 1];
    --j;
  }
  scan->items[j] = c;
  return true;
}

struct Chosen {
  char fiat[6];
  char source_id[kIdBytes];
};

bool chosen_has(const Chosen* chosen, size_t count, const char* source_id) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(chosen[i].source_id, source_id) == 0) return true;
  return false;
}

// Demand rows whose source ID is in `fresh` and whose lastRefreshedAt is not
// already `now` (a later pass after a full buffer), packed "network\0token\0".
struct TouchScan {
  const char* const* fresh;
  size_t fresh_count;
  int64_t now;
  char* buffer;
  size_t used;
  const char** networks;
  const char** tokens;
  size_t count, capacity;
  bool overflow;
};

bool touch_visit(void* context, const Value* row) {
  auto* scan = static_cast<TouchScan*>(context);
  const Value* source = tj::Get(row, "sourceId");
  if (!tj::IsString(source) || !source->length) return true;
  bool match = false;
  for (size_t i = 0; i < scan->fresh_count && !match; ++i)
    match = strlen(scan->fresh[i]) == source->length && memcmp(scan->fresh[i], source->text, source->length) == 0;
  if (!match) return true;
  const Value* last = tj::Get(row, "lastRefreshedAt");
  if (tj::IsNumber(last) && last->number == static_cast<double>(scan->now)) return true;
  const Value* network = tj::Get(row, "network");
  const Value* token = tj::Get(row, "tokenId");
  if (!tj::IsString(network) || !tj::IsString(token)) return true;
  const size_t need = network->length + token->length + 2;
  if (scan->count == scan->capacity || scan->used + need > kTouchBufferBytes) {
    scan->overflow = true;
    return false;
  }
  char* out = scan->buffer + scan->used;
  memcpy(out, network->text, network->length);
  out[network->length] = '\0';
  memcpy(out + network->length + 1, token->text, token->length);
  out[need - 1] = '\0';
  scan->networks[scan->count] = out;
  scan->tokens[scan->count] = out + network->length + 1;
  ++scan->count;
  scan->used += need;
  return true;
}

// for (offset; offset < touched.length; offset += 128) tokens.write(...)
bool touch(token_data::TokenDataStore& data, const char* const* fresh, size_t fresh_count, int64_t now,
           Arena& rows, RefreshResult* result) {
  const size_t capacity = 4096;
  auto buffer_storage = base::AllocateStorage<char[]>(kTouchBufferBytes);
  auto* buffer = buffer_storage.get();
  auto networks_storage = base::AllocateStorage<const char*[]>(sizeof(char*) * capacity);
  auto* networks = networks_storage.get();
  auto tokens_storage = base::AllocateStorage<const char*[]>(sizeof(char*) * capacity);
  auto* tokens = tokens_storage.get();
  uint8_t field_memory[512];
  Arena field_arena{field_memory, sizeof(field_memory), 0, true};
  Value* fields = tj::Obj(field_arena);
  bool ok = buffer && networks && tokens && fields &&
            tj::Set(field_arena, fields, "lastRefreshedAt", tj::Num(field_arena, static_cast<double>(now)));
  // Every pass touches at least one row or ends; rows touched by an earlier
  // pass carry lastRefreshedAt == now and are skipped.
  for (bool more = ok; ok && more;) {
    TouchScan scan{fresh, fresh_count, now, buffer, 0, networks, tokens, 0, capacity, false};
    data.ForEachDemand(now, rows, touch_visit, &scan);
    more = scan.overflow && scan.count > 0;
    for (size_t offset = 0; ok && offset < scan.count; offset += kTouchBatch) {
      const size_t take = scan.count - offset < kTouchBatch ? scan.count - offset : kTouchBatch;
      size_t frames = 0;
      // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
      // cppcheck-suppress arithOperationsOnVoidPointer
      ok = data.PatchDemandMany(networks + offset, tokens + offset, take, fields, now, &frames);
      result->touch_frames += frames;
      if (ok) result->touched += take;
    }
  }
  buffer_storage.reset();
  networks_storage.reset();
  tokens_storage.reset();
  return ok;
}

// ---- the owner-backed spot call ------------------------------------------------

struct ChunkWait {
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  bool finished = false;
  bool complete = false;
  std::unique_ptr<uint8_t[], base::StorageDeleter> body;
  size_t length = 0;
};

void chunk_done(void* context, const prices::SpotProviderOwnerResult& result) {
  auto* wait = static_cast<ChunkWait*>(context);
  fortis::base::LockGuard lock(wait->mutex);
  wait->complete = result.status == prices::SpotProviderOwnerResultStatus::kComplete && result.body && result.body_length;
  if (wait->complete) {
    wait->body = base::AllocateStorage<uint8_t[]>(result.body_length);
    if (wait->body) {
      memcpy(wait->body.get(), result.body, result.body_length);
      wait->length = result.body_length;
    } else {
      wait->complete = false;
    }
  }
  wait->finished = true;
  wait->done.Signal();
}

}  // namespace

bool SpotOwnerFetch(void* opaque, const char* fiat, const char* const* ids, size_t count, bool* fresh) {
  const auto* context = static_cast<const SpotOwnerFetchContext*>(opaque);
  prices::SpotProviderOwner* owner = context ? context->owner : nullptr;
  for (size_t i = 0; i < count; ++i) fresh[i] = false;
  if (!owner || !count) return owner != nullptr;
  const size_t chunks = (count + prices::kSpotResolutionMaxIds - 1) / prices::kSpotResolutionMaxIds;
  std::unique_ptr<ChunkWait[]> waits_storage;
  try { waits_storage = std::make_unique<ChunkWait[]>(chunks); } catch (const std::bad_alloc&) { waits_storage.reset(); }
  auto* waits = waits_storage.get();
  auto views_storage = base::AllocateStorage<std::string_view[]>(sizeof(std::string_view) * count);
  auto* views = views_storage.get();
  auto submitted_storage = base::AllocateZeroedStorage<bool[]>(chunks, sizeof(bool));
  auto* submitted = submitted_storage.get();
  auto request_ids_storage = base::AllocateZeroedStorage<uint64_t[]>(chunks, sizeof(uint64_t));
  auto* request_ids = request_ids_storage.get();
  if (!waits || !views || !submitted || !request_ids) {
    waits_storage.reset(); views_storage.reset(); submitted_storage.reset(); request_ids_storage.reset();
    return false;
  }
  for (size_t i = 0; i < count; ++i) new (&views[i]) std::string_view{ids[i], strlen(ids[i])};
  auto drain = [&](prices::SpotProviderOwner*) noexcept {
    for (size_t c = 0; c < chunks; ++c) {
      if (!submitted[c]) continue;
      base::UniqueLock lock(waits[c].mutex);
      if (waits[c].finished) continue;
      lock.Unlock();
      owner->Cancel(request_ids[c]);
      lock.Lock();
      while (!waits[c].finished) waits[c].done.WaitLocked(waits[c].mutex);
    }
  };
  std::unique_ptr<prices::SpotProviderOwner, decltype(drain)> pending(owner, drain);
  const std::string_view fiat_view{fiat, strlen(fiat)};
  bool any = false;
  // Submitted back to back, the chunks' misses share one batch window.
  for (size_t c = 0; c < chunks; ++c) {
    ChunkWait& wait = waits[c];


    const size_t first = c * prices::kSpotResolutionMaxIds;
    const size_t take = count - first < prices::kSpotResolutionMaxIds ? count - first : prices::kSpotResolutionMaxIds;
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    const auto submission = owner->Submit(views + first, take, fiat_view, prices::SpotProviderOwnerFormat::kJson,
        monotonic_ms() + prices::kSpotProviderOwnerMaxDeadlineMs, chunk_done, &wait,
        prices::SpotSubmitMode::kBackground);
    submitted[c] = submission.ticket() != nullptr;
    if (const auto* ticket = submission.ticket()) request_ids[c] = ticket->request_id;
    any = any || submitted[c];
  }
  for (size_t c = 0; c < chunks; ++c) {
    ChunkWait& wait = waits[c];
    if (submitted[c]) {
      // Every accepted request completes exactly once (a cancellation too).
      bool cancelled = false;
      fortis::base::UniqueLock lock(wait.mutex);
      while (!wait.finished) {
        if (!cancelled && context->stop && context->stop(context->stop_context)) {
          lock.Unlock();
          for (size_t k = c; k < chunks; ++k)
            if (submitted[k]) owner->Cancel(request_ids[k]);
          cancelled = true;
          lock.Lock();
          continue;
        }
        timespec wake{};
        clock_gettime(CLOCK_REALTIME, &wake);
        wake.tv_nsec += 50 * 1000000L;
        if (wake.tv_nsec >= 1000000000L) { ++wake.tv_sec; wake.tv_nsec -= 1000000000L; }
        wait.done.WaitUntilLocked(wait.mutex, wake);
      }
    }
    if (wait.complete) {
      // A 100-entry reply (~17 KiB) parses into several times its size;
      // growing blocks are at least tj::kArenaBlockBytes.
      Arena a = tj::GrowingArena(tj::kArenaBlockBytes + wait.length * 32);
      std::unique_ptr<Arena, tj::ArenaBlocksDeleter> arena_blocks(&a);
      const Value* parsed = tj::Parse(a, wait.body.get(), wait.length);
      if (!parsed) fortis::base::LogError("tokens", "active price refresh: unreadable spot reply");
      {
        const Value* prices = tj::Get(parsed, "prices");
        const size_t first = c * prices::kSpotResolutionMaxIds;
        const size_t take = count - first < prices::kSpotResolutionMaxIds ? count - first : prices::kSpotResolutionMaxIds;
        for (size_t i = first; i < first + take; ++i) {
          const Value* entry = tj::Get(prices, ids[i]);
          const Value* hit = tj::Get(tj::Get(entry, "cache"), "hit");
          fresh[i] = tj::IsObject(entry) && hit && hit->type == tj::Type::kFalse;
        }
      }
      tj::FreeArena(a);
    }
    wait.body.reset();


  }
  pending.reset();
  waits_storage.reset();
  views_storage.reset();
  submitted_storage.reset();
  request_ids_storage.reset();
  return any;
}

RefreshResult RefreshActive(token_data::TokenDataStore& data, const prices::PriceSeriesStore& series, int64_t now,
                            SpotFetchFn fetch, void* fetch_context, const RefreshOptions& options) {
  RefreshResult result{};
  const size_t cap = options.cap;
  auto row_memory_storage = base::AllocateStorage<uint8_t[]>(kRowArenaBytes);
  auto* row_memory = row_memory_storage.get();
  auto ranked_storage = base::AllocateStorage<Ranked[]>(sizeof(Ranked) * (cap ? cap : 1));
  auto* ranked = ranked_storage.get();
  auto chosen_storage = base::AllocateStorage<Chosen[]>(sizeof(Chosen) * (cap ? cap : 1));
  auto* chosen = chosen_storage.get();
  const size_t recent_capacity = cap < prices::kPriceSeriesMaxRecentSpots ? cap : prices::kPriceSeriesMaxRecentSpots;
  auto recent_storage = base::AllocateStorage<prices::PriceSeriesRecentSpot[]>(sizeof(prices::PriceSeriesRecentSpot) * (recent_capacity ? recent_capacity : 1));
  auto* recent = recent_storage.get();
  auto ids_storage = base::AllocateStorage<const char*[]>(sizeof(char*) * (cap ? cap : 1));
  auto* ids = ids_storage.get();
  auto fresh_storage = base::AllocateStorage<bool[]>(sizeof(bool) * (cap ? cap : 1));
  auto* fresh = fresh_storage.get();
  if (!row_memory || !ranked || !chosen || !recent || !ids || !fresh) {
    result.demand_failed = true;
  } else {
    Arena rows{row_memory, kRowArenaBytes, 0, true};
    auto scan_storage = base::AllocateStorage<RankScan[]>(sizeof(RankScan));
  auto* scan = scan_storage.get();
    if (!scan) {
      result.demand_failed = true;
    } else {
      *scan = RankScan{ranked, cap, 0, 0, now, {}};
      result.demand_failed = !data.ForEachDemand(now, rows, rank_visit, scan);
      result.ranked = scan->kept;
      // const chosen = new Set(ranked.flatMap((row) => row.sourceId ...)): demand ids are USD.
      size_t count = 0;
      for (size_t i = 0; i < scan->count; ++i) {
        if (chosen_has(chosen, count, ranked[i].source_id)) continue;
        snprintf(chosen[count].fiat, sizeof(chosen[count].fiat), "usd");
        snprintf(chosen[count].source_id, kIdBytes, "%s", ranked[i].source_id);
        ++count;
      }
      scan_storage.reset();
      // series.recentSpots(Date.now() - sinceHours * 3_600_000, cap)
      size_t recent_count = 0;
      const prices::PriceSeriesReadStatus read = series.RecentSpots(now - options.recent_window_ms, now, recent,
                                                            recent_capacity, &recent_count, &result.recent_scanned);
      result.recent_failed = read != prices::PriceSeriesReadStatus::kFound && read != prices::PriceSeriesReadStatus::kMissing;
      // A failed store read throws in TypeScript: the run ends before any fetch.
      if (result.demand_failed || result.recent_failed) recent_count = 0, count = 0;
      for (size_t i = 0; i < recent_count; ++i) {
        if (count >= cap || chosen_has(chosen, count, recent[i].source_id)) continue;
        memcpy(chosen[count].fiat, recent[i].fiat, sizeof(chosen[count].fiat));
        snprintf(chosen[count].source_id, kIdBytes, "%s", recent[i].source_id);
        ++count;
        ++result.from_recent;
      }
      result.chosen = count;
      // byFiat in Map insertion order: one spot call per fiat.
      auto done_storage = base::AllocateZeroedStorage<bool[]>(count ? count : 1, sizeof(bool));
  auto* done = done_storage.get();
      if (!done) result.fetch_failed = count > 0;
      for (size_t g = 0; done && g < count; ++g) {
        if (done[g]) continue;
        if (options.stop && options.stop(options.stop_context)) { result.stopped = true; break; }
        size_t n = 0;
        for (size_t i = g; i < count; ++i)
          if (!done[i] && strcmp(chosen[i].fiat, chosen[g].fiat) == 0) { ids[n++] = chosen[i].source_id; done[i] = true; }
        ++result.groups;
        if (options.observe) options.observe(options.observe_context, chosen[g].fiat, ids, n);
        if (!fetch || !fetch(fetch_context, chosen[g].fiat, ids, n, fresh)) {
          result.fetch_failed = true;
          continue;
        }
        size_t successful = 0;
        for (size_t i = 0; i < n; ++i)
          if (fresh[i]) ids[successful++] = ids[i];
        result.refreshed += successful;
        rows.used = 0;
        rows.ok = true;
        // A failed write throws in TypeScript and ends the run.
        if (successful && !touch(data, ids, successful, now, rows, &result)) { result.touch_failed = true; break; }
      }
      done_storage.reset();
    }
  }
  row_memory_storage.reset();
  ranked_storage.reset();
  chosen_storage.reset();
  recent_storage.reset();
  ids_storage.reset();
  fresh_storage.reset();
  return result;
}

SealResult SealDueYielding(prices::PriceSeriesStore& series, int64_t now, size_t limit, size_t batch, StopFn stop,
                           void* stop_context, uint32_t yield_us) {
  SealResult result{};
  if (!batch) return result;
  // Every stored frame seals a row or retires an empty marker, so at most
  // `limit` frames run even when markers hold no open rows.
  while (result.sealed < limit && result.frames < limit) {
    const size_t size = limit - result.sealed < batch ? limit - result.sealed : batch;
    size_t sealed = 0;
    const prices::PriceSeriesStoreStatus status = series.SealDue(now, size, &sealed);
    if (status == prices::PriceSeriesStoreStatus::kNoItems) break;
    if (status != prices::PriceSeriesStoreStatus::kStored) { result.failed = true; break; }
    ++result.frames;
    result.sealed += sealed;
    if (stop && stop(stop_context)) { result.stopped = true; break; }
    // await new Promise((resolve) => setImmediate(resolve))
    pause_us(yield_us);
  }
  return result;
}

}  // namespace fortis::tokens::price_jobs
