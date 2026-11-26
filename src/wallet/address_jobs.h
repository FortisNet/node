#pragma once

#include <memory>
#include "address_facets.h"
#include "ownership.h"

#include <stddef.h>
#include <stdint.h>

// The TypeScript EvmChangeDetector with the CHANGE_DETECTION policy
// budgets. Balances-only coverage, as the
// TypeScript composition runs it (no log-capable RPC is configured).
//
// Native bounds (documented differences): at most kMaxWatched watched
// accounts across all EVM networks per pass (TypeScript: 10,000 per
// network), and the dirty-account ingests of a pass run one after another
// instead of two at a time.
namespace fortis::wallet::address {

class ChangeDetector {
 public:
  static constexpr size_t kMaxWatched = 2048;
  ChangeDetector();
  ~ChangeDetector();
  ChangeDetector(const ChangeDetector&) = delete;
  ChangeDetector& operator=(const ChangeDetector&) = delete;
  // One pass; returns how many accounts were observed.
  size_t Run(Facets& facets, const FacetDeps& deps, Arena& arena);

 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

}  // namespace fortis::wallet::address
