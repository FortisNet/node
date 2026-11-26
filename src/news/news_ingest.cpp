#include "news_ingest.h"

#include "crash_report_codec.h"
#include "device_auth.h"
#include "market_view.h"
#include "media_registry.h"

#include <stdio.h>
#include <string.h>

namespace fortis::news {
namespace {

void log(const IngestDeps& deps, const char* message) {
  if (deps.log) deps.log(deps.log_context, message);
}

// Deep copy of one article out of a feed's scratch arena.
bool copy_article(const Article& in, Arena* keep, Article* out) {
  *out = Article{};
  out->source_id = keep->Copy(in.source_id);
  out->publisher = keep->Copy(in.publisher);
  out->trust = in.trust;
  out->external_id = keep->Copy(in.external_id);
  out->title = keep->Copy(in.title);
  out->summary = keep->Copy(in.summary);
  out->url = keep->Copy(in.url);
  out->has_image = in.has_image;
  if (in.has_image) out->image_url = keep->Copy(in.image_url);
  out->published_ms = in.published_ms;
  if (in.category_count) {
    out->categories = static_cast<U16*>(keep->Alloc(in.category_count * sizeof(U16), alignof(U16)));
    if (out->categories) {
      for (size_t i = 0; i < in.category_count; ++i) out->categories[i] = keep->Copy(in.categories[i]);
      out->category_count = in.category_count;
    }
  }
  if (in.declared_topic_count) {
    out->declared_topics =
        static_cast<Topic*>(keep->Alloc(in.declared_topic_count * sizeof(Topic), alignof(Topic)));
    if (out->declared_topics) {
      memcpy(out->declared_topics, in.declared_topics, in.declared_topic_count * sizeof(Topic));
      out->declared_topic_count = in.declared_topic_count;
    }
  }
  return keep->ok;
}

// One RssNewsSource.fetch(): appends the feed's usable, recent articles.
// False only for a run-arena overflow; a failed feed adds nothing.
bool fetch_feed(const IngestDeps& deps, const FeedRegistration& feed, double now, Arena* keep,
                uint8_t* scratch_memory, uint8_t* body, Article* articles, size_t capacity,
                size_t* count, bool* failed, size_t* scratch_peak) {
  *failed = false;
  size_t length = 0;
  if (!deps.fetch(deps.fetch_context, feed, body, kMaxFeedBodyBytes, &length)) {
    *failed = true;
    return true;
  }
  if (length == 0) return true;  // `if (xml === '') return []`
  Arena scratch{scratch_memory, kFeedScratchBytes};
  struct Peak {
    Arena* arena;
    size_t* peak;
    ~Peak() { if (arena->used > *peak) *peak = arena->used; }
  } peak{&scratch, scratch_peak};
  const U16 xml = scratch.FromUtf8(reinterpret_cast<const char*>(body), length);
  if (!scratch.ok) {
    *failed = true;
    return true;
  }
  FeedEntry entries[kMaxFeedEntries];
  size_t entry_count = 0;
  if (!ParseFeed(xml, &scratch, entries, kMaxFeedEntries, &entry_count)) {
    *failed = true;
    return true;
  }
  const size_t keep_mark = keep->used;
  const size_t count_mark = *count;
  for (size_t i = 0; i < entry_count; ++i) {
    Article article;
    if (!ToArticle(feed, entries[i], now, &scratch, &article)) {
      if (!scratch.ok) { *failed = true; *count = count_mark; keep->used = keep_mark; return true; }
      continue;
    }
    // RawArticle.summary is `summaryHtml ?? ''`; the ingester's summarize().
    article.summary = Summarize(true, article.summary, article.title, &scratch);
    if (!scratch.ok || *count == capacity || !copy_article(article, keep, &articles[*count]) ||
        keep->used > kFeedArticleBudgetBytes) {
      // This feed alone costs its articles; the run continues without them.
      *failed = true;
      *count = count_mark;
      keep->used = keep_mark;
      keep->ok = true;
      return true;
    }
    ++*count;
  }
  return true;
}

bool contains(const U16* keys, size_t count, U16 key) {
  for (size_t i = 0; i < count; ++i)
    if (Equal(keys[i], key)) return true;
  return false;
}

// A fresh ULID at the ingest clock.
class DefaultIds final : public base::IdSource {
 public:
  explicit DefaultIds(base::Clock* clock) : clock_(clock) {}
  bool NewId(char output[kIdBytes]) override { return NewNewsId(clock_->NowMs(), output); }

 private:
  base::Clock* clock_;
};

// attachImages(): register each story's lead image and record its key; a
// non-http(s) URL gets no key (null), a KV failure still yields the key.
void attach_images(const IngestDeps& deps, Cluster* const* clusters, size_t count, int64_t now,
                   Arena* arena) {
  for (size_t i = 0; i < count; ++i) {
    Cluster& cluster = *clusters[i];
    if (!cluster.has_image) continue;
    const size_t capacity = cluster.image_url.length * 3 + 1;
    uint8_t* url = static_cast<uint8_t*>(arena->Alloc(capacity, 1));
    if (!url) { cluster.has_media_key = false; continue; }
    OutBuffer out{url, capacity};
    AppendUtf8(out, cluster.image_url);
    char key[discovery::market_view::kMediaKeyChars + 1];
    if (out.ok && discovery::RegisterMediaSource(deps.media_kv, reinterpret_cast<const char*>(url), out.used,
                                      now, key)) {
      cluster.has_media_key = true;
      memcpy(cluster.media_key, key, sizeof(cluster.media_key));
    } else {
      cluster.has_media_key = false;
      cluster.media_key[0] = '\0';
    }
  }
}

}  // namespace

bool NewNewsId(int64_t now_ms, char output[27]) {
  uint8_t random[10];
  return fortis::auth::SecureRandomBytes(random, sizeof(random)) &&
         codec::crash_report::NewUlid(now_ms, random, output);
}

bool NewsIngestor::Run(uint8_t* run_memory, uint8_t* scratch, uint8_t* body, IngestResult* result) const {
  const IngestDeps& deps = deps_;
  StoredNewsRepository stored(deps.store);
  NewsRepository* repository = deps.repository ? deps.repository : &stored;
  *result = IngestResult{};
  if (!deps.clock || !deps.fetch || !deps.dictionary || !run_memory || !scratch || !body) return false;
  Arena arena{run_memory, kIngestArenaBytes};
  const int64_t now = deps.clock->NowMs();

  // Promise.all(sources.map(fetch)).flat(), in registry order.
  constexpr size_t kMaxArticles = kMaxFeedEntries * 6;
  auto* articles = static_cast<Article*>(arena.Alloc(sizeof(Article) * kMaxArticles));
  if (!articles) { log(deps, "news ingest: article array allocation failed"); return false; }
  size_t article_count = 0;
  for (size_t f = 0; f < kNewsFeedCount; ++f) {
    bool failed = false;
    if (!fetch_feed(deps, kNewsFeeds[f], static_cast<double>(now), &arena, scratch, body, articles,
                    kMaxArticles, &article_count, &failed, &result->scratch_peak)) {
      log(deps, "news ingest: run arena exhausted while copying feed articles");
      return false;
    }
    if (failed) {
      ++result->feeds_failed;
      char message[96];
      snprintf(message, sizeof(message), "news feed %s fetch failed", kNewsFeeds[f].id);
      log(deps, message);
    }
  }
  result->fetched = article_count;

  const char* source_ids[16];
  for (size_t f = 0; f < kNewsFeedCount; ++f) source_ids[f] = kNewsFeeds[f].id;
  U16* known = nullptr;
  size_t known_count = 0;
  if (repository->KnownArticleKeys( source_ids, kNewsFeedCount, now - kKnownWindowMs, now, &arena,
                       &known, &known_count) != StoreStatus::kOk) {
    log(deps, "news ingest: known-article read failed");
    return false;
  }

  // Both dedupe passes: `known` (previous runs) and `seen` (this run).
  auto* inputs = static_cast<ClusterInput*>(arena.Alloc(sizeof(ClusterInput) * (article_count + 1)));
  auto* seen = static_cast<U16*>(arena.Alloc(sizeof(U16) * (article_count + 1)));
  if (!inputs || !seen) { log(deps, "news ingest: dedupe array allocation failed"); return false; }
  size_t input_count = 0;
  size_t seen_count = 0;
  for (size_t i = 0; i < article_count; ++i) {
    const Article& article = articles[i];
    const U16 key = ArticleKey(article.source_id, article.external_id, &arena);
    if (!arena.ok) { log(deps, "news ingest: article key allocation failed"); return false; }
    if (contains(known, known_count, key) || contains(seen, seen_count, key)) continue;
    seen[seen_count++] = key;
    ClusterInput& input = inputs[input_count];
    input = ClusterInput{};
    const U16 summary = article.summary;  // derived per feed (fetch_feed)
    input.topic_count = ClassifyTopics(article.title, summary, article.categories,
                                       article.category_count, article.declared_topics,
                                       article.declared_topic_count, input.topics, &arena);
    // `${article.title}. ${summary}`
    const size_t text_length = article.title.length + 2 + summary.length;
    auto* text = static_cast<uint16_t*>(arena.Alloc(text_length * 2 + 2, 2));
    if (!text) { log(deps, "news ingest: classification text allocation failed"); return false; }
    memcpy(text, article.title.data, article.title.length * 2);
    text[article.title.length] = '.';
    text[article.title.length + 1] = ' ';
    if (summary.length) memcpy(text + article.title.length + 2, summary.data, summary.length * 2);
    input.ticker_count = ExtractTickers(U16{text, text_length}, *deps.dictionary, input.tickers,
                                        &arena);
    input.article = article;
    input.article.summary = summary;
    input.summary = summary;
    ++input_count;
    if (!arena.ok) { log(deps, "news ingest: classification allocation failed"); return false; }
  }
  result->fresh = input_count;

  Cluster* existing = nullptr;
  size_t existing_count = 0;
  if (repository->RecentClusters( now - static_cast<int64_t>(kClusterWindowMs), now, &arena,
                     &existing, &existing_count) != StoreStatus::kOk) {
    log(deps, "news ingest: recent-story read failed");
    return false;
  }
  DefaultIds default_ids(deps.clock);
  base::IdSource* new_id = deps.new_id ? deps.new_id : &default_ids;
  Cluster* changed = nullptr;
  size_t changed_count = 0;
  bool* touched = static_cast<bool*>(arena.Alloc(existing_count + 1, 1));
  if (!touched || !ClusterArticles(inputs, input_count, existing, existing_count, *new_id,
                                   &arena, &changed, &changed_count, touched)) {
    log(deps, "news ingest: clustering failed");
    return false;
  }

  // rescoreUntouched + scoreImpact over everything persisted.
  auto* persist = static_cast<Cluster**>(
      arena.Alloc(sizeof(Cluster*) * (changed_count + existing_count + 1)));
  if (!persist) { log(deps, "news ingest: persistence array allocation failed"); return false; }
  size_t persist_count = 0;
  for (size_t i = 0; i < changed_count; ++i) persist[persist_count++] = &changed[i];
  for (size_t i = 0; i < existing_count; ++i) {
    if (touched[i]) continue;
    if (ScoreImpact(existing[i], static_cast<double>(now), &arena) != existing[i].impact)
      persist[persist_count++] = &existing[i];
  }
  for (size_t i = 0; i < persist_count; ++i)
    persist[i]->impact = ScoreImpact(*persist[i], static_cast<double>(now), &arena);
  attach_images(deps, persist, persist_count, now, &arena);
  if (!arena.ok) {
    log(deps, "news ingest: run arena exhausted");
    return false;
  }

  result->run_arena_used = arena.used;
  if (persist_count) {
    if (repository->Persist( persist, persist_count, *deps.clock, &arena) !=
        StoreStatus::kOk) {
      log(deps, "news persist failed");
      result->persist_failed = true;
      return true;
    }
  }
  result->stored = persist_count;

  size_t removed = 0;
  const StoreStatus pruned = repository->Prune( now - kNewsRetentionMs, &arena, &removed);
  if (arena.used > result->run_arena_used) result->run_arena_used = arena.used;
  if (pruned != StoreStatus::kOk) {
    log(deps, "news prune failed");
    result->prune_failed = true;
  } else {
    result->pruned = removed;
  }
  return true;
}

bool RunIngest(const IngestDeps& deps, uint8_t* run_memory, uint8_t* scratch, uint8_t* body,
               IngestResult* result) {
  return NewsIngestor(deps).Run(run_memory, scratch, body, result);
}

}  // namespace fortis::news
