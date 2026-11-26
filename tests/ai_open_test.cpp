#include "test_support.h"
// Unit checks of POST /v1/ai/conversations (R38) below the HTTP layer: the
// store's open()/replay (src/ai/ai_store.cpp Store::Open) and the hourly open
// quota table (src/compat/hourly_windows.h), against the frozen TypeScript route
// fixture (tests/corpus/ai_conversations_route.json):
//  - every open of the fixture's main group, in order, through Store::Open
//    with the fixture's id counter: each 201 gives the TypeScript id,
//    message id, createdAt, expiresAt and (after SaveReply, standing in for
//    the provider) assistant id; each 401 is a proof failure that consumes
//    the same ids TypeScript consumed; the 503 open leaves exactly its user
//    turn; the retry reuses it (one user row, no second user turn);
//  - the six records of a fresh open (and nothing else) with the opening
//    expiry; replays and conflicts write nothing;
//  - every proof/proof2 401 of the fixture: refused, no id, no write; the
//    other-device proof opens for the device it was signed for;
//  - the timestamp window on one signed proof (+300000/−30000 ms accepted,
//    one millisecond further refused);
//  - storage faults are storage errors (HTTP 500), never PROOF_INVALID: a KV
//    write failure (FORTIS_KV_TEST_FAIL, both before and after the frame
//    body is on disk), a later read on the closed KV, and corrupt
//    conversation / first-message records found by a replay; absent
//    records stay PROOF_INVALID; a reopened KV is all-or-nothing and the
//    retried open succeeds;
//  - a hard restart (KV reopen) keeps the replay;
//  - the hourly windows: 20 allowed, the 21st over, fixed from the first
//    hit to first + 1 h exactly, per device and per scope, fail-open only
//    when the table is full of live keys.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ai_snapshot.h"
#include "ai_store.h"
#include "fake_seams.h"
#include "hourly_windows.h"
#include "kv_store.h"
#include "token_json.h"

namespace {

namespace tj = fortis::codec::tj;
namespace ai = fortis::ai;

int g_failures = 0;
void check(bool condition, const char* what) {
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s\n", what);
}
void checkf(bool condition, const char* format, const char* detail) {
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: ");
  fprintf(stderr, format, detail);
  fprintf(stderr, "\n");
}

int64_t g_now = 0;
unsigned long long g_next_id = 0;
fortis::testing::PointerClock g_clock(&g_now);
// The fixture's ulid mock: "01JZ" + a 22-digit counter.
struct CounterIds final : fortis::base::IdSource {
  bool NewId(char output[kIdBytes]) override {
    snprintf(output, 27, "01JZ%022llu", g_next_id++);
    return true;
  }
} g_ids;

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file);
    free(data);
    return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

// "YYYY-MM-DDTHH:MM:SS.mmmZ" → epoch ms.
bool parse_iso(const char* text, int64_t* ms) {
  struct tm t{};
  int millis = 0;
  if (!text || strlen(text) != 24 ||
      !fortis::test::ParseTimestampParts(text, &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min,
             &t.tm_sec, &millis))
    return false;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  *ms = static_cast<int64_t>(timegm(&t)) * 1000 + millis;
  return true;
}

off_t file_size(const char* directory) {
  char path[512];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  struct stat info{};
  return stat(path, &info) == 0 ? info.st_size : -1;
}

struct KeyCounter {
  size_t count = 0;
};
bool count_key(const char*, const void*, size_t, void* context) {
  ++static_cast<KeyCounter*>(context)->count;
  return true;
}
size_t key_count(fortis::storage::KvStore& kv, const char* prefix, int64_t now) {
  KeyCounter counter;
  if (!kv.Scan(prefix, nullptr, 4096, now, [&, visitor_state = &counter](const char* key, const void* value, size_t size) { return (count_key)(key, value, size, visitor_state); })) return static_cast<size_t>(-1);
  return counter.count;
}

// A KV value as text (empty when absent).
bool kv_text(fortis::storage::KvStore& kv, const char* key, int64_t now, char* out, size_t capacity) {
  size_t size = 0;
  if (kv.GetStatus(key, now, out, capacity - 1, &size) != fortis::storage::KvReadStatus::Found) {
    out[0] = '\0';
    return false;
  }
  out[size] = '\0';
  return true;
}

// ---- protobuf (the fixture's request and response wires) ----

bool read_varint(const uint8_t* data, size_t length, size_t* at, uint64_t* value) {
  *value = 0;
  for (int shift = 0; shift < 64 && *at < length; shift += 7) {
    const uint8_t byte = data[(*at)++];
    *value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return true;
  }
  return false;
}
uint8_t* from_hex(const char* hex, size_t* length) {
  const size_t n = strlen(hex) / 2;
  uint8_t* out = static_cast<uint8_t*>(malloc(n + 1));
  for (size_t i = 0; i < n; ++i) {
    unsigned byte = 0;
    if (!fortis::test::ParseHexByte(hex + 2 * i, &byte)) throw std::runtime_error("invalid hex in test fixture");
    out[i] = static_cast<uint8_t>(byte);
  }
  *length = n;
  return out;
}
// Length-delimited fields 1..count of one message level as NUL-terminated
// arena strings (submessages included as raw bytes).
bool wire_fields(const uint8_t* data, size_t length, tj::Arena& arena, const char** fields, size_t* lengths,
                 size_t count) {
  size_t at = 0;
  while (at < length) {
    uint64_t tag = 0, size = 0;
    if (!read_varint(data, length, &at, &tag) || (tag & 7) != 2 || !read_varint(data, length, &at, &size) ||
        size > length - at)
      return false;
    const uint64_t field = tag >> 3;
    if (field >= 1 && field <= count) {
      const char* value = arena.CopyText(reinterpret_cast<const char*>(data + at), size);
      if (!value) return false;
      fields[field - 1] = value;
      lengths[field - 1] = size;
    }
    at += size;
  }
  return true;
}

// ---- the fixture ----

struct Fixture {
  tj::Arena arena = tj::GrowingArena(512u << 20);
  const tj::Value* root = nullptr;
  const tj::Value* cases = nullptr;
  const char* secret = nullptr;
  const char* device_a = nullptr;
  const char* device_b = nullptr;
};
Fixture g_fixture;

const tj::Value* find_case(const char* name) {
  for (uint32_t i = 0; i < g_fixture.cases->count; ++i)
    if (tj::StringEquals(tj::Get(g_fixture.cases->items[i], "name"), name)) return g_fixture.cases->items[i];
  fprintf(stderr, "fixture case %s missing\n", name);
  exit(2);
}
int64_t at_of(const tj::Value* c) { return static_cast<int64_t>(tj::Get(c, "at")->number); }

const char* const kOpenNames[10] = {"address", "nonce", "timestamp", "snapshotHash", "accessToken",
                                    "accessTokenHash", "snapshotJson", "signature", "firstMessage",
                                    "clientMessageId"};

// The open request of a fixture case (JSON text or protobuf hex), with
// firstMessage zod-trimmed as the handler hands it to the store.
struct Request {
  const char* field[10]{};
  size_t length[10]{};
};
Request request_of(const tj::Value* c) {
  Request r;
  if (const tj::Value* hex = tj::Get(c, "requestHex")) {
    size_t length = 0;
    uint8_t* wire = from_hex(hex->text, &length);
    check(wire_fields(wire, length, g_fixture.arena, r.field, r.length, 10), "request wire decodes");
    free(wire);
  } else {
    const tj::Value* text = tj::Get(c, "requestText");
    const tj::Value* json = tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
    for (size_t i = 0; i < 10; ++i) {
      const tj::Value* v = tj::Get(json, kOpenNames[i]);
      r.field[i] = tj::IsString(v) ? v->text : "";
      r.length[i] = tj::IsString(v) ? v->length : 0;
    }
  }
  size_t begin = 0, end = 0;
  if (ai::ZodTrimmedBody(r.field[8], r.length[8], &begin, &end)) {
    r.field[8] = g_fixture.arena.CopyText(r.field[8] + begin, end - begin);
    r.length[8] = end - begin;
  }
  return r;
}
ai::OpenInput input_of(const Request& r, const char* device) {
  return ai::OpenInput{device, r.field[0], r.field[1], r.field[2], r.field[3], r.field[6], r.length[6],
                       r.field[4], r.field[5], r.field[7], r.field[8], r.length[8], r.field[9]};
}

// The 201 answer of a fixture case: id, expiresAt, message {id, body,
// createdAt}, assistant {id, body}.
struct Answer {
  const char* id = "";
  const char* expires_at = "";
  const char* message_id = "";
  const char* message_body = "";
  const char* message_created_at = "";
  const char* assistant_id = "";
  const char* assistant_body = "";
};
Answer answer_of(const tj::Value* c) {
  Answer a;
  if (const tj::Value* hex = tj::Get(c, "wireHex")) {
    size_t length = 0;
    uint8_t* wire = from_hex(hex->text, &length);
    const char* top[5] = {"", "", "", "", ""};
    size_t top_length[5]{};
    check(wire_fields(wire, length, g_fixture.arena, top, top_length, 5), "response wire decodes");
    const char* message[4] = {"", "", "", ""};
    size_t message_length[4]{};
    const char* assistant[4] = {"", "", "", ""};
    size_t assistant_length[4]{};
    check(wire_fields(reinterpret_cast<const uint8_t*>(top[3]), top_length[3], g_fixture.arena, message,
                      message_length, 4) &&
              wire_fields(reinterpret_cast<const uint8_t*>(top[4]), top_length[4], g_fixture.arena, assistant,
                          assistant_length, 4),
          "response submessages decode");
    a = {top[0], top[2], message[0], message[2], message[3], assistant[0], assistant[2]};
    free(wire);
    return a;
  }
  const tj::Value* body = tj::Get(c, "body");
  const tj::Value* message = tj::Get(body, "message");
  const tj::Value* assistant = tj::Get(body, "assistant");
  a = {tj::Get(body, "id")->text, tj::Get(body, "expiresAt")->text, tj::Get(message, "id")->text,
       tj::Get(message, "body")->text, tj::Get(message, "createdAt")->text, tj::Get(assistant, "id")->text,
       tj::Get(assistant, "body")->text};
  return a;
}

struct Env {
  char directory[64];
  fortis::storage::KvStore kv;
  ai::Store store;
  bool Begin() {
    snprintf(directory, sizeof(directory), "/tmp/fortis-ai-open-XXXXXX");
    return mkdtemp(directory) && kv.Open(directory) &&
           store.Start(&kv, g_fixture.secret, strlen(g_fixture.secret), &g_clock, &g_ids);
  }
};

size_t message_count(ai::Store& store, const char* id) {
  tj::Arena arena = tj::GrowingArena(64u << 20);
  ai::MessageView* rows = nullptr;
  size_t count = 0;
  const bool ok = store.Messages(id, arena, &rows, &count);
  tj::FreeArena(arena);
  return ok ? count : static_cast<size_t>(-1);
}

// ---- the main group's opens, in fixture order ----

void main_group_opens() {
  Env env;
  check(env.Begin(), "main: store starts");
  g_next_id = 0;  // the main group's first id is 01JZ…00
  const char* names[] = {"openJson", "openReplaySameNonce", "openReplayProtobuf", "openReplayOtherClientId",
                         "openReplayOtherToken", "openReplayOtherSnapshot", "openTokenReuse",
                         "openReplayStaleTimestamp", "openLowercaseAddress", "openV0", "openV35", "openFutureEdge",
                         "openStaleEdge", "openOffsetTimestamp", "openMinuteTimestamp", "openProtobuf",
                         "openProviderStatus500", "openProviderRetry"};
  char main_id[27] = "";
  char failed_id[27] = "", failed_message[27] = "";
  for (const char* name : names) {
    const tj::Value* c = find_case(name);
    g_now = at_of(c);
    const int status = static_cast<int>(tj::Get(c, "status")->number);
    const Request request = request_of(c);
    ai::Opened opened;
    bool storage_error = false;
    const off_t before = file_size(env.directory);
    const unsigned long long ids_before = g_next_id;
    const bool ok = env.store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error);
    checkf(!storage_error, "%s: no storage error", name);
    if (status == 401) {
      checkf(!ok, "%s: PROOF_INVALID", name);
      checkf(file_size(env.directory) == before, "%s: a refused open writes nothing", name);
      // TypeScript draws both ids before the nonce check, so a proof that
      // verifies consumes two even when the replay/token check refuses it;
      // a proof that fails verification consumes none.
      const bool verified = strcmp(name, "openReplayStaleTimestamp") != 0;
      checkf(g_next_id - ids_before == (verified ? 2u : 0u), "%s: TypeScript's id consumption", name);
      continue;
    }
    checkf(ok, "%s: opens", name);
    if (!ok) continue;
    int64_t created = 0;
    ai::MessageView user;
    tj::Arena arena = tj::GrowingArena(64u << 20);
    checkf(env.store.LoadMessage(opened.id, opened.message_id, arena, &user), "%s: first turn stored", name);
    char reply_id[27] = "";
    const bool replied = env.store.FindReply(opened.id, opened.message_id, reply_id);
    if (status == 503) {
      // The provider failed after the open: only the user turn exists.
      checkf(!replied && message_count(env.store, opened.id) == 1, "%s: exactly the user turn saved", name);
      memcpy(failed_id, opened.id, 27);
      memcpy(failed_message, opened.message_id, 27);
      tj::FreeArena(arena);
      continue;
    }
    const Answer answer = answer_of(c);
    checkf(strcmp(opened.id, answer.id) == 0, "%s: TypeScript conversation id", name);
    checkf(strcmp(opened.message_id, answer.message_id) == 0, "%s: TypeScript first message id", name);
    int64_t expires = 0;
    checkf(parse_iso(answer.expires_at, &expires) && opened.expires_at_ms == expires, "%s: TypeScript expiresAt",
           name);
    checkf(user.body_length == strlen(answer.message_body) && memcmp(user.body, answer.message_body,
                                                                     user.body_length) == 0,
           "%s: TypeScript first message body (zod-trimmed, redacted)", name);
    checkf(parse_iso(answer.message_created_at, &created) && user.created_at_ms == created &&
               strcmp(user.role, "user") == 0,
           "%s: TypeScript createdAt and role", name);
    if (!replied) {
      // Stand-in for the provider: the assistant row the reply persists.
      bool fresh = false;
      checkf(env.store.SaveReply(opened.id, opened.message_id, answer.assistant_body, strlen(answer.assistant_body),
                                 reply_id, &fresh) && fresh,
             "%s: reply saved", name);
    }
    checkf(strcmp(reply_id, answer.assistant_id) == 0, "%s: TypeScript assistant id", name);
    if (!strcmp(name, "openJson")) {
      memcpy(main_id, opened.id, 27);
      // A fresh open is exactly these six records, all expiring at opening + 7 days.
      char key[256], value[512], hash[65];
      check(key_count(env.kv, "ai/", g_now) == 8, "openJson: the six open records + the reply's two");
      snprintf(key, sizeof(key), "ai/conversation/%s", opened.id);
      check(kv_text(env.kv, key, g_now, value, sizeof(value)), "openJson: conversation record");
      char expected[512];
      ai::Sha256Hex(request.field[1], request.length[1], hash);
      snprintf(expected, sizeof(expected),
               "\"deviceId\":\"%s\",\"proofNonceHash\":\"%s\",\"accessTokenHash\":\"%s\",\"snapshotHash\":\"%s\","
               "\"createdAt\":%lld,\"updatedAt\":%lld,\"expiresAt\":%lld}",
               g_fixture.device_a, hash, request.field[5], request.field[3], static_cast<long long>(g_now),
               static_cast<long long>(g_now), static_cast<long long>(expires));
      check(strstr(value, expected) != nullptr, "openJson: conversation record fields");
      snprintf(key, sizeof(key), "ai/nonce/%s", hash);
      check(kv_text(env.kv, key, g_now, value, sizeof(value)) && !strcmp(value, opened.id), "openJson: nonce index");
      snprintf(key, sizeof(key), "ai/token/%s", request.field[5]);
      check(kv_text(env.kv, key, g_now, value, sizeof(value)) && !strcmp(value, opened.id), "openJson: token index");
      snprintf(key, sizeof(key), "ai/client/%s/%s", opened.id, request.field[9]);
      check(kv_text(env.kv, key, g_now, value, sizeof(value)) && !strcmp(value, opened.message_id),
            "openJson: client index");
      snprintf(key, sizeof(key), "ai/snapshot/%s/%s", opened.id, request.field[3]);
      size_t snapshot_size = 0;
      const fortis::storage::KvReadStatus snapshot_status = env.kv.GetStatus(key, g_now, value, 0, &snapshot_size);
      check((snapshot_status == fortis::storage::KvReadStatus::Found ||
             snapshot_status == fortis::storage::KvReadStatus::BufferTooSmall) && snapshot_size > 2,
            "openJson: snapshot record");
      snprintf(key, sizeof(key), "ai/message/%s/%s", opened.id, opened.message_id);
      check(kv_text(env.kv, key, expires - 1, value, sizeof(value)) && !kv_text(env.kv, key, expires, value,
                                                                                 sizeof(value)),
            "openJson: records live until opening + 7 days exactly");
      check(message_count(env.store, opened.id) == 2, "openJson: user + assistant");
    }
    if (!strcmp(name, "openReplaySameNonce") || !strcmp(name, "openReplayProtobuf")) {
      checkf(file_size(env.directory) == before && g_next_id - ids_before == 2, "%s: replay writes nothing", name);
      checkf(message_count(env.store, main_id) == 2, "%s: no second user turn", name);
    }
    if (!strcmp(name, "openProviderRetry")) {
      checkf(!strcmp(opened.id, failed_id) && !strcmp(opened.message_id, failed_message),
             "%s: the retry reuses the saved turn", name);
      checkf(message_count(env.store, opened.id) == 2, "%s: one user + one assistant row", name);
    }
    tj::FreeArena(arena);
  }
  // A replay with another firstMessage answers the stored first turn.
  const tj::Value* c = find_case("openJson");
  Request request = request_of(c);
  request.field[8] = "A different question";
  request.length[8] = strlen(request.field[8]);
  g_now = at_of(c) + 1000;
  ai::Opened opened;
  bool storage_error = false;
  const off_t before = file_size(env.directory);
  check(env.store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error) &&
            !strcmp(opened.id, main_id) && file_size(env.directory) == before,
        "replay with another firstMessage: same conversation, nothing written");
  tj::Arena arena = tj::GrowingArena(1u << 20);
  ai::MessageView user;
  check(env.store.LoadMessage(opened.id, opened.message_id, arena, &user) &&
            user.body_length == strlen(answer_of(c).message_body),
        "replay with another firstMessage: the original body");
  tj::FreeArena(arena);
  // Hard restart: a fresh KV and store over the same directory keep the replay.
  env.store.Stop();
  env.kv.Close();
  fortis::storage::KvStore kv;
  ai::Store store;
  check(kv.Open(env.directory) && store.Start(&kv, g_fixture.secret, strlen(g_fixture.secret), &g_clock, &g_ids),
        "restart: reopens");
  ai::Opened again;
  check(store.Open(input_of(request_of(c), g_fixture.device_a), &again, &storage_error) && !strcmp(again.id, main_id) &&
            !strcmp(again.message_id, opened.message_id) && again.expires_at_ms == opened.expires_at_ms,
        "restart: the replay answers the same conversation");
  fprintf(stdout, "main group: %zu opens match TypeScript ids, bodies, expiry and id consumption\n",
          sizeof(names) / sizeof(names[0]));
}

// ---- every fixture proof failure, and the device binding ----

void proof_failures() {
  Env env;
  check(env.Begin(), "proof: store starts");
  size_t refused = 0;
  for (uint32_t i = 0; i < g_fixture.cases->count; ++i) {
    const tj::Value* c = g_fixture.cases->items[i];
    const tj::Value* group = tj::Get(c, "group");
    if (!tj::StringEquals(group, "proof") && !tj::StringEquals(group, "proof2")) continue;
    g_now = at_of(c);
    const unsigned long long ids = g_next_id;
    const off_t before = file_size(env.directory);
    ai::Opened opened;
    bool storage_error = false;
    const bool ok = env.store.Open(input_of(request_of(c), g_fixture.device_a), &opened, &storage_error);
    const char* name = tj::Get(c, "name")->text;
    checkf(!ok && !storage_error && g_next_id == ids && file_size(env.directory) == before,
           "%s: refused before any id or write", name);
    ++refused;
  }
  // proofOtherDevice was signed for device B: it opens for B only.
  const tj::Value* c = find_case("proofOtherDevice");
  g_now = at_of(c);
  ai::Opened opened;
  bool storage_error = false;
  check(env.store.Open(input_of(request_of(c), g_fixture.device_b), &opened, &storage_error),
        "proofOtherDevice: opens for the device it was signed for");
  check(!env.store.Open(input_of(request_of(c), g_fixture.device_a), &opened, &storage_error) && !storage_error,
        "proofOtherDevice: still refused for device A after B opened");
  fprintf(stdout, "proof: %zu fixture proof failures refused before any id or write\n", refused);
}

// ---- the timestamp window on one proof ----

void timestamp_window() {
  const tj::Value* c = find_case("openJson");
  const Request request = request_of(c);
  int64_t signed_at = 0;
  check(parse_iso(request.field[2], &signed_at), "openJson timestamp");
  struct { int64_t offset; bool accepted; } window[] = {
      {300000, true}, {300001, false}, {-30000, true}, {-30001, false}, {0, true}};
  for (const auto& probe : window) {
    Env env;
    check(env.Begin(), "window: store starts");
    g_now = signed_at + probe.offset;
    ai::Opened opened;
    bool storage_error = false;
    const bool ok = env.store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error);
    char label[96];
    snprintf(label, sizeof(label), "age %lld ms %s", static_cast<long long>(probe.offset),
             probe.accepted ? "accepted" : "refused");
    check(ok == probe.accepted && !storage_error, label);
  }
}

// ---- storage faults ----

void storage_faults() {
  const tj::Value* c = find_case("openJson");
  const Request request = request_of(c);
  const char* stages[] = {"write_after_header", "write_before_fsync"};
  for (const char* stage : stages) {
    Env env;
    check(env.Begin(), "fault: store starts");
    g_now = at_of(c);
    ai::Opened opened;
    bool storage_error = false;
    setenv("FORTIS_KV_TEST_FAIL", stage, 1);
    const bool ok = env.store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error);
    unsetenv("FORTIS_KV_TEST_FAIL");
    checkf(!ok && storage_error, "%s: a failed commit is a storage error (500), not PROOF_INVALID", stage);
    // The KV closed itself after the uncertain commit: the next open reads
    // nothing and must still not claim the proof is invalid or open anew.
    storage_error = false;
    checkf(!env.store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error) && storage_error,
           "%s: a read on the closed KV is a storage error", stage);
    env.store.Stop();
    env.kv.Close();
    // Reopened: the frame is all or nothing; the retried open succeeds.
    fortis::storage::KvStore kv;
    ai::Store store;
    check(kv.Open(env.directory) && store.Start(&kv, g_fixture.secret, strlen(g_fixture.secret), &g_clock,
                                                &g_ids),
          "fault: reopens");
    const size_t records = key_count(kv, "ai/", g_now);
    checkf(records == 0 || records == 6, "%s: all six records or none after reopen", stage);
    storage_error = false;
    checkf(store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error) && !storage_error &&
               message_count(store, opened.id) == 1 && key_count(kv, "ai/", g_now) == 6,
           "%s: the retried open succeeds with one first turn", stage);
  }

  // Corrupt or missing records reached by a replay.
  Env env;
  check(env.Begin(), "corrupt: store starts");
  g_now = at_of(c);
  ai::Opened opened;
  bool storage_error = false;
  check(env.store.Open(input_of(request, g_fixture.device_a), &opened, &storage_error), "corrupt: opens");
  char conversation_key[128], message_key[128], client_key[128];
  snprintf(conversation_key, sizeof(conversation_key), "ai/conversation/%s", opened.id);
  snprintf(message_key, sizeof(message_key), "ai/message/%s/%s", opened.id, opened.message_id);
  snprintf(client_key, sizeof(client_key), "ai/client/%s/%s", opened.id, request.field[9]);
  char saved_conversation[1024], saved_message[1024];
  check(kv_text(env.kv, conversation_key, g_now, saved_conversation, sizeof(saved_conversation)) &&
            kv_text(env.kv, message_key, g_now, saved_message, sizeof(saved_message)),
        "corrupt: records read");
  const int64_t expiry = opened.expires_at_ms;
  auto put = [&](const char* key, const char* value) {
    const fortis::storage::KvMutation write{key, value, static_cast<uint32_t>(strlen(value)), expiry, false};
    return env.kv.Write(&write, 1);
  };
  auto replay = [&](bool* error) {
    ai::Opened again;
    *error = false;
    return env.store.Open(input_of(request, g_fixture.device_a), &again, error);
  };
  bool error = false;
  check(put(conversation_key, "{\"ownerId\":") && !replay(&error) && error,
        "corrupt conversation record: storage error (TypeScript's decode throws: 500)");
  check(put(conversation_key, saved_conversation) && replay(&error) && !error, "conversation restored: replay");
  check(put(message_key, "not json") && !replay(&error) && error, "corrupt first message: storage error");
  check(put(message_key, saved_message) && put(client_key, "01JZ9999999999999999999999") && !replay(&error) && !error,
        "client index naming an absent message: PROOF_INVALID");
  check(put(client_key, "short") && !replay(&error) && !error, "malformed client index: PROOF_INVALID");
  check(put(client_key, opened.message_id) && replay(&error) && !error, "client index restored: replay");
  const fortis::storage::KvMutation erase{conversation_key, nullptr, 0, 0, true};
  check(env.kv.Write(&erase, 1) && !replay(&error) && !error, "absent conversation: PROOF_INVALID");
}

// ---- the hourly open quota (kv.incrWindow(`q:ai:open:${device}`, 1 h) > 20) ----

fortis::compat::HourlyWindows<32> g_windows;
fortis::compat::HourlyWindows<32> g_full;

void hourly_windows() {
  const int64_t hour = 60 * 60 * 1000;
  const int64_t first = 1767323045000;
  uint64_t count = 0;
  bool ok = true;
  for (int i = 0; i < 20; ++i) ok &= g_windows.Increment('o', g_fixture.device_a, first + i, hour, &count);
  check(ok && count == 20, "20 opens within the hour are allowed (count 20)");
  check(g_windows.Increment('o', g_fixture.device_a, first + hour - 1, hour, &count) && count == 21,
        "the 21st in the window is over (count 21 > 20)");
  check(g_windows.Increment('o', g_fixture.device_b, first + 30, hour, &count) && count == 1,
        "another device has its own window");
  check(g_windows.Increment('m', g_fixture.device_a, first + 30, hour, &count) && count == 1,
        "the message scope does not share the open window");
  check(g_windows.Increment('o', g_fixture.device_a, first + hour, hour, &count) && count == 1,
        "the window is fixed from its first hit: a new one at first + 1 h exactly");
  // A table full of live keys fails open for a new key (MemoryKv eviction);
  // an expired entry is reused.
  char id[32];
  size_t stored = 0;
  for (size_t i = 0; i < fortis::compat::HourlyWindows<32>::kCapacity; ++i) {
    snprintf(id, sizeof(id), "device-%zu", i);
    stored += g_full.Increment('o', id, first, hour, &count) ? 1 : 0;
  }
  check(stored == fortis::compat::HourlyWindows<32>::kCapacity, "the table holds its capacity");
  check(!g_full.Increment('o', "one-more", first + 1, hour, &count), "a full table of live keys fails open");
  check(g_full.Increment('o', "one-more", first + hour, hour, &count) && count == 1,
        "an expired entry is reused for a new key");
  check(g_full.Increment('o', "device-7", first + hour + 1, hour, &count) && count == 1,
        "an expired key starts a new window");
}

}  // namespace

int main(int argc, char** argv) try {
  const char* fixture_path = argc > 1 ? argv[1] : "tests/corpus/ai_conversations_route.json";
  size_t fixture_length = 0;
  char* fixture_text = read_file(fixture_path, &fixture_length);
  if (!fixture_text) {
    fprintf(stderr, "cannot read %s\n", fixture_path);
    return 2;
  }
  g_fixture.root = tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(fixture_text), fixture_length);
  if (!g_fixture.root) return 2;
  g_fixture.cases = tj::Get(g_fixture.root, "cases");
  g_fixture.secret = tj::Get(tj::Get(g_fixture.root, "env"), "SUPPORT_AUTH_SECRET")->text;
  g_fixture.device_a = tj::Get(tj::Get(g_fixture.root, "devices"), "A")->text;
  g_fixture.device_b = tj::Get(tj::Get(g_fixture.root, "devices"), "B")->text;

  main_group_opens();
  proof_failures();
  timestamp_window();
  storage_faults();
  hourly_windows();
  free(fixture_text);
  if (g_failures) {
    fprintf(stderr, "ai_open_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("ai_open_test: store open/replay/conflicts/window/faults/restart and hourly quota OK\n");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
