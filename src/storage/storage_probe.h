#pragma once

#include <stdint.h>

namespace fortis::storage {

// Tests mounted-volume read, file fsync, rename, and directory fsync.
// Returns false if any required durability primitive is unavailable.
bool storage_probe_read(uint64_t* value);
bool storage_probe_increment(uint64_t* value);

}  // namespace fortis::storage
