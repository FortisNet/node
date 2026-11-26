#include "test_support.h"
#include <string_view>
// Unit checks of GET /v1/networks/{network}/token/{tokenId}/page (R24) below
// the HTTP layer, against the TypeScript oracle token_page_edges.json:
//  - every `units` scenario: the stub TokenFacts, PricesFacet.spot and
//    PricesFacet.history answers TypeScript's TokenPageFacet.get composed,
//    rebuilt as native Facts and Env seams, give TypeScript's JSON text and
//    StructuredReadResponse wire byte for byte through ComposePage (identity
//    precedence, the quote decision market → spot → dex → null, the series
//    window and interval, per-section provenance in the facet's fixed order,
//    asOf the oldest section, errors), with the facts `only` set
//    (FacetsForSections), the spot ids and the history calls TypeScript made;
//  - one byte short of a scenario's encoding is refused, never truncated;
//  - every request record of the socket fixture whose path parameters are
//    valid: its query as fast-querystring decoded it, through
//    ParsePageQuery, gives the outcome TypeScript answered (the bare 400 for
//    a pageQuerySchema issue, the exact "unknown include: …" detail, or a
//    page whose `included` list is the parsed sections).
// Usage: token_page_test token_page_edges.json
#include "strict_unit_expectations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "fake_seams.h"
#include "kv_store.h"
#include "native_config.h"
#include "token_json.h"
#include "token_page.h"
#include "token_data.h"
#include "token_provider.h"
#include "token_swr.h"

using namespace fortis::config;
using namespace fortis::providers;

namespace {

namespace tj = fortis::codec::tj;
namespace tf = fortis::tokens::token_facts;
namespace tp = fortis::tokens::token_page;

int g_failures = 0;
int g_checks = 0;
void check(bool condition, const char* what, const char* detail = "") {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s %s\n", what, detail);
}

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file);
    free(data);
    return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

void to_hex(const uint8_t* bytes, size_t length, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 15];
  }
  out[2 * length] = '\0';
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// "2026-01-02T03:04:05.000Z" → epoch ms (the stub timestamps are UTC ISO).
bool parse_iso(const tj::Value* text, int64_t* out) {
  int year, month, day, hour, minute, second, millis;
  if (!tj::IsString(text) ||
      !fortis::test::ParseTimestampParts(text->text, &year, &month, &day, &hour, &minute, &second, &millis))
    return false;
  *out = ((days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 24 + hour) * 60 + minute) *
             60000 + second * 1000 + millis;
  return true;
}

bool is_true(const tj::Value* value) { return value && value->type == tj::Type::kTrue; }

const tj::Value* at(const tj::Value* array, uint32_t index) {
  return tj::IsArray(array) && index < array->count ? array->items[index] : nullptr;
}

tj::Value* member(const tj::Value* object, const char* key) {
  tj::Value* value = tj::Get(object, key);
  return value && value->type != tj::Type::kNull ? value : nullptr;
}

tf::Provenance provenance(const tj::Value* p) {
  tf::Provenance out{};
  out.hit = is_true(tj::Get(p, "hit"));
  out.partial = is_true(tj::Get(p, "partial"));
  out.has_timestamp = parse_iso(tj::Get(p, "timestamp"), &out.timestamp);
  return out;
}

bool resolution_source(const tj::Value* name, tf::ResolutionSource* out) {
  static const tf::ResolutionSource kAll[] = {tf::ResolutionSource::kRef, tf::ResolutionSource::kRegistry,
                                              tf::ResolutionSource::kContract, tf::ResolutionSource::kSearch,
                                              tf::ResolutionSource::kHint};
  *out = tf::ResolutionSource::kNone;
  if (!tj::IsString(name)) return !name || name->type == tj::Type::kNull;
  for (tf::ResolutionSource source : kAll)
    if (strcmp(tf::ResolutionSourceName(source), name->text) == 0) { *out = source; return true; }
  return false;
}

// The stub TokenFacts as native Facts: the network, seed and ref resolved as
// facts() does, the rest taken from the stub.
bool build_facts(tj::Arena& a, const tj::Value* unit, const tj::Value* stub, tf::Facts* facts) {
  new (facts) tf::Facts();
  const tj::Value* network = tj::Get(unit, "network");
  const tj::Value* ref = tj::Get(unit, "ref");
  if (!tj::IsString(network) || !tj::IsString(ref) ||
      tf::Resolve(std::string_view{network->text, network->length}, std::string_view{ref->text, ref->length}, &facts->network, &facts->seed, &facts->ref) != tf::FactsOutcome::kOk)
    return false;
  const tj::Value* input = tj::Get(tj::Get(stub, "ref"), "input");
  facts->input = tj::IsString(input) ? tj::Clone(a, input) : tj::Str(a, ref->text);
  const tj::Value* resolution = tj::Get(stub, "resolution");
  const tj::Value* coin = tj::Get(resolution, "coinId");
  if (tj::IsString(coin)) {
    if (coin->length >= sizeof(facts->resolution.coin_id)) return false;
    facts->resolution.has_coin = true;
    memcpy(facts->resolution.coin_id, coin->text, coin->length + 1);
  }
  if (!resolution_source(tj::Get(resolution, "source"), &facts->resolution.source)) return false;
  facts->resolution.ambiguous = is_true(tj::Get(resolution, "ambiguous"));
  const tj::Value* registry = tj::Get(stub, "registry");
  if (tj::IsObject(registry)) {
    facts->registry.found = true;
    facts->registry.symbol = tj::Get(registry, "symbol");
    facts->registry.name = tj::Get(registry, "name");
  }
  facts->market = member(stub, "market");
  facts->profile = member(stub, "profile");
  facts->liquidity = member(stub, "liquidity");
  facts->holders = member(stub, "holders");
  facts->chain_meta = member(stub, "chainMeta");
  facts->insight = member(stub, "insight");
  facts->contract = member(stub, "contract");
  facts->safety = member(stub, "safety");
  facts->errors = tj::Get(stub, "errors");
  static const struct { const char* name; tf::Facet facet; } kCache[] = {
      {"market", tf::kMarket}, {"profile", tf::kProfile}, {"liquidity", tf::kLiquidity},
      {"holders", tf::kHolders}, {"chainMeta", tf::kChainMeta}, {"insight", tf::kInsight},
      {"contract", tf::kContract}};
  const tj::Value* cache = tj::Get(stub, "cache");
  for (const auto& entry : kCache) {
    const tj::Value* p = tj::Get(cache, entry.name);
    if (!tj::IsObject(p)) return false;
    facts->cache[entry.facet] = provenance(p);
  }
  return true;
}

// The stub PricesFacet.spot / history and the calls TypeScript's facet made.
struct Stubs {
  const tj::Value* spot = nullptr;  // the `prices` map, or the string "throw"
  const char* history = "null";     // null | throw | empty
  const tj::Value* history_cache = nullptr;
  char spot_calls[8][160]{};
  size_t spot_count = 0;
  struct { char id[160]; int64_t from, to; bool hourly; } history_calls[8]{};
  size_t history_count = 0;
};

bool stub_spot(void* context, const char* price_id, uint64_t, tf::SpotQuote* quote) {
  Stubs& stubs = *static_cast<Stubs*>(context);
  if (stubs.spot_count < 8) snprintf(stubs.spot_calls[stubs.spot_count++], 160, "%s", price_id);
  // `prices[id]` of a thrown or missing read is no quote.
  if (!tj::IsObject(stubs.spot)) return false;
  const tj::Value* hit = tj::Get(stubs.spot, price_id);
  if (!tj::IsObject(hit)) return false;
  const tj::Value* price = tj::Get(hit, "price");
  if (!tj::IsNumber(price)) return false;
  *quote = tf::SpotQuote{};
  quote->price = price->number;
  const tj::Value* change = tj::Get(hit, "change24hPct");
  quote->has_change = tj::IsNumber(change);
  if (quote->has_change) quote->change = change->number;
  const tj::Value* as_of = tj::Get(hit, "asOf");
  quote->has_as_of = tj::IsString(as_of) && as_of->length < sizeof(quote->as_of);
  if (quote->has_as_of) memcpy(quote->as_of, as_of->text, as_of->length + 1);
  quote->cache = provenance(tj::Get(hit, "cache"));
  return true;
}

tf::SeriesStatus stub_series(void* context, tf::Work&, const char* price_id, int64_t from, int64_t to, bool hourly,
                             tf::SeriesRead* out) {
  Stubs& stubs = *static_cast<Stubs*>(context);
  if (stubs.history_count < 8) {
    auto& call = stubs.history_calls[stubs.history_count++];
    snprintf(call.id, sizeof(call.id), "%s", price_id);
    call.from = from;
    call.to = to;
    call.hourly = hourly;
  }
  if (strcmp(stubs.history, "throw") == 0) return tf::SeriesStatus::kUnavailable;
  if (strcmp(stubs.history, "empty") != 0) return tf::SeriesStatus::kNoSeries;
  *out = tf::SeriesRead{};
  out->cache = provenance(stubs.history_cache);
  return tf::SeriesStatus::kOk;
}

int64_t g_now_ms = 0;
fortis::testing::PointerClock g_clock(&g_now_ms);

bool encode(const tj::Value* document, bool protobuf, uint8_t* out, size_t capacity, size_t* used) {
  tj::OutBuffer buffer{out, capacity, 0, true};
  const bool ok = protobuf ? tj::AppendStructuredReadResponse(document, buffer) : tj::Stringify(document, buffer);
  *used = buffer.used;
  return ok && buffer.ok;
}

bool section_of(const tj::Value* name, tp::Section* out) {
  for (int s = 0; s < tp::kSectionCount; ++s)
    if (tj::IsString(name) && strcmp(tp::SectionName(static_cast<tp::Section>(s)), name->text) == 0) {
      *out = static_cast<tp::Section>(s);
      return true;
    }
  return false;
}

uint32_t facet_mask(const tj::Value* names) {
  static const struct { const char* name; tf::Facet facet; } kFacets[] = {
      {"market", tf::kMarket}, {"profile", tf::kProfile}, {"liquidity", tf::kLiquidity},
      {"holders", tf::kHolders}, {"chainMeta", tf::kChainMeta}, {"insight", tf::kInsight},
      {"contract", tf::kContract}};
  uint32_t mask = 0;
  for (uint32_t i = 0; names && i < names->count; ++i) {
    const tj::Value* name = at(names, i);
    bool found = false;
    for (const auto& entry : kFacets)
      if (tj::IsString(name) && strcmp(entry.name, name->text) == 0) { mask |= tf::FacetBit(entry.facet); found = true; }
    if (!found) return 0xffffffffu;
  }
  return mask;
}

constexpr size_t kBytes = 4 * 1024 * 1024;

void run_unit(const tj::Value* unit, uint8_t* out, char* hex) {
  const tj::Value* name = tj::Get(unit, "name");
  const tj::Value* facts_text = tj::Get(unit, "factsText");
  const tj::Value* spot_text = tj::Get(unit, "spotText");
  const tj::Value* response = tj::Get(unit, "responseText");
  const tj::Value* wire = tj::Get(unit, "wireHex");
  const tj::Value* options_value = tj::Get(unit, "options");
  char label[160];
  snprintf(label, sizeof(label), "unit %s", tj::IsString(name) ? name->text : "?");
  tj::Arena a = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* stub = tj::Parse(a, reinterpret_cast<const uint8_t*>(facts_text->text), facts_text->length);
  auto* facts = static_cast<tf::Facts*>(a.Alloc(sizeof(tf::Facts), alignof(tf::Facts)));
  const bool built = stub && facts && build_facts(a, unit, stub, facts);
  check(built, label, "stub facts");
  if (!built) { tj::FreeArena(a); return; }

  // The options as TypeScript passed them (defaults: 200 days, 1d).
  tp::PageOptions options{};
  const tj::Value* sections = tj::Get(options_value, "sections");
  for (uint32_t i = 0; sections && i < sections->count; ++i) {
    tp::Section section{};
    const bool known = section_of(at(sections, i), &section);
    check(known, label, "section name");
    if (known) options.sections.Add(section);
  }
  const tj::Value* days = tj::Get(options_value, "days");
  if (tj::IsNumber(days)) options.days = days->number;
  const tj::Value* interval = tj::Get(options_value, "interval");
  if (tj::IsString(interval) && strcmp(interval->text, "1h") == 0) options.interval = tp::SeriesInterval::k1h;
  const tj::Value* hint = tj::Get(options_value, "coingeckoIdHint");
  options.coingecko_hint = tj::IsString(hint) ? hint->text : nullptr;

  // The facts sub-facets TypeScript asked for (onlyCalls[0]) and the hint.
  const tj::Value* only_calls = tj::Get(unit, "onlyCalls");
  const bool comparison = options.sections.Has(tp::kComparison);
  check(only_calls && only_calls->count == (comparison ? 3 : 1) &&
            tp::FacetsForSections(options.sections) == facet_mask(at(only_calls, 0)) &&
            (!comparison || (facet_mask(at(only_calls, 1)) == tf::FacetBit(tf::kMarket) &&
                             facet_mask(at(only_calls, 2)) == tf::FacetBit(tf::kMarket))),
        label, "facts only set");
  const tj::Value* hint_call = at(tj::Get(unit, "hintCalls"), 0);
  check(tj::IsString(hint_call) ? hint && strcmp(hint_call->text, hint->text) == 0 : !hint, label, "hint passed");

  Stubs stubs;
  const tj::Value* spot = spot_text ? tj::Parse(a, reinterpret_cast<const uint8_t*>(spot_text->text), spot_text->length)
                                    : nullptr;
  stubs.spot = spot;
  const tj::Value* history = tj::Get(unit, "history");
  stubs.history = tj::IsString(history) ? history->text : "null";
  stubs.history_cache = tj::Get(unit, "historyCache");

  fortis::tokens::token_swr::Seams seams{};
  seams.clock = &g_clock;
  // An unopened store returns I/O errors. It lets the comparison branch
  // exercise the same failure shape as the oracle's throwing compare stub,
  // while the benchmark market reads still have a valid store object.
  fortis::storage::KvStore unavailable_store;
  fortis::tokens::token_data::TokenDataStore unavailable_data(&unavailable_store);
  NativeConfig unavailable_config{};
  TokenProviderOwner unavailable_owner(unavailable_config, nullptr);
  fortis::tokens::token_swr::Cache clock(comparison ? &unavailable_store : nullptr, &seams);
  tf::ProviderOwnerExecution execution(unavailable_owner);
  struct Readers final : tf::TokenSpotReader, tf::TokenSeriesReader {
    decltype(stubs)& state;
    explicit Readers(decltype(stubs)& stubs) : state(stubs) {}
    bool Quote(const char* id, uint64_t deadline, tf::SpotQuote* out) override {
      return stub_spot(&state, id, deadline, out);
    }
    tf::SeriesStatus Read(tf::Work& work, const char* id, int64_t from, int64_t to,
                         bool hourly, tf::SeriesRead* out) override {
      return stub_series(&state, work, id, from, to, hourly, out);
    }
  } readers(stubs);
  tf::Env env{};
  if (comparison) {
    env.kv = &unavailable_store;
    env.data = &unavailable_data;
    env.executor = &execution;
  }
  env.swr = &clock;
  env.spot = &readers;
  env.series = &readers;
  tf::Work work{};
  work.env = &env;
  work.arena = a;
  tj::Value* document = nullptr;
  const tf::FactsOutcome status = tp::ComposePage(work, *facts, options, &document);
  a = work.arena;
  check(status == tf::FactsOutcome::kOk && document, label, "composition");

  // PricesFacet calls: spot ids and the history window/interval.
  const tj::Value* spot_calls = tj::Get(unit, "spotCalls");
  bool spot_same = spot_calls && spot_calls->count == stubs.spot_count;
  for (uint32_t i = 0; spot_same && i < spot_calls->count; ++i) {
    const tj::Value* ids = at(spot_calls, i);
    spot_same = ids && ids->count == 1 && tj::IsString(at(ids, 0)) &&
                strcmp(at(ids, 0)->text, stubs.spot_calls[i]) == 0;
  }
  check(spot_same, label, "spot calls");
  const tj::Value* history_calls = tj::Get(unit, "historyCalls");
  bool history_same = history_calls && history_calls->count == stubs.history_count;
  for (uint32_t i = 0; history_same && i < history_calls->count; ++i) {
    const tj::Value* call = at(history_calls, i);
    const tj::Value* id = tj::Get(call, "id");
    const tj::Value* from = tj::Get(call, "from");
    const tj::Value* to = tj::Get(call, "to");
    const tj::Value* every = tj::Get(call, "interval");
    history_same = tj::IsString(id) && strcmp(id->text, stubs.history_calls[i].id) == 0 && tj::IsNumber(from) &&
                   static_cast<int64_t>(from->number) == stubs.history_calls[i].from && tj::IsNumber(to) &&
                   static_cast<int64_t>(to->number) == stubs.history_calls[i].to && tj::IsString(every) &&
                   (strcmp(every->text, "1h") == 0) == stubs.history_calls[i].hourly &&
                   strcmp(tj::Get(call, "fiat")->text, "usd") == 0;
  }
  check(history_same, label, "history calls");

  size_t used = 0;
  const bool json_ok = document && encode(document, false, out, kBytes, &used);
  const bool same_json = json_ok && tj::IsString(response) && used == response->length &&
                         memcmp(out, response->text, used) == 0;
  if (!same_json && json_ok) {
    size_t at = 0;
    while (at < used && at < response->length && out[at] == static_cast<uint8_t>(response->text[at])) ++at;
    char detail[400];
    snprintf(detail, sizeof(detail), "at %zu: expected …%.120s\n  actual …%.120s", at,
             response->text + (at > 60 ? at - 60 : 0), reinterpret_cast<const char*>(out) + (at > 60 ? at - 60 : 0));
    check(false, label, detail);
  } else {
    check(same_json, label, "JSON text");
  }
  if (json_ok && used > 1) {
    size_t short_used = 0;
    check(!encode(document, false, out, used - 1, &short_used), label, "JSON one byte short is refused");
  }
  const bool wire_ok = document && encode(document, true, out, kBytes, &used);
  if (wire_ok) to_hex(out, used, hex);
  check(wire_ok && tj::IsString(wire) && used * 2 == wire->length && memcmp(hex, wire->text, wire->length) == 0,
        label, "protobuf wire");
  const tj::Value* json_wire = tj::Get(unit, "jsonWireHex");
  check(tj::IsString(json_wire) && tj::IsString(wire) && strcmp(json_wire->text, wire->text) == 0, label,
        "TypeScript's live wire equals its JSON round trip");
  if (wire_ok && used > 1) {
    size_t short_used = 0;
    check(!encode(document, true, out, used - 1, &short_used), label, "protobuf one byte short is refused");
  }
  tj::FreeArena(a);
}

// fast-querystring's value for one key: absent, a string or an array.
tp::PageQueryValue query_value(const tj::Value* query, const char* key) {
  tp::PageQueryValue value{};
  const tj::Value* found = tj::Get(query, key);
  if (!found) return value;
  if (tj::IsString(found)) {
    value.text = found->text;
    value.length = found->length;
    value.count = 1;
  } else if (found->type == tj::Type::kArray) {
    value.count = found->count ? 1 : 0;
    const tj::Value* first = at(found, 0);
    if (tj::IsString(first)) { value.text = first->text; value.length = first->length; }
  }
  return value;
}

struct QueryCounts { int ok = 0, invalid = 0, unknown = 0, skipped = 0; };

int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// networkTokenParams over the target's {network} and {tokenId} segments as
// find-my-way decodes them: 1..64 and 1..160 UTF-16 units.
bool params_valid(const tj::Value* target_hex) {
  if (!tj::IsString(target_hex) || target_hex->length % 2) return false;
  char target[16384]{};
  const size_t length = target_hex->length / 2;
  if (length >= sizeof(target)) return false;
  for (size_t i = 0; i < length; ++i)
    target[i] = static_cast<char>(hex_digit(target_hex->text[2 * i]) * 16 + hex_digit(target_hex->text[2 * i + 1]));
  target[length] = '\0';
  const size_t path_length = strcspn(target, "?");
  // /v1/networks/{network}/token/{tokenId}/page
  size_t starts[8], ends[8], segments = 0;
  for (size_t i = 0; i < path_length && segments < 8;) {
    if (target[i] == '/') { ++i; starts[segments] = i; while (i < path_length && target[i] != '/') ++i;
      ends[segments++] = i; } else ++i;
  }
  if (segments != 6) return false;
  const size_t indices[2] = {2, 4};
  const size_t limits[2] = {64, 160};
  for (size_t k = 0; k < 2; ++k) {
    size_t units = 0;
    for (size_t i = starts[indices[k]]; i < ends[indices[k]]; ++i) {
      unsigned char c = static_cast<unsigned char>(target[i]);
      if (c == '%' && i + 2 < ends[indices[k]] + 1 && hex_digit(target[i + 1]) >= 0 && hex_digit(target[i + 2]) >= 0) {
        c = static_cast<unsigned char>(hex_digit(target[i + 1]) * 16 + hex_digit(target[i + 2]));
        i += 2;
      }
      if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
    }
    if (units < 1 || units > limits[k]) return false;
  }
  return true;
}

// One socket record whose hooks passed and whose params are valid: the
// native query parser must reach TypeScript's outcome.
void run_query(const tj::Value* record, QueryCounts* counts) {
  const tj::Value* name = tj::Get(record, "name");
  const tj::Value* status = tj::Get(record, "status");
  const tj::Value* text = tj::Get(record, "responseText");
  const tj::Value* query = tj::Get(record, "query");
  const tj::Value* method = tj::Get(record, "method");
  char label[200];
  snprintf(label, sizeof(label), "query %s", tj::IsString(name) ? name->text : "?");
  if (!tj::IsNumber(status) || !tj::IsString(method) || strcmp(method->text, "GET") != 0 || !tj::IsString(text)) {
    ++counts->skipped;
    return;
  }
  // Records answered before validation (hooks, the router, the 414) or
  // refused on the path, and the facet's RangeErrors on valid queries, carry
  // nothing for the query parser.
  tj::Arena a = tj::GrowingArena(1024 * 1024);
  const tj::Value* body = tj::Parse(a, reinterpret_cast<const uint8_t*>(text->text), text->length);
  const tj::Value* error = tj::Get(body, "error");
  const tj::Value* detail = tj::Get(body, "detail");
  const int code = static_cast<int>(status->number);
  const char* expected = nullptr;
  if (code == 200 && tj::IsObject(tj::Get(body, "identity"))) expected = "ok";
  else if (code == 400 && tj::IsString(error) && strcmp(error->text, "BAD_REQUEST") == 0 && !detail) expected = "invalid";
  else if (code == 400 && tj::IsString(detail) && strncmp(detail->text, "unknown include: ", 17) == 0) expected = "unknown";
  else if (code == 400 && tj::IsString(detail)) expected = "ok";  // the facet's RangeError: the query was valid
  if (tj::IsString(tj::Get(record, "strictExpected"))) expected = tj::Get(record, "strictExpected")->text;
  if (!expected) { ++counts->skipped; tj::FreeArena(a); return; }
  // A bare 400 also answers invalid params (validate() checks both): those
  // records carry no query outcome.
  if (!params_valid(tj::Get(record, "targetHex"))) { ++counts->skipped; tj::FreeArena(a); return; }
  tp::PageQueryValue values[4] = {query_value(query, "include"), query_value(query, "days"),
                                  query_value(query, "interval"), query_value(query, "coingeckoId")};
  size_t scratch_capacity = 16;
  for (const auto& v : values) scratch_capacity += v.length;
  char* scratch = static_cast<char*>(malloc(scratch_capacity));
  tp::PageOptions options{};
  char hint[128];
  bool has_hint = false;
  char unknown[tp::kUnknownIncludeBytes];
  size_t unknown_length = 0;
  const tp::PageQueryStatus parsed = tp::ParsePageQuery(values[0], values[1], values[2], values[3], scratch,
                                                        scratch_capacity, &options, hint, sizeof(hint), &has_hint,
                                                        unknown, sizeof(unknown), &unknown_length);
  free(scratch);
  if (strcmp(expected, "invalid") == 0) {
    // The no-auth and Origin refusals precede validation: their query is
    // whatever the case sent, so only a page or a validation 400 counts.
    check(parsed == tp::PageQueryStatus::kInvalid, label, "pageQuerySchema refuses");
    ++counts->invalid;
  } else if (strcmp(expected, "unknown") == 0) {
    check(parsed == tp::PageQueryStatus::kUnknownInclude && unknown_length == detail->length &&
              memcmp(unknown, detail->text, unknown_length) == 0,
          label, "unknown include detail");
    ++counts->unknown;
  } else {
    check(parsed == tp::PageQueryStatus::kOk, label, "query accepted");
    const tj::Value* included = tj::Get(body, "included");
    if (parsed == tp::PageQueryStatus::kOk && included) {
      tp::Sections with_identity = options.sections;
      with_identity.Add(tp::kIdentity);
      bool same = included->count == with_identity.count;
      for (uint32_t i = 0; same && i < included->count; ++i)
        same = tj::IsString(at(included, i)) &&
               strcmp(at(included, i)->text, tp::SectionName(with_identity.order[i])) == 0;
      check(same, label, "included sections");
    }
    ++counts->ok;
  }
  tj::FreeArena(a);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s token_page_edges.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* text = read_file(argv[1], &length);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena a = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(a, reinterpret_cast<const uint8_t*>(text), length);
  strict::PatchRows(a, tj::Get(fixture, "cases"), tj::Get(strict::UnitExpectations(a), "page"), "name");
  if (!fixture) {
    fprintf(stderr, "cannot parse %s\n", argv[1]);
    return 2;
  }
  const tj::Value* source = tj::Get(fixture, "source");
  int64_t now = 0;
  parse_iso(tj::Get(source, "fixtureTime"), &now);
  g_now_ms = now;
  uint8_t* out = static_cast<uint8_t*>(malloc(kBytes));
  char* hex = static_cast<char*>(malloc(2 * kBytes + 1));
  const tj::Value* units = tj::Get(fixture, "units");
  size_t unit_count = 0;
  for (uint32_t i = 0; units && i < units->count; ++i, ++unit_count) run_unit(at(units, i), out, hex);
  QueryCounts counts;
  const tj::Value* cases = tj::Get(fixture, "cases");
  for (uint32_t i = 0; cases && i < cases->count; ++i) {
    const tj::Value* record = at(cases, i);
    if (tj::Get(record, "state") || tj::Get(record, "concurrent")) continue;
    run_query(record, &counts);
  }
  free(out);
  free(hex);
  printf("token page units: %zu scenarios; queries: %d pages, %d schema refusals, %d unknown-include details, "
         "%d records skipped; %d checks, %d failures\n",
         unit_count, counts.ok, counts.invalid, counts.unknown, counts.skipped, g_checks, g_failures);
  tj::FreeArena(a);
  free(text);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
