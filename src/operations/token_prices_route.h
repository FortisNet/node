#pragma once

#include "http_server.h"
#include "kv_store.h"

namespace fortis::operations {

// Public, read-only view of USD spot rows and five-minute candles already in
// the server's price series store. It never calls a provider or backfills.
// `price_kv` is the split price store (FORTIS_SPLIT_PRICE_STORE=1), where the
// series rows live; null means prices share the main runtime store. The store
// must outlive the router.
bool RegisterTokenPricesRoutes(http::HttpRouter& router,
                               fortis::storage::KvStore* price_kv = nullptr);

}  // namespace fortis::operations
