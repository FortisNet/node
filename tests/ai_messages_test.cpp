#include "test_support.h"
#include <cstring>
// Unit checks of GET /v1/ai/conversations/:id/messages (R04) below the HTTP
// layer (src/ai/ai_service.cpp, src/ai/ai_store.cpp):
//  - the id and chat-bearer policy (paramsSchema, authorized());
//  - the body encoder against every 200 GET in the frozen TypeScript route
//    fixture (JSON text and protobuf wire byte for byte), plus empty
//    history, JSON.stringify escaping, proto3 omission, varint boundaries,
//    the oldest-first trim under the response bound and createdAt text;
//  - the store's credential policy (device, token hash, shape, durable
//    lifetime) and history read (newest 100, oldest first, creation order
//    within one millisecond, read-only).
// The signed opening comes from the fixture's openJson case.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ai_service.h"
#include "ai_store.h"
#include "fake_seams.h"
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

int64_t g_now = 0;
fortis::testing::PointerClock g_clock(&g_now);

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
  if (strlen(text) != 24 || !fortis::test::ParseTimestampParts(text, &t.tm_year, &t.tm_mon, &t.tm_mday,
                                   &t.tm_hour, &t.tm_min, &t.tm_sec, &millis))
    return false;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  *ms = static_cast<int64_t>(timegm(&t)) * 1000 + millis;
  return true;
}

ai::MessageView row(const char* id, const char* role, const char* body, size_t body_length, int64_t created) {
  ai::MessageView view;
  snprintf(view.id, sizeof(view.id), "%s", id);
  snprintf(view.role, sizeof(view.role), "%s", role);
  view.body = body;
  view.body_length = body_length;
  view.created_at_ms = created;
  return view;
}

struct Encoded {
  std::unique_ptr<uint8_t[], fortis::base::StorageDeleter> data;
  size_t length = 0;
  size_t first = 0;
  bool ok = false;
};
void encode(const ai::MessageView* rows, size_t count, bool protobuf, size_t limit, Encoded* out) {
  out->ok = ai::EncodeMessagesBody(rows, count, protobuf, limit, &out->data, &out->length, &out->first);
}
bool equals(const Encoded& e, const void* bytes, size_t length) {
  return e.ok && e.length == length && memcmp(e.data.get(), bytes, length) == 0;
}
bool equals_text(const Encoded& e, const char* text) { return equals(e, text, strlen(text)); }

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// ---- protobuf reading (the fixture wire) ----

bool read_varint(const uint8_t* data, size_t length, size_t* at, uint64_t* value) {
  *value = 0;
  for (int shift = 0; shift < 64 && *at < length; shift += 7) {
    const uint8_t byte = data[(*at)++];
    *value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return true;
  }
  return false;
}

// ConversationMessagesResponse wire → rows (strings point into `data`, so
// `data` must outlive them; each string is copied NUL-terminated into ids).
size_t decode_wire(const uint8_t* data, size_t length, ai::MessageView* rows, size_t capacity, bool* ok) {
  size_t at = 0, count = 0;
  *ok = true;
  while (at < length) {
    uint64_t tag = 0, size = 0;
    if (!read_varint(data, length, &at, &tag) || tag != 0x0a || !read_varint(data, length, &at, &size) ||
        size > length - at || count == capacity) {
      *ok = false;
      return count;
    }
    const size_t end = at + size;
    ai::MessageView view;
    view.body = "";
    char created[32] = "";
    while (at < end) {
      uint64_t field = 0, field_length = 0;
      if (!read_varint(data, end, &at, &field) || (field & 7) != 2 || !read_varint(data, end, &at, &field_length) ||
          field_length > end - at) {
        *ok = false;
        return count;
      }
      const char* text = reinterpret_cast<const char*>(data + at);
      switch (field >> 3) {
        case 1: snprintf(view.id, sizeof(view.id), "%.*s", static_cast<int>(field_length), text); break;
        case 2: snprintf(view.role, sizeof(view.role), "%.*s", static_cast<int>(field_length), text); break;
        case 3: view.body = text; view.body_length = field_length; break;
        case 4: snprintf(created, sizeof(created), "%.*s", static_cast<int>(field_length), text); break;
        default: *ok = false; return count;
      }
      at += field_length;
    }
    if (!parse_iso(created, &view.created_at_ms)) *ok = false;
    rows[count++] = view;
  }
  return count;
}

// ---- id and bearer policy ----

void check_policy() {
  check(ai::IsConversationId("01JZ0000000000000000000000"), "fixture id accepted");
  check(ai::IsConversationId("0123456789ABCDEFGHJKMNPQRS"), "Crockford digits accepted");
  check(ai::IsConversationId("TVWXYZ00000000000000000000"), "Crockford T..Z accepted");
  check(!ai::IsConversationId(nullptr), "null id refused");
  check(!ai::IsConversationId(""), "empty id refused");
  check(!ai::IsConversationId("01JZ000000000000000000000"), "25 characters refused");
  check(!ai::IsConversationId("01JZ00000000000000000000000"), "27 characters refused");
  check(!ai::IsConversationId("01jz0000000000000000000000"), "lower case refused");
  static const char* const kExcluded[] = {"I", "L", "O", "U"};
  for (const char* letter : kExcluded) {
    char id[27];
    snprintf(id, sizeof(id), "01JZ000000000000000000000%s", letter);
    check(!ai::IsConversationId(id), "excluded Crockford letter refused");
  }
  check(!ai::IsConversationId("01JZ00000000000000000000-0"), "punctuation refused");
  check(!ai::IsConversationId("01JZ000000000000000000000\xc3\x89"), "non-ASCII refused");

  const char* token = "fintel_chat_lPJdyesj8fAIY2bwobgX4DD4m9zs6a2Yf8v11lp9-oY";
  char header[96];
  snprintf(header, sizeof(header), "Bearer %s", token);
  check(strcmp(ai::ChatBearerToken(header), token) == 0, "Bearer prefix stripped");
  check(*ai::ChatBearerToken(nullptr) == '\0', "missing header is no bearer");
  snprintf(header, sizeof(header), "bearer %s", token);
  check(*ai::ChatBearerToken(header) == '\0', "scheme is case-sensitive");
  check(*ai::ChatBearerToken("Bearer") == '\0', "bare scheme is no bearer");
  check(*ai::ChatBearerToken("Bearer ") == '\0', "empty token");
  snprintf(header, sizeof(header), "Bearer  %s", token);
  check(strcmp(ai::ChatBearerToken(header), header + 7) == 0 && header[7] == ' ',
        "a second space stays in the token (then refused by shape)");
  snprintf(header, sizeof(header), "Basic %s", token);
  check(*ai::ChatBearerToken(header) == '\0', "other scheme is no bearer");
}

// ---- body encoding ----

// Every 200 GET of the TypeScript fixture, re-encoded from its own rows.
void check_fixture_bodies(const tj::Value* cases) {
  size_t json_cases = 0, wire_cases = 0, rows_seen = 0;
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    if (!tj::StringEquals(tj::Get(c, "method"), "GET") || tj::Get(c, "status")->number != 200) continue;
    const tj::Value* wire_hex = tj::Get(c, "wireHex");
    ai::MessageView rows[ai::kMaxHistory];
    size_t count = 0;
    if (tj::IsString(wire_hex)) {
      const size_t length = wire_hex->length / 2;
      uint8_t* wire = static_cast<uint8_t*>(malloc(length ? length : 1));
      for (size_t k = 0; k < length; ++k)
        wire[k] = static_cast<uint8_t>(hex_value(wire_hex->text[2 * k]) << 4 | hex_value(wire_hex->text[2 * k + 1]));
      bool ok = false;
      count = decode_wire(wire, length, rows, ai::kMaxHistory, &ok);
      check(ok, "fixture wire decodes");
      Encoded protobuf;
      encode(rows, count, true, 2u << 20, &protobuf);
      check(equals(protobuf, wire, length) && protobuf.first == 0, "protobuf body equals the TypeScript wire");
      if (!equals(protobuf, wire, length)) fprintf(stderr, "  case %s\n", tj::Get(c, "name")->text);
      free(wire);
      ++wire_cases;
    } else {
      const tj::Value* text = tj::Get(c, "bodyText");
      tj::Arena arena = tj::GrowingArena(16u << 20);
      const tj::Value* body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
      const tj::Value* messages = tj::Get(body, "messages");
      check(tj::IsArray(messages) && messages->count <= ai::kMaxHistory, "fixture JSON shape");
      for (uint32_t k = 0; k < messages->count && k < ai::kMaxHistory; ++k) {
        const tj::Value* m = messages->items[k];
        int64_t created = 0;
        check(parse_iso(tj::Get(m, "createdAt")->text, &created), "fixture createdAt parses");
        rows[count++] = row(tj::Get(m, "id")->text, tj::Get(m, "role")->text, tj::Get(m, "body")->text,
                            tj::Get(m, "body")->length, created);
      }
      Encoded json;
      encode(rows, count, false, 2u << 20, &json);
      check(equals(json, text->text, text->length) && json.first == 0, "JSON body equals the TypeScript text");
      if (!equals(json, text->text, text->length)) fprintf(stderr, "  case %s\n", tj::Get(c, "name")->text);
      tj::FreeArena(arena);
      ++json_cases;
    }
    // Oldest first: ids (monotonic ULIDs) strictly ascend.
    for (size_t k = 1; k < count; ++k) check(strcmp(rows[k - 1].id, rows[k].id) < 0, "fixture rows ascend");
    rows_seen += count;
  }
  // The sha-pinned fixture: getJson, getAfterRetry, getNewest100, expiryProtoConversationStillLive (JSON);
  // getProtobuf, getNewest100Protobuf, getNotLimitedByMessageQuota, restartGet, expiryLastMillisecond.
  check(json_cases == 4 && wire_cases == 5, "fixture has the nine 200 GET cases");
  printf("fixture: %zu JSON and %zu protobuf GET bodies (%zu rows) re-encoded byte for byte\n", json_cases,
         wire_cases, rows_seen);
}

void check_owned_output_alias() {
  for (bool protobuf : {false, true}) {
    Encoded output;
    output.data = fortis::base::AllocateStorage<uint8_t[]>(5);
    check(output.data != nullptr, "allocate previous output");
    std::memcpy(output.data.get(), "hello", 5);
    const auto message = row("01JZ0000000000000000000009", "user",
        reinterpret_cast<const char*>(output.data.get()), 5, 1767323075123);
    encode(&message, 1, protobuf, 2u << 20, &output);
    const char* expected = protobuf ? "hello" : "\"body\":\"hello\"";
    check(output.ok && memmem(output.data.get(), output.length, expected,
          std::strlen(expected)), "encoding retains an aliased previous output");
  }
}

void check_shapes() {
  Encoded json, protobuf;
  encode(nullptr, 0, false, 2u << 20, &json);
  encode(nullptr, 0, true, 2u << 20, &protobuf);
  check(equals_text(json, "{\"messages\":[]}") && json.first == 0, "empty JSON history");
  check(protobuf.ok && protobuf.length == 0 && protobuf.data && protobuf.first == 0, "empty protobuf history");

  // JSON.stringify escaping (expected bytes from Node): quote, backslash,
  // C0 controls as short or \u00XX escapes; DEL, U+2028/2029, non-ASCII
  // and "</script>" stay raw.
  const char* tricky = "q\"b\\s\x01\x1f\n\t\b\f\r\x7f\xe2\x80\xa8\xe2\x80\xa9\xc3\xa9\xf0\x9f\x98\x80</script>";
  const ai::MessageView one = row("01JZ0000000000000000000009", "assistant", tricky, strlen(tricky), 1767323075123);
  Encoded escaped;
  encode(&one, 1, false, 2u << 20, &escaped);
  check(equals_text(escaped,
                    "{\"messages\":[{\"id\":\"01JZ0000000000000000000009\",\"role\":\"assistant\",\"body\":"
                    "\"q\\\"b\\\\s\\u0001\\u001f\\n\\t\\b\\f\\r\x7f\xe2\x80\xa8\xe2\x80\xa9\xc3\xa9\xf0\x9f\x98\x80"
                    "</script>\",\"createdAt\":\"2026-01-02T03:04:35.123Z\"}]}"),
        "JSON.stringify escaping");
  Encoded raw;
  encode(&one, 1, true, 2u << 20, &raw);
  const size_t body_length = strlen(tricky);
  check(raw.ok && raw.length > body_length && memmem(raw.data.get(), raw.length, tricky, body_length) != nullptr,
        "protobuf body bytes are verbatim UTF-8");

  // A WTF-8 lone surrogate (never stored: the store writes U+FFFD) becomes
  // U+FFFD on the wire, as protobuf-es' TextEncoder writes it.
  const char lone[] = "a\xed\xa0\xbd" "b";
  const ai::MessageView surrogate = row("01JZ000000000000000000000A", "user", lone, 5, 0);
  Encoded replaced;
  encode(&surrogate, 1, true, 2u << 20, &replaced);
  const uint8_t expected_surrogate[] = {0x0a, 0x49, 0x0a, 0x1a, '0', '1', 'J', 'Z', '0', '0', '0', '0', '0', '0', '0',
                                        '0',  '0',  '0',  '0',  '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', 'A',
                                        0x12, 0x04, 'u',  's',  'e', 'r', 0x1a, 0x0b, 'a', 0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd, 'b', 0x22,
                                        0x18, '1',  '9',  '7',  '0', '-', '0', '1', '-', '0', '1', 'T', '0', '0', ':',
                                        '0',  '0',  ':',  '0',  '0', '.', '0', '0', '0', 'Z'};
  check(equals(replaced, expected_surrogate, sizeof(expected_surrogate)), "malformed raw UTF-8 → maximal-subpart replacements, epoch text");

  // proto3: empty strings are omitted; JSON keeps them.
  const ai::MessageView blank = row("01JZ000000000000000000000B", "", "", 0, 0);
  Encoded blank_wire, blank_json;
  encode(&blank, 1, true, 2u << 20, &blank_wire);
  encode(&blank, 1, false, 2u << 20, &blank_json);
  check(blank_wire.ok && blank_wire.length == 2 + 28 + 26 && blank_wire.data[1] == 28 + 26 &&
            blank_wire.data[2] == 0x0a && blank_wire.data[30] == 0x22,
        "empty role/body omitted on the wire");
  check(equals_text(blank_json, "{\"messages\":[{\"id\":\"01JZ000000000000000000000B\",\"role\":\"\",\"body\":\"\","
                                "\"createdAt\":\"1970-01-01T00:00:00.000Z\"}]}"),
        "empty strings kept in JSON");

  // Varint boundaries: a 127-byte body has a one-byte length, 128 two; the
  // submessage length crosses 128 too.
  char body[200];
  memset(body, 'x', sizeof(body));
  static const size_t kLengths[] = {127, 128};
  for (size_t length : kLengths) {
    const ai::MessageView sized = row("01JZ000000000000000000000C", "user", body, length, 0);
    Encoded wire;
    encode(&sized, 1, true, 2u << 20, &wire);
    const size_t inner = 28 + 6 + (1 + (length < 128 ? 1 : 2) + length) + 26;
    check(wire.ok && wire.length == 1 + 2 + inner && wire.data[0] == 0x0a &&
              wire.data[1] == static_cast<uint8_t>(0x80 | (inner & 0x7f)) && wire.data[2] == inner >> 7,
          "two-byte submessage length");
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    const uint8_t* body_tag = wire.data.get() + 3 + 28 + 6;
    check(body_tag[0] == 0x1a && (length < 128 ? body_tag[1] == 127 : (body_tag[1] == 0x80 && body_tag[2] == 1)),
          "body length varint boundary");
  }

  // createdAt text: the extremes of the four-digit year.
  const ai::MessageView late = row("01JZ000000000000000000000D", "user", "x", 1, 253402300799999);
  Encoded late_json;
  encode(&late, 1, false, 2u << 20, &late_json);
  check(late_json.ok && memmem(late_json.data.get(), late_json.length, "\"9999-12-31T23:59:59.999Z\"", 26),
        "createdAt at the end of year 9999");
}

// The response bound keeps the newest rows that fit and drops the oldest.
void check_limit() {
  ai::MessageView rows[5];
  char ids[5][27];
  const char* bodies[5] = {"first", "second turn", "third", "fourth turn here", "fifth"};
  for (size_t i = 0; i < 5; ++i) {
    snprintf(ids[i], sizeof(ids[i]), "01JZ%022zu", i);
    rows[i] = row(ids[i], i % 2 ? "assistant" : "user", bodies[i], strlen(bodies[i]), 1767323075000 + i);
  }
  static const bool kForms[] = {false, true};
  for (bool protobuf : kForms) {
    Encoded full;
    encode(rows, 5, protobuf, 2u << 20, &full);
    check(full.ok && full.first == 0, "all rows under a large bound");
    for (size_t drop = 0; drop <= 5; ++drop) {
      Encoded tail;
      encode(rows + drop, 5 - drop, protobuf, 2u << 20, &tail);
      // Exactly the tail's size keeps the tail; one byte less drops one more.
      Encoded fit, short_by_one;
      encode(rows, 5, protobuf, tail.length, &fit);
      check(fit.ok && fit.first == drop && fit.length == tail.length &&
                memcmp(fit.data.get(), tail.data.get(), tail.length) == 0,
            "bound keeps the newest rows that fit");
      if (drop < 5 && tail.length > (protobuf ? 0u : 15u)) {
        encode(rows, 5, protobuf, tail.length - 1, &short_by_one);
        check(short_by_one.ok && short_by_one.first == drop + 1 && short_by_one.length < tail.length,
              "one byte short drops the next oldest row");
      }
    }
    Encoded none;
    encode(rows, 5, protobuf, 0, &none);
    check(none.ok && none.first == 5 && none.length == (protobuf ? 0u : 15u), "nothing fits: empty list");
  }
}

// ---- the store: credentials and the history read ----

void check_store(const tj::Value* fixture) {
  const tj::Value* cases = tj::Get(fixture, "cases");
  const tj::Value* open_case = nullptr;
  for (uint32_t i = 0; i < cases->count; ++i)
    if (tj::StringEquals(tj::Get(cases->items[i], "name"), "openJson")) open_case = cases->items[i];
  check(open_case != nullptr, "fixture openJson");
  if (!open_case) return;
  tj::Arena arena = tj::GrowingArena(64u << 20);
  const tj::Value* request_text = tj::Get(open_case, "requestText");
  const tj::Value* request = tj::Parse(arena, reinterpret_cast<const uint8_t*>(request_text->text),
                                       request_text->length);
  auto text = [&](const char* name) { return tj::Get(request, name)->text; };
  const tj::Value* snapshot_json = tj::Get(request, "snapshotJson");
  const char* secret = tj::Get(tj::Get(fixture, "env"), "SUPPORT_AUTH_SECRET")->text;
  const char* device = tj::Get(tj::Get(fixture, "devices"), "A")->text;
  const char* other_device = tj::Get(tj::Get(fixture, "devices"), "B")->text;
  const char* token = text("accessToken");
  const size_t token_length = strlen(token);

  char directory[] = "/tmp/fortis-ai-messages-XXXXXX";
  if (!mkdtemp(directory)) {
    check(false, "temporary directory");
    return;
  }
  {
    fortis::storage::KvStore kv;
    check(kv.Open(directory), "KV opens");
    ai::Store store;
    // The production id generator (monotonic CSPRNG ULIDs).
    check(store.Start(&kv, secret, strlen(secret), &g_clock, nullptr), "store starts");
    g_now = static_cast<int64_t>(tj::Get(open_case, "at")->number);
    const char* first = "What do I hold?";
    ai::OpenInput input{device, text("address"), text("nonce"), text("timestamp"), text("snapshotHash"),
                        snapshot_json->text, snapshot_json->length, token, text("accessTokenHash"),
                        text("signature"), first, strlen(first), text("clientMessageId")};
    ai::Opened opened;
    bool storage_error = false;
    check(store.Open(input, &opened, &storage_error), "fixture proof opens");
    check(opened.expires_at_ms == 0, "thread has no expiry");
    char job_status[16]{};
    char job_conversation[ai::kIdBytes]{}, job_message[ai::kIdBytes]{};
    check(store.JobStatus(opened.id, opened.message_id, job_status, sizeof(job_status)) &&
          strcmp(job_status, "pending") == 0, "opening turn commits pending job");
    check(store.NextPendingJob(job_conversation, job_message) &&
          strcmp(job_conversation, opened.id) == 0 && strcmp(job_message, opened.message_id) == 0,
          "worker claims opening turn");
    check(store.JobStatus(opened.id, opened.message_id, job_status, sizeof(job_status)) &&
          strcmp(job_status, "running") == 0, "claimed job is running");
    check(store.ResetRunningJobs() &&
          store.JobStatus(opened.id, opened.message_id, job_status, sizeof(job_status)) &&
          strcmp(job_status, "pending") == 0, "restart requeues running job");
    check(store.SetJobStatus(opened.id, opened.message_id, "failed") &&
          store.JobStatus(opened.id, opened.message_id, job_status, sizeof(job_status)) &&
          strcmp(job_status, "failed") == 0, "provider failure is durable");
    check(store.SetJobStatus(opened.id, opened.message_id, "pending"), "retry requeues same turn");
    ai::MessageView opening_row;
    const char* opening_snapshot = nullptr;
    size_t opening_snapshot_length = 0;
    check(store.LoadMessage(opened.id, opened.message_id, arena, &opening_row,
                            &opening_snapshot, &opening_snapshot_length) &&
              opening_row.has_attachment && opening_row.has_snapshot &&
              strcmp(opening_row.snapshot_hash, text("snapshotHash")) == 0 &&
              opening_snapshot_length > 0,
          "opening message links to its portfolio attachment payload");

    // Credential policy (authorize()).
    check(store.Authorize(opened.id, device, token, token_length), "owner device + token");
    check(!store.Authorize(opened.id, other_device, token, token_length), "other device refused");
    char altered[96];
    snprintf(altered, sizeof(altered), "%s", token);
    altered[token_length - 1] = altered[token_length - 1] == 'A' ? 'B' : 'A';
    check(!store.Authorize(opened.id, device, altered, token_length), "other well-shaped token refused");
    check(!store.Authorize(opened.id, device, token, token_length - 1), "short token refused");
    check(!store.Authorize(opened.id, device, "", 0), "empty token refused");
    snprintf(altered, sizeof(altered), "%s ", token);
    check(!store.Authorize(opened.id, device, altered, token_length + 1), "token with a trailing space refused");
    check(!store.Authorize("01JZ9999999999999999999999", device, token, token_length), "unknown id refused");

    // History: 105 user turns in ONE millisecond, then their order.
    for (int i = 0; i < 105; ++i) {
      char client[33], body[16], message_id[27];
      snprintf(client, sizeof(client), "%032x", i + 1);
      snprintf(body, sizeof(body), "turn %03d", i);
      check(store.AddMessage(opened.id, client, body, strlen(body), nullptr, 0, nullptr, message_id) ==
                ai::AddStatus::kAdded,
            "turn saved");
    }
    ai::MessageView* rows = nullptr;
    size_t count = 0;
    check(store.Messages(opened.id, arena, &rows, &count) && count == ai::kMaxHistory, "newest 100 rows");
    bool ordered = count == ai::kMaxHistory;
    for (size_t i = 0; ordered && i < count; ++i) {
      char expected[16];
      snprintf(expected, sizeof(expected), "turn %03zu", i + 5);
      ordered = rows[i].body_length == strlen(expected) && memcmp(rows[i].body, expected, strlen(expected)) == 0 &&
                strcmp(rows[i].role, "user") == 0 && rows[i].has_attachment &&
                strcmp(rows[i].snapshot_hash, text("snapshotHash")) == 0 &&
                rows[i].created_at_ms == g_now &&
                (i == 0 || strcmp(rows[i - 1].id, rows[i].id) < 0);
    }
    check(ordered, "oldest first, creation order within one millisecond, first 6 rows dropped");

    // Cursor pages cover the complete persisted history, including messages
    // older than the legacy 100-row tail, with no overlap.
    char cursor[ai::kIdBytes] = "";
    char previous_oldest[ai::kIdBytes] = "";
    size_t paged = 0;
    bool more = true;
    for (size_t page = 0; page < 6 && more; ++page) {
      ai::MessageView* slice = nullptr;
      size_t size = 0;
      check(store.MessagePage(opened.id, cursor[0] ? cursor : nullptr, 20,
                              arena, &slice, &size, &more), "cursor page read");
      check(size == (page < 5 ? 20u : 6u), "cursor page size");
      if (size) {
        check(!previous_oldest[0] || strcmp(slice[size - 1].id, previous_oldest) < 0,
              "cursor pages do not overlap");
        snprintf(previous_oldest, sizeof(previous_oldest), "%s", slice[0].id);
        snprintf(cursor, sizeof(cursor), "%s", slice[0].id);
      }
      paged += size;
    }
    check(paged == 106 && !more, "all saved turns paged to the beginning");

    // Reading changes nothing: same rows, same KV file size.
    char data_path[512];
    snprintf(data_path, sizeof(data_path), "%s/fortis.kv", directory);
    FILE* file = fopen(data_path, "rb");
    long before = -1, after = -2;
    if (file) { fseek(file, 0, SEEK_END); before = ftell(file); fclose(file); }
    ai::MessageView* again = nullptr;
    size_t again_count = 0;
    check(store.Authorize(opened.id, device, token, token_length) &&
              store.Messages(opened.id, arena, &again, &again_count) && again_count == count &&
              strcmp(again[0].id, rows[0].id) == 0,
          "repeat read identical");
    file = fopen(data_path, "rb");
    if (file) { fseek(file, 0, SEEK_END); after = ftell(file); fclose(file); }
    check(before == after && before > 0, "authorize + messages write nothing");

    // A dormant thread remains readable well beyond the former 7-day limit.
    g_now += 365LL * 24 * 60 * 60 * 1000;
    check(store.Authorize(opened.id, device, token, token_length), "thread remains authorized after a year");
    check(store.Messages(opened.id, arena, &rows, &count) && count == ai::kMaxHistory,
          "history remains after a year");
    store.Stop();
  }
  // A reopened KV (restart) lists the same history before expiry.
  {
    fortis::storage::KvStore kv;
    check(kv.Open(directory), "KV reopens");
    ai::Store store;
    check(store.Start(&kv, secret, strlen(secret), &g_clock, nullptr), "store restarts");
    g_now = static_cast<int64_t>(tj::Get(open_case, "at")->number) + 1000;
    ai::MessageView* rows = nullptr;
    size_t count = 0;
    char id[27] = "";
    // The id is the only conversation's; find it through the history keys.
    struct Finder {
      static bool visit(const char* key, const void*, size_t, void* context) {
        snprintf(static_cast<char*>(context), 27, "%.26s", key + strlen("ai/conversation/"));
        return false;
      }
    };
    kv.Scan("ai/conversation/", nullptr, 1, g_now, [&, visitor_state = id](const char* key, const void* value, size_t size) { return (Finder::visit)(key, value, size, visitor_state); }, false);
    check(strlen(id) == 26 && store.Authorize(id, device, token, token_length) &&
              store.Messages(id, arena, &rows, &count) && count == ai::kMaxHistory &&
              rows[count - 1].body_length == 8 && memcmp(rows[count - 1].body, "turn 104", 8) == 0,
          "history survives a reopen");
    // A corrupt conversation record is an internal storage failure, not an
    // unknown id. Both GET and POST use this authorization result.
    char key[80];
    snprintf(key, sizeof(key), "ai/conversation/%s", id);
    const char invalid[] = "{broken";
    const fortis::storage::KvMutation corrupt{key, invalid, sizeof(invalid) - 1, g_now + 60000, false};
    check(kv.Write(&corrupt, 1), "corrupt conversation fixture written");
    bool auth_error = false;
    check(!store.Authorize(id, device, token, token_length, &auth_error) && auth_error,
          "corrupt conversation distinguishes storage error from denial");
    auth_error = true;
    check(!store.Authorize(id, device, "bad", 3, &auth_error) && !auth_error,
          "malformed bearer is denial even with corrupt storage");
    auth_error = true;
    check(!store.Authorize("01JZ9999999999999999999999", device, token, token_length, &auth_error) && !auth_error,
          "missing conversation is denial without storage error");
    store.Stop();
  }
  tj::FreeArena(arena);
  fortis::test::RemoveTree(directory);
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
  tj::Arena arena = tj::GrowingArena(256u << 20);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(fixture_text), fixture_length);
  if (!fixture) {
    fprintf(stderr, "fixture does not parse\n");
    return 2;
  }
  check_policy();
  check_fixture_bodies(tj::Get(fixture, "cases"));
  check_owned_output_alias();
  check_shapes();
  check_limit();
  check_store(fixture);
  tj::FreeArena(arena);
  free(fixture_text);
  if (g_failures) {
    fprintf(stderr, "ai_messages_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("ai_messages_test: id/bearer policy, body encoding, bound and store reads OK\n");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
