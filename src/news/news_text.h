#pragma once

#include "price_history_batch_codec.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::networks { class NativeNetworkRegistry; }

// The pure news pipeline of the TypeScript service, over JavaScript strings:
//   feed parsing    parseFeed, decodeEntities, stripTags, collapse
//   feed entries    RssNewsSource.toArticle (usable/recent entries)
//   summaries       summarize
//   classification  classifyTopics
//   tickers         buildTickerDictionary, extractTickers
//   clustering      clusterArticles, canonicalUrl and similarity
//   impact          scoreImpact
//   article keys    articleKey
// Strings are UTF-16 code-unit arrays exactly as V8 holds them (lone
// surrogates included), so lengths, slices and regular expressions match
// JavaScript. Regular expressions run on src/compat/js_regex.cpp with ECMAScript
// non-unicode semantics. Dates follow V8's `new Date(string)` parser.
// No C++ standard library. Working memory comes from the caller's Arena;
// the only process-global state is the regular-expression programs, compiled
// once on first use, and one mutex-guarded URL scratch for CanonicalUrl.
// Guest-safe and thread-safe per arena. Callers need about 64 KiB of stack.
namespace fortis::news {

using codec::history_batch::OutBuffer;

struct U16 {
  const uint16_t* data = nullptr;
  size_t length = 0;
};

// Bump allocator over caller memory. A failed allocation sets ok = false and
// returns null; callers check `ok` once at the end of a pipeline step.
struct Arena {
  uint8_t* base = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  bool ok = true;
  // A regular expression exhausted its step or frame budget (treated as no
  // match, where JavaScript would keep searching); diagnostics only.
  bool regex_budget = false;
  void* Alloc(size_t bytes, size_t align = 8);
  U16 Copy(U16 text);
  U16 FromUtf8(const char* text, size_t length);  // invalid UTF-8 -> U+FFFD (TextDecoder)
};

// UTF-16 -> UTF-8 (lone surrogates -> U+FFFD, as TextEncoder and protobuf-es).
bool AppendUtf8(OutBuffer& out, U16 text);
// JSON.stringify(string) (well-formed: lone surrogates as \udXXX escapes).
void AppendJsonString(OutBuffer& out, U16 text);
bool Equal(U16 a, U16 b);
bool EqualAscii(U16 a, const char* ascii);
// Strict code-unit order (JavaScript `<` on strings).
int Compare(U16 a, U16 b);

enum class Topic : uint8_t { kTrend, kMacro, kChains, kPolicy, kSecurity, kFlows, kProtocol };
constexpr size_t kTopicCount = 7;
const char* TopicName(Topic topic);  // "trend", "macro", ...
bool TopicFromName(U16 name, Topic* topic);

enum class Impact : uint8_t { kNormal, kWatch, kBreaking };
const char* ImpactName(Impact impact);  // "normal", "watch", "breaking"
bool ImpactFromName(U16 name, Impact* impact);

// ---- feed parsing ------------------------------------------------------
constexpr size_t kMaxFeedUnits = 4 * 1024 * 1024;  // MAX_FEED_BYTES (UTF-16 units)
constexpr size_t kMaxFeedEntries = 100;              // MAX_ENTRIES

struct FeedEntry {
  bool has_guid = false;
  U16 guid;
  U16 title;
  bool has_link = false;
  U16 link;
  bool has_summary_html = false;
  U16 summary_html;
  bool has_published = false;
  double published_ms = 0;  // Date value; finite when has_published
  bool has_author = false;
  U16 author;
  U16* categories = nullptr;
  size_t category_count = 0;
  bool has_image = false;
  U16 image_url;
};

// parseFeed(xml). Writes at most kMaxFeedEntries entries in document order.
// False only when the arena overflowed (the caller treats it as a failed
// feed); an oversized or entry-less document is an empty success.
bool ParseFeed(U16 xml, Arena* arena, FeedEntry* entries, size_t capacity, size_t* count);

U16 DecodeEntities(U16 text, Arena* arena);
U16 StripTags(U16 text, Arena* arena);
U16 Collapse(U16 text, Arena* arena);

// V8 `new Date(text).getTime()`; false for NaN.
bool ParseJsDate(U16 text, double* epoch_ms);

// ---- articles, clusters --------------------------------------------------
struct Article {
  U16 source_id;
  U16 publisher;
  double trust = 0;
  U16 external_id;
  U16 title;
  U16 summary;
  U16 url;
  bool has_image = false;
  U16 image_url;
  double published_ms = 0;
  U16* categories = nullptr;  // lowercased publisher labels (ingest input only)
  size_t category_count = 0;
  Topic* declared_topics = nullptr;
  size_t declared_topic_count = 0;
};

struct Ticker {
  U16 symbol;
  bool has_coin_id = false;
  U16 coin_id;
  // Joined at read time from the price store; never persisted.
  bool has_change24h = false;
  double change24h_pct = 0;
};

struct Cluster {
  U16 id;
  U16 headline;
  U16 summary;
  Topic* topics = nullptr;
  size_t topic_count = 0;
  Impact impact = Impact::kNormal;
  Ticker* tickers = nullptr;
  size_t ticker_count = 0;
  U16 publisher;
  U16 url;
  bool has_image = false;
  U16 image_url;
  bool has_media_key = false;   // imageMediaKey !== undefined && !== null
  char media_key[33]{};
  double published_ms = 0;
  bool has_updated = false;
  double updated_ms = 0;
  Article* articles = nullptr;
  size_t article_count = 0;
};

// The feed registry row (the TypeScript NEWS_FEEDS).
struct FeedRegistration {
  const char* id;
  const char* publisher;
  const char* url;
  const Topic* declared_topics;
  size_t declared_topic_count;
  double trust;
};
extern const FeedRegistration kNewsFeeds[];
extern const size_t kNewsFeedCount;

// RssNewsSource.toArticle: null (false) for an entry with no link, an empty
// title, no publish time, older than MAX_ENTRY_AGE_MS or more than an hour in
// the future. The externalId falls back to CanonicalUrl(link).
bool ToArticle(const FeedRegistration& feed, const FeedEntry& entry, double now_ms,
               Arena* arena, Article* output);

// JSON.stringify([sourceId, externalId]).
U16 ArticleKey(U16 source_id, U16 external_id, Arena* arena);

// ---- summaries, classification, tickers, impact ---------------------------
U16 Summarize(bool has_html, U16 html, U16 fallback_title, Arena* arena);

constexpr size_t kMaxTopics = 3;
// classifyTopics; returns the count (1..3) written to `output`. `arena`
// holds temporaries only (lower-cased labels, the joined text).
size_t ClassifyTopics(U16 title, U16 summary, const U16* categories, size_t category_count,
                      const Topic* declared, size_t declared_count, Topic output[kMaxTopics],
                      Arena* arena);

struct TickerEntry {
  U16 symbol;       // upper-cased, trimmed (never empty)
  bool has_coin_id;
  U16 coin_id;
  bool primary;     // the first registration of this symbol (bySymbol's value)
};
struct TickerAlias {
  U16 alias;        // trimmed, lower-cased
  size_t entry;     // index into entries: the registration that added it
};
// buildTickerDictionary: `entries` holds every normalized registration in
// order (bySymbol maps a symbol to its primary entry) and `aliases` keeps the
// byAlias Map insertion order (first registration of an alias wins).
struct TickerDictionary {
  TickerEntry* entries = nullptr;
  size_t entry_count = 0;
  TickerAlias* aliases = nullptr;
  size_t alias_count = 0;
};
// The TypeScript service's composition-time dictionary: non-testnet networks
// with a native coin, symbol + coingeckoId, aliases [coingeckoId, network id].
bool BuildTickerDictionary(const networks::NativeNetworkRegistry& registry, Arena* arena,
                           TickerDictionary* output);

constexpr size_t kMaxTickers = 4;
// The output symbols/coin ids point into the dictionary.
size_t ExtractTickers(U16 text, const TickerDictionary& dictionary, Ticker output[kMaxTickers],
                      Arena* arena);

Impact ScoreImpact(const Cluster& cluster, double now_ms, Arena* arena);

// ---- clustering --------------------------------------------------------
constexpr double kClusterWindowMs = 12.0 * 60 * 60 * 1000;
constexpr size_t kMaxClusterMembers = 8;

struct ClusterInput {
  Article article;  // summary already derived
  Topic topics[kMaxTopics];
  size_t topic_count = 0;
  Ticker tickers[kMaxTickers];
  size_t ticker_count = 0;
  U16 summary;
};

// clusterArticles(inputs, { existing, newId }): deep-copies `existing` into
// the arena (they are never modified) and returns the created or changed
// stories in TypeScript order — existing stories first in their given order,
// then new stories in creation order.
// `new_id` mints the id of a story that turns out to be new (the TypeScript
// `ulid`). A new_id failure or arena exhaustion returns false. `existing_touched`
// (existing_count flags, may be null) marks the existing stories returned.
bool ClusterArticles(const ClusterInput* inputs, size_t input_count, const Cluster* existing,
                     size_t existing_count, base::IdSource& new_id,
                     Arena* arena, Cluster** output, size_t* output_count,
                     bool* existing_touched);

// canonicalUrl(url), including its `url.trim().toLowerCase()` fallback.
U16 CanonicalUrl(U16 url, Arena* arena);

// String.prototype.toLowerCase (lone surrogates unchanged, Final_Sigma).
U16 JsToLowerCase(U16 text, Arena* arena);
// String.prototype.trim (ECMAScript WhiteSpace and LineTerminator).
U16 JsTrim(U16 text);
// The TypeScript sourceCount:
//   Math.max(new Set(articles.map(a => a.publisher.toLowerCase())).size, 1)
size_t SourceCount(const Cluster& cluster, Arena* arena);
// The TypeScript NEWS_TOPICS order (isNewsTopic).
extern const Topic kTopicOrder[kTopicCount];
// Compiles every regular expression once; false if one is refused (a build
// error).
bool NewsTextInitialize();

// The verbatim TypeScript pattern tables CATEGORY_TOPICS, TEXT_RULES,
// WATCH_PATTERNS and BOILERPLATE: UTF-8 `.source`
// and `.flags`, with the topic for the two classifier tables. For the
// corpus test, which compares them with the TypeScript values.
enum class PatternTable : uint8_t { kCategory, kText, kWatch, kBoilerplate };
size_t PatternCount(PatternTable table);
bool PatternAt(PatternTable table, size_t index, const char** source, const char** flags,
               Topic* topic);

}  // namespace fortis::news
