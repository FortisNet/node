#include "test_support.h"
#include <algorithm>
// Unit checks of POST /v1/ai/conversations/:id/messages (R39) below the HTTP
// layer: the store's addMessage()/replyNow() steps (src/ai/ai_store.cpp
// Store::AddMessage, FindReply, LoadReplyContext, SaveReply) and the model
// request they feed (ai_reply BuildOpenAiRequest), against the frozen
// TypeScript route fixture (tests/corpus/ai_conversations_route.json):
//  - every message POST of the fixture that reaches the store, in order,
//    from postJsonSavedContext on, with the fixture's id counter and clock:
//    each 201 gives the TypeScript message id, stored body (trimmed,
//    redacted), createdAt, clientMessageId and portfolioSnapshot, the exact
//    OpenAI request body TypeScript sent, and the assistant id/createdAt
//    after SaveReply; each 503 saves exactly one user turn (a retry with the
//    same clientMessageId draws no id and the retry's reply is the only
//    assistant turn); each store-level SNAPSHOT_INVALID draws no id; the
//    concurrent pair adds one row; hard restarts (KV reopen) between the
//    fixture groups keep repeat answers; the seven-day expiry is fixed;
//  - the records a message writes (2, or 3 with a new snapshot plus the
//    rewritten conversation), the conversation snapshot update and its
//    unchanged createdAt/expiresAt, idempotent repeats (same or changed
//    body, a valid new snapshot: no write, the old snapshot stays; an
//    invalid one: SNAPSHOT_INVALID);
//  - snapshot validation and ownership (hash, JSON, BOM, schema, links,
//    watch-only/non-"evm"/foreign owner, owner by case-insensitive address
//    in any position);
//  - storage faults are storage errors (HTTP 500), never SNAPSHOT_INVALID:
//    KV write failures before/after the frame body, reads on the closed KV,
//    corrupt conversation/snapshot/message records and a missing snapshot
//    record; an index naming an absent message and an absent conversation
//    stay SNAPSHOT_INVALID; a reopened KV is all-or-nothing;
//  - concurrent submissions of one clientMessageId add one row, distinct ids
//    add one row each;
//  - the hourly message window: 120 allowed, the 121st over, per device.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ai_reply.h"
#include "ai_service.h"
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
// The fixture's ulid mock: "01JZ" + a 22-digit counter (called under the
// store mutex).
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

bool count_key(const char*, const void*, size_t, void* context) {
  ++*static_cast<size_t*>(context);
  return true;
}
size_t key_count(fortis::storage::KvStore& kv, const char* prefix, int64_t now) {
  size_t count = 0;
  if (!kv.Scan(prefix, nullptr, 100000, now, [&, visitor_state = &count](const char* key, const void* value, size_t size) { return (count_key)(key, value, size, visitor_state); })) return static_cast<size_t>(-1);
  return count;
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
// arena strings (submessages included as raw bytes); `present` marks the
// fields the wire carried.
bool wire_fields(const uint8_t* data, size_t length, tj::Arena& arena, const char** fields, size_t* lengths,
                 bool* present, size_t count) {
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
      if (present) present[field - 1] = true;
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
  const char* model = nullptr;
  const char* device_a = nullptr;
  const char* device_b = nullptr;
  const char* token_b = nullptr;
};
Fixture g_fixture;

const tj::Value* find_case(const char* name) {
  for (uint32_t i = 0; i < g_fixture.cases->count; ++i)
    if (tj::StringEquals(tj::Get(g_fixture.cases->items[i], "name"), name)) return g_fixture.cases->items[i];
  fprintf(stderr, "fixture case %s missing\n", name);
  exit(2);
}
int64_t at_of(const tj::Value* c) { return static_cast<int64_t>(tj::Get(c, "at")->number); }
const char* text_of(const tj::Value* v) { return tj::IsString(v) ? v->text : ""; }

const char* const kOpenNames[10] = {"address", "nonce", "timestamp", "snapshotHash", "accessToken",
                                    "accessTokenHash", "snapshotJson", "signature", "firstMessage",
                                    "clientMessageId"};

// An open request (JSON text) with firstMessage zod-trimmed.
struct OpenRequest {
  const char* field[10]{};
  size_t length[10]{};
};
OpenRequest open_request_of(const tj::Value* c) {
  OpenRequest r;
  const tj::Value* text = tj::Get(c, "requestText");
  const tj::Value* json = tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
  for (size_t i = 0; i < 10; ++i) {
    const tj::Value* v = tj::Get(json, kOpenNames[i]);
    r.field[i] = tj::IsString(v) ? v->text : "";
    r.length[i] = tj::IsString(v) ? v->length : 0;
  }
  size_t begin = 0, end = 0;
  if (ai::ZodTrimmedBody(r.field[8], r.length[8], &begin, &end)) {
    r.field[8] = g_fixture.arena.CopyText(r.field[8] + begin, end - begin);
    r.length[8] = end - begin;
  }
  return r;
}
ai::OpenInput open_input(const OpenRequest& r, const char* device) {
  return ai::OpenInput{device, r.field[0], r.field[1], r.field[2], r.field[3], r.field[6], r.length[6],
                       r.field[4], r.field[5], r.field[7], r.field[8], r.length[8], r.field[9]};
}

// A message request (JSON text or protobuf hex) as the handler hands it to
// the store: body zod-trimmed, the snapshot pair with its presence.
struct MessageRequest {
  const char* client = "";
  const char* body = "";
  size_t body_length = 0;
  const char* snapshot = nullptr;
  size_t snapshot_length = 0;
  const char* hash = nullptr;
};
MessageRequest message_request_of(const tj::Value* item) {
  MessageRequest r;
  const char* field[4]{};
  size_t length[4]{};
  bool present[4]{};
  if (const tj::Value* hex = tj::Get(item, "requestHex")) {
    size_t n = 0;
    uint8_t* wire = from_hex(hex->text, &n);
    check(wire_fields(wire, n, g_fixture.arena, field, length, present, 4), "message request wire decodes");
    free(wire);
  } else {
    const tj::Value* text = tj::Get(item, "requestText");
    const tj::Value* json = tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
    const char* names[4] = {"clientMessageId", "body", "snapshotJson", "snapshotHash"};
    for (size_t i = 0; i < 4; ++i) {
      const tj::Value* v = tj::Get(json, names[i]);
      present[i] = tj::IsString(v);
      field[i] = present[i] ? v->text : "";
      length[i] = present[i] ? v->length : 0;
    }
  }
  r.client = field[0] ? field[0] : "";
  size_t begin = 0, end = 0;
  if (field[1] && ai::ZodTrimmedBody(field[1], length[1], &begin, &end)) {
    r.body = g_fixture.arena.CopyText(field[1] + begin, end - begin);
    r.body_length = end - begin;
  }
  if (present[2]) {
    r.snapshot = field[2];
    r.snapshot_length = length[2];
  }
  if (present[3]) r.hash = field[3];
  return r;
}

// The 201 answer of a message POST: message {id, body, createdAt,
// clientMessageId?, portfolioSnapshot?}, assistant {id, body, createdAt}.
struct MessageAnswer {
  const char* message_id = "";
  const char* message_body = "";
  size_t message_body_length = 0;
  const char* message_created_at = "";
  const char* client = nullptr;           // JSON only
  const tj::Value* snapshot = nullptr;    // JSON only
  const char* assistant_id = "";
  const char* assistant_body = "";
  size_t assistant_body_length = 0;
  const char* assistant_created_at = "";
};
MessageAnswer message_answer_of(const tj::Value* item) {
  MessageAnswer a;
  if (const tj::Value* hex = tj::Get(item, "wireHex")) {
    size_t n = 0;
    uint8_t* wire = from_hex(hex->text, &n);
    const char* top[2] = {"", ""};
    size_t top_length[2]{};
    const char* m[4] = {"", "", "", ""};
    size_t ml[4]{};
    const char* s[4] = {"", "", "", ""};
    size_t sl[4]{};
    check(wire_fields(wire, n, g_fixture.arena, top, top_length, nullptr, 2) &&
              wire_fields(reinterpret_cast<const uint8_t*>(top[0]), top_length[0], g_fixture.arena, m, ml, nullptr,
                          4) &&
              wire_fields(reinterpret_cast<const uint8_t*>(top[1]), top_length[1], g_fixture.arena, s, sl, nullptr, 4),
          "message response wire decodes");
    free(wire);
    a.message_id = m[0];
    a.message_body = m[2];
    a.message_body_length = ml[2];
    a.message_created_at = m[3];
    a.assistant_id = s[0];
    a.assistant_body = s[2];
    a.assistant_body_length = sl[2];
    a.assistant_created_at = s[3];
    return a;
  }
  const tj::Value* body = tj::Get(item, "body");
  const tj::Value* message = tj::Get(body, "message");
  const tj::Value* assistant = tj::Get(body, "assistant");
  a.message_id = text_of(tj::Get(message, "id"));
  a.message_body = text_of(tj::Get(message, "body"));
  a.message_body_length = tj::Get(message, "body")->length;
  a.message_created_at = text_of(tj::Get(message, "createdAt"));
  a.client = text_of(tj::Get(message, "clientMessageId"));
  a.snapshot = tj::Get(message, "portfolioSnapshot");
  a.assistant_id = text_of(tj::Get(assistant, "id"));
  a.assistant_body = text_of(tj::Get(assistant, "body"));
  a.assistant_body_length = tj::Get(assistant, "body")->length;
  a.assistant_created_at = text_of(tj::Get(assistant, "createdAt"));
  return a;
}

const char* header_of(const tj::Value* item, const char* name) {
  return text_of(tj::Get(tj::Get(item, "requestHeaders"), name));
}
const char* device_of(const tj::Value* item) {
  const char* authorization = header_of(item, "authorization");
  return strstr(authorization, g_fixture.token_b) ? g_fixture.device_b : g_fixture.device_a;
}

struct Env {
  char directory[64];
  fortis::storage::KvStore kv;
  ai::Store store;
  bool Begin() {
    snprintf(directory, sizeof(directory), "/tmp/fortis-ai-post-XXXXXX");
    return mkdtemp(directory) && kv.Open(directory) && Restart(false);
  }
  // A hard restart: the store and the KV are reopened from disk.
  bool Restart(bool reopen = true) {
    if (reopen) {
      store.Stop();
      kv.Close();
      if (!kv.Open(directory)) return false;
    }
    return store.Start(&kv, g_fixture.secret, strlen(g_fixture.secret), &g_clock, &g_ids);
  }
};

ai::AddStatus add(Env& env, const char* id, const MessageRequest& r, char message_id[ai::kIdBytes]) {
  return env.store.AddMessage(id, r.client, r.body, r.body_length, r.snapshot, r.snapshot_length, r.hash, message_id);
}

// Stringified JSON of a fixture value (the expected portfolioSnapshot).
bool same_json(const tj::Value* value, const char* text, size_t length) {
  tj::OutBuffer counter{};
  if (!tj::Stringify(value, counter) || !counter.ok || counter.used != length) return false;
  char* out = static_cast<char*>(malloc(length + 1));
  tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(out), length + 1};
  const bool ok = tj::Stringify(value, buffer) && buffer.ok && buffer.used == length && !memcmp(out, text, length);
  free(out);
  return ok;
}

// replyNow()'s model request for `user_message`, compared with the body
// TypeScript sent for the case.
bool request_matches(Env& env, const char* id, const char* user_message, const tj::Value* provider_call) {
  tj::Arena arena = tj::GrowingArena(64u << 20);
  const tj::Value* snapshot = nullptr;
  ai::Turn* turns = nullptr;
  size_t count = 0;
  const uint8_t* body = nullptr;
  size_t length = 0;
  const tj::Value* expected = tj::Get(provider_call, "body");
  const bool ok = env.store.LoadReplyContext(id, user_message, arena, &snapshot, &turns, &count) &&
                  ai::BuildOpenAiRequest(g_fixture.model, strlen(g_fixture.model), snapshot, turns, count, arena,
                                         &body, &length) &&
                  tj::IsString(expected) && expected->length == length && !memcmp(expected->text, body, length);
  tj::FreeArena(arena);
  return ok;
}

size_t message_count(ai::Store& store, const char* id) {
  tj::Arena arena = tj::GrowingArena(64u << 20);
  ai::MessageView* rows = nullptr;
  size_t count = 0;
  const bool ok = store.Messages(id, arena, &rows, &count);
  tj::FreeArena(arena);
  return ok ? count : static_cast<size_t>(-1);
}

// ---- the fixture's message POSTs, in order ----

// A 201/503 message turn of the fixture: the store adds (or finds) the user
// row; a 201 then finds or saves the assistant reply.
void fixture_turn(Env& env, const char* name, const char* id, const tj::Value* item, const tj::Value* provider,
                  bool answered, bool* reply_saved) {
  const MessageRequest r = message_request_of(item);
  char message_id[ai::kIdBytes]{};
  const unsigned long long ids_before = g_next_id;
  const ai::AddStatus status = add(env, id, r, message_id);
  checkf(status == ai::AddStatus::kAdded, "%s: AddMessage adds or finds the user row", name);
  char reply_id[ai::kIdBytes]{};
  const bool replied = env.store.FindReply(id, message_id, reply_id);
  if (!answered) {
    // 503: the user turn is saved; nothing answers it.
    checkf(!replied, "%s: a 503 leaves no assistant turn", name);
    if (provider && provider->count)
      checkf(request_matches(env, id, message_id, provider->items[provider->count - 1]),
             "%s: the OpenAI request body equals TypeScript's", name);
    return;
  }
  const MessageAnswer a = message_answer_of(item);
  checkf(!strcmp(message_id, a.message_id), "%s: TypeScript's message id", name);
  tj::Arena arena = tj::GrowingArena(64u << 20);
  ai::MessageView row;
  const char* snapshot = nullptr;
  size_t snapshot_length = 0;
  checkf(env.store.LoadMessage(id, message_id, arena, &row, &snapshot, &snapshot_length) &&
             !strcmp(row.role, "user") && row.body_length == a.message_body_length &&
             !memcmp(row.body, a.message_body, row.body_length),
         "%s: stored body (trimmed, redacted) equals TypeScript's", name);
  int64_t created = 0;
  checkf(parse_iso(a.message_created_at, &created) && row.created_at_ms == created, "%s: message createdAt", name);
  if (a.client) checkf(!strcmp(row.client_message_id, a.client), "%s: clientMessageId", name);
  if (a.snapshot)
    checkf(snapshot && same_json(a.snapshot, snapshot, snapshot_length),
           "%s: portfolioSnapshot equals TypeScript's zod output", name);
  tj::FreeArena(arena);
  if (replied) {
    // A repeat: the earlier reply answers, nothing is drawn.
    checkf(!strcmp(reply_id, a.assistant_id) && g_next_id == ids_before, "%s: the saved reply answers", name);
    return;
  }
  checkf(provider && provider->count && request_matches(env, id, message_id, provider->items[provider->count - 1]),
         "%s: the OpenAI request body equals TypeScript's", name);
  bool created_reply = false;
  checkf(env.store.SaveReply(id, message_id, a.assistant_body, a.assistant_body_length, reply_id, &created_reply) &&
             created_reply && !strcmp(reply_id, a.assistant_id),
         "%s: SaveReply gives TypeScript's assistant id", name);
  arena = tj::GrowingArena(1u << 20);
  ai::MessageView reply;
  checkf(env.store.LoadMessage(id, reply_id, arena, &reply) && parse_iso(a.assistant_created_at, &created) &&
             reply.created_at_ms == created && !strcmp(reply.role, "assistant"),
         "%s: assistant createdAt", name);
  tj::FreeArena(arena);
  *reply_saved = true;
}

void fixture_posts() {
  Env env;
  check(env.Begin(), "fixture: store starts");
  // The conversation every fixture POST uses: openJson (ids 0..2). The
  // other main-group opens drew ids 3..43 (R38 scope); the counter resumes
  // at the first message id TypeScript drew.
  const tj::Value* opening = find_case("openJson");
  g_now = at_of(opening);
  g_next_id = 0;
  ai::Opened opened;
  bool storage_error = false;
  check(env.store.Open(open_input(open_request_of(opening), g_fixture.device_a), &opened, &storage_error),
        "fixture: openJson opens");
  char reply_id[ai::kIdBytes];
  bool created = false;
  const tj::Value* open_assistant = tj::Get(tj::Get(opening, "body"), "assistant");
  check(env.store.SaveReply(opened.id, opened.message_id, text_of(tj::Get(open_assistant, "body")),
                            tj::Get(open_assistant, "body")->length, reply_id, &created),
        "fixture: openJson reply saved");
  const char* conversation = opened.id;
  check(!strcmp(conversation, "01JZ0000000000000000000000"), "fixture: the conversation id");
  int64_t created_at = 0, expires_at = 0;
  check(env.store.ConversationTimes(conversation, &created_at, &expires_at) &&
            expires_at == created_at + ai::kConversationLifetimeMs,
        "fixture: seven-day lifetime");
  g_next_id = 44;

  bool started = false;
  const char* group = "main";
  size_t turns = 0, invalid = 0, refused = 0, opens = 0;
  for (uint32_t i = 0; i < g_fixture.cases->count; ++i) {
    const tj::Value* c = g_fixture.cases->items[i];
    const char* name = text_of(tj::Get(c, "name"));
    if (!strcmp(name, "postJsonSavedContext")) started = true;
    if (!started) continue;
    const char* case_group = text_of(tj::Get(c, "group"));
    if (strcmp(case_group, group) != 0) {
      group = case_group;
      // Each fixture group is a fresh app over the same store.
      checkf(env.Restart(), "%s: hard restart (KV reopen)", group);
    }
    if (tj::Get(c, "seed")) continue;
    g_now = at_of(c);
    const tj::Value* provider = tj::Get(c, "provider");
    if (const tj::Value* items = tj::Get(c, "concurrent")) {
      // Two submissions of one clientMessageId: one row, one reply.
      bool saved = false;
      for (uint32_t k = 0; k < items->count; ++k) {
        char label[96];
        snprintf(label, sizeof(label), "%s[%u]", name, k);
        fixture_turn(env, label, conversation, items->items[k], provider, true, &saved);
      }
      ++turns;
      continue;
    }
    const char* url = text_of(tj::Get(c, "url"));
    const int status = static_cast<int>(tj::Get(c, "status")->number);
    const char* body_text = text_of(tj::Get(c, "bodyText"));
    if (!strcmp(text_of(tj::Get(c, "method")), "GET")) continue;
    if (!strcmp(url, "/v1/ai/conversations")) {
      // Opens of later groups draw ids too (Store::Open, R38).
      if (status != 201 && status != 503 && !(status == 401 && strstr(body_text, "PROOF_INVALID"))) continue;
      ai::Opened other;
      storage_error = false;
      const bool ok = env.store.Open(open_input(open_request_of(c), device_of(c)), &other, &storage_error);
      checkf(ok == (status != 401) && !storage_error, "%s: open outcome", name);
      if (status == 201) {
        const tj::Value* assistant = tj::Get(tj::Get(c, "body"), "assistant");
        checkf(env.store.SaveReply(other.id, other.message_id, text_of(tj::Get(assistant, "body")),
                                   tj::Get(assistant, "body")->length, reply_id, &created) &&
                   !strcmp(reply_id, text_of(tj::Get(assistant, "id"))),
               "%s: open reply id", name);
      }
      ++opens;
      continue;
    }
    // A message POST. The chat bearer must name the conversation (404 else).
    const char* chat = ai::ChatBearerToken(header_of(c, "x-intelligence-authorization"));
    const bool authorized = env.store.Authorize(conversation, device_of(c), chat, strlen(chat));
    if (status == 404) {
      checkf(!authorized, "%s: 404 is a refused credential", name);
      char message_id[ai::kIdBytes];
      const unsigned long long ids = g_next_id;
      checkf(add(env, conversation, message_request_of(c), message_id) == ai::AddStatus::kInvalid &&
                 g_next_id == ids,
             "%s: an expired conversation takes no message", name);
      ++refused;
      continue;
    }
    checkf(authorized, "%s: the chat bearer authorizes", name);
    if (status == 429) continue;
    if (status == 400) {
      const MessageRequest r = message_request_of(c);
      if (!strstr(body_text, "SNAPSHOT_INVALID") || !r.snapshot || !r.hash) continue;  // refused by the route
      char message_id[ai::kIdBytes];
      const unsigned long long ids = g_next_id;
      const size_t rows = message_count(env.store, conversation);
      checkf(add(env, conversation, r, message_id) == ai::AddStatus::kInvalid && g_next_id == ids &&
                 message_count(env.store, conversation) == rows,
             "%s: SNAPSHOT_INVALID draws no id and writes nothing", name);
      ++invalid;
      continue;
    }
    bool saved = false;
    fixture_turn(env, name, conversation, c, provider, status == 201, &saved);
    ++turns;
    if (!strcmp(name, "expiryLastUpdate")) {
      // A snapshot update on the last day does not extend the lifetime.
      int64_t c2 = 0, e2 = 0;
      check(env.store.ConversationTimes(conversation, &c2, &e2) && c2 == created_at && e2 == expires_at,
            "expiryLastUpdate: createdAt and expiresAt unchanged");
      const int64_t saved_now = g_now;
      g_now = expires_at - 1;
      check(message_count(env.store, conversation) == 100, "the last millisecond still lists the history");
      g_now = expires_at;
      check(message_count(env.store, conversation) == 0, "at createdAt + 7 d every row is invisible");
      g_now = saved_now;
    }
  }
  check(g_next_id == 178, "fixture: ids end where TypeScript's did (178)");
  checkf(turns == 71 && invalid == 123 && refused == 1 && opens == 25, "%s", "fixture: case counts");
  if (turns != 71 || invalid != 123 || refused != 1 || opens != 25)
    fprintf(stderr, "  turns %zu invalid %zu refused %zu opens %zu\n", turns, invalid, refused, opens);
  printf("fixture: %zu message turns, %zu SNAPSHOT_INVALID, %zu expired and %zu opens match TypeScript\n", turns,
         invalid, refused, opens);
}

// ---- records, snapshot updates and repeats ----

// A conversation opened with the fixture's openJson proof (ids from 0).
bool open_main(Env& env, ai::Opened* opened) {
  const tj::Value* opening = find_case("openJson");
  g_now = at_of(opening);
  g_next_id = 0;
  bool storage_error = false;
  return env.Begin() &&
         env.store.Open(open_input(open_request_of(opening), g_fixture.device_a), opened, &storage_error);
}

// The fixture wallet's snapshot with a capture time and a variation.
char* snapshot_text(int64_t captured, const char* address, const char* extra_wallet, bool watch_only = false,
                    const char* family = "evm") {
  char iso[32];
  const time_t seconds = static_cast<time_t>(captured / 1000);
  struct tm t{};
  gmtime_r(&seconds, &t);
  snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(captured % 1000));
  char* out = static_cast<char*>(malloc(4096));
  snprintf(out, 4096,
           "{\"version\":1,\"capturedAt\":\"%s\",\"balancesComplete\":false,\"wallets\":[{\"id\":\"hd:evm:0\","
           "\"address\":\"%s\",\"family\":\"%s\",\"networkIds\":[\"eth-mainnet\"],\"watchOnly\":%s}%s],"
           "\"tokens\":[{\"assetId\":\"eth-mainnet:native\",\"networkId\":\"eth-mainnet\",\"symbol\":\"ETH\","
           "\"decimals\":18,\"source\":\"native\"}],\"holdings\":[{\"walletId\":\"hd:evm:0\","
           "\"assetId\":\"eth-mainnet:native\",\"displayUnits\":\"2\"}]}",
           iso, address, family, watch_only ? "true" : "false", extra_wallet ? extra_wallet : "");
  return out;
}

const char kOwner[] = "0x1C5A77d9FA7eF466951B2F01F724BCa3A5820b63";    // the fixture signer
const char kStranger[] = "0x0331CEd0875e2eee97ad88F2b85B02f4944f78FC";  // another wallet

MessageRequest plain(const char* client, const char* body) {
  MessageRequest r;
  r.client = client;
  r.body = body;
  r.body_length = strlen(body);
  return r;
}
MessageRequest with_snapshot(const char* client, const char* body, const char* snapshot, char hash[65]) {
  MessageRequest r = plain(client, body);
  ai::Sha256Hex(snapshot, strlen(snapshot), hash);
  r.snapshot = snapshot;
  r.snapshot_length = strlen(snapshot);
  r.hash = hash;
  return r;
}

void records_and_repeats() {
  Env env;
  ai::Opened opened;
  check(open_main(env, &opened), "records: opens");
  const char* id = opened.id;
  char key[160], value[8192];
  snprintf(key, sizeof(key), "ai/conversation/%s", id);
  kv_text(env.kv, key, g_now, value, sizeof(value));
  const tj::Value* original = tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(value), strlen(value));
  const char* original_hash = g_fixture.arena.CopyText(text_of(tj::Get(original, "snapshotHash")),
                                                       strlen(text_of(tj::Get(original, "snapshotHash"))));
  int64_t created_at = 0, expires_at = 0;
  check(env.store.ConversationTimes(id, &created_at, &expires_at), "records: times");

  // No snapshot: the message and its client index (2 records), the
  // conversation's snapshot referenced; the conversation is not rewritten.
  g_now += 1000;
  size_t keys = key_count(env.kv, "ai/", g_now);
  off_t size;
  char first[ai::kIdBytes];
  check(add(env, id, plain("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "no snapshot"), first) == ai::AddStatus::kAdded &&
            key_count(env.kv, "ai/", g_now) == keys + 2,
        "records: a plain message writes the message and its client index");
  char conversation_after[8192];
  kv_text(env.kv, key, g_now, conversation_after, sizeof(conversation_after));
  check(!strcmp(conversation_after, value), "records: the conversation is not rewritten without a snapshot");
  snprintf(key, sizeof(key), "ai/message/%s/%s", id, first);
  char row[8192];
  kv_text(env.kv, key, g_now, row, sizeof(row));
  check(strstr(row, original_hash) && strstr(row, "\"clientMessageId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"") &&
            strstr(row, "\"role\":\"user\""),
        "records: the row references the conversation's snapshot");

  // A new snapshot: message, client index and snapshot record are new, the
  // conversation is rewritten with the new hash and updatedAt.
  g_now += 1000;
  char* update = snapshot_text(g_now - 500, kOwner, nullptr);
  char hash[65];
  keys = key_count(env.kv, "ai/", g_now);
  char second[ai::kIdBytes];
  check(add(env, id, with_snapshot("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "new snapshot", update, hash), second) ==
                ai::AddStatus::kAdded &&
            key_count(env.kv, "ai/", g_now) == keys + 3,
        "records: a snapshot update adds the message, client index and snapshot record");
  snprintf(key, sizeof(key), "ai/conversation/%s", id);
  kv_text(env.kv, key, g_now, value, sizeof(value));
  const tj::Value* updated = tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(value), strlen(value));
  check(tj::StringEquals(tj::Get(updated, "snapshotHash"), hash) &&
            static_cast<int64_t>(tj::Get(updated, "updatedAt")->number) == g_now &&
            static_cast<int64_t>(tj::Get(updated, "createdAt")->number) == created_at &&
            static_cast<int64_t>(tj::Get(updated, "expiresAt")->number) == expires_at &&
            tj::StringEquals(tj::Get(updated, "ownerId"), text_of(tj::Get(original, "ownerId"))) &&
            tj::StringEquals(tj::Get(updated, "accessTokenHash"), text_of(tj::Get(original, "accessTokenHash"))),
        "records: the conversation takes the new snapshot hash and updatedAt; lifetime and owner unchanged");
  // The next plain message is answered from the updated snapshot.
  g_now += 1000;
  char third[ai::kIdBytes];
  check(add(env, id, plain("cccccccccccccccccccccccccccccccc", "which snapshot?"), third) == ai::AddStatus::kAdded,
        "records: a later plain message");
  tj::Arena arena = tj::GrowingArena(16u << 20);
  const tj::Value* context = nullptr;
  ai::Turn* turns = nullptr;
  size_t count = 0;
  char captured[32];
  {
    const int64_t at = g_now - 1500;
    const time_t seconds = static_cast<time_t>(at / 1000);
    struct tm t{};
    gmtime_r(&seconds, &t);
    snprintf(captured, sizeof(captured), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(at % 1000));
  }
  check(env.store.LoadReplyContext(id, third, arena, &context, &turns, &count) &&
            tj::StringEquals(tj::Get(context, "capturedAt"), captured) && count == 4 && !turns[count - 1].assistant &&
            turns[count - 1].length == 15 && !memcmp(turns[count - 1].body, "which snapshot?", 15),
        "records: the model sees the updated snapshot and every user turn (no assistant yet)");
  tj::FreeArena(arena);
  // The same snapshot again under a new client id: no new snapshot record.
  g_now += 1000;
  keys = key_count(env.kv, "ai/", g_now);
  char fourth[ai::kIdBytes];
  check(add(env, id, with_snapshot("dddddddddddddddddddddddddddddddd", "same snapshot", update, hash), fourth) ==
                ai::AddStatus::kAdded &&
            key_count(env.kv, "ai/", g_now) == keys + 2,
        "records: a known snapshot hash writes no second snapshot record");

  // Repeats: same body, changed body, a valid new snapshot (no write; the
  // conversation keeps its snapshot), an invalid snapshot (refused).
  g_now += 1000;
  size = file_size(env.directory);
  kv_text(env.kv, key, g_now, value, sizeof(value));
  const unsigned long long ids = g_next_id;
  char again[ai::kIdBytes];
  check(add(env, id, plain("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "no snapshot"), again) == ai::AddStatus::kAdded &&
            !strcmp(again, first),
        "repeat: same body answers the existing row");
  check(add(env, id, plain("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "changed text"), again) == ai::AddStatus::kAdded &&
            !strcmp(again, first),
        "repeat: changed body answers the existing row");
  char* newer = snapshot_text(g_now, kOwner, nullptr);
  char newer_hash[65];
  check(add(env, id, with_snapshot("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "x", newer, newer_hash), again) ==
                ai::AddStatus::kAdded &&
            !strcmp(again, first),
        "repeat: a valid new snapshot answers the existing row");
  char bad_hash[65];
  memset(bad_hash, 'a', 64);
  bad_hash[64] = '\0';
  MessageRequest mismatched = with_snapshot("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "x", newer, newer_hash);
  mismatched.hash = bad_hash;
  check(add(env, id, mismatched, again) == ai::AddStatus::kInvalid, "repeat: an invalid snapshot is SNAPSHOT_INVALID");
  char conversation_now[8192];
  kv_text(env.kv, key, g_now, conversation_now, sizeof(conversation_now));
  check(file_size(env.directory) == size && g_next_id == ids && !strcmp(conversation_now, value),
        "repeat: nothing written, no id drawn, the conversation snapshot unchanged");
  arena = tj::GrowingArena(1u << 20);
  ai::MessageView view;
  check(env.store.LoadMessage(id, first, arena, &view) && view.body_length == 11 &&
            !memcmp(view.body, "no snapshot", 11),
        "repeat: the stored body is the first one");
  tj::FreeArena(arena);

  // Redaction before storage.
  g_now += 1000;
  const char phrase[] = "my phrase is abandon ability able about above absent absorb abstract absurd abuse access "
                        "accident";
  char redacted_id[ai::kIdBytes];
  check(add(env, id, plain("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", phrase), redacted_id) == ai::AddStatus::kAdded,
        "redaction: added");
  snprintf(key, sizeof(key), "ai/message/%s/%s", id, redacted_id);
  kv_text(env.kv, key, g_now, row, sizeof(row));
  check(!strstr(row, "abandon ability") && strstr(row, "[redacted"), "redaction: the phrase is never stored");

  // Every record expires with the conversation (createdAt + 7 d), whenever
  // it was written.
  g_now = expires_at - 1;
  check(key_count(env.kv, "ai/", g_now) > 0 && message_count(env.store, id) == 6, "expiry: live 1 ms before");
  char late[ai::kIdBytes];
  check(add(env, id, plain("ffffffffffffffffffffffffffffffff", "last ms"), late) == ai::AddStatus::kAdded,
        "expiry: a message in the last millisecond is added");
  g_now = expires_at;
  check(key_count(env.kv, "ai/", g_now) == 0, "expiry: every record (incl. the late one) is gone at createdAt + 7 d");
  check(add(env, id, plain("ffffffffffffffffffffffffffffffff", "last ms"), late) == ai::AddStatus::kInvalid,
        "expiry: an expired conversation is SNAPSHOT_INVALID at the store (the route answers 404 first)");
  free(update);
  free(newer);
}

// ---- snapshot validation and ownership ----

void snapshot_validation() {
  Env env;
  ai::Opened opened;
  check(open_main(env, &opened), "validation: opens");
  const char* id = opened.id;
  g_now += 1000;
  char lower[64], upper[64];
  snprintf(lower, sizeof(lower), "%s", kOwner);
  for (char* p = lower + 2; *p; ++p)
    if (*p >= 'A' && *p <= 'F') *p = static_cast<char>(*p - 'A' + 'a');
  snprintf(upper, sizeof(upper), "0x%s", kOwner + 2);
  for (char* p = upper + 2; *p; ++p)
    if (*p >= 'a' && *p <= 'f') *p = static_cast<char>(*p - 'a' + 'A');
  char second_wallet[512];
  snprintf(second_wallet, sizeof(second_wallet),
           ",{\"id\":\"hd:evm:9\",\"address\":\"%s\",\"family\":\"evm\",\"networkIds\":[],\"watchOnly\":false}", lower);
  char duplicate[512];
  snprintf(duplicate, sizeof(duplicate),
           ",{\"id\":\"hd:evm:0\",\"address\":\"%s\",\"family\":\"evm\",\"networkIds\":[],\"watchOnly\":false}", kOwner);
  struct Case {
    const char* label;
    char* text;
    bool valid;
  } cases[] = {
      {"owner", snapshot_text(g_now, kOwner, nullptr), true},
      {"owner, lower-case address", snapshot_text(g_now, lower, nullptr), true},
      {"owner, upper-case hex", snapshot_text(g_now, upper, nullptr), true},
      {"owner as the second wallet", snapshot_text(g_now, kStranger, second_wallet), true},
      {"foreign wallet only", snapshot_text(g_now, kStranger, nullptr), false},
      {"owner watch-only", snapshot_text(g_now, kOwner, nullptr, true), false},
      {"owner family EVM", snapshot_text(g_now, kOwner, nullptr, false, "EVM"), false},
      {"duplicate wallet id", snapshot_text(g_now, kOwner, duplicate), false},
  };
  int n = 0;
  for (Case& c : cases) {
    char client[33];
    snprintf(client, sizeof(client), "%032d", ++n);
    char hash[65];
    const off_t size = file_size(env.directory);
    const unsigned long long ids = g_next_id;
    char message_id[ai::kIdBytes];
    const ai::AddStatus status = add(env, id, with_snapshot(client, "q", c.text, hash), message_id);
    if (c.valid) {
      checkf(status == ai::AddStatus::kAdded, "validation: %s is accepted", c.label);
    } else {
      checkf(status == ai::AddStatus::kInvalid && file_size(env.directory) == size && g_next_id == ids,
             "validation: %s is SNAPSHOT_INVALID (no write, no id)", c.label);
    }
    free(c.text);
  }
  // Text-level failures.
  char* good = snapshot_text(g_now, kOwner, nullptr);
  const size_t good_length = strlen(good);
  char* bom = static_cast<char*>(malloc(good_length + 4));
  std::copy_n("\xEF\xBB\xBF", 3, bom);
  memcpy(bom + 3, good, good_length + 1);
  char* padded = static_cast<char*>(malloc(good_length + 8));
  snprintf(padded, good_length + 8, " \n%s\t ", good);
  struct Text {
    const char* label;
    const char* text;
    bool valid;
  } texts[] = {{"not JSON", "{x", false},          {"a leading BOM", bom, false},
               {"JSON whitespace around", padded, true}, {"version 2", "{\"version\":2}", false},
               {"an array", "[1]", false},           {"null", "null", false}};
  for (const Text& t : texts) {
    char client[33];
    snprintf(client, sizeof(client), "%032d", ++n);
    char hash[65];
    MessageRequest r = plain(client, "q");
    ai::Sha256Hex(t.text, strlen(t.text), hash);
    r.snapshot = t.text;
    r.snapshot_length = strlen(t.text);
    r.hash = hash;
    char message_id[ai::kIdBytes];
    checkf((add(env, id, r, message_id) == ai::AddStatus::kAdded) == t.valid, "validation: %s", t.label);
  }
  // The hash covers the exact text: one more space mismatches.
  char hash[65];
  MessageRequest r = with_snapshot("99999999999999999999999999999999", "q", good, hash);
  r.snapshot = padded;
  r.snapshot_length = strlen(padded);
  char message_id[ai::kIdBytes];
  check(add(env, id, r, message_id) == ai::AddStatus::kInvalid, "validation: the hash must match the exact text");
  free(good);
  free(bom);
  free(padded);
}

// ---- storage faults ----

void storage_faults() {
  const char* stages[] = {"write_after_header", "write_before_fsync"};
  for (const char* stage : stages) {
    Env env;
    ai::Opened opened;
    check(open_main(env, &opened), "fault: opens");
    g_now += 1000;
    char* update = snapshot_text(g_now, kOwner, nullptr);
    char hash[65];
    const MessageRequest r = with_snapshot("abababababababababababababababab", "faulted", update, hash);
    char message_id[ai::kIdBytes];
    setenv("FORTIS_KV_TEST_FAIL", stage, 1);
    const ai::AddStatus status = add(env, opened.id, r, message_id);
    unsetenv("FORTIS_KV_TEST_FAIL");
    checkf(status == ai::AddStatus::kStorage, "%s: a failed commit is a storage error (500)", stage);
    // The KV closed itself after the uncertain commit: every read fails.
    checkf(add(env, opened.id, r, message_id) == ai::AddStatus::kStorage,
           "%s: a read on the closed KV is a storage error, not SNAPSHOT_INVALID", stage);
    checkf(add(env, opened.id, plain("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd", "plain"), message_id) ==
               ai::AddStatus::kStorage,
           "%s: a plain message on the closed KV is a storage error", stage);
    // Reopened: the frame is all or nothing; the retry adds exactly once.
    checkf(env.Restart(), "%s: reopens", stage);
    const size_t rows = message_count(env.store, opened.id);
    checkf(rows == 1 || rows == 2, "%s: the frame is all or nothing", stage);
    checkf(add(env, opened.id, r, message_id) == ai::AddStatus::kAdded && message_count(env.store, opened.id) == 2,
           "%s: the retried message is added once", stage);
    free(update);
  }

  // Corrupt or missing records reached by a message.
  Env env;
  ai::Opened opened;
  check(open_main(env, &opened), "corrupt: opens");
  const char* id = opened.id;
  g_now += 1000;
  char existing[ai::kIdBytes];
  check(add(env, id, plain("12121212121212121212121212121212", "kept"), existing) == ai::AddStatus::kAdded,
        "corrupt: a first message");
  char conversation_key[160], snapshot_key[256], message_key[160], client_key[160];
  char saved_conversation[8192], saved_snapshot[8192], saved_message[8192];
  snprintf(conversation_key, sizeof(conversation_key), "ai/conversation/%s", id);
  kv_text(env.kv, conversation_key, g_now, saved_conversation, sizeof(saved_conversation));
  const tj::Value* record =
      tj::Parse(g_fixture.arena, reinterpret_cast<const uint8_t*>(saved_conversation), strlen(saved_conversation));
  snprintf(snapshot_key, sizeof(snapshot_key), "ai/snapshot/%s/%s", id, text_of(tj::Get(record, "snapshotHash")));
  snprintf(message_key, sizeof(message_key), "ai/message/%s/%s", id, existing);
  snprintf(client_key, sizeof(client_key), "ai/client/%s/12121212121212121212121212121212", id);
  check(kv_text(env.kv, snapshot_key, g_now, saved_snapshot, sizeof(saved_snapshot)) &&
            kv_text(env.kv, message_key, g_now, saved_message, sizeof(saved_message)),
        "corrupt: records read");
  const int64_t expiry = opened.expires_at_ms;
  auto put = [&](const char* key, const char* value) {
    const fortis::storage::KvMutation write{key, value, static_cast<uint32_t>(strlen(value)), expiry, false};
    return env.kv.Write(&write, 1);
  };
  auto erase = [&](const char* key) {
    const fortis::storage::KvMutation write{key, nullptr, 0, 0, true};
    return env.kv.Write(&write, 1);
  };
  const MessageRequest fresh = plain("34343434343434343434343434343434", "new");
  const MessageRequest repeat = plain("12121212121212121212121212121212", "kept");
  char got[ai::kIdBytes];
  const unsigned long long ids = g_next_id;
  check(put(conversation_key, "{\"ownerId\":") && add(env, id, fresh, got) == ai::AddStatus::kStorage,
        "corrupt conversation record: storage error (TypeScript's decode throws: 500)");
  check(put(conversation_key, saved_conversation), "conversation restored");
  check(put(snapshot_key, "not json") && add(env, id, fresh, got) == ai::AddStatus::kStorage,
        "corrupt stored snapshot (inside the TypeScript conversation record): storage error");
  check(put(snapshot_key, "{\"version\":2}") && add(env, id, fresh, got) == ai::AddStatus::kStorage,
        "stored snapshot failing the schema: storage error");
  check(erase(snapshot_key) && add(env, id, fresh, got) == ai::AddStatus::kStorage,
        "missing stored snapshot record: storage error");
  check(put(snapshot_key, saved_snapshot), "snapshot restored");
  check(put(message_key, "not json") && add(env, id, repeat, got) == ai::AddStatus::kStorage,
        "corrupt existing message on a repeat: storage error");
  check(put(message_key, saved_message) && put(client_key, "01JZ9999999999999999999999") &&
            add(env, id, repeat, got) == ai::AddStatus::kInvalid,
        "client index naming an absent message: SNAPSHOT_INVALID (TypeScript's null)");
  check(put(client_key, "short") && add(env, id, repeat, got) == ai::AddStatus::kInvalid,
        "malformed client index: SNAPSHOT_INVALID");
  check(put(client_key, existing) && add(env, id, repeat, got) == ai::AddStatus::kAdded && !strcmp(got, existing),
        "client index restored: the repeat answers");
  check(g_next_id == ids, "corrupt: no id drawn by any refused message");
  check(erase(conversation_key) && add(env, id, fresh, got) == ai::AddStatus::kInvalid,
        "absent conversation: SNAPSHOT_INVALID");
}

// ---- concurrent submissions ----

struct Racer {
  Env* env;
  const char* id;
  char client[33];
  char result[ai::kIdBytes];
  ai::AddStatus status;
};
pthread_mutex_t g_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_gate = PTHREAD_COND_INITIALIZER;
bool g_open_gate = false;
void* race(void* opaque) {
  auto* racer = static_cast<Racer*>(opaque);
  pthread_mutex_lock(&g_gate_mutex);
  while (!g_open_gate) pthread_cond_wait(&g_gate, &g_gate_mutex);
  pthread_mutex_unlock(&g_gate_mutex);
  racer->status = add(*racer->env, racer->id, plain(racer->client, "concurrent"), racer->result);
  return nullptr;
}
void run_racers(Racer* racers, size_t count) {
  pthread_t threads[16];
  g_open_gate = false;
  for (size_t i = 0; i < count; ++i) pthread_create(&threads[i], nullptr, race, &racers[i]);
  pthread_mutex_lock(&g_gate_mutex);
  g_open_gate = true;
  pthread_cond_broadcast(&g_gate);
  pthread_mutex_unlock(&g_gate_mutex);
  for (size_t i = 0; i < count; ++i) pthread_join(threads[i], nullptr);
}

void concurrency() {
  Env env;
  ai::Opened opened;
  check(open_main(env, &opened), "concurrency: opens");
  g_now += 1000;
  Racer same[16];
  for (Racer& r : same) {
    r = Racer{&env, opened.id, {}, {}, ai::AddStatus::kStorage};
    snprintf(r.client, sizeof(r.client), "%s", "56565656565656565656565656565656");
  }
  const size_t before = message_count(env.store, opened.id);
  run_racers(same, 16);
  bool all_same = true;
  for (const Racer& r : same) all_same &= r.status == ai::AddStatus::kAdded && !strcmp(r.result, same[0].result);
  check(all_same && message_count(env.store, opened.id) == before + 1,
        "concurrency: 16 submissions of one clientMessageId add one row and all answer it");
  Racer distinct[16];
  for (size_t i = 0; i < 16; ++i) {
    distinct[i] = Racer{&env, opened.id, {}, {}, ai::AddStatus::kStorage};
    snprintf(distinct[i].client, sizeof(distinct[i].client), "%032zx", i + 1000);
  }
  run_racers(distinct, 16);
  bool all_added = true, unique = true;
  for (size_t i = 0; i < 16; ++i) {
    all_added &= distinct[i].status == ai::AddStatus::kAdded;
    for (size_t k = 0; k < i; ++k) unique &= strcmp(distinct[i].result, distinct[k].result) != 0;
  }
  check(all_added && unique && message_count(env.store, opened.id) == before + 17,
        "concurrency: 16 distinct clientMessageIds add 16 rows with unique ids");
  // A hard restart keeps every row and the repeat answers.
  check(env.Restart() && message_count(env.store, opened.id) == before + 17, "restart: rows kept");
  char again[ai::kIdBytes];
  check(add(env, opened.id, plain("56565656565656565656565656565656", "concurrent"), again) == ai::AddStatus::kAdded &&
            !strcmp(again, same[0].result),
        "restart: the repeat answers the stored row");
}

// ---- the hourly message quota (kv.incrWindow(`q:ai:message:${device}`, 1 h) > 120) ----

fortis::compat::HourlyWindows<32> g_windows;

void hourly_windows() {
  const int64_t hour = 60 * 60 * 1000;
  const int64_t first = 1767323475000;
  uint64_t count = 0;
  bool ok = true;
  for (int i = 0; i < 120; ++i) ok &= g_windows.Increment('m', g_fixture.device_a, first + i * 100, hour, &count);
  check(ok && count == 120, "120 messages within the hour are allowed (count 120)");
  check(g_windows.Increment('m', g_fixture.device_a, first + hour - 1, hour, &count) && count == 121,
        "the 121st in the window is over");
  check(g_windows.Increment('m', g_fixture.device_b, first + 1, hour, &count) && count == 1,
        "another device has its own message window");
  check(g_windows.Increment('o', g_fixture.device_a, first + 1, hour, &count) && count == 1,
        "the open scope does not share the message window");
  check(g_windows.Increment('m', g_fixture.device_a, first + hour, hour, &count) && count == 1,
        "the message window ends at first + 1 h exactly");
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
  const tj::Value* env = tj::Get(g_fixture.root, "env");
  g_fixture.secret = text_of(tj::Get(env, "SUPPORT_AUTH_SECRET"));
  g_fixture.model = text_of(tj::Get(env, "OPENAI_MODEL"));
  g_fixture.device_a = text_of(tj::Get(tj::Get(g_fixture.root, "devices"), "A"));
  g_fixture.device_b = text_of(tj::Get(tj::Get(g_fixture.root, "devices"), "B"));
  g_fixture.token_b = text_of(tj::Get(tj::Get(g_fixture.root, "tokens"), "B"));
  if (!ai::OutboundPatternsReady()) return 2;

  fixture_posts();
  records_and_repeats();
  snapshot_validation();
  storage_faults();
  concurrency();
  hourly_windows();
  free(fixture_text);
  if (g_failures) {
    fprintf(stderr, "ai_post_message_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("ai_post_message_test: store addMessage/replies/records/validation/faults/concurrency/restart and hourly "
         "quota OK\n");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
