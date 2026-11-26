#include <string_view>
#include "query_string.h"
#include "token_page.h"

#include "provider_runtime_config.h"
#include "price_history_view.h"
#include "price_series_store.h"
#include "token_comparison.h"
#include "token_comparison_store.h"
#include "token_jobs.h"
#include "token_liquidity.h"
#include "provider_text.h"
#include "token_views.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_page {
namespace {

using token_facts::Facts;
using token_facts::Provenance;
using token_facts::FactsOutcome;
using tj::Arena;

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }
Value* or_null(Arena& a, Value* v) { return v ? v : tj::Null(a); }

const char* const kSectionNames[kSectionCount] = {
    "identity", "quote", "market", "profile", "liquidity", "holders",
    "safety", "contract", "comparison", "insight", "series"};

// `a ?? b ?? …`: the first value that is neither absent nor JSON null.
const Value* first_present(const Value* const* values, size_t count) {
  for (size_t i = 0; i < count; ++i) if (!is_null(values[i])) return values[i];
  return nullptr;
}

// transferTokenIdFor(ref).
Value* transfer_token_id(Arena& a, const networks::TokenRef& ref) {
  switch (ref.kind) {
    case networks::TokenRefKind::kNative: case networks::TokenRefKind::kErc20: case networks::TokenRefKind::kTrc10: case networks::TokenRefKind::kTrc20:
    case networks::TokenRefKind::kSpl: case networks::TokenRefKind::kAsa: case networks::TokenRefKind::kHts: case networks::TokenRefKind::kBittensorAlpha:
    case networks::TokenRefKind::kAptos: case networks::TokenRefKind::kDenom:
      return tj::Str(a, ref.token_id);
    case networks::TokenRefKind::kSui: {
      if (!ref.has_address) return tj::Null(a);
      char text[networks::kTokenRefMaxInputBytes + 8];
      snprintf(text, sizeof(text), "coin:%s", ref.address);
      return tj::Str(a, text);
    }
    case networks::TokenRefKind::kCardano: {
      if (!ref.has_address) return tj::Null(a);
      char text[networks::kTokenRefMaxInputBytes + 8];
      snprintf(text, sizeof(text), "cnt:%s", ref.address);
      return tj::Str(a, text);
    }
    case networks::TokenRefKind::kStellar: case networks::TokenRefKind::kXrpl: {
      if (!ref.has_issuer || !ref.has_symbol) return tj::Null(a);
      char text[2 * networks::kTokenRefMaxInputBytes + 8];
      snprintf(text, sizeof(text), "iou:%s:%s", ref.issuer, ref.symbol);
      return tj::Str(a, text);
    }
    case networks::TokenRefKind::kSymbol: case networks::TokenRefKind::kCoinGecko:
      return tj::Null(a);
  }
  return tj::Null(a);
}

Value* opt(Arena& a, bool has, const char* text) { return has ? tj::Str(a, text) : tj::Null(a); }

// TokenPageFacet.identity(facts, market).
Value* identity(Arena& a, const Facts& facts, const Value* market) {
  const networks::TokenRef& ref = facts.ref;
  const bool native = ref.kind == networks::TokenRefKind::kNative;
  const Value* chain = facts.chain_meta;
  Value* native_symbol = native && facts.network.native_symbol ? tj::Str(a, facts.network.native_symbol) : nullptr;
  Value* native_decimals = native && facts.seed && facts.network.native_symbol
                               ? tj::Num(a, facts.seed->native_decimals) : nullptr;
  Value* ref_symbol = ref.has_symbol ? tj::Str(a, ref.symbol) : nullptr;
  const Value* registry_symbol = facts.registry.found ? facts.registry.symbol : nullptr;
  const Value* registry_name = facts.registry.found ? facts.registry.name : nullptr;
  const Value* symbols[] = {tj::Get(chain, "symbol"), registry_symbol, native_symbol, ref_symbol,
                            tj::Get(market, "symbol")};
  const Value* symbol = first_present(symbols, 5);
  Value* symbol_value = tj::Null(a);
  if (symbol) {
    // `.toUpperCase()` of a string; a cached non-string would throw.
    if (!tj::IsString(symbol)) return nullptr;
    symbol_value = codec::provider_text::ToUpperCase(a, symbol->text, symbol->length);
  }
  const Value* names[] = {tj::Get(chain, "name"), registry_name, tj::Get(market, "name")};
  const Value* decimals[] = {tj::Get(chain, "decimals"), native_decimals};
  const Value* profile_logo = tj::Get(facts.profile, "logoMediaKey");
  const Value* logos[] = {tj::Get(chain, "logoMediaKey"),
                          tj::IsString(profile_logo) && profile_logo->length ? profile_logo : nullptr};
  const Value* name = first_present(names, 3);
  const Value* decimal = first_present(decimals, 2);
  const Value* logo = first_present(logos, 2);
  const Value* verified = tj::Get(chain, "verified");
  const token_facts::Resolution& r = facts.resolution;
  const char* source = token_facts::ResolutionSourceName(r.source);
  Value* o = tj::Obj(a);
  if (!o || !symbol_value || !tj::Set(a, o, "tokenId", tj::Str(a, ref.token_id)) ||
      !tj::Set(a, o, "transferTokenId", transfer_token_id(a, ref)) ||
      !tj::Set(a, o, "kind", tj::Str(a, TokenRefKindName(ref.kind))) ||
      !tj::Set(a, o, "network", tj::Str(a, facts.network.id)) ||
      !tj::Set(a, o, "address", opt(a, ref.has_address, ref.address)) ||
      !tj::Set(a, o, "issuer", opt(a, ref.has_issuer, ref.issuer)) || !tj::Set(a, o, "symbol", symbol_value) ||
      !tj::Set(a, o, "name", name ? tj::Clone(a, name) : tj::Null(a)) ||
      !tj::Set(a, o, "decimals", decimal ? tj::Clone(a, decimal) : tj::Null(a)) ||
      !tj::Set(a, o, "native", tj::Bool(a, native)) ||
      !tj::Set(a, o, "logoMediaKey", logo ? tj::Clone(a, logo) : tj::Null(a)) ||
      !tj::Set(a, o, "coingeckoId", r.has_coin ? tj::Str(a, r.coin_id) : tj::Null(a)) ||
      !tj::Set(a, o, "coingeckoIdSource", source ? tj::Str(a, source) : tj::Null(a)) ||
      !tj::Set(a, o, "ambiguous", tj::Bool(a, r.ambiguous)) ||
      !tj::Set(a, o, "verified", is_null(verified) ? tj::Null(a) : tj::Clone(a, verified)) ||
      !tj::Set(a, o, "priceId", opt(a, ref.has_price_id, ref.price_id)) ||
      !tj::Set(a, o, "input", tj::Clone(a, facts.input)))
    return nullptr;
  return o;
}

struct SectionResult {
  Value* value = nullptr;
  Provenance cache;
};

Value* quote_value(Arena& a, double price, const Value* change, const char* source, const Value* as_of) {
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "priceUsd", tj::Num(a, price)) ||
      !tj::Set(a, o, "change24hPct", change ? tj::Clone(a, change) : tj::Null(a)) ||
      !tj::Set(a, o, "source", tj::Str(a, source)) || !tj::Set(a, o, "asOf", as_of ? tj::Clone(a, as_of) : tj::Null(a)))
    return nullptr;
  return o;
}

Value* timestamp_value(Arena& a, const Provenance& p) {
  return p.has_timestamp ? token_facts::IsoValue(a, p.timestamp) : tj::Null(a);
}

// TokenPageFacet.quote(facts, market).
bool quote(Work& work, const Facts& facts, const Value* market, SectionResult* out) {
  Arena& a = work.arena;
  const Value* price = tj::Get(market, "priceUsd");
  if (tj::IsNumber(price) && price->number != 0 && !isnan(price->number)) {
    out->value = quote_value(a, price->number, tj::Get(market, "change24hPct"), "market",
                             timestamp_value(a, facts.cache[token_facts::kMarket]));
    out->cache = facts.cache[token_facts::kMarket];
    return out->value != nullptr;
  }
  const char* price_id = facts.resolution.has_coin ? facts.resolution.coin_id
                       : facts.ref.has_price_id ? facts.ref.price_id : nullptr;
  if (price_id && work.env->spot) {
    token_facts::SpotQuote hit{};
    if (work.env->spot->Quote(price_id, work.deadline_ms, &hit)) {
      Value* change = hit.has_change ? tj::Num(a, hit.change) : tj::Null(a);
      Value* as_of = hit.has_as_of ? tj::Str(a, hit.as_of) : tj::Null(a);
      out->value = quote_value(a, hit.price, change, "spot", as_of);
      out->cache = hit.cache;
      return out->value != nullptr;
    }
  }
  double dex = 0;
  if (token_views::FiniteNumber(tj::Get(facts.liquidity, "priceUsd"), &dex) && dex > 0) {
    out->value = quote_value(a, dex, nullptr, "dex", timestamp_value(a, facts.cache[token_facts::kLiquidity]));
    out->cache = facts.cache[token_facts::kLiquidity];
    return out->value != nullptr;
  }
  out->value = nullptr;
  const Provenance entries[] = {facts.cache[token_facts::kMarket], facts.cache[token_facts::kLiquidity]};
  out->cache = token_facts::CombineProvenance(entries, 2);
  return true;
}

struct ComparisonKey {
  char network[networks::kTokenNetworkMaxGenericIdBytes + 1];
  char token_id[networks::kTokenRefMaxIdBytes + 1];
};

void copy_native(comparison::OptString* field, const char* text) {
  if (field->present || !text) return;
  const size_t n = strlen(text);
  if (n > comparison::kMaxStoredString) return;
  field->present = true;
  field->length = n;
  memcpy(field->text, text, n + 1);
}

// TokenComparisonService.compare(keys, 'usd').items over canonical keys.
bool compare_items(Work& work, const ComparisonKey* keys, size_t count, Value** items) {
  Arena& a = work.arena;
  auto* records = static_cast<comparison::Record*>(a.Alloc(sizeof(comparison::Record) * count));
  auto* result_items = static_cast<comparison::Item*>(a.Alloc(sizeof(comparison::Item) * count));
  token_store::Key store_keys[comparison::kMaxTokens];
  if (!records || !result_items || count > comparison::kMaxTokens) return false;
  for (size_t i = 0; i < count; ++i) {
    new (&records[i]) comparison::Record();
    store_keys[i].network = keys[i].network;
    store_keys[i].token_id = keys[i].token_id;
  }
  const int64_t now = work.Now();
  TokenComparisonStore store(work.env->kv);
  if (store.Read(store_keys, count, "usd", now, records) != token_store::ReadStatus::kOk) return false;
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(records[i].token_id, "native") != 0) continue;
    networks::TokenNetwork network;
    if (!ResolveTokenNetwork(keys[i].network, strlen(keys[i].network), &network)) continue;
    copy_native(&records[i].symbol, network.native_symbol);
    copy_native(&records[i].name, network.native_symbol);
    copy_native(&records[i].market_source_id, network.native_coingecko_id);
  }
  // TokenFetchComparisonDemandObserver: one foreground market sample each.
  if (work.env->stats) {
    for (size_t i = 0; i < count; ++i) {
      token_jobs::FetchSample sample{};
      sample.network = records[i].network;
      sample.token_id = records[i].token_id;
      sample.facet = token_jobs::FetchFacet::kMarket;
      sample.source_id = records[i].market_source_id.present ? records[i].market_source_id.text : nullptr;
      sample.outcome = token_jobs::FetchOutcome::kFloor;
      sample.negative = !records[i].market_price_usd.present && !records[i].market_cap_usd.present &&
                        !records[i].volume_24h_usd.present;
      sample.foreground = true;
      sample.at_ms = now;
      work.env->stats->Record(sample);
    }
  }
  comparison::CompareTokenRecords(records, count, now, result_items);
  comparison::Response response{};
  response.computed_at_ms = now;
  response.items = result_items;
  response.item_count = count;
  const size_t capacity = 64 * 1024 * (count ? count : 1);
  auto* buffer = static_cast<uint8_t*>(a.Alloc(capacity, 1));
  if (!buffer) return false;
  codec::history_batch::OutBuffer out{buffer, capacity, 0, true};
  comparison::AppendResponseJson(out, response, true);
  if (!out.ok) return false;
  Value* parsed = tj::Parse(a, buffer, out.used);
  Value* list = tj::Get(parsed, "items");
  if (!tj::IsArray(list)) return false;
  *items = list;
  return true;
}

// TokenPageFacet.comparison(facts, errors).
bool comparison_section(Work& work, const Facts& facts, Value* errors, SectionResult* out) {
  Arena& a = work.arena;
  static const char* const kBenchmarks[][2] = {{"bitcoin", "native"}, {"bnb", "native"}};
  ComparisonKey keys[3]{};
  size_t count = 0;
  auto add = [&](const char* network, const char* token_id) {
    for (size_t i = 0; i < count; ++i)
      if (strcmp(keys[i].network, network) == 0 && strcmp(keys[i].token_id, token_id) == 0) return;
    snprintf(keys[count].network, sizeof(keys[count].network), "%s", network);
    snprintf(keys[count].token_id, sizeof(keys[count].token_id), "%s", token_id);
    ++count;
  };
  add(facts.network.id, facts.ref.token_id);
  for (const auto& benchmark : kBenchmarks) add(benchmark[0], benchmark[1]);
  Provenance caches[3];
  size_t cache_count = 0;
  caches[cache_count++] = facts.cache[token_facts::kMarket];
  for (const auto& benchmark : kBenchmarks) {
    if (strcmp(benchmark[0], facts.network.id) == 0 && strcmp(benchmark[1], facts.ref.token_id) == 0) continue;
    const size_t mark = a.used;
    Facts* benchmark_facts = static_cast<Facts*>(a.Alloc(sizeof(Facts)));
    if (!benchmark_facts) return false;
    new (benchmark_facts) Facts();
    const FactsOutcome status = token_facts::ComputeFacts(work, std::string_view{benchmark[0], strlen(benchmark[0])}, std::string_view{benchmark[1], strlen(benchmark[1])}, nullptr, token_facts::FacetBit(token_facts::kMarket), benchmark_facts);
    if (status == FactsOutcome::kOk) caches[cache_count++] = benchmark_facts->cache[token_facts::kMarket];
    else a.used = mark;  // a failed benchmark is logged and skipped
  }
  out->cache = token_facts::CombineProvenance(caches, cache_count);
  Value* items = nullptr;
  if (!compare_items(work, keys, count, &items)) {
    tj::Set(a, errors, "comparison", tj::Str(a, "UNAVAILABLE"));
    out->value = nullptr;
    return true;
  }
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "fiat", tj::Str(a, "usd")) || !tj::Set(a, o, "items", items)) return false;
  out->value = o;
  return true;
}

// TokenPageFacet.series(facts, options, errors): closes and sparse volumes
// from the shared candle store through PricesFacet.history, which records
// one `history` fetch sample like the TypeScript facet.
bool series_section(Work& work, const Facts& facts, const PageOptions& options, Value* errors,
                    SectionResult* out) {
  Arena& a = work.arena;
  out->value = nullptr;
  out->cache = Provenance{};  // cacheProvenance(false, null)
  // A resolved market id first (only it can backfill gaps), else the
  // provider-native id whose candles were collected prospectively.
  const char* price_id = facts.resolution.has_coin ? facts.resolution.coin_id
                       : facts.ref.has_price_id ? facts.ref.price_id : nullptr;
  if (!price_id || !*price_id) return true;
  const bool hourly = options.interval == SeriesInterval::k1h;
  const int64_t started = work.Now();
  const int64_t to = static_cast<int64_t>(floor(static_cast<double>(started) / 1000));
  const int64_t from = to - static_cast<int64_t>(options.days * 86400);
  token_facts::SeriesRead read{};
  const token_facts::SeriesStatus status =
      work.env->series ? work.env->series->Read(work, price_id, from, to, hourly, &read)
                       : token_facts::SeriesStatus::kUnavailable;
  if (work.env->stats) {
    token_jobs::FetchSample sample{};
    sample.network = "-";
    sample.token_id = price_id;
    sample.source_id = price_id;
    sample.facet = token_jobs::FetchFacet::kHistory;
    sample.outcome = status != token_facts::SeriesStatus::kOk ? token_jobs::FetchOutcome::kFailure
                     : read.cache.hit ? token_jobs::FetchOutcome::kFresh : token_jobs::FetchOutcome::kLive;
    sample.negative = status == token_facts::SeriesStatus::kOk && read.count == 0;
    sample.foreground = true;
    sample.latency_ms = static_cast<double>(work.Now() - started);
    sample.at_ms = work.Now();
    work.env->stats->Record(sample);
  }
  if (status == token_facts::SeriesStatus::kNoSeries) return true;
  if (status != token_facts::SeriesStatus::kOk) {
    // The TypeScript catch (the native token code keeps no per-request log).
    return tj::Set(a, errors, "series", tj::Str(a, "UPSTREAM_UNAVAILABLE"));
  }
  Value* points = tj::Arr(a);
  Value* volumes = tj::Arr(a);
  if (!points || !volumes) return false;
  for (size_t i = 0; i < read.count; ++i) {
    const prices::PriceSeriesCandle& candle = read.candles[i];
    const double seconds = floor(static_cast<double>(candle.ts) / 1000);
    Value* point = tj::Arr(a);
    if (!point || !tj::Push(a, point, tj::Num(a, seconds)) || !tj::Push(a, point, tj::Num(a, candle.close)) ||
        !tj::Push(a, points, point))
      return false;
    if (!candle.has_volume) continue;
    Value* volume = tj::Arr(a);
    if (!volume || !tj::Push(a, volume, tj::Num(a, seconds)) || !tj::Push(a, volume, tj::Num(a, candle.volume)) ||
        !tj::Push(a, volumes, volume))
      return false;
  }
  Value* window = tj::Obj(a);
  if (!window) return false;
  if (read.count) {
    const int64_t first = read.candles[0].ts / 1000;
    const int64_t last = read.candles[read.count - 1].ts / 1000;
    if (!tj::Set(a, window, "from", tj::Num(a, static_cast<double>(first))) ||
        !tj::Set(a, window, "to", tj::Num(a, static_cast<double>(last))) ||
        !tj::Set(a, window, "days", tj::Num(a, prices::history_batch::HistoryAvailableDays(first, last))))
      return false;
  } else if (!tj::Set(a, window, "from", tj::Null(a)) || !tj::Set(a, window, "to", tj::Null(a)) ||
             !tj::Set(a, window, "days", tj::Num(a, 0))) {
    return false;
  }
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "interval", tj::Str(a, hourly ? "1h" : "1d")) || !tj::Set(a, o, "points", points) ||
      !tj::Set(a, o, "volumes", volumes) || !tj::Set(a, o, "window", window))
    return false;
  out->value = o;
  out->cache = read.cache;
  return true;
}

// oldestSectionAsOf(cache).
Value* oldest_as_of(Arena& a, int64_t now, const Provenance* entries, size_t count) {
  int64_t oldest = now;
  for (size_t i = 0; i < count; ++i) {
    const int64_t at = entries[i].has_timestamp ? entries[i].timestamp : now;
    if (at < oldest) oldest = at;
  }
  return token_facts::IsoValue(a, oldest);
}

}  // namespace

const char* SectionName(Section section) { return kSectionNames[section]; }

void Sections::Add(Section section) {
  if (Has(section)) return;
  order[count++] = section;
  mask |= 1u << section;
}

void DefaultSections(Sections* sections) {
  *sections = Sections{};
  static constexpr Section kDefault[] = {kIdentity, kQuote, kMarket, kProfile, kLiquidity,
                                        kHolders, kSafety, kContract, kComparison};
  for (Section s : kDefault) sections->Add(s);
}

bool ParseSections(const char* raw, size_t length, Sections* sections, char* unknown, size_t unknown_capacity,
                   size_t* unknown_length) {
  *sections = Sections{};
  *unknown_length = 0;
  if (unknown_capacity) unknown[0] = '\0';
  if (!raw || length == 0) { DefaultSections(sections); return true; }
  // raw.toLowerCase() === 'all' (at most 3 UTF-16 units lower-case to it).
  if (length <= 16) {
    uint8_t all_bytes[256];
    Arena all_arena{all_bytes, sizeof(all_bytes), 0, true};
    Value* lower = codec::provider_text::ToLowerCase(all_arena, raw, length);
    if (lower && lower->length == 3 && memcmp(lower->text, "all", 3) == 0) {
      for (size_t i = 0; i < kSectionCount; ++i) sections->Add(static_cast<Section>(i));
      return true;
    }
  }
  // names = raw.split(',').map(trim().toLowerCase()).filter(Boolean)
  uint8_t arena_bytes[8192];
  Arena arena{arena_bytes, sizeof(arena_bytes), 0, true};
  size_t unknown_used = 0;
  bool any_unknown = false;
  size_t name_count = 0;
  Section names[256];
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i < length && raw[i] != ',') continue;
    const char* segment = raw + start;
    const size_t segment_length = i - start;
    start = i + 1;
    size_t begin = 0, end = 0;
    codec::provider_text::Trim(segment, segment_length, &begin, &end);
    if (end == begin) continue;
    Value* lower = codec::provider_text::ToLowerCase(arena, segment + begin, end - begin);
    if (!lower) return false;
    int found = -1;
    for (size_t s = 0; s < kSectionCount && found < 0; ++s)
      if (strlen(kSectionNames[s]) == lower->length && memcmp(kSectionNames[s], lower->text, lower->length) == 0)
        found = static_cast<int>(s);
    if (found < 0) {
      // `unknown include: ${unknown.join(',')}`, each name by length (a NUL
      // stays a NUL, JSON.stringify writes \u0000).
      static const char kPrefix[] = "unknown include: ";
      const char* lead = any_unknown ? "," : kPrefix;
      const size_t lead_length = any_unknown ? 1 : sizeof(kPrefix) - 1;
      any_unknown = true;
      if (unknown_used + lead_length + lower->length < unknown_capacity) {
        memcpy(unknown + unknown_used, lead, lead_length);
        memcpy(unknown + unknown_used + lead_length, lower->text, lower->length);
        unknown_used += lead_length + lower->length;
        unknown[unknown_used] = '\0';
      }
      continue;
    }
    if (name_count < 256) names[name_count++] = static_cast<Section>(found);
  }
  *unknown_length = unknown_used;
  if (any_unknown) return false;
  if (name_count == 0) { DefaultSections(sections); return true; }
  for (size_t i = 0; i < name_count; ++i) sections->Add(names[i]);
  return true;
}

uint32_t FacetsForSections(const Sections& requested) {
  Sections sections = requested;
  sections.Add(kIdentity);
  uint32_t only = token_facts::FacetBit(token_facts::kChainMeta);
  if (sections.Has(kMarket) || sections.Has(kQuote) || sections.Has(kSafety) || sections.Has(kComparison))
    only |= token_facts::FacetBit(token_facts::kMarket);
  if (sections.Has(kProfile)) only |= token_facts::FacetBit(token_facts::kProfile);
  if (sections.Has(kLiquidity) || sections.Has(kQuote) || sections.Has(kSafety))
    only |= token_facts::FacetBit(token_facts::kLiquidity);
  if (sections.Has(kHolders) || sections.Has(kSafety)) only |= token_facts::FacetBit(token_facts::kHolders);
  if (sections.Has(kInsight)) only |= token_facts::FacetBit(token_facts::kInsight);
  if (sections.Has(kContract)) only |= token_facts::FacetBit(token_facts::kContract);
  return only;
}

PageQueryStatus ParsePageQuery(const PageQueryValue& include, const PageQueryValue& days,
                               const PageQueryValue& interval, const PageQueryValue& coingecko_id, char*,
                               size_t, PageOptions* options, char* hint, size_t hint_capacity,
                               bool* has_hint, char* detail, size_t detail_capacity, size_t* detail_length) {
  *options = PageOptions{};
  *has_hint = false;
  *detail_length = 0;
  if (hint_capacity) hint[0] = '\0';
  // A repeated key is an array: no field accepts one.
  if (include.count > 1 || days.count > 1 || interval.count > 1 || coingecko_id.count > 1)
    return PageQueryStatus::kInvalid;
  // include: z.string().trim().max(200) in UTF-16 units.
  size_t include_begin = 0, include_end = 0;
  if (include.count) {
    if (!codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(include.text), include.length))
      return PageQueryStatus::kInvalid;
    codec::history_batch::JsTrim(include.text, include.length, &include_begin, &include_end);
    if (codec::history_batch::Utf16Units(include.text + include_begin, include_end - include_begin) > 200)
      return PageQueryStatus::kInvalid;
  }
  // days: z.coerce.number().int().min(1).max(365).default(200) over Number().
  if (days.count) {
    uint64_t value = 0;
    if (!http::query_string::ParseUnsigned(days.text, days.length, 365, &value) || !value)
      return PageQueryStatus::kInvalid;
    options->days = static_cast<double>(value);
  }
  // interval: z.enum(['1h', '1d']).default('1d'), exact.
  if (interval.count) {
    if (interval.length == 2 && memcmp(interval.text, "1h", 2) == 0) options->interval = SeriesInterval::k1h;
    else if (interval.length == 2 && memcmp(interval.text, "1d", 2) == 0) options->interval = SeriesInterval::k1d;
    else return PageQueryStatus::kInvalid;
  }
  // coingeckoId: trim, toLowerCase, then /^[a-z0-9][a-z0-9-]{0,99}$/.
  if (coingecko_id.count) {
    if (!codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(coingecko_id.text), coingecko_id.length))
      return PageQueryStatus::kInvalid;
    size_t begin = 0, end = 0;
    codec::history_batch::JsTrim(coingecko_id.text, coingecko_id.length, &begin, &end);
    // Only ASCII can match, and toLowerCase never shortens below a third:
    // a trimmed value over 300 bytes cannot lower-case to 100 characters.
    if (end - begin > 300) return PageQueryStatus::kInvalid;
    uint8_t arena_bytes[4096];
    Arena arena{arena_bytes, sizeof(arena_bytes), 0, true};
    Value* lowered = codec::provider_text::ToLowerCase(arena, coingecko_id.text + begin, end - begin);
    if (!lowered || lowered->length < 1 || lowered->length > 100 || lowered->length >= hint_capacity)
      return PageQueryStatus::kInvalid;
    for (uint32_t i = 0; i < lowered->length; ++i) {
      const char c = lowered->text[i];
      const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (!alnum && !(i > 0 && c == '-')) return PageQueryStatus::kInvalid;
    }
    memcpy(hint, lowered->text, lowered->length);
    hint[lowered->length] = '\0';
    *has_hint = true;
  }
  if (!ParseSections(include.count ? include.text + include_begin : nullptr,
                     include.count ? include_end - include_begin : 0, &options->sections, detail, detail_capacity,
                     detail_length))
    return PageQueryStatus::kUnknownInclude;
  return PageQueryStatus::kOk;
}

FactsOutcome PageDocument(Work& work, const char* network, size_t network_length, const char* raw_ref,
                    size_t raw_length, const PageOptions& options, Value** document) {
  Arena& a = work.arena;
  *document = nullptr;
  Facts* facts = static_cast<Facts*>(a.Alloc(sizeof(Facts)));
  if (!facts) return FactsOutcome::kInternal;
  new (facts) Facts();
  const FactsOutcome status = token_facts::ComputeFacts(work, std::string_view{network, network_length}, std::string_view{raw_ref, raw_length}, options.coingecko_hint, FacetsForSections(options.sections), facts);
  if (status != FactsOutcome::kOk) return status;
  return ComposePage(work, *facts, options, document);
}

FactsOutcome ComposePage(Work& work, const Facts& facts_ref, const PageOptions& options, Value** document) {
  Arena& a = work.arena;
  *document = nullptr;
  Sections sections = options.sections;
  sections.Add(kIdentity);  // a page without identity is not a page
  const Facts* facts = &facts_ref;
  Value* errors = facts->errors ? tj::Clone(a, facts->errors) : tj::Obj(a);
  Value* market = sections.Has(kMarket) || sections.Has(kQuote)
                      ? (facts->market ? token_views::MarketView(a, facts->market) : nullptr) : nullptr;
  if (market && market->type == tj::Type::kNull) market = nullptr;
  Value* identity_value = identity(a, *facts, market);
  if (!errors || !identity_value) return FactsOutcome::kInternal;
  SectionResult quote_result, series_result, comparison_result;
  if (sections.Has(kQuote) && !quote(work, *facts, market, &quote_result)) return FactsOutcome::kInternal;
  if (sections.Has(kSeries) && !series_section(work, *facts, options, errors, &series_result)) return FactsOutcome::kInternal;
  if (sections.Has(kComparison) && !comparison_section(work, *facts, errors, &comparison_result))
    return FactsOutcome::kInternal;

  // cache, in the TypeScript assignment order.
  Value* cache = tj::Obj(a);
  if (!cache) return FactsOutcome::kInternal;
  Provenance entries[kSectionCount];
  size_t entry_count = 0;
  auto put = [&](const char* name, const Provenance& p) {
    Value* json = token_facts::ProvenanceJson(a, p);
    entries[entry_count++] = p;
    return json && tj::Set(a, cache, name, json);
  };
  {
    Provenance identity_parts[2] = {facts->cache[token_facts::kChainMeta], facts->cache[token_facts::kMarket]};
    if (!put("identity", token_facts::CombineProvenance(identity_parts, market ? 2 : 1))) return FactsOutcome::kInternal;
  }
  bool ok = true;
  if (sections.Has(kQuote)) ok = ok && put("quote", quote_result.cache);
  if (sections.Has(kMarket)) ok = ok && put("market", facts->cache[token_facts::kMarket]);
  if (sections.Has(kProfile)) ok = ok && put("profile", facts->cache[token_facts::kProfile]);
  if (sections.Has(kLiquidity)) ok = ok && put("liquidity", facts->cache[token_facts::kLiquidity]);
  if (sections.Has(kHolders)) ok = ok && put("holders", facts->cache[token_facts::kHolders]);
  if (sections.Has(kSafety)) {
    const Provenance parts[3] = {facts->cache[token_facts::kMarket], facts->cache[token_facts::kLiquidity],
                                 facts->cache[token_facts::kHolders]};
    ok = ok && put("safety", token_facts::CombineProvenance(parts, 3));
  }
  if (sections.Has(kContract)) ok = ok && put("contract", facts->cache[token_facts::kContract]);
  if (sections.Has(kComparison)) ok = ok && put("comparison", comparison_result.cache);
  if (sections.Has(kInsight)) ok = ok && put("insight", facts->cache[token_facts::kInsight]);
  if (sections.Has(kSeries)) ok = ok && put("series", series_result.cache);
  if (!ok) return FactsOutcome::kInternal;

  Value* included = tj::Arr(a);
  for (size_t i = 0; included && i < sections.count; ++i)
    if (!tj::Push(a, included, tj::Str(a, SectionName(sections.order[i])))) return FactsOutcome::kInternal;
  Value* profile = sections.Has(kProfile) ? token_views::ProfileView(a, facts->profile) : tj::Null(a);
  Value* doc = tj::Obj(a);
  if (!doc || !included || !profile ||
      !tj::Set(a, doc, "network", tj::Str(a, facts->network.id)) ||
      !tj::Set(a, doc, "ref", tj::Str(a, facts->ref.token_id)) || !tj::Set(a, doc, "input", tj::Clone(a, facts->input)) ||
      !tj::Set(a, doc, "asOf", oldest_as_of(a, work.Now(), entries, entry_count)) ||
      !tj::Set(a, doc, "included", included) || !tj::Set(a, doc, "identity", identity_value) ||
      !tj::Set(a, doc, "quote", or_null(a, quote_result.value)) ||
      !tj::Set(a, doc, "market", sections.Has(kMarket) ? or_null(a, market) : tj::Null(a)) ||
      !tj::Set(a, doc, "profile", profile) ||
      !tj::Set(a, doc, "liquidity", sections.Has(kLiquidity) ? or_null(a, facts->liquidity) : tj::Null(a)) ||
      !tj::Set(a, doc, "holders", sections.Has(kHolders) ? or_null(a, facts->holders) : tj::Null(a)) ||
      !tj::Set(a, doc, "safety", sections.Has(kSafety) ? or_null(a, facts->safety) : tj::Null(a)) ||
      !tj::Set(a, doc, "contract", sections.Has(kContract) ? or_null(a, facts->contract) : tj::Null(a)) ||
      !tj::Set(a, doc, "comparison", or_null(a, comparison_result.value)) ||
      !tj::Set(a, doc, "insight", sections.Has(kInsight) ? facts->insight : tj::Arr(a)) ||
      !tj::Set(a, doc, "series", or_null(a, series_result.value)) || !tj::Set(a, doc, "errors", errors) ||
      !tj::Set(a, doc, "cache", cache))
    return FactsOutcome::kInternal;
  *document = doc;
  return a.ok ? FactsOutcome::kOk : FactsOutcome::kInternal;
}

FactsOutcome Token360Document(Work& work, const char* network, size_t network_length, const char* raw_ref,
                        size_t raw_length, Value** document) {
  Arena& a = work.arena;
  *document = nullptr;
  Facts* facts = static_cast<Facts*>(a.Alloc(sizeof(Facts)));
  if (!facts) return FactsOutcome::kInternal;
  new (facts) Facts();
  const uint32_t only = token_facts::FacetBit(token_facts::kMarket) | token_facts::FacetBit(token_facts::kProfile) |
                        token_facts::FacetBit(token_facts::kLiquidity) |
                        token_facts::FacetBit(token_facts::kHolders) |
                        token_facts::FacetBit(token_facts::kChainMeta);
  const FactsOutcome status = token_facts::ComputeFacts(work, std::string_view{network, network_length}, std::string_view{raw_ref, raw_length}, nullptr, only, facts);
  if (status != FactsOutcome::kOk) return status;
  return Token360Project(a, *facts, document) ? FactsOutcome::kOk : FactsOutcome::kInternal;
}

bool Token360Project(Arena& a, const Facts& facts, Value** document) {
  *document = nullptr;
  // `facts.resolution.coinId ? {coingeckoId} : null`: an empty id is falsy.
  Value* meta = tj::Null(a);
  if (facts.resolution.has_coin && facts.resolution.coin_id[0]) {
    meta = tj::Obj(a);
    if (!meta || !tj::Set(a, meta, "coingeckoId", tj::Str(a, facts.resolution.coin_id))) return false;
  }
  // {count, floor: atLeast}; a member the holders object lacks is undefined,
  // which JSON.stringify drops.
  Value* holders = tj::Null(a);
  if (facts.holders) {
    holders = tj::Obj(a);
    if (!holders) return false;
    const Value* count = tj::Get(facts.holders, "count");
    const Value* at_least = tj::Get(facts.holders, "atLeast");
    if (count && !tj::Set(a, holders, "count", tj::Clone(a, count))) return false;
    if (at_least && !tj::Set(a, holders, "floor", tj::Clone(a, at_least))) return false;
  }
  Value* cache = tj::Obj(a);
  if (!cache || !tj::Set(a, cache, "market", token_facts::ProvenanceJson(a, facts.cache[token_facts::kMarket])) ||
      !tj::Set(a, cache, "profile", token_facts::ProvenanceJson(a, facts.cache[token_facts::kProfile])) ||
      !tj::Set(a, cache, "liquidity", token_facts::ProvenanceJson(a, facts.cache[token_facts::kLiquidity])) ||
      !tj::Set(a, cache, "holders", token_facts::ProvenanceJson(a, facts.cache[token_facts::kHolders])))
    return false;
  Value* doc = tj::Obj(a);
  if (!doc || !tj::Set(a, doc, "network", tj::Str(a, facts.network.id)) ||
      !tj::Set(a, doc, "tokenId", tj::Str(a, facts.ref.token_id)) || !tj::Set(a, doc, "meta", meta) ||
      !tj::Set(a, doc, "market", or_null(a, facts.market)) || !tj::Set(a, doc, "profile", or_null(a, facts.profile)) ||
      !tj::Set(a, doc, "liquidity", or_null(a, facts.liquidity)) || !tj::Set(a, doc, "holders", holders) ||
      !tj::Set(a, doc, "safety", or_null(a, facts.safety)) ||
      !tj::Set(a, doc, "errors", facts.errors ? facts.errors : tj::Obj(a)) || !tj::Set(a, doc, "cache", cache))
    return false;
  *document = doc;
  return a.ok;
}

}  // namespace fortis::tokens::token_page
