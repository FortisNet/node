#include <algorithm>
#include "threading.h"
#include "media_registry.h"

#include "discovery_cache.h"
#include "price_history_batch_codec.h"

#include <pthread.h>
#include <string.h>

namespace fortis::discovery {

namespace {
fortis::base::Mutex g_media_record_mutex;
// The registration scratch is shared, so it is guarded by the record lock.
// An 8 KiB URL escapes to at most 48 KiB of JSON.
uint8_t g_record[64 * 1024];
}  // namespace

void MediaRecordLock() { g_media_record_mutex.Lock(); }
void MediaRecordUnlock() { g_media_record_mutex.Unlock(); }

bool RegisterMediaSource(fortis::storage::KvStore* kv, const char* url, size_t length, int64_t now_ms,
                         char key[fortis::discovery::market_view::kMediaKeyChars + 1]) {
  if (!url || !fortis::discovery::market_view::IsHttpUrl(url, length)) return false;
  fortis::discovery::market_view::MediaKeyFor(url, length, key);
  if (!kv) return true;
  char record_key[64];
  std::copy_n("media/item/", 11, record_key);
  memcpy(record_key + 11, key, fortis::discovery::market_view::kMediaKeyChars + 1);
  unsigned char probe = 0;
  size_t size = 0;
  MediaRecordLock();
  if (kv->GetStatus(record_key, now_ms, &probe, sizeof(probe), &size) !=
      fortis::storage::KvReadStatus::Missing) {  // present, or unreadable: never overwrite
    MediaRecordUnlock();
    return true;
  }
  uint8_t* record = g_record;
  fortis::codec::history_batch::OutBuffer out{record, sizeof(g_record)};
  char updated[32];
  if (!DiscoveryIsoTimestamp(now_ms, updated)) { MediaRecordUnlock(); return true; }
  out.Text("{\"key\":\"");
  out.Text(key);
  out.Text("\",\"sourceUrl\":");
  fortis::codec::history_batch::AppendJsonString(out, url, length);
  out.Text(",\"status\":\"pending\",\"bytes\":null,\"contentType\":null,\"sizeBytes\":null,"
           "\"fetchedAt\":null,\"lastAccessAt\":null,\"fetchCount\":0,\"updatedAt\":\"");
  out.Text(updated);
  out.Text("\"}");
  if (!out.ok) { MediaRecordUnlock(); return true; }
  const fortis::storage::KvMutation mutation{record_key, record, static_cast<uint32_t>(out.used),
                                    now_ms + kMediaRegistrationTtlMs, false};
  kv->Write(&mutation, 1);
  MediaRecordUnlock();
  return true;
}

}  // namespace fortis::discovery
