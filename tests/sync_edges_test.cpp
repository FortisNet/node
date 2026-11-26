#include <stdexcept>
#include <new>
#include <memory>
#include "test_support.h"
// R52 POST /v1/sync units against tests/corpus/sync_edges.json
// (written by the earlier TypeScript implementation's tests; its
// SHA-256 pin is checked first):
//
//  1. The TypeScript oracle in process: every fixture case runs through the
//     real fortis::wallet::sync::Route registered on an HttpRouter, dispatched over a
//     socketpair with the request bytes the socket replay sends, at the
//     fixture's clock (Route's Clock). Seeds go through address::Store
//     (AppendSyncEvents, WriteWatch). Status, the fixture header set (not
//     x-rate-limit-remaining: the device quota window is the wall clock) and
//     the exact body bytes must match; the two documented native differences
//     are asserted. Each group boundary is a hard restart: the journal KV
//     file is copied byte for byte while the old store is still open (no
//     close path runs) and the next group opens the copy with a new Store and
//     Route (an empty scope cache). The quota group drains device B until its
//     first 429 in the current wall minute.
//  2. Journal concurrency: four writers append to watched accounts and one
//     to an unwatched account while six readers page the watched scope with
//     Journal::Read; every page is ordered, duplicate-free, inside the scope
//     and an extension of the previous complete read; the final read holds
//     every watched event once.
//  3. Crash image under concurrent writes: the KV file copied mid-write
//     reopens to a journal that is exactly 1..head over every account, with
//     every two-event frame whole, and the next append takes head + 1.
//  2b. Two followers poll on the cursors they are given during the writes:
//     each receives every watched event once, in order, never a reset.
//  4. Scope isolation in the direct-mapped cache: two devices whose ids
//     share a cache slot, read concurrently with invalidations, always get
//     their own accounts; an invalidation shows a new watch at once.
//
// Usage: sync_edges_test <sync_edges.json>
#include "strict_unit_expectations.h"
#include "compat_expectations.h"
#include "../src/wallet/address_store.h"
#include "../src/auth/device_auth.h"
#include "../src/http/http_server.h"
#include "../src/storage/kv_runtime.h"
#include "../src/storage/kv_store.h"
#include "../src/wallet/sync_journal.h"
#include "../src/wallet/sync_route.h"
#include "../src/wallet/sync_scope.h"
#include "../src/codec/token_json.h"
#include "fake_seams.h"

#include <mbedtls/sha256.h>

#include <atomic>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::auth;
using namespace fortis::http;

namespace tj = fortis::codec::tj;
using fortis::storage::KvStore;
namespace address = fortis::wallet::address;
namespace journal_sync = fortis::wallet::sync;

namespace {

int failures = 0;
int checks = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %.600s\n", what, detail);
}

#define CHECK(condition, what)                       \
  do {                                               \
    ++checks;                                        \
    if (!(condition)) fail(what, #condition);        \
  } while (0)

int64_t wall_ms() {
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

void sleep_ms(int64_t ms) {
  timespec pause{static_cast<time_t>(ms / 1000), static_cast<long>((ms % 1000) * 1000000)};
  nanosleep(&pause, nullptr);
}

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (data && fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    free(data);
    data = nullptr;
  }
  fclose(file);
  if (data) data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

bool write_file(const char* path, const char* data, size_t length) {
  FILE* file = fopen(path, "wb");
  if (!file) return false;
  const bool ok = fwrite(data, 1, length, file) == length;
  return fclose(file) == 0 && ok;
}

const char* text(const tj::Value* object, const char* key) {
  const tj::Value* value = tj::Get(object, key);
  return tj::IsString(value) ? value->text : nullptr;
}

double number(const tj::Value* object, const char* key) {
  const tj::Value* value = tj::Get(object, key);
  return tj::IsNumber(value) ? value->number : 0;
}

// A growable byte buffer.
struct Bytes {
  char* data = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  ~Bytes() { free(data); }
  void Append(const void* bytes, size_t length) {
    if (size + length + 1 > capacity) {
      size_t next = capacity ? capacity * 2 : 4096;
      while (next < size + length + 1) next *= 2;
      auto* replacement = static_cast<char*>(realloc(data, next));
      if (!replacement) throw std::bad_alloc();
      data = replacement;
      capacity = next;
    }
    if (!data) throw std::bad_alloc();
    if (length) {
      if (!bytes) throw std::invalid_argument("missing test buffer bytes");
      memcpy(data + size, bytes, length);
    }
    size += length;
    data[size] = '\0';
  }
  void Text(const char* value) { Append(value, strlen(value)); }
};

bool from_hex(const char* hex, Bytes* out) {
  const size_t length = strlen(hex);
  if (length % 2) return false;
  for (size_t i = 0; i < length; i += 2) {
    char pair[3] = {hex[i], hex[i + 1], 0};
    char* end = nullptr;
    const unsigned value = static_cast<unsigned>(strtoul(pair, &end, 16));
    if (*end) return false;
    const uint8_t byte = static_cast<uint8_t>(value);
    out->Append(&byte, 1);
  }
  return true;
}

// ---- the clock, the devices and the journal store -----------------------------------------------

int64_t g_now = 0;
fortis::testing::PointerClock g_clock(&g_now);

struct Device {
  char letter;
  char fixture_token[64];
  char token[64];
  char id[27];
};

Device g_devices[4] = {{'A', "", "", "01ARZ3NDEKTSV4RRFFQ69G5WA1"},
                       {'B', "", "", "01ARZ3NDEKTSV4RRFFQ69G5WB2"},
                       {'e', "", "", "01ARZ3NDEKTSV4RRFFQ69G5WE5"},
                       {'r', "", "", "01ARZ3NDEKTSV4RRFFQ69G5WR6"}};

Device* device(char letter) {
  for (Device& d : g_devices)
    if (d.letter == letter) return &d;
  return nullptr;
}

// Native tokens differ from the fixture's (fintel_dev_ + 43 letters, the
// same length): each request's headers are rewritten.
bool seed_devices(const tj::Value* tokens) {
  fortis::auth::DeviceAuthStore auth(fortis::storage::RuntimeKvStore());
  const int64_t now = wall_ms();
  for (Device& d : g_devices) {
    const char key[2] = {d.letter, 0};
    const char* fixture_token = text(tokens, key);
    if (!fixture_token || strlen(fixture_token) != 54) {
      fprintf(stderr, "device %c: no fixture token\n", d.letter);
      return false;
    }
    snprintf(d.fixture_token, sizeof(d.fixture_token), "%s", fixture_token);
    memcpy(d.token, "fintel_dev_", 11);
    for (size_t i = 0; i < 43; ++i) d.token[11 + i] = i % 2 ? d.letter : "0123456789"[i % 10];
    d.token[54] = '\0';
    // Premium, development attestation, 400 days; `e` expired a day ago
    // (still retained), `r` revoked now.
    const int64_t seeded = d.letter == 'e' ? now - 401LL * 86400000 : now;
    if (!auth.SeedAddressRouteFixture(d.token, d.id, seeded) || (d.letter == 'r' && !auth.Revoke(d.id, now))) {
      fprintf(stderr, "device %c: seeding failed\n", d.letter);
      return false;
    }
  }
  return true;
}

struct Journal {
  char directory[512];
  std::unique_ptr<KvStore> kv;
  std::unique_ptr<address::Store> store;
  std::unique_ptr<journal_sync::Route> route;
  std::unique_ptr<HttpRouter> router;
  int generation = 0;
  ~Journal() { if (route) route->Stop(); }
};

const char* g_root = nullptr;

bool open_journal(Journal* journal, bool attestation_required) {
  journal->kv = std::make_unique<KvStore>();
  if (!journal->kv->Open(journal->directory)) return false;
  journal->store = std::make_unique<address::Store>(journal->kv.get());
  journal->route = std::make_unique<journal_sync::Route>();
  journal->router = std::make_unique<HttpRouter>();
  return journal->route->Start(journal->kv.get(), journal->store.get(), attestation_required, &g_clock) &&
         journal->route->Register(*journal->router);
}

// A hard restart: the journal file as it is on disk now, opened by a new
// process image (new store, route, router and scope cache).
bool crash_restart(Journal* journal, bool attestation_required) {
  char path[600];
  snprintf(path, sizeof(path), "%s/fortis.kv", journal->directory);
  size_t length = 0;
  char* image = read_file(path, &length);
  if (!image) return false;
  snprintf(journal->directory, sizeof(journal->directory), "%s/journal-%d", g_root, ++journal->generation);
  mkdir(journal->directory, 0700);
  snprintf(path, sizeof(path), "%s/fortis.kv", journal->directory);
  const bool copied = write_file(path, image, length);
  free(image);
  journal->route->Stop();
  journal->router.reset();
  journal->route.reset();
  journal->store.reset();
  journal->kv.reset();
  return copied && open_journal(journal, attestation_required);
}

// ---- one HTTP exchange over a socketpair --------------------------------------------------------------

struct Exchange {
  int peer;
  const Bytes* request;
  Bytes response;
};

void* client_side(void* opaque) {
  auto* exchange = static_cast<Exchange*>(opaque);
  size_t sent = 0;
  while (sent < exchange->request->size) {
    const ssize_t n = send(exchange->peer, exchange->request->data + sent, exchange->request->size - sent, 0);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
  shutdown(exchange->peer, SHUT_WR);
  char buffer[65536];
  for (;;) {
    const ssize_t n = recv(exchange->peer, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    exchange->response.Append(buffer, static_cast<size_t>(n));
  }
  return nullptr;
}

bool dispatch(HttpRouter& router, const Bytes& request, Bytes* response) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
  Exchange exchange{pair[1], &request, {}};
  pthread_t thread;
  if (pthread_create(&thread, nullptr, client_side, &exchange) != 0) return false;
  router.Dispatch(pair[0]);
  close(pair[0]);
  pthread_join(thread, nullptr);
  close(pair[1]);
  response->size = 0;
  response->Append(exchange.response.data ? exchange.response.data : "", exchange.response.size);
  return true;
}

struct Parsed {
  int status = 0;
  char headers[32][2][256];
  size_t header_count = 0;
  const char* body = nullptr;
  size_t body_length = 0;

  const char* Header(const char* name) const {
    for (size_t i = 0; i < header_count; ++i)
      if (!strcmp(headers[i][0], name)) return headers[i][1];
    return nullptr;
  }
};

bool parse_response(const Bytes& raw, Parsed* out) {
  const char* split = raw.data ? strstr(raw.data, "\r\n\r\n") : nullptr;
  if (!split) return false;
  out->body = split + 4;
  out->body_length = raw.size - static_cast<size_t>(out->body - raw.data);
  const char* line = raw.data;
  const char* end = strstr(line, "\r\n");
  const char* space = strchr(line, ' ');
  if (!space || space > end) return false;
  out->status = fortis::test::ParseInteger<int>(space + 1);
  line = end + 2;
  while (line < split && out->header_count < 32) {
    end = strstr(line, "\r\n");
    const char* colon = static_cast<const char*>(memchr(line, ':', static_cast<size_t>(end - line)));
    if (colon) {
      size_t n = static_cast<size_t>(colon - line);
      if (n > 255) n = 255;
      for (size_t i = 0; i < n; ++i) {
        const char c = line[i];
        out->headers[out->header_count][0][i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
      }
      out->headers[out->header_count][0][n] = '\0';
      const char* value = colon + 1;
      while (value < end && *value == ' ') ++value;
      size_t m = static_cast<size_t>(end - value);
      if (m > 255) m = 255;
      memcpy(out->headers[out->header_count][1], value, m);
      out->headers[out->header_count][1][m] = '\0';
      ++out->header_count;
    }
    line = end + 2;
  }
  return true;
}

// ---- the oracle ------------------------------------------------------------------------------------------

const char* const kHeaders[] = {"content-type", "content-length", "vary", "cache-control", "etag",
                                "x-content-type-options", "referrer-policy", "permissions-policy",
                                "x-rate-limit-cost", "retry-after"};

bool replace_tokens(const char* value, char* out, size_t capacity) {
  snprintf(out, capacity, "%s", value);
  for (const Device& d : g_devices) {
    char* at = strstr(out, d.fixture_token);
    if (at) memcpy(at, d.token, 54);
  }
  return true;
}

bool build_request(const tj::Value* c, Bytes* request) {
  const char* method = text(c, "method");
  const char* url = text(c, "url");
  Bytes body;
  if (const char* hex = text(c, "requestHex")) {
    if (!from_hex(hex, &body)) return false;
  } else if (const char* json = text(c, "requestJson")) {
    // JSON.stringify({ ...json, pad: 'p'.repeat(n) })
    const size_t n = static_cast<size_t>(fortis::test::ParseInteger<long>(text(c, "pad")));
    body.Append(json, strlen(json) - 1);
    body.Text(",\"pad\":\"");
    for (size_t i = 0; i < n; ++i) body.Append("p", 1);
    body.Text("\"}");
  }
  request->Text(method);
  request->Text(" ");
  request->Text(url);
  request->Text(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
  const tj::Value* headers = tj::Get(c, "requestHeaders");
  for (uint32_t i = 0; tj::IsObject(headers) && i < headers->count; ++i) {
    const tj::Member& member = headers->members[i];
    char value[2048];
    replace_tokens(member.value->text, value, sizeof(value));
    request->Append(member.key, member.key_length);
    request->Text(": ");
    request->Text(value);
    request->Text("\r\n");
  }
  if (body.size || !strcmp(method, "POST")) {
    char length[64];
    snprintf(length, sizeof(length), "Content-Length: %zu\r\n", body.size);
    request->Text(length);
  }
  request->Text("\r\n");
  if (body.size) request->Append(body.data, body.size);
  return true;
}

bool expected_body(const tj::Value* c, Bytes* out) {
  if (const char* hex = text(c, "wireHex")) return from_hex(hex, out);
  const tj::Value* body = tj::Get(c, "bodyText");
  if (!tj::IsString(body)) return false;
  out->Append(body->text, body->length);
  return true;
}

// Compares one answer with the case; returns true when it matches.
bool compare(const tj::Value* c, const Bytes& raw) {
  const char* name = text(c, "name");
  Parsed parsed;
  char detail[1024];
  if (!parse_response(raw, &parsed)) {
    fail(name, "unparsable response");
    return false;
  }
  // compat-decisions.md#form-bodies, #fastify-errors: text/plain
  // and form bodies are the body parser's 415, and every body-framing or
  // schema refusal is this server's {"error":...} after the device policy
  // (charged), or the policy's own refusal for a request without a device
  // credential.
  // auth-first: in these the device policy refuses before the body is
  // judged (the quota is spent, or attestation is required).
  static const struct { const char* name; int status; const char* body; } kPolicyFirst[] = {
      {"quotaOverSchemaInvalid", 429, "{\"error\":\"RATE_LIMITED\"}"},
      {"attestationSchemaFirst", 401, "{\"error\":\"ATTESTATION_REQUIRED\"}"},
  };
  for (const auto& p : kPolicyFirst) {
    if (strcmp(name, p.name) != 0) continue;
    const bool ok = parsed.status == p.status && parsed.body_length == strlen(p.body) &&
                    memcmp(parsed.body, p.body, parsed.body_length) == 0;
    if (!ok) fail(name, "the device policy does not answer before the body");
    return ok;
  }
  {
    const tj::Value* request_headers = tj::Get(c, "requestHeaders");
    const tj::Value* oracle = tj::Get(c, "bodyText");
    compat_expectations::FastifyExpectation expected;
    const bool removed = compat_expectations::IsRemovedBodyMedia(text(request_headers, "content-type"));
    if (removed || compat_expectations::FastifyExpected(tj::IsString(oracle) ? oracle->text : nullptr,
                                                        text(request_headers, "authorization"),
                                                        text(request_headers, "origin") != nullptr, &expected)) {
      if (removed)
        compat_expectations::FastifyExpected("{\"statusCode\":415,\"code\":\"FST_ERR_CTP_INVALID_MEDIA_TYPE\"}",
                                             text(request_headers, "authorization"),
                                             text(request_headers, "origin") != nullptr, &expected);
      const bool ok = parsed.status == expected.status && parsed.body_length == expected.body.size() &&
                      memcmp(parsed.body, expected.body.data(), parsed.body_length) == 0 &&
                      (parsed.Header("x-rate-limit-cost") != nullptr) == expected.charged;
      if (!ok) {
        snprintf(detail, sizeof(detail), "body framing: %d %.200s is not %d %s", parsed.status, parsed.body,
                 expected.status, expected.body.c_str());
        fail(name, detail);
      }
      return ok;
    }
  }
  const int status = static_cast<int>(number(c, "status"));
  if (parsed.status != status) {
    snprintf(detail, sizeof(detail), "status %d != %d (%.300s)", parsed.status, status, parsed.body);
    fail(name, detail);
    return false;
  }
  bool ok = true;
  Bytes want;
  if (!expected_body(c, &want)) {
    fail(name, "no expected body");
    return false;
  }
  // compat-decisions.md#zod-issues: where the oracle listed zod
  // issues this server answers the plain BAD_REQUEST body.
  static const char kZodPrefix[] = "{\"error\":\"BAD_REQUEST\",\"detail\":[";
  static const char kBadRequest[] = "{\"error\":\"BAD_REQUEST\"}";
  const bool zod = want.size > strlen(kZodPrefix) && memcmp(want.data, kZodPrefix, strlen(kZodPrefix)) == 0;
  if (zod) {
    want.size = 0;
    want.Append(kBadRequest, strlen(kBadRequest));
  }
  char plain_length[8];
  snprintf(plain_length, sizeof(plain_length), "%zu", strlen(kBadRequest));
  const tj::Value* headers = tj::Get(c, "headers");
  for (const char* header : kHeaders) {
    const char* want_header = text(headers, header);
    if (zod && want_header && !strcmp(header, "content-length")) want_header = plain_length;
    const char* got = parsed.Header(header);
    if ((want_header == nullptr) != (got == nullptr) || (want_header && strcmp(want_header, got) != 0)) {
      snprintf(detail, sizeof(detail), "header %s: %s != %s", header, got ? got : "(none)",
               want_header ? want_header : "(none)");
      fail(name, detail);
      ok = false;
    }
  }
  if (want.size != parsed.body_length || memcmp(want.data, parsed.body, want.size) != 0) {
    size_t at = 0;
    while (at < want.size && at < parsed.body_length && want.data[at] == parsed.body[at]) ++at;
    snprintf(detail, sizeof(detail), "body differs at byte %zu (want %zu, got %zu)", at, want.size,
             parsed.body_length);
    fail(name, detail);
    ok = false;
  }
  return ok;
}

struct Concurrent {
  HttpRouter* router;
  const Bytes* request;
  Bytes response;
};

void* concurrent_side(void* opaque) {
  auto* job = static_cast<Concurrent*>(opaque);
  dispatch(*job->router, *job->request, &job->response);
  return nullptr;
}

struct Tally {
  int matched = 0, asserted = 0, failed = 0, seeds = 0;
};

bool run_seed(Journal& journal, const tj::Value* c) {
  const char* kind = text(c, "seed");
  const tj::Value* input = tj::Get(c, "input");
  tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
  bool ok = false;
  if (!strcmp(kind, "append")) {
    address::Account account;
    const char* network = text(input, "network");
    const char* addr = text(input, "address");
    const address::ReadStatus status = journal.store->Find(network, addr, g_now, &account);
    ok = status == address::ReadStatus::kFound ||
         (status == address::ReadStatus::kMissing && journal.store->Touch(network, addr, g_now, &account));
    const tj::Value* rollback = tj::Get(input, "rollback");
    char head[32] = {};
    ok = ok && journal.store->AppendSyncEvents(account.id, tj::Get(input, "events"), g_now,
                                               !(rollback && rollback->type == tj::Type::kTrue), arena, head);
    const char* want = text(tj::Get(c, "result"), "head");
    if (ok && (!want || strcmp(want, head) != 0)) {
      fail(text(c, "name"), head);
      ok = false;
    }
  } else {
    Device* d = device(text(input, "device")[0]);
    const tj::Value* account_ref = tj::Get(input, "account");
    address::Account account;
    const char* account_id = nullptr;
    ok = d != nullptr;
    if (ok && tj::IsObject(account_ref)) {
      ok = journal.store->Find(text(account_ref, "network"), text(account_ref, "address"), g_now, &account) ==
           address::ReadStatus::kFound;
      account_id = account.id;
    }
    ok = ok && journal.store->WriteWatch(d->id, text(input, "topic"), account_id,
                                         static_cast<int64_t>(number(input, "expiresAt")), g_now, arena);
  }
  tj::FreeArena(arena);
  return ok;
}

void run_case(Journal& journal, const tj::Value* c, Tally* tally) {
  const char* name = text(c, "name");
  Bytes request;
  if (!build_request(c, &request)) {
    fail(name, "cannot build the request");
    ++tally->failed;
    return;
  }
  if (const tj::Value* copies = tj::Get(c, "concurrent")) {
    const size_t count = static_cast<size_t>(copies->number);
    Concurrent jobs[16];
    pthread_t threads[16];
    for (size_t i = 0; i < count && i < 16; ++i) {
      jobs[i].router = journal.router.get();
      jobs[i].request = &request;
      pthread_create(&threads[i], nullptr, concurrent_side, &jobs[i]);
    }
    bool ok = count <= 16;
    for (size_t i = 0; i < count && i < 16; ++i) {
      pthread_join(threads[i], nullptr);
      ok = compare(c, jobs[i].response) && ok;
    }
    ++(ok ? tally->matched : tally->failed);
    return;
  }
  Bytes response;
  dispatch(*journal.router, request, &response);
  Parsed parsed;
  if (!strcmp(name, "modelsScalarCoerced")) {
    const bool ok = parse_response(response, &parsed) && parsed.status == 400 &&
        parsed.body_length == 23 && !memcmp(parsed.body, "{\"error\":\"BAD_REQUEST\"}", 23);
    if (!ok) fail(name, "JSON models must be an array");
    ++(ok ? tally->asserted : tally->failed);
    return;
  }
  if (!strcmp(name, "jsonTooLargeForNative")) {
    // The request JSON parser's 256 KiB bound: 413, since fastify-errors
    // after the device policy (charged).
    const bool ok = parse_response(response, &parsed) && parsed.status == 413 &&
                    parsed.body_length == 26 && !memcmp(parsed.body, "{\"error\":\"BODY_TOO_LARGE\"}", 26) &&
                    parsed.Header("x-rate-limit-cost");
    if (!ok) fail(name, "native 256 KiB parser bound");
    ++(ok ? tally->asserted : tally->failed);
    return;
  }
  if (!strcmp(name, "methodGet") || !strcmp(name, "methodPut") || !strcmp(name, "pathTrailingSlash") ||
      !strcmp(name, "pathUpperCase")) {
    // The router answers a route miss itself (router-wide body).
    const bool ok = parse_response(response, &parsed) && parsed.status == 404 && number(c, "status") == 404 &&
                    !parsed.Header("x-rate-limit-cost");
    if (!ok) fail(name, "router miss");
    ++(ok ? tally->asserted : tally->failed);
    return;
  }
  ++(compare(c, response) ? tally->matched : tally->failed);
}

// Drains device B's per-minute quota until its first 429 (the window is the
// wall-clock minute; earlier groups may already have used some of it).
bool drain_quota(Journal& journal) {
  Bytes request;
  request.Text("POST /v1/sync HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nauthorization: Bearer ");
  request.Text(device('B')->token);
  request.Text("\r\ncontent-type: application/json\r\nContent-Length: 2\r\n\r\n{}");
  for (int sent = 0; sent < 1300; ++sent) {
    Bytes response;
    Parsed parsed;
    if (!dispatch(*journal.router, request, &response) || !parse_response(response, &parsed)) return false;
    if (parsed.status == 429) return true;
    if (parsed.status != 200) return false;
  }
  return false;
}

void replay_oracle(const tj::Value* fixture) {
  const tj::Value* groups = tj::Get(fixture, "groups");
  const tj::Value* cases = tj::Get(fixture, "cases");
  Journal journal;
  snprintf(journal.directory, sizeof(journal.directory), "%s/journal-0", g_root);
  mkdir(journal.directory, 0700);
  Tally total;
  for (uint32_t g = 0; g < groups->count; ++g) {
    const tj::Value* group = groups->items[g];
    const char* name = text(group, "name");
    const bool attestation = text(tj::Get(group, "env"), "ATTESTATION_MODE") != nullptr;
    g_now = static_cast<int64_t>(number(group, "start"));
    const bool started = g == 0 ? open_journal(&journal, attestation) : crash_restart(&journal, attestation);
    if (!started) {
      fail(name, "cannot start the group");
      return;
    }
    if (!strcmp(name, "quota")) {
      // Leave the rest of the wall minute for the cases.
      if (wall_ms() % 60000 > 50000) sleep_ms(60000 - wall_ms() % 60000 + 200);
      if (!drain_quota(journal)) fail(name, "device B quota did not drain to a 429");
    }
    Tally tally;
    for (uint32_t i = 0; i < cases->count; ++i) {
      const tj::Value* c = cases->items[i];
      if (strcmp(text(c, "group"), name) != 0) continue;
      g_now = static_cast<int64_t>(number(c, "at"));
      if (tj::Get(c, "seed")) {
        if (!run_seed(journal, c)) fail(text(c, "name"), "seed failed");
        ++tally.seeds;
        continue;
      }
      run_case(journal, c, &tally);
    }
    printf("oracle %s: %d matched, %d asserted differences, %d failed (%d seeds)\n", name, tally.matched,
           tally.asserted, tally.failed, tally.seeds);
    total.matched += tally.matched;
    total.asserted += tally.asserted;
    total.failed += tally.failed;
    checks += tally.matched + tally.asserted;
  }
  printf("oracle: %d matched, %d asserted differences, %d failed\n", total.matched, total.asserted, total.failed);
  journal.route->Stop();
  journal.router.reset();
  journal.route.reset();
  journal.store.reset();
  journal.kv.reset();
}

// ---- journal concurrency and crash images -------------------------------------------------------------

constexpr const char* kNet = "eth-mainnet";
constexpr int64_t kT0 = 1767323045000;

struct Fleet {
  KvStore* kv;
  address::Store* store;
  address::Account accounts[5];  // [4] is unwatched
  int frames;                     // per writer
  int events_per_frame;
  std::atomic<bool> stop;  // set by the main thread, polled by the workers
};

struct Writer {
  Fleet* fleet;
  int account;
  int written;
};

void* write_side(void* opaque) {
  auto* writer = static_cast<Writer*>(opaque);
  Fleet& fleet = *writer->fleet;
  for (int frame = 0; frame < fleet.frames && !fleet.stop; ++frame) {
    tj::Arena arena = tj::GrowingArena(1024 * 1024);
    char json[1024];
    int used = snprintf(json, sizeof(json), "[");
    for (int e = 0; e < fleet.events_per_frame; ++e)
      used += snprintf(json + used, sizeof(json) - static_cast<size_t>(used),
                       "%s{\"entityKey\":{\"id\":\"w%d-%d-%d\"},\"operation\":\"upsert\",\"fields\":{\"n\":%d},"
                       "\"changedAt\":%lld}",
                       e ? "," : "", writer->account, frame, e, e, static_cast<long long>(kT0));
    snprintf(json + used, sizeof(json) - static_cast<size_t>(used), "]");
    const tj::Value* events = tj::Parse(arena, reinterpret_cast<const uint8_t*>(json), strlen(json));
    char head[32];
    if (fleet.store->AppendSyncEvents(fleet.accounts[writer->account].id, events, kT0, true, arena, head))
      ++writer->written;
    tj::FreeArena(arena);
  }
  return nullptr;
}

struct Seen {
  char ids[4096][24];
  uint64_t sequences[4096];
  size_t count;
  const Fleet* fleet;
  size_t outside;
};

journal_sync::Visit collect(void* opaque, size_t account, const tj::Value* event, tj::Arena&) {
  auto* seen = static_cast<Seen*>(opaque);
  if (account >= 4) ++seen->outside;
  if (seen->count >= 4096) return journal_sync::Visit::kFailed;
  const tj::Value* sequence = tj::Get(event, "sequence");
  const tj::Value* entity = tj::Get(tj::Get(event, "entityKey"), "id");
  seen->sequences[seen->count] = strtoull(sequence->text, nullptr, 10);
  snprintf(seen->ids[seen->count], sizeof(seen->ids[0]), "%s", entity->text);
  ++seen->count;
  return journal_sync::Visit::kWritten;
}

bool read_page(const Fleet& fleet, size_t account_count, const char* cursor, uint32_t limit, Seen* seen,
               journal_sync::PageResult* page) {
  const char* ids[5];
  for (size_t i = 0; i < account_count; ++i) ids[i] = fleet.accounts[i].id;
  seen->count = 0;
  seen->outside = 0;
  seen->fleet = &fleet;
  tj::Arena arena = tj::GrowingArena(8 * 1024 * 1024);
  journal_sync::Journal journal(fleet.kv);
  const bool ok = journal.Read(ids, account_count, cursor, strlen(cursor), limit, kT0, false, 0, collect, seen,
                               arena, page);
  tj::FreeArena(arena);
  return ok;
}

struct Reader {
  Fleet* fleet;
  int reads;
  int errors;
};

void* read_side(void* opaque) {
  auto* reader = static_cast<Reader*>(opaque);
  Seen* seen = static_cast<Seen*>(calloc(1, sizeof(Seen)));
  uint64_t* last = static_cast<uint64_t*>(calloc(4096, sizeof(uint64_t)));
  size_t last_count = 0;
  char zero[journal_sync::kCursorTextBytes];
  journal_sync::FormatCursor("00000000000000000000", kT0, zero);
  while (!reader->fleet->stop) {
    journal_sync::PageResult page;
    if (!read_page(*reader->fleet, 4, zero, 500, seen, &page) || page.reset || seen->outside) {
      ++reader->errors;
      break;
    }
    for (size_t i = 1; i < seen->count; ++i)
      if (seen->sequences[i] <= seen->sequences[i - 1]) ++reader->errors;
    if (!page.has_more) {
      // Writes only ever add events past the head: a later complete read
      // extends the earlier one.
      if (seen->count < last_count || memcmp(seen->sequences, last, last_count * sizeof(uint64_t)) != 0)
        ++reader->errors;
      memcpy(last, seen->sequences, seen->count * sizeof(uint64_t));
      last_count = seen->count;
    }
    ++reader->reads;
  }
  free(last);
  free(seen);
  return nullptr;
}

// The app's poll: follow the returned cursors while writes land. Every
// watched event arrives once, in increasing order, never with a reset.
struct Follower {
  Fleet* fleet;
  size_t received;
  int errors;
};

void* follow_side(void* opaque) {
  auto* follower = static_cast<Follower*>(opaque);
  Seen* seen = static_cast<Seen*>(calloc(1, sizeof(Seen)));
  char cursor[journal_sync::kCursorTextBytes];
  journal_sync::FormatCursor("00000000000000000000", kT0, cursor);
  uint64_t last = 0;
  int idle = 0;
  while (idle < 2 && follower->errors == 0) {
    journal_sync::PageResult page;
    if (!read_page(*follower->fleet, 4, cursor, 9, seen, &page) || page.reset) {
      ++follower->errors;
      break;
    }
    for (size_t i = 0; i < seen->count; ++i) {
      if (seen->sequences[i] <= last) ++follower->errors;
      last = seen->sequences[i];
    }
    follower->received += seen->count;
    snprintf(cursor, sizeof(cursor), "%s", page.cursor);
    idle = follower->fleet->stop && !seen->count && !page.has_more ? idle + 1 : 0;
  }
  free(seen);
  return nullptr;
}

bool start_fleet(Fleet* fleet, const char* directory) {
  mkdir(directory, 0700);
  fleet->kv = new KvStore();
  if (!fleet->kv->Open(directory)) return false;
  fleet->store = new address::Store(fleet->kv);
  for (int i = 0; i < 5; ++i) {
    char addr[64];
    snprintf(addr, sizeof(addr), "0x%038d%02d", 0, i);
    if (!fleet->store->Touch(kNet, addr, kT0, &fleet->accounts[i])) return false;
  }
  return true;
}

void stop_fleet(Fleet* fleet) {
  delete fleet->store;
  delete fleet->kv;
}

void test_journal_concurrency() {
  char directory[600];
  snprintf(directory, sizeof(directory), "%s/concurrency", g_root);
  Fleet fleet{};
  fleet.frames = 150;
  fleet.events_per_frame = 1;
  if (!start_fleet(&fleet, directory)) {
    fail("concurrency", "cannot open the store");
    return;
  }
  Writer writers[5];
  Reader readers[6];
  Follower followers[2];
  pthread_t writer_threads[5], reader_threads[6], follower_threads[2];
  for (int i = 0; i < 6; ++i) {
    readers[i] = Reader{&fleet, 0, 0};
    pthread_create(&reader_threads[i], nullptr, read_side, &readers[i]);
  }
  for (int i = 0; i < 2; ++i) {
    followers[i] = Follower{&fleet, 0, 0};
    pthread_create(&follower_threads[i], nullptr, follow_side, &followers[i]);
  }
  for (int i = 0; i < 5; ++i) {
    writers[i] = Writer{&fleet, i, 0};
    pthread_create(&writer_threads[i], nullptr, write_side, &writers[i]);
  }
  for (pthread_t thread : writer_threads) pthread_join(thread, nullptr);
  fleet.stop = true;
  int reads = 0, errors = 0;
  for (int i = 0; i < 6; ++i) {
    pthread_join(reader_threads[i], nullptr);
    reads += readers[i].reads;
    errors += readers[i].errors;
  }
  CHECK(errors == 0, "concurrent pages are ordered, scoped and only grow");
  for (int i = 0; i < 2; ++i) {
    pthread_join(follower_threads[i], nullptr);
    CHECK(followers[i].errors == 0, "a cursor follower sees increasing sequences without a reset");
    CHECK(followers[i].received == static_cast<size_t>(4 * fleet.frames),
          "a cursor follower receives every watched event exactly once");
  }
  for (const Writer& writer : writers) CHECK(writer.written == fleet.frames, "every append committed");
  // The final read, paged at 7, sees every watched event once, in order.
  Seen* seen = static_cast<Seen*>(calloc(1, sizeof(Seen)));
  Seen* all = static_cast<Seen*>(calloc(1, sizeof(Seen)));
  char cursor[journal_sync::kCursorTextBytes];
  journal_sync::FormatCursor("00000000000000000000", kT0, cursor);
  for (int page_index = 0; page_index < 1000; ++page_index) {
    journal_sync::PageResult page;
    if (!read_page(fleet, 4, cursor, 7, seen, &page) || page.reset) {
      fail("concurrency", "final paging failed");
      break;
    }
    for (size_t i = 0; i < seen->count && all->count < 4096; ++i) {
      all->sequences[all->count] = seen->sequences[i];
      memcpy(all->ids[all->count++], seen->ids[i], sizeof(seen->ids[0]));
    }
    snprintf(cursor, sizeof(cursor), "%s", page.cursor);
    if (!page.has_more) break;
  }
  CHECK(all->count == static_cast<size_t>(4 * fleet.frames), "the final read has every watched event");
  bool ordered = true, distinct = true;
  for (size_t i = 1; i < all->count; ++i) ordered = ordered && all->sequences[i] > all->sequences[i - 1];
  for (size_t i = 0; i < all->count && distinct; ++i)
    for (size_t j = i + 1; j < all->count && distinct; ++j) distinct = strcmp(all->ids[i], all->ids[j]) != 0;
  CHECK(ordered, "the final read is in global order");
  CHECK(distinct, "the final read has no duplicate");
  bool unwatched = false;
  for (size_t i = 0; i < all->count; ++i) unwatched = unwatched || !strncmp(all->ids[i], "w4-", 3);
  CHECK(!unwatched, "no unwatched account's event");
  printf("journal concurrency: %d reads by 6 readers and 2 cursor followers during %d appends, %zu watched "
         "events, %d errors\n", reads, 5 * fleet.frames, all->count, errors);
  free(seen);
  free(all);
  stop_fleet(&fleet);
}

void test_crash_images() {
  char directory[600];
  snprintf(directory, sizeof(directory), "%s/crash-live", g_root);
  Fleet fleet{};
  fleet.frames = 400;
  fleet.events_per_frame = 2;
  if (!start_fleet(&fleet, directory)) {
    fail("crash", "cannot open the store");
    return;
  }
  Writer writers[5];
  pthread_t threads[5];
  for (int i = 0; i < 5; ++i) {
    writers[i] = Writer{&fleet, i, 0};
    pthread_create(&threads[i], nullptr, write_side, &writers[i]);
  }
  int images = 0, verified = 0;
  Seen* seen = static_cast<Seen*>(calloc(1, sizeof(Seen)));
  for (int image = 0; image < 6; ++image) {
    sleep_ms(15);
    char path[700];
    snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    size_t length = 0;
    char* bytes = read_file(path, &length);
    if (!bytes) continue;
    ++images;
    char copy[600];
    snprintf(copy, sizeof(copy), "%s/crash-image-%d", g_root, image);
    mkdir(copy, 0700);
    char copy_path[700];
    snprintf(copy_path, sizeof(copy_path), "%s/fortis.kv", copy);
    // A torn tail: the image may end inside a frame; recovery drops it.
    write_file(copy_path, bytes, length > 7 && image % 2 ? length - 7 : length);
    free(bytes);
    KvStore kv;
    if (!kv.Open(copy)) {
      fail("crash", "the image does not reopen");
      continue;
    }
    address::Store store(&kv);
    Fleet view{};
    view.kv = &kv;
    view.store = &store;
    memcpy(view.accounts, fleet.accounts, sizeof(view.accounts));
    char head[journal_sync::kSequenceDigits + 1];
    journal_sync::Journal journal(&kv);
    if (!journal.Head(kT0, head)) {
      fail("crash", "unreadable head");
      continue;
    }
    // Every account: the journal is exactly 1..head, and the two events of
    // a frame (w{a}-{f}-0, w{a}-{f}-1) hold consecutive sequences.
    char cursor[journal_sync::kCursorTextBytes];
    journal_sync::FormatCursor("00000000000000000000", kT0, cursor);
    uint64_t expected = 1;
    bool contiguous = true, whole = true;
    char previous[24] = "";
    for (int page_index = 0; page_index < 10000; ++page_index) {
      journal_sync::PageResult page;
      if (!read_page(view, 5, cursor, 500, seen, &page) || page.reset) {
        contiguous = false;
        break;
      }
      for (size_t i = 0; i < seen->count; ++i) {
        contiguous = contiguous && seen->sequences[i] == expected++;
        const char* id = seen->ids[i];
        const size_t n = strlen(id);
        if (id[n - 1] == '1') whole = whole && !strncmp(previous, id, n - 1) && previous[n - 1] == '0';
        else if (previous[0] && previous[strlen(previous) - 1] == '0') whole = false;
        snprintf(previous, sizeof(previous), "%s", id);
      }
      snprintf(cursor, sizeof(cursor), "%s", page.cursor);
      if (!page.has_more) break;
    }
    if (previous[0] && previous[strlen(previous) - 1] == '0') whole = false;
    CHECK(contiguous && expected - 1 == strtoull(head, nullptr, 10), "a crash image's journal is 1..head");
    CHECK(whole, "a crash image keeps every frame whole");
    tj::Arena arena = tj::GrowingArena(1024 * 1024);
    const char* event = "[{\"entityKey\":{\"id\":\"after\"},\"operation\":\"upsert\",\"fields\":{},\"changedAt\":0}]";
    char next[32];
    CHECK(store.AppendSyncEvents(fleet.accounts[0].id, tj::Parse(arena, reinterpret_cast<const uint8_t*>(event),
                                                                 strlen(event)),
                                 kT0, true, arena, next) &&
              strtoull(next, nullptr, 10) == strtoull(head, nullptr, 10) + 1,
          "the next append after a crash takes head + 1");
    tj::FreeArena(arena);
    ++verified;
  }
  fleet.stop = true;
  for (pthread_t thread : threads) pthread_join(thread, nullptr);
  CHECK(images == 6 && verified == 6, "six crash images verified");
  printf("crash images: %d of %d verified while 5 writers appended two-event frames\n", verified, images);
  free(seen);
  stop_fleet(&fleet);
}

// ---- scope isolation under slot sharing -------------------------------------------------------------------

uint64_t device_hash(const char* device_id) {
  uint64_t h = 1469598103934665603ULL;
  for (const char* p = device_id; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
  return h ? h : 1;
}

struct ScopeRun {
  journal_sync::Scope* scope;
  address::Store* store;
  const char* device;
  const char* own_id;
  int reads;
  int wrong;
  std::atomic<bool>* stop;
};

void* scope_side(void* opaque) {
  auto* run = static_cast<ScopeRun*>(opaque);
  int64_t now = kT0;
  while (!*run->stop) {
    tj::Arena arena = tj::GrowingArena(1024 * 1024);
    journal_sync::ScopeAccount* accounts = nullptr;
    size_t count = 0;
    // Advancing the clock past the cache lifetime forces recomputes too.
    now += 997;
    if (!run->scope->Accounts(run->device, now, arena, &accounts, &count) || count != 1 ||
        strcmp(accounts[0].id, run->own_id) != 0)
      ++run->wrong;
    if (run->reads % 7 == 0) run->scope->Invalidate(run->device);
    ++run->reads;
    tj::FreeArena(arena);
  }
  return nullptr;
}

void test_scope_isolation() {
  // Two device ids in one of the 8,192 cache slots.
  char first[27] = "01ARZ3NDEKTSV4RRFFQ69G0000", second[27] = {};
  const uint64_t slot = device_hash(first) % journal_sync::kScopeSlots;
  for (unsigned i = 1; i < 1000000 && !second[0]; ++i) {
    char candidate[27];
    snprintf(candidate, sizeof(candidate), "01ARZ3NDEKTSV4RRFFQ6%06u", i);
    if (device_hash(candidate) % journal_sync::kScopeSlots == slot) memcpy(second, candidate, sizeof(second));
  }
  CHECK(second[0] != 0, "two device ids share a scope cache slot");
  if (!second[0]) return;
  char directory[600];
  snprintf(directory, sizeof(directory), "%s/scope", g_root);
  mkdir(directory, 0700);
  KvStore kv;
  if (!kv.Open(directory)) {
    fail("scope", "cannot open the store");
    return;
  }
  address::Store store(&kv);
  address::Account a, b;
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  const int64_t far = kT0 + 30LL * 86400000;
  const bool seeded = store.Touch(kNet, "0x00000000000000000000000000000000000000aa", kT0, &a) &&
                      store.Touch(kNet, "0x00000000000000000000000000000000000000bb", kT0, &b) &&
                      store.WriteWatch(first, "address:eth-mainnet:0x00000000000000000000000000000000000000aa",
                                       nullptr, far, kT0, arena) &&
                      store.WriteWatch(second, "address:eth-mainnet:0x00000000000000000000000000000000000000bb",
                                       nullptr, far, kT0, arena);
  tj::FreeArena(arena);
  CHECK(seeded, "scope seeds");
  journal_sync::Scope scope;
  CHECK(scope.Start(&store), "scope starts");
  std::atomic<bool> stop{false};
  ScopeRun runs[4] = {{&scope, &store, first, a.id, 0, 0, &stop}, {&scope, &store, second, b.id, 0, 0, &stop},
                      {&scope, &store, first, a.id, 0, 0, &stop}, {&scope, &store, second, b.id, 0, 0, &stop}};
  pthread_t threads[4];
  for (int i = 0; i < 4; ++i) pthread_create(&threads[i], nullptr, scope_side, &runs[i]);
  sleep_ms(300);
  stop = true;
  int reads = 0, wrong = 0;
  for (int i = 0; i < 4; ++i) {
    pthread_join(threads[i], nullptr);
    reads += runs[i].reads;
    wrong += runs[i].wrong;
  }
  CHECK(wrong == 0, "slot-sharing devices always read their own scope");
  // A cached scope ignores a new plain watch until the device's entry is
  // invalidated (rememberTopic/remember do that), then shows it at once.
  {
    tj::Arena scratch = tj::GrowingArena(1024 * 1024);
    address::Account c;
    const int64_t at = kT0 + 10LL * 86400000;
    journal_sync::ScopeAccount* accounts = nullptr;
    size_t before = 0, cached = 0, after = 0;
    CHECK(scope.Accounts(first, at, scratch, &accounts, &before) && before == 1, "scope before the new watch");
    CHECK(store.Touch(kNet, "0x00000000000000000000000000000000000000cc", at, &c) &&
              store.WriteWatch(first, "address:eth-mainnet:0x00000000000000000000000000000000000000cc", nullptr,
                               far, at, scratch),
          "new watch");
    CHECK(scope.Accounts(first, at + 1000, scratch, &accounts, &cached) && cached == 1,
          "a plain watch write leaves the cached scope");
    scope.Invalidate(first);
    CHECK(scope.Accounts(first, at + 2000, scratch, &accounts, &after) && after == 2,
          "an invalidated scope shows the new watch at once");
    tj::FreeArena(scratch);
  }
  printf("scope isolation: %d concurrent reads of two slot-sharing devices (%s, %s), %d wrong\n", reads, first,
         second, wrong);
  scope.Stop();
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s sync_edges.json\n", argv[0]);
    return 2;
  }
  size_t size = 0;
  char* data = read_file(argv[1], &size);
  if (!data) {
    perror(argv[1]);
    return 2;
  }
  char pin_path[1024];
  snprintf(pin_path, sizeof(pin_path), "%s.sha256", argv[1]);
  size_t pin_size = 0;
  char* pin = read_file(pin_path, &pin_size);
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(data), size, digest, 0);
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (!pin || pin_size < 64 || strncmp(pin, hex, 64) != 0 || strcmp(pin + 64, "  sync_edges.json\n") != 0) {
    fprintf(stderr, "sync_edges.json sha256 %s does not match its pin\n", hex);
    return 1;
  }
  free(pin);

  char root[] = "/tmp/fortis-sync-edges-unit-XXXXXX";
  if (!mkdtemp(root)) return 2;
  g_root = root;
  char auth[600];
  snprintf(auth, sizeof(auth), "%s/auth", root);
  mkdir(auth, 0700);
  fortis::storage::RuntimeKvSetDirectory(auth);

  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), size);
  strict::PatchRows(arena, tj::Get(fixture, "cases"), strict::UnitExpectations(arena, "tests/fixtures/strict_sync_order_cases.json"), "name");
  if (!tj::IsObject(fixture)) {
    fprintf(stderr, "cannot parse the fixture\n");
    return 2;
  }
  if (!seed_devices(tj::Get(fixture, "tokens"))) {
    fprintf(stderr, "cannot seed the fixture devices\n");
    return 2;
  }
  replay_oracle(fixture);
  test_journal_concurrency();
  test_crash_images();
  test_scope_isolation();
  tj::FreeArena(arena);
  free(data);
  fortis::test::RemoveTree(root);
  if (failures) {
    fprintf(stderr, "sync_edges_test: %d failure(s) over %d checks\n", failures, checks);
    return 1;
  }
  printf("sync_edges_test: %d checks passed\n", checks);
  g_root = nullptr;
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
