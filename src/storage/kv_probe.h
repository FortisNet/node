#pragma once

#include <stdint.h>

namespace fortis::storage {

bool kv_probe_read(uint64_t* value);
bool kv_probe_increment(uint64_t* value);
bool kv_probe_healthy();

}  // namespace fortis::storage
