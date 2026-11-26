// Test probes of the address, transaction, sync and network-overview plane
//. Compiled empty without FORTIS_ADDRESS_ROUTE_TEST_ONLY.
#include "test_probes.h"

#ifdef FORTIS_ADDRESS_ROUTE_TEST_ONLY
#include "address_facets.h"
#include "address_store.h"
#include "app_support.h"
#include "chain_transport.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "kv_store.h"
#include "news_store.h"
#include "outbound_https.h"
#include "service_wiring.h"
#include "test_hook_env.h"
#include "token_json.h"
#include "token_swr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifdef FORTIS_UNIKRAFT
// This target has no C++ standard-library headers; provider_execution.cpp
// defines the placement-new primitive for malloc-owned objects.
void* operator new(size_t, void* storage) noexcept;
#else
#include <new>
#endif

namespace fortis::app {
namespace {

bool allow_address_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// The address routes' clock: FORTIS_TEST_ADDRESS_NOW_FILE lets one process
// move it between requests; otherwise the wall clock (-1).
int64_t address_test_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_ADDRESS_NOW_FILE");
  if (now_file && *now_file) {
    if (FILE* handle = fopen(now_file, "r")) {
      char buffer[32]{};
      const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
      fclose(handle);
      buffer[length] = '\0';
      char* end = nullptr;
      const long long parsed = strtoll(buffer, &end, 10);
      if (end && end != buffer && parsed >= 0 && (!*end || *end == '\n'))
        return static_cast<int64_t>(parsed);
    }
  }
  return -1;
}

int64_t address_test_sweep_now_ms() {
  const int64_t fixed = address_test_now_ms();
  if (fixed >= 0) return fixed;
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// The TypeScript fixtures run with coordinatorRand () => 0.5: no jitter.
bool address_test_jitter(double* sample) {
  *sample = 0.5;
  return true;
}

TestFnClock<address_test_now_ms> address_test_clock;
TestFnClock<address_test_sweep_now_ms> address_test_sweep_clock;
TestFnJitter<address_test_jitter> address_test_jitter_source;

struct AddressTestUpstream final : wallet::AddressTransportStarter {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(fortis::chains::Transport& transport) override {
    return transport.StartRedirected("localhost", port, ca, allow_address_test_address, nullptr,
                                     "x-fortis-test-host");
  }
};

// The fixture device: premium tier.
bool seed_address_test_device(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{}; gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  char token[64];
  snprintf(token, sizeof(token), "fintel_dev_");
  for (size_t j = 0; j < 43; ++j) token[11 + j] = 'A';
  token[54] = '\0';
  // The price test route seeds its own device under the TypeScript id; this
  // one only needs to be distinct (device ids never reach the wire). The
  // sync fixture's second device uses
  // the B token.
  char token_b[64];
  snprintf(token_b, sizeof(token_b), "fintel_dev_");
  for (size_t j = 0; j < 43; ++j) token_b[11 + j] = 'B';
  token_b[54] = '\0';
  if (!auth.SeedAddressRouteFixture(token, "01ARZ3NDEKTSV4RRFFQ69G5FA1", now_ms) ||
      !auth.SeedAddressRouteFixture(token_b, "01ARZ3NDEKTSV4RRFFQ69G5FB2", now_ms))
    return false;
  // The R13 edge devices (address_balances_edges.json): a-d
  // valid development devices, e expired one second ago, r revoked.
  static const char* kEdgeIds[] = {"01ARZ3NDEKTSV4RRFFQ69G5HA1", "01ARZ3NDEKTSV4RRFFQ69G5HB2",
                                   "01ARZ3NDEKTSV4RRFFQ69G5HC3", "01ARZ3NDEKTSV4RRFFQ69G5HD4",
                                   "01ARZ3NDEKTSV4RRFFQ69G5HE5", "01ARZ3NDEKTSV4RRFFQ69G5HR6"};
  static const char kEdgeLetters[] = "abcder";
  // The test sweeper uses the fixture clock while device auth uses wall time.
  // Valid one-hour tokens need the fixture time so a long quota wait cannot
  // sweep them; the deliberately expired/revoked tokens use wall time so
  // their auth assertions remain meaningful.
  const int64_t configured_now_ms = address_test_now_ms();
  const int64_t fixture_now_ms = configured_now_ms >= 0 ? configured_now_ms : now_ms;
  for (size_t i = 0; i < 6; ++i) {
    char edge[55];
    memcpy(edge, "fintel_dev_", 11);
    memset(edge + 11, kEdgeLetters[i], 43);
    edge[54] = '\0';
    const int64_t seeded_ms = kEdgeLetters[i] == 'e' ? now_ms - 3601000
        : kEdgeLetters[i] == 'r' ? now_ms : fixture_now_ms;
    if (!auth.SeedPriceRouteFixture(edge, kEdgeIds[i], seeded_ms) ||
        (kEdgeLetters[i] == 'r' && !auth.Revoke(kEdgeIds[i], now_ms - 1000)))
      return false;
  }
  return true;
}

// GET /probe/test-address-job?job=<name>: runs one recurring job now.
void handle_address_test_job(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* service = static_cast<wallet::AddressService*>(opaque);
  struct Name { const char* name; wallet::AddressService::Job job; };
  static const Name kNames[] = {
      {"pending", wallet::AddressService::Job::kPendingPoll}, {"change", wallet::AddressService::Job::kChangeDetection},
      {"repair", wallet::AddressService::Job::kCandleRepair}, {"retention", wallet::AddressService::Job::kRetention},
  };
  const char* query = strchr(request.raw_target, '?');
  const char* value = query ? strstr(query, "job=") : nullptr;
  char summary[256] = "{}";
  bool ran = false;
  if (value) {
    value += 4;
    for (const Name& name : kNames) {
      const size_t length = strlen(name.name);
      if (strncmp(value, name.name, length) == 0 && (value[length] == '\0' || value[length] == '&')) {
        ran = service->RunJobNow(name.job, summary, sizeof(summary));
        break;
      }
    }
  }
  respond(client, ran ? 200 : 400, ran ? "OK" : "Bad Request", ran ? summary : "{\"error\":\"BAD_JOB\"}", head);
}

// GET /probe/test-address-idle: waits for revalidations and ingests.
void handle_address_test_idle(int client, const http::HttpRequest&, bool head, void* opaque) {
  auto* service = static_cast<wallet::AddressService*>(opaque);
  const bool idle = service->WaitIdle(30000);
  respond(client, 200, "OK", idle ? "{\"idle\":true}" : "{\"idle\":false}", head);
}

// The sync fixture's seeds (tests/sync_route_test.py), at the address clock.
const fortis::codec::tj::Value* sync_test_body(fortis::codec::tj::Arena& arena, const http::HttpRequest& request) {
  if (!request.body || !request.body_length) return nullptr;
  const fortis::codec::tj::Value* body =
      fortis::codec::tj::Parse(arena, reinterpret_cast<const uint8_t*>(request.body), request.body_length);
  return fortis::codec::tj::IsObject(body) ? body : nullptr;
}

const char* sync_test_text(const fortis::codec::tj::Value* object, const char* key) {
  const fortis::codec::tj::Value* value = fortis::codec::tj::Get(object, key);
  return fortis::codec::tj::IsString(value) ? value->text : nullptr;
}

// POST /probe/test-sync-append {network, address, events, rollback}: one
// journal transaction through the store's append path (the account is
// created when missing); rollback builds the frame and drops it.
void handle_sync_test_append(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  fortis::codec::tj::Arena arena = fortis::codec::tj::GrowingArena(16 * 1024 * 1024);
  const fortis::codec::tj::Value* body = sync_test_body(arena, request);
  const char* network = sync_test_text(body, "network");
  const char* address = sync_test_text(body, "address");
  const fortis::codec::tj::Value* events = fortis::codec::tj::Get(body, "events");
  const fortis::codec::tj::Value* rollback = fortis::codec::tj::Get(body, "rollback");
  const int64_t now = service->NowMs();
  bool ok = store && network && address && fortis::codec::tj::IsArray(events);
  fortis::wallet::address::Account account;
  if (ok) {
    const fortis::wallet::address::ReadStatus status = store->Find(network, address, now, &account);
    ok = status != fortis::wallet::address::ReadStatus::kError;
    if (ok && status == fortis::wallet::address::ReadStatus::kMissing) ok = store->Touch(network, address, now, &account);
  }
  char sequence[32] = {};
  ok = ok && store->AppendSyncEvents(account.id, events, now,
                                     !(rollback && rollback->type == fortis::codec::tj::Type::kTrue), arena, sequence);
  fortis::codec::tj::FreeArena(arena);
  char reply[96];
  snprintf(reply, sizeof(reply), "{\"head\":\"%s\"}", sequence);
  respond(client, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", ok ? reply : "{\"error\":\"SEED\"}", head);
}

// POST /probe/test-sync-watch {topic, account: {network, address} | null,
// expiresAt} for the bearer's device: WatchStore.put with that expiry.
void handle_sync_test_watch(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, false);
  if (policy.status) {
    respond(client, policy.status, policy.reason, policy.error_body ? policy.error_body : "{}", head);
    return;
  }
  fortis::codec::tj::Arena arena = fortis::codec::tj::GrowingArena(4 * 1024 * 1024);
  const fortis::codec::tj::Value* body = sync_test_body(arena, request);
  const char* topic = sync_test_text(body, "topic");
  const fortis::codec::tj::Value* account_ref = fortis::codec::tj::Get(body, "account");
  const fortis::codec::tj::Value* expires = fortis::codec::tj::Get(body, "expiresAt");
  const int64_t now = service->NowMs();
  bool ok = store && topic && fortis::codec::tj::IsNumber(expires);
  fortis::wallet::address::Account account;
  const char* account_id = nullptr;
  if (ok && fortis::codec::tj::IsObject(account_ref)) {
    const char* network = sync_test_text(account_ref, "network");
    const char* address = sync_test_text(account_ref, "address");
    ok = network && address &&
         store->Find(network, address, now, &account) == fortis::wallet::address::ReadStatus::kFound;
    account_id = account.id;
  }
  ok = ok && store->WriteWatch(policy.device.id, topic, account_id, static_cast<int64_t>(expires->number), now, arena);
  fortis::codec::tj::FreeArena(arena);
  respond(client, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", ok ? "{\"ok\":true}" : "{\"error\":\"SEED\"}",
          head);
}

// POST /probe/test-address-state {device, accounts: [[network, address]]}:
// the durable state the R13 contract records (no auth, no quota): the
// device's live watches and, per account, its read times and the balance
// snapshot.
void handle_address_test_state(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  tj::Arena arena = tj::GrowingArena(32 * 1024 * 1024);
  const tj::Value* body = sync_test_body(arena, request);
  const char* device = sync_test_text(body, "device");
  const tj::Value* accounts = tj::Get(body, "accounts");
  const int64_t now = service->NowMs();
  tj::Value* out = tj::Obj(arena);
  tj::Value* watches = tj::Arr(arena);
  tj::Value* rows = tj::Arr(arena);
  tj::Value* listed = nullptr;
  bool ok = store && device && out && watches && rows && store->ListDeviceWatches(device, now, arena, &listed);
  for (uint32_t i = 0; ok && tj::IsArray(listed) && i < listed->count; ++i) {
    const tj::Value* w = listed->items[i];
    const tj::Value* account = tj::Get(w, "accountId");
    const tj::Value* tokens = tj::Get(w, "trackedTokens");
    const tj::Value* expires = tj::Get(w, "expiresAt");
    tj::Value* row = tj::Obj(arena);
    ok = row && tj::Set(arena, row, "topic", tj::Clone(arena, tj::Get(w, "topic"))) &&
         tj::Set(arena, row, "account", tj::Bool(arena, tj::IsString(account))) &&
         tj::Set(arena, row, "trackedTokens", tokens ? tj::Clone(arena, tokens) : tj::Arr(arena)) &&
         tj::Set(arena, row, "expiresAt", expires ? tj::Clone(arena, expires) : tj::Null(arena)) &&
         tj::Push(arena, watches, row);
  }
  for (uint32_t i = 0; ok && tj::IsArray(accounts) && i < accounts->count; ++i) {
    const tj::Value* pair = accounts->items[i];
    const tj::Value* network = tj::IsArray(pair) && pair->count == 2 ? pair->items[0] : nullptr;
    const tj::Value* address = tj::IsArray(pair) && pair->count == 2 ? pair->items[1] : nullptr;
    ok = tj::IsString(network) && tj::IsString(address);
    if (!ok) break;
    fortis::wallet::address::Account account;
    const auto status = store->Find(network->text, address->text, now, &account);
    tj::Value* row = tj::Obj(arena);
    tj::Value* account_value = tj::Null(arena);
    tj::Value* balance = tj::Null(arena);
    if (status == fortis::wallet::address::ReadStatus::kFound) {
      account_value = tj::Obj(arena);
      ok = account_value &&
           tj::Set(arena, account_value, "firstReadAt", tj::Num(arena, static_cast<double>(account.first_read_at))) &&
           tj::Set(arena, account_value, "lastReadAt", tj::Num(arena, static_cast<double>(account.last_read_at)));
      tj::Value* payload = nullptr;
      int64_t as_of = 0;
      if (ok && store->GetBalance(account.id, now, arena, &payload, &as_of) == fortis::wallet::address::ReadStatus::kFound) {
        balance = tj::Obj(arena);
        ok = balance && tj::Set(arena, balance, "payload", payload) &&
             tj::Set(arena, balance, "asOf", tj::Num(arena, static_cast<double>(as_of)));
      }
    } else if (status == fortis::wallet::address::ReadStatus::kError) {
      ok = false;
    }
    ok = ok && row && tj::Set(arena, row, "network", tj::Clone(arena, network)) &&
         tj::Set(arena, row, "address", tj::Clone(arena, address)) && tj::Set(arena, row, "account", account_value) &&
         tj::Set(arena, row, "balance", balance) && tj::Push(arena, rows, row);
  }
  ok = ok && tj::Set(arena, out, "watches", watches) && tj::Set(arena, out, "rows", rows);
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(16 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 16 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

struct NftStateEntry {
  fortis::codec::tj::Arena* arena;
  fortis::codec::tj::Value* items;
  size_t prefix_length;
  const char* account_id;
  bool ok;
};

fortis::codec::tj::Value* nft_state_record(fortis::codec::tj::Arena& arena, const char* text, size_t length, const char* account) {
  namespace tj = fortis::codec::tj;
  tj::Value* value = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  const tj::Value* owner = tj::Get(value, "accountId");
  if (tj::IsObject(value) && tj::IsString(owner) && account && strcmp(owner->text, account) == 0)
    tj::Set(arena, value, "accountId", tj::Str(arena, "<account>"));
  return value;
}

bool nft_state_visit(const char* key, const void* value, size_t size, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* c = static_cast<NftStateEntry*>(opaque);
  tj::Value* row = tj::Obj(*c->arena);
  tj::Value* record = nft_state_record(*c->arena, static_cast<const char*>(value), size, c->account_id);
  c->ok = c->ok && row && record && tj::Set(*c->arena, row, "key", tj::Str(*c->arena, key + c->prefix_length)) &&
          tj::Set(*c->arena, row, "value", record) && tj::Push(*c->arena, c->items, row);
  return c->ok;
}

// POST /probe/test-nft-state {accounts: [[network, address]], mediaKeys,
// now?}: per account its NFT scan record and every live item record (key
// suffix after nft/item/{account}/, accountId shown as "<account>"), and
// each media record, all read at `now` (default: the address clock). No
// auth, no quota.
void handle_nft_test_state(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* body = sync_test_body(arena, request);
  const tj::Value* accounts = tj::Get(body, "accounts");
  const tj::Value* media_keys = tj::Get(body, "mediaKeys");
  const tj::Value* at = tj::Get(body, "now");
  const int64_t now = tj::IsNumber(at) ? static_cast<int64_t>(at->number) : service->NowMs();
  tj::Value* out = tj::Obj(arena);
  tj::Value* nft = tj::Arr(arena);
  tj::Value* media = tj::Arr(arena);
  bool ok = store && out && nft && media;
  for (uint32_t i = 0; ok && tj::IsArray(accounts) && i < accounts->count; ++i) {
    const tj::Value* pair = accounts->items[i];
    const tj::Value* network = tj::IsArray(pair) && pair->count == 2 ? pair->items[0] : nullptr;
    const tj::Value* address = tj::IsArray(pair) && pair->count == 2 ? pair->items[1] : nullptr;
    ok = tj::IsString(network) && tj::IsString(address);
    if (!ok) break;
    fortis::wallet::address::Account account;
    const auto status = store->Find(network->text, address->text, now, &account);
    tj::Value* row = tj::Obj(arena);
    tj::Value* items = tj::Arr(arena);
    tj::Value* scan = tj::Null(arena);
    ok = row && items && scan && status != fortis::wallet::address::ReadStatus::kError;
    if (ok && status == fortis::wallet::address::ReadStatus::kFound) {
      char scan_key[128], prefix[128];
      ok = fortis::wallet::address::NftScanKey(account.id, scan_key, sizeof(scan_key)) &&
           fortis::wallet::address::NftItemPrefix(account.id, prefix, sizeof(prefix));
      tj::Value* record = nullptr;
      if (ok && store->GetJson(scan_key, now, arena, &record) == fortis::wallet::address::ReadStatus::kFound) scan = record;
      NftStateEntry entry{&arena, items, strlen(prefix), account.id, true};
      ok = ok && store->kv()->Scan(prefix, nullptr, 1000000, now, [&, visitor_state = &entry](const char* key, const void* value, size_t size) { return (nft_state_visit)(key, value, size, visitor_state); }) && entry.ok;
    }
    ok = ok && tj::Set(arena, row, "network", tj::Clone(arena, network)) &&
         tj::Set(arena, row, "address", tj::Clone(arena, address)) && tj::Set(arena, row, "scan", scan) &&
         tj::Set(arena, row, "items", items) && tj::Push(arena, nft, row);
  }
  for (uint32_t i = 0; ok && tj::IsArray(media_keys) && i < media_keys->count; ++i) {
    const tj::Value* key = media_keys->items[i];
    ok = tj::IsString(key) && key->length < 64;
    if (!ok) break;
    char record_key[96];
    snprintf(record_key, sizeof(record_key), "media/item/%s", key->text);
    tj::Value* row = tj::Obj(arena);
    tj::Value* record = nullptr;
    ok = row && tj::Set(arena, row, "key", tj::Clone(arena, key));
    if (ok && store->GetJson(record_key, now, arena, &record) == fortis::wallet::address::ReadStatus::kFound)
      ok = tj::Set(arena, row, "value", record);
    else if (ok)
      ok = tj::Set(arena, row, "missing", tj::Bool(arena, true));
    ok = ok && tj::Push(arena, media, row);
  }
  ok = ok && tj::Set(arena, out, "nft", nft) && tj::Set(arena, out, "media", media);
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(32 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 32 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

struct TransferStateEntries {
  fortis::codec::tj::Arena* arena;
  fortis::codec::tj::Value* entries;
  const char* account_id;
  const char* encoded;  // the account id as a key component
  bool filter;          // global prefixes: only keys holding "/{encoded}/"
  bool ok;
};

// Every string equal to the account id becomes "<account>".
fortis::codec::tj::Value* transfer_state_value(fortis::codec::tj::Arena& arena, fortis::codec::tj::Value* value, const char* account) {
  namespace tj = fortis::codec::tj;
  if (!value) return value;
  if (tj::IsString(value)) return strcmp(value->text, account) == 0 ? tj::Str(arena, "<account>") : value;
  if (tj::IsArray(value)) {
    for (uint32_t i = 0; i < value->count; ++i) value->items[i] = transfer_state_value(arena, value->items[i], account);
  } else if (tj::IsObject(value)) {
    for (uint32_t i = 0; i < value->count; ++i)
      value->members[i].value = transfer_state_value(arena, value->members[i].value, account);
  }
  return value;
}

bool transfer_state_visit(const char* key, const void* value, size_t size, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* c = static_cast<TransferStateEntries*>(opaque);
  char needle[160];
  snprintf(needle, sizeof(needle), "/%s/", c->encoded);
  if (c->filter && !strstr(key, needle)) return true;
  // The key with the account component as "<account>".
  char shown[2304];
  size_t used = 0;
  const size_t encoded_length = strlen(c->encoded);
  for (const char* p = key; *p && used + 10 < sizeof(shown);) {
    if (strncmp(p, c->encoded, encoded_length) == 0) {
      memcpy(shown + used, "<account>", 9);
      used += 9;
      p += encoded_length;
    } else {
      shown[used++] = *p++;
    }
  }
  shown[used] = '\0';
  tj::Value* parsed = tj::Parse(*c->arena, static_cast<const uint8_t*>(value), size);
  if (!parsed) parsed = tj::Str(*c->arena, static_cast<const char*>(value), size);
  tj::Value* row = tj::Obj(*c->arena);
  c->ok = c->ok && row && tj::Set(*c->arena, row, "key", tj::Str(*c->arena, shown)) &&
          tj::Set(*c->arena, row, "value", transfer_state_value(*c->arena, parsed, c->account_id)) &&
          tj::Push(*c->arena, c->entries, row);
  return c->ok;
}

int compare_transfer_state_entries(const void* a, const void* b) {
  namespace tj = fortis::codec::tj;
  const tj::Value* left = tj::Get(*static_cast<tj::Value* const*>(a), "key");
  const tj::Value* right = tj::Get(*static_cast<tj::Value* const*>(b), "key");
  return strcmp(left->text, right->text);
}

// POST /probe/test-transfer-state {accounts: [[network, address]], entries?,
// now?}: per account its read times, the transfer sync-state record and
// every transfer-row, index and sync-journal entry keyed by the account (the
// account id shown as "<account>"), sorted by key; with entries false only
// their count per "{prefix}/{kind}". Also the journal head. Read at `now`
// (default: the address clock). No auth, no quota.
void handle_transfer_test_state(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* body = sync_test_body(arena, request);
  const tj::Value* accounts = tj::Get(body, "accounts");
  const tj::Value* at = tj::Get(body, "now");
  const tj::Value* entries_wanted = tj::Get(body, "entries");
  const bool with_entries = !entries_wanted || entries_wanted->type != tj::Type::kFalse;
  const int64_t now = tj::IsNumber(at) ? static_cast<int64_t>(at->number) : service->NowMs();
  tj::Value* out = tj::Obj(arena);
  tj::Value* transfer = tj::Arr(arena);
  bool ok = store && out && transfer;
  static const char* const kAccountPrefixes[] = {"transfer/row/", "transfer/cursor/", "transfer/time/",
                                                 "transfer/token-time/", "transfer/token-unpriced-time/",
                                                 "sync2/account/"};
  static const char* const kGlobalPrefixes[] = {"transfer/age/", "transfer/retention/", "transfer/unpriced/"};
  for (uint32_t i = 0; ok && tj::IsArray(accounts) && i < accounts->count; ++i) {
    const tj::Value* pair = accounts->items[i];
    const tj::Value* network = tj::IsArray(pair) && pair->count == 2 ? pair->items[0] : nullptr;
    const tj::Value* address = tj::IsArray(pair) && pair->count == 2 ? pair->items[1] : nullptr;
    ok = tj::IsString(network) && tj::IsString(address);
    if (!ok) break;
    fortis::wallet::address::Account account;
    const auto status = store->Find(network->text, address->text, now, &account);
    tj::Value* row = tj::Obj(arena);
    ok = row && status != fortis::wallet::address::ReadStatus::kError && tj::Set(arena, row, "network", tj::Clone(arena, network)) &&
         tj::Set(arena, row, "address", tj::Clone(arena, address));
    if (ok && status == fortis::wallet::address::ReadStatus::kMissing) {
      ok = tj::Set(arena, row, "account", tj::Null(arena)) && tj::Push(arena, transfer, row);
      continue;
    }
    char encoded[160];
    ok = ok && fortis::wallet::address::Store::Component(account.id, encoded, sizeof(encoded));
    tj::Value* times = tj::Obj(arena);
    ok = ok && times &&
         tj::Set(arena, times, "firstReadAt", tj::Num(arena, static_cast<double>(account.first_read_at))) &&
         tj::Set(arena, times, "lastReadAt", tj::Num(arena, static_cast<double>(account.last_read_at))) &&
         tj::Set(arena, row, "account", times);
    tj::Value* sync = nullptr;
    const auto sync_status = ok ? store->GetSyncState(account.id, now, arena, &sync) : fortis::wallet::address::ReadStatus::kError;
    ok = ok && sync_status != fortis::wallet::address::ReadStatus::kError &&
         tj::Set(arena, row, "sync", sync_status == fortis::wallet::address::ReadStatus::kFound
                                         ? transfer_state_value(arena, sync, account.id) : tj::Null(arena));
    tj::Value* entries = tj::Arr(arena);
    TransferStateEntries visit{&arena, entries, account.id, encoded, false, entries != nullptr};
    for (const char* prefix : kAccountPrefixes) {
      char scoped[256];
      snprintf(scoped, sizeof(scoped), "%s%s/", prefix, encoded);
      visit.filter = false;
      ok = ok && store->kv()->Scan(scoped, nullptr, 1000000, now, [&, visitor_state = &visit](const char* key, const void* value, size_t size) { return (transfer_state_visit)(key, value, size, visitor_state); }) && visit.ok;
    }
    for (const char* prefix : kGlobalPrefixes) {
      visit.filter = true;
      ok = ok && store->kv()->Scan(prefix, nullptr, 1000000, now, [&, visitor_state = &visit](const char* key, const void* value, size_t size) { return (transfer_state_visit)(key, value, size, visitor_state); }) && visit.ok;
    }
    if (ok && entries->count > 1)
      qsort(entries->items, entries->count, sizeof(tj::Value*), compare_transfer_state_entries);
    if (ok && with_entries) {
      ok = tj::Set(arena, row, "entries", entries);
    } else if (ok) {
      tj::Value* counts = tj::Obj(arena);
      ok = counts != nullptr;
      for (uint32_t e = 0; ok && e < entries->count; ++e) {
        const char* key = tj::Get(entries->items[e], "key")->text;
        const char* first = strchr(key, '/');
        const char* second = first ? strchr(first + 1, '/') : nullptr;
        const size_t length = second ? static_cast<size_t>(second - key) : strlen(key);
        char kind[96];
        snprintf(kind, sizeof(kind), "%.*s", static_cast<int>(length < 95 ? length : 95), key);
        const tj::Value* existing = tj::Get(counts, kind);
        ok = tj::Set(arena, counts, kind, tj::Num(arena, (tj::IsNumber(existing) ? existing->number : 0) + 1));
      }
      ok = ok && tj::Set(arena, row, "counts", counts);
    }
    ok = ok && tj::Push(arena, transfer, row);
  }
  tj::Value* journal_head = nullptr;
  const auto head_status = ok ? store->GetJson("sync2/meta/head", now, arena, &journal_head)
                              : fortis::wallet::address::ReadStatus::kError;
  ok = ok && head_status != fortis::wallet::address::ReadStatus::kError && tj::Set(arena, out, "transfer", transfer) &&
       tj::Set(arena, out, "head", head_status == fortis::wallet::address::ReadStatus::kFound ? journal_head : tj::Null(arena));
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(48 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 48 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

// POST /probe/test-news-persist {"stories": [<news_store.h story JSON>, …]}:
// KvNewsStore.persist of those stories at the address test clock, so the
// overview's news reader (R18) sees the stories the TypeScript oracle
// persisted at the same instant.
class AddressServiceClock final : public fortis::base::Clock {
 public:
  explicit AddressServiceClock(const wallet::AddressService* service) : service_(service) {}
  int64_t NowMs() override { return service_->NowMs(); }

 private:
  const wallet::AddressService* service_;
};

void handle_news_test_persist(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* body = sync_test_body(arena, request);
  const tj::Value* stories = tj::Get(body, "stories");
  const size_t news_bytes = 32 * 1024 * 1024;
  const size_t text_bytes = 1024 * 1024;
  uint8_t* memory = static_cast<uint8_t*>(malloc(news_bytes));
  uint8_t* text = static_cast<uint8_t*>(malloc(text_bytes));
  bool ok = store && memory && text && tj::IsArray(stories);
  fortis::news::Arena news_arena{memory, news_bytes};
  fortis::news::Cluster** clusters = nullptr;
  if (ok) {
    clusters = static_cast<fortis::news::Cluster**>(news_arena.Alloc(sizeof(fortis::news::Cluster*) * (stories->count + 1)));
    ok = clusters != nullptr;
  }
  for (uint32_t i = 0; ok && i < stories->count; ++i) {
    tj::OutBuffer out{text, text_bytes, 0, true};
    auto* cluster = static_cast<fortis::news::Cluster*>(news_arena.Alloc(sizeof(fortis::news::Cluster)));
    ok = cluster && tj::Stringify(stories->items[i], out) && out.ok;
    if (ok) {
      new (cluster) fortis::news::Cluster();
      ok = fortis::news::DecodeCluster(text, out.used, &news_arena, cluster);
      clusters[i] = cluster;
    }
  }
  if (ok) {
    const fortis::news::NewsStore news{store->kv(), nullptr};
    AddressServiceClock clock(service);
    ok = fortis::news::Persist(news, clusters, stories->count, clock, &news_arena) ==
         fortis::news::StoreStatus::kOk;
  }
  free(memory);
  free(text);
  tj::FreeArena(arena);
  if (ok) respond(client, 200, "OK", "{\"persisted\":true}", head);
  else respond(client, 500, "Internal Server Error", "{\"error\":\"NEWS\"}", head);
}

struct TxStatePage {
  fortis::codec::tj::Arena* arena;
  fortis::codec::tj::Value* rows;
  bool ok;
};

// The physical expiry of a live key: the first instant the store no longer
// answers it (both stores expire a record at expiresAt <= now), searched
// over the next 400 days; -1 when it outlives them (no expiry).
int64_t tx_state_expiry(const fortis::storage::KvStore* kv, const char* key, int64_t now) {
  uint8_t probe[1];
  size_t size = 0;
  auto live = [&](int64_t at) {
    const fortis::storage::KvReadStatus status = kv->GetStatus(key, at, probe, sizeof(probe), &size);
    return status == fortis::storage::KvReadStatus::Found || status == fortis::storage::KvReadStatus::BufferTooSmall;
  };
  int64_t low = now, high = now + 400LL * 86400000;
  if (!live(low) || live(high)) return -1;
  while (high - low > 1) {
    const int64_t middle = low + (high - low) / 2;
    if (live(middle)) low = middle; else high = middle;
  }
  return high;
}

// The page visitor runs under the store's lock: rows only (expiries after).
bool tx_state_pending(const char* key, const void* value, size_t size, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* c = static_cast<TxStatePage*>(opaque);
  tj::Value* row = tj::Obj(*c->arena);
  tj::Value* parsed = tj::Parse(*c->arena, static_cast<const uint8_t*>(value), size);
  c->ok = c->ok && row && parsed && tj::Set(*c->arena, row, "key", tj::Str(*c->arena, key)) &&
          tj::Set(*c->arena, row, "value", parsed) && tj::Push(*c->arena, c->rows, row);
  return c->ok;
}

// POST /probe/test-tx-state {devices: [deviceId], txs: [[network id, hash]]}:
// the durable state the R26 contract records (no auth, no quota), at the
// address clock: each device's live watches; per transaction its
// tx/detail record as stored (null when absent or expired) with its
// physical expiry; every live tx/pending entry in key order with its value
// and expiry.
void handle_tx_test_state(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  tj::Arena arena = tj::GrowingArena(32 * 1024 * 1024);
  const tj::Value* body = sync_test_body(arena, request);
  const tj::Value* devices = tj::Get(body, "devices");
  const tj::Value* txs = tj::Get(body, "txs");
  const int64_t now = service->NowMs();
  tj::Value* out = tj::Obj(arena);
  tj::Value* watches = tj::Obj(arena);
  tj::Value* records = tj::Arr(arena);
  tj::Value* pending = tj::Arr(arena);
  bool ok = store && out && watches && records && pending && tj::IsArray(devices) && tj::IsArray(txs);
  for (uint32_t d = 0; ok && d < devices->count; ++d) {
    const tj::Value* device = devices->items[d];
    tj::Value* listed = nullptr;
    tj::Value* rows = tj::Arr(arena);
    ok = tj::IsString(device) && rows && store->ListDeviceWatches(device->text, now, arena, &listed);
    for (uint32_t i = 0; ok && tj::IsArray(listed) && i < listed->count; ++i) {
      const tj::Value* w = listed->items[i];
      const tj::Value* tokens = tj::Get(w, "trackedTokens");
      const tj::Value* expires = tj::Get(w, "expiresAt");
      tj::Value* row = tj::Obj(arena);
      ok = row && tj::Set(arena, row, "topic", tj::Clone(arena, tj::Get(w, "topic"))) &&
           tj::Set(arena, row, "account", tj::Bool(arena, tj::IsString(tj::Get(w, "accountId")))) &&
           tj::Set(arena, row, "trackedTokens", tokens ? tj::Clone(arena, tokens) : tj::Arr(arena)) &&
           tj::Set(arena, row, "expiresAt", expires ? tj::Clone(arena, expires) : tj::Null(arena)) &&
           tj::Push(arena, rows, row);
    }
    ok = ok && tj::Set(arena, watches, device->text, rows);
  }
  for (uint32_t i = 0; ok && i < txs->count; ++i) {
    const tj::Value* pair = txs->items[i];
    const tj::Value* network = tj::IsArray(pair) && pair->count == 2 ? pair->items[0] : nullptr;
    const tj::Value* hash = tj::IsArray(pair) && pair->count == 2 ? pair->items[1] : nullptr;
    // txDetailRecordKey with each value's full length, so a value holding a
    // NUL is looked up under its own %00 key (TxRecordKey stops at the NUL).
    char key[fortis::wallet::address::kTxKeyBytes];
    fortis::chains::OutBuffer encoded{reinterpret_cast<uint8_t*>(key), sizeof(key) - 1, 0, true};
    ok = tj::IsString(network) && tj::IsString(hash);
    if (ok) {
      encoded.Text("tx/detail/");
      ok = fortis::chains::EncodeUriComponent(network->text, network->length, encoded);
      encoded.Byte('/');
      ok = ok && fortis::chains::EncodeUriComponent(hash->text, hash->length, encoded) && encoded.ok;
      key[ok ? encoded.used : 0] = '\0';
    }
    if (!ok) break;
    tj::Value* record = nullptr;
    const fortis::wallet::address::ReadStatus status = store->GetJson(key, now, arena, &record);
    ok = status != fortis::wallet::address::ReadStatus::kError;
    const bool found = status == fortis::wallet::address::ReadStatus::kFound;
    const int64_t expires = found ? tx_state_expiry(store->kv(), key, now) : -1;
    tj::Value* row = tj::Obj(arena);
    ok = ok && row && tj::Set(arena, row, "network", tj::Clone(arena, network)) &&
         tj::Set(arena, row, "hash", tj::Clone(arena, hash)) &&
         tj::Set(arena, row, "record", found ? record : tj::Null(arena)) &&
         tj::Set(arena, row, "expiresAt", expires < 0 ? tj::Null(arena) : tj::Num(arena, static_cast<double>(expires))) &&
         tj::Push(arena, records, row);
  }
  TxStatePage page{&arena, pending, true};
  fortis::storage::KvPageResult result;
  ok = ok && store->kv()->Page("tx/pending/", nullptr, 1000, now, false, [&, visitor_state = &page](const char* key, const void* value, size_t size) { return (tx_state_pending)(key, value, size, visitor_state); }, &result) && page.ok;
  for (uint32_t i = 0; ok && i < pending->count; ++i) {
    tj::Value* row = pending->items[i];
    const int64_t expires = tx_state_expiry(store->kv(), tj::Get(row, "key")->text, now);
    ok = tj::Set(arena, row, "expiresAt", expires < 0 ? tj::Null(arena) : tj::Num(arena, static_cast<double>(expires)));
  }
  ok = ok && tj::Set(arena, out, "watches", watches) && tj::Set(arena, out, "records", records) &&
       tj::Set(arena, out, "pending", pending);
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(16 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 16 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

// POST /probe/test-address-swr {keys: [key]}: each address-facet SWR
// envelope as stored ({asOf, freshUntil, staleUntil, d: the stored JSON
// text}, or null when absent at the address clock). No auth, no quota.
void handle_address_test_swr(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  auto* service = static_cast<wallet::AddressService*>(opaque);
  fortis::wallet::address::Store* store = service->store();
  tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* body = sync_test_body(arena, request);
  const tj::Value* keys = tj::Get(body, "keys");
  const int64_t now = service->NowMs();
  const size_t scratch_bytes = 2 * 1024 * 1024;
  uint8_t* scratch = static_cast<uint8_t*>(malloc(scratch_bytes));
  tj::Value* out = tj::Obj(arena);
  tj::Value* envelopes = tj::Arr(arena);
  bool ok = store && scratch && out && envelopes && tj::IsArray(keys);
  for (uint32_t i = 0; ok && i < keys->count; ++i) {
    const tj::Value* key = keys->items[i];
    ok = tj::IsString(key);
    if (!ok) break;
    fortis::tokens::token_swr::EnvelopeInfo info;
    tj::Value* row = tj::Obj(arena);
    tj::Value* envelope = tj::Null(arena);
    if (fortis::tokens::token_swr::PeekEnvelope(store->kv(), key->text, now, scratch, scratch_bytes, &info)) {
      envelope = tj::Obj(arena);
      ok = envelope && tj::Set(arena, envelope, "asOf", tj::Num(arena, static_cast<double>(info.as_of_ms))) &&
           tj::Set(arena, envelope, "freshUntil", tj::Num(arena, static_cast<double>(info.fresh_until_ms))) &&
           tj::Set(arena, envelope, "staleUntil", tj::Num(arena, static_cast<double>(info.stale_until_ms))) &&
           tj::Set(arena, envelope, "d",
                   tj::Str(arena, reinterpret_cast<const char*>(info.body), info.body_length));
    }
    ok = ok && row && tj::Set(arena, row, "key", tj::Clone(arena, key)) && tj::Set(arena, row, "envelope", envelope) &&
         tj::Push(arena, envelopes, row);
  }
  free(scratch);
  ok = ok && tj::Set(arena, out, "envelopes", envelopes);
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(8 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 8 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

bool address_test_route = false;
AddressTestUpstream address_test_upstream{};
wallet::AddressServiceSeams address_test_seams{};

}  // namespace

bool ConfigureAddressTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* address_flag = TestHookEnv("FORTIS_ENABLE_TEST_ADDRESS_ROUTES");
  address_test_route = address_flag && strcmp(address_flag, "1") == 0;
  const char* address_upstream_port = TestHookEnv("FORTIS_TEST_ADDRESS_UPSTREAM_PORT");
  const char* address_upstream_ca = TestHookEnv("FORTIS_TEST_ADDRESS_UPSTREAM_CA");
  if (address_test_route && config.native_address_canary) {
    fprintf(stderr, "FORTIS_ENABLE_TEST_ADDRESS_ROUTES and "
                    "FORTIS_NATIVE_ADDRESS_CANARY are mutually exclusive\n");
    return false;
  }
  if (address_test_route && (parse_port(address_upstream_port) < 0 || !address_upstream_ca ||
      !*address_upstream_ca || !http::OutboundHttpsValidateCaBundle(address_upstream_ca))) {
    fprintf(stderr, "Invalid test-only address upstream configuration\n");
    return false;
  }
  if (address_test_route) {
    snprintf(address_test_upstream.port, sizeof(address_test_upstream.port), "%d",
             parse_port(address_upstream_port));
    address_test_upstream.ca = address_upstream_ca;
    address_test_seams.clock = &address_test_clock;
    address_test_seams.jitter = &address_test_jitter_source;
    const char* manual = TestHookEnv("FORTIS_TEST_ADDRESS_MANUAL_JOBS");
    address_test_seams.manual_jobs = !(manual && strcmp(manual, "0") == 0);
    options.address_seams = &address_test_seams;
    options.address_transport = &address_test_upstream;
    options.address = true;
    // The operation candle repair reads history through the price history
    // service: its owners use the same loopback upstream (no seeded candles).
    UseLoopbackHistoryOwners(parse_port(address_upstream_port), address_upstream_ca, false, options);
  }
  return true;
}

bool SeedAddressTest(fortis::storage::KvStore* kv) {
  if (address_test_route && !seed_address_test_device(kv)) {
    fprintf(stderr, "Failed to seed test address fixtures\n");
    return false;
  }
  return true;
}

bool RegisterAddressTestRoutes(http::HttpRouter& router, Planes& planes) {
  wallet::AddressService* service = planes.address ? &*planes.address : nullptr;
  return !address_test_route ||
         (router.Add("GET", "/probe/test-address-job", handle_address_test_job, service) &&
          router.Add("GET", "/probe/test-address-idle", handle_address_test_idle, service) &&
          router.Add("POST", "/probe/test-sync-append", handle_sync_test_append, service) &&
          router.Add("POST", "/probe/test-sync-watch", handle_sync_test_watch, service) &&
          router.Add("POST", "/probe/test-address-state", handle_address_test_state, service) &&
          router.Add("POST", "/probe/test-nft-state", handle_nft_test_state, service) &&
          router.Add("POST", "/probe/test-transfer-state", handle_transfer_test_state, service) &&
          router.Add("POST", "/probe/test-address-swr", handle_address_test_swr, service) &&
          router.Add("POST", "/probe/test-tx-state", handle_tx_test_state, service) &&
          router.Add("POST", "/probe/test-news-persist", handle_news_test_persist, service));
}

void AddressTestSweepClock(fortis::base::Clock** clock) {
  if (address_test_route) *clock = &address_test_sweep_clock;
}

}  // namespace fortis::app
#endif
