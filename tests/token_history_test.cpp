#include "test_support.h"
// Unit checks of the token history/liquidity read models (R20–R23) below the
// HTTP layer, against the TypeScript oracle token_history_edges.json:
//  - R20/R21/R23: every read record carrying `unit` is rebuilt in-process.
//    The fixture's `seed` records of the same durable store (restart-b reads
//    restart-a's) that touch the unit's token, plus every rollup, go through
//    the production writers (token_history::RecordCount/RecordDistribution/
//    RecordSuccess/RecordNotFound/RecordDiagnostic/RollupLiquidityCandles) at
//    their own instants into a fresh KV per token; the document is then built
//    at the record's instant (HolderHistoryDocument, LiquidityHistoryDocument,
//    LiquiditySummaryDocument) and must give TypeScript's JSON text or
//    StructuredReadResponse wire byte for byte, and its weak ETag
//    (sha256 of the JSON text / protobuf wire of the validator payload:
//    liquidityHistoryValidator for R21, the document itself otherwise; a 304
//    record checks the ETag only);
//  - R22: every live DexScreener answer of a 200 pools record goes through
//    DexScreenerPools + CalculateLiquiditySnapshot(all pools) and must give
//    the record's `liquidity` member byte for byte;
//  - one byte short of a document's encoding is refused, never truncated;
//  - `--measure`: the arena the largest bounded documents need (a full
//    90-day hourly chart; 400 days of counts from three sources with 300
//    ranked holders on every comparison day), against kHistoryArenaBytes.
// Usage: token_history_test token_history_edges.json [--measure]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "fake_seams.h"
#include "kv_store.h"
#include "token_data.h"
#include "token_facts.h"
#include "token_history.h"
#include "token_json.h"
#include "token_liquidity.h"
#include "token_swr.h"

namespace {

namespace tj = fortis::codec::tj;
namespace th = fortis::tokens::token_history;
namespace td = fortis::tokens::token_data;
namespace tf = fortis::tokens::token_facts;

int g_failures = 0;
int g_checks = 0;
int64_t g_now = 0;
constexpr size_t kHistoryArenaBytes = 16 * 1024 * 1024;  // token_service_internal.h (a growing arena)

void check(bool condition, const char* what, const char* detail = "") {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s %s\n", what, detail);
}

fortis::testing::PointerClock g_clock(&g_now);

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

// W/"<namespace>[-protobuf]-<24 hex of sha256(bytes)>".
void weak_etag(const char* name_space, bool protobuf, const uint8_t* bytes, size_t length, char* out, size_t capacity) {
  unsigned char digest[32];
  mbedtls_sha256(bytes, length, digest, 0);
  char hex[65];
  to_hex(digest, 32, hex);
  hex[24] = '\0';
  snprintf(out, capacity, "W/\"%s%s-%s\"", name_space, protobuf ? "-protobuf" : "", hex);
}

bool same_string(const tj::Value* v, const char* text) {
  return tj::IsString(v) && strlen(text) == v->length && memcmp(v->text, text, v->length) == 0;
}

const char* text_of(tj::Arena& a, const tj::Value* v) { return tj::IsString(v) ? tj::CString(a, v) : nullptr; }

// One durable store per (fixture KV, token): the writers' view of that token.
struct Store {
  char kv_name[32]{};
  char network[64]{};
  char token[600]{};
  char directory[256]{};
  fortis::storage::KvStore kv;
  td::TokenDataStore* data = nullptr;
  fortis::tokens::token_swr::Seams seams{};
  fortis::tokens::token_swr::Cache* swr = nullptr;
  tf::Env env{};
  size_t next = 0;  // the next fixture record to consider
};

// bytes == 0: the route's growing arena (make_history_work); else a fixed one
// (the writers' task arenas).
struct WorkBox {
  tf::Work work{};
  bool growing;
  explicit WorkBox(tf::Env* env, size_t bytes = 0) : growing(bytes == 0) {
    work.env = env;
    work.arena = growing ? tj::GrowingArena(kHistoryArenaBytes)
                         : tj::Arena{static_cast<uint8_t*>(malloc(bytes)), bytes, 0, true};
    work.scratch = static_cast<uint8_t*>(malloc(1024 * 1024 + 64));
    work.scratch_capacity = 1024 * 1024 + 64;
  }
  ~WorkBox() {
    if (growing) tj::FreeArena(work.arena);
    else free(work.arena.base);
    free(work.scratch);
  }
};

bool open_store(Store* store, const char* kv_name, const char* network, const char* token, int serial) {
  snprintf(store->kv_name, sizeof(store->kv_name), "%s", kv_name);
  snprintf(store->network, sizeof(store->network), "%s", network);
  snprintf(store->token, sizeof(store->token), "%s", token);
  snprintf(store->directory, sizeof(store->directory), "/tmp/fortis-r20-unit-%d-%d", static_cast<int>(getpid()),
           serial);
  mkdir(store->directory, 0700);
  if (!store->kv.Open(store->directory)) return false;
  store->data = new td::TokenDataStore(&store->kv);
  store->seams.clock = &g_clock;
  store->swr = new fortis::tokens::token_swr::Cache(&store->kv, &store->seams);
  store->env.kv = &store->kv;
  store->env.data = store->data;
  store->env.swr = store->swr;
  return store->data->valid();
}

void close_store(Store* store) {
  delete store->swr;
  delete store->data;
  store->kv.Close();
  fortis::test::RemoveTree(store->directory);
}

// The probe's operations (POST /probe/test-token-history-seed in main.cpp).
bool apply(tf::Env* env, tj::Arena& a, const tj::Value* op) {
  WorkBox box(env, 32 * 1024 * 1024);
  tf::Work& work = box.work;
  const char* kind = text_of(a, tj::Get(op, "op"));
  const char* network = text_of(a, tj::Get(op, "network"));
  const char* token = text_of(a, tj::Get(op, "tokenId"));
  th::ObservationContext context{};
  const tj::Value* observed = tj::Get(op, "observedAt");
  context.has_observed_at = tj::IsNumber(observed);
  context.observed_at = context.has_observed_at ? static_cast<int64_t>(observed->number) : 0;
  const tj::Value* source = tj::Get(op, "source");
  context.source = tj::IsString(source) ? tj::CString(a, source) : nullptr;
  const tj::Value* sampling = tj::Get(op, "samplingIntervalSeconds");
  context.has_sampling = tj::IsNumber(sampling);
  context.sampling_seconds = context.has_sampling ? sampling->number : 0;
  if (!kind) return false;
  if (!strcmp(kind, "count")) {
    const tj::Value* count = tj::Get(op, "count");
    const tj::Value* at_least = tj::Get(op, "atLeast");
    return th::RecordCount(work, network, token, tj::IsNumber(count) ? count->number : -1,
                           at_least && at_least->type == tj::Type::kTrue, source, context.observed_at);
  }
  if (!strcmp(kind, "distribution")) {
    tj::Value* contract = tj::Obj(work.arena);
    bool ok = contract && tj::Set(work.arena, contract, "source", tj::Clone(work.arena, source)) &&
              tj::Set(work.arena, contract, "topHolders", tj::Clone(work.arena, tj::Get(op, "holders")));
    static const char* const kSummary[] = {"topHoldersPercent", "topHoldersPercentExcludingLocked", "burnedPercent"};
    for (const char* key : kSummary) ok = ok && tj::Set(work.arena, contract, key, tj::Clone(work.arena, tj::Get(op, key)));
    return ok && th::RecordDistribution(work, network, token, contract, context.observed_at);
  }
  if (!strcmp(kind, "success")) return th::RecordSuccess(work, network, token, tj::Get(op, "snapshot"), context);
  if (!strcmp(kind, "notFound")) return th::RecordNotFound(work, network, token, context);
  if (!strcmp(kind, "diagnostic")) {
    const tj::Value* checked = tj::Get(op, "checkedAt");
    return th::RecordDiagnostic(work, network, token, text_of(a, tj::Get(op, "status")),
                                tj::IsNumber(checked) ? static_cast<int64_t>(checked->number) : 0, context.source);
  }
  if (!strcmp(kind, "rollup")) {
    const tj::Value* now = tj::Get(op, "now");
    th::RollupLiquidityCandles(work, tj::IsNumber(now) ? static_cast<int64_t>(now->number) : g_now);
    return true;
  }
  return false;
}

bool touches(const tj::Value* op, const Store& store) {
  if (same_string(tj::Get(op, "op"), "rollup")) return true;
  return same_string(tj::Get(op, "network"), store.network) && same_string(tj::Get(op, "tokenId"), store.token);
}

const char* kv_of(const tj::Value* groups, const tj::Value* group) {
  for (uint32_t i = 0; tj::IsArray(groups) && i < groups->count; ++i) {
    const tj::Value* entry = groups->items[i];
    const tj::Value* name = tj::Get(entry, "name");
    if (tj::IsString(name) && tj::IsString(group) && name->length == group->length &&
        memcmp(name->text, group->text, name->length) == 0) {
      const tj::Value* kv = tj::Get(entry, "kv");
      return tj::IsString(kv) ? kv->text : nullptr;
    }
  }
  return nullptr;
}

bool hex_equals(const uint8_t* bytes, size_t length, const tj::Value* hex) {
  if (!tj::IsString(hex) || hex->length != length * 2) return false;
  char* out = static_cast<char*>(malloc(length * 2 + 1));
  to_hex(bytes, length, out);
  const bool same = memcmp(out, hex->text, hex->length) == 0;
  free(out);
  return same;
}

// The record's document rebuilt; JSON or wire plus ETag compared.
void check_unit(const tj::Value* record, Store* store, int* units) {
  tj::Arena& scratch_arena = *new tj::Arena(tj::GrowingArena(64 * 1024 * 1024));
  const tj::Value* unit = tj::Get(record, "unit");
  const char* route = text_of(scratch_arena, tj::Get(unit, "route"));
  const char* name = text_of(scratch_arena, tj::Get(record, "name"));
  g_now = static_cast<int64_t>(tj::Get(record, "at")->number);
  WorkBox box(&store->env);
  tf::Work& work = box.work;
  tj::Value* document = nullptr;
  tj::Value* validator = nullptr;
  const char* name_space = nullptr;
  bool built = false;
  if (!strcmp(route, "holders")) {
    name_space = "token-holder-history";
    const tj::Value* days = tj::Get(unit, "days");
    const char* source = text_of(scratch_arena, tj::Get(unit, "source"));
    built = th::HolderHistoryDocument(work, store->network, store->token, tj::IsNumber(days) ? days->number : 30,
                                      source, g_now, &document);
  } else if (!strcmp(route, "history")) {
    name_space = "token-liquidity-history";
    const char* range = text_of(scratch_arena, tj::Get(unit, "range"));
    const char* interval = text_of(scratch_arena, tj::Get(unit, "interval"));
    th::Range r = th::Range::k30d;
    th::Interval i = th::Interval::k1h;
    if (range) th::ParseRange(range, strlen(range), &r);
    if (interval) th::ParseInterval(interval, strlen(interval), &i);
    built = th::LiquidityHistoryDocument(work, store->network, store->token, r, i, g_now, &document, &validator);
  } else {
    name_space = "token-liquidity-summary";
    built = th::LiquiditySummaryDocument(work, store->network, store->token, g_now, &document);
  }
  check(built && document, name, "document not built");
  if (!built || !document) {
    tj::FreeArena(scratch_arena);
    delete &scratch_arena;
    return;
  }
  ++*units;
  const tj::Value* status = tj::Get(record, "status");
  const tj::Value* wire = tj::Get(record, "wireHex");
  const bool protobuf = tj::IsString(wire) ||
                        (tj::IsString(tj::Get(tj::Get(record, "headers"), "content-type")) &&
                         !strncmp(tj::Get(tj::Get(record, "headers"), "content-type")->text, "application/protobuf", 20));
  const size_t capacity = 2 * 1024 * 1024;
  uint8_t* body = static_cast<uint8_t*>(malloc(capacity));
  uint8_t* stable = static_cast<uint8_t*>(malloc(capacity));
  tj::OutBuffer out{body, capacity, 0, true};
  tj::OutBuffer stable_out{stable, capacity, 0, true};
  const tj::Value* stable_doc = validator ? validator : document;
  const bool encoded = protobuf ? tj::AppendStructuredReadResponse(document, out) && out.ok &&
                                      tj::AppendStructuredReadResponse(stable_doc, stable_out) && stable_out.ok
                                : tj::Stringify(document, out) && out.ok && tj::Stringify(stable_doc, stable_out) &&
                                      stable_out.ok;
  check(encoded, name, "encoding failed");
  char etag[128];
  weak_etag(name_space, protobuf, stable, stable_out.used, etag, sizeof(etag));
  const tj::Value* want_etag = tj::Get(tj::Get(record, "headers"), "etag");
  check(same_string(want_etag, etag), name, etag);
  if (tj::IsNumber(status) && status->number == 200) {
    if (protobuf) {
      check(hex_equals(body, out.used, wire), name, "wire differs");
    } else {
      const tj::Value* text = tj::Get(record, "responseText");
      const bool same = tj::IsString(text) && text->length == out.used && memcmp(text->text, body, out.used) == 0;
      if (!same && tj::IsString(text)) {
        size_t at = 0;
        while (at < out.used && at < text->length && body[at] == static_cast<uint8_t>(text->text[at])) ++at;
        char detail[400];
        snprintf(detail, sizeof(detail), "JSON differs at %zu: expected …%.120s actual …%.120s", at,
                 text->text + (at > 40 ? at - 40 : 0), reinterpret_cast<const char*>(body) + (at > 40 ? at - 40 : 0));
        check(false, name, detail);
      } else {
        check(same, name, "JSON differs");
      }
    }
    // One byte short is refused, never truncated.
    tj::OutBuffer short_out{body, out.used - 1, 0, true};
    const bool fits = protobuf ? tj::AppendStructuredReadResponse(document, short_out) && short_out.ok
                               : tj::Stringify(document, short_out) && short_out.ok;
    check(!fits, name, "a buffer one byte short accepted the document");
  }
  free(body);
  free(stable);
  tj::FreeArena(scratch_arena);
  delete &scratch_arena;
}

const char* slug_of(const tj::Value* network) {
  if (same_string(network, "eth-mainnet")) return "ethereum";
  if (same_string(network, "bnb")) return "bsc";
  if (same_string(network, "solana")) return "solana";
  return nullptr;
}

// R22: the live DexScreener body → the record's liquidity member.
void check_pools(tj::Arena& a, const tj::Value* record, int* units) {
  const tj::Value* text = tj::Get(record, "responseText");
  const tj::Value* calls = tj::Get(record, "upstream");
  if (!tj::IsString(text) || !tj::IsArray(calls) || calls->count != 1) return;
  const tj::Value* call = calls->items[0];
  const tj::Value* status = tj::Get(call, "status");
  if (!tj::IsNumber(status) || status->number != 200) return;
  const tj::Value* response = tj::Parse(a, reinterpret_cast<const uint8_t*>(text->text), text->length);
  const tj::Value* liquidity = tj::Get(response, "liquidity");
  const tj::Value* token = tj::Get(response, "tokenId");
  const char* slug = slug_of(tj::Get(response, "network"));
  const tj::Value* body_text = tj::Get(call, "body");
  if (!response || !tj::IsString(token) || !slug || !tj::IsString(body_text)) return;
  const char* colon = static_cast<const char*>(memchr(token->text, ':', token->length));
  if (!colon) return;
  const char* name = text_of(a, tj::Get(record, "name"));
  const tj::Value* body = tj::Parse(a, reinterpret_cast<const uint8_t*>(body_text->text), body_text->length);
  const char* address = tj::CString(a, tj::Str(a, colon + 1, token->length - (colon + 1 - token->text)));
  tj::Value* pools = nullptr;
  tj::Value* detailed = nullptr;
  const auto parsed = body ? fortis::tokens::token_liquidity::DexScreenerPools(a, body, address, slug, &pools)
                           : fortis::tokens::token_liquidity::PoolsStatus::kFailed;
  if (parsed != fortis::tokens::token_liquidity::PoolsStatus::kOk) {
    check(!liquidity || liquidity->type == tj::Type::kNull, name, "TypeScript had pools the native parser refused");
    return;
  }
  check(fortis::tokens::token_liquidity::CalculateLiquiditySnapshot(a, pools, address, fortis::tokens::token_liquidity::kAllPools,
                                                            &detailed),
        name, "snapshot failed");
  uint8_t want[1 << 16], got[1 << 16];
  tj::OutBuffer want_out{want, sizeof(want), 0, true};
  tj::OutBuffer got_out{got, sizeof(got), 0, true};
  const bool ok = liquidity && tj::Stringify(liquidity, want_out) && want_out.ok &&
                  tj::Stringify(detailed ? detailed : tj::Null(a), got_out) && got_out.ok;
  const bool same = ok && want_out.used == got_out.used && memcmp(want, got, got_out.used) == 0;
  if (!same) {
    size_t at = 0;
    while (at < want_out.used && at < got_out.used && want[at] == got[at]) ++at;
    char detail[400];
    snprintf(detail, sizeof(detail), "pools snapshot differs at %zu: expected …%.100s actual …%.100s", at,
             reinterpret_cast<const char*>(want) + (at > 30 ? at - 30 : 0),
             reinterpret_cast<const char*>(got) + (at > 30 ? at - 30 : 0));
    check(false, name, detail);
  } else {
    check(true, name);
  }
  ++*units;
}

// The largest documents the bounds admit, against kHistoryArenaBytes.
void measure() {
  Store store;
  check(open_store(&store, "measure", "eth-mainnet", "erc20:0x00000000000000000000000000000000000000aa", 9999),
        "measure", "store");
  const int64_t now = 1770724923456LL;
  g_now = now;
  // 92 days of hourly candles, a fresh latest observation.
  {
    constexpr size_t kCount = 92 * 24;
    auto* candles = static_cast<td::Candle*>(calloc(kCount, sizeof(td::Candle)));
    for (size_t i = 0; i < kCount; ++i) {
      td::Candle& c = candles[i];
      c.ts = (now / 3600000 - static_cast<int64_t>(kCount - i)) * 3600000;
      for (size_t f = 0; f < td::Candle::kFields; ++f) { c.has[f] = true; c.value[f] = 1000.0 + i + f / 7.0; }
      c.sample_count = 12;
      c.expected_samples = 12;
      snprintf(c.source, sizeof(c.source), "dexscreener");
    }
    size_t changed = 0;
    check(store.data->PutCandles(store.network, store.token, td::Interval::k1h, candles, kCount, now, &changed),
          "measure", "candles");
    free(candles);
    tj::Arena a = tj::GrowingArena(1 << 20);
    const tj::Value* snapshot = tj::Parse(a, reinterpret_cast<const uint8_t*>("{\"totalUsd\":5,\"weightedPriceUsd\":1}"), 35);
    WorkBox writer(&store.env, 32 * 1024 * 1024);
    th::ObservationContext context{};
    context.has_observed_at = true;
    context.observed_at = now - 60000;
    check(th::RecordSuccess(writer.work, store.network, store.token, snapshot, context), "measure", "observation");
    tj::FreeArena(a);
    WorkBox box(&store.env);
    tj::Value* document = nullptr;
    tj::Value* validator = nullptr;
    check(th::LiquidityHistoryDocument(box.work, store.network, store.token, th::Range::k90d, th::Interval::k1h, now,
                                       &document, &validator),
          "measure", "90d/1h document");
    uint8_t* text = static_cast<uint8_t*>(malloc(2 * 1024 * 1024));
    tj::OutBuffer json{text, 2 * 1024 * 1024, 0, true};
    tj::OutBuffer wire{text, 2 * 1024 * 1024, 0, true};
    const bool fits = document && tj::Stringify(document, json) && json.ok;
    const bool wire_fits = document && tj::AppendStructuredReadResponse(document, wire) && wire.ok;
    printf("measure: 90d/1h liquidity history (%u candles): %zu bytes of arena held (bound %zu); JSON %zu, "
           "protobuf %zu bytes (response bound 2 MiB)\n",
           document ? tj::Get(document, "series")->count : 0, box.work.arena.held, kHistoryArenaBytes, json.used,
           wire.used);
    free(text);
    check(fits && wire_fits, "measure", "the 90d/1h document exceeds the 2 MiB response");
    check(box.work.arena.held < kHistoryArenaBytes * 3 / 4, "measure", "90d/1h needs over 3/4 of the arena");
  }
  // 400 days of counts from three sources; 300 ranked holders on the latest
  // day and on every comparison baseline.
  {
    const char* token = "erc20:0x00000000000000000000000000000000000000bb";
    tj::Arena a = tj::GrowingArena(64 * 1024 * 1024);
    WorkBox writer(&store.env, 32 * 1024 * 1024);
    const int64_t today = now / 86400000 * 86400000;
    static const char* const kSources[] = {"ethplorer", "etherscan", "goplus"};
    for (int day = 399; day >= 0; --day)
      for (const char* name : kSources)
        th::RecordCount(writer.work, store.network, token, 1000 + day, false, tj::Str(a, name),
                        today - day * 86400000LL + 3600000);
    static const int kDays[] = {365, 90, 30, 7, 1, 0};
    for (int day : kDays) {
      char json[64 * 1024];
      size_t used = static_cast<size_t>(snprintf(json, sizeof(json), "{\"source\":\"goplus\",\"topHolders\":["));
      for (int i = 0; i < 300; ++i)
        used += static_cast<size_t>(snprintf(json + used, sizeof(json) - used,
                                             "%s{\"address\":\"0x%040x\",\"tag\":\"t%d\",\"isContract\":false,"
                                             "\"isLocked\":false,\"balance\":%d,\"percent\":0.%d}",
                                             i ? "," : "", (i * 7 + day) % 311 + 1, i, 1000 + i, i % 9 + 1));
      used += static_cast<size_t>(snprintf(json + used, sizeof(json) - used,
                                           "],\"topHoldersPercent\":40,\"topHoldersPercentExcludingLocked\":30,"
                                           "\"burnedPercent\":1}"));
      const tj::Value* contract = tj::Parse(a, reinterpret_cast<const uint8_t*>(json), used);
      check(th::RecordDistribution(writer.work, store.network, token, contract, today - day * 86400000LL + 7200000),
            "measure", "distribution");
    }
    WorkBox box(&store.env);
    tj::Value* document = nullptr;
    check(th::HolderHistoryDocument(box.work, store.network, token, 400, nullptr, now, &document), "measure",
          "holder document");
    printf("measure: 400-day holder history (3 sources, 300 positions x 6 days): %zu bytes of arena held "
           "(bound %zu)\n", box.work.arena.held, kHistoryArenaBytes);
    check(box.work.arena.held < kHistoryArenaBytes * 3 / 4, "measure", "the holder document needs over 3/4");
    tj::FreeArena(a);
  }
  close_store(&store);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: token_history_test token_history_edges.json [--measure]\n");
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(static_cast<size_t>(1) << 30);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const tj::Value* cases = tj::Get(fixture, "cases");
  const tj::Value* groups = tj::Get(fixture, "groups");
  if (!tj::IsArray(cases)) {
    fprintf(stderr, "fixture has no cases\n");
    return 2;
  }
  constexpr size_t kStores = 96;
  Store* stores = new Store[kStores];
  size_t store_count = 0;
  int units = 0, pools = 0, seeds = 0;
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* record = cases->items[i];
    const tj::Value* unit = tj::Get(record, "unit");
    // The pools group's history reads read what the R22 route itself wrote
    // (not seeds): the socket replay covers them.
    if (unit && same_string(tj::Get(record, "group"), "pools")) continue;
    if (!unit) {
      if (same_string(tj::Get(record, "group"), "pools") && tj::IsNumber(tj::Get(record, "status")) &&
          tj::Get(record, "status")->number == 200 && !tj::Get(record, "concurrent"))
        check_pools(arena, record, &pools);
      continue;
    }
    const char* kv_name = kv_of(groups, tj::Get(record, "group"));
    const tj::Value* network = tj::Get(unit, "network");
    const tj::Value* token = tj::Get(unit, "tokenId");
    Store* store = nullptr;
    for (size_t s = 0; s < store_count && !store; ++s)
      if (!strcmp(stores[s].kv_name, kv_name) && same_string(network, stores[s].network) &&
          same_string(token, stores[s].token))
        store = &stores[s];
    if (!store) {
      if (store_count == kStores) { check(false, "stores", "too many unit tokens"); break; }
      store = &stores[store_count];
      if (!open_store(store, kv_name, network->text, token->text, static_cast<int>(store_count))) {
        check(false, "store", "open failed");
        break;
      }
      ++store_count;
    }
    // Seeds of the same durable store before this record.
    for (uint32_t k = static_cast<uint32_t>(store->next); k < i; ++k) {
      const tj::Value* seed = cases->items[k];
      const tj::Value* ops = tj::Get(seed, "seed");
      if (!tj::IsArray(ops)) continue;
      const char* seed_kv = kv_of(groups, tj::Get(seed, "group"));
      if (!seed_kv || strcmp(seed_kv, kv_name) != 0) continue;
      g_now = static_cast<int64_t>(tj::Get(seed, "at")->number);
      for (uint32_t o = 0; o < ops->count; ++o)
        if (touches(ops->items[o], *store)) { apply(&store->env, arena, ops->items[o]); ++seeds; }
    }
    store->next = i;
    check_unit(record, store, &units);
  }
  for (size_t s = 0; s < store_count; ++s) close_store(&stores[s]);
  delete[] stores;
  if (argc > 2 && !strcmp(argv[2], "--measure")) measure();
  printf("%d units, %d pool snapshots, %d seed operations, %d checks, %d failures\n", units, pools, seeds, g_checks,
         g_failures);
  tj::FreeArena(arena);
  free(data);
  return g_failures ? 1 : 0;
}
