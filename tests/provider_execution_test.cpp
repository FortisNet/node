#include <algorithm>
#include <iterator>
#include "native_config.h"
#include "provider_execution.h"
#include "coingecko_request.h"

#include <assert.h>
#include <atomic>
#include <stdio.h>
#include <string>
#include <string.h>
#include <thread>
#include <type_traits>
#include <utility>
#include <time.h>

using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::providers;

struct TestClock final : fortis::base::Clock {
  std::atomic<uint64_t> now_ms{0};
  std::atomic<unsigned> wakes{0};
  uint64_t MonotonicMs() override { return now_ms.load(); }
};

static void wake_owner(void* context) {
  static_cast<TestClock*>(context)->wakes.fetch_add(1);
}
static bool allow_local(void*, uint32_t ip) { return (ip >> 24) == 127; }
static void pause_ms(unsigned milliseconds) {
  const timespec pause{static_cast<time_t>(milliseconds / 1000),
                       static_cast<long>((milliseconds % 1000) * 1000000)};
  nanosleep(&pause, nullptr);
}

static OutboundRequest request_for(const char* path, const char* ca, uint32_t timeout = 1000) {
  OutboundRequest request{};
  request.host = "localhost";
  request.path = path;
  request.ca_file = ca;
  request.timeout_ms = timeout;
  request.max_response_bytes = 4096;
  request.allow_address = allow_local;
  return request;
}

static ProviderExecutionResult wait_result(ProviderExecutionAdapter& adapter,
                                           TestClock& clock, uint64_t expected_id,
                                           unsigned max_wait_ms = 5000,
                                           unsigned char* captured_body = nullptr,
                                           size_t captured_capacity = 0) {
  unsigned char body[4096]{};
  const unsigned iterations = max_wait_ms;
  for (unsigned i = 0; i < iterations; ++i) {
    adapter.Poll();
    ProviderExecutionResult result{};
    const ProviderExecutionTakeStatus taken =
        adapter.TryTakeResult(&result, body, sizeof(body));
    if (taken == ProviderExecutionTakeStatus::kTaken) {
      assert(result.job_id == expected_id);
      if (captured_body && result.body_length)
        memcpy(captured_body, body, result.body_length < captured_capacity ?
               result.body_length : captured_capacity);
      return result;
    }
    const uint64_t wakeup = adapter.NextWakeupMs();
    if (wakeup != UINT64_MAX && wakeup > clock.now_ms.load())
      clock.now_ms.store(wakeup);
    pause_ms(1);
  }
  assert(!"provider execution result timed out");
  return {};
}

static void wait_result_pair(ProviderExecutionAdapter& adapter, TestClock& clock,
                             uint64_t first_id, uint64_t second_id) {
  bool first_seen = false;
  bool second_seen = false;
  unsigned char body[4096]{};
  for (unsigned elapsed = 0; elapsed < 5000 && (!first_seen || !second_seen); ++elapsed) {
    adapter.Poll();
    ProviderExecutionResult result{};
    if (adapter.TryTakeResult(&result, body, sizeof(body)) ==
        ProviderExecutionTakeStatus::kTaken) {
      assert(result.response.status == 200 && !result.cancelled);
      if (result.job_id == first_id) first_seen = true;
      else if (result.job_id == second_id) second_seen = true;
      else assert(!"unexpected result while waiting for pair");
    }
    const uint64_t wakeup = adapter.NextWakeupMs();
    if (wakeup != UINT64_MAX && wakeup > clock.now_ms.load())
      clock.now_ms.store(wakeup);
    pause_ms(1);
  }
  assert(first_seen && second_seen);
}

static void shutdown_and_join(ProviderExecutionAdapter& adapter) {
  adapter.BeginShutdown();
  std::thread lifecycle([&adapter] { adapter.JoinWorkers(); });
  lifecycle.join();
  assert(adapter.workers_joined());
  assert(adapter.live_job_count() == 0);
}

static void test_coingecko_request_adapter(uint16_t port, const char* ca) {
  NativeConfig config{};
  config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", ca);
  const char* ids[] = {"Bitcoin", "ethereum-2"};
  char path[kCoinGeckoSpotMaxPathBytes + 1]{};
  OutboundRequest request{};

  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  static const char expected_path[] =
      "/api/v3/simple/price?ids=bitcoin%2Cethereum-2&vs_currencies=usd"
      "&include_24hr_change=true&include_24hr_vol=true";
  assert(strcmp(path, expected_path) == 0 && request.path == path &&
         strcmp(request.host, "api.coingecko.com") == 0 &&
         request.header_count == 1 &&
         strcmp(request.headers[0].name, "accept") == 0 &&
         strcmp(request.headers[0].value, "application/json") == 0);

  config.price_snapshot_enabled = true;
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  assert(std::string(path) == std::string(expected_path) + "&include_last_updated_at=true");
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "eur", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  assert(strstr(path, "last_updated_at") == nullptr);
  config.price_snapshot_enabled = false;

  config.coingecko_api_key_configured = true;
  snprintf(config.coingecko_api_key, sizeof(config.coingecko_api_key), "%s",
           "integration-key-92");
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  assert(strcmp(request.host, "api.coingecko.com") == 0 && request.header_count == 2 &&
         strcmp(request.headers[0].name, "accept") == 0 &&
         strcmp(request.headers[1].name, "x-cg-demo-api-key") == 0 &&
         strcmp(request.headers[1].value, "integration-key-92") == 0);

  config.coingecko_tier = CoinGeckoTier::kPro;
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  assert(strcmp(request.host, "pro-api.coingecko.com") == 0 && request.header_count == 2 &&
         strcmp(request.headers[1].name, "x-cg-pro-api-key") == 0);

  config.coingecko_api_key_configured = false;
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  assert(strcmp(request.host, "api.coingecko.com") == 0 && request.header_count == 1);

  config.coingecko_api_key_configured = true;
  snprintf(config.coingecko_api_key, sizeof(config.coingecko_api_key), "%s",
           "integration-key-92");
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kNone);
  // The credential is copied into caller-owned request header storage.
  memset(config.coingecko_api_key, 'x', sizeof(config.coingecko_api_key));
  request.host = "localhost";  // Test-only fixture redirect; builder exposes no host override.
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  request.port = port_text;
  request.allow_address = allow_local;

  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();
  ProviderExecutionOptions options{5000, 1, false, false};
  int context = 14;
  const ProviderExecutionSubmission submitted =
      adapter.Submit("coingecko", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  memset(request.headers[1].value, 'z', sizeof(request.headers[1].value));
  unsigned char response_body[4096]{};
  const ProviderExecutionResult result = wait_result(
      adapter, clock, submitted.job_id, 5000, response_body, sizeof(response_body));
  static const char expected_response[] =
      "GET /api/v3/simple/price?ids=bitcoin%2Cethereum-2&vs_currencies=usd"
      "&include_24hr_change=true&include_24hr_vol=true HTTP/1.1|"
      "x-cg-pro-api-key: integration-key-92";
  if (!(result.response.status == 200 && result.attempts == 1 &&
        result.body_length == sizeof(expected_response) - 1 &&
        memcmp(response_body, expected_response, sizeof(expected_response) - 1) == 0)) {
    fprintf(stderr, "CoinGecko fixture: status=%d error=%s attempts=%u body=%zu text=%.*s\n",
            result.response.status, OutboundErrorName(result.response.error),
            result.attempts, result.body_length, static_cast<int>(result.body_length),
            response_body);
  }
  assert(result.response.status == 200 && result.attempts == 1 &&
         result.body_length == sizeof(expected_response) - 1 &&
         memcmp(response_body, expected_response, sizeof(expected_response) - 1) == 0);
  shutdown_and_join(adapter);

  const char* bad_id[] = {"bitcoin/evil"};
  snprintf(config.coingecko_api_key, sizeof(config.coingecko_api_key), "%s",
           "integration-key-92");
  assert(BuildCoinGeckoSpotRequest(config, bad_id, 1, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kInvalidArguments);
  assert(request.host == nullptr && request.header_count == 0 && path[0] == '\0');
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "USD", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kInvalidArguments);
  config.coingecko_api_key[3] = '\r';
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kInvalidConfig);
  snprintf(config.coingecko_api_key, sizeof(config.coingecko_api_key), "%s",
           "integration-key-92");
  assert(BuildCoinGeckoSpotRequest(config, ids, 2, "usd", path, 8, &request) ==
         CoinGeckoSpotRequestError::kBufferTooSmall);
  char long_ids_storage[100][101]{};
  const char* long_ids[100]{};
  for (size_t i = 0; i < 100; ++i) {
    memset(long_ids_storage[i], 'a', 100);
    long_ids_storage[i][98] = static_cast<char>('0' + i / 10);
    long_ids_storage[i][99] = static_cast<char>('0' + i % 10);
    long_ids_storage[i][100] = '\0';
    long_ids[i] = long_ids_storage[i];
  }
  assert(BuildCoinGeckoSpotRequest(config, long_ids, 100, "usd", path, sizeof(path),
                                   &request) == CoinGeckoSpotRequestError::kBufferTooSmall);

  CoinGeckoSpotBatchPlan plan{};
  assert(BuildCoinGeckoSpotBatchPlan(config, long_ids, 100, "usd", &plan) ==
         CoinGeckoSpotPlanError::kNone);
  assert(plan.id_count == 100 && plan.batch_count == 6);
  size_t expected_id = 0;
  const char* planned_ids[100]{};
  for (size_t i = 0; i < plan.id_count; ++i) planned_ids[i] = plan.ids[i];
  for (size_t batch = 0; batch < plan.batch_count; ++batch) {
    const CoinGeckoSpotBatch& item = plan.batches[batch];
    assert(item.first_id == expected_id && item.id_count > 0 &&
           item.path_length <= kCoinGeckoSpotMaxPathBytes);
    assert(BuildCoinGeckoSpotRequest(config, planned_ids + item.first_id,
                                     item.id_count, "usd", path, sizeof(path),
                                     &request) == CoinGeckoSpotRequestError::kNone);
    assert(strlen(path) == item.path_length);
    for (size_t i = 0; i < item.id_count; ++i)
      assert(strcmp(plan.ids[item.first_id + i], long_ids_storage[expected_id + i]) == 0);
    expected_id += item.id_count;
  }
  assert(expected_id == 100);

  // 20 slugs exactly fill the 2,048-byte path; increasing the final slug by
  // one character forces a second stable-order batch instead of truncation.
  char boundary_storage[20][101]{};
  const char* boundary_ids[20]{};
  for (size_t i = 0; i < 19; ++i) {
    memset(boundary_storage[i], 'x', 100);
    boundary_storage[i][0] = 'a';
    boundary_storage[i][98] = static_cast<char>('0' + i / 10);
    boundary_storage[i][99] = static_cast<char>('0' + i % 10);
    boundary_storage[i][100] = '\0';
    boundary_ids[i] = boundary_storage[i];
  }
  boundary_storage[19][0] = 'z'; boundary_storage[19][1] = '\0';
  boundary_ids[19] = boundary_storage[19];
  assert(BuildCoinGeckoSpotBatchPlan(config, boundary_ids, 20, "usd", &plan) ==
         CoinGeckoSpotPlanError::kNone);
  assert(plan.batch_count == 1 && plan.batches[0].id_count == 20 &&
         plan.batches[0].path_length == kCoinGeckoSpotMaxPathBytes);
  boundary_storage[19][1] = 'z'; boundary_storage[19][2] = '\0';
  assert(BuildCoinGeckoSpotBatchPlan(config, boundary_ids, 20, "usd", &plan) ==
         CoinGeckoSpotPlanError::kNone);
  assert(plan.batch_count == 2 && plan.batches[0].id_count == 19 &&
         plan.batches[1].first_id == 19 && plan.batches[1].id_count == 1);

  const char* duplicate_ids[] = {"Bitcoin", "bitcoin", "ETHEREUM", "ethereum"};
  assert(BuildCoinGeckoSpotBatchPlan(config, duplicate_ids, 4, "usd", &plan) ==
         CoinGeckoSpotPlanError::kNone);
  assert(plan.id_count == 2 && plan.batch_count == 1 &&
         strcmp(plan.ids[0], "bitcoin") == 0 && strcmp(plan.ids[1], "ethereum") == 0);
}

static void test_success_retry_429_and_safe_post(uint16_t port, uint16_t closed_port,
                                                  const char* ca) {
  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();

  // Header validation occurs in Submit before the worker can start CA loading,
  // DNS, or socket activity. Both endpoint fields intentionally fail closed.
  OutboundRequest invalid = request_for("/", "/no/such/ca-bundle");
  invalid.host = "invalid host";
  invalid.header_count = 1;
  snprintf(invalid.headers[0].name, sizeof(invalid.headers[0].name), "%s", "Host");
  snprintf(invalid.headers[0].value, sizeof(invalid.headers[0].value), "%s", "override");
  ProviderExecutionOptions invalid_options{5000, 1, false, false};
  int invalid_context = 0;
  auto invalid_submission = adapter.Submit("rpc:invalid-header", invalid,
                                           &invalid_context, invalid_options);
  assert(invalid_submission.status == ProviderExecutionSubmitStatus::kInvalidRequest);

  // A borrowed bearer (OutboundRequest::authorization_bearer) could dangle in
  // a queued job, so the adapter refuses it even when otherwise valid.
  OutboundRequest bearer = request_for("/ok", ca);
  bearer.authorization_bearer = "ya29.valid-token";
  auto bearer_submission = adapter.Submit("rpc:bearer", bearer, &invalid_context, invalid_options);
  assert(bearer_submission.status == ProviderExecutionSubmitStatus::kInvalidRequest);

  OutboundRequest request = request_for("/ok", ca);
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  request.port = port_text;
  ProviderExecutionOptions options{5000, 3, false, false};
  int context = 1;
  auto submitted = adapter.Submit("rpc:adapter-ok", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  for (unsigned i = 0; i < 1000 && clock.wakes.load() == 0; ++i) pause_ms(1);
  assert(clock.wakes.load() != 0);
  adapter.Poll();
  ProviderExecutionResult sentinel{};
  sentinel.job_id = 999;
  unsigned char short_body[1]{};
  assert(adapter.TryTakeResult(&sentinel, short_body, sizeof(short_body)) ==
         ProviderExecutionTakeStatus::kBufferTooSmall);
  assert(sentinel.job_id == 999);
  unsigned char response_body[4096]{};
  ProviderExecutionResult result{};
  assert(adapter.TryTakeResult(&result, response_body, sizeof(response_body)) ==
         ProviderExecutionTakeStatus::kTaken);
  assert(result.job_id == submitted.job_id);
  assert(!result.cancelled && result.response.error == OutboundError::kNone &&
         result.response.status == 200 && result.attempts == 1 && result.body_length == 2);

  request.path = "/retryHeader";
  request.header_count = 1;
  snprintf(request.headers[0].name, sizeof(request.headers[0].name), "%s", "X-Api-Key");
  snprintf(request.headers[0].value, sizeof(request.headers[0].value), "%s", "copied-token-73");
  submitted = adapter.Submit("rpc:adapter-retry", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  // Mutating caller storage after Submit must not affect either retry attempt.
  for (auto& header : request.headers) {
    std::fill(std::begin(header.name), std::end(header.name), 'x');
    std::fill(std::begin(header.value), std::end(header.value), 'x');
  }
  bool saw_retry_due = false;
  for (unsigned i = 0; i < 2000 && !saw_retry_due; ++i) {
    adapter.Poll();
    const uint64_t wakeup = adapter.NextWakeupMs();
    if (wakeup == 1000) saw_retry_due = true;
    else pause_ms(1);
  }
  assert(saw_retry_due);
  clock.now_ms.store(1000);
  unsigned char retry_body[4096]{};
  result = wait_result(adapter, clock, submitted.job_id, 5000, retry_body,
                       sizeof(retry_body));
  static const char expected_header[] = "X-Api-Key: copied-token-73";
  assert(result.response.status == 200 && result.attempts == 2 &&
         result.body_length == sizeof(expected_header) - 1 &&
         memcmp(retry_body, expected_header, sizeof(expected_header) - 1) == 0);
  request.header_count = 0;
  std::fill(std::begin(request.headers), std::end(request.headers), fortis::http::OutboundHeader{});
  ProviderLaneStats stats{};
  assert(lanes.Stats("rpc:adapter-retry", &stats));
  assert(stats.dispatched == 1 && stats.answered == 1 && stats.faults == 0 &&
         stats.in_flight == 0);

  request.path = "/retryDeadline";
  ProviderExecutionOptions short_deadline{clock.now_ms.load() + 500, 3, false, false};
  submitted = adapter.Submit("rpc:retry-deadline", request, &context, short_deadline);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 503 && result.attempts == 1 && result.deadline_exceeded);

  request.path = "/429";
  submitted = adapter.Submit("rpc:adapter-429", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 429 && result.attempts == 1 &&
         result.retry_after_ms == 7000);
  assert(lanes.Stats("rpc:adapter-429", &stats));
  assert(stats.breaker == ProviderBreakerState::kOpen && stats.dispatched == 1 &&
         stats.faults == 1 && stats.in_flight == 0);
  submitted = adapter.Submit("rpc:adapter-429", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kLaneRejected &&
         submitted.lane_status == ProviderLaneAdmissionStatus::kBreakerOpen &&
         submitted.retry_after_ms == 30000);

  request.path = "/429Long";
  submitted = adapter.Submit("rpc:adapter-429-long", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 429 && result.attempts == 1 &&
         result.retry_after_ms == 60000);
  submitted = adapter.Submit("rpc:adapter-429-long", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kLaneRejected &&
         submitted.retry_after_ms == 60000);

  submitted = adapter.Submit("coingecko", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.retry_after_ms == 180000);
  clock.now_ms.fetch_add(180001);
  request.path = "/429Extreme";
  ProviderExecutionOptions probe_options = options;
  probe_options.deadline_ms = clock.now_ms.load() + 8000;
  submitted = adapter.Submit("coingecko", request, &context, probe_options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 429 && result.retry_after_ms == 3600000);
  options.deadline_ms = clock.now_ms.load() + 8000;

  request = request_for("/always503", ca);
  snprintf(port_text, sizeof(port_text), "%u", port);
  request.port = port_text;
  request.method = OutboundMethod::kPostJson;
  static const unsigned char payload[] = "{}";
  request.body = payload;
  request.body_length = sizeof(payload) - 1;
  submitted = adapter.Submit("rpc:post-once", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 503 && result.attempts == 1);

  options.post_is_idempotent = true;
  request.path = "/post503";
  submitted = adapter.Submit("rpc:post-safe", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 503 && result.attempts == 3);
  options.post_is_idempotent = false;

  request = request_for("/ok", ca);
  char closed_text[6]; snprintf(closed_text, sizeof(closed_text), "%u", closed_port);
  request.port = closed_text;
  submitted = adapter.Submit("rpc:no-status", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.status == 0 && result.response.error == OutboundError::kConnect &&
         result.attempts == 3);

  request = request_for("/stall", ca, 80);
  request.port = port_text;
  submitted = adapter.Submit("rpc:timeout", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  result = wait_result(adapter, clock, submitted.job_id);
  assert(result.response.error == OutboundError::kTimeout && result.attempts == 3);
  shutdown_and_join(adapter);
}

static void test_logical_lane_lease_retries_and_probe(uint16_t port, const char* ca) {
  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  OutboundRequest request = request_for("/retryLease", ca);
  request.port = port_text;
  ProviderExecutionOptions options{60000, 3, false, false};
  int context = 1;
  auto first = adapter.Submit("coingecko", request, &context, options);
  assert(first.status == ProviderExecutionSubmitStatus::kAccepted);

  // The first 503 enters backoff but keeps its original active lane lease.
  bool in_retry_wait = false;
  for (unsigned i = 0; i < 2000 && !in_retry_wait; ++i) {
    adapter.Poll();
    in_retry_wait = adapter.NextWakeupMs() == 1000;
    if (!in_retry_wait) pause_ms(1);
  }
  assert(in_retry_wait);
  ProviderLaneStats stats{};
  assert(lanes.Stats("coingecko", &stats) && stats.in_flight == 1 && stats.dispatched == 1);

  request.path = "/ok";
  auto second = adapter.Submit("coingecko", request, &context, options);
  assert(second.status == ProviderExecutionSubmitStatus::kAccepted);
  assert(lanes.Stats("coingecko", &stats) && stats.in_flight == 2 && stats.dispatched == 2);
  auto third = adapter.Submit("coingecko", request, &context, options);
  assert(third.status == ProviderExecutionSubmitStatus::kAccepted &&
         third.lane_status == ProviderLaneAdmissionStatus::kQueued);
  assert(lanes.Stats("coingecko", &stats) && stats.in_flight == 2 && stats.queued == 1);
  assert(adapter.Cancel(first.job_id));
  adapter.Poll();
  unsigned char body[4096]{};
  ProviderExecutionResult result{};
  assert(adapter.TryTakeResult(&result, body, sizeof(body)) == ProviderExecutionTakeStatus::kTaken);
  assert(result.job_id == first.job_id && result.cancelled);
  assert(lanes.Stats("coingecko", &stats) && stats.in_flight == 2 && stats.queued == 0 &&
         stats.answered == 1);
  wait_result_pair(adapter, clock, second.job_id, third.job_id);
  assert(lanes.Stats("coingecko", &stats) && stats.in_flight == 0 &&
         stats.dispatched == 3 && stats.answered == 3 && stats.faults == 0);
  shutdown_and_join(adapter);

  // Five failing logical calls trip the lane once per call, despite three
  // physical HTTP attempts each. A half-open probe keeps its lease while its
  // own first 503 retries, then closes only after the 200 answer.
  TestClock breaker_clock;
  ProviderLaneRegistry breaker_lanes(&breaker_clock);
  auto breaker_adapter_made = ProviderExecutionAdapter::Create(breaker_lanes, breaker_clock, wake_owner, &breaker_clock);
  assert(breaker_adapter_made.ok());
  auto breaker_adapter = breaker_adapter_made.TakeValue();
  request.path = "/always503";
  for (unsigned i = 0; i < 5; ++i) {
    auto failure = breaker_adapter.Submit("coingecko", request, &context, options);
    assert(failure.status == ProviderExecutionSubmitStatus::kAccepted);
    auto failed = wait_result(breaker_adapter, breaker_clock, failure.job_id);
    assert(failed.response.status == 503 && failed.attempts == 3);
  }
  assert(breaker_lanes.Stats("coingecko", &stats));
  assert(stats.dispatched == 5 && stats.faults == 5 && stats.breaker_opens == 1 &&
         stats.breaker == ProviderBreakerState::kOpen);
  const uint64_t probe_at = breaker_lanes.NextWakeupMs("coingecko");
  assert(probe_at != UINT64_MAX);
  breaker_clock.now_ms.store(probe_at + 1);
  options.deadline_ms = breaker_clock.now_ms.load() + 60000;
  request.path = "/probeRetry";
  auto probe = breaker_adapter.Submit("coingecko", request, &context, options);
  assert(probe.status == ProviderExecutionSubmitStatus::kAccepted);
  auto recovered = wait_result(breaker_adapter, breaker_clock, probe.job_id);
  assert(recovered.response.status == 200 && recovered.attempts == 2);
  assert(breaker_lanes.Stats("coingecko", &stats));
  assert(stats.dispatched == 6 && stats.faults == 5 && stats.answered == 1 &&
         stats.breaker == ProviderBreakerState::kClosed && stats.in_flight == 0);
  shutdown_and_join(breaker_adapter);
}

static void test_retry_wait_deadline_and_shutdown_release(uint16_t port, const char* ca) {
  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  OutboundRequest request = request_for("/retryDeadlineLease", ca);
  request.port = port_text;
  ProviderExecutionOptions options{1300, 3, false, false};
  int context = 7;
  auto submitted = adapter.Submit("rpc:retry-deadline-lease", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  bool waiting = false;
  for (unsigned i = 0; i < 2000 && !waiting; ++i) {
    adapter.Poll();
    waiting = adapter.NextWakeupMs() == 1250;
    if (!waiting) {
      const uint64_t wakeup = adapter.NextWakeupMs();
      if (wakeup != UINT64_MAX && wakeup > clock.now_ms.load()) clock.now_ms.store(wakeup);
      pause_ms(1);
    }
  }
  assert(waiting);
  ProviderLaneStats stats{};
  assert(lanes.Stats("rpc:retry-deadline-lease", &stats) && stats.in_flight == 1);
  clock.now_ms.store(1300);
  adapter.Poll();
  unsigned char body[4096]{};
  ProviderExecutionResult result{};
  assert(adapter.TryTakeResult(&result, body, sizeof(body)) == ProviderExecutionTakeStatus::kTaken);
  assert(result.job_id == submitted.job_id && result.deadline_exceeded && result.attempts == 2);
  assert(lanes.Stats("rpc:retry-deadline-lease", &stats) && stats.in_flight == 0 &&
         stats.faults == 1 && stats.dispatched == 1);
  shutdown_and_join(adapter);

  TestClock shutdown_clock;
  ProviderLaneRegistry shutdown_lanes(&shutdown_clock);
  auto shutdown_adapter_made = ProviderExecutionAdapter::Create(shutdown_lanes, shutdown_clock, wake_owner, &shutdown_clock);
  assert(shutdown_adapter_made.ok());
  auto shutdown_adapter = shutdown_adapter_made.TakeValue();
  request.path = "/retryShutdownLease";
  options.deadline_ms = 5000;
  submitted = shutdown_adapter.Submit("rpc:retry-shutdown-lease", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  waiting = false;
  for (unsigned i = 0; i < 2000 && !waiting; ++i) {
    shutdown_adapter.Poll();
    waiting = shutdown_adapter.NextWakeupMs() == 250;
    if (!waiting) {
      const uint64_t wakeup = shutdown_adapter.NextWakeupMs();
      if (wakeup != UINT64_MAX && wakeup > shutdown_clock.now_ms.load())
        shutdown_clock.now_ms.store(wakeup);
      pause_ms(1);
    }
  }
  assert(waiting);
  shutdown_adapter.BeginShutdown();
  shutdown_adapter.Poll();
  assert(shutdown_lanes.Stats("rpc:retry-shutdown-lease", &stats));
  assert(stats.in_flight == 0 && stats.answered == 1 && stats.faults == 0);
  unsigned char shutdown_body[4096]{};
  ProviderExecutionResult shutdown_result{};
  assert(shutdown_adapter.TryTakeResult(&shutdown_result, shutdown_body,
                                        sizeof(shutdown_body)) ==
         ProviderExecutionTakeStatus::kTaken);
  assert(shutdown_result.cancelled && shutdown_result.job_id == submitted.job_id);
  std::thread lifecycle([&shutdown_adapter] { shutdown_adapter.JoinWorkers(); });
  lifecycle.join();
  assert(shutdown_adapter.live_job_count() == 0 && shutdown_adapter.workers_joined());
}

static void test_cancel_race_queue_and_worker_saturation(uint16_t port, const char* ca) {
  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  OutboundRequest request = request_for("/ok", ca);
  request.port = port_text;
  ProviderExecutionOptions options{5000, 3, false, false};
  int context = 0;

  auto submitted = adapter.Submit("rpc:cancel-race", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  for (unsigned i = 0; i < 1000 && clock.wakes.load() == 0; ++i) pause_ms(1);
  assert(clock.wakes.load() != 0);
  assert(adapter.Cancel(submitted.job_id));
  auto result = wait_result(adapter, clock, submitted.job_id);
  assert(result.cancelled && result.attempts == 1);

  request.path = "/slow";
  auto first = adapter.Submit("rpc:queue-cancel", request, &context, options);
  ProviderExecutionSubmission burst[4];
  for (int i = 0; i < 4; ++i) {
    burst[i] = adapter.Submit("rpc:queue-cancel", request, &context, options);
    assert(burst[i].status == ProviderExecutionSubmitStatus::kAccepted);
  }
  assert(first.status == ProviderExecutionSubmitStatus::kAccepted);
  auto waiting = adapter.Submit("rpc:queue-cancel", request, &context, options);
  assert(waiting.status == ProviderExecutionSubmitStatus::kAccepted &&
         waiting.lane_status == ProviderLaneAdmissionStatus::kQueued);
  assert(adapter.Cancel(waiting.job_id));
  adapter.Poll();
  unsigned char body[4096]{};
  ProviderExecutionResult cancelled_result{};
  assert(adapter.TryTakeResult(&cancelled_result, body, sizeof(body)) ==
         ProviderExecutionTakeStatus::kTaken);
  assert(cancelled_result.job_id == waiting.job_id && cancelled_result.cancelled);

  ProviderExecutionSubmission saturation[3];
  for (int i = 0; i < 3; ++i) {
    char lane[32]; snprintf(lane, sizeof(lane), "rpc:saturation-tail-%d", i);
    saturation[i] = adapter.Submit(lane, request, &context, options);
    assert(saturation[i].status == ProviderExecutionSubmitStatus::kAccepted);
  }
  auto over = adapter.Submit("rpc:over-capacity", request, &context, options);
  assert(over.status == ProviderExecutionSubmitStatus::kBusy);

  bool saw_waiting_cancel = false;
  size_t completed = 0;
  saw_waiting_cancel = cancelled_result.cancelled;
  for (unsigned elapsed = 0; elapsed < 8000 && completed < 8; ++elapsed) {
    adapter.Poll();
    ProviderExecutionResult result{};
    const auto status = adapter.TryTakeResult(&result, body, sizeof(body));
    if (status == ProviderExecutionTakeStatus::kTaken) {
      if (result.job_id == waiting.job_id) {
        assert(result.cancelled);
        saw_waiting_cancel = true;
      } else {
        assert(result.response.status == 200);
      }
      ++completed;
    }
    const uint64_t wakeup = adapter.NextWakeupMs();
    if (wakeup != UINT64_MAX && wakeup > clock.now_ms.load())
      clock.now_ms.store(wakeup);
    pause_ms(1);
  }
  assert(saw_waiting_cancel && completed == 8);
  shutdown_and_join(adapter);
}

static void test_shutdown_cancels_and_joins(uint16_t port, const char* ca) {
  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  OutboundRequest request = request_for("/stall", ca, 150);
  request.port = port_text;
  ProviderExecutionOptions options{2000, 1, false, false};
  int context = 3;
  auto submitted = adapter.Submit("rpc:shutdown", request, &context, options);
  assert(submitted.status == ProviderExecutionSubmitStatus::kAccepted);
  adapter.BeginShutdown();
  std::thread lifecycle([&adapter] { adapter.JoinWorkers(); });
  unsigned char body[4096]{};
  ProviderExecutionResult result{};
  for (unsigned i = 0; i < 3000; ++i) {
    adapter.Poll();
    if (adapter.TryTakeResult(&result, body, sizeof(body)) ==
        ProviderExecutionTakeStatus::kTaken) break;
    pause_ms(1);
  }
  lifecycle.join();
  assert(result.job_id == submitted.job_id && result.cancelled &&
         adapter.workers_joined() && adapter.live_job_count() == 0);
}

static uint64_t wall_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

// Cancel and BeginShutdown end an attempt that is already on the wire (the
// fixture's /stall answers after 3 s) through OutboundRequest::cancel,
// instead of waiting out its 5 s timeout.
static void test_cancel_interrupts_running_attempt(uint16_t port, const char* ca) {
  TestClock clock;
  ProviderLaneRegistry lanes(&clock);
  auto adapter_made = ProviderExecutionAdapter::Create(lanes, clock, wake_owner, &clock);
  assert(adapter_made.ok());
  auto adapter = adapter_made.TakeValue();
  char port_text[6]; snprintf(port_text, sizeof(port_text), "%u", port);
  OutboundRequest request = request_for("/stall", ca, 5000);
  request.port = port_text;
  ProviderExecutionOptions options{20000, 1, false, false};
  int context = 4;
  auto running = adapter.Submit("rpc:cancel-running", request, &context, options);
  assert(running.status == ProviderExecutionSubmitStatus::kAccepted);
  pause_ms(300);  // connected and waiting for the stalled reply
  uint64_t started = wall_ms();
  assert(adapter.Cancel(running.job_id));
  ProviderExecutionResult result = wait_result(adapter, clock, running.job_id, 2000);
  assert(result.cancelled && result.attempts == 1);
  assert(wall_ms() - started < 1000);

  auto stopping = adapter.Submit("rpc:shutdown-running", request, &context, options);
  assert(stopping.status == ProviderExecutionSubmitStatus::kAccepted);
  pause_ms(300);
  started = wall_ms();
  adapter.BeginShutdown();
  std::thread lifecycle([&adapter] { adapter.JoinWorkers(); });
  unsigned char body[4096]{};
  result = ProviderExecutionResult{};
  for (unsigned i = 0; i < 2000; ++i) {
    adapter.Poll();
    if (adapter.TryTakeResult(&result, body, sizeof(body)) == ProviderExecutionTakeStatus::kTaken) break;
    pause_ms(1);
  }
  lifecycle.join();
  assert(result.job_id == stopping.job_id && result.cancelled);
  assert(wall_ms() - started < 1000);
  assert(adapter.workers_joined() && adapter.live_job_count() == 0);
}

static void test_factory_failure_and_move() {
  static_assert(!std::is_default_constructible_v<ProviderExecutionAdapter>);
  static_assert(std::is_nothrow_move_constructible_v<ProviderExecutionAdapter>);
  TestClock clock;
  ProviderLaneRegistry invalid(nullptr);
  auto rejected = ProviderExecutionAdapter::Create(invalid, clock);
  assert(!rejected && rejected.status().code() == fortis::base::StatusCode::kUnavailable);
  ProviderLaneRegistry lanes(&clock);
  auto created = ProviderExecutionAdapter::Create(lanes, clock);
  assert(created.ok());
  auto first = created.TakeValue();
  auto moved = std::move(first);
  moved.BeginShutdown();
  moved.JoinWorkers();
  assert(moved.workers_joined() && moved.live_job_count() == 0);
}

int main(int argc, char** argv) {
  assert(argc == 4);
  test_factory_failure_and_move();
  const uint16_t port = static_cast<uint16_t>(strtoul(argv[1], nullptr, 10));
  const uint16_t closed_port = static_cast<uint16_t>(strtoul(argv[2], nullptr, 10));
  test_success_retry_429_and_safe_post(port, closed_port, argv[3]);
  test_logical_lane_lease_retries_and_probe(port, argv[3]);
  test_retry_wait_deadline_and_shutdown_release(port, argv[3]);
  test_coingecko_request_adapter(port, argv[3]);
  test_cancel_race_queue_and_worker_saturation(port, argv[3]);
  test_shutdown_cancels_and_joins(port, argv[3]);
  test_cancel_interrupts_running_attempt(port, argv[3]);
  puts("provider execution adapter tests passed");
  return 0;
}
