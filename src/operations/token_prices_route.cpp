#include "ownership.h"
#include "query_string.h"
#include "token_prices_route.h"

#include "kv_runtime.h"
#include "price_history_batch_codec.h"
#include "price_series_store.h"
#include "token_prices_page.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::operations {

namespace {
using fortis::codec::history_batch::AppendInteger;
using fortis::codec::history_batch::AppendJsonString;
using fortis::codec::history_batch::AppendJsNumber;
using fortis::codec::history_batch::OutBuffer;

constexpr size_t kListLimit = fortis::prices::kPriceSeriesMaxRecentSpots;
constexpr size_t kListBodyBytes = 768 * 1024;
constexpr size_t kChartBodyBytes = 32 * 1024;
constexpr size_t kChartPoints = 100;

int64_t now_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void reply(int client, bool head, int status, const char* reason, const char* body) {
  const http::HttpResponse response = http::JsonResponse(status, reason, body);
  WriteHttpResponse(client, response, head);
}

bool source_id_ok(const char* id) {
  if (!id) return false;
  const size_t length = strnlen(id, fortis::prices::kPriceSpotMaxSourceIdBytes + 1);
  if (!length || length > fortis::prices::kPriceSpotMaxSourceIdBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(id[i]);
    if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') return false;
  }
  return true;
}

// The split price store when configured, else the main runtime store.
fortis::storage::KvStore* price_store(void* context) {
  return context ? static_cast<fortis::storage::KvStore*>(context)
                 : fortis::storage::RuntimeKvStore();
}

void handle_page(int client, const http::HttpRequest&, bool head, void*) {
  const http::HttpResponse response{200, "OK", "text/html; charset=utf-8",
      reinterpret_cast<const uint8_t*>(kTokenPricesPage), sizeof(kTokenPricesPage) - 1};
  WriteHttpResponse(client, response, head);
}

void handle_list(int client, const http::HttpRequest&, bool head, void* context) {
  fortis::storage::KvStore* kv = price_store(context);
  if (!kv) { reply(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_UNAVAILABLE\"}"); return; }
  auto spots_storage = base::AllocateZeroedStorage<fortis::prices::PriceSeriesRecentSpot[]>(kListLimit, sizeof(fortis::prices::PriceSeriesRecentSpot));
  auto* spots = spots_storage.get();
  auto body_storage = base::AllocateStorage<uint8_t[]>(kListBodyBytes);
  auto* body = body_storage.get();
  if (!spots || !body) {
    spots_storage.reset(); body_storage.reset();
    reply(client, head, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}");
    return;
  }
  fortis::prices::PriceSeriesStore series(*kv);
  size_t count = 0;
  const int64_t now = now_ms();
  const auto status = series.RecentSpots(0, now, spots, kListLimit, &count);
  if (status != fortis::prices::PriceSeriesReadStatus::kFound &&
      status != fortis::prices::PriceSeriesReadStatus::kMissing) {
    spots_storage.reset(); body_storage.reset();
    reply(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_UNAVAILABLE\"}");
    return;
  }
  OutBuffer out{body, kListBodyBytes};
  out.Text("{\"tokens\":[");
  size_t included = 0;
  // RecentSpots orders oldest first. The panel should surface the newest.
  for (size_t i = count; i > 0; --i) {
    const auto& spot = spots[i - 1];
    if (strcmp(spot.fiat, "usd") != 0) continue;
    if (included++) out.Byte(',');
    out.Text("{\"id\":");
    AppendJsonString(out, spot.source_id, strlen(spot.source_id));
    out.Text(",\"asOf\":"); AppendInteger(out, spot.as_of_ms);
    out.Text(",\"price\":"); AppendJsNumber(out, spot.price);
    out.Byte('}');
  }
  out.Text("]}");
  if (out.ok) {
    const http::HttpResponse response{200, "OK", "application/json; charset=utf-8", body, out.used};
    WriteHttpResponse(client, response, head);
  } else {
    reply(client, head, 503, "Service Unavailable", "{\"error\":\"RESPONSE_TOO_LARGE\"}");
  }
  spots_storage.reset(); body_storage.reset();
}

void handle_chart(int client, const http::HttpRequest& request, bool head, void* context) {
  const char* id = HttpPathParamValue(request, "id");
  if (!source_id_ok(id)) { reply(client, head, 400, "Bad Request", "{\"error\":\"BAD_TOKEN_ID\"}"); return; }
  const auto query = http::query_string::Query::FromTarget(request.raw_target);
  http::query_string::Component value;
  char range_text[16] = "range=1h";
  if (!query.Valid()) { reply(client, head, 400, "Bad Request", "{\"error\":\"BAD_RANGE\"}"); return; }
  if (query.First("range", &value)) {
    const size_t length = value.Decode(range_text + 6, sizeof(range_text) - 7);
    if (length == SIZE_MAX || length > sizeof(range_text) - 7) {
      reply(client, head, 400, "Bad Request", "{\"error\":\"BAD_RANGE\"}"); return;
    }
    range_text[6 + length] = '\0';
  }
  const char* range = range_text;
  int64_t minutes = 0;
  if (strcmp(range, "range=5m") == 0) minutes = 5;
  else if (strcmp(range, "range=15m") == 0) minutes = 15;
  else if (strcmp(range, "range=30m") == 0) minutes = 30;
  else if (strcmp(range, "range=1h") == 0) minutes = 60;
  else if (strcmp(range, "range=8h") == 0) minutes = 480;
  else { reply(client, head, 400, "Bad Request", "{\"error\":\"BAD_RANGE\"}"); return; }
  fortis::storage::KvStore* kv = price_store(context);
  if (!kv) { reply(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_UNAVAILABLE\"}"); return; }
  fortis::prices::PriceSeriesStore series(*kv);
  const int64_t now = now_ms();
  fortis::prices::PriceSeriesCandle points[kChartPoints]{};
  size_t count = 0;
  const auto status = series.Range(fortis::prices::PriceSeriesInterval::k5m, "usd", id,
      now - minutes * 60000, now, now, points, kChartPoints, &count);
  if (status != fortis::prices::PriceSeriesReadStatus::kFound &&
      status != fortis::prices::PriceSeriesReadStatus::kMissing) {
    reply(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_UNAVAILABLE\"}");
    return;
  }
  uint8_t body[kChartBodyBytes];
  OutBuffer out{body, sizeof(body)};
  out.Text("{\"id\":"); AppendJsonString(out, id, strlen(id));
  out.Text(",\"now\":"); AppendInteger(out, now);
  out.Text(",\"range\":"); AppendJsonString(out, range + 6, strlen(range + 6));
  out.Text(",\"bucketMinutes\":5,\"points\":[");
  for (size_t i = 0; i < count; ++i) {
    if (i) out.Byte(',');
    out.Byte('['); AppendInteger(out, points[i].ts);
    out.Byte(','); AppendJsNumber(out, points[i].close); out.Byte(']');
  }
  out.Text("],\"spot\":");
  fortis::prices::PriceSeriesSpot spot{};
  if (series.GetSpot("usd", id, now, &spot) == fortis::prices::PriceSeriesReadStatus::kFound) {
    out.Text("{\"price\":"); AppendJsNumber(out, spot.price);
    out.Text(",\"asOf\":"); AppendInteger(out, spot.as_of_ms); out.Byte('}');
  } else out.Text("null");
  out.Byte('}');
  if (out.ok) {
    const http::HttpResponse response{200, "OK", "application/json; charset=utf-8", body, out.used};
    WriteHttpResponse(client, response, head);
  } else reply(client, head, 503, "Service Unavailable", "{\"error\":\"RESPONSE_TOO_LARGE\"}");
}
}  // namespace

bool RegisterTokenPricesRoutes(http::HttpRouter& router, fortis::storage::KvStore* price_kv) {
  return router.Add("GET", "/tokens", handle_page) &&
      router.Add("GET", "/tokens/data", handle_list, price_kv) &&
      router.Add("GET", "/tokens/data/:id", handle_chart, price_kv);
}

}  // namespace fortis::operations
