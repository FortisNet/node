#include "price_markets_route.h"
#include "ownership.h"

// POST /v1/prices/markets. One route object for the normal image (behind
// FORTIS_NATIVE_PRICE_ROUTE_CANARY=1) and the test binaries; the test clock
// and jitter arrive only through the DiscoveryCache seams. The contract is the
// comment above `handle`.

#include "coin_popularity.h"
#include "device_enrollment.h"
#include "js_case.h"
#include "market_view.h"
#include "media_registry.h"
#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "threading.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::discovery {

namespace {

using fortis::compat::js_json::OutBuffer;
namespace hb = fortis::codec::history_batch;

constexpr size_t kRequests = 8;          // deferred requests in flight
constexpr size_t kMaxIds = kPriceMarketsMaxIds;
constexpr size_t kMaxIdUnits = 100;      // zod string max(100)
constexpr size_t kArenaBytes = 64 * 1024;
constexpr size_t kBatches = 16;
constexpr size_t kBatchIds = kPriceMarketsBatchIds;
constexpr char kBatchSuffix[] = "&price_change_percentage=1h,24h,7d,14d,30d,200d,1y&sparkline=true";
constexpr size_t kRowTokens = 8192;
constexpr DiscoveryCacheTtl kTtl = kDiscoveryTtlTokenMarket;
constexpr char kMarketPrefix[] = "mkt:usd:";
constexpr char kNullEntry[] = "{\"market\":null}";

enum class CoinState : uint8_t { kCold, kHit, kLive, kPending, kWaiting };

struct Coin {
  uint32_t offset = 0;
  uint16_t length = 0;
  CoinState state = CoinState::kCold;
  bool refresh = false;
  uint8_t batch = 0;
  int64_t as_of = -1;
};

struct Request {
  bool used = false, deferred = false, cancelled = false, protobuf = false, responded = false;
  unsigned references = 0;
  uint64_t token = 0;
  int client = -1;
  int rate_cost = -1;
  uint32_t remaining = 0;
  uint32_t waiting = 0;  // batch bitmask
  size_t count = 0;
  Coin coins[kMaxIds];
  size_t arena_used = 0;
  char arena[kArenaBytes];
};

struct Batch {
  bool used = false, background = false;
  uint8_t owner = 0;  // request slot holding the ids
  size_t count = 0;
  uint8_t coins[kBatchIds]{};
  CoinState outcome[kBatchIds]{};
  int64_t as_of[kBatchIds]{};
  uint64_t request_id = 0;
  char path[kDiscoveryMaxPathBytes];
};

struct Span {
  bool found = false;
  size_t start = 0;
  size_t length = 0;
};

}  // namespace

struct PriceMarketsState {
  DiscoveryRouteContext* route = nullptr;
  fortis::base::Mutex refs_mutex;
  // requests[kRequests] is the synchronous fallback used when every deferred
  // slot is busy: it never owns a batch, so its cold coins are pending.
  Request requests[kRequests + 1];
  Batch batches[kBatches];
  Span spans[kBatchIds];
  codec::BoundedJsonToken row_tokens[kRowTokens];
  char image[16 * 1024];
  fortis::discovery::market_view::MarketViewScratch view;
};

// The registered state. Like the batch history driver, at most one markets
// route is registered per process.
PriceMarketsState* g_markets_state = nullptr;

namespace {

PriceMarketsState* state_of(DiscoveryRouteContext* route) { return route->markets; }

const char* coin_id(const Request& request, const Coin& coin) {
  return request.arena + coin.offset;
}

void release(PriceMarketsState* state, Request* request) {
  fortis::base::LockGuard refs_lock(state->refs_mutex);
  if (request->references) --request->references;
  if (!request->references) request->used = false;
}

void retain(PriceMarketsState* state, Request* request) {
  fortis::base::LockGuard refs_lock(state->refs_mutex);
  ++request->references;
}

void handoff_terminal(void* opaque, bool cancelled) {
  auto* request = static_cast<Request*>(opaque);
  PriceMarketsState* state = g_markets_state;
  if (cancelled) {
    fortis::base::LockGuard refs_lock(state->refs_mutex);
    request->cancelled = true;
  }
  release(state, request);
}

// UTF-16 code unit order of two valid UTF-8 strings (JavaScript's default
// Array.prototype.sort comparison).
int utf16_compare(const char* a, size_t a_length, const char* b, size_t b_length) {
  size_t i = 0, j = 0;
  auto next = [](const char* text, size_t length, size_t* position) -> uint32_t {
    const unsigned char c = static_cast<unsigned char>(text[(*position)++]);
    if (c < 0x80) return c;
    const size_t trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
    uint32_t value = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
    for (size_t k = 0; k < trailing && *position < length; ++k)
      value = value << 6 | (static_cast<unsigned char>(text[(*position)++]) & 0x3f);
    // Supplementary characters sort by their high surrogate.
    return value >= 0x10000 ? 0xd800 + ((value - 0x10000) >> 10) : value;
  };
  while (i < a_length && j < b_length) {
    const size_t i_before = i, j_before = j;
    const uint32_t x = next(a, a_length, &i), y = next(b, b_length, &j);
    if (x != y) return x < y ? -1 : 1;
    // Equal high surrogates: compare the low surrogates.
    if (x >= 0xd800 && x <= 0xdbff) {
      size_t ii = i_before, jj = j_before;
      uint32_t full_a = 0, full_b = 0;
      {
        const unsigned char c = static_cast<unsigned char>(a[ii++]);
        full_a = c & 0x07;
        for (int k = 0; k < 3; ++k) full_a = full_a << 6 | (static_cast<unsigned char>(a[ii++]) & 0x3f);
      }
      {
        const unsigned char c = static_cast<unsigned char>(b[jj++]);
        full_b = c & 0x07;
        for (int k = 0; k < 3; ++k) full_b = full_b << 6 | (static_cast<unsigned char>(b[jj++]) & 0x3f);
      }
      const uint32_t low_a = 0xdc00 + ((full_a - 0x10000) & 0x3ff);
      const uint32_t low_b = 0xdc00 + ((full_b - 0x10000) & 0x3ff);
      if (low_a != low_b) return low_a < low_b ? -1 : 1;
    }
  }
  if (i < a_length) return 1;
  if (j < b_length) return -1;
  return 0;
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

bool cache_key(const char* id, size_t length, char* output, size_t capacity) {
  const size_t prefix = sizeof(kMarketPrefix) - 1;
  if (prefix + length + 1 > capacity) return false;
  memcpy(output, kMarketPrefix, prefix);
  memcpy(output + prefix, id, length);
  output[prefix + length] = '\0';
  return true;
}

// ── Responses ──

// The cached entry `{"market":<entry|null>}` exactly as stored.
bool entry_market(const unsigned char* body, size_t length, const unsigned char** market,
                  size_t* market_length) {
  static const char kOpen[] = "{\"market\":";
  const size_t open = sizeof(kOpen) - 1;
  if (length < open + 2 || memcmp(body, kOpen, open) != 0 || body[length - 1] != '}')
    return false;
  *market = body + open;
  *market_length = length - open - 1;
  return true;
}

// Builds and sends the response for `request`; caller holds the workspace
// mutex. A deferred request completes through the handoff. Each served coin
// is re-read from the cache (a coin whose entry is gone is pending).
void finalize(DiscoveryRouteContext* route, Request& request, bool head) {
  PriceMarketsState* state = state_of(route);
  DiscoveryWorkspace* workspace = route->workspace;
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  size_t hit = 0, live = 0, pending = 0;
  int64_t newest = -1;
  static const uint8_t* market_texts[kMaxIds];
  static size_t market_lengths[kMaxIds];
  static const char* pending_ids[kMaxIds];
  static size_t pending_lengths[kMaxIds];
  size_t served_count = 0, pending_count = 0;
  OutBuffer markets{workspace->payload, sizeof(workspace->payload)};
  static size_t offsets[kMaxIds];
  for (size_t i = 0; i < request.count; ++i) {
    Coin& coin = request.coins[i];
    if (coin.state != CoinState::kHit && coin.state != CoinState::kLive) continue;
    char key[1024];
    const DiscoveryCacheEntry entry = cache_key(coin_id(request, coin), coin.length, key,
        sizeof(key)) ? cache->Read(key, now) : DiscoveryCacheEntry{};
    const unsigned char* market = nullptr; size_t market_length = 0;
    const bool readable = (entry.state == DiscoveryCacheState::kFresh ||
                           entry.state == DiscoveryCacheState::kStale) &&
        entry_market(entry.body, entry.body_length, &market, &market_length);
    if (!readable) { coin.state = CoinState::kPending; continue; }
    if (coin.state == CoinState::kHit && coin.as_of > newest) newest = coin.as_of;
    if (market_length == 4 && memcmp(market, "null", 4) == 0) continue;
    offsets[served_count] = markets.used;
    market_lengths[served_count] = market_length;
    ++served_count;
    markets.Append(market, market_length);
  }
  for (size_t i = 0; i < served_count; ++i) market_texts[i] = workspace->payload + offsets[i];
  for (size_t i = 0; i < request.count; ++i) {
    const Coin& coin = request.coins[i];
    if (coin.state == CoinState::kHit) { ++hit; continue; }
    if (coin.state == CoinState::kLive) { ++live; continue; }
    ++pending;
    pending_ids[pending_count] = coin_id(request, coin);
    pending_lengths[pending_count++] = coin.length;
  }
  // combineCacheProvenance over fulfilled coins, then the pending override.
  PriceMarketsRenderInput input;
  input.markets = market_texts;
  input.market_lengths = market_lengths;
  input.market_count = served_count;
  input.pending = pending_ids;
  input.pending_lengths = pending_lengths;
  input.pending_count = pending_count;
  const size_t entries = hit + live;
  input.hit = entries > 0 && hit == entries;
  input.partial = hit > 0 && hit < entries;
  if (pending) { input.hit = false; input.partial = true; }
  input.newest_ms = newest;
  http::HttpResponse response{};
  const bool ok = markets.ok && PriceMarketsRender(workspace, input, request.protobuf, &response);
  if (!ok) {
    response = http::JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    response.content_type = "application/json; charset=utf-8";
  }
  response.rate_limit_cost = request.rate_cost;
  response.rate_limit_remaining = request.remaining;
  request.responded = true;
  if (!request.deferred) {
    WriteHttpResponse(request.client, response, head);
    return;
  }
  fortis::base::UniqueLock refs_lock(state->refs_mutex);
  const bool cancelled = request.cancelled;
  refs_lock.Unlock();
  if (!cancelled) route->handoff->Complete(request.token, response);
}

// ── Provider batches ──

void batch_complete(void* opaque, const DiscoveryProviderResult& result,
                    const unsigned char* body, size_t body_length);

struct ScanContext {
  PriceMarketsState* state;
  Request* owner;
  Batch* batch;
  const unsigned char* base;
  bool failed;
};

bool scan_row(void* opaque, const unsigned char* element, size_t length) {
  auto* scan = static_cast<ScanContext*>(opaque);
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error{};
  if (!document.Parse(element, length, scan->state->row_tokens, kRowTokens, &error)) {
    scan->failed = true;  // TypeScript's JSON.parse succeeded or failed as a whole
    return false;
  }
  const codec::BoundedJsonToken* row = document.token(document.root());
  // coinMarket()'s `rows.flatMap((row) => typeof row.id === 'string' …)`
  // throws on a null element, failing the whole batch; any other non-object
  // simply has no string id.
  if (row->type == codec::BoundedJsonType::kNull) {
    scan->failed = true;
    return false;
  }
  if (row->type != codec::BoundedJsonType::kObject) return true;
  const uint32_t id = document.FindObjectValue(document.root(), "id");
  if (id == codec::kBoundedJsonNoToken || document.token(id)->type != codec::BoundedJsonType::kString)
    return true;
  static char decoded[4096];
  size_t decoded_length = 0;
  if (!document.DecodeString(id, decoded, sizeof(decoded), &decoded_length)) return true;
  for (size_t i = 0; i < scan->batch->count; ++i) {
    const Coin& coin = scan->owner->coins[scan->batch->coins[i]];
    if (coin.length == decoded_length &&
        memcmp(coin_id(*scan->owner, coin), decoded, decoded_length) == 0) {
      // new Map(rows): a later row with the same id replaces an earlier one.
      scan->state->spans[i] = Span{true, static_cast<size_t>(element - scan->base), length};
    }
  }
  return true;
}

// Stores every coin of a successful batch; false when the body is not JSON
// TypeScript could parse within the native bounds.
bool store_batch(DiscoveryRouteContext* route, Batch& batch, const unsigned char* body,
                 size_t length, int64_t now) {
  PriceMarketsState* state = state_of(route);
  DiscoveryWorkspace* workspace = route->workspace;
  Request& owner = state->requests[batch.owner];
  for (size_t i = 0; i < batch.count; ++i) state->spans[i] = Span{};
  if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) {
    body += 3; length -= 3;
  }
  bool is_array = false;
  ScanContext scan{state, &owner, &batch, body, false};
  if (!length || !fortis::compat::js_json::ForEachTopLevelArrayElement(body, length, &is_array,
                                                              scan_row, &scan)) return false;
  if (!is_array) {
    // `Array.isArray(body) ? body : []`: any other JSON value lists nothing.
    codec::BoundedJsonDocument document;
    codec::BoundedJsonError error{};
    if (!document.ParseWithLimits(body, length, workspace->tokens, kDiscoveryTokens,
                                  kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes, &error))
      return false;
  }
  DiscoveryCache* cache = workspace->cache;
  for (size_t i = 0; i < batch.count; ++i) {
    const Coin& coin = owner.coins[batch.coins[i]];
    OutBuffer out{workspace->payload, sizeof(workspace->payload)};
    if (state->spans[i].found) {
      codec::BoundedJsonDocument document;
      codec::BoundedJsonError error{};
      document.Parse(body + state->spans[i].start, state->spans[i].length, state->row_tokens,
                     kRowTokens, &error);
      char logo[fortis::discovery::market_view::kMediaKeyChars + 1];
      const char* logo_key = nullptr;
      size_t image_length = 0;
      if (fortis::discovery::market_view::RowImage(document, document.root(), state->image,
                                        sizeof(state->image), &image_length) &&
          RegisterMediaSource(cache->kv(), state->image, image_length, now, logo))
        logo_key = logo;
      out.Text("{\"market\":");
      fortis::discovery::market_view::AppendMarketEntry(document, document.root(), coin_id(owner, coin),
                                             coin.length, logo_key, out, &state->view);
      out.Byte('}');
    } else {
      out.Text(kNullEntry);
    }
    char key[1024];
    int64_t as_of = -1;
    const bool stored = out.ok && cache_key(coin_id(owner, coin), coin.length, key, sizeof(key)) &&
        cache->Write(key, kTtl, now, workspace->payload, out.used, &as_of);
    batch.outcome[i] = stored ? CoinState::kLive : CoinState::kPending;
    batch.as_of[i] = as_of;
  }
  return true;
}

void batch_complete(void* opaque, const DiscoveryProviderResult& result,
                    const unsigned char* body, size_t body_length) {
  PriceMarketsState* state = g_markets_state;
  auto* batch = static_cast<Batch*>(opaque);
  DiscoveryRouteContext* route = state->route;
  DiscoveryWorkspace* workspace = route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  Request& owner = state->requests[batch->owner];
  const bool fetched = !result.transport_error && !result.cancelled &&
      result.http_status >= 200 && result.http_status < 300 &&
      store_batch(route, *batch, body, body_length, now);
  if (!fetched) {
    for (size_t i = 0; i < batch->count; ++i) {
      const Coin& coin = owner.coins[batch->coins[i]];
      char key[1024];
      if (cache_key(coin_id(owner, coin), coin.length, key, sizeof(key))) {
        if (batch->background) cache->RecordRevalidateFailure(key, now, result.lane_rejected);
        else cache->RecordMissFailure(key, now, result.lane_rejected, 502);
      }
      batch->outcome[i] = CoinState::kPending;
    }
  }
  const size_t batch_index = static_cast<size_t>(batch - state->batches);
  const uint32_t bit = 1u << batch_index;
  for (size_t r = 0; r < kRequests; ++r) {
    Request& request = state->requests[r];
    if (!request.used || !(request.waiting & bit)) continue;
    for (size_t c = 0; c < request.count; ++c) {
      Coin& coin = request.coins[c];
      if (coin.state != CoinState::kWaiting || coin.batch != batch_index) continue;
      coin.state = CoinState::kPending;
      for (size_t i = 0; i < batch->count; ++i) {
        const Coin& member = owner.coins[batch->coins[i]];
        if (member.length == coin.length &&
            memcmp(coin_id(owner, member), coin_id(request, coin), coin.length) == 0) {
          coin.state = batch->outcome[i];
          break;
        }
      }
    }
    request.waiting &= ~bit;
    if (!request.waiting && !request.responded) {
      finalize(route, request, false);
      release(state, &request);  // the deferred response's reference
    }
  }
  batch->used = false;
  lock.Unlock();
  release(state, &owner);  // the batch's reference on its id owner
}

// Builds and submits batches for `indices` of request `request_index`.
// Coins that cannot be batched are marked pending. Caller holds the
// workspace mutex.
void submit_batches(DiscoveryRouteContext* route, size_t request_index, const uint8_t* indices,
                    size_t count, bool background) {
  PriceMarketsState* state = state_of(route);
  Request& request = state->requests[request_index];
  size_t next = 0;
  while (next < count) {
    Batch* batch = nullptr;
    size_t batch_index = 0;
    if (request_index < kRequests) {
      for (size_t i = 0; i < kBatches; ++i)
        if (!state->batches[i].used) { batch = &state->batches[i]; batch_index = i; break; }
    }
    if (!batch) break;
    batch->used = true; batch->background = background; batch->owner =
        static_cast<uint8_t>(request_index); batch->count = 0; batch->request_id = 0;
    static const char* ids[kBatchIds];
    static size_t lengths[kBatchIds];
    size_t candidates = 0;
    for (size_t k = next; k < count && candidates < kBatchIds; ++k, ++candidates) {
      ids[candidates] = coin_id(request, request.coins[indices[k]]);
      lengths[candidates] = request.coins[indices[k]].length;
    }
    const size_t taken = PriceMarketsBatchPath(ids, lengths, candidates, batch->path,
                                               sizeof(batch->path));
    batch->count = 0;
    for (size_t k = 0; k < taken; ++k) batch->coins[batch->count++] = indices[next++];
    if (!batch->count) { batch->used = false; break; }
    for (size_t i = 0; i < batch->count; ++i) {
      Coin& coin = request.coins[batch->coins[i]];
      if (!background) { coin.state = CoinState::kWaiting; coin.batch = static_cast<uint8_t>(batch_index); }
    }
    if (!background) request.waiting |= 1u << batch_index;
    retain(state, &request);  // the batch's reference on its id owner
    uint64_t request_id = 0;
    // lanes.run('coingecko', …) under the caller's work reason: a cold read
    // queues at foreground priority, a revalidation (swr_revalidate, its own
    // window-batcher group) is background work, shed while the lane is busy.
    // An unparseable 2xx is a transport failure, retried like a 5xx.
    if (!route->owner->Submit(DiscoveryUpstream::kCoinGecko, batch->path,
                              DiscoveryMonotonicMs() + kDiscoveryOwnerDeadlineMs, background,
                              batch_complete, batch, &request_id,
                              /*retry_unparseable=*/true)) {
      for (size_t i = 0; i < batch->count; ++i) {
        Coin& coin = request.coins[batch->coins[i]];
        if (!background) coin.state = CoinState::kPending;
      }
      if (!background) request.waiting &= ~(1u << batch_index);
      batch->used = false;
      release(state, &request);
      continue;
    }
    batch->request_id = request_id;
  }
  for (; next < count; ++next)
    if (!background) request.coins[indices[next]].state = CoinState::kPending;
}

// TypeScript revalidates each stale coin as it reads it, in parallel with the
// request's cold reads; the native refresh is submitted right after them, in
// one batch, skipping coins already refreshing.
void schedule_refresh(DiscoveryRouteContext* route, size_t request_index) {
  if (request_index >= kRequests) return;
  PriceMarketsState* state = state_of(route);
  Request& request = state->requests[request_index];
  uint8_t indices[kMaxIds];
  size_t count = 0;
  for (size_t c = 0; c < request.count; ++c) {
    Coin& coin = request.coins[c];
    if (!coin.refresh) continue;
    coin.refresh = false;
    bool refreshing = false;
    for (const Batch& batch : state->batches) {
      if (!batch.used || !batch.background) continue;
      const Request& owner = state->requests[batch.owner];
      for (size_t i = 0; i < batch.count && !refreshing; ++i) {
        const Coin& member = owner.coins[batch.coins[i]];
        refreshing = member.length == coin.length &&
            memcmp(coin_id(owner, member), coin_id(request, coin), coin.length) == 0;
      }
    }
    if (!refreshing) indices[count++] = static_cast<uint8_t>(c);
  }
  if (count) submit_batches(route, request_index, indices, count, true);
}

// ── Request decoding ──

struct RawId {
  const char* text;
  size_t length;
};

// zod marketsBody over a parsed JSON object: ids 1..100 strings, fiat literal
// 'usd' (default), strict. Decoded views go into `ids`.
bool validate_json(const codec::BoundedJsonDocument& json, uint8_t* scratch,
                   size_t capacity, RawId* ids, size_t* count) {
  const codec::BoundedJsonToken* root = json.token(json.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) return false;
  uint32_t list = codec::kBoundedJsonNoToken, fiat = codec::kBoundedJsonNoToken;
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value = json.token(key)->next_sibling;
    if (json.StringEquals(key, "ids")) list = value;
    else if (json.StringEquals(key, "fiat")) fiat = value;
    else return false;
    key = json.token(value)->next_sibling;
  }
  if (fiat != codec::kBoundedJsonNoToken && (json.token(fiat)->type != codec::BoundedJsonType::kString ||
                                      !json.StringEquals(fiat, "usd"))) return false;
  if (list == codec::kBoundedJsonNoToken || json.token(list)->type != codec::BoundedJsonType::kArray)
    return false;
  size_t n = 0, used = 0;
  for (uint32_t item = json.token(list)->first_child; item != codec::kBoundedJsonNoToken;
       item = json.token(item)->next_sibling) {
    if (n == kMaxIds || json.token(item)->type != codec::BoundedJsonType::kString) return false;
    size_t length = 0;
    char* text = reinterpret_cast<char*>(scratch) + used;
    if (used >= capacity || !json.DecodeString(item, text, capacity - used, &length)) return false;
    used += length + 1;
    ids[n++] = RawId{text, length};
  }
  *count = n;
  return n >= 1;
}

// fromBinary(MarketBatchRequestSchema), read as protobuf-es 2.16 reads it:
// `repeated string ids = 1` and `optional string fiat = 2` at any wire type
// (strict UTF-8, a leading BOM dropped, the last fiat wins), every other
// field skipped; then zod on {ids, fiat?}.
bool validate_protobuf(const uint8_t* body, size_t length, RawId* ids, size_t* count) {
  DiscoveryProtoReader r(body, length);
  size_t n = 0;
  bool has_fiat = false, too_many = false;
  const uint8_t* fiat = nullptr;
  size_t fiat_length = 0;
  while (r.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!r.Tag(&field, &wire)) return false;
    if (field == 1 || field == 2) {
      const uint8_t* text = nullptr; size_t text_length = 0;
      if (!r.String(&text, &text_length)) return false;
      if (field == 2) { has_fiat = true; fiat = text; fiat_length = text_length; continue; }
      if (n == kMaxIds) { too_many = true; continue; }
      ids[n++] = RawId{reinterpret_cast<const char*>(text), text_length};
    } else if (!r.Skip(wire, field, 99)) {
      return false;
    }
  }
  if (too_many || !n) return false;
  if (has_fiat && !(fiat_length == 3 && memcmp(fiat, "usd", 3) == 0)) return false;
  *count = n;
  return true;
}

// ── POST /v1/prices/markets ──────────────────────────────────────────────
//
// TypeScript: getTokenMarkets over TokenDiscoveryFacet.markets and
// CoinGeckoClient.coinMarket/coinMarkets; its recorded edge cases are
// tests/corpus/price_markets_edges.json, replayed by
// tests/price_markets_edges_test.py, unit tests/price_markets_test.cpp.
// Flutter: IntelligenceApi.tokenMarkets trims, lower-cases and de-duplicates
// a portfolio's CoinGecko ids, posts them in chunks of maxIdsPerRequest as a
// protobuf MarketBatchRequest with fiat 'usd' (Accept: application/protobuf),
// keeps its previous value for every id in `pending` and asks again later.
//
// Method. POST only; GET/HEAD/PUT, a trailing slash or another case are the
// router's 404. A query string is ignored.
//
// Order. 1. The body parser, exactly as POST /v1/bittensor/analytics: JSON
// (empty or invalid: 400 {"error":"BAD_REQUEST"}), protobuf bytes; anything
// else (text/plain and form included), or a body without a content type,
// 415 {"error":"UNSUPPORTED_MEDIA_TYPE"}. A framing refusal is answered
// after step 2 (auth-first), charged cost 1. 2. preHandler: Origin →
// 403 BROWSER_ORIGIN_FORBIDDEN; the bearer → 401 UNAUTHORIZED /
// SESSION_EXPIRED / ATTESTATION_REQUIRED; then the device quota charged
// PriceMarketsCost: ceil(raw ids.length / 10), at least 1, from the parsed
// body before validation (101 ids cost 11 and are then refused; a protobuf
// Buffer or a string `ids` cost 1) — over it 429 RATE_LIMITED with
// retry-after. 3. zod, every refusal a bare 400 {"error":"BAD_REQUEST"}
// after the quota: {ids: 1..100 strings, each JS-trimmed to 1..100 UTF-16
// units (a NUL, zero-width space or astral character counts as kept), fiat:
// literal 'usd' (absent → usd; 'USD', ' usd', null, numbers, '' refused)}
// .strict(); a protobuf body decoded as protobuf-es 2.16 decodes it
// (DiscoveryProtoReader; a malformed body is the same 400). The raw count is
// bounded, so 100 duplicates are one id and 101 duplicates a 400.
//
// Facet. ids → trimmed and ASCII lower-cased (only A-Z changes),
// de-duplicated, sorted by UTF-16 code units (astral characters by their
// high surrogate); the coin popularity tracker records them with the device
// id (the price schedule's demand input). Each coin reads `mkt:usd:<id>`
// through SwrCache (60 s fresh, 15 min stale; the test seam's jitter 0.5 is
// none): fresh/stale entries are served (stale ones revalidated, below); a
// miss joins any in-flight cold read of the coin (another request's batch),
// else is fetched. Cold coins of one request go to CoinGecko in one call per
// 50 ids: GET /api/v3/coins/markets?vs_currency=usd&ids=<encodeURIComponent
// of the ids joined by ','>&price_change_percentage=1h,24h,7d,14d,30d,200d,
// 1y&sparkline=true (accept: application/json, the demo/pro key header),
// foreground lane priority, retried on 5xx/408/transport/unparseable 2xx,
// not on 429/404. A listed coin (`row.id` exactly equal; the last duplicate
// row wins) is stored as {"market": {id, …marketView(row), logoMediaKey}},
// its https/http image registered as a media record; an unlisted coin (no
// such row, an object or other non-array reply) as {"market": null} and is
// absent from both lists; a failed batch (non-2xx after retries, invalid
// JSON, a null row, a lane refusal) leaves each of its coins `pending` and
// held 10 s (5 s for a lane refusal) without provider work. Stale coins are
// revalidated in one batch submitted with the request's cold batches, at
// background lane priority: shed while the lane is busy (5 s backoff), a
// failure backs off 30 s.
//
// Response. 200 {"markets":[stored market objects, in id order],
// "pending":[ids in id order],"cache":{"hit","timestamp"[,"partial":true]}}:
// combineCacheProvenance over the fulfilled coins (hit only when every one
// was cached; timestamp the newest cached asOf; partial when cached and live
// mix), then any pending coin forces hit false, partial true. With Accept
// naming application/protobuf (q > 0) MarketBatchResponse {markets:
// repeated Struct, pending, cache} (application/protobuf). Both carry Vary:
// Accept and the API baseline headers; errors do not carry Vary.
//
// Effects and bounds. `mkt:usd:<id>` entries in the durable KV, logo media
// records, the popularity record; miss holds and backoffs are in memory.
// Eight requests may wait on provider batches; a ninth concurrent cold
// request is answered at once with its cold coins pending (the synchronous
// slot). At most 16 batches in flight (a coin beyond them is pending), ids
// in a request's 64 KiB arena, provider bodies ≤ 512 KiB, the encoded answer
// within the 1 MiB workspace buffers (else 500 INTERNAL). JSON bodies over
// 256 KiB / 8,192 tokens / depth 16 → 413.
//
// Named differences (asserted by the edge replay):
//  - batch size: TypeScript's window batcher sends up to 250 ids per call;
//    natively a request's cold coins are split into calls of 50 (a 100-row
//    reply with 7-day sparklines measured 433 KiB against the 512 KiB
//    provider body bound) — the same coins, rows and answer;
//  - window batching: cold coins of concurrent requests arriving within
//    TypeScript's 20 ms window share one call; natively each request batches
//    its own cold coins (a coin already in flight is shared, never fetched
//    twice) — the same answers;
//  - after a restart the durable entries answer (hit, the first process's
//    timestamp) where TypeScript's in-memory Kv reads cold;
//  - the shared bounded JSON reader refuses a lone surrogate escape and an
//    overflowing number (400 BAD_REQUEST after the device policy, cost 1;
//    TypeScript answers the id pending, or charges and refuses it);
//  - a coin whose fetched entry cannot be stored (KV failure) is pending
//    rather than served uncached.
void handle(int client, const http::HttpRequest& http, bool head, void* opaque) {
  auto* route = static_cast<DiscoveryRouteContext*>(opaque);
  PriceMarketsState* state = state_of(route);
  DiscoveryWorkspace* workspace = route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  DiscoveryBody body{};
  const int framing = ParseDiscoveryBody(http, workspace->tokens, kDiscoveryTokens, &body);
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(http, framing ? 1 : PriceMarketsCost(body),
                                                           route->attestation_required);
  if (policy.status) {
    lock.Unlock();
    SendDiscoveryJson(client, policy.status, policy.reason,
        policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}", head,
        policy.rate_limit_cost, policy.rate_limit_remaining, policy.retry_after);
    return;
  }
  if (framing) {
    lock.Unlock();
    SendDiscoveryFraming(client, framing, head, policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }

  // A deferred slot when one is free, else the synchronous fallback.
  size_t index = kRequests;
  fortis::base::UniqueLock refs_lock(state->refs_mutex);
  for (size_t i = 0; i < kRequests; ++i) if (!state->requests[i].used) { index = i; break; }
  Request& request = state->requests[index];
  request.used = true; request.references = 1; request.deferred = false;
  request.cancelled = false; request.responded = false; request.waiting = 0;
  request.count = 0; request.arena_used = 0; request.token = 0;
  refs_lock.Unlock();
  request.client = client;
  request.protobuf = AcceptsProtobuf(http);
  request.rate_cost = policy.rate_limit_cost;
  request.remaining = policy.rate_limit_remaining;

  static PriceMarketsId normalized[kMaxIds];
  size_t count = 0;
  // Decoded JSON ids live in the workspace payload buffer until normalized.
  if (!PriceMarketsIds(body, workspace->payload, sizeof(workspace->payload), request.arena,
                       kArenaBytes, normalized, &count)) {
    lock.Unlock();
    release(state, &request);
    SendDiscoveryJson(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head,
                      policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  for (size_t c = 0; c < count; ++c) {
    Coin coin{};
    coin.offset = normalized[c].offset;
    coin.length = normalized[c].length;
    request.coins[c] = coin;
    request.arena_used = coin.offset + coin.length + 1;
  }
  request.count = count;
  // Client demand feeds the coin price popularity schedule.
  if (route->popularity && request.count) {
    fortis::discovery::CoinIdView views[kMaxIds];
    for (size_t c = 0; c < request.count; ++c)
      views[c] = fortis::discovery::CoinIdView{coin_id(request, request.coins[c]), request.coins[c].length};
    route->popularity->Record(views, request.count, policy.device.id[0] ? policy.device.id : nullptr);
  }

  // SwrCache.read per coin.
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  uint8_t cold[kMaxIds];
  size_t cold_count = 0;
  for (size_t c = 0; c < request.count; ++c) {
    Coin& coin = request.coins[c];
    char key[1024];
    if (!cache_key(coin_id(request, coin), coin.length, key, sizeof(key))) {
      coin.state = CoinState::kPending;
      continue;
    }
    const DiscoveryCacheEntry entry = cache->Read(key, now);
    if (entry.state == DiscoveryCacheState::kFresh || entry.state == DiscoveryCacheState::kStale) {
      coin.state = CoinState::kHit;
      coin.as_of = entry.as_of_ms;
      coin.refresh = entry.state == DiscoveryCacheState::kStale && cache->AllowRevalidate(key, now);
      continue;
    }
    int replay = 0;
    if (cache->MissReplay(key, now, &replay)) { coin.state = CoinState::kPending; continue; }
    // A coin another request is fetching joins that flight.
    bool joined = false;
    for (size_t b = 0; b < kBatches && !joined; ++b) {
      Batch& batch = state->batches[b];
      if (!batch.used || batch.background) continue;
      const Request& owner = state->requests[batch.owner];
      for (size_t i = 0; i < batch.count; ++i) {
        const Coin& member = owner.coins[batch.coins[i]];
        if (member.length == coin.length &&
            memcmp(coin_id(owner, member), coin_id(request, coin), coin.length) == 0) {
          coin.state = CoinState::kWaiting;
          coin.batch = static_cast<uint8_t>(b);
          request.waiting |= 1u << b;
          joined = true;
          break;
        }
      }
    }
    if (!joined) cold[cold_count++] = static_cast<uint8_t>(c);
  }
  if (cold_count) submit_batches(route, index, cold, cold_count, false);
  // Stale coins revalidate now, after (and so behind) the cold batches.
  schedule_refresh(route, index);
  if (!request.waiting) {
    finalize(route, request, head);
    lock.Unlock();
    release(state, &request);
    return;
  }
  // Provider work: the socket moves to the handoff; the last batch answers.
  request.deferred = true;
  retain(state, &request);  // the deferred response
  retain(state, &request);  // the handoff terminal
  if (!http::ReserveHttpResponse(client, head, kDiscoveryHandoffTimeoutMs, &request.token,
                           handoff_terminal, &request)) {
    // Answer now: the in-flight coins are pending, their batches still store.
    request.deferred = false;
    for (size_t c = 0; c < request.count; ++c)
      if (request.coins[c].state == CoinState::kWaiting) request.coins[c].state = CoinState::kPending;
    request.waiting = 0;
    finalize(route, request, head);
    lock.Unlock();
    release(state, &request);  // terminal
    release(state, &request);  // deferred response
    release(state, &request);  // handler
    return;
  }
  lock.Unlock();
  release(state, &request);  // handler
}

}  // namespace

uint32_t PriceMarketsCost(const DiscoveryBody& body) {
  if (body.kind != DiscoveryBodyKind::kJson) return 1;
  const codec::BoundedJsonToken* root = body.json.token(body.json.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) return 1;
  // FindObjectValue keeps the last member, as JSON.parse does.
  const uint32_t ids = body.json.FindObjectValue(body.json.root(), "ids");
  if (ids == codec::kBoundedJsonNoToken || body.json.token(ids)->type != codec::BoundedJsonType::kArray) return 1;
  size_t n = 0;
  for (uint32_t item = body.json.token(ids)->first_child; item != codec::kBoundedJsonNoToken;
       item = body.json.token(item)->next_sibling) ++n;
  const uint32_t cost = static_cast<uint32_t>((n + 9) / 10);
  return cost ? cost : 1;
}

bool PriceMarketsIds(const DiscoveryBody& body, uint8_t* scratch, size_t scratch_capacity,
                     char* arena, size_t arena_capacity, PriceMarketsId* ids, size_t* count) {
  static RawId raw[kMaxIds];
  size_t raw_count = 0;
  bool valid = false;
  if (body.kind == DiscoveryBodyKind::kJson)
    valid = validate_json(body.json, scratch, scratch_capacity, raw, &raw_count);
  else if (body.kind == DiscoveryBodyKind::kProtobuf)
    valid = validate_protobuf(body.bytes, body.length, raw, &raw_count);
  // z.string().trim().min(1).max(100) on every id.
  for (size_t i = 0; valid && i < raw_count; ++i) {
    size_t begin = 0, end = 0;
    hb::JsTrim(raw[i].text, raw[i].length, &begin, &end);
    raw[i].text += begin;
    raw[i].length = end - begin;
    const size_t units = hb::Utf16Units(raw[i].text, raw[i].length);
    valid = units >= 1 && units <= kMaxIdUnits;
  }
  if (!valid) return false;
  // Lower-cased (after the zod trim, JavaScript trims again: a no-op),
  // de-duplicated and sorted by UTF-16 code units.
  size_t used = 0, n = 0;
  for (size_t i = 0; i < raw_count; ++i) {
    char* out = arena + used;
    size_t length = 0;
    if (used >= arena_capacity ||
        !fortis::compat::AsciiToLowerCase(raw[i].text, raw[i].length, out, arena_capacity - used, &length) ||
        !length || length > 0xffff || used + length + 1 > arena_capacity) continue;
    bool duplicate = false;
    for (size_t c = 0; c < n && !duplicate; ++c)
      duplicate = ids[c].length == length && memcmp(arena + ids[c].offset, out, length) == 0;
    if (duplicate) continue;
    out[length] = '\0';
    ids[n++] = PriceMarketsId{static_cast<uint32_t>(used), static_cast<uint16_t>(length)};
    used += length + 1;
  }
  for (size_t i = 1; i < n; ++i) {
    const PriceMarketsId moving = ids[i];
    size_t j = i;
    while (j > 0 && utf16_compare(arena + ids[j - 1].offset, ids[j - 1].length,
                                  arena + moving.offset, moving.length) > 0) {
      ids[j] = ids[j - 1];
      --j;
    }
    ids[j] = moving;
  }
  *count = n;
  return true;
}

size_t PriceMarketsBatchPath(const char* const* ids, const size_t* lengths, size_t count,
                             char* path_out, size_t capacity) {
  if (!capacity) return 0;
  OutBuffer path{reinterpret_cast<uint8_t*>(path_out), capacity - 1};
  path.Text("/api/v3/coins/markets?vs_currency=usd&ids=");
  const size_t suffix = sizeof(kBatchSuffix) - 1;
  size_t taken = 0;
  while (taken < count && taken < kBatchIds) {
    OutBuffer measure{};
    if (taken) measure.Text("%2C");
    append_component(measure, ids[taken], lengths[taken]);
    if (path.used + measure.used + suffix >= path.capacity) break;
    if (taken) path.Text("%2C");
    append_component(path, ids[taken], lengths[taken]);
    ++taken;
  }
  path.Text(kBatchSuffix);
  path_out[path.ok ? path.used : 0] = '\0';
  return path.ok ? taken : 0;
}

bool PriceMarketsRender(DiscoveryWorkspace* workspace, const PriceMarketsRenderInput& input,
                        bool protobuf, http::HttpResponse* response) {
  if (!protobuf) {
    OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
    out.Text("{\"markets\":[");
    for (size_t i = 0; i < input.market_count; ++i) {
      if (i) out.Byte(',');
      out.Append(input.markets[i], input.market_lengths[i]);
    }
    out.Text("],\"pending\":[");
    for (size_t i = 0; i < input.pending_count; ++i) {
      if (i) out.Byte(',');
      hb::AppendJsonString(out, input.pending[i], input.pending_lengths[i]);
    }
    out.Text("],\"cache\":");
    AppendDiscoveryCacheJson(out, input.hit, input.newest_ms, input.partial, input.partial);
    out.Byte('}');
    if (!out.ok) return false;
    *response = http::HttpResponse{200, "OK", "application/json; charset=utf-8", workspace->json_out,
                             out.used};
    response->vary_accept = true;
    return true;
  }
  OutBuffer out{workspace->proto_out, sizeof(workspace->proto_out)};
  const auto scratch = DiscoveryScratch(workspace);
  for (size_t i = 0; i < input.market_count; ++i) {
    codec::BoundedJsonDocument document;
    codec::BoundedJsonError error{};
    if (!document.Parse(input.markets[i], input.market_lengths[i], workspace->tokens,
                        kDiscoveryTokens, &error)) return false;
    OutBuffer measure{};
    if (!fortis::compat::js_json::AppendProtoStruct(document, document.root(), measure, scratch))
      return false;
    fortis::codec::proto_wire::PutTag(out, 1, 2);
    fortis::codec::proto_wire::PutVarint(out, measure.used);
    if (!fortis::compat::js_json::AppendProtoStruct(document, document.root(), out, scratch)) return false;
  }
  for (size_t i = 0; i < input.pending_count; ++i)
    fortis::codec::proto_wire::PutBytes(out, 2, input.pending[i], input.pending_lengths[i]);
  OutBuffer cache_size{};
  AppendDiscoveryCacheProto(cache_size, input.hit, input.newest_ms, input.partial);
  fortis::codec::proto_wire::PutTag(out, 3, 2);
  fortis::codec::proto_wire::PutVarint(out, cache_size.used);
  AppendDiscoveryCacheProto(out, input.hit, input.newest_ms, input.partial);
  if (!out.ok) return false;
  *response = http::HttpResponse{200, "OK", "application/protobuf", workspace->proto_out, out.used};
  response->vary_accept = true;
  return true;
}

PriceMarketsState* CreatePriceMarketsState() {
  return fortis::base::New<PriceMarketsState>();
}

void DestroyPriceMarketsState(PriceMarketsState* state) {
  if (!state) return;
  if (g_markets_state == state) g_markets_state = nullptr;
  fortis::base::Delete(state);
}

bool RegisterPriceMarketsRoute(http::HttpRouter& router, DiscoveryRouteContext* context) {
  if (!context || !context->owner || !context->workspace || !context->handoff ||
      !context->markets || g_markets_state) return false;
  context->markets->route = context;
  g_markets_state = context->markets;
  return router.Add("POST", "/v1/prices/markets", handle, context);
}

}  // namespace fortis::discovery
