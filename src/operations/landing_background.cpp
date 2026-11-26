#include "landing_background.h"

// FORTIS_LANDING_BACKGROUND_PATH is the absolute path of the project-owned asset,
// set by the Makefile.
#ifndef FORTIS_LANDING_BACKGROUND_PATH
#error "FORTIS_LANDING_BACKGROUND_PATH must name assets/background.jpg"
#endif

#if defined(__APPLE__)
#define FORTIS_ASSET_SYMBOL(name) "_" name
#define FORTIS_ASSET_SECTION ".const_data\n"
#define FORTIS_ASSET_RESTORE ".text\n"
#else
#define FORTIS_ASSET_SYMBOL(name) name
#define FORTIS_ASSET_SECTION ".pushsection .rodata,\"a\"\n"
#define FORTIS_ASSET_RESTORE ".popsection\n"
#endif

namespace fortis::operations {

__asm__(FORTIS_ASSET_SECTION
        ".balign 16\n"
        ".globl " FORTIS_ASSET_SYMBOL("fortis_landing_background_start") "\n"
        FORTIS_ASSET_SYMBOL("fortis_landing_background_start") ":\n"
        ".incbin \"" FORTIS_LANDING_BACKGROUND_PATH "\"\n"
        ".globl " FORTIS_ASSET_SYMBOL("fortis_landing_background_end") "\n"
        FORTIS_ASSET_SYMBOL("fortis_landing_background_end") ":\n"
        FORTIS_ASSET_RESTORE);

}  // namespace fortis::operations

extern "C" const uint8_t fortis_landing_background_start[];
extern "C" const uint8_t fortis_landing_background_end[];

namespace fortis::operations {

namespace {
#include "node_assets.inc"
}  // namespace

NodeBrandAsset FindNodeBrandAsset(std::string_view path) {
  if (path == "/assets/node-hall.webp") return {kNodeAsset0, sizeof(kNodeAsset0), "image/webp"};
  if (path == "/assets/fortis-favicon.svg") return {kNodeAsset1, sizeof(kNodeAsset1), "image/svg+xml"};
  if (path == "/assets/fortisnet-lockup.svg") return {kNodeAsset2, sizeof(kNodeAsset2), "image/svg+xml"};
  if (path == "/assets/social-preview.jpg") return {kNodeAsset3, sizeof(kNodeAsset3), "image/jpeg"};
  return {};
}

const uint8_t* LandingBackground(size_t* length) {
  if (length) {
    const auto begin = reinterpret_cast<uintptr_t>(fortis_landing_background_start);
    const auto end = reinterpret_cast<uintptr_t>(fortis_landing_background_end);
    *length = end >= begin ? static_cast<size_t>(end - begin) : 0;
  }
  return fortis_landing_background_start;
}

}  // namespace fortis::operations
