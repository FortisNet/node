#include "ownership.h"
#include "exception_boundary.h"
// The Robinhood service's job side: the workers, the upstream reads behind
// each route (catalog, instrument, diagnose, prepare, submit), the replies and
// the lifecycle. The HTTP-worker side is robinhood_routes.cpp and the pure
// request steps are robinhood_write_requests.cpp.
#include "robinhood_service_internal.h"
#include "threading.h"

namespace cc = fortis::exchange::cross_chain;

namespace rh = fortis::exchange::robinhood;

namespace tj = fortis::codec::tj;

namespace fortis::exchange {

namespace {

constexpr const char* kTextType = "text/plain; charset=utf-8";

constexpr size_t kResponseLimit = 2 * 1024 * 1024;  // http_server.cpp kCapturedResponseLimit

constexpr const char* kSimulationBase = "https://robinhood-mainnet.g.alchemy.com";
constexpr const char* kWalletBase = "https://api.g.alchemy.com";

http::HttpResponse internal_error(const Policy& policy) {
  return json_response(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", policy);
}
http::HttpResponse upstream_unavailable(const Policy& policy) {
  return json_response(502, "Bad Gateway", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", policy);
}

}  // namespace

namespace robinhood_service_internal {

uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

http::HttpResponse json_response(int status, const char* reason, const char* body, const Policy& policy) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = policy.cost;
  response.rate_limit_remaining = policy.remaining;
  return response;
}

http::HttpResponse bad_request(const Policy& policy) {
  return json_response(400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", policy);
}

http::HttpResponse sponsored_unavailable(const Policy& policy) {
  return json_response(503, "Service Unavailable", "{\"error\":\"SPONSORED_TRADES_UNAVAILABLE\"}", policy);
}

// replyBackPressure(): 429 RATE_LIMITED with retry-after (seconds, >= 1).
http::HttpResponse back_pressure(uint32_t retry_after_ms, const Policy& policy) {
  http::HttpResponse response = json_response(
      429, "Too Many Requests",
      "{\"error\":\"RATE_LIMITED\",\"detail\":\"Upstream data is temporarily busy; retained data remains available.\"}",
      policy);
  const uint32_t ms = retry_after_ms ? retry_after_ms : 2000;
  const uint32_t seconds = (ms + 999) / 1000;
  response.retry_after = seconds < 1 ? 1 : seconds;
  return response;
}

}  // namespace robinhood_service_internal

// capabilities(): both documents are built once at Start and immutable after.
bool RobinhoodService::Impl::BuildCapabilities() {
  return rh::EncodeSponsoredCapabilities(sponsored_enabled, capabilities_json, sizeof(capabilities_json),
                                         &capabilities_json_length, capabilities_proto,
                                         sizeof(capabilities_proto), &capabilities_proto_length);
}

void RobinhoodService::Impl::Release(Job* job) {
  if (job->claimed) {
    // A job that never reached Alchemy leaves its ticket claimable.
    tickets.Finish(job->claim, false);
  }
  job->claim.prepared.reset();
  tj::FreeArena(job->arena);
  fortis::base::LockGuard lock(mutex);
  *job = Job{};
  idle.Broadcast();
}

// ---- workers ----

void* RobinhoodService::Impl::WorkerMain(void* opaque) {
  auto* side = static_cast<Side*>(opaque);
  side->impl->WorkerLoop(*side);
  return nullptr;
}

void* RobinhoodService::Impl::SideMain(void* opaque) {
  auto* side = static_cast<Side*>(opaque);
  side->impl->SideLoop(*side);
  return nullptr;
}

// The companion: one posted corporate-action read at a time.
void RobinhoodService::Impl::SideLoop(Side& side) {
  fortis::base::UniqueLock lock(side.mutex);
  for (;;) {
    while (!side.pending && !side.stop) side.changed.Wait(lock);
    if (!side.pending) break;
    Job* job = side.job;
    lock.Unlock();
    std::shared_ptr<const rh::StockClient::Snapshot> held = nullptr;
    const rh::Outcome outcome = client.CorporateActions(job->deadline, &held, job->actions_issued);
    lock.Lock();
    // Promise.all rejects with the first rejection.
    const bool answer = !outcome.ok() && !side.answered;
    if (answer) side.answered = true;
    lock.Unlock();
    if (answer) Fail(job, outcome, true);
    lock.Lock();
    side.held = held;
    side.pending = false;
    side.done = true;
    side.changed.Broadcast();
  }
}

void RobinhoodService::Impl::WorkerLoop(Side& side) {
  for (;;) {
    fortis::base::UniqueLock lock(mutex);
    while (!queue_count && !stopping) changed.Wait(lock);
    if (!queue_count && stopping) return;
    Job* job = queue[queue_head];
    queue_head = (queue_head + 1) % kJobs;
    --queue_count;
    lock.Unlock();
    switch (job->kind) {
      case Kind::kCatalog: RunCatalog(job); break;
      case Kind::kInstrument: RunInstrument(job, side); break;
      case Kind::kDiagnose: RunDiagnose(job); break;
      case Kind::kPrepare: RunPrepare(job); break;
      case Kind::kSubmit: RunSubmit(job); break;
    }
    Release(job);
  }
}

// A failed read answers as its route maps the error: runFacet's 502 (or
// 429 for back-pressure) for the two GET routes, the error handler's 500
// (or 429) for the three POST routes.
void RobinhoodService::Impl::Fail(Job* job, const rh::Outcome& outcome, bool facet) {
  if (outcome.failure == rh::Failure::kLaneBusy) Complete(job, back_pressure(outcome.retry_after_ms, job->policy));
  else Complete(job, facet ? upstream_unavailable(job->policy) : internal_error(job->policy));
}

// sendProtobufOrJson: JSON (a string payload is sent as text) or protobuf.
void RobinhoodService::Impl::Respond(Job* job, const Value* value, Encoder encode) {
  const bool text = !job->protobuf && tj::IsString(value);
  size_t text_length = 0;
  const char* text_bytes = text ? cc::WellFormed(job->arena, value->text, value->length, &text_length) : nullptr;
  OutBuffer measure{nullptr, 0, 0, true};
  const bool measured = text ? text_bytes != nullptr
                        : job->protobuf ? encode(measure, value)
                                        : tj::Stringify(value, measure);
  if (text) measure.used = text_length;
  http::HttpResponse response;
  std::unique_ptr<uint8_t[], base::StorageDeleter> buffer_storage;
  uint8_t* buffer = nullptr;
  if (!measured || !measure.ok) {
    // A failing toBinary throws after Vary was set: the error handler's 500.
    response = internal_error(job->policy);
    response.vary_accept = true;
  } else if (measure.used > kResponseLimit) {
    response = json_response(500, "Internal Server Error", "{\"error\":\"RESPONSE_TOO_LARGE\"}", job->policy);
  } else {
    buffer_storage = base::AllocateStorage<uint8_t[]>(measure.used ? measure.used : 1);
      buffer = buffer_storage.get();
    OutBuffer out{buffer, measure.used, 0, buffer != nullptr};
    bool written = buffer != nullptr;
    if (written && text) out.Append(text_bytes, text_length);
    else if (written) written = job->protobuf ? encode(out, value) : tj::Stringify(value, out);
    written = written && out.ok;
    if (!written) {
      response = internal_error(job->policy);
    } else {
      response = http::HttpResponse{200, "OK", job->protobuf ? kProtobufType : text ? kTextType : kJsonType, buffer, out.used};
      response.vary_accept = true;
      response.rate_limit_cost = job->policy.cost;
      response.rate_limit_remaining = job->policy.remaining;
    }
  }
  Complete(job, response);
  buffer_storage.reset();
}

rh::MarketsRequest RobinhoodService::Impl::Markets(const Job* job) {
  rh::MarketsRequest request;
  request.client = &client;
  request.kv = kv;
  request.now_ms = Now();
  request.deadline_ms = job->deadline;
  request.assets_issued = job->assets_issued;
  request.quote_issued = job->quote_issued;
  request.actions_issued = job->actions_issued;
  return request;
}

void RobinhoodService::Impl::RunCatalog(Job* job) {
  Value* result = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> held = nullptr;
  const rh::Outcome outcome = rh::Catalog(job->arena, Markets(job), &result, &held);
  if (!outcome.ok()) Fail(job, outcome, true);
  else Respond(job, result, rh::EncodeCatalog);
  held.reset();
}

// RobinhoodMarketsFacet.instrument: the asset, then Promise.all of the
// quote (this worker) and the corporate actions (the companion), started
// together. The first failure answers at once; the other read still runs
// to its end (it may fill a cache), and the job is released only then.
void RobinhoodService::Impl::RunInstrument(Job* job, Side& side) {
  std::shared_ptr<const rh::StockClient::Snapshot> assets = nullptr;
  const Value* asset = nullptr;
  const rh::Outcome found = rh::FindInstrument(Markets(job), job->address, &asset, &assets);
  if (!found.ok() || !asset) {
    if (!found.ok()) Fail(job, found, true);
    else Complete(job, json_response(404, "Not Found", "{\"error\":\"NOT_FOUND\"}", job->policy));
    assets.reset();
    return;
  }
  fortis::base::UniqueLock lock(side.mutex);
  side.job = job;
  side.answered = false;
  side.done = false;
  side.held = nullptr;
  side.pending = true;
  side.changed.Broadcast();
  lock.Unlock();
  Value* quote = nullptr;
  const rh::Outcome quoted = client.ReferenceQuote(job->arena, job->deadline, asset, &quote, false, job->quote_issued);
  lock.Lock();
  const bool answer = !quoted.ok() && !side.answered;
  if (answer) side.answered = true;
  lock.Unlock();
  if (answer) Fail(job, quoted, true);
  lock.Lock();
  while (!side.done) side.changed.Wait(lock);
  const bool answered = side.answered;
  std::shared_ptr<const rh::StockClient::Snapshot> actions = side.held;
  side.held = nullptr;
  side.job = nullptr;
  lock.Unlock();
  if (!answered) {
    Value* result = nullptr;
    if (rh::BuildInstrument(job->arena, Markets(job), asset, quote, actions.get(), &result))
      Respond(job, result, rh::EncodeInstrument);
    else
      Complete(job, upstream_unavailable(job->policy));
  }
  actions.reset();
  assets.reset();
}

// The active catalog tokens as a StockSet in the job arena.
rh::Outcome RobinhoodService::Impl::ActiveStocks(Job* job, std::shared_ptr<const rh::StockClient::Snapshot>* held, rh::StockSet* stocks) {
  const rh::Outcome outcome = client.Assets(job->deadline, held);
  if (!outcome.ok()) return outcome;
  const Value* assets = rh::StockClient::Document(held->get());
  auto* tokens = static_cast<rh::StockToken*>(job->arena.Alloc(sizeof(rh::StockToken) * (assets->count + 1)));
  rh::Outcome failed;
  failed.failure = rh::Failure::kError;
  if (!tokens) return failed;
  size_t count = 0;
  for (uint32_t i = 0; i < assets->count; ++i) {
    const Value* asset = assets->items[i];
    if (!cc::Is(tj::Get(asset, "status"), "active")) continue;
    const Value* contract = tj::Get(asset, "contractAddress");
    for (size_t c = 0; c < 42; ++c) {
      const char ch = contract->text[c];
      tokens[count].address[c] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch | 0x20) : ch;
    }
    tokens[count].address[42] = '\0';
    tokens[count].symbol = tj::Get(asset, "tokenSymbol");
    ++count;
  }
  stocks->items = tokens;
  stocks->count = count;
  return outcome;
}

// One JSON-RPC POST {jsonrpc:'2.0', id:1, method, params}.
rh::Outcome RobinhoodService::Impl::Rpc(Job* job, const char* base, const char* path, const char* lane,
                                        const char* method, Value* params, Value** body) {
  rh::Outcome outcome;
  tj::Arena& arena = job->arena;
  Value* envelope = RobinhoodRpcEnvelope(arena, method, params);
  if (!envelope) {
    outcome.failure = rh::Failure::kError;
    return outcome;
  }
  const char* const urls[] = {base};
  fortis::chains::Call call;
  call.kind = fortis::chains::CallKind::kPost;
  call.urls = urls;
  call.url_count = 1;
  call.path = path;
  call.body = envelope;
  call.lane = lane;
  transport->Run(&call, 1, arena, job->deadline);
  if (call.failure == fortis::chains::CallFailure::kNone && call.value) {
    *body = call.value;
  } else if (call.failure == fortis::chains::CallFailure::kLaneBusy) {
    outcome.failure = rh::Failure::kLaneBusy;
    outcome.retry_after_ms = call.retry_after_ms;
  } else {
    outcome.failure = rh::Failure::kError;
  }
  return outcome;
}

void RobinhoodService::Impl::RunDiagnose(Job* job) {
  tj::Arena& arena = job->arena;
  std::shared_ptr<const rh::StockClient::Snapshot> held = nullptr;
  rh::StockSet stocks;
  rh::Outcome outcome = ActiveStocks(job, &held, &stocks);
  if (!outcome.ok()) {
    Fail(job, outcome, false);
    held.reset();
    return;
  }
  const Value* from = tj::Get(job->input, "from");
  const Value* calls = tj::Get(job->input, "calls");
  bool exhausted = false;
  const Value* executable = rh::ValidateTradeCalls(arena, calls, stocks, from->text, from->length, &exhausted);
  if (!executable) {
    Complete(job, exhausted ? internal_error(job->policy) : bad_request(job->policy));
    held.reset();
    return;
  }
  rh::DiagnosticsRequest request;
  request.calls = calls;
  request.executable = executable;
  request.from = from->text;
  request.from_length = from->length;
  request.sponsored = job->sponsored;
  request.stocks = &stocks;
  request.now_ms = Now();
  Value* simulation_calls = rh::SimulationCalls(arena, request);
  Value* params = RobinhoodSimulateParams(arena, simulation_calls, job->sponsored);
  Value* body = nullptr;
  if (!params) {
    Complete(job, internal_error(job->policy));
    held.reset();
    return;
  }
  outcome = Rpc(job, kSimulationBase, simulation_path, "alchemy-simulation", "eth_simulateV1", params, &body);
  if (!outcome.ok()) {
    Fail(job, outcome, false);
    held.reset();
    return;
  }
  Value* simulation = nullptr;
  Value* message = nullptr;
  const Value* data = nullptr;
  const rh::RpcReply reply = rh::ReadRpcReply(arena, body, "eth_simulateV1", &simulation, &message, &data);
  if (reply == rh::RpcReply::kInternal) {
    Complete(job, internal_error(job->policy));
    held.reset();
    return;
  }
  if (reply == rh::RpcReply::kRpcError) {
    // decodeRevertReason(error.rpcData) ?? safeMessage(error).
    const Value* reason = rh::DecodeRevertReason(arena, data);
    if (!reason && !cc::Blank(message->text, message->length)) reason = message;
    const Value* report = rh::FailedReport(arena, request, reason);
    if (report) Respond(job, report, RobinhoodEncodeDynamic);
    else Complete(job, internal_error(job->policy));
    held.reset();
    return;
  }
  // debug_traceCallMany: any failure leaves the trace unavailable.
  Value* trace_params = RobinhoodTraceParams(arena, simulation_calls);
  const Value* trace = nullptr;
  if (trace_params) {
    Value* trace_body = nullptr;
    if (Rpc(job, kSimulationBase, simulation_path, "alchemy-simulation", "debug_traceCallMany", trace_params,
            &trace_body).ok()) {
      Value* result = nullptr;
      if (rh::ReadRpcReply(arena, trace_body, "debug_traceCallMany", &result, &message, &data) ==
              rh::RpcReply::kResult &&
          result->type != tj::Type::kNull)
        trace = result;
    }
  }
  const Value* report = rh::BuildReport(arena, request, simulation, trace);
  if (report) Respond(job, report, RobinhoodEncodeDynamic);
  else Complete(job, internal_error(job->policy));
  held.reset();
}

void RobinhoodService::Impl::RunPrepare(Job* job) {
  tj::Arena& arena = job->arena;
  std::shared_ptr<const rh::StockClient::Snapshot> held = nullptr;
  rh::StockSet stocks;
  rh::Outcome outcome = ActiveStocks(job, &held, &stocks);
  if (!outcome.ok()) {
    Fail(job, outcome, false);
    held.reset();
    return;
  }
  const Value* from = tj::Get(job->input, "from");
  Value* from_lower = cc::Lower(arena, from);
  bool exhausted = false;
  Value* executable = from_lower ? rh::ValidateTradeCalls(arena, tj::Get(job->input, "calls"), stocks, from_lower->text,
                                                         from_lower->length, &exhausted)
                                 : nullptr;
  held.reset();
  if (!executable) {
    Complete(job, exhausted || !from_lower ? internal_error(job->policy) : bad_request(job->policy));
    return;
  }
  Value* params = RobinhoodPrepareParams(arena, from_lower, executable, config.alchemy_gas_policy_id);
  Value* body = nullptr;
  if (!params) {
    Complete(job, internal_error(job->policy));
    return;
  }
  outcome = Rpc(job, kWalletBase, wallet_path, "alchemy-wallet", "wallet_prepareCalls", params, &body);
  if (!outcome.ok()) {
    Fail(job, outcome, false);
    return;
  }
  Value* prepared = nullptr;
  Value* message = nullptr;
  const Value* data = nullptr;
  Value* requests = nullptr;
  size_t nodes = 0;
  if (rh::ReadRpcReply(arena, body, "wallet_prepareCalls", &prepared, &message, &data) != rh::RpcReply::kResult ||
      !rh::ValidatePrepared(arena, prepared, from_lower->text, &requests, &nodes)) {
    Complete(job, internal_error(job->policy));
    return;
  }
  const int64_t now = Now();
  if (tickets.Pending(now) >= rh::TicketStore::kCapacity) {
    Complete(job, sponsored_unavailable(job->policy));
    return;
  }
  uint8_t random[24];
  char ticket[33];
  const bool drawn = seams.ticket_random ? seams.ticket_random->Bytes(random, sizeof(random))
                                         : fortis::auth::SecureRandomBytes(random, sizeof(random));
  OutBuffer measure{nullptr, 0, 0, true};
  tj::Stringify(prepared, measure);
  auto text_storage = base::AllocateStorage<char[]>(measure.used ? measure.used : 1);
  auto* text = text_storage.get();
  OutBuffer stored{reinterpret_cast<uint8_t*>(text), measure.used, 0, text != nullptr};
  const int64_t expires = now + rh::TicketStore::kTtlMs;
  char expires_iso[32];
  const bool ready = drawn && text && tj::Stringify(prepared, stored) && stored.ok &&
                     RobinhoodTicketText(random, ticket) &&
                     fortis::compat::token_text::IsoString(expires, expires_iso);
  if (!ready) {
    text_storage.reset();
    Complete(job, internal_error(job->policy));
    return;
  }
  const rh::TicketStore::InsertResult inserted =
      tickets.Insert(ticket, job->device_id, expires, text, stored.used, nodes, now);
  text_storage.reset();
  if (inserted != rh::TicketStore::kInserted) {
    Complete(job, sponsored_unavailable(job->policy));
    return;
  }
  Value* result = RobinhoodPrepareResult(arena, ticket, expires_iso, prepared, requests);
  if (!result) {
    Complete(job, internal_error(job->policy));
    return;
  }
  Respond(job, result, RobinhoodEncodeDynamic);
}

void RobinhoodService::Impl::RunSubmit(Job* job) {
  tj::Arena& arena = job->arena;
  const Value* signatures = tj::Get(job->input, "signatures");
  const Value* prepared = tj::Parse(arena, reinterpret_cast<const uint8_t*>(job->claim.prepared.get()), job->claim.length);
  Value* signed_call = prepared ? rh::AttachSignatures(arena, prepared, signatures->items, signatures->count) : nullptr;
  Value* params = tj::Arr(arena);
  if (!signed_call || !params || !tj::Push(arena, params, signed_call)) {
    Complete(job, internal_error(job->policy));
    return;  // Release clears the in-flight mark
  }
  Value* body = nullptr;
  const rh::Outcome outcome =
      Rpc(job, kWalletBase, wallet_path, "alchemy-wallet", "wallet_sendPreparedCalls", params, &body);
  if (!outcome.ok()) {
    Fail(job, outcome, false);
    return;
  }
  Value* result = nullptr;
  Value* message = nullptr;
  const Value* data = nullptr;
  if (rh::ReadRpcReply(arena, body, "wallet_sendPreparedCalls", &result, &message, &data) != rh::RpcReply::kResult) {
    Complete(job, internal_error(job->policy));
    return;
  }
  // Delivered: the ticket is spent before the wallet sees the result.
  tickets.Finish(job->claim, true);
  job->claimed = false;
  Respond(job, result, RobinhoodEncodeDynamic);
}

bool RobinhoodService::Impl::WaitIdle(uint32_t timeout_ms) {
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  fortis::base::UniqueLock lock(mutex);
  for (;;) {
    bool busy = queue_count > 0;
    for (const Job& job : jobs) busy = busy || job.used;
    if (!busy) return true;
    if (monotonic_ms() >= deadline) return false;
    timespec wake{};
    clock_gettime(CLOCK_REALTIME, &wake);
    wake.tv_nsec += 50 * 1000 * 1000;
    if (wake.tv_nsec >= 1000000000L) { ++wake.tv_sec; wake.tv_nsec -= 1000000000L; }
    idle.WaitUntil(lock, wake);
  }
}

void RobinhoodService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

RobinhoodService::~RobinhoodService() { Stop(); }

RobinhoodService::RobinhoodService(Impl* impl) : impl_(impl) {}
RobinhoodService::RobinhoodService(RobinhoodService&&) noexcept = default;

base::Result<RobinhoodService> RobinhoodService::Create(
    const RobinhoodConfig& config, RobinhoodServiceDeps deps, const RobinhoodServiceSeams* seams) try {
  auto& coordinator = deps.coordinator;
  auto* kv = deps.kv;
  Impl* impl = fortis::base::New<Impl>();
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "robinhood allocation");
  RobinhoodService service(impl);
  impl->config = config;
  impl->kv = kv;
  if (seams) impl->seams = *seams;
  impl->diagnostics_enabled = config.alchemy_api_key_configured;
  impl->sponsored_enabled =
      rh::SponsoredTradesEnabled(config.alchemy_api_key_configured, config.alchemy_gas_policy_id_configured);
  snprintf(impl->wallet_path, sizeof(impl->wallet_path), "/v2/%s", config.alchemy_api_key);
  snprintf(impl->simulation_path, sizeof(impl->simulation_path), "/v2/%s", config.alchemy_api_key);
  bool ok = impl->BuildCapabilities();
  if (ok) {
    impl->transport.reset(fortis::base::New<fortis::chains::Transport>(config.transport, &coordinator));
    ok = impl->transport != nullptr;
  }
  if (ok) {
    ok = impl->seams.transport_starter ? impl->seams.transport_starter->Start(*impl->transport)
                                       : impl->transport->Start();
  }
  impl->client.Configure(impl->transport.get(), &impl->Clock());
  pthread_attr_t attributes;
  ok = ok && pthread_attr_init(&attributes) == 0;
  if (ok) {
    ok = pthread_attr_setstacksize(&attributes, 256 * 1024) == 0;
    for (size_t i = 0; ok && i < kWorkers; ++i) {
      impl->sides[i].impl = impl;
      ok = (impl->sides[i].thread.StartEntry<Impl::SideMain>(&impl->sides[i], &attributes) ? 0 : -1) == 0;
      if (ok) ++impl->started_sides;
    }
    for (size_t i = 0; ok && i < kWorkers; ++i) {
      ok = (impl->workers[i].StartEntry<Impl::WorkerMain>(&impl->sides[i], &attributes) ? 0 : -1) == 0;
      if (ok) ++impl->started_workers;
    }
    pthread_attr_destroy(&attributes);
  }
  if (!ok) {
    return base::Status(base::StatusCode::kUnavailable, "robinhood initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "robinhood allocation");
}

bool RobinhoodService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  impl_->handoff = &handoff;
  return router.Add<&Impl::HandleCatalog>("GET", "/v1/networks/robinhood/stock-tokens", *impl_) &&
         router.Add<&Impl::HandleInstrument>("GET", "/v1/networks/robinhood/stock-tokens/:address", *impl_) &&
         router.Add<&Impl::HandleCapabilities>("GET", "/v1/networks/robinhood/sponsored-trades/capabilities", *impl_) &&
         router.Add<&Impl::HandleDiagnose>("POST", "/v1/networks/robinhood/transactions/diagnose", *impl_) &&
         router.Add<&Impl::HandlePrepare>("POST", "/v1/networks/robinhood/sponsored-trades/prepare", *impl_) &&
         router.Add<&Impl::HandleSubmit>("POST", "/v1/networks/robinhood/sponsored-trades/submit", *impl_);
}

void RobinhoodService::Stop() {
  if (!impl_) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  for (size_t i = 0; i < impl_->started_workers; ++i) impl_->workers[i].Join();
  // The workers are gone, so no companion has a posted read.
  for (size_t i = 0; i < impl_->started_sides; ++i) {
    Impl::Side& side = impl_->sides[i];
    fortis::base::UniqueLock side_lock(side.mutex);
    side.stop = true;
    side.changed.Broadcast();
    side_lock.Unlock();
    side.thread.Join();
  }
  if (impl_->transport) {
    impl_->transport->Stop();
    impl_->transport.reset();
  }
  impl_.reset();
}

bool RobinhoodService::WaitIdle(uint32_t timeout_ms) { return impl_->WaitIdle(timeout_ms); }

}  // namespace fortis::exchange
