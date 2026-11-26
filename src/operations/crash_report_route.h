#pragma once

#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "http_server.h"
#include "kv_store.h"

namespace fortis::operations {

// POST /v1/crash-reports, as the earlier TypeScript implementation served
// it: the device preHandler (Origin, bearer,
// quota cost 5), content-type parsing, protobuf-es decode, the Zod
// body schema, diagnostic redaction, then one KV record
// `crash/report/<ULID>` that expires after seven days. There is deliberately
// no client read route.
struct CrashReportRouteContext {
  // NativeConfig::attestation_required, applied by the shared device policy.
  bool attestation_required = false;
  // Wall clock in epoch milliseconds for createdAt and expiry; tests may
  // substitute their own.
  fortis::base::Clock* clock = nullptr;
};

bool RegisterCrashReportRoute(http::HttpRouter& router, CrashReportRouteContext* context);

// The facet's internal find(): the stored JSON record, or false when the id
// is malformed, unknown or expired at `now_ms`.
bool FindCrashReport(fortis::storage::KvStore& kv, const char* id, int64_t now_ms, char* output,
                     size_t capacity, size_t* length);

}  // namespace fortis::operations
