#include "provider_execution_coordinator.h"

#include <assert.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::http;
using namespace fortis::providers;

namespace {
struct Errors final : fortis::base::ErrorReporter {
  std::atomic<unsigned> reports{0};
  std::atomic<unsigned> identity_reports{0};
  void Report(std::string_view service, std::string_view code, std::string_view identifier,
              fortis::base::ErrorDetails details) noexcept override {
    if (service == "prices") {
      assert(code == "provider_rejected_coin_id" && identifier == "obsolete-coin");
      assert(details.origin == fortis::base::ErrorOrigin::kThirdParty && details.provider == "coingecko" && details.http_status == 404);
      identity_reports.fetch_add(1);
      reports.fetch_add(1);
      return;
    }
    assert(service == "providers" && identifier.empty());
    assert(details.origin == fortis::base::ErrorOrigin::kThirdParty && details.provider == "coingecko");
    assert((details.http_status == 503 && code == "http_server_error") ||
           (details.http_status == 429 && code == "http_rate_limited") ||
           (details.http_status == 404 && code == "http_request_rejected"));
    reports.fetch_add(1);
  }
};
uint64_t now_ms(void*) {
  timespec now{}; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }
void wake(void* context) { static_cast<std::atomic<unsigned>*>(context)->fetch_add(1); }

OutboundRequest make_request(uint16_t port, const char* ca, const char* path) {
  static thread_local char port_text[6];
  snprintf(port_text, sizeof(port_text), "%u", port);
  OutboundRequest request{};
  request.host = "localhost"; request.port = port_text; request.path = path;
  request.ca_file = ca; request.timeout_ms = 5000; request.max_response_bytes = 4096;
  request.allow_address = allow_loopback;
  return request;
}

ProviderExecutionResult take(ProviderExecutionCoordinator& coordinator,
    uint64_t owner, uint64_t job_id, void* expected_context, const char* expected_body,
    bool expected_cancelled) {
  const uint64_t deadline = now_ms(nullptr) + 8000;
  unsigned char body[4096]{};
  while (now_ms(nullptr) < deadline) {
    ProviderExecutionResult result{};
    const auto status = coordinator.TryTakeResult(owner, &result, body, sizeof(body));
    if (status == ProviderExecutionTakeStatus::kTaken) {
      assert(result.job_id == job_id);
      assert(result.context == expected_context);
      assert(result.cancelled == expected_cancelled);
      if (expected_body) {
        assert(result.body_length == strlen(expected_body));
        assert(memcmp(body, expected_body, result.body_length) == 0);
      }
      return result;
    }
    assert(status == ProviderExecutionTakeStatus::kNone);
    const timespec pause{0, 1000000}; nanosleep(&pause, nullptr);
  }
  assert(!"timed out waiting for coordinator result");
  return {};
}

void wait_wakes(std::atomic<unsigned>& wakes, unsigned target) {
  const uint64_t deadline = now_ms(nullptr) + 8000;
  while (wakes.load() < target && now_ms(nullptr) < deadline) {
    const timespec pause{0, 1000000}; nanosleep(&pause, nullptr);
  }
  assert(wakes.load() >= target);
}
}

int main(int argc, char** argv) {
  assert(argc == 4);
  const uint16_t port = static_cast<uint16_t>(strtoul(argv[1], nullptr, 10));
  std::atomic<unsigned> wakes_a{0}, wakes_b{0};
  Errors errors;
  ProviderExecutionCoordinator coordinator(false, &errors);
  assert(coordinator.valid() && coordinator.Start());
  const uint64_t owner_a = coordinator.RegisterOwner(wake, &wakes_a);
  const uint64_t owner_b = coordinator.RegisterOwner(wake, &wakes_b);
  assert(owner_a && owner_b && owner_a != owner_b);
  const ProviderExecutionOptions options{now_ms(nullptr) + 7000, 1, false, false};
  int context_a = 1, context_b = 2, cancel_context = 3, peer_context = 4;

  auto a = coordinator.Submit(owner_a, "coingecko",
      make_request(port, argv[2], "/owner-a"), &context_a, options);
  auto b = coordinator.Submit(owner_b, "coingecko",
      make_request(port, argv[2], "/owner-b"), &context_b, options);
  assert(a.status == ProviderExecutionSubmitStatus::kAccepted);
  assert(b.status == ProviderExecutionSubmitStatus::kAccepted);
  wait_wakes(wakes_a, 1); wait_wakes(wakes_b, 1);
  // A result is still owned by its owner until that owner drains it.
  assert(!coordinator.UnregisterOwner(owner_a));
  take(coordinator, owner_b, b.job_id, &context_b, "/owner-b", false);
  take(coordinator, owner_a, a.job_id, &context_a, "/owner-a", false);

  a = coordinator.Submit(owner_a, "coingecko",
      make_request(port, argv[2], "/cancel-a"), &cancel_context, options);
  b = coordinator.Submit(owner_b, "coingecko",
      make_request(port, argv[2], "/peer-b"), &peer_context, options);
  assert(a.status == ProviderExecutionSubmitStatus::kAccepted);
  assert(b.status == ProviderExecutionSubmitStatus::kAccepted);
  const uint64_t marker_deadline = now_ms(nullptr) + 5000;
  while (access(argv[3], F_OK) != 0 && now_ms(nullptr) < marker_deadline) {
    const timespec pause{0, 1000000}; nanosleep(&pause, nullptr);
  }
  assert(access(argv[3], F_OK) == 0);
  assert(coordinator.Cancel(owner_a, a.job_id));
  wait_wakes(wakes_a, 2); wait_wakes(wakes_b, 2);
  assert(!coordinator.UnregisterOwner(owner_b));
  take(coordinator, owner_a, a.job_id, &cancel_context, nullptr, true);
  take(coordinator, owner_b, b.job_id, &peer_context, "/peer-b", false);
  assert(coordinator.UnregisterOwner(owner_a));
  assert(coordinator.UnregisterOwner(owner_b));
  assert(coordinator.Stop());
  puts("coordinator shared TLS lane, owner result routing, cancellation, and drain passed");

  assert(errors.reports.load() == 0); // successes and caller cancellation are not failures
  ProviderExecutionCoordinator keyless(true, &errors);
  assert(keyless.Start());
  const uint64_t keyless_owner = keyless.RegisterOwner(wake, &wakes_a);
  assert(keyless_owner);
  const ProviderExecutionOptions retry_options{now_ms(nullptr) + 7000, 3, false, false};
  const auto limited = keyless.Submit(keyless_owner, "coingecko",
      make_request(port, argv[2], "/keyless-503"), &context_a, retry_options);
  assert(limited.status == ProviderExecutionSubmitStatus::kAccepted);
  const auto failure = take(keyless, keyless_owner, limited.job_id, &context_a,
                            "/keyless-503", false);
  assert(failure.response.status == 503 && failure.attempts == 1);
  assert(keyless.Submit(keyless_owner, "coingecko",
      make_request(port, argv[2], "/must-not-run"), &context_a, retry_options).status ==
      ProviderExecutionSubmitStatus::kLaneRejected);
  assert(keyless.UnregisterOwner(keyless_owner));
  assert(keyless.Stop());
  assert(errors.reports.load() == 1); // one settled job; no duplicate for lane rejection
  ProviderExecutionCoordinator limited_coordinator(false, &errors);
  assert(limited_coordinator.Start());
  const auto limited_owner = limited_coordinator.RegisterOwner(wake, &wakes_a);
  assert(limited_owner);
  ProviderExecutionOptions once{now_ms(nullptr) + 7000, 1, false, false};
  const auto rate_job = limited_coordinator.Submit(limited_owner, "coingecko",
      make_request(port, argv[2], "/limited-429"), &context_a, once);
  assert(rate_job.status == ProviderExecutionSubmitStatus::kAccepted);
  const auto rate_result = take(limited_coordinator, limited_owner, rate_job.job_id, &context_a, "/limited-429", false);
  assert(rate_result.response.status == 429 && rate_result.attempts == 1);
  assert(limited_coordinator.UnregisterOwner(limited_owner));
  assert(limited_coordinator.Stop());
  assert(errors.reports.load() == 2);
  ProviderExecutionCoordinator identity(false, &errors);
  assert(identity.Start());
  const auto identity_owner = identity.RegisterOwner(wake, &wakes_a);
  assert(identity_owner);
  for (const auto path : {"/api/v3/coins/obsolete-coin?localization=false", "/api/v3/coins/list",
       "/api/v3/coins/ethereum/contract/0x123", "/api/v3/coins/quota-coin?quota=true"}) {
    ProviderExecutionOptions identity_options{now_ms(nullptr) + 7000, 1, false, false};
    const auto job = identity.Submit(identity_owner, "coingecko", make_request(port, argv[2], path), &context_a, identity_options);
    assert(job.status == ProviderExecutionSubmitStatus::kAccepted);
    const auto response = take(identity, identity_owner, job.job_id, &context_a, path, false);
    assert(response.response.status == (strstr(path, "quota=true") ? 429 : 404));
  }
  assert(identity.UnregisterOwner(identity_owner) && identity.Stop());
  assert(errors.reports.load() == 6 && errors.identity_reports.load() == 1);
  return 0;
}
