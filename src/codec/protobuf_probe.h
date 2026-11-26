#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::codec {

// Decode and re-encode the checked-in DashPayNameResponse protobuf schema.
// This is a substrate probe, not an authenticated production API handler.
bool protobuf_roundtrip(const uint8_t* input, size_t input_size,
                        uint8_t* output, size_t output_capacity,
                        size_t* output_size);

}  // namespace fortis::codec
