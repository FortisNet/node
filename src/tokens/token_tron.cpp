#include "token_tron.h"
#include "ownership.h"

#include "provider_runtime_config.h"
#include "threading.h"
#include "token_fetchers.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_tron {
namespace {

// More than the threads that can compute token facts at once (two
// foreground workers, the revalidation worker and the job worker); a full
// registry reads solo rather than refusing.
constexpr size_t kFlightSlots = 8;
constexpr size_t kKeyBytes = 200;

bool is(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

timespec realtime_after(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

void tronscan_fetch(providers::TokenFetch* fetch, const char* path, bool background) {
  *fetch = providers::TokenFetch{};
  fetch->upstream = providers::TokenUpstream::kTronScan;
  fetch->background = background;
  snprintf(fetch->path, sizeof(fetch->path), "%s", path);
}

bool trongrid_fetch(providers::TokenFetch* fetch, const networks::NativeNetworkConfig* seed, const char* path, bool background) {
  *fetch = providers::TokenFetch{};
  fetch->upstream = providers::TokenUpstream::kTronGrid;
  fetch->background = background;
  fetch->rpc_urls = seed->rpc_urls;
  fetch->rpc_url_count = seed->rpc_url_count;
  const int n = snprintf(fetch->path, sizeof(fetch->path), "%s", path);
  return n > 0 && static_cast<size_t>(n) < sizeof(fetch->path);
}

// The chain requests of onChainSnapshot / onChainTrc10Snapshot. False when
// the network has no endpoint (DriverTransport's RangeError, caught as null).
bool chain_fetches(const networks::NativeNetworkConfig* seed, const char* identifier, bool trc10, bool background,
                   providers::TokenFetch* out, size_t* count) {
  *count = 0;
  if (!seed || !seed->rpc_url_count) return false;
  if (trc10) {
    if (!trongrid_fetch(&out[0], seed, "/wallet/getassetissuebyid", background) ||
        !chains::tron_token::AssetIssueBody(identifier, out[0].rpc_body, sizeof(out[0].rpc_body),
                                    &out[0].rpc_body_length))
      return false;
    *count = 1;
    return true;
  }
  for (size_t k = 0; k < 4; ++k) {
    if (!trongrid_fetch(&out[k], seed, "/wallet/triggerconstantcontract", background) ||
        !chains::tron_token::ViewBody(identifier, identifier, chains::tron_token::kOnChainSelectors[k], "", out[k].rpc_body,
                              sizeof(out[k].rpc_body), &out[k].rpc_body_length))
      return false;
  }
  *count = 4;
  return true;
}

bool any_cancelled(const providers::TokenFetch* fetches, size_t count) {
  for (size_t i = 0; i < count; ++i) if (fetches[i].cancelled) return true;
  return false;
}

// The parsed 2xx JSON body, or nullptr for a failed request (an HttpError,
// a transport failure, a busy lane, invalid JSON).
const tj::Value* json_body(tj::Arena& a, const providers::TokenFetch& fetch) {
  tj::Value* body = nullptr;
  if (token_fetch::Json(a, fetch, &body) != token_fetch::Outcome::kValue) return nullptr;
  return body ? body : tj::Null(a);
}

// HttpClient.json for a TronScan GET: a 2xx whose JSON does not parse is a
// transport-class failure, retried like a 5xx (three attempts in all). The
// parsed body, or nullptr when the call failed.
const tj::Value* tronscan_body(token_facts::Work& work, providers::TokenFetch& fetch) {
  for (int attempt = 0;; ++attempt) {
    if (fetch.cancelled) return nullptr;
    const tj::Value* body = json_body(work.arena, fetch);
    if (body || !TokenFetchOk(fetch) || attempt == 2) return body;
    char path[providers::kTokenMaxPathBytes];
    memcpy(path, fetch.path, sizeof(path));
    const bool anonymous = fetch.anonymous;
    TokenFetchRelease(&fetch);
    tronscan_fetch(&fetch, path, work.background);
    fetch.anonymous = anonymous;
    work.env->executor->FetchAll(&fetch, 1, work.deadline_ms);
  }
}

Outcome interpret_chain(tj::Arena& a, const providers::TokenFetch* fetches, size_t count, const char* identifier, bool trc10,
                        chains::tron_token::Snapshot* out) {
  using chains::tron_token::Read;
  Read read = Read::kNull;
  if (trc10) {
    const tj::Value* body = json_body(a, fetches[0]);
    read = body ? chains::tron_token::OnChainTrc10(a, body, identifier, out) : Read::kNull;
  } else if (count == 4) {
    const tj::Value* bodies[4] = {};
    for (size_t k = 0; k < 4; ++k) bodies[k] = json_body(a, fetches[k]);
    read = chains::tron_token::OnChainTrc20(a, bodies, out);
  }
  if (read == Read::kThrow) return Outcome::kFailed;  // arena exhaustion only
  return read == Read::kSnapshot ? Outcome::kSnapshot : Outcome::kNull;
}

void release(providers::TokenFetch* fetches, size_t count) {
  for (size_t i = 0; i < count; ++i) TokenFetchRelease(&fetches[i]);
}

// The provider stages of one snapshot read; `batch[0..extra)` ride stage 1.
Outcome compute(token_facts::Work& work, const networks::NativeNetworkConfig* seed, const char* identifier,
                providers::TokenFetch* batch, size_t extra, chains::tron_token::Snapshot* out) {
  tj::Arena& a = work.arena;
  const bool trc10 = chains::tron_token::IsTrc10Id(identifier, strlen(identifier));
  const config::ProviderRuntimeConfig* config = work.env->config;
  const bool keyed = config && config->tronscan_api_key_configured;
  providers::TokenFetch* stage = batch + extra;
  size_t count = 0;
  if (keyed) {
    char path[providers::kTokenMaxPathBytes];
    const bool built = trc10 ? chains::tron_token::Trc10Path(identifier, path, sizeof(path))
                             : chains::tron_token::Trc20Path(identifier, path, sizeof(path));
    if (!built) {
      if (extra) work.env->executor->FetchAll(batch, extra, work.deadline_ms);
      return Outcome::kFailed;
    }
    tronscan_fetch(stage, path, work.background);
    count = 1;
  } else if (!chain_fetches(seed, identifier, trc10, work.background, stage, &count)) {
    if (extra) work.env->executor->FetchAll(batch, extra, work.deadline_ms);
    return count ? Outcome::kFailed : Outcome::kNull;
  }
  work.env->executor->FetchAll(batch, extra + count, work.deadline_ms);
  if (keyed) {
    providers::TokenFetch& tronscan = stage[0];
    const tj::Value* body = tronscan_body(work, tronscan);
    // TronscanClient.get: a rejected key is re-issued once anonymously.
    if (!body && tronscan.answered &&
        (tronscan.status == 401 || tronscan.status == 403 || tronscan.status == 429)) {
      TokenFetchRelease(&tronscan);
      char path[providers::kTokenMaxPathBytes];
      memcpy(path, tronscan.path, sizeof(path));
      tronscan_fetch(&tronscan, path, work.background);
      tronscan.anonymous = true;
      work.env->executor->FetchAll(&tronscan, 1, work.deadline_ms);
      body = tronscan_body(work, tronscan);
    }
    const bool cancelled = tronscan.cancelled;
    TokenFetchRelease(&tronscan);
    if (cancelled) return Outcome::kFailed;
    if (body) {
      const chains::tron_token::Read read = trc10 ? chains::tron_token::TronscanTrc10(a, body, identifier, out)
                                          : chains::tron_token::TronscanTrc20(a, body, identifier, out);
      if (read == chains::tron_token::Read::kSnapshot) return Outcome::kSnapshot;
      if (!a.ok) return Outcome::kFailed;
    }
    // The optional index is enrichment: the chain is the truthful fallback.
    if (!chain_fetches(seed, identifier, trc10, work.background, stage, &count))
      return count ? Outcome::kFailed : Outcome::kNull;
    work.env->executor->FetchAll(stage, count, work.deadline_ms);
  }
  if (any_cancelled(stage, count)) { release(stage, count); return Outcome::kFailed; }
  const Outcome outcome = interpret_chain(a, stage, count, identifier, trc10, out);
  release(stage, count);
  return outcome;
}

}  // namespace

struct Flights {
  struct Flight {
    bool used = false;
    bool done = false;
    Outcome outcome = Outcome::kFailed;
    char key[kKeyBytes]{};
    std::unique_ptr<uint8_t[], base::StorageDeleter> json;  // the snapshot document (kSnapshot only)
    size_t length = 0;
    size_t refs = 0;
  };
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable settled;
  Flight slots[kFlightSlots];

  ~Flights() {
    for (Flight& f : slots) f.json.reset();


  }

  // Drops one reference; the last one frees the slot. Called locked.
  void Leave(Flight& f) {
    if (--f.refs) return;
    f.json.reset();
    f = Flight{};
  }
};

Flights* NewFlights() {
  return fortis::base::New<Flights>();
}

void DeleteFlights(Flights* flights) {
  fortis::base::Delete(flights);
}

bool Snapshotted(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  return is(network.family, "tron") && ref.has_address &&
         (chains::tron_token::IsTrc10Id(ref.address, strlen(ref.address)) ||
          chains::tron_token::IsAddress(ref.address, strlen(ref.address)));
}

bool HoldersSupported(const config::ProviderRuntimeConfig* config, const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  return config && config->tronscan_api_key_configured && is(network.family, "tron") && ref.has_address;
}

Outcome Read(token_facts::Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed,
             const char* identifier, providers::TokenFetch* batch, size_t extra, chains::tron_token::Snapshot* snapshot) {
  tj::Arena& a = work.arena;
  *snapshot = chains::tron_token::Snapshot{};
  Flights* flights = work.env->tron_flights;
  char key[kKeyBytes];
  const int n = snprintf(key, sizeof(key), "%s:%s", network.id, identifier);
  Flights::Flight* flight = nullptr;
  bool leader = true;
  if (flights && n > 0 && static_cast<size_t>(n) < sizeof(key)) {
    fortis::base::LockGuard lock(flights->mutex);
    for (Flights::Flight& f : flights->slots)
      if (f.used && !f.done && strcmp(f.key, key) == 0) { flight = &f; leader = false; break; }
    if (!flight) {
      for (Flights::Flight& f : flights->slots) if (!f.used) { flight = &f; break; }
      if (flight) {
        flight->used = true;
        flight->done = false;
        memcpy(flight->key, key, static_cast<size_t>(n) + 1);
      }
    }
    if (flight) ++flight->refs;
  }

  if (leader) {
    const Outcome outcome = compute(work, seed, identifier, batch, extra, snapshot);
    if (!flight) return outcome;
    std::unique_ptr<uint8_t[], base::StorageDeleter> json;
    size_t length = 0;
    Outcome published = outcome;
    if (outcome == Outcome::kSnapshot) {
      // Serialize for the joiners: the supply digits are the only long text.
      tj::Value* doc = chains::tron_token::ToJson(a, snapshot);
      size_t capacity = 4096;
      if (snapshot->total_supply_raw) capacity += snapshot->total_supply_raw->length;
      if (snapshot->symbol) capacity += 6 * snapshot->symbol->length;
      if (snapshot->name) capacity += 6 * snapshot->name->length;
      json = base::AllocateStorage<uint8_t[]>(capacity);
      tj::OutBuffer out{json.get(), capacity, 0, true};
      if (!doc || !json || !tj::Stringify(doc, out) || !out.ok) {
        json.reset();
        json = nullptr;
        published = Outcome::kFailed;
      } else {
        length = out.used;
      }
    }
    fortis::base::LockGuard lock(flights->mutex);
    flight->done = true;
    flight->outcome = published;
    flight->json = std::move(json);
    flight->length = length;
    flights->Leave(*flight);
    flights->settled.Broadcast();
    return outcome;
  }

  // A joiner: its own requests first, then the running flight's answer.
  if (extra) work.env->executor->FetchAll(batch, extra, work.deadline_ms);
  Outcome outcome = Outcome::kFailed;
  fortis::base::LockGuard flights_lock(flights->mutex);
  while (!flight->done) {
    const uint64_t now = providers::TokenMonotonicMs();
    if (now >= work.deadline_ms) break;
    const timespec wake = realtime_after(work.deadline_ms - now < 1000 ? work.deadline_ms - now : 1000);
    flights->settled.WaitUntilLocked(flights->mutex, wake);
  }
  if (flight->done) {
    outcome = flight->outcome;
    if (outcome == Outcome::kSnapshot) {
      bool present = false;
      tj::Value* doc = tj::Parse(a, flight->json.get(), flight->length);
      if (!doc || !chains::tron_token::FromJson(a, doc, snapshot, &present) || !present) outcome = Outcome::kFailed;
    }
  }
  flights->Leave(*flight);
  return outcome;
}

}  // namespace fortis::tokens::token_tron
