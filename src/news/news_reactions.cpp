// GET /v1/news/{id}/reactions: the reaction jobs (one per story being
// measured), their CoinGecko day samples, the measurement and the handler.
#include "news_routes_internal.h"

#include "threading.h"

namespace pw = fortis::codec::proto_wire;

namespace hb = fortis::codec::history_batch;

namespace fortis::news {

namespace {

// sampleMessage: BigInt(sample[0]) throws for a non-integer, which makes the
// protobuf reply a 500 exactly as in TypeScript.
bool sample_proto(Proto& p, const json::Value& sample, OutBuffer& out, uint32_t field) {
  if (sample.type != json::Type::kArray || sample.count < 2 ||
      sample.items[0].type != json::Type::kNumber) return false;
  const double at = sample.items[0].number;
  if (!isfinite(at) || floor(at) != at || fabs(at) > 9.2e18) return false;
  OutBuffer body = p.sub(32);
  const int64_t millis = static_cast<int64_t>(at);
  if (millis) { pw::PutTag(body, 1, 0); pw::PutVarint(body, static_cast<uint64_t>(millis)); }
  const json::Value& usd = sample.items[1];
  const double price = usd.type == json::Type::kNumber ? usd.number : 0;
  pw::PutImplicitDouble(body, 2, price);
  p.message(out, field, body);
  return p.ok;
}

bool window_proto(Proto& p, const json::Value& window, OutBuffer& out) {
  const json::Value* seconds = json::Find(window, "seconds");
  if (seconds && seconds->type == json::Type::kNumber && seconds->number > 0)
    p.varint(out, 1, static_cast<uint64_t>(seconds->number));
  p.string(out, 2, json::Find(window, "status"), true);
  p.optional_double(out, 3, json::Find(window, "returnPct"));
  const json::Value* baseline = json::Find(window, "baseline");
  if (baseline && baseline->type != json::Type::kNull && !sample_proto(p, *baseline, out, 4)) return false;
  const json::Value* endpoint = json::Find(window, "endpoint");
  if (endpoint && endpoint->type != json::Type::kNull && !sample_proto(p, *endpoint, out, 5)) return false;
  p.optional_double(out, 6, json::Find(window, "benchmarkReturnPct"));
  const json::Value* bb = json::Find(window, "benchmarkBaseline");
  if (bb && bb->type != json::Type::kNull && !sample_proto(p, *bb, out, 7)) return false;
  const json::Value* be = json::Find(window, "benchmarkEndpoint");
  if (be && be->type != json::Type::kNull && !sample_proto(p, *be, out, 8)) return false;
  p.optional_double(out, 9, json::Find(window, "differencePp"));
  return p.ok;
}

bool asset_proto(Proto& p, const json::Value& asset, OutBuffer& out) {
  p.string(out, 1, json::Find(asset, "coinId"), false);
  p.string(out, 2, json::Find(asset, "symbol"), true);
  if (const json::Value* points = json::Find(asset, "points"))
    for (size_t i = 0; i < points->count; ++i)
      if (!sample_proto(p, points->items[i], out, 3)) return false;
  if (const json::Value* windows = json::Find(asset, "windows"))
    for (size_t i = 0; i < windows->count; ++i) {
      OutBuffer body = p.sub(windows->items[i].raw_length * 2 + 128);
      if (!window_proto(p, windows->items[i], body)) return false;
      p.message(out, 4, body);
    }
  return p.ok;
}

bool reactions_proto(const json::Value& value, Arena* arena, OutBuffer& out) {
  Proto p{arena};
  p.string(out, 1, json::Find(value, "eventId"), true);
  if (json::Find(value, "unavailable")) {
    p.varint(out, 6, 1);
    return p.ok && out.ok;
  }
  p.string(out, 2, json::Find(value, "publishedAt"), false);
  p.string(out, 3, json::Find(value, "asOf"), false);
  p.string(out, 4, json::Find(value, "currency"), false);
  p.string(out, 5, json::Find(value, "source"), false);
  if (const json::Value* benchmark = json::Find(value, "benchmark")) {
    OutBuffer body = p.sub(benchmark->raw_length * 2 + 128);
    if (!asset_proto(p, *benchmark, body)) return false;
    p.message(out, 7, body);
  }
  if (const json::Value* assets = json::Find(value, "assets"))
    for (size_t i = 0; i < assets->count; ++i) {
      OutBuffer body = p.sub(assets->items[i].raw_length * 2 + 128);
      if (!asset_proto(p, assets->items[i], body)) return false;
      p.message(out, 8, body);
    }
  return p.ok && out.ok;
}

using State = NewsReactionsState;

void free_job(State::Job& job) {
  job = State::Job{};
}



// read(): each element of one day's cached `prices` value. A non-array value
// or an element that cannot be destructured is the TypeError that makes the
// whole asset empty; a kept element is copied with its exact JSON text.
bool consume_day(State::Coin& coin, const State::Job& job, const uint8_t* text, size_t length,
                 Arena* arena) {
  const size_t mark = arena->used;
  const json::Value* value = json::Parse(text, length, arena);
  bool ok = value && value->type == json::Type::kArray;
  for (size_t i = 0; ok && i < value->count; ++i) {
    const json::Value& element = value->items[i];
    if (element.type == json::Type::kString) continue;  // at = a character: never kept
    if (element.type != json::Type::kArray) { ok = false; break; }
    if (element.count < 1 || element.items[0].type != json::Type::kNumber) continue;
    const double at = element.items[0].number;
    if (!(at >= static_cast<double>(job.from_ms) && at <= static_cast<double>(job.to_ms))) continue;
    // cleanSamples(points, now): finite positive time not after now, finite
    // positive number price.
    if (element.count < 2 || element.items[1].type != json::Type::kNumber) continue;
    const double price = element.items[1].number;
    if (!(isfinite(at) && at > 0 && at <= static_cast<double>(job.now_ms) && isfinite(price) &&
          price > 0)) continue;
    if (coin.sample_count == kReactionMaxSamples ||
        coin.raw_used + element.raw_length > kReactionMaxRawBytes) { ok = false; break; }
    if (coin.raw_used + element.raw_length > coin.raw_capacity) {
      size_t next = coin.raw_capacity ? coin.raw_capacity * 2 : 4096;
      while (next < coin.raw_used + element.raw_length) next *= 2;
      if (next > kReactionMaxRawBytes) next = kReactionMaxRawBytes;
      if (!base::ResizeStorage(coin.raw, coin.raw_used, next)) { ok = false; break; }
      coin.raw_capacity = next;
    }
    if (coin.sample_count == coin.sample_capacity) {
      size_t next = coin.sample_capacity ? coin.sample_capacity * 2 : 64;
      if (next > kReactionMaxSamples) next = kReactionMaxSamples;
      if (!base::ResizeStorage(coin.samples, coin.sample_count * sizeof(State::Sample), next * sizeof(State::Sample))) { ok = false; break; }
      coin.sample_capacity = next;
    }
    memcpy(coin.raw.get() + coin.raw_used, element.raw, element.raw_length);
    coin.samples[coin.sample_count++] = State::Sample{at, price,
        static_cast<uint32_t>(coin.raw_used), static_cast<uint32_t>(element.raw_length)};
    coin.raw_used += element.raw_length;
  }
  arena->used = mark;
  return ok;
}

// The day key and provider path of coin.day.
bool sample_request(const State::Job& job, const State::Coin& coin, int64_t now, char key[200],
                    char* path, size_t path_capacity) {
  if (snprintf(key, 200, "news-samples:v1:%s:%lld", coin.id, static_cast<long long>(coin.day)) >= 200)
    return false;
  (void)job;
  const int64_t to = coin.day + kDayMs < now ? coin.day + kDayMs : now;
  OutBuffer out{reinterpret_cast<uint8_t*>(path), path_capacity - 1};
  out.Text("/api/v3/coins/");
  append_component(out, coin.id, strlen(coin.id));
  char tail[128];
  snprintf(tail, sizeof(tail), "/market_chart/range?vs_currency=usd&from=%lld&to=%lld",
           static_cast<long long>(coin.day / 1000), static_cast<long long>(to >= 0 ? to / 1000 : 0));
  out.Text(tail);
  if (!out.ok) return false;
  path[out.used] = '\0';
  return true;
}






// Runs one asset's day loop until it needs the provider, fails or ends.


// cleanSamples: unique times (the last element with a time wins, at the first
// position), then ascending time.
size_t clean(const State::Coin& coin, size_t* order) {
  size_t count = 0;
  for (size_t i = 0; i < coin.sample_count; ++i) {
    bool replaced = false;
    for (size_t j = 0; j < count; ++j)
      if (coin.samples[order[j]].at == coin.samples[i].at) { order[j] = i; replaced = true; break; }
    if (!replaced) order[count++] = i;
  }
  for (size_t i = 1; i < count; ++i) {
    const size_t moving = order[i];
    size_t j = i;
    while (j > 0 && coin.samples[order[j - 1]].at > coin.samples[moving].at) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = moving;
  }
  return count;
}

struct Measured {
  int seconds = 0;
  int status = 0;  // 0 pending, 1 unavailable, 2 measured
  double return_pct = 0;
  long baseline = -1;  // index into the cleaned order
  long endpoint = -1;
};

// measureReaction(samples, published, now, seconds).
Measured measure(const State::Coin& coin, const size_t* order, size_t count, int64_t published,
                 int64_t now, int seconds) {
  Measured m;
  m.seconds = seconds;
  const double target = static_cast<double>(published) + seconds * 1000.0;
  if (target > static_cast<double>(now)) return m;
  const double lag = 300000.0 < seconds * 1000.0 / 5 ? 300000.0 : seconds * 1000.0 / 5;
  long baseline = -1, endpoint = -1;
  for (size_t i = 0; i < count; ++i) {
    const double at = coin.samples[order[i]].at;
    if (at <= static_cast<double>(published)) baseline = static_cast<long>(i);
    if (at <= target) endpoint = static_cast<long>(i);
  }
  m.status = 1;
  if (baseline < 0 || endpoint < 0) return m;
  const State::Sample& b = coin.samples[order[baseline]];
  const State::Sample& e = coin.samples[order[endpoint]];
  if (static_cast<double>(published) - b.at > lag || target - e.at > lag || e.at <= b.at) return m;
  const double value = (e.price / b.price - 1) * 100;
  if (!isfinite(value)) return m;
  m.status = 2;
  m.return_pct = value;
  m.baseline = baseline;
  m.endpoint = endpoint;
  return m;
}

void append_raw(OutBuffer& out, const State::Coin& coin, const size_t* order, long index) {
  const State::Sample& s = coin.samples[order[index]];
  out.Append(coin.raw.get() + s.raw_offset, s.raw_length);
}

void append_points(OutBuffer& out, const State::Coin* coin, const size_t* order, size_t count) {
  out.Byte('[');
  for (size_t i = 0; coin && i < count; ++i) {
    if (i) out.Byte(',');
    append_raw(out, *coin, order, static_cast<long>(i));
  }
  out.Byte(']');
}

void append_window(OutBuffer& out, const Measured& m, const State::Coin* coin, const size_t* order) {
  char seconds[16];
  snprintf(seconds, sizeof(seconds), "%d", m.seconds);
  out.Text("{\"seconds\":");
  out.Text(seconds);
  if (m.status == 2) {
    out.Text(",\"status\":\"measured\",\"returnPct\":");
    hb::AppendJsNumber(out, m.return_pct);
    out.Text(",\"baseline\":");
    append_raw(out, *coin, order, m.baseline);
    out.Text(",\"endpoint\":");
    append_raw(out, *coin, order, m.endpoint);
  } else {
    out.Text(",\"returnPct\":null,\"baseline\":null,\"endpoint\":null,\"status\":");
    out.Text(m.status == 0 ? "\"pending\"" : "\"unavailable\"");
  }
}



}  // namespace

NewsReactionsState* g_news_reactions = nullptr;

namespace {

// NewsReactions.measure(story), started for one job.




}  // namespace

ReactionService::Pending* ReactionService::AcquirePending() {
  auto* state = this;
  for (State::Pending& slot : state->pending) if (!slot.job) return &slot;
  return nullptr;
}

bool ReactionService::Submit(State::Job& job, size_t index, const char* key, const char* path,
            bool refresh) {
  auto* state = this;
  State::Pending* slot = state->AcquirePending();
  if (!slot) return false;
  slot->owner = state;
  slot->job = &job;
  slot->coin = index;
  slot->refresh = refresh;
  snprintf(slot->key, sizeof(slot->key), "%s", key);
  uint64_t id = 0;
  ++job.outstanding;
  if (!state->route->owner->Submit(discovery::DiscoveryUpstream::kCoinGecko, path,
                                   discovery::DiscoveryMonotonicMs() + kOwnerDeadlineMs,
                                   refresh || job.background, OnSample, slot, &id,
                                   /*retry_unparseable=*/true)) {
    --job.outstanding;
    *slot = State::Pending{};
    return false;
  }
  if (!refresh) job.coins[index].request_id = id;
  return true;
}

void ReactionService::OnSample(void* context, const discovery::DiscoveryProviderResult& result, const unsigned char* body,
               size_t body_length) {
  auto* slot = static_cast<State::Pending*>(context);
  State::Job* job = slot->job;
  State* state = slot->owner;
  NewsRouteContext* route = state->route;
  NewsWorkspace* workspace = route->workspace;
  fortis::base::LockGuard lock(workspace->mutex);
  discovery::DiscoveryCache* cache = workspace->cache;
  const int64_t now = route_now(route);
  const size_t index = slot->coin;
  const bool refresh = slot->refresh;
  char key[200];
  snprintf(key, sizeof(key), "%s", slot->key);
  *slot = State::Pending{};
  --job->outstanding;
  const bool success = !result.cancelled && !result.transport_error && !result.lane_rejected &&
                       result.http_status >= 200 && result.http_status < 300;
  uint8_t* value = workspace->json_out;
  size_t value_length = 0;
  Arena parse_arena{workspace->arena, sizeof(workspace->arena)};
  const bool parsed = success && prices_value(body, body_length, value, sizeof(workspace->json_out),
                                              &value_length, &parse_arena);
  if (refresh) {
    int64_t as_of = 0;
    if (parsed) cache->Write(key, kTtlSamples, now, value, value_length, &as_of);
    else cache->RecordRevalidateFailure(key, now, result.lane_rejected);
  } else {
    State::Coin& coin = job->coins[index];
    coin.waiting = false;
    if (parsed) {
      int64_t as_of = 0;
      cache->Write(key, kTtlSamples, now, value, value_length, &as_of);
      Arena arena{workspace->arena, sizeof(workspace->arena)};
      if (consume_day(coin, *job, value, value_length, &arena)) coin.day += kDayMs;
      else coin.failed = true;
    } else {
      cache->RecordMissFailure(key, now, result.lane_rejected,
                               result.http_status ? result.http_status : 502);
      coin.failed = true;
    }
    state->Advance(*job, index);
  }
  bool all = true;
  for (size_t i = 0; i < job->coin_count; ++i)
    all = all && (job->coins[i].done || job->coins[i].failed) && !job->coins[i].waiting;
  if (job->used && all && !job->outstanding) state->Finish(*job);
}

void ReactionService::Advance(State::Job& job, size_t index) {
  auto* state = this;
  State::Coin& coin = job.coins[index];
  NewsRouteContext* route = state->route;
  discovery::DiscoveryCache* cache = route->workspace->cache;
  while (!coin.done && !coin.failed && !coin.waiting) {
    if (coin.day > job.to_ms) { coin.done = true; break; }
    const int64_t now = route_now(route);
    char key[200];
    char path[512];
    if (!sample_request(job, coin, now, key, path, sizeof(path))) { coin.failed = true; break; }
    const discovery::DiscoveryCacheEntry entry = cache->Read(key, now);
    if (entry.state == discovery::DiscoveryCacheState::kFresh || entry.state == discovery::DiscoveryCacheState::kStale) {
      Arena arena{route->workspace->arena, sizeof(route->workspace->arena)};
      uint8_t* copy = static_cast<uint8_t*>(arena.Alloc(entry.body_length + 1, 1));
      if (!copy) { coin.failed = true; break; }
      memcpy(copy, entry.body, entry.body_length);
      const size_t mark = arena.used;
      Arena rest{route->workspace->arena + mark, sizeof(route->workspace->arena) - mark};
      if (!consume_day(coin, job, copy, entry.body_length, &rest)) { coin.failed = true; break; }
      if (entry.state == discovery::DiscoveryCacheState::kStale && cache->AllowRevalidate(key, now))
        state->Submit(job, index, key, path, true);
      coin.day += kDayMs;
      continue;
    }
    int replay = 0;
    if (cache->MissReplay(key, now, &replay)) { coin.failed = true; break; }
    coin.waiting = true;
    if (!state->Submit(job, index, key, path, false)) {
      coin.waiting = false;
      cache->RecordMissFailure(key, now, true, 429);
      coin.failed = true;
    }
  }
}

void ReactionService::Finish(State::Job& job) {
  auto* state = this;
  NewsRouteContext* route = state->route;
  NewsWorkspace* workspace = route->workspace;
  OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
  const State::Coin* btc = nullptr;
  for (size_t i = 0; i < job.coin_count; ++i)
    if (strcmp(job.coins[i].id, "bitcoin") == 0) btc = job.coins[i].failed ? nullptr : &job.coins[i];
  std::unique_ptr<size_t[], base::StorageDeleter> orders[kReactionCoins];
  const State::Coin empty;
  size_t counts[kReactionCoins]{};
  bool memory = true;
  for (size_t i = 0; i < job.coin_count; ++i) {
    if (job.coins[i].failed || !job.coins[i].sample_count) continue;
    orders[i] = base::AllocateStorage<size_t[]>(job.coins[i].sample_count * sizeof(size_t));
    if (!orders[i]) { memory = false; continue; }
    counts[i] = clean(job.coins[i], orders[i].get());
  }
  size_t btc_index = 0;
  for (size_t i = 0; i < job.coin_count; ++i) if (&job.coins[i] == btc) btc_index = i;
  const size_t* btc_order = btc ? orders[btc_index].get() : nullptr;
  const size_t btc_count = btc ? counts[btc_index] : 0;
  Measured bench[4];
  for (size_t w = 0; w < 4; ++w)
    bench[w] = measure(btc ? *btc : empty, btc_order, btc_count, job.published_ms,
                       job.now_ms, kWindows[w]);
  out.Text("{\"eventId\":\"");
  out.Text(job.id);
  out.Text("\",\"publishedAt\":");
  append_iso(out, job.published_ms);
  out.Text(",\"asOf\":");
  append_iso(out, job.now_ms);
  out.Text(",\"currency\":\"USD\",\"source\":\"CoinGecko\",\"benchmark\":{\"coinId\":\"bitcoin\","
           "\"symbol\":\"BTC\",\"points\":");
  append_points(out, btc, btc_order, btc_count);
  out.Text(",\"windows\":[");
  for (size_t w = 0; w < 4; ++w) {
    if (w) out.Byte(',');
    append_window(out, bench[w], btc, btc_order);
    out.Byte('}');
  }
  out.Text("]},\"assets\":[");
  for (size_t t = 0; t < job.ticker_count; ++t) {
    const State::TickerCopy& ticker = job.tickers[t];
    const State::Coin* coin = nullptr;
    size_t coin_index = 0;
    if (ticker.has_coin_id)
      for (size_t i = 0; i < job.coin_count; ++i)
        if (strcmp(job.coins[i].id, ticker.coin_id) == 0) { coin_index = i; coin = &job.coins[i]; }
    if (coin && coin->failed) coin = nullptr;
    const size_t* order = coin ? orders[coin_index].get() : nullptr;
    const size_t count = coin ? counts[coin_index] : 0;
    if (t) out.Byte(',');
    out.Text("{\"coinId\":");
    if (ticker.has_coin_id) {
      U16 text{nullptr, 0};
      Arena scratch{workspace->arena, sizeof(workspace->arena)};
      text = scratch.FromUtf8(ticker.coin_id, strlen(ticker.coin_id));
      fortis::news::AppendJsonString(out, text);
    } else {
      out.Text("null");
    }
    out.Text(",\"symbol\":");
    fortis::news::AppendJsonString(out, U16{ticker.symbol.get(), ticker.symbol_length});
    out.Text(",\"points\":");
    append_points(out, coin, order, count);
    out.Text(",\"windows\":[");
    const bool is_bitcoin = ticker.has_coin_id && strcmp(ticker.coin_id, "bitcoin") == 0;
    for (size_t w = 0; w < 4; ++w) {
      const Measured m = measure(coin ? *coin : empty, order, count, job.published_ms,
                                 job.now_ms, kWindows[w]);
      if (w) out.Byte(',');
      append_window(out, m, coin, order);
      const Measured& b = bench[w];
      const bool aligned = !is_bitcoin && m.baseline >= 0 && m.endpoint >= 0 && b.baseline >= 0 &&
          b.endpoint >= 0 &&
          fabs(coin->samples[order[m.baseline]].at - btc->samples[btc_order[b.baseline]].at) <= 60000 &&
          fabs(coin->samples[order[m.endpoint]].at - btc->samples[btc_order[b.endpoint]].at) <= 60000;
      out.Text(",\"benchmarkReturnPct\":");
      if (aligned) hb::AppendJsNumber(out, b.return_pct); else out.Text("null");
      out.Text(",\"benchmarkBaseline\":");
      if (aligned) append_raw(out, *btc, btc_order, b.baseline); else out.Text("null");
      out.Text(",\"benchmarkEndpoint\":");
      if (aligned) append_raw(out, *btc, btc_order, b.endpoint); else out.Text("null");
      out.Text(",\"differencePp\":");
      // aligned implies both measured, so both returns are numbers.
      if (aligned) hb::AppendJsNumber(out, m.return_pct - b.return_pct); else out.Text("null");
      out.Byte('}');
    }
    out.Text("]}");
  }
  out.Text("]}");
  for (size_t i = 0; i < job.coin_count; ++i) orders[i].reset();
  const bool built = out.ok && memory;
  discovery::DiscoveryCache* cache = workspace->cache;
  const int64_t now = route_now(route);
  if (built) {
    int64_t as_of = 0;
    cache->Write(job.cache_key, kTtlReactions, now, out.data, out.used, &as_of);
  }
  for (size_t i = 0; i < job.waiter_count; ++i) {
    const State::Waiter& waiter = job.waiters[i];
    http::HttpResponse response;
    if (!built) {
      response = json_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    } else {
      // The protobuf/JSON choice was taken from the waiter's own Accept.
      http::HttpRequest synthetic{};
      synthetic.accept = waiter.protobuf ? "application/protobuf" : "application/json";
      synthetic.if_none_match = waiter.has_if_none_match ? waiter.if_none_match : nullptr;
      Arena arena{workspace->arena, sizeof(workspace->arena)};
      response = conditional(synthetic, "news-reactions", out.data, out.used, reactions_proto,
                             workspace, &arena, kFeedCache);
    }
    response.rate_limit_cost = waiter.cost;
    response.rate_limit_remaining = waiter.remaining;
    route->handoff->Complete(waiter.token, response);
  }
  free_job(job);
}

void ReactionService::StartJob(State::Job& job, const Cluster& cluster, int64_t now) {
  auto* state = this;
  job.published_ms = static_cast<int64_t>(cluster.published_ms);
  job.now_ms = now;
  job.from_ms = job.published_ms - 1800000;
  job.to_ms = now < job.published_ms + kDayMs ? now : job.published_ms + kDayMs;
  job.ticker_count = cluster.ticker_count;
  if (cluster.ticker_count) {
    try { job.tickers = std::make_unique<State::TickerCopy[]>(cluster.ticker_count); }
    catch (const std::bad_alloc&) {
      for (size_t i = 0; i < job.waiter_count; ++i) {
        auto response = json_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
        response.rate_limit_cost = job.waiters[i].cost;
        response.rate_limit_remaining = job.waiters[i].remaining;
        state->route->handoff->Complete(job.waiters[i].token, response);
      }
      free_job(job);
      return;
    }
  }
  // ids: distinct truthy coin ids, the first six; then bitcoin.
  for (size_t t = 0; t < cluster.ticker_count; ++t) {
    const fortis::news::Ticker& ticker = cluster.tickers[t];
    State::TickerCopy& copy = job.tickers[t];
    copy.symbol = base::AllocateStorage<uint16_t[]>((ticker.symbol.length + 1) * 2);
    if (copy.symbol) {
      memcpy(copy.symbol.get(), ticker.symbol.data, ticker.symbol.length * 2);
      copy.symbol_length = ticker.symbol.length;
    }
    if (ticker.has_coin_id) {
      OutBuffer id{reinterpret_cast<uint8_t*>(copy.coin_id), sizeof(copy.coin_id) - 1};
      fortis::news::AppendUtf8(id, ticker.coin_id);
      copy.coin_id[id.ok ? id.used : 0] = '\0';
      copy.has_coin_id = id.ok;
    }
  }
  auto add_coin = [&](const char* id) {
    for (size_t i = 0; i < job.coin_count; ++i) if (strcmp(job.coins[i].id, id) == 0) return;
    if (job.coin_count == kReactionCoins) return;
    State::Coin& coin = job.coins[job.coin_count++];
    snprintf(coin.id, sizeof(coin.id), "%s", id);
    coin.day = (job.from_ms >= 0 ? job.from_ms / kDayMs : (job.from_ms - kDayMs + 1) / kDayMs) * kDayMs;
  };
  size_t distinct = 0;
  for (size_t t = 0; t < job.ticker_count && distinct < 6; ++t) {
    const State::TickerCopy& copy = job.tickers[t];
    if (!copy.has_coin_id || !copy.coin_id[0]) continue;
    const size_t before = job.coin_count;
    add_coin(copy.coin_id);
    if (job.coin_count > before) ++distinct;
  }
  add_coin("bitcoin");
  for (size_t i = 0; i < job.coin_count; ++i) {
    if (job.published_ms > now) { job.coins[i].done = true; continue; }
    state->Advance(job, i);
  }
  bool all = true;
  for (size_t i = 0; i < job.coin_count; ++i)
    all = all && (job.coins[i].done || job.coins[i].failed) && !job.coins[i].waiting;
  if (all && !job.outstanding) state->Finish(job);
}

ReactionService::Job* ReactionService::AcquireJob() {
  auto* state = this;
  for (State::Job& job : state->jobs)
    if (!job.used) { job = State::Job{}; job.used = true; return &job; }
  return nullptr;
}

namespace news_routes_internal {

// GET /v1/news/{id}/reactions — R30. The TypeScript readNewsReactions over
// NewsFacet.reactions, NewsReactions.measure, measureReaction/cleanSamples
// and CoinGeckoNewsSamples. Caller:
// IntelligenceApi.newsReactions() (protobuf; 404 → null).
//
// Order and id rule: exactly GET /v1/news/{id}'s steps 1–4 (router 404,
// raw-byte 400, 414, Origin → bearer → quota, trim/upper/ULID), except that
// a malformed id is 400 {"error":"BAD_REQUEST"} without a detail.
// The story is read directly (no item cache): unknown or expired → 404
// {"error":"NOT_FOUND"}; a record that is not a story → 500 INTERNAL.
// Cache: SWR key news-reactions:v1:<id>:<publishedAt ms>, 60 s fresh and
// 120 s stale, durable. Fresh: served. Stale: served, then measured again in
// the background (one job per key; its sample days follow the same rules).
// Miss: measured while the request waits (handed off, 30 s); concurrent
// misses for one key join the running measurement (up to 8 waiters, each
// answered with its own Accept and If-None-Match); 8 jobs at most, else 503
// {"error":"UPSTREAM_UNAVAILABLE"} (native admission bound).
// Measurement at now: coins = the first six distinct non-empty tagged coin
// ids, then bitcoin; window [publishedAt − 30 min, min(now, publishedAt +
// 24 h)]; a story published after now reads no samples. Per coin, each UTC
// day from the window's first: SWR key news-samples:v1:<coin>:<day ms> (60 s
// fresh, 300 s stale; a stale day is used and revalidated in the
// background) fetched from CoinGecko /coins/<encodeURIComponent(coin)>/
// market_chart/range?vs_currency=usd&from=<day s>&to=<min(day + 1 d,
// now) s> through the coingecko lane (GET retries on 5xx, 408, transport
// failure and an unparseable 2xx: three attempts; a 4xx fails at once). The
// stored day is `body.prices ?? []` re-serialized as JSON.stringify would
// (prices_value; 1e400 → null). A failed fetch is held 10 s and fails the
// coin; a day whose value is not an array, or holds an element that is not
// an array or a string (null, a number, an object), fails the coin (the
// TypeError); otherwise elements in the window are kept. cleanSamples:
// finite positive time not after now, finite positive price, the last
// element of a time wins at the first one's position, ascending. Windows
// 300/900/3600/86400 s: pending when publishedAt + window > now; else
// baseline = last sample at or before publishedAt, endpoint = last at or
// before the target, each within min(5 min, window / 5), endpoint after
// baseline, return (e/b − 1) × 100 when finite; else unavailable.
// Payload: eventId, publishedAt, asOf (now), currency USD, source
// CoinGecko, benchmark {coinId bitcoin, symbol BTC, points, windows}, assets
// per ticker in order {coinId, symbol, points (the exact sample elements),
// windows + benchmarkReturnPct/Baseline/Endpoint/differencePp when the
// asset is not bitcoin and both baselines and both endpoints are within
// 60 s, else null}.
// Answer: 200, cache-control private, max-age=60, JSON (ETag
// W/"news-reactions-…") or NewsReactionsResponse (W/"news-reactions-
// protobuf-…", Vary: Accept), If-None-Match → 304. Protobuf of a sample
// whose time is not an integer is 500 INTERNAL (BigInt throws), as in
// TypeScript.
// Bounds: at most 4,096 kept samples and 256 KiB of sample text per coin
// (beyond either the coin has no points: rule 'reaction sample ceiling');
// a provider body over 512 KiB is a failed fetch; sample JSON nested over
// 64 levels, or holding invalid UTF-8 in a string, is a failed fetch.
// Differences (named rules): those of GET /v1/news/{id} (router 404,
// native parser 400, HEAD 304), the sample
// ceiling, and durable envelopes across a restart.
void handle_reactions(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* route = static_cast<NewsRouteContext*>(opaque);
  if (reject_raw_non_ascii(client, request, head) || reject_long_id(client, request, head)) return;
  State* state = route->reactions;
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, route->attestation_required);
  if (policy.status) {
    const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
    http::HttpResponse response{policy.status, policy.reason, kJsonType,
                          reinterpret_cast<const uint8_t*>(body), strlen(body)};
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    WriteHttpResponse(client, response, head);
    return;
  }
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  char id[27];
  if (!normalize_id(request, id)) {
    send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining);
    return;
  }
  NewsWorkspace* workspace = route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  Arena arena{workspace->arena, sizeof(workspace->arena)};
  const int64_t now = route_now(route);
  Cluster cluster;
  bool found = false;
  if (fortis::news::Item(route->store, id, now, &arena, &cluster, &found) != StoreStatus::kOk) {
    lock.Unlock();
    send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, cost, remaining);
    return;
  }
  if (!found) {
    lock.Unlock();
    send_json(client, 404, "Not Found", "{\"error\":\"NOT_FOUND\"}", head, cost, remaining);
    return;
  }
  char key[128];
  snprintf(key, sizeof(key), "news-reactions:v1:%s:%lld", id,
           static_cast<long long>(cluster.published_ms));
  discovery::DiscoveryCache* cache = workspace->cache;
  const discovery::DiscoveryCacheEntry entry = cache->Read(key, now);
  const bool protobuf = AcceptsProtobuf(request);
  if (entry.state == discovery::DiscoveryCacheState::kFresh || entry.state == discovery::DiscoveryCacheState::kStale) {
    uint8_t* copy = static_cast<uint8_t*>(arena.Alloc(entry.body_length + 1, 1));
    if (copy) {
      memcpy(copy, entry.body, entry.body_length);
      http::HttpResponse response = conditional(request, "news-reactions", copy, entry.body_length,
                                          reactions_proto, workspace, &arena, kFeedCache);
      response.rate_limit_cost = cost;
      response.rate_limit_remaining = remaining;
      WriteHttpResponse(client, response, head);
      if (entry.state == discovery::DiscoveryCacheState::kStale && cache->AllowRevalidate(key, now)) {
        bool running = false;
        for (const State::Job& job : state->jobs)
          running = running || (job.used && strcmp(job.cache_key, key) == 0);
        State::Job* job = running ? nullptr : state->AcquireJob();
        if (job) {
          job->background = true;
          snprintf(job->cache_key, sizeof(job->cache_key), "%s", key);
          memcpy(job->id, id, sizeof(job->id));
          state->StartJob(*job, cluster, now);
        }
      }
      return;
    }
  }
  // A reaction measurement in flight for this story answers this request too.
  State::Job* job = nullptr;
  for (State::Job& candidate : state->jobs)
    if (candidate.used && !candidate.background && strcmp(candidate.cache_key, key) == 0 &&
        candidate.waiter_count < kReactionWaiters) { job = &candidate; break; }
  const bool joined = job != nullptr;
  if (!job) job = state->AcquireJob();
  if (!job) {
    lock.Unlock();
    send_json(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
              cost, remaining);
    return;
  }
  uint64_t token = 0;
  if (!http::ReserveHttpResponse(client, head, kHandoffTimeoutMs, &token)) {
    if (!joined) free_job(*job);
    lock.Unlock();
    send_json(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
              cost, remaining);
    return;
  }
  State::Waiter& waiter = job->waiters[job->waiter_count++];
  waiter = State::Waiter{};
  waiter.token = token;
  waiter.protobuf = protobuf;
  waiter.cost = cost;
  waiter.remaining = remaining;
  if (request.if_none_match &&
      strlen(request.if_none_match) < sizeof(waiter.if_none_match)) {
    waiter.has_if_none_match = true;
    snprintf(waiter.if_none_match, sizeof(waiter.if_none_match), "%s", request.if_none_match);
  }
  if (!joined) {
    snprintf(job->cache_key, sizeof(job->cache_key), "%s", key);
    memcpy(job->id, id, sizeof(job->id));
    state->StartJob(*job, cluster, now);
  }
}

}  // namespace news_routes_internal

ReactionService::~ReactionService() {
  for (Job& job : jobs) if (job.used) free_job(job);
  if (g_news_reactions == this) g_news_reactions = nullptr;
}

NewsReactionsState* CreateNewsReactionsState() {
  return base::New<NewsReactionsState>();
}

void DestroyNewsReactionsState(NewsReactionsState* state) {
  if (!state) return;

  base::Delete(state);
}

namespace news_read_rules {

size_t MeasureReaction(const double* at, const double* price, size_t count, int64_t published_ms,
                       int64_t now_ms, int seconds, double* cleaned_at, double* cleaned_price,
                       NewsReactionWindow* window) {
  State::Coin coin{};
  coin.samples = base::AllocateZeroedStorage<State::Sample[]>(count + 1, sizeof(State::Sample));
  auto order_storage = base::AllocateZeroedStorage<size_t[]>(count + 1, sizeof(size_t));
  auto* order = order_storage.get();
  size_t cleaned = 0;
  if (coin.samples && order) {
    // cleanSamples' filter (consume_day applies it to every kept element).
    for (size_t i = 0; i < count; ++i)
      if (isfinite(at[i]) && at[i] > 0 && at[i] <= static_cast<double>(now_ms) && isfinite(price[i]) &&
          price[i] > 0)
        coin.samples[coin.sample_count++] = State::Sample{at[i], price[i], 0, 0};
    cleaned = clean(coin, order);
    for (size_t i = 0; i < cleaned; ++i) {
      cleaned_at[i] = coin.samples[order[i]].at;
      cleaned_price[i] = coin.samples[order[i]].price;
    }
    const Measured m = measure(coin, order, cleaned, published_ms, now_ms, seconds);
    *window = NewsReactionWindow{};
    window->status = m.status;
    window->return_pct = m.return_pct;
    if (m.status == 2) {
      window->baseline[0] = coin.samples[order[m.baseline]].at;
      window->baseline[1] = coin.samples[order[m.baseline]].price;
      window->endpoint[0] = coin.samples[order[m.endpoint]].at;
      window->endpoint[1] = coin.samples[order[m.endpoint]].price;
    }
  }
  coin.samples.reset();
  order_storage.reset();
  return cleaned;
}

}  // namespace news_read_rules

}  // namespace fortis::news
