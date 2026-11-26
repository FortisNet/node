#include "exception_boundary.h"
#include "price_history_route.h"

// POST /v1/prices/history, registered in the normal image only behind
// FORTIS_NATIVE_PRICE_ROUTE_CANARY=1. The full contract is the comment above
// handle_batch. It reproduces the earlier TypeScript implementation (its
// prices route, CandleHistory and quota policy), frozen in
// tests/corpus/price_history_batch_route.json,
// price_history_batch_other_ids.json and price_history_batch_edges.json.
//
// Memory is fixed: five batch slots (four may wait on the provider), one
// 3,700-candle read workspace (the ten-year daily bound; hourly reads stay at
// 2,500), one 1 MiB response buffer and one JSON token arena, all in static
// storage and serialized by work_mutex. Provider work goes through the shared
// PriceHistoryBackfillOwner; one driver thread keeps at most eight per-ID
// histories of a batch active and retries owner Busy without a queue.

#include "bounded_json.h"
#include "device_enrollment.h"
#include "price_history_batch_codec.h"
#include "price_history_view.h"
#include "price_id_parser.h"
#include "threading.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

namespace {
using fortis::codec::history_batch::AppendInteger;
using fortis::codec::history_batch::AppendJsNumber;
using fortis::codec::history_batch::AppendJsonString;
using fortis::codec::history_batch::BatchProtoRequest;
using fortis::prices::history_batch::HistoryAvailability;
using fortis::prices::history_batch::HistoryView;
using fortis::prices::history_batch::HistoryWindow;
using fortis::codec::history_batch::OutBuffer;
using fortis::prices::history_batch::PutBool;
using fortis::prices::history_batch::PutImplicitString;
using fortis::prices::history_batch::PutInt64;
using fortis::prices::history_batch::PutMessage;
using fortis::prices::history_batch::PutString;

constexpr size_t kMaxIds = 50;
constexpr size_t kMaxIdUnits = 200;
// 200 UTF-16 units are at most 600 UTF-8 bytes (three per BMP unit, four
// per surrogate pair).
constexpr size_t kMaxKeyBytes = 3 * kMaxIdUnits;
constexpr size_t kActivePerBatch = 8;  // HISTORY_BATCH_CONCURRENCY
constexpr size_t kMaxGaps = 8;         // CandleHistory MAX_GAP_REQUESTS
// Four slots may wait on the provider; the fifth is only ever used by the
// one handler that holds work_mutex, so a cached batch always gets a slot.
constexpr size_t kDeferredSlots = 4;
constexpr size_t kBatchSlots = kDeferredSlots + 1;
constexpr int64_t kJsMaxSafeInteger = 9007199254740991LL;
// Response-size policy (BatchWorstResponseBytes): the handoff copies at most
// 1 MiB, so a request whose worst-case reply could exceed it is refused
// before any provider work.
constexpr size_t kResponseCapacity = 1024 * 1024;
constexpr uint32_t kHandoffTimeoutMs = kPriceHistoryOwnerMaxDeadlineMs;
// Owner requests end early enough to encode inside the handoff deadline.
constexpr uint64_t kOwnerDeadlineMs = 25000;
constexpr size_t kBitmapBytes = fortis::prices::history_batch::kHistoryBitmapBytes;
static_assert(kMaxGaps == fortis::prices::history_batch::kHistoryMaxGaps,
              "batch gap storage must match the coverage planner");

enum class ItemState : uint8_t {
  kUnrecognized, kStoreFailure, kSettled, kPending, kInFlight, kShutdown,
};

struct BatchItem {
  char key[kMaxKeyBytes + 1]{};
  uint16_t key_length = 0;
  uint8_t slot = 0;
  ItemState state = ItemState::kUnrecognized;
  // cache.hit is false only after this request filled at least one gap.
  bool fetched_live = false;
  bool has_initial = false;
  int64_t latest_initial = 0;  // seconds of the newest initially cached point
  bool bitmap_valid = false;
  uint8_t initial[kBitmapBytes]{};  // initially present buckets, from first_bucket
  uint8_t gap_count = 0, next_gap = 0;
  int64_t gaps[kMaxGaps][2]{};
  uint64_t owner_request_id = 0;
};

struct BatchSlot {
  bool used = false;
  bool deferred = false;        // the driver owns completion
  bool finalizing = false;
  bool finalized = false;
  bool terminal_pending = false;
  bool client_gone = false;
  bool shutdown = false;
  unsigned in_flight = 0;
  uint64_t handoff_token = 0;
  uint64_t deadline_ms = 0;
  bool protobuf = false;
  int rate_cost = 0;
  uint32_t rate_remaining = 0;
  int64_t from = 0, to = 0, width = 0, first_bucket = 0;
  bool hourly = true;
  size_t expected = 0, range_capacity = 0;
  size_t count = 0;
  uint8_t order[kMaxIds]{};
  BatchItem items[kMaxIds];
};

PriceHistoryRouteContext* route = nullptr;
BatchSlot slots[kBatchSlots];
fortis::base::Mutex batch_mutex;
fortis::base::ConditionVariable batch_changed;
fortis::base::ThreadHandle driver_thread;
bool driver_started = false;
bool stopping = false;

// work_mutex owns every buffer below. Lock order: work_mutex, then
// batch_mutex, then the owner's mutex (Submit/Cancel); never take work_mutex
// while holding batch_mutex. Series reads, including the coverage record,
// happen only under work_mutex (series lock, then the KV lock, innermost);
// never call the series store while holding batch_mutex.
fortis::base::Mutex work_mutex;
uint8_t response_body[kResponseCapacity];
codec::BoundedJsonToken json_tokens[codec::kBoundedJsonMaxTokens];
char decode_scratch[codec::kBoundedJsonMaxInputBytes + 8];
fortis::prices::PriceSeriesCandle candles[fortis::prices::kPriceSeriesMaxDailyRangeCandles];
// 3,700 x 72 bytes = 266,400 bytes.
static_assert(sizeof(candles) < 272 * 1024, "batch candle workspace must stay below 272 KiB");

uint64_t monotonic_ms() {
  timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

timespec realtime_after(uint64_t milliseconds) {
  timespec deadline{}; clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += static_cast<time_t>(milliseconds / 1000);
  deadline.tv_nsec += static_cast<long>((milliseconds % 1000) * 1000000);
  if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
  return deadline;
}

// ── HTTP replies ──

void send_body(int client, int status, const char* reason, const char* body,
               int rate_cost = -1, uint32_t remaining = 0) {
  http::HttpResponse response{status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = rate_cost;
  response.rate_limit_remaining = remaining;
  WriteHttpResponse(client, response);
}

const char* kUpstreamUnavailable = "{\"error\":\"UPSTREAM_UNAVAILABLE\"}";

// ── Request body ──

enum class BodyKind : uint8_t { kUndefined, kJson, kProtobuf };

struct RequestBody {
  BodyKind kind = BodyKind::kUndefined;
  codec::BoundedJsonDocument json;
  BatchProtoRequest proto;
  bool proto_valid = false;  // DecodeBatchProtoRequest accepted the body
  const uint8_t* bytes = nullptr;
  size_t length = 0;
};

const codec::BoundedJsonToken* token(const RequestBody& body, uint32_t index) {
  return index == codec::kBoundedJsonNoToken ? nullptr : body.json.token(index);
}

size_t child_count(const RequestBody& body, uint32_t index) {
  const codec::BoundedJsonToken* item = token(body, index);
  size_t count = 0;
  if (!item) return 0;
  for (uint32_t child = item->first_child; child != codec::kBoundedJsonNoToken;
       child = body.json.token(child)->next_sibling) ++count;
  return count;
}

uint32_t root_field(const RequestBody& body, const char* name) {
  const codec::BoundedJsonToken* root = token(body, body.json.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) return codec::kBoundedJsonNoToken;
  return body.json.FindObjectValue(body.json.root(), name);
}

// The TypeScript requestCost: the raw `ids` length of a parsed JSON object
// body or of a decoded protobuf body, before dedup or validation, at least 1.
// A malformed protobuf body costs 1.
uint32_t request_cost(const RequestBody& body) {
  if (body.kind == BodyKind::kProtobuf) {
    // A 512 KiB body holds at most 256 Ki ids, far below INT32_MAX.
    return body.proto_valid && body.proto.id_count
        ? static_cast<uint32_t>(body.proto.id_count) : 1;
  }
  if (body.kind != BodyKind::kJson) return 1;
  const uint32_t ids = root_field(body, "ids");
  const codec::BoundedJsonToken* item = token(body, ids);
  if (!item || item->type != codec::BoundedJsonType::kArray) return 1;
  const size_t count = child_count(body, ids);
  return count ? static_cast<uint32_t>(count) : 1;
}

// Decoded UTF-8 of a JSON string token in decode_scratch.
bool decode_json_string(const RequestBody& body, uint32_t index, size_t* length) {
  return body.json.DecodeString(index, decode_scratch, sizeof(decode_scratch), length);
}

// Request bodies require JSON numbers; strings, bools and arrays are not
// silently converted. Range and integer validation follows below.
double json_number(const RequestBody& body, uint32_t index) {
  const auto* item = token(body, index);
  double value = NAN;
  return item && item->type == codec::BoundedJsonType::kNumber && body.json.Number(index, &value)
      ? value : NAN;
}

// One `ids` element after the trim: anything but a 1–200 unit string is
// refused.
bool id_string_valid(const char* text, size_t length) {
  size_t begin = 0, end = 0;
  fortis::codec::history_batch::JsTrim(text, length, &begin, &end);
  const size_t units = fortis::codec::history_batch::Utf16Units(text + begin, end - begin);
  return units >= 1 && units <= kMaxIdUnits;
}

bool ids_valid(const RequestBody& body, size_t* id_count) {
  *id_count = 0;
  bool valid = true;
  if (body.kind == BodyKind::kProtobuf) {
    size_t cursor = 0;
    const uint8_t* id = nullptr; size_t length = 0;
    while (fortis::codec::history_batch::NextBatchProtoId(body.bytes, body.length, &cursor,
                                                   &id, &length)) {
      if (!id_string_valid(reinterpret_cast<const char*>(id), length)) valid = false;
    }
    *id_count = body.proto.id_count;
  } else {
    const uint32_t ids = root_field(body, "ids");
    const codec::BoundedJsonToken* item = token(body, ids);
    if (!item || item->type != codec::BoundedJsonType::kArray) return false;
    size_t index = 0;
    for (uint32_t child = item->first_child; child != codec::kBoundedJsonNoToken;
         child = body.json.token(child)->next_sibling, ++index) {
      size_t length = 0;
      if (body.json.token(child)->type != codec::BoundedJsonType::kString ||
          !decode_json_string(body, child, &length) ||
          !id_string_valid(decode_scratch, length))
        valid = false;
    }
    *id_count = index;
  }
  return valid && *id_count >= 1 && *id_count <= kMaxIds;
}

// z.coerce.number().int().safe().positive().
bool integer_valid(double value, int64_t* output) {
  if (!isfinite(value) || floor(value) != value) return false;
  if (fabs(value) > static_cast<double>(kJsMaxSafeInteger) || !(value > 0)) return false;
  *output = static_cast<int64_t>(value);
  return true;
}

// .strict(): any own key outside the shape is refused.
bool has_unrecognized_key(const RequestBody& body) {
  const codec::BoundedJsonToken* root = token(body, body.json.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) return false;
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value_index = body.json.token(key)->next_sibling;
    if (!body.json.StringEquals(key, "ids") && !body.json.StringEquals(key, "from") &&
        !body.json.StringEquals(key, "to") && !body.json.StringEquals(key, "interval"))
      return true;
    key = body.json.token(value_index)->next_sibling;
  }
  return false;
}

struct ValidRequest {
  int64_t from = 0, to = 0;
  bool hourly = true;
  size_t id_count = 0;
};

// Mirrors historyBatchBody.safeParse.
bool validate(const RequestBody& body, ValidRequest* request) {
  const codec::BoundedJsonToken* root = body.kind == BodyKind::kJson
      ? token(body, body.json.root()) : nullptr;
  bool valid = !(body.kind == BodyKind::kUndefined ||
                 (body.kind == BodyKind::kJson && root->type != codec::BoundedJsonType::kObject));
  if (valid) {
    if (!ids_valid(body, &request->id_count)) valid = false;
    const double from = body.kind == BodyKind::kProtobuf
        ? static_cast<double>(body.proto.from) : json_number(body, root_field(body, "from"));
    const double to = body.kind == BodyKind::kProtobuf
        ? static_cast<double>(body.proto.to) : json_number(body, root_field(body, "to"));
    if (!integer_valid(from, &request->from)) valid = false;
    if (!integer_valid(to, &request->to)) valid = false;
    if (body.kind == BodyKind::kProtobuf) {
      if (body.proto.has_interval) {
        const auto* value = reinterpret_cast<const char*>(body.proto.interval);
        const size_t length = body.proto.interval_length;
        if (length == 2 && memcmp(value, "1d", 2) == 0) request->hourly = false;
        else if (!(length == 2 && memcmp(value, "1h", 2) == 0)) valid = false;
      }
    } else {
      const uint32_t interval = root_field(body, "interval");
      if (interval != codec::kBoundedJsonNoToken) {
        if (body.json.StringEquals(interval, "1d")) request->hourly = false;
        else if (!body.json.StringEquals(interval, "1h")) valid = false;
      }
      if (has_unrecognized_key(body)) valid = false;
    }
  }
  return valid;
}

// The trimmed `ids`, in request order, for dedup and slot filling.
bool next_trimmed_id(const RequestBody& body, size_t* cursor, uint32_t* json_child,
                     const char** text, size_t* length) {
  const char* raw = nullptr; size_t raw_length = 0;
  if (body.kind == BodyKind::kProtobuf) {
    const uint8_t* id = nullptr;
    if (!fortis::codec::history_batch::NextBatchProtoId(body.bytes, body.length, cursor, &id,
                                                 &raw_length)) return false;
    raw = reinterpret_cast<const char*>(id);
  } else {
    if (*json_child == codec::kBoundedJsonNoToken) return false;
    const uint32_t child = *json_child;
    *json_child = body.json.token(child)->next_sibling;
    if (!decode_json_string(body, child, &raw_length)) return false;
    raw = decode_scratch;
  }
  size_t begin = 0, end = 0;
  fortis::codec::history_batch::JsTrim(raw, raw_length, &begin, &end);
  *text = raw + begin; *length = end - begin;
  return true;
}

// ── Slots ──

void reset_slot(BatchSlot& slot) {
  slot.used = slot.deferred = slot.finalizing = slot.finalized = false;
  slot.terminal_pending = slot.client_gone = slot.shutdown = false;
  slot.in_flight = 0; slot.handoff_token = 0; slot.deadline_ms = 0; slot.count = 0;
}

// Caller holds batch_mutex.
void maybe_release(BatchSlot& slot) {
  if (slot.used && slot.finalized && !slot.in_flight && !slot.terminal_pending)
    reset_slot(slot);
}

BatchSlot* acquire_slot() {
  fortis::base::LockGuard batch_lock(batch_mutex);
  BatchSlot* result = nullptr;
  if (!stopping) {
    for (size_t i = 0; i < kBatchSlots; ++i) if (!slots[i].used) {
      reset_slot(slots[i]); slots[i].used = true; result = &slots[i];
      for (size_t j = 0; j < kMaxIds; ++j) slots[i].items[j].slot = static_cast<uint8_t>(i);
      break;
    }
  }
  return result;
}

void release_unused(BatchSlot* slot) {
  fortis::base::LockGuard batch_lock(batch_mutex);
  slot->finalized = true; slot->terminal_pending = false;
  maybe_release(*slot);
}

// ── Series reads (caller holds work_mutex) ──

fortis::prices::PriceSeriesInterval series_interval(const BatchSlot& slot) {
  return slot.hourly ? fortis::prices::PriceSeriesInterval::k1h : fortis::prices::PriceSeriesInterval::k1d;
}

// One window read of a parser-accepted ID. Found and Missing are its stored
// series. kInvalidKey is the empty series only when the store cannot key the
// source ID, because nothing can ever be stored under it (CandleHistory reads
// such an ID as empty). Every other status, including kInvalidKey for a
// storable ID, is a per-ID UPSTREAM_UNAVAILABLE.
bool read_range(const BatchSlot& slot, const char* source_id, size_t* count) {
  *count = 0;
  // A test binary may report this source ID's reads as failed.
  const PriceHistoryRouteSeams* seams = route->seams;
  if (seams && seams->fail_source_read &&
      seams->fail_source_read(seams->context, source_id)) return false;
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  const auto status = route->series->Range(series_interval(slot), "usd", source_id,
      slot.from * 1000, slot.to * 1000, now, candles, slot.range_capacity, count);
  if (status == fortis::prices::PriceSeriesReadStatus::kInvalidKey) {
    *count = 0;
    return !fortis::prices::history_batch::HistorySourceIdStorable(source_id);
  }
  return status == fortis::prices::PriceSeriesReadStatus::kFound ||
      status == fortis::prices::PriceSeriesReadStatus::kMissing;
}

HistoryWindow window_of(const BatchSlot& slot) {
  HistoryWindow window{};
  window.width = slot.width; window.first_bucket = slot.first_bucket;
  window.expected = slot.expected;
  return window;
}

// Records the initial read and CandleHistory's gap plan for one CoinGecko ID:
// missingCandleRanges, then subtractCoveredRanges against the ID's coverage
// record for this interval, then the >8-gaps collapse. The record is read
// only when a bucket is missing; a missing, expired, corrupt or unreadable
// record counts as uncovered (readCoveredRanges' catch -> []), so the worst
// case is a refetch, never a skipped hole.
void plan_gaps(const BatchSlot& slot, BatchItem& item, size_t count,
               const char* source_id) {
  const HistoryWindow window = window_of(slot);
  fortis::prices::history_batch::MarkHistoryBuckets(window, candles, count, item.initial);
  item.bitmap_valid = true;
  item.gap_count = 0; item.next_gap = 0;
  if (!fortis::prices::history_batch::HistoryAnyBucketMissing(window, item.initial)) return;
  fortis::prices::HistoryCoverage covered{};
  const bool has_coverage = route->series->ReadHistoryCoverage(series_interval(slot),
      "usd", source_id, PriceHistoryCoverageNowMs(), &covered) ==
      fortis::prices::PriceSeriesReadStatus::kFound;
  item.gap_count = static_cast<uint8_t>(fortis::prices::history_batch::PlanHistoryGaps(
      window, item.initial, has_coverage ? &covered : nullptr, item.gaps));
}

// Fills the slot's items from the validated, deduplicated IDs and reads the
// store once per ID. Returns true when any ID still needs the provider.
bool initial_pass(BatchSlot& slot) {
  bool pending = false;
  providers::ParsedPriceId parsed{};
  for (size_t i = 0; i < slot.count; ++i) {
    BatchItem& item = slot.items[i];
    item.fetched_live = item.has_initial = item.bitmap_valid = false;
    item.latest_initial = 0; item.gap_count = item.next_gap = 0;
    item.owner_request_id = 0;
    parsed = providers::ParsedPriceId{};
    if (!ParsePriceId(item.key, item.key_length, &parsed) ||
        parsed.kind == providers::PriceIdKind::kInvalid) {
      item.state = ItemState::kUnrecognized;
      continue;
    }
    size_t count = 0;
    if (!read_range(slot, parsed.source_id, &count)) {
      item.state = ItemState::kStoreFailure;
      continue;
    }
    if (count) { item.has_initial = true; item.latest_initial = candles[count - 1].ts / 1000; }
    item.state = ItemState::kSettled;
    // Only CoinGecko IDs are backfilled, and only into a storable series.
    if (parsed.kind != providers::PriceIdKind::kCoinGecko || !route->owner || !route->handoff ||
        !fortis::prices::history_batch::HistorySourceIdStorable(parsed.source_id)) continue;
    plan_gaps(slot, item, count, parsed.source_id);
    // A fully covered ID settles here: no provider call and cache.hit stays true.
    if (item.gap_count) { item.state = ItemState::kPending; pending = true; }
  }
  return pending;
}

// ── Response encoding (caller holds work_mutex) ──

void write_iso(OutBuffer& out, int64_t seconds) {
  char text[32]{};
  if (!fortis::prices::history_batch::HistoryTimestampIso(seconds, text)) { out.ok = false; return; }
  out.Byte('"'); out.Text(text); out.Byte('"');
}

bool read_availability(const char* source_id, HistoryAvailability* availability) {
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  return fortis::prices::history_batch::ReadHistoryAvailability(*route->series, source_id, now,
                                                         availability);
}

void json_failure(OutBuffer& out, const char* error) {
  out.Text("{\"error\":\""); out.Text(error);
  out.Text("\",\"availability\":{\"from\":null,\"to\":null,\"days\":0,\"resolutions\":[]}}");
}

void proto_failure(OutBuffer& out, const char* error) {
  PutImplicitString(out, 1, error);
  PutMessage(out, 2, [](OutBuffer&) {});
}

// Preserve request insertion order in JSON maps and protobuf entries.
void output_order(BatchSlot& slot) {
  for (size_t i = 0; i < slot.count; ++i) slot.order[i] = static_cast<uint8_t>(i);
}

// Builds the 200 response in response_body. Returns false on an internal
// encoding failure (the size policy should make this unreachable).
bool encode_batch(BatchSlot& slot, size_t* length) {
  OutBuffer out{response_body, sizeof(response_body)};
  const char* interval = slot.hourly ? "1h" : "1d";
  bool all_cached = true, any_cached = false, has_latest = false;
  int64_t latest = 0;
  if (slot.protobuf) {
    PutInt64(out, 1, slot.from); PutInt64(out, 2, slot.to);
    PutImplicitString(out, 3, interval);
  } else {
    out.Text("{\"from\":"); AppendInteger(out, slot.from);
    out.Text(",\"to\":"); AppendInteger(out, slot.to);
    out.Text(",\"interval\":\""); out.Text(interval); out.Text("\",\"series\":{");
  }
  output_order(slot);
  providers::ParsedPriceId parsed{};
  for (size_t position = 0; position < slot.count; ++position) {
    BatchItem& item = slot.items[slot.order[position]];
    const char* failure = nullptr;
    HistoryView view{};
    if (item.state == ItemState::kUnrecognized) {
      failure = "UNRECOGNIZED_ID";
    } else if (item.state != ItemState::kSettled) {
      failure = "UPSTREAM_UNAVAILABLE";
    } else {
      parsed = providers::ParsedPriceId{};
      ParsePriceId(item.key, item.key_length, &parsed);
      size_t count = 0;
      if (!read_range(slot, parsed.source_id, &count) ||
          !read_availability(parsed.source_id, &view.availability)) {
        failure = "UPSTREAM_UNAVAILABLE";
      } else {
        view.source_id = parsed.source_id; view.interval = interval;
        view.candles = candles; view.count = count;
        view.hit = !item.fetched_live;
        view.has_timestamp = item.has_initial; view.timestamp = item.latest_initial;
        view.cached = count;
        // coverage.cachedPoints: final points that existed before the request.
        if (item.bitmap_valid)
          view.cached = fortis::prices::history_batch::CountInitiallyCached(window_of(slot),
              item.initial, candles, count);
      }
    }
    if (failure) {
      all_cached = false;
    } else {
      if (!view.hit) all_cached = false;
      if (view.hit || view.has_timestamp) any_cached = true;
      if (view.has_timestamp && (!has_latest || view.timestamp > latest)) {
        has_latest = true; latest = view.timestamp;
      }
    }
    if (slot.protobuf) {
      PutMessage(out, 4, [&](OutBuffer& entry) {
        PutString(entry, 1, item.key, item.key_length);
        if (failure) PutMessage(entry, 3, [&](OutBuffer& body) { proto_failure(body, failure); });
        else PutMessage(entry, 2, [&](OutBuffer& body) {
          fortis::prices::history_batch::AppendHistoryProto(body, view);
        });
      });
    } else {
      if (position) out.Byte(',');
      AppendJsonString(out, item.key, item.key_length);
      out.Byte(':');
      if (failure) json_failure(out, failure);
      else fortis::prices::history_batch::AppendHistoryJson(out, view);
    }
  }
  const bool partial = any_cached && !all_cached;
  if (slot.protobuf) {
    PutMessage(out, 5, [&](OutBuffer& cache) {
      PutBool(cache, 1, all_cached);
      if (has_latest) {
        char text[32]{};
        if (!fortis::prices::history_batch::HistoryTimestampIso(latest, text)) {
          cache.ok = false; return;
        }
        PutString(cache, 2, text, strlen(text));
      }
      PutBool(cache, 3, partial);
    });
  } else {
    out.Text("},\"cache\":{\"hit\":"); out.Text(all_cached ? "true" : "false");
    out.Text(",\"timestamp\":");
    if (has_latest) write_iso(out, latest); else out.Text("null");
    out.Text(",\"partial\":"); out.Text(partial ? "true" : "false"); out.Text("}}");
  }
  *length = out.used;
  return out.ok;
}

http::HttpResponse batch_response(BatchSlot& slot, bool encode) {
  http::HttpResponse response = http::JsonResponse(503, "Service Unavailable", kUpstreamUnavailable);
  if (encode) {
    size_t length = 0;
    if (encode_batch(slot, &length)) {
      response = http::HttpResponse{200, "OK",
          slot.protobuf ? "application/protobuf" : "application/json; charset=utf-8",
          response_body, length};
      // TypeScript sets Vary only from sendProtobufOrJson, on the 200.
      response.vary_accept = true;
    } else {
      response = http::JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    }
  }
  response.rate_limit_cost = slot.rate_cost;
  response.rate_limit_remaining = slot.rate_remaining;
  return response;
}

// ── Owner, handoff and driver ──

void owner_complete(void* opaque, const PriceHistoryOwnerResult& result) {
  auto* item = static_cast<BatchItem*>(opaque);
  fortis::base::LockGuard batch_lock(batch_mutex);
  BatchSlot& slot = slots[item->slot];
  item->owner_request_id = 0;
  if (result.status == PriceHistoryOwnerResultStatus::kShutdown) {
    item->state = ItemState::kShutdown; slot.shutdown = true;
  } else {
    // CandleHistory swallows a failed backfill: the ID keeps its cached
    // points and only a filled gap makes cache.hit false.
    // A caller that joined another request's flight is TypeScript's lock
    // loser: it waits, re-reads the filled candles and keeps cache.hit true.
    if (result.status == PriceHistoryOwnerResultStatus::kComplete && !result.joined)
      item->fetched_live = true;
    ++item->next_gap;
    item->state = item->next_gap < item->gap_count ? ItemState::kPending : ItemState::kSettled;
  }
  if (slot.in_flight) --slot.in_flight;
  maybe_release(slot);
  batch_changed.Broadcast();
}

void handoff_terminal(void* opaque, bool cancelled) {
  auto* slot = static_cast<BatchSlot*>(opaque);
  fortis::base::LockGuard batch_lock(batch_mutex);
  slot->terminal_pending = false;
  if (cancelled) slot->client_gone = true;
  maybe_release(*slot);
  batch_changed.Broadcast();
}

// Caller holds batch_mutex. Cancels owner waiters of a slot whose client or
// process is going away; their completions still arrive and are counted.
void cancel_in_flight(BatchSlot& slot) {
  for (size_t i = 0; i < slot.count; ++i) {
    BatchItem& item = slot.items[i];
    if (item.state == ItemState::kInFlight && item.owner_request_id)
      route->owner->Cancel(item.owner_request_id);
    if (item.state == ItemState::kPending) item.state = ItemState::kShutdown;
  }
}

// Caller holds batch_mutex. Submits pending gaps while fewer than eight
// histories are active. Returns true when the owner reported Busy.
bool advance(BatchSlot& slot, uint64_t now) {
  bool busy = false;
  providers::ParsedPriceId parsed{};
  for (size_t i = 0; i < slot.count && slot.in_flight < kActivePerBatch && !busy; ++i) {
    BatchItem& item = slot.items[i];
    if (item.state != ItemState::kPending) continue;
    if (slot.shutdown) { item.state = ItemState::kShutdown; continue; }
    if (now >= slot.deadline_ms) {
      // The gaps this request could not start are treated like a failed
      // backfill: the cached points are served with cache.hit unchanged.
      item.state = ItemState::kSettled;
      continue;
    }
    parsed = providers::ParsedPriceId{};
    ParsePriceId(item.key, item.key_length, &parsed);
    uint64_t request_id = 0;
    const auto status = route->owner->Submit(parsed.address, parsed.source_id, "usd",
        series_interval(slot), item.gaps[item.next_gap][0], item.gaps[item.next_gap][1],
        slot.deadline_ms,
        owner_complete, &item, &request_id);
    switch (status) {
      case PriceHistoryOwnerSubmitStatus::kAccepted:
        item.state = ItemState::kInFlight; item.owner_request_id = request_id;
        ++slot.in_flight;
        break;
      case PriceHistoryOwnerSubmitStatus::kBusy:
        busy = true;
        break;
      case PriceHistoryOwnerSubmitStatus::kInvalidRequest:
        ++item.next_gap;
        if (item.next_gap >= item.gap_count) item.state = ItemState::kSettled;
        break;
      case PriceHistoryOwnerSubmitStatus::kNotRunning:
      case PriceHistoryOwnerSubmitStatus::kStopping:
        item.state = ItemState::kShutdown; slot.shutdown = true;
        break;
    }
  }
  return busy;
}

bool slot_ready(const BatchSlot& slot) {
  if (slot.in_flight) return false;
  for (size_t i = 0; i < slot.count; ++i)
    if (slot.items[i].state == ItemState::kPending) return false;
  return true;
}

// Completes one deferred slot outside batch_mutex.
void finalize(BatchSlot& slot, bool force_unavailable) {
  fortis::base::UniqueLock work_lock(work_mutex);
  const http::HttpResponse response = batch_response(slot, !force_unavailable && !slot.shutdown);
  route->handoff->Complete(slot.handoff_token, response);
  work_lock.Unlock();
  fortis::base::LockGuard batch_lock(batch_mutex);
  slot.finalizing = false; slot.finalized = true;
  maybe_release(slot);
}

void* driver_main(void*) {
  fortis::base::UniqueLock batch_lock(batch_mutex);
  while (!stopping) {
    BatchSlot* ready = nullptr;
    bool busy = false;
    const uint64_t now = monotonic_ms();
    for (BatchSlot& slot : slots) {
      if (!slot.used || !slot.deferred || slot.finalized || slot.finalizing) continue;
      if (slot.client_gone) {
        cancel_in_flight(slot);
        slot.finalized = true;
        maybe_release(slot);
        continue;
      }
      busy = advance(slot, now) || busy;
      if (slot_ready(slot)) { ready = &slot; break; }
    }
    if (ready) {
      ready->finalizing = true;
      batch_lock.Unlock();
      finalize(*ready, false);
      batch_lock.Lock();
      continue;
    }
    // Owner Busy has no completion signal for this batch, so poll briefly;
    // otherwise wake for completions, terminals, or the next deadline.
    const timespec wake = realtime_after(busy ? 25 : 250);
    batch_changed.WaitUntil(batch_lock, wake);
  }
  // Shutdown: answer every waiting client now. Owner completions for
  // cancelled waiters arrive later and only release their slots.
  for (BatchSlot& slot : slots) {
    if (!slot.used || !slot.deferred || slot.finalized || slot.finalizing) continue;
    cancel_in_flight(slot);
    slot.finalizing = true;
    batch_lock.Unlock();
    finalize(slot, true);
    batch_lock.Lock();
  }
  return nullptr;
}

// ── Handler ──

// POST /v1/prices/history — the TypeScript getPriceHistoryBatch
// over PricesFacet.history → CandleHistory.history
// for each ID: USD close-price series of up to 50 IDs over one window, for
// the app's comparison charts. The GET with the same path (one ID, query
// string) is handle_history in price_history_route.cpp.
//
// Order. 1) Body parsing, decided first but answered after the device
// policy (auth-first: an unauthenticated request is 401 whatever its body),
// at cost 1 with the quota headers: application/json (any case, parameters
// ignored; one leading BOM dropped; empty or invalid JSON → 400
// {"error":"BAD_REQUEST"}); application/protobuf (bytes, decoded once
// here); no Content-Type with no body (undefined); anything else
// (text/plain and form bodies included), or a body without Content-Type →
// 415 {"error":"UNSUPPORTED_MEDIA_TYPE"}.
// 2) Device policy (AuthorizeDeviceRequest): Origin present → 403
// BROWSER_ORIGIN_FORBIDDEN; missing/malformed/unknown bearer → 401
// UNAUTHORIZED; revoked/expired → 401 SESSION_EXPIRED; attestation required
// and missing → 401 ATTESTATION_REQUIRED. Then the per-device minute quota
// (development 240, default 120, attested 600, premium 1200) charged with a
// weight: the raw length of `ids` of a JSON object body or of a decoded
// protobuf body, before validation and dedupe, at least 1 (a malformed
// protobuf or a non-array `ids` costs 1). Every later reply carries
// x-rate-limit-cost and x-rate-limit-remaining; over quota → 429
// {"error":"RATE_LIMITED"} with Retry-After: 60.
// 3) Validation, all as 400 with the quota headers: a protobuf body
// protobuf-es rejects (truncation, invalid UTF-8 strings, a lone end-group)
// → {"error":"BAD_REQUEST","detail":"invalid protobuf body"}. Then the schema
// (historyBatchBody, .strict()) refuses with {"error":"BAD_REQUEST"}: the
// body must be an object;
// ids: an array of 1–50 strings, each 1–200 UTF-16 units after
// String.prototype.trim; from, to: JavaScript Number() of the value (so
// "0x…", " 1767225600 ", [1767225600] and 1.7672256e9 are accepted; "" and
// [] are 0; null/false are 0, true is 1), then int, safe
// and positive; interval: "1h" or "1d", default "1h" (null is invalid);
// any unknown key (`__proto__` included) is refused. A protobuf body
// has int64 from/to (absent = 0, Number(BigInt)), optional interval and no
// strict check. Then the range: to > from and to − from ≤ 7,776,000 s
// (90 days) for 1h or 316,224,000 s (10 × 366 days) for 1d, else 400
// {"error":"BAD_REQUEST","detail":"invalid history range"}. IDs are never
// rejected as a whole after this point.
//
// IDs and per-ID results. The trimmed IDs are deduplicated by exact string
// (a Set: "Ethereum" and "ethereum" stay two entries) in first-seen order;
// each gets one entry keyed by its trimmed spelling. ParsePriceId failing →
// {"error":"UNRECOGNIZED_ID","availability":{"from":null,"to":null,"days":0,
// "resolutions":[]}}; a store read failure (CandleHistory throws) → the
// same with "UPSTREAM_UNAVAILABLE"; otherwise the GET body for that ID
// (canonical id, points, volumes, cache, coverage, window, availability).
// A provider failure is not an item failure: CandleHistory swallows it and
// the entry is the stored series with cache.hit unchanged. Entries enumerate
// like Object.entries: canonical array-index keys ("0"…"4294967294")
// ascending first, then insertion order; `__proto__` is an ordinary key.
//
// Provenance. cache.hit is true only when every entry is a history with
// cache.hit true; cache.timestamp is the newest per-entry timestamp (the
// newest point that existed before the request) or null; cache.partial is
// true when some entry had a hit or a timestamp but not every entry hit.
// Unrecognized or failed entries count as not hit.
//
// Response. 200 with Vary: Accept and either JSON {"from","to","interval",
// "series":{…},"cache":{"hit","timestamp","partial"}} (application/json;
// charset=utf-8) or PriceHistoryBatchResponse protobuf (application/
// protobuf; entries in the same order, failures in the `failure` oneof)
// when Accept names application/protobuf with q > 0. Every reply has
// Cache-Control: no-store and the security header baseline. Error replies
// never carry Vary.
//
// Cache and provider (per ID, as GET /v1/prices/history): the candle store
// is the cache. Only CoinGecko slugs are backfilled; every other recognized
// ID answers its stored series. Missing bucket runs minus the interval's
// coverage record are gaps (more than eight collapse into one aligned
// window); each gap is one CoinGecko market_chart/range request on the shared
// lane (burst 4, 24/min, 2 concurrent; transport/5xx/408 retried twice);
// an answer, even empty, is stored as 1h and 1d candles and marks the gap
// covered for 6 h; any failure leaves it uncovered. At most eight IDs of a
// batch are in flight (HISTORY_BATCH_CONCURRENCY). Identical gap windows
// share one owner flight; a request that joined another's flight is
// TypeScript's backfill-lock loser and keeps cache.hit true.
//
// Persistence: candles and coverage records are durable KV frames (survive
// SIGKILL); candles are kept until max(now + 7 days, segment end + 90 days
// for 1h or 366 days for 1d); coverage records expire after 6 h. Nothing is
// written for a request that needs no provider work.
//
// Concurrency, deadline and bounds. Cached-only batches are answered on the
// HTTP worker. Batches with gaps move to the response handoff and a driver
// thread; the gap plan stops 25 s after the request (unstarted gaps count as
// failed backfills) and the reply is sent before the 30 s handoff limit.
// Bounded native differences (TypeScript has none of these limits):
// - Response-size policy: a request whose worst-case reply could exceed the
//   1 MiB handoff (BatchWorstResponseBytes: JSON 90, protobuf 40 bytes per
//   candle) → 503 {"error":"UPSTREAM_UNAVAILABLE"} before provider work
//   (e.g. 6 JSON or 12 protobuf IDs over 90 hourly days; 50 IDs fit the
//   app's protobuf windows);
// - at most four batches wait on the provider: a fifth → 503; a cached
//   batch is still served; the owner holds 8 requests (a full owner ends the
//   gap plan like a failure); shutdown → 503; an encoder failure → 500;
// - JSON bodies are read by the bounded parser: over 256 KiB, 8,192 tokens or
//   depth 16 → 413 {"error":"BODY_TOO_LARGE"} (after the device policy, at
//   cost 1); a number that overflows a
//   double or has 64+ characters is invalid JSON (TypeScript: Infinity, then
//   a zod issue); the HTTP framing refuses bodies over 512 KiB (413);
// - IDs over 600 UTF-8 bytes cannot pass zod; canonical IDs over 512 bytes
//   are unrecognized (TypeScript: an empty series);
// - a joined request waits up to its own deadline (TypeScript's lock loser
//   ≤ 10 s); a sole caller that disconnects cancels its fetch.
// Privacy: replies and logs never carry the bearer or the device id; error
// bodies are fixed strings.
void handle_batch(int client, const http::HttpRequest& request, bool head, void*) {
  static_cast<void>(head);
  // 1. Content-type parsing. A refusal is decided here but answered after
  // the device policy (auth-first), at cost 1.
  fortis::base::UniqueLock work_lock(work_mutex);
  RequestBody body{};
  body.bytes = reinterpret_cast<const uint8_t*>(request.body);
  body.length = request.body_length;
  int framing_status = 0;
  const char* framing_body = nullptr;
  if (!request.has_content_type) {
    if (request.body_length) {
      framing_status = 415;
      framing_body = "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}";
    }
  } else if (request.json_content_type) {
    const unsigned char* json = reinterpret_cast<const unsigned char*>(request.body);
    size_t json_length = request.body_length;
    // One leading byte-order mark is dropped.
    if (json_length >= 3 && json[0] == 0xef && json[1] == 0xbb && json[2] == 0xbf) {
      json += 3; json_length -= 3;
    }
    codec::BoundedJsonError error = codec::BoundedJsonError::kNone;
    if (!request.body_length) {
      framing_status = 400;
      framing_body = "{\"error\":\"BAD_REQUEST\"}";
    } else if (!body.json.Parse(json, json_length, json_tokens, codec::kBoundedJsonMaxTokens, &error)) {
      // Invalid JSON is 400; the native bound (256 KiB, 8,192 tokens, depth
      // 16) is 413.
      framing_status = error == codec::BoundedJsonError::kInvalidInput ? 400 : 413;
      framing_body = framing_status == 400 ? "{\"error\":\"BAD_REQUEST\"}" : "{\"error\":\"BODY_TOO_LARGE\"}";
    } else {
      body.kind = BodyKind::kJson;
    }
  } else if (request.protobuf_content_type) {
    body.kind = BodyKind::kProtobuf;
    // Decoded once, here, for the quota cost; the handler reuses the fields
    // and answers a malformed body with its 400 after authentication. The
    // decode is allocation-free and linear in the framed (at most 512 KiB) body.
    body.proto_valid = fortis::codec::history_batch::DecodeBatchProtoRequest(
        body.bytes, body.length, &body.proto);
  } else {
    framing_status = 415;
    framing_body = "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}";
  }

  // 2. preHandler: Origin, device auth, quota charged at the request cost.
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, framing_status ? 1 : request_cost(body),
                                                           route->attestation_required);
  if (policy.status) {
    work_lock.Unlock();
    http::HttpResponse response{policy.status, policy.reason, "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t*>(policy.error_body ? policy.error_body
                                                           : "{\"error\":\"INTERNAL\"}"),
        strlen(policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}")};
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    WriteHttpResponse(client, response);
    return;
  }
  if (framing_status) {
    work_lock.Unlock();
    send_body(client, framing_status,
              framing_status == 415   ? "Unsupported Media Type"
              : framing_status == 413 ? "Payload Too Large"
                                      : "Bad Request",
              framing_body, policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  // `text` may live in response_body, so write before releasing work_mutex.
  auto reply = [&](int status, const char* reason, const char* text) {
    send_body(client, status, reason, text, cost, remaining);
    work_lock.Unlock();
  };

  // 3. Handler: protobuf decode result, Zod, range.
  if (body.kind == BodyKind::kProtobuf && !body.proto_valid) {
    reply(400, "Bad Request", "{\"error\":\"BAD_REQUEST\",\"detail\":\"invalid protobuf body\"}");
    return;
  }
  ValidRequest valid{};
  if (!validate(body, &valid)) {
    reply(400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}");
    return;
  }
  const int64_t max_range = valid.hourly ? 90 * 86400LL : 10 * 366 * 86400LL;
  if (!(valid.to > valid.from) || valid.to - valid.from > max_range) {
    reply(400, "Bad Request", "{\"error\":\"BAD_REQUEST\",\"detail\":\"invalid history range\"}");
    return;
  }

  // 4. Native window and response-size policy, before provider work.
  BatchSlot* slot = acquire_slot();
  if (!slot) { reply(503, "Service Unavailable", kUpstreamUnavailable); return; }
  slot->protobuf = AcceptsProtobuf(request);
  slot->rate_cost = cost; slot->rate_remaining = remaining;
  slot->from = valid.from; slot->to = valid.to; slot->hourly = valid.hourly;
  slot->width = valid.hourly ? 3600 : 86400;
  slot->first_bucket = (valid.from + slot->width - 1) / slot->width * slot->width;
  const int64_t last = (valid.to + slot->width - 1) / slot->width * slot->width - slot->width;
  slot->expected = last < slot->first_bucket ? 0
      : static_cast<size_t>((last - slot->first_bucket) / slot->width + 1);
  // Ordered Set of trimmed IDs.
  size_t cursor = 0;
  uint32_t json_child = codec::kBoundedJsonNoToken;
  if (body.kind == BodyKind::kJson) {
    const codec::BoundedJsonToken* ids = token(body, root_field(body, "ids"));
    json_child = ids ? ids->first_child : codec::kBoundedJsonNoToken;
  }
  const char* text = nullptr; size_t length = 0;
  while (next_trimmed_id(body, &cursor, &json_child, &text, &length)) {
    bool duplicate = false;
    for (size_t i = 0; i < slot->count && !duplicate; ++i)
      duplicate = slot->items[i].key_length == length &&
          memcmp(slot->items[i].key, text, length) == 0;
    if (duplicate || slot->count == kMaxIds || length > kMaxKeyBytes) continue;
    BatchItem& item = slot->items[slot->count++];
    memcpy(item.key, text, length); item.key[length] = '\0';
    item.key_length = static_cast<uint16_t>(length);
  }
  // The Zod range check above already bounds the window (90 days hourly,
  // 10 * 366 days daily), so the owner and read bounds below hold for every
  // valid request; they stay as defensive checks. Only the response-size
  // policy can refuse a valid request.
  const size_t read_cap = fortis::prices::PriceSeriesMaxRangeCandles(series_interval(*slot));
  const size_t worst = fortis::codec::history_batch::BatchWorstResponseBytes(
      slot->count, slot->expected + 1, slot->protobuf);
  if (valid.to - valid.from > PriceHistoryOwnerMaxWindowSeconds(series_interval(*slot)) ||
      slot->expected > read_cap || worst > kResponseCapacity) {
    release_unused(slot);
    reply(503, "Service Unavailable", kUpstreamUnavailable);
    return;
  }
  slot->range_capacity = slot->expected < read_cap ? slot->expected + 1 : read_cap;

  // 5. One store read per ID; cached-only batches answer synchronously.
  if (!initial_pass(*slot)) {
    const http::HttpResponse response = batch_response(*slot, true);
    WriteHttpResponse(client, response);
    // Released before work_mutex, so the spare slot is free for the next holder.
    release_unused(slot);
    work_lock.Unlock();
    return;
  }

  // 6. Provider gaps: the socket moves to the handoff, the driver submits.
  // At most four batches wait on the provider (or on late owner completions
  // of a finished one); a fifth is refused here, before any provider work.
  fortis::base::UniqueLock batch_lock(batch_mutex);
  size_t waiting = 0;
  for (const BatchSlot& other : slots)
    if (other.used && (other.deferred || other.terminal_pending)) ++waiting;
  const bool admitted = waiting < kDeferredSlots;
  if (admitted) slot->terminal_pending = true;
  batch_lock.Unlock();
  if (!admitted) {
    release_unused(slot);
    reply(503, "Service Unavailable", kUpstreamUnavailable);
    return;
  }
  work_lock.Unlock();
  uint64_t token_value = 0;
  if (!http::ReserveHttpResponse(client, false, kHandoffTimeoutMs, &token_value,
                           handoff_terminal, slot)) {
    release_unused(slot);
    send_body(client, 503, "Service Unavailable", kUpstreamUnavailable, cost, remaining);
    return;
  }
  batch_lock.Lock();
  slot->handoff_token = token_value;
  slot->deadline_ms = monotonic_ms() + kOwnerDeadlineMs;
  slot->deferred = true;
  batch_changed.Broadcast();
}

}  // namespace

bool StartPriceHistoryBatchDriver(PriceHistoryRouteContext* context) {
  if (!context || !context->series || !context->owner) return false;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  // Like the owner threads: every large workspace is static, never stack.
  if (pthread_attr_setstacksize(&attributes, 256 * 1024) != 0) {
    pthread_attr_destroy(&attributes);
    return false;
  }
  fortis::base::UniqueLock batch_lock(batch_mutex);
  if (driver_started) {
    batch_lock.Unlock();
    pthread_attr_destroy(&attributes);
    return false;
  }
  route = context;
  stopping = false;
  driver_started = (driver_thread.StartEntry<driver_main>(nullptr, &attributes) ? 0 : -1) == 0;
  const bool started = driver_started;
  if (!started) route = nullptr;
  batch_lock.Unlock();
  pthread_attr_destroy(&attributes);
  return started;
}

// The handler reads the driver's route context, so it registers only for the
// context the running driver serves.
bool RegisterPriceHistoryBatchRoute(http::HttpRouter& router, PriceHistoryRouteContext* context) {
  fortis::base::UniqueLock batch_lock(batch_mutex);
  const bool serving = driver_started && route == context;
  batch_lock.Unlock();
  return serving && context->handoff &&
      router.Add("POST", "/v1/prices/history", handle_batch, context);
}

void StopPriceHistoryBatchDriver() {
  fortis::base::UniqueLock batch_lock(batch_mutex);
  if (!driver_started) return;
  stopping = true;
  batch_changed.Broadcast();
  batch_lock.Unlock();
  driver_thread.Join();
  batch_lock.Lock();
  driver_started = false;
}

}  // namespace fortis::prices
