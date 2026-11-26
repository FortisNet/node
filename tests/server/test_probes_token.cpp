// Test probes of the token, holder and liquidity plane. Compiled
// empty without FORTIS_TOKEN_ROUTE_TEST_ONLY.
#include "test_probes.h"

#ifdef FORTIS_TOKEN_ROUTE_TEST_ONLY
#include "app_support.h"
#include "kv_expiry_sweeper.h"
#include "outbound_https.h"
#include "service_wiring.h"
#include "test_hook_env.h"
#include "device_auth.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "price_series_store.h"
#include "token_provider.h"
#include "token_comparison_store.h"
#include "token_data.h"
#include "token_facts.h"
#include "token_history.h"
#include "token_json.h"
#include "token_swr.h"
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace fortis::app {
namespace {

bool allow_token_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// The token routes' clock: a file lets one process move it between requests;
// otherwise the wall clock (-1).
int64_t token_test_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_TOKEN_NOW_FILE");
  if (now_file && *now_file) {
    if (FILE* handle = fopen(now_file, "r")) {
      char buffer[32]{};
      const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
      fclose(handle);
      buffer[length] = '\0';
      char* end = nullptr;
      const long long parsed = strtoll(buffer, &end, 10);
      if (end && end != buffer && parsed >= 0 && (!*end || *end == '\n'))
        return static_cast<int64_t>(parsed);
    }
  }
  return -1;
}

int64_t token_test_sweep_now_ms() {
  const int64_t fixed = token_test_now_ms();
  if (fixed >= 0) return fixed;
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// The TypeScript fixtures run with coordinatorRand () => 0.5: no jitter.
// FORTIS_TEST_TOKEN_JITTER (a unit sample) moves the scheduler's first runs
// (unit * interval) and later delays, for tests of the scheduled jobs.
bool token_test_jitter(double* sample) {
  *sample = 0.5;
  const char* fixed = TestHookEnv("FORTIS_TEST_TOKEN_JITTER");
  if (fixed && *fixed) {
    char* end = nullptr;
    const double value = strtod(fixed, &end);
    if (end && *end == '\0' && value >= 0 && value <= 1) *sample = value;
  }
  return true;
}

struct TokenTestUpstream final : tokens::TokenOwnerStarter {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(providers::TokenProviderOwner& owner) override {
    // FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS: a replay lists a failing endpoint
    // first for some networks.
    const char* endpoints = TestHookEnv("FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS");
    if (endpoints && *endpoints && !owner.SetNetworkEndpoints(endpoints)) return false;
    return owner.StartRedirected("localhost", port, ca, allow_token_test_address, nullptr,
                                 "x-fortis-test-host");
  }
};

TestFnClock<token_test_now_ms> token_test_clock;
TestFnClock<token_test_sweep_now_ms> token_test_sweep_clock;
TestFnJitter<token_test_jitter> token_test_jitter_source;

// Development-tier fixture devices, one token per letter (Crockford ids).
bool seed_token_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{}; gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  const char letters[] = "ABCDEFGIJKLMNOPQRSTUVWXYZ";
  static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  for (size_t i = 0; letters[i]; ++i) {
    char token[64];
    char id[27];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = letters[i];
    token[54] = '\0';
    snprintf(id, sizeof(id), "01ARZ3NDEKTSV4RRFFQ69G5FT%c", kCrockford[i]);
    if (!auth.SeedPriceRouteFixture(token, id, now_ms)) return false;
  }
  return true;
}

// GET /probe/test-token-job?job=<name>: runs one recurring job now.
void handle_token_test_job(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* service = static_cast<tokens::TokenService*>(opaque);
  struct Name { const char* name; tokens::TokenService::Job job; };
  static const Name kNames[] = {
      {"stats", tokens::TokenService::Job::kStatsFlush}, {"demand", tokens::TokenService::Job::kDemand},
      {"market-active", tokens::TokenService::Job::kMarketActive}, {"market-broad", tokens::TokenService::Job::kMarketBroad},
      {"liquidity", tokens::TokenService::Job::kLiquidity}, {"holders", tokens::TokenService::Job::kHolders},
      {"rollup", tokens::TokenService::Job::kRollup},
      {"active-prices", tokens::TokenService::Job::kActivePrices}, {"price-seal", tokens::TokenService::Job::kPriceSeal},
      {"coin-prices", tokens::TokenService::Job::kCoinPrices}, {"coin-prune", tokens::TokenService::Job::kCoinPopularityPrune},
  };
  const char* query = strchr(request.raw_target, '?');
  const char* value = query ? strstr(query, "job=") : nullptr;
  char summary[1024] = "{}";
  bool ran = false;
  if (value) {
    value += 4;
    for (const Name& name : kNames) {
      const size_t length = strlen(name.name);
      if (strncmp(value, name.name, length) == 0 && (value[length] == '\0' || value[length] == '&')) {
        ran = service->RunJobNow(name.job, summary, sizeof(summary));
        break;
      }
    }
  }
  respond(client, ran ? 200 : 400, ran ? "OK" : "Bad Request", ran ? summary : "{\"error\":\"BAD_JOB\"}", head);
}

// GET /probe/test-price-maintenance: the KV sweeper's ticks and the rows its
// price seal hook moved.
void handle_price_maintenance_test(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* sweeper = static_cast<const storage::KvExpirySweeper*>(opaque);
  char body[128];
  snprintf(body, sizeof(body), "{\"ticks\":%llu,\"sealed\":%llu}",
           static_cast<unsigned long long>(sweeper->ticks()),
           static_cast<unsigned long long>(sweeper->maintained()));
  respond(client, 200, "OK", body, head);
}

// Copies query parameter `name` of `target`, percent-decoded, into `out`.
bool token_test_query_value(const char* target, const char* name, char* out, size_t capacity) {
  const char* query = strchr(target, '?');
  const size_t name_length = strlen(name);
  for (const char* at = query; at && *at;) {
    ++at;
    if (strncmp(at, name, name_length) == 0 && at[name_length] == '=') {
      at += name_length + 1;
      size_t used = 0;
      while (*at && *at != '&') {
        if (used + 1 >= capacity) return false;
        if (*at == '%' && isxdigit(static_cast<unsigned char>(at[1])) &&
            isxdigit(static_cast<unsigned char>(at[2]))) {
          const char hex[3] = {at[1], at[2], '\0'};
          out[used++] = static_cast<char>(strtol(hex, nullptr, 16));
          at += 3;
        } else {
          out[used++] = *at == '+' ? ' ' : *at;
          ++at;
        }
      }
      out[used] = '\0';
      return true;
    }
    at = strchr(at, '&');
  }
  return false;
}

// GET /probe/test-token-series-seed?interval=1h|1d&source=<id>&rows=
// <ts>,<open>,<high>,<low>,<close>,<volume|n>;... upserts fixture candles at
// the token clock: the series TypeScript fixture's priceSeries.upsertCandles.
void handle_token_series_seed(int client, const http::HttpRequest& request, bool head, void*) {
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  char interval[8], source[520], rows[8192];
  static constexpr size_t kMaxRows = 64;
  fortis::prices::PriceSeriesCandleInput candles[kMaxRows]{};
  size_t count = 0;
  bool ok = kv && token_test_query_value(request.raw_target, "interval", interval, sizeof(interval)) &&
            token_test_query_value(request.raw_target, "source", source, sizeof(source)) &&
            token_test_query_value(request.raw_target, "rows", rows, sizeof(rows)) &&
            (strcmp(interval, "1h") == 0 || strcmp(interval, "1d") == 0);
  for (char* row = rows; ok && *row && count < kMaxRows;) {
    char* end = nullptr;
    fortis::prices::PriceSeriesCandleInput& candle = candles[count++];
    candle.ts = strtoll(row, &end, 10);
    double* fields[] = {&candle.open, &candle.high, &candle.low, &candle.close};
    for (double* field : fields) {
      ok = ok && end && *end == ',';
      if (ok) *field = strtod(end + 1, &end);
    }
    ok = ok && end && *end == ',';
    if (!ok) break;
    if (end[1] == 'n') {
      end += 2;
    } else {
      candle.volume = strtod(end + 1, &end);
      candle.has_volume = true;
    }
    ok = end && (*end == ';' || *end == '\0');
    row = *end ? end + 1 : end;
  }
  if (ok && count) {
    fortis::prices::PriceSeriesStore series(*kv);
    const auto interval_value = interval[1] == 'h' ? fortis::prices::PriceSeriesInterval::k1h
                                                   : fortis::prices::PriceSeriesInterval::k1d;
    ok = series.UpsertCandles(interval_value, "usd", source, candles, count, token_test_now_ms()) ==
         fortis::prices::PriceSeriesStoreStatus::kStored;
  }
  respond(client, ok ? 200 : 400, ok ? "OK" : "Bad Request", ok ? "{\"seeded\":true}" : "{\"error\":\"BAD_SEED\"}",
          head);
}

// GET /probe/test-token-idle: waits for queued revalidations to finish.
void handle_token_test_idle(int client, const http::HttpRequest&, bool head, void* opaque) {
  auto* service = static_cast<tokens::TokenService*>(opaque);
  const bool idle = service->WaitIdle(15000);
  respond(client, 200, "OK", idle ? "{\"idle\":true}" : "{\"idle\":false}", head);
}

// POST /probe/test-token-state {keys: [...], tokens: [[network, tokenId], ...]}
// (R19): the SWR envelopes under `keys` as stored (asOf, freshUntil,
// staleUntil, the value's JSON text) and per token the 'facts' document and
// the 'registry' identity fields, at the token clock.
void handle_token_test_state(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* body = request.body && request.body_length
                              ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(request.body), request.body_length)
                              : nullptr;
  const tj::Value* keys = tj::Get(body, "keys");
  const tj::Value* tokens = tj::Get(body, "tokens");
  const int64_t fixed = token_test_now_ms();
  const int64_t now = fixed >= 0 ? fixed : token_test_sweep_now_ms();
  const size_t scratch_bytes = 2 * 1024 * 1024;
  uint8_t* scratch = static_cast<uint8_t*>(malloc(scratch_bytes));
  fortis::tokens::token_data::TokenDataStore data(kv);
  fortis::tokens::TokenComparisonStore identities(kv);
  tj::Value* out = tj::Obj(arena);
  tj::Value* envelopes = tj::Arr(arena);
  tj::Value* rows = tj::Arr(arena);
  bool ok = scratch && out && envelopes && rows && tj::IsArray(keys) && tj::IsArray(tokens);
  for (uint32_t i = 0; ok && i < keys->count; ++i) {
    const tj::Value* key = keys->items[i];
    ok = tj::IsString(key);
    if (!ok) break;
    fortis::tokens::token_swr::EnvelopeInfo info;
    tj::Value* row = tj::Obj(arena);
    tj::Value* envelope = tj::Null(arena);
    if (fortis::tokens::token_swr::PeekEnvelope(kv, key->text, now, scratch, scratch_bytes, &info)) {
      envelope = tj::Obj(arena);
      ok = envelope && tj::Set(arena, envelope, "asOf", tj::Num(arena, static_cast<double>(info.as_of_ms))) &&
           tj::Set(arena, envelope, "freshUntil", tj::Num(arena, static_cast<double>(info.fresh_until_ms))) &&
           tj::Set(arena, envelope, "staleUntil", tj::Num(arena, static_cast<double>(info.stale_until_ms))) &&
           tj::Set(arena, envelope, "d", tj::Str(arena, reinterpret_cast<const char*>(info.body), info.body_length));
    }
    ok = ok && row && tj::Set(arena, row, "key", tj::Clone(arena, key)) && tj::Set(arena, row, "envelope", envelope) &&
         tj::Push(arena, envelopes, row);
  }
  for (uint32_t i = 0; ok && i < tokens->count; ++i) {
    const tj::Value* pair = tokens->items[i];
    const tj::Value* network = tj::IsArray(pair) && pair->count == 2 ? pair->items[0] : nullptr;
    const tj::Value* token_id = network ? pair->items[1] : nullptr;
    ok = tj::IsString(network) && tj::IsString(token_id);
    if (!ok) break;
    tj::Value* facts = nullptr;
    const fortis::tokens::token_data::DataOutcome status = data.ReadFacts(network->text, token_id->text, now, arena, &facts);
    ok = status == fortis::tokens::token_data::DataOutcome::kOk || status == fortis::tokens::token_data::DataOutcome::kMissing;
    if (status != fortis::tokens::token_data::DataOutcome::kOk) facts = tj::Null(arena);
    fortis::tokens::token_store::IdentityRecord identity;
    const bool read = ok && identities.ReadIdentity(network->text, token_id->text, now, &identity) ==
                                fortis::tokens::token_store::ReadStatus::kOk;
    tj::Value* registry = tj::Null(arena);
    if (read && identity.found) {
      registry = tj::Obj(arena);
      const fortis::tokens::comparison::OptString* fields[] = {&identity.symbol, &identity.name, &identity.logo_media_key,
                                                       &identity.coingecko_id};
      const char* names[] = {"symbol", "name", "logoMediaKey", "coingeckoId"};
      for (size_t f = 0; ok && registry && f < 4; ++f)
        if (fields[f]->present) ok = tj::Set(arena, registry, names[f], tj::Str(arena, fields[f]->text, fields[f]->length));
      ok = ok && registry;
    }
    tj::Value* row = tj::Obj(arena);
    ok = ok && row && tj::Set(arena, row, "network", tj::Clone(arena, network)) &&
         tj::Set(arena, row, "tokenId", tj::Clone(arena, token_id)) && tj::Set(arena, row, "facts", facts) &&
         tj::Set(arena, row, "registry", registry) && tj::Push(arena, rows, row);
  }
  free(scratch);
  ok = ok && tj::Set(arena, out, "envelopes", envelopes) && tj::Set(arena, out, "rows", rows);
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(8 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 8 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

// POST /probe/test-token-history-seed {"ops": [...]} (R20–R23): the
// operations tests/token_history_edges_test.py replays from the oracle's
// `seed` records, through the production writers at the token clock
// (token_history::RecordCount / RecordDistribution / RecordSuccess /
// RecordNotFound / RecordDiagnostic / RollupLiquidityCandles, i.e.
// HolderSnapshotStore.recordCount/recordDistribution, LiquiditySnapshotStore
// and rollupLiquidityCandles). {"applied": [bool per op]}; a writer refusing
// its input (an invalid count, an empty source) is false, as TypeScript
// returns without writing. Its own TokenDataStore over the runtime KV: every
// write is one KV frame, so readers see each op whole.
void handle_token_history_seed(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  namespace th = fortis::tokens::token_history;
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* body = request.body && request.body_length
                              ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(request.body), request.body_length)
                              : nullptr;
  const tj::Value* ops = tj::Get(body, "ops");
  fortis::tokens::token_data::TokenDataStore data(kv);
  fortis::tokens::token_swr::Seams seams{};
  seams.clock = &token_test_clock;
  fortis::tokens::token_swr::Cache swr(kv, &seams);
  fortis::tokens::token_facts::Env env{};
  env.kv = kv;
  env.data = &data;
  env.swr = &swr;
  constexpr size_t kWorkBytes = 32 * 1024 * 1024;
  fortis::tokens::token_facts::Work work{};
  work.env = &env;
  work.arena = tj::Arena{static_cast<uint8_t*>(malloc(kWorkBytes)), kWorkBytes, 0, true};
  work.scratch = static_cast<uint8_t*>(malloc(1024 * 1024 + 64));
  work.scratch_capacity = 1024 * 1024 + 64;
  tj::Value* applied = tj::Arr(arena);
  bool ok = kv && data.valid() && work.arena.base && work.scratch && applied && tj::IsArray(ops);
  auto number = [](const tj::Value* v, double fallback) { return tj::IsNumber(v) ? v->number : fallback; };
  for (uint32_t i = 0; ok && i < ops->count; ++i) {
    const tj::Value* op = ops->items[i];
    const char* kind = tj::CString(arena, tj::Get(op, "op"));
    const char* network = tj::IsString(tj::Get(op, "network")) ? tj::CString(arena, tj::Get(op, "network")) : "";
    const char* token_id = tj::IsString(tj::Get(op, "tokenId")) ? tj::CString(arena, tj::Get(op, "tokenId")) : "";
    th::ObservationContext context{};
    const tj::Value* observed = tj::Get(op, "observedAt");
    context.has_observed_at = tj::IsNumber(observed);
    context.observed_at = context.has_observed_at ? static_cast<int64_t>(observed->number) : 0;
    const tj::Value* source = tj::Get(op, "source");
    context.source = tj::IsString(source) ? tj::CString(arena, source) : nullptr;
    const tj::Value* sampling = tj::Get(op, "samplingIntervalSeconds");
    context.has_sampling = tj::IsNumber(sampling);
    context.sampling_seconds = context.has_sampling ? sampling->number : 0;
    bool done = false;
    work.arena.used = 0;
    work.arena.ok = true;
    if (!kind) {
      ok = false;
    } else if (strcmp(kind, "count") == 0) {
      const tj::Value* at_least = tj::Get(op, "atLeast");
      done = th::RecordCount(work, network, token_id, number(tj::Get(op, "count"), -1),
                             at_least && at_least->type == tj::Type::kTrue, source, context.observed_at);
    } else if (strcmp(kind, "distribution") == 0) {
      // recordDistribution's snapshot is the normalized contract's holders.
      tj::Value* contract = tj::Obj(work.arena);
      done = contract && tj::Set(work.arena, contract, "source", tj::Clone(work.arena, source)) &&
             tj::Set(work.arena, contract, "topHolders", tj::Clone(work.arena, tj::Get(op, "holders")));
      static const char* const kSummary[] = {"topHoldersPercent", "topHoldersPercentExcludingLocked", "burnedPercent"};
      for (const char* key : kSummary)
        done = done && tj::Set(work.arena, contract, key, tj::Clone(work.arena, tj::Get(op, key)));
      done = done && th::RecordDistribution(work, network, token_id, contract, context.observed_at);
    } else if (strcmp(kind, "success") == 0) {
      done = th::RecordSuccess(work, network, token_id, tj::Get(op, "snapshot"), context);
    } else if (strcmp(kind, "notFound") == 0) {
      done = th::RecordNotFound(work, network, token_id, context);
    } else if (strcmp(kind, "diagnostic") == 0) {
      const char* status = tj::CString(arena, tj::Get(op, "status"));
      done = status && th::RecordDiagnostic(work, network, token_id, status,
                                            static_cast<int64_t>(number(tj::Get(op, "checkedAt"), 0)),
                                            context.source);
    } else if (strcmp(kind, "rollup") == 0) {
      th::RollupLiquidityCandles(work, static_cast<int64_t>(number(tj::Get(op, "now"), 0)));
      done = true;
    } else {
      ok = false;
    }
    ok = ok && tj::Push(arena, applied, tj::Bool(arena, done));
  }
  free(work.arena.base);
  free(work.scratch);
  tj::Value* out = tj::Obj(arena);
  char text[64 * 1024];
  tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(text), sizeof(text), 0, true};
  ok = ok && out && tj::Set(arena, out, "applied", applied) && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", reinterpret_cast<const uint8_t*>(text), buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 400, "Bad Request", "{\"error\":\"BAD_SEED\"}", head);
  }
  tj::FreeArena(arena);
}

// POST /probe/test-token-history-state {"tokens": [[network, tokenId], ...]}
// (R20–R23): per token the holder snapshot rows as stored (HolderRows: every
// source, key order, epoch-ms dates), the raw liquidity observations and the
// 1h/1d/1w candles (TypeScript's field names; an observation keeps only the
// h24 volume and trade gauges the reads use) and the facts document, at the
// token clock.
void handle_token_history_state(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  namespace td = fortis::tokens::token_data;
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* body = request.body && request.body_length
                              ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(request.body), request.body_length)
                              : nullptr;
  const tj::Value* tokens = tj::Get(body, "tokens");
  const int64_t fixed = token_test_now_ms();
  const int64_t now = fixed >= 0 ? fixed : token_test_sweep_now_ms();
  td::TokenDataStore data(kv);
  constexpr size_t kRows = 16384;
  auto* observations = static_cast<td::Observation*>(malloc(sizeof(td::Observation) * kRows));
  auto* candles = static_cast<td::Candle*>(malloc(sizeof(td::Candle) * kRows));
  tj::Value* rows = tj::Arr(arena);
  bool ok = kv && data.valid() && observations && candles && rows && tj::IsArray(tokens);
  auto num = [&](bool has, double v) { return has ? tj::Num(arena, v) : tj::Null(arena); };
  for (uint32_t i = 0; ok && i < tokens->count; ++i) {
    const tj::Value* pair = tokens->items[i];
    const tj::Value* network = tj::IsArray(pair) && pair->count == 2 ? pair->items[0] : nullptr;
    const tj::Value* token_id = network ? pair->items[1] : nullptr;
    ok = tj::IsString(network) && tj::IsString(token_id);
    if (!ok) break;
    tj::Value* row = tj::Obj(arena);
    tj::Value* holders = nullptr;
    ok = row && data.HolderRows(network->text, token_id->text, now, arena, &holders) == td::DataOutcome::kOk;
    if (ok && !holders) holders = tj::Arr(arena);
    td::Rows<td::Observation> raw{observations, kRows, 0, false};
    ok = ok && data.Observations(network->text, token_id->text, 0, INT64_MAX / 4, now, &raw) == td::DataOutcome::kOk;
    tj::Value* raw_json = tj::Arr(arena);
    for (size_t k = 0; ok && raw_json && k < raw.count; ++k) {
      const td::Observation& o = raw.items[k];
      tj::Value* item = tj::Obj(arena);
      ok = item && tj::Set(arena, item, "ts", tj::Num(arena, static_cast<double>(o.ts))) &&
           tj::Set(arena, item, "status", tj::Str(arena, o.ok ? "ok" : "not_found")) &&
           tj::Set(arena, item, "source", tj::Str(arena, o.source)) &&
           tj::Set(arena, item, "samplingIntervalSeconds", num(o.has_sampling, o.sampling_seconds)) &&
           tj::Set(arena, item, "totalLiquidityUsd", num(o.has_liquidity, o.liquidity)) &&
           tj::Set(arena, item, "weightedPriceUsd", num(o.has_price, o.price)) &&
           tj::Set(arena, item, "priceSpreadPct", num(o.has_spread, o.spread)) &&
           tj::Set(arena, item, "priceSpreadPools", num(o.has_spread_pools, o.spread_pools)) &&
           tj::Set(arena, item, "poolCount", tj::Num(arena, o.pool_count)) &&
           tj::Set(arena, item, "topPoolShare", num(o.has_top_share, o.top_share)) &&
           tj::Set(arena, item, "fdvUsd", num(o.has_fdv, o.fdv)) &&
           tj::Set(arena, item, "marketCapUsd", num(o.has_market_cap, o.market_cap)) &&
           tj::Set(arena, item, "volumeH24", num(o.has_volume_h24, o.volume_h24)) &&
           tj::Set(arena, item, "buysH24", num(o.has_buys_h24, o.buys_h24)) &&
           tj::Set(arena, item, "sellsH24", num(o.has_sells_h24, o.sells_h24)) && tj::Push(arena, raw_json, item);
    }
    tj::Value* candle_json = tj::Obj(arena);
    static const char* const kFields[] = {
        "liquidityOpen", "liquidityHigh", "liquidityLow", "liquidityClose", "priceOpen", "priceHigh",
        "priceLow", "priceClose", "spreadClosePct", "spreadMaxPct", "topPoolShareClose", "topPoolShareMax",
        "poolCountOpen", "poolCountClose", "volume24hClose", "buys24hClose", "sells24hClose"};
    static const td::Interval kIntervals[] = {td::Interval::k1h, td::Interval::k1d, td::Interval::k1w};
    for (td::Interval interval : kIntervals) {
      td::Rows<td::Candle> stored{candles, kRows, 0, false};
      ok = ok && candle_json &&
           data.Candles(network->text, token_id->text, interval, 0, INT64_MAX / 4, now, &stored) == td::DataOutcome::kOk;
      tj::Value* list = tj::Arr(arena);
      for (size_t k = 0; ok && list && k < stored.count; ++k) {
        const td::Candle& c = stored.items[k];
        tj::Value* item = tj::Obj(arena);
        ok = item && tj::Set(arena, item, "ts", tj::Num(arena, static_cast<double>(c.ts)));
        for (size_t f = 0; ok && f < td::Candle::kFields; ++f) ok = tj::Set(arena, item, kFields[f], num(c.has[f], c.value[f]));
        ok = ok && tj::Set(arena, item, "sampleCount", tj::Num(arena, c.sample_count)) &&
             tj::Set(arena, item, "expectedSamples", tj::Num(arena, c.expected_samples)) &&
             tj::Set(arena, item, "source", tj::Str(arena, c.source)) && tj::Push(arena, list, item);
      }
      ok = ok && list && tj::Set(arena, candle_json, td::IntervalName(interval), list);
    }
    tj::Value* facts = nullptr;
    const td::DataOutcome status = ok ? data.ReadFacts(network->text, token_id->text, now, arena, &facts) : td::DataOutcome::kIoError;
    ok = ok && (status == td::DataOutcome::kOk || status == td::DataOutcome::kMissing);
    if (status != td::DataOutcome::kOk) facts = tj::Null(arena);
    ok = ok && raw_json && tj::Set(arena, row, "network", tj::Clone(arena, network)) &&
         tj::Set(arena, row, "tokenId", tj::Clone(arena, token_id)) && tj::Set(arena, row, "holders", holders) &&
         tj::Set(arena, row, "observations", raw_json) && tj::Set(arena, row, "candles", candle_json) &&
         tj::Set(arena, row, "facts", facts) && tj::Push(arena, rows, row);
  }
  free(observations);
  free(candles);
  tj::Value* out = tj::Obj(arena);
  constexpr size_t kTextBytes = 64 * 1024 * 1024;
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(kTextBytes)) : nullptr;
  tj::OutBuffer buffer{text, kTextBytes, 0, true};
  ok = ok && out && text && tj::Set(arena, out, "rows", rows) && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

}  // namespace

namespace {
bool token_test_route = false;
TokenTestUpstream token_test_upstream{};
tokens::TokenServiceSeams token_test_seams{};
}  // namespace

bool ConfigureTokenTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* token_flag = TestHookEnv("FORTIS_ENABLE_TEST_TOKEN_ROUTES");
  token_test_route = token_flag && strcmp(token_flag, "1") == 0;
  const char* token_upstream_port = TestHookEnv("FORTIS_TEST_TOKEN_UPSTREAM_PORT");
  const char* token_upstream_ca = TestHookEnv("FORTIS_TEST_TOKEN_UPSTREAM_CA");
  if (token_test_route && config.native_price_route_canary) {
    fprintf(stderr, "FORTIS_ENABLE_TEST_TOKEN_ROUTES and "
                    "FORTIS_NATIVE_PRICE_ROUTE_CANARY are mutually exclusive\n");
    return false;
  }
  if (token_test_route && (parse_port(token_upstream_port) < 0 || !token_upstream_ca ||
      !*token_upstream_ca || !http::OutboundHttpsValidateCaBundle(token_upstream_ca))) {
    fprintf(stderr, "Invalid test-only token upstream configuration\n");
    return false;
  }
  if (token_test_route) {
    snprintf(token_test_upstream.port, sizeof(token_test_upstream.port), "%d",
             parse_port(token_upstream_port));
    token_test_upstream.ca = token_upstream_ca;
    token_test_seams.clock = &token_test_clock;
    token_test_seams.jitter = &token_test_jitter_source;
    const char* manual = TestHookEnv("FORTIS_TEST_TOKEN_MANUAL_JOBS");
    token_test_seams.manual_jobs = !(manual && strcmp(manual, "0") == 0);
    const char* no_saucerswap = TestHookEnv("FORTIS_TEST_TOKEN_NO_SAUCERSWAP");
    token_test_seams.saucerswap_disabled = no_saucerswap && strcmp(no_saucerswap, "1") == 0;
    options.token_seams = &token_test_seams;
    options.token_owner = &token_test_upstream;
    options.comparison_clock = &token_test_clock;
    options.popularity_clock = &token_test_clock;
    options.token = true;
    // The page series reads through the price history service: its owners
    // use the token upstream, and the history seams' read-failure hook
    // (FORTIS_TEST_HISTORY_FAIL_SOURCE) applies. The owner clock is
    // FORTIS_TEST_HISTORY_NOW_FILE (the replay points it at the token clock).
    UseLoopbackHistoryOwners(parse_port(token_upstream_port), token_upstream_ca, true, options);
  }
  return true;
}

bool SeedTokenTest(fortis::storage::KvStore* kv) {
  if (token_test_route && !seed_token_test_devices(kv)) {
    fprintf(stderr, "Failed to seed test token fixtures\n");
    return false;
  }
  return true;
}

bool RegisterTokenTestRoutes(http::HttpRouter& router, Planes& planes) {
  bool registered = true;
  // The token replay reads what the writers stored through the comparison
  // route too; it is registered here when discovery is not.
  if (token_test_route && !planes.discovery.has_value())
    registered = RegisterTokenComparisonRoute(router, &planes.comparison_context);
  if (registered && token_test_route)
    registered =
        router.Add("GET", "/probe/test-token-job", handle_token_test_job, planes.token ? &*planes.token : nullptr) &&
        router.Add("GET", "/probe/test-token-idle", handle_token_test_idle, planes.token ? &*planes.token : nullptr) &&
        router.Add("POST", "/probe/test-token-state", handle_token_test_state, nullptr) &&
        router.Add("POST", "/probe/test-token-history-seed", handle_token_history_seed, nullptr) &&
        router.Add("POST", "/probe/test-token-history-state", handle_token_history_state, nullptr) &&
        router.Add("GET", "/probe/test-token-series-seed", handle_token_series_seed, nullptr) &&
        router.Add("GET", "/probe/test-price-maintenance", handle_price_maintenance_test, &planes.kv_sweeper);
  return registered;
}

void TokenTestSweepClock(fortis::base::Clock** clock) {
  if (token_test_route) *clock = &token_test_sweep_clock;
}

}  // namespace fortis::app
#endif
