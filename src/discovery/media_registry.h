#pragma once

#include "kv_store.h"
#include "market_view.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::discovery {

// The TypeScript MediaFacet.register for one source URL: an http(s) URL gets
// its content-addressed key (sha256(url)[:32]) and, unless a record already
// exists, a pending registration record so GET /v1/media/{key} may later
// proxy it. Non-http(s) values get no key.
//
// Contract with the media service, which serves these records: KV key
// "media/item/<key>", 7-day expiry, value = the TypeScript StoredMedia JSON
// {"key","sourceUrl","status":"pending","bytes":null,"contentType":null,
//  "sizeBytes":null,"fetchedAt":null,"lastAccessAt":null,"fetchCount":0,
//  "updatedAt":<ISO>}. Registration never overwrites an existing record. A KV
// failure is ignored like the TypeScript `logError` path: the key is still
// returned. The check-then-insert runs under MediaRecordLock, the one lock
// every media record read-modify-write in this process takes (price markets,
// the news ingester and the media service), standing in for the TypeScript
// embedded-store write transaction.
constexpr int64_t kMediaRegistrationTtlMs = 7LL * 24 * 3600000;

void MediaRecordLock();
void MediaRecordUnlock();

bool RegisterMediaSource(fortis::storage::KvStore* kv, const char* url, size_t length, int64_t now_ms,
                         char key[fortis::discovery::market_view::kMediaKeyChars + 1]);

}  // namespace fortis::discovery
