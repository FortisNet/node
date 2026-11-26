#pragma once

#include <stddef.h>

namespace fortis::operations {

// Longest configured METRICS_TOKEN, after trimming (native_config caps it).
constexpr size_t kMetricsTokenMaxBytes = 512;

// The GET /metrics bearer policy. True only when `authorization` (the
// Authorization header value, already stripped of surrounding SP/HTAB by the
// request parser) is exactly "Bearer " followed by `token`, byte for byte:
// the scheme is case-sensitive, exactly one space, no extra bytes either
// side. This is TypeScript's `authorization !== \`Bearer ${expected}\``.
//
// A null or empty token, or one longer than kMetricsTokenMaxBytes, never
// matches (the route is disabled or misconfigured, never open). The scan
// always covers kMetricsTokenMaxBytes positions, so the time taken does not
// depend on where a wrong byte sits or on the token's length; only the
// "Bearer " prefix check exits early, and it reveals nothing secret. Neither
// value is copied, logged or retained.
bool MatchesMetricsBearer(const char* authorization, const char* token);

}  // namespace fortis::operations
