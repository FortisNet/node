#include <memory>
#include "exception_boundary.h"
#include "coin_popularity.h"

#include "js_case.h"
#include "kv_store.h"
#include "price_history_batch_codec.h"
#include "price_id_parser.h"
#include "threading.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <new>

namespace fortis::discovery {

namespace coin_popularity {

double Decayed(double value, int64_t from_ms, int64_t to_ms) {
  if (to_ms <= from_ms) return value;
  return value * pow(0.5, static_cast<double>(to_ms - from_ms) /
                              static_cast<double>(kCoinPopularityHalfLifeMs));
}

CoinPopularityState RecordRequest(const CoinPopularityState* state, int64_t now_ms, bool new_user) {
  const double users = state ? Decayed(state->users, state->updated_at_ms, now_ms) : 0;
  const double hits = state ? Decayed(state->hits, state->updated_at_ms, now_ms) : 0;
  CoinPopularityState out;
  out.users = users + (new_user ? 1 : 0);
  out.hits = hits + 1;
  out.updated_at_ms = now_ms;
  out.last_requested_at_ms = now_ms;
  return out;
}

double Score(const CoinPopularityState& state, int64_t now_ms) {
  const double users = Decayed(state.users, state.updated_at_ms, now_ms);
  double hits = Decayed(state.hits, state.updated_at_ms, now_ms);
  if (hits > 500) hits = 500;  // Math.min(hits, 500); hits is never NaN
  return 4 * log1p(users) + log1p(hits);
}

bool IsExpired(const CoinPopularityState& state, int64_t now_ms) {
  return now_ms - state.last_requested_at_ms > kCoinPopularityExpireAfterMs;
}

// ⌊log₂(rank/64 + 1)⌋ for an integer rank is the largest k with
// 64·(2^k − 1) ≤ rank: computed exactly, without trusting libm's log2 at the
// powers of two.
uint32_t BucketForRank(int64_t rank) {
  if (rank < 0) rank = 0;
  uint32_t bucket = 0;
  while (bucket < 62 &&
         static_cast<int64_t>(kCoinPopularityBucketBaseSize) * ((int64_t{1} << (bucket + 1)) - 1) <= rank)
    ++bucket;
  return bucket;
}

int64_t IntervalForBucket(int64_t bucket) {
  if (bucket < 0) bucket = 0;
  if (bucket >= 6) return kCoinPopularityMaxIntervalMs;
  const int64_t interval = kCoinPopularityBaseIntervalMs << bucket;
  return interval < kCoinPopularityMaxIntervalMs ? interval : kCoinPopularityMaxIntervalMs;
}

namespace {

// The next UTF-16 code unit(s) of UTF-8 text. Malformed bytes read as
// themselves (route ids are valid UTF-8; surrogate escapes decode as their
// code point, which is the unit JavaScript holds).
size_t utf16_units(const unsigned char* text, size_t length, size_t* position, uint16_t units[2]) {
  const unsigned char c = text[(*position)++];
  if (c < 0x80) { units[0] = c; return 1; }
  size_t trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
  if (!trailing || *position + trailing > length) { units[0] = c; return 1; }
  uint32_t value = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
  for (size_t k = 0; k < trailing; ++k) value = value << 6 | (text[(*position)++] & 0x3f);
  if (value < 0x10000) { units[0] = static_cast<uint16_t>(value); return 1; }
  value -= 0x10000;
  units[0] = static_cast<uint16_t>(0xd800 + (value >> 10));
  units[1] = static_cast<uint16_t>(0xdc00 + (value & 0x3ff));
  return 2;
}

void fnv_text(uint32_t* hash, const char* text, size_t length) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  size_t position = 0;
  while (position < length) {
    uint16_t units[2];
    const size_t n = utf16_units(bytes, length, &position, units);
    for (size_t i = 0; i < n; ++i) {
      *hash ^= units[i];
      *hash *= 0x01000193u;  // Math.imul
    }
  }
}

}  // namespace

uint32_t PairHash(const char* device, size_t device_length, const char* id, size_t id_length) {
  uint32_t hash = 0x811c9dc5u;
  fnv_text(&hash, device, device_length);
  hash ^= 0;  // the \u0000 separator
  hash *= 0x01000193u;
  fnv_text(&hash, id, id_length);
  return hash;
}

int IdCompare(const char* a, size_t a_length, const char* b, size_t b_length) {
  const size_t common = a_length < b_length ? a_length : b_length;
  const int order = common ? memcmp(a, b, common) : 0;
  if (order) return order < 0 ? -1 : 1;
  return a_length == b_length ? 0 : a_length < b_length ? -1 : 1;
}

}  // namespace coin_popularity

namespace {

namespace cp = coin_popularity;

constexpr int64_t kDayMs = 86400000;
constexpr size_t kMinRows = 256;
constexpr uint8_t kDirty = 1;
constexpr uint8_t kRefreshed = 2;

struct Row {
  CoinPopularityState state;
  int64_t refreshed_at_ms;
  uint64_t seq;
  uint32_t id_offset;
  uint16_t id_length;
  uint8_t flags;
};

int64_t wall_ms() {
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

int64_t floor_div(int64_t value, int64_t divisor) {
  int64_t q = value / divisor;
  if ((value % divisor) && ((value < 0) != (divisor < 0))) --q;
  return q;
}

uint64_t id_hash(const char* text, size_t length) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<unsigned char>(text[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

size_t power_of_two_at_least(size_t value) {
  size_t out = 1;
  while (out < value) out <<= 1;
  return out;
}

// Sort keys for due() and prune(): self-contained for qsort.
struct Ranked {
  double score;
  const char* id;
  uint64_t seq;
  uint32_t row;
  uint16_t id_length;
};

int rank_order(const void* left, const void* right) {
  const auto* a = static_cast<const Ranked*>(left);
  const auto* b = static_cast<const Ranked*>(right);
  // Higher score first, then id in byte order, then insertion order.
  if (a->score != b->score) return a->score > b->score ? -1 : 1;
  const int order = cp::IdCompare(a->id, a->id_length, b->id, b->id_length);
  if (order) return order;
  return a->seq < b->seq ? -1 : a->seq > b->seq ? 1 : 0;
}

int prune_order(const void* left, const void* right) {
  const auto* a = static_cast<const Ranked*>(left);
  const auto* b = static_cast<const Ranked*>(right);
  // a.score - b.score, stable.
  if (a->score != b->score) return a->score < b->score ? -1 : 1;
  return a->seq < b->seq ? -1 : a->seq > b->seq ? 1 : 0;
}

}  // namespace

template <typename T>
void CoinPopularityList<T>::Release() {
  items.reset();
  text.reset();
  items = nullptr;
  text = nullptr;
  count = 0;
}
template struct CoinPopularityList<RankedCoin>;
template struct CoinPopularityList<PopularityRow>;

struct CoinPopularity::Impl {
  CoinPopularityOptions options;
  mutable fortis::base::Mutex mutex;
  std::unique_ptr<Row[], base::StorageDeleter> rows;
  size_t row_count = 0, row_capacity = 0, hard_capacity = 0;
  std::unique_ptr<char[], base::StorageDeleter> ids;
  size_t id_used = 0, id_capacity = 0;
  std::unique_ptr<uint32_t[], base::StorageDeleter> index;  // row + 1, 0 = empty
  size_t index_slots = 0;
  uint64_t next_seq = 1;
  int64_t seen_day = INT64_MIN;
  std::unique_ptr<uint32_t[], base::StorageDeleter> seen;  // 0 = empty; the zero hash is `seen_zero`
  size_t seen_count = 0, seen_limit = 0;
  bool seen_zero = false;
  size_t dropped = 0;
  size_t peak_bytes = 0;
  // TypeScript's refreshedAt map is separate from its states map: a mark for
  // a coin with no state (pruned while its batch was in flight) survives and
  // applies when the coin is tracked again. A small FIFO table keeps such
  // marks (a native bound: 64 marks of ids up to 128 bytes).
  struct GhostMark {
    bool used;
    uint8_t length;
    int64_t at;
    char id[128];
  };
  static constexpr size_t kGhostMarks = 64;
  GhostMark ghosts[kGhostMarks]{};
  size_t ghost_next = 0;

  GhostMark* FindGhost(const char* id, size_t length) {
    for (GhostMark& ghost : ghosts)
      if (ghost.used && ghost.length == length && memcmp(ghost.id, id, length) == 0) return &ghost;
    return nullptr;
  }
  void PutGhost(const char* id, size_t length, int64_t at) {
    if (length > sizeof(GhostMark::id)) return;
    GhostMark* ghost = FindGhost(id, length);
    if (!ghost) {
      ghost = &ghosts[ghost_next];
      ghost_next = (ghost_next + 1) % kGhostMarks;
      ghost->used = true;
      ghost->length = static_cast<uint8_t>(length);
      memcpy(ghost->id, id, length);
    }
    ghost->at = at;
  }

  int64_t Now() const {
    if (options.clock) {
      const int64_t now = options.clock->NowMs();
      if (now >= 0) return now;
    }
    return wall_ms();
  }

  size_t AllocatedBytes() const {
    return row_capacity * sizeof(Row) + id_capacity + index_slots * sizeof(uint32_t) +
           (seen ? kCoinPopularitySeenSlots * sizeof(uint32_t) : 0);
  }
  void NotePeak() {
    const size_t bytes = AllocatedBytes();
    if (bytes > peak_bytes) peak_bytes = bytes;
  }

  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  const char* IdOf(const Row& row) const { return ids.get() + row.id_offset; }

  // ---- id index -------------------------------------------------------------
  bool RebuildIndex(size_t slots) {
    auto table = base::AllocateZeroedStorage<uint32_t[]>(slots, sizeof(uint32_t));
    if (!table) return false;
    index.reset();
    index = std::move(table);
    index_slots = slots;
    for (size_t r = 0; r < row_count; ++r) {
      size_t slot = id_hash(IdOf(rows[r]), rows[r].id_length) & (slots - 1);
      while (index[slot]) slot = (slot + 1) & (slots - 1);
      index[slot] = static_cast<uint32_t>(r + 1);
    }
    return true;
  }

  Row* Find(const char* id, size_t length) {
    if (!index_slots) return nullptr;
    size_t slot = id_hash(id, length) & (index_slots - 1);
    while (index[slot]) {
      Row& row = rows[index[slot] - 1];
      if (row.id_length == length && memcmp(IdOf(row), id, length) == 0) return &row;
      slot = (slot + 1) & (index_slots - 1);
    }
    return nullptr;
  }

  // Appends a row (Map.set of a new key); null at capacity or on OOM.
  Row* Insert(const char* id, size_t length) {
    if (row_count == row_capacity) {
      if (row_capacity >= hard_capacity) return nullptr;
      size_t next = row_capacity ? row_capacity * 2 : kMinRows;
      if (next > hard_capacity) next = hard_capacity;
      if (!base::ResizeStorage(rows, row_count * sizeof(Row), next * sizeof(Row))) return nullptr;
      row_capacity = next;
    }
    if (id_used + length + 1 > id_capacity) {
      size_t next = id_capacity ? id_capacity * 2 : 4096;
      while (next < id_used + length + 1) next *= 2;
      if (!base::ResizeStorage(ids, id_used, next)) return nullptr;
      id_capacity = next;
    }
    if ((row_count + 1) * 2 > index_slots &&
        !RebuildIndex(power_of_two_at_least((row_count + 1) * 2 < 512 ? 512 : (row_count + 1) * 2)))
      return nullptr;
    Row& row = rows[row_count];
    row = Row{};
    row.seq = next_seq++;
    row.id_offset = static_cast<uint32_t>(id_used);
    row.id_length = static_cast<uint16_t>(length);
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    memcpy(ids.get() + id_used, id, length);
    ids[id_used + length] = '\0';
    id_used += length + 1;
    size_t slot = id_hash(id, length) & (index_slots - 1);
    while (index[slot]) slot = (slot + 1) & (index_slots - 1);
    index[slot] = static_cast<uint32_t>(row_count + 1);
    ++row_count;
    NotePeak();
    if (GhostMark* ghost = FindGhost(id, length)) {
      row.refreshed_at_ms = ghost->at;
      row.flags |= kRefreshed;
      ghost->used = false;
    }
    return &row;
  }

  // ---- daily distinct set -----------------------------------------------------
  void ResetSeen() {
    seen.reset();
    seen = nullptr;
    seen_count = 0;
    seen_zero = false;
  }
  // Set.has / Set.add; false when already present (or the table is gone).
  bool SeenAdd(uint32_t hash) {
    if (hash == 0) {
      if (seen_zero) return false;
      seen_zero = true;
      ++seen_count;
      return true;
    }
    if (!seen) {
      seen = base::AllocateZeroedStorage<uint32_t[]>(kCoinPopularitySeenSlots, sizeof(uint32_t));
      if (!seen) return false;
      NotePeak();
    }
    const size_t mask = kCoinPopularitySeenSlots - 1;
    size_t slot = (hash * 0x9e3779b1u) & mask;
    while (seen[slot]) {
      if (seen[slot] == hash) return false;
      slot = (slot + 1) & mask;
    }
    seen[slot] = hash;
    ++seen_count;
    return true;
  }

  // ---- prune ------------------------------------------------------------------
  // Keeps rows for which keep[r] is true, in order; rebuilds the index.
  void Compact(const bool* keep) {
    size_t out = 0, id_out = 0;
    for (size_t r = 0; r < row_count; ++r) {
      if (!keep[r]) continue;
      Row row = rows[r];
      // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
      // cppcheck-suppress arithOperationsOnVoidPointer
      memmove(ids.get() + id_out, ids.get() + row.id_offset, row.id_length + 1u);
      row.id_offset = static_cast<uint32_t>(id_out);
      id_out += row.id_length + 1u;
      rows[out++] = row;
    }
    row_count = out;
    id_used = id_out;
    // Give memory back once the table is far below its capacity.
    if (row_capacity > kMinRows && row_count * 4 < row_capacity) {
      size_t next = power_of_two_at_least(row_count * 2 < kMinRows ? kMinRows : row_count * 2);
      if (base::ResizeStorage(rows, row_count * sizeof(Row), next * sizeof(Row))) {
        row_capacity = next;
      }
      size_t id_next = power_of_two_at_least(id_used * 2 < 4096 ? 4096 : id_used * 2);
      if (id_next < id_capacity) {
        if (base::ResizeStorage(ids, id_used, id_next)) {
          id_capacity = id_next;
        }
      }
    }
    if (!row_count) {
      index.reset();
      index = nullptr;
      index_slots = 0;
      return;
    }
    const size_t slots = power_of_two_at_least(row_count * 2 < 512 ? 512 : row_count * 2);
    if (!RebuildIndex(slots < index_slots ? slots : index_slots)) RebuildIndex(index_slots);
  }

  void PruneLocked(size_t max, int64_t now) {
    if (!row_count) return;
    auto keep_storage = base::AllocateStorage<bool[]>(row_count);
    auto* keep = keep_storage.get();
    if (!keep) return;
    size_t live = 0;
    for (size_t r = 0; r < row_count; ++r) {
      keep[r] = !cp::IsExpired(rows[r].state, now);
      if (keep[r]) ++live;
    }
    if (live > max) {
      auto order_storage = base::AllocateStorage<Ranked[]>(live * sizeof(Ranked));
      auto* order = order_storage.get();
      if (order) {
        size_t n = 0;
        for (size_t r = 0; r < row_count; ++r) {
          if (!keep[r]) continue;
          order[n++] = Ranked{cp::Score(rows[r].state, now), IdOf(rows[r]), rows[r].seq,
                              static_cast<uint32_t>(r), rows[r].id_length};
        }
        qsort(order, n, sizeof(Ranked), fortis::base::FatalCallback<prune_order>);
        for (size_t i = 0; i < n - max; ++i) keep[order[i].row] = false;
        order_storage.reset();
      }
    }
    Compact(keep);
    keep_storage.reset();
  }

  bool OverPruneThreshold() const {
    return static_cast<double>(row_count) > static_cast<double>(options.max_coins) * 1.2;
  }

  // ---- record -------------------------------------------------------------------
  void RecordOne(const char* raw, size_t raw_length, const char* device, size_t device_length,
                 int64_t now) {
    size_t begin = 0, end = 0;
    if (!codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(raw), raw_length)) {
      ++dropped;
      return;
    }
    codec::history_batch::JsTrim(raw, raw_length, &begin, &end);
    if (begin == end) return;  // id === ''
    char id[kCoinPopularityMaxIdBytes + 1];
    size_t length = 0;
    if (options.preserve_case && end - begin < sizeof(id)) {
      length = end - begin;
      memcpy(id, raw + begin, length);
      id[length] = '\0';
    } else if (options.preserve_case ||
               !compat::AsciiToLowerCase(raw + begin, end - begin, id, sizeof(id), &length) || !length) {
      ++dropped;
      return;
    }
    bool new_user = false;
    if (device && seen_count < seen_limit)
      new_user = SeenAdd(cp::PairHash(device, device_length, id, length));
    Row* row = Find(id, length);
    const bool existing = row != nullptr;
    if (!row) row = Insert(id, length);
    if (!row) {
      ++dropped;
      return;
    }
    row->state = cp::RecordRequest(existing ? &row->state : nullptr, now, new_user);
    row->flags |= kDirty;
  }
};

base::Result<std::unique_ptr<CoinPopularity, CoinPopularityDeleter>> CoinPopularity::Create(const CoinPopularityOptions& options) {
  auto storage = base::AllocateStorage<CoinPopularity>(sizeof(CoinPopularity));
  if (!storage) return base::Status(base::StatusCode::kResourceExhausted, "coin popularity tracker allocation");
  std::unique_ptr<CoinPopularity, CoinPopularityDeleter> self(new (storage.release()) CoinPopularity());
  self->impl_ = base::MakeUnique<Impl>();
  if (!self->impl_) return base::Status(base::StatusCode::kResourceExhausted, "coin popularity tracker allocation");
  Impl* impl = self->impl_.get();
  impl->options = options;
  if (!impl->options.max_coins) impl->options.max_coins = kCoinPopularityDefaultMaxCoins;
  // Prune triggers past max·1.2; one record call adds at most 100 more.
  impl->hard_capacity = static_cast<size_t>(static_cast<double>(impl->options.max_coins) * 1.2) +
                        kCoinPopularityMaxIdsPerRecord + 1;
  // The pair table keeps its load at or below 3/4 (a native bound).
  impl->seen_limit = options.max_seen_per_day;
  if (impl->seen_limit > kCoinPopularitySeenSlots / 4 * 3) impl->seen_limit = kCoinPopularitySeenSlots / 4 * 3;

  return self;
}

void CoinPopularity::Destroy(CoinPopularity* popularity) {
  if (!popularity) return;
  popularity->~CoinPopularity();
  base::StorageDeleter{}(popularity);
}

CoinPopularity::CoinPopularity() noexcept = default;
CoinPopularity::~CoinPopularity() = default;

int64_t CoinPopularity::Now() const { return impl_->Now(); }

void CoinPopularity::Record(const CoinIdView* ids, size_t count, const char* device_id) {
  if (!ids || !count) return;
  fortis::base::LockGuard lock(impl_->mutex);
  const int64_t now = impl_->Now();
  const int64_t day = floor_div(now, kDayMs);
  if (day != impl_->seen_day) {
    impl_->seen_day = day;
    impl_->ResetSeen();
  }
  const size_t device_length = device_id ? strlen(device_id) : 0;
  for (size_t i = 0; i < count; ++i)
    if (ids[i].data) impl_->RecordOne(ids[i].data, ids[i].size, device_id, device_length, now);
  if (impl_->OverPruneThreshold()) impl_->PruneLocked(impl_->options.max_coins, now);
}

void CoinPopularity::RecordPriceIds(const CoinIdView* raw_ids, size_t count, const char* device_id) {
  if (!raw_ids || !count) return;
  if (count > kCoinPopularityMaxIdsPerRecord) count = kCoinPopularityMaxIdsPerRecord;
  // The parsed source ids, de-duplicated in first-seen order (bySource).
  static constexpr size_t kIdBytes = 101;  // CoinGecko slugs are at most 100 bytes
  char unique[kCoinPopularityMaxIdsPerRecord][kIdBytes];
  CoinIdView views[kCoinPopularityMaxIdsPerRecord];
  size_t n = 0;
  providers::ParsedPriceId parsed;
  for (size_t i = 0; i < count; ++i) {
    if (!raw_ids[i].data || !ParsePriceId(raw_ids[i].data, raw_ids[i].size, &parsed)) continue;
    if (parsed.kind != providers::PriceIdKind::kCoinGecko) continue;
    const size_t length = strlen(parsed.source_id);
    if (!length || length >= kIdBytes) continue;
    bool seen = false;
    for (size_t k = 0; k < n && !seen; ++k)
      seen = views[k].size == length && memcmp(unique[k], parsed.source_id, length) == 0;
    if (seen) continue;
    memcpy(unique[n], parsed.source_id, length + 1);
    views[n] = CoinIdView{unique[n], length};
    ++n;
  }
  // `record([])` still rolls the day over and may prune, as TypeScript's does.
  fortis::base::LockGuard lock(impl_->mutex);
  const int64_t now = impl_->Now();
  const int64_t day = floor_div(now, kDayMs);
  if (day != impl_->seen_day) {
    impl_->seen_day = day;
    impl_->ResetSeen();
  }
  const size_t device_length = device_id ? strlen(device_id) : 0;
  for (size_t i = 0; i < n; ++i) impl_->RecordOne(views[i].data, views[i].size, device_id, device_length, now);
  if (impl_->OverPruneThreshold()) impl_->PruneLocked(impl_->options.max_coins, now);
}

bool CoinPopularity::Due(size_t budget, RankedCoinList* out, unsigned interval_divisor,
                         int64_t demand_max_age_ms) {
  *out = RankedCoinList{};
  fortis::base::UniqueLock lock(impl_->mutex);
  Impl& s = *impl_;
  const int64_t now = s.Now();
  bool ok = true;
  std::unique_ptr<Ranked[], base::StorageDeleter> order_storage;
  Ranked* order = nullptr;
  size_t live = 0;
  if (s.row_count) {
    order_storage = base::AllocateStorage<Ranked[]>(s.row_count * sizeof(Ranked));
    order = order_storage.get();
    ok = order != nullptr;
    for (size_t r = 0; ok && r < s.row_count; ++r) {
      if (cp::IsExpired(s.rows[r].state, now) || (demand_max_age_ms > 0 &&
          now - s.rows[r].state.last_requested_at_ms > demand_max_age_ms)) continue;
      order[live++] = Ranked{cp::Score(s.rows[r].state, now), s.IdOf(s.rows[r]), s.rows[r].seq,
                             static_cast<uint32_t>(r), s.rows[r].id_length};
    }
    if (ok) qsort(order, live, sizeof(Ranked), fortis::base::FatalCallback<rank_order>);
  }
  // Pick the due coins (most popular first) up to the budget.
  size_t picked = 0, text_bytes = 0;
  if (ok && live && budget) {
    out->items = base::AllocateStorage<RankedCoin[]>((budget < live ? budget : live) * sizeof(RankedCoin));
    ok = out->items != nullptr;
    for (size_t rank = 0; ok && rank < live && picked < budget; ++rank) {
      const Row& row = s.rows[order[rank].row];
      const uint32_t bucket = cp::BucketForRank(static_cast<int64_t>(rank));
      const int64_t interval = cp::IntervalForBucket(bucket);
      if ((row.flags & kRefreshed) && now - row.refreshed_at_ms < interval / (interval_divisor ? interval_divisor : 1)) continue;
      out->items[picked++] = RankedCoin{nullptr, row.id_length, rank, bucket, interval};
      order[picked - 1].row = order[rank].row;  // reuse the prefix for the copy below
      text_bytes += row.id_length + 1u;
    }
  }
  if (ok && picked) {
    out->text = base::AllocateStorage<char[]>(text_bytes);
    ok = out->text != nullptr;
    size_t used = 0;
    for (size_t i = 0; ok && i < picked; ++i) {
      const Row& row = s.rows[order[i].row];
      // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
      // cppcheck-suppress arithOperationsOnVoidPointer
      memcpy(out->text.get() + used, s.IdOf(row), row.id_length + 1u);
      // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
      // cppcheck-suppress arithOperationsOnVoidPointer
      out->items[i].id = out->text.get() + used;
      used += row.id_length + 1u;
    }
  }
  lock.Unlock();
  order_storage.reset();
  if (!ok) {
    out->Release();
    return false;
  }
  out->count = picked;
  return true;
}

void CoinPopularity::MarkRefreshed(const CoinIdView* ids, size_t count) {
  fortis::base::LockGuard lock(impl_->mutex);
  const int64_t now = impl_->Now();
  for (size_t i = 0; i < count; ++i) {
    Row* row = impl_->Find(ids[i].data, ids[i].size);
    if (!row) {
      impl_->PutGhost(ids[i].data, ids[i].size, now);
      continue;
    }
    row->refreshed_at_ms = now;
    row->flags |= kRefreshed;
  }
}

namespace {

template <typename Filter>
bool copy_rows(const CoinPopularity::Impl& s, PopularityRowList* out, Filter filter, bool clear_dirty) {
  *out = PopularityRowList{};
  size_t count = 0, text_bytes = 0;
  for (size_t r = 0; r < s.row_count; ++r)
    if (filter(s.rows[r])) { ++count; text_bytes += s.rows[r].id_length + 1u; }
  if (!count) return true;
  out->items = base::AllocateStorage<PopularityRow[]>(count * sizeof(PopularityRow));
  out->text = base::AllocateStorage<char[]>(text_bytes);
  if (!out->items || !out->text) {
    out->Release();
    return false;
  }
  size_t used = 0, n = 0;
  for (size_t r = 0; r < s.row_count; ++r) {
    Row& row = s.rows[r];
    if (!filter(row)) continue;
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    memcpy(out->text.get() + used, s.ids.get() + row.id_offset, row.id_length + 1u);
    out->items[n++] = PopularityRow{out->text.get() + used, row.id_length, row.state,
                                    (row.flags & kRefreshed) != 0, row.refreshed_at_ms};
    used += row.id_length + 1u;
    if (clear_dirty) row.flags = static_cast<uint8_t>(row.flags & ~kDirty);
  }
  out->count = n;
  return true;
}

}  // namespace

bool CoinPopularity::TakeDirty(PopularityRowList* out) {
  fortis::base::LockGuard lock(impl_->mutex);
  const bool ok = copy_rows(*impl_, out, [](const Row& row) { return (row.flags & kDirty) != 0; }, true);
  return ok;
}

void CoinPopularity::RequeueDirty(const PopularityRow* rows, size_t count) {
  if (!rows) return;
  fortis::base::LockGuard lock(impl_->mutex);
  for (size_t i = 0; i < count; ++i) {
    Row* row = impl_->Find(rows[i].id, rows[i].id_length);
    if (row) row->flags = static_cast<uint8_t>(row->flags | kDirty);
  }
}

bool CoinPopularity::Snapshot(PopularityRowList* out) const {
  fortis::base::LockGuard lock(impl_->mutex);
  const bool ok = copy_rows(*impl_, out, [](const Row&) { return true; }, false);
  return ok;
}

void CoinPopularity::Restore(const PopularityRow* rows, size_t count) {
  fortis::base::LockGuard lock(impl_->mutex);
  const int64_t now = impl_->Now();
  for (size_t i = 0; i < count; ++i) {
    const PopularityRow& in = rows[i];
    if (!in.id || !in.id_length || in.id_length > kCoinPopularityMaxIdBytes) continue;
    if (impl_->Find(in.id, in.id_length) || cp::IsExpired(in.state, now)) continue;
    if (impl_->row_count == impl_->hard_capacity) impl_->PruneLocked(impl_->options.max_coins, now);
    Row* row = impl_->Insert(in.id, in.id_length);
    if (!row) {
      ++impl_->dropped;
      continue;
    }
    row->state = in.state;
  }
}

void CoinPopularity::Prune(size_t max) {
  fortis::base::LockGuard lock(impl_->mutex);
  impl_->PruneLocked(max, impl_->Now());
}

void CoinPopularity::Prune() { Prune(impl_->options.max_coins); }

size_t CoinPopularity::Size() const {
  fortis::base::LockGuard lock(impl_->mutex);
  const size_t size = impl_->row_count;
  return size;
}

CoinPopularityStats CoinPopularity::Stats() const {
  fortis::base::LockGuard lock(impl_->mutex);
  CoinPopularityStats stats;
  stats.coins = impl_->row_count;
  stats.row_capacity = impl_->row_capacity;
  stats.id_bytes = impl_->id_used;
  stats.id_capacity = impl_->id_capacity;
  stats.index_slots = impl_->index_slots;
  stats.seen_pairs = impl_->seen_count;
  stats.seen_allocated = impl_->seen != nullptr;
  stats.dropped = impl_->dropped;
  stats.allocated_bytes = impl_->AllocatedBytes();
  stats.peak_allocated_bytes = impl_->peak_bytes;
  return stats;
}

// ---- durable store ------------------------------------------------------------

namespace coin_popularity_store {

namespace {

bool unreserved(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// decodeURIComponent of a key this store wrote.
bool decode_component(const char* text, char* output, size_t capacity, size_t* length) {
  size_t used = 0;
  for (size_t i = 0; text[i]; ++i) {
    if (used + 1 >= capacity) return false;
    if (text[i] != '%') { output[used++] = text[i]; continue; }
    const int high = hex_value(static_cast<unsigned char>(text[i + 1]));
    const int low = high < 0 ? -1 : hex_value(static_cast<unsigned char>(text[i + 2]));
    if (low < 0) return false;
    output[used++] = static_cast<char>(high << 4 | low);
    i += 2;
  }
  output[used] = '\0';
  *length = used;
  return true;
}

void put_u64(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t get_u64(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = value << 8 | in[i];
  return value;
}

}  // namespace

bool Key(const char* id, size_t id_length, char* output, size_t capacity) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  size_t used = sizeof(kPrefix) - 1;
  if (used >= capacity) return false;
  memcpy(output, kPrefix, used);
  for (size_t i = 0; i < id_length; ++i) {
    const unsigned char c = static_cast<unsigned char>(id[i]);
    if (unreserved(c)) {
      if (used + 1 >= capacity) return false;
      output[used++] = static_cast<char>(c);
    } else {
      if (used + 3 >= capacity) return false;
      output[used++] = '%';
      output[used++] = kHex[c >> 4];
      output[used++] = kHex[c & 15];
    }
  }
  output[used] = '\0';
  return used <= 1024;  // the KV key bound
}

bool Encode(const CoinPopularityState& state, uint8_t output[kValueBytes]) {
  output[0] = 'C';
  output[1] = 1;  // version
  uint64_t bits = 0;
  memcpy(&bits, &state.users, 8); put_u64(output + 2, bits);
  memcpy(&bits, &state.hits, 8); put_u64(output + 10, bits);
  put_u64(output + 18, static_cast<uint64_t>(state.updated_at_ms));
  put_u64(output + 26, static_cast<uint64_t>(state.last_requested_at_ms));
  return true;
}

bool Decode(const void* value, size_t size, CoinPopularityState* state) {
  const auto* in = static_cast<const uint8_t*>(value);
  if (size != kValueBytes || in[0] != 'C' || in[1] != 1) return false;
  uint64_t bits = get_u64(in + 2);
  memcpy(&state->users, &bits, 8);
  bits = get_u64(in + 10);
  memcpy(&state->hits, &bits, 8);
  state->updated_at_ms = static_cast<int64_t>(get_u64(in + 18));
  state->last_requested_at_ms = static_cast<int64_t>(get_u64(in + 26));
  return isfinite(state->users) && isfinite(state->hits);
}

SaveResult Save(storage::KvStore& kv, const PopularityRow* rows, size_t count) {
  SaveResult result;
  if (!count) return result;
  struct Frame {
    storage::KvMutation mutations[kSaveBatch];
    char keys[kSaveBatch][1025];
    uint8_t values[kSaveBatch][kValueBytes];
  };
  auto frame_storage = base::AllocateStorage<Frame>(sizeof(Frame));
  auto* frame = frame_storage.get();
  if (!frame) {
    result.failed = true;
    return result;
  }
  for (size_t offset = 0; offset < count; offset += kSaveBatch) {
    size_t n = 0;
    for (size_t i = offset; i < count && i < offset + kSaveBatch; ++i) {
      if (!Key(rows[i].id, rows[i].id_length, frame->keys[n], sizeof(frame->keys[n]))) {
        ++result.skipped;
        continue;
      }
      Encode(rows[i].state, frame->values[n]);
      frame->mutations[n] = storage::KvMutation{frame->keys[n], frame->values[n], static_cast<uint32_t>(kValueBytes),
                                       rows[i].state.last_requested_at_ms + kCoinPopularityExpireAfterMs + 1,
                                       false};
      result.bytes += strlen(frame->keys[n]) + kValueBytes;
      ++n;
    }
    if (!n) continue;
    if (!kv.Write(frame->mutations, n)) {
      result.failed = true;
      break;
    }
    result.rows += n;
    ++result.frames;
  }
  frame_storage.reset();
  return result;
}

namespace {

struct LoadPage {
  PopularityRow rows[kPageSize];
  size_t count = 0;
  char text[kPageSize * 1025];
  size_t used = 0;
  int64_t cutoff = 0;
  bool corrupt = false;
};

bool load_visit(const char* key, const void* value, size_t size, LoadPage* page) {
  CoinPopularityState state;
  if (!Decode(value, size, &state)) return true;  // an unreadable row is skipped
  if (state.last_requested_at_ms < page->cutoff) return true;
  const size_t prefix = sizeof(kPrefix) - 1;
  size_t length = 0;
  if (page->count == kPageSize ||
      !decode_component(key + prefix, page->text + page->used, sizeof(page->text) - page->used, &length))
    return true;
  page->rows[page->count++] = PopularityRow{page->text + page->used, length, state, false, 0};
  page->used += length + 1;
  return true;
}

}  // namespace

size_t LoadInto(storage::KvStore& kv, int64_t now_ms, CoinPopularity& popularity) {
  auto page_storage = base::AllocateStorage<LoadPage>(sizeof(LoadPage));
  auto* page = page_storage.get();
  if (!page) return SIZE_MAX;
  char after[1025] = "";
  size_t total = 0;
  for (;;) {
    page->count = 0;
    page->used = 0;
    page->cutoff = now_ms - kCoinPopularityExpireAfterMs;
    storage::KvPageResult result;
    if (!kv.Page(kPrefix, after[0] ? after : nullptr, kPageSize, now_ms, false, [&, visitor_state = page](const char* key, const void* value, size_t size) { return (load_visit)(key, value, size, visitor_state); }, &result)) {
      page_storage.reset();
      return SIZE_MAX;
    }
    popularity.Restore(page->rows, page->count);
    total += page->count;
    if (result.done || !result.next_cursor[0]) break;
    memcpy(after, result.next_cursor, sizeof(after));
  }
  page_storage.reset();
  return total;
}

}  // namespace coin_popularity_store

}  // namespace fortis::discovery
