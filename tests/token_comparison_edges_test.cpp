#include <algorithm>
#include <stdexcept>
#include <new>
#include "test_support.h"
// R53 POST /v1/tokens/compare units against
// tests/corpus/token_comparison_edges.json (written by
// the earlier TypeScript implementation's tests; its
// SHA-256 pin is checked first):
//
//  1. The TypeScript oracle in process: every fixture case runs through the
//     real route (RegisterTokenComparisonRoute on an HttpRouter), dispatched
//     over a socketpair with the bytes the socket replay sends, at the case's
//     clock (the context's now_ms). Each group boundary is a hard restart:
//     the comparison KV file is copied byte for byte while the old store is
//     still open (no close path runs) and the group opens the copy with a new
//     KvStore and router; the group's seed rows are then patched through
//     LoadTokenComparisonSeedFile, as TypeScript patches them after its boot.
//     Devices live in the runtime KV (development tier, 240 per minute; the
//     fixture uses each device letter in one group and one clock minute), so
//     status, every fixture header including x-rate-limit-remaining, and the
//     exact body bytes must match. The one documented native difference
//     (jsonLoneSurrogate) is asserted. `concurrent` cases dispatch their
//     copies from threads at once; each answer must equal the recorded one
//     and the remaining values must be the recorded multiset.
//  2. Restart durability: the KV image of the last group, reopened, answers
//     the restart group's comparison byte for byte (no row was rewritten).
//  3. Concurrency: 16 threads send 40 requests each over nine request kinds
//     (JSON and protobuf answers, a 304, a protobuf request, a zod refusal,
//     invalid/duplicate refs, a store failure, a form body, an unknown
//     bearer); every answer must equal its kind's answer taken alone.
//
// Usage: token_comparison_edges_test <token_comparison_edges.json>
#include "strict_proto_cases.h"
#include "../src/auth/device_auth.h"
#include "../src/http/http_server.h"
#include "../src/storage/kv_runtime.h"
#include "../src/storage/kv_store.h"
#include "../src/tokens/token_comparison_route.h"
#include "../src/codec/token_json.h"
#include "compat_expectations.h"
#include "fake_seams.h"
#include "token_comparison_seed.h"

#include <mbedtls/sha256.h>

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
using namespace fortis::tokens;

namespace tj = fortis::codec::tj;
using fortis::storage::KvStore;

namespace {

int failures = 0;
int checks = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %.600s\n", what, detail);
}

#define CHECK(condition, what)                \
  do {                                        \
    ++checks;                                 \
    if (!(condition)) fail(what, #condition); \
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

// The native quota window is the wall-clock minute: start a group with at
// least `budget_ms` of it left.
void wait_for_fresh_minute(int64_t budget_ms) {
  const int64_t left = 60000 - wall_ms() % 60000;
  if (left < budget_ms) sleep_ms(left + 200);
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
  Bytes() = default;
  Bytes(const Bytes&) = delete;
  Bytes& operator=(const Bytes&) = delete;
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
  void Assign(const Bytes& other) {
    size = 0;
    Append(other.data ? other.data : "", other.size);
  }
};

bool from_hex(const char* hex, Bytes* out) {
  const size_t length = strlen(hex);
  if (length % 2) return false;
  for (size_t i = 0; i < length; i += 2) {
    char pair[3] = {hex[i], hex[i + 1], 0};
    char* end = nullptr;
    const uint8_t byte = static_cast<uint8_t>(strtoul(pair, &end, 16));
    if (*end) return false;
    out->Append(&byte, 1);
  }
  return true;
}

// ---- the clock, the devices and the store ----------------------------------------------------------------

int64_t g_now = 0;
fortis::testing::PointerClock g_clock(&g_now);

// The letters the discovery route test binary seeds; ids as it derives them.
const char kLetters[] = "HABCDEFGIJKLMNOPQRSTUVWXYZ";

bool seed_devices() {
  fortis::auth::DeviceAuthStore auth(fortis::storage::RuntimeKvStore());
  const int64_t now = wall_ms();
  static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  for (size_t i = 0; kLetters[i]; ++i) {
    char token[64];
    char id[27];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = kLetters[i];
    token[54] = '\0';
    snprintf(id, sizeof(id), "01ARZ3NDEKTSV4RRFFQ69G5F%c%c", kLetters[i] == 'H' ? 'A' : 'B',
             kLetters[i] == 'H' ? 'W' : kCrockford[i]);
    // Two hours: the whole run, including a minute-boundary wait per group.
    if (!auth.SeedPriceRouteFixture(token, id, now, 2 * 3600000)) return false;
  }
  return true;
}

struct Instance {
  char directory[512] = {};
  KvStore* kv = nullptr;
  HttpRouter* router = nullptr;
  TokenComparisonRouteContext context{};
  int generation = 0;
};

const char* g_root = nullptr;

bool open_instance(Instance* instance, bool attestation_required) {
  instance->kv = new KvStore();
  if (!instance->kv->Open(instance->directory)) return false;
  instance->router = new HttpRouter();
  instance->context = TokenComparisonRouteContext{};
  instance->context.kv = instance->kv;
  instance->context.attestation_required = attestation_required;
  instance->context.clock = &g_clock;
  return RegisterTokenComparisonRoute(*instance->router, &instance->context);
}

// A hard restart: the KV file as it is on disk now, opened by a new process
// image (new store and router). The first call starts an empty store.
bool crash_restart(Instance* instance, bool attestation_required) {
  char path[600];
  size_t length = 0;
  char* image = nullptr;
  if (instance->kv) {
    snprintf(path, sizeof(path), "%s/fortis.kv", instance->directory);
    image = read_file(path, &length);
    if (!image) return false;
  }
  snprintf(instance->directory, sizeof(instance->directory), "%s/kv-%d", g_root, ++instance->generation);
  mkdir(instance->directory, 0700);
  bool copied = true;
  if (image) {
    snprintf(path, sizeof(path), "%s/fortis.kv", instance->directory);
    copied = write_file(path, image, length);
    free(image);
  }
  delete instance->router;
  delete instance->kv;
  instance->router = nullptr;
  instance->kv = nullptr;
  return copied && open_instance(instance, attestation_required);
}

// The seed rows as tests/token_comparison_seed.h lines.
bool write_seed(const tj::Value* rows, const char* failure_network, const char* path) {
  Bytes out;
  for (uint32_t r = 0; tj::IsArray(rows) && r < rows->count; ++r) {
    const tj::Value* row = rows->items[r];
    const tj::Value* fields = tj::Get(row, "fields");
    for (uint32_t f = 0; tj::IsObject(fields) && f < fields->count; ++f) {
      const tj::Member& member = fields->members[f];
      const tj::Value* value = member.value;
      char head[512];
      snprintf(head, sizeof(head), "%s\t%s\t%s\t%.*s\t", text(row, "bucket"), text(row, "network"),
               text(row, "tokenId"), static_cast<int>(member.key_length), member.key);
      out.Text(head);
      char scalar[64];
      if (value->type == tj::Type::kNull) {
        out.Text("null\t-");
      } else if (tj::IsObject(value)) {
        snprintf(scalar, sizeof(scalar), "date\t%lld", static_cast<long long>(number(value, "ms")));
        out.Text(scalar);
      } else if (tj::IsString(value)) {
        out.Text("str\t");
        for (uint32_t i = 0; i < value->length; ++i) {
          snprintf(scalar, sizeof(scalar), "%02x", static_cast<unsigned char>(value->text[i]));
          out.Text(scalar);
        }
      } else if (tj::IsNumber(value)) {
        snprintf(scalar, sizeof(scalar), "num\t%.17g", value->number);
        out.Text(scalar);
      } else {
        return false;
      }
      out.Text("\n");
    }
  }
  if (failure_network) {
    char line[256];
    snprintf(line, sizeof(line), "comparison\t%s\tnative\t-\tcorrupt\t-\n", failure_network);
    out.Text(line);
  }
  return write_file(path, out.data ? out.data : "", out.size);
}

// ---- one HTTP exchange over a socketpair ------------------------------------------------------------

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
  response->Assign(exchange.response);
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
      while (value < end && (*value == ' ' || *value == '\t')) ++value;
      const char* value_end = end;
      while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
      size_t m = static_cast<size_t>(value_end - value);
      if (m > 255) m = 255;
      memcpy(out->headers[out->header_count][1], value, m);
      out->headers[out->header_count][1][m] = '\0';
      ++out->header_count;
    }
    line = end + 2;
  }
  return true;
}

// ---- the oracle ------------------------------------------------------------------------------------------------

const char* const kHeaders[] = {"content-type", "content-length", "vary", "cache-control", "etag",
                                "x-content-type-options", "referrer-policy", "permissions-policy",
                                "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after"};

// The exact bytes the oracle wrote: Host, Connection: close, the recorded
// headers in order, Content-Length, the body.
bool build_request(const tj::Value* c, Bytes* request) {
  Bytes body;
  if (const char* hex = text(c, "requestHex"))
    if (!from_hex(hex, &body)) return false;
  request->Text("POST /v1/tokens/compare HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
  const tj::Value* headers = tj::Get(c, "requestHeaders");
  for (uint32_t i = 0; tj::IsObject(headers) && i < headers->count; ++i) {
    const tj::Member& member = headers->members[i];
    request->Append(member.key, member.key_length);
    request->Text(": ");
    request->Append(member.value->text, member.value->length);
    request->Text("\r\n");
  }
  char length[64];
  snprintf(length, sizeof(length), "Content-Length: %zu\r\n\r\n", body.size);
  request->Text(length);
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

compat_expectations::QuotaShift g_quota_shift;

// compat-decisions.md#form-bodies, #js-prototype-keys,
// #fastify-errors: the answers
// this server gives where the oracle recorded a removed behaviour. True when
// `c` is such a case (the check is done).
bool compare_removed(const tj::Value* c, const Parsed& parsed, bool* ok) {
  namespace ce = compat_expectations;
  const char* name = text(c, "name");
  const tj::Value* request_headers = tj::Get(c, "requestHeaders");
  const char* bearer = text(request_headers, "authorization");
  const tj::Value* headers = tj::Get(c, "headers");
  const char* oracle_cost = text(headers, "x-rate-limit-cost");
  Bytes body;
  if (const char* hex = text(c, "requestHex")) from_hex(hex, &body);
  const tj::Value* oracle = tj::Get(c, "bodyText");
  if (ce::IsPrototypePoisoningCase(body.data, body.size, tj::IsString(oracle) ? oracle->text : nullptr)) {
    static const char kRefused[] = "{\"error\":\"BAD_REQUEST\",\"detail\":\"invalid comparison request\"}";
    const char* cost = parsed.Header("x-rate-limit-cost");
    if (cost) g_quota_shift.Charged(bearer ? bearer : "", static_cast<int>(strtol(cost, nullptr, 10)));
    *ok = parsed.status == 400 && cost && parsed.body_length == strlen(kRefused) &&
          memcmp(parsed.body, kRefused, parsed.body_length) == 0;
    if (!*ok) fail(name, "a __proto__ key is not the schema's refusal after the quota");
    return true;
  }
  // auth-first: in these the device policy refuses before the body is
  // judged (the quota is spent, or attestation is required).
  static const struct { const char* name; int status; const char* body; } kPolicyFirst[] = {
      {"quotaOverForm", 429, "{\"error\":\"RATE_LIMITED\"}"},
      {"quotaOverInvalidJson", 429, "{\"error\":\"RATE_LIMITED\"}"},
      {"quotaOverMediaType", 429, "{\"error\":\"RATE_LIMITED\"}"},
      {"attestationInvalidJsonFirst", 401, "{\"error\":\"ATTESTATION_REQUIRED\"}"},
  };
  for (const auto& p : kPolicyFirst) {
    if (strcmp(name, p.name) != 0) continue;
    *ok = parsed.status == p.status && parsed.body_length == strlen(p.body) &&
          memcmp(parsed.body, p.body, parsed.body_length) == 0;
    if (!*ok) fail(name, "the device policy does not answer before the body");
    return true;
  }
  // fastify-errors: a body-framing refusal (text/plain and form bodies
  // included) is judged after the device policy and charged; without a
  // device credential the policy's refusal answers.
  ce::FastifyExpectation expected;
  const tj::Value* oracle_body = tj::Get(c, "bodyText");
  const bool removed = ce::IsRemovedBodyMedia(text(request_headers, "content-type"));
  if (removed || ce::FastifyExpected(tj::IsString(oracle_body) ? oracle_body->text : nullptr, bearer,
                                     text(request_headers, "origin") != nullptr, &expected)) {
    if (removed)
      ce::FastifyExpected("{\"statusCode\":415,\"code\":\"FST_ERR_CTP_INVALID_MEDIA_TYPE\"}", bearer,
                          text(request_headers, "origin") != nullptr, &expected);
    if (oracle_cost) g_quota_shift.Uncharged(bearer ? bearer : "", static_cast<int>(strtol(oracle_cost, nullptr, 10)));
    const char* cost = parsed.Header("x-rate-limit-cost");
    if (cost) g_quota_shift.Charged(bearer ? bearer : "", static_cast<int>(strtol(cost, nullptr, 10)));
    *ok = parsed.status == expected.status && parsed.body_length == expected.body.size() &&
          memcmp(parsed.body, expected.body.data(), parsed.body_length) == 0 && (cost != nullptr) == expected.charged;
    if (!*ok) fail(name, "a body-framing refusal is not this server's answer after the device policy");
    return true;
  }
  return false;
}

// Compares one answer with the case; returns true when it matches.
bool compare(const tj::Value* c, const Bytes& raw, bool remaining) {
  const char* name = text(c, "name");
  Parsed parsed;
  char detail[1024];
  ++checks;
  if (!parse_response(raw, &parsed)) {
    fail(name, "unparsable response");
    return false;
  }
  bool removed_ok = false;
  if (compare_removed(c, parsed, &removed_ok)) return removed_ok;
  const char* bearer = text(tj::Get(c, "requestHeaders"), "authorization");
  const int status = static_cast<int>(number(c, "status"));
  if (parsed.status != status) {
    snprintf(detail, sizeof(detail), "status %d != %d (%.300s)", parsed.status, status, parsed.body);
    fail(name, detail);
    return false;
  }
  bool ok = true;
  const tj::Value* headers = tj::Get(c, "headers");
  for (const char* header : kHeaders) {
    if (!remaining && !strcmp(header, "x-rate-limit-remaining")) continue;
    const char* want = text(headers, header);
    std::string shifted;
    if (want && !strcmp(header, "x-rate-limit-remaining")) {
      shifted = g_quota_shift.Remaining(bearer ? bearer : "", want);
      want = shifted.c_str();
    }
    const char* got = parsed.Header(header);
    if ((want == nullptr) != (got == nullptr) || (want && strcmp(want, got) != 0)) {
      snprintf(detail, sizeof(detail), "header %s: %s != %s", header, got ? got : "(none)", want ? want : "(none)");
      fail(name, detail);
      ok = false;
    }
  }
  Bytes want;
  if (!expected_body(c, &want)) {
    fail(name, "no expected body");
    return false;
  }
  if (want.size != parsed.body_length || (want.size && memcmp(want.data, parsed.body, want.size) != 0)) {
    size_t at = 0;
    while (at < want.size && at < parsed.body_length && want.data[at] == parsed.body[at]) ++at;
    snprintf(detail, sizeof(detail), "body differs at byte %zu (want %zu, got %zu): %.120s", at, want.size,
             parsed.body_length, parsed.body + (at > 40 ? at - 40 : 0));
    fail(name, detail);
    ok = false;
  }
  return ok;
}

// JSON lone-surrogate escapes normalize to the same UTF-8 used on the wire.
bool assert_lone_surrogate(const tj::Value* c, const Bytes& raw) {
  static const char kExpected[] = "{\"fiat\":\"usd\",\"computedAt\":\"2026-01-02T03:04:12.000Z\",\"items\":[],\"errors\":[{\"inputIndex\":0,\"network\":\"�\",\"tokenId\":\"native\",\"code\":\"INVALID_TOKEN\"}]}";
  Parsed parsed;
  ++checks;
  const bool ok = parse_response(raw, &parsed) && parsed.status == 200 && number(c, "status") == 200 &&
      parsed.body_length == strlen(kExpected) && !memcmp(parsed.body, kExpected, parsed.body_length) &&
      parsed.Header("etag") && !strcmp(parsed.Header("etag"), "W/\"token-comparison-58bbe9249db0a6d93f4e02e5\"") &&
      parsed.Header("x-rate-limit-cost") && !strcmp(parsed.Header("x-rate-limit-cost"), "1");
  if (!ok) fail(text(c, "name"), "Unicode replacement body/ETag differs");
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
  int matched = 0, asserted = 0, failed = 0, concurrent = 0;
};


void run_case(Instance& instance, const tj::Value* c, Tally* tally) {
  const char* name = text(c, "name");
  g_now = static_cast<int64_t>(number(c, "at"));
  Bytes request;
  if (!build_request(c, &request)) {
    fail(name, "cannot build the request");
    ++tally->failed;
    return;
  }
  if (const tj::Value* copies = tj::Get(c, "concurrent")) {
    const size_t count = static_cast<size_t>(copies->number);
    if (count > 16) {
      fail(name, "too many copies");
      ++tally->failed;
      return;
    }
    Concurrent jobs[16];
    pthread_t threads[16];
    for (size_t i = 0; i < count; ++i) {
      jobs[i].router = instance.router;
      jobs[i].request = &request;
      pthread_create(&threads[i], nullptr, concurrent_side, &jobs[i]);
    }
    bool ok = true;
    const char* got[16];
    char got_storage[16][16];
    const char* want[16];
    const tj::Value* responses = tj::Get(c, "responses");
    for (size_t i = 0; i < count; ++i) {
      pthread_join(threads[i], nullptr);
      ok = compare(c, jobs[i].response, false) && ok;
      Parsed parsed;
      parse_response(jobs[i].response, &parsed);
      const char* remaining = parsed.Header("x-rate-limit-remaining");
      snprintf(got_storage[i], sizeof(got_storage[i]), "%s", remaining ? remaining : "");
      got[i] = got_storage[i];
      const tj::Value* recorded = tj::IsArray(responses) && i < responses->count ? responses->items[i] : nullptr;
      want[i] = tj::IsString(recorded) ? recorded->text : "";
    }
    std::sort(got, got + count, [](const char* a, const char* b) { return strcmp(a, b) < 0; });
    std::sort(want, want + count, [](const char* a, const char* b) { return strcmp(a, b) < 0; });
    ++checks;
    for (size_t i = 0; i < count; ++i) {
      if (strcmp(got[i], want[i]) != 0) {
        fail(name, "concurrent x-rate-limit-remaining multiset differs");
        ok = false;
        break;
      }
    }
    ++tally->concurrent;
    ++(ok ? tally->matched : tally->failed);
    return;
  }
  Bytes response;
  dispatch(*instance.router, request, &response);
  if (strict::ProtoReject("comparison", name)) {
    Parsed parsed;
    constexpr const char* expected = "{\"error\":\"BAD_REQUEST\",\"detail\":\"invalid comparison request\"}";
    const bool ok = parse_response(response, &parsed) && parsed.status == 400 &&
        parsed.body_length == strlen(expected) && !memcmp(parsed.body, expected, parsed.body_length);
    if (!ok) fail(name, "strict protobuf fiat retains BOM and refuses non-usd");
    ++(ok ? tally->asserted : tally->failed);
    return;
  }
  if (!strcmp(name, "jsonLoneSurrogate")) {
    ++(assert_lone_surrogate(c, response) ? tally->asserted : tally->failed);
    return;
  }
  ++(compare(c, response, true) ? tally->matched : tally->failed);
}

const tj::Value* g_fixture = nullptr;
Instance g_instance;

void replay_oracle() {
  const tj::Value* groups = tj::Get(g_fixture, "groups");
  const tj::Value* cases = tj::Get(g_fixture, "cases");
  const char* failure_network = text(g_fixture, "storeFailureNetwork");
  Tally tally;
  for (uint32_t g = 0; tj::IsArray(groups) && g < groups->count; ++g) {
    const tj::Value* group = groups->items[g];
    const char* group_name = text(group, "name");
    const tj::Value* env = tj::Get(group, "env");
    const char* mode = text(env, "ATTESTATION_MODE");
    const bool attestation = mode && !strcmp(mode, "required");
    if (!crash_restart(&g_instance, attestation)) {
      fail(group_name, "cannot restart the store");
      return;
    }
    char seed[600];
    snprintf(seed, sizeof(seed), "%s/seed-%d.tsv", g_root, g_instance.generation);
    ++checks;
    if (!write_seed(tj::Get(group, "seed"), g == 0 ? failure_network : nullptr, seed) ||
        !LoadTokenComparisonSeedFile(*g_instance.kv, seed)) {
      fail(group_name, "cannot seed the group");
      return;
    }
    wait_for_fresh_minute(20000);
    g_quota_shift.Reset();
    const int before = tally.failed;
    int count = 0;
    for (uint32_t i = 0; tj::IsArray(cases) && i < cases->count; ++i) {
      const tj::Value* c = cases->items[i];
      if (strcmp(text(c, "group"), group_name) != 0) continue;
      run_case(g_instance, c, &tally);
      ++count;
    }
    printf("group %s: %d cases, %d failed\n", group_name, count, tally.failed - before);
  }
  printf("oracle: %d matched (%d concurrent), %d asserted differences, %d failed\n", tally.matched,
         tally.concurrent, tally.asserted, tally.failed);
}

// ---- native-only: concurrency and restart durability ------------------------------------------------------

struct Kind {
  const char* name;
  const char* accept;
  const char* content_type;
  const char* body;  // hex when `hex`
  bool hex;
  bool unknown_bearer;
  bool revalidate;
};

const Kind kKinds[] = {
    {"json", "application/json", "application/json",
     "{\"tokens\":[{\"network\":\"eth-mainnet\",\"tokenId\":\"erc20:0x1111111111111111111111111111111111111111\"},"
     "{\"network\":\"bnb\",\"tokenId\":\"native\"}]}", false, false, false},
    {"protobufAnswer", "application/protobuf", "application/json",
     "{\"tokens\":[{\"network\":\"eth-mainnet\",\"tokenId\":\"0x4444444444444444444444444444444444444444\"},"
     "{\"network\":\"base\",\"tokenId\":\"native\"}]}", false, false, false},
    {"protobufRequest", "application/protobuf", "application/protobuf",
     "0a0d0a03626e6212066e61746976651203757364", true, false, false},
    {"zodRefusal", "application/json", "application/json", "{\"tokens\":[]}", false, false, false},
    {"invalidRefs", "application/json", "application/json",
     "{\"tokens\":[{\"network\":\"nowhere\",\"tokenId\":\"native\"},{\"network\":\"bnb\",\"tokenId\":\"native\"},"
     "{\"network\":\"eip155:56\",\"tokenId\":\"native\"}]}", false, false, false},
    {"storeFailure", "application/json", "application/json",
     "{\"tokens\":[{\"network\":\"cronos\",\"tokenId\":\"native\"}]}", false, false, false},
    {"form", "application/json", "application/x-www-form-urlencoded", "tokens=bnb", false, false, false},
    {"unknownBearer", "application/json", "application/json", "{\"tokens\":[]}", false, true, false},
    {"revalidate", "application/json", "application/json",
     "{\"tokens\":[{\"network\":\"eth-mainnet\",\"tokenId\":\"erc20:0x1111111111111111111111111111111111111111\"},"
     "{\"network\":\"bnb\",\"tokenId\":\"native\"}]}", false, false, true},
};
constexpr size_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);
char g_etag[128] = {};

void kind_request(const Kind& kind, char letter, Bytes* out) {
  Bytes body;
  if (kind.hex) from_hex(kind.body, &body);
  else body.Text(kind.body);
  char token[64];
  snprintf(token, sizeof(token), "fintel_dev_");
  for (size_t j = 0; j < 43; ++j) token[11 + j] = kind.unknown_bearer ? '0' : letter;
  token[54] = '\0';
  char head[1024];
  snprintf(head, sizeof(head),
           "POST /v1/tokens/compare HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\naccept: %s\r\n"
           "authorization: Bearer %s\r\ncontent-type: %s\r\n%s%s%sContent-Length: %zu\r\n\r\n",
           kind.accept, token, kind.content_type, kind.revalidate ? "if-none-match: " : "",
           kind.revalidate ? g_etag : "", kind.revalidate ? "\r\n" : "", body.size);
  out->Text(head);
  out->Append(body.data ? body.data : "", body.size);
}

struct Baseline {
  int status = 0;
  Bytes body;
  char headers[11][256] = {};
};
Baseline g_baselines[kKindCount];

bool same_as_baseline(const Baseline& baseline, const Bytes& raw) {
  Parsed parsed;
  if (!parse_response(raw, &parsed) || parsed.status != baseline.status ||
      parsed.body_length != baseline.body.size ||
      (parsed.body_length && memcmp(parsed.body, baseline.body.data, parsed.body_length) != 0))
    return false;
  for (size_t h = 0; h < 11; ++h) {
    if (!strcmp(kHeaders[h], "x-rate-limit-remaining")) continue;
    const char* got = parsed.Header(kHeaders[h]);
    if (strcmp(got ? got : "", baseline.headers[h]) != 0) return false;
  }
  return true;
}

struct Client {
  size_t index;
  int requests = 0;
  int wrong = 0;
};

void* concurrency_client(void* opaque) {
  auto* client = static_cast<Client*>(opaque);
  // The six letters the fixture never uses: the in-process quota counters
  // of the replay's letters are still live in this minute.
  const char letter = "HUWXYZ"[client->index % 6];
  for (size_t step = 0; step < 40; ++step) {
    const size_t k = (client->index + step) % kKindCount;
    Bytes request;
    kind_request(kKinds[k], letter, &request);
    Bytes response;
    dispatch(*g_instance.router, request, &response);
    ++client->requests;
    if (!same_as_baseline(g_baselines[k], response)) ++client->wrong;
  }
  return nullptr;
}

void test_concurrency() {
  // At most three clients share a letter: 40 requests cost about 58 each.
  wait_for_fresh_minute(30000);
  g_now = static_cast<int64_t>(number(g_fixture, "now")) + 20000;
  for (size_t k = 0; k < kKindCount; ++k) {
    if (kKinds[k].revalidate) {
      Parsed parsed;
      Bytes raw;
      kind_request(kKinds[0], 'H', &raw);
      Bytes response;
      dispatch(*g_instance.router, raw, &response);
      if (parse_response(response, &parsed) && parsed.Header("etag"))
        snprintf(g_etag, sizeof(g_etag), "%s", parsed.Header("etag"));
    }
    Bytes request;
    kind_request(kKinds[k], 'H', &request);
    Bytes response;
    dispatch(*g_instance.router, request, &response);
    Parsed parsed;
    CHECK(parse_response(response, &parsed), "concurrency baseline parses");
    g_baselines[k].status = parsed.status;
    g_baselines[k].body.Append(parsed.body, parsed.body_length);
    for (size_t h = 0; h < 11; ++h) {
      const char* value = parsed.Header(kHeaders[h]);
      snprintf(g_baselines[k].headers[h], sizeof(g_baselines[k].headers[h]), "%s", value ? value : "");
    }
  }
  CHECK(g_baselines[0].status == 200 && g_baselines[1].status == 200 && g_baselines[2].status == 200,
        "concurrency baselines answer");
  // g_baselines[6] is the form kind: 415 (compat-decisions.md#form-bodies).
  CHECK(g_baselines[3].status == 400 && g_baselines[5].status == 500 && g_baselines[6].status == 415 &&
            g_baselines[7].status == 401 && g_baselines[8].status == 304,
        "concurrency baselines refuse");
  Client clients[16];
  pthread_t threads[16];
  for (size_t i = 0; i < 16; ++i) {
    clients[i].index = i;
    pthread_create(&threads[i], nullptr, concurrency_client, &clients[i]);
  }
  int requests = 0, wrong = 0;
  for (size_t i = 0; i < 16; ++i) {
    pthread_join(threads[i], nullptr);
    requests += clients[i].requests;
    wrong += clients[i].wrong;
  }
  ++checks;
  if (wrong) fail("concurrency", "an answer differs from its kind's baseline");
  printf("concurrency: %d requests from 16 threads over %zu kinds, %d wrong\n", requests, kKindCount, wrong);
}

// The last group's image reopened: the warm rows answer the warm case.
void test_restart_durability() {
  const tj::Value* cases = tj::Get(g_fixture, "cases");
  const tj::Value* warm = nullptr;
  for (uint32_t i = 0; tj::IsArray(cases) && i < cases->count; ++i)
    if (!strcmp(text(cases->items[i], "name"), "restartSame")) warm = cases->items[i];
  ++checks;
  if (!warm || !crash_restart(&g_instance, false)) {
    fail("restart", "cannot reopen the image");
    return;
  }
  wait_for_fresh_minute(5000);
  g_now = static_cast<int64_t>(number(warm, "at"));
  // Device Z is unused by the fixture: the body and status are compared.
  Bytes body;
  from_hex(text(warm, "requestHex"), &body);
  Bytes request;
  request.Text("POST /v1/tokens/compare HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\naccept: application/json\r\n"
               "authorization: Bearer fintel_dev_ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ\r\n"
               "content-type: application/json\r\n");
  char length[64];
  snprintf(length, sizeof(length), "Content-Length: %zu\r\n\r\n", body.size);
  request.Text(length);
  request.Append(body.data, body.size);
  Bytes response;
  dispatch(*g_instance.router, request, &response);
  Parsed parsed;
  Bytes want;
  expected_body(warm, &want);
  ++checks;
  if (!parse_response(response, &parsed) || parsed.status != 200 || parsed.body_length != want.size ||
      (want.size && memcmp(parsed.body, want.data, want.size) != 0))
    fail("restart", "the reopened image does not answer the warm comparison");
  else
    printf("restart: reopened image %d answers restartSame byte for byte (%zu bytes)\n", g_instance.generation,
           want.size);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s token_comparison_edges.json\n", argv[0]);
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
  if (!pin || pin_size < 64 || strncmp(pin, hex, 64) != 0 || strcmp(pin + 64, "  token_comparison_edges.json\n") != 0) {
    fprintf(stderr, "token_comparison_edges.json sha256 %s does not match its pin\n", hex);
    return 1;
  }
  free(pin);

  char root[] = "/tmp/fortis-compare-edges-unit-XXXXXX";
  if (!mkdtemp(root)) return 2;
  g_root = root;
  char auth[600];
  snprintf(auth, sizeof(auth), "%s/auth", root);
  mkdir(auth, 0700);
  fortis::storage::RuntimeKvSetDirectory(auth);

  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  g_fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), size);
  if (!tj::IsObject(g_fixture)) {
    fprintf(stderr, "cannot parse the fixture\n");
    return 2;
  }
  if (!seed_devices()) {
    fprintf(stderr, "cannot seed the fixture devices\n");
    return 2;
  }
  replay_oracle();
  test_restart_durability();
  test_concurrency();
  delete g_instance.router;
  delete g_instance.kv;
  tj::FreeArena(arena);
  free(data);
  fortis::test::RemoveTree(root);
  if (failures) {
    fprintf(stderr, "token_comparison_edges_test: %d failure(s) over %d checks\n", failures, checks);
    return 1;
  }
  printf("token_comparison_edges_test: %d checks passed\n", checks);
  g_root = nullptr;
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
