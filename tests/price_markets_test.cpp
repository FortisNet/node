#include "test_support.h"
#include "strict_unit_expectations.h"
// Unit checks of POST /v1/prices/markets (R47) below the HTTP layer, against
// the TypeScript oracle price_markets_edges.json:
//  - every request record that reached the quota charges PriceMarketsCost
//    (TypeScript's x-rate-limit-cost), and every one that reached zod goes
//    through PriceMarketsIds: refused exactly where zod or protobuf-es
//    refused, else exactly the ids the facet normalized (`unit.ids`: trimmed,
//    lower-cased, de-duplicated, UTF-16 sorted);
//  - every single successful provider call's ids (the coins it stored)
//    give TypeScript's /coins/markets path through PriceMarketsBatchPath
//    (encodeURIComponent, the fixed suffix);
//  - every 200 answer re-renders through PriceMarketsRender from the
//    entries it was served from (this case's writes for live coins, the
//    group's latest stored entry for cached ones, TypeScript's pending list)
//    with finalize's provenance, to TypeScript's JSON text or protobuf wire
//    (a −0 read as 0, the L1 difference).
// Usage: price_markets_test price_markets_edges.json
#include "strict_proto_cases.h"
#include "price_markets_route.h"
#include "discovery_post_unit.h"

#include <map>

using namespace fortis::discovery;
using namespace fortis::http;

namespace {

using namespace post_unit;

struct Counts {
  int costs = 0, accepted = 0, refused = 0, paths = 0, json = 0, protobuf = 0, skipped = 0, named = 0,
      unlisted_units = 0, ascii_case = 0;
};

struct Stored { std::string market; int64_t as_of; int64_t stale_until; };

// The stored `{"market":<m>}` envelope value's market text.
std::string market_of(const std::string& d) {
  const std::string open = "{\"market\":";
  return d.compare(0, open.size(), open) == 0 ? d.substr(open.size(), d.size() - open.size() - 1) : "";
}

std::vector<std::string> strings(const tj::Value* array) {
  std::vector<std::string> out;
  for (uint32_t i = 0; tj::IsArray(array) && i < array->count; ++i) out.push_back(text(array->items[i]));
  return out;
}

void run(const tj::Value* fixture, DiscoveryWorkspace* workspace, Counts* counts) {
  std::map<std::string, Stored> stored;
  std::string group;
  static uint8_t scratch[1 << 20];
  static char arena[64 * 1024];
  static PriceMarketsId ids[kPriceMarketsMaxIds];
  const tj::Value* cases = tj::Get(fixture, "cases");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* record = cases->items[i];
    const std::string name = text(tj::Get(record, "name"));
    if (text(tj::Get(record, "group")) != group) {
      group = text(tj::Get(record, "group"));
      stored.clear();
    }
    std::string label = group;
    label += "/";
    label += name;
    const tj::Value* writes = tj::Get(record, "kvWrites");
    auto apply_writes = [&]() {
      for (uint32_t w = 0; tj::IsArray(writes) && w < writes->count; ++w) {
        const tj::Value* write = writes->items[w];
        stored[text(tj::Get(write, "key"))] = Stored{market_of(text(tj::Get(write, "d"))),
            static_cast<int64_t>(tj::Get(write, "asOf")->number),
            static_cast<int64_t>(tj::Get(write, "staleUntil")->number)};
      }
    };
    if (tj::Get(record, "state") || tj::Get(record, "concurrent") ||
        text(tj::Get(record, "target")) != "/v1/prices/markets" || text(tj::Get(record, "method")) != "POST" ||
        before_validation(record)) {
      apply_writes();
      ++counts->skipped;
      continue;
    }
    Body body;
    if (!make_body(record, &body)) {
      // json_reader_bounds (named beside the handler): the shared bounded
      // JSON reader refuses a lone surrogate escape and an overflowing number.
      const bool named = name == "lone-surrogate-id" || name == "number-overflow-cost";
      check(named, label, "the parser refused a body TypeScript validated");
      if (named) ++counts->named;
      apply_writes();
      continue;
    }
    const std::string cost = text(tj::Get(tj::Get(record, "headers"), "x-rate-limit-cost"));
    check(std::to_string(PriceMarketsCost(body.body)) == cost, label,
          "cost " + std::to_string(PriceMarketsCost(body.body)) + " != " + cost);
    ++counts->costs;
    size_t count = 0;
    const bool valid = PriceMarketsIds(body.body, scratch, sizeof(scratch), arena, sizeof(arena), ids, &count);
    if (strict::ProtoReject("markets", name.c_str())) {
      check(!valid, label, "strict protobuf request refusal");
      ++counts->refused;
      continue;
    }
    const int status = static_cast<int>(tj::Get(record, "status")->number);
    if (status != 200) {
      check(!valid, label, "zod refused: the native ids must be refused");
      ++counts->refused;
      apply_writes();
      continue;
    }
    ++counts->accepted;
    const tj::Value* unit_ids = tj::Get(tj::Get(record, "unit"), "ids");
    std::vector<std::string> got;
    for (size_t c = 0; valid && c < count; ++c) got.emplace_back(arena + ids[c].offset, ids[c].length);
    check(valid, label, "ids refused where zod accepted");
    // js-case: ids are ASCII lower-cased. Where TypeScript's
    // Unicode lower-casing gave other ids (İ -> i̇, Ä -> ä, ß-COIN -> ß-coin)
    // the recorded batch and answer are for those ids, so only the native
    // normalization is checked: no A-Z left, every id distinct.
    bool non_ascii = false;
    for (uint8_t b : body.bytes) non_ascii = non_ascii || b >= 0x80;
    if (tj::IsArray(unit_ids) && got != strings(unit_ids) && non_ascii) {
      bool lowered = true;
      for (size_t a = 0; a < got.size(); ++a) {
        for (char c : got[a]) lowered = lowered && !(c >= 'A' && c <= 'Z');
        for (size_t b = a + 1; b < got.size(); ++b) lowered = lowered && got[a] != got[b];
      }
      check(lowered, label, "ASCII lower-cased, distinct ids");
      ++counts->ascii_case;
      apply_writes();
      continue;
    }
    // The oracle computes `unit.ids` for lower-case JSON and protobuf bodies.
    if (tj::IsArray(unit_ids)) check(got == strings(unit_ids), label, "normalized ids");
    else ++counts->unlisted_units;
    // One successful call: its coins are exactly the ones it stored.
    const tj::Value* upstream = tj::Get(record, "upstream");
    std::vector<std::string> live;
    for (uint32_t w = 0; tj::IsArray(writes) && w < writes->count; ++w)
      if (!tj::Get(writes->items[w], "afterResponse"))
        live.push_back(text(tj::Get(writes->items[w], "key")).substr(8));
    if (tj::IsArray(upstream) && upstream->count == 1 && !live.empty() &&
        tj::Get(upstream->items[0], "status")->number == 200 && !tj::Get(upstream->items[0], "afterResponse") &&
        live.size() <= kPriceMarketsBatchIds) {
      std::vector<const char*> pointers;
      std::vector<size_t> lengths;
      for (const std::string& coin : got)
        for (const std::string& written : live)
          if (coin == written) { pointers.push_back(coin.c_str()); lengths.push_back(coin.size()); }
      char path[kDiscoveryMaxPathBytes];
      const size_t taken = PriceMarketsBatchPath(pointers.data(), lengths.data(), pointers.size(), path,
                                                 sizeof(path));
      const std::string url = text(tj::Get(upstream->items[0], "url"));
      const std::string want_path = url.substr(url.find("/api/v3/"));
      check(taken == pointers.size() && want_path == path, label, std::string("batch path ") + path);
      ++counts->paths;
    }
    // Re-render from the entries it was served from.
    std::vector<std::string> pending;
    {
      tj::Arena parse = tj::GrowingArena(64 << 20);
      const tj::Value* answer = nullptr;
      if (const tj::Value* decoded = tj::Get(record, "decoded")) answer = decoded;
      else {
        const std::string response = text(tj::Get(record, "responseText"));
        answer = tj::Parse(parse, reinterpret_cast<const uint8_t*>(response.data()), response.size());
      }
      pending = strings(tj::Get(answer, "pending"));
      tj::FreeArena(parse);
    }
    std::vector<std::string> markets;
    size_t hit = 0, fetched = 0;
    int64_t newest = -1;
    const int64_t at = static_cast<int64_t>(tj::Get(record, "at")->number);
    bool known = true;
    for (const std::string& coin : got) {
      bool is_pending = false;
      for (const std::string& p : pending) is_pending |= p == coin;
      if (is_pending) continue;
      std::string market;
      bool is_live = false;
      for (uint32_t w = 0; tj::IsArray(writes) && w < writes->count; ++w) {
        const tj::Value* write = writes->items[w];
        if (!tj::Get(write, "afterResponse") && text(tj::Get(write, "key")) == "mkt:usd:" + coin) {
          market = market_of(text(tj::Get(write, "d")));
          is_live = true;
        }
      }
      if (is_live) {
        ++fetched;
      } else {
        const auto entry = stored.find("mkt:usd:" + coin);
        if (entry == stored.end() || at >= entry->second.stale_until) { known = false; break; }
        market = entry->second.market;
        ++hit;
        if (entry->second.as_of > newest) newest = entry->second.as_of;
      }
      if (market != "null") markets.push_back(market);
    }
    check(known, label, "a served coin with no entry");
    if (known) {
      std::vector<const uint8_t*> texts;
      std::vector<size_t> lengths;
      for (const std::string& m : markets) {
        texts.push_back(reinterpret_cast<const uint8_t*>(m.data()));
        lengths.push_back(m.size());
      }
      std::vector<const char*> pending_ids;
      std::vector<size_t> pending_lengths;
      for (const std::string& p : pending) { pending_ids.push_back(p.c_str()); pending_lengths.push_back(p.size()); }
      PriceMarketsRenderInput input;
      input.markets = texts.data();
      input.market_lengths = lengths.data();
      input.market_count = texts.size();
      input.pending = pending_ids.data();
      input.pending_lengths = pending_lengths.data();
      input.pending_count = pending_ids.size();
      const size_t entries = hit + fetched;
      input.hit = entries > 0 && hit == entries;
      input.partial = hit > 0 && hit < entries;
      if (!pending.empty()) { input.hit = false; input.partial = true; }
      input.newest_ms = newest;
      const bool protobuf = tj::Get(record, "wireHex") != nullptr;
      HttpResponse rendered{};
      const bool ok = PriceMarketsRender(workspace, input, protobuf, &rendered);
      if (protobuf) {
        check(ok && to_hex(rendered.body, rendered.body_length) ==
                  zero_negative_zero(text(tj::Get(record, "wireHex"))), label, "protobuf wire");
        ++counts->protobuf;
      } else {
        const std::string response = text(tj::Get(record, "responseText"));
        check(ok && std::string(reinterpret_cast<const char*>(rendered.body), rendered.body_length) == response,
              label, "JSON text\n   expected " + response.substr(0, 300) + "\n   actual   " +
              std::string(reinterpret_cast<const char*>(rendered.body), rendered.body_length).substr(0, 300));
        ++counts->json;
      }
    }
    apply_writes();
  }
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s price_markets_edges.json\n", argv[0]);
    return 2;
  }
  const std::vector<uint8_t> data = read_file(argv[1]);
  tj::Arena arena = tj::GrowingArena(256u << 20);
  const tj::Value* fixture = data.empty() ? nullptr : tj::Parse(arena, data.data(), data.size());
  if (!strict::ContractExpectations(arena, fixture, "price_markets_edges.json")) return 2;
  if (!tj::IsArray(tj::Get(fixture, "cases"))) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  DiscoveryWorkspace* workspace = new_workspace();
  Counts counts;
  run(fixture, workspace, &counts);
  printf("price markets unit: %d costs, %d accepted id lists (%d without oracle ids), %d refusals, %d "
         "batch paths, %d JSON and %d protobuf answers re-rendered (%d records skipped, %d named "
         "json_reader_bounds); %d checks, %d failures\n",
         counts.costs, counts.accepted, counts.unlisted_units, counts.refused, counts.paths, counts.json,
         counts.protobuf, counts.skipped, counts.named, g_checks, g_failures);
  printf("price markets unit: %d records with non-ASCII ids checked as ASCII case (js-case)\n", counts.ascii_case);
  check(counts.ascii_case >= 3, "fixture", "the three non-ASCII id records (unicode, unicode-protobuf, id-lower-grows)");
  free(workspace);
  tj::FreeArena(arena);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
