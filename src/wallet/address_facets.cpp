#include <string_view>
#include "exception_boundary.h"
#include "utf8.h"
#include "address_facets_internal.h"
#include "ownership.h"

#include "kv_store.h"
#include "media_registry.h"
#include "spot_provider_owner.h"
#include "threading.h"
#include "provider_text.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace tj = fortis::codec::tj;

namespace fortis::wallet::address {
namespace {

constexpr int64_t kNftScanMaxAgeMs = 10 * 60000;
constexpr int64_t kNftErrorCooldownMs = 60000;
constexpr int64_t kNftInventoryTtlMs = 7LL * 86400000;
constexpr int64_t kTxPollWindowMs = 15 * 60000;
constexpr int64_t kTxDetailTtlMs = 7LL * 86400000;
constexpr int64_t kSettledRecheckMs = 2 * 60000;
constexpr uint64_t kFlightWaitMs = 30000;

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

const char* str(const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  return tj::IsString(v) ? v->text : nullptr;
}

timespec wake_at(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

bool key_text(char* out, size_t capacity, const char* format, ...) __attribute__((format(printf, 3, 4)));
bool key_text(char* out, size_t capacity, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(out, capacity, format, args);
  va_end(args);
  return n >= 0 && static_cast<size_t>(n) < capacity;
}

// encodeURIComponent of one component into a static-size buffer.
bool enc(const char* text, char* out, size_t capacity) { return Store::Component(text, out, capacity); }

// The next UTF-16 code unit of a UTF-8 string (`pending` holds a low
// surrogate still owed); an invalid byte stands for itself. 0 at the end.
uint32_t next_utf16_unit(const unsigned char*& p, uint32_t& pending) {
  if (pending) { const uint32_t unit = pending; pending = 0; return unit; }
  const unsigned char c = *p;
  if (!c) return 0;
  int extra = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : 0;
  uint32_t cp = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : extra == 1 ? c & 0x1F : c;
  for (int i = 1; i <= extra; ++i) {
    if ((p[i] & 0xC0) != 0x80) { extra = 0; cp = c; break; }
    cp = cp << 6 | (p[i] & 0x3F);
  }
  p += 1 + extra;
  if (cp < 0x10000) return cp;
  cp -= 0x10000;
  pending = 0xDC00 + (cp & 0x3FF);
  return 0xD800 + (cp >> 10);
}

// Array.prototype.sort's default order: UTF-16 code units, so an astral
// character (a surrogate pair) sorts before U+E000..U+FFFF.
int compare_utf16(const void* a, const void* b) {
  const unsigned char* x = reinterpret_cast<const unsigned char*>(*static_cast<const char* const*>(a));
  const unsigned char* y = reinterpret_cast<const unsigned char*>(*static_cast<const char* const*>(b));
  uint32_t px = 0, py = 0;
  for (;;) {
    const uint32_t ux = next_utf16_unit(x, px);
    const uint32_t uy = next_utf16_unit(y, py);
    if (ux != uy) return ux < uy ? -1 : 1;
    if (!ux) return 0;
  }
}

}  // namespace

// ---- shared helpers ------------------------------------------------------------

uint64_t KeyHash(const char* key) {
  uint64_t h = 1469598103934665603ULL;
  for (const char* p = key; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
  return h ? h : 1;
}

Value* CacheJson(Arena& arena, bool hit, int64_t timestamp_ms, bool has_timestamp) {
  Value* cache = tj::Obj(arena);
  if (!cache || !tj::Set(arena, cache, "hit", tj::Bool(arena, hit)) ||
      !tj::Set(arena, cache, "timestamp", hit && has_timestamp ? chains::IsoValue(arena, timestamp_ms) : tj::Null(arena)))
    return nullptr;
  return cache;
}

Value* SwrProvenance(Arena& arena, const SwrResult& result) {
  return CacheJson(arena, result.source != SwrSource::kLive, result.as_of_ms, true);
}

void SetError(FacetReply* result, const chains::Error& error) {
  switch (error.kind) {
    case chains::ErrorKind::kRange:
      result->outcome = Outcome::kBadRequest;
      snprintf(result->detail, sizeof(result->detail), "%s", error.message);
      return;
    case chains::ErrorKind::kLaneBusy:
      result->outcome = Outcome::kLaneBusy;
      result->retry_after_ms = error.retry_after_ms;
      return;
    default:
      result->outcome = Outcome::kUpstream;
      return;
  }
}

const char* Canonical(Arena& arena, const Network& network, const char* address) {
  if (network.family != chains::Family::kEvm) return address;
  // String.prototype.toLowerCase (any non-ASCII character maps as in JS).
  Value* folded = fortis::codec::provider_text::ToLowerCase(arena, address, strlen(address));
  if (!folded) return nullptr;
  return folded->text;
}

bool IsUtxoFamily(chains::Family family) {
  return family == chains::Family::kBitcoin || family == chains::Family::kBitcoinCash || family == chains::Family::kDash ||
         family == chains::Family::kDogecoin || family == chains::Family::kLitecoin || family == chains::Family::kZcash;
}

void FreeRevalidation(Revalidation* job) {
  if (!job) return;
  fortis::base::Delete(job);
}

static Revalidation* new_revalidation(Revalidation::Kind kind, const char* key) {
  auto* job = fortis::base::New<Revalidation>();
  if (!job) return nullptr;
  job->kind = kind;
  job->key.Assign(key);
  return job;
}

bool SplitTrackedTokens(const char* value, size_t length, std::unique_ptr<char*[], base::CStringArrayDeleter>& items, size_t* count) {
  *count = 0;
  size_t segments = 1;
  for (size_t i = 0; i < length; ++i)
    if (value[i] == ',') ++segments;
  items = base::AllocateCStringArray(segments);
  if (!items) return false;
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i < length && value[i] != ',') continue;
    size_t b = 0, e = 0;
    fortis::codec::provider_text::Trim(value + start, i - start, &b, &e);
    if (e > b) {
      auto item = base::AllocateStorage<char[]>(e - b + 1);
      if (!item) return false;
      memcpy(item.get(), value + start + b, e - b);
      item[e - b] = '\0';
      items[(*count)++] = item.release();
    }
    start = i + 1;
  }
  return true;
}

bool BalancesCacheKey(const char* network, const char* address, const char* const* tokens, size_t count,
                      char* output, size_t capacity) {
  // [...tokens].sort().join(',') -> sha1 hex [:12].
  auto sorted_storage = count ? base::AllocateStorage<const char*[]>(sizeof(char*) * count) : nullptr;
  auto* sorted = sorted_storage.get();
  if (count && !sorted) return false;
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    sorted[i] = tokens[i];
    total += strlen(tokens[i]) + 1;
  }
  if (count > 1) qsort(reinterpret_cast<void*>(sorted), count, sizeof(char*), fortis::base::FatalCallback<compare_utf16>);
  auto joined_storage = base::AllocateStorage<uint8_t[]>(total + 1);
  auto* joined = joined_storage.get();
  if (!joined) { sorted_storage.reset(); return false; }
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    if (i) joined[used++] = ',';
    const size_t n = strlen(sorted[i]);
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    memcpy(joined + used, sorted[i], n);
    used += n;
  }
  uint8_t digest[20];
  chains::Sha1(joined, used, digest);
  joined_storage.reset();
  sorted_storage.reset();
  char hex[41];
  chains::HexEncode(digest, sizeof(digest), hex);
  hex[12] = '\0';
  return key_text(output, capacity, "bal:%s:%s:%s", network, address, hex);
}

// ---- Impl: flights, context, SWR -------------------------------------------------

bool WalletQueryContext::BeginFlight(const char* key, size_t* slot, bool* waited, uint64_t* generation) {
  *waited = false;
  const uint64_t hash = KeyHash(key);
  const bool storable = strlen(key) < kFlightKeyBytes;
  uint64_t counted = 0;  // the flight this caller already counted itself on
  fortis::base::UniqueLock flight_lock(flight_mutex);
  for (;;) {
    size_t found = kFlights, free_slot = kFlights;
    for (size_t i = 0; i < kFlights; ++i) {
      if (!flights[i].used) { if (free_slot == kFlights) free_slot = i; continue; }
      if (flights[i].hash == hash && (!storable || strcmp(flights[i].key, key) == 0)) { found = i; break; }
    }
    if (found == kFlights) {
      if (*waited || free_slot == kFlights || !storable) {
        // After a wait, or with no slot: proceed without leading a flight.
        flight_lock.Unlock();
        *slot = kFlights;
        return *waited ? false : true;
      }
      flights[free_slot].used = true;
      flights[free_slot].hash = hash;
      flights[free_slot].generation = ++flight_generation;
      flights[free_slot].waiters = 0;
      snprintf(flights[free_slot].key, sizeof(flights[free_slot].key), "%s", key);
      flight_lock.Unlock();
      *slot = free_slot;
      return true;
    }
    if (flights[found].generation != counted) {
      counted = flights[found].generation;
      ++flights[found].waiters;
      if (generation) *generation = counted;
    }
    *waited = true;
    const timespec deadline = wake_at(kFlightWaitMs);
    if (flight_done.WaitUntilLocked(flight_mutex, deadline) != 0) {
      flight_lock.Unlock();
      *slot = kFlights;
      return false;
    }
  }
}

void WalletQueryContext::EndFlight(size_t slot, const Value* share) {
  if (slot >= kFlights) return;
  fortis::base::LockGuard flight_lock(flight_mutex);
  if (share && flights[slot].waiters) {
    // Serialized under the lock: a follower may still be joining.
    auto text = base::AllocateStorage<uint8_t[]>(kSwrScratchBytes);
    tj::OutBuffer out{text.get(), kSwrScratchBytes, 0, true};
    if (text && tj::Stringify(share, out) && out.ok) {
      SharedLive& entry = shared_live[shared_live_next];
      shared_live_next = (shared_live_next + 1) % kSharedLive;
      entry.text.reset();
      entry = SharedLive{flights[slot].generation, flights[slot].waiters, std::move(text), out.used};
    } else {
      text.reset();
    }
  }
  flights[slot].used = false;
  flights[slot].key[0] = '\0';
  flights[slot].waiters = 0;
  flight_done.Broadcast();
}

bool WalletQueryContext::TakeShared(uint64_t generation, Arena& arena, Value** value) {
  if (!generation) return false;
  fortis::base::LockGuard flight_lock(flight_mutex);
  bool found = false;
  for (SharedLive& entry : shared_live) {
    if (entry.generation != generation || !entry.text) continue;
    *value = tj::Parse(arena, entry.text.get(), entry.length);
    found = *value != nullptr;
    if (--entry.remaining == 0) {
      entry.text.reset();
      entry = SharedLive{};
    }
    break;
  }
  return found;
}

namespace {
const char* context_selector(void* opaque, const char* selector, Arena& arena) {
  return static_cast<WalletQueryContext*>(opaque)->MethodSignature(selector, arena);
}
}  // namespace

chains::Context WalletQueryContext::ChainContext(Arena& arena) {
  chains::Context ctx;
  ctx.transport = deps.transport;
  ctx.arena = &arena;
  ctx.config = deps.config;
  ctx.clock = &clock;
  ctx.resolve_selector = context_selector;
  ctx.selector_context = this;
  return ctx;
}

WalletQueryContext::NetworkResolution WalletQueryContext::Resolve(const char* id) const {
  Network network;
  if (deps.catalog->Resolve(id, strlen(id), &network)) return network;
  FacetReply failure;
  failure.outcome = Outcome::kBadRequest;
  snprintf(failure.detail, sizeof(failure.detail), "unknown network: %s", id);
  return failure;
}

bool WalletQueryContext::Enqueue(Revalidation* job) {
  if (!job) return false;
  if (!deps.enqueue || !deps.enqueue(deps.enqueue_context, job)) {
    FreeRevalidation(job);
    return false;
  }
  return true;
}

static int hold_status(const chains::Error& error, bool unknown) {
  if (unknown) return 404;
  switch (error.kind) {
    case chains::ErrorKind::kLaneBusy: return 429;
    case chains::ErrorKind::kRange: return 400;
    default: return 502;
  }
}

void WalletQueryContext::HoldRange(const char* key, const char* message) {
  if (strlen(key) >= kFlightKeyBytes) return;
  fortis::base::LockGuard flight_lock(flight_mutex);
  HeldRange* slot = nullptr;
  for (HeldRange& held : held_ranges)
    if (!strcmp(held.key, key)) slot = &held;
  if (!slot) {
    slot = &held_ranges[held_range_next];
    held_range_next = (held_range_next + 1) % kHeldRanges;
  }
  snprintf(slot->key, sizeof(slot->key), "%s", key);
  snprintf(slot->message, sizeof(slot->message), "%s", message);
}

bool WalletQueryContext::HeldRangeMessage(const char* key, chains::Error* error) {
  fortis::base::LockGuard flight_lock(flight_mutex);
  bool found = false;
  for (const HeldRange& held : held_ranges) {
    if (!held.key[0] || strcmp(held.key, key) != 0) continue;
    error->Range("%s", held.message);
    found = true;
    break;
  }
  return found;
}

bool WalletQueryContext::SwrRead(const char* key, const tokens::token_swr::Ttl& ttl, const SwrFetcher& fetcher, Arena& arena,
                           SwrResult* out) {
  *out = SwrResult{};
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kSwrScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) {
    out->error.Upstream("out of memory");
    return false;
  }
  const int64_t now = Now();
  Value* value = nullptr;
  int64_t as_of = 0;
  tokens::token_swr::State state = deps.swr->Read(key, now, scratch, kSwrScratchBytes, arena, &value, &as_of);
  if (state == tokens::token_swr::State::kFresh) {
    scratch_storage.reset();
    *out = SwrResult{true, value, as_of, false, SwrSource::kCache, {}, false};
    return true;
  }
  if (state == tokens::token_swr::State::kStale) {
    if (fetcher.revalidation && deps.swr->BeginRevalidate(key, now)) {
      Revalidation* job = fetcher.revalidation(fetcher.context, key);
      if (!Enqueue(job)) deps.swr->EndRevalidate(key, now, false, false);
    }
    scratch_storage.reset();
    *out = SwrResult{true, value, as_of, true, SwrSource::kCache, {}, false};
    return true;
  }
  // A miss that just failed: the floor, else the replayed failure.
  auto replay = [&](int status) -> bool {
    if (fetcher.floor) {
      Value* floored = nullptr;
      int64_t floor_as_of = 0;
      if (fetcher.floor(this, fetcher.context, arena, &floored, &floor_as_of)) {
        *out = SwrResult{true, floored, floor_as_of, true, SwrSource::kFloor, {}, false};
        return true;
      }
    }
    if (status == 400) {
      // A RangeError: TypeScript replays the same error object. Its message
      // is held beside the hold; once evicted, the fetcher's validation
      // throws it again (without an upstream request when it precedes
      // them).
      chains::Error error;
      if (HeldRangeMessage(key, &error)) {
        out->error = error;
        return false;
      }
      Value* ignored = nullptr;
      if (!fetcher.fetch(this, fetcher.context, arena, &ignored, &error) && error.kind == chains::ErrorKind::kRange) {
        out->error = error;
        return false;
      }
      out->error.Upstream("held failure");
      return false;
    }
    if (status == 404) {
      out->unknown = true;
      out->error.Upstream("unknown");
      return false;
    }
    if (status == 429) out->error.LaneBusy(0);
    else out->error.Upstream("held failure");
    return false;
  };
  int held = 0;
  if (deps.swr->MissHeld(key, now, &held)) {
    scratch_storage.reset();
    return replay(held);
  }
  size_t slot = kFlights;
  bool waited = false;
  uint64_t generation = 0;
  const bool leader = BeginFlight(key, &slot, &waited, &generation);
  if (!leader) {
    // Coalesced: the leader's live answer, as its stored envelope.
    const int64_t at = Now();
    state = deps.swr->Read(key, at, scratch, kSwrScratchBytes, arena, &value, &as_of);
    if (state != tokens::token_swr::State::kMissing) {
      scratch_storage.reset();
      *out = SwrResult{true, value, as_of, false, SwrSource::kLive, {}, false};
      return true;
    }
    if (deps.swr->MissHeld(key, at, &held)) {
      scratch_storage.reset();
      return replay(held);
    }
    // The leader's live value that was not stored (a pending view).
    if (waited && TakeShared(generation, arena, &value)) {
      scratch_storage.reset();
      *out = SwrResult{true, value, at, false, SwrSource::kLive, {}, false};
      return true;
    }
  }
  chains::Error error;
  Value* fetched = nullptr;
  const bool ok = fetcher.fetch(this, fetcher.context, arena, &fetched, &error);
  bool result = false;
  const Value* share = nullptr;
  if (ok) {
    const int64_t at = Now();
    if (fetcher.should_store && !fetcher.should_store(this, fetcher.context, fetched)) {
      share = fetched;
      *out = SwrResult{true, fetched, at, false, SwrSource::kLive, {}, false};
    } else {
      int64_t stored_at = at;
      deps.swr->Write(key, ttl, at, fetched, scratch, kSwrScratchBytes, &stored_at);
      *out = SwrResult{true, fetched, stored_at, false, SwrSource::kLive, {}, false};
    }
    result = true;
  } else {
    const bool unknown = strcmp(error.message, kUnknownMarker) == 0;
    deps.swr->HoldMiss(key, now, error.kind == chains::ErrorKind::kLaneBusy, hold_status(error, unknown));
    if (error.kind == chains::ErrorKind::kRange && !unknown) HoldRange(key, error.message);
    result = false;
    out->error = error;
    out->unknown = unknown;
    if (fetcher.floor) {
      Value* floored = nullptr;
      int64_t floor_as_of = 0;
      if (fetcher.floor(this, fetcher.context, arena, &floored, &floor_as_of)) {
        *out = SwrResult{true, floored, floor_as_of, true, SwrSource::kFloor, {}, false};
        result = true;
      }
    }
  }
  if (leader) EndFlight(slot, share);
  scratch_storage.reset();
  return result;
}

void WalletQueryContext::Revalidate(const char* key, const tokens::token_swr::Ttl& ttl, const SwrFetcher& fetcher, Arena& arena) {
  chains::Error error;
  Value* value = nullptr;
  if (!fetcher.fetch(this, fetcher.context, arena, &value, &error)) {
    deps.swr->EndRevalidate(key, Now(), true, error.kind == chains::ErrorKind::kLaneBusy);
    return;
  }
  if (!fetcher.should_store || fetcher.should_store(this, fetcher.context, value)) {
    auto scratch_storage = base::AllocateStorage<uint8_t[]>(kSwrScratchBytes);
  auto* scratch = scratch_storage.get();
    if (scratch) {
      int64_t as_of = 0;
      deps.swr->Write(key, ttl, Now(), value, scratch, kSwrScratchBytes, &as_of);
      scratch_storage.reset();
    }
  }
  deps.swr->EndRevalidate(key, Now(), false, false);
}

Value* WalletValuation::SpotPrices(Arena& arena, const char* const* ids, size_t count) {
  return deps_.spot && count ? deps_.spot->Read(arena, ids, count) : codec::tj::Obj(arena);
}

// ---- balances ----------------------------------------------------------------------

namespace {

Value* balance_entry(Arena& arena, const char* key, const Value* t, const Value* prices,
                     const char* const* price_keys, const char* const* price_ids, size_t price_count) {
  const char* id = nullptr;
  for (size_t i = 0; i < price_count; ++i)
    if (!strcmp(price_keys[i], key)) { id = price_ids[i]; break; }
  const Value* quote = id ? tj::Get(prices, id) : nullptr;
  const bool priced = tj::IsNumber(quote);
  const Value* display = tj::Get(t, "displayUnits");
  double units = 0;
  if (display) {
    units = chains::JsNumber(display);
  } else {
    const Value* raw = tj::Get(t, "balanceRaw");
    const double number = tj::IsString(raw) ? fortis::codec::provider_text::ToNumber(raw->text, raw->length) : chains::JsNumber(raw);
    const double decimals = chains::JsNumber(tj::Get(t, "decimals"));
    units = number / (decimals >= 0 && decimals <= 22 && trunc(decimals) == decimals ? pow(10, decimals)
                                                                                     : pow(10, decimals));
  }
  Value* entry = tj::Obj(arena);
  const Value* token_id = tj::Get(t, "tokenId");
  auto copy = [&](const char* name) {
    const Value* v = tj::Get(t, name);
    return tj::Set(arena, entry, name, v ? tj::Clone(arena, v) : tj::Null(arena));
  };
  bool ok = entry && tj::Set(arena, entry, "tokenId", token_id ? tj::Clone(arena, token_id) : tj::Str(arena, "native")) &&
            copy("balanceRaw") && copy("decimals") && copy("symbol") &&
            tj::Set(arena, entry, "price", priced ? tj::Num(arena, quote->number) : tj::Null(arena)) &&
            tj::Set(arena, entry, "fiat", !priced || !isfinite(units) ? tj::Null(arena)
                                                                     : tj::Num(arena, units * quote->number)) &&
            copy("source");
  if (ok && display) ok = tj::Set(arena, entry, "displayUnits", tj::Clone(arena, display));
  return ok ? entry : nullptr;
}

}  // namespace

bool WalletQueryContext::BalancesFetch(const Network& network, const char* address, const char* const* tokens, size_t count,
                                 Arena& arena, Value** payload, chains::Error* error) {
  const chains::ChainDriver* driver = Chains().DriverFor(*deps.config, network.family);
  if (!driver) {
    error->Range("no driver for %s", chains::FamilyName(network.family));
    return false;
  }
  chains::Context ctx = ChainContext(arena);
  Value* raw = nullptr;
  if (!driver->Balances(ctx, network, address, tokens, count, &raw, error)) return false;
  // priceJoin: native -> coingeckoId; erc20 -> `${network}:${tokenId}`;
  // spl:/asa: -> the token id.
  const Value* raw_tokens = tj::Get(raw, "tokens");
  const size_t token_total = tj::IsArray(raw_tokens) ? raw_tokens->count : 0;
  const char** keys = static_cast<const char**>(arena.Alloc(sizeof(char*) * (token_total + 1)));
  const char** ids = static_cast<const char**>(arena.Alloc(sizeof(char*) * (token_total + 1)));
  if (!keys || !ids) { error->Upstream("arena"); return false; }
  size_t mapped = 0;
  auto map_key = [&](const char* key, const char* id) {
    for (size_t i = 0; i < mapped; ++i)
      if (!strcmp(keys[i], key)) { ids[i] = id; return; }  // Map.set replaces
    keys[mapped] = key;
    ids[mapped] = id;
    ++mapped;
  };
  if (network.has_native && network.native_coingecko_id) map_key("native", network.native_coingecko_id);
  for (size_t i = 0; i < token_total; ++i) {
    const char* id = str(raw_tokens->items[i], "tokenId");
    if (!id) continue;
    if (chains::StartsWith(id, "erc20:")) {
      Value* price_id = chains::Format(arena, "%s:%s", network.id, id);
      if (price_id) map_key(id, price_id->text);
    } else if (chains::StartsWith(id, "spl:") || chains::StartsWith(id, "asa:")) {
      map_key(id, id);
    }
  }
  // [...new Set(priceIds.values())].
  const char** unique = static_cast<const char**>(arena.Alloc(sizeof(char*) * (mapped + 1)));
  size_t unique_count = 0;
  for (size_t i = 0; unique && i < mapped; ++i) {
    bool seen = false;
    for (size_t j = 0; j < unique_count; ++j) if (!strcmp(unique[j], ids[i])) { seen = true; break; }
    if (!seen) unique[unique_count++] = ids[i];
  }
  Value* prices = unique_count ? valuation.SpotPrices(arena, unique, unique_count) : tj::Obj(arena);
  Value* out = tj::Obj(arena);
  const Value* raw_native = tj::Get(raw, "native");
  Value* native_input = tj::Clone(arena, raw_native);
  if (!out || !native_input || !tj::Set(arena, native_input, "tokenId", tj::Str(arena, "native"))) {
    error->Upstream("arena");
    return false;
  }
  Value* entries = tj::Arr(arena);
  bool ok = entries && tj::Set(arena, out, "network", tj::Str(arena, network.id)) &&
            tj::Set(arena, out, "address", tj::Str(arena, address)) &&
            tj::Set(arena, out, "native", balance_entry(arena, "native", native_input, prices, keys, ids, mapped));
  for (size_t i = 0; ok && i < token_total; ++i) {
    const char* id = str(raw_tokens->items[i], "tokenId");
    ok = tj::Push(arena, entries, balance_entry(arena, id ? id : "", raw_tokens->items[i], prices, keys, ids, mapped));
  }
  ok = ok && tj::Set(arena, out, "tokens", entries);
  const Value* reference = tj::Get(raw, "reference");
  if (ok && reference) ok = tj::Set(arena, out, "reference", tj::Clone(arena, reference));
  const Value* finality = tj::Get(raw, "finality");
  if (ok && finality) ok = tj::Set(arena, out, "finality", tj::Clone(arena, finality));
  if (!ok) { error->Upstream("arena"); return false; }
  // writeThrough runs synchronously before the fetch resolves: touch the
  // account and merge the snapshot (failures are logged only).
  Account account;
  if (deps.store->Touch(network.id, address, Now(), &account))
    deps.store->PutBalance(account.id, out, tokens, count, Now(), arena);
  *payload = out;
  return true;
}

namespace {

struct BalancesContext {
  const Network* network;
  const char* address;
  const char* const* tokens;
  size_t count;
};

bool balances_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  auto* c = static_cast<BalancesContext*>(opaque);
  return impl->BalancesFetch(*c->network, c->address, c->tokens, c->count, arena, value, error);
}

bool balances_floor(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, int64_t* as_of) {
  auto* c = static_cast<BalancesContext*>(opaque);
  Account account;
  if (impl->deps.store->Find(c->network->id, c->address, impl->Now(), &account) != ReadStatus::kFound) return false;
  Value* payload = nullptr;
  int64_t snapshot_at = 0;
  if (impl->deps.store->GetBalance(account.id, impl->Now(), arena, &payload, &snapshot_at) != ReadStatus::kFound)
    return false;
  Value* out = tj::Obj(arena);
  Value* tokens = tj::Arr(arena);
  if (!out || !tokens) return false;
  const Value* stored = tj::Get(payload, "tokens");
  for (uint32_t i = 0; tj::IsArray(stored) && i < stored->count; ++i) {
    bool keep = c->count == 0;
    const char* id = str(stored->items[i], "tokenId");
    for (size_t r = 0; !keep && id && r < c->count; ++r) keep = strcmp(c->tokens[r], id) == 0;
    if (keep && !tj::Push(arena, tokens, stored->items[i])) return false;
  }
  const Value* native = tj::Get(payload, "native");
  bool ok = tj::Set(arena, out, "network", tj::Str(arena, c->network->id)) &&
            tj::Set(arena, out, "address", tj::Str(arena, c->address)) &&
            tj::Set(arena, out, "native", native ? tj::Clone(arena, native) : tj::Null(arena)) &&
            tj::Set(arena, out, "tokens", tokens);
  const Value* reference = tj::Get(payload, "reference");
  if (ok && reference) ok = tj::Set(arena, out, "reference", tj::Clone(arena, reference));
  const Value* finality = tj::Get(payload, "finality");
  if (ok && finality) ok = tj::Set(arena, out, "finality", tj::Clone(arena, finality));
  if (!ok) return false;
  *value = out;
  *as_of = snapshot_at;
  return true;
}

Revalidation* balances_revalidation(void* opaque, const char* key) {
  auto* c = static_cast<BalancesContext*>(opaque);
  Revalidation* job = new_revalidation(Revalidation::Kind::kBalances, key);
  if (!job) return nullptr;
  job->network.Assign(c->network->id);
  job->subject.Assign(c->address);
  if (c->count) {
    job->tokens = base::AllocateCStringArray(c->count);
    if (!job->tokens) { FreeRevalidation(job); return nullptr; }
    for (size_t i = 0; i < c->count; ++i) {
      const size_t n = strlen(c->tokens[i]);
      job->tokens[i] = base::AllocateStorage<char[]>(n + 1).release();
      if (!job->tokens[i]) { FreeRevalidation(job); return nullptr; }
      memcpy(job->tokens[i], c->tokens[i], n + 1);
      job->token_count = i + 1;
    }
  }
  return job;
}

// {...value, asOf, stale, cache} from an SWR result.
Value* with_provenance(Arena& arena, const SwrResult& result) {
  Value* out = tj::Clone(arena, result.value);
  if (!tj::IsObject(out)) return nullptr;
  if (!tj::Set(arena, out, "asOf", chains::IsoValue(arena, result.as_of_ms)) ||
      !tj::Set(arena, out, "stale", tj::Bool(arena, result.stale)) ||
      !tj::Set(arena, out, "cache", SwrProvenance(arena, result)))
    return nullptr;
  return out;
}

}  // namespace

FacetReply WalletQueryService::Balances(Arena& arena, const char* network_id, const char* address, const char* const* tokens,
                        size_t token_count) {
  FacetReply result;
  auto resolved_network = impl_->Resolve(network_id);
  if (const auto* failure = std::get_if<FacetReply>(&resolved_network)) return *failure;
  Network& network = std::get<Network>(resolved_network);
  if (!network.caps.balances) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "balances not served for %s", network_id);
    return result;
  }
  if (!impl_->Chains().DriverFor(*impl_->deps.config, network.family)) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "no driver for %s", chains::FamilyName(network.family));
    return result;
  }
  const char* normalized = Canonical(arena, network, address);
  char key[1024];
  if (!normalized || !BalancesCacheKey(network.id, normalized, tokens, token_count, key, sizeof(key))) return result;
  BalancesContext context{&network, normalized, tokens, token_count};
  SwrFetcher fetcher;
  fetcher.fetch = balances_fetch;
  fetcher.context = &context;
  fetcher.floor = balances_floor;
  fetcher.revalidation = balances_revalidation;
  SwrResult swr;
  if (!impl_->SwrRead(key, IsUtxoFamily(network.family) ? kTtlBalancesUtxo : kTtlBalances, fetcher, arena, &swr)) {
    SetError(&result, swr.error);
    return result;
  }
  result.payload = with_provenance(arena, swr);
  result.outcome = result.payload ? Outcome::kOk : Outcome::kUpstream;
  return result;
}

// ---- insight ---------------------------------------------------------------------

namespace {
struct AddressContext {
  const Network* network;
  const char* address;
};

bool insight_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  auto* c = static_cast<AddressContext*>(opaque);
  const chains::ChainDriver* driver = impl->Chains().DriverFor(*impl->deps.config, c->network->family);
  chains::Context ctx = impl->ChainContext(arena);
  return driver->Insight(ctx, *c->network, c->address, value, error);
}

Revalidation* address_revalidation(Revalidation::Kind kind, const AddressContext& c, const char* key) {
  Revalidation* job = new_revalidation(kind, key);
  if (!job) return nullptr;
  job->network.Assign(c.network->id);
  job->subject.Assign(c.address);
  return job;
}

Revalidation* insight_revalidation(void* opaque, const char* key) {
  return address_revalidation(Revalidation::Kind::kInsight, *static_cast<AddressContext*>(opaque), key);
}
}  // namespace

bool InsightCacheKey(Arena& arena, const Network& network, const char* address, char* output, size_t capacity) {
  const char* normalized = Canonical(arena, network, address);
  return normalized && key_text(output, capacity, "ins:%s:%s", network.id, normalized);
}

FacetReply WalletQueryService::Insight(Arena& arena, const char* network_id, const char* address) {
  FacetReply result;
  auto resolved_network = impl_->Resolve(network_id);
  if (const auto* failure = std::get_if<FacetReply>(&resolved_network)) return *failure;
  Network& network = std::get<Network>(resolved_network);
  const chains::ChainDriver* driver = impl_->Chains().DriverFor(*impl_->deps.config, network.family);
  if (!driver || !driver->HasInsight() || !network.caps.insight) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "insight not served for %s", network_id);
    return result;
  }
  const char* normalized = Canonical(arena, network, address);
  char key[1024];
  if (!normalized || !InsightCacheKey(arena, network, address, key, sizeof(key))) return result;
  AddressContext context{&network, normalized};
  SwrFetcher fetcher;
  fetcher.fetch = insight_fetch;
  fetcher.context = &context;
  fetcher.revalidation = insight_revalidation;
  SwrResult swr;
  if (!impl_->SwrRead(key, kTtlInsight, fetcher, arena, &swr)) {
    SetError(&result, swr.error);
    return result;
  }
  result.payload = with_provenance(arena, swr);
  result.outcome = result.payload ? Outcome::kOk : Outcome::kUpstream;
  return result;
}

// ---- NFTs ------------------------------------------------------------------------------

namespace {

// The TypeScript NftsFacet reconcile and read page size.
constexpr size_t kNftBatch = 200;
// The native KV's own key bound (kv_store_internal.h kMaxKey).
constexpr size_t kNativeMaxKeyBytes = 1024;
constexpr size_t kNftKeyBytes = 2048;
constexpr size_t kNftRecordBytes = 1024 * 1024;
// The read path's per-page scratch (keys, stored records, their parse trees).
constexpr size_t kNftPageArenaBytes = 8 * 1024 * 1024;

bool encoded_unreserved(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

struct PageEntry {
  char* key;
  char* value;
  size_t length;
};
struct PageCollect {
  Arena* arena;
  PageEntry* entries;
  size_t count;
  size_t capacity;
  bool ok;
};
bool page_collect(const char* key, const void* value, size_t size, PageCollect* c) {
  if (c->count == c->capacity) return false;
  PageEntry& e = c->entries[c->count];
  e.key = c->arena->CopyText(key, strlen(key));
  e.value = c->arena->CopyText(static_cast<const char*>(value), size);
  e.length = size;
  if (!e.key || !e.value) { c->ok = false; return false; }
  ++c->count;
  return true;
}

// One disk.page(prefix, max, now, reverse, after) into the arena.
bool kv_page(storage::KvStore* kv, const char* prefix, const char* after, size_t max, int64_t now, bool reverse, Arena& arena,
             PageCollect* out, storage::KvPageResult* result) {
  out->arena = &arena;
  out->entries = static_cast<PageEntry*>(arena.Alloc(sizeof(PageEntry) * max));
  out->count = 0;
  out->capacity = max;
  out->ok = out->entries != nullptr;
  *result = storage::KvPageResult{};
  return out->ok && kv->Page(prefix, after, max, now, reverse, [&, visitor_state = out](const char* key, const void* value, size_t size) { return (page_collect)(key, value, size, visitor_state); }, result) && out->ok;
}

using NftPage = PageCollect;
// One disk.page(prefix, 200, now, false, after) into `arena`.
bool nft_page(storage::KvStore* kv, const char* prefix, const char* after, int64_t now, Arena& arena, NftPage* out,
              storage::KvPageResult* result) {
  return kv_page(kv, prefix, after, kNftBatch, now, false, arena, out, result);
}

int compare_text(const void* a, const void* b) {
  return strcmp(*static_cast<const char* const*>(a), *static_cast<const char* const*>(b));
}

bool write_record(Arena& arena, const Value* record, uint8_t* scratch, const char* key, int64_t expires,
                  storage::KvMutation* mutation) {
  chains::OutBuffer out{scratch, kNftRecordBytes, 0, true};
  if (!tj::Stringify(record, out) || !out.ok) return false;
  char* text = arena.CopyText(reinterpret_cast<const char*>(scratch), out.used);
  char* key_copy = arena.CopyText(key, strlen(key));
  if (!text || !key_copy) return false;
  *mutation = storage::KvMutation{key_copy, text, static_cast<uint32_t>(out.used), expires, false};
  return true;
}

// NftsFacet.reconcile: upsert every scanned item in batches of 200 (one
// write each; firstSeenAt kept from a live previous record, goneAt cleared),
// then page the account's range and stamp goneAt on every live record the
// scan did not return. A batch that fails (a key encodeURIComponent or LMDB
// refuses) is not written and ends the reconcile: earlier batches stay, no
// item is marked gone, and the failure is the scan's error.
bool nft_reconcile(WalletQueryContext* impl, const char* account_id, const Value* items, int64_t at, uint8_t* scratch,
                   Arena& arena, char* failure, size_t failure_capacity) {
  storage::KvStore* kv = impl->deps.kv;
  Store* store = impl->deps.store;
  const int64_t expires = at + kNftInventoryTtlMs;
  const char** seen = static_cast<const char**>(arena.Alloc(sizeof(char*) * (items->count + 1)));
  if (!seen) { snprintf(failure, failure_capacity, "Error: out of memory"); return false; }
  size_t seen_count = 0;
  for (uint32_t i = 0; i < items->count; ++i)
    if (const char* id = str(items->items[i], "nftId")) seen[seen_count++] = id;
  qsort(reinterpret_cast<void*>(seen), seen_count, sizeof(char*), fortis::base::FatalCallback<compare_text>);
  for (uint32_t offset = 0; offset < items->count; offset += kNftBatch) {
    const uint32_t end = offset + kNftBatch < items->count ? offset + static_cast<uint32_t>(kNftBatch) : items->count;
    const tj::Mark mark = tj::MarkOf(arena);
    storage::KvMutation* mutations = static_cast<storage::KvMutation*>(arena.Alloc(sizeof(storage::KvMutation) * (end - offset)));
    size_t m = 0;
    bool ok = mutations != nullptr;
    if (!ok) snprintf(failure, failure_capacity, "Error: out of memory");
    for (uint32_t i = offset; ok && i < end; ++i) {
      const Value* item = items->items[i];
      const char* nft_id = str(item, "nftId");
      if (!nft_id) continue;
      char key[kNftKeyBytes];
      const NftKeyStatus status = NftItemKey(account_id, nft_id, key, sizeof(key));
      if (status != NftKeyStatus::kOk) {
        snprintf(failure, failure_capacity, "%s",
                 status == NftKeyStatus::kUriError ? "URIError: URI malformed"
                 : status == NftKeyStatus::kTooLong ? "Error: key exceeds the native KV key bound (1024)"
                                                    : "Error: key does not fit");
        ok = false;
        break;
      }
      Value* previous = nullptr;
      if (store->GetJson(key, at, arena, &previous) == ReadStatus::kError) previous = nullptr;
      const Value* first_seen = previous ? tj::Get(previous, "firstSeenAt") : nullptr;
      Value* record = tj::Obj(arena);
      Value* urls = tj::Obj(arena);
      auto copy = [&](const char* field) {
        const Value* v = tj::Get(item, field);
        return tj::Set(arena, record, field, v ? tj::Clone(arena, v) : tj::Null(arena));
      };
      const Value* image = tj::Get(item, "imageUrl");
      const Value* flags = tj::Get(item, "flags");
      ok = record && urls && tj::Set(arena, record, "accountId", tj::Str(arena, account_id)) && copy("nftId") &&
           copy("contractAddress") && copy("tokenId") && copy("standard") && copy("name") &&
           copy("collectionName") &&
           tj::Set(arena, urls, "image", image ? tj::Clone(arena, image) : tj::Null(arena)) &&
           tj::Set(arena, record, "sourceUrls", urls) &&
           tj::Set(arena, record, "flags", flags ? tj::Clone(arena, flags) : tj::Obj(arena)) &&
           tj::Set(arena, record, "firstSeenAt",
                   tj::Num(arena, tj::IsNumber(first_seen) ? first_seen->number : static_cast<double>(at))) &&
           tj::Set(arena, record, "goneAt", tj::Null(arena)) &&
           write_record(arena, record, scratch, key, expires, &mutations[m]);
      if (!ok) snprintf(failure, failure_capacity, "Error: NFT record too large");
      else ++m;
    }
    if (ok && m && !kv->Write(mutations, m)) {
      snprintf(failure, failure_capacity, "Error: KV write failed");
      ok = false;
    }
    tj::Rewind(arena, mark);
    if (!ok) return false;
  }
  char prefix[128];
  if (!NftItemPrefix(account_id, prefix, sizeof(prefix))) {
    snprintf(failure, failure_capacity, "Error: NFT item prefix too large");
    return false;
  }
  char after[sizeof(storage::KvPageResult::next_cursor)] = {};
  bool has_after = false;
  for (;;) {
    const tj::Mark mark = tj::MarkOf(arena);
    NftPage page{};
    storage::KvPageResult page_result;
    if (!nft_page(kv, prefix, has_after ? after : nullptr, at, arena, &page, &page_result)) {
      tj::Rewind(arena, mark);
      snprintf(failure, failure_capacity, "Error: NFT item page failed");
      return false;
    }
    storage::KvMutation* mutations = static_cast<storage::KvMutation*>(arena.Alloc(sizeof(storage::KvMutation) * (page.count + 1)));
    size_t m = 0;
    if (!mutations) {
      tj::Rewind(arena, mark);
      snprintf(failure, failure_capacity, "Error: out of memory");
      return false;
    }
    bool ok = true;
    for (size_t i = 0; i < page.count; ++i) {
      Value* record = tj::Parse(arena, reinterpret_cast<const uint8_t*>(page.entries[i].value), page.entries[i].length);
      const char* nft_id = str(record, "nftId");
      if (!nft_id || !is_null(tj::Get(record, "goneAt"))) continue;
      if (bsearch(reinterpret_cast<const void*>(&nft_id), reinterpret_cast<const void*>(seen), seen_count, sizeof(char*), fortis::base::FatalCallback<compare_text>)) continue;
      if (!tj::Set(arena, record, "goneAt", tj::Num(arena, static_cast<double>(at))) ||
          !write_record(arena, record, scratch, page.entries[i].key, expires, &mutations[m])) {
        ok = false;
        break;
      }
      ++m;
    }
    if (ok && m) ok = kv->Write(mutations, m);
    if (!ok) {
      tj::Rewind(arena, mark);
      snprintf(failure, failure_capacity, "Error: NFT gone-item write failed");
      return false;
    }
    const bool done = page_result.done || !page_result.next_cursor[0];
    if (!done) {
      snprintf(after, sizeof(after), "%s", page_result.next_cursor);
      has_after = true;
    }
    tj::Rewind(arena, mark);
    if (done) break;
  }
  return true;
}

// ensureScanned's body for a due account: the driver's full scan, the
// reconcile, then saveScanState(null | String(err)).
void nft_scan(WalletQueryContext* impl, const chains::NftReader* driver, const Network& network, const char* address,
              const char* account_id, const char* scan_key, Arena& arena) {
  chains::Context ctx = impl->ChainContext(arena);
  Value* items = nullptr;
  chains::Error error;
  bool scanned = driver->ReadNfts(ctx, network, address, &items, &error) && tj::IsArray(items);
  const int64_t at = impl->Now();
  char failure[sizeof(error.message) + 32] = {};
  if (!scanned)
    snprintf(failure, sizeof(failure), "%s: %s", error.kind == chains::ErrorKind::kRange ? "RangeError" : "Error",
             error.message[0] ? error.message : "scan failed");
  Store* store = impl->deps.store;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kNftRecordBytes);
  auto* scratch = scratch_storage.get();
  store->Lock();
  if (scanned && !scratch) {
    snprintf(failure, sizeof(failure), "Error: out of memory");
    scanned = false;
  }
  if (scanned) scanned = nft_reconcile(impl, account_id, items, at, scratch, arena, failure, sizeof(failure));
  Value* previous = nullptr;
  if (store->GetJson(scan_key, at, arena, &previous) != ReadStatus::kFound) previous = nullptr;
  Value* next = tj::Obj(arena);
  bool ok = next != nullptr;
  if (ok && scanned) {
    ok = tj::Set(arena, next, "lastScanAt", tj::Num(arena, static_cast<double>(at))) &&
         tj::Set(arena, next, "lastFullScanAt", tj::Num(arena, static_cast<double>(at))) &&
         tj::Set(arena, next, "lastError", tj::Null(arena)) && tj::Set(arena, next, "lastErrorAt", tj::Null(arena));
  } else if (ok) {
    const Value* last_scan = previous ? tj::Get(previous, "lastScanAt") : nullptr;
    const Value* last_full = previous ? tj::Get(previous, "lastFullScanAt") : nullptr;
    Value* message = tj::Str(arena, failure);
    ok = message && tj::Set(arena, next, "lastScanAt", last_scan ? tj::Clone(arena, last_scan) : tj::Null(arena)) &&
         tj::Set(arena, next, "lastFullScanAt", last_full ? tj::Clone(arena, last_full) : tj::Null(arena)) &&
         tj::Set(arena, next, "lastError", message) &&
         tj::Set(arena, next, "lastErrorAt", tj::Num(arena, static_cast<double>(at)));
  }
  if (ok && scratch) {
    chains::OutBuffer out{scratch, kNftRecordBytes, 0, true};
    if (tj::Stringify(next, out) && out.ok) {
      const storage::KvMutation mutation{scan_key, scratch, static_cast<uint32_t>(out.used), at + kNftInventoryTtlMs, false};
      impl->deps.kv->Write(&mutation, 1);
    }
  }
  store->Unlock();
  scratch_storage.reset();
}

}  // namespace

NftKeyStatus NftItemKey(const char* account_id, const char* nft_id, char* output, size_t capacity) {
  const size_t id_length = strlen(nft_id);
  if (!codec::utf8::Valid(reinterpret_cast<const uint8_t*>(nft_id), id_length)) return NftKeyStatus::kUriError;
  size_t encoded = 0;
  for (size_t i = 0; i < id_length; ++i) {
    encoded += encoded_unreserved(static_cast<uint8_t>(nft_id[i])) ? 1 : 3;
  }
  char account[128];
  if (!enc(account_id, account, sizeof(account))) return NftKeyStatus::kNoRoom;
  // key = nft/item/{account}/{id}, refused over the native KV key bound.
  const size_t key_length = 9 + strlen(account) + 1 + encoded;
  if (key_length > kNativeMaxKeyBytes) return NftKeyStatus::kTooLong;
  if (key_length >= capacity) return NftKeyStatus::kNoRoom;
  const int prefix = snprintf(output, capacity, "nft/item/%s/", account);
  chains::OutBuffer out{reinterpret_cast<uint8_t*>(output + prefix), capacity - static_cast<size_t>(prefix) - 1, 0, true};
  if (!chains::EncodeUriComponent(nft_id, id_length, out) || !out.ok) return NftKeyStatus::kNoRoom;
  output[prefix + static_cast<int>(out.used)] = '\0';
  return NftKeyStatus::kOk;
}

bool NftItemPrefix(const char* account_id, char* output, size_t capacity) {
  char account[128];
  return enc(account_id, account, sizeof(account)) && key_text(output, capacity, "nft/item/%s/", account);
}

bool NftScanKey(const char* account_id, char* output, size_t capacity) {
  char account[128];
  return enc(account_id, account, sizeof(account)) && key_text(output, capacity, "nft/scan/%s", account);
}

bool NftScanDue(const Value* state, int64_t now_ms) {
  if (!tj::IsObject(state)) return true;
  const Value* error_at = tj::Get(state, "lastErrorAt");
  if (!is_null(tj::Get(state, "lastError")) && tj::IsNumber(error_at) &&
      now_ms - static_cast<int64_t>(error_at->number) < kNftErrorCooldownMs)
    return false;
  const Value* scan_at = tj::Get(state, "lastScanAt");
  return !tj::IsNumber(scan_at) || now_ms - static_cast<int64_t>(scan_at->number) > kNftScanMaxAgeMs;
}

FacetReply WalletQueryService::Nfts(Arena& arena, const char* network_id, const char* address) {
  FacetReply result;
  auto resolved_network = impl_->Resolve(network_id);
  if (const auto* failure = std::get_if<FacetReply>(&resolved_network)) return *failure;
  Network& network = std::get<Network>(resolved_network);
  const chains::ChainDriver* driver = impl_->Chains().DriverFor(*impl_->deps.config, network.family);
  if (!driver || !driver->Nfts(*impl_->deps.config) || !network.caps.nfts) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "nfts not served for %s", network_id);
    return result;
  }
  const char* normalized = Canonical(arena, network, address);
  Store* store = impl_->deps.store;
  Account account;
  if (!normalized || !store->Touch(network.id, normalized, Now(), &account)) return result;
  char scan_key[128], flight_key[128], prefix[128];
  if (!NftScanKey(account.id, scan_key, sizeof(scan_key)) || !NftItemPrefix(account.id, prefix, sizeof(prefix)) ||
      !key_text(flight_key, sizeof(flight_key), "nftscan:%s", account.id))
    return result;
  // ensureScanned: one flight per account; a caller that joined it does not
  // scan again, it reads what the flight stored.
  size_t slot = kFlights;
  bool waited = false;
  if (impl_->BeginFlight(flight_key, &slot, &waited) && !waited) {
    // Nothing the scan allocates (the provider body, its parse tree, the
    // records) outlives it: the read below starts from this mark.
    const tj::Mark mark = tj::MarkOf(arena);
    Value* state = nullptr;
    if (store->GetJson(scan_key, Now(), arena, &state) != ReadStatus::kFound) state = nullptr;
    if (NftScanDue(state, Now())) nft_scan(impl_.get(), driver->Nfts(*impl_->deps.config), network, normalized, account.id, scan_key, arena);
    tj::Rewind(arena, mark);
    impl_->EndFlight(slot);
  }
  // The live records with goneAt null, in key order, paged 200 keys at a
  // time through a scratch arena (gone records cost no response memory);
  // each image registered with the media facet.
  const int64_t now = Now();
  Value* items = tj::Arr(arena);
  if (!items) return result;
  tj::Arena page_arena = tj::GrowingArena(kNftPageArenaBytes);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> page_arena_blocks(&page_arena);
  const tj::Mark page_start = tj::MarkOf(page_arena);
  char after[sizeof(storage::KvPageResult::next_cursor)] = {};
  bool has_after = false;
  bool ok = true;
  for (;;) {
    NftPage page{};
    storage::KvPageResult page_result;
    if (!nft_page(impl_->deps.kv, prefix, has_after ? after : nullptr, now, page_arena, &page, &page_result)) {
      ok = false;
      break;
    }
    for (size_t i = 0; ok && i < page.count; ++i) {
      Value* row =
          tj::Parse(page_arena, reinterpret_cast<const uint8_t*>(page.entries[i].value), page.entries[i].length);
      if (!tj::IsObject(row) || !is_null(tj::Get(row, "goneAt"))) continue;
      const Value* image = tj::Get(tj::Get(row, "sourceUrls"), "image");
      Value* proxy = tj::Null(arena);
      if (tj::IsString(image)) {
        char media_key[fortis::discovery::market_view::kMediaKeyChars + 1];
        if (discovery::RegisterMediaSource(impl_->deps.kv, image->text, image->length, now, media_key))
          proxy = chains::Format(arena, "/v1/media/%s", media_key);
      }
      Value* item = tj::Obj(arena);
      Value* media = tj::Obj(arena);
      auto copy = [&](const char* to, const char* from) {
        const Value* v = tj::Get(row, from);
        return tj::Set(arena, item, to, v ? tj::Clone(arena, v) : tj::Null(arena));
      };
      const Value* flags = tj::Get(row, "flags");
      const Value* first_seen = tj::Get(row, "firstSeenAt");
      ok = item && media && proxy && copy("id", "nftId") && copy("contractAddress", "contractAddress") &&
           copy("tokenId", "tokenId") && copy("standard", "standard") && copy("name", "name") &&
           copy("collectionName", "collectionName") &&
           // images[i] ?? null: whatever the driver stored (a string, or an
           // EVM provider's number/object), only a string is registered.
           tj::Set(arena, media, "image", is_null(image) ? tj::Null(arena) : tj::Clone(arena, image)) &&
           tj::Set(arena, media, "imageProxy", proxy) && tj::Set(arena, item, "media", media) &&
           tj::Set(arena, item, "flags", is_null(flags) ? tj::Obj(arena) : tj::Clone(arena, flags)) &&
           tj::Set(arena, item, "firstSeenAt",
                   tj::IsNumber(first_seen) ? chains::IsoValue(arena, static_cast<int64_t>(first_seen->number))
                                            : tj::Null(arena)) &&
           tj::Push(arena, items, item);
    }
    const bool done = page_result.done || !page_result.next_cursor[0];
    if (!done) {
      snprintf(after, sizeof(after), "%s", page_result.next_cursor);
      has_after = true;
    }
    tj::Rewind(page_arena, page_start);
    if (!ok || done) break;
  }
  tj::FreeArena(page_arena);
  if (!ok) return result;
  Value* state = nullptr;
  const bool found = store->GetJson(scan_key, now, arena, &state) == ReadStatus::kFound;
  const bool stale = found && !is_null(tj::Get(state, "lastError"));
  const Value* scan_at = found ? tj::Get(state, "lastScanAt") : nullptr;
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "network", tj::Str(arena, network.id)) ||
      !tj::Set(arena, out, "address", tj::Str(arena, normalized)) || !tj::Set(arena, out, "items", items) ||
      !tj::Set(arena, out, "stale", tj::Bool(arena, stale)) ||
      !tj::Set(arena, out, "cache", CacheJson(arena, true, tj::IsNumber(scan_at) ? static_cast<int64_t>(scan_at->number) : 0,
                                              tj::IsNumber(scan_at))))
    return result;
  result.payload = out;
  result.outcome = Outcome::kOk;
  return result;
}

// ---- tx detail (TxDetailFacet) ------------------------------------------------------

namespace {

// enc() of a path value: at most 600 bytes (AddressTask::subject), so at
// most 1,800 encoded (a three-byte character is nine).
constexpr size_t kTxEncodedBytes = 1800 + 1;

bool tx_record_key(const char* network, const char* hash, char* out, size_t capacity) {
  char n[kTxEncodedBytes], h[kTxEncodedBytes];
  return enc(network, n, sizeof(n)) && enc(hash, h, sizeof(h)) && key_text(out, capacity, "tx/detail/%s/%s", n, h);
}

bool tx_pending_key(int64_t poll_until, const char* network, const char* hash, char* out, size_t capacity) {
  char n[kTxEncodedBytes], h[kTxEncodedBytes], pad[24];
  Store::Pad13(poll_until, pad);
  return enc(network, n, sizeof(n)) && enc(hash, h, sizeof(h)) &&
         key_text(out, capacity, "tx/pending/%s/%s/%s", pad, n, h);
}

Value* tx_serialize(Arena& arena, const Value* status, const Value* block, const Value* detail, const Value* raw,
                    int64_t fetched_at, bool cached) {
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "status", tj::Clone(arena, status)) ||
      !tj::Set(arena, out, "blockNumber", block ? tj::Clone(arena, block) : tj::Null(arena)) ||
      !tj::Set(arena, out, "detail", is_null(detail) ? tj::Obj(arena) : tj::Clone(arena, detail)) ||
      !tj::Set(arena, out, "raw", raw ? tj::Clone(arena, raw) : tj::Null(arena)) ||
      !tj::Set(arena, out, "fetchedAt", chains::IsoValue(arena, fetched_at)) ||
      !tj::Set(arena, out, "cache", CacheJson(arena, cached, fetched_at, true)))
    return nullptr;
  return out;
}

bool write_json(storage::KvStore* kv, const char* key, const Value* value, int64_t expires, uint8_t* scratch, size_t capacity,
                storage::KvMutation* mutation, Arena& arena) {
  chains::OutBuffer out{scratch, capacity, 0, true};
  if (!value || !tj::Stringify(value, out) || !out.ok) return false;
  char* text = arena.CopyText(reinterpret_cast<const char*>(scratch), out.used);
  char* key_copy = arena.CopyText(key, strlen(key));
  if (!text || !key_copy) return false;
  (void)kv;
  *mutation = storage::KvMutation{key_copy, text, static_cast<uint32_t>(out.used), expires, false};
  return true;
}

}  // namespace

bool TxRecordKey(const char* network, const char* hash, char* out, size_t capacity) {
  return tx_record_key(network, hash, out, capacity);
}

bool TxViewKey(const char* network, const char* hash, char* out, size_t capacity) {
  return key_text(out, capacity, "txd:%s:%s", network, hash);
}

bool TxPendingKey(int64_t poll_until, const char* network, const char* hash, char* out, size_t capacity) {
  return tx_pending_key(poll_until, network, hash, out, capacity);
}

FacetReply WalletQueryService::TxDetail(Arena& arena, const char* network_id, const char* hash) {
  FacetReply result;
  auto resolved_network = impl_->Resolve(network_id);
  if (const auto* failure = std::get_if<FacetReply>(&resolved_network)) return *failure;
  Network& network = std::get<Network>(resolved_network);
  const chains::ChainDriver* driver = impl_->Chains().DriverFor(*impl_->deps.config, network.family);
  if (!driver || !driver->HasTxStatus() || !network.caps.tx_status) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "tx status not served for %s", network_id);
    return result;
  }
  Store* store = impl_->deps.store;
  char record_key[kTxKeyBytes];
  if (!tx_record_key(network.id, hash, record_key, sizeof(record_key))) return result;
  const int64_t start = Now();
  Value* row = nullptr;
  const ReadStatus row_status = store->GetJson(record_key, start, arena, &row);
  if (row_status == ReadStatus::kFound) {
    const Value* status = tj::Get(row, "status");
    const Value* fetched = tj::Get(row, "fetchedAt");
    if (!chains::Equals(status, "pending") && !chains::Equals(status, "unknown") && tj::IsNumber(fetched) &&
        static_cast<int64_t>(fetched->number) > start - kSettledRecheckMs) {
      result.payload = tx_serialize(arena, status, tj::Get(row, "blockNumber"), tj::Get(row, "detail"),
                                    tj::Get(row, "raw"), static_cast<int64_t>(fetched->number), true);
      result.outcome = result.payload ? Outcome::kOk : Outcome::kUpstream;
      return result;
    }
  } else {
    row = nullptr;
  }
  chains::Context ctx = impl_->ChainContext(arena);
  Value* detail = nullptr;
  chains::Error error;
  if (!driver->TxStatus(ctx, network, hash, &detail, &error)) {
    SetError(&result, error);
    return result;
  }
  const int64_t now = Now();
  Value* status = detail ? tj::Get(detail, "status") : nullptr;
  if (!tj::IsString(status)) status = tj::Str(arena, "unknown");
  const bool pending = chains::Equals(status, "pending") || chains::Equals(status, "unknown");
  const Value* block = detail ? tj::Get(detail, "blockNumber") : nullptr;
  const Value* confirmed = detail ? tj::Get(detail, "confirmedAt") : nullptr;
  const Value* body = detail ? tj::Get(detail, "detail") : nullptr;
  const Value* raw = detail ? tj::Get(detail, "raw") : nullptr;
  Value* values = tj::Obj(arena);
  const int64_t poll_until = now + kTxPollWindowMs;
  bool ok = values && tj::Set(arena, values, "network", tj::Str(arena, network.id)) &&
            tj::Set(arena, values, "hash", tj::Str(arena, hash)) && tj::Set(arena, values, "status", status) &&
            tj::Set(arena, values, "blockNumber", is_null(block) ? tj::Null(arena) : tj::Clone(arena, block)) &&
            tj::Set(arena, values, "confirmedAt", is_null(confirmed) ? tj::Null(arena) : tj::Clone(arena, confirmed)) &&
            tj::Set(arena, values, "detail", is_null(body) ? tj::Obj(arena) : tj::Clone(arena, body)) &&
            tj::Set(arena, values, "raw", is_null(raw) ? tj::Null(arena) : tj::Clone(arena, raw)) &&
            tj::Set(arena, values, "fetchedAt", tj::Num(arena, static_cast<double>(now))) &&
            tj::Set(arena, values, "pollUntil", pending ? tj::Num(arena, static_cast<double>(poll_until)) : tj::Null(arena));
  if (!ok) return result;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(1024 * 1024);
  auto* scratch = scratch_storage.get();
  if (scratch) {
    store->Lock();
    storage::KvMutation mutations[3];
    size_t m = 0;
    const Value* old_poll = row ? tj::Get(row, "pollUntil") : nullptr;
    char old_pending[kTxKeyBytes], new_pending[kTxKeyBytes];
    if (tj::IsNumber(old_poll) &&
        tx_pending_key(static_cast<int64_t>(old_poll->number), network.id, hash, old_pending, sizeof(old_pending)))
      mutations[m++] = storage::KvMutation{old_pending, nullptr, 0, 0, true};
    bool written = write_json(impl_->deps.kv, record_key, values, now + kTxDetailTtlMs, scratch, 1024 * 1024,
                              &mutations[m], arena);
    if (written) ++m;
    if (written && pending) {
      Value* ref = tj::Obj(arena);
      if (ref && tj::Set(arena, ref, "network", tj::Str(arena, network.id)) &&
          tj::Set(arena, ref, "hash", tj::Str(arena, hash)) &&
          tx_pending_key(poll_until, network.id, hash, new_pending, sizeof(new_pending)) &&
          write_json(impl_->deps.kv, new_pending, ref, poll_until, scratch, 1024 * 1024, &mutations[m], arena))
        ++m;
    }
    if (written) impl_->deps.kv->Write(mutations, m);  // a failure is logged only
    store->Unlock();
    scratch_storage.reset();
  }
  result.payload = tx_serialize(arena, status, tj::Get(values, "blockNumber"), tj::Get(values, "detail"),
                                tj::Get(values, "raw"), now, false);
  result.outcome = result.payload ? Outcome::kOk : Outcome::kUpstream;
  return result;
}

size_t WalletQueryService::PollPendingOnce(Arena& arena) {
  const int64_t now = Now();
  char pad[24], after[64];
  Store::Pad13(now, pad);
  snprintf(after, sizeof(after), "tx/pending/%s", pad);
  PageCollect page{};
  storage::KvPageResult page_result;
  if (!kv_page(impl_->deps.kv, "tx/pending/", after, 50, now, false, arena, &page, &page_result)) return 0;
  size_t transitions = 0;
  // Each pending row is independent: its parse and status read are dropped
  // before the next one.
  const tj::Mark mark = tj::MarkOf(arena);
  for (size_t i = 0; i < page.count; ++i) {
    tj::Rewind(arena, mark);
    Value* ref = tj::Parse(arena, reinterpret_cast<const uint8_t*>(page.entries[i].value), page.entries[i].length);
    const char* network_id = str(ref, "network");
    const char* hash = str(ref, "hash");
    Network network;
    if (!network_id || !hash || !impl_->deps.catalog->Resolve(network_id, strlen(network_id), &network)) continue;
    const chains::ChainDriver* driver = impl_->Chains().DriverFor(*impl_->deps.config, network.family);
    if (!driver || !driver->HasTxStatus()) continue;
    chains::Context ctx = impl_->ChainContext(arena);
    Value* detail = nullptr;
    chains::Error error;
    if (!driver->TxStatus(ctx, network, hash, &detail, &error)) continue;  // logged in TypeScript
    const Value* status = detail ? tj::Get(detail, "status") : nullptr;
    if (!detail || chains::Equals(status, "pending") || chains::Equals(status, "unknown")) continue;
    ++transitions;
    char record_key[kTxKeyBytes];
    if (!tx_record_key(network_id, hash, record_key, sizeof(record_key))) continue;
    auto scratch_storage = base::AllocateStorage<uint8_t[]>(1024 * 1024);
  auto* scratch = scratch_storage.get();
    if (!scratch) continue;
    Store* store = impl_->deps.store;
    store->Lock();
    Value* current = nullptr;
    storage::KvMutation mutations[2];
    size_t m = 0;
    char old_pending[kTxKeyBytes];
    if (store->GetJson(record_key, now, arena, &current) == ReadStatus::kFound) {
      Value* next = tj::Clone(arena, current);
      auto set_from = [&](const char* key) {
        const Value* v = tj::Get(detail, key);
        return tj::Set(arena, next, key, v ? tj::Clone(arena, v) : tj::Null(arena));
      };
      if (next && set_from("status") && set_from("blockNumber") && set_from("confirmedAt") && set_from("detail") &&
          set_from("raw") && tj::Set(arena, next, "fetchedAt", tj::Num(arena, static_cast<double>(Now()))) &&
          tj::Set(arena, next, "pollUntil", tj::Null(arena)) &&
          write_json(impl_->deps.kv, record_key, next, now + kTxDetailTtlMs, scratch, 1024 * 1024, &mutations[m], arena))
        ++m;
      const Value* poll = tj::Get(current, "pollUntil");
      if (tj::IsNumber(poll) &&
          tx_pending_key(static_cast<int64_t>(poll->number), network_id, hash, old_pending, sizeof(old_pending)))
        mutations[m++] = storage::KvMutation{old_pending, nullptr, 0, 0, true};
    }
    if (m) impl_->deps.kv->Write(mutations, m);
    store->Unlock();
    scratch_storage.reset();
  }
  return transitions;
}

// ---- tx detail views ------------------------------------------------------------------

bool WalletQueryContext::TxViewFetch(const Network& network, const char* hash, Arena& arena, Value** detail,
                               chains::Error* error) {
  const chains::TransactionView* view = Chains().ViewFor(network.family);
  if (!view) {
    error->Range("no detail view for %s", chains::FamilyName(network.family));
    return false;
  }
  chains::Context ctx = ChainContext(arena);
  Value* fetched = nullptr;
  if (!view->Fetch(ctx, network, hash, &fetched, error)) return false;
  if (!fetched) {
    // An unknown hash may become visible moments later: never cached.
    error->Upstream("%s", kUnknownMarker);
    return false;
  }
  *detail = fetched;
  return true;
}

namespace {
struct ViewContext {
  const Network* network;
  const char* hash;
};
bool view_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  auto* c = static_cast<ViewContext*>(opaque);
  return impl->TxViewFetch(*c->network, c->hash, arena, value, error);
}
bool view_should_store(WalletQueryContext* impl, void* opaque, const Value* value) {
  auto* c = static_cast<ViewContext*>(opaque);
  const chains::TransactionView* view = impl->Chains().ViewFor(c->network->family);
  return view && view->IsFinal(value, *c->network);
}
Revalidation* view_revalidation(void* opaque, const char* key) {
  auto* c = static_cast<ViewContext*>(opaque);
  Revalidation* job = new_revalidation(Revalidation::Kind::kView, key);
  if (!job) return nullptr;
  job->network.Assign(c->network->id);
  job->subject.Assign(c->hash);
  return job;
}
}  // namespace

FacetReply WalletQueryService::TxView(Arena& arena, const char* network_id, const char* hash) {
  FacetReply result;
  Network network;
  if (!impl_->deps.catalog->Resolve(network_id, strlen(network_id), &network)) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "unknown network");
    return result;
  }
  if (!impl_->Chains().ViewFor(network.family)) {
    result.outcome = Outcome::kUnsupported;
    snprintf(result.detail, sizeof(result.detail), "no detail view for %s", chains::FamilyName(network.family));
    return result;
  }
  char key[1024];
  if (!TxViewKey(network.id, hash, key, sizeof(key))) return result;
  ViewContext context{&network, hash};
  SwrFetcher fetcher;
  fetcher.fetch = view_fetch;
  fetcher.context = &context;
  fetcher.should_store = view_should_store;
  fetcher.revalidation = view_revalidation;
  SwrResult swr;
  if (!impl_->SwrRead(key, kTtlTxDetail, fetcher, arena, &swr)) {
    if (swr.unknown) {
      result.outcome = Outcome::kNotFound;
      return result;
    }
    SetError(&result, swr.error);
    return result;
  }
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "network", tj::Str(arena, network.id)) ||
      !tj::Set(arena, out, "family", tj::Str(arena, chains::FamilyName(network.family))) ||
      !tj::Set(arena, out, "detail", swr.value) || !tj::Set(arena, out, "cache", SwrProvenance(arena, swr)))
    return result;
  result.payload = out;
  result.outcome = Outcome::kOk;
  return result;
}

// ---- method signatures (MethodSignatureResolver + FourByteClient) ---------------

namespace {
struct SelectorContext {
  const char* selector;
};
bool selector_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  auto* c = static_cast<SelectorContext*>(opaque);
  Value* out = tj::Obj(arena);
  if (!out) { error->Upstream("arena"); return false; }
  const char* s = c->selector;
  bool valid = strlen(s) == 10 && s[0] == '0' && s[1] == 'x';
  for (size_t i = 2; valid && i < 10; ++i) valid = (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f');
  if (!valid) {
    tj::Set(arena, out, "signature", tj::Null(arena));
    *value = out;
    return true;
  }
  static const char* const kBase[] = {"https://www.4byte.directory"};
  Value* path = chains::Format(arena, "/api/v1/signatures/?hex_signature=%s", s);
  chains::Call call;
  call.kind = chains::CallKind::kGet;
  call.urls = kBase;
  call.url_count = 1;
  call.path = path ? path->text : nullptr;
  call.lane = "4byte";
  impl->deps.transport->Run(call, arena);
  if (!call.ok()) {
    // lanes.run + http.json: any failure throws (no failover semantics).
    call.ToError(error);
    if (call.answer()) error->Upstream("4byte %d", call.http_status);
    return false;
  }
  const Value* results = tj::Get(call.value, "results");
  const Value* first = tj::IsArray(results) && results->count ? results->items[0] : nullptr;
  const Value* signature = tj::Get(first, "text_signature");
  tj::Set(arena, out, "signature", tj::IsString(signature) ? tj::Clone(arena, signature) : tj::Null(arena));
  *value = out;
  return true;
}
Revalidation* selector_revalidation(void* opaque, const char* key) {
  auto* c = static_cast<SelectorContext*>(opaque);
  Revalidation* job = new_revalidation(Revalidation::Kind::kMethodSignature, key);
  if (job) snprintf(job->subject.data(), job->subject.capacity(), "%s", c->selector);
  return job;
}
}  // namespace

const char* WalletQueryContext::MethodSignature(const char* selector, Arena& arena) {
  Value* lowered = fortis::codec::provider_text::ToLowerCase(arena, selector, strlen(selector));
  if (!lowered) return nullptr;
  char key[256];
  if (!key_text(key, sizeof(key), "4byte:%s", lowered->text)) return nullptr;
  SelectorContext context{lowered->text};
  SwrFetcher fetcher;
  fetcher.fetch = selector_fetch;
  fetcher.context = &context;
  fetcher.revalidation = selector_revalidation;
  SwrResult swr;
  if (!SwrRead(key, kTtlMethodSignature, fetcher, arena, &swr)) return nullptr;
  const Value* signature = tj::Get(swr.value, "signature");
  return tj::IsString(signature) ? signature->text : nullptr;
}

// ---- Tron token holdings ---------------------------------------------------------------

namespace {
struct HoldingsContext {
  const char* address;
};
bool holdings_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  auto* c = static_cast<HoldingsContext*>(opaque);
  chains::Context ctx = impl->ChainContext(arena);
  return chains::TronscanWalletTokens(ctx, c->address, value, error);
}
Revalidation* holdings_revalidation(void* opaque, const char* key) {
  auto* c = static_cast<HoldingsContext*>(opaque);
  Revalidation* job = new_revalidation(Revalidation::Kind::kTronHoldings, key);
  if (job) snprintf(job->subject.data(), job->subject.capacity(), "%s", c->address);
  return job;
}
}  // namespace

bool ValidTronAddress(const char* address) {
  static const char kAlphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  if (!address || strlen(address) != 34 || address[0] != 'T') return false;
  for (size_t i = 1; i < 34; ++i)
    if (!memchr(kAlphabet, address[i], sizeof(kAlphabet) - 1)) return false;
  return true;
}

bool TronHoldingsCacheKey(const char* address, char* output, size_t capacity) {
  return key_text(output, capacity, "tronhold:%s", address);
}

FacetReply WalletQueryService::TronHoldings(Arena& arena, const char* address) {
  FacetReply result;
  char key[512];
  if (!TronHoldingsCacheKey(address, key, sizeof(key))) return result;
  HoldingsContext context{address};
  SwrFetcher fetcher;
  fetcher.fetch = holdings_fetch;
  fetcher.context = &context;
  fetcher.revalidation = holdings_revalidation;
  SwrResult swr;
  if (!impl_->SwrRead(key, kTtlBalances, fetcher, arena, &swr)) {
    SetError(&result, swr.error);
    return result;
  }
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "data", swr.value)) return result;
  result.payload = out;
  result.outcome = Outcome::kOk;
  return result;
}

// ---- topics ------------------------------------------------------------------------------

void WalletQueryService::RememberAddressTopic(Arena& arena, const char* device_id, const char* network_id, const char* address) {
  if (!device_id || !*device_id) return;
  Network network;
  const char* canonical = address;
  if (impl_->deps.catalog->Resolve(network_id, strlen(network_id), &network)) {
    canonical = Canonical(arena, network, address);
    if (!canonical) return;
  }
  Value* topic = chains::Format(arena, "address:%s:%s", network_id, canonical);
  if (topic) impl_->deps.store->RememberTopic(device_id, topic->text, Now(), arena);
}

void WalletQueryService::RememberTxTopic(Arena& arena, const char* device_id, const char* network_id, const char* hash) {
  if (!device_id || !*device_id) return;
  Value* topic = chains::Format(arena, "tx:%s:%s", network_id, hash);
  if (topic) impl_->deps.store->RememberTopic(device_id, topic->text, Now(), arena);
}

// ---- revalidation dispatch -----------------------------------------------------------

void WalletQueryService::RunRevalidation(Revalidation* job, Arena& arena) {
  Network network;
  const bool resolved = job->network[0] && impl_->deps.catalog->Resolve(job->network.data(), strlen(job->network.data()), &network);
  SwrFetcher fetcher;
  switch (job->kind) {
    case Revalidation::Kind::kBalances: {
      if (!resolved) break;
      BalancesContext context{&network, job->subject.data(), job->tokens.get(), job->token_count};
      fetcher.fetch = balances_fetch;
      fetcher.context = &context;
      impl_->Revalidate(job->key.data(), IsUtxoFamily(network.family) ? kTtlBalancesUtxo : kTtlBalances, fetcher, arena);
      return;
    }
    case Revalidation::Kind::kInsight: {
      if (!resolved) break;
      AddressContext context{&network, job->subject.data()};
      fetcher.fetch = insight_fetch;
      fetcher.context = &context;
      impl_->Revalidate(job->key.data(), kTtlInsight, fetcher, arena);
      return;
    }
    case Revalidation::Kind::kView: {
      if (!resolved) break;
      ViewContext context{&network, job->subject.data()};
      fetcher.fetch = view_fetch;
      fetcher.context = &context;
      fetcher.should_store = view_should_store;
      impl_->Revalidate(job->key.data(), kTtlTxDetail, fetcher, arena);
      return;
    }
    case Revalidation::Kind::kMethodSignature: {
      SelectorContext context{job->subject.data()};
      fetcher.fetch = selector_fetch;
      fetcher.context = &context;
      impl_->Revalidate(job->key.data(), kTtlMethodSignature, fetcher, arena);
      return;
    }
    case Revalidation::Kind::kTronHoldings: {
      HoldingsContext context{job->subject.data()};
      fetcher.fetch = holdings_fetch;
      fetcher.context = &context;
      impl_->Revalidate(job->key.data(), kTtlBalances, fetcher, arena);
      return;
    }
    case Revalidation::Kind::kOverviewTokens:
    case Revalidation::Kind::kHolders:
      if (!resolved) break;
      RunOverviewRevalidation(impl_.get(), job, network, arena);
      return;
  }
  impl_->deps.swr->EndRevalidate(job->key.data(), Now(), true, false);
}

// ---- lifecycle ----------------------------------------------------------------------------

WalletQueryService::WalletQueryService(const FacetDeps& deps) : impl_(fortis::base::New<WalletQueryContext>(deps)) {}

WalletQueryService::~WalletQueryService() {
  impl_.reset();
}

int64_t WalletQueryService::Now() const { return impl_->Now(); }

chains::Context WalletQueryService::ChainContext(Arena& arena) { return impl_->ChainContext(arena); }

}  // namespace fortis::wallet::address
