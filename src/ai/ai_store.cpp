#include "ownership.h"
#include "utf8.h"
#include "ai_store.h"

#include "ai_service.h"
#include "ai_reply.h"
#include "ai_snapshot.h"
#include "crash_report_codec.h"
#include "device_auth.h"
#include "diagnostic_redaction.h"
#include "js_date_parse.h"
#include "kv_store.h"
#include "price_history_batch_codec.h"
#include "threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::ai {

struct Store::Conversation {
  char owner_id[kHashHexBytes]{};
  char device_id[kIdBytes]{};
  char nonce_hash[kHashHexBytes]{};
  char token_hash[kHashHexBytes]{};
  char snapshot_hash[kHashHexBytes]{};
  int64_t created_at_ms = 0;
  int64_t updated_at_ms = 0;
  int64_t expires_at_ms = 0;
};

namespace {

using tj::OutBuffer;
using tj::Value;

constexpr size_t kKeyBytes = 256;

// Called under Store::mutex_ so counter reads and the message frame cannot
// interleave with another AI writer. One checksummed frame commits both.
bool write_with_stats(storage::KvStore& kv, const storage::KvMutation* base, size_t base_count,
                      const char* owner, int64_t now, StatsEvent event,
                      const TokenUsage& usage = {}) {
  if (base_count > 8) return false;
  StatsWrites stats;
  if (!PrepareStatsWrites(kv, owner, now, event, usage, &stats)) return false;
  storage::KvMutation writes[14];
  memcpy(writes, base, base_count * sizeof(storage::KvMutation));
  memcpy(writes + base_count, stats.mutations, stats.count * sizeof(storage::KvMutation));
  return kv.Write(writes, base_count + stats.count);
}

void conversation_key(const char* id, char key[kKeyBytes]) { snprintf(key, kKeyBytes, "ai/conversation/%s", id); }
void owner_key(const char* owner, char key[kKeyBytes]) { snprintf(key, kKeyBytes, "ai/owner/%s", owner); }
void nonce_key(const char* hash, char key[kKeyBytes]) { snprintf(key, kKeyBytes, "ai/nonce/%s", hash); }
void token_key(const char* hash, char key[kKeyBytes]) { snprintf(key, kKeyBytes, "ai/token/%s", hash); }
void snapshot_key(const char* id, const char* hash, char key[kKeyBytes]) {
  snprintf(key, kKeyBytes, "ai/snapshot/%s/%s", id, hash);
}
void message_key(const char* id, const char* message_id, char key[kKeyBytes]) {
  snprintf(key, kKeyBytes, "ai/message/%s/%s", id, message_id);
}
void message_prefix(const char* id, char key[kKeyBytes]) { snprintf(key, kKeyBytes, "ai/message/%s/", id); }
// encodeURIComponent of a 32-hex client id is itself; the assistant's
// `assistant:${userMessageId}` encodes its colon.
void client_key(const char* id, const char* client_id, char key[kKeyBytes]) {
  snprintf(key, kKeyBytes, "ai/client/%s/%s", id, client_id);
}
void job_key(const char* id, const char* user_message_id, char key[kKeyBytes]) {
  snprintf(key, kKeyBytes, "ai/job/%s/%s", id, user_message_id);
}
void reply_key(const char* id, const char* user_message_id, char key[kKeyBytes]) {
  snprintf(key, kKeyBytes, "ai/client/%s/assistant%%3A%s", id, user_message_id);
}

bool copy_text(const Value* v, char* output, size_t capacity) {
  if (!tj::IsString(v) || v->length >= capacity) return false;
  memcpy(output, v->text, v->length);
  output[v->length] = '\0';
  return true;
}

bool number(const Value* v, int64_t* output) {
  if (!tj::IsNumber(v) || !tj::IsFinite(v)) return false;
  *output = static_cast<int64_t>(v->number);
  return true;
}

// Serializes `value` into the arena.
bool stringify(const Value* value, tj::Arena& arena, const uint8_t** bytes, size_t* length) {
  OutBuffer counter{};
  if (!tj::Stringify(value, counter) || !counter.ok) return false;
  uint8_t* out = static_cast<uint8_t*>(arena.Alloc(counter.used + 1, 1));
  if (!out) return false;
  OutBuffer buffer{out, counter.used + 1};
  if (!tj::Stringify(value, buffer) || !buffer.ok) return false;
  *bytes = out;
  *length = buffer.used;
  return true;
}

// The stored form of a message body: valid UTF-8, each lone surrogate one
// U+FFFD (the response that created the row still carries the original
// text).
const char* storable(const char* text, size_t length, tj::Arena& arena, size_t* output_length) {
  const size_t size = fortis::codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, nullptr, 0);
  char* out = static_cast<char*>(arena.Alloc(size + 1, 1));
  if (!out) return nullptr;
  fortis::codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, out, size);
  out[size] = '\0';
  if (output_length) *output_length = size;
  return out;
}

bool encode_message(tj::Arena& arena, const char* id, const char* client_id, const char* role, const char* body,
                    size_t body_length, const char* snapshot_hash, const Value* snapshot,
                    int64_t created_at_ms, const uint8_t** bytes, size_t* length) {
  Value* row = tj::Obj(arena);
  Value* attachment = nullptr;
  if (snapshot_hash) {
    const Value* captured = tj::Get(snapshot, "capturedAt");
    attachment = tj::Obj(arena);
    if (!tj::IsString(captured) || !attachment ||
        !tj::Set(arena, attachment, "kind", tj::Str(arena, "portfolio_snapshot")) ||
        !tj::Set(arena, attachment, "version", tj::Get(snapshot, "version")) ||
        !tj::Set(arena, attachment, "sha256", tj::Str(arena, snapshot_hash)) ||
        !tj::Set(arena, attachment, "capturedAt", tj::Str(arena, captured->text, captured->length)))
      return false;
  }
  return row && tj::Set(arena, row, "id", tj::Str(arena, id)) &&
         tj::Set(arena, row, "clientMessageId", tj::Str(arena, client_id)) &&
         tj::Set(arena, row, "role", tj::Str(arena, role)) &&
         tj::Set(arena, row, "body", tj::Str(arena, body, body_length)) &&
         tj::Set(arena, row, "snapshotHash", snapshot_hash ? tj::Str(arena, snapshot_hash) : tj::Null(arena)) &&
         tj::Set(arena, row, "attachment", attachment ? attachment : tj::Null(arena)) &&
         tj::Set(arena, row, "createdAt", tj::Num(arena, static_cast<double>(created_at_ms))) &&
         stringify(row, arena, bytes, length);
}

bool decode_message(const uint8_t* bytes, size_t length, tj::Arena& arena, MessageView* row) {
  const Value* value = tj::Parse(arena, bytes, length);
  const Value* body = tj::Get(value, "body");
  const Value* hash = tj::Get(value, "snapshotHash");
  const Value* attachment = tj::Get(value, "attachment");
  if (!copy_text(tj::Get(value, "id"), row->id, sizeof(row->id)) ||
      !copy_text(tj::Get(value, "role"), row->role, sizeof(row->role)) ||
      !copy_text(tj::Get(value, "clientMessageId"), row->client_message_id, sizeof(row->client_message_id)) ||
      !tj::IsString(body) || !number(tj::Get(value, "createdAt"), &row->created_at_ms))
    return false;
  row->body = body->text;
  row->body_length = body->length;
  row->has_snapshot = tj::IsString(hash);
  if (row->has_snapshot && !copy_text(hash, row->snapshot_hash, sizeof(row->snapshot_hash))) return false;
  row->has_attachment = tj::IsObject(attachment);
  if (row->has_attachment) {
    const Value* digest = tj::Get(attachment, "sha256");
    const Value* version = tj::Get(attachment, "version");
    if (!row->has_snapshot || !tj::StringEquals(tj::Get(attachment, "kind"), "portfolio_snapshot") ||
        !tj::IsNumber(version) || (version->number != 1 && version->number != 2) ||
        !tj::IsString(digest) || digest->length != strlen(row->snapshot_hash) ||
        memcmp(digest->text, row->snapshot_hash, digest->length) != 0 ||
        !copy_text(tj::Get(attachment, "capturedAt"), row->attachment_captured_at,
                   sizeof(row->attachment_captured_at)))
      return false;
  }
  return true;
}

// Scan visitor: copies each value into the arena (the store mutex is held;
// the visitor must not touch the KV).
struct Collected {
  const uint8_t* bytes;
  size_t length;
};
struct Collector {
  tj::Arena* arena;
  Collected* rows;
  size_t count;
  size_t capacity;
  bool ok;
};
bool collect(const char*, const void* value, size_t size, Collector* collector) {
  if (collector->count == collector->capacity) return false;
  uint8_t* copy = static_cast<uint8_t*>(collector->arena->Alloc(size + 1, 1));
  if (!copy) { collector->ok = false; return false; }
  memcpy(copy, value, size);
  collector->rows[collector->count++] = {copy, size};
  return true;
}

bool chat_token_shape(const char* token, size_t length) {
  static const char kPrefix[] = "fintel_chat_";
  if (length != sizeof(kPrefix) - 1 + 43 || memcmp(token, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
  for (size_t i = sizeof(kPrefix) - 1; i < length; ++i) {
    const char c = token[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return true;
}

}  // namespace

bool NewMonotonicUlid(int64_t now_ms, int64_t* last_ms, uint8_t last_random[10], char output[kIdBytes]) {
  if (now_ms > *last_ms) {
    if (!auth::SecureRandomBytes(last_random, 10)) return false;
    *last_ms = now_ms;
  } else {
    // The same (or an earlier) millisecond: increment the 80 random bits.
    int i = 9;
    while (i >= 0 && ++last_random[i] == 0) --i;
    if (i < 0) return false;
  }
  return fortis::codec::crash_report::NewUlid(*last_ms, last_random, output);
}

Store::~Store() { Stop(); }

bool Store::Start(storage::KvStore* kv, const char* secret, size_t secret_length, base::Clock* clock, base::IdSource* new_id) {
  if (started_ || !kv || !clock || !secret || !secret_length || secret_length >= sizeof(secret_)) return false;
  kv_ = kv;
  memcpy(secret_, secret, secret_length);
  secret_length_ = secret_length;
  clock_ = clock;
  new_id_ = new_id;
  started_ = true;
  return true;
}

void Store::Stop() {
  if (!started_) return;
  memset(secret_, 0, sizeof(secret_));
  secret_length_ = 0;
  started_ = false;
}

bool Store::NewId(char output[kIdBytes]) {
  if (new_id_) return new_id_->NewId(output);
  return NewMonotonicUlid(clock_->NowMs(), &last_ms_, last_random_, output);
}

Store::Lookup Store::GetTextStatus(const char* key, int64_t now, tj::Arena& arena,
                                   const char** text, size_t* length) {
  size_t size = 0;
  char probe[1];
  storage::KvReadStatus status = kv_->GetStatus(key, now, probe, 0, &size);
  if (status == storage::KvReadStatus::Missing) return Lookup::kMissing;
  if (status != storage::KvReadStatus::BufferTooSmall && status != storage::KvReadStatus::Found) return Lookup::kError;
  // Each GetStatus is one consistent read under the KV mutex, but a writer
  // may replace the record between the size probe and the read: the
  // unlocked GET .../messages reads the conversation while an AddMessage
  // snapshot update rewrites it (updatedAt can gain a digit). A larger
  // record answers BufferTooSmall with its new size; retry at that size.
  // Bound the retries so a hot writer cannot hold an HTTP worker forever.
  char* buffer = nullptr;
  for (int attempt = 0;; ++attempt) {
    buffer = static_cast<char*>(arena.Alloc(size + 1, 1));
    if (!buffer) return Lookup::kError;
    status = kv_->GetStatus(key, now, buffer, size, &size);
    if (status != storage::KvReadStatus::BufferTooSmall || attempt == 3) break;
  }
  if (status == storage::KvReadStatus::Missing) return Lookup::kMissing;
  if (status != storage::KvReadStatus::Found) return Lookup::kError;
  buffer[size] = '\0';
  *text = buffer;
  *length = size;
  return Lookup::kFound;
}

bool Store::GetText(const char* key, int64_t now, tj::Arena& arena, const char** text, size_t* length) {
  return GetTextStatus(key, now, arena, text, length) == Lookup::kFound;
}

bool Store::LoadConversation(const char* id, int64_t now, tj::Arena& arena, Conversation* out,
                             bool* storage_error) {
  if (storage_error) *storage_error = false;
  char key[kKeyBytes];
  conversation_key(id, key);
  const char* text = nullptr;
  size_t length = 0;
  const Lookup lookup = GetTextStatus(key, now, arena, &text, &length);
  if (lookup != Lookup::kFound) {
    if (storage_error) *storage_error = lookup == Lookup::kError;
    return false;
  }
  const Value* v = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  const bool decoded = copy_text(tj::Get(v, "ownerId"), out->owner_id, sizeof(out->owner_id)) &&
         copy_text(tj::Get(v, "deviceId"), out->device_id, sizeof(out->device_id)) &&
         copy_text(tj::Get(v, "proofNonceHash"), out->nonce_hash, sizeof(out->nonce_hash)) &&
         copy_text(tj::Get(v, "accessTokenHash"), out->token_hash, sizeof(out->token_hash)) &&
         copy_text(tj::Get(v, "snapshotHash"), out->snapshot_hash, sizeof(out->snapshot_hash)) &&
         number(tj::Get(v, "createdAt"), &out->created_at_ms) &&
         number(tj::Get(v, "updatedAt"), &out->updated_at_ms) &&
         number(tj::Get(v, "expiresAt"), &out->expires_at_ms);
  if (storage_error) *storage_error = !decoded;
  return decoded;
}

bool Store::Open(const OpenInput& input, Opened* opened, bool* storage_error) {
  *storage_error = false;
  if (!started_) { *storage_error = true; return false; }
  const int64_t now = clock_->NowMs();
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);

  // validateConversationOpen(input, now)
  // The timestamp is ASCII (zod datetime), so its UTF-16 units are its bytes.
  const size_t timestamp_length = strlen(input.timestamp);
  uint16_t* wide = static_cast<uint16_t*>(arena.Alloc(sizeof(uint16_t) * (timestamp_length + 1)));
  if (!wide) { *storage_error = true; return false; }
  for (size_t i = 0; i < timestamp_length; ++i) wide[i] = static_cast<uint8_t>(input.timestamp[i]);
  double signed_at = 0;
  if (!compat::JsDateParse(wide, timestamp_length, &signed_at)) return false;
  const double age = static_cast<double>(now) - signed_at;
  if (age > kProofTimestampToleranceMs || age < -kProofTimestampToleranceMs) return false;
  char hash[kHashHexBytes];
  Sha256Hex(input.snapshot_json, input.snapshot_json_length, hash);
  if (strcmp(hash, input.snapshot_hash) != 0) return false;
  if (!chat_token_shape(input.access_token, strlen(input.access_token))) return false;
  Sha256Hex(input.access_token, strlen(input.access_token), hash);
  if (strcmp(hash, input.access_token_hash) != 0) return false;
  const Value* snapshot = ParseSnapshot(input.snapshot_json, input.snapshot_json_length, arena);
  if (!snapshot) return false;
  char primary[kAddressBytes], recovered[kAddressBytes];
  const size_t message_capacity = 512 + timestamp_length;
  char* message = static_cast<char*>(arena.Alloc(message_capacity, 1));
  if (!message) { *storage_error = true; return false; }
  const size_t message_length = ProofMessage(input.address, input.device_id, input.nonce, input.timestamp,
                                             input.snapshot_hash, input.access_token_hash, message, message_capacity);
  if (!message_length || !ChecksumAddress(input.address, primary) ||
      !VerifyMessage(message, message_length, input.signature, recovered) || strcmp(primary, recovered) != 0)
    return false;
  char primary_lower[kAddressBytes];
  for (size_t i = 0; i < kAddressBytes; ++i)
    primary_lower[i] = primary[i] >= 'A' && primary[i] <= 'F' ? static_cast<char>(primary[i] - 'A' + 'a') : primary[i];
  const Value* wallets = tj::Get(snapshot, "wallets");
  bool signer_listed = false;
  for (uint32_t i = 0; !signer_listed && i < wallets->count; ++i) {
    const Value* wallet = wallets->items[i];
    const Value* address = tj::Get(wallet, "address");
    signer_listed = tj::StringEquals(tj::Get(wallet, "family"), "evm") &&
                    tj::Get(wallet, "watchOnly")->type == tj::Type::kFalse &&
                    LowercaseEquals(address->text, address->length, primary_lower);
  }
  if (!signer_listed || !SnapshotLinksValid(snapshot)) return false;

  char nonce_hash[kHashHexBytes], owner[kHashHexBytes];
  Sha256Hex(input.nonce, strlen(input.nonce), nonce_hash);
  std::unique_ptr<char[], base::StorageDeleter> body_owner;
  size_t body_length = 0;
  bool redacted = false;
  const bool body_ok = compat::redaction::RedactSecrets(input.first_message, input.first_message_length, &body_owner, &body_length, &redacted);
  char* body = body_owner.get();
  if (!body_ok ||
      !OwnerId(secret_, secret_length_, primary, strlen(primary), owner)) {
    *storage_error = true;
    return false;
  }
  const int64_t expiry = 0;  // Persistent thread; zero is the KV's no-expiry marker.
  // NewMonotonicUlid mutates last_ms_ and last_random_. Keep ID generation
  // inside the same store lock as the nonce check and write. The source also
  // consumes two IDs on a validated replay, so generate them before that
  // check while holding the lock.
  fortis::base::LockGuard guard(mutex_);
  char id[kIdBytes], first_id[kIdBytes];
  if (!NewId(id) || !NewId(first_id)) { *storage_error = true; return false; }

  char key[kKeyBytes];
  nonce_key(nonce_hash, key);
  const char* replay = nullptr;
  size_t replay_length = 0;
  const Lookup replay_status = GetTextStatus(key, now, arena, &replay, &replay_length);
  if (replay_status == Lookup::kError) { *storage_error = true; return false; }
  if (replay_status == Lookup::kFound) {
    Conversation existing;
    char replay_id[kIdBytes];
    if (replay_length != kIdBytes - 1) return false;
    memcpy(replay_id, replay, replay_length + 1);
    char client[kKeyBytes];
    client_key(replay_id, input.client_message_id, client);
    const char* first_message_id = nullptr;
    size_t first_length = 0;
    MessageView first;
    // The source's three tx.get reads run before any comparison, and a read
    // or decode failure throws (HTTP 500); only absent records mean
    // PROOF_INVALID.
    bool conversation_error = false;
    const bool has_conversation = LoadConversation(replay_id, now, arena, &existing, &conversation_error);
    const Lookup client_status = GetTextStatus(client, now, arena, &first_message_id, &first_length);
    const Lookup first_status = client_status != Lookup::kFound ? Lookup::kMissing
                                : first_length != kIdBytes - 1 ? Lookup::kMissing
                                : GetMessage(replay_id, first_message_id, now, arena, &first);
    if (conversation_error || client_status == Lookup::kError || first_status == Lookup::kError) {
      *storage_error = true;
      return false;
    }
    if (!has_conversation || first_status != Lookup::kFound ||
        strcmp(existing.device_id, input.device_id) != 0 ||
        strcmp(existing.token_hash, input.access_token_hash) != 0 ||
        strcmp(existing.snapshot_hash, input.snapshot_hash) != 0 ||
        (existing.expires_at_ms && existing.expires_at_ms <= now))
      return false;
    memcpy(opened->id, replay_id, kIdBytes);
    memcpy(opened->message_id, first.id, kIdBytes);
    opened->expires_at_ms = existing.expires_at_ms;
    return true;
  }
  token_key(input.access_token_hash, key);
  const char* reused = nullptr;
  size_t reused_length = 0;
  const Lookup token_status = GetTextStatus(key, now, arena, &reused, &reused_length);
  if (token_status == Lookup::kError) { *storage_error = true; return false; }
  if (token_status == Lookup::kFound) return false;

  // A signed opening after local cache loss reuses the owner's thread and
  // rotates its chat token to the newly proved device. The old credential is
  // no longer authorized, while every prior message stays in the same thread.
  owner_key(owner, key);
  const char* owner_thread = nullptr;
  size_t owner_thread_length = 0;
  const Lookup owner_status = GetTextStatus(key, now, arena, &owner_thread, &owner_thread_length);
  if (owner_status == Lookup::kError) { *storage_error = true; return false; }
  if (owner_status == Lookup::kFound) {
    if (owner_thread_length != kIdBytes - 1) { *storage_error = true; return false; }
    char thread_id[kIdBytes];
    memcpy(thread_id, owner_thread, kIdBytes);
    Conversation previous;
    bool load_error = false;
    if (!LoadConversation(thread_id, now, arena, &previous, &load_error)) {
      *storage_error = true;
      return false;
    }
    Value* conversation = tj::Obj(arena);
    const uint8_t* conversation_bytes = nullptr;
    size_t conversation_length = 0;
    const uint8_t* message_bytes = nullptr;
    size_t message_length_bytes = 0;
    const uint8_t* snapshot_bytes = nullptr;
    size_t snapshot_length = 0;
    if (!conversation || !tj::Set(arena, conversation, "ownerId", tj::Str(arena, owner)) ||
        !tj::Set(arena, conversation, "deviceId", tj::Str(arena, input.device_id)) ||
        !tj::Set(arena, conversation, "proofNonceHash", tj::Str(arena, nonce_hash)) ||
        !tj::Set(arena, conversation, "accessTokenHash", tj::Str(arena, input.access_token_hash)) ||
        !tj::Set(arena, conversation, "snapshotHash", tj::Str(arena, input.snapshot_hash)) ||
        !tj::Set(arena, conversation, "createdAt", tj::Num(arena, static_cast<double>(previous.created_at_ms))) ||
        !tj::Set(arena, conversation, "updatedAt", tj::Num(arena, static_cast<double>(now))) ||
        !tj::Set(arena, conversation, "expiresAt", tj::Num(arena, 0)) ||
        !stringify(conversation, arena, &conversation_bytes, &conversation_length) ||
        !encode_message(arena, first_id, input.client_message_id, "user", body, body_length,
                        input.snapshot_hash, snapshot, now, &message_bytes, &message_length_bytes) ||
        !stringify(snapshot, arena, &snapshot_bytes, &snapshot_length)) {
      *storage_error = true;
      return false;
    }
    char keys[8][kKeyBytes];
    conversation_key(thread_id, keys[0]);
    nonce_key(nonce_hash, keys[1]);
    token_key(input.access_token_hash, keys[2]);
    message_key(thread_id, first_id, keys[3]);
    client_key(thread_id, input.client_message_id, keys[4]);
    snapshot_key(thread_id, input.snapshot_hash, keys[5]);
    token_key(previous.token_hash, keys[6]);
    job_key(thread_id, first_id, keys[7]);
    const storage::KvMutation writes[] = {
        {keys[0], conversation_bytes, static_cast<uint32_t>(conversation_length), 0, false},
        {keys[1], thread_id, kIdBytes - 1, 0, false},
        {keys[2], thread_id, kIdBytes - 1, 0, false},
        {keys[3], message_bytes, static_cast<uint32_t>(message_length_bytes), 0, false},
        {keys[4], first_id, kIdBytes - 1, 0, false},
        {keys[5], snapshot_bytes, static_cast<uint32_t>(snapshot_length), 0, false},
        {keys[6], nullptr, 0, 0, true},
        {keys[7], "pending", 7, 0, false},
    };
    if (!write_with_stats(*kv_, writes, 8, owner, now, StatsEvent::UserMessage)) {
      *storage_error = true; return false;
    }
    memcpy(opened->id, thread_id, kIdBytes);
    memcpy(opened->message_id, first_id, kIdBytes);
    opened->expires_at_ms = 0;
    return true;
  }

  Value* conversation = tj::Obj(arena);
  const uint8_t* conversation_bytes = nullptr;
  size_t conversation_length = 0;
  const uint8_t* message_bytes = nullptr;
  size_t message_length_bytes = 0;
  const uint8_t* snapshot_bytes = nullptr;
  size_t snapshot_length = 0;
  if (!conversation || !tj::Set(arena, conversation, "ownerId", tj::Str(arena, owner)) ||
      !tj::Set(arena, conversation, "deviceId", tj::Str(arena, input.device_id)) ||
      !tj::Set(arena, conversation, "proofNonceHash", tj::Str(arena, nonce_hash)) ||
      !tj::Set(arena, conversation, "accessTokenHash", tj::Str(arena, input.access_token_hash)) ||
      !tj::Set(arena, conversation, "snapshotHash", tj::Str(arena, input.snapshot_hash)) ||
      !tj::Set(arena, conversation, "createdAt", tj::Num(arena, static_cast<double>(now))) ||
      !tj::Set(arena, conversation, "updatedAt", tj::Num(arena, static_cast<double>(now))) ||
      !tj::Set(arena, conversation, "expiresAt", tj::Num(arena, static_cast<double>(expiry))) ||
      !stringify(conversation, arena, &conversation_bytes, &conversation_length) ||
      !encode_message(arena, first_id, input.client_message_id, "user", body, body_length, input.snapshot_hash, snapshot, now,
                      &message_bytes, &message_length_bytes) ||
      !stringify(snapshot, arena, &snapshot_bytes, &snapshot_length)) {
    *storage_error = true;
    return false;
  }
  char keys[8][kKeyBytes];
  conversation_key(id, keys[0]);
  nonce_key(nonce_hash, keys[1]);
  token_key(input.access_token_hash, keys[2]);
  message_key(id, first_id, keys[3]);
  client_key(id, input.client_message_id, keys[4]);
  snapshot_key(id, input.snapshot_hash, keys[5]);
  owner_key(owner, keys[6]);
  job_key(id, first_id, keys[7]);
  const storage::KvMutation writes[] = {
      {keys[0], conversation_bytes, static_cast<uint32_t>(conversation_length), expiry, false},
      {keys[1], id, kIdBytes - 1, expiry, false},
      {keys[2], id, kIdBytes - 1, expiry, false},
      {keys[3], message_bytes, static_cast<uint32_t>(message_length_bytes), expiry, false},
      {keys[4], first_id, kIdBytes - 1, expiry, false},
      {keys[5], snapshot_bytes, static_cast<uint32_t>(snapshot_length), expiry, false},
      {keys[6], id, kIdBytes - 1, expiry, false},
      {keys[7], "pending", 7, expiry, false},
  };
  if (!write_with_stats(*kv_, writes, 8, owner, now, StatsEvent::UserMessage)) {
    *storage_error = true; return false;
  }
  memcpy(opened->id, id, kIdBytes);
  memcpy(opened->message_id, first_id, kIdBytes);
  opened->expires_at_ms = expiry;
  return true;
}

bool Store::Authorize(const char* id, const char* device_id, const char* token, size_t token_length,
                      bool* storage_error) {
  if (storage_error) *storage_error = false;
  if (!started_ || !chat_token_shape(token, token_length)) return false;
  const int64_t now = clock_->NowMs();
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  Conversation conversation;
  char hash[kHashHexBytes];
  Sha256Hex(token, token_length, hash);
  const bool ok = LoadConversation(id, now, arena, &conversation, storage_error) &&
                  strcmp(conversation.device_id, device_id) == 0 && strcmp(conversation.token_hash, hash) == 0 &&
                  (!conversation.expires_at_ms || conversation.expires_at_ms > now);
  tj::FreeArena(arena);
  return ok;
}

bool Store::ConversationTimes(const char* id, int64_t* created_at_ms, int64_t* expires_at_ms) {
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  Conversation conversation;
  const bool ok = LoadConversation(id, clock_->NowMs(), arena, &conversation);
  if (ok) {
    *created_at_ms = conversation.created_at_ms;
    *expires_at_ms = conversation.expires_at_ms;
  }
  tj::FreeArena(arena);
  return ok;
}

Store::Lookup Store::GetMessage(const char* id, const char* message_id, int64_t now, tj::Arena& arena,
                                MessageView* row) {
  char key[kKeyBytes];
  message_key(id, message_id, key);
  const char* text = nullptr;
  size_t length = 0;
  const Lookup lookup = GetTextStatus(key, now, arena, &text, &length);
  if (lookup != Lookup::kFound) return lookup;
  return decode_message(reinterpret_cast<const uint8_t*>(text), length, arena, row) ? Lookup::kFound
                                                                                     : Lookup::kError;
}

bool Store::LoadMessage(const char* id, const char* message_id, tj::Arena& arena, MessageView* row,
                        const char** snapshot, size_t* snapshot_length) {
  if (strlen(message_id) != kIdBytes - 1) return false;
  char key[kKeyBytes];
  message_key(id, message_id, key);
  const char* text = nullptr;
  size_t length = 0;
  const int64_t now = clock_->NowMs();
  if (!GetText(key, now, arena, &text, &length) ||
      !decode_message(reinterpret_cast<const uint8_t*>(text), length, arena, row))
    return false;
  if (!snapshot) return true;
  *snapshot = nullptr;
  *snapshot_length = 0;
  if (!row->has_snapshot) return true;
  snapshot_key(id, row->snapshot_hash, key);
  return GetText(key, now, arena, snapshot, snapshot_length);
}

bool Store::Messages(const char* id, tj::Arena& arena, MessageView** rows, size_t* count) {
  bool has_more = false;
  return MessagePage(id, nullptr, kMaxHistory, arena, rows, count, &has_more);
}

bool Store::MessagePage(const char* id, const char* before_id, size_t limit,
                        tj::Arena& arena, MessageView** rows, size_t* count,
                        bool* has_more) {
  if (!limit || limit > kMaxHistory || (before_id && !IsConversationId(before_id))) return false;
  char prefix[kKeyBytes];
  message_prefix(id, prefix);
  char before_key[kKeyBytes];
  if (before_id) message_key(id, before_id, before_key);
  Collected* collected = static_cast<Collected*>(arena.Alloc(sizeof(Collected) * (limit + 1)));
  if (!collected) return false;
  Collector collector{&arena, collected, 0, limit + 1, true};
  if (!kv_->Scan(prefix, before_id ? before_key : nullptr, limit + 1, clock_->NowMs(), [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }, true) || !collector.ok)
    return false;
  *has_more = collector.count > limit;
  const size_t visible = *has_more ? limit : collector.count;
  MessageView* out = static_cast<MessageView*>(arena.Alloc(sizeof(MessageView) * (visible ? visible : 1)));
  if (!out) return false;
  for (size_t i = 0; i < visible; ++i) {
    // rows.reverse(): oldest first.
    const Collected& row = collected[visible - 1 - i];
    out[i] = MessageView{};
    if (!decode_message(row.bytes, row.length, arena, &out[i])) return false;
  }
  *rows = out;
  *count = visible;
  return true;
}

AddStatus Store::AddMessage(const char* id, const char* client_message_id, const char* text, size_t text_length,
                            const char* snapshot_json, size_t snapshot_json_length, const char* snapshot_hash,
                            char message_id[kIdBytes]) {
  if (!started_) return AddStatus::kStorage;
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  const Value* decoded = nullptr;
  if (snapshot_json) {
    char hash[kHashHexBytes];
    Sha256Hex(snapshot_json, snapshot_json_length, hash);
    if (strcmp(hash, snapshot_hash) != 0) return AddStatus::kInvalid;
    // JSON.parse: no byte order mark.
    if (snapshot_json_length >= 3 && memcmp(snapshot_json, "\xEF\xBB\xBF", 3) == 0) return AddStatus::kInvalid;
    decoded = tj::Parse(arena, reinterpret_cast<const uint8_t*>(snapshot_json), snapshot_json_length);
    if (!decoded) return AddStatus::kInvalid;
  }
  const int64_t now = clock_->NowMs();
  // A KV read failure or an undecodable stored record is a storage error
  // (the source's disk.get/tx.get throw: HTTP 500), never SNAPSHOT_INVALID.
  Conversation owner;
  bool owner_error = false;
  const bool has_owner = LoadConversation(id, now, arena, &owner, &owner_error);
  if (owner_error) return AddStatus::kStorage;
  const Value* parsed = nullptr;
  if (decoded) {
    parsed = ValidateSnapshot(decoded, arena);
  } else if (has_owner) {
    // The source keeps the snapshot inside the conversation record; here it
    // is a separate record written in the same frame, so a missing or
    // undecodable one is a damaged conversation.
    char key[kKeyBytes];
    snapshot_key(id, owner.snapshot_hash, key);
    const char* stored = nullptr;
    size_t stored_length = 0;
    if (GetTextStatus(key, now, arena, &stored, &stored_length) != Lookup::kFound) return AddStatus::kStorage;
    parsed = ParseSnapshot(stored, stored_length, arena);
    if (!parsed) return AddStatus::kStorage;
  }
  if (!parsed || !SnapshotLinksValid(parsed) || !has_owner) return AddStatus::kInvalid;
  const Value* wallets = tj::Get(parsed, "wallets");
  bool owned = false;
  for (uint32_t i = 0; !owned && i < wallets->count; ++i) {
    const Value* wallet = wallets->items[i];
    const Value* address = tj::Get(wallet, "address");
    char candidate[kHashHexBytes];
    owned = tj::StringEquals(tj::Get(wallet, "family"), "evm") &&
            tj::Get(wallet, "watchOnly")->type == tj::Type::kFalse &&
            OwnerId(secret_, secret_length_, address->text, address->length, candidate) &&
            strcmp(candidate, owner.owner_id) == 0;
  }
  if (!owned) return AddStatus::kInvalid;
  std::unique_ptr<char[], base::StorageDeleter> body_owner;
  size_t body_length = 0;
  bool redacted = false;
  const bool body_ok = compat::redaction::RedactSecrets(text, text_length, &body_owner, &body_length, &redacted);
  char* body = body_owner.get();
  if (!body_ok) {
    return AddStatus::kStorage;
  }
  // The row's snapshot: the update, else the conversation's snapshot as read.
  const char* row_snapshot = snapshot_json ? snapshot_hash : owner.snapshot_hash;

  fortis::base::LockGuard guard(mutex_);
  char key[kKeyBytes];
  client_key(id, client_message_id, key);
  const char* existing = nullptr;
  size_t existing_length = 0;
  const Lookup client_status = GetTextStatus(key, now, arena, &existing, &existing_length);
  if (client_status == Lookup::kError) return AddStatus::kStorage;
  if (client_status == Lookup::kFound) {
    // `existing ? publicMessage(existing) : null`: an index naming no
    // message is SNAPSHOT_INVALID; an unreadable message is a storage error.
    if (existing_length != kIdBytes - 1) return AddStatus::kInvalid;
    MessageView row;
    const Lookup message_status = GetMessage(id, existing, now, arena, &row);
    if (message_status == Lookup::kError) return AddStatus::kStorage;
    if (message_status == Lookup::kMissing) return AddStatus::kInvalid;
    memcpy(message_id, row.id, kIdBytes);
    return AddStatus::kAdded;
  }
  Conversation current;
  bool current_error = false;
  if (!LoadConversation(id, now, arena, &current, &current_error))
    return current_error ? AddStatus::kStorage : AddStatus::kInvalid;
  char row_id[kIdBytes];
  if (!NewId(row_id)) return AddStatus::kStorage;
  const uint8_t* message_bytes = nullptr;
  size_t message_length = 0;
  if (!encode_message(arena, row_id, client_message_id, "user", body, body_length, row_snapshot, parsed, now, &message_bytes,
                      &message_length))
    return AddStatus::kStorage;
  const int64_t expiry = current.expires_at_ms;
  char keys[5][kKeyBytes];
  message_key(id, row_id, keys[0]);
  client_key(id, client_message_id, keys[1]);
  storage::KvMutation writes[4] = {
      {keys[0], message_bytes, static_cast<uint32_t>(message_length), expiry, false},
      {keys[1], row_id, kIdBytes - 1, expiry, false},
  };
  size_t write_count = 2;
  if (snapshot_json) {
    Value* conversation = tj::Obj(arena);
    const uint8_t* conversation_bytes = nullptr;
    size_t conversation_length = 0;
    if (!conversation || !tj::Set(arena, conversation, "ownerId", tj::Str(arena, current.owner_id)) ||
        !tj::Set(arena, conversation, "deviceId", tj::Str(arena, current.device_id)) ||
        !tj::Set(arena, conversation, "proofNonceHash", tj::Str(arena, current.nonce_hash)) ||
        !tj::Set(arena, conversation, "accessTokenHash", tj::Str(arena, current.token_hash)) ||
        !tj::Set(arena, conversation, "snapshotHash", tj::Str(arena, snapshot_hash)) ||
        !tj::Set(arena, conversation, "createdAt", tj::Num(arena, static_cast<double>(current.created_at_ms))) ||
        !tj::Set(arena, conversation, "updatedAt", tj::Num(arena, static_cast<double>(now))) ||
        !tj::Set(arena, conversation, "expiresAt", tj::Num(arena, static_cast<double>(expiry))) ||
        !stringify(conversation, arena, &conversation_bytes, &conversation_length))
      return AddStatus::kStorage;
    conversation_key(id, keys[2]);
    writes[write_count++] = {keys[2], conversation_bytes, static_cast<uint32_t>(conversation_length), expiry, false};
    snapshot_key(id, snapshot_hash, keys[3]);
    const char* stored = nullptr;
    size_t stored_length = 0;
    const Lookup snapshot_status = GetTextStatus(keys[3], now, arena, &stored, &stored_length);
    if (snapshot_status == Lookup::kError) return AddStatus::kStorage;
    if (snapshot_status == Lookup::kMissing) {
      const uint8_t* snapshot_bytes = nullptr;
      size_t snapshot_length = 0;
      if (!stringify(parsed, arena, &snapshot_bytes, &snapshot_length)) return AddStatus::kStorage;
      writes[write_count++] = {keys[3], snapshot_bytes, static_cast<uint32_t>(snapshot_length), expiry, false};
    }
  }
  job_key(id, row_id, keys[4]);
  writes[write_count++] = {keys[4], "pending", 7, expiry, false};
  if (!write_with_stats(*kv_, writes, write_count, current.owner_id, now,
                        StatsEvent::UserMessage)) return AddStatus::kStorage;
  memcpy(message_id, row_id, kIdBytes);
  return AddStatus::kAdded;
}

bool Store::FindReply(const char* id, const char* user_message_id, char reply_id[kIdBytes]) {
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  char key[kKeyBytes];
  reply_key(id, user_message_id, key);
  const char* text = nullptr;
  size_t length = 0;
  MessageView row;
  const bool ok = GetText(key, clock_->NowMs(), arena, &text, &length) && length == kIdBytes - 1 &&
                  LoadMessage(id, text, arena, &row);
  if (ok) memcpy(reply_id, row.id, kIdBytes);
  tj::FreeArena(arena);
  return ok;
}

bool Store::LoadReplyContext(const char* id, const char* user_message_id, tj::Arena& arena, const Value** snapshot,
                             Turn** turns, size_t* count) {
  MessageView user;
  const char* snapshot_text = nullptr;
  size_t snapshot_length = 0;
  if (!LoadMessage(id, user_message_id, arena, &user, &snapshot_text, &snapshot_length) ||
      strcmp(user.role, "user") != 0 || !user.has_snapshot || !snapshot_text)
    return false;
  char prefix[kKeyBytes], after[kKeyBytes];
  message_prefix(id, prefix);
  message_key(id, user_message_id, after);
  Collected* collected = static_cast<Collected*>(arena.Alloc(sizeof(Collected) * kReplyHistory));
  if (!collected) return false;
  Collector collector{&arena, collected, 0, kReplyHistory, true};
  if (!kv_->Scan(prefix, after, kReplyHistory, clock_->NowMs(), [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }, true) || !collector.ok)
    return false;
  Turn* out = static_cast<Turn*>(arena.Alloc(sizeof(Turn) * (collector.count + 1)));
  if (!out) return false;
  size_t n = 0;
  // [user, ...older].reverse(), keeping user and assistant rows.
  for (size_t i = collector.count; i-- > 0;) {
    MessageView row;
    if (!decode_message(collected[i].bytes, collected[i].length, arena, &row)) return false;
    const bool assistant = strcmp(row.role, "assistant") == 0;
    if (!assistant && strcmp(row.role, "user") != 0) continue;
    out[n++] = {assistant, row.body, row.body_length};
  }
  out[n++] = {false, user.body, user.body_length};
  *snapshot = ParseSnapshot(snapshot_text, snapshot_length, arena);
  if (!*snapshot) return false;
  *turns = out;
  *count = n;
  return true;
}

bool Store::JobStatus(const char* id, const char* user_message_id, char* status, size_t capacity) {
  if (!started_ || !status || capacity < 9) return false;
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  char key[kKeyBytes];
  job_key(id, user_message_id, key);
  const char* value = nullptr;
  size_t length = 0;
  const Lookup result = GetTextStatus(key, clock_->NowMs(), arena, &value, &length);
  if (result == Lookup::kError) return false;
  if (result == Lookup::kMissing) { memcpy(status, "complete", 9); return true; }
  if (length >= capacity) return false;
  memcpy(status, value, length);
  status[length] = '\0';
  return true;
}

bool Store::SetJobStatus(const char* id, const char* user_message_id, const char* status) {
  if (!started_ || (strcmp(status, "pending") != 0 && strcmp(status, "running") != 0 && !IsFailedReplyStatus(status)))
    return false;
  fortis::base::LockGuard guard(mutex_);
  // SaveReply publishes the assistant/index and removes the job in one frame.
  // A late failure (including an exception after commit) must not recreate a
  // failed/pending job over that success. Serialize the check with SaveReply.
  char completed[kKeyBytes];
  reply_key(id, user_message_id, completed);
  char reply[kIdBytes];
  size_t reply_length = 0;
  const auto found = kv_->GetStatus(completed, clock_->NowMs(), reply, sizeof(reply), &reply_length);
  if (found == storage::KvReadStatus::Found) return reply_length == kIdBytes - 1;
  if (found != storage::KvReadStatus::Missing) return false;
  char key[kKeyBytes];
  job_key(id, user_message_id, key);
  int64_t created = 0, expiry = 0;
  if (!ConversationTimes(id, &created, &expiry)) return false;
  const storage::KvMutation write{key, status, static_cast<uint32_t>(strlen(status)), expiry, false};
  return kv_->Write(&write, 1);
}

bool Store::ResetRunningJobs() {
  if (!started_) return false;
  struct Rows { char keys[16][kKeyBytes]{}; size_t count = 0; } rows;
  auto visitor = [](const char* key, const void* value, size_t size, void* context) -> bool {
    auto* out = static_cast<Rows*>(context);
    if (size == 7 && memcmp(value, "running", 7) == 0 && out->count < 16)
      snprintf(out->keys[out->count++], kKeyBytes, "%s", key);
    return true;
  };
  if (!kv_->Scan("ai/job/", nullptr, 100000, clock_->NowMs(), [&, visitor_state = &rows](const char* key, const void* value, size_t size) { return (visitor)(key, value, size, visitor_state); })) return false;
  for (size_t i = 0; i < rows.count; ++i) {
    char id[kIdBytes]{};
    memcpy(id, rows.keys[i] + strlen("ai/job/"), kIdBytes - 1);
    int64_t created = 0, expiry = 0;
    if (!ConversationTimes(id, &created, &expiry)) continue;
    const storage::KvMutation write{rows.keys[i], "pending", 7, expiry, false};
    if (!kv_->Write(&write, 1)) return false;
  }
  return true;
}

bool Store::NextPendingJob(char conversation[kIdBytes], char user_message[kIdBytes]) {
  if (!started_) return false;
  fortis::base::LockGuard guard(mutex_);
  struct Selection {
    char key[kKeyBytes]{}; char id[kIdBytes]{}; char message[kIdBytes]{};
    char running[16][kIdBytes]{}; size_t running_count = 0;
    bool found = false;
  } selection;
  auto running_visitor = [](const char* key, const void* value, size_t size, void* context) -> bool {
    auto* selected = static_cast<Selection*>(context);
    if (size != 7 || memcmp(value, "running", 7) != 0 || selected->running_count == 16) return true;
    const char* id = key + strlen("ai/job/");
    const char* slash = strchr(id, '/');
    if (slash && slash - id == kIdBytes - 1) {
      memcpy(selected->running[selected->running_count], id, kIdBytes - 1);
      selected->running[selected->running_count++][kIdBytes - 1] = '\0';
    }
    return true;
  };
  if (!kv_->Scan("ai/job/", nullptr, 100000, clock_->NowMs(), [&, visitor_state = &selection](const char* key, const void* value, size_t size) { return (running_visitor)(key, value, size, visitor_state); })) return false;
  auto visitor = [](const char* key, const void* value, size_t size, void* context) -> bool {
    auto* selected = static_cast<Selection*>(context);
    if (selected->found || size != 7 || memcmp(value, "pending", 7) != 0) return true;
    const char* id = key + strlen("ai/job/");
    const char* slash = strchr(id, '/');
    if (!slash || slash - id != kIdBytes - 1 || strlen(slash + 1) != kIdBytes - 1) return true;
    for (size_t i = 0; i < selected->running_count; ++i)
      if (memcmp(selected->running[i], id, kIdBytes - 1) == 0) return true;
    snprintf(selected->key, sizeof(selected->key), "%s", key);
    memcpy(selected->id, id, kIdBytes - 1);
    selected->id[kIdBytes - 1] = '\0';
    memcpy(selected->message, slash + 1, kIdBytes);
    selected->found = true;
    return true;
  };
  if (!kv_->Scan("ai/job/", nullptr, 100000, clock_->NowMs(), [&, visitor_state = &selection](const char* key, const void* value, size_t size) { return (visitor)(key, value, size, visitor_state); }) || !selection.found) return false;
  int64_t created = 0, expiry = 0;
  if (!ConversationTimes(selection.id, &created, &expiry)) return false;
  const storage::KvMutation claim{selection.key, "running", 7, expiry, false};
  if (!kv_->Write(&claim, 1)) return false;
  memcpy(conversation, selection.id, kIdBytes);
  memcpy(user_message, selection.message, kIdBytes);
  return true;
}

bool Store::SaveReply(const char* id, const char* user_message_id, const char* text, size_t length,
                      char reply_id[kIdBytes], bool* created, const TokenUsage& usage) {
  *created = false;
  if (!started_) return false;
  tj::Arena arena = tj::GrowingArena(4 * 1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  fortis::base::LockGuard guard(mutex_);
  const int64_t now = clock_->NowMs();
  char key[kKeyBytes];
  reply_key(id, user_message_id, key);
  const char* winner = nullptr;
  size_t winner_length = 0;
  MessageView row;
  const Lookup reply_status = GetTextStatus(key, now, arena, &winner, &winner_length);
  if (reply_status == Lookup::kError) return false;
  if (reply_status == Lookup::kFound && winner_length == kIdBytes - 1 &&
      LoadMessage(id, winner, arena, &row)) {
    memcpy(reply_id, row.id, kIdBytes);
    return true;
  }
  Conversation conversation;
  if (!LoadConversation(id, now, arena, &conversation)) return false;
  char row_id[kIdBytes];
  if (!NewId(row_id)) return false;
  char client[64];
  snprintf(client, sizeof(client), "assistant:%s", user_message_id);
  size_t stored_length = 0;
  const char* stored = storable(text, length, arena, &stored_length);
  const uint8_t* bytes = nullptr;
  size_t bytes_length = 0;
  if (!stored ||
      !encode_message(arena, row_id, client, "assistant", stored, stored_length, nullptr, nullptr, now, &bytes, &bytes_length))
    return false;
  char message[kKeyBytes];
  message_key(id, row_id, message);
  const int64_t expiry = conversation.expires_at_ms;
  char job[kKeyBytes];
  job_key(id, user_message_id, job);
  const storage::KvMutation writes[] = {
      {message, bytes, static_cast<uint32_t>(bytes_length), expiry, false},
      {key, row_id, kIdBytes - 1, expiry, false},
      {job, nullptr, 0, 0, true},
  };
  if (!write_with_stats(*kv_, writes, 3, conversation.owner_id, now,
                        StatsEvent::AssistantMessage, usage)) return false;
  memcpy(reply_id, row_id, kIdBytes);
  *created = true;
  return true;
}

}  // namespace fortis::ai
