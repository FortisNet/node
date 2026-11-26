#include "discovery_cache.h"

#include "device_auth.h"

#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace fortis::discovery {

namespace {

struct EnvelopeHeader {
  uint32_t magic = 0x31435344;  // DSC1
  uint32_t body_length = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
  int64_t as_of_ms = 0;
};

}  // namespace

DiscoveryCache::DiscoveryCache(fortis::storage::KvStore* kv, unsigned char* scratch,
                               size_t scratch_capacity, const DiscoveryCacheSeams* seams)
    : kv_(kv), scratch_(scratch), capacity_(scratch_capacity), seams_(seams) {}

int64_t DiscoveryCache::Now() const {
  if (seams_ && seams_->clock) {
    const int64_t fixed = seams_->clock->NowMs();
    if (fixed >= 0) return fixed;
  }
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

double DiscoveryCache::JitterSample() const {
  double sample = 0;
  if (seams_ && seams_->jitter && seams_->jitter->Sample(&sample)) return sample;
  uint32_t bits = 0;
  if (!fortis::auth::SecureRandomBytes(reinterpret_cast<uint8_t*>(&bits), sizeof(bits))) return 0.5;
  return static_cast<double>(bits) / 4294967295.0;
}

DiscoveryCacheEntry DiscoveryCache::Read(const char* key, int64_t now_ms) {
  DiscoveryCacheEntry entry{};
  size_t size = 0;
  const auto status = kv_->GetStatus(key, now_ms, scratch_, capacity_, &size);
  if (status == fortis::storage::KvReadStatus::Missing) return entry;
  EnvelopeHeader header{};
  if (status != fortis::storage::KvReadStatus::Found || size < sizeof(header)) {
    entry.state = DiscoveryCacheState::kError;
    return entry;
  }
  memcpy(&header, scratch_, sizeof(header));
  if (header.magic != EnvelopeHeader{}.magic ||
      header.body_length != size - sizeof(header) ||
      header.stale_until_ms < header.fresh_until_ms) {
    entry.state = DiscoveryCacheState::kError;
    return entry;
  }
  entry.as_of_ms = header.as_of_ms;
  entry.fresh_until_ms = header.fresh_until_ms;
  entry.stale_until_ms = header.stale_until_ms;
  entry.body = scratch_ + sizeof(header);
  entry.body_length = header.body_length;
  entry.state = now_ms < header.fresh_until_ms ? DiscoveryCacheState::kFresh
              : now_ms < header.stale_until_ms ? DiscoveryCacheState::kStale
              : DiscoveryCacheState::kMissing;
  return entry;
}

bool DiscoveryCache::Write(const char* key, const DiscoveryCacheTtl& ttl, int64_t now_ms,
                           const void* body, size_t body_length, int64_t* as_of_ms) {
  EnvelopeHeader header{};
  if (capacity_ < sizeof(header) || (body_length && !body) ||
      body_length > capacity_ - sizeof(header) || body_length > UINT32_MAX) return false;
  const int64_t fresh = OhlcJitteredFreshMs(ttl.fresh_ms, JitterSample());
  header.body_length = static_cast<uint32_t>(body_length);
  header.as_of_ms = now_ms;
  header.fresh_until_ms = now_ms + fresh;
  header.stale_until_ms = now_ms + fresh + ttl.stale_ms;
  const int64_t kv_ttl = fresh + ttl.stale_ms < kDiscoveryKvMaxTtlMs
      ? fresh + ttl.stale_ms : kDiscoveryKvMaxTtlMs;
  // The body may already live in the scratch (a re-store of a read value).
  if (body_length) memmove(scratch_ + sizeof(header), body, body_length);
  memcpy(scratch_, &header, sizeof(header));
  const fortis::storage::KvMutation mutation{key, scratch_,
      static_cast<uint32_t>(sizeof(header) + body_length), now_ms + kv_ttl, false};
  if (as_of_ms) *as_of_ms = now_ms;
  return kv_->Write(&mutation, 1);
}

void DiscoveryCache::PolicyKey(const char* key, char output[kOhlcCachePolicyKeyBytes]) const {
  const size_t length = strlen(key);
  if (length < kOhlcCachePolicyKeyBytes) {
    memcpy(output, key, length + 1);
    return;
  }
  // Long keys (search queries) share the fixed policy table by digest.
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(key), length, digest, 0);
  static const char kHex[] = "0123456789abcdef";
  memcpy(output, "#", 1);
  for (size_t i = 0; i < 32; ++i) {
    output[1 + i * 2] = kHex[digest[i] >> 4];
    output[2 + i * 2] = kHex[digest[i] & 15];
  }
  output[65] = '\0';
}

bool DiscoveryCache::AllowRevalidate(const char* key, int64_t now_ms) {
  char policy_key[kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  return policy_.AllowRevalidate(policy_key, now_ms);
}

void DiscoveryCache::RecordRevalidateFailure(const char* key, int64_t now_ms, bool lane_busy) {
  char policy_key[kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  policy_.RecordRevalidateFailure(policy_key, now_ms, lane_busy);
}

bool DiscoveryCache::MissReplay(const char* key, int64_t now_ms, int* status) {
  char policy_key[kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  return policy_.MissReplay(policy_key, now_ms, status);
}

void DiscoveryCache::RecordMissFailure(const char* key, int64_t now_ms, bool lane_busy,
                                       int status) {
  char policy_key[kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  policy_.RecordMissFailure(policy_key, now_ms, lane_busy, status);
}

bool DiscoveryIsoTimestamp(int64_t epoch_ms, char output[32]) {
  if (epoch_ms < 0) return false;
  const time_t seconds = static_cast<time_t>(epoch_ms / 1000);
  struct tm utc{};
  if (!gmtime_r(&seconds, &utc)) return false;
  char base[24]{};
  if (!strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc)) return false;
  return snprintf(output, 32, "%s.%03dZ", base, static_cast<int>(epoch_ms % 1000)) > 0;
}

}  // namespace fortis::discovery
