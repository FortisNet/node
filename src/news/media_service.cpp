#include <charconv>
#include <atomic>
#include "media_service.h"
#include "media_components_internal.h"
#include "http_middleware.h"
#include "ownership.h"

#include "bounded_json.h"
#include "discovery_cache.h"
#include "market_view.h"
#include "media_registry.h"
#include "price_history_batch_codec.h"
#include "threading.h"
#include "whatwg_url.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace fortis::news {

namespace {

using fortis::codec::history_batch::OutBuffer;

constexpr size_t kRecordBytes = 64 * 1024;
constexpr size_t kMaxChunks = kMediaMaxBytes / kMediaChunkBytes;
constexpr size_t kJobs = 16;
constexpr size_t kWaitersPerJob = 8;
constexpr uint32_t kHandoffTimeoutMs = 30000;
constexpr int64_t kFetchBudgetMs = 25000;
constexpr uint32_t kHopTimeoutMs = 10000;  // the shared transport's default
constexpr char kPruneCursor[] = "media/meta/prune_cursor";
constexpr char kBlobCursor[] = "media/meta/blob_cursor";

const char kImageCache[] = "public, max-age=31536000, immutable";
const char kImageHeaders[] =
    "content-security-policy: default-src 'none'; sandbox\r\n"
    "content-disposition: inline\r\n";
const char kJsonType[] = "application/json; charset=utf-8";
const char kNotFound[] = "{\"error\":\"NOT_FOUND\"}";
const char kBadRequest[] = "{\"error\":\"BAD_REQUEST\"}";

int64_t wall_ms() {
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

int64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// A key over 100 UTF-16 units once decoded: 414 {"error":"URI_TOO_LONG"}
// before the handler's other checks.
void reject_long_key(int client, const http::HttpRequest&, bool head) {
  http::WriteRefusal(client, 414, "URI Too Long", "URI_TOO_LONG", head);
}

void record_key(const char* key, char output[64]) {
  snprintf(output, 64, "media/item/%s", key);
}

void blob_key(const char* key, size_t index, char output[64]) {
  snprintf(output, 64, "media/blob/%s/%zu", key, index);
}

// Date.prototype.toISOString output, as every native media writer stores it.
bool parse_iso(const char* text, size_t length, int64_t* ms) {
  if (length != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text[19] != '.' || text[23] != 'Z') return false;
  auto digits = [&](size_t at, size_t n, int64_t* value) {
    *value = 0;
    for (size_t i = 0; i < n; ++i) {
      const char c = text[at + i];
      if (c < '0' || c > '9') return false;
      *value = *value * 10 + (c - '0');
    }
    return true;
  };
  int64_t y, mo, d, h, mi, s, f;
  if (!digits(0, 4, &y) || !digits(5, 2, &mo) || !digits(8, 2, &d) || !digits(11, 2, &h) ||
      !digits(14, 2, &mi) || !digits(17, 2, &s) || !digits(20, 3, &f) || mo < 1 || mo > 12)
    return false;
  // days_from_civil (Howard Hinnant).
  const int64_t yy = y - (mo <= 2);
  const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const int64_t yoe = yy - era * 400;
  const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  *ms = ((days * 24 + h) * 60 + mi) * 60000 + s * 1000 + f;
  return true;
}

const char* status_name(MediaStatus status) {
  switch (status) {
    case MediaStatus::kPending: return "pending";
    case MediaStatus::kOk: return "ok";
    case MediaStatus::kFailedPermanent: return "failed_permanent";
    case MediaStatus::kFailedTransient: return "failed_transient";
  }
  return "pending";
}

const char* known_type(const char* text, size_t length) {
  static const char* const kTypes[] = {"image/png", "image/jpeg", "image/gif", "image/webp"};
  for (const char* type : kTypes)
    if (strlen(type) == length && memcmp(type, text, length) == 0) return type;
  return nullptr;
}

}  // namespace

const char* SniffImageType(const unsigned char* b, size_t length) {
  if (!b || length < 12) return nullptr;
  static const unsigned char kPng[8] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  if (memcmp(b, kPng, 8) == 0) return "image/png";
  if (b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return "image/jpeg";
  if (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0) return "image/gif";
  if (memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) return "image/webp";
  return nullptr;
}

bool MediaAddressBlocked(uint32_t ip) {
  const uint8_t a = static_cast<uint8_t>(ip >> 24);
  const uint8_t b = static_cast<uint8_t>(ip >> 16);
  const uint8_t c = static_cast<uint8_t>(ip >> 8);
  // The TypeScript isBlockedV4.
  if (a == 0 || a == 10 || a == 127 || (a == 100 && b >= 64 && b <= 127) ||
      (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
      (a == 192 && b == 0) || (a == 198 && (b == 18 || b == 19)) || (a == 198 && b == 51) ||
      (a == 203 && b == 0) || a >= 224) return true;
  // The further special-use blocks outbound_https.cpp refuses for every caller.
  return (a == 192 && b == 88 && c == 99) || (a == 192 && b == 175 && c == 48) ||
         (a == 192 && b == 31 && c == 196) || (a == 192 && b == 52 && c == 193);
}

bool MediaKeyValid(const char* key) {
  if (!key || strlen(key) != 32) return false;
  for (size_t i = 0; i < 32; ++i)
    if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) return false;
  return true;
}

bool MediaRequestTarget(const fortis::compat::url::Url& url, char* output, size_t capacity) {
  const fortis::compat::url::View pathname = fortis::compat::url::Pathname(url);
  const fortis::compat::url::View search = fortis::compat::url::Search(url);
  if (!output || pathname.length + search.length + 1 > capacity) return false;
  memcpy(output, pathname.data, pathname.length);
  memcpy(output + pathname.length, search.data, search.length);
  output[pathname.length + search.length] = '\0';
  return true;
}

bool MediaResolveLocation(const char* location, const fortis::compat::url::Url& base,
                          fortis::compat::url::Url* next, fortis::compat::url::UrlScratch* scratch) {
  // A Location value is at most 1023 bytes (OutboundResponse::location), so
  // its UTF-8 form fits twice that.
  char converted[2 * sizeof(http::OutboundResponse{}.location)];
  size_t used = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(location); *p; ++p) {
    if (used + 2 > sizeof(converted)) return false;
    if (*p < 0x80) {
      converted[used++] = static_cast<char>(*p);
    } else {
      converted[used++] = static_cast<char>(0xc0 | (*p >> 6));
      converted[used++] = static_cast<char>(0x80 | (*p & 0x3f));
    }
  }
  return fortis::compat::url::Parse(converted, used, &base, next, scratch);
}

bool MediaUrlAllowed(const fortis::compat::url::Url& url) {
  using namespace fortis::compat::url;
  const View protocol = Protocol(url);
  const bool https = protocol.length == 6 && memcmp(protocol.data, "https:", 6) == 0;
  const bool http = protocol.length == 5 && memcmp(protocol.data, "http:", 5) == 0;
  if (!https && !http) return false;  // scheme not allowed
  const View port = Port(url);
  if (!(port.length == 0 || (port.length == 2 && memcmp(port.data, "80", 2) == 0) ||
        (port.length == 3 && memcmp(port.data, "443", 3) == 0))) return false;
  // Native: the guest network stack is IPv4-only (lwIP without IPv6), so an
  // IPv6 literal is refused before anything is dialled.
  if (url.host_type == HostType::kIpv6) return false;
  if (url.host_type == HostType::kIpv4) {
    const View hostname = Hostname(url);
    if (hostname.length >= 16) return false;
    const char* cursor = hostname.data;
    const char* end = cursor + hostname.length;
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      unsigned part = 0;
      const auto parsed = std::from_chars(cursor, end, part);
      if (parsed.ec != std::errc{} || part > 255) return false;
      value = value << 8 | part;
      if (i == 3) {
        if (parsed.ptr != end) return false;
      } else {
        if (parsed.ptr == end || *parsed.ptr != '.') return false;
        cursor = parsed.ptr + 1;
      }
    }
    return !MediaAddressBlocked(value);
  }
  return url.host_type == HostType::kDomain;
}

MediaHopVerdict MediaClassifyHop(const http::OutboundResponse& response, const unsigned char* body,
                                 const char** content_type) {
  *content_type = nullptr;
  if (response.status == 0) {
    // No answer. The transport refuses a host it cannot name, one that does
    // not resolve, or one with any non-public address before dialling: the
    // guard's verdict. Anything else threw (the transient catch path).
    return response.error == http::OutboundError::kInvalidRequest ||
                   response.error == http::OutboundError::kPrivateAddress ||
                   response.error == http::OutboundError::kResolve
               ? MediaHopVerdict::kPermanent
               : MediaHopVerdict::kTransient;
  }
  // Manual redirect, before any size or status rule.
  if (response.status >= 300 && response.status < 400)
    return response.has_location ? MediaHopVerdict::kRedirect : MediaHopVerdict::kPermanent;
  // HttpClient.bytes: a declared length over the cap is `tooLarge` whatever
  // the status, and fetchAndStore checks tooLarge before the status.
  if (response.declared_too_large) return MediaHopVerdict::kPermanent;
  if (response.status < 200 || response.status >= 300)
    return response.status >= 400 && response.status < 500 ? MediaHopVerdict::kPermanent
                                                            : MediaHopVerdict::kTransient;
  if (response.error == http::OutboundError::kTooLarge) return MediaHopVerdict::kPermanent;
  if (response.error != http::OutboundError::kNone) return MediaHopVerdict::kTransient;
  // The upstream's Content-Type is not trusted: only the bytes decide.
  *content_type = SniffImageType(body, response.body_length);
  return *content_type ? MediaHopVerdict::kServe : MediaHopVerdict::kPermanent;
}

bool ParseMediaRecord(const unsigned char* text, size_t length, MediaRecord* record,
                      char* url_storage, size_t url_capacity) {
  *record = MediaRecord{};
  codec::BoundedJsonToken tokens[64];
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error = codec::BoundedJsonError::kNone;
  if (!document.Parse(text, length, tokens, 64, &error)) return false;
  const uint32_t root = document.root();
  if (document.token(root)->type != codec::BoundedJsonType::kObject) return false;
  auto field = [&](const char* name) { return document.FindObjectValue(root, name); };
  auto string_value = [&](uint32_t index, char* output, size_t capacity, size_t* out_length) {
    return index != codec::kBoundedJsonNoToken && document.token(index)->type == codec::BoundedJsonType::kString &&
           document.DecodeString(index, output, capacity, out_length);
  };
  auto is_null = [&](uint32_t index) {
    return index != codec::kBoundedJsonNoToken && document.token(index)->type == codec::BoundedJsonType::kNull;
  };
  size_t n = 0;
  char small[64];
  if (!string_value(field("key"), record->key, sizeof(record->key), &n) || n != 32) return false;
  if (!string_value(field("sourceUrl"), url_storage, url_capacity, &record->source_url_length))
    return false;
  record->source_url = url_storage;
  if (!string_value(field("status"), small, sizeof(small), &n)) return false;
  small[n] = '\0';
  if (strcmp(small, "pending") == 0) record->status = MediaStatus::kPending;
  else if (strcmp(small, "ok") == 0) record->status = MediaStatus::kOk;
  else if (strcmp(small, "failed_permanent") == 0) record->status = MediaStatus::kFailedPermanent;
  else if (strcmp(small, "failed_transient") == 0) record->status = MediaStatus::kFailedTransient;
  else return false;
  const uint32_t bytes = field("bytes");
  if (bytes != codec::kBoundedJsonNoToken && document.token(bytes)->type == codec::BoundedJsonType::kObject) {
    double chunks = 0, total = 0, expires = 0;
    const uint32_t c = document.FindObjectValue(bytes, "chunks");
    const uint32_t l = document.FindObjectValue(bytes, "length");
    const uint32_t e = document.FindObjectValue(bytes, "expiresAt");
    if (c == codec::kBoundedJsonNoToken || l == codec::kBoundedJsonNoToken || !document.Number(c, &chunks) ||
        !document.Number(l, &total) || chunks < 1 || chunks > kMaxChunks || total < 1 ||
        total > kMediaMaxBytes) return false;
    // Chunks written before blob expiry existed carry none: refreshed on view.
    if (e != codec::kBoundedJsonNoToken && (!document.Number(e, &expires) || expires < 0)) return false;
    record->has_bytes = true;
    record->chunks = static_cast<uint32_t>(chunks);
    record->length = static_cast<uint32_t>(total);
    record->blob_expires_ms = static_cast<int64_t>(expires);
  } else if (!is_null(bytes)) {
    return false;
  }
  const uint32_t type = field("contentType");
  if (!is_null(type)) {
    if (!string_value(type, small, sizeof(small), &n)) return false;
    record->content_type = known_type(small, n);
    if (!record->content_type) return false;
  }
  auto timestamp = [&](const char* name, bool* has, int64_t* ms) {
    const uint32_t index = field(name);
    if (is_null(index)) { *has = false; return true; }
    size_t length_out = 0;
    if (!string_value(index, small, sizeof(small), &length_out)) return false;
    *has = parse_iso(small, length_out, ms);
    return *has;
  };
  bool has_updated = false;
  if (!timestamp("fetchedAt", &record->has_fetched_at, &record->fetched_at_ms) ||
      !timestamp("lastAccessAt", &record->has_last_access_at, &record->last_access_at_ms) ||
      !timestamp("updatedAt", &has_updated, &record->updated_at_ms) || !has_updated) return false;
  const uint32_t count = field("fetchCount");
  if (count == codec::kBoundedJsonNoToken || !document.Number(count, &record->fetch_count)) return false;
  return true;
}

bool WriteMediaRecord(const MediaRecord& record, unsigned char* output, size_t capacity,
                      size_t* length) {
  OutBuffer out{output, capacity};
  char stamp[32];
  out.Text("{\"key\":\"");
  out.Text(record.key);
  out.Text("\",\"sourceUrl\":");
  fortis::codec::history_batch::AppendJsonString(out, record.source_url, record.source_url_length);
  out.Text(",\"status\":\"");
  out.Text(status_name(record.status));
  out.Text("\",\"bytes\":");
  char number[64];
  if (record.has_bytes) {
    snprintf(number, sizeof(number), "{\"chunks\":%u,\"length\":%u,\"expiresAt\":%lld}",
             record.chunks, record.length, static_cast<long long>(record.blob_expires_ms));
    out.Text(number);
  } else {
    out.Text("null");
  }
  out.Text(",\"contentType\":");
  if (record.content_type) { out.Byte('"'); out.Text(record.content_type); out.Byte('"'); }
  else out.Text("null");
  out.Text(",\"sizeBytes\":");
  if (record.has_bytes) { snprintf(number, sizeof(number), "%u", record.length); out.Text(number); }
  else out.Text("null");
  auto iso = [&](const char* name, bool has, int64_t ms) {
    out.Text(name);
    if (has && discovery::DiscoveryIsoTimestamp(ms, stamp)) { out.Byte('"'); out.Text(stamp); out.Byte('"'); }
    else out.Text("null");
  };
  iso(",\"fetchedAt\":", record.has_fetched_at, record.fetched_at_ms);
  iso(",\"lastAccessAt\":", record.has_last_access_at, record.last_access_at_ms);
  out.Text(",\"fetchCount\":");
  fortis::codec::history_batch::AppendJsNumber(out, record.fetch_count);
  iso(",\"updatedAt\":", true, record.updated_at_ms);
  out.Byte('}');
  if (!out.ok) return false;
  *length = out.used;
  return true;
}

bool MediaFetcher::AllocateBuffers() {
  fetch_buffer = fortis::base::Buffer::Allocate(kMediaMaxBytes);
  worker_record = fortis::base::Buffer::Allocate(kRecordBytes);
  worker_url = fortis::base::Buffer::Allocate(kRecordBytes);
  urls = base::AllocateStorage<fortis::compat::url::Url>(2 * sizeof(fortis::compat::url::Url));
  url_scratch = base::AllocateStorage<fortis::compat::url::UrlScratch>(sizeof(fortis::compat::url::UrlScratch));
  if (!fetch_buffer || !worker_record || !worker_url || !urls || !url_scratch) {
    ReleaseBuffers();
    return false;
  }
  return true;
}

void MediaFetcher::ReleaseBuffers() {
    fetch_buffer.reset(); worker_record.reset(); worker_url.reset();
    urls.reset(); url_scratch.reset();
  }

bool MediaRepository::ReadRecord(const char* key, int64_t now, unsigned char* buffer, char* url, MediaRecord* record,
                  bool* present) {
    char name[64];
    record_key(key, name);
    size_t size = 0;
    const fortis::storage::KvReadStatus status = root_.kv->GetStatus(name, now, buffer, kRecordBytes, &size);
    *present = false;
    if (status == fortis::storage::KvReadStatus::Missing) return true;
    if (status != fortis::storage::KvReadStatus::Found) return false;
    if (!ParseMediaRecord(buffer, size, record, url, kRecordBytes)) return false;
    *present = true;
    return true;
  }

bool MediaRepository::WriteRecord(const MediaRecord& record, int64_t now, const unsigned char* bytes,
                   bool write_chunks, bool erase_chunks) {
    auto value_storage = base::AllocateStorage<unsigned char[]>(kRecordBytes);
    auto* value = value_storage.get();
    if (!value) return false;
    size_t length = 0;
    if (!WriteMediaRecord(record, value, kRecordBytes, &length)) { value_storage.reset(); return false; }
    char name[64];
    record_key(record.key, name);
    char chunk_names[kMaxChunks][64];
    fortis::storage::KvMutation mutations[1 + kMaxChunks];
    size_t count = 0;
    mutations[count++] = fortis::storage::KvMutation{name, value, static_cast<uint32_t>(length),
                                            now + kMediaTtlMs, false};
    for (size_t i = 0; i < kMaxChunks; ++i) {
      blob_key(record.key, i, chunk_names[i]);
      if (write_chunks && i < record.chunks) {
        const size_t offset = i * kMediaChunkBytes;
        const size_t size = record.length - offset < kMediaChunkBytes ? record.length - offset
                                                                       : kMediaChunkBytes;
        mutations[count++] = fortis::storage::KvMutation{chunk_names[i], bytes + offset,
                                                static_cast<uint32_t>(size),
                                                record.blob_expires_ms, false};
      } else if (write_chunks || erase_chunks) {
        mutations[count++] = fortis::storage::KvMutation{chunk_names[i], nullptr, 0, 0, true};
      }
    }
    const bool ok = root_.kv->Write(mutations, count);
    value_storage.reset();
    return ok;
  }

bool MediaRepository::ReadBlob(const MediaRecord& record, int64_t now, unsigned char* output) {
    size_t offset = 0;
    for (uint32_t i = 0; i < record.chunks; ++i) {
      char name[64];
      blob_key(record.key, i, name);
      const size_t expected = record.length - offset < kMediaChunkBytes ? record.length - offset
                                                                         : kMediaChunkBytes;
      size_t size = 0;
      if (root_.kv->GetStatus(name, now, output + offset, expected, &size) != fortis::storage::KvReadStatus::Found ||
          size != expected) return false;
      offset += size;
    }
    return offset == record.length;
  }

void MediaRepository::MarkFailed(const char* key, bool permanent) {
    discovery::MediaRecordLock();
    const int64_t now = root_.Now();
    MediaRecord record;
    bool present = false;
    if (ReadRecord(key, now, root_.fetcher.worker_record.data(), root_.fetcher.worker_url.chars(), &record, &present) && present) {
      record.status = permanent ? MediaStatus::kFailedPermanent : MediaStatus::kFailedTransient;
      record.updated_at_ms = now;
      WriteRecord(record, now, nullptr, false, false);
    }
    discovery::MediaRecordUnlock();
  }

bool MediaRepository::StoreOk(const char* key, const unsigned char* bytes, size_t length, const char* type) {
    discovery::MediaRecordLock();
    const int64_t now = root_.Now();
    MediaRecord record;
    bool present = false;
    bool ok = ReadRecord(key, now, root_.fetcher.worker_record.data(), root_.fetcher.worker_url.chars(), &record, &present);
    if (ok && present) {
      record.has_bytes = true;
      record.length = static_cast<uint32_t>(length);
      record.chunks = static_cast<uint32_t>((length + kMediaChunkBytes - 1) / kMediaChunkBytes);
      record.blob_expires_ms = now + kMediaTtlMs;
      record.content_type = type;
      record.status = MediaStatus::kOk;
      record.has_fetched_at = true;
      record.fetched_at_ms = now;
      record.has_last_access_at = true;
      record.last_access_at_ms = now;
      record.fetch_count += 1;
      record.updated_at_ms = now;
      ok = WriteRecord(record, now, bytes, true, false);
    }
    discovery::MediaRecordUnlock();
    return ok;
  }

void MediaRepository::TouchKey(const char* key) {
    const int64_t now = root_.Now();
    fortis::base::UniqueLock touch_lock(touch_mutex);
    Touch* entry = nullptr;
    for (Touch& candidate : touches)
      if (candidate.used && strcmp(candidate.key, key) == 0) { entry = &candidate; break; }
    if (entry && now - entry->written_at < kMediaTouchIntervalMs) {
      ++entry->pending;
      return;
    }
    const uint64_t views = (entry ? entry->pending : 0) + 1;
    if (!entry) {
      // Map semantics: a new key is the newest; past the bound the oldest goes.
      Touch* oldest = nullptr;
      for (Touch& candidate : touches) {
        if (!candidate.used) { entry = &candidate; break; }
        if (!oldest || candidate.order < oldest->order) oldest = &candidate;
      }
      if (!entry) entry = oldest;
      snprintf(entry->key, sizeof(entry->key), "%s", key);
      entry->used = true;
    }
    entry->written_at = now;
    entry->pending = 0;
    entry->order = touch_order++;
    touch_lock.Unlock();

    auto buffer_storage = base::AllocateStorage<unsigned char[]>(kRecordBytes);
    auto* buffer = buffer_storage.get();
    auto url_storage = base::AllocateStorage<char[]>(kRecordBytes);
    auto* url = url_storage.get();
    if (buffer && url) {
      discovery::MediaRecordLock();
      MediaRecord record;
      bool present = false;
      if (ReadRecord(key, now, buffer, url, &record, &present) && present) {
        record.has_last_access_at = true;
        record.last_access_at_ms = now;
        record.fetch_count += static_cast<double>(views);
        // The chunks expire on their own clock; a viewed object's chunks
        // are rewritten at most once per kMediaBlobRefreshMs, so bytes
        // outlive their last view by at most kMediaTtlMs (the TypeScript
        // record lifetime) without a rewrite per view.
        std::unique_ptr<unsigned char[], base::StorageDeleter> blob_storage;
    unsigned char* blob = nullptr;
        if (record.has_bytes && record.blob_expires_ms - now < kMediaTtlMs - kMediaBlobRefreshMs) {
          blob_storage = base::AllocateStorage<unsigned char[]>(record.length);
          blob = blob_storage.get();
          if (blob && ReadBlob(record, now, blob)) {
            record.blob_expires_ms = now + kMediaTtlMs;
            WriteRecord(record, now, blob, true, false);
          } else {
            WriteRecord(record, now, nullptr, false, false);
          }
        } else {
          WriteRecord(record, now, nullptr, false, false);
        }
        blob_storage.reset();
      }
      discovery::MediaRecordUnlock();
    }
    buffer_storage.reset();
    url_storage.reset();
  }

bool MediaFetcher::allow_media_address(void*, uint32_t ip) { return !MediaAddressBlocked(ip); }

bool MediaFetcher::allow_loopback(void*, uint32_t ip) { return ip == 0x7f000001U; }

bool MediaFetcher::cancelled() const { return (cancel).load(std::memory_order_acquire) != 0; }

MediaFetcher::HopResult MediaFetcher::Hop(const fortis::compat::url::Url& target, int64_t deadline, char* location,
                size_t location_capacity) {
    using namespace fortis::compat::url;
    HopResult result;
    // Scheme, port and a literal host are judged before anything is dialled.
    if (!MediaUrlAllowed(target)) return result;
    const bool http = Protocol(target).length == 5;
    const View hostname = Hostname(target);
    char host[256];
    if (!hostname.length || hostname.length >= sizeof(host)) return result;
    memcpy(host, hostname.data, hostname.length);
    host[hostname.length] = '\0';
    const View port = Port(target);
    char port_text[6];
    if (port.length) { memcpy(port_text, port.data, port.length); port_text[port.length] = '\0'; }
    else snprintf(port_text, sizeof(port_text), "%s", http ? "80" : "443");
    char path[8192];
    if (!MediaRequestTarget(target, path, sizeof(path))) return result;

    http::OutboundRequest request{};
    request.method = http::OutboundMethod::kGet;
    request.path = path;
    request.plaintext = http;
    request.skip_non_2xx_body = true;  // HttpClient.bytes reads only 2xx bodies.
    request.max_response_bytes = kMediaMaxBytes;
    request.cancel = &cancel;
    const int64_t remaining = deadline - monotonic_ms();
    if (remaining <= 0) { result.verdict = Verdict::kTransient; return result; }
    request.timeout_ms = remaining < kHopTimeoutMs ? static_cast<uint32_t>(remaining) : kHopTimeoutMs;
    snprintf(request.headers[0].name, sizeof(request.headers[0].name), "accept");
    snprintf(request.headers[0].value, sizeof(request.headers[0].value), "image/*");
    request.header_count = 1;
    if (root_.have_seams && root_.seams.resolve) {
      // Test DNS: judge the seam's answer, then dial the loopback upstream.
      uint32_t addresses[8];
      size_t count = 0;
      if (target.host_type == HostType::kIpv4) {
        // MediaUrlAllowed judged the literal.
      } else if (!root_.seams.resolve(root_.seams.context, host, addresses, &count) || !count) {
        return result;  // dns failure / dns empty
      }
      for (size_t i = 0; i < count; ++i) if (MediaAddressBlocked(addresses[i])) return result;
      request.host = root_.seams.dial_host;
      request.port = http ? root_.seams.dial_plain_port : root_.seams.dial_port;
      request.ca_file = http ? nullptr : root_.seams.dial_ca;
      request.allow_address = allow_loopback;
      if (root_.seams.dial_host_header && *root_.seams.dial_host_header) {
        snprintf(request.headers[1].name, sizeof(request.headers[1].name), "%s",
                 root_.seams.dial_host_header);
        snprintf(request.headers[1].value, sizeof(request.headers[1].value), "%.240s:%.5s", host,
                 port_text);
        request.header_count = 2;
      }
    } else {
      // A missing trust bundle is a deployment fault, not a verdict on the
      // URL: transient, so the key is retried after the cooldown. (The
      // composition refuses to start the media route without a validated
      // bundle, so this is defensive.)
      if (!http && !root_.config.ca_bundle_configured) {
        result.verdict = Verdict::kTransient;
        return result;
      }
      request.host = host;
      request.port = port_text;
      request.ca_file = root_.config.ca_bundle_path;
      request.allow_address = allow_media_address;
      request.require_all_addresses_allowed = true;
    }
    const http::OutboundResponse response = (root_.have_seams && root_.seams.transport ? root_.seams.transport->Execute(request, fetch_buffer.data(), kMediaMaxBytes) : OutboundHttpsRequest(request, fetch_buffer.data(), kMediaMaxBytes));
    if (cancelled()) { result.verdict = Verdict::kCancelled; return result; }
    switch (MediaClassifyHop(response, fetch_buffer.data(), &result.content_type)) {
      case MediaHopVerdict::kServe:
        result.verdict = Verdict::kFetch;
        result.length = response.body_length;
        break;
      case MediaHopVerdict::kRedirect:
        snprintf(location, location_capacity, "%s", response.location);
        result.verdict = Verdict::kFetch;
        result.redirect = true;
        break;
      case MediaHopVerdict::kPermanent: result.verdict = Verdict::kPermanent; break;
      case MediaHopVerdict::kTransient: result.verdict = Verdict::kTransient; break;
    }
    return result;
  }

MediaFetcher::Fetched MediaFetcher::FetchAndStore(const char* key, const char* source_url, size_t source_length,
                        const char** content_type, size_t* length) {
    using namespace fortis::compat::url;
    const int64_t deadline = monotonic_ms() + kFetchBudgetMs;
    Url* current = &urls.get()[0];
    Url* next = &urls.get()[1];
    if (!Parse(source_url, source_length, nullptr, current, url_scratch.get())) {
      root_.repository.MarkFailed(key, true);  // unparseable url
      return Fetched::kRefused;
    }
    char location[sizeof(http::OutboundResponse{}.location)];
    for (size_t hop = 0; hop <= kMediaMaxRedirects; ++hop) {
      const HopResult result = Hop(*current, deadline, location, sizeof(location));
      if (result.verdict == Verdict::kCancelled) return Fetched::kCancelled;
      if (result.verdict == Verdict::kPermanent) { root_.repository.MarkFailed(key, true); return Fetched::kRefused; }
      if (result.verdict == Verdict::kTransient) { root_.repository.MarkFailed(key, false); return Fetched::kRefused; }
      if (result.redirect) {
        // new URL(location, current); a throw is the transient catch path.
        if (!MediaResolveLocation(location, *current, next, url_scratch.get())) {
          root_.repository.MarkFailed(key, false);
          return Fetched::kRefused;
        }
        Url* swap = current;
        current = next;
        next = swap;
        continue;
      }
      if (!root_.repository.StoreOk(key, fetch_buffer.data(), result.length, result.content_type)) {
        // The update threw inside the try: TypeScript records a transient
        // failure and answers null.
        root_.repository.MarkFailed(key, false);
        return Fetched::kRefused;
      }
      *content_type = result.content_type;
      *length = result.length;
      return Fetched::kStored;
    }
    root_.repository.MarkFailed(key, true);  // too many hops
    return Fetched::kRefused;
  }

MediaFetcher::Outcome MediaFetcher::Process(const char* key) {
    Outcome outcome;
    discovery::MediaRecordLock();
    const int64_t now = root_.Now();
    MediaRecord record;
    bool present = false;
    const bool read = root_.repository.ReadRecord(key, now, worker_record.data(), worker_url.chars(), &record, &present);
    if (!read || !present) { discovery::MediaRecordUnlock(); return outcome; }
    if (record.has_bytes && record.content_type && root_.repository.ReadBlob(record, now, fetch_buffer.data())) {
      discovery::MediaRecordUnlock();
      outcome.found = true;
      outcome.content_type = record.content_type;
      outcome.bytes = fetch_buffer.data();
      outcome.length = record.length;
      return outcome;
    }
    const bool refused = record.status == MediaStatus::kFailedPermanent ||
        (record.status == MediaStatus::kFailedTransient && now - record.updated_at_ms >= 0 &&
         now - record.updated_at_ms < kMediaRetryCooldownMs);
    auto source_storage = base::AllocateStorage<char[]>(record.source_url_length + 1);
    auto* source = source_storage.get();
    const size_t source_length = record.source_url_length;
    if (source) {
      memcpy(source, record.source_url, source_length);
      source[source_length] = '\0';
    }
    discovery::MediaRecordUnlock();
    if (refused || !source || cancelled()) { source_storage.reset(); return outcome; }
    const char* type = nullptr;
    size_t length = 0;
    if (FetchAndStore(key, source, source_length, &type, &length) == Fetched::kStored) {
      outcome.found = true;
      outcome.content_type = type;
      outcome.bytes = fetch_buffer.data();
      outcome.length = length;
    }
    source_storage.reset();
    return outcome;
  }

bool MediaPruner::collect_key(const char* key, const void*, size_t, KeyList* list) {
    if (list->count < list->capacity && strlen(key) < 128)
      snprintf(list->keys[list->count++], 128, "%s", key);
    return true;
  }

size_t MediaPruner::Prune(int64_t now, size_t* orphans) {
    size_t freed = 0;
    if (orphans) *orphans = 0;
    auto keys_storage = base::AllocateStorage<char[][128]>(kMediaPrunePage * 128);
    auto* keys = keys_storage.get();
    auto buffer_storage = base::AllocateStorage<unsigned char[]>(kRecordBytes);
    auto* buffer = buffer_storage.get();
    auto url_storage = base::AllocateStorage<char[]>(kRecordBytes);
    auto* url = url_storage.get();
    if (!keys || !buffer || !url) {
      keys_storage.reset(); buffer_storage.reset(); url_storage.reset();
      return 0;
    }
    discovery::MediaRecordLock();
    // pruneMediaBytes: one page per run, resumed from the stored cursor.
    // Each freed record and its chunks change in one frame; TypeScript writes
    // the page in one transaction, which is not observable beyond a crash.
    char cursor[1025] = {};
    size_t size = 0;
    const bool have_cursor = root_.kv->GetStatus(kPruneCursor, -1, cursor, sizeof(cursor) - 1, &size) ==
                             fortis::storage::KvReadStatus::Found;
    cursor[have_cursor ? size : 0] = '\0';
    KeyList list{keys, 0, kMediaPrunePage};
    fortis::storage::KvPageResult result;
    if (root_.kv->Page("media/item/", have_cursor && cursor[0] ? cursor : nullptr, kMediaPrunePage, now, false, [&, visitor_state = &list](const char* key, const void* value, size_t size) { return (collect_key)(key, value, size, visitor_state); }, &result)) {
      const int64_t cutoff = now - kMediaTtlMs;
      for (size_t i = 0; i < list.count; ++i) {
        const char* key = list.keys[i] + strlen("media/item/");
        MediaRecord record;
        bool present = false;
        if (!root_.repository.ReadRecord(key, now, buffer, url, &record, &present) || !present) continue;
        const bool has_access = record.has_last_access_at || record.has_fetched_at;
        const int64_t last = record.has_last_access_at ? record.last_access_at_ms
                                                       : record.fetched_at_ms;
        if (!record.has_bytes || !has_access || last >= cutoff) continue;
        record.has_bytes = false;
        record.chunks = 0;
        record.length = 0;
        record.status = MediaStatus::kPending;
        record.updated_at_ms = now;
        // The content type stays, as `{...current, bytes: null}` keeps it.
        if (root_.repository.WriteRecord(record, now, nullptr, false, true)) ++freed;
      }
      const fortis::storage::KvMutation cursor_write = result.done
          ? fortis::storage::KvMutation{kPruneCursor, nullptr, 0, 0, true}
          : fortis::storage::KvMutation{kPruneCursor, result.next_cursor,
                               static_cast<uint32_t>(strlen(result.next_cursor)), 0, false};
      root_.kv->Write(&cursor_write, 1);
    }

    // Orphan chunks: their record expired, lost its bytes, or names fewer.
    char blob_cursor[1025] = {};
    const bool have_blob_cursor =
        root_.kv->GetStatus(kBlobCursor, -1, blob_cursor, sizeof(blob_cursor) - 1, &size) ==
        fortis::storage::KvReadStatus::Found;
    blob_cursor[have_blob_cursor ? size : 0] = '\0';
    list = KeyList{keys, 0, kMediaPrunePage};
    if (root_.kv->Page("media/blob/", have_blob_cursor && blob_cursor[0] ? blob_cursor : nullptr, kMediaPrunePage, -1, false, [&, visitor_state = &list](const char* key, const void* value, size_t size) { return (collect_key)(key, value, size, visitor_state); }, &result)) {
      auto mutations_storage = base::AllocateStorage<fortis::storage::KvMutation[]>((kMediaPrunePage + 1) * sizeof(fortis::storage::KvMutation));
      auto* mutations = mutations_storage.get();
      size_t count = 0;
      if (mutations) {
        for (size_t i = 0; i < list.count; ++i) {
          const char* name = list.keys[i] + strlen("media/blob/");
          char key[33];
          if (strlen(name) < 34 || name[32] != '/') continue;
          memcpy(key, name, 32);
          key[32] = '\0';
          const unsigned long index = strtoul(name + 33, nullptr, 10);
          MediaRecord record;
          bool present = false;
          if (!root_.repository.ReadRecord(key, now, buffer, url, &record, &present)) continue;
          if (present && record.has_bytes && index < record.chunks) continue;
          mutations[count++] = fortis::storage::KvMutation{list.keys[i], nullptr, 0, 0, true};
        }
        const size_t removed = count;
        if (result.done) mutations[count++] = fortis::storage::KvMutation{kBlobCursor, nullptr, 0, 0, true};
        else mutations[count++] = fortis::storage::KvMutation{kBlobCursor, result.next_cursor,
                                                    static_cast<uint32_t>(strlen(result.next_cursor)),
                                                    0, false};
        if (root_.kv->Write(mutations, count) && orphans) *orphans = removed;
      }
      mutations_storage.reset();
    }
    discovery::MediaRecordUnlock();
    keys_storage.reset(); buffer_storage.reset(); url_storage.reset();
    return freed;
  }

int64_t MediaRuntime::Now() const {
    return have_seams && seams.clock ? seams.clock->NowMs() : wall_ms();
  }

void MediaRuntime::WorkerMain(Impl* self) { self->WorkerLoop(); }

void MediaRuntime::Answer(const Waiter* waiters, size_t count, const Outcome& outcome) {
    for (size_t i = 0; i < count; ++i) {
      http::HttpResponse response = outcome.found
          ? http::HttpResponse{200, "OK", outcome.content_type, outcome.bytes, outcome.length}
          : http::HttpResponse{404, "Not Found", kJsonType,
                         reinterpret_cast<const uint8_t*>(kNotFound), sizeof(kNotFound) - 1};
      if (outcome.found) {
        response.cache_control = kImageCache;
        response.extra_headers = kImageHeaders;
      }
      handoff->Complete(waiters[i].token, response);
    }
  }

void MediaRuntime::WorkerLoop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      Job* job = nullptr;
      while (!stopping) {
        for (Job& candidate : jobs)
          if (candidate.used && !candidate.running && (!job || candidate.sequence < job->sequence))
            job = &candidate;
        if (job) break;
        wake.Wait(lock);
      }
      if (stopping) {
        // Every queued reader gets the uniform 404 now (the app falls back
        // to the source URL) rather than a socket closed by the handoff's
        // shutdown; nothing is recorded for their keys.
        Waiter queued[kJobs * kWaitersPerJob];
        size_t count = 0;
        for (Job& candidate : jobs) {
          if (!candidate.used) continue;
          for (size_t i = 0; i < candidate.waiter_count; ++i) queued[count++] = candidate.waiters[i];
          candidate = Job{};
        }
        lock.Unlock();
        Answer(queued, count, Outcome{});
        return;
      }
      job->running = true;
      char key[33];
      memcpy(key, job->key, sizeof(key));
      lock.Unlock();

      const Outcome outcome = fetcher.Process(key);

      lock.Lock();
      Waiter waiters[kWaitersPerJob];
      const size_t count = job->waiter_count;
      memcpy(waiters, job->waiters, sizeof(waiters));
      *job = Job{};
      lock.Unlock();
      Answer(waiters, count, outcome);
    }
  }

bool MediaRuntime::Enqueue(int client, bool head, const char* key) {
    fortis::base::LockGuard lock(mutex);
    Job* job = nullptr;
    for (Job& candidate : jobs)
      if (candidate.used && strcmp(candidate.key, key) == 0 &&
          candidate.waiter_count < kWaitersPerJob) { job = &candidate; break; }
    if (!job) {
      for (Job& candidate : jobs) if (!candidate.used) { job = &candidate; break; }
      if (job) {
        *job = Job{};
        job->used = true;
        job->sequence = next_sequence++;
        memcpy(job->key, key, 33);
      }
    }
    if (!job || stopping) return false;
    uint64_t token = 0;
    if (!http::ReserveHttpResponse(client, head, kHandoffTimeoutMs, &token)) {
      if (!job->waiter_count && !job->running) *job = Job{};
      return false;
    }
    job->waiters[job->waiter_count++] = Waiter{token, head};
    wake.Signal();
    return true;
  }

void MediaRuntime::Serve(int client, const http::HttpRequest& request, bool head) {
    const http::HttpPathParam* param = HttpPathParamFind(request, "key");
    if (param && param->utf16_units > 100) {
      reject_long_key(client, request, head);
      return;
    }
    const char* key = param ? param->value : nullptr;
    if (!MediaKeyValid(key)) {
      http::HttpResponse response{400, "Bad Request", kJsonType,
                            reinterpret_cast<const uint8_t*>(kBadRequest), sizeof(kBadRequest) - 1};
      WriteHttpResponse(client, response, head);
      return;
    }
    http::HttpResponse not_found{404, "Not Found", kJsonType,
                           reinterpret_cast<const uint8_t*>(kNotFound), sizeof(kNotFound) - 1};
    auto buffer_storage = base::AllocateStorage<unsigned char[]>(kRecordBytes);
    auto* buffer = buffer_storage.get();
    auto url_storage = base::AllocateStorage<char[]>(kRecordBytes);
    auto* url = url_storage.get();
    if (!buffer || !url) {
      buffer_storage.reset(); url_storage.reset();
      WriteHttpResponse(client, not_found, head);
      return;
    }
    discovery::MediaRecordLock();
    const int64_t now = Now();
    MediaRecord record;
    bool present = false;
    const bool read = repository.ReadRecord(key, now, buffer, url, &record, &present);
    std::unique_ptr<unsigned char[], base::StorageDeleter> bytes_storage;
    unsigned char* bytes = nullptr;
    bool cached = false;
    if (read && present && record.has_bytes && record.content_type) {
      bytes_storage = base::AllocateStorage<unsigned char[]>(record.length);
          bytes = bytes_storage.get();
      cached = bytes && repository.ReadBlob(record, now, bytes);
    }
    const bool refused = read && present &&
        (record.status == MediaStatus::kFailedPermanent ||
         (record.status == MediaStatus::kFailedTransient && now - record.updated_at_ms >= 0 &&
          now - record.updated_at_ms < kMediaRetryCooldownMs));
    discovery::MediaRecordUnlock();
    url_storage.reset();
    buffer_storage.reset();
    if (cached) {
      http::HttpResponse response{200, "OK", record.content_type, bytes, record.length};
      response.cache_control = kImageCache;
      response.extra_headers = kImageHeaders;
      WriteHttpResponse(client, response, head);
      bytes_storage.reset();
      repository.TouchKey(key);  // after the reply: a read never waits on bookkeeping
      return;
    }
    bytes_storage.reset();
    // Not registered, refused, or (read error) unknowable — the app falls
    // back to the source URL.
    if (!read || !present || refused || !Enqueue(client, head, key))
      WriteHttpResponse(client, not_found, head);
  }

void MediaRuntime::Handle(int client, const http::HttpRequest& request, bool head, void* opaque) {
    static_cast<Impl*>(opaque)->Serve(client, request, head);
  }

void MediaService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

MediaService::~MediaService() { Stop(); }

bool MediaService::Start(const MediaServiceConfig& config, fortis::storage::KvStore& kv, const MediaSeams* seams) {
  if (impl_) return false;
  Impl* impl = fortis::base::New<Impl>();
  if (!impl) return false;
  impl->config = config;
  impl->kv = &kv;
  if (seams) { impl->seams = *seams; impl->have_seams = true; }
  if (!impl->fetcher.AllocateBuffers()) {
    fortis::base::Delete(impl);
    return false;
  }
  impl_.reset(impl);
  return true;
}

bool MediaService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (!impl_ || impl_->handoff) return false;
  impl_->handoff = &handoff;
  impl_->worker_started = impl_->worker.Start(Impl::WorkerMain, impl_.get(), 256 * 1024);
  // An empty, control-character or over-512-byte key is the router's 404;
  // Serve answers 414 over 100 units and 400 for anything else malformed.
  return impl_->worker_started &&
         router.Add("GET", "/v1/media/:key", Impl::Handle, impl_.get());
}

void MediaService::Stop() {
  if (!impl_) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->fetcher.Cancel();
  impl_->wake.Broadcast();
  lock.Unlock();
  if (impl_->worker_started) impl_->worker.Join();
  impl_->worker_started = false;
  impl_->fetcher.ReleaseBuffers();
  impl_.reset();
}

size_t MediaService::PruneBytes(int64_t now_ms, size_t* orphans_removed) {
  return impl_ ? impl_->pruner.Prune(now_ms, orphans_removed) : 0;
}

}  // namespace fortis::news
