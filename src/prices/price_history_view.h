#pragma once

#include "price_history_batch_codec.h"
#include "price_history_coverage.h"
#include "price_series_store.h"
#include "proto_wire.h"

#include <stddef.h>
#include <stdint.h>

// Shared by the gated GET and POST /v1/prices/history routes:
// the encoder for one CandleHistory.history() result (JSON.stringify and
// protobuf-es toBinary bytes) and CandleHistory's gap plan. Linked only into
// the history route test binary. Everything is allocation-free and works on
// caller-owned candle and bitmap storage.
namespace fortis::prices::history_batch {

// The widest window either route reads: a ten-year daily window. A bitmap
// holds one bit per bucket of [first, last] plus the inclusive `to` bucket.
constexpr size_t kHistoryMaxBuckets = kPriceSeriesMaxDailyRangeCandles;
constexpr size_t kHistoryBitmapBytes = (kHistoryMaxBuckets + 8) / 8;
constexpr size_t kHistoryMaxGaps = kHistoryCoverageMaxGaps;  // MAX_GAP_REQUESTS

// new Date(seconds * 1000).toISOString() for whole seconds.
bool HistoryTimestampIso(int64_t seconds, char output[32]);
// availableDays(from, to): max(1, ceil((to - from) / 86400) + 1).
double HistoryAvailableDays(int64_t from, int64_t to);

struct HistoryAvailability {
  bool hourly = false, daily = false;
  PriceSeriesAvailability hour{}, day{};
  bool any() const { return hourly || daily; }
  int64_t from() const;
  int64_t to() const;
};
// Whether PriceSeriesStore can key `source_id`: its IdOk rule, 1–512 bytes of
// printable ASCII other than '/' and '\'. A parser-accepted ID that fails it,
// such as an EVM network name containing a space, can never have a stored
// row, so CandleHistory reads it as an empty series. Only for such an ID does
// a kInvalidKey read mean "empty"; for any other ID it is a failed read.
bool HistorySourceIdStorable(const char* source_id);

// Reads both resolutions. Returns false unless each read is Found or Missing
// (or kInvalidKey for an unstorable source ID); the flags still describe
// whatever was found.
bool ReadHistoryAvailability(const PriceSeriesStore& series, const char* source_id,
                             int64_t now_ms, HistoryAvailability* availability);

// One CandleHistory.history() result over candles[0..count).
struct HistoryView {
  const char* source_id = "";
  const char* interval = "1h";
  const PriceSeriesCandle* candles = nullptr;
  size_t count = 0;
  bool hit = true;
  bool has_timestamp = false;
  int64_t timestamp = 0;  // seconds of the newest initially cached point
  size_t cached = 0;      // coverage.cachedPoints
  HistoryAvailability availability{};
};
void AppendHistoryJson(codec::history_batch::OutBuffer& out, const HistoryView& view);
void AppendHistoryProto(codec::history_batch::OutBuffer& out, const HistoryView& view);

// protobuf-es toBinary presence rules for this module's messages, on the
// shared proto_wire writer: fields in number order, implicit-presence zero
// values omitted, explicit `optional` fields and set messages written.
// PutString always writes. Shared proto_wire::PutImplicitDouble omits +0
// and preserves -0 according to the protobuf standard.
void PutString(codec::history_batch::OutBuffer& out, uint32_t field, const char* text, size_t length);
void PutImplicitString(codec::history_batch::OutBuffer& out, uint32_t field, const char* text);
void PutInt64(codec::history_batch::OutBuffer& out, uint32_t field, int64_t value, bool explicit_presence = false);
void PutBool(codec::history_batch::OutBuffer& out, uint32_t field, bool value);
// A sub-message, always written (also when empty): measured with a counting
// buffer, then written (two-pass length prefix).
template <typename Body>
void PutMessage(codec::history_batch::OutBuffer& out, uint32_t field, Body&& body) {
  codec::history_batch::OutBuffer measure{};
  body(measure);
  codec::proto_wire::PutLengthPrefix(out, field, measure.used);
  body(out);
}

// The bucket grid of a request window: missingCandleRanges' first and last
// buckets, with `expected` = number of buckets in [first, last].
struct HistoryWindow {
  int64_t width = 0;
  int64_t first_bucket = 0;
  size_t expected = 0;
};
HistoryWindow MakeHistoryWindow(int64_t from_seconds, int64_t to_seconds, int64_t width);

// Marks the buckets present in candles[0..count) (bits 0..expected; the last
// bit is the inclusive `to` bucket). The bitmap must hold expected + 1 bits.
void MarkHistoryBuckets(const HistoryWindow& window, const PriceSeriesCandle* candles,
                        size_t count, uint8_t* bitmap);
bool HistoryBucketPresent(const uint8_t* bitmap, size_t bit);
bool HistoryAnyBucketMissing(const HistoryWindow& window, const uint8_t* bitmap);
// CandleHistory's plan: missing bucket runs, minus covered ranges, and more
// than eight gaps collapse into [ceil(from), ceil(to)] (ignoring coverage).
// Returns the gap count (0 when nothing is missing or all is covered).
size_t PlanHistoryGaps(const HistoryWindow& window, const uint8_t* bitmap,
                       const HistoryCoverage* covered, int64_t gaps[][2]);
// coverage.cachedPoints: final points whose bucket existed before the request.
size_t CountInitiallyCached(const HistoryWindow& window, const uint8_t* bitmap,
                            const PriceSeriesCandle* candles, size_t count);

}  // namespace fortis::prices::history_batch
