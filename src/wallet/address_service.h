#pragma once

#include <memory>
#include "status.h"

#include "clock.h"
#include "ownership.h"
#include "http_server.h"
#include "provider_runtime_config.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }
namespace fortis::wallet::address { class Store; }
namespace fortis::chains { class Transport; }
namespace fortis::prices { class PriceHistoryService; }
namespace fortis::providers { class ProviderExecutionCoordinator; }
namespace fortis::prices { class SpotProviderOwner; }

namespace fortis::wallet {

// Normal startup passes none and uses Transport::Start(). A replay binary
// supplies a start that redirects the transport (Transport::StartRedirected).
using AddressTransportStarter = fortis::base::Starter<fortis::chains::Transport>;

// Test seams. A normal composition passes none: the wall clock, CSPRNG
// jitter and the recurring jobs on their own scheduler thread.
struct AddressServiceSeams {
  // The wall clock (a negative NowMs means the real one) and the jitter.
  fortis::base::Clock* clock = nullptr;
  fortis::base::JitterSource* jitter = nullptr;
  bool manual_jobs = false;
};

}  // namespace fortis::wallet

namespace fortis::wallet::address {

// The transfers route's query: the TypeScript zod transfersQuery
// over fast-querystring's parse of the raw request target. Every string is
// decoded, valid UTF-8 and copied with its byte length (an escaped NUL is
// part of the value; the copies are NUL-terminated after it).
struct TransfersQueryInput {
  bool has_since = false;
  size_t since_length = 0;
  char since[200]{};       // z.string().max(40)
  bool has_before = false;
  size_t before_length = 0;
  char before[2400]{};     // z.string().max(600)
  bool refresh = false;    // z.enum(['true'])
  double limit = 100;      // z.coerce.number().int().min(1).max(500).default(100)
  bool has_token = false;
  size_t token_length = 0;
  char token[600]{};       // z.string().max(120)
  bool has_price = false;
  char price_id[128]{};    // trimmed, lowercased, /^[a-z0-9][a-z0-9-]{0,99}$/
  bool has_from = false;
  double from = 0;         // z.coerce.number().int().min(0).max(253402300799)
  bool has_to = false;
  double to = 0;
  int flow = -1;           // 0 in, 1 out, 2 neutral
};

// False when zod refuses the query (400 {"error":"BAD_REQUEST"}): a
// duplicate key (an array), a bound, a coercion, or one of the refinements
// (non-empty `since` and `before` together, `from` after `to`, `flow`
// without `token`).
bool ParseTransfersQuery(const char* raw_target, TransfersQueryInput* out);

}  // namespace fortis::wallet::address

namespace fortis::wallet {

// The address, transaction and network routes behind
// FORTIS_NATIVE_ADDRESS_CANARY=1 (which requires a validated CA bundle):
//   GET /v1/networks/:network/address/:address/balances       (cost 1)
//   GET /v1/networks/:network/address/:address/insight        (cost 1)
//   GET /v1/networks/:network/address/:address/nfts           (cost 20)
//   GET /v1/networks/:network/address/:address/token-holdings (cost 5)
//   GET /v1/networks/:network/address/:address/transfers      (cost 10)
//   GET /v1/networks/:network/tx/:hash                        (cost 1)
//   GET /v1/networks/:network/tx/:hash/view                   (cost 1)
//   GET /v1/networks/:network/overview                        (cost 1)
//   POST /v1/sync (sync_route.h)                              (cost 1)
// Validation, auth and quota answer on the HTTP worker; the facets run on
// three foreground workers (32 queue slots) that block on chain provider
// batches and answer through the response handoff. One revalidation worker
// runs stale-while-revalidate refreshes (64 slots), one ingest worker the
// delta polls' background transfer ingests (16 slots), and one scheduler
// thread the recurring jobs: pending transaction polling (5 s, first after
// 5 s, no jitter), EVM change detection (60 s), operation candle repair
// (10 min) and transfer retention (daily); each other first run at a random
// point of its interval, later runs completion-scheduled with +-10 % jitter.
// Per-task arenas (16 MiB foreground, 8 MiB revalidation, 24 MiB ingest and
// job) are allocated per task and freed after it.
//
// `spot` (the price canary's spot owner) prices balances; null answers every
// price as null. `history` (the price canary's history service) feeds the
// operation candle repair; null leaves repairs pending. The overview reads
// the durable news store (the news canary's ingest fills it).
struct AddressServiceDeps {
  storage::KvStore& kv;
  providers::ProviderExecutionCoordinator& coordinator;
  prices::SpotProviderOwner* spot = nullptr;
  prices::PriceHistoryService* history = nullptr;
};
struct AddressServiceOptions {
  AddressTransportStarter* start_transport = nullptr;
  const AddressServiceSeams* seams = nullptr;
};

// Factory-created resource handle. Moved-from handles are destruction-only.
class AddressService {
 public:
  enum class Job : uint8_t { kPendingPoll, kChangeDetection, kCandleRepair, kRetention };

  AddressService(AddressService&&) noexcept;
  AddressService& operator=(AddressService&&) = delete;
  ~AddressService();
  AddressService(const AddressService&) = delete;
  AddressService& operator=(const AddressService&) = delete;

  static base::Result<AddressService> Create(const config::ProviderRuntimeConfig& config, AddressServiceDeps deps,
                                      const AddressServiceOptions& options = {});
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);

  // Runs one job on the caller's thread (serialized with the scheduler);
  // `summary` receives a short JSON result.
  bool RunJobNow(Job job, char* summary, size_t capacity);
  // Waits until the revalidation and ingest queues are empty and idle.
  bool WaitIdle(uint32_t timeout_ms);
  // The durable address store (journal, watches) and the service clock;
  // test binaries seed fixtures through them.
  fortis::wallet::address::Store* store();
  int64_t NowMs() const;

  struct Impl;

 private:
  explicit AddressService(Impl* impl);
  void Stop();
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::wallet
