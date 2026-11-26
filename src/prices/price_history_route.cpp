#include "fixed_string.h"
#include <cassert>
#include "price_history_route.h"

// GET /v1/prices/history and GET /v1/prices/ohlc. One object serves the normal
// image (behind FORTIS_NATIVE_PRICE_ROUTE_CANARY=1) and the test binaries;
// test clocks and jitter arrive only through PriceHistoryRouteSeams.

#include "device_enrollment.h"
#include "bounded_json.h"
#include "price_history_batch_codec.h"
#include "price_id_parser.h"
#include "price_history_backfill_owner.h"
#include "price_history_query.h"
#include "price_ohlc_query.h"
#include "price_history_view.h"
#include "device_auth.h"
#include "ohlc_cache_policy.h"
#include "threading.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

namespace {
using fortis::prices::history_batch::HistoryView;
using fortis::prices::history_batch::HistoryWindow;
using fortis::codec::history_batch::OutBuffer;
using fortis::prices::history_batch::kHistoryBitmapBytes;
using fortis::prices::history_batch::kHistoryMaxGaps;

constexpr size_t kJsonCapacity = 384 * 1024;
constexpr size_t kProtoCapacity = 262144;
constexpr size_t kMaxIdBytes = 200;
constexpr size_t kOhlcProviderMaxPoints = 400;
constexpr size_t kOhlcCacheMaxBytes = codec::kBoundedJsonMaxInputBytes + 64;
constexpr int64_t kJsMaxSafeInteger = 9007199254740991LL;
// Owner requests end early enough to encode inside the handoff deadline.
constexpr uint64_t kOwnerDeadlineMs = 25000;
// GET encodes one CandleHistory.history() result before handoff. Worst case
// per candle: a JSON point and volume pair is at most 90 bytes (as the batch
// size policy assumes); a protobuf point and volume are two 22-byte messages.
// The id, cache, coverage, window and availability fit in 2,400 bytes.
constexpr size_t kHistoryWorstJsonBytesPerCandle = 90;
constexpr size_t kHistoryWorstProtoBytesPerCandle = 44;
constexpr size_t kHistoryWorstMetadataBytes = 2400;
static_assert(kHistoryWorstMetadataBytes + fortis::prices::kPriceSeriesMaxDailyRangeCandles *
              kHistoryWorstJsonBytesPerCandle <= kJsonCapacity,
              "a ten-year daily JSON history must fit the GET response buffer");
static_assert(kHistoryWorstMetadataBytes + fortis::prices::kPriceSeriesMaxDailyRangeCandles *
              kHistoryWorstProtoBytesPerCandle <= kProtoCapacity,
              "a ten-year daily protobuf history must fit the GET response buffer");
char json_body[kJsonCapacity];
uint8_t protobuf_body[kProtoCapacity];
unsigned char ohlc_cache_body[codec::kBoundedJsonMaxInputBytes];
unsigned char ohlc_kv_scratch[kOhlcCacheMaxBytes];
// Range() returns at most 3,700 daily candles (about 260 KiB). Keep it in BSS
// rather than the 128 KiB HTTP worker stack; response_mutex owns it.
fortis::prices::PriceSeriesCandle candle_buffer[fortis::prices::kPriceSeriesMaxDailyRangeCandles];
static_assert(sizeof(candle_buffer) < 272 * 1024,
              "history candle workspace must stay below 272 KiB");
fortis::base::Mutex response_mutex;
fortis::base::Mutex ohlc_cache_mutex;
// In-process revalidation backoff and miss replay holds (the TypeScript SwrCache maps).
// ohlc_cache_mutex owns it; reads may clear expired holds, and provider
// failures add holds. Neither path writes the durable KV.
discovery::OhlcCachePolicy ohlc_policy;

constexpr size_t kRequestSlots = http::HttpResponseHandoff::kCapacity;
// One GET whose CoinGecko gaps are being filled. Its TypeScript gap plan is
// walked one owner request at a time from the completion callback; the
// initially present buckets are a bitmap (463 bytes), not timestamps.
struct RequestSlot {
  bool used = false;
  bool cancelled = false;
  unsigned references = 0;
  uint64_t handoff_token = 0;
  uint64_t owner_request_id = 0;
  PriceHistoryRouteContext* route = nullptr;
  char source_id[providers::kPriceIdMaxSourceIdBytes + 1]{};  // canonical (ParsePriceId)
  const char* interval_name = "1h";
  char coin[kMaxIdBytes + 1]{};  // CoinGecko id for owner submits
  bool protobuf = false;
  int rate_cost = 0;
  uint32_t rate_remaining = 0;
  int64_t from_ms = 0;
  int64_t to_ms = 0;
  size_t range_capacity = 0;
  fortis::prices::PriceSeriesInterval interval = fortis::prices::PriceSeriesInterval::k1h;
  HistoryWindow window{};
  uint8_t initial[kHistoryBitmapBytes]{};
  bool has_initial = false;
  int64_t latest_initial = 0;  // seconds of the newest initially cached point
  bool fetched_live = false;   // a gap reported kComplete
  int64_t gaps[kHistoryMaxGaps][2]{};
  size_t gap_count = 0;
  size_t next_gap = 0;
  uint64_t owner_deadline_ms = 0;
};
RequestSlot request_slots[kRequestSlots]{};
fortis::base::Mutex request_slots_mutex;

struct OhlcCacheHeader {
  uint32_t magic = 0x31434c4f; // OLC1
  uint32_t body_length = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
  int64_t as_of_ms = 0;
};
struct OhlcRequestSlot {
  bool used = false, cancelled = false, background = false;
  unsigned references = 0;
  uint64_t handoff_token = 0, owner_request_id = 0;
  PriceHistoryRouteContext* route = nullptr;
  char slug[101]{}, cache_key[160]{};
  int days = 0, rate_cost = 0;
  uint32_t rate_remaining = 0;
  bool protobuf = false;
};
OhlcRequestSlot ohlc_slots[kRequestSlots]{};
fortis::base::Mutex ohlc_slots_mutex;

void release_ohlc_slot(OhlcRequestSlot* slot) {
  fortis::base::LockGuard ohlc_slots_lock(ohlc_slots_mutex);
  if (slot->references) --slot->references;
  if (!slot->references) *slot = OhlcRequestSlot{};
}
OhlcRequestSlot* acquire_ohlc_slot(PriceHistoryRouteContext* route) {
  fortis::base::LockGuard ohlc_slots_lock(ohlc_slots_mutex); OhlcRequestSlot* result = nullptr;
  for (auto& slot : ohlc_slots) if (!slot.used) {
    slot = OhlcRequestSlot{}; slot.used = true; slot.references = 1;
    slot.route = route; result = &slot; break;
  }
  return result;
}
OhlcRequestSlot* acquire_ohlc_background_slot(PriceHistoryRouteContext* route,
    const char* slug, int days, const char* cache_key) {
  fortis::base::LockGuard ohlc_slots_lock(ohlc_slots_mutex);
  for (auto& slot : ohlc_slots) if (slot.used && slot.background &&
      strcmp(slot.cache_key, cache_key) == 0) {
    return nullptr;
  }
  OhlcRequestSlot* result = nullptr;
  for (auto& slot : ohlc_slots) if (!slot.used) {
    slot = OhlcRequestSlot{}; slot.used = true; slot.background = true;
    slot.references = 1; slot.route = route; slot.days = days;
    snprintf(slot.slug, sizeof(slot.slug), "%s", slug);
    snprintf(slot.cache_key, sizeof(slot.cache_key), "%s", cache_key);
    result = &slot; break;
  }
  return result;
}
void ohlc_handoff_terminal(void* opaque, bool cancelled) {
  auto* slot = static_cast<OhlcRequestSlot*>(opaque);
  if (cancelled) {
    fortis::base::UniqueLock ohlc_slots_lock(ohlc_slots_mutex); slot->cancelled = true;
    const uint64_t request_id = slot->owner_request_id;
    PriceHistoryRouteContext* route = slot->route;
    ohlc_slots_lock.Unlock();
    if (request_id && route && route->ohlc_owner) route->ohlc_owner->Cancel(request_id);
  }
  release_ohlc_slot(slot);
}

bool read_ohlc_cache(PriceHistoryRouteContext* route, const char* key,
    int64_t now, OhlcCacheHeader* header, unsigned char* body, size_t capacity) {
  // Caller holds ohlc_cache_mutex; the shared KV scratch and cache body are
  // bounded BSS buffers rather than 256 KiB HTTP/owner stack allocations.
  if (!route || !route->kv || !header || !body) return false;
  size_t size = 0;
  const auto status = route->kv->GetStatus(key, now, ohlc_kv_scratch,
      sizeof(ohlc_kv_scratch), &size);
  if (status != fortis::storage::KvReadStatus::Found || size < sizeof(OhlcCacheHeader)) return false;
  OhlcCacheHeader saved{}; memcpy(&saved, ohlc_kv_scratch, sizeof(saved));
  if (saved.magic != 0x31434c4f || saved.as_of_ms < 0 ||
      saved.fresh_until_ms <= saved.as_of_ms ||
      saved.stale_until_ms <= saved.fresh_until_ms || saved.body_length > capacity ||
      size != sizeof(saved) + saved.body_length || saved.stale_until_ms <= now) return false;
  *header = saved; memcpy(body, ohlc_kv_scratch + sizeof(saved), saved.body_length); return true;
}

// The coordinator's jitter sample in [0, 1). A test seam may pin it so
// freshness boundaries are exact; production draws eight CSPRNG bytes per
// cache write (at most one write per key per fresh window). An entropy
// failure stores the unjittered freshness.
double ohlc_jitter_rand(const PriceHistoryRouteContext* route) {
  const PriceHistoryRouteSeams* seams = route ? route->seams : nullptr;
  double fixed = 0;
  if (seams && seams->ohlc_jitter && seams->ohlc_jitter->Sample(&fixed) &&
      isfinite(fixed) && fixed >= 0 && fixed <= 1) return fixed;
  uint8_t bytes[8]{};
  if (!fortis::auth::SecureRandomBytes(bytes, sizeof(bytes))) return 0.5;
  uint64_t value = 0; memcpy(&value, bytes, sizeof(value));
  return static_cast<double>(value >> 11) * (1.0 / 9007199254740992.0);
}

bool write_ohlc_cache(PriceHistoryRouteContext* route, const char* key,
    int64_t now, const unsigned char* body, size_t length) {
  // Caller holds ohlc_cache_mutex across the synchronous durable KV write.
  if (!route || !route->kv || !body || length > codec::kBoundedJsonMaxInputBytes ||
      length > UINT32_MAX) return false;
  OhlcCacheHeader header{}; header.body_length = static_cast<uint32_t>(length);
  // SwrCache.store: ONE jittered freshness feeds the envelope, the stale
  // boundary, and the KV expiry, so the stored row never outlives its claim.
  header.as_of_ms = now;
  header.fresh_until_ms = now + discovery::OhlcJitteredFreshMs(discovery::kOhlcCacheFreshMs, ohlc_jitter_rand(route));
  header.stale_until_ms = header.fresh_until_ms + discovery::kOhlcCacheStaleMs;
  memcpy(ohlc_kv_scratch, &header, sizeof(header));
  memcpy(ohlc_kv_scratch + sizeof(header), body, length);
  fortis::storage::KvMutation mutation{key, ohlc_kv_scratch, static_cast<uint32_t>(sizeof(header) + length),
      header.stale_until_ms, false};
  return route->kv->Write(&mutation, 1);
}

uint64_t monotonic_ms() {
  timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool seconds_to_milliseconds(int64_t seconds, int64_t* milliseconds) {
  if (!milliseconds || seconds < 0 || seconds > INT64_MAX / 1000) return false;
  *milliseconds = seconds * 1000;
  return true;
}

void release_slot(RequestSlot* slot) {
  fortis::base::LockGuard request_slots_lock(request_slots_mutex);
  if (slot->references) --slot->references;
  if (!slot->references) *slot = RequestSlot{};
}

RequestSlot* acquire_slot(PriceHistoryRouteContext* route) {
  fortis::base::LockGuard request_slots_lock(request_slots_mutex);
  RequestSlot* result = nullptr;
  for (auto& slot : request_slots) if (!slot.used) {
    slot = RequestSlot{}; slot.used = true; slot.references = 1; slot.route = route;
    result = &slot; break;
  }
  return result;
}

struct JsonWriter {
  char* output;
  size_t capacity;
  size_t used = 0;
  bool ok = true;

  void text(const char* value) {
    const size_t length = strlen(value);
    if (!ok || used >= capacity || length >= capacity - used) {
      ok = false;
      return;
    }
    memcpy(output + used, value, length);
    used += length;
    output[used] = '\0';
  }
  void integer(int64_t value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    text(buffer);
  }
  // Number.prototype.toString through the batch codec's writer, which needs
  // no <charconv> and so also compiles into the guest.
  void real(double value) {
    if (!ok || used >= capacity) { ok = false; return; }
    fortis::codec::history_batch::OutBuffer out{reinterpret_cast<uint8_t*>(output + used),
                                         capacity - used - 1};
    fortis::codec::history_batch::AppendJsNumber(out, value);
    if (!out.ok) { ok = false; return; }
    used += out.used;
    output[used] = '\0';
  }
};

void send_json(int client, int status, const char* reason, const char* body,
               bool head, int rate_cost = -1, uint32_t remaining = 0,
               unsigned retry_after = 0) {
  http::HttpResponse response{status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = rate_cost;
  response.rate_limit_remaining = remaining;
  response.retry_after = retry_after;
  WriteHttpResponse(client, response, head);
}

// 400 {"error":"BAD_REQUEST","detail":"<fixed text>"}.
void send_error(int client, int status, const char* reason, const char* detail,
                bool head, int rate_cost, uint32_t remaining) {
  char body[512];
  snprintf(body, sizeof(body), "{\"error\":\"BAD_REQUEST\",\"detail\":\"%s\"}", detail);
  send_json(client, status, reason, body, head, rate_cost, remaining);
}

bool timestamp_iso_ms(int64_t milliseconds, char output[32]) {
  if (milliseconds < 0) return false;
  const int64_t seconds = milliseconds / 1000;
  const int millis = static_cast<int>(milliseconds % 1000);
  const time_t value = static_cast<time_t>(seconds); struct tm utc{};
  if (!gmtime_r(&value, &utc)) return false;
  char base[24]{};
  if (!strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc)) return false;
  return snprintf(output, 32, "%s.%03dZ", base, millis) > 0;
}

// The OHLC cache clock. Only a test seam replaces the wall clock; a negative
// seam value falls back to it.
int64_t ohlc_now_ms(const PriceHistoryRouteContext* route) {
  const PriceHistoryRouteSeams* seams = route ? route->seams : nullptr;
  if (seams && seams->ohlc_clock) {
    const int64_t fixed = seams->ohlc_clock->NowMs();
    if (fixed >= 0) return fixed;
  }
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// The OHLC JSON body: JSON.stringify({points, cache}) of the TypeScript
// result. `timestamp_ms` < 0 writes "timestamp":null.
void add_json_ohlc(JsonWriter& writer, const fortis::prices::PriceSeriesCandle* candles,
                   size_t count, bool cache_hit, int64_t timestamp_ms) {
  writer.text("{\"points\":[");
  for (size_t i = 0; i < count; ++i) {
    if (i) writer.text(",");
    writer.text("["); writer.integer(candles[i].ts); writer.text(",");
    writer.real(candles[i].open); writer.text(",");
    writer.real(candles[i].high); writer.text(",");
    writer.real(candles[i].low); writer.text(",");
    writer.real(candles[i].close); writer.text("]");
  }
  writer.text("],\"cache\":{\"hit\":"); writer.text(cache_hit ? "true" : "false");
  writer.text(",\"timestamp\":");
  if (timestamp_ms >= 0) {
    char timestamp[32]{};
    if (!timestamp_iso_ms(timestamp_ms, timestamp)) { writer.ok = false; return; }
    writer.text("\""); writer.text(timestamp); writer.text("\"");
  } else {
    writer.text("null");
  }
  writer.text("}}");
}

// The OHLC protobuf body: toBinary(OhlcResponse) as protobuf-es writes it.
// proto3 implicit fields at their default (unix_millis 0, a +0 price,
// cache.hit false) are omitted; every point and the cache message are
// always present. `timestamp_ms` < 0 leaves cache.timestamp unset.
bool add_proto_ohlc(const fortis::prices::PriceSeriesCandle* candles, size_t count,
                    size_t* output_length, bool cache_hit, int64_t timestamp_ms) {
  namespace pw = fortis::codec::proto_wire;
  if (!output_length) return false;
  pw::Span body{protobuf_body, sizeof(protobuf_body)};
  for (size_t i = 0; i < count; ++i) {
    uint8_t point_bytes[64]{};
    pw::Span point{point_bytes, sizeof(point_bytes)};
    // The schema and Flutter chart consumer expect Unix epoch milliseconds.
    if (candles[i].ts) pw::PutVarintField(point, 1, static_cast<uint64_t>(candles[i].ts));
    codec::proto_wire::PutImplicitDouble(point, 2, candles[i].open);
    codec::proto_wire::PutImplicitDouble(point, 3, candles[i].high);
    codec::proto_wire::PutImplicitDouble(point, 4, candles[i].low);
    codec::proto_wire::PutImplicitDouble(point, 5, candles[i].close);
    pw::PutBytes(body, 1, point.data, point.used);
    if (!point.ok || !body.ok) return false;
  }
  uint8_t cache_bytes[64]{};
  pw::Span cache{cache_bytes, sizeof(cache_bytes)};
  if (cache_hit) pw::PutVarintField(cache, 1, 1);
  if (timestamp_ms >= 0) {
    char timestamp[32]{};
    if (!timestamp_iso_ms(timestamp_ms, timestamp)) return false;
    pw::PutBytes(cache, 2, timestamp, strlen(timestamp));
  }
  pw::PutBytes(body, 2, cache.data, cache.used);
  if (!cache.ok || !body.ok) return false;
  *output_length = body.used;
  return true;
}

// What a CoinGecko /ohlc 2xx body is to coingecko.ohlc():
enum class OhlcBody : uint8_t {
  kRows,        // an array of [ms, open, high, low, close] number rows
  kNotArray,    // other JSON (object, null, …): `Array.isArray(body) ? body : []`
  kUnparseable, // not JSON: HttpClient's parse failure, a retried transport error
  kRefused,     // JSON the native route does not serve (documented bound)
};

// Rows must be exactly five JSON numbers with a safe non-negative integer
// timestamp, at most kOhlcProviderMaxPoints of them, in a body the bounded
// parser accepts (256 KiB, 8,192 tokens); anything else is kRefused, an
// upstream failure. Rows keep the provider's order. Callers hold
// response_mutex (the token arena is shared).
OhlcBody parse_provider_ohlc(const unsigned char* body, size_t length,
    fortis::prices::PriceSeriesCandle* output, size_t capacity, size_t* count) {
  static codec::BoundedJsonToken tokens[codec::kBoundedJsonMaxTokens];
  codec::BoundedJsonDocument document; codec::BoundedJsonError error{};
  if (count) *count = 0;
  if (!body || !output || !count) return OhlcBody::kRefused;
  if (!document.Parse(body, length, tokens, codec::kBoundedJsonMaxTokens, &error))
    return error == codec::BoundedJsonError::kInvalidInput ? OhlcBody::kUnparseable
                                                    : OhlcBody::kRefused;
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (!root) return OhlcBody::kRefused;
  if (root->type != codec::BoundedJsonType::kArray) return OhlcBody::kNotArray;
  size_t rows = 0;
  for (uint32_t row = root->first_child; row != codec::kBoundedJsonNoToken;
       row = document.token(row)->next_sibling) {
    if (rows >= capacity || rows >= kOhlcProviderMaxPoints) return OhlcBody::kRefused;
    const codec::BoundedJsonToken* tuple = document.token(row);
    if (!tuple || tuple->type != codec::BoundedJsonType::kArray) return OhlcBody::kRefused;
    double values[5]{}; size_t field = 0;
    for (uint32_t item = tuple->first_child; item != codec::kBoundedJsonNoToken;
         item = document.token(item)->next_sibling) {
      if (field >= 5 || !document.Number(item, &values[field])) return OhlcBody::kRefused;
      ++field;
    }
    if (field != 5 || values[0] < 0 || values[0] > kJsMaxSafeInteger ||
        floor(values[0]) != values[0]) return OhlcBody::kRefused;
    output[rows] = fortis::prices::PriceSeriesCandle{};
    output[rows].ts = static_cast<int64_t>(values[0]);
    output[rows].open = values[1]; output[rows].high = values[2];
    output[rows].low = values[3]; output[rows].close = values[4];
    ++rows;
  }
  *count = rows; return OhlcBody::kRows;
}

// The owner's retry check: a 2xx body that is not JSON is retried like a
// transport failure (HttpClient.json).
bool ohlc_body_unparseable(const unsigned char* body, size_t length) {
  fortis::base::LockGuard response_lock(response_mutex);
  size_t count = 0;
  const bool unparseable = parse_provider_ohlc(body, length, candle_buffer,
      kOhlcProviderMaxPoints, &count) == OhlcBody::kUnparseable;
  return unparseable;
}

// The TypeScript replyBackPressure() body for a lane refusal.
const char kOhlcRateLimited[] =
    "{\"error\":\"RATE_LIMITED\",\"detail\":"
    "\"Upstream data is temporarily busy; retained data remains available.\"}";

// replyBackPressure()'s retry-after: max(1, ceil(retryAfterMs / 1000)).
unsigned ohlc_retry_after_seconds(uint32_t retry_after_ms) {
  const unsigned seconds = (retry_after_ms + 999) / 1000;
  return seconds < 1 ? 1 : seconds;
}

// Answers a deferred slug miss. A 200 is the live provider answer
// (cache.hit false, no timestamp); 429 is replyBackPressure(); 503 is the
// native shutdown answer; any other status is runFacet's 502.
void complete_ohlc_slot(OhlcRequestSlot* slot, int status,
    const fortis::prices::PriceSeriesCandle* candles, size_t count, unsigned retry_after = 0) {
  if (!slot || slot->background) return;
  fortis::base::UniqueLock ohlc_slots_lock(ohlc_slots_mutex); const bool cancelled = slot->cancelled;
  ohlc_slots_lock.Unlock();
  if (cancelled) return;
  http::HttpResponse response{};
  fortis::base::LockGuard response_lock(response_mutex);
  const uint8_t* body = nullptr; size_t length = 0;
  const char* content_type = "application/json; charset=utf-8";
  if (status == 200) {
    if (slot->protobuf) {
      if (!add_proto_ohlc(candles, count, &length, false, -1)) status = 500;
      else { body = protobuf_body; content_type = "application/protobuf"; }
    } else {
      JsonWriter writer{json_body, sizeof(json_body)};
      add_json_ohlc(writer, candles, count, false, -1);
      if (!writer.ok) status = 500;
      else { body = reinterpret_cast<const uint8_t*>(json_body); length = writer.used; }
    }
  }
  if (status == 429) {
    body = reinterpret_cast<const uint8_t*>(kOhlcRateLimited);
    length = strlen(kOhlcRateLimited);
  } else if (status != 200) {
    if (status != 500 && status != 503) status = 502;
    body = reinterpret_cast<const uint8_t*>(status == 500 ? "{\"error\":\"INTERNAL\"}"
                                                          : "{\"error\":\"UPSTREAM_UNAVAILABLE\"}");
    length = strlen(reinterpret_cast<const char*>(body));
    content_type = "application/json; charset=utf-8";
  }
  response = http::HttpResponse{status, status == 200 ? "OK" : status == 429 ? "Too Many Requests" :
      status == 500 ? "Internal Server Error" : status == 503 ? "Service Unavailable" :
      "Bad Gateway", content_type, body, length};
  // sendProtobufOrJson sets `vary: Accept`; runFacet's error replies do not.
  response.vary_accept = status == 200;
  response.retry_after = status == 429 ? retry_after : 0;
  response.rate_limit_cost = slot->rate_cost;
  response.rate_limit_remaining = slot->rate_remaining;
  slot->route->handoff->Complete(slot->handoff_token, response);
}

// The owner's completion for one slug request (a cold miss with an HTTP
// waiter, or a background refresh). SwrCache.fetchLive / revalidate:
//  - a usable 2xx body (rows, or JSON that is not an array, stored as [])
//    is written to the cache with fresh jitter, unless a concurrent
//    completion already stored a fresh entry; the waiter gets the live rows
//    even when the durable write fails (SwrCache logs a failed kv.set);
//  - a provider verdict (non-2xx, unparseable after the owner's retries,
//    refused rows, or the 25 s deadline) holds the cold-miss failure (502)
//    or, for a refresh, backs off;
//  - a lane refusal is back-pressure: 429 with retry-after, held 5 s;
//  - a cancellation that is not a verdict (this client left, or shutdown)
//    holds nothing.
void ohlc_provider_complete(void* opaque, const OhlcProviderResult& result,
    const unsigned char* body, size_t body_length) {
  auto* slot = static_cast<OhlcRequestSlot*>(opaque);
  fortis::prices::PriceSeriesCandle parsed[kOhlcProviderMaxPoints]{}; size_t count = 0;
  int status = 502;
  unsigned retry_after = 0;
  const bool answered = !result.transport_error && !result.cancelled &&
      result.http_status >= 200 && result.http_status < 300;
  fortis::base::UniqueLock response_lock(response_mutex);
  const OhlcBody kind = answered
      ? parse_provider_ohlc(body, body_length, parsed, kOhlcProviderMaxPoints, &count)
      : OhlcBody::kRefused;
  response_lock.Unlock();
  const bool usable = kind == OhlcBody::kRows || kind == OhlcBody::kNotArray;
  fortis::base::UniqueLock ohlc_slots_lock(ohlc_slots_mutex);
  const bool background = slot->background;
  ohlc_slots_lock.Unlock();
  fortis::base::UniqueLock ohlc_cache_lock(ohlc_cache_mutex);
  const int64_t now = ohlc_now_ms(slot->route);
  if (usable) {
    OhlcCacheHeader existing{};
    if (!(read_ohlc_cache(slot->route, slot->cache_key, now, &existing,
            ohlc_cache_body, sizeof(ohlc_cache_body)) && existing.fresh_until_ms > now)) {
      static const unsigned char kEmpty[] = "[]";
      const bool rows = kind == OhlcBody::kRows;
      write_ohlc_cache(slot->route, slot->cache_key, now, rows ? body : kEmpty,
                       rows ? body_length : 2);
    }
    status = 200;
  } else if (result.cancelled && !result.deadline_exceeded && !result.lane_rejected) {
    status = 503;
  } else if (background) {
    ohlc_policy.RecordRevalidateFailure(slot->cache_key, now, result.lane_rejected);
  } else if (result.lane_rejected) {
    // The LaneBusyError is replayed as itself: 429 with the same
    // retry-after, carried in the held status as 429 + 1000 * seconds.
    retry_after = ohlc_retry_after_seconds(result.retry_after_ms ? result.retry_after_ms : 2000);
    status = 429;
    ohlc_policy.RecordMissFailure(slot->cache_key, now, true,
                                  429 + 1000 * static_cast<int>(retry_after));
  } else {
    ohlc_policy.RecordMissFailure(slot->cache_key, now, false, 502);
  }
  ohlc_cache_lock.Unlock();
  complete_ohlc_slot(slot, status, parsed, count, retry_after);
  release_ohlc_slot(slot);
}

// The single GET response for candle_buffer[0..view.count), encoded with
// the same JSON.stringify / protobuf-es writer as the batch route's entries
// (the GET body and a batch entry are the same CandleHistory object). The
// static asserts above bound both buffers for a ten-year daily window, so an
// overflow (false) is an internal error. Caller holds response_mutex.
bool encode_history(const HistoryView& view, bool protobuf, const uint8_t** body,
                    size_t* length, const char** content_type) {
  if (protobuf) {
    OutBuffer out{protobuf_body, sizeof(protobuf_body)};
    fortis::prices::history_batch::AppendHistoryProto(out, view);
    *body = protobuf_body; *length = out.used; *content_type = "application/protobuf";
    return out.ok;
  }
  OutBuffer out{reinterpret_cast<uint8_t*>(json_body), sizeof(json_body)};
  fortis::prices::history_batch::AppendHistoryJson(out, view);
  *body = reinterpret_cast<const uint8_t*>(json_body); *length = out.used;
  *content_type = "application/json; charset=utf-8";
  return out.ok;
}

// The GET route has always treated an unreadable resolution as absent.
void read_history_availability(const PriceHistoryRouteContext* route, const char* source_id,
                               int64_t now, HistoryView* view) {
  static_cast<void>(fortis::prices::history_batch::ReadHistoryAvailability(*route->series, source_id,
      now, &view->availability));
}

void handoff_terminal(void* opaque, bool cancelled) {
  auto* slot = static_cast<RequestSlot*>(opaque);
  uint64_t request_id = 0; PriceHistoryBackfillOwner* owner = nullptr;
  fortis::base::UniqueLock request_slots_lock(request_slots_mutex);
  if (cancelled) slot->cancelled = true;
  request_id = slot->owner_request_id;
  owner = slot->route ? slot->route->owner : nullptr;
  request_slots_lock.Unlock();
  if (cancelled && request_id && owner) owner->Cancel(request_id);
  release_slot(slot);
}

void history_owner_complete(void* opaque, const PriceHistoryOwnerResult& result);

// Completes a deferred GET through its handoff. Only a 200 comes from
// sendProtobufOrJson and carries `Vary: Accept`. Caller holds response_mutex.
void complete_slot(RequestSlot* slot, http::HttpResponse response) {
  assert(slot->route && slot->route->handoff);
  response.vary_accept = response.status == 200;
  response.rate_limit_cost = slot->rate_cost;
  response.rate_limit_remaining = slot->rate_remaining;
  slot->route->handoff->Complete(slot->handoff_token, response);
}

void complete_unavailable(RequestSlot* slot) {
  complete_slot(slot, http::JsonResponse(503, "Service Unavailable",
                                   "{\"error\":\"UPSTREAM_UNAVAILABLE\"}"));
}

// CandleHistory.history() after its gap loop: re-read the window and report
// cache.hit false only if a gap was filled. A swallowed provider failure
// therefore answers the cached points, exactly as TypeScript does. Caller
// holds response_mutex.
void complete_history(RequestSlot* slot) {
  assert(slot->route && slot->route->series);
  auto* route = slot->route;
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  size_t count = 0;
  const auto read = route->series->Range(slot->interval, "usd", slot->source_id,
      slot->from_ms, slot->to_ms, now, candle_buffer, slot->range_capacity, &count);
  if (read == fortis::prices::PriceSeriesReadStatus::kIoError ||
      read == fortis::prices::PriceSeriesReadStatus::kCorrupt ||
      read == fortis::prices::PriceSeriesReadStatus::kCapacityExceeded ||
      read == fortis::prices::PriceSeriesReadStatus::kInvalidKey) {
    complete_unavailable(slot);
    return;
  }
  HistoryView view{};
  view.source_id = slot->source_id; view.interval = slot->interval_name;
  view.candles = candle_buffer; view.count = count;
  view.hit = !slot->fetched_live;
  view.has_timestamp = slot->has_initial; view.timestamp = slot->latest_initial;
  view.cached = fortis::prices::history_batch::CountInitiallyCached(slot->window, slot->initial,
                                                            candle_buffer, count);
  read_history_availability(route, slot->source_id, now, &view);
  const uint8_t* body = nullptr; size_t length = 0;
  const char* content_type = nullptr;
  if (!encode_history(view, slot->protobuf, &body, &length, &content_type)) {
    complete_slot(slot, http::JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"));
    return;
  }
  complete_slot(slot, http::HttpResponse{200, "OK", content_type, body, length});
}

enum class GapSubmit : uint8_t { kAccepted, kDone, kShutdown };

// Starts the slot's next gap. A gap the owner refuses as invalid is skipped;
// owner Busy or the owner deadline ends the plan early, which CandleHistory
// would see as a failed backfill (cached points, cache.hit unchanged). The
// caller holds response_mutex and one slot reference of its own.
GapSubmit submit_next_gap(RequestSlot* slot) {
  PriceHistoryBackfillOwner* owner = slot->route->owner;
  while (slot->next_gap < slot->gap_count) {
    if (monotonic_ms() >= slot->owner_deadline_ms) return GapSubmit::kDone;
    fortis::base::UniqueLock request_slots_lock(request_slots_mutex);
    assert(slot->references > 0 && slot->references < UINT32_MAX);  // caller retains a reference
    ++slot->references;  // owner completion callback
    request_slots_lock.Unlock();
    uint64_t request_id = 0;
    const auto status = owner->Submit(slot->coin, slot->source_id, "usd", slot->interval,
        slot->gaps[slot->next_gap][0], slot->gaps[slot->next_gap][1],
        slot->owner_deadline_ms, history_owner_complete, slot, &request_id);
    if (status == PriceHistoryOwnerSubmitStatus::kAccepted) {
      request_slots_lock.Lock();
      slot->owner_request_id = request_id;
      const bool cancelled = slot->cancelled;
      request_slots_lock.Unlock();
      if (cancelled) owner->Cancel(request_id);
      return GapSubmit::kAccepted;
    }
    release_slot(slot);  // no owner callback
    if (status == PriceHistoryOwnerSubmitStatus::kNotRunning ||
        status == PriceHistoryOwnerSubmitStatus::kStopping) return GapSubmit::kShutdown;
    if (status == PriceHistoryOwnerSubmitStatus::kBusy) return GapSubmit::kDone;
    ++slot->next_gap;  // kInvalidRequest
  }
  return GapSubmit::kDone;
}

// Runs on the owner thread, never under the owner mutex, so it may submit the
// next gap. Lock order matches the HTTP handler: response_mutex, then
// request_slots_mutex or the owner mutex (Submit/Cancel).
void history_owner_complete(void* opaque, const PriceHistoryOwnerResult& result) {
  auto* slot = static_cast<RequestSlot*>(opaque);
  fortis::base::UniqueLock response_lock(response_mutex);
  fortis::base::UniqueLock request_slots_lock(request_slots_mutex);
  slot->owner_request_id = 0;
  const bool cancelled = slot->cancelled;
  request_slots_lock.Unlock();
  if (cancelled) {
    // The client is gone and its handoff terminal has run: nothing to answer.
  } else if (result.status == PriceHistoryOwnerResultStatus::kShutdown) {
    complete_unavailable(slot);
  } else {
    // CandleHistory swallows a failed backfill (provider error, 401, invalid
    // or over-bound body, timeout): the gap stays unfetched and uncovered,
    // and only a gap this request filled makes cache.hit false. A caller
    // that joined another request's fetch is TypeScript's lock loser: it
    // waits, re-reads the filled candles and keeps cache.hit true.
    if (result.status == PriceHistoryOwnerResultStatus::kComplete && !result.joined)
      slot->fetched_live = true;
    ++slot->next_gap;
    switch (submit_next_gap(slot)) {
      case GapSubmit::kAccepted: break;
      case GapSubmit::kShutdown: complete_unavailable(slot); break;
      case GapSubmit::kDone: complete_history(slot); break;
    }
  }
  response_lock.Unlock();
  release_slot(slot);  // this completion's reference
}

// GET /v1/prices/history?id=&from=&to=&interval=1h|1d — the TypeScript
// getPriceHistory over PricesFacet.history →
// CandleHistory.history: one id's USD close-price
// series for a window, read from the candle store and, for CoinGecko ids,
// backfilled from CoinGecko where buckets are missing.
//
// Route. GET only; HEAD runs this handler and writes the same status and
// headers (Content-Length of the would-be body) without a body, including a
// cold backfill. Exact path; no request body is read. The POST batch with the
// same path is price_history_batch_route.cpp.
//
// Order and device policy (AuthorizeDeviceRequest, shared by /v1): Origin
// present → 403 BROWSER_ORIGIN_FORBIDDEN; missing/malformed/unknown bearer →
// 401 UNAUTHORIZED; revoked/expired → 401 SESSION_EXPIRED; attestation
// required and not attested → 401 ATTESTATION_REQUIRED (no rate headers).
// Then the per-device minute quota at cost 1 (development 240, default 120,
// attested 600, premium 1200): every later reply, 400s included, carries
// x-rate-limit-cost: 1 and x-rate-limit-remaining; over quota → 429
// {"error":"RATE_LIMITED"} with Retry-After: 60. Validation runs after that.
//
// Query (fortis::prices::history_query::ParseQuery). fast-querystring as for GET
// /v1/prices: '&' pairs, first '=' splits, '+' is a space, strict UTF-8
// percent escapes or else the raw component text, a pair without '=' is an
// empty value, other keys ignored, a repeated key is an array. zod then
// checks the fields below; any failure is 400 {"error":"BAD_REQUEST"}:
// - id: a string of 1–200 UTF-16 code units (missing/array refused).
// - from, to: JavaScript Number() of the value (trimmed with JS whitespace;
//   "" → 0; decimal with sign, fraction and exponent; Infinity; unsigned
//   0x/0o/0b; missing or repeated → NaN), then int (a non-integer stops
//   here), safe (|n| ≤ 2^53 − 1) and positive. So `from=0x6955b900`,
//   `from=1.7672256e9` and `from=%201767225600` are valid and `from=` is
//   refused.
// - interval: exactly "1h" or "1d", default "1h" (repeated refused).
// Next the range: to > from and to − from ≤ 7,776,000 s (90 days) for 1h or
// 316,224,000 s (10 × 366 days) for 1d, else 400 {"error":"BAD_REQUEST",
// "detail":"range for 1h must be 1s–7776000s"} (1d: 316224000s). Last the
// id: ParsePriceId (JS trim, slug lower-casing, canonical source id) fails →
// 400 {"error":"BAD_REQUEST","detail":"unrecognized id"}.
//
// Result. 200 with Content-Type application/json; charset=utf-8 and Vary:
// Accept: {"id":<canonical source id>,"interval","points":[[sec,close]…],
// "volumes":[[sec,volume]…],"cache":{"hit","timestamp"},"coverage":
// {"cachedPoints","fetchedPoints"},"window":{"from","to","days"},
// "availability":{"from","to","days","resolutions":[{"interval","from","to",
// "days"}…]}}, or the PriceHistory protobuf (application/protobuf, same Vary)
// when Accept names application/protobuf with q > 0. Points are every stored
// candle of the interval with from·1000 ≤ ts ≤ to·1000 (both ends inclusive),
// ascending, one per bucket, close price; volumes list only the candles that
// have a volume (absent means unknown). cache.timestamp is the newest point
// that existed before this request (ISO, or null); cache.hit is false only
// when this request itself filled a gap; coverage splits the points into
// those present before the request and those it fetched. window spans the
// returned points; availability spans every stored 1h and 1d candle of the
// id ("1d" listed first); days = max(1, ceil(span / 86400) + 1), 0 if none.
// Cache-Control: no-store plus the security header baseline on every reply.
//
// Cache and provider. The candle store is the cache; there is no fresh/stale
// split. Only CoinGecko slugs are backfilled; every other recognized id
// answers its stored series, even an empty one, with no provider call. For a
// slug: the window's buckets (ceil(from/width)·width up to the bucket before
// ceil(to/width)·width) are compared with the stored candles; missing runs
// become gaps, minus the ranges this interval's coverage record says were
// already answered; more than eight gaps collapse into one aligned window.
// Each gap, in order, is one CoinGecko /coins/{id}/market_chart/range
// ?vs_currency=usd&from=&to= request on the shared CoinGecko lane (burst 4,
// 24/min, 2 concurrent; transport/5xx/408 retried twice after 250 and 1000
// ms; 429 not retried and opens the lane breaker for 30 s). An answer, even
// an empty one, is bucketed into both 1h and 1d candles (open first, close
// last, high/low, volume the last sample in the bucket) and the gap is marked
// covered for 6 h; any failure (error, 401, 429, breaker, invalid body,
// timeout) is swallowed: the gap stays uncovered for the next request and
// the reply is the stored points with cache.hit unchanged. After any
// attempted gap the window is read again.
//
// Persistence. Candles are written durably (KV frames, survive SIGKILL):
// kept until max(now + 7 days, end of their storage segment + 90 days for
// 1h or 366 days for 1d). The coverage record
// (price/history-coverage/v2/{1h|1d}/usd/{sourceId}) is rewritten with a
// 6 h TTL whenever a gap is answered, so no other write happens on a read.
//
// Concurrency and deadline. Requests with gaps are completed from the
// backfill owner's thread through the response handoff, never pinning an
// HTTP worker. Identical gap windows share one owner flight; a request that
// joined another's flight is TypeScript's lock loser and keeps cache.hit
// true. A client that disconnects leaves a flight others joined running;
// a sole caller's flight is cancelled with it (nothing stored). The gap plan
// stops 25 s after the request (the owner deadline); remaining gaps count as
// failed and the stored points are answered, before the 30 s handoff limit.
//
// Bounded native differences (TypeScript has no such limits):
// - ids whose raw or canonical text exceeds 512 bytes, or with a NUL in a
//   contract network, → "unrecognized id" (TypeScript: an empty series);
// - at most 8 owner requests and 16 deferred replies: a full owner ends the
//   gap plan (stored points, as on failure); no free reply slot → 503
//   {"error":"UPSTREAM_UNAVAILABLE"}; shutdown → 503; a store read failure
//   → 503 (TypeScript: 500 INTERNAL); an encoder overflow → 500;
// - a provider body over 512 KiB is an invalid answer (uncovered);
// - the gap plan stops at 25 s (TypeScript ends a hung provider after its
//   third 10 s attempt, ~31.25 s, with the same stored-points answer);
// - a joining request waits for the shared flight up to its own deadline
//   (TypeScript's lock loser waits at most 10 s); flights are keyed by the
//   exact gap window and coverage is marked for every joined interval, where
//   TypeScript's lock is keyed by (source id, from) alone;
// - a sole caller that disconnects cancels its fetch, so the next request
//   fetches again (TypeScript finishes the fetch and marks coverage);
// - request targets over 12,500 bytes are refused by the HTTP parser and raw
//   (unescaped) non-ASCII target bytes are UTF-8, not Latin-1 (router-wide).
// Privacy: replies never echo the bearer or device id; error bodies are fixed
// strings without the input value.
void handle_history(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* context = static_cast<PriceHistoryRouteContext*>(opaque);
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1,
                                                           context->attestation_required);
  if (policy.status) {
    send_json(client, policy.status, policy.reason,
      policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}", head,
      policy.rate_limit_cost, policy.rate_limit_remaining, policy.retry_after);
    return;
  }

  fortis::prices::history_query::Query query;  // about 2.7 KiB of the 128 KiB worker stack
  const char* mark = request.raw_target ? strchr(request.raw_target, '?') : nullptr;
  fortis::prices::history_query::ParseQuery(mark ? mark + 1 : nullptr, mark ? strlen(mark + 1) : 0,
                                    &query);
  if (query.status != fortis::prices::history_query::HistoryQueryOutcome::kOk) {
    send_json(client, 400, "Bad Request", query.error_body, head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  providers::ParsedPriceId parsed{};
  if (!ParsePriceId(query.id, query.id_length, &parsed) ||
      parsed.kind == providers::PriceIdKind::kInvalid) {
    send_error(client, 400, "Bad Request", "unrecognized id", head,
               policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  // Safe integers (< 2^53) always fit in signed milliseconds.
  int64_t from_ms = 0, to_ms = 0;
  if (!seconds_to_milliseconds(query.from, &from_ms) ||
      !seconds_to_milliseconds(query.to, &to_ms)) {
    send_json(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  const char* interval_name = query.daily ? "1d" : "1h";
  const int64_t width = query.daily ? 86400 : 3600;
  const auto interval = query.daily
      ? fortis::prices::PriceSeriesInterval::k1d : fortis::prices::PriceSeriesInterval::k1h;
  const HistoryWindow window =
      fortis::prices::history_batch::MakeHistoryWindow(query.from, query.to, width);
  // 2,500 buckets for 1h (a 90-day window is 2,160) and 3,700 for 1d (a
  // ten-year window is 3,660), so every valid range fits; this is defensive.
  const size_t read_cap = fortis::prices::PriceSeriesMaxRangeCandles(interval);
  if (window.expected > read_cap) {
    send_json(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  // Range allocates two scratch arrays sized by cap. Allow one extra bucket
  // because an exact `to` boundary is included by the store's range read.
  const size_t range_capacity = window.expected < read_cap ? window.expected + 1 : read_cap;

  fortis::base::UniqueLock response_lock(response_mutex);
  size_t count = 0;
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  const auto read = context->series->Range(interval, "usd", parsed.source_id,
      from_ms, to_ms, now, candle_buffer,
      range_capacity, &count);
  if (read == fortis::prices::PriceSeriesReadStatus::kIoError ||
      read == fortis::prices::PriceSeriesReadStatus::kCorrupt ||
      read == fortis::prices::PriceSeriesReadStatus::kCapacityExceeded) {
    response_lock.Unlock();
    send_json(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  // CandleHistory's gap plan: missing buckets minus covered ranges, more
  // than eight gaps collapsed into the aligned window. Only CoinGecko IDs can
  // be backfilled. Every other recognized ID answers its stored series, even
  // an empty one, with no provider call and no coverage mark. Its kInvalidKey
  // read (a parser-accepted ID the store cannot key, such as an EVM network
  // name containing a space) has nothing stored and reads as empty.
  const bool backfill = parsed.kind == providers::PriceIdKind::kCoinGecko &&
      context->owner && context->handoff;
  uint8_t initial[kHistoryBitmapBytes]{};  // 463 bytes on the 128 KiB worker stack
  int64_t gaps[kHistoryMaxGaps][2]{};
  size_t gap_count = 0;
  if (backfill) {
    fortis::prices::history_batch::MarkHistoryBuckets(window, candle_buffer, count, initial);
    if (fortis::prices::history_batch::HistoryAnyBucketMissing(window, initial)) {
      // A missing, expired, corrupt or unreadable record is uncovered.
      fortis::prices::HistoryCoverage covered{};
      const bool has_coverage = context->series->ReadHistoryCoverage(interval, "usd",
          parsed.source_id, PriceHistoryCoverageNowMs(), &covered) ==
          fortis::prices::PriceSeriesReadStatus::kFound;
      gap_count = fortis::prices::history_batch::PlanHistoryGaps(window, initial,
          has_coverage ? &covered : nullptr, gaps);
    }
  }
  const bool protobuf = AcceptsProtobuf(request);
  if (gap_count) {
    RequestSlot* slot = acquire_slot(context);
    if (slot) {
      snprintf(slot->source_id, sizeof(slot->source_id), "%s", parsed.source_id);
      slot->interval_name = interval_name;
      base::CopyCString(slot->coin, parsed.address);
      slot->protobuf = protobuf;
      slot->rate_cost = policy.rate_limit_cost;
      slot->rate_remaining = policy.rate_limit_remaining;
      slot->from_ms = from_ms;
      slot->to_ms = to_ms;
      slot->range_capacity = range_capacity;
      slot->interval = interval;
      slot->window = window;
      memcpy(slot->initial, initial, sizeof(slot->initial));
      slot->has_initial = count > 0;
      slot->latest_initial = count ? candle_buffer[count - 1].ts / 1000 : 0;
      memcpy(slot->gaps, gaps, sizeof(slot->gaps));
      slot->gap_count = gap_count;
      slot->next_gap = 0;
      slot->owner_deadline_ms = monotonic_ms() + kOwnerDeadlineMs;
      fortis::base::UniqueLock request_slots_lock(request_slots_mutex);
      ++slot->references; // response-handoff terminal callback
      request_slots_lock.Unlock();
      if (http::ReserveHttpResponse(client, head, kPriceHistoryOwnerMaxDeadlineMs,
                              &slot->handoff_token, handoff_terminal, slot)) {
        switch (submit_next_gap(slot)) {
          case GapSubmit::kAccepted: break;
          case GapSubmit::kShutdown: complete_unavailable(slot); break;
          // No gap could start: the swallowed-backfill answer.
          case GapSubmit::kDone: complete_history(slot); break;
        }
        response_lock.Unlock();
        release_slot(slot); // route callback reference
        return;
      }
      release_slot(slot); // failed reserve terminal reference was not installed
      release_slot(slot); // route callback reference
    }
    response_lock.Unlock();
    send_json(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  // No gap to fetch: the initial read is the answer and cache.hit is true.
  HistoryView view{};
  view.source_id = parsed.source_id; view.interval = interval_name;
  view.candles = candle_buffer; view.count = count;
  view.hit = true;
  view.has_timestamp = count > 0;
  view.timestamp = count ? candle_buffer[count - 1].ts / 1000 : 0;
  view.cached = count;
  read_history_availability(context, parsed.source_id, now, &view);
  const uint8_t* body = nullptr;
  size_t length = 0;
  const char* content_type = nullptr;
  if (!encode_history(view, protobuf, &body, &length, &content_type)) {
    response_lock.Unlock();
    send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head);
    return;
  }
  http::HttpResponse response{200, "OK", content_type, body, length};
  response.vary_accept = true;
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  WriteHttpResponse(client, response, head);
}

// Sends a complete OHLC 200 (JSON or protobuf by Accept) from candles the
// caller holds under response_mutex; an encoder overflow is a 500.
void send_ohlc(int client, const http::HttpRequest& request, bool head,
               const auth::DevicePolicyResult& policy, const fortis::prices::PriceSeriesCandle* candles,
               size_t count, int64_t timestamp_ms) {
  const uint8_t* body = nullptr;
  size_t length = 0;
  const char* content_type = "application/json; charset=utf-8";
  bool ok = true;
  if (AcceptsProtobuf(request)) {
    ok = add_proto_ohlc(candles, count, &length, true, timestamp_ms);
    body = protobuf_body;
    content_type = "application/protobuf";
  } else {
    JsonWriter writer{json_body, sizeof(json_body)};
    add_json_ohlc(writer, candles, count, true, timestamp_ms);
    ok = writer.ok;
    body = reinterpret_cast<const uint8_t*>(json_body);
    length = writer.used;
  }
  if (!ok) {
    send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  http::HttpResponse response{200, "OK", content_type, body, length};
  response.vary_accept = true;
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  WriteHttpResponse(client, response, head);
}

void send_upstream_unavailable(int client, bool head, const auth::DevicePolicyResult& policy,
                               int status = 502) {
  send_json(client, status, status == 503 ? "Service Unavailable" : "Bad Gateway",
            "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
            policy.rate_limit_cost, policy.rate_limit_remaining);
}

// GET /v1/prices/ohlc — OHLC candles for one id and a supported day window
// (TypeScript getTokenOhlc: TokenDiscoveryFacet.ohlc,
// CandleHistory.ohlc, CoinGeckoClient.ohlc, SwrCache).
//
// Policy. AuthorizeDeviceRequest runs first, as the TypeScript onRequest
// hooks do: a browser Origin is 403 BROWSER_ORIGIN_FORBIDDEN, a missing or
// unknown bearer 401 UNAUTHORIZED, and an authorized request is charged 5
// quota units (the development tier allows 240 per device minute; over
// quota is 429 RATE_LIMITED with Retry-After). Every later reply, a 400
// included, carries x-rate-limit-cost/-remaining.
//
// Query (price_ohlc_query.h): fast-querystring pairs (unknown keys, empty
// pairs and keys without '=' are ignored; malformed escapes stay raw text),
// then zod: `id` trimmed of JavaScript whitespace and 1–200 UTF-16 units;
// `fiat` absent or exactly "usd"; `days` coerced by JavaScript Number()
// (" 7 ", "7.0", "0x7", "3.65e2" all qualify) to one of 1, 7, 14, 30, 90,
// 180, 365. A repeated key fails. Any failure is the bare 400
// {"error":"BAD_REQUEST"} without Vary.
//
// Dispatch on the trimmed id:
//  - /^[a-z0-9][a-z0-9-]{0,99}$/ is a CoinGecko slug served through the
//    SWR cache below;
//  - anything else must be a price id (ParsePriceId; parsePriceId trims and
//    lower-cases bare names, so "Binancecoin" reads the store as
//    "binancecoin"); unrecognized is 400
//    {"error":"BAD_REQUEST","detail":"unsupported price id"}. A recognized
//    id reads the embedded price series for (now − days·24 h, now], both
//    ends inclusive: hourly candles up to 90 days (at most 2,161), daily
//    beyond (at most 366), ascending by start time; future candles are not
//    returned. cache is {hit:true, timestamp: the last candle's start as ISO
//    milliseconds, or null when empty}. No provider is asked. An id the
//    store cannot key reads as empty; a store I/O or corruption failure is
//    runFacet's 502 UPSTREAM_UNAVAILABLE.
//
// Slug path (TokenDiscoveryFacet.ohlc → SwrCache.read, TTL.chart): one
// durable KV entry per "ohlc:v1:<slug>:usd:<days>" holding the provider
// body verbatim (or [] for a non-array JSON answer) behind a header with
// asOf, fresh-until (as_of + 300 s jittered ±10 % from one sample) and
// stale-until (+1 h); the KV entry expires with stale-until. Under
// ohlc_cache_mutex:
//  - fresh: answer {hit:true, timestamp: asOf};
//  - stale: answer the same and start one background refresh per key (a
//    foreground-priority owner job, unless a failed refresh's backoff — 30 s,
//    5 s after a lane refusal — is running); it rewrites the entry on success;
//  - missing, expired or unreadable: replay a held cold-miss failure (502
//    for 10 s, or the 429 lane refusal with its retry-after for 5 s), else
//    defer the socket (HttpResponseHandoff, 30 s) and submit the slug to
//    OhlcProviderOwner with a 25 s deadline. Identical concurrent misses join
//    one owner flight (one CoinGecko request, every waiter answered
//    {hit:false, timestamp:null} with the same rows, like SwrCache's
//    single-flight). See ohlc_provider_complete for the outcomes.
// The owner asks GET /api/v3/coins/<slug>/ohlc?vs_currency=usd&days=<d>
// through the shared CoinGecko lane (burst 4, 24/min, breaker), retries
// transport errors, 5xx and 408 up to three attempts, and retries a 2xx body
// that is not JSON twice more (250 ms, 1 s) like HttpClient.json.
//
// Output. Points keep the provider's (or store's) order as
// [unix ms, open, high, low, close] with JavaScript number text; protobuf
// (Accept: application/protobuf, q > 0) is OhlcResponse as protobuf-es
// writes it, omitting proto3 defaults (+0 prices, unix_millis 0,
// cache.hit false). Every 200 carries Vary: Accept. HEAD runs the same
// handler and sends the headers with Content-Length and no body.
//
// Bounds and concurrency. All workspaces are BSS: the KV scratch and cache
// body are 256 KiB each under ohlc_cache_mutex; the JSON (384 KiB) and
// protobuf (256 KiB) bodies, the token arena (8,192 tokens) and candle
// buffer under response_mutex. A provider body must fit 256 KiB and 400
// rows of five numbers with a safe integer timestamp; otherwise it is an
// upstream failure. 16 deferred replies and the owner's 8 waiters/flights
// bound slug misses: no free slot or owner refusal is 503
// UPSTREAM_UNAVAILABLE, a full flight table a 502 (held like a failure).
//
// Intentional differences from TypeScript:
//  - malformed provider rows, over 400 rows or over 256 KiB are a 502 here;
//    TypeScript echoes any array verbatim in JSON;
//  - a recognized price id over 512 bytes (non-ASCII networks) is
//    "unsupported price id" here; TypeScript reads an empty series;
//  - a hung provider answers 502 at the 25 s owner deadline (TypeScript:
//    after its third 10 s attempt, about 31 s);
//  - a sole waiter that disconnects cancels its flight, so nothing is
//    cached (TypeScript finishes the fetch); joined waiters keep it alive;
//  - the owner and the lane charge one token per attempt; slot exhaustion
//    and shutdown answer 503;
//  - holds, backoffs and in-flight state are per process and lost on
//    restart (as TypeScript's in-memory maps are); the cache entries are
//    durable and survive a restart.
void handle_ohlc(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* context = static_cast<PriceHistoryRouteContext*>(opaque);
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 5,
                                                           context->attestation_required);
  if (policy.status) {
    send_json(client, policy.status, policy.reason,
      policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}", head,
      policy.rate_limit_cost, policy.rate_limit_remaining, policy.retry_after);
    return;
  }
  fortis::prices::ohlc_query::Query query{};
  const char* question = request.raw_target ? strchr(request.raw_target, '?') : nullptr;
  fortis::prices::ohlc_query::ParseQuery(question ? question + 1 : nullptr,
                                 question ? strlen(question + 1) : 0, &query);
  if (!query.ok) {
    send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head,
        policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  if (query.slug) {
    if (!context->ohlc_owner || !context->handoff || !context->kv) {
      send_upstream_unavailable(client, head, policy, 503);
      return;
    }
    char cache_key[160]{};
    snprintf(cache_key, sizeof(cache_key), "ohlc:v1:%s:usd:%d", query.id, query.days);
    OhlcCacheHeader cache{}; size_t cached_count = 0;
    fortis::base::UniqueLock ohlc_cache_lock(ohlc_cache_mutex);
    const int64_t now = ohlc_now_ms(context);
    const bool cached = read_ohlc_cache(context, cache_key, now, &cache,
        ohlc_cache_body, sizeof(ohlc_cache_body));
    fortis::base::UniqueLock response_lock(response_mutex);
    const bool valid_cached = cached && parse_provider_ohlc(ohlc_cache_body,
        cache.body_length, candle_buffer, kOhlcProviderMaxPoints, &cached_count) ==
        OhlcBody::kRows;
    if (valid_cached) {
      // SwrCache.revalidate(): a backoff from a failed refresh skips the
      // provider for this stale read; an expired backoff is cleared here.
      const bool stale = now >= cache.fresh_until_ms &&
          ohlc_policy.AllowRevalidate(cache_key, now);
      send_ohlc(client, request, head, policy, candle_buffer, cached_count, cache.as_of_ms);
      response_lock.Unlock();
      ohlc_cache_lock.Unlock();
      if (stale) {
        OhlcRequestSlot* refresh = acquire_ohlc_background_slot(context,
            query.id, query.days, cache_key);
        if (refresh) {
          uint64_t request_id = 0;
          fortis::base::UniqueLock ohlc_slots_lock(ohlc_slots_mutex); ++refresh->references;
          ohlc_slots_lock.Unlock(); // provider completion
          // The refresh has no HTTP waiter, but TypeScript runs coingecko.ohlc
          // as an ordinary lane job that queues for a token (lanes.run without
          // the background flag). Background lane priority would shed it
          // whenever the bucket is empty and every retry would need its own
          // admission, so the native refresh queues as foreground too.
          if (context->ohlc_owner->Submit(query.id, query.days,
              monotonic_ms() + kOwnerDeadlineMs,
              ohlc_provider_complete, refresh, &request_id, false)) {
            ohlc_slots_lock.Lock(); refresh->owner_request_id = request_id;
            ohlc_slots_lock.Unlock();
          } else release_ohlc_slot(refresh);
          release_ohlc_slot(refresh); // route callback
        }
      }
      return;
    }
    // Cold miss or past the stale window. SwrCache.read: a miss whose fetch
    // just failed replays that failure instead of queueing for the provider;
    // an expired hold is cleared here and the request proceeds.
    int replay_status = 0;
    const bool replay = ohlc_policy.MissReplay(cache_key, now, &replay_status);
    response_lock.Unlock();
    ohlc_cache_lock.Unlock();
    if (replay) {
      if (replay_status % 1000 == 429) {
        send_json(client, 429, "Too Many Requests", kOhlcRateLimited, head,
                  policy.rate_limit_cost, policy.rate_limit_remaining,
                  static_cast<unsigned>(replay_status / 1000));
        return;
      }
      // runFacet's replayed error carries no `vary` header.
      send_upstream_unavailable(client, head, policy, replay_status == 503 ? 503 : 502);
      return;
    }
    OhlcRequestSlot* slot = acquire_ohlc_slot(context);
    if (!slot) {
      send_upstream_unavailable(client, head, policy, 503);
      return;
    }
    snprintf(slot->slug, sizeof(slot->slug), "%s", query.id);
    snprintf(slot->cache_key, sizeof(slot->cache_key), "%s", cache_key);
    slot->days = query.days; slot->protobuf = AcceptsProtobuf(request);
    slot->rate_cost = policy.rate_limit_cost; slot->rate_remaining = policy.rate_limit_remaining;
    fortis::base::UniqueLock slot_lock(ohlc_slots_mutex); ++slot->references;
    slot_lock.Unlock(); // handoff terminal
    if (!http::ReserveHttpResponse(client, head, kPriceHistoryOwnerMaxDeadlineMs,
        &slot->handoff_token, ohlc_handoff_terminal, slot)) {
      release_ohlc_slot(slot); release_ohlc_slot(slot); return;
    }
    slot_lock.Lock(); ++slot->references;
    slot_lock.Unlock(); // provider completion
    uint64_t request_id = 0;
    if (!context->ohlc_owner->Submit(query.id, query.days,
        monotonic_ms() + kOwnerDeadlineMs,
        ohlc_provider_complete, slot, &request_id)) {
      release_ohlc_slot(slot);
      http::HttpResponse unavailable = http::JsonResponse(503, "Service Unavailable",
          "{\"error\":\"UPSTREAM_UNAVAILABLE\"}");
      unavailable.content_type = "application/json; charset=utf-8";
      unavailable.rate_limit_cost = policy.rate_limit_cost;
      unavailable.rate_limit_remaining = policy.rate_limit_remaining;
      context->handoff->Complete(slot->handoff_token, unavailable);
      release_ohlc_slot(slot); return;
    }
    slot_lock.Lock(); slot->owner_request_id = request_id;
    const bool cancelled = slot->cancelled; slot_lock.Unlock();
    if (cancelled) context->ohlc_owner->Cancel(request_id);
    release_ohlc_slot(slot); // route callback
    return;
  }
  providers::ParsedPriceId parsed{};
  if (!ParsePriceId(query.id, query.id_length, &parsed) ||
      parsed.kind == providers::PriceIdKind::kInvalid) {
    send_error(client, 400, "Bad Request", "unsupported price id", head,
        policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  const fortis::prices::PriceSeriesInterval interval = query.days <= 90
      ? fortis::prices::PriceSeriesInterval::k1h : fortis::prices::PriceSeriesInterval::k1d;
  const int64_t width = interval == fortis::prices::PriceSeriesInterval::k1h
      ? 3600000LL : 86400000LL;
  const int64_t now = ohlc_now_ms(context);
  const int64_t range_ms = static_cast<int64_t>(query.days) * 86400000LL;
  const int64_t from = now > range_ms ? now - range_ms : 0;
  // Both ends are inclusive: an aligned window holds range / width + 1 buckets.
  const size_t capacity = static_cast<size_t>(range_ms / width) + 1;
  static_assert(90 * 24 + 1 <= fortis::prices::kPriceSeriesMaxRangeCandles &&
                366 <= fortis::prices::kPriceSeriesMaxDailyRangeCandles,
                "the largest OHLC windows must fit one Range() read");

  fortis::base::UniqueLock series_lock(response_mutex);
  size_t count = 0;
  const auto read = context->series->Range(interval, "usd", parsed.source_id,
      from, now, now, candle_buffer, capacity, &count);
  // A parser-accepted id the store cannot key (a quote, control character or
  // non-ASCII byte in an EVM network name) has nothing stored: kInvalidKey
  // reads as the empty series, as TypeScript's read of that key does.
  if (read == fortis::prices::PriceSeriesReadStatus::kInvalidKey) count = 0;
  else if (read != fortis::prices::PriceSeriesReadStatus::kFound &&
           read != fortis::prices::PriceSeriesReadStatus::kMissing) {
    series_lock.Unlock();
    send_upstream_unavailable(client, head, policy);
    return;
  }
  send_ohlc(client, request, head, policy, candle_buffer, count,
            count ? candle_buffer[count - 1].ts : -1);
}
}  // namespace

bool RegisterPriceHistoryRoutes(http::HttpRouter& router, PriceHistoryRouteContext* context) {
  if (!context || !context->series || !context->owner || !context->ohlc_owner ||
      !context->kv || !context->handoff) return false;
  context->ohlc_owner->SetBodyRetryCheck(ohlc_body_unparseable);
  return router.Add("GET", "/v1/prices/history", handle_history, context) &&
      router.Add("GET", "/v1/prices/ohlc", handle_ohlc, context) &&
      RegisterPriceHistoryBatchRoute(router, context);
}

}  // namespace fortis::prices
