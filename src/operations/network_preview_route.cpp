#include "ownership.h"
#include "network_preview_route.h"

#include "discovery_cache.h"
#include "kv_runtime.h"
#include "network_preview_page.h"
#include "network_route.h"
#include "price_history_batch_codec.h"
#include "price_series_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::operations {

namespace {
using fortis::codec::history_batch::AppendInteger;
using fortis::codec::history_batch::AppendJsonString;
using fortis::codec::history_batch::AppendJsNumber;
using fortis::codec::history_batch::OutBuffer;

constexpr size_t kCacheScratchBytes = 1024 * 1024;  // KV value limit
constexpr size_t kCacheReplyBytes = 1024 * 1024;    // HTTP response limit

int64_t now_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void error(int client, bool head, int status, const char* reason, const char* body) {
  WriteHttpResponse(client, http::JsonResponse(status, reason, body), head);
}

void handle_page(int client, const http::HttpRequest&, bool head, void*) {
  const http::HttpResponse response{200, "OK", "text/html; charset=utf-8",
      reinterpret_cast<const uint8_t*>(kNetworkPreviewPage),
      sizeof(kNetworkPreviewPage) - 1};
  WriteHttpResponse(client, response, head);
}

void handle_catalog(int client, const http::HttpRequest&, bool head, void*) {
  size_t length = 0;
  const char* body = NetworkCatalogJson(&length);
  if (!body) {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"CATALOG_UNAVAILABLE\"}");
    return;
  }
  http::HttpResponse response{200, "OK", "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), length};
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_cache(int client, const http::HttpRequest& request, bool head, void* opaque) {
  const auto* registry = static_cast<const networks::NativeNetworkRegistry*>(opaque);
  const char* requested = HttpPathParamValue(request, "id");
  size_t count = 0;
  const networks::NativeNetworkConfig* rows = registry->list(&count);
  const networks::NativeNetworkConfig* selected = nullptr;
  if (requested && rows)
    for (size_t i = 0; i < count; ++i)
      if (strcmp(requested, rows[i].id) == 0) { selected = &rows[i]; break; }
  if (!selected) {
    error(client, head, 404, "Not Found", "{\"error\":\"UNKNOWN_NETWORK\"}");
    return;
  }
  const char* id = selected->id;
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (!kv) {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_UNAVAILABLE\"}");
    return;
  }
  char key[96];
  const int key_length = snprintf(key, sizeof(key), "netoverview:v3:%s", id);
  if (key_length < 0 || static_cast<size_t>(key_length) >= sizeof(key)) {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_KEY_TOO_LONG\"}");
    return;
  }
  auto scratch_storage = base::AllocateStorage<unsigned char[]>(kCacheScratchBytes);
  auto* scratch = scratch_storage.get();
  auto body_storage = base::AllocateStorage<uint8_t[]>(kCacheReplyBytes);
  auto* body = body_storage.get();
  if (!scratch || !body) {
    scratch_storage.reset(); body_storage.reset();
    error(client, head, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}");
    return;
  }
  discovery::DiscoveryCache cache(kv, scratch, kCacheScratchBytes, nullptr);
  const int64_t now = now_ms();
  const discovery::DiscoveryCacheEntry entry = cache.Read(key, now);
  const char* state = "missing";
  if (entry.state == discovery::DiscoveryCacheState::kFresh) state = "fresh";
  else if (entry.state == discovery::DiscoveryCacheState::kStale) state = "stale";
  else if (entry.state == discovery::DiscoveryCacheState::kError) state = "error";
  else if (entry.as_of_ms) state = "expired";
  OutBuffer out{body, kCacheReplyBytes};
  out.Text("{\"id\":"); AppendJsonString(out, id, strlen(id));
  out.Text(",\"key\":"); AppendJsonString(out, key, strlen(key));
  out.Text(",\"state\":"); AppendJsonString(out, state, strlen(state));
  out.Text(",\"asOf\":"); AppendInteger(out, entry.as_of_ms);
  out.Text(",\"freshUntil\":"); AppendInteger(out, entry.fresh_until_ms);
  out.Text(",\"staleUntil\":"); AppendInteger(out, entry.stale_until_ms);
  out.Text(",\"body\":");
  if ((entry.state == discovery::DiscoveryCacheState::kFresh ||
       entry.state == discovery::DiscoveryCacheState::kStale) && entry.body)
    AppendJsonString(out, reinterpret_cast<const char*>(entry.body), entry.body_length);
  else out.Text("null");
  out.Text(",\"nativePrice\":");
  fortis::prices::PriceSeriesSpot price{};
  fortis::prices::PriceSeriesStore series(*kv);
  if (selected->native_coingecko_id &&
      series.GetSpot("usd", selected->native_coingecko_id, now, &price) ==
          fortis::prices::PriceSeriesReadStatus::kFound) {
    out.Text("{\"price\":"); AppendJsNumber(out, price.price);
    out.Text(",\"asOf\":"); AppendInteger(out, price.as_of_ms);
    out.Byte('}');
  } else out.Text("null");
  out.Byte('}');
  if (out.ok) {
    const http::HttpResponse response{200, "OK", "application/json; charset=utf-8", body, out.used};
    WriteHttpResponse(client, response, head);
  } else {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_RESPONSE_TOO_LARGE\"}");
  }
  scratch_storage.reset(); body_storage.reset();
}
}  // namespace

bool RegisterNetworkPreviewRoutes(http::HttpRouter& router,
                                  const networks::NativeNetworkRegistry* registry) {
  return registry && router.Add("GET", "/networks", handle_page) &&
      router.Add("GET", "/networks/data", handle_catalog) &&
      router.Add("GET", "/networks/data/:id", handle_cache,
                 const_cast<networks::NativeNetworkRegistry*>(registry));
}

}  // namespace fortis::operations
