#pragma once
#include "http_server.h"
#include "device_enrollment.h"
#include "price_spot_cache.h"
#include "price_core_ids.h"
#include "threading.h"
#include <pthread.h>

namespace fortis::prices {

struct PriceSnapshotGeneration {
  static constexpr size_t kCapacity = 8192;
  uint8_t body[kCapacity]{};
  size_t length = 0;
  uint64_t sequence = 0;
  int64_t built_at_ms = 0;
  int64_t valid_until_ms = 0;
  struct Freshness {
    size_t offset;
    int64_t until_ms;
  };
  Freshness freshness[fortis::prices::kPriceCoreCount]{};
  size_t freshness_count = 0;
};

struct PriceSnapshotStatus {
  uint64_t sequence = 0;
  uint64_t unavailable = 0;
  int64_t age_ms = 0;
  bool ready = false;
};

// Read model shared within the authority process. Publication replaces the
// entire generation. Neither copying a response nor sampling status uses KV.
class PriceSnapshotView {
 public:
  ~PriceSnapshotView() = default;
  PriceSnapshotView() = default;
  PriceSnapshotView(const PriceSnapshotView&) = delete;
  PriceSnapshotView& operator=(const PriceSnapshotView&) = delete;

  bool Publish(const PriceSnapshotGeneration& generation, int64_t now_ms);
  bool Copy(int64_t now_ms, uint8_t* body, size_t capacity, size_t* length,
            char* etag, size_t etag_capacity);
  PriceSnapshotStatus Status(int64_t now_ms);

 private:
  bool Ready(int64_t now_ms) const;
  fortis::base::Mutex mutex_;
  PriceSnapshotGeneration current_{};
  uint64_t body_hash_ = 0;
  uint64_t unavailable_ = 0;
};

// One instance per price service. The owner starts this worker after the core
// warmer. Failed refreshes leave the last complete generation in the view.
class PriceSnapshotPublisher {
 public:
  PriceSnapshotPublisher(fortis::prices::PriceSpotCache& cache, PriceSnapshotView& view)
      : cache_(cache), view_(view) {}
  ~PriceSnapshotPublisher() { Stop();   }
  PriceSnapshotPublisher(const PriceSnapshotPublisher&) = delete;
  PriceSnapshotPublisher& operator=(const PriceSnapshotPublisher&) = delete;
  bool Start();
  void Stop();
  // Single-producer test seam. Call before Start or after Stop.
  bool Rebuild(int64_t now_ms);

 private:
  static void Run(PriceSnapshotPublisher* snapshot);
  fortis::prices::PriceSpotCache& cache_;
  PriceSnapshotView& view_;
  fortis::base::Mutex mutex_;
  fortis::base::ConditionVariable changed_;
  fortis::base::Thread thread_;
  uint64_t sequence_ = 0;
  int64_t next_boundary_ms_ = 0;
  bool started_ = false;
  bool stopping_ = false;
};

class PriceSnapshotRoute {
 public:
  using Authorize = auth::DevicePolicyResult (*)(const http::HttpRequest&, bool);
  explicit PriceSnapshotRoute(PriceSnapshotView& view,
                              Authorize authority = auth::AuthorizeSnapshotDeviceRequest)
      : view_(view), authority_(authority) {}
  bool Register(http::HttpRouter& router, bool attestation);

 private:
  static void Handle(int client, const http::HttpRequest& request, bool head, void* context);
  PriceSnapshotView& view_;
  Authorize authority_;
  bool attestation_ = false;
};

}  // namespace fortis::prices
