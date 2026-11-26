#include "news_store.h"
#include "utf8.h"

#include "price_series_store.h"
#include "threading.h"

#include <mbedtls/sha256.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::news {
namespace {

constexpr size_t kBatch = 32;
constexpr size_t kValueCapacity = 1024 * 1024;  // the KV value bound
constexpr size_t kKeyBytes = 160;

// ---- small helpers -------------------------------------------------------

void append_ms(OutBuffer& out, int64_t value) {
  char text[32];
  const int length = snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
  if (length > 0) out.Append(text, static_cast<size_t>(length));
}

// Thirteen digits cover every millisecond time before the year 2286; the
// clamp keeps the key layout fixed (and tells the compiler so).
void pad13(char* output, int64_t value) {
  const int64_t bounded = value < 0 ? 0 : value > 9999999999999LL ? 9999999999999LL : value;
  snprintf(output, 14, "%013lld", static_cast<long long>(bounded));
}

bool u16_ascii(U16 text, char* output, size_t capacity) {
  if (text.length + 1 > capacity) return false;
  for (size_t i = 0; i < text.length; ++i) {
    if (text.data[i] == 0 || text.data[i] > 0x7e || text.data[i] < 0x20) return false;
    output[i] = static_cast<char>(text.data[i]);
  }
  output[text.length] = '\0';
  return true;
}

// sha256 of the article key's UTF-8, as 64 hex characters.
bool article_hash(U16 article_key, Arena* arena, char output[65]) {
  const size_t capacity = article_key.length * 3 + 1;
  uint8_t* bytes = static_cast<uint8_t*>(arena->Alloc(capacity, 1));
  if (!bytes) return false;
  OutBuffer out{bytes, capacity};
  AppendUtf8(out, article_key);
  if (!out.ok) return false;
  unsigned char digest[32];
  if (mbedtls_sha256(bytes, out.used, digest, 0) != 0) return false;
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = kHex[digest[i] >> 4];
    output[i * 2 + 1] = kHex[digest[i] & 15];
  }
  output[64] = '\0';
  return true;
}

bool item_key(U16 id, char output[kKeyBytes]) {
  char ascii[64];
  return u16_ascii(id, ascii, sizeof(ascii)) &&
         snprintf(output, kKeyBytes, "news/item/%s", ascii) < static_cast<int>(kKeyBytes);
}

bool index_key(const char* space, int64_t at, U16 id, char output[kKeyBytes]) {
  char ascii[64], stamp[16];
  if (!u16_ascii(id, ascii, sizeof(ascii))) return false;
  pad13(stamp, at);
  return snprintf(output, kKeyBytes, "news/%s/%s/%s", space, stamp, ascii) <
         static_cast<int>(kKeyBytes);
}

bool owner_key(const char hash[65], char output[kKeyBytes]) {
  return snprintf(output, kKeyBytes, "news/article_owner/%s", hash) < static_cast<int>(kKeyBytes);
}

bool source_key(const Article& article, const char hash[65], char output[kKeyBytes]) {
  char source[96], stamp[16];
  if (!u16_ascii(article.source_id, source, sizeof(source))) return false;
  pad13(stamp, static_cast<int64_t>(article.published_ms));
  return snprintf(output, kKeyBytes, "news/source/%s/%s/%s", source, stamp, hash) <
         static_cast<int>(kKeyBytes);
}

int64_t key_stamp(const char* key, size_t prefix_length) {
  int64_t value = 0;
  for (size_t i = 0; i < 13; ++i) {
    const char c = key[prefix_length + i];
    if (c < '0' || c > '9') return -1;
    value = value * 10 + (c - '0');
  }
  return value;
}

char* arena_cstr(Arena* arena, const char* text) {
  const size_t length = strlen(text);
  char* copy = static_cast<char*>(arena->Alloc(length + 1, 1));
  if (copy) memcpy(copy, text, length + 1);
  return copy;
}

// ---- story JSON ------------------------------------------------------------

size_t text_units(const Cluster& cluster) {
  size_t units = cluster.id.length + cluster.headline.length + cluster.summary.length +
                 cluster.publisher.length + cluster.url.length + cluster.image_url.length;
  for (size_t i = 0; i < cluster.ticker_count; ++i)
    units += cluster.tickers[i].symbol.length + cluster.tickers[i].coin_id.length + 32;
  for (size_t i = 0; i < cluster.article_count; ++i) {
    const Article& a = cluster.articles[i];
    units += a.source_id.length + a.publisher.length + a.external_id.length + a.title.length +
             a.summary.length + a.url.length + a.image_url.length + 160;
  }
  return units;
}

void append_key(OutBuffer& out, const char* key, bool first = false) {
  if (!first) out.Byte(',');
  out.Byte('"');
  out.Text(key);
  out.Text("\":");
}

// New records have one Unicode scalar representation. Legacy records are
// still decoded as stored: existing U+FFFD characters are never collapsed.
void append_stored(OutBuffer& out, U16 text, Arena*) {
  AppendJsonString(out, text);
}

void append_nullable(OutBuffer& out, bool present, U16 text, Arena* arena) {
  if (present) append_stored(out, text, arena);
  else out.Text("null");
}

}  // namespace

bool EncodeCluster(const Cluster& cluster, Arena* arena, const uint8_t** data, size_t* length) {
  // Preserve the conservative 9-bytes-per-unit allocation estimate.
  // The serialized KV value cannot exceed 1 MiB. Keep the conservative
  // expansion estimate, but do not reserve several MiB for a value that the
  // store would reject anyway.
  const size_t units = text_units(cluster);
  if (cluster.topic_count > (kValueCapacity - 1024) / 16) return false;
  const size_t overhead = 1024 + cluster.topic_count * 16;
  const size_t capacity = units > (kValueCapacity - overhead) / 9
                              ? kValueCapacity : units * 9 + overhead;
  uint8_t* buffer = static_cast<uint8_t*>(arena->Alloc(capacity, 1));
  if (!buffer) return false;
  OutBuffer out{buffer, capacity};
  out.Byte('{');
  append_key(out, "id", true); append_stored(out, cluster.id, arena);
  append_key(out, "headline"); append_stored(out, cluster.headline, arena);
  append_key(out, "summary"); append_stored(out, cluster.summary, arena);
  append_key(out, "topics");
  out.Byte('[');
  for (size_t i = 0; i < cluster.topic_count; ++i) {
    if (i) out.Byte(',');
    out.Byte('"'); out.Text(TopicName(cluster.topics[i])); out.Byte('"');
  }
  out.Byte(']');
  append_key(out, "impact"); out.Byte('"'); out.Text(ImpactName(cluster.impact)); out.Byte('"');
  append_key(out, "tickers");
  out.Byte('[');
  for (size_t i = 0; i < cluster.ticker_count; ++i) {
    const Ticker& ticker = cluster.tickers[i];
    if (i) out.Byte(',');
    out.Byte('{');
    append_key(out, "symbol", true); append_stored(out, ticker.symbol, arena);
    append_key(out, "coinId"); append_nullable(out, ticker.has_coin_id, ticker.coin_id, arena);
    out.Byte('}');
  }
  out.Byte(']');
  append_key(out, "publisher"); append_stored(out, cluster.publisher, arena);
  append_key(out, "url"); append_stored(out, cluster.url, arena);
  append_key(out, "imageUrl"); append_nullable(out, cluster.has_image, cluster.image_url, arena);
  append_key(out, "imageMediaKey");
  if (cluster.has_media_key) { out.Byte('"'); out.Text(cluster.media_key); out.Byte('"'); }
  else out.Text("null");
  append_key(out, "publishedAt"); append_ms(out, static_cast<int64_t>(cluster.published_ms));
  append_key(out, "updatedAt"); append_ms(out, static_cast<int64_t>(cluster.updated_ms));
  append_key(out, "articles");
  out.Byte('[');
  for (size_t i = 0; i < cluster.article_count; ++i) {
    const Article& a = cluster.articles[i];
    if (i) out.Byte(',');
    out.Byte('{');
    append_key(out, "sourceId", true); append_stored(out, a.source_id, arena);
    append_key(out, "publisher"); append_stored(out, a.publisher, arena);
    append_key(out, "externalId"); append_stored(out, a.external_id, arena);
    append_key(out, "title"); append_stored(out, a.title, arena);
    append_key(out, "summary"); append_stored(out, a.summary, arena);
    append_key(out, "url"); append_stored(out, a.url, arena);
    append_key(out, "imageUrl"); append_nullable(out, a.has_image, a.image_url, arena);
    append_key(out, "publishedAt"); append_ms(out, static_cast<int64_t>(a.published_ms));
    out.Byte('}');
  }
  out.Byte(']');
  out.Byte('}');
  if (!out.ok || out.used > kValueCapacity) return false;
  *data = buffer;
  *length = out.used;
  return true;
}

namespace {

// A reader for the story JSON above (any member order; unknown members are
// skipped). Strings decode JSON escapes into UTF-16 code units, lone
// surrogate escapes included.
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  Arena* arena;
  bool ok = true;

  void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
  bool eat(uint8_t c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }
  bool peek(uint8_t c) { ws(); return p < end && *p == c; }
  bool literal(const char* text) {
    ws();
    const size_t n = strlen(text);
    if (static_cast<size_t>(end - p) < n || memcmp(p, text, n) != 0) return false;
    p += n;
    return true;
  }
  static int hex(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  bool string(U16* output) {
    ws();
    if (p >= end || *p != '"') return false;
    ++p;
    const uint8_t* start = p;
    size_t capacity = 0;
    while (p < end && *p != '"') { if (*p == '\\') ++p; ++p; ++capacity; }
    if (p >= end) return false;
    p = start;
    uint16_t* units = static_cast<uint16_t*>(arena->Alloc((capacity + 1) * 2, 2));
    if (!units) return false;
    size_t used = 0;
    while (p < end && *p != '"') {
      uint32_t c = *p++;
      if (c == '\\') {
        if (p >= end) return false;
        const uint8_t e = *p++;
        switch (e) {
          case '"': c = '"'; break;
          case '\\': c = '\\'; break;
          case '/': c = '/'; break;
          case 'b': c = 8; break;
          case 'f': c = 12; break;
          case 'n': c = 10; break;
          case 'r': c = 13; break;
          case 't': c = 9; break;
          case 'u': {
            if (end - p < 4) return false;
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
              const int h = hex(p[i]);
              if (h < 0) return false;
              v = v << 4 | static_cast<uint32_t>(h);
            }
            p += 4;
            units[used++] = static_cast<uint16_t>(v);
            continue;
          }
          default: return false;
        }
        units[used++] = static_cast<uint16_t>(c);
        continue;
      }
      // UTF-8 (the encoder only writes well-formed UTF-8).
      uint32_t cp = c;
      size_t extra = 0;
      if (c >= 0xf0) { cp = c & 7; extra = 3; }
      else if (c >= 0xe0) { cp = c & 15; extra = 2; }
      else if (c >= 0xc0) { cp = c & 31; extra = 1; }
      else if (c >= 0x80) return false;
      if (static_cast<size_t>(end - p) < extra) return false;
      for (size_t i = 0; i < extra; ++i) {
        if ((p[i] & 0xc0) != 0x80) return false;
        cp = cp << 6 | (p[i] & 0x3f);
      }
      p += extra;
      if (cp >= 0x10000) {
        cp -= 0x10000;
        units[used++] = static_cast<uint16_t>(0xd800 + (cp >> 10));
        units[used++] = static_cast<uint16_t>(0xdc00 + (cp & 0x3ff));
      } else {
        units[used++] = static_cast<uint16_t>(cp);
      }
    }
    if (p >= end) return false;
    ++p;
    for (size_t i = 0; i < used; ++i) {
      if (units[i] >= 0xd800 && units[i] <= 0xdbff) {
        if (i + 1 < used && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) ++i;
        else units[i] = 0xfffd;
      } else if (units[i] >= 0xdc00 && units[i] <= 0xdfff) units[i] = 0xfffd;
    }
    output->data = units;
    output->length = used;
    return true;
  }
  bool nullable_string(bool* present, U16* output) {
    if (literal("null")) { *present = false; *output = U16{}; return true; }
    *present = true;
    return string(output);
  }
  bool number(double* value) {
    ws();
    char text[48];
    size_t n = 0;
    while (p < end && n + 1 < sizeof(text) &&
           ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' ||
            *p == 'E')) text[n++] = static_cast<char>(*p++);
    if (!n) return false;
    text[n] = '\0';
    char* stop = nullptr;
    *value = strtod(text, &stop);
    return stop == text + n;
  }
  bool skip_value() {
    ws();
    if (p >= end) return false;
    if (*p == '"') { U16 ignored; return string(&ignored); }
    if (*p == '{' || *p == '[') {
      const uint8_t close = *p == '{' ? '}' : ']';
      const bool object = *p == '{';
      ++p;
      if (eat(close)) return true;
      for (;;) {
        if (object) { U16 key; if (!string(&key) || !eat(':')) return false; }
        if (!skip_value()) return false;
        if (eat(close)) return true;
        if (!eat(',')) return false;
      }
    }
    if (literal("null") || literal("true") || literal("false")) return true;
    double ignored;
    return number(&ignored);
  }
  // Iterates object members: calls `member(key)` with the reader positioned
  // at the value. The callback must consume the value.
  template <typename F>
  bool object(F member) {
    if (!eat('{')) return false;
    if (eat('}')) return true;
    for (;;) {
      U16 key;
      if (!string(&key) || !eat(':')) return false;
      if (!member(key)) return false;
      if (eat('}')) return true;
      if (!eat(',')) return false;
    }
  }
  template <typename F>
  bool array(F element) {
    if (!eat('[')) return false;
    if (eat(']')) return true;
    for (;;) {
      if (!element()) return false;
      if (eat(']')) return true;
      if (!eat(',')) return false;
    }
  }
};

// Grows an arena array by doubling (old storage stays in the arena).
template <typename T>
bool push(Arena* arena, T** items, size_t* count, size_t* capacity, const T& value) {
  if (*count == *capacity) {
    const size_t next = *capacity ? *capacity * 2 : 4;
    T* grown = static_cast<T*>(arena->Alloc(next * sizeof(T), alignof(T)));
    if (!grown) return false;
    if (*count) memcpy(static_cast<void*>(grown), *items, *count * sizeof(T));
    *items = grown;
    *capacity = next;
  }
  (*items)[(*count)++] = value;
  return true;
}

bool decode_article(Reader& r, Article* article) {
  *article = Article{};
  return r.object([&](U16 key) {
    double number = 0;
    if (EqualAscii(key, "sourceId")) return r.string(&article->source_id);
    if (EqualAscii(key, "publisher")) return r.string(&article->publisher);
    if (EqualAscii(key, "externalId")) return r.string(&article->external_id);
    if (EqualAscii(key, "title")) return r.string(&article->title);
    if (EqualAscii(key, "summary")) return r.string(&article->summary);
    if (EqualAscii(key, "url")) return r.string(&article->url);
    if (EqualAscii(key, "imageUrl")) return r.nullable_string(&article->has_image, &article->image_url);
    if (EqualAscii(key, "publishedAt")) {
      if (!r.number(&number)) return false;
      article->published_ms = number;
      return true;
    }
    return r.skip_value();
  });
}

}  // namespace

bool DecodeCluster(const uint8_t* data, size_t length, Arena* arena, Cluster* cluster) {
  *cluster = Cluster{};
  if (!codec::utf8::Valid(data, length)) return false;
  Reader r{data, data + length, arena};
  size_t topic_capacity = 0, ticker_capacity = 0, article_capacity = 0;
  bool have_updated = false;
  const bool parsed = r.object([&](U16 key) {
    double number = 0;
    if (EqualAscii(key, "id")) return r.string(&cluster->id);
    if (EqualAscii(key, "headline")) return r.string(&cluster->headline);
    if (EqualAscii(key, "summary")) return r.string(&cluster->summary);
    if (EqualAscii(key, "publisher")) return r.string(&cluster->publisher);
    if (EqualAscii(key, "url")) return r.string(&cluster->url);
    if (EqualAscii(key, "imageUrl")) return r.nullable_string(&cluster->has_image, &cluster->image_url);
    if (EqualAscii(key, "imageMediaKey")) {
      bool present = false;
      U16 value;
      if (!r.nullable_string(&present, &value)) return false;
      cluster->has_media_key = present && value.length == 32;
      if (cluster->has_media_key) {
        for (size_t i = 0; i < 32; ++i) cluster->media_key[i] = static_cast<char>(value.data[i]);
        cluster->media_key[32] = '\0';
      }
      return true;
    }
    if (EqualAscii(key, "impact")) {
      U16 value;
      return r.string(&value) && ImpactFromName(value, &cluster->impact);
    }
    if (EqualAscii(key, "publishedAt")) {
      if (!r.number(&number)) return false;
      cluster->published_ms = number;
      return true;
    }
    if (EqualAscii(key, "updatedAt")) {
      if (!r.number(&number)) return false;
      cluster->updated_ms = number;
      have_updated = true;
      return true;
    }
    if (EqualAscii(key, "topics")) {
      return r.array([&]() {
        U16 value;
        Topic topic;
        return r.string(&value) && TopicFromName(value, &topic) &&
               push(arena, &cluster->topics, &cluster->topic_count, &topic_capacity, topic);
      });
    }
    if (EqualAscii(key, "tickers")) {
      return r.array([&]() {
        Ticker ticker{};
        const bool ok = r.object([&](U16 field) {
          if (EqualAscii(field, "symbol")) return r.string(&ticker.symbol);
          if (EqualAscii(field, "coinId")) return r.nullable_string(&ticker.has_coin_id, &ticker.coin_id);
          return r.skip_value();
        });
        return ok && push(arena, &cluster->tickers, &cluster->ticker_count, &ticker_capacity, ticker);
      });
    }
    if (EqualAscii(key, "articles")) {
      return r.array([&]() {
        Article article;
        return decode_article(r, &article) &&
               push(arena, &cluster->articles, &cluster->article_count, &article_capacity, article);
      });
    }
    return r.skip_value();
  });
  if (!parsed || !arena->ok) return false;
  cluster->has_updated = have_updated;
  // decode(): members ordered by publishedAt ascending (Array sort is stable).
  for (size_t i = 1; i < cluster->article_count; ++i) {
    const Article moving = cluster->articles[i];
    size_t j = i;
    while (j > 0 && cluster->articles[j - 1].published_ms > moving.published_ms) {
      cluster->articles[j] = cluster->articles[j - 1];
      --j;
    }
    cluster->articles[j] = moving;
  }
  return true;
}

namespace {

// ---- reads ----------------------------------------------------------------

// Reads one value into arena memory. now_ms -1 ignores expiry.
StoreStatus read_value(storage::KvStore* kv, const char* key, int64_t now_ms, Arena* arena,
                       const uint8_t** data, size_t* length, bool* found) {
  *found = false;
  size_t size = 0;
  unsigned char probe = 0;
  storage::KvReadStatus status = kv->GetStatus(key, now_ms, &probe, sizeof(probe), &size);
  if (status == storage::KvReadStatus::Missing) return StoreStatus::kOk;
  if (status == storage::KvReadStatus::IoError) return StoreStatus::kIoError;
  uint8_t* buffer = static_cast<uint8_t*>(arena->Alloc(size ? size : 1, 1));
  if (!buffer) return StoreStatus::kNoMemory;
  if (status == storage::KvReadStatus::Found) {
    buffer[0] = probe;
  } else {
    status = kv->GetStatus(key, now_ms, buffer, size, &size);
    if (status == storage::KvReadStatus::Missing) return StoreStatus::kOk;
    if (status != storage::KvReadStatus::Found) return StoreStatus::kIoError;
  }
  *data = buffer;
  *length = size;
  *found = true;
  return StoreStatus::kOk;
}

StoreStatus read_cluster(storage::KvStore* kv, U16 id, int64_t now_ms, Arena* arena, Cluster* cluster,
                         bool* found) {
  char key[kKeyBytes];
  *found = false;
  if (!item_key(id, key)) return StoreStatus::kOk;
  const uint8_t* data = nullptr;
  size_t length = 0;
  const StoreStatus status = read_value(kv, key, now_ms, arena, &data, &length, found);
  if (status != StoreStatus::kOk || !*found) return status;
  if (!DecodeCluster(data, length, arena, cluster)) {
    *found = false;
    return arena->ok ? StoreStatus::kCorrupt : StoreStatus::kNoMemory;
  }
  return StoreStatus::kOk;
}

struct PageEntry {
  char* key;
  uint8_t* value;
  size_t length;
};

struct PageCollect {
  Arena* arena;
  PageEntry* entries;
  size_t count;
  size_t capacity;
  bool ok;
};

bool collect_entry(const char* key, const void* value, size_t size, PageCollect* page) {
  if (!page->ok) return true;
  PageEntry entry{};
  entry.key = arena_cstr(page->arena, key);
  entry.value = static_cast<uint8_t*>(page->arena->Alloc(size ? size : 1, 1));
  entry.length = size;
  if (!entry.key || !entry.value) { page->ok = false; return true; }
  if (size) memcpy(entry.value, value, size);
  if (!push(page->arena, &page->entries, &page->count, &page->capacity, entry)) page->ok = false;
  return true;
}

// One store.page(prefix, 200, now, reverse, after).
StoreStatus page(storage::KvStore* kv, const char* prefix, int64_t now_ms, bool reverse,
                 const char* after, Arena* arena, PageCollect* out, storage::KvPageResult* result) {
  *out = PageCollect{arena, nullptr, 0, 0, true};
  if (!kv->Page(prefix, after, kNewsPageSize, now_ms, reverse, [&, visitor_state = out](const char* key, const void* value, size_t size) { return (collect_entry)(key, value, size, visitor_state); }, result))
    return StoreStatus::kIoError;
  return out->ok ? StoreStatus::kOk : StoreStatus::kNoMemory;
}

U16 entry_id(const PageEntry& entry, Arena* arena) {
  return arena->FromUtf8(reinterpret_cast<const char*>(entry.value), entry.length);
}

bool cluster_has_topic(const Cluster& cluster, Topic topic) {
  for (size_t i = 0; i < cluster.topic_count; ++i) if (cluster.topics[i] == topic) return true;
  return false;
}

// readTimeIndex({topics, limit, since, from, beforeId}). limit 0 means unbounded.
StoreStatus read_time_index(const NewsStore& store, const Topic* topics, size_t topic_count,
                            size_t limit, bool has_since, int64_t since_ms,
                            bool has_from, int64_t from_ms, const char* before_id, int64_t now_ms,
                            Arena* arena, Cluster** clusters, size_t* count) {
  *clusters = nullptr;
  *count = 0;
  size_t capacity = 0;
  static const char kPrefix[] = "news/time/";
  char cursor[1025];
  bool have_cursor = false;
  while (limit == 0 || *count < limit) {
    PageCollect collected;
    storage::KvPageResult result;
    StoreStatus status = page(store.kv, kPrefix, now_ms, true, have_cursor ? cursor : nullptr,
                              arena, &collected, &result);
    if (status != StoreStatus::kOk) return status;
    bool reached_since = false;
    for (size_t i = 0; i < collected.count; ++i) {
      const int64_t at = key_stamp(collected.entries[i].key, sizeof(kPrefix) - 1);
      if (has_since && at < since_ms) { reached_since = true; break; }
      if (has_from && (at > from_ms ||
          (at == from_ms && before_id && collected.entries[i].length == 26 &&
           memcmp(collected.entries[i].value, before_id, 26) >= 0))) continue;
      Cluster cluster;
      bool found = false;
      status = read_cluster(store.kv, entry_id(collected.entries[i], arena), now_ms, arena,
                            &cluster, &found);
      // KvNewsStore.decode() throws on a record that is not a story, which
      // fails the whole read (feed 500, recentClusters rejects): a corrupt
      // record is an error here too, never silently skipped.
      if (status != StoreStatus::kOk) return status;
      if (!found) continue;
      if (topic_count) {
        bool any = false;
        for (size_t t = 0; t < topic_count && !any; ++t) any = cluster_has_topic(cluster, topics[t]);
        if (!any) continue;
      }
      if (!push(arena, clusters, count, &capacity, cluster)) return StoreStatus::kNoMemory;
      if (limit && *count >= limit) break;
    }
    if (reached_since || result.done) break;
    memcpy(cursor, result.next_cursor, sizeof(cursor));
    cursor[sizeof(cursor) - 1] = '\0';
    have_cursor = true;
  }
  return StoreStatus::kOk;
}

// withPrices(): change24hPct from the durable usd spot row of each coin id.
void with_prices(const NewsStore& store, Cluster* clusters, size_t count, int64_t now_ms) {
  for (size_t c = 0; c < count; ++c) {
    for (size_t t = 0; t < clusters[c].ticker_count; ++t) {
      Ticker& ticker = clusters[c].tickers[t];
      ticker.has_change24h = false;
      ticker.change24h_pct = 0;
      if (!ticker.has_coin_id || !store.prices) continue;
      char id[256];
      if (!u16_ascii(ticker.coin_id, id, sizeof(id))) continue;
      prices::PriceSeriesSpot spot{};
      if (store.prices->GetSpot("usd", id, now_ms, &spot) == prices::PriceSeriesReadStatus::kFound &&
          spot.has_change24h_pct) {
        ticker.has_change24h = true;
        ticker.change24h_pct = spot.change24h_pct;
      }
    }
  }
}

// ---- the write transaction --------------------------------------------------

struct TxEntry {
  char* key;
  const uint8_t* value;
  size_t length;
  int64_t expires;
  bool erase;
};

struct Tx {
  storage::KvStore* kv;
  Arena* arena;
  TxEntry* entries = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  bool ok = true;

  // Reads through the overlay. now -1 ignores expiry.
  StoreStatus get(const char* key, int64_t now_ms, const uint8_t** data, size_t* length,
                  bool* found) {
    for (size_t i = count; i-- > 0;) {
      if (strcmp(entries[i].key, key) != 0) continue;
      if (entries[i].erase || (now_ms >= 0 && entries[i].expires && entries[i].expires <= now_ms)) {
        *found = false;
        return StoreStatus::kOk;
      }
      *data = entries[i].value;
      *length = entries[i].length;
      *found = true;
      return StoreStatus::kOk;
    }
    return read_value(kv, key, now_ms, arena, data, length, found);
  }
  void put(const char* key, const uint8_t* value, size_t length, int64_t expires) {
    TxEntry entry{arena_cstr(arena, key), value, length, expires < 0 ? 0 : expires, false};
    if (!entry.key || !push(arena, &entries, &count, &capacity, entry)) ok = false;
  }
  void put_text(const char* key, U16 text, int64_t expires) {
    const size_t capacity_bytes = text.length * 3 + 1;
    uint8_t* bytes = static_cast<uint8_t*>(arena->Alloc(capacity_bytes, 1));
    if (!bytes) { ok = false; return; }
    OutBuffer out{bytes, capacity_bytes};
    AppendUtf8(out, text);
    if (!out.ok) { ok = false; return; }
    put(key, bytes, out.used, expires);
  }
  void erase(const char* key) {
    TxEntry entry{arena_cstr(arena, key), nullptr, 0, 0, true};
    if (!entry.key || !push(arena, &entries, &count, &capacity, entry)) ok = false;
  }
  StoreStatus commit() {
    if (!ok) return StoreStatus::kNoMemory;
    if (!count) return StoreStatus::kOk;
    storage::KvMutation* mutations = static_cast<storage::KvMutation*>(arena->Alloc(count * sizeof(storage::KvMutation)));
    if (!mutations) return StoreStatus::kNoMemory;
    size_t used = 0;
    // The last mutation of a key wins; earlier ones are dropped.
    for (size_t i = 0; i < count; ++i) {
      bool superseded = false;
      for (size_t j = i + 1; j < count && !superseded; ++j)
        superseded = strcmp(entries[i].key, entries[j].key) == 0;
      if (superseded) continue;
      const TxEntry& e = entries[i];
      mutations[used++] = storage::KvMutation{e.key, e.erase ? nullptr : e.value,
                                     static_cast<uint32_t>(e.erase ? 0 : e.length),
                                     e.erase ? 0 : e.expires, e.erase};
    }
    return kv->Write(mutations, used) ? StoreStatus::kOk : StoreStatus::kIoError;
  }
};

StoreStatus tx_get_cluster(Tx& tx, U16 id, Cluster* cluster, bool* found) {
  char key[kKeyBytes];
  *found = false;
  if (!item_key(id, key)) return StoreStatus::kOk;
  const uint8_t* data = nullptr;
  size_t length = 0;
  const StoreStatus status = tx.get(key, -1, &data, &length, found);
  if (status != StoreStatus::kOk || !*found) return status;
  if (!DecodeCluster(data, length, tx.arena, cluster)) {
    *found = false;
    return tx.arena->ok ? StoreStatus::kCorrupt : StoreStatus::kNoMemory;
  }
  return StoreStatus::kOk;
}

bool tx_put_cluster(Tx& tx, const Cluster& cluster, int64_t expires) {
  char key[kKeyBytes];
  const uint8_t* data = nullptr;
  size_t length = 0;
  if (!item_key(cluster.id, key) || !EncodeCluster(cluster, tx.arena, &data, &length)) return false;
  tx.put(key, data, length, expires);
  return tx.ok;
}

StoreStatus tx_get_text(Tx& tx, const char* key, U16* text, bool* found) {
  const uint8_t* data = nullptr;
  size_t length = 0;
  const StoreStatus status = tx.get(key, -1, &data, &length, found);
  if (status == StoreStatus::kOk && *found)
    *text = tx.arena->FromUtf8(reinterpret_cast<const char*>(data), length);
  return status;
}

struct Member {
  U16 key;
  char hash[65];
  Article article;
};

// removeCluster(tx, stored).
StoreStatus remove_cluster(Tx& tx, const Cluster& item) {
  char key[kKeyBytes];
  if (item_key(item.id, key)) tx.erase(key);
  if (index_key("time", static_cast<int64_t>(item.published_ms), item.id, key)) tx.erase(key);
  if (item.has_updated && index_key("updated", static_cast<int64_t>(item.updated_ms), item.id, key))
    tx.erase(key);
  for (size_t i = 0; i < item.article_count; ++i) {
    const Article& article = item.articles[i];
    const U16 article_key = ArticleKey(article.source_id, article.external_id, tx.arena);
    char hash[65];
    if (!tx.arena->ok || !article_hash(article_key, tx.arena, hash)) return StoreStatus::kNoMemory;
    if (source_key(article, hash, key)) tx.erase(key);
    char owner[kKeyBytes];
    if (!owner_key(hash, owner)) continue;
    U16 value;
    bool found = false;
    const StoreStatus status = tx_get_text(tx, owner, &value, &found);
    if (status != StoreStatus::kOk) return status;
    if (found && Equal(value, item.id)) tx.erase(owner);
  }
  return tx.ok ? StoreStatus::kOk : StoreStatus::kNoMemory;
}

StoreStatus persist_one(Tx& tx, const Cluster& incoming, int64_t now) {
  Cluster prior_storage;
  bool have_prior = false;
  StoreStatus status = tx_get_cluster(tx, incoming.id, &prior_storage, &have_prior);
  if (status == StoreStatus::kIoError || status == StoreStatus::kNoMemory) return status;
  const Cluster* prior = have_prior ? &prior_storage : nullptr;
  const double published = prior && prior->published_ms < incoming.published_ms
      ? prior->published_ms : incoming.published_ms;
  const int64_t expires = static_cast<int64_t>(published) + kNewsTtlMs;

  Member* members = nullptr;
  size_t member_count = 0, member_capacity = 0;
  auto find_member = [&](U16 key) -> Member* {
    for (size_t i = 0; i < member_count; ++i) if (Equal(members[i].key, key)) return &members[i];
    return nullptr;
  };
  auto member_for = [&](const Article& article, Member* out) {
    out->key = ArticleKey(article.source_id, article.external_id, tx.arena);
    out->article = article;
    return tx.arena->ok && article_hash(out->key, tx.arena, out->hash);
  };
  if (prior) {
    for (size_t i = 0; i < prior->article_count; ++i) {
      Member member;
      if (!member_for(prior->articles[i], &member)) return StoreStatus::kNoMemory;
      Member* existing = find_member(member.key);
      if (existing) existing->article = member.article;  // Map.set keeps the position
      else if (!push(tx.arena, &members, &member_count, &member_capacity, member))
        return StoreStatus::kNoMemory;
    }
  }
  char key[kKeyBytes];
  for (size_t i = 0; i < incoming.article_count; ++i) {
    Member member;
    if (!member_for(incoming.articles[i], &member)) return StoreStatus::kNoMemory;
    char owner[kKeyBytes];
    if (!owner_key(member.hash, owner)) return StoreStatus::kNoMemory;
    U16 old_owner;
    bool owned = false;
    status = tx_get_text(tx, owner, &old_owner, &owned);
    if (status != StoreStatus::kOk) return status;
    if (owned && old_owner.length && !Equal(old_owner, incoming.id)) {
      Cluster other;
      bool have_other = false;
      status = tx_get_cluster(tx, old_owner, &other, &have_other);
      if (status == StoreStatus::kIoError || status == StoreStatus::kNoMemory) return status;
      if (have_other) {
        size_t kept = 0;
        for (size_t a = 0; a < other.article_count; ++a) {
          const U16 other_key = ArticleKey(other.articles[a].source_id,
                                           other.articles[a].external_id, tx.arena);
          if (!tx.arena->ok) return StoreStatus::kNoMemory;
          if (Equal(other_key, member.key)) {
            // The first matching member's source entry goes (Array.find).
            bool first = true;
            for (size_t b = 0; b < a; ++b) {
              const U16 earlier = ArticleKey(other.articles[b].source_id,
                                             other.articles[b].external_id, tx.arena);
              if (Equal(earlier, member.key)) first = false;
            }
            if (first && source_key(other.articles[a], member.hash, key)) tx.erase(key);
            continue;
          }
          other.articles[kept++] = other.articles[a];
        }
        other.article_count = kept;
        if (!tx_put_cluster(tx, other, static_cast<int64_t>(other.published_ms) + kNewsTtlMs))
          return StoreStatus::kNoMemory;
      }
    }
    Member* existing = find_member(member.key);
    if (existing) {
      if (source_key(existing->article, existing->hash, key)) tx.erase(key);
      existing->article = member.article;
    } else if (!push(tx.arena, &members, &member_count, &member_capacity, member)) {
      return StoreStatus::kNoMemory;
    }
    tx.put_text(owner, incoming.id, expires);
  }
  for (size_t i = 0; i < member_count; ++i) {
    char owner[kKeyBytes];
    if (!owner_key(members[i].hash, owner) || !source_key(members[i].article, members[i].hash, key))
      return StoreStatus::kNoMemory;
    tx.put_text(owner, incoming.id, expires);
    tx.put_text(key, members[i].key, expires);
  }
  Cluster stored = incoming;
  stored.published_ms = published;
  stored.has_updated = true;
  stored.updated_ms = static_cast<double>(now);
  Article* articles = member_count
      ? static_cast<Article*>(tx.arena->Alloc(member_count * sizeof(Article), alignof(Article)))
      : nullptr;
  if (member_count && !articles) return StoreStatus::kNoMemory;
  for (size_t i = 0; i < member_count; ++i) articles[i] = members[i].article;
  stored.articles = articles;
  stored.article_count = member_count;
  if (prior) {
    if (index_key("time", static_cast<int64_t>(prior->published_ms), prior->id, key)) tx.erase(key);
    if (prior->has_updated &&
        index_key("updated", static_cast<int64_t>(prior->updated_ms), prior->id, key))
      tx.erase(key);
  }
  if (!tx_put_cluster(tx, stored, expires)) return StoreStatus::kNoMemory;
  if (!index_key("time", static_cast<int64_t>(published), incoming.id, key))
    return StoreStatus::kNoMemory;
  tx.put_text(key, incoming.id, expires);
  if (!index_key("updated", now, incoming.id, key)) return StoreStatus::kNoMemory;
  tx.put_text(key, incoming.id, expires);
  return tx.ok ? StoreStatus::kOk : StoreStatus::kNoMemory;
}

}  // namespace

StoreStatus StoredNewsRepository::KnownArticleKeys(const char* const* source_ids,
                             size_t source_count, int64_t since_ms, int64_t now_ms,
                             Arena* arena, U16** keys, size_t* key_count) const {
  const NewsStore& store = store_;
  *keys = nullptr;
  *key_count = 0;
  size_t capacity = 0;
  for (size_t s = 0; s < source_count; ++s) {
    char prefix[kKeyBytes];
    if (snprintf(prefix, sizeof(prefix), "news/source/%s/", source_ids[s]) >=
        static_cast<int>(sizeof(prefix))) return StoreStatus::kNoMemory;
    const size_t prefix_length = strlen(prefix);
    char cursor[1025];
    bool have_cursor = false;
    for (;;) {
      PageCollect collected;
      storage::KvPageResult result;
      const StoreStatus status = page(store.kv, prefix, now_ms, true,
                                      have_cursor ? cursor : nullptr, arena, &collected, &result);
      if (status != StoreStatus::kOk) return status;
      bool reached_since = false;
      for (size_t i = 0; i < collected.count; ++i) {
        const int64_t at = key_stamp(collected.entries[i].key, prefix_length);
        if (at < since_ms) { reached_since = true; break; }
        const U16 value = arena->FromUtf8(reinterpret_cast<const char*>(collected.entries[i].value),
                                          collected.entries[i].length);
        if (!arena->ok || !push(arena, keys, key_count, &capacity, value)) return StoreStatus::kNoMemory;
      }
      if (reached_since || result.done) break;
      memcpy(cursor, result.next_cursor, sizeof(cursor));
      cursor[sizeof(cursor) - 1] = '\0';
      have_cursor = true;
    }
  }
  return StoreStatus::kOk;
}

namespace {

// One process-wide story lock. TypeScript's KvNewsStore.persist and prune
// run their write transactions synchronously, so no read on the event loop
// ever observes half of a persist call (some of its 32-story batches, or a
// feed whose lastIngestAt and items come from different sides of it).
// Natively the ingest thread writes while route workers read, so a whole
// persist and each prune batch holds this lock, as do the multi-read news
// snapshots (FeedSnapshot, RecentClusters). Prune yields between its 32-story
// batches in TypeScript. Single-key reads (Item) need no lock: each KV frame
// is atomic.
fortis::base::Mutex g_story_lock;

}  // namespace

StoreStatus StoredNewsRepository::RecentClusters(int64_t since_ms, int64_t now_ms,
                           Arena* arena, Cluster** clusters, size_t* count) const {
  const NewsStore& store = store_;
  fortis::base::LockGuard lock(g_story_lock);
  return read_time_index(store, nullptr, 0, 0, true, since_ms, false, 0, nullptr,
                         now_ms, arena, clusters, count);
}

StoreStatus StoredNewsRepository::Persist(Cluster* const* clusters, size_t count,
                    base::Clock& clock, Arena* arena) const {
  const NewsStore& store = store_;
  fortis::base::LockGuard lock(g_story_lock);
  for (size_t offset = 0; offset < count; offset += kBatch) {
    const size_t mark = arena->used;
    Tx tx{store.kv, arena};
    const size_t stop = offset + kBatch < count ? offset + kBatch : count;
    for (size_t i = offset; i < stop; ++i) {
      const StoreStatus status = persist_one(tx, *clusters[i], clock.NowMs());
      if (status != StoreStatus::kOk) return status;
    }
    const StoreStatus status = tx.commit();
    if (status != StoreStatus::kOk) return status;
    arena->used = mark;  // the batch's scratch is no longer referenced
  }
  return StoreStatus::kOk;
}

StoreStatus StoredNewsRepository::Prune(int64_t before_ms, Arena* arena, size_t* removed) const {
  const NewsStore& store = store_;
  *removed = 0;
  static const char kPrefix[] = "news/time/";
  char cursor[1025];
  bool have_cursor = false;
  for (;;) {
    const size_t mark = arena->used;
    PageCollect collected;
    storage::KvPageResult result;
    StoreStatus status = page(store.kv, kPrefix, -1, false, have_cursor ? cursor : nullptr,
                              arena, &collected, &result);
    if (status != StoreStatus::kOk) return status;
    // TypeScript filters the whole page, so any newer entry ends the walk.
    size_t* old = collected.count
        ? static_cast<size_t*>(arena->Alloc(collected.count * sizeof(size_t))) : nullptr;
    if (collected.count && !old) return StoreStatus::kNoMemory;
    size_t old_count = 0;
    for (size_t i = 0; i < collected.count; ++i)
      if (key_stamp(collected.entries[i].key, sizeof(kPrefix) - 1) < before_ms) old[old_count++] = i;
    const bool reached_newer = old_count < collected.count;
    for (size_t offset = 0; offset < old_count; offset += kBatch) {
      fortis::base::LockGuard lock(g_story_lock);
      Tx tx{store.kv, arena};
      const size_t stop = offset + kBatch < old_count ? offset + kBatch : old_count;
      for (size_t n = offset; n < stop; ++n) {
        const PageEntry& entry = collected.entries[old[n]];
        Cluster cluster;
        bool found = false;
        status = tx_get_cluster(tx, entry_id(entry, arena), &cluster, &found);
        if (status == StoreStatus::kIoError || status == StoreStatus::kNoMemory) return status;
        if (found) {
          status = remove_cluster(tx, cluster);
          if (status != StoreStatus::kOk) return status;
          ++*removed;
        } else {
          tx.erase(entry.key);
        }
      }
      status = tx.commit();
      if (status != StoreStatus::kOk) return status;
    }
    if (reached_newer || result.done) { arena->used = mark; break; }
    memcpy(cursor, result.next_cursor, sizeof(cursor));
    cursor[sizeof(cursor) - 1] = '\0';
    have_cursor = true;
    arena->used = mark;
  }
  return StoreStatus::kOk;
}

StoreStatus StoredNewsRepository::Feed(const Topic* topics, size_t topic_count, size_t limit,
                 int64_t now_ms, Arena* arena, Cluster** clusters, size_t* count) const {
  const NewsStore& store = store_;
  if (limit < 1) limit = 1;
  if (limit > kNewsFeedMaxLimit) limit = kNewsFeedMaxLimit;
  const StoreStatus status = read_time_index(store, topics, topic_count, limit, false, 0,
                                             false, 0, nullptr, now_ms,
                                             arena, clusters, count);
  if (status == StoreStatus::kOk) with_prices(store, *clusters, *count, now_ms);
  return status;
}

StoreStatus StoredNewsRepository::Item(const char* id, int64_t now_ms, Arena* arena,
                 Cluster* cluster, bool* found) const {
  const NewsStore& store = store_;
  const U16 text = arena->FromUtf8(id, strlen(id));
  if (!arena->ok) return StoreStatus::kNoMemory;
  const StoreStatus status = read_cluster(store.kv, text, now_ms, arena, cluster, found);
  if (status == StoreStatus::kOk && *found) with_prices(store, cluster, 1, now_ms);
  return status;
}

StoreStatus StoredNewsRepository::LastIngestAt(int64_t now_ms, bool* found, int64_t* at_ms) const {
  const NewsStore& store = store_;
  *found = false;
  static const char kPrefix[] = "news/updated/";
  struct First {
    bool found = false;
    int64_t at = 0;
  } first;
  char cursor[1025];
  bool have_cursor = false;
  for (;;) {
    storage::KvPageResult result;
    auto visit = [](const char* key, const void*, size_t, void* opaque) {
      auto* state = static_cast<First*>(opaque);
      if (!state->found) {
        state->found = true;
        state->at = key_stamp(key, sizeof(kPrefix) - 1);
      }
      return true;
    };
    if (!store.kv->Page(kPrefix, have_cursor ? cursor : nullptr, kNewsPageSize, now_ms, true, [&, visitor_state = &first](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }, &result)) return StoreStatus::kIoError;
    if (first.found) {
      *found = true;
      *at_ms = first.at;
      return StoreStatus::kOk;
    }
    if (result.done) return StoreStatus::kOk;
    memcpy(cursor, result.next_cursor, sizeof(cursor));
    cursor[sizeof(cursor) - 1] = '\0';
    have_cursor = true;
  }
}

StoreStatus StoredNewsRepository::FeedSnapshot(const Topic* topics, size_t topic_count,
                         size_t limit, bool has_from, int64_t from_ms, const char* before_id,
                         int64_t now_ms, Arena* arena, Cluster** clusters,
                         size_t* count, bool* found, int64_t* last_ingest_ms) const {
  const NewsStore& store = store_;
  if (limit < 1) limit = 1;
  if (limit > kNewsFeedMaxLimit) limit = kNewsFeedMaxLimit;
  {
    fortis::base::LockGuard lock(g_story_lock);
    const StoreStatus status = read_time_index(store, topics, topic_count, limit, false, 0,
                                               has_from, from_ms, before_id, now_ms,
                                               arena, clusters, count);
    if (status != StoreStatus::kOk) return status;
    const StoreStatus ingest_status = fortis::news::LastIngestAt(store, now_ms, found, last_ingest_ms);
    if (ingest_status != StoreStatus::kOk) return ingest_status;
  }
  // Price joins use a separate store and do not affect the consistent story
  // index/last-ingest snapshot. Keep them outside the story write lock.
  with_prices(store, *clusters, *count, now_ms);
  return StoreStatus::kOk;
}

StoreStatus KnownArticleKeys(const NewsStore& store, const char* const* source_ids,
                             size_t source_count, int64_t since_ms, int64_t now_ms,
                             Arena* arena, U16** keys, size_t* key_count) {
  return StoredNewsRepository(store).KnownArticleKeys(source_ids, source_count, since_ms, now_ms, arena, keys, key_count);
}

StoreStatus RecentClusters(const NewsStore& store, int64_t since_ms, int64_t now_ms,
                           Arena* arena, Cluster** clusters, size_t* count) {
  return StoredNewsRepository(store).RecentClusters(since_ms, now_ms, arena, clusters, count);
}

StoreStatus Persist(const NewsStore& store, Cluster* const* clusters, size_t count,
                    base::Clock& clock, Arena* arena) {
  return StoredNewsRepository(store).Persist(clusters, count, clock, arena);
}

StoreStatus Prune(const NewsStore& store, int64_t before_ms, Arena* arena, size_t* removed) {
  return StoredNewsRepository(store).Prune(before_ms, arena, removed);
}

StoreStatus Feed(const NewsStore& store, const Topic* topics, size_t topic_count, size_t limit,
                 int64_t now_ms, Arena* arena, Cluster** clusters, size_t* count) {
  return StoredNewsRepository(store).Feed(topics, topic_count, limit, now_ms, arena, clusters, count);
}

StoreStatus Item(const NewsStore& store, const char* id, int64_t now_ms, Arena* arena,
                 Cluster* cluster, bool* found) {
  return StoredNewsRepository(store).Item(id, now_ms, arena, cluster, found);
}

StoreStatus LastIngestAt(const NewsStore& store, int64_t now_ms, bool* found, int64_t* at_ms) {
  return StoredNewsRepository(store).LastIngestAt(now_ms, found, at_ms);
}

StoreStatus FeedSnapshot(const NewsStore& store, const Topic* topics, size_t topic_count,
                         size_t limit, bool has_from, int64_t from_ms, const char* before_id,
                         int64_t now_ms, Arena* arena, Cluster** clusters,
                         size_t* count, bool* found, int64_t* last_ingest_ms) {
  return StoredNewsRepository(store).FeedSnapshot(topics, topic_count, limit, has_from, from_ms, before_id, now_ms, arena, clusters, count, found, last_ingest_ms);
}

}  // namespace fortis::news
