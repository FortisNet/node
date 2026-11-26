#include "query_string.h"
#include "token_search_route.h"
#include "http_middleware.h"
#include "ownership.h"

// GET /v1/networks/{network}/tokens/search. One route object for the normal
// image (behind FORTIS_NATIVE_PRICE_ROUTE_CANARY=1) and the test binaries;
// the test clock and jitter arrive only through the DiscoveryCache seams.

#include "device_enrollment.h"
#include "js_case.h"
#include "media_registry.h"
#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "price_id_parser.h"
#include "text_sanitize.h"
#include "threading.h"
#include "token_ref.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::discovery {

namespace {

using fortis::compat::js_json::OutBuffer;
namespace hb = fortis::codec::history_batch;

constexpr uint32_t kCost = 10;
constexpr size_t kJobs = 8;
constexpr size_t kWaiters = 8;
constexpr size_t kHits = 5;
constexpr size_t kMaxItems = 20;
constexpr size_t kMaxQueryUnits = 100;
constexpr size_t kQueryBytes = 640;
constexpr size_t kKeyBytes = 1024;
constexpr size_t kCoinIdBytes = 512;
constexpr size_t kRecordBytes = 96 * 1024;
constexpr size_t kDetailRefreshes = 16;
constexpr int64_t kCatalogMs = 15 * 60000;
constexpr size_t kCatalogAssets = 512;
constexpr size_t kCatalogArena = 256 * 1024;
constexpr char kDetailQuery[] =
    "?localization=false&tickers=false&market_data=false&community_data=false"
    "&developer_data=false&sparkline=false";

enum class Source : uint8_t { kNone, kCoinGecko, kJupiter, kRobinhood };
enum class Detail : uint8_t { kUnread, kPending, kReady, kFailed };
enum class CallKind : uint8_t { kSearch, kDetail, kJupiter, kRobinhood };

struct Job;

struct Waiter {
  Job* job = nullptr;
  bool cancelled = false;
  bool protobuf = false;
  uint64_t token = 0;
  int rate_cost = -1;
  uint32_t remaining = 0;
};

struct Call {
  Job* job = nullptr;
  CallKind kind = CallKind::kSearch;
  uint8_t hit = 0;
};

struct Job {
  bool used = false, background = false, partial = false, done = false;
  unsigned references = 0;
  size_t outstanding = 0;
  Source source = Source::kNone;
  const char* platform = nullptr;
  networks::TokenNetwork network{};
  char query[kQueryBytes]{};
  size_t query_length = 0;
  char cache_key[kKeyBytes]{};
  size_t waiter_count = 0;
  Waiter waiters[kWaiters];
  Call calls[1 + kHits];
  size_t hit_count = 0;
  char hits[kHits][kCoinIdBytes]{};
  size_t hit_lengths[kHits]{};
  Detail details[kHits]{};
};

struct DetailRefresh {
  bool used = false;
  char key[kKeyBytes]{};
  char path[kDiscoveryMaxPathBytes]{};
};

struct CatalogAsset {
  uint32_t symbol = 0, symbol_length = 0, name = 0, name_length = 0, logo = 0, logo_length = 0;
  bool active = false, has_logo = false;
  char address[43]{};
};

struct Catalog {
  bool valid = false;
  int64_t until = 0;
  size_t count = 0;
  CatalogAsset assets[kCatalogAssets];
  size_t used = 0;
  char arena[kCatalogArena];
};

struct JupiterRow { uint32_t index; bool verified; double holders; };

// One search answer: the native record stored under the `search:` key.
struct Item {
  const char* network; size_t network_length;
  const char* token_id; size_t token_id_length;
  const char* address; size_t address_length;
  const char* symbol; size_t symbol_length;   // sanitized WTF-8
  const char* name; size_t name_length;       // sanitized WTF-8
  double decimals;
  bool verified;
  bool has_holders;
  double holders;
  bool has_logo;
  char logo[fortis::discovery::market_view::kMediaKeyChars + 1];
};

}  // namespace

struct TokenSearchState {
  DiscoveryRouteContext* route = nullptr;
  JupiterRow rows[kDiscoveryTokens / 2];
  fortis::base::Mutex refs_mutex;
  Job jobs[kJobs];
  DetailRefresh refreshes[kDetailRefreshes];
  Catalog catalog;
  Item items[64];
  char strings[64 * 1024];
  size_t strings_used = 0;
  uint8_t record[kRecordBytes];
  codec::BoundedJsonToken row_tokens[codec::kBoundedJsonMaxTokens];
};

TokenSearchState* g_search_state = nullptr;

namespace {

// ── Small helpers ──

void release(TokenSearchState* state, Job* job) {
  fortis::base::LockGuard refs_lock(state->refs_mutex);
  if (job->references) --job->references;
  if (!job->references) job->used = false;
}

void retain(TokenSearchState* state, Job* job) {
  fortis::base::LockGuard refs_lock(state->refs_mutex);
  ++job->references;
}

void waiter_terminal(void* opaque, bool cancelled) {
  auto* waiter = static_cast<Waiter*>(opaque);
  TokenSearchState* state = g_search_state;
  Job* job = waiter->job;
  if (cancelled) {
    fortis::base::LockGuard refs_lock(state->refs_mutex);
    waiter->cancelled = true;
  }
  release(state, job);
}

// encodeURIComponent byte serializer.
void append_component(OutBuffer& out, const char* text, size_t length) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' ||
        c == '(' || c == ')') {
      out.Byte(c);
    } else {
      out.Byte('%'); out.Byte(static_cast<uint8_t>(kHex[c >> 4]));
      out.Byte(static_cast<uint8_t>(kHex[c & 15]));
    }
  }
}

int hex(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// ── Path refusals, before any hook ──

// A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}
// before any hook (no quota).
bool reject_raw_non_ascii(int client, const http::HttpRequest& request, bool head) {
  for (const char* p = request.raw_target ? request.raw_target : ""; *p; ++p) {
    if (static_cast<unsigned char>(*p) < 0x80) continue;
    http::WriteRefusal(client, 400, "Bad Request", "BAD_REQUEST", head);
    return true;
  }
  return false;
}

// {network} over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"}
// before any hook (no auth, no quota).
bool reject_long_network(int client, const http::HttpRequest& request, bool head) {
  const http::HttpPathParam* param = HttpPathParamFind(request, "network");
  if (!param || param->utf16_units <= 100) return false;
  http::WriteRefusal(client, 414, "URI Too Long", "URI_TOO_LONG", head);
  return true;
}

// {network}, percent-decoded by the router (which has refused an empty
// segment, a control character/DEL and a value over 512 bytes with 404).
// SIZE_MAX when it does not fit `capacity`.
size_t network_param(const http::HttpRequest& request, char* out, size_t capacity) {
  const http::HttpPathParam* param = HttpPathParamFind(request, "network");
  if (!param) return SIZE_MAX;
  const size_t used = strlen(param->value);
  if (used + 1 > capacity) return SIZE_MAX;
  memcpy(out, param->value, used + 1);
  return used;
}

// The facet's RangeError `unknown network: ${networkId}` with the id as
// decoded, built by length (a NUL is \u0000, as JSON.stringify writes it).
void send_unknown_network(int client, const char* network, size_t length, bool head, int cost,
                          uint32_t remaining) {
  static const char kPrefix[] = "unknown network: ";
  char detail[sizeof(kPrefix) + 600];
  char body[4096];
  OutBuffer out{reinterpret_cast<uint8_t*>(body), sizeof(body) - 1};
  memcpy(detail, kPrefix, sizeof(kPrefix) - 1);
  memcpy(detail + sizeof(kPrefix) - 1, network, length);
  out.Text("{\"error\":\"BAD_REQUEST\",\"detail\":");
  hb::AppendJsonString(out, detail, sizeof(kPrefix) - 1 + length);
  out.Byte('}');
  body[out.ok ? out.used : 0] = '\0';
  SendDiscoveryJson(client, 400, "Bad Request", body, head, cost, remaining);
}

// num(): a finite JSON number, else 0.
double num(const codec::BoundedJsonDocument& document, uint32_t object, const char* name) {
  const uint32_t index = document.FindObjectValue(object, name);
  double value = 0;
  if (index == codec::kBoundedJsonNoToken || document.token(index)->type != codec::BoundedJsonType::kNumber ||
      !document.Number(index, &value)) return 0;
  return value;
}

bool is_true(const codec::BoundedJsonDocument& document, uint32_t object, const char* name) {
  const uint32_t index = document.FindObjectValue(object, name);
  return index != codec::kBoundedJsonNoToken && document.token(index)->type == codec::BoundedJsonType::kTrue;
}

// A decoded string member into the state's string arena.
bool arena_string(TokenSearchState* state, const codec::BoundedJsonDocument& document, uint32_t index,
                  const char** text, size_t* length) {
  if (index == codec::kBoundedJsonNoToken || document.token(index)->type != codec::BoundedJsonType::kString)
    return false;
  if (state->strings_used >= sizeof(state->strings)) return false;
  char* out = state->strings + state->strings_used;
  if (!document.DecodeString(index, out, sizeof(state->strings) - state->strings_used, length))
    return false;
  state->strings_used += *length + 1;
  *text = out;
  return true;
}

const char* arena_copy(TokenSearchState* state, const char* text, size_t length) {
  if (sizeof(state->strings) - state->strings_used <= length) return nullptr;
  char* out = state->strings + state->strings_used;
  memcpy(out, text, length);
  out[length] = '\0';
  state->strings_used += length + 1;
  return out;
}

// ── The answer record ──

void put_string(OutBuffer& out, const char* text, size_t length) {
  out.Byte(static_cast<uint8_t>(length)); out.Byte(static_cast<uint8_t>(length >> 8));
  out.Byte(static_cast<uint8_t>(length >> 16));
  out.Append(text, length);
}

void put_double(OutBuffer& out, double value) {
  uint8_t bytes[8];
  memcpy(bytes, &value, 8);
  out.Append(bytes, 8);
}

bool write_record(TokenSearchState* state, const Item* items, size_t count, size_t* length) {
  OutBuffer out{state->record, sizeof(state->record)};
  out.Text("TSR1");
  out.Byte(static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; ++i) {
    const Item& item = items[i];
    put_string(out, item.network, item.network_length);
    put_string(out, item.token_id, item.token_id_length);
    put_string(out, item.address, item.address_length);
    put_string(out, item.symbol, item.symbol_length);
    put_string(out, item.name, item.name_length);
    put_double(out, item.decimals);
    out.Byte(item.verified);
    out.Byte(item.has_holders);
    put_double(out, item.holders);
    out.Byte(item.has_logo);
    if (item.has_logo) out.Append(item.logo, fortis::discovery::market_view::kMediaKeyChars);
  }
  *length = out.used;
  return out.ok;
}

struct Reader {
  const uint8_t* data;
  size_t length;
  size_t position = 0;
  bool ok = true;
  bool Bytes(size_t count, const uint8_t** out) {
    if (!ok || length - position < count) { ok = false; return false; }
    *out = data + position;
    position += count;
    return true;
  }
  bool String(const char** text, size_t* text_length) {
    const uint8_t* size = nullptr;
    if (!Bytes(3, &size)) return false;
    *text_length = static_cast<size_t>(size[0]) | static_cast<size_t>(size[1]) << 8 |
                   static_cast<size_t>(size[2]) << 16;
    const uint8_t* bytes = nullptr;
    if (!Bytes(*text_length, &bytes)) return false;
    *text = reinterpret_cast<const char*>(bytes);
    return true;
  }
  bool Double(double* value) {
    const uint8_t* bytes = nullptr;
    if (!Bytes(8, &bytes)) return false;
    memcpy(value, bytes, 8);
    return true;
  }
  bool Byte(uint8_t* value) {
    const uint8_t* bytes = nullptr;
    if (!Bytes(1, &bytes)) return false;
    *value = bytes[0];
    return true;
  }
};

bool read_record(const uint8_t* data, size_t length, Item* items, size_t* count) {
  Reader reader{data, length};
  const uint8_t* magic = nullptr;
  uint8_t n = 0;
  if (!reader.Bytes(4, &magic) || memcmp(magic, "TSR1", 4) != 0 || !reader.Byte(&n) ||
      n > kMaxItems) return false;
  for (size_t i = 0; i < n; ++i) {
    Item& item = items[i];
    uint8_t verified = 0, has_holders = 0, has_logo = 0;
    if (!reader.String(&item.network, &item.network_length) ||
        !reader.String(&item.token_id, &item.token_id_length) ||
        !reader.String(&item.address, &item.address_length) ||
        !reader.String(&item.symbol, &item.symbol_length) ||
        !reader.String(&item.name, &item.name_length) || !reader.Double(&item.decimals) ||
        !reader.Byte(&verified) || !reader.Byte(&has_holders) || !reader.Double(&item.holders) ||
        !reader.Byte(&has_logo)) return false;
    item.verified = verified; item.has_holders = has_holders; item.has_logo = has_logo;
    if (item.has_logo) {
      const uint8_t* logo = nullptr;
      if (!reader.Bytes(fortis::discovery::market_view::kMediaKeyChars, &logo)) return false;
      memcpy(item.logo, logo, fortis::discovery::market_view::kMediaKeyChars);
      item.logo[fortis::discovery::market_view::kMediaKeyChars] = '\0';
    }
  }
  *count = n;
  return reader.ok && reader.position == length;
}

// JSON.stringify(items): the members in TokenSearchItem order.
void append_items_json(OutBuffer& out, const Item* items, size_t count) {
  out.Byte('[');
  for (size_t i = 0; i < count; ++i) {
    const Item& item = items[i];
    if (i) out.Byte(',');
    out.Text("{\"network\":");
    hb::AppendJsonString(out, item.network, item.network_length);
    out.Text(",\"tokenId\":");
    hb::AppendJsonString(out, item.token_id, item.token_id_length);
    out.Text(",\"address\":");
    hb::AppendJsonString(out, item.address, item.address_length);
    out.Text(",\"symbol\":");
    compat::AppendSanitizedJsonString(out, item.symbol, item.symbol_length);
    out.Text(",\"name\":");
    compat::AppendSanitizedJsonString(out, item.name, item.name_length);
    out.Text(",\"decimals\":");
    hb::AppendJsNumber(out, item.decimals);
    out.Text(item.verified ? ",\"verified\":true,\"holderCount\":" :
                             ",\"verified\":false,\"holderCount\":");
    if (item.has_holders) hb::AppendJsNumber(out, item.holders);
    else out.Text("null");
    out.Text(",\"logoMediaKey\":");
    if (item.has_logo) { out.Byte('"'); out.Text(item.logo); out.Byte('"'); }
    else out.Text("null");
    out.Byte('}');
  }
  out.Byte(']');
}

// JSON.stringify({ items, cache }).
bool render_json(OutBuffer& out, const Item* items, size_t count, bool hit, int64_t as_of) {
  out.Text("{\"items\":");
  append_items_json(out, items, count);
  out.Text(",\"cache\":");
  AppendDiscoveryCacheJson(out, hit, hit ? as_of : -1, false, false);
  out.Byte('}');
  return out.ok;
}

bool encode_json(DiscoveryWorkspace* workspace, const Item* items, size_t count, bool hit,
                 int64_t as_of, http::HttpResponse* response) {
  OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
  if (!render_json(out, items, count, hit, as_of)) return false;
  *response = http::HttpResponse{200, "OK", "application/json; charset=utf-8", workspace->json_out,
                           out.used};
  response->vary_accept = true;
  return true;
}

void append_proto_string(OutBuffer& out, uint32_t field, const char* text, size_t length,
                         bool sanitized) {
  if (!length) return;  // proto3 implicit presence: "" is not written
  if (!sanitized) { fortis::codec::proto_wire::PutBytes(out, field, text, length); return; }
  static char converted[1024];
  const size_t used = compat::SanitizedToProtobufUtf8(text, length, converted, sizeof(converted));
  if (used == compat::kSanitizeFailed) { out.ok = false; return; }
  fortis::codec::proto_wire::PutBytes(out, field, converted, used);
}

// protobuf-es create(TokenSearchItemSchema): decimals is a uint32 and
// holderCount `BigInt(n)` a uint64; a value outside them throws (500).
bool append_proto_item(OutBuffer& out, const Item& item) {
  append_proto_string(out, 1, item.network, item.network_length, false);
  append_proto_string(out, 2, item.token_id, item.token_id_length, false);
  append_proto_string(out, 3, item.address, item.address_length, false);
  append_proto_string(out, 4, item.symbol, item.symbol_length, true);
  append_proto_string(out, 5, item.name, item.name_length, true);
  if (!(item.decimals >= 0 && item.decimals <= 4294967295.0 && floor(item.decimals) == item.decimals))
    return false;
  fortis::codec::proto_wire::PutTag(out, 6, 0);
  fortis::codec::proto_wire::PutVarint(out, static_cast<uint64_t>(item.decimals));
  if (item.verified) { fortis::codec::proto_wire::PutTag(out, 7, 0); out.Byte(1); }
  if (item.has_holders) {
    if (!(item.holders >= 0 && item.holders < 18446744073709551616.0 &&
          floor(item.holders) == item.holders)) return false;
    fortis::codec::proto_wire::PutTag(out, 8, 0);
    fortis::codec::proto_wire::PutVarint(out, static_cast<uint64_t>(item.holders));
  }
  if (item.has_logo)
    fortis::codec::proto_wire::PutBytes(out, 9, item.logo, fortis::discovery::market_view::kMediaKeyChars);
  return out.ok;
}

// create(TokenSearchResponseSchema) wire; false when protobuf-es would throw.
bool render_protobuf(OutBuffer& out, const Item* items, size_t count, bool hit, int64_t as_of) {
  for (size_t i = 0; i < count; ++i) {
    OutBuffer measure{};
    if (!append_proto_item(measure, items[i])) return false;
    fortis::codec::proto_wire::PutTag(out, 1, 2);
    fortis::codec::proto_wire::PutVarint(out, measure.used);
    append_proto_item(out, items[i]);
  }
  OutBuffer cache{};
  AppendDiscoveryCacheProto(cache, hit, hit ? as_of : -1, false);
  fortis::codec::proto_wire::PutTag(out, 2, 2);
  fortis::codec::proto_wire::PutVarint(out, cache.used);
  AppendDiscoveryCacheProto(out, hit, hit ? as_of : -1, false);
  return out.ok;
}

bool encode_protobuf(DiscoveryWorkspace* workspace, const Item* items, size_t count, bool hit,
                     int64_t as_of, http::HttpResponse* response) {
  OutBuffer out{workspace->proto_out, sizeof(workspace->proto_out)};
  if (!render_protobuf(out, items, count, hit, as_of)) return false;
  *response = http::HttpResponse{200, "OK", "application/protobuf", workspace->proto_out, out.used};
  response->vary_accept = true;
  return true;
}

// sendProtobufOrJson sets `vary: Accept` before encoding, so a value
// protobuf-es refuses (a fractional holder count) answers 500 with it.
http::HttpResponse internal_error() {
  http::HttpResponse response = http::JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
  response.content_type = "application/json; charset=utf-8";
  response.vary_accept = true;
  return response;
}

// ── Item lists ──

// dedupe() then slice(0, 20): `${network}:${address.toLowerCase()}`.
size_t dedupe(Item* items, size_t count) {
  static char lowered[kMaxItems][1100];
  static size_t lengths[kMaxItems];
  size_t kept = 0;
  for (size_t i = 0; i < count && kept < kMaxItems; ++i) {
    size_t length = 0;
    if (!fortis::compat::JsToLowerCase(items[i].address, items[i].address_length, lowered[kept],
                               sizeof(lowered[kept]), &length)) continue;
    bool seen = false;
    for (size_t j = 0; j < kept && !seen; ++j)
      seen = lengths[j] == length && items[j].network_length == items[i].network_length &&
             memcmp(lowered[j], lowered[kept], length) == 0 &&
             memcmp(items[j].network, items[i].network, items[i].network_length) == 0;
    if (seen) continue;
    lengths[kept] = length;
    items[kept++] = items[i];
  }
  return kept;
}

bool sanitized(TokenSearchState* state, const char* text, size_t length, bool symbol,
               const char** out, size_t* out_length) {
  char buffer[compat::kSanitizeNameMaxBytes + 1];
  const size_t used = symbol ? compat::SanitizeSymbol(text, length, buffer, sizeof(buffer))
                             : compat::SanitizeName(text, length, buffer, sizeof(buffer));
  if (used == compat::kSanitizeFailed) return false;
  *out = arena_copy(state, buffer, used);
  *out_length = used;
  return *out != nullptr;
}

const char* register_logo(DiscoveryWorkspace* workspace, const char* url, size_t length,
                          char key[fortis::discovery::market_view::kMediaKeyChars + 1]) {
  DiscoveryCache* cache = workspace->cache;
  return RegisterMediaSource(cache->kv(), url, length, cache->Now(), key) ? key : nullptr;
}

void set_logo(Item* item, const char* key) {
  item->has_logo = key != nullptr;
  if (key) memcpy(item->logo, key, fortis::discovery::market_view::kMediaKeyChars + 1);
}

// The kinds parseTokenRef may return that name an on-chain token.
bool family_token_kind(networks::TokenRefKind kind) {
  switch (kind) {
    case networks::TokenRefKind::kErc20: case networks::TokenRefKind::kTrc20: case networks::TokenRefKind::kSpl:
    case networks::TokenRefKind::kAsa: case networks::TokenRefKind::kCardano: case networks::TokenRefKind::kStellar:
    case networks::TokenRefKind::kHts: case networks::TokenRefKind::kXrpl: case networks::TokenRefKind::kAptos:
    case networks::TokenRefKind::kSui: case networks::TokenRefKind::kDenom: return true;
    default: return false;
  }
}

// searchCoinGecko's item construction over the per-coin details.
bool build_coingecko(TokenSearchState* state, Job& job, Item* items, size_t* count) {
  DiscoveryWorkspace* workspace = state->route->workspace;
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  *count = 0;
  static networks::TokenRef ref;
  for (size_t h = 0; h < job.hit_count; ++h) {
    if (job.details[h] != Detail::kReady) continue;
    char key[kKeyBytes];
    snprintf(key, sizeof(key), "cgdetail:%s", job.hits[h]);
    const DiscoveryCacheEntry entry = cache->Read(key, now);
    if (entry.state != DiscoveryCacheState::kFresh && entry.state != DiscoveryCacheState::kStale) {
      job.partial = true;  // stored a moment ago; unreadable now
      continue;
    }
    codec::BoundedJsonDocument document;
    codec::BoundedJsonError error{};
    if (!document.ParseWithLimits(entry.body, entry.body_length, workspace->tokens,
                                  kDiscoveryTokens, kDiscoveryTokens,
                                  codec::kBoundedJsonMaxExpandedInputBytes, &error)) {
      job.partial = true;
      continue;
    }
    const uint32_t root = document.root();
    if (document.token(root)->type != codec::BoundedJsonType::kObject) continue;  // null detail
    // The logo is registered for every detail, kept or not.
    const char* logo_key = nullptr;
    char logo[fortis::discovery::market_view::kMediaKeyChars + 1];
    const uint32_t image = document.FindObjectValue(root, "image");
    if (image != codec::kBoundedJsonNoToken && document.token(image)->type == codec::BoundedJsonType::kObject) {
      const uint32_t small = document.FindObjectValue(image, "small");
      const char* url = nullptr; size_t url_length = 0;
      if (arena_string(state, document, small, &url, &url_length))
        logo_key = register_logo(workspace, url, url_length, logo);
    }
    const uint32_t platforms = document.FindObjectValue(root, "platforms");
    if (platforms == codec::kBoundedJsonNoToken ||
        document.token(platforms)->type != codec::BoundedJsonType::kObject) continue;
    const char* raw_address = nullptr; size_t raw_length = 0;
    if (!arena_string(state, document, document.FindObjectValue(platforms, job.platform),
                      &raw_address, &raw_length)) continue;
    size_t begin = 0, end = 0;
    hb::JsTrim(raw_address, raw_length, &begin, &end);
    if (begin == end) continue;
    const uint32_t detail_platforms = document.FindObjectValue(root, "detail_platforms");
    if (detail_platforms == codec::kBoundedJsonNoToken ||
        document.token(detail_platforms)->type != codec::BoundedJsonType::kObject) continue;
    const uint32_t selected = document.FindObjectValue(detail_platforms, job.platform);
    if (selected == codec::kBoundedJsonNoToken ||
        document.token(selected)->type != codec::BoundedJsonType::kObject) continue;
    const uint32_t places = document.FindObjectValue(selected, "decimal_place");
    double decimals = 0;
    if (places == codec::kBoundedJsonNoToken || document.token(places)->type != codec::BoundedJsonType::kNumber ||
        !document.Number(places, &decimals) || floor(decimals) != decimals || decimals < 0 ||
        decimals > 255) continue;
    if (!ParseTokenRef(&job.network, raw_address + begin, end - begin, &ref) ||
        !family_token_kind(ref.kind)) continue;
    if (*count >= sizeof(state->items) / sizeof(state->items[0])) break;
    Item& item = items[(*count)++];
    item = Item{};
    item.network = job.network.id; item.network_length = strlen(job.network.id);
    item.token_id = arena_copy(state, ref.token_id, strlen(ref.token_id));
    item.token_id_length = strlen(ref.token_id);
    item.address = raw_address + begin; item.address_length = end - begin;
    const char* symbol = ""; size_t symbol_length = 0;
    arena_string(state, document, document.FindObjectValue(root, "symbol"), &symbol, &symbol_length);
    const char* name = ""; size_t name_length = 0;
    arena_string(state, document, document.FindObjectValue(root, "name"), &name, &name_length);
    if (!item.token_id || !sanitized(state, symbol, symbol_length, true, &item.symbol,
                                     &item.symbol_length) ||
        !sanitized(state, name, name_length, false, &item.name, &item.name_length)) return false;
    item.decimals = decimals;
    item.verified = true;
    set_logo(&item, logo_key);
  }
  return true;
}

// searchJupiter: a stable sort (verified first, then holders descending),
// the first 20 rows, logo registration, then the rows with an id and
// integer decimals. False is a TypeError in TypeScript (a null row).
bool build_jupiter(TokenSearchState* state, const codec::BoundedJsonDocument& document, Item* items,
                   size_t* count) {
  DiscoveryWorkspace* workspace = state->route->workspace;
  *count = 0;
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (root->type != codec::BoundedJsonType::kArray) return true;  // `Array.isArray(body) ? body : []`
  JupiterRow* rows = state->rows;
  size_t n = 0;
  for (uint32_t child = root->first_child; child != codec::kBoundedJsonNoToken;
       child = document.token(child)->next_sibling) {
    const codec::BoundedJsonType type = document.token(child)->type;
    if (type == codec::BoundedJsonType::kNull) return false;
    JupiterRow row{child, false, 0};
    if (type == codec::BoundedJsonType::kObject) {
      row.verified = is_true(document, child, "isVerified");
      row.holders = num(document, child, "holderCount");
    }
    rows[n++] = row;
  }
  // Stable insertion sort by the TypeScript comparator.
  for (size_t i = 1; i < n; ++i) {
    const JupiterRow moving = rows[i];
    size_t j = i;
    while (j > 0) {
      const JupiterRow& a = rows[j - 1];
      double order = static_cast<double>(moving.verified) - static_cast<double>(a.verified);
      if (order == 0) order = moving.holders - a.holders;
      if (!(order > 0)) break;  // comparator(a, moving) <= 0 keeps a first
      rows[j] = rows[j - 1];
      --j;
    }
    rows[j] = moving;
  }
  const size_t limit = n < kMaxItems ? n : kMaxItems;
  for (size_t i = 0; i < limit; ++i) {
    const uint32_t row = rows[i].index;
    if (document.token(row)->type != codec::BoundedJsonType::kObject) continue;
    const char* logo_key = nullptr;
    char logo[fortis::discovery::market_view::kMediaKeyChars + 1];
    const char* icon = nullptr; size_t icon_length = 0;
    if (arena_string(state, document, document.FindObjectValue(row, "icon"), &icon, &icon_length))
      logo_key = register_logo(workspace, icon, icon_length, logo);
    const char* raw = nullptr; size_t raw_length = 0;
    if (!arena_string(state, document, document.FindObjectValue(row, "id"), &raw, &raw_length))
      continue;
    size_t begin = 0, end = 0;
    hb::JsTrim(raw, raw_length, &begin, &end);
    const uint32_t places = document.FindObjectValue(row, "decimals");
    double decimals = 0;
    if (begin == end || places == codec::kBoundedJsonNoToken ||
        document.token(places)->type != codec::BoundedJsonType::kNumber ||
        !document.Number(places, &decimals) || floor(decimals) != decimals) continue;
    Item& item = items[(*count)++];
    item = Item{};
    item.network = "solana";  // replaced by the caller with the network id
    item.address = raw + begin; item.address_length = end - begin;
    char token_id[2048];
    const int written = snprintf(token_id, sizeof(token_id), "spl:%.*s",
                                 static_cast<int>(item.address_length), item.address);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(token_id)) return false;
    item.token_id = arena_copy(state, token_id, static_cast<size_t>(written));
    item.token_id_length = static_cast<size_t>(written);
    const char* symbol = ""; size_t symbol_length = 0;
    arena_string(state, document, document.FindObjectValue(row, "symbol"), &symbol, &symbol_length);
    const char* name = ""; size_t name_length = 0;
    arena_string(state, document, document.FindObjectValue(row, "name"), &name, &name_length);
    if (!item.token_id || !sanitized(state, symbol, symbol_length, true, &item.symbol,
                                     &item.symbol_length) ||
        !sanitized(state, name, name_length, false, &item.name, &item.name_length)) return false;
    item.decimals = decimals;
    item.verified = is_true(document, row, "isVerified");
    item.has_holders = true;
    item.holders = num(document, row, "holderCount");
    set_logo(&item, logo_key);
  }
  return true;
}

// ── Robinhood catalog (the TypeScript parseAssets) ──

// The catalog order of ticker symbols: byte order after ASCII lower-casing,
// a prefix first; symbols equal under that order are ordered by their own
// bytes, so the order is total and deterministic.
int symbol_compare(const char* a, size_t a_length, const char* b, size_t b_length) {
  const size_t common = a_length < b_length ? a_length : b_length;
  auto fold = [](char c) {
    return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c | 0x20 : c);
  };
  for (size_t i = 0; i < common; ++i) {
    const unsigned char x = fold(a[i]), y = fold(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  if (a_length != b_length) return a_length < b_length ? -1 : 1;
  const int order = common ? memcmp(a, b, common) : 0;
  return order < 0 ? -1 : order > 0 ? 1 : 0;
}

// text(value, max): a string, trimmed, non-empty and at most `max` units.
bool catalog_text(Catalog& catalog, const codec::BoundedJsonDocument& document, uint32_t object,
                  const char* name, size_t max, uint32_t* offset, uint32_t* length) {
  const uint32_t index = document.FindObjectValue(object, name);
  if (index == codec::kBoundedJsonNoToken || document.token(index)->type != codec::BoundedJsonType::kString)
    return false;
  char* out = catalog.arena + catalog.used;
  size_t decoded = 0;
  if (catalog.used >= sizeof(catalog.arena) ||
      !document.DecodeString(index, out, sizeof(catalog.arena) - catalog.used, &decoded))
    return false;
  size_t begin = 0, end = 0;
  hb::JsTrim(out, decoded, &begin, &end);
  if (begin == end || hb::Utf16Units(out + begin, end - begin) > max) return false;
  memmove(out, out + begin, end - begin);
  out[end - begin] = '\0';
  *offset = static_cast<uint32_t>(catalog.used);
  *length = static_cast<uint32_t>(end - begin);
  catalog.used += end - begin + 1;
  return true;
}

bool hex_address(const char* text, size_t length, size_t digits) {
  if (length != digits + 2 || text[0] != '0' || text[1] != 'x') return false;
  for (size_t i = 2; i < length; ++i) if (hex(static_cast<unsigned char>(text[i])) < 0) return false;
  return true;
}

// decimal(): /^\d+(?:\.\d{1,18})?$/ after trim, and Number() > 0.
bool positive_decimal(const char* text, size_t length) {
  size_t begin = 0, end = 0;
  hb::JsTrim(text, length, &begin, &end);
  size_t i = begin, digits = 0, fraction = 0;
  bool nonzero = false;
  while (i < end && text[i] >= '0' && text[i] <= '9') { nonzero |= text[i] != '0'; ++i; ++digits; }
  if (!digits) return false;
  if (i < end && text[i] == '.') {
    ++i;
    while (i < end && text[i] >= '0' && text[i] <= '9') { nonzero |= text[i] != '0'; ++i; ++fraction; }
    if (!fraction || fraction > 18) return false;
  }
  return i == end && nonzero;
}

// httpsUrl(): https: with no credentials, as URL.prototype.toString writes it
// for the canonical spellings Robinhood serves (a native subset).
bool https_url(const char* text, size_t length) {
  if (!fortis::discovery::market_view::IsHttpUrl(text, length) || length < 8 ||
      strncmp(text, "https://", 8) != 0) return false;
  const char* authority_end = text + 8;
  while (authority_end < text + length && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#') ++authority_end;
  for (const char* c = text + 8; c < authority_end; ++c)
    if (*c == '@' || (*c >= 'A' && *c <= 'Z') || *c == '\\') return false;
  for (const char* c = text; c < text + length; ++c)
    if (static_cast<unsigned char>(*c) <= 0x20 || static_cast<unsigned char>(*c) >= 0x7f ||
        *c == '\\') return false;
  return authority_end < text + length;  // a path (even "/") is serialized
}

// Number(value) === 4663 for a chainId.
bool chain_matches(const codec::BoundedJsonDocument& document, uint32_t index) {
  if (index == codec::kBoundedJsonNoToken) return false;
  const codec::BoundedJsonToken* token = document.token(index);
  double value = 0;
  if (token->type == codec::BoundedJsonType::kNumber) return document.Number(index, &value) && value == 4663;
  if (token->type != codec::BoundedJsonType::kString) return false;
  static char text[256], scratch[260];
  size_t length = 0;
  if (!document.DecodeString(index, text, sizeof(text), &length)) return false;
  return hb::JsStringToNumber(text, length, scratch, sizeof(scratch)) == 4663;
}

bool parse_catalog(Catalog& catalog, const codec::BoundedJsonDocument& document) {
  catalog.count = 0;
  catalog.used = 0;
  const uint32_t root = document.root();
  if (document.token(root)->type != codec::BoundedJsonType::kObject) return false;
  const uint32_t assets = document.FindObjectValue(root, "assets");
  if (assets == codec::kBoundedJsonNoToken || document.token(assets)->type != codec::BoundedJsonType::kArray)
    return false;
  for (uint32_t raw = document.token(assets)->first_child; raw != codec::kBoundedJsonNoToken;
       raw = document.token(raw)->next_sibling) {
    if (document.token(raw)->type != codec::BoundedJsonType::kObject) continue;
    const size_t mark = catalog.used;
    CatalogAsset asset{};
    uint32_t id = 0, id_length = 0, multiplier = 0, multiplier_length = 0;
    const bool ok = catalog_text(catalog, document, raw, "id", 68, &id, &id_length) &&
        hex_address(catalog.arena + id, id_length, 64) &&
        catalog_text(catalog, document, raw, "tokenSymbol", 32, &asset.symbol, &asset.symbol_length) &&
        catalog_text(catalog, document, raw, "tokenName", 160, &asset.name, &asset.name_length);
    const uint32_t current = document.FindObjectValue(raw, "currentMultiplier");
    static char decimal[512];
    size_t decimal_length = 0;
    const bool multiplier_ok = current != codec::kBoundedJsonNoToken &&
        document.token(current)->type == codec::BoundedJsonType::kString &&
        document.DecodeString(current, decimal, sizeof(decimal), &decimal_length) &&
        positive_decimal(decimal, decimal_length);
    static_cast<void>(multiplier); static_cast<void>(multiplier_length);
    const uint32_t deployments = document.FindObjectValue(raw, "deployments");
    bool deployed = false;
    if (ok && multiplier_ok && deployments != codec::kBoundedJsonNoToken &&
        document.token(deployments)->type == codec::BoundedJsonType::kArray) {
      for (uint32_t value = document.token(deployments)->first_child;
           value != codec::kBoundedJsonNoToken && !deployed; value = document.token(value)->next_sibling) {
        if (document.token(value)->type != codec::BoundedJsonType::kObject ||
            !chain_matches(document, document.FindObjectValue(value, "chainId"))) continue;
        const uint32_t contract = document.FindObjectValue(value, "contractAddress");
        size_t length = 0;
        if (contract == codec::kBoundedJsonNoToken ||
            document.token(contract)->type != codec::BoundedJsonType::kString ||
            !document.DecodeString(contract, asset.address, sizeof(asset.address), &length) ||
            !hex_address(asset.address, length, 40)) continue;
        deployed = true;
      }
    }
    if (!ok || !multiplier_ok || !deployed) { catalog.used = mark; continue; }
    // `seen.add()` returns the Set (truthy): TypeScript never drops a
    // repeated contract here; the search dedupes its answer instead.
    const uint32_t logo = document.FindObjectValue(raw, "logoUrl");
    if (logo != codec::kBoundedJsonNoToken && document.token(logo)->type == codec::BoundedJsonType::kString) {
      char* out = catalog.arena + catalog.used;
      size_t length = 0;
      if (catalog.used < sizeof(catalog.arena) &&
          document.DecodeString(logo, out, sizeof(catalog.arena) - catalog.used, &length) &&
          https_url(out, length)) {
        asset.logo = static_cast<uint32_t>(catalog.used);
        asset.logo_length = static_cast<uint32_t>(length);
        asset.has_logo = true;
        catalog.used += length + 1;
      }
    }
    const uint32_t status = document.FindObjectValue(raw, "status");
    asset.active = status != codec::kBoundedJsonNoToken &&
                   document.StringEquals(status, "ASSET_STATUS_ACTIVE");
    if (catalog.count == kCatalogAssets) return false;  // native bound
    catalog.assets[catalog.count++] = asset;
  }
  // Stable sort by ticker symbol (symbol_compare).
  for (size_t i = 1; i < catalog.count; ++i) {
    const CatalogAsset moving = catalog.assets[i];
    size_t j = i;
    while (j > 0 && symbol_compare(catalog.arena + catalog.assets[j - 1].symbol,
                                   catalog.assets[j - 1].symbol_length,
                                   catalog.arena + moving.symbol, moving.symbol_length) > 0) {
      catalog.assets[j] = catalog.assets[j - 1];
      --j;
    }
    catalog.assets[j] = moving;
  }
  return true;
}

bool contains(const char* text, size_t length, const char* needle, size_t needle_length) {
  if (!needle_length) return true;
  for (size_t i = 0; i + needle_length <= length; ++i)
    if (memcmp(text + i, needle, needle_length) == 0) return true;
  return false;
}

bool build_robinhood(TokenSearchState* state, const Job& job, Item* items, size_t* count) {
  Catalog& catalog = state->catalog;
  DiscoveryWorkspace* workspace = state->route->workspace;
  *count = 0;
  static char lowered[2048];
  for (size_t i = 0; i < catalog.count && *count < kMaxItems; ++i) {
    const CatalogAsset& asset = catalog.assets[i];
    if (!asset.active) continue;
    size_t length = 0;
    bool match = fortis::compat::JsToLowerCase(catalog.arena + asset.symbol, asset.symbol_length, lowered,
                                       sizeof(lowered), &length) &&
                 contains(lowered, length, job.query, job.query_length);
    if (!match)
      match = fortis::compat::JsToLowerCase(catalog.arena + asset.name, asset.name_length, lowered,
                                    sizeof(lowered), &length) &&
              contains(lowered, length, job.query, job.query_length);
    if (!match) continue;
    Item& item = items[(*count)++];
    item = Item{};
    item.network = job.network.id; item.network_length = strlen(job.network.id);
    char token_id[64];
    snprintf(token_id, sizeof(token_id), "erc20:%s", asset.address);
    for (char* c = token_id + 6; *c; ++c) if (*c >= 'A' && *c <= 'F') *c = static_cast<char>(*c | 0x20);
    item.token_id = arena_copy(state, token_id, strlen(token_id));
    item.token_id_length = strlen(token_id);
    item.address = arena_copy(state, asset.address, strlen(asset.address));
    item.address_length = strlen(asset.address);
    if (!item.token_id || !item.address ||
        !sanitized(state, catalog.arena + asset.symbol, asset.symbol_length, true, &item.symbol,
                   &item.symbol_length) ||
        !sanitized(state, catalog.arena + asset.name, asset.name_length, false, &item.name,
                   &item.name_length)) return false;
    item.decimals = 18;
    item.verified = true;
    char logo[fortis::discovery::market_view::kMediaKeyChars + 1];
    set_logo(&item, asset.has_logo
        ? register_logo(workspace, catalog.arena + asset.logo, asset.logo_length, logo) : nullptr);
  }
  return true;
}

// ── Job lifecycle ──

void respond(TokenSearchState* state, Job& job, const http::HttpResponse& response) {
  job.done = true;
  for (size_t i = 0; i < job.waiter_count; ++i) {
    Waiter& waiter = job.waiters[i];
    fortis::base::UniqueLock refs_lock(state->refs_mutex);
    const bool cancelled = waiter.cancelled;
    refs_lock.Unlock();
    if (cancelled) continue;
    http::HttpResponse copy = response;
    copy.rate_limit_cost = waiter.rate_cost;
    copy.rate_limit_remaining = waiter.remaining;
    state->route->handoff->Complete(waiter.token, copy);
  }
}

// Answers every waiter with the live result (and stores it when complete).
void finish(TokenSearchState* state, Job& job, Item* items, size_t count) {
  job.done = true;
  DiscoveryWorkspace* workspace = state->route->workspace;
  DiscoveryCache* cache = workspace->cache;
  count = dedupe(items, count);
  size_t length = 0;
  if (!job.partial && write_record(state, items, count, &length))
    cache->Write(job.cache_key, kDiscoveryTtlSearch, cache->Now(), state->record, length, nullptr);
  // Responses differ by representation only.
  http::HttpResponse json{}, protobuf{};
  bool json_ready = false, protobuf_ready = false, protobuf_failed = false;
  for (size_t i = 0; i < job.waiter_count; ++i) {
    Waiter& waiter = job.waiters[i];
    fortis::base::UniqueLock refs_lock(state->refs_mutex);
    const bool cancelled = waiter.cancelled;
    refs_lock.Unlock();
    if (cancelled) continue;
    http::HttpResponse response{};
    if (waiter.protobuf) {
      if (!protobuf_ready && !protobuf_failed) {
        protobuf_failed = !encode_protobuf(workspace, items, count, false, -1, &protobuf);
        protobuf_ready = !protobuf_failed;
      }
      response = protobuf_failed ? internal_error() : protobuf;
    } else {
      if (!json_ready) json_ready = encode_json(workspace, items, count, false, -1, &json);
      response = json_ready ? json : internal_error();
    }
    response.rate_limit_cost = waiter.rate_cost;
    response.rate_limit_remaining = waiter.remaining;
    state->route->handoff->Complete(waiter.token, response);
  }
}

void fail(TokenSearchState* state, Job& job, const DiscoveryProviderResult& result) {
  DiscoveryCache* cache = state->route->workspace->cache;
  const int64_t now = cache->Now();
  job.done = true;
  if (job.background) {
    cache->RecordRevalidateFailure(job.cache_key, now, result.lane_rejected);
    return;
  }
  const uint32_t retry_ms = result.retry_after_ms ? result.retry_after_ms : 2000;
  const http::HttpResponse response = result.lane_rejected ? DiscoveryRateLimitedResponse(retry_ms)
                                                     : DiscoveryUpstreamUnavailableResponse();
  cache->RecordMissFailure(job.cache_key, now, result.lane_rejected,
      result.lane_rejected ? 429 + 1000 * static_cast<int>(response.retry_after) : 502);
  respond(state, job, response);
}

void provider_complete(void* opaque, const DiscoveryProviderResult& result,
                       const unsigned char* body, size_t length);

bool submit(TokenSearchState* state, Job& job, Call& call, DiscoveryUpstream upstream,
            const char* path) {
  retain(state, &job);
  ++job.outstanding;
  uint64_t request_id = 0;
  // HttpClient.json retries a 2xx it cannot parse (three attempts).
  if (!state->route->owner->Submit(upstream, path, DiscoveryMonotonicMs() + kDiscoveryOwnerDeadlineMs,
                                   false, provider_complete, &call, &request_id, true)) {
    --job.outstanding;
    release(state, &job);
    return false;
  }
  return true;
}

void build_and_finish(TokenSearchState* state, Job& job) {
  state->strings_used = 0;
  size_t count = 0;
  if (!build_coingecko(state, job, state->items, &count)) {
    respond(state, job, internal_error());
    return;
  }
  finish(state, job, state->items, count);
}

void detail_refresh_complete(void* opaque, const DiscoveryProviderResult& result,
                             const unsigned char* body, size_t length);

// Stores one fetched coin detail as the subset search reads; a 404 stores
// null. False for any other failure.
bool store_detail(DiscoveryWorkspace* workspace, const char* key,
                  const DiscoveryProviderResult& result, const unsigned char* body, size_t length) {
  DiscoveryCache* cache = workspace->cache;
  if (result.transport_error || result.cancelled) return false;
  if (result.http_status == 404)
    return cache->Write(key, kDiscoveryTtlTokenIdentity, cache->Now(), "null", 4, nullptr) || true;
  if (result.http_status < 200 || result.http_status >= 300) return false;
  if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) { body += 3; length -= 3; }
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error{};
  if (!length || !document.ParseWithLimits(body, length, workspace->tokens, kDiscoveryTokens,
                                           kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes,
                                           &error)) return false;
  OutBuffer out{workspace->payload, sizeof(workspace->payload)};
  const uint32_t root = document.root();
  const codec::BoundedJsonType type = document.token(root)->type;
  if (type == codec::BoundedJsonType::kNull) out.Text("null");
  else if (type != codec::BoundedJsonType::kObject) out.Text("{}");
  else {
    static const char* const kFields[] = {"symbol", "name", "image", "platforms", "detail_platforms"};
    out.Byte('{');
    bool first = true;
    for (const char* name : kFields) {
      const uint32_t value = document.FindObjectValue(root, name);
      if (value == codec::kBoundedJsonNoToken) continue;
      if (!first) out.Byte(',');
      first = false;
      out.Byte('"'); out.Text(name); out.Text("\":");
      if (!fortis::compat::js_json::Stringify(document, value, out, DiscoveryScratch(workspace))) return false;
    }
    out.Byte('}');
  }
  if (!out.ok) return false;
  // A failed write is logged by TypeScript; the read path then sees a miss.
  cache->Write(key, kDiscoveryTtlTokenIdentity, cache->Now(), workspace->payload, out.used, nullptr);
  return true;
}

void detail_path(const char* id, size_t length, char* path, size_t capacity) {
  OutBuffer out{reinterpret_cast<uint8_t*>(path), capacity - 1};
  out.Text("/api/v3/coins/");
  append_component(out, id, length);
  out.Text(kDetailQuery);
  path[out.ok ? out.used : 0] = '\0';
}

// The CoinGecko search reply: the first five rows with string id, symbol
// and name. False is a TypeScript failure (TypeError or non-JSON).
bool read_hits(DiscoveryWorkspace* workspace, Job& job, const unsigned char* body, size_t length) {
  if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) { body += 3; length -= 3; }
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error{};
  if (!length || !document.ParseWithLimits(body, length, workspace->tokens, kDiscoveryTokens,
                                           kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes,
                                           &error)) return false;
  job.hit_count = 0;
  const codec::BoundedJsonToken* root = document.token(document.root());
  if (root->type == codec::BoundedJsonType::kNull) return false;  // null.coins
  if (root->type != codec::BoundedJsonType::kObject) return true;  // undefined → []
  const uint32_t coins = document.FindObjectValue(document.root(), "coins");
  if (coins == codec::kBoundedJsonNoToken || document.token(coins)->type == codec::BoundedJsonType::kNull)
    return true;  // `body.coins ?? []`
  if (document.token(coins)->type != codec::BoundedJsonType::kArray) return false;  // no flatMap
  for (uint32_t row = document.token(coins)->first_child; row != codec::kBoundedJsonNoToken;
       row = document.token(row)->next_sibling) {
    const codec::BoundedJsonType type = document.token(row)->type;
    if (type == codec::BoundedJsonType::kNull) return false;  // null.id
    if (type != codec::BoundedJsonType::kObject) continue;
    const uint32_t id = document.FindObjectValue(row, "id");
    const uint32_t symbol = document.FindObjectValue(row, "symbol");
    const uint32_t name = document.FindObjectValue(row, "name");
    auto string = [&](uint32_t index) {
      return index != codec::kBoundedJsonNoToken && document.token(index)->type == codec::BoundedJsonType::kString;
    };
    if (!string(id) || !string(symbol) || !string(name)) continue;
    if (job.hit_count == kHits) continue;  // slice(0, 5), but every row is checked
    size_t decoded = 0;
    if (!document.DecodeString(id, job.hits[job.hit_count], kCoinIdBytes, &decoded)) return false;
    job.hit_lengths[job.hit_count++] = decoded;
  }
  return true;
}

void start_details(TokenSearchState* state, Job& job) {
  DiscoveryWorkspace* workspace = state->route->workspace;
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  for (size_t h = 0; h < job.hit_count; ++h) {
    char key[kKeyBytes];
    snprintf(key, sizeof(key), "cgdetail:%s", job.hits[h]);
    const DiscoveryCacheEntry entry = cache->Read(key, now);
    if (entry.state == DiscoveryCacheState::kFresh || entry.state == DiscoveryCacheState::kStale) {
      job.details[h] = Detail::kReady;
      if (entry.state == DiscoveryCacheState::kStale && cache->AllowRevalidate(key, now)) {
        DetailRefresh* refresh = nullptr;
        for (DetailRefresh& candidate : state->refreshes) {
          if (candidate.used && strcmp(candidate.key, key) == 0) { refresh = nullptr; break; }
          if (!candidate.used && !refresh) refresh = &candidate;
        }
        if (refresh) {
          refresh->used = true;
          snprintf(refresh->key, sizeof(refresh->key), "%s", key);
          detail_path(job.hits[h], job.hit_lengths[h], refresh->path, sizeof(refresh->path));
          uint64_t id = 0;
          if (!state->route->owner->Submit(DiscoveryUpstream::kCoinGecko, refresh->path,
                  DiscoveryMonotonicMs() + kDiscoveryOwnerDeadlineMs, false,
                  detail_refresh_complete, refresh, &id, true)) refresh->used = false;
        }
      }
      continue;
    }
    int replay = 0;
    if (cache->MissReplay(key, now, &replay)) { job.details[h] = Detail::kFailed; job.partial = true; continue; }
    static char path[kDiscoveryMaxPathBytes];
    detail_path(job.hits[h], job.hit_lengths[h], path, sizeof(path));
    Call& call = job.calls[1 + h];
    call.job = &job; call.kind = CallKind::kDetail; call.hit = static_cast<uint8_t>(h);
    job.details[h] = Detail::kPending;
    if (!submit(state, job, call, DiscoveryUpstream::kCoinGecko, path)) {
      job.details[h] = Detail::kFailed;
      job.partial = true;
    }
  }
}

void provider_complete(void* opaque, const DiscoveryProviderResult& result,
                       const unsigned char* body, size_t length) {
  TokenSearchState* state = g_search_state;
  auto* call = static_cast<Call*>(opaque);
  Job& job = *call->job;
  DiscoveryWorkspace* workspace = state->route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  --job.outstanding;
  const bool ok = !result.transport_error && !result.cancelled &&
                  result.http_status >= 200 && result.http_status < 300;
  switch (call->kind) {
    case CallKind::kSearch:
      if (!ok || !read_hits(workspace, job, body, length)) {
        fail(state, job, result);
        break;
      }
      start_details(state, job);
      if (!job.outstanding) build_and_finish(state, job);
      break;
    case CallKind::kDetail: {
      char key[kKeyBytes];
      snprintf(key, sizeof(key), "cgdetail:%s", job.hits[call->hit]);
      if (store_detail(workspace, key, result, body, length)) {
        job.details[call->hit] = Detail::kReady;
      } else {
        workspace->cache->RecordMissFailure(key, workspace->cache->Now(), result.lane_rejected, 502);
        job.details[call->hit] = Detail::kFailed;
        job.partial = true;
      }
      if (!job.outstanding) build_and_finish(state, job);
      break;
    }
    case CallKind::kJupiter: {
      if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) { body += 3; length -= 3; }
      codec::BoundedJsonDocument document;
      codec::BoundedJsonError error{};
      size_t count = 0;
      state->strings_used = 0;
      if (!ok || !length ||
          !document.ParseWithLimits(body, length, workspace->tokens, kDiscoveryTokens,
                                    kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes, &error) ||
          !build_jupiter(state, document, state->items, &count)) {
        fail(state, job, result);
        break;
      }
      for (size_t i = 0; i < count; ++i) {
        state->items[i].network = job.network.id;
        state->items[i].network_length = strlen(job.network.id);
      }
      finish(state, job, state->items, count);
      break;
    }
    case CallKind::kRobinhood: {
      if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) { body += 3; length -= 3; }
      codec::BoundedJsonDocument document;
      codec::BoundedJsonError error{};
      const bool parsed = ok && length &&
          document.ParseWithLimits(body, length, workspace->tokens, kDiscoveryTokens,
                                   kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes, &error) &&
          parse_catalog(state->catalog, document);
      if (!parsed) {
        state->catalog.valid = false;
        fail(state, job, result);
        break;
      }
      state->catalog.valid = true;
      state->catalog.until = workspace->cache->Now() + kCatalogMs;
      size_t count = 0;
      state->strings_used = 0;
      if (!build_robinhood(state, job, state->items, &count)) respond(state, job, internal_error());
      else finish(state, job, state->items, count);
      break;
    }
  }
  lock.Unlock();
  release(state, &job);  // the call's reference
}

void detail_refresh_complete(void* opaque, const DiscoveryProviderResult& result,
                             const unsigned char* body, size_t length) {
  TokenSearchState* state = g_search_state;
  auto* refresh = static_cast<DetailRefresh*>(opaque);
  DiscoveryWorkspace* workspace = state->route->workspace;
  fortis::base::LockGuard lock(workspace->mutex);
  if (!store_detail(workspace, refresh->key, result, body, length))
    workspace->cache->RecordRevalidateFailure(refresh->key, workspace->cache->Now(),
                                              result.lane_rejected);
  refresh->used = false;
}

// Starts the fetcher for `job`. Caller holds the workspace mutex and a job
// reference. Synchronous answers (no platform, a cached catalog) finish here.
void start(TokenSearchState* state, Job& job) {
  DiscoveryWorkspace* workspace = state->route->workspace;
  static char path[kDiscoveryMaxPathBytes];
  OutBuffer out{reinterpret_cast<uint8_t*>(path), sizeof(path) - 1};
  Call& call = job.calls[0];
  call.job = &job;
  if (strcmp(job.network.id, "robinhood") == 0) {
    job.source = Source::kRobinhood;
    if (state->catalog.valid && state->catalog.until > workspace->cache->Now()) {
      size_t count = 0;
      state->strings_used = 0;
      if (!build_robinhood(state, job, state->items, &count)) respond(state, job, internal_error());
      else finish(state, job, state->items, count);
      return;
    }
    call.kind = CallKind::kRobinhood;
    out.Text("/rhj/assets");
    path[out.used] = '\0';
    if (!submit(state, job, call, DiscoveryUpstream::kRobinhood, path))
      fail(state, job, DiscoveryProviderResult{});
    return;
  }
  if (strcmp(job.network.family, "solana") == 0) {
    job.source = Source::kJupiter;
    call.kind = CallKind::kJupiter;
    out.Text("/tokens/v2/search?query=");
    append_component(out, job.query, job.query_length);
    path[out.used] = '\0';
    if (!out.ok || !submit(state, job, call, DiscoveryUpstream::kJupiter, path))
      fail(state, job, DiscoveryProviderResult{});
    return;
  }
  job.source = Source::kCoinGecko;
  job.platform = providers::CoinGeckoPlatformForNetwork(job.network.id);
  if (!job.platform) {
    finish(state, job, state->items, 0);
    return;
  }
  call.kind = CallKind::kSearch;
  out.Text("/api/v3/search?query=");
  append_component(out, job.query, job.query_length);
  path[out.used] = '\0';
  if (!out.ok || !submit(state, job, call, DiscoveryUpstream::kCoinGecko, path))
    fail(state, job, DiscoveryProviderResult{});
}

Job* acquire_job(TokenSearchState* state) {
  fortis::base::LockGuard refs_lock(state->refs_mutex);
  Job* found = nullptr;
  for (Job& job : state->jobs) if (!job.used) { found = &job; break; }
  if (found) {
    found->used = true; found->references = 1; found->background = false; found->partial = false;
    found->done = false;
    found->outstanding = 0; found->waiter_count = 0; found->hit_count = 0;
    for (Detail& detail : found->details) detail = Detail::kUnread;
  }
  return found;
}

enum class TermStatus : uint8_t { kOk, kInvalid, kMemory };

// searchQuery's q over the raw target (fast-querystring, JS trim, 1..100
// UTF-16 units; absent, repeated or out of bounds is kInvalid, the bare
// 400), then the facet's term: trim().toLowerCase() into `term` (100 units
// lower-case to at most 300 bytes).
TermStatus parse_term(const char* target, char* term, size_t capacity, size_t* term_length) {
  *term_length = 0;
  const auto query = http::query_string::Query::FromTarget(target);
  http::query_string::Component value;
  if (!query.Valid() || !query.First("q", &value)) return TermStatus::kInvalid;
  auto memory_storage = base::AllocateStorage<char[]>(value.size + 1);
  auto* memory = memory_storage.get();
  if (!memory) return TermStatus::kMemory;
  const size_t raw_length = value.Decode(memory, value.size);
  bool valid = raw_length != SIZE_MAX;
  size_t begin = 0, end = 0;
  if (valid) {
    hb::JsTrim(memory, raw_length, &begin, &end);
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    const size_t units = hb::Utf16Units(memory + begin, end - begin);
    valid = units >= 1 && units <= kMaxQueryUnits &&
            // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
            // cppcheck-suppress arithOperationsOnVoidPointer
            fortis::compat::JsToLowerCase(memory + begin, end - begin, term, capacity, term_length);
  }
  memory_storage.reset();
  return valid ? TermStatus::kOk : TermStatus::kInvalid;
}

// GET /v1/networks/{network}/tokens/search — R25, token discovery search
// (TypeScript: searchTokens → searchQuery
// {q: z.string().trim().min(1).max(100)} and searchParams {network: 1..64}
// → runFacet('token search', TokenDiscoveryFacet.search) →
// sendProtobufOrJson(TokenSearchResponse)).
// The app's caller is IntelligenceApi.searchTokens: an encodeComponent'd
// network, `q` the typed term, a protobuf read; it keeps items with an
// address and decimals and caches answers per term on the device.
//
// Request. GET, plus HEAD (Fastify's exposed HEAD route: the search still
// runs, providers included; same status and headers, no body). Other
// methods, a trailing '/', '/TOKENS/', an extra segment and ';' are the
// router's 404, and so are an empty {network}, one holding a decoded control
// character/DEL and one over 512 bytes. {network} reaches the handler
// percent-decoded. In order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"}, before any hook or quota;
//   2. {network} over 100 UTF-16 units once decoded: 414
//      {"error":"URI_TOO_LONG"}, no quota;
//   3. the /v1 device policy: any Origin 403 BROWSER_ORIGIN_FORBIDDEN, the
//      bearer 401 UNAUTHORIZED (ATTESTATION_REQUIRED for an unattested device
//      under ATTESTATION_MODE=required), then the device's per-minute quota at
//      cost 10 (requestCost's '/tokens/search' rule; 429 RATE_LIMITED,
//      retry-after). Every later answer carries x-rate-limit-cost: 10;
//   4. both schemas, any issue the bare 400 {"error":"BAD_REQUEST"}: q as
//      fast-querystring decodes it ('+' a space; escapes decoded unless one is
//      malformed or the result is not UTF-8, then the value is kept as written;
//      keys compared decoded; a repeated q is an array → 400; other keys
//      ignored; buffers as long as the target, so a padded q or a long
//      unrelated key parses as in Node), JS-trimmed (tab, LF, VT, FF, CR,
//      space, NBSP, U+2028/9, BOM…) to 1..100 UTF-16 units; network 1..64
//      units as decoded;
//   5. registry.network(network): exact id, alias (eip155:1, eip155:0001) or
//      a generic EVM network for any other eip155:<digits>; else the facet's
//      RangeError 400 {"error":"BAD_REQUEST","detail":"unknown network: <as
//      decoded>"} (JSON.stringify'd: a NUL is \u0000, DEL raw).
// The term is q.trim().toLowerCase() (String.prototype.toLowerCase: U+212A
// becomes "k", U+0130 "i̇", ẞ stays) and the answer is cached under
// keys.tokenSearch(network.id, term) — aliases share the canonical entry.
//
// Search by network (each a fresh read only on a cache miss):
//  - robinhood: the stock-token catalog (GET api.robinhood.com/rhj/assets,
//    kept 15 min in process), active assets whose lower-cased symbol or name
//    contains the term, first 20 in the catalog's symbol order; tokenId
//    erc20:<lower-cased address>, decimals 18, verified, no holder count;
//  - the solana family: Jupiter GET /tokens/v2/search?query=<term,
//    encodeURIComponent> (lite-api.jup.ag; api.jup.ag with x-api-key when
//    JUPITER_API_KEY is set), a non-array body is no rows; a stable sort
//    (verified first, then holderCount descending), the first 20 rows'
//    icons registered, then rows with a trimmed id and integer decimals;
//    holderCount a finite number or null;
//  - any other network: its CoinGecko platform (none → no items, stored);
//    GET /api/v3/search?query=<term>, the first five coins with string id,
//    symbol and name, each coin's detail through `cgdetail:<id>` (6 h fresh,
//    7 d stale, a 404 cached as null), image.small registered; an item needs
//    the platform's trimmed address, integer decimals 0..255 from
//    detail_platforms and a family token ref (parseTokenRef) on this network.
//    A detail that fails is omitted and the answer is not stored (partial).
// Then dedupe by `${network}:${address.toLowerCase()}` (first wins) and the
// first 20; symbol/name through sanitizeSymbol (16) / sanitizeName (64).
//
// Response (200), JSON.stringify order: {items: [{network, tokenId, address,
// symbol, name, decimals, verified, holderCount, logoMediaKey}], cache: {hit,
// timestamp}} — a live read {hit: false, timestamp: null}, a cached one
// {hit: true, timestamp: asOf}; no `partial` member. Protobuf
// TokenSearchResponse for an Accept naming application/protobuf
// (case-insensitive, any list position) with q > 0: proto3 strings ("" not
// written), decimals an optional uint32, holderCount an optional uint64
// (absent for null), logoMediaKey optional, cache CacheInfo {hit, timestamp?,
// partial: false}. A value protobuf-es refuses (a negative decimals, a
// fractional, negative or >= 2^64 holderCount) answers 500 {"error":
// "INTERNAL"} — only in protobuf; JSON serves it. `vary: Accept` on every
// answer the facet completed (200 and that 500, JSON included), never on a
// refusal; cache-control is the writer's no-store; there is no ETag and no
// conditional response (If-None-Match is ignored).
//
// Errors: 400/401/403/414/429 above; a provider failure (a non-2xx after
// HttpClient's retries, an unparseable or non-object body, a TypeError such
// as a null row) 502 UPSTREAM_UNAVAILABLE, held 10 s per key (a lane-busy
// failure 5 s, replayed as 429 RATE_LIMITED); a stale entry whose
// revalidation fails keeps serving, with a 30 s backoff (5 s when busy).
//
// Cache, durable effects and providers: the `search:` and `cgdetail:`
// envelopes (5 min fresh + 1 h stale; 6 h + 7 d) live in the discovery KV,
// the logos' media records (`media/item/<key>`, status pending, 7 d) in the
// same store as TypeScript's EmbeddedStore. Provider requests carry
// `accept: application/json` and only their own key; no client header or
// device credential reaches a provider.
//
// Concurrency and bounds: a read of a key already being fetched joins that
// job (8 jobs, 8 waiters each; beyond them 503 UPSTREAM_UNAVAILABLE); a stale
// read starts at most one background job per key; the workspace mutex
// serializes the cache. The term is at most 300 bytes (100 units lower-cased),
// the KV key 1,024, the stored answer 96 KiB, a provider body 1 MiB / 32,768
// JSON tokens / depth 16 (beyond them a provider failure), the catalog 512
// assets / 256 KiB.
//
// Differences from TypeScript, each a named rule in
// tests/token_search_edges_test.py:
//  - parser_baseline / bad_url / router_404: router- and parser-wide (the
//    414 and raw-byte 400 carry the native security headers; a malformed
//    path escape is {"error":"BAD_REQUEST"}; the router's 404 body);
//  - parser_fragment: a raw '#' in the request line is refused by the shared
//    request parser (400 before any hook); Node searches "one#fragment";
//  - accept_media_trim: "application/protobuf ; q=1" is protobuf natively
//    (the router-wide AcceptsProtobuf trims the media type, as R04/R06/R34);
//  - restart_durable_swr: the `search:` and `cgdetail:` envelopes are
//    durable, so after a restart the answer is served from the first
//    process's write with no provider request (TypeScript's Kv is in
//    memory);
//  - detail_subset (state only): a `cgdetail:` envelope keeps the members
//    search reads (symbol, name, image, platforms, detail_platforms);
//  - the `search:` KV key writes a NUL of the term %00 and a '%' %25
//    (SearchCacheKey); distinct terms stay distinct, as TypeScript's keys.
void handle(int client, const http::HttpRequest& request, bool head, void* opaque) {
  if (reject_raw_non_ascii(client, request, head) || reject_long_network(client, request, head)) return;
  auto* route = static_cast<DiscoveryRouteContext*>(opaque);
  TokenSearchState* state = route->search;
  DiscoveryWorkspace* workspace = route->workspace;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, route->attestation_required);
  if (policy.status) {
    SendDiscoveryJson(client, policy.status, policy.reason,
        policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}", head,
        policy.rate_limit_cost, policy.rate_limit_remaining, policy.retry_after);
    return;
  }
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  // zod over both inputs (q trimmed 1..100 UTF-16 units, network 1..64),
  // then the facet's term.
  char query[kQueryBytes];
  size_t query_length = 0;
  char network_id[512];
  const size_t network_length = network_param(request, network_id, sizeof(network_id));
  const TermStatus term = parse_term(request.raw_target ? request.raw_target : "", query, sizeof(query),
                                     &query_length);
  if (term == TermStatus::kMemory) {
    SendDiscoveryJson(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  const size_t network_units =
      network_length == SIZE_MAX ? 0 : hb::Utf16Units(network_id, network_length);
  if (term != TermStatus::kOk || network_units < 1 || network_units > 64) {
    SendDiscoveryJson(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  // registry.network(id): no catalog id, alias or eip155:<digits> holds a
  // NUL, which the resolver's C string could not carry.
  networks::TokenNetwork network{};
  if (memchr(network_id, '\0', network_length) || !ResolveTokenNetwork(network_id, network_length, &network)) {
    send_unknown_network(client, network_id, network_length, head, cost, remaining);
    return;
  }
  char cache_key[kKeyBytes];
  size_t key_length = 0;
  if (!SearchCacheKey(network.id, query, query_length, cache_key, sizeof(cache_key), &key_length)) {
    SendDiscoveryJson(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  const bool protobuf = AcceptsProtobuf(request);

  fortis::base::UniqueLock lock(workspace->mutex);
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  const DiscoveryCacheEntry entry = cache->Read(cache_key, now);
  if (entry.state == DiscoveryCacheState::kFresh || entry.state == DiscoveryCacheState::kStale) {
    static Item items[kMaxItems];
    size_t count = 0;
    http::HttpResponse response{};
    if (read_record(entry.body, entry.body_length, items, &count)) {
      const bool encoded = protobuf
          ? encode_protobuf(workspace, items, count, true, entry.as_of_ms, &response)
          : encode_json(workspace, items, count, true, entry.as_of_ms, &response);
      if (!encoded) response = internal_error();
      response.rate_limit_cost = cost;
      response.rate_limit_remaining = remaining;
      WriteHttpResponse(client, response, head);
      if (entry.state == DiscoveryCacheState::kStale && cache->AllowRevalidate(cache_key, now)) {
        bool refreshing = false;
        for (const Job& job : state->jobs)
          refreshing = refreshing || (job.used && job.background && strcmp(job.cache_key, cache_key) == 0);
        Job* job = refreshing ? nullptr : acquire_job(state);
        if (job) {
          job->background = true;
          // ResolveTokenNetwork points generic ids into the structure itself.
          ResolveTokenNetwork(network_id, network_length, &job->network);
          memcpy(job->query, query, query_length + 1);
          job->query_length = query_length;
          memcpy(job->cache_key, cache_key, key_length + 1);
          start(state, *job);
          lock.Unlock();
          release(state, job);
          return;
        }
      }
      return;
    }
  }
  int replay = 0;
  if (cache->MissReplay(cache_key, now, &replay)) {
    lock.Unlock();
    http::HttpResponse response = replay % 1000 == 429
        ? DiscoveryRateLimitedResponse(static_cast<uint32_t>(replay / 1000) * 1000)
        : DiscoveryUpstreamUnavailableResponse();
    response.rate_limit_cost = cost;
    response.rate_limit_remaining = remaining;
    WriteHttpResponse(client, response, head);
    return;
  }
  // A search already in flight for this key answers this request too.
  Job* job = nullptr;
  for (Job& candidate : state->jobs)
    if (candidate.used && !candidate.background && !candidate.done &&
        candidate.waiter_count < kWaiters &&
        strcmp(candidate.cache_key, cache_key) == 0) { job = &candidate; retain(state, job); break; }
  const bool joined = job != nullptr;
  if (!job) job = acquire_job(state);
  if (!job) {
    lock.Unlock();
    SendDiscoveryJson(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}",
                      head, cost, remaining);
    return;
  }
  if (!joined) {
    ResolveTokenNetwork(network_id, network_length, &job->network);
    memcpy(job->query, query, query_length + 1);
    job->query_length = query_length;
    memcpy(job->cache_key, cache_key, key_length + 1);
  }
  Waiter& waiter = job->waiters[job->waiter_count];
  waiter = Waiter{};
  waiter.job = job; waiter.protobuf = protobuf; waiter.rate_cost = cost; waiter.remaining = remaining;
  retain(state, job);  // the handoff terminal
  if (!http::ReserveHttpResponse(client, head, kDiscoveryHandoffTimeoutMs, &waiter.token,
                           waiter_terminal, &waiter)) {
    release(state, job);
    lock.Unlock();
    release(state, job);
    SendDiscoveryJson(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}",
                      head, cost, remaining);
    return;
  }
  ++job->waiter_count;
  if (!joined) start(state, *job);
  lock.Unlock();
  release(state, job);  // the handler's reference
}

}  // namespace

bool SearchCacheKey(const char* network_id, const char* term, size_t term_length, char* out, size_t capacity,
                    size_t* length) {
  if (!network_id || (!term && term_length) || !out || !length || !capacity) return false;
  OutBuffer key{reinterpret_cast<uint8_t*>(out), capacity - 1};
  key.Text("search:");
  key.Text(network_id);
  key.Byte(':');
  for (size_t i = 0; i < term_length; ++i) {
    if (term[i] == '\0') key.Text("%00");
    else if (term[i] == '%') key.Text("%25");
    else key.Byte(static_cast<uint8_t>(term[i]));
  }
  if (!key.ok) return false;
  out[key.used] = '\0';
  *length = key.used;
  return true;
}

bool TokenSearchTerm(const char* target, char* term, size_t capacity, size_t* length) {
  return parse_term(target, term, capacity, length) == TermStatus::kOk;
}

bool TokenSearchRender(const unsigned char* record, size_t length, bool hit, int64_t as_of_ms, bool protobuf,
                       unsigned char* out, size_t capacity, size_t* used) {
  Item items[kMaxItems];
  size_t count = 0;
  *used = 0;
  if (!read_record(record, length, items, &count)) return false;
  OutBuffer buffer{out, capacity};
  const bool ok = protobuf ? render_protobuf(buffer, items, count, hit, as_of_ms)
                           : render_json(buffer, items, count, hit, as_of_ms);
  *used = buffer.used;
  return ok;
}

bool TokenSearchRecordJson(const unsigned char* record, size_t length, unsigned char* out, size_t capacity,
                           size_t* used) {
  Item items[kMaxItems];
  size_t count = 0;
  if (!read_record(record, length, items, &count)) return false;
  OutBuffer buffer{out, capacity};
  append_items_json(buffer, items, count);
  *used = buffer.used;
  return buffer.ok;
}

int RobinhoodSymbolCompare(const char* a, size_t a_length, const char* b, size_t b_length) {
  return symbol_compare(a, a_length, b, b_length);
}

TokenSearchState* CreateTokenSearchState() {
  return fortis::base::New<TokenSearchState>();
}

void DestroyTokenSearchState(TokenSearchState* state) {
  if (!state) return;
  if (g_search_state == state) g_search_state = nullptr;
  fortis::base::Delete(state);
}

bool RegisterTokenSearchRoute(http::HttpRouter& router, DiscoveryRouteContext* context) {
  if (!context || !context->owner || !context->workspace || !context->handoff ||
      !context->search || g_search_state) return false;
  context->search->route = context;
  g_search_state = context->search;
  return router.Add("GET", "/v1/networks/:network/tokens/search", handle, context);
}

}  // namespace fortis::discovery
