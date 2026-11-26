#pragma once

#include <stddef.h>
#include <stdint.h>

// This server's IDNA (domain to ASCII) step for the URL parser: a literal
// port of ada::idna::to_ascii from Ada v2.9.2 (ada-idna generated
// 2023-09-19, the implementation behind `new URL()` in Node.js 22). Where
// Ada and UTS #46 differ, Ada's behaviour is the one implemented and tested:
// its normalization composer, the ZWNJ/ZWJ early return in is_label_valid,
// per-label bidi checks, the 0xd880 bound in punycode encoding and the
// absence of an ASCII-only check on decoded xn-- labels.
//
// Security-relevant (docs/compat-decisions.md#idna): the host that is judged
// by the address policy is the host this function produces. It may be
// replaced only by something at least as strict.
//
// No allocation and no C++ standard library: the caller passes bounded
// UTF-32 scratch. Inputs whose code points, labels or output exceed the
// bounds below are refused (returned as failure); Ada has no such bounds.
namespace fortis::compat::idna {

// Maximum code points of one host after mapping (and of one label), and the
// maximum ASCII output.
constexpr size_t kMaxCodePoints = 4096;

struct Scratch {
  uint32_t domain[kMaxCodePoints];  // mapped + normalized domain
  uint32_t work[kMaxCodePoints];    // decoded punycode label / mapping copy
  uint32_t check[kMaxCodePoints];   // post-map/normalize comparison
};

// ada::idna::to_ascii(utf8): writes the ASCII result (not NUL-terminated)
// and its length. False on any error, including Ada's empty-string result.
bool ToAscii(const char* utf8, size_t length, char* output, size_t capacity,
             size_t* output_length, Scratch* scratch);

// Exposed for tests.
bool PunycodeToUtf32(const char* input, size_t length, uint32_t* out, size_t capacity,
                     size_t* out_length);
bool Utf32ToPunycode(const uint32_t* input, size_t size, char* output, size_t capacity,
                     size_t* output_length);

}  // namespace fortis::compat::idna

// Tables extracted from the pinned Ada source (src/compat/idna_tables.cpp).
namespace fortis::compat::idna_tables {
extern const uint32_t kMappings[];
extern const size_t kMappingsCount;
extern const uint32_t kMappingTable[];  // pairs {first code point, descriptor}
extern const size_t kMappingTableCount;
extern const uint8_t kDecompositionIndex[];
extern const size_t kDecompositionIndexCount;
extern const uint16_t kDecompositionBlock[];  // [67][257]
extern const size_t kDecompositionBlockCount;
extern const uint32_t kDecompositionData[];
extern const size_t kDecompositionDataCount;
extern const uint8_t kCccIndex[];
extern const size_t kCccIndexCount;
extern const uint8_t kCccBlock[];  // [67][256]
extern const size_t kCccBlockCount;
extern const uint8_t kCompositionIndex[];
extern const size_t kCompositionIndexCount;
extern const uint16_t kCompositionBlock[];  // [67][257]
extern const size_t kCompositionBlockCount;
extern const uint32_t kCompositionData[];
extern const size_t kCompositionDataCount;
extern const uint32_t kCombining[];
extern const size_t kCombiningCount;
extern const uint32_t kVirama[];
extern const size_t kViramaCount;
extern const uint32_t kJoiningR[];
extern const size_t kJoiningRCount;
extern const uint32_t kJoiningL[];
extern const size_t kJoiningLCount;
extern const uint32_t kJoiningD[];
extern const size_t kJoiningDCount;
extern const uint32_t kDirections[];  // triples {start, final, direction}
extern const size_t kDirectionsCount;
}  // namespace fortis::compat::idna_tables
