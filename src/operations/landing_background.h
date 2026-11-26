#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string_view>

namespace fortis::operations {

// Project-owned dashboard background embedded byte-for-byte in read-only data.
const uint8_t* LandingBackground(size_t* length);

/** Borrowed immutable asset bytes and MIME type, valid for the process lifetime.
 * Empty for unrecognized paths. Lookup is allocation-free and thread-safe. */
struct NodeBrandAsset {
  const uint8_t* bytes = nullptr;
  size_t length = 0;
  const char* content_type = nullptr;
};
NodeBrandAsset FindNodeBrandAsset(std::string_view path);

}  // namespace fortis::operations
