#include <algorithm>
#include "ownership.h"
#include "token_robinhood.h"

#include "media_registry.h"
#include "token_provider.h"
#include "token_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_robinhood {
namespace {

constexpr int kParseAttempts = 3;
constexpr const char* kDocsLink = "https://docs.robinhood.com/chain/stock-tokens";
constexpr const char* kDescriptionTail =
    " is an issuer-registered tokenized debt security providing economic exposure to its underlying reference "
    "asset. It is not direct share ownership and carries no shareholder voting rights.";

// a.toLowerCase() === b for a lower-case ASCII `b`.
bool same_address(const Value* a, const char* b) {
  if (!tj::IsString(a) || a->length != strlen(b)) return false;
  for (size_t i = 0; i < a->length; ++i) {
    char c = a->text[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
    if (c != b[i]) return false;
  }
  return true;
}

// media.register([logoUrl])[0]: the key of an http(s) URL, else null.
Value* logo_key(Arena& arena, const Value* asset, storage::KvStore* kv, int64_t now_ms) {
  const Value* logo = tj::Get(asset, "logoUrl");
  char key[fortis::discovery::market_view::kMediaKeyChars + 1];
  if (tj::IsString(logo) && discovery::RegisterMediaSource(kv, logo->text, logo->length, now_ms, key)) return tj::Str(arena, key);
  return tj::Null(arena);
}

bool put(Arena& arena, Value* object, const char* key, Value* value) {
  return value && tj::Set(arena, object, key, value);
}

}  // namespace

bool IssuerRef(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  return ref.has_address && strcmp(network.id, "robinhood") == 0;
}

exchange::robinhood::Outcome OwnerFetcher::Fetch(Arena& arena, uint64_t deadline_ms, const char* path, bool background,
                                       Value** body) {
  providers::TokenProviderOwner* owner = owner_;
  exchange::robinhood::Outcome outcome;
  outcome.failure = exchange::robinhood::Failure::kError;
  *body = nullptr;
  const size_t length = path ? strlen(path) : 0;
  if (!owner || !length || length >= providers::kTokenMaxPathBytes) return outcome;
  auto destroy = [](providers::TokenFetch* fetch) noexcept {
    TokenFetchRelease(fetch);
    base::Delete(fetch);
  };
  std::unique_ptr<providers::TokenFetch, decltype(destroy)> fetch(base::New<providers::TokenFetch>(), destroy);
  if (!fetch) return outcome;
  for (int attempt = 0; attempt < kParseAttempts; ++attempt) {
    *fetch = providers::TokenFetch{};
    fetch->upstream = providers::TokenUpstream::kRobinhood;
    memcpy(fetch->path, path, length + 1);
    fetch->background = background;
    owner->FetchAll(fetch.get(), 1, deadline_ms);
    if (fetch->lane_rejected) {
      outcome.failure = exchange::robinhood::Failure::kLaneBusy;
      outcome.retry_after_ms = fetch->retry_after_ms;
      TokenFetchRelease(fetch.get());
      break;
    }
    if (!TokenFetchOk(*fetch)) {
      TokenFetchRelease(fetch.get());
      break;
    }
    Value* parsed = fetch->body ? tj::Parse(arena, fetch->body.get(), fetch->body_length) : nullptr;
    TokenFetchRelease(fetch.get());
    if (parsed) {
      *body = parsed;
      outcome.failure = exchange::robinhood::Failure::kNone;
      break;
    }
    // An unparseable 2xx: HttpClient's parse failure is retried like a
    // transport failure.
  }
  fetch.reset();
  return outcome;
}

Listing Asset(token_facts::Work& work, exchange::robinhood::StockClient* client, Memo* memo, const networks::TokenRef& ref,
              bool* lane_busy) {
  if (!memo->catalog_read) {
    memo->catalog_read = true;
    std::shared_ptr<const exchange::robinhood::StockClient::Snapshot> held = nullptr;
    memo->catalog = client->Assets(work.deadline_ms, &held, work.background);
    if (memo->catalog.ok()) {
      const Value* assets = exchange::robinhood::StockClient::Document(held.get());
      for (uint32_t i = 0; tj::IsArray(assets) && i < assets->count; ++i) {
        if (!same_address(tj::Get(assets->items[i], "contractAddress"), ref.address)) continue;
        // Copied: the snapshot is released (and may be replaced) below.
        memo->asset = tj::Clone(work.arena, assets->items[i]);
        if (!memo->asset) memo->catalog.failure = exchange::robinhood::Failure::kError;
        break;
      }
    }
    held.reset();
  }
  if (!memo->catalog.ok()) {
    if (lane_busy) *lane_busy = memo->catalog.failure == exchange::robinhood::Failure::kLaneBusy;
    return Listing::kFailed;
  }
  return memo->asset ? Listing::kListed : Listing::kUnlisted;
}

bool Quote(token_facts::Work& work, exchange::robinhood::StockClient* client, Memo* memo, Value** quote, bool* lane_busy) {
  if (!memo->quote_read) {
    memo->quote_read = true;
    memo->quote_outcome = client->ReferenceQuote(work.arena, work.deadline_ms, memo->asset, &memo->quote,
                                                 work.background);
  }
  if (!memo->quote_outcome.ok()) {
    if (lane_busy) *lane_busy = memo->quote_outcome.failure == exchange::robinhood::Failure::kLaneBusy;
    return false;
  }
  *quote = memo->quote;
  return true;
}

Value* ChainMeta(Arena& arena, const Value* asset, storage::KvStore* kv, int64_t now_ms) {
  const Value* symbol = tj::Get(asset, "tokenSymbol");
  const Value* name = tj::Get(asset, "tokenName");
  if (!tj::IsString(symbol) || !tj::IsString(name)) return nullptr;
  Value* out = tj::Obj(arena);
  if (!out || !put(arena, out, "symbol", compat::token_text::SanitizeSymbol(arena, symbol->text, symbol->length)) ||
      !put(arena, out, "name", compat::token_text::SanitizeName(arena, name->text, name->length)) ||
      !put(arena, out, "decimals", tj::Num(arena, 18)) || !put(arena, out, "verified", tj::Bool(arena, true)) ||
      !put(arena, out, "holderCount", tj::Null(arena)) ||
      !put(arena, out, "logoMediaKey", logo_key(arena, asset, kv, now_ms)))
    return nullptr;
  return out;
}

Value* MarketRow(Arena& arena, const Value* asset, const Value* quote) {
  const Value* midpoint = tj::Get(quote, "midpointUsd");
  if (!tj::IsNumber(midpoint)) return nullptr;
  Value* out = tj::Obj(arena);
  if (!out || !put(arena, out, "symbol", tj::Get(asset, "tokenSymbol")) ||
      !put(arena, out, "name", tj::Get(asset, "tokenName")) ||
      !put(arena, out, "current_price", tj::Num(arena, midpoint->number)) ||
      !put(arena, out, "total_volume", tj::Null(arena)) || !put(arena, out, "high_24h", tj::Null(arena)) ||
      !put(arena, out, "low_24h", tj::Null(arena)))
    return nullptr;
  return out;
}

Value* Profile(Arena& arena, const Value* asset, storage::KvStore* kv, int64_t now_ms) {
  const Value* name = tj::Get(asset, "tokenName");
  if (!tj::IsString(name)) return nullptr;
  const size_t tail = strlen(kDescriptionTail);
  char* text = static_cast<char*>(arena.Alloc(name->length + tail, 1));
  if (!text) return nullptr;
  memcpy(text, name->text, name->length);
  std::copy_n(kDescriptionTail, tail, text + name->length);
  Value* categories = tj::Arr(arena);
  Value* links = tj::Obj(arena);
  Value* out = tj::Obj(arena);
  if (!categories || !links || !out || !tj::Push(arena, categories, tj::Str(arena, "Tokenized security")) ||
      !tj::Push(arena, categories, tj::Str(arena, "Stock Token")) ||
      !put(arena, links, "homepage", tj::Str(arena, kDocsLink)) ||
      !put(arena, out, "description", tj::Str(arena, text, name->length + tail)) ||
      !put(arena, out, "categories", categories) || !put(arena, out, "links", links) ||
      !put(arena, out, "logoMediaKey", logo_key(arena, asset, kv, now_ms)) ||
      !put(arena, out, "marketsCount", tj::Null(arena)) || !put(arena, out, "sentimentUpPct", tj::Null(arena)))
    return nullptr;
  return out;
}

}  // namespace fortis::tokens::token_robinhood
