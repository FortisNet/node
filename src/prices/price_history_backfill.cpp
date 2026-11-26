#include "ownership.h"
#include "price_history_backfill.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::prices {

namespace {
bool number(const codec::BoundedJsonDocument& doc, uint32_t token, double* value) {
  return token != codec::kBoundedJsonNoToken && doc.Number(token, value) && isfinite(*value);
}

PriceHistoryParseStatus bucket_points(const codec::BoundedJsonDocument& doc,
    uint32_t array, int64_t width, PriceHistoryBucket* output,
    size_t capacity, size_t* count, bool volumes) {
  if (!array || array == codec::kBoundedJsonNoToken ||
      doc.token(array)->type != codec::BoundedJsonType::kArray)
    return PriceHistoryParseStatus::kInvalidShape;
  size_t points = 0;
  for (uint32_t row = doc.token(array)->first_child;
       row != codec::kBoundedJsonNoToken; row = doc.token(row)->next_sibling) {
    if (++points > kPriceHistoryMaxPointCount)
      return PriceHistoryParseStatus::kTooManyCandles;
    const auto* pair = doc.token(row);
    if (!pair || pair->type != codec::BoundedJsonType::kArray ||
        pair->first_child == codec::kBoundedJsonNoToken)
      return PriceHistoryParseStatus::kInvalidPoint;
    const uint32_t time_token = pair->first_child;
    const uint32_t value_token = doc.token(time_token)->next_sibling;
    double time = 0, value = 0;
    if (!number(doc, time_token, &time) || !number(doc, value_token, &value) ||
        time < 0 || time >= 9.2233720368547758e18 || floor(time) != time)
      return PriceHistoryParseStatus::kInvalidPoint;
    const int64_t ts = static_cast<int64_t>(time);
    const int64_t bucket_ts = (ts / width) * width;
    // Buckets are unique, so finding one is order-independent. Provider
    // samples are ascending, so the newest bucket is checked first; any other
    // order falls back to the full scan (O(n^2) for 3,700 points is ~27M
    // comparisons, which the fast path avoids on the owner thread).
    size_t i;
    if (*count && output[*count - 1].candle.ts == bucket_ts) i = *count - 1;
    else for (i = 0; i < *count && output[i].candle.ts != bucket_ts; ++i) {}
    if (volumes) {
      // Match TS: last volume sample in provider order wins, and volume only
      // attaches to a price-created bucket.
      if (i < *count) {
        output[i].candle.volume = value;
        output[i].candle.has_volume = true;
      }
      continue;
    }
    if (i == *count) {
      if (*count == capacity) return PriceHistoryParseStatus::kTooManyCandles;
      auto& candle = output[(*count)++].candle;
      candle = {bucket_ts, value, value, value, value, 0, false};
    } else {
      auto& candle = output[i].candle;
      if (value > candle.high) candle.high = value;
      if (value < candle.low) candle.low = value;
      candle.close = value;
    }
  }
  return PriceHistoryParseStatus::kOk;
}

void sort(PriceHistoryBucket* values, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const auto value = values[i]; size_t j = i;
    while (j && values[j - 1].candle.ts > value.candle.ts) {
      values[j] = values[j - 1]; --j;
    }
    values[j] = value;
  }
}
}  // namespace

PriceHistoryParseStatus CandleAggregator::Parse(
    const unsigned char* body, size_t body_length,
    PriceHistoryWorkspace* workspace) {
  if (!workspace) return PriceHistoryParseStatus::kInvalidShape;
  workspace->hourly_count = workspace->daily_count = 0;
  codec::BoundedJsonDocument doc; codec::BoundedJsonError error{};
  if (!doc.ParseWithLimits(body, body_length, workspace->tokens,
                 kPriceHistoryMaxJsonTokens, kPriceHistoryMaxJsonTokens,
                 kPriceHistoryMaxProviderBodyBytes, &error))
    return PriceHistoryParseStatus::kInvalidJson;
  const uint32_t root = doc.root();
  if (!doc.token(root) || doc.token(root)->type != codec::BoundedJsonType::kObject)
    return PriceHistoryParseStatus::kInvalidShape;
  const uint32_t prices = doc.FindObjectValue(root, "prices");
  const uint32_t volumes = doc.FindObjectValue(root, "total_volumes");
  PriceHistoryParseStatus status = PriceHistoryParseStatus::kOk;
  if (prices != codec::kBoundedJsonNoToken && doc.token(prices)->type != codec::BoundedJsonType::kNull) {
    status = bucket_points(doc, prices, 3600000, workspace->hourly,
        kPriceHistoryMaxHourlyCandles, &workspace->hourly_count, false);
    if (status != PriceHistoryParseStatus::kOk) return status;
    status = bucket_points(doc, prices, 86400000, workspace->daily,
        kPriceHistoryMaxDailyCandles, &workspace->daily_count, false);
    if (status != PriceHistoryParseStatus::kOk) return status;
  }
  if (volumes != codec::kBoundedJsonNoToken && doc.token(volumes)->type != codec::BoundedJsonType::kNull) {
    status = bucket_points(doc, volumes, 3600000, workspace->hourly,
        kPriceHistoryMaxHourlyCandles, &workspace->hourly_count, true);
    if (status != PriceHistoryParseStatus::kOk) return status;
    status = bucket_points(doc, volumes, 86400000, workspace->daily,
        kPriceHistoryMaxDailyCandles, &workspace->daily_count, true);
    if (status != PriceHistoryParseStatus::kOk) return status;
  }
  sort(workspace->hourly, workspace->hourly_count);
  sort(workspace->daily, workspace->daily_count);
  return PriceHistoryParseStatus::kOk;
}

size_t CandleAggregator::BuildChunk(const PriceHistoryWorkspace& workspace,
    size_t* hourly_cursor, size_t* daily_cursor,
    fortis::prices::PriceSeriesCandleBatchInput output[kPriceHistoryMaxCandles]) {
  if (!hourly_cursor || !daily_cursor || !output ||
      *hourly_cursor > workspace.hourly_count || *daily_cursor > workspace.daily_count)
    return 0;
  size_t count = 0;
  while (count < kPriceHistoryMaxCandles &&
         (*hourly_cursor < workspace.hourly_count ||
          *daily_cursor < workspace.daily_count)) {
    const bool take_hourly = *daily_cursor >= workspace.daily_count ||
      (*hourly_cursor < workspace.hourly_count &&
       workspace.hourly[*hourly_cursor].candle.ts <=
           workspace.daily[*daily_cursor].candle.ts);
    if (take_hourly) {
      output[count++] = {fortis::prices::PriceSeriesInterval::k1h,
                         workspace.hourly[(*hourly_cursor)++].candle};
    } else {
      output[count++] = {fortis::prices::PriceSeriesInterval::k1d,
                         workspace.daily[(*daily_cursor)++].candle};
    }
  }
  return count;
}

PriceHistoryParseStatus CandleAggregator::Parse(
    const unsigned char* body, size_t body_length, codec::BoundedJsonToken* tokens,
    size_t token_capacity, PriceHistoryCandles* output) {
  if (!output || !tokens) return PriceHistoryParseStatus::kInvalidShape;
  output->count = 0;
  auto workspace = base::AllocateZeroedStorage<PriceHistoryWorkspace>(1, sizeof(PriceHistoryWorkspace));
  if (!workspace) return PriceHistoryParseStatus::kInvalidJson;
  PriceHistoryParseStatus status = Parse(body, body_length, workspace.get());
  if (status == PriceHistoryParseStatus::kOk) {
    size_t h = 0, d = 0;
    output->count = BuildChunk(*workspace, &h, &d, output->items);
    if (h != workspace->hourly_count || d != workspace->daily_count)
      status = PriceHistoryParseStatus::kTooManyCandles;
  }
  // Preserve the legacy test API's token result for caller-owned scratch.
  codec::BoundedJsonDocument doc; codec::BoundedJsonError error{};
  if (status == PriceHistoryParseStatus::kOk && token_capacity >= codec::kBoundedJsonMaxTokens)
    (void)doc.Parse(body, body_length, tokens, token_capacity, &error);
  workspace.reset();
  return status;
}

fortis::prices::PriceSeriesStoreStatus CandleAggregator::Store(
    fortis::prices::PriceSeriesStore& store, const char* fiat, const char* source_id,
    const PriceHistoryCandles& candles, int64_t now_ms) {
  if (candles.count > kPriceHistoryMaxCandles)
    return fortis::prices::PriceSeriesStoreStatus::kInvalidArguments;
  return store.UpsertCandlesAtomic(fiat, source_id, candles.items,
                                   candles.count, now_ms);
}

// Compatibility entry points share the same bounded aggregation implementation.
PriceHistoryParseStatus ParseCoinGeckoMarketChartRange(const unsigned char* body, size_t length,
    PriceHistoryWorkspace* workspace) { return CandleAggregator::Parse(body, length, workspace); }
PriceHistoryParseStatus ParseCoinGeckoMarketChartRange(const unsigned char* body, size_t length,
    codec::BoundedJsonToken* tokens, size_t capacity, PriceHistoryCandles* output) {
  return CandleAggregator::Parse(body, length, tokens, capacity, output);
}
size_t BuildPriceHistoryChunk(const PriceHistoryWorkspace& workspace, size_t* hourly, size_t* daily,
    PriceSeriesCandleBatchInput output[kPriceHistoryMaxCandles]) {
  return CandleAggregator::BuildChunk(workspace, hourly, daily, output);
}
PriceSeriesStoreStatus StorePriceHistoryCandles(PriceSeriesStore& store, const char* fiat,
    const char* id, const PriceHistoryCandles& candles, int64_t now) {
  return CandleAggregator::Store(store, fiat, id, candles, now);
}

}  // namespace fortis::prices
