#pragma once

#include "token_comparison_store.h"
#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// Pure ports of the TypeScript token page projections: token safety,
// the market view (marketView, profileView), the Token360 CoinGecko
// profile trim, marketWithChainSupply and the comparison read-model
// projections. Guest-safe.
namespace fortis::tokens::token_views {

struct SafetyInputs {
  bool is_native = false;
  bool coingecko_listed = false;
  bool has_liquidity = false;
  double liquidity_usd = 0;
  bool has_holders = false;
  double holder_count = 0;
  bool has_volume = false;
  double volume_24h_usd = 0;
  bool has_market_cap = false;
  double market_cap_usd = 0;
  bool has_age = false;
  double trading_age_days = 0;
};

// scoreTokenSafety(inputs) as {score, level, signals}.
codec::tj::Value* ScoreTokenSafety(codec::tj::Arena& arena, const SafetyInputs& inputs);

// marketView(row) / profileView(row); null row -> JSON null value.
codec::tj::Value* MarketView(codec::tj::Arena& arena, const codec::tj::Value* row);
codec::tj::Value* ProfileView(codec::tj::Arena& arena, const codec::tj::Value* row);

// Token360.trimProfile(raw) with the logo registered through the media
// registry (`kv`, `now_ms`). False where TypeScript throws (a truthy
// non-string `description.en`).
bool TrimProfile(codec::tj::Arena& arena, const codec::tj::Value* raw, storage::KvStore* kv, int64_t now_ms,
                 codec::tj::Value** profile);

// marketWithChainSupply(market, chainMeta).
codec::tj::Value* MarketWithChainSupply(codec::tj::Arena& arena, const codec::tj::Value* market, const codec::tj::Value* chain_meta);

// Token360Facet.chainMeta's Jupiter branch over the exact-mint search row:
// {symbol, name, decimals, verified, holderCount, logoMediaKey} with
// sanitized string names, Number.isInteger decimals, a boolean verification,
// any numeric holder count and the icon registered as media (null for a
// non-http(s) or absent icon). Null only on arena exhaustion.
codec::tj::Value* SolanaChainMeta(codec::tj::Arena& arena, const codec::tj::Value* row, storage::KvStore* kv, int64_t now_ms);
// Token360Facet.solanaInsight(meta), appended to `sections`: the "Token
// list" section, nothing for a null meta or one without any row. False on
// arena exhaustion.
bool AppendSolanaInsight(codec::tj::Arena& arena, const codec::tj::Value* meta, codec::tj::Value* sections);

// marketComparisonProjection / liquidityComparisonProjection as native
// TokenComparisonStore patches (fiat 'usd').
void MarketComparisonPatch(codec::tj::Arena& arena, const codec::tj::Value* market, int64_t as_of_ms,
                           const char* source_id, const char* source, token_store::ComparisonPatch* patch);
void LiquidityComparisonPatch(const codec::tj::Value* snapshot, int64_t as_of_ms, const char* source,
                              token_store::ComparisonPatch* patch);

// `typeof v === 'number' && Number.isFinite(v)`.
bool FiniteNumber(const codec::tj::Value* v, double* out);
// finiteNumber(v): a number or a numeric string, finite.
bool FiniteNumberOrString(const codec::tj::Value* v, double* out);

}  // namespace fortis::tokens::token_views
