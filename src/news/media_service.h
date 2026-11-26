#pragma once

#include <memory>
#include "clock.h"
#include "ownership.h"
#include "http_server.h"
#include "kv_store.h"
#include "outbound_https.h"
#include "whatwg_url.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::news {

// The TypeScript MediaFacet behind GET /v1/media/{key} (a root-scoped,
// public route) and pruneMediaBytes, on the runtime KV. The
// route contract is the comment above MediaService::Impl::Serve in
// media_service.cpp.
//
// Records are the discovery registration format (media_registry.h), extended
// only in `bytes`: null, or {"chunks":N,"length":L,"expiresAt":ms} naming the
// verified bytes, stored beside the record as N <= 4 values of at most
// 512 KiB under "media/blob/<key>/<i>" (a KV value is at most 1 MiB;
// TypeScript keeps the Buffer inline). The record and its chunks change in
// one KV frame. Chunks carry their own KV expiry (`expiresAt`, set to
// write time + 7 days), removed by the process KV expiry sweeper; a view
// rewrites them with a new expiry at most once per kMediaBlobRefreshMs. So
// no byte outlives its last view by more than seven days, without a rewrite
// per view and without depending on the 200-record daily prune page to
// reach it. A record whose chunks have expired is treated as uncached and
// re-fetched, as a pruned record is.
constexpr size_t kMediaMaxBytes = 2 * 1024 * 1024;
constexpr size_t kMediaChunkBytes = 512 * 1024;
constexpr int64_t kMediaTtlMs = 7LL * 24 * 3600000;
constexpr int64_t kMediaRetryCooldownMs = 3600000;
constexpr int64_t kMediaTouchIntervalMs = 3600000;
constexpr size_t kMediaMaxRedirects = 3;
constexpr size_t kMediaPrunePage = 200;
constexpr size_t kMediaTouchEntries = 4096;
// A viewed object's chunks are rewritten (new expiry) once their remaining
// lifetime falls below kMediaTtlMs - kMediaBlobRefreshMs.
constexpr int64_t kMediaBlobRefreshMs = 24LL * 3600000;

// Test seams. A normal composition passes none: the wall clock, real DNS and
// the configured CA bundle.
// The media fetcher's slice of the startup configuration: the validated
// FORTIS_CA_BUNDLE_PATH (NativeConfig fields of the same names) trusted for
// https media hosts. The news service copies it at startup.
struct MediaServiceConfig {
  bool ca_bundle_configured = false;
  char ca_bundle_path[1024]{};
};

class MediaTransport {
 public:
  virtual http::OutboundResponse Execute(const http::OutboundRequest& request,
      uint8_t* response, size_t capacity) = 0;
 protected:
  ~MediaTransport() = default;
};
struct MediaSeams {
  fortis::base::Clock* clock = nullptr;  // NowMs: the media clock
  // Replaces DNS for the address verdict: fills up to 8 IPv4 addresses (host
  // order) for `host`, false for a lookup failure. When set, an allowed
  // request is sent to `dial_host`:`dial_port` with `dial_ca` and a loopback
  // address policy, keeping the original Host header value in the request
  // header `dial_host_header` (none when unset).
  bool (*resolve)(void* context, const char* host, uint32_t* addresses, size_t* count) = nullptr;
  const char* dial_host = nullptr;
  const char* dial_host_header = nullptr;
  const char* dial_port = nullptr;        // TLS (https:) upstream
  const char* dial_plain_port = nullptr;  // plaintext (http:) upstream
  const char* dial_ca = nullptr;
  void* context = nullptr;  // resolve's context
  MediaTransport* transport = nullptr;  // borrowed synchronous effect
};

enum class MediaStatus : uint8_t { kPending, kOk, kFailedPermanent, kFailedTransient };

struct MediaRecord {
  char key[33]{};
  const char* source_url = nullptr;  // points into caller storage
  size_t source_url_length = 0;
  MediaStatus status = MediaStatus::kPending;
  bool has_bytes = false;
  uint32_t chunks = 0;
  uint32_t length = 0;
  int64_t blob_expires_ms = 0;  // the chunks' KV expiry (0: written before expiry existed)
  const char* content_type = nullptr;  // static string or null
  bool has_fetched_at = false;
  int64_t fetched_at_ms = 0;
  bool has_last_access_at = false;
  int64_t last_access_at_ms = 0;
  double fetch_count = 0;
  int64_t updated_at_ms = 0;
};

// Parses a record value. `text` must stay valid while the record is used.
bool ParseMediaRecord(const unsigned char* text, size_t length, MediaRecord* record,
                      char* url_storage, size_t url_capacity);
// Serializes a record (the media_registry.h field order).
bool WriteMediaRecord(const MediaRecord& record, unsigned char* output, size_t capacity,
                      size_t* length);
// sniffImageType: the served type these bytes prove, or null.
const char* SniffImageType(const unsigned char* bytes, size_t length);
// checkPublicUrl's isBlockedAddress for IPv4 (host order), widened by the
// special-use blocks the outbound transport also refuses.
bool MediaAddressBlocked(uint32_t ipv4);
// The route's key grammar, /^[0-9a-f]{32}$/.
bool MediaKeyValid(const char* key);
// The request target pinnedFetch sends for `url`: pathname + search, so no
// fragment and no lone "?" (url.search is "" for an empty query). False when
// it does not fit `capacity` (with its NUL).
bool MediaRequestTarget(const fortis::compat::url::Url& url, char* output, size_t capacity);
// new URL(location, base) for a Location header value whose bytes Node reads
// as Latin-1 (each byte one code point). False when it does not parse.
bool MediaResolveLocation(const char* location, const fortis::compat::url::Url& base,
                          fortis::compat::url::Url* next, fortis::compat::url::UrlScratch* scratch);
// checkPublicUrl's rules a URL can fail without DNS: http/https, port ""/80/
// 443, and an IPv4 literal host that is public. Native refusal: every IPv6
// literal (the guest network stack is IPv4-only). A domain host passes here;
// its resolved addresses are judged by the transport (every one public).
bool MediaUrlAllowed(const fortis::compat::url::Url& url);
// fetchAndStore's decision for one transport answer, in the TypeScript order:
// a 3xx redirects (no Location: permanent); a declared Content-Length over
// 2 MiB is permanent whatever the status; a 2xx body over 2 MiB is
// permanent; another 4xx is permanent and any other non-2xx transient; a
// transport failure is transient (a guard refusal, before any packet,
// permanent); a 2xx body is served only when its magic bytes prove a raster
// type (`*content_type`), else permanent.
enum class MediaHopVerdict : uint8_t { kServe, kRedirect, kPermanent, kTransient };
MediaHopVerdict MediaClassifyHop(const http::OutboundResponse& response, const unsigned char* body,
                                 const char** content_type);

struct MediaRuntime;
class MediaService {
 public:
  MediaService() = default;
  ~MediaService();
  MediaService(const MediaService&) = delete;
  MediaService& operator=(const MediaService&) = delete;

  bool Start(const MediaServiceConfig& config, fortis::storage::KvStore& kv, const MediaSeams* seams = nullptr);
  // Starts the fetch worker and registers GET /v1/media/:key; cold fetches
  // complete their reserved sockets through `handoff`.
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  void Stop();
  bool started() const { return impl_ != nullptr; }
  // One pruneMediaBytes pass (one 200-key page) plus one orphan-chunk page.
  // Returns the number of records whose bytes were freed.
  size_t PruneBytes(int64_t now_ms, size_t* orphans_removed = nullptr);

 private:
  using Impl = MediaRuntime;
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::news
