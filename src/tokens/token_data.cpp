#include "fixed_string.h"
#include "token_data.h"

#include "kv_store.h"
#include "threading.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_data {
namespace {

using tj::Arena;
using tj::Value;

constexpr size_t kKeyBytes = 1025;
constexpr size_t kArenaBytes = 4 * 1024 * 1024;

int64_t block_start(int64_t ts, int64_t span) {
  int64_t start = ts / span * span;
  if (ts < 0 && start != ts) start -= span;
  return start;
}

int64_t candle_span(Interval interval) {
  switch (interval) {
    case Interval::k1h: return 4LL * 86400000;
    case Interval::k1d: return 32LL * 86400000;
    case Interval::k1w: return 182LL * 86400000;
  }
  return 86400000;
}
int64_t candle_keep(Interval interval) {
  switch (interval) {
    case Interval::k1h: return 92LL * 86400000;
    case Interval::k1d: return 367LL * 86400000;
    case Interval::k1w: return 371LL * 86400000;
  }
  return 86400000;
}

bool key_of(char* out, const char* bucket, const char* network, const char* token_id,
            const char* tail = nullptr) {
  size_t used = 0;
  const int n = snprintf(out, kKeyBytes, "token/%s/", bucket);
  if (n <= 0 || static_cast<size_t>(n) >= kKeyBytes) return false;
  used = static_cast<size_t>(n);
  if (network) {
    if (!AppendComponent(out, kKeyBytes, &used, network) || used + 1 >= kKeyBytes) return false;
    out[used++] = '/'; out[used] = '\0';
  }
  if (token_id) {
    if (!AppendComponent(out, kKeyBytes, &used, token_id)) return false;
  }
  if (tail) {
    const size_t t = strlen(tail);
    if (used + t >= kKeyBytes) return false;
    memcpy(out + used, tail, t + 1);
  }
  return true;
}

void pad13(char out[16], int64_t value) {
  char formatted[32];
  snprintf(formatted, sizeof(formatted), "%013lld", static_cast<long long>(value < 0 ? 0 : value));
  // Retain the historical 15-byte key width for existing persisted records.
  base::CopyCString(out, 16, formatted);
}

Value* num_or_null(Arena& a, bool has, double v) { return has ? tj::Num(a, v) : tj::Null(a); }
bool read_num(const Value* v, bool* has, double* out) {
  *has = tj::IsNumber(v);
  *out = *has ? v->number : 0;
  return true;
}

// ---- positional encodings -------------------------------------------------

Value* encode_observation(Arena& a, const Observation& o) {
  Value* row = tj::Arr(a);
  if (!row) return nullptr;
  Value* items[] = {
      tj::Num(a, static_cast<double>(o.ts)), tj::Num(a, o.ok ? 1 : 2), tj::Str(a, o.source),
      num_or_null(a, o.has_sampling, o.sampling_seconds), num_or_null(a, o.has_liquidity, o.liquidity),
      num_or_null(a, o.has_price, o.price), num_or_null(a, o.has_spread, o.spread),
      num_or_null(a, o.has_spread_pools, o.spread_pools), tj::Num(a, o.pool_count),
      num_or_null(a, o.has_top_share, o.top_share), num_or_null(a, o.has_fdv, o.fdv),
      num_or_null(a, o.has_market_cap, o.market_cap), num_or_null(a, o.has_volume_h24, o.volume_h24),
      num_or_null(a, o.has_buys_h24, o.buys_h24), num_or_null(a, o.has_sells_h24, o.sells_h24),
      tj::Num(a, static_cast<double>(o.expires_at)),
  };
  for (Value* item : items) if (!tj::Push(a, row, item)) return nullptr;
  return row;
}

bool decode_observation(const Value* row, Observation* o) {
  if (!tj::IsArray(row) || row->count != 16 || !tj::IsNumber(row->items[0]) ||
      !tj::IsNumber(row->items[1]) || !tj::IsString(row->items[2]) || !tj::IsNumber(row->items[8]) ||
      !tj::IsNumber(row->items[15]))
    return false;
  *o = Observation{};
  o->ts = static_cast<int64_t>(row->items[0]->number);
  o->ok = row->items[1]->number == 1;
  const size_t n = row->items[2]->length < sizeof(o->source) - 1 ? row->items[2]->length : sizeof(o->source) - 1;
  memcpy(o->source, row->items[2]->text, n);
  read_num(row->items[3], &o->has_sampling, &o->sampling_seconds);
  read_num(row->items[4], &o->has_liquidity, &o->liquidity);
  read_num(row->items[5], &o->has_price, &o->price);
  read_num(row->items[6], &o->has_spread, &o->spread);
  read_num(row->items[7], &o->has_spread_pools, &o->spread_pools);
  o->pool_count = row->items[8]->number;
  read_num(row->items[9], &o->has_top_share, &o->top_share);
  read_num(row->items[10], &o->has_fdv, &o->fdv);
  read_num(row->items[11], &o->has_market_cap, &o->market_cap);
  read_num(row->items[12], &o->has_volume_h24, &o->volume_h24);
  read_num(row->items[13], &o->has_buys_h24, &o->buys_h24);
  read_num(row->items[14], &o->has_sells_h24, &o->sells_h24);
  o->expires_at = static_cast<int64_t>(row->items[15]->number);
  return true;
}

Value* encode_candle(Arena& a, const Candle& c) {
  Value* row = tj::Arr(a);
  if (!row || !tj::Push(a, row, tj::Num(a, static_cast<double>(c.ts)))) return nullptr;
  for (size_t i = 0; i < Candle::kFields; ++i)
    if (!tj::Push(a, row, num_or_null(a, c.has[i], c.value[i]))) return nullptr;
  if (!tj::Push(a, row, tj::Num(a, c.sample_count)) || !tj::Push(a, row, tj::Num(a, c.expected_samples)) ||
      !tj::Push(a, row, tj::Str(a, c.source))) return nullptr;
  return row;
}

bool decode_candle(const Value* row, Candle* c) {
  if (!tj::IsArray(row) || row->count != Candle::kFields + 4 || !tj::IsNumber(row->items[0])) return false;
  *c = Candle{};
  c->ts = static_cast<int64_t>(row->items[0]->number);
  for (size_t i = 0; i < Candle::kFields; ++i) read_num(row->items[1 + i], &c->has[i], &c->value[i]);
  const Value* samples = row->items[Candle::kFields + 1];
  const Value* expected = row->items[Candle::kFields + 2];
  const Value* source = row->items[Candle::kFields + 3];
  if (!tj::IsNumber(samples) || !tj::IsNumber(expected) || !tj::IsString(source)) return false;
  c->sample_count = samples->number;
  c->expected_samples = expected->number;
  const size_t n = source->length < sizeof(c->source) - 1 ? source->length : sizeof(c->source) - 1;
  memcpy(c->source, source->text, n);
  return true;
}

uint32_t fnv(const char* a, const char* b) {
  uint32_t h = 2166136261u;
  for (const char* p = a; *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
  h ^= 0; h *= 16777619u;
  for (const char* p = b; *p; ++p) { h ^= static_cast<uint8_t>(*p); h *= 16777619u; }
  return h;
}

bool string_equal(const Value* v, const char* text) { return tj::StringEquals(v, text); }

struct ScanCollect {
  Arena* arena;
  Value* blocks;  // array of parsed block values
  int64_t stop_after_start;  // stop once a block start exceeds this (or INT64_MAX)
  size_t prefix_length;
  bool failed;
  bool stopped = false;  // a block past stop_after_start ended the scan
};

// Raw observation blocks parsed per scan page (6-hour blocks: four days).
constexpr size_t kObservationPage = 16;
// Candle blocks parsed per scan page (1h: 4-day blocks, 1d: 32, 1w: 182).
constexpr size_t kCandlePage = 4;

int64_t key_tail_number(const char* key) {
  const char* slash = strrchr(key, '/');
  return slash ? strtoll(slash + 1, nullptr, 10) : 0;
}

bool collect_visit(const char* key, const void* value, size_t size, ScanCollect* state) {
  const int64_t start = key_tail_number(key);
  if (start > state->stop_after_start) { state->stopped = true; return false; }
  Value* parsed = tj::Parse(*state->arena, static_cast<const uint8_t*>(value), size);
  if (!parsed) { if (!state->arena->ok) { state->failed = true; return false; } return true; }
  Value* entry = tj::Arr(*state->arena);
  if (!entry || !tj::Push(*state->arena, entry, tj::Num(*state->arena, static_cast<double>(start))) ||
      !tj::Push(*state->arena, entry, parsed) || !tj::Push(*state->arena, state->blocks, entry)) {
    state->failed = true;
    return false;
  }
  return true;
}

}  // namespace

const char* IntervalName(Interval interval) {
  switch (interval) {
    case Interval::k1h: return "1h";
    case Interval::k1d: return "1d";
    case Interval::k1w: return "1w";
  }
  return "1h";
}

bool AppendComponent(char* out, size_t capacity, size_t* used, const char* text) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    const bool keep = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                      strchr("-_.!~*'()", *c) != nullptr;
    if (keep) {
      if (*used + 1 >= capacity) return false;
      out[(*used)++] = static_cast<char>(*c);
    } else {
      if (*used + 3 >= capacity) return false;
      out[(*used)++] = '%';
      out[(*used)++] = kHex[*c >> 4];
      out[(*used)++] = kHex[*c & 15];
    }
  }
  out[*used] = '\0';
  return true;
}

TokenDataStore::TokenDataStore(storage::KvStore* kv)
    : kv_(kv), mutex_(), scratch_(base::AllocateStorage<uint8_t[]>(kMaxDocumentBytes)),
      output_(base::AllocateStorage<uint8_t[]>(kMaxDocumentBytes)),
      arena_memory_(base::AllocateStorage<uint8_t[]>(kArenaBytes)) {
  if (!scratch_ || !output_ || !arena_memory_) {
    scratch_.reset(); output_.reset(); arena_memory_.reset();
  }
}

TokenDataStore::~TokenDataStore() {
  scratch_.reset(); output_.reset(); arena_memory_.reset();

}

DataOutcome TokenDataStore::ReadDocument(const char* key, int64_t now_ms, Arena& arena, Value** document,
                                    const storage::KvBatch* batch) {
  *document = nullptr;
  size_t size = 0;
  const storage::KvReadStatus status = batch ? batch->Get(kv_, key, now_ms, scratch_.get(), kMaxDocumentBytes, &size)
                                    : kv_->GetStatus(key, now_ms, scratch_.get(), kMaxDocumentBytes, &size);
  if (status == storage::KvReadStatus::Missing) return DataOutcome::kMissing;
  if (status == storage::KvReadStatus::BufferTooSmall) return DataOutcome::kTooLarge;
  if (status != storage::KvReadStatus::Found) return DataOutcome::kIoError;
  *document = tj::Parse(arena, scratch_.get(), size);
  if (!*document) return arena.ok ? DataOutcome::kCorrupt : DataOutcome::kTooLarge;
  return DataOutcome::kOk;
}

bool TokenDataStore::WriteDocument(const char* key, const Value* document, int64_t expires_at_ms) {
  tj::OutBuffer out{output_.get(), kMaxDocumentBytes, 0, true};
  if (!tj::Stringify(document, out) || !out.ok) return false;
  const storage::KvMutation mutation{key, output_.get(), static_cast<uint32_t>(out.used), expires_at_ms, false};
  return kv_->Write(&mutation, 1);
}

bool TokenDataStore::StageDocument(storage::KvBatch& batch, const char* key, const Value* document,
                                   int64_t expires_at_ms) {
  tj::OutBuffer out{output_.get(), kMaxDocumentBytes, 0, true};
  return tj::Stringify(document, out) && out.ok && batch.Put(key, output_.get(), out.used, expires_at_ms);
}

bool TokenDataStore::PreparePatchLocked(storage::KvBatch& batch, const char* bucket, const char* network,
                                        const char* token_id, const Value* fields, int64_t now_ms) {
  char key[kKeyBytes];
  if (!valid() || !kv_ || !tj::IsObject(fields) || !key_of(key, bucket, network, token_id)) return false;
  Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
  Value* prior = nullptr;
  const DataOutcome status = ReadDocument(key, now_ms, arena, &prior, &batch);
  bool ok = status == DataOutcome::kOk || status == DataOutcome::kMissing;
  Value* next = ok ? tj::Obj(arena) : nullptr;
  if (ok && next && tj::IsObject(prior))
    for (uint32_t i = 0; i < prior->count; ++i)
      ok = ok && tj::Set(arena, next, prior->members[i].key, prior->members[i].key_length, prior->members[i].value);
  for (uint32_t i = 0; ok && next && i < fields->count; ++i)
    ok = tj::Set(arena, next, fields->members[i].key, fields->members[i].key_length,
                 tj::Clone(arena, fields->members[i].value));
  return ok && next && tj::Set(arena, next, "network", tj::Str(arena, network)) &&
         tj::Set(arena, next, "tokenId", tj::Str(arena, token_id)) && StageDocument(batch, key, next, 0);
}

bool TokenDataStore::PatchDocument(const char* bucket, const char* network, const char* token_id,
                                   const Value* fields, int64_t now_ms) {
  if (!valid() || !kv_) return false;
  storage::KvBatch batch(kSingleDocumentFrameBytes, 1);
  Lock();
  const bool ok = PreparePatchLocked(batch, bucket, network, token_id, fields, now_ms) && batch.Commit(kv_);
  Unlock();
  return ok;
}

void TokenDataStore::Lock() { mutex_.Lock(); }
void TokenDataStore::Unlock() { mutex_.Unlock(); }

bool TokenDataStore::PrepareFacts(storage::KvBatch& batch, const char* network, const char* token_id, const Value* fields,
                                  int64_t now_ms) {
  return PreparePatchLocked(batch, "facts", network, token_id, fields, now_ms);
}

bool TokenDataStore::PrepareDemand(storage::KvBatch& batch, const char* network, const char* token_id, const Value* fields,
                                   int64_t now_ms) {
  return PreparePatchLocked(batch, "demand", network, token_id, fields, now_ms);
}

DataOutcome TokenDataStore::ReadFacts(const char* network, const char* token_id, int64_t now_ms, Arena& arena,
                                 Value** document) {
  char key[kKeyBytes];
  *document = nullptr;
  if (!valid() || !key_of(key, "facts", network, token_id)) return DataOutcome::kMissing;
  fortis::base::LockGuard lock(mutex_);
  const DataOutcome status = ReadDocument(key, now_ms, arena, document);
  return status;
}

bool TokenDataStore::PatchFacts(const char* network, const char* token_id, const Value* fields, int64_t now_ms) {
  return PatchDocument("facts", network, token_id, fields, now_ms);
}

DataOutcome TokenDataStore::ReadDemand(const char* network, const char* token_id, int64_t now_ms, Arena& arena,
                                  Value** document) {
  char key[kKeyBytes];
  *document = nullptr;
  if (!valid() || !key_of(key, "demand", network, token_id)) return DataOutcome::kMissing;
  fortis::base::LockGuard lock(mutex_);
  const DataOutcome status = ReadDocument(key, now_ms, arena, document);
  return status;
}

bool TokenDataStore::PatchDemand(const char* network, const char* token_id, const Value* fields, int64_t now_ms) {
  return PatchDocument("demand", network, token_id, fields, now_ms);
}

bool TokenDataStore::PatchDemandMany(const char* const* networks, const char* const* token_ids, size_t count,
                                     const Value* fields, int64_t now_ms, size_t* frames) {
  if (frames) *frames = 0;
  if (!valid() || !kv_ || !tj::IsObject(fields) || count > kDemandPatchBatch || (count && (!networks || !token_ids)))
    return false;
  if (!count) return true;
  auto keys_storage = base::AllocateStorage<char[][kKeyBytes]>(count * kKeyBytes);
  auto* keys = keys_storage.get();
  if (!keys) return false;
  storage::KvMutation mutations[kDemandPatchBatch];
  size_t pending = 0, used = 0;
  bool ok = true;
  fortis::base::UniqueLock lock(mutex_);
  Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
  auto flush = [&]() {
    if (!pending) return true;
    const bool written = kv_->Write(mutations, pending);
    if (written && frames) ++*frames;
    pending = 0;
    used = 0;
    return written;
  };
  for (size_t i = 0; ok && i < count; ++i) {
    if (!key_of(keys[i], "demand", networks[i], token_ids[i])) { ok = false; break; }
    const size_t mark = arena.used;
    Value* prior = nullptr;
    const DataOutcome status = ReadDocument(keys[i], now_ms, arena, &prior);
    ok = status == DataOutcome::kOk || status == DataOutcome::kMissing;
    Value* next = ok ? tj::Obj(arena) : nullptr;
    if (ok && next && tj::IsObject(prior))
      for (uint32_t m = 0; m < prior->count; ++m)
        ok = ok && tj::Set(arena, next, prior->members[m].key, prior->members[m].key_length, prior->members[m].value);
    for (uint32_t m = 0; ok && next && m < fields->count; ++m)
      ok = tj::Set(arena, next, fields->members[m].key, fields->members[m].key_length,
                   tj::Clone(arena, fields->members[m].value));
    ok = ok && next && tj::Set(arena, next, "network", tj::Str(arena, networks[i])) &&
         tj::Set(arena, next, "tokenId", tj::Str(arena, token_ids[i]));
    if (!ok) break;
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    tj::OutBuffer out{output_.get() + used, kMaxDocumentBytes - used, 0, true};
    if (!tj::Stringify(next, out) || !out.ok) {
      // The rest of the shared buffer is too small: commit and retry alone.
      if (!pending) { ok = false; break; }
      ok = flush();
      out = tj::OutBuffer{output_.get(), kMaxDocumentBytes, 0, true};
      ok = ok && tj::Stringify(next, out) && out.ok;
      if (!ok) break;
    }
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    mutations[pending++] = storage::KvMutation{keys[i], output_.get() + used, static_cast<uint32_t>(out.used), 0, false};
    used += out.used;
    arena.used = mark;
    arena.ok = true;
  }
  if (ok) ok = flush();
  lock.Unlock();
  keys_storage.reset();
  return ok;
}

namespace {
struct DemandScan {
  Arena* arena;
  TokenDataStore::DemandVisitor visitor;
  void* context;
  bool stopped;
};
bool demand_visit(const char*, const void* value, size_t size, DemandScan* state) {
  const size_t mark = state->arena->used;
  Value* row = tj::Parse(*state->arena, static_cast<const uint8_t*>(value), size);
  bool keep = true;
  if (row) keep = state->visitor(state->context, row);
  state->arena->used = mark;
  state->arena->ok = true;
  if (!keep) state->stopped = true;
  return keep;
}
}  // namespace

bool TokenDataStore::ForEachDemand(int64_t now_ms, Arena& arena, DemandVisitor visitor, void* context) {
  if (!valid() || !visitor) return false;
  DemandScan state{&arena, visitor, context, false};
  const bool ok = kv_->Scan("token/demand/", nullptr, static_cast<size_t>(-1) / 2, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (demand_visit)(key, value, size, visitor_state); });
  return ok || state.stopped;
}

namespace {
struct StaleCollect {
  char (*keys)[kKeyBytes];
  size_t capacity;
  size_t count;
  int64_t at_ms;
  Arena* arena;
};
bool stale_visit(const char* key, const void* value, size_t size, StaleCollect* state) {
  const size_t mark = state->arena->used;
  Value* row = tj::Parse(*state->arena, static_cast<const uint8_t*>(value), size);
  const Value* computed = tj::Get(row, "computedAt");
  const bool stale = row && (!tj::IsNumber(computed) || static_cast<int64_t>(computed->number) != state->at_ms);
  state->arena->used = mark;
  state->arena->ok = true;
  if (stale && state->count < state->capacity) {
    snprintf(state->keys[state->count++], kKeyBytes, "%s", key);
  }
  return state->count < state->capacity;
}
}  // namespace

bool TokenDataStore::ReplaceDemand(Value* const* rows, size_t count, int64_t at_ms, int64_t now_ms) {
  if (!valid()) return false;
  bool ok = true;
  for (size_t i = 0; i < count && ok; ++i) {
    const Value* network = tj::Get(rows[i], "network");
    const Value* token = tj::Get(rows[i], "tokenId");
    if (!tj::IsString(network) || !tj::IsString(token)) continue;
    char key[kKeyBytes];
    char net[256], id[1100];
    if (network->length >= sizeof(net) || token->length >= sizeof(id)) continue;
    memcpy(net, network->text, network->length); net[network->length] = '\0';
    memcpy(id, token->text, token->length); id[token->length] = '\0';
    if (!key_of(key, "demand", net, id)) continue;
    fortis::base::LockGuard lock(mutex_);
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    Value* prior = nullptr;
    ReadDocument(key, now_ms, arena, &prior);
    Value* next = tj::Obj(arena);
    const Value* refreshed = tj::Get(prior, "lastRefreshedAt");
    ok = next != nullptr;
    if (ok && refreshed && refreshed->type != tj::Type::kNull)
      ok = tj::Set(arena, next, "lastRefreshedAt", tj::Clone(arena, refreshed));
    for (uint32_t m = 0; ok && m < rows[i]->count; ++m)
      ok = tj::Set(arena, next, rows[i]->members[m].key, rows[i]->members[m].key_length,
                   tj::Clone(arena, rows[i]->members[m].value));
    ok = ok && WriteDocument(key, next, 0);
  }
  // Delete rows from older projections in bounded passes.
  static constexpr size_t kBatch = 128;
  auto keys_storage = base::AllocateStorage<char[][kKeyBytes]>(kBatch * kKeyBytes);
  auto* keys = keys_storage.get();
  if (!keys) return false;
  for (int pass = 0; ok && pass < 1024; ++pass) {
    fortis::base::UniqueLock lock(mutex_);
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    StaleCollect state{keys, kBatch, 0, at_ms, &arena};
    kv_->Scan("token/demand/", nullptr, static_cast<size_t>(-1) / 2, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (stale_visit)(key, value, size, visitor_state); });
    storage::KvMutation mutations[kBatch];
    for (size_t i = 0; i < state.count; ++i) mutations[i] = storage::KvMutation{keys[i], nullptr, 0, 0, true};
    if (state.count) ok = kv_->Write(mutations, state.count);
    lock.Unlock();
    if (state.count < kBatch) break;
  }
  keys_storage.reset();
  return ok;
}

// ---- observations -------------------------------------------------------------

bool TokenDataStore::PrepareObservation(storage::KvBatch& batch, const char* network, const char* token_id,
                                        const Observation& row, int64_t now_ms) {
  if (!valid()) return false;
  const int64_t start = block_start(row.ts, kRawBlockMs);
  char tail[32], key[kKeyBytes], pair_key[kKeyBytes];
  char pad[16];
  pad13(pad, start);
  snprintf(tail, sizeof(tail), "/%s", pad);
  if (!key_of(key, "liqraw", network, token_id, tail) || !key_of(pair_key, "liqpair", network, token_id))
    return false;
  Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
  Value* block = nullptr;
  const DataOutcome status = ReadDocument(key, now_ms, arena, &block, &batch);
  bool ok = status == DataOutcome::kOk || status == DataOutcome::kMissing;
  if (ok && !tj::IsArray(block)) block = tj::Arr(arena);
  Observation merged = row;
  // putInTransaction: the row lives seven days from max(arrival, ts).
  merged.expires_at = (now_ms > row.ts ? now_ms : row.ts) + kRawRetentionMs;
  int64_t block_expires = merged.expires_at;
  Value* next = ok ? tj::Arr(arena) : nullptr;
  bool placed = false;
  for (uint32_t i = 0; ok && next && i < block->count; ++i) {
    Observation existing{};
    if (!decode_observation(block->items[i], &existing)) continue;
    if (existing.expires_at <= now_ms) continue;  // already expired: drop it
    if (existing.ts != row.ts && existing.expires_at > block_expires) block_expires = existing.expires_at;
    if (existing.ts == row.ts) {
      if (!merged.has_sampling && existing.has_sampling) {
        merged.has_sampling = true;
        merged.sampling_seconds = existing.sampling_seconds;
      }
      continue;
    }
    if (!placed && existing.ts > row.ts) {
      ok = tj::Push(arena, next, encode_observation(arena, merged));
      placed = true;
    }
    ok = ok && tj::Push(arena, next, block->items[i]);
  }
  if (ok && next && !placed) ok = tj::Push(arena, next, encode_observation(arena, merged));
  ok = ok && next && StageDocument(batch, key, next, block_expires);
  if (!ok) return false;
  // The pair row shares the frame (putInTransaction writes both in one tx).
  Value* pair = nullptr;
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  Arena pair_arena{arena_memory_.get() + arena.used, kArenaBytes - arena.used, 0, true};
  const DataOutcome pair_status = ReadDocument(pair_key, now_ms, pair_arena, &pair, &batch);
  if (pair_status != DataOutcome::kOk && pair_status != DataOutcome::kMissing) return false;
  const Value* last = tj::Get(pair, "lastTs");
  const int64_t last_ts = tj::IsNumber(last) && static_cast<int64_t>(last->number) > row.ts
                              ? static_cast<int64_t>(last->number) : row.ts;
  Value* doc = tj::Obj(pair_arena);
  return doc && tj::Set(pair_arena, doc, "network", tj::Str(pair_arena, network)) &&
         tj::Set(pair_arena, doc, "tokenId", tj::Str(pair_arena, token_id)) &&
         tj::Set(pair_arena, doc, "lastTs", tj::Num(pair_arena, static_cast<double>(last_ts))) &&
         StageDocument(batch, pair_key, doc, (now_ms > last_ts ? now_ms : last_ts) + kRawRetentionMs);
}

bool TokenDataStore::PutObservation(const char* network, const char* token_id, const Observation& row,
                                    int64_t now_ms) {
  if (!valid()) return false;
  storage::KvBatch batch(kObservationFrameBytes, 2);
  Lock();
  const bool ok = PrepareObservation(batch, network, token_id, row, now_ms) && batch.Commit(kv_);
  Unlock();
  return ok;
}

DataOutcome TokenDataStore::Observations(const char* network, const char* token_id, int64_t from_ms, int64_t to_ms,
                                    int64_t now_ms, Rows<Observation>* rows) {
  rows->count = 0;
  rows->truncated = false;
  char prefix[kKeyBytes], after[kKeyBytes];
  if (!valid() || !key_of(prefix, "liqraw", network, token_id, "/")) return DataOutcome::kOk;
  const int64_t first = block_start(from_ms < 0 ? 0 : from_ms, kRawBlockMs);
  char pad[16];
  pad13(pad, first - 1);
  base::ConcatCString(after, sizeof(after), {prefix, pad});
  fortis::base::LockGuard lock(mutex_);
  // Pages of kObservationPage blocks parsed into the store arena, until a
  // block starts after `to_ms` or the token has no more blocks: a backfilled
  // row lives seven days from its arrival, so a range can cover any number of
  // blocks (TypeScript pages its rows without a bound).
  DataOutcome result = DataOutcome::kOk;
  for (bool more = true; more && result == DataOutcome::kOk && !rows->truncated;) {
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    Value* blocks = tj::Arr(arena);
    ScanCollect state{&arena, blocks, to_ms, strlen(prefix), false};
    if (!blocks || !kv_->Scan(prefix, after, kObservationPage, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (collect_visit)(key, value, size, visitor_state); })) {
      if (!blocks || state.failed) result = DataOutcome::kTooLarge;
      else if (!state.stopped) result = DataOutcome::kIoError;
    }
    if (state.failed) result = DataOutcome::kTooLarge;
    for (uint32_t b = 0; blocks && b < blocks->count && result == DataOutcome::kOk && !rows->truncated; ++b) {
      const Value* block = blocks->items[b]->items[1];
      if (!tj::IsArray(block)) continue;
      for (uint32_t i = 0; i < block->count; ++i) {
        Observation o{};
        if (!decode_observation(block->items[i], &o)) continue;
        if (o.ts < from_ms || o.ts > to_ms) continue;
        if (o.expires_at <= now_ms) continue;  // the row's own expiry
        if (rows->count >= rows->capacity) { rows->truncated = true; break; }
        rows->items[rows->count++] = o;
      }
    }
    more = blocks && !state.stopped && blocks->count == kObservationPage;
    if (more) {
      pad13(pad, static_cast<int64_t>(blocks->items[blocks->count - 1]->items[0]->number));
      base::ConcatCString(after, sizeof(after), {prefix, pad});
    }
  }
  return result;
}

DataOutcome TokenDataStore::LatestObservation(const char* network, const char* token_id, int64_t before_ms,
                                         int64_t now_ms, bool (*match)(const Observation&), Observation* out,
                                         bool* found) {
  *found = false;
  char prefix[kKeyBytes], after[kKeyBytes];
  if (!valid() || before_ms < 0 || !key_of(prefix, "liqraw", network, token_id, "/")) return DataOutcome::kOk;
  char pad[16];
  pad13(pad, block_start(before_ms, kRawBlockMs) + 1);
  base::ConcatCString(after, sizeof(after), {prefix, pad});
  fortis::base::LockGuard lock(mutex_);
  // Newest block first (keys below `after`, descending); the first block
  // holding a matching row holds the newest one (rows of a block are not kept
  // in time order, so the block is searched whole).
  DataOutcome result = DataOutcome::kOk;
  for (bool more = true; more && result == DataOutcome::kOk && !*found;) {
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    Value* blocks = tj::Arr(arena);
    ScanCollect state{&arena, blocks, INT64_MAX, strlen(prefix), false};
    if (!blocks || !kv_->Scan(prefix, after, kObservationPage, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (collect_visit)(key, value, size, visitor_state); }, true))
      result = !blocks || state.failed ? DataOutcome::kTooLarge : DataOutcome::kIoError;
    for (uint32_t b = 0; blocks && b < blocks->count && result == DataOutcome::kOk && !*found; ++b) {
      const Value* block = blocks->items[b]->items[1];
      if (!tj::IsArray(block)) continue;
      for (uint32_t i = 0; i < block->count; ++i) {
        Observation o{};
        if (!decode_observation(block->items[i], &o)) continue;
        if (o.ts > before_ms || o.expires_at <= now_ms || !match(o)) continue;
        if (!*found || o.ts > out->ts) *out = o;
        *found = true;
      }
    }
    more = blocks && blocks->count == kObservationPage;
    if (more) {
      pad13(pad, static_cast<int64_t>(blocks->items[blocks->count - 1]->items[0]->number));
      base::ConcatCString(after, sizeof(after), {prefix, pad});
    }
  }
  return result;
}

namespace {
struct PairCollect {
  Arena* arena;
  int64_t since;
  Rows<TokenDataStore::Pair>* pairs;
};
bool pair_visit(const char*, const void* value, size_t size, PairCollect* state) {
  const size_t mark = state->arena->used;
  Value* doc = tj::Parse(*state->arena, static_cast<const uint8_t*>(value), size);
  const Value* last = tj::Get(doc, "lastTs");
  const Value* network = tj::Get(doc, "network");
  const Value* token = tj::Get(doc, "tokenId");
  if (tj::IsNumber(last) && last->number >= static_cast<double>(state->since) && tj::IsString(network) &&
      tj::IsString(token) && network->length < sizeof(TokenDataStore::Pair::network) &&
      token->length < sizeof(TokenDataStore::Pair::token_id)) {
    if (state->pairs->count >= state->pairs->capacity) { state->pairs->truncated = true; }
    else {
      TokenDataStore::Pair& p = state->pairs->items[state->pairs->count++];
      memcpy(p.network, network->text, network->length); p.network[network->length] = '\0';
      memcpy(p.token_id, token->text, token->length); p.token_id[token->length] = '\0';
    }
  }
  state->arena->used = mark;
  state->arena->ok = true;
  return true;
}
}  // namespace

DataOutcome TokenDataStore::RecentPairs(int64_t since_ms, int64_t now_ms, Rows<Pair>* pairs) {
  pairs->count = 0;
  pairs->truncated = false;
  if (!valid()) return DataOutcome::kIoError;
  fortis::base::LockGuard lock(mutex_);
  Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
  PairCollect state{&arena, since_ms, pairs};
  const bool ok = kv_->Scan("token/liqpair/", nullptr, static_cast<size_t>(-1) / 2, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (pair_visit)(key, value, size, visitor_state); });
  return ok ? DataOutcome::kOk : DataOutcome::kIoError;
}

// ---- candles -------------------------------------------------------------------

bool TokenDataStore::PutCandles(const char* network, const char* token_id, Interval interval,
                                const Candle* candles, size_t count, int64_t now_ms, size_t* changed) {
  if (changed) *changed = 0;
  if (!valid()) return false;
  const int64_t span = candle_span(interval);
  bool ok = true;
  size_t i = 0;
  while (i < count && ok) {
    const int64_t start = block_start(candles[i].ts, span);
    size_t end = i;
    while (end < count && block_start(candles[end].ts, span) == start) ++end;
    char tail[40], key[kKeyBytes], pad[16];
    pad13(pad, start);
    snprintf(tail, sizeof(tail), "/%s/%s", IntervalName(interval), pad);
    if (!key_of(key, "liqcandle", network, token_id, tail)) return false;
    fortis::base::UniqueLock lock(mutex_);
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    Value* block = nullptr;
    const DataOutcome status = ReadDocument(key, now_ms, arena, &block);
    ok = status == DataOutcome::kOk || status == DataOutcome::kMissing;
    if (ok && !tj::IsArray(block)) block = tj::Arr(arena);
    size_t differing = 0;
    Value* next = ok ? tj::Arr(arena) : nullptr;
    // merge: existing rows not replaced, plus every incoming candle, by ts
    size_t incoming = i;
    uint32_t existing = 0;
    while (ok && next && (incoming < end || existing < block->count)) {
      Candle current{};
      const bool have_existing = existing < block->count && decode_candle(block->items[existing], &current);
      if (existing < block->count && !have_existing) { ++existing; continue; }
      if (incoming < end && (!have_existing || candles[incoming].ts <= current.ts)) {
        Value* encoded = encode_candle(arena, candles[incoming]);
        if (!encoded) { ok = false; break; }
        bool same = false;
        if (have_existing && candles[incoming].ts == current.ts) {
          tj::OutBuffer x{nullptr, 0, 0, true}, y{nullptr, 0, 0, true};
          // Compare serialized forms: identical candles skip the write.
          uint8_t bx[512], by[512];
          x = tj::OutBuffer{bx, sizeof(bx), 0, true};
          y = tj::OutBuffer{by, sizeof(by), 0, true};
          tj::Stringify(encoded, x);
          tj::Stringify(block->items[existing], y);
          same = x.ok && y.ok && x.used == y.used && memcmp(bx, by, x.used) == 0;
          ++existing;
        }
        if (!same) ++differing;
        ok = tj::Push(arena, next, encoded);
        ++incoming;
      } else {
        ok = tj::Push(arena, next, block->items[existing]);
        ++existing;
      }
    }
    if (ok && next && differing) {
      const int64_t expires = start + span + candle_keep(interval);
      ok = WriteDocument(key, next, expires > now_ms + 7LL * 86400000 ? expires : now_ms + 7LL * 86400000);
    }
    if (changed) *changed += differing;
    lock.Unlock();
    i = end;
  }
  return ok;
}

DataOutcome TokenDataStore::Candles(const char* network, const char* token_id, Interval interval, int64_t from_ms,
                               int64_t to_ms, int64_t now_ms, Rows<Candle>* rows) {
  rows->count = 0;
  rows->truncated = false;
  char tail[16], prefix[kKeyBytes], after[kKeyBytes];
  snprintf(tail, sizeof(tail), "/%s/", IntervalName(interval));
  if (!valid() || !key_of(prefix, "liqcandle", network, token_id, tail)) return DataOutcome::kOk;
  const int64_t span = candle_span(interval);
  const int64_t first = block_start(from_ms < 0 ? 0 : from_ms, span);
  char pad[16];
  pad13(pad, first - 1);
  base::ConcatCString(after, sizeof(after), {prefix, pad});
  fortis::base::LockGuard lock(mutex_);
  // Pages of kCandlePage blocks parsed into the store arena (a full block of
  // 96 hourly candles parses to about 0.25 MiB, so 92 days of hourly candles
  // do not fit the arena at once), until a block starts after `to_ms`.
  DataOutcome result = DataOutcome::kOk;
  const int64_t keep = candle_keep(interval);
  for (bool more = true; more && result == DataOutcome::kOk && !rows->truncated;) {
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    Value* blocks = tj::Arr(arena);
    ScanCollect state{&arena, blocks, to_ms, strlen(prefix), false};
    if (!blocks || !kv_->Scan(prefix, after, kCandlePage, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (collect_visit)(key, value, size, visitor_state); })) {
      if (!blocks || state.failed) result = DataOutcome::kTooLarge;
      else if (!state.stopped) result = DataOutcome::kIoError;
    }
    if (state.failed) result = DataOutcome::kTooLarge;
    for (uint32_t b = 0; blocks && b < blocks->count && result == DataOutcome::kOk && !rows->truncated; ++b) {
      const Value* block = blocks->items[b]->items[1];
      if (!tj::IsArray(block)) continue;
      for (uint32_t i = 0; i < block->count; ++i) {
        Candle c{};
        if (!decode_candle(block->items[i], &c)) continue;
        if (c.ts < from_ms || c.ts > to_ms) continue;
        if (c.ts + keep <= now_ms) continue;
        if (rows->count >= rows->capacity) { rows->truncated = true; break; }
        rows->items[rows->count++] = c;
      }
    }
    more = blocks && !state.stopped && blocks->count == kCandlePage;
    if (more) {
      pad13(pad, static_cast<int64_t>(blocks->items[blocks->count - 1]->items[0]->number));
      base::ConcatCString(after, sizeof(after), {prefix, pad});
    }
  }
  return result;
}

// ---- holder snapshots ----------------------------------------------------------

DataOutcome TokenDataStore::HolderRows(const char* network, const char* token_id, int64_t now_ms, Arena& arena,
                                  Value** rows) {
  *rows = nullptr;
  char prefix[kKeyBytes];
  if (!valid() || !key_of(prefix, "holder", network, token_id, "/")) return DataOutcome::kOk;
  Value* out = tj::Arr(arena);
  if (!out) return DataOutcome::kTooLarge;
  fortis::base::UniqueLock lock(mutex_);
  Value* blocks = tj::Arr(arena);
  ScanCollect state{&arena, blocks, INT64_MAX, strlen(prefix), false};
  if (blocks) kv_->Scan(prefix, nullptr, 4096, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (collect_visit)(key, value, size, visitor_state); });
  lock.Unlock();
  if (!blocks || state.failed) return DataOutcome::kTooLarge;
  for (uint32_t b = 0; b < blocks->count; ++b) {
    const Value* block = blocks->items[b]->items[1];
    if (!tj::IsArray(block)) continue;
    for (uint32_t i = 0; i < block->count; ++i) {
      const Value* ts = tj::Get(block->items[i], "ts");
      if (!tj::IsNumber(ts) || ts->number + static_cast<double>(kHolderRetainMs) <= static_cast<double>(now_ms))
        continue;
      if (!tj::Push(arena, out, block->items[i])) return DataOutcome::kTooLarge;
    }
  }
  *rows = out;
  return DataOutcome::kOk;
}

bool TokenDataStore::PutHolderRow(const char* network, const char* token_id, const char* source, int64_t day_ms,
                                  const Value* row, int64_t now_ms) {
  if (!valid()) return false;
  const int64_t start = block_start(day_ms, kHolderBlockMs);
  char tail[600], key[kKeyBytes], pad[16];
  size_t used = 1;
  tail[0] = '/';
  tail[1] = '\0';
  if (!AppendComponent(tail, sizeof(tail), &used, source)) return false;
  pad13(pad, start);
  if (used + 15 >= sizeof(tail)) return false;
  snprintf(tail + used, sizeof(tail) - used, "/%s", pad);
  if (!key_of(key, "holder", network, token_id, tail)) return false;
  fortis::base::LockGuard lock(mutex_);
  Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
  Value* block = nullptr;
  const DataOutcome status = ReadDocument(key, now_ms, arena, &block);
  bool ok = status == DataOutcome::kOk || status == DataOutcome::kMissing;
  if (ok && !tj::IsArray(block)) block = tj::Arr(arena);
  Value* next = ok ? tj::Arr(arena) : nullptr;
  bool placed = false;
  for (uint32_t i = 0; ok && next && i < block->count; ++i) {
    const Value* ts = tj::Get(block->items[i], "ts");
    const double t = tj::IsNumber(ts) ? ts->number : -1;
    if (t == static_cast<double>(day_ms)) continue;
    if (!placed && t > static_cast<double>(day_ms)) { ok = tj::Push(arena, next, tj::Clone(arena, row)); placed = true; }
    ok = ok && tj::Push(arena, next, block->items[i]);
  }
  if (ok && next && !placed) ok = tj::Push(arena, next, tj::Clone(arena, row));
  ok = ok && next && WriteDocument(key, next, start + kHolderBlockMs + kHolderRetainMs);
  return ok;
}

DataOutcome TokenDataStore::HolderRow(const char* network, const char* token_id, const char* source, int64_t day_ms,
                                 int64_t now_ms, Arena& arena, Value** row) {
  *row = nullptr;
  Value* rows = nullptr;
  const DataOutcome status = HolderRows(network, token_id, now_ms, arena, &rows);
  if (status != DataOutcome::kOk || !rows) return status;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const Value* ts = tj::Get(rows->items[i], "ts");
    if (tj::IsNumber(ts) && ts->number == static_cast<double>(day_ms) &&
        string_equal(tj::Get(rows->items[i], "source"), source)) {
      *row = rows->items[i];
      return DataOutcome::kOk;
    }
  }
  return DataOutcome::kMissing;
}

// ---- fetch stats -----------------------------------------------------------------

namespace {
double counter(const Value* row, const char* key) {
  const Value* v = tj::Get(row, key);
  return tj::IsNumber(v) ? v->number : 0;
}
}  // namespace

bool TokenDataStore::UpsertFetchRows(Value* const* rows, size_t count, int64_t now_ms, size_t* dropped) {
  if (dropped) *dropped = 0;
  if (!valid()) return false;
  bool ok = true;
  // One block write per distinct (hour, shard) among the rows.
  auto done_storage = base::AllocateZeroedStorage<bool[]>(count ? count : 1, sizeof(bool));
  auto* done = done_storage.get();
  if (!done) return false;
  static const char* kCounters[] = {"requests", "freshHits", "staleHits", "floorHits", "upstreamSuccesses",
                                    "upstreamFailures", "negativeResults", "latencyMs"};
  for (size_t i = 0; i < count && ok; ++i) {
    if (done[i]) continue;
    const Value* bucket = tj::Get(rows[i], "bucket");
    const Value* network = tj::Get(rows[i], "network");
    const Value* token = tj::Get(rows[i], "tokenId");
    if (!tj::IsNumber(bucket) || !tj::IsString(network) || !tj::IsString(token)) { done[i] = true; continue; }
    char net[256], id[1100];
    if (network->length >= sizeof(net) || token->length >= sizeof(id)) { done[i] = true; continue; }
    memcpy(net, network->text, network->length); net[network->length] = '\0';
    memcpy(id, token->text, token->length); id[token->length] = '\0';
    const int64_t hour = static_cast<int64_t>(bucket->number);
    const uint32_t shard = fnv(net, id) % kFetchShards;
    char key[kKeyBytes], pad[16];
    pad13(pad, hour);
    snprintf(key, sizeof(key), "token/fetch/%s/%u", pad, shard);
    fortis::base::LockGuard lock(mutex_);
    Arena arena{arena_memory_.get(), kArenaBytes, 0, true};
    Value* block = nullptr;
    const DataOutcome status = ReadDocument(key, now_ms, arena, &block);
    ok = status == DataOutcome::kOk || status == DataOutcome::kMissing;
    if (ok && !tj::IsArray(block)) block = tj::Arr(arena);
    for (size_t j = i; j < count && ok; ++j) {
      if (done[j]) continue;
      const Value* b = tj::Get(rows[j], "bucket");
      const Value* n = tj::Get(rows[j], "network");
      const Value* t = tj::Get(rows[j], "tokenId");
      if (!tj::IsNumber(b) || static_cast<int64_t>(b->number) != hour || !tj::IsString(n) || !tj::IsString(t) ||
          n->length >= sizeof(net) || t->length >= sizeof(id)) continue;
      char jn[256], jt[1100];
      memcpy(jn, n->text, n->length); jn[n->length] = '\0';
      memcpy(jt, t->text, t->length); jt[t->length] = '\0';
      if (fnv(jn, jt) % kFetchShards != shard) continue;
      done[j] = true;
      const Value* facet = tj::Get(rows[j], "facet");
      Value* existing = nullptr;
      for (uint32_t k = 0; k < block->count && !existing; ++k) {
        Value* candidate = block->items[k];
        if (tj::StringEquals(tj::Get(candidate, "network"), jn) && tj::StringEquals(tj::Get(candidate, "tokenId"), jt) &&
            tj::IsString(facet) && tj::Get(candidate, "facet") && tj::IsString(tj::Get(candidate, "facet")) &&
            tj::Get(candidate, "facet")->length == facet->length &&
            memcmp(tj::Get(candidate, "facet")->text, facet->text, facet->length) == 0) existing = candidate;
      }
      if (!existing) {
        if (block->count >= kFetchRowsPerShard) { if (dropped) ++*dropped; continue; }
        ok = tj::Push(arena, block, tj::Clone(arena, rows[j]));
        continue;
      }
      // mergeBuckets(old, new): new's fields, summed counters, sourceId
      // new ?? old, lastRequestedAt the later.
      Value* merged = tj::Clone(arena, rows[j]);
      if (!merged) { ok = false; break; }
      for (const char* c : kCounters)
        ok = ok && tj::Set(arena, merged, c, tj::Num(arena, counter(existing, c) + counter(rows[j], c)));
      const Value* source = tj::Get(rows[j], "sourceId");
      if ((!source || source->type == tj::Type::kNull) && tj::Get(existing, "sourceId"))
        ok = ok && tj::Set(arena, merged, "sourceId", tj::Clone(arena, tj::Get(existing, "sourceId")));
      const Value* left = tj::Get(existing, "lastRequestedAt");
      const Value* right = tj::Get(rows[j], "lastRequestedAt");
      Value* later = nullptr;
      if (!tj::IsNumber(left)) later = right ? tj::Clone(arena, right) : tj::Null(arena);
      else if (!tj::IsNumber(right)) later = tj::Clone(arena, left);
      else later = tj::Num(arena, left->number > right->number ? left->number : right->number);
      ok = ok && tj::Set(arena, merged, "lastRequestedAt", later);
      for (uint32_t k = 0; k < block->count; ++k) if (block->items[k] == existing) block->items[k] = merged;
    }
    ok = ok && WriteDocument(key, block, now_ms + kFetchRetentionMs);
  }
  done_storage.reset();
  return ok;
}

namespace {
struct FetchScan {
  Arena* arena;
  int64_t from, to;
  TokenDataStore::FetchVisitor visitor;
  void* context;
  bool stopped;
};
bool fetch_visit(const char* key, const void* value, size_t size, FetchScan* state) {
  // key: token/fetch/{hour13}/{shard}
  const char* hour_text = key + strlen("token/fetch/");
  const int64_t hour = strtoll(hour_text, nullptr, 10);
  if (hour > state->to) { state->stopped = true; return false; }
  if (hour < state->from) return true;
  const size_t mark = state->arena->used;
  Value* block = tj::Parse(*state->arena, static_cast<const uint8_t*>(value), size);
  bool keep = true;
  for (uint32_t i = 0; tj::IsArray(block) && i < block->count && keep; ++i)
    keep = state->visitor(state->context, block->items[i]);
  state->arena->used = mark;
  state->arena->ok = true;
  if (!keep) state->stopped = true;
  return keep;
}
}  // namespace

bool TokenDataStore::ForEachFetchRow(int64_t from_ms, int64_t to_ms, int64_t now_ms, Arena& arena,
                                     FetchVisitor visitor, void* context) {
  if (!valid() || !visitor) return false;
  char after[64], pad[16];
  pad13(pad, from_ms - 1);
  snprintf(after, sizeof(after), "token/fetch/%s", pad);
  FetchScan state{&arena, from_ms, to_ms, visitor, context, false};
  const bool ok = kv_->Scan("token/fetch/", after, static_cast<size_t>(-1) / 2, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (fetch_visit)(key, value, size, visitor_state); });
  return ok || state.stopped;
}

bool TokenDataStore::Erase(const char* key) {
  if (!kv_) return false;
  const storage::KvMutation mutation{key, nullptr, 0, 0, true};
  return kv_->Write(&mutation, 1);
}

}  // namespace fortis::tokens::token_data
