#pragma once

#include "http_server.h"
#include "network_registry.h"

namespace fortis::operations {

// Public, read-only preview of the startup network catalogue and the
// per-network popular-token cache. No provider work or device data is read.
bool RegisterNetworkPreviewRoutes(http::HttpRouter& router,
                                  const networks::NativeNetworkRegistry* registry);

}  // namespace fortis::operations
