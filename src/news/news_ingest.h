#pragma once

#include "news_store.h"
#include "news_text.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// The TypeScript NewsIngester.run over the six registered feeds:
//   fetch -> skip what we have -> summarize -> classify -> tag -> cluster
//         -> score impact -> register images -> persist -> prune
// Feeds are fetched one after another through `fetch` (TypeScript fetches
// them concurrently; only the run's duration differs). Each document is
// decoded (Response.text()), parsed and summarized in a scratch arena that is
// reset per feed; the usable articles are deep-copied into the run arena with
// their derived summary (summarize() is pure, so deriving it before the
// dedupe changes nothing observable). A fetch failure, an oversized document,
// a scratch overflow, or a feed whose articles would take the run arena past
// kFeedArticleBudgetBytes costs that feed's articles only — the
// RssNewsSource "never throws" contract.
namespace fortis::news {

// Native bound on one feed document (TypeScript reads the whole body and
// parseFeed then refuses more than 4 Mi UTF-16 units). The six live feeds
// measure 15-53 KiB.
constexpr size_t kMaxFeedBodyBytes = 1024 * 1024;
// Per-feed scratch: the UTF-16 document (2 bytes per byte) plus parse and
// summary output. The largest live feed peaked at 309 KiB.
constexpr size_t kFeedScratchBytes = 6 * 1024 * 1024;
// The run arena: copied articles, store reads, clustering and persistence.
// A run over the six live feeds (115 articles, 114 stories) peaked at
// 1.43 MiB. Copied articles may take at most half of it.
constexpr size_t kIngestArenaBytes = 8 * 1024 * 1024;
constexpr size_t kFeedArticleBudgetBytes = kIngestArenaBytes / 2;
// The TypeScript NEWS_RETENTION_MS and KNOWN_WINDOW_MS.
constexpr int64_t kNewsRetentionMs = 7LL * 24 * 3600000;
constexpr int64_t kKnownWindowMs = 14LL * 24 * 3600000;

struct IngestResult {
  size_t fetched = 0;  // articles the sources handed over
  size_t fresh = 0;    // of those, not already stored
  size_t stored = 0;   // stories written
  size_t pruned = 0;   // stories dropped by retention
  size_t feeds_failed = 0;  // native diagnostic: feeds that yielded nothing on error
  bool persist_failed = false;
  bool prune_failed = false;
  // High-water marks of the run arena and the largest feed's scratch, so the
  // fixed transient buffers can be sized from measurements.
  size_t run_arena_used = 0;
  size_t scratch_peak = 0;
};

// Fetches one feed document into `body` (at most `capacity` bytes). False is
// a failed fetch (logged by the caller's transport); TypeScript's source then
// yields no articles.
using FeedFetchFn = bool (*)(void* context, const FeedRegistration& feed, uint8_t* body,
                             size_t capacity, size_t* length);
// Optional diagnostics sink (never given secrets or URLs with credentials).
using IngestLogFn = void (*)(void* context, const char* message);

struct IngestDeps {
  NewsStore store{};
  storage::KvStore* media_kv = nullptr;  // MediaFacet.register target; null registers nothing
  const TickerDictionary* dictionary = nullptr;
  FeedFetchFn fetch = nullptr;
  void* fetch_context = nullptr;
  base::Clock* clock = nullptr;  // NowMs: epoch ms
  base::IdSource* new_id = nullptr;  // null: a fresh ULID from the clock and CSPRNG
  IngestLogFn log = nullptr;
  void* log_context = nullptr;
  NewsRepository* repository = nullptr;  // borrowed; null adapts store
};

class NewsIngestor {
 public:
  explicit NewsIngestor(const IngestDeps& deps) : deps_(deps) {}
  bool Run(uint8_t* run_memory, uint8_t* scratch, uint8_t* body, IngestResult* result) const;
 private:
  IngestDeps deps_;
};

// One NewsIngester.run(). `run_arena` (kIngestArenaBytes), `scratch`
// (kFeedScratchBytes) and `body` (kMaxFeedBodyBytes) are caller memory, so
// a run allocates nothing. False only when the run itself could not proceed
// (no clock, a run-arena overflow or a store read failure before persist);
// a persist or prune failure is reported in `result` like TypeScript's
// logged-and-returned outcomes.
bool RunIngest(const IngestDeps& deps, uint8_t* run_memory, uint8_t* scratch, uint8_t* body,
               IngestResult* result);

// ulid(): 48-bit millisecond time and 80 CSPRNG bits.
bool NewNewsId(int64_t now_ms, char output[27]);

}  // namespace fortis::news
