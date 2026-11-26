#pragma once

#include "http_server.h"

// Read-only, bounded previews of explicitly allowlisted KV namespaces.
namespace fortis::storage { class KvStore; }

namespace fortis::operations {

bool RegisterCacheExplorerRoutes(http::HttpRouter& router, fortis::storage::KvStore* price_kv = nullptr);

}  // namespace fortis::operations
