#include <cstdio>
#include "test_support.h"
#include "price_snapshot.h"
#include "price_core_ids.h"
#include "device_auth.h"
#include "kv_runtime.h"
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <filesystem>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>

using namespace fortis::auth;
using namespace fortis::http;
using namespace fortis::prices;

std::atomic<unsigned> parallel_authorizations{0};

DevicePolicyResult authorize_parallel(const HttpRequest&, bool) {
  ++parallel_authorizations;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (parallel_authorizations < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  return {};
}

std::string dispatch(HttpRouter& router, const std::string& raw) {
  int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send(pair[1], raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
  shutdown(pair[1], SHUT_WR);
  if (!router.Dispatch(pair[0], "127.0.0.1")) close(pair[0]);
  std::string response; char bytes[4096]; ssize_t n;
  while ((n = recv(pair[1], bytes, sizeof(bytes), 0)) > 0) response.append(bytes, static_cast<size_t>(n));
  assert(n == 0); close(pair[1]); return response;
}
void routes(PriceSnapshotPublisher& publisher, PriceSnapshotView& view, fortis::storage::KvStore& prices) {
  char auth_directory[] = "/tmp/fortis-snapshot-auth-XXXXXX";
  assert(mkdtemp(auth_directory));
  fortis::storage::RuntimeKvSetDirectory(auth_directory);
  auto* kv = fortis::storage::RuntimeKvStore(); assert(kv);
  timespec time{}; clock_gettime(CLOCK_REALTIME, &time);
  const int64_t now = static_cast<int64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
  fortis::auth::DeviceCredentials device;
  assert(fortis::auth::DeviceAuthStore(kv).EnrollDevelopment(fortis::auth::DevicePlatform::Linux,
      "test", 4, now, 3600000, &device));
  HttpRouter router; HttpResponseHandoff handoff; assert(handoff.Start());
  router.SetResponseHandoff(&handoff);
  PriceSnapshotRoute route(view);
  assert(route.Register(router, false));
  assert(publisher.Rebuild(now));
  HttpRouter parallel;
  PriceSnapshotRoute parallel_route(view, authorize_parallel);
  assert(parallel_route.Register(parallel, false));
  std::thread workers[4];
  for (auto& worker : workers) worker = std::thread([&] {
    assert(dispatch(parallel, "GET /v1/prices/snapshot HTTP/1.1\r\nHost: localhost\r\n\r\n")
        .find("200 OK") != std::string::npos);
  });
  for (auto& worker : workers) worker.join();
  assert(parallel_authorizations == 4);
  prices.Close();
  const std::string auth = std::string("Authorization: Bearer ") + device.token + "\r\n";
  auto get = [&](const std::string& headers, const char* method = "GET", const char* query = "") {
    return dispatch(router, std::string(method) + " /v1/prices/snapshot" + query +
                    " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n");
  };
  assert(get("If-None-Match: *\r\n").find("401") != std::string::npos);
  assert(get(auth + "Origin: https://example.org\r\nIf-None-Match: *\r\n").find("403 Forbidden") != std::string::npos);
  assert(get(auth, "GET", "?fiat=eur").find("400 Bad Request") != std::string::npos);
  // A full provider response handoff must not block ready snapshots.
  int peers[HttpResponseHandoff::kCapacity];
  for (size_t i = 0; i < HttpResponseHandoff::kCapacity; ++i) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    peers[i] = pair[1];
    uint64_t token = 0;
    assert(handoff.Reserve(pair[0], false, 5000, &token));
  }
  const auto first = get(auth);
  assert(first.find("200 OK") != std::string::npos);
  assert(first.find("cache-control: private, no-cache") != std::string::npos);
  const size_t start = first.find("ETag: "); assert(start != std::string::npos);
  const std::string tag = first.substr(start + 6, first.find("\r\n", start) - start - 6);
  const auto conditional = get(auth + "If-None-Match: W/" + tag + "\r\n");
  assert(conditional.find("304 Not Modified") != std::string::npos);
  assert(conditional.substr(conditional.find("\r\n\r\n") + 4).empty());
  const auto head = get(auth, "HEAD");
  assert(head.find("200 OK") != std::string::npos && head.substr(head.find("\r\n\r\n") + 4).empty());
  assert(get(auth + "Accept: application/x-protobuf\r\n").find("406") != std::string::npos);
  assert(get(auth + "Accept: application/json;q=0, */*;q=1\r\n").find("406") != std::string::npos);
  assert(get(auth + "Accept: Application/Json\r\n").find("200 OK") != std::string::npos);
  char quota_key[96]; uint64_t used = 0;
  snprintf(quota_key, sizeof(quota_key), "q:dev:%s:%lld", device.id, static_cast<long long>(now / 60000));
  assert(fortis::storage::RuntimeKvIncrementByWindow(quota_key, 240, now, 60000, &used));
  assert(get(auth + "If-None-Match: *\r\n").find("429") != std::string::npos);
  HttpRouter strict; strict.SetResponseHandoff(&handoff);
  PriceSnapshotRoute strict_route(view);
  assert(strict_route.Register(strict, true));
  const auto denied = dispatch(strict, "GET /v1/prices/snapshot HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n");
  assert(denied.find("401") != std::string::npos);
  // A cached bearer must still be rejected before 304 after revocation.
  fortis::auth::DeviceCredentials revoked;
  assert(fortis::auth::DeviceAuthStore(kv).EnrollDevelopment(fortis::auth::DevicePlatform::Linux,
      "test", 4, now, 3600000, &revoked));
  const std::string revoked_auth = std::string("Authorization: Bearer ") + revoked.token + "\r\n";
  assert(get(revoked_auth + "If-None-Match: *\r\n").find("304 Not Modified") != std::string::npos);
  assert(fortis::auth::DeviceAuthStore(kv).Revoke(revoked.id, now));
  assert(get(revoked_auth + "If-None-Match: *\r\n").find("401 Unauthorized") != std::string::npos);
  HttpRouter denied_router;
  PriceSnapshotRoute disconnected(view, nullptr);
  assert(disconnected.Register(denied_router, false));
  const auto unavailable = dispatch(denied_router,
      "GET /v1/prices/snapshot HTTP/1.1\r\nHost: localhost\r\n" + auth + "If-None-Match: *\r\n\r\n");
  assert(unavailable.find("503 Service Unavailable") != std::string::npos);
  handoff.Stop();
  for (int peer : peers) close(peer);
  std::filesystem::remove_all(auth_directory);
}

int main() try {
  char directory[] = "/tmp/fortis-snapshot-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore kv; assert(kv.Open(directory));
    fortis::prices::PriceSpotCache cache(kv);
    PriceSnapshotView view;
    PriceSnapshotPublisher publisher(cache, view);
    const int64_t now = 1000000;
    uint8_t body[PriceSnapshotGeneration::kCapacity]{}; size_t length = 0; char etag[128], previous[128];
    assert(publisher.Rebuild(now));
    assert(view.Copy(now, body, sizeof(body), &length, etag, sizeof(etag)));
    std::string text(reinterpret_cast<char*>(body), length);
    assert(text.find("\"available\":0") != std::string::npos);
    fortis::prices::PriceSpotQuoteInput q{};
    q.source_id = "bitcoin"; q.price = 100; q.as_of_ms = now; q.provider_observed_at_ms = now - 1000;
    assert(cache.StoreProviderBatch("usd", "coingecko", &q, 1, nullptr, 0, true, now) == fortis::prices::PriceSpotStoreStatus::kStored);
    auto zero = q;
    zero.source_id = "ethereum"; zero.price = 0;
    assert(cache.StoreProviderBatch("usd", "coingecko", &zero, 1, nullptr, 0, true, now) == fortis::prices::PriceSpotStoreStatus::kStored);
    auto plain = cache.Lookup("usd", "bitcoin", now);
    assert(plain.value.provider_observed_at_ms == 0);
    auto metadata = cache.Lookup("usd", "bitcoin", now, true);
    assert(metadata.value.provider_observed_at_ms == now - 1000 && metadata.value.received_at_ms == now);
    assert(publisher.Rebuild(now));
    assert(view.Copy(now, body, sizeof(body), &length, etag, sizeof(etag)));
    text.assign(reinterpret_cast<char*>(body), length);
    assert(text.find("\"available\":1") != std::string::npos);
    assert(text.find("\"providerObservedAtMs\":999000") != std::string::npos);
    std::snprintf(previous, sizeof(previous), "%s", etag);
    assert(publisher.Rebuild(now + 1000));
    assert(view.Copy(now + 1000, body, sizeof(body), &length, etag, sizeof(etag)));
    assert(strcmp(previous, etag) == 0); // no per-second body/ETag churn
    assert(view.Copy(now + 3000, body, sizeof(body), &length, etag, sizeof(etag)));
    assert(view.Copy(now + fortis::prices::kPriceSpotDefaultFreshMs, body, sizeof(body), &length, etag, sizeof(etag)));
    text.assign(reinterpret_cast<char*>(body), length);
    assert(text.find("\"stale\":true }") != std::string::npos && strcmp(previous, etag) != 0);
    assert(publisher.Rebuild(now + fortis::prices::kPriceSpotDefaultFreshMs));
    assert(view.Copy(now + fortis::prices::kPriceSpotDefaultFreshMs, body, sizeof(body), &length, etag, sizeof(etag)));
    text.assign(reinterpret_cast<char*>(body), length);
    assert(text.find("\"stale\":true") != std::string::npos && strcmp(previous, etag) != 0);
    assert(!view.Copy(now + fortis::prices::kPriceSpotDefaultFreshMs + fortis::prices::kPriceSpotStaleMs, body, sizeof(body), &length, etag, sizeof(etag)));
    assert(publisher.Rebuild(now + fortis::prices::kPriceSpotDefaultFreshMs + fortis::prices::kPriceSpotStaleMs));
    assert(view.Copy(now + fortis::prices::kPriceSpotDefaultFreshMs + fortis::prices::kPriceSpotStaleMs, body, sizeof(body), &length, etag, sizeof(etag)));
    text.assign(reinterpret_cast<char*>(body), length);
    assert(text.find("\"available\":0") != std::string::npos); // no expired floor sold as snapshot hit
    // An ordinary/legacy producer changes the exact envelope, invalidating the
    // old sidecar without extra metadata mutations.
    q.price = 101; q.as_of_ms = now + 1; q.provider_observed_at_ms = 0;
    assert(cache.StoreProviderBatch("usd", "coingecko", &q, 1, nullptr, 0, true, now + 1) == fortis::prices::PriceSpotStoreStatus::kStored);
    metadata = cache.Lookup("usd", "bitcoin", now + 1, true);
    assert(metadata.value.price == 101 && metadata.value.provider_observed_at_ms == 0 && metadata.value.received_at_ms == 0);
    // The reader keeps serving a complete generation when the price store
    // cannot refresh it. A late or expired generation cannot replace it.
    const int64_t later = now + fortis::prices::kPriceSpotDefaultFreshMs + fortis::prices::kPriceSpotStaleMs + 2;
    assert(publisher.Rebuild(later));
    const auto current = view.Status(later);
    PriceSnapshotGeneration invalid{};
    invalid.body[0] = '{';
    invalid.length = 1;
    invalid.sequence = current.sequence;
    invalid.built_at_ms = later;
    invalid.valid_until_ms = later + 2;
    assert(!view.Publish(invalid, later + 1));
    ++invalid.sequence;
    assert(!view.Publish(invalid, later + 2));
    kv.Close();
    assert(!publisher.Rebuild(later + 1));
    assert(view.Copy(later + 1, body, sizeof(body), &length, etag, sizeof(etag)));
    assert(view.Status(later + 1).sequence == current.sequence);
    assert(kv.Open(directory));
    routes(publisher, view, kv);
    assert(publisher.Start());
    publisher.Stop();
  }
  std::filesystem::remove_all(directory);
  puts("snapshot bounds, coverage, stable ETag, expiry and timestamp identity passed");
} catch (...) {
  return fortis::test::ReportException();
}
