// Replays the scripted traffic of tests/corpus/
// operations_telemetry.json into the native ApiTelemetry and compares the
// /errorsz document and the /statusz stats block byte-for-byte with what the
// TypeScript handlers produced at each checkpoint.
#include <assert.h>
#include <atomic>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_telemetry.h"
#include "exception_boundary.h"
#include "telemetry_aggregate.h"
#include "http_server.h"
#include "bounded_json.h"

using namespace fortis::codec;
using namespace fortis::http;
using namespace fortis::operations;

namespace {

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file); free(data); return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

BoundedJsonToken tokens[kBoundedJsonMaxTokens];
BoundedJsonDocument json;

const char* text(uint32_t object, const char* key, char* buffer, size_t capacity) {
  size_t length = 0;
  const uint32_t value = json.FindObjectValue(object, key);
  if (value == kBoundedJsonNoToken || json.token(value)->type == BoundedJsonType::kNull)
    return nullptr;
  assert(json.DecodeString(value, buffer, capacity, &length));
  return buffer;
}

double number(uint32_t object, const char* key) {
  double value = 0;
  assert(json.Number(json.FindObjectValue(object, key), &value));
  return value;
}

// TypeScript's stats block minus databaseOperations10m, which the native
// service does not measure and omits.
void without_database_operations(char* stats) {
  const char key[] = ",\"databaseOperations10m\":[]";
  char* found = strstr(stats, key);
  assert(found);
  memmove(found, found + sizeof(key) - 1, strlen(found + sizeof(key) - 1) + 1);
}

void check_services(uint32_t services) {
  size_t count = 0;
  for (uint32_t key = json.token(services)->first_child; key != kBoundedJsonNoToken;) {
    const uint32_t value = json.token(key)->next_sibling;
    char route[256], service[64];
    size_t length = 0;
    assert(json.DecodeString(key, route, sizeof(route), &length));
    assert(json.DecodeString(value, service, sizeof(service), &length));
    if (strcmp(ServiceForRoute(route), service) != 0) {
      fprintf(stderr, "service for %s: wanted %s, got %s\n", route, service, ServiceForRoute(route));
      assert(false);
    }
    ++count;
    key = json.token(value)->next_sibling;
  }
  printf("serviceForRoute: %zu route templates match\n", count);
  assert(count >= 60);
}

void check_aggregate_overflow() {
  using fortis::operations::detail::RequestTotals;
  using fortis::operations::detail::AddCount;
  constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
  assert(AddCount(max - 2, 2) == max && AddCount(max - 2, 3) == max);
  assert(AddCount(0, max) == max && AddCount(max, 0) == max);
  for (size_t i = 0; i < ApiTelemetry::kDurationBucketCount - 1; ++i) {
    const double bound = fortis::operations::detail::kDurationBoundsMs[i];
    assert(fortis::operations::detail::DurationBucket(bound) == i);
    assert(fortis::operations::detail::DurationBucket(bound + .001) == i + 1);
  }
  RequestTotals owner;
  owner.Add(200, 0, 0, NAN);
  owner.Add(200, 0, 0, INFINITY);
  owner.Add(200, 0, 0, -1);
  owner.Add(200, 0, 0, 0);
  assert(owner.owner_samples == 0 && owner.requests == 4 && owner.histogram[0] == 4);
  owner.Add(200, 0, 0, 1);
  assert(owner.owner_samples == 1 && owner.owner_time == 1);
  RequestTotals left, right;
  left.requests = left.histogram[0] = UINT32_MAX;
  right.requests = right.histogram[0] = 42;
  left.Merge(right);
  assert(left.requests == static_cast<uint64_t>(UINT32_MAX) + 42);
  assert(left.histogram[0] == left.requests);
  left.requests = left.errors = left.client_errors = left.limited = left.owner_samples = max;
  left.histogram[0] = max;
  left.Add(503, 1, 0, 2);
  assert(left.requests == max && left.errors == max && left.histogram[0] == max && left.owner_samples == max);
  left.Add(429, 1, 0, 2);
  assert(left.limited == max && left.client_errors == max);
  right.requests = right.errors = right.client_errors = right.limited = right.owner_samples = max;
  right.histogram[0] = max;
  left.Merge(right);
  assert(left.requests == max && left.errors == max && left.client_errors == max && left.limited == max &&
         left.owner_samples == max && left.histogram[0] == max);
  char output[32]{};
  fortis::codec::history_batch::OutBuffer out{reinterpret_cast<uint8_t*>(output), sizeof(output)};
  fortis::operations::detail::AppendCount(out, max);
  assert(out.ok && out.used == 20 && memcmp(output, "18446744073709551615", 20) == 0);
  out = {reinterpret_cast<uint8_t*>(output), 19};
  fortis::operations::detail::AppendCount(out, max);
  assert(!out.ok);
  out = {reinterpret_cast<uint8_t*>(output), sizeof(output)};
  fortis::operations::detail::AppendCount(out, 0);
  assert(out.ok && out.used == 1 && output[0] == '0');
  assert(ApiTelemetry::StorageBytes() < 4 * 1024 * 1024);
}

void check_collision_and_rollover() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  constexpr int64_t now = 1767324600000LL;
  HttpCompletionTag detail{}; detail.method = "GET";
  // Exercise a worst-case hash chain, at the 50% table fill bound. Interning
  // retains insertion order, recognizes existing routes even at capacity, and
  // refuses a new route without evicting any old route.
  size_t matched = 0;
  char last[64]{};
  for (size_t candidate = 0; matched < ApiTelemetry::kMaxRoutes; ++candidate) {
    char route[64]; snprintf(route, sizeof(route), "/v1/collision/%zu", candidate);
    uint64_t hash = 1469598103934665603ULL;
    for (const char* p = route; *p; ++p) { hash ^= static_cast<unsigned char>(*p); hash *= 1099511628211ULL; }
    if (hash % (2 * ApiTelemetry::kMaxRoutes) != 0) continue;
    telemetry.Record(route, 200, nullptr, 2, now, &detail);
    memcpy(last, route, strlen(route) + 1);
    ++matched;
  }
  telemetry.Record(last, 200, nullptr, 3, now + 60000, &detail);
  telemetry.Record("/v1/table-full", 500, nullptr, 1, now + 60000, &detail);
  assert(telemetry.revision() == ApiTelemetry::kMaxRoutes + 1);
  auto output = std::make_unique<char[]>(ApiTelemetry::kHistoryCapacity + 1);
  size_t length = telemetry.RenderRequestStats(now + 60000, output.get(), ApiTelemetry::kHistoryCapacity);
  assert(length); output[length] = 0;
  assert(!strstr(output.get(), "/v1/table-full"));
  // Reuse both per-minute indexes, including a new method; late completions
  // sharing the circular index must not destroy the current minute.
  detail.method = "POST";
  telemetry.Record(last, 201, nullptr, 4, now + 15 * 60000, &detail);
  telemetry.Record(last, 503, nullptr, 5, now, &detail);
  length = telemetry.RenderRequestStats(now + 15 * 60000, output.get(), ApiTelemetry::kHistoryCapacity);
  assert(length); output[length] = 0;
  assert(strstr(output.get(), "\"method\":\"POST\",\"requests\":1,\"errors5xx\":0"));
  length = telemetry.RenderStatusStats(now + 15 * 60000, output.get(), ApiTelemetry::kHistoryCapacity);
  assert(length); output[length] = 0;
  assert(strstr(output.get(), "\"window5m\":{\"clients\":0,\"requests\":1,\"errors\":0}"));
  // The older completion is still inside the separate eight-hour history.
  length = telemetry.RenderHistory(now + 15 * 60000, output.get(), ApiTelemetry::kHistoryCapacity);
  assert(length); output[length] = 0;
  assert(strstr(output.get(), "\"total\":129,\"errors\":1"));
  ApiTelemetry cold;
  assert(cold.Start());
  length = cold.RenderHistory(0, output.get(), ApiTelemetry::kHistoryCapacity);
  assert(length); output[length] = 0;
  assert(!strstr(output.get(), "\"observed\":true"));
}

void check_snapshot_readers_and_shutdown() {
  // Every user is joined before the object is destroyed; no reader snapshot or
  // borrowed completion pointer may outlive its owner, even while rolling slots.
  for (int cycle = 0; cycle < 3; ++cycle) {
    ApiTelemetry telemetry;
    assert(telemetry.Start());
    std::atomic<bool> start{false};
    std::vector<std::thread> users;
    users.reserve(7);
    constexpr int64_t now = 1767324600000LL;
    for (int writer = 0; writer < 4; ++writer) users.emplace_back([&] {
      HttpCompletionTag detail{}; detail.method = "GET";
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (int i = 0; i < 4000; ++i)
        telemetry.Record("/v1/networks", i % 2 ? 200 : 503, "ephemeral", 2, now, &detail);
    });
    for (int reader = 0; reader < 3; ++reader) users.emplace_back([&] {
      auto output = std::make_unique<char[]>(ApiTelemetry::kHistoryCapacity + 1);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      auto local_tokens = std::make_unique<BoundedJsonToken[]>(kBoundedJsonMaxTokens);
      BoundedJsonDocument document;
      for (int i = 0; i < 40; ++i) {
        size_t length = telemetry.RenderHistory(now, output.get(), ApiTelemetry::kHistoryCapacity);
        assert(length && output[0] == '{' && output[length - 1] == '}');
        assert(telemetry.RenderStatusStats(now, output.get(), ApiTelemetry::kHistoryCapacity));
        length = telemetry.RenderRequestStats(now, output.get(), ApiTelemetry::kHistoryCapacity);
        assert(length);
        BoundedJsonError error = BoundedJsonError::kNone;
        assert(document.Parse(reinterpret_cast<const uint8_t*>(output.get()), length,
                              local_tokens.get(), kBoundedJsonMaxTokens, &error));
        const uint32_t routes = document.FindObjectValue(document.root(), "routes");
        const uint32_t platforms = document.FindObjectValue(document.root(), "platforms");
        const uint32_t route = document.token(routes)->first_child;
        if (route != kBoundedJsonNoToken) {
          double route_count = 0, platform_count = 0;
          assert(document.Number(document.FindObjectValue(route, "requests"), &route_count));
          assert(document.Number(document.FindObjectValue(document.token(platforms)->first_child, "requests"), &platform_count));
          assert(route_count == platform_count);
        }
        assert(telemetry.RenderErrorsz(now, output.get(), ApiTelemetry::kHistoryCapacity));
        const double cpu = i;
        telemetry.SampleResources(now, nullptr, &cpu);
      }
    });
    start.store(true, std::memory_order_release);
    for (auto& user : users) user.join();
    assert(telemetry.revision() == 16000);
    char output[ApiTelemetry::kStatsCapacity + 1];
    const size_t length = telemetry.RenderStatusStats(now, output, sizeof(output) - 1);
    assert(length); output[length] = 0;
    assert(strstr(output, "\"window5m\":{\"clients\":1,\"requests\":16000,\"errors\":8000}"));
  }
}

void check_group_percentiles() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  constexpr int64_t now = 1767323045000LL;
  HttpCompletionTag tag{}; tag.method = "GET";
  for (int i = 0; i < 99; ++i) telemetry.Record("/v1/prices", 200, nullptr, 10, now, &tag);
  telemetry.Record("/v1/prices/history", 503, nullptr, 1000, now, &tag);
  telemetry.Record("/v1/ai/conversations", 201, nullptr, 20, now, &tag);
  static char output[ApiTelemetry::kRequestStatsCapacity + 1];
  const size_t length = telemetry.RenderRequestStats(now, output, sizeof(output) - 1);
  assert(length); output[length] = 0;
  const char* groups = strstr(output, "\"groups\":[");
  assert(groups && strstr(groups, "\"service\":\"Prices\",\"requests\":100,\"errors5xx\":1"));
  const char* prices = strstr(groups, "\"service\":\"Prices\"");
  const char* next = strchr(prices, '}');
  const char* p95 = strstr(prices, "\"p95Ms\":10,");
  assert(next && p95 && p95 < next); // Merge histograms; averaging route p95s would give 505 ms.
  assert(strstr(groups, "\"service\":\"AI\",\"requests\":1"));
  assert(telemetry.RenderRequestStats(now, output, 16) == 0);
}

void check_group_coverage_bounds() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  constexpr int64_t now = 1767323045000LL;
  constexpr const char* methods[] = {"GET", "POST", "DELETE"};
  for (int i = 0; i < 128; ++i) {
    char route[64]; snprintf(route, sizeof(route), "/v1/bounded/route-%d", i);
    for (const char* method : methods) {
      HttpCompletionTag detail{}; detail.method = method;
      telemetry.Record(route, 200, nullptr, 1, now, &detail);
    }
  }
  static char output[ApiTelemetry::kRequestStatsCapacity + 1];
  size_t length = telemetry.RenderRequestStats(now, output, sizeof(output) - 1);
  assert(length); output[length] = 0;
  assert(strstr(output, "\"omittedRouteSamples\":128"));
  const char* groups = strstr(output, "\"groups\":[");
  assert(groups && strstr(groups, "\"service\":\"API\",\"requests\":256"));
  const char* platforms = strstr(output, "\"platforms\":[");
  assert(platforms && strstr(platforms, "\"os\":\"unknown\",\"requests\":384"));
  length = telemetry.RenderRequestStats(now + 15 * 60000, output, sizeof(output) - 1);
  assert(length); output[length] = 0;
  assert(strstr(output, "\"omittedRouteSamples\":0"));
}

void check_request_stats() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  constexpr int64_t now = 1767323045000LL;
  HttpCompletionTag tag{};
  tag.method = "GET";
  tag.platform = 2;
  tag.admission = HttpAdmission::SpotOwner;
  tag.queue_ms = 17;
  telemetry.Record("/v1/prices", 503, "private-device-marker", 42, now, &tag);
  tag.method = "HEAD";
  tag.platform = 1;
  tag.admission = HttpAdmission::None;
  telemetry.Record("/v1/prices", 429, nullptr, 8, now, &tag);
  telemetry.NoteAdmission(HttpAdmission::ConnectionQueue);
  HttpCompletionTag expired{};
  expired.admission = HttpAdmission::ConnectionQueueExpired;
  ApiTelemetry::OnHttpDetails(&telemetry, expired, 503, 0);
  static char output[ApiTelemetry::kRequestStatsCapacity + 1];
  size_t size = telemetry.RenderRequestStats(now, output, sizeof(output) - 1);
  assert(size);
  output[size] = 0;
  assert(strstr(output, "\"method\":\"GET\",\"requests\":1,\"errors5xx\":1"));
  assert(strstr(output, "\"method\":\"HEAD\",\"requests\":1,\"errors5xx\":0,\"errors4xx\":1,\"rateLimited\":1"));
  assert(strstr(output, "\"os\":\"Android\",\"requests\":1"));
  assert(strstr(output, "\"avgQueueMs\":17"));
  assert(strstr(output, "\"connectionQueue\":1"));
  assert(strstr(output, "\"connectionQueueExpired\":1"));
  assert(strstr(output, "\"spotOwner\":1"));
  assert(!strstr(output, "private-device-marker"));
  size = telemetry.RenderRequestStats(now + 15 * 60000, output, sizeof(output) - 1);
  assert(size);
  output[size] = 0;
  assert(strstr(output, "\"routes\":[]"));
  assert(strstr(output, "\"connectionQueue\":1"));
}

void check_scope_and_bounds() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  const int64_t now = 1767323045000LL;
  static_cast<void>(now);
  // Like the apiPlugin-scoped onResponse hook, the router adapter records
  // only /v1 routes outside the root-scoped media proxy.
  assert(!ApiTelemetry::TracksRoute("/statusz") && !ApiTelemetry::TracksRoute("/v1/media/:key"));
  assert(!ApiTelemetry::TracksRoute("/") && !ApiTelemetry::TracksRoute("/v1"));
  assert(ApiTelemetry::TracksRoute("/v1/crash-reports"));
  ApiTelemetry::OnHttpCompletion(&telemetry, "/statusz", 500, nullptr, 1);
  ApiTelemetry::OnHttpCompletion(&telemetry, "/", 200, nullptr, 1);
  ApiTelemetry::OnHttpCompletion(&telemetry, "/v1/media/:key", 500, nullptr, 1);
  ApiTelemetry::OnHttpCompletion(&telemetry, "/errorsz", 500, nullptr, 1);
  ApiTelemetry::OnHttpCompletion(&telemetry, "/v1/networks", 503, "device-a", 2.5);
  static char output[ApiTelemetry::kErrorszCapacity];
  timespec wall{};
  clock_gettime(CLOCK_REALTIME, &wall);
  const int64_t wall_ms = static_cast<int64_t>(wall.tv_sec) * 1000 + wall.tv_nsec / 1000000;
  size_t length = telemetry.RenderErrorsz(wall_ms, output, sizeof(output));
  assert(length);
  output[length] = '\0';
  assert(strstr(output, "\"totalErrors\":1,\"totalRequests\":1,"));
  assert(strstr(output, "\"route\":\"/v1/networks\""));
  assert(!strstr(output, "/statusz") && !strstr(output, "/errorsz") && !strstr(output, "media"));
  // Device ids are counted, never rendered.
  length = telemetry.RenderStatusStats(wall_ms, output, sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"window5m\":{\"clients\":1,") && !strstr(output, "device-a"));

  // A minute with more distinct devices than the per-slot bound counts up to
  // the bound (a lower bound), never more, and stays renderable.
  ApiTelemetry crowded;
  assert(crowded.Start());
  char id[32];
  for (int i = 0; i < 3000; ++i) {
    snprintf(id, sizeof(id), "device-%d", i);
    crowded.Record("/v1/sync", 200, id, 1, now);
  }
  length = crowded.RenderStatusStats(now, output, sizeof(output));
  output[length] = '\0';
  char expected[96];
  snprintf(expected, sizeof(expected), "\"window5m\":{\"clients\":%zu,\"requests\":3000,",
           ApiTelemetry::kMaxDevicesPerMinute - 1);
  assert(strstr(output, expected));

  // Every route template the router can hold still renders inside the bound,
  // with the recent log full of the longest templates.
  ApiTelemetry wide;
  assert(wide.Start());
  char route[ApiTelemetry::kMaxRouteBytes + 1];
  for (size_t i = 0; i < ApiTelemetry::kMaxRoutes; ++i) {
    memset(route, 'x', sizeof(route) - 1);
    route[sizeof(route) - 1] = '\0';
    char prefix[16];
    const int prefix_length = snprintf(prefix, sizeof(prefix), "/v1/r%03zu/", i);
    memcpy(route, prefix, static_cast<size_t>(prefix_length));
    wide.Record(route, 500, nullptr, 1, now);
  }
  wide.Record("/v1/overflow-route", 500, nullptr, 1, now);  // past kMaxRoutes: dropped
  length = wide.RenderErrorsz(now, output, sizeof(output));
  assert(length > 0 && length <= sizeof(output));
  output[length < sizeof(output) ? length : sizeof(output) - 1] = '\0';
  assert(!strstr(output, "overflow-route"));
  printf("errorsz with %zu over-long routes: %zu bytes (bound %zu)\n",
         ApiTelemetry::kMaxRoutes, length, ApiTelemetry::kErrorszCapacity);
  // Totals count every interned template; the per-minute route list keeps
  // the first kMaxRoutesPerMinute of them.
  assert(strstr(output, "\"totalErrors\":128,\"totalRequests\":128,"));
}

struct ConcurrentContext {
  ApiTelemetry* telemetry;
  int index;
};

void* concurrent_writer(void* opaque) {
  auto* context = static_cast<ConcurrentContext*>(opaque);
  char id[32];
  for (int i = 0; i < 5000; ++i) {
    snprintf(id, sizeof(id), "writer-%d", context->index);
    context->telemetry->Record("/v1/networks", i % 7 ? 200 : 502, id, 0.5, 1767323045000LL);
  }
  return nullptr;
}

void check_concurrency() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  pthread_t threads[4];
  ConcurrentContext contexts[4];
  for (int i = 0; i < 4; ++i) {
    contexts[i] = {&telemetry, i};
    assert(pthread_create(&threads[i], nullptr, concurrent_writer, &contexts[i]) == 0);
  }
  static char output[ApiTelemetry::kErrorszCapacity];
  for (int i = 0; i < 200; ++i) assert(telemetry.RenderErrorsz(1767323045000LL, output, sizeof(output)));
  for (pthread_t thread : threads) pthread_join(thread, nullptr);
  assert(telemetry.revision() == 20000);
  const size_t length = telemetry.RenderStatusStats(1767323045000LL, output, sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"window5m\":{\"clients\":4,\"requests\":20000,"));
}

void check_compact_history() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  const int64_t base = 1767323040000LL;
  const double memory = 120.0, cpu = 12.5, disk = 42;
  telemetry.SampleResources(base, &memory, &cpu, &disk);
  telemetry.Record("/v1/networks", 200, "private-device", 7, base + 1000);
  telemetry.Record("/v1/networks", 503, nullptr, 13, base + 2000);
  static char history[ApiTelemetry::kHistoryCapacity + 1];
  size_t size = telemetry.RenderHistory(base + 3000, history, ApiTelemetry::kHistoryCapacity);
  assert(size && size < ApiTelemetry::kHistoryCapacity);
  history[size] = '\0';
  assert(strstr(history, "\"maxMinutes\":480,\"slots\":["));
  assert(strstr(history, "\"observed\":true"));
  assert(strstr(history, "\"observed\":false"));
  assert(strstr(history, "\"diskUsedMb\":{\"min\":42,\"avg\":42,\"max\":42,\"samples\":1}"));
  assert(strstr(history, "\"memoryMb\":{\"min\":120,\"avg\":120,\"max\":120,\"samples\":1}"));
  assert(strstr(history, "\"cpuPercent\":{\"min\":12.5,\"avg\":12.5,\"max\":12.5,\"samples\":1}"));
  assert(strstr(history, "\"requests\":{\"total\":2,\"errors\":1,\"avgDurationMs\":10,\"maxDurationMs\":13}"));
  assert(!strstr(history, "private-device") && !strstr(history, "/v1/networks"));
  assert(telemetry.RenderHistory(base, history, 16) == 0);
  const double tree = 2;
  telemetry.SampleResources(base + 4000, nullptr, nullptr, &tree, false);
  size = telemetry.RenderHistory(base + 5000, history, ApiTelemetry::kHistoryCapacity);
  assert(size); history[size] = 0;
  assert(strstr(history, "\"diskScope\":\"data-tree\",\"diskUsedMb\":{\"min\":2,\"avg\":2,\"max\":2,\"samples\":1}"));
  // The same ring index is reused at 480 minutes, without retaining stale
  // measurements or accepting an out-of-window late completion.
  const int64_t rolled = base + ApiTelemetry::kHistoryMinutes * 60000;
  telemetry.SampleResources(rolled, &memory, nullptr);
  telemetry.Record("/v1/networks", 200, nullptr, 1, base);
  size = telemetry.RenderHistory(rolled, history, ApiTelemetry::kHistoryCapacity);
  assert(size);
  history[size] = '\0';
  assert(strstr(history, "\"memoryMb\":{\"min\":120"));
  assert(strstr(history, "\"cpuPercent\":null,\"diskScope\":null,\"diskUsedMb\":null,\"requests\":{\"total\":0"));
  printf("compact history: 480 slots, aggregates, bounds and rollover OK\n");
}

void check_full_history_serialization() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  constexpr int64_t now = 1767324600000LL;
  const double longest = 0.0000012345678901234567;
  for (int age = 479; age >= 0; --age) {
    const int64_t instant = now - age * 60000;
    telemetry.SampleResources(instant, &longest, &longest, &longest);
    telemetry.Record("/v1/networks", 200, nullptr, longest, instant);
  }
  auto output = std::make_unique<char[]>(ApiTelemetry::kHistoryCapacity + 1);
  const size_t length = telemetry.RenderHistory(now, output.get(), ApiTelemetry::kHistoryCapacity);
  assert(length && length <= ApiTelemetry::HistoryWorstCaseBytes()); output[length] = 0;
  // Exact-fit succeeds; a short destination must fail without crossing its bound.
  assert(telemetry.RenderHistory(now, output.get(), length) == length);
  assert(telemetry.RenderHistory(now, output.get(), length - 1) == 0);
  assert(telemetry.RenderHistory(now, nullptr, ApiTelemetry::kHistoryCapacity) == 0);
}

// ---- /statusz stats block: windows, bounds and malformed samples ----------

BoundedJsonToken stats_tokens[kBoundedJsonMaxTokens];
BoundedJsonDocument stats_json;
char stats_text[ApiTelemetry::kStatsCapacity + 1];
size_t stats_length = 0;

// Renders and parses the stats block; every render must be valid JSON.
void render_stats(ApiTelemetry& telemetry, int64_t now) {
  stats_length = telemetry.RenderStatusStats(now, stats_text, ApiTelemetry::kStatsCapacity);
  assert(stats_length > 0 && stats_length <= ApiTelemetry::StatusStatsWorstCaseBytes());
  stats_text[stats_length] = '\0';
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(stats_json.Parse(reinterpret_cast<const unsigned char*>(stats_text), stats_length,
                          stats_tokens, kBoundedJsonMaxTokens, &error));
}

uint32_t stats_node(const char* key) {
  const uint32_t node = stats_json.FindObjectValue(stats_json.root(), key);
  assert(node != kBoundedJsonNoToken);
  return node;
}

uint32_t element(uint32_t array, size_t index) {
  uint32_t node = stats_json.token(array)->first_child;
  for (size_t i = 0; i < index && node != kBoundedJsonNoToken; ++i)
    node = stats_json.token(node)->next_sibling;
  return node;
}

size_t element_count(uint32_t array) {
  size_t count = 0;
  for (uint32_t node = stats_json.token(array)->first_child; node != kBoundedJsonNoToken;
       node = stats_json.token(node)->next_sibling)
    ++count;
  return count;
}

double field(uint32_t object, const char* key) {
  double value = -1;
  const uint32_t node = stats_json.FindObjectValue(object, key);
  assert(node != kBoundedJsonNoToken && stats_json.Number(node, &value));
  return value;
}

bool field_is_null(uint32_t object, const char* key) {
  const uint32_t node = stats_json.FindObjectValue(object, key);
  return node != kBoundedJsonNoToken && stats_json.token(node)->type == BoundedJsonType::kNull;
}

double window(const char* name, const char* key) { return field(stats_node(name), key); }

const char* top_route(size_t index, char* buffer, size_t capacity) {
  const uint32_t entry = element(stats_node("topRoutes10m"), index);
  assert(entry != kBoundedJsonNoToken);
  size_t length = 0;
  assert(stats_json.DecodeString(stats_json.FindObjectValue(entry, "route"), buffer, capacity,
                                 &length));
  return buffer;
}

// 2026-01-02T03:30:00Z, the start of an epoch minute.
constexpr int64_t kMinuteStart = 1767324600000LL;
constexpr int64_t kMinute = 60000;

void check_status_cold_and_unstarted() {
  // Never started: nothing renders and Record is a no-op.
  ApiTelemetry unstarted;
  char tiny[64];
  assert(unstarted.RenderStatusStats(kMinuteStart, tiny, sizeof(tiny)) == 0);
  unstarted.Record("/v1/sync", 200, "device", 1, kMinuteStart);
  assert(unstarted.revision() == 0);

  // Cold: the exact TypeScript shape (operations_pages.json without
  // databaseOperations10m) with 15 explicit zero minutes, oldest first.
  ApiTelemetry cold;
  assert(cold.Start());
  const int64_t now = kMinuteStart + 59999;
  char expected[4096];
  size_t used = static_cast<size_t>(snprintf(expected, sizeof(expected),
      "{\"window5m\":{\"clients\":0,\"requests\":0,\"errors\":0},"
      "\"window10m\":{\"clients\":0,\"requests\":0,\"errors\":0},"
      "\"topRoutes10m\":[],\"perMinute\":["));
  for (int age = 14; age >= 0; --age)
    used += static_cast<size_t>(snprintf(expected + used, sizeof(expected) - used,
        "%s{\"startSec\":%lld,\"requests\":0,\"errors\":0,\"clients\":0}", age == 14 ? "" : ",",
        static_cast<long long>(kMinuteStart / 1000 - age * 60)));
  snprintf(expected + used, sizeof(expected) - used, "]}");
  render_stats(cold, now);
  assert(strcmp(stats_text, expected) == 0);
  // Renders are reads: the revision stays put and the bytes repeat.
  assert(cold.revision() == 0);
  render_stats(cold, now);
  assert(strcmp(stats_text, expected) == 0);
  // All-or-nothing output: exact fit works, one byte short fails cleanly.
  char exact[4096];
  const size_t length = strlen(expected);
  memset(exact, '#', sizeof(exact));
  assert(cold.RenderStatusStats(now, exact, length) == length);
  assert(memcmp(exact, expected, length) == 0 && exact[length] == '#');
  assert(cold.RenderStatusStats(now, exact, length - 1) == 0);
  assert(cold.RenderStatusStats(now, nullptr, sizeof(exact)) == 0);
}

void check_status_window_boundaries() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  const int64_t now_minute = kMinuteStart / kMinute;
  // Minute (now - age) carries age + 1 requests, so every window sum is a
  // distinct number. Age 15 shares a ring slot with age 0 and must vanish.
  for (int age = 15; age >= 0; --age)
    for (int i = 0; i <= age; ++i)
      telemetry.Record("/v1/prices", 200, nullptr, 1, (now_minute - age) * kMinute + i * 1000);
  const auto sum = [](int from_age, int to_age) {
    double total = 0;
    for (int age = from_age; age <= to_age; ++age) total += age + 1;
    return total;
  };
  // Windows are whole epoch minutes: the current one plus the previous 4/9.
  const int64_t instants[] = {kMinuteStart, kMinuteStart + 30000, kMinuteStart + kMinute - 1};
  for (const int64_t now : instants) {
    render_stats(telemetry, now);
    assert(window("window5m", "requests") == sum(0, 4));
    assert(window("window10m", "requests") == sum(0, 9));
    const uint32_t minutes = stats_node("perMinute");
    assert(element_count(minutes) == 15);
    for (size_t i = 0; i < 15; ++i) {
      const uint32_t point = element(minutes, i);
      const int age = 14 - static_cast<int>(i);
      assert(field(point, "startSec") == static_cast<double>((now_minute - age) * 60));
      assert(field(point, "requests") == age + 1);
    }
    // Age 15 was overwritten in place by age 0, never added to it.
    assert(field(element(minutes, 14), "requests") == 1);
  }
  // At the first millisecond of the next minute every window slides by one
  // whole minute: the 5-minute window loses the minute that was age 4 and the
  // 10-minute window the one that was age 9 (ages below are pre-slide).
  render_stats(telemetry, kMinuteStart + kMinute);
  assert(window("window5m", "requests") == sum(0, 3));
  assert(window("window10m", "requests") == sum(0, 8));
  assert(field(element(stats_node("perMinute"), 14), "requests") == 0);
  assert(field(element(stats_node("perMinute"), 0), "requests") == 14);
  // Ten quiet minutes later only the per-minute series still has traffic;
  // fifteen later nothing does.
  render_stats(telemetry, kMinuteStart + 10 * kMinute);
  assert(window("window10m", "requests") == 0 && window("window5m", "requests") == 0);
  assert(element_count(stats_node("topRoutes10m")) == 0);
  assert(field(element(stats_node("perMinute"), 0), "requests") == 5);
  render_stats(telemetry, kMinuteStart + 15 * kMinute);
  for (size_t i = 0; i < 15; ++i)
    assert(field(element(stats_node("perMinute"), i), "requests") == 0);

  // A sample one millisecond before a minute starts belongs to the earlier
  // minute (floor), so it leaves the 5-minute window one minute sooner.
  ApiTelemetry edge;
  assert(edge.Start());
  edge.Record("/v1/sync", 200, nullptr, 1, kMinuteStart - 4 * kMinute);      // age 4
  edge.Record("/v1/sync", 200, nullptr, 1, kMinuteStart - 4 * kMinute - 1);  // age 5
  render_stats(edge, kMinuteStart);
  assert(window("window5m", "requests") == 1 && window("window10m", "requests") == 2);
}

void check_status_errors_clients_and_routes() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  const int64_t now = kMinuteStart + 1000;
  // Only status >= 500 is an error; 0 (never answered) and 4xx are not.
  const int statuses[] = {0, 200, 404, 429, 499, 500, 503, 599};
  for (int status : statuses) telemetry.Record("/v1/sync", status, nullptr, 1, now);
  render_stats(telemetry, now);
  assert(window("window5m", "requests") == 8 && window("window5m", "errors") == 3);

  // Distinct devices are a union across minutes, not a per-minute sum.
  ApiTelemetry devices;
  assert(devices.Start());
  struct Visit { const char* device; int age; } const visits[] = {
      {"d1", 0}, {"d1", 1}, {"d1", 1}, {"d3", 1}, {"d2", 6}, {"d3", 7}, {"d4", 12},
      {"", 0}, {nullptr, 0}};
  for (const Visit& visit : visits)
    devices.Record("/v1/networks", 200, visit.device, 1, kMinuteStart - visit.age * kMinute);
  render_stats(devices, kMinuteStart);
  assert(window("window5m", "clients") == 2 && window("window10m", "clients") == 3);
  assert(window("window5m", "requests") == 6);
  const uint32_t minutes = stats_node("perMinute");
  assert(field(element(minutes, 14), "clients") == 1);  // d1 (empty/null ids are no client)
  assert(field(element(minutes, 13), "clients") == 2);  // d1, d3
  assert(field(element(minutes, 2), "clients") == 1);   // d4, outside both windows
  assert(!strstr(stats_text, "\"d1\"") && !strstr(stats_text, "d3"));

  // Pseudo-random population checked against a brute-force union of ids.
  ApiTelemetry crowd;
  assert(crowd.Start());
  static bool seen5[4096], seen10[4096];
  memset(seen5, 0, sizeof(seen5));
  memset(seen10, 0, sizeof(seen10));
  uint32_t state = 12345;
  char id[32];
  for (int age = 14; age >= 0; --age) {
    for (int i = 0; i < 300; ++i) {
      state = state * 1103515245u + 12345u;
      const unsigned device = (state >> 8) % 3000;
      snprintf(id, sizeof(id), "pool-%u", device);
      crowd.Record("/v1/sync", 200, id, 1, kMinuteStart - age * kMinute + i);
      if (age < 5) seen5[device] = true;
      if (age < 10) seen10[device] = true;
    }
  }
  size_t want5 = 0, want10 = 0;
  for (size_t i = 0; i < 4096; ++i) { want5 += seen5[i]; want10 += seen10[i]; }
  render_stats(crowd, kMinuteStart);
  assert(window("window5m", "clients") == static_cast<double>(want5));
  assert(window("window10m", "clients") == static_cast<double>(want10));

  // Saturated slots: 1100 distinct devices per minute keep kMaxDevicesPerMinute
  // - 1 each. Disjoint minutes add up; the same ids every minute do not.
  ApiTelemetry full, same;
  assert(full.Start() && same.Start());
  for (int age = 9; age >= 0; --age) {
    for (int i = 0; i < 1100; ++i) {
      snprintf(id, sizeof(id), "m%d-%d", age, i);
      full.Record("/v1/sync", 200, id, 1, kMinuteStart - age * kMinute);
      snprintf(id, sizeof(id), "same-%d", i);
      same.Record("/v1/sync", 200, id, 1, kMinuteStart - age * kMinute);
    }
  }
  const double cap = static_cast<double>(ApiTelemetry::kMaxDevicesPerMinute - 1);
  render_stats(full, kMinuteStart);
  assert(window("window5m", "clients") == 5 * cap && window("window10m", "clients") == 10 * cap);
  render_stats(same, kMinuteStart);
  // Each minute kept the same first 1023 ids, so the union is exactly one cap.
  assert(window("window10m", "clients") == cap);
  assert(window("window10m", "requests") == 11000);

  // Top routes: at most five, most requests first, ties in first-seen order
  // of the newest minute (JavaScript Map insertion order).
  ApiTelemetry routes;
  assert(routes.Start());
  struct Hits { const char* route; int count; } const hits[] = {
      {"/v1/a", 2}, {"/v1/b", 7}, {"/v1/c", 2}, {"/v1/d", 5}, {"/v1/e", 1}, {"/v1/f", 5},
      {"/v1/g", 2}};
  for (const Hits& hit : hits)
    for (int i = 0; i < hit.count; ++i) routes.Record(hit.route, 200, nullptr, 1, kMinuteStart);
  render_stats(routes, kMinuteStart);
  assert(element_count(stats_node("topRoutes10m")) == 5);
  const char* const order[] = {"/v1/b", "/v1/d", "/v1/f", "/v1/a", "/v1/c"};
  char name[160];
  for (size_t i = 0; i < 5; ++i) assert(strcmp(top_route(i, name, sizeof(name)), order[i]) == 0);
}

void check_status_malformed_samples() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  const int64_t now = kMinuteStart;
  // Dropped without a revision bump: null, empty and over-long templates.
  char long_route[ApiTelemetry::kMaxRouteBytes + 2];
  memset(long_route, 'x', sizeof(long_route) - 1);
  long_route[sizeof(long_route) - 1] = '\0';
  memcpy(long_route, "/v1/", 4);
  telemetry.Record(nullptr, 500, "d", 1, now);
  telemetry.Record("", 500, "d", 1, now);
  telemetry.Record(long_route, 500, "d", 1, now);
  assert(telemetry.revision() == 0);
  long_route[ApiTelemetry::kMaxRouteBytes] = '\0';  // exactly the bound: kept
  telemetry.Record(long_route, 200, nullptr, 1, now);
  assert(telemetry.revision() == 1);
  render_stats(telemetry, now);
  assert(window("window5m", "requests") == 1 && strstr(stats_text, long_route));

  // Durations: negative and NaN count as 0 ms (bucket 0); +Inf is kept and,
  // like JSON.stringify(Infinity), renders null; huge finite values stay
  // finite numbers.
  ApiTelemetry durations;
  assert(durations.Start());
  durations.Record("/v1/neg", 200, nullptr, -50, now);
  durations.Record("/v1/neg", 200, nullptr, NAN, now);
  durations.Record("/v1/inf", 200, nullptr, INFINITY, now);
  durations.Record("/v1/big", 200, nullptr, 1.7e308, now);
  durations.Record("/v1/tiny", 200, nullptr, 0.0000012345678901234567, now);
  render_stats(durations, now);
  const uint32_t top = stats_node("topRoutes10m");
  const uint32_t neg = element(top, 0);
  assert(field(neg, "durationMs") == 0 && field(neg, "maxDurationMs") == 0);
  assert(field(neg, "durationP95Ms") == 5);
  const uint32_t buckets = stats_json.FindObjectValue(neg, "durationBuckets");
  assert(element_count(buckets) == ApiTelemetry::kDurationBucketCount);
  double first_bucket = 0;
  assert(stats_json.Number(element(buckets, 0), &first_bucket) && first_bucket == 2);
  const uint32_t inf = element(top, 1);
  assert(field_is_null(inf, "durationMs") && field_is_null(inf, "maxDurationMs") &&
         field_is_null(inf, "durationP95Ms"));
  assert(field(element(top, 2), "durationMs") == 1.7e308);
  assert(strstr(stats_text, "\"durationMs\":0.0000012345678901234567,"));

  // The largest output the renderer can produce: five 127-byte templates of
  // control characters (6-byte escapes), long durations, all 15 minutes.
  ApiTelemetry heavy;
  assert(heavy.Start());
  char route[ApiTelemetry::kMaxRouteBytes + 1];
  for (int r = 0; r < 6; ++r) {
    memset(route, 0x01, sizeof(route) - 1);
    route[sizeof(route) - 1] = '\0';
    route[0] = '/'; route[1] = 'v'; route[2] = '1'; route[3] = '/';
    route[4] = static_cast<char>('a' + r);
    for (int age = 14; age >= 0; --age)
      for (int i = 0; i < 3; ++i)
        heavy.Record(route, 503, "dev", 0.0000012345678901234567 * (i + 1),
                     kMinuteStart - age * kMinute);
  }
  render_stats(heavy, kMinuteStart);
  assert(element_count(stats_node("topRoutes10m")) == 5);
  assert(!memchr(stats_text, 0x01, stats_length));  // every control byte escaped
  printf("statusz stats: heavy render %zu bytes, worst case %zu, capacity %zu\n", stats_length,
         ApiTelemetry::StatusStatsWorstCaseBytes(), ApiTelemetry::kStatsCapacity);
  assert(ApiTelemetry::StatusStatsWorstCaseBytes() <= ApiTelemetry::kStatsCapacity);
}

// Writers race readers: every render must be complete JSON whose totals only
// grow, and the final count is exact.
struct StatusWriter {
  ApiTelemetry* telemetry;
  int index;
};

void* status_writer(void* opaque) {
  auto* writer = static_cast<StatusWriter*>(opaque);
  char id[32];
  for (int i = 0; i < 4000; ++i) {
    snprintf(id, sizeof(id), "w%d-%d", writer->index, i % 50);
    writer->telemetry->Record(i % 2 ? "/v1/sync" : "/v1/prices", i % 9 ? 200 : 500, id, 2,
                              kMinuteStart + (i % 3) * 1000);
  }
  return nullptr;
}

void check_status_concurrency() {
  ApiTelemetry telemetry;
  assert(telemetry.Start());
  pthread_t threads[4];
  StatusWriter writers[4];
  for (int i = 0; i < 4; ++i) {
    writers[i] = {&telemetry, i};
    assert(pthread_create(&threads[i], nullptr, status_writer, &writers[i]) == 0);
  }
  double last = 0;
  for (int i = 0; i < 300; ++i) {
    render_stats(telemetry, kMinuteStart + 5000);
    const double requests = window("window10m", "requests");
    assert(requests >= last);
    last = requests;
  }
  for (pthread_t thread : threads) pthread_join(thread, nullptr);
  render_stats(telemetry, kMinuteStart + 5000);
  assert(window("window5m", "requests") == 16000 && window("window5m", "clients") == 200);
  assert(telemetry.revision() == 16000);
}

int RunTelemetryTests(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s operations_telemetry.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* fixture = read_file(argv[1], &length);
  assert(fixture);
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(json.Parse(reinterpret_cast<const unsigned char*>(fixture), length, tokens,
                    kBoundedJsonMaxTokens, &error));
  check_services(json.FindObjectValue(json.root(), "services"));

  ApiTelemetry telemetry;
  assert(telemetry.Start());
  const uint32_t samples = json.FindObjectValue(json.root(), "samples");
  const uint32_t checkpoints = json.FindObjectValue(json.root(), "checkpoints");
  uint32_t sample = json.token(samples)->first_child;
  size_t replayed = 0, compared = 0;
  static char wanted[ApiTelemetry::kErrorszCapacity], actual[ApiTelemetry::kErrorszCapacity];
  for (uint32_t checkpoint = json.token(checkpoints)->first_child;
       checkpoint != kBoundedJsonNoToken; checkpoint = json.token(checkpoint)->next_sibling) {
    const uint64_t revision = static_cast<uint64_t>(number(checkpoint, "revision"));
    while (telemetry.revision() < revision) {
      assert(sample != kBoundedJsonNoToken);
      char route[128], device[64];
      text(sample, "route", route, sizeof(route));
      const char* device_id = text(sample, "deviceId", device, sizeof(device));
      telemetry.Record(route, static_cast<int>(number(sample, "status")), device_id,
                       number(sample, "durationMs"),
                       static_cast<int64_t>(number(sample, "atMs")));
      sample = json.token(sample)->next_sibling;
      ++replayed;
    }
    const int64_t now = static_cast<int64_t>(number(checkpoint, "nowMs"));
    char label[64];
    text(checkpoint, "label", label, sizeof(label));
    text(checkpoint, "errorsz", wanted, sizeof(wanted));
    size_t size = telemetry.RenderErrorsz(now, actual, sizeof(actual));
    actual[size] = '\0';
    if (strcmp(actual, wanted) != 0) {
      fprintf(stderr, "%s: /errorsz differs\nwanted: %s\nactual: %s\n", label, wanted, actual);
      assert(false);
    }
    // The memo replays the same document while nothing new is recorded.
    assert(telemetry.RenderErrorsz(now, actual, sizeof(actual)) == size &&
           memcmp(actual, wanted, size) == 0);
    text(checkpoint, "statsJson", wanted, sizeof(wanted));
    without_database_operations(wanted);
    size = telemetry.RenderStatusStats(now, actual, sizeof(actual));
    actual[size] = '\0';
    if (strcmp(actual, wanted) != 0) {
      fprintf(stderr, "%s: stats differ\nwanted: %s\nactual: %s\n", label, wanted, actual);
      assert(false);
    }
    ++compared;
  }
  printf("telemetry: %zu samples replayed, %zu checkpoints identical to TypeScript\n",
         replayed, compared);
  assert(compared == 5 && replayed > 50);
  check_aggregate_overflow();
  check_collision_and_rollover();
  check_snapshot_readers_and_shutdown();
  check_scope_and_bounds();
  check_request_stats();
  check_group_percentiles();
  check_group_coverage_bounds();
  check_concurrency();
  check_compact_history();
  check_full_history_serialization();
  check_status_cold_and_unstarted();
  check_status_window_boundaries();
  check_status_errors_clients_and_routes();
  check_status_malformed_samples();
  check_status_concurrency();
  printf("statusz stats: cold, windows, clients, routes, malformed samples and concurrency OK\n");
  free(fixture);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return fortis::base::FatalBoundary("api telemetry test", [&] { return RunTelemetryTests(argc, argv); });
}
