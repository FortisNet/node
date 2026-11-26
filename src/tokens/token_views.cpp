#include "fixed_string.h"
#include <initializer_list>
#include <string_view>
#include <algorithm>
#include "token_views.h"

#include "market_view.h"
#include "media_registry.h"
#include "price_history_batch_codec.h"
#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_views {
namespace {

using tj::Arena;
using tj::Value;

constexpr size_t kSparklineCap = 400;

Value* num(Arena& a, const Value* v) {
  double n = 0;
  return FiniteNumber(v, &n) ? tj::Num(a, n) : tj::Null(a);
}
Value* int_(Arena& a, const Value* v) {
  double n = 0;
  return FiniteNumber(v, &n) ? tj::Num(a, trunc(n)) : tj::Null(a);
}
// str(): a non-empty string.
const Value* str_value(const Value* v) { return tj::IsString(v) && v->length > 0 ? v : nullptr; }
Value* str(Arena& a, const Value* v) {
  const Value* s = str_value(v);
  return s ? tj::Str(a, s->text, s->length) : tj::Null(a);
}
Value* iso(Arena& a, const Value* v) {
  const Value* s = str_value(v);
  double ms = 0;
  if (!s || !codec::provider_text::DateParse(s->text, s->length, &ms)) return tj::Null(a);
  return tj::Str(a, s->text, s->length);
}

Value* sparkline(Arena& a, const Value* value) {
  const Value* raw = tj::IsObject(value) ? tj::Get(value, "price") : value;
  Value* out = tj::Arr(a);
  if (!out || !tj::IsArray(raw)) return out;
  size_t finite = 0;
  for (uint32_t i = 0; i < raw->count; ++i) if (tj::IsFinite(raw->items[i])) ++finite;
  const size_t skip = finite > kSparklineCap ? finite - kSparklineCap : 0;
  size_t seen = 0;
  for (uint32_t i = 0; i < raw->count; ++i) {
    if (!tj::IsFinite(raw->items[i])) continue;
    if (seen++ < skip) continue;
    if (!tj::Push(a, out, tj::Num(a, raw->items[i]->number))) return nullptr;
  }
  return out;
}

bool set(Arena& a, Value* o, const char* key, Value* v) { return v && tj::Set(a, o, key, v); }

// usd(), count() and ageLabel() of the Dart scorer.
void usd(double v, char* out, size_t capacity) {
  char fixed[64];
  if (v >= 1e9) { codec::provider_text::ToFixed(v / 1e9, 1, fixed, sizeof(fixed)); base::ConcatCString(out, capacity, {"$", fixed, "B"}); }
  else if (v >= 1e6) { codec::provider_text::ToFixed(v / 1e6, 1, fixed, sizeof(fixed)); base::ConcatCString(out, capacity, {"$", fixed, "M"}); }
  else if (v >= 1e3) { codec::provider_text::ToFixed(v / 1e3, 1, fixed, sizeof(fixed)); base::ConcatCString(out, capacity, {"$", fixed, "K"}); }
  else { codec::provider_text::ToFixed(v, 0, fixed, sizeof(fixed)); base::ConcatCString(out, capacity, {"$", fixed, ""}); }
}
void js_number(double v, char* out, size_t capacity) {
  uint8_t buffer[64];
  tj::OutBuffer o{buffer, sizeof(buffer) - 1, 0, true};
  codec::history_batch::AppendJsNumber(o, v);
  buffer[o.used] = 0;
  snprintf(out, capacity, "%s", reinterpret_cast<char*>(buffer));
}
void count(double n, char* out, size_t capacity) {
  char fixed[64];
  if (n >= 1e6) { codec::provider_text::ToFixed(n / 1e6, 1, fixed, sizeof(fixed)); base::ConcatCString(out, capacity, {fixed, "M"}); }
  else if (n >= 1e3) { codec::provider_text::ToFixed(n / 1e3, 1, fixed, sizeof(fixed)); base::ConcatCString(out, capacity, {fixed, "K"}); }
  else js_number(n, out, capacity);
}
// Math.round: ties toward +Infinity.
double js_round(double x) {
  const double f = floor(x);
  return x - f >= 0.5 ? f + 1 : f;
}
void age_label(double days, char* out, size_t capacity) {
  char text[64];
  if (days >= 365) { codec::provider_text::ToFixed(days / 365, 1, text, sizeof(text)); base::ConcatCString(out, capacity, {text, "y"}); }
  else if (days >= 30) { js_number(js_round(days / 30), text, sizeof(text)); base::ConcatCString(out, capacity, {text, "mo"}); }
  else { js_number(days, text, sizeof(text)); base::ConcatCString(out, capacity, {text, "d"}); }
}

bool signal(Arena& a, Value* signals, const char* label, const char* detail, const char* sentiment) {
  Value* row = tj::Obj(a);
  return row && set(a, row, "label", tj::Str(a, label)) && set(a, row, "detail", tj::Str(a, detail)) &&
         set(a, row, "sentiment", tj::Str(a, sentiment)) && tj::Push(a, signals, row);
}

}  // namespace

bool FiniteNumber(const Value* v, double* out) {
  if (!tj::IsFinite(v)) return false;
  *out = v->number;
  return true;
}

bool FiniteNumberOrString(const Value* v, double* out) {
  if (tj::IsString(v)) {
    const double n = codec::provider_text::ToNumber(v->text, v->length);
    if (!isfinite(n)) return false;
    *out = n;
    return true;
  }
  return FiniteNumber(v, out);
}

Value* ScoreTokenSafety(Arena& a, const SafetyInputs& in) {
  Value* out = tj::Obj(a);
  Value* signals = tj::Arr(a);
  if (!out || !signals) return nullptr;
  if (in.is_native) {
    if (!signal(a, signals, "Native coin", "The chain's base asset", "good") ||
        !set(a, out, "score", tj::Num(a, 95)) || !set(a, out, "level", tj::Str(a, "high")) ||
        !set(a, out, "signals", signals)) return nullptr;
    return out;
  }
  double score = 50;
  int observed = 0;
  char money[64], detail[128];
  bool ok = true;
  if (in.has_liquidity) {
    ++observed;
    const double liq = in.liquidity_usd;
    usd(liq, money, sizeof(money));
    if (liq >= 1000000) { score += 22; ok = ok && signal(a, signals, "Liquidity", money, "good"); }
    else if (liq >= 100000) { score += 12; ok = ok && signal(a, signals, "Liquidity", money, "good"); }
    else if (liq >= 10000) {
      score -= 6;
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 thin", money);
      ok = ok && signal(a, signals, "Liquidity", detail, "caution");
    } else {
      score -= 22;
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 very thin", money);
      ok = ok && signal(a, signals, "Liquidity", detail, "bad");
    }
  }
  if (in.has_holders) {
    ++observed;
    const double holders = in.holder_count;
    char text[64];
    if (holders >= 10000) { score += 14; count(holders, text, sizeof(text)); ok = ok && signal(a, signals, "Holders", text, "good"); }
    else if (holders >= 1000) { score += 8; count(holders, text, sizeof(text)); ok = ok && signal(a, signals, "Holders", text, "good"); }
    else if (holders >= 100) {
      score -= 4;
      js_number(holders, text, sizeof(text));
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 few", text);
      ok = ok && signal(a, signals, "Holders", detail, "caution");
    } else {
      score -= 16;
      js_number(holders, text, sizeof(text));
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 very few", text);
      ok = ok && signal(a, signals, "Holders", detail, "bad");
    }
  }
  if (in.has_volume) {
    ++observed;
    const double vol = in.volume_24h_usd;
    usd(vol, money, sizeof(money));
    if (vol >= 100000) { score += 8; ok = ok && signal(a, signals, "24h volume", money, "good"); }
    else if (vol >= 5000) {
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 low", money);
      ok = ok && signal(a, signals, "24h volume", detail, "caution");
    } else {
      score -= 10;
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 illiquid", money);
      ok = ok && signal(a, signals, "24h volume", detail, "bad");
    }
  }
  if (in.has_market_cap && in.market_cap_usd > 0) {
    ++observed;
    usd(in.market_cap_usd, money, sizeof(money));
    if (in.market_cap_usd >= 10000000) { score += 8; ok = ok && signal(a, signals, "Market cap", money, "good"); }
    else {
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 micro", money);
      ok = ok && signal(a, signals, "Market cap", detail, "caution");
    }
  }
  if (in.has_age) {
    ++observed;
    char label[64];
    age_label(in.trading_age_days, label, sizeof(label));
    if (in.trading_age_days >= 180) {
      score += 10;
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 established", label);
      ok = ok && signal(a, signals, "Trading age", detail, "good");
    } else if (in.trading_age_days <= 30) {
      score -= 12;
      snprintf(detail, sizeof(detail), "%s \xC2\xB7 new", label);
      ok = ok && signal(a, signals, "Trading age", detail, "bad");
    } else {
      ok = ok && signal(a, signals, "Trading age", label, "neutral");
    }
  }
  if (in.coingecko_listed) {
    score += 4;
    ok = ok && signal(a, signals, "Market data", "Listed on CoinGecko", "good");
  }
  const double clamped = js_round(fmin(100, fmax(0, score)));
  const char* level = observed < 2 ? "unknown" : clamped >= 70 ? "high" : clamped >= 45 ? "medium" : "low";
  if (signals->count == 0) ok = ok && signal(a, signals, "No data", "Not indexed by our sources", "neutral");
  if (!ok || !set(a, out, "score", tj::Num(a, clamped)) || !set(a, out, "level", tj::Str(a, level)) ||
      !set(a, out, "signals", signals)) return nullptr;
  return out;
}

Value* MarketView(Arena& a, const Value* row) {
  if (!tj::IsObject(row)) return tj::Null(a);
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  const Value* symbol = str_value(tj::Get(row, "symbol"));
  Value* upper = symbol ? codec::provider_text::ToUpperCase(a, symbol->text, symbol->length) : tj::Null(a);
  auto g = [&](const char* key) { return tj::Get(row, key); };
  const bool ok =
      set(a, out, "symbol", upper) && set(a, out, "name", str(a, g("name"))) &&
      set(a, out, "priceUsd", num(a, g("current_price"))) &&
      set(a, out, "change1hPct", num(a, g("price_change_percentage_1h_in_currency"))) &&
      set(a, out, "change24hPct", num(a, g("price_change_percentage_24h"))) &&
      set(a, out, "change7dPct", num(a, g("price_change_percentage_7d_in_currency"))) &&
      set(a, out, "change14dPct", num(a, g("price_change_percentage_14d_in_currency"))) &&
      set(a, out, "change30dPct", num(a, g("price_change_percentage_30d_in_currency"))) &&
      set(a, out, "change200dPct", num(a, g("price_change_percentage_200d_in_currency"))) &&
      set(a, out, "change1yPct", num(a, g("price_change_percentage_1y_in_currency"))) &&
      set(a, out, "marketCapUsd", num(a, g("market_cap"))) &&
      set(a, out, "marketCapRank", int_(a, g("market_cap_rank"))) &&
      set(a, out, "fdvUsd", num(a, g("fully_diluted_valuation"))) &&
      set(a, out, "volume24hUsd", num(a, g("total_volume"))) &&
      set(a, out, "high24hUsd", num(a, g("high_24h"))) && set(a, out, "low24hUsd", num(a, g("low_24h"))) &&
      set(a, out, "circulatingSupply", num(a, g("circulating_supply"))) &&
      set(a, out, "totalSupply", num(a, g("total_supply"))) && set(a, out, "maxSupply", num(a, g("max_supply"))) &&
      set(a, out, "athUsd", num(a, g("ath"))) && set(a, out, "athAt", iso(a, g("ath_date"))) &&
      set(a, out, "athChangePct", num(a, g("ath_change_percentage"))) && set(a, out, "atlUsd", num(a, g("atl"))) &&
      set(a, out, "atlAt", iso(a, g("atl_date"))) &&
      set(a, out, "atlChangePct", num(a, g("atl_change_percentage"))) &&
      set(a, out, "sparkline7d", sparkline(a, g("sparkline_in_7d")));
  return ok ? out : nullptr;
}

Value* ProfileView(Arena& a, const Value* row) {
  if (!tj::IsObject(row)) return tj::Null(a);
  const Value* links = tj::Get(row, "links");
  const Value* homepage = str_value(tj::Get(links, "homepage"));
  const Value* twitter = str_value(tj::Get(links, "twitter"));
  Value* twitter_url = nullptr;
  if (twitter) {
    if (twitter->length >= 4 && memcmp(twitter->text, "http", 4) == 0) {
      twitter_url = tj::Str(a, twitter->text, twitter->length);
    } else {
      char* text = static_cast<char*>(a.Alloc(twitter->length + 16, 1));
      if (!text) return nullptr;
      std::copy_n("https://x.com/", 14, text);
      memcpy(text + 14, twitter->text, twitter->length);
      twitter_url = tj::Str(a, text, 14 + twitter->length);
    }
  }
  Value* out = tj::Obj(a);
  Value* categories = tj::Arr(a);
  Value* link_list = tj::Arr(a);
  if (!out || !categories || !link_list) return nullptr;
  const Value* cats = tj::Get(row, "categories");
  if (tj::IsArray(cats))
    for (uint32_t i = 0; i < cats->count; ++i)
      if (tj::IsString(cats->items[i]) && !tj::Push(a, categories, tj::Clone(a, cats->items[i]))) return nullptr;
  struct Link { const char* label; const Value* url; Value* built; };
  Link entries[] = {
      {"Website", homepage, nullptr},
      {"Twitter", nullptr, twitter_url},
      {"GitHub", str_value(tj::Get(links, "github")), nullptr},
      {"Discord", str_value(tj::Get(links, "discord")), nullptr},
      {"Telegram", str_value(tj::Get(links, "telegram")), nullptr},
      {"Reddit", str_value(tj::Get(links, "reddit")), nullptr},
      {"Whitepaper", str_value(tj::Get(links, "whitepaper")), nullptr},
  };
  for (const Link& link : entries) {
    Value* url = link.built ? link.built : link.url ? tj::Str(a, link.url->text, link.url->length) : nullptr;
    if (!url) continue;
    Value* item = tj::Obj(a);
    if (!item || !set(a, item, "label", tj::Str(a, link.label)) || !set(a, item, "url", url) ||
        !tj::Push(a, link_list, item)) return nullptr;
  }
  const bool ok = set(a, out, "description", str(a, tj::Get(row, "description"))) &&
                  set(a, out, "categories", categories) && set(a, out, "links", link_list) &&
                  set(a, out, "marketsCount", int_(a, tj::Get(row, "marketsCount"))) &&
                  set(a, out, "sentimentUpPct", num(a, tj::Get(row, "sentimentUpPct"))) &&
                  set(a, out, "logoMediaKey", str(a, tj::Get(row, "logoMediaKey")));
  return ok ? out : nullptr;
}

bool TrimProfile(Arena& a, const Value* raw, storage::KvStore* kv, int64_t now_ms, Value** profile) {
  *profile = nullptr;
  const Value* description = tj::Get(tj::Get(raw, "description"), "en");
  const Value* links = tj::Get(raw, "links");
  const Value* repos = tj::Get(links, "repos_url");
  const Value* chat = tj::Get(links, "chat_url");
  const Value* sentiment = tj::Get(raw, "sentiment_votes_up_percentage");
  const Value* categories = tj::Get(raw, "categories");
  const Value* tickers = tj::Get(raw, "tickers");
  const Value* image = tj::Get(raw, "image");
  const Value* logo = nullptr;
  if (tj::IsObject(image))
    for (uint32_t i = 0; i < image->count && !logo; ++i) {
      const Value* v = image->members[i].value;
      if (tj::IsString(v) && v->length >= 8 && memcmp(v->text, "https://", 8) == 0) logo = v;
    }
  Value* logo_key = tj::Null(a);
  if (logo) {
    char key[discovery::market_view::kMediaKeyChars + 1];
    const char* url = tj::CString(a, logo);
    if (url && codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(url), logo->length) &&
        discovery::RegisterMediaSource(kv, url, logo->length, now_ms, key))
      logo_key = tj::Str(a, key);
  }
  Value* description_value = tj::Null(a);
  if (description && !(description->type == tj::Type::kNull || description->type == tj::Type::kFalse ||
                       (description->type == tj::Type::kString && description->length == 0) ||
                       (description->type == tj::Type::kNumber && (description->number == 0 || isnan(description->number))))) {
    if (!tj::IsString(description)) return false;  // sanitizeText(non-string) throws
    description_value = codec::provider_text::SanitizeText(a, description->text, description->length);
  }
  Value* category_list = tj::Arr(a);
  if (!category_list) return false;
  if (tj::IsArray(categories))
    for (uint32_t i = 0; i < categories->count; ++i) {
      const Value* c = categories->items[i];
      if (!tj::IsString(c)) continue;
      if (!tj::Push(a, category_list, codec::provider_text::SanitizeName(a, c->text, c->length))) return false;
    }
  auto first_string = [&](const Value* list, bool discord) -> Value* {
    if (!tj::IsArray(list)) return tj::Null(a);
    for (uint32_t i = 0; i < list->count; ++i) {
      const Value* v = list->items[i];
      if (!tj::IsString(v)) continue;
      if (discord) {
        Value* lowered = codec::provider_text::ToLowerCase(a, v->text, v->length);
        if (!lowered) return nullptr;
        bool has = false;
        for (uint32_t k = 0; k + 7 <= lowered->length && !has; ++k) has = memcmp(lowered->text + k, "discord", 7) == 0;
        if (!has) continue;
      }
      if (v->length == 0) continue;
      return tj::Str(a, v->text, v->length);
    }
    return tj::Null(a);
  };
  Value* link_object = tj::Obj(a);
  if (!link_object) return false;
  const Value* screen = tj::Get(links, "twitter_screen_name");
  const Value* telegram = tj::Get(links, "telegram_channel_identifier");
  Value* telegram_url = tj::Null(a);
  if (tj::IsString(telegram) && telegram->length > 0) {
    const size_t skip = telegram->text[0] == '@' ? 1 : 0;
    char* text = static_cast<char*>(a.Alloc(telegram->length + 16, 1));
    if (!text) return false;
    std::copy_n("https://t.me/", 13, text);
    memcpy(text + 13, telegram->text + skip, telegram->length - skip);
    telegram_url = tj::Str(a, text, 13 + telegram->length - skip);
  }
  const Value* subreddit = tj::Get(links, "subreddit_url");
  const Value* whitepaper = tj::IsString(tj::Get(raw, "whitepaper")) ? tj::Get(raw, "whitepaper")
                            : tj::IsString(tj::Get(links, "whitepaper")) ? tj::Get(links, "whitepaper") : nullptr;
  bool ok = set(a, link_object, "homepage", first_string(tj::Get(links, "homepage"), false)) &&
            set(a, link_object, "twitter", tj::IsString(screen) ? tj::Clone(a, screen) : tj::Null(a)) &&
            set(a, link_object, "github", first_string(tj::Get(repos, "github"), false)) &&
            set(a, link_object, "discord", first_string(chat, true)) &&
            set(a, link_object, "telegram", telegram_url) &&
            set(a, link_object, "reddit", tj::IsString(subreddit) ? tj::Clone(a, subreddit) : tj::Null(a)) &&
            set(a, link_object, "whitepaper", whitepaper ? tj::Clone(a, whitepaper) : tj::Null(a));
  Value* out = tj::Obj(a);
  ok = ok && out && set(a, out, "description", description_value) && set(a, out, "categories", category_list) &&
       set(a, out, "links", link_object) &&
       set(a, out, "sentimentUpPct", tj::IsNumber(sentiment) ? tj::Num(a, sentiment->number) : tj::Null(a)) &&
       set(a, out, "marketsCount", tj::IsArray(tickers) ? tj::Num(a, tickers->count) : tj::Null(a)) &&
       set(a, out, "logoMediaKey", logo_key);
  if (!ok) return false;
  *profile = out;
  return true;
}

Value* MarketWithChainSupply(Arena& a, const Value* market, const Value* chain_meta) {
  if (!tj::IsObject(market)) return tj::Null(a);
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  for (uint32_t i = 0; i < market->count; ++i)
    if (!tj::Set(a, out, market->members[i].key, market->members[i].key_length, market->members[i].value))
      return nullptr;
  double total = 0, max = 0, price = 0;
  const bool has_total = FiniteNumberOrString(tj::Get(chain_meta, "totalSupply"), &total);
  const bool has_max = FiniteNumberOrString(tj::Get(chain_meta, "maxSupply"), &max);
  const bool has_price = FiniteNumberOrString(tj::Get(market, "current_price"), &price);
  if (has_total && !set(a, out, "total_supply", tj::Num(a, total))) return nullptr;
  if (has_max && !set(a, out, "max_supply", tj::Num(a, max))) return nullptr;
  if (has_price && has_max && !set(a, out, "fully_diluted_valuation", tj::Num(a, price * max))) return nullptr;
  return out;
}

void MarketComparisonPatch(Arena& a, const Value* market, int64_t as_of_ms, const char* source_id,
                           const char* source, token_store::ComparisonPatch* patch) {
  using token_store::FieldState;
  *patch = token_store::ComparisonPatch{};
  const Value* view = MarketView(a, market);
  auto number = [&](token_store::NumberPatch& field, const char* key) {
    const Value* v = tj::Get(view, key);
    if (tj::IsFinite(v)) { field.state = FieldState::kValue; field.value = v->number; }
    else field.state = FieldState::kNull;
  };
  patch->fiat = {FieldState::kValue, "usd", 3};
  if (source_id) patch->market_source_id = {FieldState::kValue, source_id, strlen(source_id)};
  else patch->market_source_id.state = FieldState::kNull;
  number(patch->market_price_usd, "priceUsd");
  number(patch->price_change_1h_pct, "change1hPct");
  number(patch->price_change_24h_pct, "change24hPct");
  number(patch->price_change_7d_pct, "change7dPct");
  number(patch->price_change_30d_pct, "change30dPct");
  number(patch->market_cap_usd, "marketCapUsd");
  number(patch->fdv_usd, "fdvUsd");
  number(patch->volume_24h_usd, "volume24hUsd");
  number(patch->market_cap_rank, "marketCapRank");
  patch->market_as_of = {FieldState::kValue, as_of_ms};
  patch->market_source = {FieldState::kValue, source, strlen(source)};
  patch->updated_at = {FieldState::kValue, as_of_ms};
}

void LiquidityComparisonPatch(const Value* snapshot, int64_t as_of_ms, const char* source,
                              token_store::ComparisonPatch* patch) {
  using token_store::FieldState;
  *patch = token_store::ComparisonPatch{};
  auto number = [&](token_store::NumberPatch& field, const Value* v) {
    if (tj::IsFinite(v)) { field.state = FieldState::kValue; field.value = v->number; }
    else field.state = FieldState::kNull;
  };
  patch->fiat = {FieldState::kValue, "usd", 3};
  const Value* weighted = tj::Get(snapshot, "weightedPriceUsd");
  number(patch->dex_price_usd, weighted && weighted->type != tj::Type::kNull ? weighted : tj::Get(snapshot, "priceUsd"));
  number(patch->dex_market_cap_usd, tj::Get(snapshot, "marketCap"));
  number(patch->dex_fdv_usd, tj::Get(snapshot, "fdv"));
  number(patch->dex_volume_24h_usd, tj::Get(snapshot, "volume24hUsd"));
  number(patch->liquidity_usd, tj::Get(snapshot, "totalUsd"));
  number(patch->spread_pct, tj::Get(snapshot, "priceSpreadPct"));
  number(patch->top_pool_share, tj::Get(snapshot, "topPoolShare"));
  number(patch->pool_count, tj::Get(snapshot, "poolCount"));
  patch->liquidity_status = {FieldState::kValue, comparison::LiquidityStatus::kOk};
  patch->liquidity_checked_at = {FieldState::kValue, as_of_ms};
  patch->liquidity_as_of = {FieldState::kValue, as_of_ms};
  patch->liquidity_source = {FieldState::kValue, source, strlen(source)};
  patch->updated_at = {FieldState::kValue, as_of_ms};
}

Value* SolanaChainMeta(Arena& a, const Value* row, storage::KvStore* kv, int64_t now_ms) {
  const Value* symbol = tj::Get(row, "symbol");
  const Value* name = tj::Get(row, "name");
  const Value* decimals = tj::Get(row, "decimals");
  const Value* verified = tj::Get(row, "isVerified");
  const Value* holders = tj::Get(row, "holderCount");
  const Value* icon = tj::Get(row, "icon");
  // media.register([icon]): an http(s) string gets its key.
  Value* logo = tj::Null(a);
  if (tj::IsString(icon)) {
    char key[discovery::market_view::kMediaKeyChars + 1];
    const char* url = tj::CString(a, icon);
    if (url && codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(url), icon->length) &&
        discovery::RegisterMediaSource(kv, url, icon->length, now_ms, key))
      logo = tj::Str(a, key);
  }
  const bool integer = tj::IsNumber(decimals) && isfinite(decimals->number) &&
                       floor(decimals->number) == decimals->number;
  Value* symbol_value = tj::IsString(symbol) ? codec::provider_text::SanitizeSymbol(a, symbol->text, symbol->length)
                                             : tj::Null(a);
  Value* name_value = tj::IsString(name) ? codec::provider_text::SanitizeName(a, name->text, name->length) : tj::Null(a);
  Value* decimals_value = integer ? tj::Num(a, decimals->number) : tj::Null(a);
  Value* verified_value = tj::IsBool(verified) ? tj::Bool(a, verified->type == tj::Type::kTrue) : tj::Null(a);
  Value* holders_value = tj::IsNumber(holders) ? tj::Num(a, holders->number) : tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !logo || !symbol_value || !name_value || !decimals_value || !verified_value || !holders_value ||
      !tj::Set(a, o, "symbol", symbol_value) || !tj::Set(a, o, "name", name_value) ||
      !tj::Set(a, o, "decimals", decimals_value) || !tj::Set(a, o, "verified", verified_value) ||
      !tj::Set(a, o, "holderCount", holders_value) || !tj::Set(a, o, "logoMediaKey", logo))
    return nullptr;
  return o;
}

namespace {

// `${value}` for the stored chain-meta fields: a number as JavaScript prints
// it, an absent key as "undefined" (the value is otherwise one this port
// wrote, so a number, a boolean or a string).
Value* template_text(Arena& a, const Value* v) {
  if (!v) return tj::Str(a, "undefined");
  if (tj::IsString(v)) return tj::Str(a, v->text, v->length);
  if (v->type == tj::Type::kTrue) return tj::Str(a, "true");
  if (v->type == tj::Type::kFalse) return tj::Str(a, "false");
  if (tj::IsNumber(v)) {
    char text[64];
    codec::history_batch::OutBuffer out{reinterpret_cast<uint8_t*>(text), sizeof(text) - 1};
    codec::history_batch::AppendJsNumber(out, v->number);
    if (!out.ok) return nullptr;
    return tj::Str(a, text, out.used);
  }
  if (tj::IsObject(v)) return tj::Str(a, "[object Object]");
  return nullptr;
}

bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull: case tj::Type::kFalse: return false;
    case tj::Type::kTrue: case tj::Type::kArray: case tj::Type::kObject: return true;
    case tj::Type::kNumber: return v->number != 0 && !isnan(v->number);
    case tj::Type::kString: return v->length > 0;
  }
  return false;
}

}  // namespace

bool AppendSolanaInsight(Arena& a, const Value* meta, Value* sections) {
  if (!meta || meta->type == tj::Type::kNull) return true;
  // `meta.x !== null`: an absent key is undefined, which is not null.
  auto present = [](const Value* v) { return !v || v->type != tj::Type::kNull; };
  const Value* verified = tj::Get(meta, "verified");
  const Value* holders = tj::Get(meta, "holderCount");
  const Value* decimals = tj::Get(meta, "decimals");
  Value* rows = tj::Arr(a);
  if (!rows) return false;
  auto row = [&](const char* label, Value* value, const char* sentiment) {
    Value* r = tj::Obj(a);
    return r && value && tj::Set(a, r, "label", tj::Str(a, label)) && tj::Set(a, r, "value", value) &&
           (!sentiment || tj::Set(a, r, "sentiment", tj::Str(a, sentiment))) && tj::Push(a, rows, r);
  };
  if (present(verified) &&
      !row("Listed as verified", tj::Str(a, truthy(verified) ? "Yes" : "No"), truthy(verified) ? "good" : "caution"))
    return false;
  if (present(holders) && !row("Holders", template_text(a, holders), nullptr)) return false;
  if (present(decimals) && !row("Decimals", template_text(a, decimals), nullptr)) return false;
  if (rows->count == 0) return true;
  const bool listed = verified && verified->type == tj::Type::kTrue;
  Value* section = tj::Obj(a);
  return section && tj::Set(a, section, "title", tj::Str(a, "Token list")) &&
         tj::Set(a, section, "summary", tj::Str(a, listed ? "Verified on the Solana token list"
                                                           : "Not verified on the Solana token list")) &&
         tj::Set(a, section, "ok", tj::Bool(a, true)) && tj::Set(a, section, "rows", rows) &&
         tj::Push(a, sections, section);
}

}  // namespace fortis::tokens::token_views
