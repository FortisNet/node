#include "token_swr.h"

#include "device_auth.h"
#include "kv_store.h"
#include "threading.h"

#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_swr {
namespace {

struct Header {
  uint32_t magic = 0x31575354;  // TSW1
  uint32_t body_length = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
  int64_t as_of_ms = 0;
};

}  // namespace

bool PeekEnvelope(storage::KvStore* kv, const char* key, int64_t now_ms, uint8_t* scratch, size_t capacity,
                  EnvelopeInfo* out) {
  *out = EnvelopeInfo{};
  size_t size = 0;
  Header header{};
  if (!kv || kv->GetStatus(key, now_ms, scratch, capacity, &size) != storage::KvReadStatus::Found || size < sizeof(header))
    return false;
  memcpy(&header, scratch, sizeof(header));
  if (header.magic != Header{}.magic || header.body_length != size - sizeof(header)) return false;
  out->as_of_ms = header.as_of_ms;
  out->fresh_until_ms = header.fresh_until_ms;
  out->stale_until_ms = header.stale_until_ms;
  out->body = scratch + sizeof(header);
  out->body_length = header.body_length;
  return true;
}

Cache::Cache(storage::KvStore* kv, const Seams* seams) : kv_(kv), seams_(seams), mutex_() {}


int64_t Cache::Now() const {
  if (seams_ && seams_->clock) {
    const int64_t fixed = seams_->clock->NowMs();
    if (fixed >= 0) return fixed;
  }
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

double Cache::JitterSample() const {
  double sample = 0;
  if (seams_ && seams_->jitter && seams_->jitter->Sample(&sample)) return sample;
  uint32_t bits = 0;
  if (!auth::SecureRandomBytes(reinterpret_cast<uint8_t*>(&bits), sizeof(bits))) return 0.5;
  return static_cast<double>(bits) / 4294967295.0;
}

State Cache::Read(const char* key, int64_t now_ms, uint8_t* scratch, size_t capacity, tj::Arena& arena,
                  tj::Value** value, int64_t* as_of_ms) {
  *value = nullptr;
  size_t size = 0;
  const storage::KvReadStatus status = kv_->GetStatus(key, now_ms, scratch, capacity, &size);
  Header header{};
  if (status != storage::KvReadStatus::Found || size < sizeof(header)) return State::kMissing;
  memcpy(&header, scratch, sizeof(header));
  if (header.magic != Header{}.magic || header.body_length != size - sizeof(header) ||
      header.stale_until_ms < header.fresh_until_ms)
    return State::kMissing;
  if (now_ms >= header.stale_until_ms) return State::kMissing;
  tj::Value* parsed = tj::Parse(arena, scratch + sizeof(header), header.body_length);
  if (!parsed) return State::kMissing;
  *value = parsed;
  *as_of_ms = header.as_of_ms;
  return now_ms < header.fresh_until_ms ? State::kFresh : State::kStale;
}

bool Cache::Write(const char* key, const Ttl& ttl, int64_t now_ms, const tj::Value* value,
                  uint8_t* scratch, size_t capacity, int64_t* as_of_ms) {
  if (as_of_ms) *as_of_ms = now_ms;
  Header header{};
  if (capacity <= sizeof(header)) return false;
  tj::OutBuffer out{scratch + sizeof(header), capacity - sizeof(header), 0, true};
  if (!tj::Stringify(value, out) || !out.ok) return false;
  const int64_t fresh = discovery::OhlcJitteredFreshMs(ttl.fresh_ms, JitterSample());
  header.body_length = static_cast<uint32_t>(out.used);
  header.as_of_ms = now_ms;
  header.fresh_until_ms = now_ms + fresh;
  header.stale_until_ms = now_ms + fresh + ttl.stale_ms;
  const int64_t kv_ttl = fresh + ttl.stale_ms < kKvMaxTtlMs ? fresh + ttl.stale_ms : kKvMaxTtlMs;
  memcpy(scratch, &header, sizeof(header));
  const storage::KvMutation mutation{key, scratch, static_cast<uint32_t>(sizeof(header) + out.used),
                            now_ms + kv_ttl, false};
  return kv_->Write(&mutation, 1);
}

bool Cache::Invalidate(const char* key) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::UniqueLock lock(mutex_);
  policy_.Invalidate(policy_key);
  lock.Unlock();
  const storage::KvMutation mutation{key, nullptr, 0, 0, true};
  return kv_->Write(&mutation, 1);
}

void Cache::PolicyKey(const char* key, char output[discovery::kOhlcCachePolicyKeyBytes]) const {
  const size_t length = strlen(key);
  if (length < discovery::kOhlcCachePolicyKeyBytes) {
    memcpy(output, key, length + 1);
    return;
  }
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(key), length, digest, 0);
  static const char kHex[] = "0123456789abcdef";
  output[0] = '#';
  for (size_t i = 0; i < 32; ++i) {
    output[1 + i * 2] = kHex[digest[i] >> 4];
    output[2 + i * 2] = kHex[digest[i] & 15];
  }
  output[65] = '\0';
}

bool Cache::BeginRevalidate(const char* key, int64_t now_ms) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::LockGuard lock(mutex_);
  size_t free_slot = kRevalidating;
  for (size_t i = 0; i < kRevalidating; ++i) {
    if (!revalidating_[i][0]) { if (free_slot == kRevalidating) free_slot = i; continue; }
    if (strcmp(revalidating_[i], policy_key) == 0) return false;
  }
  // A full set sheds the revalidation; the stale value is still served.
  const bool allowed = free_slot != kRevalidating && policy_.AllowRevalidate(policy_key, now_ms);
  if (allowed) memcpy(revalidating_[free_slot], policy_key, strlen(policy_key) + 1);
  return allowed;
}

void Cache::EndRevalidate(const char* key, int64_t now_ms, bool failed, bool lane_busy) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::LockGuard lock(mutex_);
  for (size_t i = 0; i < kRevalidating; ++i)
    if (revalidating_[i][0] && strcmp(revalidating_[i], policy_key) == 0) revalidating_[i][0] = '\0';
  if (failed) policy_.RecordRevalidateFailure(policy_key, now_ms, lane_busy);
}

bool Cache::MissHeld(const char* key, int64_t now_ms) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::LockGuard lock(mutex_);
  int status = 0;
  const bool held = policy_.MissReplay(policy_key, now_ms, &status);
  return held;
}

void Cache::HoldMiss(const char* key, int64_t now_ms, bool lane_busy) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::LockGuard lock(mutex_);
  policy_.RecordMissFailure(policy_key, now_ms, lane_busy, 502);
}

bool Cache::MissHeld(const char* key, int64_t now_ms, int* status) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::LockGuard lock(mutex_);
  const bool held = policy_.MissReplay(policy_key, now_ms, status);
  return held;
}

void Cache::HoldMiss(const char* key, int64_t now_ms, bool lane_busy, int status) {
  char policy_key[discovery::kOhlcCachePolicyKeyBytes];
  PolicyKey(key, policy_key);
  fortis::base::LockGuard lock(mutex_);
  policy_.RecordMissFailure(policy_key, now_ms, lane_busy, status);
}

}  // namespace fortis::tokens::token_swr
