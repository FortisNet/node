#include "ownership.h"
#include "query_string.h"
#include "cache_explorer_route.h"

#include "cache_explorer_page.h"
#include "kv_runtime.h"
#include "price_history_batch_codec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::operations {

namespace {
using fortis::codec::history_batch::AppendInteger;
using fortis::codec::history_batch::AppendJsonString;
using fortis::codec::history_batch::OutBuffer;

enum class View : uint8_t { Json, Envelope, Binary, Metadata };
struct Namespace {
  const char* group;
  const char* slug;
  const char* label;
  const char* prefix;
  View view;
};

// Prefixes are explicit so browser input can never select an arbitrary key.
// Metadata hides both keys and values for anything that can identify a user,
// a wallet, a support case or a provider URL with credentials.
constexpr Namespace kNamespaces[] = {
  {"tokens", "facts", "Token facts", "token/facts/", View::Json},
  {"tokens", "demand", "Token demand", "token/demand/", View::Json},
  {"tokens", "fetch", "Fetch statistics", "token/fetch/", View::Json},
  {"tokens", "registry", "Token identity registry", "token/registry/", View::Binary},
  {"tokens", "comparison", "Market comparisons", "token/comparison/", View::Binary},
  {"tokens", "liquidity-raw", "Liquidity observations", "token/liqraw/", View::Binary},
  {"tokens", "liquidity-pair", "Latest liquidity pairs", "token/liqpair/", View::Binary},
  {"tokens", "liquidity-candles", "Liquidity candles", "token/liqcandle/", View::Binary},
  {"tokens", "holders", "Holder history", "token/holder/", View::Binary},
  {"prices", "spot-cache", "Spot quote cache", "price/spot/v1/", View::Binary},
  {"prices", "spot-series", "Spot observations", "price/series/v1/", View::Binary},
  {"prices", "open", "Open candles", "price/open/v1/", View::Binary},
  {"prices", "blocks", "Sealed candle blocks", "price/block/v1/", View::Binary},
  {"prices", "seals", "Seal markers", "price/seal/v1/", View::Binary},
  {"prices", "coverage", "History coverage", "price/history-coverage/v2/", View::Binary},
  {"discovery", "markets", "Market snapshots", "mkt:usd:", View::Envelope},
  {"discovery", "search", "Token search", "search:", View::Metadata},
  {"discovery", "details", "Coin details", "cgdetail:", View::Envelope},
  {"discovery", "taostats", "Bittensor analytics", "taostats:", View::Envelope},
  {"discovery", "overviews", "Network popular tokens", "netoverview:v3:", View::Envelope},
  {"news", "stories", "News stories", "news/item/", View::Json},
  {"news", "time", "News time index", "news/time/", View::Binary},
  {"news", "updated", "News update index", "news/updated/", View::Binary},
  {"news", "sources", "News source index", "news/source/", View::Binary},
  {"news", "media", "Media records", "media/item/", View::Metadata},
  {"news", "media-blobs", "Media blob chunks", "media/blob/", View::Metadata},
  {"address", "accounts", "Accounts", "account/id/", View::Metadata},
  {"address", "balances", "Balance snapshots", "balance/account/", View::Metadata},
  {"address", "transfer-rows", "Transfers", "transfer/row/", View::Metadata},
  {"address", "transfer-sync", "Transfer sync state", "transfer/sync/", View::Metadata},
  {"address", "sync-log", "Account sync log", "sync2/account/", View::Metadata},
  {"address", "pending-tx", "Pending transactions", "tx/pending/", View::Metadata},
  {"address", "tx-detail", "Transaction details", "tx/detail/", View::Metadata},
  {"address", "watches", "Device watches", "watch/", View::Metadata},
  {"private", "devices", "Device records", "device/", View::Metadata},
  {"private", "conversations", "AI conversations", "ai/", View::Metadata},
  {"private", "support", "Support records", "support/", View::Metadata},
  {"private", "crashes", "Crash reports", "crash/report/", View::Metadata},
};

constexpr size_t kPageSize = 32;
constexpr size_t kPreviewBytes = 4096;
constexpr size_t kReplyBytes = 768 * 1024;
constexpr size_t kKeyBytes = 1025;

bool valid_group(const char* group) {
  return group && (strcmp(group, "inventory") == 0 || strcmp(group, "tokens") == 0 ||
      strcmp(group, "prices") == 0 || strcmp(group, "discovery") == 0 ||
      strcmp(group, "news") == 0 || strcmp(group, "address") == 0);
}

bool visible(const Namespace& item, const char* group) {
  return strcmp(group, "inventory") == 0 || strcmp(item.group, group) == 0;
}

const Namespace* find_namespace(const char* group, const char* slug) {
  if (!valid_group(group) || !slug) return nullptr;
  for (const Namespace& item : kNamespaces)
    if (visible(item, group) && strcmp(item.slug, slug) == 0) return &item;
  return nullptr;
}

const char* view_name(View view) {
  switch (view) {
    case View::Json: return "json";
    case View::Envelope: return "envelope";
    case View::Binary: return "binary";
    case View::Metadata: return "metadata";
  }
  return "metadata";
}

int64_t now_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void error(int client, bool head, int status, const char* reason, const char* body) {
  WriteHttpResponse(client, http::JsonResponse(status, reason, body), head);
}

void handle_page(int client, const http::HttpRequest& request, bool head, void*) {
  if (!valid_group(HttpPathParamValue(request, "group"))) {
    error(client, head, 404, "Not Found", "{\"error\":\"UNKNOWN_CACHE_PAGE\"}");
    return;
  }
  const http::HttpResponse response{200, "OK", "text/html; charset=utf-8",
      reinterpret_cast<const uint8_t*>(kCacheExplorerPage), sizeof(kCacheExplorerPage) - 1};
  WriteHttpResponse(client, response, head);
}

void handle_namespaces(int client, const http::HttpRequest& request, bool head, void*) {
  const char* group = HttpPathParamValue(request, "group");
  if (!valid_group(group)) {
    error(client, head, 404, "Not Found", "{\"error\":\"UNKNOWN_CACHE_PAGE\"}");
    return;
  }
  uint8_t body[16 * 1024];
  OutBuffer out{body, sizeof(body)};
  out.Text("{\"group\":"); AppendJsonString(out, group, strlen(group));
  out.Text(",\"namespaces\":[");
  bool first = true;
  for (const Namespace& item : kNamespaces) {
    if (!visible(item, group)) continue;
    if (!first) out.Byte(',');
    first = false;
    out.Text("{\"slug\":"); AppendJsonString(out, item.slug, strlen(item.slug));
    out.Text(",\"label\":"); AppendJsonString(out, item.label, strlen(item.label));
    out.Text(",\"prefix\":"); AppendJsonString(out, item.prefix, strlen(item.prefix));
    out.Text(",\"group\":"); AppendJsonString(out, item.group, strlen(item.group));
    out.Text(",\"view\":");
    const char* view = view_name(item.view);
    AppendJsonString(out, view, strlen(view));
    out.Byte('}');
  }
  out.Byte(']');
  if (strcmp(group, "inventory") == 0) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    if (kv) {
      fortis::storage::KvCompactionStats stats{};
      kv->CompactionStats(&stats);
      out.Text(",\"stats\":{\"keyCount\":"); AppendInteger(out, kv->Size());
      out.Text(",\"fileBytes\":"); AppendInteger(out, stats.file_bytes);
      out.Text(",\"liveBytes\":"); AppendInteger(out, stats.live_bytes);
      out.Text(",\"otherBytes\":");
      AppendInteger(out, stats.file_bytes > stats.live_bytes ?
          stats.file_bytes - stats.live_bytes : 0);
      out.Byte('}');
    }
  }
  out.Byte('}');
  if (!out.ok) {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"RESPONSE_TOO_LARGE\"}");
    return;
  }
  const http::HttpResponse response{200, "OK", "application/json; charset=utf-8", body, out.used};
  WriteHttpResponse(client, response, head);
}

struct EnvelopeHeader {
  uint32_t magic;
  uint32_t body_length;
  int64_t fresh_until_ms;
  int64_t stale_until_ms;
  int64_t as_of_ms;
};
static_assert(sizeof(EnvelopeHeader) == 32, "DiscoveryCache envelope layout changed");

struct Row {
  char key[kKeyBytes]{};
  char preview[kPreviewBytes + 1]{};
  size_t bytes = 0;
  size_t preview_length = 0;
  bool truncated = false;
  bool corrupt = false;
  int64_t as_of_ms = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
};
struct PageData {
  const Namespace* bucket = nullptr;
  Row rows[kPageSize]{};
  size_t count = 0;
  uint64_t bytes = 0;
};

bool collect(const char* key, const void* value, size_t size, PageData* page) {
  if (page->count >= kPageSize) return false;
  Row& row = page->rows[page->count++];
  row.bytes = size;
  page->bytes += size;
  if (page->bucket->view == View::Metadata) return true;
  const size_t key_length = strnlen(key, sizeof(row.key));
  if (key_length >= sizeof(row.key)) return false;
  memcpy(row.key, key, key_length + 1);
  if (page->bucket->view == View::Binary) return true;
  const auto* bytes = static_cast<const uint8_t*>(value);
  if (page->bucket->view == View::Envelope) {
    EnvelopeHeader header{};
    if (size < sizeof(header)) { row.corrupt = true; return true; }
    memcpy(&header, bytes, sizeof(header));
    if (header.magic != 0x31435344 || header.body_length != size - sizeof(header) ||
        header.stale_until_ms < header.fresh_until_ms) {
      row.corrupt = true;
      return true;
    }
    row.as_of_ms = header.as_of_ms;
    row.fresh_until_ms = header.fresh_until_ms;
    row.stale_until_ms = header.stale_until_ms;
    bytes += sizeof(header);
    size = header.body_length;
  }
  row.preview_length = size < kPreviewBytes ? size : kPreviewBytes;
  row.truncated = size > kPreviewBytes;
  // A clipped JSON string still needs valid UTF-8 for the enclosing response.
  // If the first omitted byte continues a code point, drop its partial prefix.
  if (row.truncated)
    while (row.preview_length && (bytes[row.preview_length] & 0xc0) == 0x80)
      --row.preview_length;
  memcpy(row.preview, bytes, row.preview_length);
  row.preview[row.preview_length] = '\0';
  return true;
}

// Accept only one encoded `after` query parameter. The cursor is a KV key
// returned by a previous page, bounded by the store's key limit.
bool parse_after(const http::HttpRequest& request, char output[kKeyBytes], bool* present) {
  *present = false;
  output[0] = '\0';
  const auto query = http::query_string::Query::FromTarget(request.raw_target);
  if (!query.Valid()) return false;
  http::query_string::Component value;
  if (!query.First("after", &value)) return true;
  const size_t used = value.Decode(output, kKeyBytes - 1);
  if (used == SIZE_MAX || used >= kKeyBytes) return false;
  for (size_t i = 0; i < used; ++i)
    if (static_cast<unsigned char>(output[i]) < 0x21 || static_cast<unsigned char>(output[i]) > 0x7e) return false;
  output[used] = '\0';
  *present = used > 0;
  return true;
}

void handle_records(int client, const http::HttpRequest& request, bool head, void* context) {
  const Namespace* bucket = find_namespace(HttpPathParamValue(request, "group"),
                                            HttpPathParamValue(request, "bucket"));
  if (!bucket) {
    error(client, head, 404, "Not Found", "{\"error\":\"UNKNOWN_NAMESPACE\"}");
    return;
  }
  char after[kKeyBytes];
  bool has_after = false;
  if (!parse_after(request, after, &has_after) ||
      (has_after && strncmp(after, bucket->prefix, strlen(bucket->prefix)) != 0) ||
      (bucket->view == View::Metadata && has_after)) {
    error(client, head, 400, "Bad Request", "{\"error\":\"BAD_CURSOR\"}");
    return;
  }
  fortis::storage::KvStore* kv = context && strcmp(bucket->group, "prices") == 0
      ? static_cast<fortis::storage::KvStore*>(context) : fortis::storage::RuntimeKvStore();
  if (!kv) {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_UNAVAILABLE\"}");
    return;
  }
  auto page_storage = base::AllocateZeroedStorage<PageData>(1, sizeof(PageData));
  auto* page = page_storage.get();
  auto body_storage = base::AllocateStorage<uint8_t[]>(kReplyBytes);
  auto* body = body_storage.get();
  if (!page || !body) {
    page_storage.reset(); body_storage.reset();
    error(client, head, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}");
    return;
  }
  page->bucket = bucket;
  fortis::storage::KvPageResult result{};
  const bool ok = kv->Page(bucket->prefix, has_after ? after : nullptr, kPageSize, now_ms(), false, [&, visitor_state = page](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }, &result);
  if (!ok) {
    page_storage.reset(); body_storage.reset();
    error(client, head, 503, "Service Unavailable", "{\"error\":\"CACHE_READ_FAILED\"}");
    return;
  }
  OutBuffer out{body, kReplyBytes};
  out.Text("{\"namespace\":"); AppendJsonString(out, bucket->label, strlen(bucket->label));
  out.Text(",\"prefix\":"); AppendJsonString(out, bucket->prefix, strlen(bucket->prefix));
  out.Text(",\"view\":");
  const char* view = view_name(bucket->view);
  AppendJsonString(out, view, strlen(view));
  out.Text(",\"scanned\":"); AppendInteger(out, result.scanned);
  out.Text(",\"live\":"); AppendInteger(out, page->count);
  out.Text(",\"bytes\":"); AppendInteger(out, page->bytes);
  out.Text(",\"hasMore\":"); out.Text(result.done ? "false" : "true");
  out.Text(",\"nextCursor\":");
  if (!result.done && bucket->view != View::Metadata)
    AppendJsonString(out, result.next_cursor, strlen(result.next_cursor));
  else out.Text("null");
  out.Text(",\"rows\":[");
  for (size_t i = 0; i < page->count; ++i) {
    const Row& row = page->rows[i];
    if (i) out.Byte(',');
    out.Text("{\"bytes\":"); AppendInteger(out, row.bytes);
    if (bucket->view != View::Metadata) {
      out.Text(",\"key\":"); AppendJsonString(out, row.key, strlen(row.key));
      out.Text(",\"preview\":");
      if (bucket->view == View::Json || bucket->view == View::Envelope)
        AppendJsonString(out, row.preview, row.preview_length);
      else out.Text("null");
      out.Text(",\"truncated\":"); out.Text(row.truncated ? "true" : "false");
      out.Text(",\"corrupt\":"); out.Text(row.corrupt ? "true" : "false");
      if (bucket->view == View::Envelope) {
        out.Text(",\"asOf\":"); AppendInteger(out, row.as_of_ms);
        out.Text(",\"freshUntil\":"); AppendInteger(out, row.fresh_until_ms);
        out.Text(",\"staleUntil\":"); AppendInteger(out, row.stale_until_ms);
      }
    }
    out.Byte('}');
  }
  out.Text("]}");
  if (out.ok) {
    const http::HttpResponse response{200, "OK", "application/json; charset=utf-8", body, out.used};
    WriteHttpResponse(client, response, head);
  } else {
    error(client, head, 503, "Service Unavailable", "{\"error\":\"RESPONSE_TOO_LARGE\"}");
  }
  page_storage.reset(); body_storage.reset();
}
}  // namespace

bool RegisterCacheExplorerRoutes(http::HttpRouter& router, fortis::storage::KvStore* price_kv) {
  return router.Add("GET", "/cache/:group", handle_page) &&
      router.Add("GET", "/cache-data/:group", handle_namespaces) &&
      router.Add("GET", "/cache-data/:group/:bucket", handle_records, price_kv);
}

}  // namespace fortis::operations
