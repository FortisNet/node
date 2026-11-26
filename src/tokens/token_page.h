#pragma once

#include "token_facts.h"

#include <stddef.h>
#include <stdint.h>

// Native ports of the TypeScript TokenPageFacet.get, the legacy
// Token360Facet.get document and the read side of the token comparison that
// the page's comparison section uses. Documents are built in the work arena
// in TypeScript key order. Guest-safe.
namespace fortis::tokens::token_page {

using token_facts::Work;
using codec::tj::Value;

enum Section : uint8_t {
  kIdentity, kQuote, kMarket, kProfile, kLiquidity, kHolders, kSafety, kContract, kComparison,
  kInsight, kSeries, kSectionCount
};
const char* SectionName(Section section);

// The included sections in Set insertion order.
struct Sections {
  Section order[kSectionCount];
  size_t count = 0;
  uint32_t mask = 0;
  void Add(Section section);
  bool Has(Section section) const { return (mask & (1u << section)) != 0; }
};

// The TypeScript parseSections(raw). `raw` is the trimmed `include` value
// (null when absent). False with `unknown` (the "unknown include: …" detail,
// `*unknown_length` bytes, NUL bytes of a name kept) for an unknown name.
// The detail of a 200-unit include fits kUnknownIncludeBytes: each name is
// lower-cased (U+0130 grows from 2 to 3 bytes, nothing grows more).
constexpr size_t kUnknownIncludeBytes = 1024;
bool ParseSections(const char* raw, size_t length, Sections* sections, char* unknown, size_t unknown_capacity,
                   size_t* unknown_length);
void DefaultSections(Sections* sections);

enum class SeriesInterval : uint8_t { k1h, k1d };

struct PageOptions {
  Sections sections;
  double days = 200;
  SeriesInterval interval = SeriesInterval::k1d;
  const char* coingecko_hint = nullptr;
};

// One query key as fast-querystring decoded it: the first value (`length`
// bytes, valid UTF-8) and how often the key occurred (0: absent; more than
// one is an array, which every page field refuses).
struct PageQueryValue {
  const char* text = "";
  size_t length = 0;
  size_t count = 0;
};
enum class PageQueryStatus : uint8_t {
  kOk,
  kInvalid,         // a pageQuerySchema issue: the bare 400
  kUnknownInclude,  // parseSections: 400 with `detail`
};
// The TypeScript pageQuerySchema then parseSections over decoded values:
//   include     z.string().trim().max(200) (UTF-16 units), optional;
//   days        z.coerce.number().int().min(1).max(365).default(200) (Number());
//   interval    z.enum(['1h', '1d']).default('1d');
//   coingeckoId z.string().trim().toLowerCase().regex(/^[a-z0-9][a-z0-9-]{0,99}$/).
// `scratch` holds `scratch_capacity` bytes for Number() (at least the days
// value's length + 1). On kOk `options` is filled and `hint` (>= 101 bytes)
// holds the lower-cased hint when `*has_hint`; on kUnknownInclude `detail`
// holds `*detail_length` bytes (capacity kUnknownIncludeBytes).
PageQueryStatus ParsePageQuery(const PageQueryValue& include, const PageQueryValue& days,
                               const PageQueryValue& interval, const PageQueryValue& coingecko_id, char* scratch,
                               size_t scratch_capacity, PageOptions* options, char* hint, size_t hint_capacity,
                               bool* has_hint, char* detail, size_t detail_capacity, size_t* detail_length);

// The facts sub-facets TokenPageFacet.get asks for (its `only` set, a
// FacetBit mask) for `sections` with identity added: chainMeta always,
// market for market/quote/safety/comparison, profile, liquidity for
// liquidity/quote/safety, holders for holders/safety, insight, contract.
uint32_t FacetsForSections(const Sections& requested);

// TokenPageFacet.get(network, ref, options).
token_facts::FactsOutcome PageDocument(Work& work, const char* network, size_t network_length, const char* raw_ref,
                                 size_t raw_length, const PageOptions& options, Value** document);
// TokenPageFacet.get after facts(): identity, the quote (work.env->spot),
// series (work.env->series), comparison (the comparison store and the
// benchmarks' market facts), per-section provenance and asOf over `facts`.
// kInternal only when the arena is exhausted.
token_facts::FactsOutcome ComposePage(Work& work, const token_facts::Facts& facts, const PageOptions& options,
                                Value** document);
// Token360Facet.get(network, tokenId): facts() with only market, profile,
// liquidity, holders and chainMeta, then Token360Project.
token_facts::FactsOutcome Token360Document(Work& work, const char* network, size_t network_length,
                                     const char* raw_ref, size_t raw_length, Value** document);
// The legacy wire over composed facts: {network, tokenId, meta, market,
// profile, liquidity, holders: {count, floor}, safety, errors, cache: {market,
// profile, liquidity, holders}}. False only when the arena is exhausted.
bool Token360Project(codec::tj::Arena& arena, const token_facts::Facts& facts, Value** document);
}  // namespace fortis::tokens::token_page
