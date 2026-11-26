#pragma once

#include "token_json.h"
#include "token_provider.h"

#include <stddef.h>
#include <stdint.h>

// Request builders and response interpreters for the token
// upstreams, one pair per TypeScript client method:
//   CoinGeckoClient.coinMarkets / coinProfile / contractInfo / search
//   DexScreenerClient.pools, GoPlusClient.tokenSecurity,
//   EthplorerClient.holders, EtherscanHolderClient.holders,
//   EvmDriver.contractFacts (DriverTransport.jsonRpc eth_call),
//   JupiterClient.searchTokens.
// Builders return false where the TypeScript client sends nothing (or the
// request exceeds a native bound). Interpreters return kFailed wherever the
// TypeScript method throws (a non-2xx answer, invalid JSON, a provider error
// code, a TypeError on an unexpected shape) and kValue otherwise; *value may
// then be JSON null. Guest-safe.
namespace fortis::tokens::token_fetch {

enum class Outcome : uint8_t { kValue, kFailed };

bool CoinMarkets(providers::TokenFetch* fetch, const char* const* coin_ids, size_t count);
bool CoinProfile(providers::TokenFetch* fetch, const char* coin_id);
bool ContractInfo(providers::TokenFetch* fetch, const char* platform, const char* address);
bool Search(providers::TokenFetch* fetch, const char* query);
bool DexPools(providers::TokenFetch* fetch, const char* address);
bool GoPlus(providers::TokenFetch* fetch, const char* chain, const char* address);
// `key` null uses Ethplorer's public "freekey".
bool EthplorerHolders(providers::TokenFetch* fetch, const char* address, const char* key);
bool EtherscanHolders(providers::TokenFetch* fetch, uint32_t chain_id, const char* address, const char* key);
bool EvmContractFacts(providers::TokenFetch* fetch, const char* const* rpc_urls, size_t url_count,
                      const char* address);
// searchTokens(query): GET {base}/tokens/v2/search?query=<encodeURIComponent>.
bool JupiterSearch(providers::TokenFetch* fetch, const char* query);

// The parsed 2xx JSON body (Response.json()), or kFailed.
Outcome Json(codec::tj::Arena& arena, const providers::TokenFetch& fetch, codec::tj::Value** body);

// coinMarkets(ids): every array row; a non-array body is []. A null row
// fails the batch (`row.id` on null throws in the TypeScript batcher).
Outcome MarketRows(codec::tj::Arena& arena, const providers::TokenFetch& fetch, codec::tj::Value** rows);
// coinMarket(coinId): rows.get(coinId) ?? null over a Map built from the
// string-id rows, so the LAST row carrying the id wins.
codec::tj::Value* MarketRowFor(codec::tj::Arena& arena, const codec::tj::Value* rows, const char* coin_id);
// coinProfile(id): the raw body (any JSON value).
Outcome Profile(codec::tj::Arena& arena, const providers::TokenFetch& fetch, codec::tj::Value** body);
// contractInfo(platform, address): a 404 is JSON null.
Outcome ContractInfoBody(codec::tj::Arena& arena, const providers::TokenFetch& fetch, codec::tj::Value** info);
// search(query): [{id, symbol, name}] of the string-typed rows.
Outcome SearchHits(codec::tj::Arena& arena, const providers::TokenFetch& fetch, codec::tj::Value** hits);
// pools(address, slug).
Outcome Pools(codec::tj::Arena& arena, const providers::TokenFetch& fetch, const char* address, const char* slug,
              codec::tj::Value** pools);
// tokenSecurity(chain, address) then normalizeContractSecurity: null for an
// absent row or a documented coverage code.
Outcome Contract(codec::tj::Arena& arena, const providers::TokenFetch& fetch, const char* chain, const char* address,
                 int64_t now_ms, codec::tj::Value** contract);
// holders(address): *has false for an answer without a numeric holdersCount.
Outcome EthplorerCount(codec::tj::Arena& arena, const providers::TokenFetch& fetch, bool* has, double* count);
// holders(chainId, address). *unsupported reports the "free api access is
// not supported for this chain" NOTOK answer (remembered per chain).
Outcome EtherscanCount(codec::tj::Arena& arena, const providers::TokenFetch& fetch, bool* has, double* count,
                       bool* unsupported);
// contractFacts: *facts is the {supply, decimals, symbol, name} object or
// JSON null (no usable aggregate3 result).
Outcome EvmFacts(codec::tj::Arena& arena, const providers::TokenFetch& fetch, codec::tj::Value** facts);

// Token360Facet.chainMeta's row selection over searchTokens(mint):
// `rows.find((r) => typeof r.id === 'string' && r.id === mint)`. A non-array
// body is [] (no row, *row nullptr). A null element reached before the match
// fails like the TypeScript TypeError (`r.id` on null).
Outcome JupiterExactRow(codec::tj::Arena& arena, const providers::TokenFetch& fetch, const char* mint, const codec::tj::Value** row);

// encodeURIComponent into `out` (NUL-terminated); false when it does not fit.
bool EncodeComponent(const char* text, char* out, size_t capacity);

}  // namespace fortis::tokens::token_fetch
