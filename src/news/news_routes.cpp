// GET /v1/news and GET /v1/news/{id}: the feed query rules, the feed and
// detail builders, their protobuf encodings, the conditional reply and route
// registration. GET /v1/news/{id}/reactions lives in news_reactions.cpp.
#include "query_string.h"
#include "news_routes_internal.h"
#include "feed_query_service.h"
#include "http_middleware.h"
#include "threading.h"

namespace pw = fortis::codec::proto_wire;

namespace hb = fortis::codec::history_batch;

namespace fortis::news {

namespace {

constexpr char kProtobufType[] = "application/protobuf";

constexpr char kItemCache[] = "private, max-age=300";
constexpr discovery::DiscoveryCacheTtl kTtlNews{60000, 3600000};
constexpr discovery::DiscoveryCacheTtl kTtlNewsItem{300000, 86400000};

constexpr int64_t kStaleAfterMs = 45LL * 60 * 1000;   // NEWS_STALE_AFTER_MS

void append_ascii_string(OutBuffer& out, const char* text) {
  out.Byte('"');
  out.Text(text);
  out.Byte('"');
}

// Selected first value, decoded into caller storage. Invalid unknown or
// later components refuse the entire query as well.
bool query_values(const char* target, const char* name, char* const* values, size_t* lengths,
                  size_t capacity, size_t* count, char*, char*, size_t bytes) {
  *count = 0;
  const auto query = http::query_string::Query::FromTarget(target);
  if (!query.Valid()) return false;
  http::query_string::Component value;
  if (!query.First(name, &value)) return true;
  if (!capacity || !bytes) return false;
  const size_t length = value.Decode(values[0], bytes - 1);
  if (length == SIZE_MAX || length >= bytes) return false;
  values[0][length] = '\0';
  lengths[0] = length;
  *count = 1;
  return true;
}

// ---- GET /v1/news query (zod 4 feedQuery) -----------------------------------



// One request's parse scratch on the heap, sized by the request target and
// freed by its owner: two value buffers, query_values' raw and key buffers
// and the number scratch (target length + 1 bytes each), then the UTF-16
// arena the topic CSV is trimmed and lowercased in (UTF-8 → at most one
// unit per byte; lowercase at most triples a part). Nothing is static or
// shared, so concurrent /v1/news reads parse independently and outside the
// workspace mutex; the HTTP parser's 16 KiB header bound caps the target,
// so one parse holds at most about 13 x 16 KiB.
struct FeedScratch {
  std::unique_ptr<uint8_t[], base::StorageDeleter> storage;
  uint8_t* block = nullptr;
  size_t bytes = 0;
  char* values[2] = {nullptr, nullptr};
  char* raw = nullptr;
  char* key = nullptr;
  char* number = nullptr;
  Arena arena{nullptr, 0};

  bool Init(const char* target) {
    bytes = strlen(target) + 1;
    const size_t text = 5 * bytes;
    const size_t units = 8 * bytes + 1024;
    storage = base::AllocateStorage<uint8_t[]>(text + units);
    block = storage.get();
    if (!block) return false;
    values[0] = reinterpret_cast<char*>(block);
    values[1] = values[0] + bytes;
    raw = values[1] + bytes;
    key = raw + bytes;
    number = key + bytes;
    arena = Arena{block + text, units};
    return true;
  }

};

// False when the native scratch cannot be allocated (the caller answers a
// plain 400 BAD_REQUEST); otherwise `query` holds the topics and the limit,
// or `valid` is false.
bool parse_feed_query(const http::HttpRequest& request, FeedQuery* query) {
  const char* target = request.raw_target ? request.raw_target : "";
  FeedScratch scratch;
  if (!scratch.Init(target)) return false;
  Arena* arena = &scratch.arena;
  size_t lengths[2] = {0, 0};
  size_t count = 0;
  // topics: z.string().trim().max(200).optional().transform(csv → known topics)
  if (!query_values(target, "topics", scratch.values, lengths, 2, &count, scratch.raw, scratch.key,
                    scratch.bytes)) return false;
  if (count > 1) {
    query->valid = false;
  } else if (count == 1) {
    const U16 raw = arena->FromUtf8(scratch.values[0], lengths[0]);
    const U16 trimmed = fortis::news::JsTrim(raw);
    if (!arena->ok) return false;
    if (trimmed.length > 200) {
      query->valid = false;
    } else if (trimmed.length) {
      query->has_topics = true;
      size_t start = 0;
      for (size_t i = 0; i <= trimmed.length; ++i) {
        if (i < trimmed.length && trimmed.data[i] != ',') continue;
        const U16 part{trimmed.data + start, i - start};
        const U16 lowered = fortis::news::JsToLowerCase(fortis::news::JsTrim(part), arena);
        Topic topic;
        if (fortis::news::TopicFromName(lowered, &topic) && query->topic_count < 64)
          query->topics[query->topic_count++] = topic;
        start = i + 1;
      }
      if (!arena->ok) return false;
    }
  }
  // limit: z.coerce.number().int().min(1).max(100).default(40)
  count = 0;
  if (!query_values(target, "limit", scratch.values, lengths, 2, &count, scratch.raw, scratch.key,
                    scratch.bytes)) return false;
  if (count) {
    uint64_t value = 0;
    if (http::query_string::ParseUnsigned(scratch.values[0], lengths[0], 100, &value) && value > 0)
      query->limit = static_cast<size_t>(value);
    else
      query->valid = false;
  }
  // `from` is an inclusive Unix millisecond upper bound. `beforeId` makes
  // the boundary exact when multiple stories share the same millisecond.
  count = 0;
  if (!query_values(target, "from", scratch.values, lengths, 2, &count, scratch.raw, scratch.key,
                    scratch.bytes)) return false;
  if (count) {
    if (count != 1 || lengths[0] < 1 || lengths[0] > 13) return false;
    int64_t value = 0;
    for (size_t i = 0; i < lengths[0]; ++i) {
      const char digit = scratch.values[0][i];
      if (digit < '0' || digit > '9') return false;
      value = value * 10 + digit - '0';
    }
    query->has_from = true;
    query->from_ms = value;
  }
  count = 0;
  if (!query_values(target, "beforeId", scratch.values, lengths, 2, &count, scratch.raw,
                    scratch.key, scratch.bytes)) return false;
  if (count) {
    if (!query->has_from || count != 1 || lengths[0] != 26) return false;
    for (size_t i = 0; i < 26; ++i) {
      const char c = scratch.values[0][i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'H') || c == 'J' || c == 'K' ||
            c == 'M' || c == 'N' || (c >= 'P' && c <= 'T') || (c >= 'V' && c <= 'Z')))
        return false;
    }
    memcpy(query->before_id, scratch.values[0], 26);
  }
  return true;
}

// variantKey(): topics sorted (string order) joined by '+', then '|' limit.
void variant_key(const FeedQuery& query, char* output, size_t capacity) {
  const char* names[64];
  size_t count = 0;
  for (size_t i = 0; i < query.topic_count; ++i) names[count++] = fortis::news::TopicName(query.topics[i]);
  for (size_t i = 1; i < count; ++i) {
    const char* moving = names[i];
    size_t j = i;
    while (j > 0 && strcmp(names[j - 1], moving) > 0) { names[j] = names[j - 1]; --j; }
    names[j] = moving;
  }
  size_t used = static_cast<size_t>(snprintf(output, capacity, "news:"));
  for (size_t i = 0; i < count && used < capacity; ++i)
    used += static_cast<size_t>(snprintf(output + used, capacity - used, "%s%s", i ? "+" : "", names[i]));
  if (used < capacity) {
    used += static_cast<size_t>(snprintf(output + used, capacity - used, "|%zu", query.limit));
    if (query.has_from && used < capacity)
      snprintf(output + used, capacity - used, "|%lld|%s",
               static_cast<long long>(query.from_ms), query.before_id);
  }
}

// The route's id rule over the decoded parameter bytes: trim, ASCII
// upper-casing (an identifier: js-case), then the ULID pattern
// /^[0-9A-HJKMNP-TV-Z]{26}$/.
bool normalize_id_text(const char* param, size_t length, char output[27]) {
  if (!hb::ValidUtf8(reinterpret_cast<const uint8_t*>(param), length)) return false;
  size_t begin = 0, end = 0;
  hb::JsTrim(param, length, &begin, &end);
  char upper[http::kHttpPathParamValueBytes * 3 + 1];
  size_t upper_length = 0;
  if (!fortis::compat::AsciiToUpperCase(param + begin, end - begin, upper, sizeof(upper), &upper_length) ||
      upper_length != 26) return false;
  for (size_t i = 0; i < 26; ++i) {
    const char c = upper[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'H') || c == 'J' || c == 'K' ||
                    c == 'M' || c == 'N' || (c >= 'P' && c <= 'T') || (c >= 'V' && c <= 'Z');
    if (!ok) return false;
  }
  memcpy(output, upper, 26);
  output[26] = '\0';
  return true;
}

// ---- payloads (JSON.stringify of the facet objects) ----------------------------

void append_item_fields(OutBuffer& out, const Cluster& cluster, Arena* arena) {
  using fortis::news::AppendJsonString;
  out.Text("\"id\":"); AppendJsonString(out, cluster.id);
  out.Text(",\"headline\":"); AppendJsonString(out, cluster.headline);
  out.Text(",\"summary\":"); AppendJsonString(out, cluster.summary);
  out.Text(",\"topics\":[");
  for (size_t i = 0; i < cluster.topic_count; ++i) {
    if (i) out.Byte(',');
    append_ascii_string(out, fortis::news::TopicName(cluster.topics[i]));
  }
  out.Text("],\"impact\":");
  append_ascii_string(out, fortis::news::ImpactName(cluster.impact));
  out.Text(",\"tickers\":[");
  for (size_t i = 0; i < cluster.ticker_count; ++i) {
    const fortis::news::Ticker& t = cluster.tickers[i];
    if (i) out.Byte(',');
    out.Text("{\"symbol\":"); AppendJsonString(out, t.symbol);
    out.Text(",\"coinId\":");
    if (t.has_coin_id) AppendJsonString(out, t.coin_id); else out.Text("null");
    out.Text(",\"change24hPct\":");
    if (t.has_change24h && isfinite(t.change24h_pct)) hb::AppendJsNumber(out, t.change24h_pct);
    else out.Text("null");
    out.Byte('}');
  }
  out.Text("],\"publisher\":"); AppendJsonString(out, cluster.publisher);
  out.Text(",\"url\":"); AppendJsonString(out, cluster.url);
  out.Text(",\"imageMediaKey\":");
  if (cluster.has_media_key) append_ascii_string(out, cluster.media_key); else out.Text("null");
  out.Text(",\"publishedAt\":"); append_iso(out, static_cast<int64_t>(cluster.published_ms));
  out.Text(",\"sourceCount\":");
  char number[32];
  snprintf(number, sizeof(number), "%zu", fortis::news::SourceCount(cluster, arena));
  out.Text(number);
}

bool build_feed(const NewsRouteContext* route, const FeedQuery& query, int64_t now, Arena* arena,
                OutBuffer& out) {
  StoredNewsRepository stored(route->store);
  NewsRepository* repository = route->repository ? route->repository : &stored;
  const FeedQueryService service(*repository);
  return service.Feed(query, now, arena, out);
}

// toDetail(cluster), or the literal null for an unknown story.
bool build_detail(const NewsRouteContext* route, const char* id, int64_t now, Arena* arena,
                  OutBuffer& out) {
  StoredNewsRepository stored(route->store);
  NewsRepository* repository = route->repository ? route->repository : &stored;
  const FeedQueryService service(*repository);
  return service.Detail(id, now, arena, out);
}

bool item_proto(Proto& p, const json::Value& item, OutBuffer& out) {
  p.string(out, 1, json::Find(item, "id"), true);
  p.string(out, 2, json::Find(item, "headline"), true);
  p.string(out, 3, json::Find(item, "summary"), true);
  if (const json::Value* topics = json::Find(item, "topics"))
    for (size_t i = 0; i < topics->count; ++i) {
      const json::Value& topic = topics->items[i];
      OutBuffer text = p.sub(topic.string.length * 3 + 1);
      fortis::news::AppendUtf8(text, topic.string);
      pw::PutBytes(out, 4, text.data, text.used);
    }
  p.string(out, 5, json::Find(item, "impact"), true);
  if (const json::Value* tickers = json::Find(item, "tickers"))
    for (size_t i = 0; i < tickers->count; ++i) {
      const json::Value& ticker = tickers->items[i];
      OutBuffer body = p.sub(ticker.raw_length * 2 + 64);
      p.string(body, 1, json::Find(ticker, "symbol"), true);
      p.string(body, 2, json::Find(ticker, "coinId"), false);
      p.optional_double(body, 3, json::Find(ticker, "change24hPct"));
      p.message(out, 6, body);
    }
  p.string(out, 7, json::Find(item, "publisher"), true);
  p.string(out, 8, json::Find(item, "url"), true);
  p.string(out, 9, json::Find(item, "imageMediaKey"), false);
  p.string(out, 10, json::Find(item, "publishedAt"), true);
  const json::Value* count = json::Find(item, "sourceCount");
  if (count && count->type == json::Type::kNumber && count->number > 0)
    p.varint(out, 11, static_cast<uint64_t>(count->number));
  return p.ok && out.ok;
}

bool feed_proto(const json::Value& feed, Arena* arena, OutBuffer& out) {
  Proto p{arena};
  if (const json::Value* items = json::Find(feed, "items"))
    for (size_t i = 0; i < items->count; ++i) {
      OutBuffer body = p.sub(items->items[i].raw_length * 2 + 64);
      if (!item_proto(p, items->items[i], body)) return false;
      p.message(out, 1, body);
    }
  p.string(out, 2, json::Find(feed, "asOf"), false);
  const json::Value* stale = json::Find(feed, "stale");
  if (stale && stale->type == json::Type::kTrue) p.varint(out, 3, 1);
  return p.ok && out.ok;
}

bool detail_proto(const json::Value& detail, Arena* arena, OutBuffer& out) {
  Proto p{arena};
  OutBuffer item = p.sub(detail.raw_length * 2 + 64);
  if (!item_proto(p, detail, item)) return false;
  p.message(out, 1, item);
  if (const json::Value* sources = json::Find(detail, "sources"))
    for (size_t i = 0; i < sources->count; ++i) {
      const json::Value& source = sources->items[i];
      OutBuffer body = p.sub(source.raw_length * 2 + 64);
      p.string(body, 1, json::Find(source, "publisher"), true);
      p.string(body, 2, json::Find(source, "url"), true);
      p.string(body, 3, json::Find(source, "title"), true);
      p.string(body, 4, json::Find(source, "publishedAt"), true);
      p.string(body, 5, json::Find(source, "subtitle"), false);
      p.string(body, 6, json::Find(source, "abstract"), false);
      p.string(body, 7, json::Find(source, "body"), false);
      p.string(body, 8, json::Find(source, "summary"), false);
      p.string(body, 9, json::Find(source, "imageMediaKey"), false);
      p.message(out, 2, body);
    }
  return p.ok && out.ok;
}

// ---- handlers --------------------------------------------------------------------

// GET /v1/news — R28. The TypeScript readNews over NewsFacet.feed and
// KvNewsStore.
// Caller: the app's IntelligenceApi.news() (protobuf, conditional, optional
// limit, comma-joined topics, and an optional older-page cursor).
//
// Routing: GET; HEAD is Fastify's automatic HEAD (same status and headers,
// no body). Another method, a trailing slash, /v1/NEWS or /v1//news is the
// router's 404 (rule 'router 404'); /v1/news/ is R29's malformed id.
// Order: Origin present → 403 BROWSER_ORIGIN_FORBIDDEN; bearer → 401
// UNAUTHORIZED / SESSION_EXPIRED (ATTESTATION_REQUIRED for a development
// device when attestation is required); device quota, cost 1 (development
// 240/min) → 429 RATE_LIMITED, retry-after 60. From the quota on, every
// answer carries x-rate-limit-cost/-remaining; an invalid query still costs 1.
// Query: fast-querystring over the raw target ('+' is a space, a malformed
// escape is kept verbatim, keys exact; other keys ignored), then zod 4:
//   topics  string().trim().max(200) → CSV parts trimmed and lowercased,
//           unknown parts dropped, duplicates kept; repeated key → 400
//           invalid_type (array); > 200 UTF-16 units after trim → 400
//           too_big; empty, blank or no known part = no topic filter.
//   limit   coerce.number().int().min(1).max(100).default(40): Number() of
//           the text (0x/0b/0o, exponents, JS whitespace, '' → 0; a repeated
//           key is NaN).
//   from    optional inclusive Unix millisecond upper bound (1–13 digits).
//   beforeId optional ULID requiring from; at the same millisecond only IDs
//           below this one are included, so pages cannot split a time group.
//   Any failure is 400 {"error":"BAD_REQUEST"}.
// The parse uses per-request heap scratch sized by the target (the HTTP
// parser's 16 KiB header bound caps it near 210 KiB), runs before the route
// mutex and shares nothing; if it cannot be allocated: 400
// {"error":"BAD_REQUEST"} (native only).
// Cache: SWR key news:<topics sorted, '+'-joined>|<limit>, 60 s fresh and
// 3,600 s stale (TTL.news), in the durable KV. Fresh: served. Stale:
// served, then recomputed after the response is written (a failure backs
// off 30 s). Cursor requests bypass the SWR cache. Miss: computed and stored. Requests serialize on the route
// mutex, so a cold variant is computed once.
// Projection (computeFeed): FeedSnapshot — the time index newest first
// (publishedAt, then id, descending), unexpired (publishedAt + 7 days),
// any requested topic, up to `limit` — and lastIngestAt (newest
// update-index stamp), read under the story lock so an ingest is seen
// entirely or not at all. Items: id, headline, summary, topics, impact,
// tickers {symbol, coinId, change24hPct: the durable usd spot row's or
// null}, publisher, url, imageMediaKey, publishedAt (ISO), sourceCount
// (distinct lowercased publishers, at least 1); asOf = lastIngestAt or
// null; stale = no ingest, or more than 45 minutes since it. Story text is
// what TypeScript's lmdb/msgpack round trip returns (news_store.cpp
// append_stored). A record that is not a story fails the read: 500
// {"error":"INTERNAL"}, as KvNewsStore.decode throws.
// Answer: 200, cache-control private, max-age=60. JSON (application/json;
// charset=utf-8, ETag W/"news-<sha256(body)[:24]>"), or with an Accept
// naming application/protobuf at q > 0 NewsFeedResponse (ETag
// W/"news-protobuf-…", Vary: Accept). If-None-Match equal to that ETag →
// 304 with the etag and cache-control.
// Bounds: 1 MiB JSON or protobuf output, 4 MiB route arena per computation.
// Differences (named rules, tests/news_reads_edges_test.py): the router's
// 404 body; the native parser's 400 for a fragment, a repeated or
// over-256-byte If-None-Match, or a malformed or non-UTF-8 escape; a HEAD
// 304 without content-length; the shared AcceptsProtobuf trims the media
// type before ';' ("application/protobuf ; q=0.5" answers protobuf, JSON in
// TypeScript); envelopes survive a restart (TypeScript's production Kv is
// in-memory, so its first read after a restart recomputes).
void handle_feed(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* route = static_cast<NewsRouteContext*>(opaque);
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, route->attestation_required);
  if (policy.status) {
    http::HttpResponse response{policy.status, policy.reason, kJsonType,
        reinterpret_cast<const uint8_t*>(policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}"),
        strlen(policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}")};
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    WriteHttpResponse(client, response, head);
    return;
  }
  FeedQuery query{};
  if (!parse_feed_query(request, &query) || !query.valid) {
    send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head,
              policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  NewsWorkspace* workspace = route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  Arena arena{workspace->arena, sizeof(workspace->arena)};
  char key[256];
  variant_key(query, key, sizeof(key));
  discovery::DiscoveryCache* cache = workspace->cache;
  const int64_t now = route_now(route);
  const discovery::DiscoveryCacheEntry entry = query.has_from
      ? discovery::DiscoveryCacheEntry{} : cache->Read(key, now);
  const uint8_t* payload = nullptr;
  size_t length = 0;
  bool stale = false;
  if (entry.state == discovery::DiscoveryCacheState::kFresh || entry.state == discovery::DiscoveryCacheState::kStale) {
    uint8_t* copy = static_cast<uint8_t*>(arena.Alloc(entry.body_length + 1, 1));
    if (copy) {
      memcpy(copy, entry.body, entry.body_length);
      payload = copy;
      length = entry.body_length;
      stale = entry.state == discovery::DiscoveryCacheState::kStale;
    }
  }
  if (!payload) {
    OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
    if (!build_feed(route, query, now, &arena, out)) {
      lock.Unlock();
      send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head,
                policy.rate_limit_cost, policy.rate_limit_remaining);
      return;
    }
    int64_t as_of = 0;
    if (!query.has_from) cache->Write(key, kTtlNews, now, out.data, out.used, &as_of);
    payload = out.data;
    length = out.used;
  }
  http::HttpResponse response = conditional(request, "news", payload, length, feed_proto, workspace,
                                      &arena, kFeedCache);
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  WriteHttpResponse(client, response, head);
  if (stale && cache->AllowRevalidate(key, now)) {
    // The background revalidation of a stale read: recompute and store.
    Arena again{workspace->arena, sizeof(workspace->arena)};
    OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
    const int64_t at = route_now(route);
    int64_t as_of = 0;
    if (build_feed(route, query, at, &again, out)) cache->Write(key, kTtlNews, at, out.data, out.used, &as_of);
    else cache->RecordRevalidateFailure(key, at, false);
  }
}

// GET /v1/news/{id} — R29. The TypeScript readNewsItem over NewsFacet.item
// and KvNewsStore.item. Caller: IntelligenceApi.newsStory() (protobuf; 404
// is a normal "gone" answer, any other non-200 an error).
//
// Routing: an empty segment (/v1/news/) and one holding a decoded control
// character or DEL are the router's 404 before any hook. Order:
//   1. a raw byte >= 0x80 in the target: 400 {"error":"BAD_REQUEST"},
//      before any hook, no quota;
//   2. {id} longer than 100 UTF-16 units once decoded: 414
//      {"error":"URI_TOO_LONG"}, before any hook (no Origin, auth,
//      attestation or quota);
//   3. Origin → bearer → quota (cost 1), as GET /v1/news;
//   4. the id: request.params.id (percent-decoded), String.trim (space,
//      NBSP, U+1680, U+2000–U+200A, U+2028/9, U+202F, U+205F, U+3000, BOM;
//      not NEL or ZWSP), ASCII upper-casing, then
//      /^[0-9A-HJKMNP-TV-Z]{26}$/; otherwise 400 {"error":"BAD_REQUEST",
//      "detail":"malformed news id"}. Non-UTF-8 bytes and any other
//      character are malformed. The query string is ignored.
// Cache: SWR key newsitem:<normalized id>, 300 s fresh and 86,400 s stale
// (TTL.newsItem), durable. The fetch reads the story (unexpired,
// publishedAt + 7 days) and stores its detail, or the literal null for an
// unknown or expired id: a cached null answers 404 until it is stale, and
// a story persisted meanwhile appears only after the stale read's
// recompute; likewise a cached detail outlives its story's expiry and
// keeps a joined member hidden until stale. Stale: served, then recomputed
// after the response. A record that is not a story: 500 {"error":
// "INTERNAL"} (nothing stored).
// Projection (toDetail): the feed item fields, then sources — every member
// in publishedAt order: publisher, url, title, publishedAt, subtitle null,
// abstract (its summary, or null when empty), body null, summary null,
// imageMediaKey (the story's key only for the member whose imageUrl is the
// story's lead image, else null). Stored text as the msgpack round trip
// returns it.
// Answer: 404 {"error":"NOT_FOUND"} for null (cache-control no-store);
// else 200, cache-control private, max-age=300, JSON (ETag
// W/"news-item-…") or NewsItemDetailResponse (W/"news-item-protobuf-…",
// Vary: Accept), If-None-Match → 304.
// Differences (named rules): the router's 404 body for /v1/news/{id}/ and
// other methods; the native parser's 400 for a raw byte that is not UTF-8,
// a malformed or non-UTF-8 escape, or a fragment (Node strips one and
// answers the id); a HEAD 304 without content-length; envelopes survive a restart.
void handle_item(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* route = static_cast<NewsRouteContext*>(opaque);
  if (reject_raw_non_ascii(client, request, head) || reject_long_id(client, request, head)) return;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, route->attestation_required);
  if (policy.status) {
    const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
    http::HttpResponse response{policy.status, policy.reason, kJsonType,
                          reinterpret_cast<const uint8_t*>(body), strlen(body)};
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    WriteHttpResponse(client, response, head);
    return;
  }
  char id[27];
  if (!normalize_id(request, id)) {
    send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\",\"detail\":\"malformed news id\"}",
              head, policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  NewsWorkspace* workspace = route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  Arena arena{workspace->arena, sizeof(workspace->arena)};
  char key[64];
  snprintf(key, sizeof(key), "newsitem:%s", id);
  discovery::DiscoveryCache* cache = workspace->cache;
  const int64_t now = route_now(route);
  const discovery::DiscoveryCacheEntry entry = cache->Read(key, now);
  const uint8_t* payload = nullptr;
  size_t length = 0;
  bool stale = false;
  if (entry.state == discovery::DiscoveryCacheState::kFresh || entry.state == discovery::DiscoveryCacheState::kStale) {
    uint8_t* copy = static_cast<uint8_t*>(arena.Alloc(entry.body_length + 1, 1));
    if (copy) {
      memcpy(copy, entry.body, entry.body_length);
      payload = copy;
      length = entry.body_length;
      stale = entry.state == discovery::DiscoveryCacheState::kStale;
    }
  }
  if (!payload) {
    OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
    if (!build_detail(route, id, now, &arena, out)) {
      lock.Unlock();
      send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head,
                policy.rate_limit_cost, policy.rate_limit_remaining);
      return;
    }
    int64_t as_of = 0;
    cache->Write(key, kTtlNewsItem, now, out.data, out.used, &as_of);
    payload = out.data;
    length = out.used;
  }
  http::HttpResponse response;
  if (length == 4 && memcmp(payload, "null", 4) == 0) {
    // A story that aged out of retention is genuinely gone, not empty.
    response = json_response(404, "Not Found", "{\"error\":\"NOT_FOUND\"}");
  } else {
    response = conditional(request, "news-item", payload, length, detail_proto, workspace, &arena,
                           kItemCache);
  }
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  WriteHttpResponse(client, response, head);
  if (stale && cache->AllowRevalidate(key, now)) {
    Arena again{workspace->arena, sizeof(workspace->arena)};
    OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
    const int64_t at = route_now(route);
    int64_t as_of = 0;
    if (build_detail(route, id, at, &again, out)) cache->Write(key, kTtlNewsItem, at, out.data, out.used, &as_of);
    else cache->RecordRevalidateFailure(key, at, false);
  }
}

}  // namespace

bool FeedQueryService::Feed(const FeedQuery& query, int64_t now, Arena* arena,
                OutBuffer& out) const {
  Cluster* clusters = nullptr;
  size_t count = 0;
  bool found = false;
  int64_t last = 0;
  const StoreStatus status = repository_.FeedSnapshot( query.topics, query.topic_count,
                                                        query.limit, query.has_from, query.from_ms,
                                                        query.before_id[0] ? query.before_id : nullptr,
                                                        now, arena, &clusters, &count,
                                                        &found, &last);
  if (status != StoreStatus::kOk) return false;
  out.Text("{\"items\":[");
  for (size_t i = 0; i < count; ++i) {
    if (i) out.Byte(',');
    out.Byte('{');
    append_item_fields(out, clusters[i], arena);
    out.Byte('}');
  }
  out.Text("],\"asOf\":");
  if (found) append_iso(out, last); else out.Text("null");
  out.Text(",\"stale\":");
  out.Text(!found || now - last > kStaleAfterMs ? "true" : "false");
  out.Byte('}');
  return out.ok && arena->ok;
}
bool FeedQueryService::Detail(const char* id, int64_t now, Arena* arena,
                  OutBuffer& out) const {
  using fortis::news::AppendJsonString;
  Cluster cluster;
  bool found = false;
  if (repository_.Item( id, now, arena, &cluster, &found) != StoreStatus::kOk)
    return false;
  if (!found) { out.Text("null"); return out.ok; }
  out.Byte('{');
  append_item_fields(out, cluster, arena);
  out.Text(",\"sources\":[");
  for (size_t i = 0; i < cluster.article_count; ++i) {
    const fortis::news::Article& a = cluster.articles[i];
    if (i) out.Byte(',');
    out.Text("{\"publisher\":"); AppendJsonString(out, a.publisher);
    out.Text(",\"url\":"); AppendJsonString(out, a.url);
    out.Text(",\"title\":"); AppendJsonString(out, a.title);
    out.Text(",\"publishedAt\":"); append_iso(out, static_cast<int64_t>(a.published_ms));
    out.Text(",\"subtitle\":null,\"abstract\":");
    if (a.summary.length) AppendJsonString(out, a.summary); else out.Text("null");
    out.Text(",\"body\":null,\"summary\":null,\"imageMediaKey\":");
    const bool lead = a.has_image && a.image_url.length && cluster.has_image &&
                      fortis::news::Equal(a.image_url, cluster.image_url);
    if (lead && cluster.has_media_key) append_ascii_string(out, cluster.media_key);
    else out.Text("null");
    out.Byte('}');
  }
  out.Text("]}");
  return out.ok && arena->ok;
}

namespace news_routes_internal {

// ---- replies --------------------------------------------------------------

void send_json(int client, int status, const char* reason, const char* body, bool head,
               int cost, uint32_t remaining) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body),
                        strlen(body)};
  response.rate_limit_cost = cost;
  response.rate_limit_remaining = remaining;
  WriteHttpResponse(client, response, head);
}

http::HttpResponse json_response(int status, const char* reason, const char* body) {
  return http::HttpResponse{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body),
                      strlen(body)};
}

int64_t route_now(const NewsRouteContext* route) {
  return route->clock ? route->clock->NowMs() : route->workspace->cache->Now();
}

// ---- small text helpers -------------------------------------------------------

void append_iso(OutBuffer& out, int64_t ms) {
  char stamp[32];
  if (discovery::DiscoveryIsoTimestamp(ms, stamp)) { out.Byte('"'); out.Text(stamp); out.Byte('"'); }
  else out.Text("null");
}

// encodeURIComponent over UTF-8 (unpaired surrogates cannot occur here).
void append_component(OutBuffer& out, const char* text, size_t length) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' ||
        c == '(' || c == ')') {
      out.Byte(c);
    } else {
      out.Byte('%');
      out.Byte(static_cast<uint8_t>(kHex[c >> 4]));
      out.Byte(static_cast<uint8_t>(kHex[c & 15]));
    }
  }
}

// request.params.id (percent-decoded by the router, which has refused an
// empty segment and a control character/DEL with 404), then the id rule.
// Bytes that are not UTF-8 are malformed. A padded id is trimmed as
// String.prototype.trim does.
bool normalize_id(const http::HttpRequest& request, char output[27]) {
  const http::HttpPathParam* param = HttpPathParamFind(request, "id");
  return param && normalize_id_text(param->value, strlen(param->value), output);
}

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

// An {id} over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"}
// before any hook (no Origin, auth or quota).
bool reject_long_id(int client, const http::HttpRequest& request, bool head) {
  const http::HttpPathParam* param = HttpPathParamFind(request, "id");
  if (!param || param->utf16_units <= 100) return false;
  http::WriteRefusal(client, 414, "URI Too Long", "URI_TOO_LONG", head);
  return true;
}

// sendConditionalJson / sendConditionalProtobuf for a cached JSON payload.
http::HttpResponse conditional(const http::HttpRequest& request, const char* name_space,
                         const uint8_t* payload, size_t length, ProtoFn proto,
                         NewsWorkspace* workspace, Arena* arena, const char* cache_control) {
  http::HttpResponse json_reply{200, "OK", kJsonType, payload, length};
  json_reply.cache_control = cache_control;
  http::HttpResponse proto_reply{200, "OK", kProtobufType, nullptr, 0};
  proto_reply.cache_control = cache_control;
  const bool protobuf = AcceptsProtobuf(request);
  if (protobuf) {
    const json::Value* value = json::Parse(payload, length, arena);
    OutBuffer out{workspace->proto_out, sizeof(workspace->proto_out)};
    if (!value || !proto(*value, arena, out)) return json_response(500, "Internal Server Error",
                                                                    "{\"error\":\"INTERNAL\"}");
    proto_reply.body = out.data;
    proto_reply.body_length = out.used;
  }
  http::HttpResponse response{};
  if (!ConditionalStructuredResponse(request, name_space, json_reply, proto_reply, &response))
    return json_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
  return response;
}

}  // namespace news_routes_internal

bool RegisterNewsRoutes(http::HttpRouter& router, NewsRouteContext* context) {
  if (!context || !context->workspace || !context->handoff || !context->owner ||
      !context->reactions || g_news_reactions) return false;
  context->reactions->route = context;
  g_news_reactions = context->reactions;
  return router.Add("GET", "/v1/news", handle_feed, context) &&
         router.Add("GET", "/v1/news/:id", handle_item, context) &&
         router.Add("GET", "/v1/news/:id/reactions", handle_reactions, context);
}

bool NewsOverviewFeed(const fortis::news::NewsStore& store, discovery::DiscoveryCache* cache, int64_t now_ms,
                      uint8_t* arena_memory, size_t arena_bytes, uint8_t* output, size_t capacity,
                      size_t* length) {
  NewsRouteContext route{};
  route.store = store;
  FeedQuery query{};
  query.topics[0] = Topic::kChains;
  query.topics[1] = Topic::kProtocol;
  query.topics[2] = Topic::kSecurity;
  query.topic_count = 3;
  query.has_topics = true;
  query.limit = 100;
  char key[256];
  variant_key(query, key, sizeof(key));
  const discovery::DiscoveryCacheEntry entry = cache->Read(key, now_ms);
  if (entry.state == discovery::DiscoveryCacheState::kFresh || entry.state == discovery::DiscoveryCacheState::kStale) {
    if (entry.body_length > capacity) return false;
    memcpy(output, entry.body, entry.body_length);
    *length = entry.body_length;
    if (entry.state == discovery::DiscoveryCacheState::kStale && cache->AllowRevalidate(key, now_ms)) {
      Arena again{arena_memory, arena_bytes};
      uint8_t* fresh = arena_memory + arena_bytes / 2;
      OutBuffer out{fresh, arena_bytes - arena_bytes / 2};
      again.capacity = arena_bytes / 2;
      int64_t as_of = 0;
      if (build_feed(&route, query, now_ms, &again, out)) cache->Write(key, kTtlNews, now_ms, out.data, out.used, &as_of);
      else cache->RecordRevalidateFailure(key, now_ms, false);
    }
    return true;
  }
  Arena arena{arena_memory, arena_bytes};
  OutBuffer out{output, capacity};
  if (!build_feed(&route, query, now_ms, &arena, out)) return false;
  int64_t as_of = 0;
  cache->Write(key, kTtlNews, now_ms, out.data, out.used, &as_of);
  *length = out.used;
  return true;
}

namespace news_read_rules {

// ---- pure rule entry points (tests/news_reads_test.cpp) -------------------------
// Pure entry points into the handlers' rules; no route, store or clock.

bool NormalizeId(const char* bytes, size_t length, char output[27]) {
  return normalize_id_text(bytes, length, output);
}

bool FeedQueryOf(const char* raw_target, bool* valid, char* key, size_t key_capacity, char* body,
                 size_t body_capacity) {
  http::HttpRequest request{};
  request.raw_target = const_cast<char*>(raw_target);
  FeedQuery query{};
  if (!parse_feed_query(request, &query)) query.valid = false;
  *valid = query.valid;
  if (query.valid) variant_key(query, key, key_capacity);
  else snprintf(body, body_capacity, "{\"error\":\"BAD_REQUEST\"}");
  return true;
}

}  // namespace news_read_rules

}  // namespace fortis::news
