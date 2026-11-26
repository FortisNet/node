#include "threading.h"
#pragma once
#include "ownership.h"

#include "token_json.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::config { struct ProviderRuntimeConfig; }

// The TypeScript Bittensor driver's subnet token reads (tokenMetadata,
// tokenMarket, tokenInsight) and Token360's composition of the
// market row (chainNativeMarketRow). The driver exists
// only when TAOSTATS_API_KEY is configured: an unkeyed deployment has no
// tokenMetadata/tokenMarket/tokenInsight, so a subnet page answers honest
// nulls without a read.
//
// Two keyed Taostats GETs on the 'taostats' lane against the network's
// endpoints (token_provider: kTaostats, Authorization = the key):
//  - the pool: `/api/dtao/pool/latest/v1?network=finney&netuid=N&limit=1`,
//    shared by identity (chainMeta), market and insight; a row is accepted
//    only when Number(row.netuid) is the requested netuid;
//  - the TAO/USD row: `/api/price/latest/v1?asset=tao`, read only by the
//    market and fail-soft (Token360 then falls back to its ordinary
//    CoinGecko TAO quote).
// The driver keeps both answers for 15 s (subnetCache, 256 netuids, and
// taoPriceCache); the WorkingSet below is that state. Failed reads are never
// kept.
//
// Provider bodies are read with JavaScript property semantics. Projections
// work on a "pool page": the selected row reduced to the values the driver
// derives from it (sanitized symbol/name, finite numbers already scaled from
// rao to TAO, the supply string), or JSON null for an absent pool. Numbers
// keep their sign (−0 formats as "-0"). Guest-safe.
namespace fortis::tokens::token_bittensor {

using codec::tj::Arena;
using codec::tj::Value;

// The driver exists (TAOSTATS_API_KEY configured).
bool Keyed(const config::ProviderRuntimeConfig* config);

// subnetNetuid(address): /^\d{1,5}$/ with a value in 1..65,535.
bool SubnetNetuid(const char* address, uint32_t* netuid);

// `/api/dtao/pool/latest/v1?network=finney&netuid=${netuid}&limit=1`.
bool PoolPath(uint32_t netuid, char* out, size_t capacity);
constexpr const char* kTaoPricePath = "/api/price/latest/v1?asset=tao";

// readSubnetPool's selection over the parsed body: `body.data?.[0]`, kept
// when `Number(pool.netuid) === netuid`, reduced to a pool page. JSON null
// for no matching row. nullptr where the driver throws (a JSON null body or
// a null row: `null.data`, `null.netuid`) or on arena exhaustion.
Value* PoolPage(Arena& arena, const Value* body, uint32_t netuid);

// readTaoPriceUsd: `finite(row?.price)` of `data[0]` (an array) or `data`
// as a number, or JSON null. nullptr where it throws (a JSON null body) or
// on arena exhaustion.
Value* TaoPricePage(Arena& arena, const Value* body);

// tokenMetadata over a pool page: JSON null for an absent pool, else
// {symbol, name, decimals: 9, verified: null, holderCount: null,
// logoMediaKey: null, totalSupply, maxSupply: null}.
Value* Metadata(Arena& arena, const Value* pool);

// tokenInsight over a pool page: [] for an absent pool, else the "Bittensor
// subnet pool" section (Netuid, then the present Price / Market cap /
// Liquidity / TAO in pool / Alpha in pool / Alpha staked rows).
Value* Insight(Arena& arena, uint32_t netuid, const Value* pool);

// Token360's market row for tokenMarket's answer: JSON null for an absent
// pool, else chainNativeMarketRow(market, nativePriceUsd) with `source:
// 'taostats'`. `has_usd`/`usd` is the TAO/USD price after the fallback.
Value* MarketRow(Arena& arena, const Value* pool, bool has_usd, double usd);

// `value.toLocaleString('en-US', { maximumFractionDigits: 9 })` of a finite
// double: the shortest round-trip decimal rounded half away from zero to nine
// fraction digits, trailing zeros dropped, the integer part grouped by
// commas; a negative value that rounds to zero (and −0) is "-0". False when
// `capacity` is too small (a finite double needs at most 420 bytes).
bool FormatEnUs(double value, char* out, size_t capacity);

// The driver's 15 s working set: pool pages by netuid (at most 256: expired
// entries are dropped first, then the oldest) and the one TAO/USD page.
// Copies live in allocated blocks; a pool page whose copy exceeds
// kMaxEntryBytes is not kept (only an absurd supply string reaches it).
// Thread-safe; times are the token clock (epoch ms).
class WorkingSet {
 public:
  static constexpr int64_t kTtlMs = 15000;
  static constexpr size_t kMaxEntries = 256;
  static constexpr size_t kMaxEntryBytes = 2048;

  WorkingSet();
  ~WorkingSet();
  WorkingSet(const WorkingSet&) = delete;
  WorkingSet& operator=(const WorkingSet&) = delete;

  // A live copy of the page in `arena`, or false (absent or expired).
  bool Pool(uint32_t netuid, int64_t now_ms, Arena& arena, Value** page);
  void RememberPool(uint32_t netuid, int64_t now_ms, const Value* page);
  bool Price(int64_t now_ms, Arena& arena, Value** page);
  void RememberPrice(int64_t now_ms, const Value* page);

  size_t pool_count();
  size_t retained_bytes();

 private:
  struct Entry {
    bool used = false;
    uint32_t netuid = 0;
    int64_t expires = 0;
    uint64_t order = 0;
    std::unique_ptr<uint8_t[], base::StorageDeleter> blob;
    size_t blob_length = 0;
  };
  fortis::base::Mutex mutex_;
  Entry entries_[kMaxEntries];
  uint64_t next_order_ = 1;
  bool price_set_ = false;
  int64_t price_expires_ = 0;
  bool price_has_ = false;
  double price_ = 0;
};

// Process-lifetime working set (null on allocation failure: every read then
// goes to the provider).
WorkingSet* NewWorkingSet();
void DeleteWorkingSet(WorkingSet* set);

}  // namespace fortis::tokens::token_bittensor
