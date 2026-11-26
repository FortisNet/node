#include "test_support.h"
#include "strict_unit_expectations.h"
// Unit checks of POST /v1/bittensor/analytics (R40) below the HTTP layer,
// against the TypeScript oracle bittensor_analytics_edges.json:
//  - every request record that reached zod goes through
//    PlanBittensorAnalytics (the protobuf-es 2.16 reader, zod and the
//    facet's allowlist and bounds): a bare 400 where zod or protobuf-es
//    refused, the facet's exact RangeError message, a prototype-member path
//    (an ordinary unsupported path; the oracle's 502), or a
//    fetch whose cache key is the `taostats:` key TypeScript read and whose
//    provider path is the URL TypeScript asked (path, URLSearchParams order
//    and escaping, finney);
//  - every provider 2xx TypeScript stored normalizes to the stored JSON
//    (JSON.parse → JSON.stringify) byte for byte;
//  - every 200 answer re-encodes from the entry it was served from (live,
//    or the group's latest stored entry with its asOf) to TypeScript's JSON
//    text or protobuf wire (a −0 read as 0, the L1 difference).
// Usage: bittensor_analytics_test bittensor_analytics_edges.json
#include "strict_proto_cases.h"
#include "bittensor_route.h"
#include "discovery_post_unit.h"

#include <map>

using namespace fortis::discovery;
using namespace fortis::http;

namespace {

using namespace post_unit;

struct Counts {
  int plans = 0, refusals = 0, details = 0, prototype = 0, fetches = 0, l1 = 0, paths = 0, normalized = 0,
      json = 0, protobuf = 0, skipped = 0, named = 0;
};

std::string path_of(const std::string& url) {
  const size_t start = url.find("/api/");
  return start == std::string::npos ? url : url.substr(start);
}

struct Stored { std::string d; int64_t as_of; };

void run(const tj::Value* fixture, DiscoveryWorkspace* workspace, Counts* counts) {
  std::map<std::string, Stored> stored;
  std::string group;
  static uint8_t arena[1 << 20];
  static BittensorAnalyticsPlan plan;
  const tj::Value* cases = tj::Get(fixture, "cases");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* record = cases->items[i];
    const std::string name = text(tj::Get(record, "name"));
    if (text(tj::Get(record, "group")) != group) {
      group = text(tj::Get(record, "group"));
      stored.clear();  // a fresh process, a fresh in-memory Kv
    }
    std::string label = group;
    label += "/";
    label += name;
    const tj::Value* writes = tj::Get(record, "kvWrites");
    auto apply_writes = [&]() {
      for (uint32_t w = 0; tj::IsArray(writes) && w < writes->count; ++w)
        stored[text(tj::Get(writes->items[w], "key"))] =
            Stored{text(tj::Get(writes->items[w], "d")),
                   static_cast<int64_t>(tj::Get(writes->items[w], "asOf")->number)};
    };
    if (tj::Get(record, "concurrent") || text(tj::Get(record, "target")) != "/v1/bittensor/analytics" ||
        text(tj::Get(record, "method")) != "POST" || before_validation(record)) {
      apply_writes();
      ++counts->skipped;
      continue;
    }
    Body body;
    if (!make_body(record, &body)) {
      // json_reader_bounds (named beside the handler): the shared bounded
      // JSON reader refuses a lone surrogate escape and an overflowing number.
      const bool named = name == "zod-number-overflow" || name == "value-lone-surrogate";
      check(named, label, "the parser refused a body TypeScript validated");
      if (named) ++counts->named;
      apply_writes();
      continue;
    }
    PlanBittensorAnalytics(body.body, group != "unconfigured", arena, sizeof(arena), &plan);
    ++counts->plans;
    if (strict::ProtoReject("bittensor", name.c_str())) {
      check(plan.verdict == BittensorPlanVerdict::kBadRequest && !plan.detail, label,
            "strict protobuf request refusal");
      ++counts->refusals;
      continue;
    }
    if (name == "pb-path-bom" || name == "pb-key-bom") {
      const char* detail = name == "pb-path-bom" ? "unsupported bittensor analytics path" : "unsupported bittensor query";
      check(plan.verdict == BittensorPlanVerdict::kBadRequest && plan.detail && !strcmp(plan.detail, detail),
            label, "BOM remains string content for path/key validation");
      ++counts->details;
      continue;
    }
    if (name == "pb-query-unknown-entry-field") {
      check(plan.verdict == BittensorPlanVerdict::kFetch &&
          !strcmp(plan.provider_path, "/api/account/latest/v1?address=5FHneW46xGXgs5mUiveU4sbTyGBzmstUspZC92UhjJM694ty&network=finney"),
          label, "valid unknown map-entry field is skipped");
      ++counts->paths;
      continue;
    }
    if (name == "pb-value-bom") {
      check(plan.verdict == BittensorPlanVerdict::kFetch &&
          !strcmp(plan.provider_path, "/api/subnet/owner/v1?owner=%EF%BB%BFpb-bom-value"),
          label, "provider query retains BOM as UTF-8 bytes");
      ++counts->paths;
      continue;
    }
    if (name == "pb-query-proto-key") {
      // compat-decisions.md#js-prototype-keys: a `__proto__` map
      // entry is an ordinary query key, which no path allows (the oracle
      // dropped the entry and answered 200).
      check(plan.verdict == BittensorPlanVerdict::kBadRequest && plan.detail &&
                strcmp(plan.detail, "unsupported bittensor query") == 0,
            label, "a __proto__ query key is an unsupported query");
      ++counts->prototype;
      continue;
    }
    const int status = static_cast<int>(tj::Get(record, "status")->number);
    const std::string response = text(tj::Get(record, "responseText"));
    const tj::Value* reads = tj::Get(record, "kvReads");
    const tj::Value* upstream = tj::Get(record, "upstream");
    // A 200 served from TypeScript's in-process L1 reads no KV: its key is
    // one this group stored before.
    const bool l1 = status == 200 && !(tj::IsArray(reads) && reads->count) &&
        !(tj::IsArray(upstream) && upstream->count);
    const bool reached_cache = (tj::IsArray(reads) && reads->count > 0) || l1;
    if (!reached_cache) {
      if (status == 400 && response == "{\"error\":\"BAD_REQUEST\"}") {
        check(plan.verdict == BittensorPlanVerdict::kBadRequest && !plan.detail, label, "the bare 400");
        ++counts->refusals;
      } else if (status == 400) {
        tj::Arena scratch = tj::GrowingArena(1 << 20);
        const tj::Value* parsed = tj::Parse(scratch, reinterpret_cast<const uint8_t*>(response.data()),
                                            response.size());
        const std::string detail = text(tj::Get(parsed, "detail"));
        check(plan.verdict == BittensorPlanVerdict::kBadRequest && plan.detail && detail == plan.detail, label,
              "detail " + detail + " != " + (plan.detail ? plan.detail : "(none)"));
        tj::FreeArena(scratch);
        ++counts->details;
      } else if (status == 502) {
        // compat-decisions.md#js-prototype-keys: an Object.prototype
        // member name is an ordinary unsupported path, not the oracle's 502.
        check(plan.verdict == BittensorPlanVerdict::kBadRequest && plan.detail &&
                  strcmp(plan.detail, "unsupported bittensor analytics path") == 0,
              label, "a prototype-member path is an unsupported path");
        ++counts->prototype;
      } else {
        check(false, label, "an answer without a cache read: status " + std::to_string(status));
      }
      apply_writes();
      continue;
    }
    // The facet reached SWR: the key and, on a provider request, the path.
    ++counts->fetches;
    std::string key;
    if (l1) {
      key = plan.cache_key;
      check(plan.verdict == BittensorPlanVerdict::kFetch && stored.count(key), label,
            "an L1 answer whose key " + key + " this group never stored");
      ++counts->l1;
    } else {
      key = text(reads->items[0]);
      check(plan.verdict == BittensorPlanVerdict::kFetch && key == plan.cache_key, label,
            "cache key " + std::string(plan.verdict == BittensorPlanVerdict::kFetch ? plan.cache_key : "(none)") +
            " != " + key);
    }
    bool live = false;
    for (uint32_t u = 0; tj::IsArray(upstream) && u < upstream->count; ++u) {
      const tj::Value* call = upstream->items[u];
      if (u == 0) {
        const std::string want = path_of(text(tj::Get(call, "url")));
        check(want == plan.provider_path && plan.provider_path_length == want.size(), label,
              "provider path " + std::string(plan.provider_path) + " != " + want);
        ++counts->paths;
      }
      if (!tj::Get(call, "afterResponse")) live = true;
    }
    // A provider 2xx TypeScript stored: the normalized payload.
    for (uint32_t w = 0; tj::IsArray(writes) && w < writes->count; ++w) {
      const tj::Value* write = writes->items[w];
      const tj::Value* call = upstream->items[upstream->count - 1];
      const std::string reply = text(tj::Get(call, "body"));
      size_t length = 0;
      const bool ok = BittensorNormalizePayload(workspace, reinterpret_cast<const unsigned char*>(reply.data()),
                                                reply.size(), &length);
      const std::string want = text(tj::Get(write, "d"));
      check(ok && std::string(reinterpret_cast<const char*>(workspace->payload), length) == want, label,
            "normalized payload");
      ++counts->normalized;
    }
    if (status == 200) {
      // The entry it was served from: this case's write when TypeScript
      // answered live, else the latest stored entry.
      std::string payload;
      int64_t as_of = -1;
      bool hit = false;
      bool found = false;
      if (live && tj::IsArray(writes) && writes->count) {
        for (uint32_t w = 0; w < writes->count; ++w)
          if (!tj::Get(writes->items[w], "afterResponse")) { payload = text(tj::Get(writes->items[w], "d")); found = true; }
      }
      if (!found) {
        const auto entry = stored.find(key);
        found = entry != stored.end();
        if (found) { payload = entry->second.d; as_of = entry->second.as_of; hit = true; }
      }
      check(found, label, "no entry to serve from");
      if (found) {
        HttpResponse encoded{};
        const bool protobuf = tj::Get(record, "wireHex") != nullptr;
        const bool ok = BittensorEncodeResponse(workspace, reinterpret_cast<const uint8_t*>(payload.data()),
                                                payload.size(), hit, as_of, protobuf, &encoded);
        if (protobuf) {
          const std::string want = zero_negative_zero(text(tj::Get(record, "wireHex")));
          check(ok && to_hex(encoded.body, encoded.body_length) == want, label, "protobuf wire");
          ++counts->protobuf;
        } else {
          check(ok && std::string(reinterpret_cast<const char*>(encoded.body), encoded.body_length) == response,
                label, "JSON text");
          ++counts->json;
        }
      }
    }
    apply_writes();
  }
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s bittensor_analytics_edges.json\n", argv[0]);
    return 2;
  }
  const std::vector<uint8_t> data = read_file(argv[1]);
  tj::Arena arena = tj::GrowingArena(256u << 20);
  const tj::Value* fixture = data.empty() ? nullptr : tj::Parse(arena, data.data(), data.size());
  if (!strict::ContractExpectations(arena, fixture, "bittensor_analytics_edges.json")) return 2;
  if (!tj::IsArray(tj::Get(fixture, "cases"))) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  DiscoveryWorkspace* workspace = new_workspace();
  Counts counts;
  run(fixture, workspace, &counts);
  printf("bittensor analytics unit: %d plans (%d bare refusals, %d facet details, %d prototype paths, "
         "%d cache reads of which %d L1, %d provider paths), %d normalized payloads, %d JSON and %d protobuf answers "
         "re-encoded (%d records skipped, %d named json_reader_bounds); %d checks, %d failures\n",
         counts.plans, counts.refusals, counts.details, counts.prototype, counts.fetches, counts.l1, counts.paths,
         counts.normalized, counts.json, counts.protobuf, counts.skipped, counts.named, g_checks, g_failures);
  free(workspace);
  tj::FreeArena(arena);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
