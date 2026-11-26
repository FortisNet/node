#pragma once

#include "clock.h"
#include "http_server.h"
#include "sync_journal.h"
#include "sync_scope.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }
namespace fortis::wallet::address { class Store; }

// POST /v1/sync (the TypeScript sync route and SyncFacet): one device-scoped
// page of
// transfer changes from the durable journal, JSON or protobuf both ways,
// answered `private, no-store` and never cached. The request is a read: the
// server keeps no per-device cursor (the app checkpoints the opaque cursor
// after applying a batch). Served on the HTTP worker (KV reads only, no
// provider work), with per-request memory: an arena of at most
// kSyncArenaBytes (request strings, the scope, one parsed event at a time)
// and a response of at most kSyncResponseBytes, the router's inline capture
// bound. A page that would pass it ends early with hasMore and the cursor at
// its last event (the source answers the whole page; the app pages on
// hasMore either way).
//
// Composed by AddressService behind FORTIS_NATIVE_ADDRESS_CANARY=1: the
// journal and watches it reads are that service's store. The full contract
// (check order, parsers including form bodies, validation, paging, wire
// formats, isolation, persistence and native differences) is the comment
// on Route::Serve in sync_route.cpp.
namespace fortis::wallet::sync {

constexpr size_t kSyncArenaBytes = 8 * 1024 * 1024;
constexpr size_t kSyncResponseBytes = 2 * 1024 * 1024;  // http_server.cpp kCapturedResponseLimit

class Route {
 public:
  Route() = default;
  ~Route();
  Route(const Route&) = delete;
  Route& operator=(const Route&) = delete;

  // `clock` (NowMs) is the wall clock in production (a test clock in test
  // binaries); it must outlive the route.
  bool Start(const storage::KvStore* kv, address::Store* store, bool attestation_required, base::Clock* clock);
  void Stop();
  bool Register(http::HttpRouter& router);

 private:
  static void Handle(int client, const http::HttpRequest& request, bool head, void* context);
  void Serve(int client, const http::HttpRequest& request);

  const storage::KvStore* kv_ = nullptr;
  Scope scope_;
  bool attestation_required_ = false;
  base::Clock* clock_ = nullptr;
  bool started_ = false;
};

}  // namespace fortis::wallet::sync
