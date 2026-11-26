#include "token_identity_resolver.h"

#include "price_id_parser.h"
#include "token_comparison_store.h"
#include "token_liquidity.h"
#include "provider_text.h"

#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;
namespace fortis::tokens::token_facts {
namespace {
Value* resolution_json(Arena& a, const char* coin_id, ResolutionSource source, bool ambiguous) {
  Value* o = tj::Obj(a);
  const char* name = ResolutionSourceName(source);
  if (!o || !tj::Set(a, o, "coinId", coin_id ? tj::Str(a, coin_id) : tj::Null(a)) ||
      !tj::Set(a, o, "source", name ? tj::Str(a, name) : tj::Null(a)) ||
      !tj::Set(a, o, "ambiguous", tj::Bool(a, ambiguous)))
    return nullptr;
  return o;
}

void apply_resolution(const Value* value, Resolution* resolution) {
  *resolution = Resolution{};
  const Value* coin = tj::Get(value, "coinId");
  const Value* source = tj::Get(value, "source");
  if (tj::IsString(coin) && coin->length < kCoinIdBytes) {
    resolution->has_coin = true;
    memcpy(resolution->coin_id, coin->text, coin->length);
    resolution->coin_id[coin->length] = '\0';
  }
  if (tj::StringEquals(source, "registry")) resolution->source = ResolutionSource::kRegistry;
  else if (tj::StringEquals(source, "contract")) resolution->source = ResolutionSource::kContract;
  else if (tj::StringEquals(source, "search")) resolution->source = ResolutionSource::kSearch;
  else if (tj::StringEquals(source, "ref")) resolution->source = ResolutionSource::kRef;
  else if (tj::StringEquals(source, "hint")) resolution->source = ResolutionSource::kHint;
  resolution->ambiguous = tj::Get(value, "ambiguous") && tj::Get(value, "ambiguous")->type == tj::Type::kTrue;
}

bool hint_valid(const char* text, size_t length) {
  // /^[a-z0-9][a-z0-9-]{0,99}$/ on the trimmed, lower-cased hint.
  if (!length || length > 100) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && !(i > 0 && c == '-')) return false;
  }
  return true;
}

}  // namespace

bool TokenIdentityResolver::ResolveLive(const networks::TokenNetwork& network, const networks::TokenRef& ref, Value** out) {
  Arena& a = arena_;
  const int64_t now = cache_.Now();
  // 1. The registry already resolved one.
  char stored[kCoinIdBytes] = {};
  registry_.FindCoin(a, network, ref, now, stored);
  if (stored[0]) {
    *out = resolution_json(a, stored, ResolutionSource::kRegistry, false);
    return *out != nullptr;
  }
  if (!provider_.BeginLookup()) return false;
  // 2. A ticker: exact symbol matches of /search.
  if (ref.kind == networks::TokenRefKind::kSymbol) {
    Value* hits = nullptr;
    if (!provider_.Search(a, ref.symbol, &hits)) return false;
    const Value* first = nullptr;
    size_t exact = 0;
    for (uint32_t i = 0; i < hits->count; ++i) {
      const Value* symbol = tj::Get(hits->items[i], "symbol");
      Value* upper = codec::provider_text::ToUpperCase(a, symbol->text, symbol->length);
      if (!upper) return false;
      if (upper->length != strlen(ref.symbol) || memcmp(upper->text, ref.symbol, upper->length) != 0) continue;
      if (!first) first = hits->items[i];
      ++exact;
    }
    if (!first) {
      *out = resolution_json(a, nullptr, ResolutionSource::kNone, false);
    } else {
      const char* id = tj::CString(a, tj::Get(first, "id"));
      *out = id ? resolution_json(a, id, ResolutionSource::kSearch, exact > 1) : nullptr;
    }
    return *out != nullptr;
  }
  // 3. The aggregator resolves the contract.
  const char* platform = ref.has_address ? providers::CoinGeckoPlatformForNetwork(network.id) : nullptr;
  if (!platform) {
    *out = resolution_json(a, nullptr, ResolutionSource::kNone, false);
    return *out != nullptr;
  }
  Value* info = nullptr;
  if (!provider_.Contract(a, platform, ref.address, &info)) return false;
  const Value* id = tj::Get(info, "id");
  if (!tj::IsString(id) || id->length == 0 || id->length >= kCoinIdBytes) {
    *out = resolution_json(a, nullptr, ResolutionSource::kNone, false);
    return *out != nullptr;
  }
  const char* coin = tj::CString(a, id);
  // tokens.patch('registry', …): sanitized symbol and name, the coin id.
  // A non-string truthy symbol/name throws inside the try and skips the write.
  const Value* symbol = tj::Get(info, "symbol");
  const Value* name = tj::Get(info, "name");
  const bool symbol_ok = !token_liquidity::Truthy(symbol) || tj::IsString(symbol);
  const bool name_ok = !token_liquidity::Truthy(name) || tj::IsString(name);
  if (symbol_ok && name_ok) {
    token_store::IdentityPatch patch{};
    using token_store::FieldState;
    Value* sym = token_liquidity::Truthy(symbol) ? codec::provider_text::SanitizeSymbol(a, symbol->text, symbol->length) : nullptr;
    Value* nm = token_liquidity::Truthy(name) ? codec::provider_text::SanitizeName(a, name->text, name->length) : nullptr;
    patch.symbol = sym ? token_store::StringPatch{FieldState::kValue, sym->text, sym->length}
                       : token_store::StringPatch{FieldState::kNull, nullptr, 0};
    patch.name = nm ? token_store::StringPatch{FieldState::kValue, nm->text, nm->length}
                    : token_store::StringPatch{FieldState::kNull, nullptr, 0};
    patch.coingecko_id = {FieldState::kValue, coin, id->length};
    registry_.Patch(network, ref, patch, now);
  }
  *out = resolution_json(a, coin, ResolutionSource::kContract, false);
  return *out != nullptr;
}

void TokenIdentityResolver::Resolve(const networks::TokenNetwork& network, const networks::TokenRef& ref,
                                    const char* hint, Resolution* resolution) {
  *resolution = Resolution{};
  if (ref.has_coingecko_id) {
    resolution->has_coin = true;
    snprintf(resolution->coin_id, sizeof(resolution->coin_id), "%s", ref.coingecko_id);
    resolution->source = ResolutionSource::kRef;
    return;
  }
  Arena& a = arena_;
  char key[1100];
  const int n = snprintf(key, sizeof(key), "tref:%s:%s", network.id, ref.token_id);
  if (n > 0 && static_cast<size_t>(n) < sizeof(key)) {
    const int64_t now = cache_.Now();
    Value* cached = nullptr;
    const token_swr::State state =
        cache_.Read(key, now, a, &cached);
    if (state != token_swr::State::kMissing) {
      apply_resolution(cached, resolution);
      if (state == token_swr::State::kStale) cache_.RequestRefresh(key, network, ref, now);
    } else if (!cache_.MissHeld(key, now)) {
      Value* live = nullptr;
      if (ResolveLive(network, ref, &live)) {
        cache_.Store(key, cache_.Now(), live);
        apply_resolution(live, resolution);
      } else {
        cache_.HoldMiss(key, cache_.Now());
      }
    }
  }
  if (resolution->has_coin) return;
  // hint?.trim().toLowerCase(), then /^[a-z0-9][a-z0-9-]{0,99}$/.
  if (!hint) return;
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(hint, strlen(hint), &begin, &end);
  Value* lowered = codec::provider_text::ToLowerCase(a, hint + begin, end - begin);
  if (!lowered || !hint_valid(lowered->text, lowered->length)) return;
  resolution->has_coin = true;
  memcpy(resolution->coin_id, lowered->text, lowered->length);
  resolution->coin_id[lowered->length] = '\0';
  resolution->source = ResolutionSource::kHint;
  resolution->ambiguous = false;
}

}  // namespace fortis::tokens::token_facts
