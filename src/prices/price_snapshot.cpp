#include <algorithm>
#include "price_snapshot.h"
#include "price_core_ids.h"
#include "device_enrollment.h"
#include "threading.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

namespace fortis::prices {

namespace {
int64_t now_ms() {
  timespec now{}; clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
constexpr int64_t kRefreshMs = 1000;
// Snapshot accepts JSON only. Other routes retain their protobuf negotiation.
bool accepts_json(const char* accept) {
  if (!accept || !*accept) return true;
  int specificity = -1; double quality = 0;
  for (const char* at = accept; *at;) {
    const char* end = strchr(at, ','); if (!end) end = at + strlen(at);
    while (at < end && (*at == ' ' || *at == '\t')) ++at;
    const char* semi = at < end ? static_cast<const char*>(memchr(at, ';', end - at)) : nullptr;
    const char* type_end = semi ? semi : end;
    while (type_end > at && (type_end[-1] == ' ' || type_end[-1] == '\t')) --type_end;
    int rank = -1;
    if (type_end - at == 16 && strncasecmp(at, "application/json", 16) == 0) rank = 2;
    else if (type_end - at == 13 && strncasecmp(at, "application/*", 13) == 0) rank = 1;
    else if (type_end - at == 3 && memcmp(at, "*/*", 3) == 0) rank = 0;
    double q = 1;
    for (const char* p = semi; p && p < end;) {
      ++p; while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (end - p >= 2 && (p[0] == 'q' || p[0] == 'Q') && p[1] == '=') {
        char* after = nullptr; q = strtod(p + 2, &after);
        if (after == p + 2 || after > end || q < 0 || q > 1) q = 0;
      }
      p = static_cast<const char*>(memchr(p, ';', end - p));
    }
    if (rank > specificity) { specificity = rank; quality = q; }
    else if (rank == specificity && q > quality) quality = q;
    at = *end ? end + 1 : end;
  }
  return specificity >= 0 && quality > 0;
}
bool matches_etag(const char* header, const char* etag) {
  if (!header) return false;
  for (const char* at = header; *at;) {
    const char* end = strchr(at, ','); if (!end) end = at + strlen(at);
    while (at < end && (*at == ' ' || *at == '\t')) ++at;
    while (end > at && (end[-1] == ' ' || end[-1] == '\t')) --end;
    if (end - at == 1 && *at == '*') return true;
    if (end - at > 2 && at[0] == 'W' && at[1] == '/') at += 2;
    if (static_cast<size_t>(end - at) == strlen(etag) && memcmp(at, etag, end - at) == 0) return true;
    const char* comma = strchr(end, ','); if (!comma) break; at = comma + 1;
  }
  return false;
}
}

bool PriceSnapshotPublisher::Rebuild(int64_t now) {
  if (now < 0) return false;
  PriceSnapshotGeneration generation;
  auto& bytes = generation.body;
  size_t used = 0;
  auto append = [&](const char* text) {
    const size_t n = strlen(text);
    if (n >= sizeof(bytes) - used) return false;
    memcpy(bytes + used, text, n); used += n; return true;
  };
  char line[768];
  // Copy transitions each quote to stale in memory. Only the hard expiry
  // requires a new generation, so a late publisher cannot interrupt a
  // fresh-to-stale transition or cause a stale quote to be reported fresh.
  int64_t valid_until = INT64_MAX;
  snprintf(line, sizeof(line), "{\"schemaVersion\":1,\"fiat\":\"usd\",\"quotes\":[");
  if (!append(line)) return false;
  bool first = true;
  bool missing[fortis::prices::kPriceCoreCount]{};
  size_t available = 0;
  for (size_t i = 0; i < fortis::prices::kPriceCoreCount; ++i) {
    const auto quote = cache_.Lookup("usd", fortis::prices::kPriceCoreIds[i], now, true);
    if (quote.state == fortis::prices::PriceSpotLookupState::kIoError) return false;
    if ((quote.state != fortis::prices::PriceSpotLookupState::kFresh &&
         quote.state != fortis::prices::PriceSpotLookupState::kStale) || quote.value.price <= 0) {
      missing[i] = true; continue;
    }
    const auto& v = quote.value;
    constexpr int64_t kObservationMaxAgeMs = 300000;
    const int64_t observed_at = v.provider_observed_at_ms > 0
        ? v.provider_observed_at_ms : v.as_of_ms;
    const bool stale = quote.stale || now - observed_at >= kObservationMaxAgeMs;
    const int64_t observation_until = observed_at + kObservationMaxAgeMs;
    const int64_t fresh_until = observation_until < v.fresh_until_ms ? observation_until : v.fresh_until_ms;
    if (v.stale_until_ms < valid_until) valid_until = v.stale_until_ms;
    char observed[32], received[32];
    strcpy(observed, "null"); strcpy(received, "null");
    if (v.provider_observed_at_ms > 0) snprintf(observed, sizeof(observed), "%lld", static_cast<long long>(v.provider_observed_at_ms));
    if (v.received_at_ms > 0) {
      snprintf(received, sizeof(received), "%lld", static_cast<long long>(v.received_at_ms));
    }
    // The cache only accepts provider identifiers matching [a-z0-9-]+.
    snprintf(line, sizeof(line), "%s{\"id\":\"%s\",\"price\":%.17g,\"source\":\"%s\",\"receivedAtMs\":%s,\"providerObservedAtMs\":%s,\"cacheAsOfMs\":%lld,\"freshUntilMs\":%lld,\"stale\":%s}",
        first ? "" : ",", fortis::prices::kPriceCoreIds[i], v.price, v.provider, received, observed, static_cast<long long>(v.as_of_ms), static_cast<long long>(v.fresh_until_ms), stale ? "true" : "false");
    if (!append(line)) return false;
    if (!stale) generation.freshness[generation.freshness_count++] = {used - (sizeof("false}") - 1), fresh_until};
    first = false; ++available;
  }
  if (!append("],\"missing\":[")) return false;
  first = true;
  for (size_t i = 0; i < fortis::prices::kPriceCoreCount; ++i) if (missing[i]) {
    snprintf(line, sizeof(line), "%s\"%s\"", first ? "" : ",", fortis::prices::kPriceCoreIds[i]);
    if (!append(line)) return false;
    first = false;
  }
  snprintf(line, sizeof(line), "],\"coverage\":{\"available\":%zu,\"requested\":%zu}}", available, fortis::prices::kPriceCoreCount);
  if (!append(line)) return false;
  generation.length = used;
  generation.sequence = sequence_ + 1;
  generation.built_at_ms = now;
  generation.valid_until_ms = valid_until;
  if (!view_.Publish(generation, now)) return false;
  sequence_ = generation.sequence;
  next_boundary_ms_ = valid_until;
  return true;
}

bool PriceSnapshotView::Ready(int64_t now) const {
  return current_.length && now >= current_.built_at_ms && now < current_.valid_until_ms;
}

bool PriceSnapshotView::Publish(const PriceSnapshotGeneration& generation, int64_t now) {
  if (!generation.length || generation.length > sizeof(generation.body) ||
      generation.built_at_ms < 0 || generation.built_at_ms > now ||
      now >= generation.valid_until_ms || !generation.sequence ||
      generation.freshness_count > fortis::prices::kPriceCoreCount) return false;
  for (size_t i = 0; i < generation.freshness_count; ++i) {
    const auto& entry = generation.freshness[i];
    if (generation.length < 5 || entry.offset > generation.length - 5 ||
        memcmp(generation.body + entry.offset, "false", 5) != 0) return false;
  }
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < generation.length; ++i) {
    hash ^= generation.body[i];
    hash *= 1099511628211ULL;
  }
  fortis::base::LockGuard lock(mutex_);
  const bool newer = generation.sequence > current_.sequence &&
                     generation.built_at_ms >= current_.built_at_ms;
  if (newer) {
    current_ = generation;
    body_hash_ = hash;
  }
  return newer;
}

bool PriceSnapshotView::Copy(int64_t now, uint8_t* body, size_t capacity, size_t* length,
                             char* etag, size_t etag_capacity) {
  fortis::base::LockGuard lock(mutex_);
  const bool fresh = Ready(now);
  const bool copied = fresh && capacity >= current_.length && etag_capacity >= sizeof("\"px1-0000000000000000-00\"");
  if (copied) {
    memcpy(body, current_.body, current_.length);
    *length = current_.length;
    unsigned stale_mask = 0;
    for (size_t i = 0; i < current_.freshness_count; ++i) {
      const auto& entry = current_.freshness[i];
      if (now < entry.until_ms) continue;
      std::copy_n("true ", 5, body + entry.offset);
      stale_mask |= 1u << i;
    }
    snprintf(etag, etag_capacity, "\"px1-%016llx-%02x\"",
             static_cast<unsigned long long>(body_hash_), stale_mask);
  }
  if (!fresh) ++unavailable_;
  return copied;
}

PriceSnapshotStatus PriceSnapshotView::Status(int64_t now) {
  fortis::base::LockGuard lock(mutex_);
  const PriceSnapshotStatus out{current_.sequence, unavailable_,
      current_.sequence && now >= current_.built_at_ms ? now - current_.built_at_ms : 0, Ready(now)};
  return out;
}

bool PriceSnapshotPublisher::Start() {
  if (started_) return false;
  Rebuild(now_ms());  // A cold or temporarily unavailable store can recover on the worker.
  stopping_ = false;
  if (!thread_.Start(Run, this, 256 * 1024)) return false;
  started_ = true; return true;
}
void PriceSnapshotPublisher::Stop() {
  if (!started_) return;
  fortis::base::UniqueLock lock(mutex_); stopping_ = true; changed_.Signal(); lock.Unlock();
  thread_.Join(); started_ = false;
}
void PriceSnapshotPublisher::Run(PriceSnapshotPublisher* snapshot) {
  bool published = snapshot->next_boundary_ms_ > 0;
  for (;;) {
    fortis::base::UniqueLock lock(snapshot->mutex_);
    const int64_t now = now_ms();
    int64_t wake_ms = now + kRefreshMs;
    if (published && snapshot->next_boundary_ms_ < wake_ms) wake_ms = snapshot->next_boundary_ms_;
    timespec wake{static_cast<time_t>(wake_ms / 1000),
                  static_cast<long>((wake_ms % 1000) * 1000000)};
    if (!snapshot->stopping_) snapshot->changed_.WaitUntilLocked(snapshot->mutex_, wake);
    const bool stop = snapshot->stopping_;
    lock.Unlock();
    if (stop) return;
    published = snapshot->Rebuild(now_ms());
  }
}
void PriceSnapshotRoute::Handle(int client, const http::HttpRequest& request, bool head, void* context) {
  auto* self = static_cast<PriceSnapshotRoute*>(context);
  http::HttpResponse response{};
  response.cache_control = "private, no-cache";
  response.vary_accept = true;
  auto fail = [&](int status, const char* reason, const char* body) {
    response.status = status; response.reason = reason;
    response.body = reinterpret_cast<const uint8_t*>(body); response.body_length = strlen(body);
    response.cache_control = "no-store";
    WriteHttpResponse(client, response, head);
  };
  // The fixed HTTP pool already bounds synchronous authorization. Rejecting
  // here at two requests also shed valid cache hits and cold misses during
  // short bursts. The isolated health listener does not use this pool.
  if (!self->authority_) {
    HttpNoteAdmission(http::HttpAdmission::SnapshotAuthorityBusy);
    response.retry_after = 1;
    fail(503, "Service Unavailable", "{\"error\":\"SNAPSHOT_AUTH_UNAVAILABLE\"}");
    return;
  }
  const auto policy = self->authority_(request, self->attestation_);
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  if (policy.status) { response.retry_after = policy.retry_after; fail(policy.status, policy.reason, policy.error_body ? policy.error_body : "{}"); return; }
  if (request.raw_target && strchr(request.raw_target, '?')) { fail(400, "Bad Request", "{\"error\":\"SNAPSHOT_HAS_NO_QUERY\"}"); return; }
  if (!accepts_json(request.accept)) { fail(406, "Not Acceptable", "{\"error\":\"JSON_REQUIRED\"}"); return; }
  uint8_t body[PriceSnapshotGeneration::kCapacity]; size_t length = 0;
  if (!self->view_.Copy(now_ms(), body, sizeof(body), &length, response.etag, sizeof(response.etag))) {
    HttpNoteAdmission(http::HttpAdmission::SnapshotUnavailable);
    response.retry_after = 1; fail(503, "Service Unavailable", "{\"error\":\"SNAPSHOT_NOT_READY\"}"); return;
  }
  response.body = body; response.body_length = length;
  response.write_timeout_ms = 100;
  if (matches_etag(request.if_none_match, response.etag)) {
    response.status = 304; response.reason = "Not Modified"; response.body_length = 0;
  }
  // The router copies the ready response, then sends it outside the route lock.
  // Provider-backed work cannot exhaust its response slots.
  WriteHttpResponse(client, response, head);
}
bool PriceSnapshotRoute::Register(http::HttpRouter& router, bool attestation) {
  attestation_ = attestation;
  return router.Add("GET", "/v1/prices/snapshot", Handle, this);
}

}  // namespace fortis::prices
