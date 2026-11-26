#pragma once

#include <array>
#include <memory>
#include <new>
#include <stddef.h>
#include <stdexcept>

// Start-up proof that the C++ runtime this binary was linked with works: the
// global allocator (operator new/delete), a standard container and smart
// pointer, and a standard exception thrown by the library, unwound through a
// frame that owns an object and caught here.
//
// The guest links libc++, libc++abi, libunwind and compiler-rt.
// Exceptions need all four plus the image's unwind tables. A broken link or
// stripped .eh_frame could
// otherwise stay hidden until the first real throw (in practice an
// allocation failure under load). ServerMain() refuses to start when this
// returns false; a throw the unwinder cannot deliver ends in std::terminate,
// which is just as visible at boot.
//
// Why this runs on every boot and is not a test: the property belongs to the
// linked guest image, and the only place that image runs is a boot. The host
// unit test (tests/base_ownership_test.cpp) proves the check itself, not the
// guest's runtime, and no test code may be compiled into the image
// (scripts/check_production_binary.py). The result is the same on every boot
// of one image, so a bad image fails its first boot, before it takes traffic,
// and a good one pays a 16-byte allocation and one throw (microseconds).
namespace fortis::base {

#ifndef __cpp_exceptions
#error "The server is built with C++ exceptions enabled (host and guest)."
#endif

inline constexpr size_t kStandardLibraryProbeSize = 16;

namespace stdlib_check_detail {

using Block = std::array<unsigned char, kStandardLibraryProbeSize>;

// Records that its destructor ran: on a throw, that the unwinder ran this
// frame's cleanup on its way to the handler.
struct UnwindWitness {
  bool* destroyed;
  ~UnwindWitness() { *destroyed = true; }
};

// A frame of its own between the throw and the handler. `index` is read
// through a volatile so the compiler cannot fold the range check away.
[[gnu::noinline]] inline unsigned char ReadChecked(const Block& block, size_t position, bool* destroyed) {
  UnwindWitness witness{destroyed};
  volatile size_t index = position;
  // Intentional bounds exception verifies guest standard-library unwinding.
  // cppcheck-suppress containerOutOfBounds
  return block.at(index);  // throws std::out_of_range past the end
}

}  // namespace stdlib_check_detail

inline bool StandardLibraryUsable() noexcept {
  using stdlib_check_detail::Block;
  std::unique_ptr<Block> block(new (std::nothrow) Block{});
  if (!block) return false;
  (*block)[3] = 7;
  bool destroyed = false;
  bool caught = false;
  try {
    if (stdlib_check_detail::ReadChecked(*block, 3, &destroyed) != 7 || !destroyed) return false;
    destroyed = false;
    (void)stdlib_check_detail::ReadChecked(*block, kStandardLibraryProbeSize, &destroyed);
  } catch (const std::out_of_range& error) {
    caught = error.what() != nullptr;
  } catch (...) {
    return false;
  }
  return caught && destroyed;
}

}  // namespace fortis::base
