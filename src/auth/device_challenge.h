#pragma once
#include "kv_store.h"
namespace fortis::http { class HttpRouter; }

namespace fortis::auth {

bool RegisterDeviceChallengeRoute(http::HttpRouter& router);

bool PersistDeviceChallenge(storage::KvStore* kv, const char* challenge, int64_t now_ms);
}
