#include <cstdio>
#include <algorithm>
#include <memory>
#include <atomic>
#include "price_spot_cache.h"
#include "ownership.h"
#include "price_series_store.h"
#include "threading.h"

#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::prices {
namespace {
constexpr char kKeyPrefix[] = "price/spot/v1/";
constexpr size_t kKeyPrefixBytes = sizeof(kKeyPrefix) - 1;
constexpr size_t kKeyCapacity = kKeyPrefixBytes + 5 + 1 +
                                kPriceSpotMaxSourceIdBytes + 1;
constexpr size_t kProviderBytes = kPriceSpotMaxProviderBytes + 1;
constexpr size_t kEncodedBytes = kPriceSpotEncodedBytes;
constexpr size_t kMutationRecordOverhead = 15;
constexpr uint32_t kMaxKvFrameBytes = 8 * 1024 * 1024;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kPositive = 1;
constexpr uint8_t kNegative = 2;
constexpr uint8_t kHasChange = 1u << 0;
constexpr uint8_t kHasVolume = 1u << 1;
constexpr uint8_t kHasFloor = 1u << 2;
constexpr uint8_t kFloorHasChange = 1u << 3;
constexpr uint8_t kFloorHasVolume = 1u << 4;
constexpr uint8_t kKnownFlags = kHasChange | kHasVolume | kHasFloor |
                                kFloorHasChange | kFloorHasVolume;
static_assert(sizeof(double) == sizeof(uint64_t), "spot format needs binary64");
static_assert(__DBL_MANT_DIG__ == 53 && __DBL_MAX_EXP__ == 1024,
              "spot format requires IEEE-754 binary64");
static_assert(sizeof(kKeyPrefix) - 1 + 5 + 1 + kPriceSpotMaxSourceIdBytes <= 1024,
              "spot keys must fit KvStore's key cap");
static_assert(4 + kPriceSpotMaxBatchItems *
                  (kMutationRecordOverhead + kKeyCapacity - 1 + kEncodedBytes) <
                  kMaxKvFrameBytes,
              "a full spot batch must fit one KvStore frame");

// Store batches share one lock across repository instances. This makes the
// read/merge/one-frame-write sequence atomic with respect to other spot-cache
// batches, while ordinary reads rely on KvStore's own operation mutex.
fortis::base::Mutex g_spot_batch_mutex;

struct Record {
  uint8_t state = 0;
  uint8_t flags = 0;
  uint64_t identity = 0;
  int64_t as_of = 0;
  int64_t fresh_until = 0;
  int64_t stale_until = 0;
  double price = 0;
  double change = 0;
  double volume = 0;
  char provider[kProviderBytes]{};
  int64_t floor_as_of = 0;
  int64_t floor_until = 0;
  double floor_price = 0;
  double floor_change = 0;
  double floor_volume = 0;
  char floor_provider[kProviderBytes]{};
};

struct BatchScratch {
  storage::KvMutation mutations[2 * kPriceSpotMaxBatchItems]{};
  char metadata_keys[kPriceSpotMaxBatchItems][kKeyCapacity + 8]{};
  uint8_t metadata[kPriceSpotMaxBatchItems][kEncodedBytes + 20]{};
  char keys[kPriceSpotMaxBatchItems][kKeyCapacity]{};
  uint8_t values[kPriceSpotMaxBatchItems][kEncodedBytes]{};
};

uint32_t Read32(const uint8_t* input) {
  uint32_t result = 0;
  for (unsigned i = 0; i < 4; ++i)
    result |= static_cast<uint32_t>(input[i]) << (8 * i);
  return result;
}

uint64_t Read64(const uint8_t* input) {
  uint64_t result = 0;
  for (unsigned i = 0; i < 8; ++i)
    result |= static_cast<uint64_t>(input[i]) << (8 * i);
  return result;
}

void Write32(uint8_t* output, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) output[i] = static_cast<uint8_t>(value >> (8 * i));
}

void Write64(uint8_t* output, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) output[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t Crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool ValidFiat(const char* fiat, size_t* length) {
  if (!fiat) return false;
  const size_t n = strnlen(fiat, 6);
  if (n < 3 || n > 5) return false;
  for (size_t i = 0; i < n; ++i)
    if (fiat[i] < 'a' || fiat[i] > 'z') return false;
  if (length) *length = n;
  return true;
}

bool ValidSourceId(const char* source_id, size_t* length) {
  if (!source_id) return false;
  const size_t n = strnlen(source_id, kPriceSpotMaxSourceIdBytes + 1);
  if (!n || n > kPriceSpotMaxSourceIdBytes ||
      source_id[0] == ' ' || source_id[n - 1] == ' ') return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(source_id[i]);
    // Canonical provider ids are opaque and case-sensitive (for example,
    // base58 addresses), but cannot add key path segments or control bytes.
    if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') return false;
  }
  if (length) *length = n;
  return true;
}

bool ValidProvider(const char* provider, size_t* length) {
  if (!provider) return false;
  const size_t n = strnlen(provider, kProviderBytes);
  if (!n || n >= kProviderBytes) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = provider[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  if (length) *length = n;
  return true;
}

bool BuildKey(const char* fiat, size_t fiat_length, const char* source_id,
              size_t source_length, char output[kKeyCapacity]) {
  size_t cursor = 0;
  memcpy(output + cursor, kKeyPrefix, kKeyPrefixBytes);
  cursor += kKeyPrefixBytes;
  memcpy(output + cursor, fiat, fiat_length);
  cursor += fiat_length;
  output[cursor++] = '/';
  memcpy(output + cursor, source_id, source_length);
  cursor += source_length;
  output[cursor] = '\0';
  return cursor < kKeyCapacity;
}

uint64_t IdentityHash(const char* fiat, size_t fiat_length,
                      const char* source_id, size_t source_length) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < fiat_length; ++i) {
    hash ^= static_cast<uint8_t>(fiat[i]);
    hash *= 1099511628211ULL;
  }
  hash ^= 0;
  hash *= 1099511628211ULL;
  for (size_t i = 0; i < source_length; ++i) {
    hash ^= static_cast<uint8_t>(source_id[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

void WriteDouble(uint8_t* output, double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  Write64(output, bits);
}

double ReadDouble(const uint8_t* input) {
  const uint64_t bits = Read64(input);
  double value = 0;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteProvider(uint8_t* output, const char* provider) {
  memset(output, 0, kProviderBytes);
  std::copy_n(provider, strlen(provider), output);
}

bool ReadProvider(const uint8_t* input, char output[kProviderBytes]) {
  const size_t n = strnlen(reinterpret_cast<const char*>(input), kProviderBytes);
  if (!n || n >= kProviderBytes) return false;
  for (size_t i = n + 1; i < kProviderBytes; ++i)
    if (input[i] != 0) return false;
  memcpy(output, input, n);
  output[n] = '\0';
  return ValidProvider(output, nullptr);
}

bool ZeroBytes(const uint8_t* input, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (input[i] != 0) return false;
  return true;
}

void Encode(const Record& record, uint8_t output[kEncodedBytes]) {
  memset(output, 0, kEncodedBytes);
  memcpy(output, "FPSC", 4);
  output[4] = kVersion;
  output[5] = record.state;
  output[6] = record.flags;
  output[7] = 0;
  Write64(output + 8, record.identity);
  Write64(output + 16, static_cast<uint64_t>(record.as_of));
  Write64(output + 24, static_cast<uint64_t>(record.fresh_until));
  Write64(output + 32, static_cast<uint64_t>(record.stale_until));
  WriteDouble(output + 40, record.price);
  WriteDouble(output + 48, record.change);
  WriteDouble(output + 56, record.volume);
  WriteProvider(output + 64, record.provider);
  Write64(output + 96, static_cast<uint64_t>(record.floor_as_of));
  Write64(output + 104, static_cast<uint64_t>(record.floor_until));
  WriteDouble(output + 112, record.floor_price);
  WriteDouble(output + 120, record.floor_change);
  WriteDouble(output + 128, record.floor_volume);
  WriteProvider(output + 136, record.floor_provider);
  Write32(output + 168, Crc32(output, 168));
}

bool Decode(const uint8_t* input, size_t length, uint64_t expected_identity,
            Record* output) {
  if (!input || !output || length != kEncodedBytes ||
      memcmp(input, "FPSC", 4) != 0 || input[4] != kVersion || input[7] != 0 ||
      (input[6] & ~kKnownFlags) != 0 || Read32(input + 168) != Crc32(input, 168) ||
      Read64(input + 8) != expected_identity) return false;
  Record record{};
  record.state = input[5];
  record.flags = input[6];
  record.identity = Read64(input + 8);
  record.as_of = static_cast<int64_t>(Read64(input + 16));
  record.fresh_until = static_cast<int64_t>(Read64(input + 24));
  record.stale_until = static_cast<int64_t>(Read64(input + 32));
  record.price = ReadDouble(input + 40);
  record.change = ReadDouble(input + 48);
  record.volume = ReadDouble(input + 56);
  if (!ReadProvider(input + 64, record.provider)) return false;
  record.floor_as_of = static_cast<int64_t>(Read64(input + 96));
  record.floor_until = static_cast<int64_t>(Read64(input + 104));
  record.floor_price = ReadDouble(input + 112);
  record.floor_change = ReadDouble(input + 120);
  record.floor_volume = ReadDouble(input + 128);
  const bool has_floor = (record.flags & kHasFloor) != 0;
  if (has_floor) {
    if (!ReadProvider(input + 136, record.floor_provider)) return false;
    if (record.floor_until <= record.floor_as_of || !isfinite(record.floor_price) ||
        ((record.flags & kFloorHasChange) && !isfinite(record.floor_change)) ||
        ((record.flags & kFloorHasVolume) && !isfinite(record.floor_volume)))
      return false;
  } else if (record.floor_as_of || record.floor_until || record.floor_price != 0 ||
             record.floor_change != 0 || record.floor_volume != 0 ||
             (record.flags & (kFloorHasChange | kFloorHasVolume)) ||
             !ZeroBytes(input + 136, kProviderBytes)) {
    return false;
  }
  if (record.as_of < 0 || record.fresh_until <= record.as_of ||
      record.stale_until < record.fresh_until) return false;
  if (record.state == kPositive) {
    if (!has_floor || record.stale_until - record.fresh_until != kPriceSpotStaleMs ||
        !isfinite(record.price) ||
        ((record.flags & kHasChange) && !isfinite(record.change)) ||
        ((record.flags & kHasVolume) && !isfinite(record.volume)))
      return false;
  } else if (record.state == kNegative) {
    if (record.stale_until != record.fresh_until ||
        (record.flags & (kHasChange | kHasVolume)) || record.price != 0 ||
        record.change != 0 || record.volume != 0 ||
        !ZeroBytes(input + 40, 24)) return false;
  } else {
    return false;
  }
  *output = record;
  return true;
}

void SetFloor(Record* record, const Record& previous) {
  record->flags |= kHasFloor;
  if (previous.flags & kFloorHasChange) record->flags |= kFloorHasChange;
  if (previous.flags & kFloorHasVolume) record->flags |= kFloorHasVolume;
  record->floor_as_of = previous.floor_as_of;
  record->floor_until = previous.floor_until;
  record->floor_price = previous.floor_price;
  record->floor_change = previous.floor_change;
  record->floor_volume = previous.floor_volume;
  memcpy(record->floor_provider, previous.floor_provider, sizeof(record->floor_provider));
}

void SetValue(const Record& record, const char* source_id, bool floor,
              PriceSpotValue* output) {
  *output = PriceSpotValue{};
  memcpy(output->source_id, source_id, strlen(source_id) + 1);
  if (floor) {
    memcpy(output->provider, record.floor_provider, sizeof(output->provider));
    output->price = record.floor_price;
    output->change24h_pct = record.floor_change;
    output->volume24h = record.floor_volume;
    output->has_change24h_pct = (record.flags & kFloorHasChange) != 0;
    output->has_volume24h = (record.flags & kFloorHasVolume) != 0;
    output->as_of_ms = record.floor_as_of;
  } else {
    memcpy(output->provider, record.provider, sizeof(output->provider));
    output->price = record.price;
    output->change24h_pct = record.change;
    output->volume24h = record.volume;
    output->has_change24h_pct = (record.flags & kHasChange) != 0;
    output->has_volume24h = (record.flags & kHasVolume) != 0;
    output->as_of_ms = record.as_of;
    output->fresh_until_ms = record.fresh_until;
    output->stale_until_ms = record.stale_until;
  }
}

bool MakeIdentity(const char* fiat, const char* source_id, size_t* fiat_length,
                  size_t* source_length, char key[kKeyCapacity], uint64_t* hash) {
  if (!ValidFiat(fiat, fiat_length) || !ValidSourceId(source_id, source_length) ||
      !BuildKey(fiat, *fiat_length, source_id, *source_length, key)) return false;
  *hash = IdentityHash(fiat, *fiat_length, source_id, *source_length);
  return true;
}

bool AddTtl(int64_t now_ms, int64_t ttl, int64_t* expires) {
  if (now_ms < 0 || ttl <= 0 || now_ms > INT64_MAX - ttl) return false;
  *expires = now_ms + ttl;
  return true;
}

}  // namespace

// Fixed, direct-mapped storage. The full canonical key disambiguates
// collisions. The generation fences reads across durable-store recovery.
struct PriceSpotCache::Memory {
  static constexpr size_t kSlots = 4096;
  struct Entry {
    uint64_t epoch = 0;
    int64_t expires = 0;
    int64_t received = 0;
    int64_t observed = 0;
    char key[kKeyCapacity]{};
    uint8_t bytes[kEncodedBytes]{};
  };
  fortis::base::Mutex mutex;
  uint64_t revision = 0;
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> contract_hits{0};
  Entry entries[kSlots]{};

  Entry& Slot(uint64_t hash) { return entries[hash % kSlots]; }
  void Put(uint64_t hash, uint64_t epoch, const char* key, const void* bytes,
           int64_t expiry, int64_t received, int64_t observed) {
    auto& entry = Slot(hash);
    entry.epoch = epoch;
    entry.expires = expiry;
    entry.received = received;
    entry.observed = observed;
    std::snprintf(entry.key, sizeof(entry.key), "%s", key);
    memcpy(entry.bytes, bytes, kEncodedBytes);
  }
};

void PriceSpotCache::MemoryDeleter::operator()(Memory* memory) const { fortis::base::Delete(memory); }

PriceSpotCache::~PriceSpotCache() {
  if (!memory_) return;

  memory_.reset();
}

bool PriceSpotCache::EnableMemory() {
  if (memory_) return true;
  memory_.reset(fortis::base::New<Memory>());
  return memory_ != nullptr;
}

uint64_t PriceSpotCache::MemoryHits() const {
  return memory_ ? (memory_->hits).load(std::memory_order_relaxed) : 0;
}

uint64_t PriceSpotCache::ContractMemoryHits() const {
  return memory_ ? (memory_->contract_hits).load(std::memory_order_relaxed) : 0;
}

PriceSpotLookup PriceSpotCache::Lookup(const char* fiat, const char* source_id,
                                      int64_t now_ms, bool include_metadata) const {
  PriceSpotLookup result{};
  size_t fiat_length = 0, source_length = 0;
  char key[kKeyCapacity];
  uint64_t identity = 0;
  if (now_ms < 0 || !MakeIdentity(fiat, source_id, &fiat_length, &source_length,
                                  key, &identity)) {
    result.state = PriceSpotLookupState::kInvalidKey;
    return result;
  }
  uint8_t bytes[kEncodedBytes];
  size_t length = 0;
  uint64_t revision = 0;
  const uint64_t epoch = store_.CacheEpoch();
  int64_t received = 0, observed = 0;
  bool from_memory = false;
  if (memory_ && epoch) {
    fortis::base::LockGuard lock(memory_->mutex);
    const auto& entry = memory_->Slot(identity);
    if (lock.locked()) revision = memory_->revision;
    if (lock.locked() && entry.epoch == epoch && (!entry.expires || now_ms < entry.expires) &&
        strcmp(entry.key, key) == 0 && store_.CacheEpoch() == epoch) {
      memcpy(bytes, entry.bytes, sizeof(bytes));
      received = entry.received;
      observed = entry.observed;
      from_memory = true;
      (memory_->hits).fetch_add(1, std::memory_order_relaxed);
      // Canonical contract and chain-token IDs carry a namespace separator.
      if (strchr(source_id, ':'))
        (memory_->contract_hits).fetch_add(1, std::memory_order_relaxed);
    }
  }
  const storage::KvReadStatus read = from_memory ? storage::KvReadStatus::Found
      : store_.GetStatus(key, now_ms, bytes, sizeof(bytes), &length);
  if (read == storage::KvReadStatus::Missing || read == storage::KvReadStatus::IoError) {
    result.state = read == storage::KvReadStatus::Missing ? PriceSpotLookupState::kMissing : PriceSpotLookupState::kIoError;
    result.revalidate_needed = true;
    return result;
  }
  Record record{};
  if (read != storage::KvReadStatus::Found ||
      !Decode(bytes, from_memory ? sizeof(bytes) : length, identity, &record)) {
    result.state = PriceSpotLookupState::kCorrupt;
    result.revalidate_needed = true;
    return result;
  }
  if (!from_memory && (include_metadata || memory_) && record.state == kPositive) {
    char meta_key[kKeyCapacity + 8];
    snprintf(meta_key, sizeof(meta_key), "price/spot/meta/v1/%s/%s", fiat, source_id);
    uint8_t meta[kEncodedBytes + 20];
    size_t meta_length = 0;
    if (store_.GetStatus(meta_key, now_ms, meta, sizeof(meta), &meta_length) == storage::KvReadStatus::Found &&
        meta_length == sizeof(meta) && memcmp(meta, bytes, kEncodedBytes) == 0 &&
        Read32(meta + kEncodedBytes + 16) == Crc32(meta, kEncodedBytes + 16)) {
      received = static_cast<int64_t>(Read64(meta + kEncodedBytes));
      observed = static_cast<int64_t>(Read64(meta + kEncodedBytes + 8));
    }
  }
  if (memory_ && !from_memory && epoch) {
    fortis::base::LockGuard lock(memory_->mutex);
    if (lock.locked() && epoch == store_.CacheEpoch() && revision == memory_->revision) {
      const int64_t expiry = record.flags & kHasFloor ? record.floor_until : record.fresh_until;
      memory_->Put(identity, epoch, key, bytes, expiry, received, observed);
    }
  }
  if (record.state == kNegative && now_ms < record.fresh_until) {
    result.state = PriceSpotLookupState::kNegative;
    memcpy(result.value.source_id, source_id, source_length + 1);
    memcpy(result.value.provider, record.provider, sizeof(result.value.provider));
    result.value.as_of_ms = record.as_of;
    result.value.fresh_until_ms = record.fresh_until;
    result.value.stale_until_ms = record.stale_until;
    return result;  // A fresh provider negative suppresses an older floor.
  }
  if (record.state == kPositive && now_ms < record.fresh_until) {
    result.state = PriceSpotLookupState::kFresh;
    result.cache_hit = true;
    SetValue(record, source_id, false, &result.value);
    if (include_metadata) {
      result.value.received_at_ms = received;
      result.value.provider_observed_at_ms = observed;
    }
    return result;
  }
  if (record.state == kPositive && now_ms < record.stale_until) {
    result.state = PriceSpotLookupState::kStale;
    result.cache_hit = true;
    result.stale = true;
    result.revalidate_needed = true;
    result.has_floor_candidate = true;
    SetValue(record, source_id, false, &result.value);
    if (include_metadata) {
      result.value.received_at_ms = received;
      result.value.provider_observed_at_ms = observed;
    }
    return result;
  }
  result.revalidate_needed = true;
  if ((record.flags & kHasFloor) && record.floor_until > now_ms) {
    result.state = PriceSpotLookupState::kExpired;
    result.has_floor_candidate = true;
    SetValue(record, source_id, true, &result.value);
    return result;
  }
  result.state = PriceSpotLookupState::kMissing;
  return result;
}

PriceSpotStoreStatus PriceSpotCache::StoreProviderBatch(
    const char* fiat, const char* provider,
    const PriceSpotQuoteInput* quotes, size_t quote_count,
    const char* const* unknown_source_ids, size_t unknown_count,
    bool provider_answered, int64_t now_ms) {
  return StoreProviderBatchInternal(nullptr, fiat, provider, quotes,
      quote_count, unknown_source_ids, unknown_count, provider_answered,
      now_ms, nullptr);
}

PriceSpotStoreStatus PriceSpotCache::StoreProviderBatchWithSeries(
    PriceSeriesStore& series, const char* fiat, const char* provider,
    const PriceSpotQuoteInput* quotes, size_t quote_count,
    const char* const* unknown_source_ids, size_t unknown_count,
    bool provider_answered, int64_t now_ms,
    PriceSeriesStoreStatus* series_status) {
  if (&series.store_ != &store_) {
    if (series_status) *series_status = PriceSeriesStoreStatus::kInvalidArguments;
    return PriceSpotStoreStatus::kInvalidArguments;
  }
  return StoreProviderBatchInternal(&series, fiat, provider, quotes,
      quote_count, unknown_source_ids, unknown_count, provider_answered,
      now_ms, series_status);
}

PriceSpotStoreStatus PriceSpotCache::StoreProviderBatchInternal(
    PriceSeriesStore* series, const char* fiat, const char* provider,
    const PriceSpotQuoteInput* quotes, size_t quote_count,
    const char* const* unknown_source_ids, size_t unknown_count,
    bool provider_answered, int64_t now_ms,
    PriceSeriesStoreStatus* series_status) {
  if (series && series_status)
    *series_status = PriceSeriesStoreStatus::kNoItems;  // not reached yet
  if (!provider_answered) return PriceSpotStoreStatus::kTransportFailureNoWrite;
  size_t fiat_length = 0, provider_length = 0;
  if (now_ms < 0 || !ValidFiat(fiat, &fiat_length) ||
      !ValidProvider(provider, &provider_length) ||
      (quote_count && !quotes) || (unknown_count && !unknown_source_ids) ||
      quote_count > kPriceSpotMaxBatchItems ||
      unknown_count > kPriceSpotMaxBatchItems - quote_count)
    return PriceSpotStoreStatus::kInvalidArguments;
  const size_t item_count = quote_count + unknown_count;
  if (!item_count) return PriceSpotStoreStatus::kNoItems;

  for (size_t i = 0; i < quote_count; ++i) {
    size_t source_length = 0;
    int64_t floor_until = 0;
    const uint32_t fresh_ms = quotes[i].fresh_ms ? quotes[i].fresh_ms : kPriceSpotDefaultFreshMs;
    if (!ValidSourceId(quotes[i].source_id, &source_length) ||
        !isfinite(quotes[i].price) ||
        (quotes[i].has_change24h_pct && !isfinite(quotes[i].change24h_pct)) ||
        (quotes[i].has_volume24h && !isfinite(quotes[i].volume24h)) ||
        quotes[i].as_of_ms < 0 || quotes[i].as_of_ms > now_ms ||
        fresh_ms > kPriceSpotMaxFreshMs ||
        !AddTtl(now_ms, kPriceSpotFloorMs, &floor_until) ||
        now_ms > INT64_MAX - static_cast<int64_t>(fresh_ms) - kPriceSpotStaleMs)
      return PriceSpotStoreStatus::kInvalidArguments;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(quotes[j].source_id, quotes[i].source_id) == 0)
        return PriceSpotStoreStatus::kInvalidArguments;
    for (size_t j = 0; j < unknown_count; ++j)
      if (unknown_source_ids[j] && strcmp(unknown_source_ids[j], quotes[i].source_id) == 0)
        return PriceSpotStoreStatus::kInvalidArguments;
  }
  for (size_t i = 0; i < unknown_count; ++i) {
    size_t source_length = 0;
    int64_t negative_until = 0;
    if (!ValidSourceId(unknown_source_ids[i], &source_length) ||
        !AddTtl(now_ms, kPriceSpotNegativeMs, &negative_until))
      return PriceSpotStoreStatus::kInvalidArguments;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(unknown_source_ids[j], unknown_source_ids[i]) == 0)
        return PriceSpotStoreStatus::kInvalidArguments;
  }

  fortis::base::LockGuard batch_lock(g_spot_batch_mutex);
  if (!batch_lock.locked()) return PriceSpotStoreStatus::kWriteFailure;
  // Freed on every return below, still under the batch lock.
  std::unique_ptr<BatchScratch, fortis::base::ObjectDeleter<BatchScratch>> scratch(fortis::base::New<BatchScratch>());
  if (!scratch) return PriceSpotStoreStatus::kAllocationFailure;

  size_t metadata_count = 0;
  for (size_t i = 0; i < item_count; ++i) {
    const bool positive = i < quote_count;
    const char* source_id = positive ? quotes[i].source_id : unknown_source_ids[i - quote_count];
    size_t source_length = 0;
    (void)ValidSourceId(source_id, &source_length);
    char* key = scratch->keys[i];
    if (!BuildKey(fiat, fiat_length, source_id, source_length, key)) {
      return PriceSpotStoreStatus::kInvalidArguments;
    }
    Record record{};
    record.identity = IdentityHash(fiat, fiat_length, source_id, source_length);

    // A provider negative replaces the envelope but keeps a still-live floor.
    uint8_t previous_bytes[kEncodedBytes];
    size_t previous_length = 0;
    const storage::KvReadStatus read = store_.GetStatus(key, now_ms, previous_bytes,
                                               sizeof(previous_bytes), &previous_length);
    if (read == storage::KvReadStatus::IoError) {
      return PriceSpotStoreStatus::kReadFailure;
    }
    if (read == storage::KvReadStatus::BufferTooSmall) {
      return PriceSpotStoreStatus::kReadFailure;
    }
    Record previous{};
    bool keep_previous_floor = false;
    if (read == storage::KvReadStatus::Found) {
      if (!Decode(previous_bytes, previous_length, record.identity, &previous)) {
        // Do not let a negative response erase an unreadable durable floor.
        return PriceSpotStoreStatus::kReadFailure;
      }
      if ((previous.flags & kHasFloor) && previous.floor_until > now_ms) {
        SetFloor(&record, previous);
        keep_previous_floor = true;
      }
    }

    int64_t key_expiry = 0;
    record.provider[provider_length] = '\0';
    memcpy(record.provider, provider, provider_length);
    if (positive) {
      const PriceSpotQuoteInput& quote = quotes[i];
      const uint32_t fresh_ms = quote.fresh_ms ? quote.fresh_ms : kPriceSpotDefaultFreshMs;
      record.state = kPositive;
      record.flags |= kHasFloor;
      if (!keep_previous_floor)
        record.flags &= static_cast<uint8_t>(~(kFloorHasChange | kFloorHasVolume));
      if (quote.has_change24h_pct) record.flags |= kHasChange;
      if (quote.has_volume24h) record.flags |= kHasVolume;
      record.as_of = quote.as_of_ms;
      record.fresh_until = now_ms + fresh_ms;
      record.stale_until = record.fresh_until + kPriceSpotStaleMs;
      record.price = quote.price;
      record.change = quote.has_change24h_pct ? quote.change24h_pct : 0;
      record.volume = quote.has_volume24h ? quote.volume24h : 0;
      if (!keep_previous_floor || quote.as_of_ms >= record.floor_as_of) {
        record.flags &= static_cast<uint8_t>(~(kFloorHasChange | kFloorHasVolume));
        if (quote.has_change24h_pct) record.flags |= kFloorHasChange;
        if (quote.has_volume24h) record.flags |= kFloorHasVolume;
        memcpy(record.floor_provider, provider, provider_length);
        record.floor_as_of = quote.as_of_ms;
        AddTtl(now_ms, kPriceSpotFloorMs, &record.floor_until);
        record.floor_price = quote.price;
        record.floor_change = record.change;
        record.floor_volume = record.volume;
      }
      key_expiry = record.floor_until;
    } else {
      record.state = kNegative;
      record.as_of = now_ms;
      AddTtl(now_ms, kPriceSpotNegativeMs, &record.fresh_until);
      record.stale_until = record.fresh_until;
      key_expiry = record.flags & kHasFloor ? record.floor_until : record.fresh_until;
    }

    Encode(record, scratch->values[i]);
    scratch->mutations[i] = {key, scratch->values[i],
                             static_cast<uint32_t>(kEncodedBytes), key_expiry, false};
    // Preserve the v1 envelope for rollback compatibility. The sidecar is in
    // the same KV frame and contains the entire matching envelope. A reader
    // racing another commit drops metadata on mismatch, never misattributes it.
    if (!positive || quotes[i].provider_observed_at_ms <= 0 || quotes[i].provider_observed_at_ms > now_ms) continue;
    char* meta_key = scratch->metadata_keys[i];
    snprintf(meta_key, sizeof(scratch->metadata_keys[i]), "price/spot/meta/v1/%s/%s", fiat,
             positive ? quotes[i].source_id : unknown_source_ids[i - quote_count]);
    uint8_t* meta = scratch->metadata[i];
    memcpy(meta, scratch->values[i], kEncodedBytes);
    Write64(meta + kEncodedBytes, static_cast<uint64_t>(now_ms));
    const int64_t observed = positive ? quotes[i].provider_observed_at_ms : 0;
    Write64(meta + kEncodedBytes + 8, static_cast<uint64_t>(observed > 0 && observed <= now_ms ? observed : 0));
    Write32(meta + kEncodedBytes + 16, Crc32(meta, kEncodedBytes + 16));
    scratch->mutations[item_count + metadata_count++] = {meta_key, meta,
        static_cast<uint32_t>(kEncodedBytes + 20), key_expiry, false};
  }

  const uint64_t write_epoch = memory_ ? store_.CacheEpoch() : 0;
  bool stored = false;
  if (series) {
    const PriceSeriesStoreStatus result = series->RecordSpotsWithAdditional(
        fiat, provider, quotes, quote_count, now_ms, scratch->mutations,
        item_count + metadata_count);
    if (series_status) *series_status = result;
    stored = result == PriceSeriesStoreStatus::kStored ||
             result == PriceSeriesStoreStatus::kNoItems;
    if (!stored) {
      PriceSpotStoreStatus mapped = PriceSpotStoreStatus::kWriteFailure;
      if (result == PriceSeriesStoreStatus::kInvalidArguments)
        mapped = PriceSpotStoreStatus::kInvalidArguments;
      else if (result == PriceSeriesStoreStatus::kAllocationFailure)
        mapped = PriceSpotStoreStatus::kAllocationFailure;
      else if (result == PriceSeriesStoreStatus::kReadFailure)
        mapped = PriceSpotStoreStatus::kReadFailure;
      return mapped;
    }
  } else {
    stored = store_.Write(scratch->mutations, item_count + metadata_count);
  }
  if (stored && write_epoch && store_.CacheEpoch() == write_epoch) {
    fortis::base::LockGuard lock(memory_->mutex);
    ++memory_->revision;
    for (size_t i = 0; i < item_count; ++i) {
      const char* id = i < quote_count ? quotes[i].source_id : unknown_source_ids[i - quote_count];
      const int64_t observed = i < quote_count ? quotes[i].provider_observed_at_ms : 0;
      const bool has_metadata = observed > 0 && observed <= now_ms;
      const uint64_t identity = IdentityHash(fiat, fiat_length, id, strlen(id));
      memory_->Put(identity, write_epoch, scratch->keys[i], scratch->values[i],
                   scratch->mutations[i].expires_at_ms, has_metadata ? now_ms : 0,
                   has_metadata ? observed : 0);
    }
  }
  return stored ? PriceSpotStoreStatus::kStored : PriceSpotStoreStatus::kWriteFailure;
}

}  // namespace fortis::prices
