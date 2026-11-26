#include "token_fetchers.h"

#include "token_contract.h"
#include "token_evm.h"
#include "token_liquidity.h"
#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_fetch {
namespace {

using tj::Arena;
using tj::Value;

bool unreserved(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

bool set_path(providers::TokenFetch* fetch, providers::TokenUpstream upstream, const char* path) {
  const size_t length = strlen(path);
  if (!length || length >= sizeof(fetch->path)) return false;
  fetch->upstream = upstream;
  memcpy(fetch->path, path, length + 1);
  return true;
}

// URLSearchParams serialization (application/x-www-form-urlencoded).
void append_form(char* out, size_t capacity, size_t* used, const char* text, bool* ok) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p && *ok; ++p) {
    const unsigned char c = *p;
    char chunk[4];
    size_t n = 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' ||
        c == '-' || c == '.' || c == '_') {
      chunk[n++] = static_cast<char>(c);
    } else if (c == ' ') {
      chunk[n++] = '+';
    } else {
      chunk[n++] = '%'; chunk[n++] = kHex[c >> 4]; chunk[n++] = kHex[c & 15];
    }
    if (*used + n >= capacity) { *ok = false; return; }
    memcpy(out + *used, chunk, n);
    *used += n;
  }
}

bool ok_answer(const providers::TokenFetch& fetch) { return TokenFetchOk(fetch); }

}  // namespace

bool EncodeComponent(const char* text, char* out, size_t capacity) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  size_t used = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    if (unreserved(*p)) {
      if (used + 1 >= capacity) return false;
      out[used++] = static_cast<char>(*p);
    } else {
      if (used + 3 >= capacity) return false;
      out[used++] = '%'; out[used++] = kHex[*p >> 4]; out[used++] = kHex[*p & 15];
    }
  }
  if (used >= capacity) return false;
  out[used] = '\0';
  return true;
}

bool CoinMarkets(providers::TokenFetch* fetch, const char* const* coin_ids, size_t count) {
  if (!count) return false;
  char joined[2048];
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t n = strlen(coin_ids[i]);
    if (used + n + 2 >= sizeof(joined)) return false;
    if (i) joined[used++] = ',';
    memcpy(joined + used, coin_ids[i], n);
    used += n;
  }
  joined[used] = '\0';
  char encoded[3 * sizeof(joined)];
  if (!EncodeComponent(joined, encoded, sizeof(encoded))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path),
                         "/api/v3/coins/markets?vs_currency=usd&ids=%s"
                         "&price_change_percentage=1h,24h,7d,14d,30d,200d,1y&sparkline=true",
                         encoded);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kCoinGecko, path);
}

bool CoinProfile(providers::TokenFetch* fetch, const char* coin_id) {
  char encoded[1024];
  if (!EncodeComponent(coin_id, encoded, sizeof(encoded))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path),
                         "/api/v3/coins/%s?localization=false&tickers=true&market_data=false"
                         "&community_data=true&developer_data=false",
                         encoded);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kCoinGecko, path);
}

bool ContractInfo(providers::TokenFetch* fetch, const char* platform, const char* address) {
  char p[256], a[1536];
  if (!EncodeComponent(platform, p, sizeof(p)) || !EncodeComponent(address, a, sizeof(a))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path), "/api/v3/coins/%s/contract/%s", p, a);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kCoinGecko, path);
}

bool Search(providers::TokenFetch* fetch, const char* query) {
  char q[1536];
  if (!EncodeComponent(query, q, sizeof(q))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path), "/api/v3/search?query=%s", q);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kCoinGecko, path);
}

bool DexPools(providers::TokenFetch* fetch, const char* address) {
  char a[1536];
  if (!EncodeComponent(address, a, sizeof(a))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path), "/latest/dex/tokens/%s", a);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kDexScreener, path);
}

bool GoPlus(providers::TokenFetch* fetch, const char* chain, const char* address) {
  char path[providers::kTokenMaxPathBytes];
  return token_contract::GoPlusPath(chain, address, path, sizeof(path)) &&
         set_path(fetch, providers::TokenUpstream::kGoPlus, path);
}

bool EthplorerHolders(providers::TokenFetch* fetch, const char* address, const char* key) {
  // address.toLowerCase(), then encodeURIComponent; the key is interpolated
  // raw (a configured key is printable ASCII without spaces).
  char lowered[1024]{};
  const size_t length = strlen(address);
  if (length >= sizeof(lowered)) return false;
  for (size_t i = 0; i <= length; ++i) {
    const char c = address[i];
    lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  char a[3 * sizeof(lowered)];
  if (!EncodeComponent(lowered, a, sizeof(a))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path), "/getTokenInfo/%s?apiKey=%s", a, key ? key : "freekey");
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kEthplorer, path);
}

bool EtherscanHolders(providers::TokenFetch* fetch, uint32_t chain_id, const char* address, const char* key) {
  if (!key) return false;
  char lowered[1024]{};
  const size_t length = strlen(address);
  if (length >= sizeof(lowered)) return false;
  for (size_t i = 0; i <= length; ++i) {
    const char c = address[i];
    lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  char path[providers::kTokenMaxPathBytes];
  size_t used = 0;
  bool ok = true;
  const int head = snprintf(path, sizeof(path),
                            "/v2/api?chainid=%u&module=token&action=tokenholdercount&contractaddress=",
                            chain_id);
  if (head <= 0 || static_cast<size_t>(head) >= sizeof(path)) return false;
  used = static_cast<size_t>(head);
  append_form(path, sizeof(path), &used, lowered, &ok);
  const char* apikey = "&apikey=";
  if (!ok || used + strlen(apikey) >= sizeof(path)) return false;
  memcpy(path + used, apikey, strlen(apikey));
  used += strlen(apikey);
  append_form(path, sizeof(path), &used, key, &ok);
  if (!ok || used >= sizeof(path)) return false;
  path[used] = '\0';
  return set_path(fetch, providers::TokenUpstream::kEtherscan, path);
}

bool EvmContractFacts(providers::TokenFetch* fetch, const char* const* rpc_urls, size_t url_count,
                      const char* address) {
  if (!url_count) return false;
  size_t length = 0;
  if (!token_evm::ContractFactsRequest(address, fetch->rpc_body, sizeof(fetch->rpc_body), &length))
    return false;
  fetch->upstream = providers::TokenUpstream::kRpc;
  fetch->rpc_urls = rpc_urls;
  fetch->rpc_url_count = url_count < providers::kTokenMaxRpcUrls ? url_count : providers::kTokenMaxRpcUrls;
  fetch->rpc_body_length = length;
  fetch->path[0] = '\0';
  return true;
}

bool JupiterSearch(providers::TokenFetch* fetch, const char* query) {
  char q[1536];
  if (!EncodeComponent(query, q, sizeof(q))) return false;
  char path[providers::kTokenMaxPathBytes];
  const int n = snprintf(path, sizeof(path), "/tokens/v2/search?query=%s", q);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && set_path(fetch, providers::TokenUpstream::kJupiter, path);
}

Outcome Json(Arena& arena, const providers::TokenFetch& fetch, Value** body) {
  *body = nullptr;
  if (!ok_answer(fetch) || !fetch.body || !fetch.body_length) return Outcome::kFailed;
  Value* parsed = tj::Parse(arena, fetch.body.get(), fetch.body_length);
  if (!parsed) return Outcome::kFailed;
  *body = parsed;
  return Outcome::kValue;
}

Outcome MarketRows(Arena& arena, const providers::TokenFetch& fetch, Value** rows) {
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  if (!tj::IsArray(body)) {
    *rows = tj::Arr(arena);
    return *rows ? Outcome::kValue : Outcome::kFailed;
  }
  for (uint32_t i = 0; i < body->count; ++i)
    if (!body->items[i] || body->items[i]->type == tj::Type::kNull) return Outcome::kFailed;
  *rows = body;
  return Outcome::kValue;
}

Value* MarketRowFor(Arena& arena, const Value* rows, const char* coin_id) {
  const Value* found = nullptr;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i) {
    const Value* id = tj::Get(rows->items[i], "id");
    if (tj::IsString(id) && id->length == strlen(coin_id) && memcmp(id->text, coin_id, id->length) == 0)
      found = rows->items[i];
  }
  return found ? const_cast<Value*>(found) : tj::Null(arena);
}

Outcome Profile(Arena& arena, const providers::TokenFetch& fetch, Value** body) {
  return Json(arena, fetch, body);
}

Outcome ContractInfoBody(Arena& arena, const providers::TokenFetch& fetch, Value** info) {
  if (fetch.answered && fetch.status == 404) {
    *info = tj::Null(arena);
    return *info ? Outcome::kValue : Outcome::kFailed;
  }
  return Json(arena, fetch, info);
}

Outcome SearchHits(Arena& arena, const providers::TokenFetch& fetch, Value** hits) {
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  // `body.coins ?? []` on a null body throws.
  if (!body || body->type == tj::Type::kNull) return Outcome::kFailed;
  Value* out = tj::Arr(arena);
  if (!out) return Outcome::kFailed;
  const Value* coins = tj::Get(body, "coins");
  if (!coins || coins->type == tj::Type::kNull) { *hits = out; return Outcome::kValue; }
  // (body.coins ?? []).flatMap: a non-array `coins` throws (no flatMap).
  if (!tj::IsArray(coins)) {
    // A string has no flatMap either; every non-array value throws.
    return Outcome::kFailed;
  }
  for (uint32_t i = 0; i < coins->count; ++i) {
    const Value* row = coins->items[i];
    if (!row || row->type == tj::Type::kNull) return Outcome::kFailed;  // row.id on null
    const Value* id = tj::Get(row, "id");
    const Value* symbol = tj::Get(row, "symbol");
    const Value* name = tj::Get(row, "name");
    if (!tj::IsString(id) || !tj::IsString(symbol) || !tj::IsString(name)) continue;
    Value* hit = tj::Obj(arena);
    if (!hit || !tj::Set(arena, hit, "id", tj::Clone(arena, id)) ||
        !tj::Set(arena, hit, "symbol", tj::Clone(arena, symbol)) ||
        !tj::Set(arena, hit, "name", tj::Clone(arena, name)) || !tj::Push(arena, out, hit))
      return Outcome::kFailed;
  }
  *hits = out;
  return Outcome::kValue;
}

Outcome Pools(Arena& arena, const providers::TokenFetch& fetch, const char* address, const char* slug,
              Value** pools) {
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  return token_liquidity::DexScreenerPools(arena, body, address, slug, pools) ==
                 token_liquidity::PoolsStatus::kOk
             ? Outcome::kValue
             : Outcome::kFailed;
}

Outcome Contract(Arena& arena, const providers::TokenFetch& fetch, const char* chain, const char* address,
                 int64_t now_ms, Value** contract) {
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  const Value* token = nullptr;
  switch (token_contract::ClassifyGoPlusBody(body, address, &token)) {
    case token_contract::GoPlusStatus::kFailed: return Outcome::kFailed;
    case token_contract::GoPlusStatus::kAbsent:
      *contract = tj::Null(arena);
      return *contract ? Outcome::kValue : Outcome::kFailed;
    case token_contract::GoPlusStatus::kRow: break;
  }
  *contract = token_contract::NormalizeContractSecurity(arena, token, chain, address, now_ms);
  return *contract ? Outcome::kValue : Outcome::kFailed;
}

Outcome EthplorerCount(Arena& arena, const providers::TokenFetch& fetch, bool* has, double* count) {
  *has = false;
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  if (!body || body->type == tj::Type::kNull) return Outcome::kFailed;  // body.holdersCount on null
  const Value* holders = tj::Get(body, "holdersCount");
  if (tj::IsNumber(holders)) { *has = true; *count = holders->number; }
  return Outcome::kValue;
}

Outcome EtherscanCount(Arena& arena, const providers::TokenFetch& fetch, bool* has, double* count,
                       bool* unsupported) {
  *has = false;
  *unsupported = false;
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  if (!body || body->type == tj::Type::kNull) return Outcome::kFailed;
  const Value* status = tj::Get(body, "status");
  const Value* result = tj::Get(body, "result");
  if (!tj::StringEquals(status, "1")) {
    // body.result?.trim(): a non-string, non-nullish result throws.
    if (result && result->type != tj::Type::kNull && !tj::IsString(result)) return Outcome::kFailed;
    if (tj::IsString(result)) {
      static const char kNeedle[] = "free api access is not supported for this chain";
      const size_t needle = sizeof(kNeedle) - 1;
      for (size_t i = 0; i + needle <= result->length; ++i) {
        bool same = true;
        for (size_t k = 0; k < needle && same; ++k) {
          char c = result->text[i + k];
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
          same = c == kNeedle[k];
        }
        if (same) { *unsupported = true; return Outcome::kValue; }
      }
    }
    return Outcome::kFailed;
  }
  // Number(result) for undefined is NaN; a JSON null is 0 (Number(null)).
  double value = NAN;
  if (!result) value = NAN;
  else if (result->type == tj::Type::kNull) value = 0;
  else if (tj::IsString(result)) value = compat::token_text::ToNumber(result->text, result->length);
  else if (tj::IsNumber(result)) value = result->number;
  else if (result->type == tj::Type::kTrue) value = 1;
  else if (result->type == tj::Type::kFalse) value = 0;
  else if (tj::IsArray(result) && result->count == 0) value = 0;
  else if (tj::IsArray(result) && result->count == 1 && tj::IsString(result->items[0]))
    value = compat::token_text::ToNumber(result->items[0]->text, result->items[0]->length);
  else if (tj::IsArray(result) && result->count == 1 && tj::IsNumber(result->items[0]))
    value = result->items[0]->number;
  if (isfinite(value) && value >= 0 && value <= 9007199254740991.0 && floor(value) == value) {
    *has = true;
    *count = value;
  }
  return Outcome::kValue;
}

Outcome JupiterExactRow(Arena& arena, const providers::TokenFetch& fetch, const char* mint, const Value** row) {
  *row = nullptr;
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  if (!tj::IsArray(body)) return Outcome::kValue;  // Array.isArray(body) ? body : []
  const size_t mint_length = strlen(mint);
  for (uint32_t i = 0; i < body->count; ++i) {
    const Value* r = body->items[i];
    if (!r || r->type == tj::Type::kNull) return Outcome::kFailed;
    // Primitives and arrays have no own `id`; only an object row can match.
    const Value* id = tj::IsObject(r) ? tj::Get(r, "id") : nullptr;
    if (tj::IsString(id) && id->length == mint_length && memcmp(id->text, mint, mint_length) == 0) {
      *row = r;
      return Outcome::kValue;
    }
  }
  return Outcome::kValue;
}

Outcome EvmFacts(Arena& arena, const providers::TokenFetch& fetch, Value** facts) {
  Value* body = nullptr;
  if (Json(arena, fetch, &body) != Outcome::kValue) return Outcome::kFailed;
  switch (token_evm::ContractFacts(arena, body, facts)) {
    case token_evm::FactsStatus::kFailed: return Outcome::kFailed;
    case token_evm::FactsStatus::kNone:
      *facts = tj::Null(arena);
      return *facts ? Outcome::kValue : Outcome::kFailed;
    case token_evm::FactsStatus::kFacts: return Outcome::kValue;
  }
  return Outcome::kFailed;
}

}  // namespace fortis::tokens::token_fetch
