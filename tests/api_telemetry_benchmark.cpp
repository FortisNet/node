// Opt-in repeatable component measurement; never a correctness timing gate.
#include "api_telemetry.h"
#include "exception_boundary.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using fortis::operations::ApiTelemetry;
namespace {
using Clock = std::chrono::steady_clock;
constexpr int64_t kNow = 1767324600000LL;
constexpr size_t kRecords = 1000000;
// Coprime with 96 routes; per-writer offsets avoid synchronized sampling.
constexpr size_t kSampleInterval = 67;

void Run(size_t writers, bool polling, bool saturated) {
  ApiTelemetry telemetry;
  if (!telemetry.Start()) std::abort();
  char routes[96][32];
  for (size_t i = 0; i < 96; ++i) std::snprintf(routes[i], sizeof(routes[i]), "/v1/prices/route-%zu", i);
  fortis::http::HttpCompletionTag detail{};
  detail.method = "GET";
  for (int minute = 14; minute >= 0; --minute) {
    for (size_t i = 0; i < (saturated ? 1023U : 96U); ++i) {
      char device[32];
      std::snprintf(device, sizeof(device), "seed-device-%zu", i);
      telemetry.Record(routes[i % 96], 200, device, 2, kNow - minute * 60000, &detail);
    }
  }
  std::atomic<bool> start{false}, stop{false};
  std::atomic<size_t> renders{0};
  std::thread reader;
  if (polling)
    reader = std::thread([&] {
      auto output = std::make_unique<char[]>(ApiTelemetry::kHistoryCapacity);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      while (!stop.load(std::memory_order_acquire)) {
        if (!telemetry.RenderHistory(kNow, output.get(), ApiTelemetry::kHistoryCapacity) ||
            !telemetry.RenderStatusStats(kNow, output.get(), ApiTelemetry::kHistoryCapacity) ||
            !telemetry.RenderErrorsz(kNow, output.get(), ApiTelemetry::kHistoryCapacity) ||
            !telemetry.RenderRequestStats(kNow, output.get(), ApiTelemetry::kHistoryCapacity))
          std::abort();
        renders.fetch_add(4, std::memory_order_relaxed);
      }
    });
  std::vector<std::vector<double>> timings(writers);
  std::vector<std::thread> threads;
  threads.reserve(writers);
  for (size_t thread = 0; thread < writers; ++thread) {
    timings[thread].reserve((kRecords + kSampleInterval - 1) / kSampleInterval);
    threads.emplace_back([&, thread] {
      fortis::http::HttpCompletionTag tag{};
      tag.method = "GET";
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t i = 0; i < kRecords; ++i) {
        const bool sample = i % kSampleInterval == thread % kSampleInterval;
        const auto before = sample ? Clock::now() : Clock::time_point{};
        telemetry.Record(routes[(i + thread) % 96], i % 100 ? 200 : 503, "new-device-after-bound", 2, kNow, &tag);
        if (sample) timings[thread].push_back(std::chrono::duration<double, std::micro>(Clock::now() - before).count());
      }
    });
  }
  const auto before = Clock::now();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  const double seconds = std::chrono::duration<double>(Clock::now() - before).count();
  stop.store(true, std::memory_order_release);
  if (reader.joinable()) reader.join();
  std::vector<double> samples;
  for (const auto& values : timings) samples.insert(samples.end(), values.begin(), values.end());
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&](size_t percent) { return samples[(samples.size() - 1) * percent / 100]; };
  std::printf(
      "{\"writers\":%zu,\"polling\":%s,\"deviceSetSaturated\":%s,\"records\":%zu,\"seconds\":%.6f,\"recordsPerSec\":%."
      "0f,\"sampleEvery\":67,"
      "\"p50Us\":%.3f,\"p95Us\":%.3f,\"p99Us\":%.3f,\"maxSampleUs\":%.3f,\"renders\":%zu}\n",
      writers, polling ? "true" : "false", saturated ? "true" : "false", writers * kRecords, seconds,
      static_cast<double>(writers * kRecords) / seconds, percentile(50), percentile(95), percentile(99), samples.back(),
      renders.load());
}
}  // namespace
int RunBenchmarks() {
#ifndef FORTIS_TELEMETRY_BASELINE
  std::printf("{\"fixedStorageBytes\":%zu}\n", ApiTelemetry::StorageBytes());
#endif
  for (bool saturated : {false, true}) {
    Run(1, false, saturated);
    Run(1, true, saturated);
    Run(4, false, saturated);
    Run(4, true, saturated);
  }
  return 0;
}

int main() { return fortis::base::FatalBoundary("telemetry benchmark", RunBenchmarks); }
