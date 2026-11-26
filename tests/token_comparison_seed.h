#pragma once

namespace fortis::storage { class KvStore; }

// Test-only loader for the POST /v1/tokens/compare replay
// (tests/token_comparison_route_test.py writes the file from the frozen
// TypeScript fixture's `seed`). One line per stored field:
//
//   bucket \t network \t tokenId \t field \t kind \t value
//
// bucket: registry | comparison. kind: `str` (value = hex of the TypeScript
// string), `num` (a JavaScript number literal), `date` (epoch ms of a Date),
// `null`, or `corrupt` (field and value ignored: an undecodable document is
// written for the bucket key, the native form of a store read that throws).
// Values are converted exactly as the TypeScript repository reads them
// (Number() for number fields, Date-only dates, string-only strings, the four
// liquidity statuses) and written through TokenComparisonStore's public
// patch API. False on any malformed line or failed write.
bool LoadTokenComparisonSeedFile(fortis::storage::KvStore& kv, const char* path);
