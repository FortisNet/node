// Store-level properties of the AI conversation repository
// (src/ai/ai_store.cpp) that the HTTP replay cannot observe directly:
//  - every record and index of a conversation expires at the instant fixed
//    at opening (a later snapshot update and reply do not extend it), and
//    the KV sweep then removes all of them;
//  - each change is one KV frame: truncating the data file anywhere inside
//    a change's frame leaves exactly the previous state, with every index
//    naming a stored record (torn write / power loss);
//  - the raw chat token, nonce and signature never reach the data file
//    (only the token's and nonce's SHA-256).
// The signed opening comes from the frozen route fixture (openJson).
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai_store.h"
#include "fake_seams.h"
#include "kv_store.h"
#include "token_json.h"

namespace {

namespace tj = fortis::codec::tj;
namespace ai = fortis::ai;

int64_t g_now = 0;
unsigned long long g_next_id = 0;
fortis::testing::PointerClock g_clock(&g_now);
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
  assert(data && fread(data, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size));
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

bool contains(const char* data, size_t length, const char* needle) {
  const size_t n = strlen(needle);
  for (size_t i = 0; i + n <= length; ++i)
    if (memcmp(data + i, needle, n) == 0) return true;
  return false;
}

struct Row {
  char key[256];
  char* value;
  size_t size;
};
struct Rows {
  Row items[256];
  size_t count = 0;
};
bool collect(const char* key, const void* value, size_t size, Rows* rows) {
  assert(rows->count < 256);
  Row& row = rows->items[rows->count++];
  snprintf(row.key, sizeof(row.key), "%s", key);
  row.value = static_cast<char*>(malloc(size + 1));
  memcpy(row.value, value, size);
  row.value[size] = '\0';
  row.size = size;
  return true;
}
void release(Rows& rows) {
  for (size_t i = 0; i < rows.count; ++i) free(rows.items[i].value);
  rows.count = 0;
}
bool has(fortis::storage::KvStore& kv, const char* key, int64_t now) {
  size_t size = 0;
  char probe[1];
  const fortis::storage::KvReadStatus status = kv.GetStatus(key, now, probe, 0, &size);
  return status == fortis::storage::KvReadStatus::Found || status == fortis::storage::KvReadStatus::BufferTooSmall;
}

// Every index names a stored record; every record's snapshot exists.
void check_invariants(fortis::storage::KvStore& kv, int64_t now) {
  Rows rows;
  assert(kv.Scan("ai/", nullptr, 256, now, [&, visitor_state = &rows](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }));
  for (size_t i = 0; i < rows.count; ++i) {
    const Row& row = rows.items[i];
    char key[300];
    if (!strncmp(row.key, "ai/nonce/", 9) || !strncmp(row.key, "ai/token/", 9)) {
      snprintf(key, sizeof(key), "ai/conversation/%s", row.value);
      assert(has(kv, key, now));
    } else if (!strncmp(row.key, "ai/client/", 10)) {
      char id[27];
      memcpy(id, row.key + 10, 26);
      id[26] = '\0';
      snprintf(key, sizeof(key), "ai/message/%s/%s", id, row.value);
      assert(has(kv, key, now));
    } else if (!strncmp(row.key, "ai/message/", 11) || !strncmp(row.key, "ai/conversation/", 16)) {
      const bool message = row.key[3] == 'm';
      char id[27];
      memcpy(id, row.key + (message ? 11 : 16), 26);
      id[26] = '\0';
      tj::Arena arena = tj::GrowingArena(1 << 20);
      const tj::Value* value = tj::Parse(arena, reinterpret_cast<const uint8_t*>(row.value), row.size);
      assert(value);
      const tj::Value* hash = tj::Get(value, "snapshotHash");
      if (tj::IsString(hash)) {
        snprintf(key, sizeof(key), "ai/snapshot/%s/%s", id, hash->text);
        assert(has(kv, key, now));
      }
      if (message) {
        snprintf(key, sizeof(key), "ai/conversation/%s", id);
        assert(has(kv, key, now));
      }
      tj::FreeArena(arena);
    }
  }
  release(rows);
}

size_t key_count(fortis::storage::KvStore& kv, int64_t now) {
  Rows rows;
  assert(kv.Scan("ai/", nullptr, 256, now, [&, visitor_state = &rows](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }));
  const size_t count = rows.count;
  release(rows);
  return count;
}

off_t file_size(const char* path) {
  struct stat info{};
  assert(stat(path, &info) == 0);
  return info.st_size;
}

}  // namespace

int main(int argc, char** argv) {
  const char* fixture_path = argc > 1 ? argv[1] : "tests/corpus/ai_conversations_route.json";
  size_t fixture_length = 0;
  char* fixture_text = read_file(fixture_path, &fixture_length);
  assert(fixture_text);
  tj::Arena fixture_arena = tj::GrowingArena(256u << 20);
  const tj::Value* fixture = tj::Parse(fixture_arena, reinterpret_cast<const uint8_t*>(fixture_text), fixture_length);
  assert(fixture);
  const tj::Value* cases = tj::Get(fixture, "cases");
  const tj::Value* open_case = nullptr;
  for (uint32_t i = 0; i < cases->count; ++i)
    if (tj::StringEquals(tj::Get(cases->items[i], "name"), "openJson")) open_case = cases->items[i];
  assert(open_case);
  const tj::Value* request_text = tj::Get(open_case, "requestText");
  const tj::Value* request = tj::Parse(fixture_arena, reinterpret_cast<const uint8_t*>(request_text->text),
                                       request_text->length);
  auto text = [&](const char* name) { return tj::Get(request, name)->text; };
  const tj::Value* snapshot_json = tj::Get(request, "snapshotJson");
  const char* secret = tj::Get(tj::Get(fixture, "env"), "SUPPORT_AUTH_SECRET")->text;
  const char* device = tj::Get(tj::Get(fixture, "devices"), "A")->text;

  char directory[] = "/tmp/fortis-ai-store-XXXXXX";
  assert(mkdtemp(directory));
  char data_path[512];
  snprintf(data_path, sizeof(data_path), "%s/fortis.kv", directory);
  // File size after each acknowledged change (index 0: the empty store).
  off_t sizes[8];
  size_t counts[8];
  size_t changes = 0;
  int64_t expiry = 0;
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    ai::Store store;
    assert(store.Start(&kv, secret, strlen(secret), &g_clock, &g_ids));
    g_now = static_cast<int64_t>(tj::Get(open_case, "at")->number);
    sizes[changes] = file_size(data_path);
    counts[changes++] = key_count(kv, g_now);

    const char* body = "What do I hold?";
    ai::OpenInput input{device, text("address"), text("nonce"), text("timestamp"), text("snapshotHash"),
                        snapshot_json->text, snapshot_json->length, text("accessToken"), text("accessTokenHash"),
                        text("signature"), body, strlen(body), text("clientMessageId")};
    ai::Opened opened;
    bool storage_error = false;
    assert(store.Open(input, &opened, &storage_error));
    expiry = opened.expires_at_ms;
    assert(expiry == g_now + ai::kConversationLifetimeMs);
    sizes[changes] = file_size(data_path);
    counts[changes++] = key_count(kv, g_now);
    // A replay writes nothing and keeps the id.
    ai::Opened replay;
    assert(store.Open(input, &replay, &storage_error) && strcmp(replay.id, opened.id) == 0);
    assert(file_size(data_path) == sizes[changes - 1]);

    char reply_id[27];
    bool created = false;
    assert(store.SaveReply(opened.id, opened.message_id, "First reply", 11, reply_id, &created) && created);
    sizes[changes] = file_size(data_path);
    counts[changes++] = key_count(kv, g_now);

    // Six days later: a turn with a snapshot update (same content, new hash).
    g_now += 6LL * 24 * 3600 * 1000;
    const char* updated = "{\"version\":1,\"capturedAt\":\"2026-01-08T00:00:00Z\",\"balancesComplete\":true,"
                          "\"wallets\":[{\"id\":\"hd:evm:0\",\"address\":\"0x1C5A77d9FA7eF466951B2F01F724BCa3A5820b63\","
                          "\"family\":\"evm\",\"networkIds\":[],\"watchOnly\":false}],\"tokens\":[],\"holdings\":[]}";
    char hash[65];
    ai::Sha256Hex(updated, strlen(updated), hash);
    char message_id[27];
    assert(store.AddMessage(opened.id, "0123456789abcdef0123456789abcdef", "Six days on", 11, updated,
                            strlen(updated), hash, message_id) == ai::AddStatus::kAdded);
    sizes[changes] = file_size(data_path);
    counts[changes++] = key_count(kv, g_now);
    assert(store.SaveReply(opened.id, message_id, "Second reply", 12, reply_id, &created) && created);
    sizes[changes] = file_size(data_path);
    counts[changes++] = key_count(kv, g_now);
    // The same clientMessageId reuses the row and writes nothing.
    char again[27];
    assert(store.AddMessage(opened.id, "0123456789abcdef0123456789abcdef", "other", 5, nullptr, 0, nullptr,
                            again) == ai::AddStatus::kAdded &&
           strcmp(again, message_id) == 0);
    assert(file_size(data_path) == sizes[changes - 1]);

    // The lifetime is fixed at opening: everything is visible one
    // millisecond before it and nothing at it.
    const size_t live = key_count(kv, expiry - 1);
    // 6 at opening, 2 per reply, 3 for a turn with a new snapshot.
    assert(live == counts[changes - 1] && live == 13);
    check_invariants(kv, expiry - 1);
    assert(key_count(kv, expiry) == 0);
    store.Stop();
    kv.Close();
  }

  // Secrets never reach the data file.
  {
    size_t length = 0;
    char* data = read_file(data_path, &length);
    assert(!contains(data, length, text("accessToken")));
    assert(!contains(data, length, text("signature") + 2));
    assert(!contains(data, length, text("nonce")));
    free(data);
  }

  // Torn writes: a truncation inside change k's frame leaves state k-1.
  size_t length = 0;
  char* data = read_file(data_path, &length);
  size_t truncations = 0;
  for (size_t k = 1; k < changes; ++k) {
    const off_t from = sizes[k - 1], to = sizes[k];
    const off_t points[] = {from + 1, from + (to - from) / 2, to - 1};
    for (off_t cut : points) {
      char torn[] = "/tmp/fortis-ai-torn-XXXXXX";
      assert(mkdtemp(torn));
      char path[512];
      snprintf(path, sizeof(path), "%s/fortis.kv", torn);
      const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
      assert(fd >= 0 && write(fd, data, static_cast<size_t>(cut)) == cut);
      close(fd);
      fortis::storage::KvStore kv;
      assert(kv.Open(torn));
      assert(key_count(kv, expiry - 1) == counts[k - 1]);
      check_invariants(kv, expiry - 1);
      kv.Close();
      ++truncations;
      unlink(path);
      char lock[512];
      snprintf(lock, sizeof(lock), "%s/fortis.kv.lock", torn);
      unlink(lock);
      rmdir(torn);
    }
  }
  free(data);

  // The sweep removes every expired AI record.
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    assert(kv.Size() == 13);
    assert(kv.Sweep(expiry - 1, 256) == 0 && kv.Size() == 13);
    assert(kv.Sweep(expiry, 256) == 13 && kv.Size() == 0);
    kv.Close();
  }
  tj::FreeArena(fixture_arena);
  free(fixture_text);
  printf("ai_store_test: 13 records share one expiry; %zu torn-write truncations keep the previous state\n",
         truncations);
  return 0;
}
