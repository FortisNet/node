#pragma once
#include <memory>
#include "media_service.h"
#include "threading.h"
#include <atomic>
namespace fortis::news {
namespace media_components {
constexpr size_t kWaitersPerJob = 8;
enum class Verdict { kFetch, kPermanent, kTransient, kCancelled };
enum class Fetched { kStored, kRefused, kCancelled };
  struct Waiter {
    uint64_t token;
    bool head;
  };
  struct Job {
    bool used = false;
    bool running = false;
    char key[33]{};
    Waiter waiters[kWaitersPerJob]{};
    size_t waiter_count = 0;
    uint64_t sequence = 0;
  };
  struct Touch {
    char key[33]{};
    int64_t written_at = 0;
    uint64_t pending = 0;
    uint64_t order = 0;
    bool used = false;
  };
  // Bytes borrow the fetcher buffer until its next job; callers never free them.
  struct Outcome {
    bool found = false;
    const char* content_type = nullptr;
    unsigned char* bytes = nullptr;
    size_t length = 0;
  };
  struct HopResult {
    Verdict verdict = Verdict::kPermanent;
    bool redirect = false;
    const char* content_type = nullptr;
    size_t length = 0;
  };
  struct KeyList {
    char (*keys)[128];
    size_t count;
    size_t capacity;
  };
}  // namespace media_components
class MediaRepository {
 public:
  using Impl = MediaRuntime;
  using Touch = media_components::Touch;
  explicit MediaRepository(Impl& root) : root_(root) {}
  MediaRepository(const MediaRepository&) = delete;
  MediaRepository& operator=(const MediaRepository&) = delete;
  bool ReadRecord(const char* key, int64_t now, unsigned char* buffer, char* url, MediaRecord* record,
                  bool* present);
  bool WriteRecord(const MediaRecord& record, int64_t now, const unsigned char* bytes,
                   bool write_chunks, bool erase_chunks);
  bool ReadBlob(const MediaRecord& record, int64_t now, unsigned char* output);
  void MarkFailed(const char* key, bool permanent);
  bool StoreOk(const char* key, const unsigned char* bytes, size_t length, const char* type);
  void TouchKey(const char* key);
 private:
  Impl& root_;
  fortis::base::Mutex touch_mutex;
  Touch touches[kMediaTouchEntries]{};
  uint64_t touch_order = 1;
};
class MediaFetcher {
 public:
  using Impl = MediaRuntime;
  using Outcome = media_components::Outcome;
  using HopResult = media_components::HopResult;
  using Verdict = media_components::Verdict;
  using Fetched = media_components::Fetched;
  explicit MediaFetcher(Impl& root) : root_(root) {}
  MediaFetcher(const MediaFetcher&) = delete;
  MediaFetcher& operator=(const MediaFetcher&) = delete;
  bool AllocateBuffers();
  void Cancel() { cancel.store(1, std::memory_order_release); }
  void ReleaseBuffers();
  static bool allow_media_address(void*, uint32_t ip);
  static bool allow_loopback(void*, uint32_t ip);
  bool cancelled() const;
  HopResult Hop(const fortis::compat::url::Url& target, int64_t deadline, char* location,
                size_t location_capacity);
  Fetched FetchAndStore(const char* key, const char* source_url, size_t source_length,
                        const char** content_type, size_t* length);
  Outcome Process(const char* key);
 private:
  friend class MediaRepository;
  Impl& root_;
  std::atomic<int> cancel{0};
  fortis::base::Buffer fetch_buffer;   // kMediaMaxBytes
  fortis::base::Buffer worker_record;  // kRecordBytes
  fortis::base::Buffer worker_url;     // kRecordBytes
  std::unique_ptr<fortis::compat::url::Url, fortis::base::StorageDeleter> urls;  // current and next hop
  std::unique_ptr<fortis::compat::url::UrlScratch, fortis::base::StorageDeleter> url_scratch;
};
class MediaPruner {
 public:
  using Impl = MediaRuntime;
  using KeyList = media_components::KeyList;
  explicit MediaPruner(Impl& root) : root_(root) {}
  MediaPruner(const MediaPruner&) = delete;
  MediaPruner& operator=(const MediaPruner&) = delete;
  static bool collect_key(const char* key, const void*, size_t, KeyList* list);
  size_t Prune(int64_t now, size_t* orphans);
 private:
  Impl& root_;
};
struct MediaRuntime {
  using Impl = MediaRuntime;
  using Waiter = media_components::Waiter;
  using Job = media_components::Job;
  using Outcome = media_components::Outcome;
  MediaRuntime() : pruner(*this), fetcher(*this), repository(*this) {}
  fortis::base::Thread worker;
  fortis::storage::KvStore* kv = nullptr;
  http::HttpResponseHandoff* handoff = nullptr;
  uint64_t next_sequence = 1;
  MediaPruner pruner;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable wake;
  MediaSeams seams{};
  MediaFetcher fetcher;
  Job jobs[16]{};
  MediaRepository repository;
  bool have_seams = false;
  bool worker_started = false;
  bool stopping = false;
  MediaServiceConfig config;

  // The outcome of one job, handed to its waiters.

  // The transport's cancel flag (OutboundRequest::cancel), raised by Stop().

  // Worker-owned storage.
  // Frees the five blocks above in the order Start took them.

  int64_t Now() const;

  // ---- KV helpers (callers hold MediaRecordLock) ------------------------

  // Reads and parses the record; *present false when missing/expired.

  // Reads the record's verified bytes; false when any chunk is missing or
  // inconsistent (then the record is treated as having no bytes).

  // update(key, change): only when the record still exists.

  // MediaFacet.touch(): at most one bookkeeping write per key per hour.

  // ---- the cold fetch -----------------------------------------------------

  // One checkPublicUrl + http.bytes round trip for `target`. A redirect's
  // Location is copied to `location`.

  // fetchAndStore(key, sourceUrl). On kStored the verified bytes are in
  // fetch_buffer[0, *length). A cancelled fetch (Stop) records nothing, as
  // if the process had stopped before it began.

  // One job: re-read the record (another job may have finished it), serve
  // cached bytes, honour a remembered failure, or fetch.

  static void WorkerMain(Impl* self);

  void Answer(const Waiter* waiters, size_t count, const Outcome& outcome);

  void WorkerLoop();

  // Queues `client` behind the fetch for `key`. False when no capacity is
  // left (the caller answers 404, which the app treats as "use the source").
  bool Enqueue(int client, bool head, const char* key);

  // GET (and HEAD) /v1/media/{key} — the TypeScript service's media proxy
  // over MediaFacet.get. Contract:
  //
  // Access. Public: no device bearer, no device quota, no Origin check
  // (TypeScript registers it on the root scope, outside the /v1 plugin's
  // hooks); an Authorization, Cookie or Origin header changes nothing and is
  // never forwarded. It is safe to leave open because a key only resolves
  // when the server itself registered its source URL (media_registry.h:
  // token, market, news and NFT reads), and only verified raster bytes are
  // returned. HEAD runs the same handler (a cold HEAD fetches too) and sends
  // the headers without the body.
  //
  // Key. Decoded path parameter, exactly /^[0-9a-f]{32}$/ (sha256(sourceUrl)
  // [:32]); percent escapes are decoded first, so %61 is 'a'. An empty,
  // NUL/control or over-512-byte value is the router's 404; over 100 UTF-16
  // units → 414 {"error":"URI_TOO_LONG"}; anything else that is
  // not the grammar → 400 {"error":"BAD_REQUEST"}. A trailing slash or
  // another method is the router's 404.
  //
  // Lookup. The "media/item/<key>" record (7-day KV expiry from its last
  // write). Missing/expired → 404 {"error":"NOT_FOUND"}, nothing fetched (not
  // an open proxy). Bytes stored (chunks readable) → 200 at once; the view is
  // counted afterwards, with at most one record write per key per hour
  // (lastAccessAt, fetchCount += views since; a 4,096-key in-memory table,
  // TypeScript 50,000). failed_permanent → 404; failed_transient younger
  // than one hour (and not stamped in the future) → 404 with no fetch, so a
  // refused, unreachable or filtered source all answer alike and at once.
  // An unreadable record → 404. Otherwise the key is fetched.
  //
  // Fetch (one bounded worker, src: Impl::Hop). Each hop is parsed with
  // WHATWG rules (whatwg_url.h) and judged before anything is dialled:
  // http/https, port ""/80/443, an IPv4 literal must be public, a domain must
  // resolve and EVERY resolved address be public (MediaAddressBlocked), and
  // only those judged addresses are dialled (no second lookup, no DNS
  // rebinding window). The request is GET pathname+search with
  // `accept: image/*`; no client header is forwarded. Redirects are followed
  // manually, at most 3, each Location resolved against the hop (Latin-1
  // bytes) and re-judged, so a public URL that 302s to 169.254.169.254 is
  // refused. A hop gets 10 s and the whole fetch 25 s. The decision table is
  // MediaClassifyHop: a body over 2 MiB (declared or streamed) and any
  // non-raster body are permanent; the upstream Content-Type is ignored and
  // the type comes from magic bytes (PNG, JPEG, GIF87a/89a, RIFF…WEBP, at
  // least 12 bytes; SVG/HTML/BMP/WAVE refused). 4xx/3xx-without-Location/
  // too many hops/guard refusals are failed_permanent; 5xx, timeouts,
  // transport faults and an unparseable Location are failed_transient (one
  // hour cooldown). Success stores status ok, the bytes, fetchedAt =
  // lastAccessAt = updatedAt = now, fetchCount + 1.
  //
  // Response. 200: Content-Type = the sniffed type, Cache-Control
  // "public, max-age=31536000, immutable" (content-addressed: the bytes for
  // a key never change), X-Content-Type-Options nosniff,
  // Content-Security-Policy "default-src 'none'; sandbox",
  // Content-Disposition inline, the exact bytes (at most 2 MiB, the router's
  // captured/handoff body bound is the same 2 MiB). 404/400 are JSON. Every
  // native response also carries the server's security baseline
  // (Referrer-Policy, Permissions-Policy, Cache-Control no-store on errors),
  // which the root-scoped TypeScript route does not send.
  //
  // Concurrency. Identical cold reads join one job (8 readers per job; a
  // ninth opens a second job that re-reads the stored outcome and never
  // fetches again). Jobs run one at a time on the worker (16 job slots); the
  // reader's socket waits in the shared response handoff (16 sockets, 30 s).
  // With no job slot or handoff slot left the reader gets 404 at once and
  // nothing is recorded (TypeScript fetches every distinct key concurrently).
  //
  // Retention. Bytes live in 512 KiB chunks with their own 7-day KV expiry,
  // rewritten by a view at most once a day, so bytes can be re-fetched up to
  // a day before TypeScript (whose bytes live as long as the record) would;
  // the daily prune (pruneMediaBytes: one 200-record page; frees bytes of a
  // live record unviewed for 7 days) also deletes orphan chunks. Records,
  // chunks and remembered failures survive a restart (one KV frame per
  // change); the touch table and in-flight jobs do not.
  //
  // Cancellation. Stop() raises the transport's cancel flag (polled every
  // 50 ms), records nothing for the interrupted key (it is fetched again
  // after a restart, as if never begun) and answers every queued reader 404.
  //
  // Intentional native differences include these refusals and address choices:
  // IPv6 literals and IPv6-only hosts (AAAA answers are never consulted; a
  // host whose AAAA is private but A public is fetched over IPv4), the extra
  // special-use IPv4 blocks the transport refuses, at most 8 resolved
  // addresses judged, source/Location URLs over the WHATWG parser's 8 KiB
  // input or a Location over 1023 bytes, response headers over the
  // transport's bounds, a 2xx with a non-identity Content-Encoding or
  // without Content-Length/chunked/close framing (transient).
  void Serve(int client, const http::HttpRequest& request, bool head);

  static void Handle(int client, const http::HttpRequest& request, bool head, void* opaque);

  // ---- retention --------------------------------------------------------

};
}  // namespace fortis::news
