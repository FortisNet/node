#pragma once
#include <string.h>

// Explicit malformed/non-canonical request cases whose old accept set was
// protobuf-es-specific. BOM is retained as string content; downstream string
// schemas can therefore refuse it. Unknown *valid* fields remain accepted.
namespace strict {
inline bool ProtoReject(const char* suite, const char* name) {
  struct Row { const char* suite; const char* name; };
  static const Row rows[] = {
    {"comparison", "protobufFiatBom"},
    {"quotes", "pb-amount-bom"}, {"quotes", "pb-provider-bom"},
    {"quotes", "pb-slippage-uint32-wrap"}, {"quotes", "pb-amount-wire-varint"},
    {"quotes", "pb-slippage-wire-ld"}, {"quotes", "pb-from-wire-fixed32"}, {"quotes", "pb-endpoint-overrun"},
    {"status", "pb-provider-bom"}, {"status", "pb-decimals-uint32-wrap"},
    {"status", "pb-provider-wire-varint"}, {"status", "pb-decimals-wire-ld"},
    {"status", "pb-tx-wire-fixed64"}, {"status", "pb-unknown-11-byte-varint"},
    {"R43", "pb-from-bom"}, {"R43", "pb-to-bom"}, {"R43", "pb-data-bom"}, {"R43", "pb-value-bom"},
    {"R43", "pb-call-overrun"}, {"R43", "pb-from-varint-wire"}, {"R43", "pb-call-fixed32-wire"},
    {"R43", "pb-unknown-varint-long"}, {"R43", "pb-varint-11-bytes"},
    {"R44", "pb-ticket-bom"}, {"R44", "pb-signature-bom"}, {"R44", "pb-unknown-fields"}, {"R44", "pb-ticket-varint-wire"},
    {"R45", "pb-from-bom"}, {"R45", "pb-gas-bom"}, {"R45", "pb-call-overrun"},
    {"bittensor", "pb-path-wire-fixed32"}, {"bittensor", "pb-unknown-long-varint"},
    {"bittensor", "pb-entry-unknown-before-key"},
    {"markets", "pb-fiat-bom"}, {"markets", "pb-unknown-long-varint"},
  };
  for (const auto& row : rows)
    if (!strcmp(suite, row.suite) && !strcmp(name, row.name)) return true;
  return false;
}
}  // namespace strict
