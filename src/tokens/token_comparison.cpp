#include "token_comparison.h"

#include "proto_wire.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace fortis::tokens::comparison {
namespace {

using fortis::codec::history_batch::AppendJsNumber;
using fortis::codec::history_batch::AppendJsonString;
using fortis::codec::history_batch::OutBuffer;

bool fresh(const OptDate& as_of, int64_t now, int64_t max_age) {
  if (!as_of.present) return false;
  const int64_t age = now - as_of.ms;
  return age >= 0 && age <= max_age;
}

Metric metric(const OptNumber& value, const OptDate& as_of, const OptString& source,
              int64_t now, int64_t fresh_ms) {
  Metric out;
  out.as_of = as_of;
  out.source = &source;
  if (!value.present || !isfinite(value.value)) {
    out.status = MetricStatus::kUnavailable;
    return out;
  }
  out.value = value;
  out.status = fresh(as_of, now, fresh_ms) ? MetricStatus::kAvailable : MetricStatus::kStale;
  return out;
}

// Market data is authoritative while equally fresh; a fresh DEX observation
// beats an expired market floor.
Metric prefer_market(const Metric& market, const Metric& dex) {
  if (!market.value.present) return dex;
  if (!dex.value.present) return market;
  return market.status == MetricStatus::kStale && dex.status == MetricStatus::kAvailable
      ? dex : market;
}

Metric liquidity_metric(const Record& r, int64_t now) {
  Metric base = metric(r.liquidity_usd, r.liquidity_as_of, r.liquidity_source, now,
                       kLiquidityFreshMs);
  if (base.value.present) {
    if (r.liquidity_status != LiquidityStatus::kOk) base.status = MetricStatus::kStale;
    return base;
  }
  base.as_of = r.liquidity_checked_at;
  base.status = r.liquidity_status == LiquidityStatus::kUnsupported ? MetricStatus::kUnsupported
      : r.liquidity_status == LiquidityStatus::kError ? MetricStatus::kError
      : MetricStatus::kUnavailable;
  return base;
}

// Math.round: nearest integer, ties toward +Infinity, signed zero kept.
double js_round(double x) {
  if (!isfinite(x) || x == 0) return x;
  const double f = floor(x);
  const double r = x - f >= 0.5 ? f + 1 : f;
  return r == 0 && x < 0 ? -0.0 : r;
}

OptNumber ratio(const OptNumber& numerator, const OptNumber& denominator, double scale) {
  OptNumber out;
  if (!numerator.present || !denominator.present || denominator.value <= 0) return out;
  out.present = true;
  out.value = js_round((numerator.value / denominator.value) * scale * 1000000) / 1000000;
  return out;
}

void assign_ranks(Item* items, size_t count, const Metric Item::*metric_field,
                  uint32_t Item::*rank_field) {
  size_t order[kMaxTokens];
  size_t ranked = 0;
  for (size_t i = 0; i < count && ranked < kMaxTokens; ++i)
    if ((items[i].*metric_field).value.present) order[ranked++] = i;
  // Stable insertion sort: amount descending, then input index.
  for (size_t i = 1; i < ranked; ++i) {
    const size_t current = order[i];
    const double amount = (items[current].*metric_field).value.value;
    size_t j = i;
    while (j > 0 && (items[order[j - 1]].*metric_field).value.value < amount) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = current;
  }
  bool have_previous = false;
  double previous = 0;
  uint32_t rank = 0;
  for (size_t i = 0; i < ranked; ++i) {
    const double amount = (items[order[i]].*metric_field).value.value;
    if (!have_previous || amount != previous) rank = static_cast<uint32_t>(i + 1);
    items[order[i]].*rank_field = rank;
    previous = amount;
    have_previous = true;
  }
}

const char* status_name(MetricStatus status) {
  switch (status) {
    case MetricStatus::kAvailable: return "available";
    case MetricStatus::kStale: return "stale";
    case MetricStatus::kUnavailable: return "unavailable";
    case MetricStatus::kUnsupported: return "unsupported";
    case MetricStatus::kError: return "error";
  }
  return "";
}

// ── JSON ──

void json_key(OutBuffer& out, const char* key, bool first = false) {
  if (!first) out.Byte(',');
  out.Byte('"'); out.Text(key); out.Text("\":");
}

void json_string(OutBuffer& out, const OptString* value) {
  if (!value || !value->present) { out.Text("null"); return; }
  AppendJsonString(out, value->text, value->length);
}

void json_number(OutBuffer& out, const OptNumber& value) {
  if (!value.present) { out.Text("null"); return; }
  AppendJsNumber(out, value.value);  // non-finite: null, like JSON.stringify
}

void json_date(OutBuffer& out, const OptDate& value) {
  char iso[32];
  if (!value.present || !IsoTimestamp(value.ms, iso)) { out.Text("null"); return; }
  out.Byte('"'); out.Text(iso); out.Byte('"');
}

void json_rank(OutBuffer& out, uint32_t rank) {
  if (!rank) { out.Text("null"); return; }
  char text[16];
  snprintf(text, sizeof(text), "%u", rank);
  out.Text(text);
}

void json_metric(OutBuffer& out, const char* key, const Metric& m, bool first = false) {
  json_key(out, key, first);
  out.Text("{\"value\":"); json_number(out, m.value);
  out.Text(",\"asOf\":"); json_date(out, m.as_of);
  out.Text(",\"source\":"); json_string(out, m.source);
  out.Text(",\"status\":\""); out.Text(status_name(m.status)); out.Text("\"}");
}

void json_item(OutBuffer& out, const Item& item) {
  const Record& r = *item.record;
  out.Text("{\"network\":"); AppendJsonString(out, r.network, strlen(r.network));
  out.Text(",\"tokenId\":"); AppendJsonString(out, r.token_id, strlen(r.token_id));
  out.Text(",\"identity\":{\"symbol\":"); json_string(out, &r.symbol);
  out.Text(",\"name\":"); json_string(out, &r.name);
  out.Text(",\"logoMediaKey\":"); json_string(out, &r.logo_media_key);
  out.Text(",\"marketId\":"); json_string(out, &r.market_source_id);
  out.Text("},\"metrics\":{");
  json_metric(out, "priceUsd", item.price, true);
  json_metric(out, "marketCapUsd", item.market_cap);
  json_metric(out, "volume24hUsd", item.volume);
  json_metric(out, "liquidityUsd", item.liquidity);
  out.Text("},\"changes\":{\"price1hPct\":"); json_number(out, r.price_change_1h_pct);
  out.Text(",\"price24hPct\":"); json_number(out, r.price_change_24h_pct);
  out.Text(",\"price7dPct\":"); json_number(out, r.price_change_7d_pct);
  out.Text(",\"price30dPct\":"); json_number(out, r.price_change_30d_pct);
  out.Text("},\"liquidityQuality\":{\"spreadPct\":"); json_number(out, r.spread_pct);
  out.Text(",\"topPoolShare\":"); json_number(out, r.top_pool_share);
  out.Text(",\"poolCount\":"); json_number(out, r.pool_count);
  out.Text("},\"derived\":{\"turnoverPct\":"); json_number(out, item.turnover_pct);
  out.Text(",\"liquidityToMarketCapPct\":"); json_number(out, item.liquidity_to_market_cap_pct);
  out.Text(",\"volumeToLiquidityRatio\":"); json_number(out, item.volume_to_liquidity_ratio);
  out.Text("},\"ranks\":{\"marketCap\":"); json_rank(out, item.rank_market_cap);
  out.Text(",\"volume24h\":"); json_rank(out, item.rank_volume);
  out.Text(",\"liquidity\":"); json_rank(out, item.rank_liquidity);
  out.Text("},\"warnings\":[");
  const struct { const char* name; const Metric* metric; } warnings[] = {
    {"priceUsd:stale", &item.price}, {"marketCapUsd:stale", &item.market_cap},
    {"volume24hUsd:stale", &item.volume}, {"liquidityUsd:stale", &item.liquidity},
  };
  bool first = true;
  for (const auto& warning : warnings) {
    if (warning.metric->status != MetricStatus::kStale) continue;
    if (!first) out.Byte(',');
    first = false;
    out.Byte('"'); out.Text(warning.name); out.Byte('"');
  }
  out.Text("]}");
}

// ── protobuf (protobuf-es toBinary order: fields ascending) ──

void put_double(OutBuffer& out, uint32_t field, const OptNumber& value) {
  if (value.present) codec::proto_wire::PutDouble(out, field, value.value);
}

void put_string(OutBuffer& out, uint32_t field, const char* text, size_t length) {
  codec::proto_wire::PutBytes(out, field, text, length);
}

void put_optional_string(OutBuffer& out, uint32_t field, const OptString* value) {
  if (value && value->present) put_string(out, field, value->text, value->length);
}

void put_implicit(OutBuffer& out, uint32_t field, const char* text, size_t length) {
  if (length) put_string(out, field, text, length);
}

void put_date(OutBuffer& out, uint32_t field, const OptDate& value) {
  char iso[32];
  if (value.present && IsoTimestamp(value.ms, iso)) put_string(out, field, iso, strlen(iso));
}

void put_uint32(OutBuffer& out, uint32_t field, uint32_t value) {
  codec::proto_wire::PutVarintField(out, field, value);
}

// assertUInt32: an integral number in [0, 4294967295].
bool uint32_value(const OptNumber& value, uint32_t* out) {
  const double v = value.value;
  if (!isfinite(v) || floor(v) != v || v < 0 || v > 4294967295.0) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

template <typename Body>
void put_message(OutBuffer& out, uint32_t field, Body&& body) {
  OutBuffer measure{};
  body(measure);
  codec::proto_wire::PutLengthPrefix(out, field, measure.used);
  body(out);
}

void proto_metric(OutBuffer& out, const Metric& m) {
  put_double(out, 1, m.value);
  put_date(out, 2, m.as_of);
  put_optional_string(out, 3, m.source);
  const char* status = status_name(m.status);
  put_implicit(out, 4, status, strlen(status));
}

bool proto_item(OutBuffer& out, const Item& item) {
  const Record& r = *item.record;
  put_implicit(out, 1, r.network, strlen(r.network));
  put_implicit(out, 2, r.token_id, strlen(r.token_id));
  put_message(out, 3, [&](OutBuffer& o) {
    put_optional_string(o, 1, &r.symbol); put_optional_string(o, 2, &r.name);
    put_optional_string(o, 3, &r.logo_media_key); put_optional_string(o, 4, &r.market_source_id);
  });
  put_message(out, 4, [&](OutBuffer& o) {
    put_message(o, 1, [&](OutBuffer& m) { proto_metric(m, item.price); });
    put_message(o, 2, [&](OutBuffer& m) { proto_metric(m, item.market_cap); });
    put_message(o, 3, [&](OutBuffer& m) { proto_metric(m, item.volume); });
    put_message(o, 4, [&](OutBuffer& m) { proto_metric(m, item.liquidity); });
  });
  put_message(out, 5, [&](OutBuffer& o) {
    put_double(o, 1, r.price_change_1h_pct); put_double(o, 2, r.price_change_24h_pct);
    put_double(o, 3, r.price_change_7d_pct); put_double(o, 4, r.price_change_30d_pct);
  });
  uint32_t pool_count = 0;
  if (r.pool_count.present && !uint32_value(r.pool_count, &pool_count)) return false;
  put_message(out, 6, [&](OutBuffer& o) {
    put_double(o, 1, r.spread_pct); put_double(o, 2, r.top_pool_share);
    if (r.pool_count.present) put_uint32(o, 3, pool_count);
  });
  put_message(out, 7, [&](OutBuffer& o) {
    put_double(o, 1, item.turnover_pct); put_double(o, 2, item.liquidity_to_market_cap_pct);
    put_double(o, 3, item.volume_to_liquidity_ratio);
  });
  put_message(out, 8, [&](OutBuffer& o) {
    if (item.rank_market_cap) put_uint32(o, 1, item.rank_market_cap);
    if (item.rank_volume) put_uint32(o, 2, item.rank_volume);
    if (item.rank_liquidity) put_uint32(o, 3, item.rank_liquidity);
  });
  const struct { const char* name; const Metric* metric; } warnings[] = {
    {"priceUsd:stale", &item.price}, {"marketCapUsd:stale", &item.market_cap},
    {"volume24hUsd:stale", &item.volume}, {"liquidityUsd:stale", &item.liquidity},
  };
  for (const auto& warning : warnings)
    if (warning.metric->status == MetricStatus::kStale)
      put_string(out, 9, warning.name, strlen(warning.name));
  return true;
}

}  // namespace

bool IsoTimestamp(int64_t ms, char output[32]) {
  if (ms < 0 || ms > kMaxDateMs) return false;
  const time_t seconds = static_cast<time_t>(ms / 1000);
  struct tm utc{};
  if (!gmtime_r(&seconds, &utc)) return false;
  return snprintf(output, 32, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(ms % 1000)) == 24;
}

void CompareTokenRecords(const Record* records, size_t count, int64_t now, Item* items) {
  if (count > kMaxTokens) count = kMaxTokens;
  for (size_t i = 0; i < count; ++i) {
    const Record& r = records[i];
    Item& item = items[i];
    item = Item{};
    item.record = &r;
    const Metric market_price = metric(r.market_price_usd, r.market_as_of, r.market_source,
                                       now, kMarketFreshMs);
    const Metric dex_price = metric(r.dex_price_usd, r.liquidity_as_of, r.liquidity_source,
                                    now, kLiquidityFreshMs);
    item.price = prefer_market(market_price, dex_price);
    item.market_cap = prefer_market(
        metric(r.market_cap_usd, r.market_as_of, r.market_source, now, kMarketFreshMs),
        metric(r.dex_market_cap_usd, r.liquidity_as_of, r.liquidity_source, now,
               kLiquidityFreshMs));
    item.volume = prefer_market(
        metric(r.volume_24h_usd, r.market_as_of, r.market_source, now, kMarketFreshMs),
        metric(r.dex_volume_24h_usd, r.liquidity_as_of, r.liquidity_source, now,
               kLiquidityFreshMs));
    item.liquidity = liquidity_metric(r, now);
    item.turnover_pct = ratio(item.volume.value, item.market_cap.value, 100);
    item.liquidity_to_market_cap_pct = ratio(item.liquidity.value, item.market_cap.value, 100);
    item.volume_to_liquidity_ratio = ratio(item.volume.value, item.liquidity.value, 1);
  }
  assign_ranks(items, count, &Item::market_cap, &Item::rank_market_cap);
  assign_ranks(items, count, &Item::volume, &Item::rank_volume);
  assign_ranks(items, count, &Item::liquidity, &Item::rank_liquidity);
}

void AppendResponseJson(OutBuffer& out, const Response& response, bool include_computed_at) {
  out.Text("{\"fiat\":\"usd\"");
  if (include_computed_at) {
    char iso[32];
    out.Text(",\"computedAt\":");
    if (IsoTimestamp(response.computed_at_ms, iso)) { out.Byte('"'); out.Text(iso); out.Byte('"'); }
    else out.ok = false;
  }
  out.Text(",\"items\":[");
  for (size_t i = 0; i < response.item_count; ++i) {
    if (i) out.Byte(',');
    json_item(out, response.items[i]);
  }
  out.Text("],\"errors\":[");
  for (size_t i = 0; i < response.error_count; ++i) {
    const ErrorEntry& e = response.errors[i];
    if (i) out.Byte(',');
    char index[16];
    snprintf(index, sizeof(index), "%u", e.input_index);
    out.Text("{\"inputIndex\":"); out.Text(index);
    out.Text(",\"network\":"); AppendJsonString(out, e.network, e.network_length);
    out.Text(",\"tokenId\":"); AppendJsonString(out, e.token_id, e.token_id_length);
    out.Text(e.duplicate ? ",\"code\":\"DUPLICATE_TOKEN\"}" : ",\"code\":\"INVALID_TOKEN\"}");
  }
  out.Text("]}");
}

bool AppendResponseProto(OutBuffer& out, const Response& response, bool include_computed_at) {
  put_string(out, 1, "usd", 3);
  if (include_computed_at) {
    char iso[32];
    if (!IsoTimestamp(response.computed_at_ms, iso)) return false;
    put_string(out, 2, iso, strlen(iso));
  }
  bool ok = true;
  for (size_t i = 0; i < response.item_count && ok; ++i)
    put_message(out, 3, [&](OutBuffer& o) { ok = proto_item(o, response.items[i]) && ok; });
  if (!ok) return false;
  for (size_t i = 0; i < response.error_count; ++i) {
    const ErrorEntry& e = response.errors[i];
    put_message(out, 4, [&](OutBuffer& o) {
      if (e.input_index) put_uint32(o, 1, e.input_index);
      put_implicit(o, 2, e.network, e.network_length);
      put_implicit(o, 3, e.token_id, e.token_id_length);
      const char* code = e.duplicate ? "DUPLICATE_TOKEN" : "INVALID_TOKEN";
      put_implicit(o, 4, code, strlen(code));
    });
  }
  return out.ok;
}

}  // namespace fortis::tokens::comparison
