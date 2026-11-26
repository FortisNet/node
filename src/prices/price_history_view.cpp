#include "price_history_view.h"

#include <math.h>
#include <string.h>
#include <time.h>

namespace fortis::prices::history_batch {

bool HistoryTimestampIso(int64_t seconds, char output[32]) {
  const time_t value = static_cast<time_t>(seconds);
  struct tm utc{};
  if (!gmtime_r(&value, &utc)) return false;
  return strftime(output, 32, "%Y-%m-%dT%H:%M:%S.000Z", &utc) != 0;
}

double HistoryAvailableDays(int64_t from, int64_t to) {
  return fmax(1.0, ceil(static_cast<double>(to - from) / 86400.0) + 1.0);
}

int64_t HistoryAvailability::from() const {
  int64_t value = INT64_MAX;
  if (hourly && hour.oldest / 1000 < value) value = hour.oldest / 1000;
  if (daily && day.oldest / 1000 < value) value = day.oldest / 1000;
  return value;
}

int64_t HistoryAvailability::to() const {
  int64_t value = INT64_MIN;
  if (hourly && hour.newest / 1000 > value) value = hour.newest / 1000;
  if (daily && day.newest / 1000 > value) value = day.newest / 1000;
  return value;
}

// Mirrors IdOk in price_series_store.cpp. The batch replay of
// price_history_batch_other_ids.json pins the space, tab and slash forms.
bool HistorySourceIdStorable(const char* source_id) {
  if (!source_id) return false;
  const size_t length = strnlen(source_id, kPriceSpotMaxSourceIdBytes + 1);
  if (!length || length > kPriceSpotMaxSourceIdBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(source_id[i]);
    if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') return false;
  }
  return true;
}

bool ReadHistoryAvailability(const PriceSeriesStore& series, const char* source_id,
                             int64_t now_ms, HistoryAvailability* availability) {
  const auto hour = series.Availability(PriceSeriesInterval::k1h, "usd", source_id,
                                        now_ms, &availability->hour);
  const auto day = series.Availability(PriceSeriesInterval::k1d, "usd", source_id,
                                       now_ms, &availability->day);
  availability->hourly = hour == PriceSeriesReadStatus::kFound;
  availability->daily = day == PriceSeriesReadStatus::kFound;
  const bool unstorable = !HistorySourceIdStorable(source_id);
  auto read_ok = [unstorable](PriceSeriesReadStatus status) {
    return status == PriceSeriesReadStatus::kFound ||
        status == PriceSeriesReadStatus::kMissing ||
        (status == PriceSeriesReadStatus::kInvalidKey && unstorable);
  };
  return read_ok(hour) && read_ok(day);
}

namespace {
void write_iso(codec::history_batch::OutBuffer& out, int64_t seconds) {
  char text[32]{};
  if (!HistoryTimestampIso(seconds, text)) { out.ok = false; return; }
  out.Byte('"'); out.Text(text); out.Byte('"');
}

void put_point(codec::history_batch::OutBuffer& out, uint32_t field, int64_t seconds, double value) {
  PutMessage(out, field, [&](codec::history_batch::OutBuffer& point) {
    PutInt64(point, 1, seconds); codec::proto_wire::PutImplicitDouble(point, 2, value);
  });
}
}  // namespace

void AppendHistoryJson(codec::history_batch::OutBuffer& out, const HistoryView& view) {
  const PriceSeriesCandle* candles = view.candles;
  out.Text("{\"id\":"); AppendJsonString(out, view.source_id, strlen(view.source_id));
  out.Text(",\"interval\":\""); out.Text(view.interval); out.Text("\",\"points\":[");
  for (size_t i = 0; i < view.count; ++i) {
    if (i) out.Byte(',');
    out.Byte('['); AppendInteger(out, candles[i].ts / 1000); out.Byte(',');
    AppendJsNumber(out, candles[i].close); out.Byte(']');
  }
  out.Text("],\"volumes\":[");
  bool first = true;
  for (size_t i = 0; i < view.count; ++i) {
    if (!candles[i].has_volume) continue;
    if (!first) out.Byte(',');
    first = false;
    out.Byte('['); AppendInteger(out, candles[i].ts / 1000); out.Byte(',');
    AppendJsNumber(out, candles[i].volume); out.Byte(']');
  }
  out.Text("],\"cache\":{\"hit\":"); out.Text(view.hit ? "true" : "false");
  out.Text(",\"timestamp\":");
  if (view.has_timestamp) write_iso(out, view.timestamp); else out.Text("null");
  out.Text("},\"coverage\":{\"cachedPoints\":");
  AppendInteger(out, static_cast<int64_t>(view.cached));
  out.Text(",\"fetchedPoints\":");
  AppendInteger(out, static_cast<int64_t>(view.count - view.cached));
  out.Text("},\"window\":{\"from\":");
  if (view.count) {
    const int64_t from = candles[0].ts / 1000, to = candles[view.count - 1].ts / 1000;
    AppendInteger(out, from); out.Text(",\"to\":"); AppendInteger(out, to);
    out.Text(",\"days\":"); AppendJsNumber(out, HistoryAvailableDays(from, to));
  } else {
    out.Text("null,\"to\":null,\"days\":0");
  }
  out.Text("},\"availability\":{\"from\":");
  const HistoryAvailability& a = view.availability;
  if (a.any()) {
    AppendInteger(out, a.from()); out.Text(",\"to\":"); AppendInteger(out, a.to());
    out.Text(",\"days\":"); AppendJsNumber(out, HistoryAvailableDays(a.from(), a.to()));
  } else {
    out.Text("null,\"to\":null,\"days\":0");
  }
  out.Text(",\"resolutions\":[");
  // CandleHistory sorts resolutions by interval name: "1d" before "1h".
  bool written = false;
  const PriceSeriesAvailability* rows[] = {a.daily ? &a.day : nullptr,
                                           a.hourly ? &a.hour : nullptr};
  const char* names[] = {"1d", "1h"};
  for (size_t i = 0; i < 2; ++i) {
    if (!rows[i]) continue;
    if (written) out.Byte(',');
    written = true;
    const int64_t from = rows[i]->oldest / 1000, to = rows[i]->newest / 1000;
    out.Text("{\"interval\":\""); out.Text(names[i]); out.Text("\",\"from\":");
    AppendInteger(out, from); out.Text(",\"to\":"); AppendInteger(out, to);
    out.Text(",\"days\":"); AppendJsNumber(out, HistoryAvailableDays(from, to)); out.Byte('}');
  }
  out.Text("]}}");
}

void PutString(codec::history_batch::OutBuffer& out, uint32_t field, const char* text, size_t length) {
  codec::proto_wire::PutBytes(out, field, text, length);
}

void PutImplicitString(codec::history_batch::OutBuffer& out, uint32_t field, const char* text) {
  const size_t length = strlen(text);
  if (length) PutString(out, field, text, length);
}

void PutInt64(codec::history_batch::OutBuffer& out, uint32_t field, int64_t value, bool explicit_presence) {
  if (!value && !explicit_presence) return;
  codec::proto_wire::PutVarintField(out, field, static_cast<uint64_t>(value));
}

void PutBool(codec::history_batch::OutBuffer& out, uint32_t field, bool value) {
  if (!value) return;
  codec::proto_wire::PutVarintField(out, field, 1);
}

void AppendHistoryProto(codec::history_batch::OutBuffer& out, const HistoryView& view) {
  const PriceSeriesCandle* candles = view.candles;
  PutImplicitString(out, 1, view.source_id);
  PutImplicitString(out, 2, view.interval);
  for (size_t i = 0; i < view.count; ++i)
    put_point(out, 3, candles[i].ts / 1000, candles[i].close);
  for (size_t i = 0; i < view.count; ++i)
    if (candles[i].has_volume) put_point(out, 4, candles[i].ts / 1000, candles[i].volume);
  PutMessage(out, 5, [&](codec::history_batch::OutBuffer& cache) {
    PutBool(cache, 1, view.hit);
    if (view.has_timestamp) {
      char text[32]{};
      if (!HistoryTimestampIso(view.timestamp, text)) { cache.ok = false; return; }
      PutString(cache, 2, text, strlen(text));
    }
  });
  PutMessage(out, 6, [&](codec::history_batch::OutBuffer& coverage) {
    PutInt64(coverage, 1, static_cast<int64_t>(view.cached));
    PutInt64(coverage, 2, static_cast<int64_t>(view.count - view.cached));
  });
  PutMessage(out, 7, [&](codec::history_batch::OutBuffer& window) {
    if (!view.count) return;
    const int64_t from = candles[0].ts / 1000, to = candles[view.count - 1].ts / 1000;
    PutInt64(window, 1, from, true); PutInt64(window, 2, to, true);
    codec::proto_wire::PutImplicitDouble(window, 3, HistoryAvailableDays(from, to));
  });
  PutMessage(out, 8, [&](codec::history_batch::OutBuffer& availability) {
    const HistoryAvailability& a = view.availability;
    if (a.any()) {
      PutInt64(availability, 1, a.from(), true); PutInt64(availability, 2, a.to(), true);
      codec::proto_wire::PutImplicitDouble(availability, 3, HistoryAvailableDays(a.from(), a.to()));
    }
    const PriceSeriesAvailability* rows[] = {a.daily ? &a.day : nullptr,
                                             a.hourly ? &a.hour : nullptr};
    const char* names[] = {"1d", "1h"};
    for (size_t i = 0; i < 2; ++i) {
      if (!rows[i]) continue;
      const int64_t from = rows[i]->oldest / 1000, to = rows[i]->newest / 1000;
      PutMessage(availability, 4, [&](codec::history_batch::OutBuffer& row) {
        PutImplicitString(row, 1, names[i]);
        PutInt64(row, 2, from); PutInt64(row, 3, to);
        codec::proto_wire::PutImplicitDouble(row, 4, HistoryAvailableDays(from, to));
      });
    }
  });
}

HistoryWindow MakeHistoryWindow(int64_t from_seconds, int64_t to_seconds, int64_t width) {
  HistoryWindow window{};
  window.width = width;
  window.first_bucket = (from_seconds + width - 1) / width * width;
  // `to` is a half-open boundary: a bucket starting exactly at `to` belongs
  // to the next window.
  const int64_t last = (to_seconds + width - 1) / width * width - width;
  window.expected = last < window.first_bucket ? 0
      : static_cast<size_t>((last - window.first_bucket) / width + 1);
  return window;
}

void MarkHistoryBuckets(const HistoryWindow& window, const PriceSeriesCandle* candles,
                        size_t count, uint8_t* bitmap) {
  memset(bitmap, 0, (window.expected + 8) / 8);
  for (size_t i = 0; i < count; ++i) {
    const int64_t seconds = candles[i].ts / 1000;
    const int64_t bucket = seconds / window.width * window.width;
    if (bucket < window.first_bucket) continue;
    const size_t bit = static_cast<size_t>((bucket - window.first_bucket) / window.width);
    if (bit <= window.expected) bitmap[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  }
}

bool HistoryBucketPresent(const uint8_t* bitmap, size_t bit) {
  return bitmap[bit / 8] & (1u << (bit % 8));
}

bool HistoryAnyBucketMissing(const HistoryWindow& window, const uint8_t* bitmap) {
  for (size_t bit = 0; bit < window.expected; ++bit)
    if (!HistoryBucketPresent(bitmap, bit)) return true;
  return false;
}

size_t PlanHistoryGaps(const HistoryWindow& window, const uint8_t* bitmap,
                       const HistoryCoverage* covered, int64_t gaps[][2]) {
  return HistoryBackfillPlanner::Plan(window, bitmap, covered, gaps);
}


size_t CountInitiallyCached(const HistoryWindow& window, const uint8_t* bitmap,
                            const PriceSeriesCandle* candles, size_t count) {
  size_t cached = 0;
  for (size_t i = 0; i < count; ++i) {
    const int64_t bucket = candles[i].ts / 1000 / window.width * window.width;
    if (bucket < window.first_bucket) continue;
    const size_t bit = static_cast<size_t>((bucket - window.first_bucket) / window.width);
    if (bit <= window.expected && HistoryBucketPresent(bitmap, bit)) ++cached;
  }
  return cached;
}

}  // namespace fortis::prices::history_batch

namespace fortis::prices {
using namespace history_batch;
size_t HistoryBackfillPlanner::Plan(const HistoryWindow& window, const uint8_t* bitmap,
                       const HistoryCoverage* covered, int64_t gaps[][2]) {
  HistoryBackfillPlanner planner(covered);
  const int64_t first = window.first_bucket, width = window.width;
  // missingCandleRanges: bucket runs absent from [first, last], ascending.
  long gap_start = -1;
  for (size_t bit = 0; bit < window.expected && !planner.collapsed(); ++bit) {
    if (!HistoryBucketPresent(bitmap, bit)) {
      if (gap_start < 0) gap_start = static_cast<long>(bit);
    } else if (gap_start >= 0) {
      planner.AddRun(first + gap_start * width, first + static_cast<int64_t>(bit) * width);
      gap_start = -1;
    }
  }
  if (!planner.collapsed() && gap_start >= 0)
    planner.AddRun(first + gap_start * width,
                   first + static_cast<int64_t>(window.expected) * width);
  if (planner.collapsed()) {
    // More than eight uncovered gaps: one range over the aligned window,
    // covered parts included, as TypeScript requests it.
    gaps[0][0] = first;
    gaps[0][1] = first + static_cast<int64_t>(window.expected) * width;
    return 1;
  }
  for (size_t i = 0; i < planner.count(); ++i) {
    gaps[i][0] = planner.gap_from(i); gaps[i][1] = planner.gap_to(i);
  }
  return planner.count();
}
}
