#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::operations {

/** Startup route switches for the public service checklist. Values are owned,
 * contain no secrets and describe registration, not provider health. */
struct NodeServices {
  bool prices = false;
  bool news = false;
  bool addresses = false;
  bool ai = false;
  bool cross_chain = false;
  bool stocks = false;
  bool payments = false;
};

extern const char kNodeHomeBeforeServices[];
extern const char kNodeHomeAfterServices[];
extern const char kNodeFutureFeaturesJson[];

/** Render UTF-8 home HTML without a terminator into borrowed caller storage.
 * Returns bytes written, or zero for null storage / insufficient capacity;
 * failed output must be discarded. No allocations, I/O or shared mutations. */
size_t RenderNodeHome(const NodeServices& services, char* output, size_t capacity);

// The public landing page and error board HTML live directly in
// src/operations/operations_pages.cpp. Text segments are UTF-8 without a terminator
// requirement; only slot segments are filled by RenderLandingPage.
enum class LandingSlot : uint8_t {
  kText,
  kNetworks,
  kFamilies,
  kBalances,
  kTransfers,
  kNfts,
  kInsight,
  kTxStatus,
  kToken,
  kTrustlines,
  kTxDetail,
  kDocsLink,
};

struct LandingSegment {
  LandingSlot slot;
  const char* text;
  size_t length;
};

struct LandingCounts {
  uint32_t networks = 0;
  uint32_t families = 0;
  uint32_t balances = 0;
  uint32_t transfers = 0;
  uint32_t nfts = 0;
  uint32_t insight = 0;
  uint32_t tx_status = 0;
  uint32_t token = 0;
  uint32_t trustlines = 0;
  uint32_t tx_detail = 0;
};

extern const char kOperationsPackageVersion[];
extern const LandingSegment kLandingSegments[];
extern const size_t kLandingSegmentCount;
extern const char kLandingDocsEnabled[];
extern const char kLandingDocsDisabled[];
extern const char kErrorsPageHtml[];
extern const size_t kErrorsPageHtmlLength;
extern const char kBenchmarkPageHtml[];
extern const size_t kBenchmarkPageHtmlLength;

// Renders the landing page into caller storage. Returns the byte length, or
// zero when `capacity` is too small. The page is static for the process life.
size_t RenderLandingPage(const LandingCounts& counts, bool docs_enabled,
                         char* output, size_t capacity);

}  // namespace fortis::operations
