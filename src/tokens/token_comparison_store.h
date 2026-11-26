#pragma once

#include <stddef.h>
#include <stdint.h>

#include "threading.h"
#include "token_comparison.h"

// Native read model behind POST /v1/tokens/compare: the 'registry' (token
// identity) and 'comparison' (market + liquidity metrics) buckets of the
// TypeScript TokenStore, on the runtime KvStore. Writers are the token market
// warmer and liquidity history
// (TokenStore.patch semantics: present fields overwrite, explicit null
// removes, absent fields are kept), which stage comparison patches into the
// same KV frame as their facts and observation rows (token_writes.h). The
// route only reads.
//
// Keys mirror TypeScript: `token/registry/{enc(network)}/{enc(tokenId)}` and
// `token/comparison/{enc(network)}/{enc(tokenId)}`, enc = encodeURIComponent.
// Values are a native binary document, not TypeScript JSON (no historic data
// migration is required):
//
//   "FTS1" | { u8 field id, u8 type, payload }*   (ids ascending, unique)
//   type 1 number: 8-byte little-endian IEEE double (finite)
//   type 2 date:   8-byte little-endian int64 epoch ms in [0, kMaxDateMs]
//   type 3 string: u16 little-endian length (<= kMaxStoredString) + UTF-8
//   type 4 status: u8 (1 ok, 2 not_found, 3 unsupported, 4 error)
//
// Numbers are stored already converted by TypeScript's Number() rule (the
// repository's `number()` read helper), so a writer passes doubles; a
// non-finite value is stored as absent. Rows never expire, like TypeScript.
namespace fortis::storage { class KvStore; class KvBatch; }

namespace fortis::tokens {

namespace token_store {

enum class FieldState : uint8_t { kUnchanged, kNull, kValue };

struct StringPatch {
  FieldState state = FieldState::kUnchanged;
  const char* text = nullptr;
  size_t length = 0;
};
struct NumberPatch {
  FieldState state = FieldState::kUnchanged;
  double value = 0;
};
struct DatePatch {
  FieldState state = FieldState::kUnchanged;
  int64_t ms = 0;
};
struct StatusPatch {
  FieldState state = FieldState::kUnchanged;
  comparison::LiquidityStatus value = comparison::LiquidityStatus::kNone;
};

// TokenStore 'registry' fields read by the comparison repository.
struct IdentityPatch {
  StringPatch symbol, name, logo_media_key, coingecko_id;
};

// TokenStore 'comparison' fields (marketComparisonProjection,
// liquidityComparisonProjection, the not-found/diagnostic writes, `fiat`).
struct ComparisonPatch {
  StringPatch fiat, market_source_id;
  NumberPatch market_price_usd, price_change_1h_pct, price_change_24h_pct,
      price_change_7d_pct, price_change_30d_pct, market_cap_usd, fdv_usd, volume_24h_usd,
      market_cap_rank;
  DatePatch market_as_of;
  StringPatch market_source;
  NumberPatch dex_price_usd, dex_market_cap_usd, dex_fdv_usd, dex_volume_24h_usd,
      liquidity_usd, spread_pct, top_pool_share, pool_count;
  StatusPatch liquidity_status;
  DatePatch liquidity_checked_at, liquidity_as_of;
  StringPatch liquidity_source;
  DatePatch updated_at;
};

struct Key {
  const char* network = nullptr;
  const char* token_id = nullptr;
};

enum class ReadStatus : uint8_t { kOk, kIoError, kCorrupt };

// One 'registry' row as stored (the symbol is not upper-cased). The
// Token360 port reads it for identity and coin-id resolution.
struct IdentityRecord {
  bool found = false;
  comparison::OptString symbol, name, logo_media_key, coingecko_id;
};

// Visits rows of one network's registry prefix in key order; return false
// to stop.
using IdentityVisitor = bool (*)(void* context, const IdentityRecord& row);

}  // namespace token_store

// Borrowed KV. Patches are serialized by a process-wide writer mutex (a
// read-modify-write of one document); reads need no lock beyond the KV's.
class TokenComparisonStore {
 public:
  // Holds the process-wide writer mutex for a multi-document transaction
  // (token_writes.h). Lock order: TokenDataStore::Lock, then this, then the
  // KvStore's own mutex. Patch* take it themselves: never call them while
  // holding it.
  class WriterLock {
   public:
    WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;

   private:
    fortis::base::LockGuard guard_;
  };

  explicit TokenComparisonStore(storage::KvStore* kv) : kv_(kv) {}
  // False for invalid input (a string over kMaxStoredString bytes or not
  // valid UTF-8, a date out of range, a key over the KV bound), a corrupt
  // existing document, or a KV failure; nothing is written then.
  bool PatchIdentity(const char* network, const char* token_id,
                     const token_store::IdentityPatch& patch, int64_t now_ms);
  bool PatchComparison(const char* network, const char* token_id,
                       const token_store::ComparisonPatch& patch, int64_t now_ms);
  // PatchComparison staged into `batch` (reading a prior value staged in the
  // same batch) instead of committed. Requires a WriterLock held for the
  // life of the batch. False as PatchComparison, or on batch.full().
  bool PrepareComparison(storage::KvBatch& batch, const char* network, const char* token_id,
                         const token_store::ComparisonPatch& patch, int64_t now_ms);
  // EmbeddedTokenComparisonRepository.read(keys, fiat): one record per key,
  // in order, identity and metrics merged; metrics only when the stored
  // `fiat` equals `fiat`. records[i].network/token_id are copied from keys.
  // A key too long for the KV reads as absent. kIoError/kCorrupt mean the
  // records are unusable (TypeScript throws).
  token_store::ReadStatus Read(const token_store::Key* keys, size_t count, const char* fiat,
                               int64_t now_ms, comparison::Record* records) const;
  // TokenStore.get('registry', network, tokenId).
  token_store::ReadStatus ReadIdentity(const char* network, const char* token_id, int64_t now_ms,
                                       token_store::IdentityRecord* record) const;
  // TokenStore.forEach('registry') restricted to one network's key prefix,
  // visiting at most `limit` rows (a native bound; TypeScript scans the
  // whole bucket). Unreadable rows are skipped. *truncated reports the bound.
  token_store::ReadStatus ScanIdentities(const char* network, size_t limit, int64_t now_ms,
                                         token_store::IdentityVisitor visitor, void* context,
                                         bool* truncated) const;

 private:
  storage::KvStore* kv_;
};

}  // namespace fortis::tokens
