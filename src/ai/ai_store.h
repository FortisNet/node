#include "threading.h"
#pragma once

#include "ai_proof.h"
#include "ai_reply.h"
#include "ai_stats.h"
#include "clock.h"
#include "seams.h"
#include "token_json.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// Durable AI conversations, the native counterpart of the TypeScript
// KvIntelligenceConversationService, over the
// process KV. Every multi-record change is one KvStore::Write frame (one
// fsynced, checksummed append), so the nonce, token and client-message
// indexes commit with the records they name. New threads have no KV expiry;
// the old seven-day threads retain their original expiry. A store mutex makes each read-check-write sequence a
// transaction against the other AI writers (the KV has no read-write
// transactions). The KV expiry sweeper removes expired records; reads at
// `now` never see them.
//
// Keys (new threads have no expiry):
//   ai/conversation/{id}             {ownerId, deviceId, hashes, times}
//   ai/owner/{ownerId}                id, so a fresh signed proof recovers the same thread
//   ai/nonce/{sha256(nonce)}         id
//   ai/token/{sha256(chat token)}    id
//   ai/snapshot/{id}/{snapshotHash}  the zod-normalized snapshot JSON
//   ai/message/{id}/{messageId}      {id, clientMessageId, role, body, snapshotHash|null,
//                                    attachment:{kind,version,sha256,capturedAt}|null, createdAt}
//   ai/client/{id}/{clientMessageId} messageId
//   ai/client/{id}/assistant%3A{userMessageId}  the assistant reply's messageId
// Only the chat token's SHA-256 is stored; snapshots are stored once per
// (conversation, hash) and referenced by the messages that carry them.
namespace fortis::ai {

constexpr int64_t kConversationLifetimeMs = 7LL * 24 * 60 * 60 * 1000;  // legacy threads
constexpr int64_t kProofTimestampToleranceMs = 5 * 60 * 1000;
constexpr size_t kIdBytes = 27;  // ULID + NUL
constexpr size_t kMaxHistory = 100;  // messages(): newest 100
constexpr size_t kReplyHistory = 19;  // replyNow(): 19 older turns

static_assert(kIdBytes == base::IdSource::kIdBytes, "ULID buffers match the id seam");

struct MessageView {
  char id[kIdBytes]{};
  char role[10]{};
  const char* body = nullptr;  // WTF-8, arena-owned
  size_t body_length = 0;
  int64_t created_at_ms = 0;
  // POST responses also carry these (publicMessage(row)).
  char client_message_id[64]{};
  char snapshot_hash[kHashHexBytes]{};
  bool has_snapshot = false;
  char attachment_captured_at[40]{};
  bool has_attachment = false;
};

struct OpenInput {
  const char* device_id;
  const char* address;            // as sent (regex-validated)
  const char* nonce;
  const char* timestamp;
  const char* snapshot_hash;
  const char* snapshot_json;
  size_t snapshot_json_length;
  const char* access_token;
  const char* access_token_hash;
  const char* signature;
  const char* first_message;      // zod-trimmed
  size_t first_message_length;
  const char* client_message_id;
};

struct Opened {
  char id[kIdBytes]{};
  int64_t expires_at_ms = 0;
  char message_id[kIdBytes]{};
};

enum class AddStatus : uint8_t { kAdded, kInvalid, kStorage };

class ConversationRepository {
 public:
  virtual int64_t Now() const = 0;
  virtual bool Open(const OpenInput& input, Opened* opened, bool* storage_error) = 0;
  virtual bool Authorize(const char* id, const char* device_id, const char* token, size_t token_length,
                 bool* storage_error) = 0;
  virtual bool Messages(const char* id, fortis::codec::tj::Arena& arena, MessageView** rows, size_t* count) = 0;
  virtual bool MessagePage(const char* id, const char* before_id, size_t limit,
                   fortis::codec::tj::Arena& arena, MessageView** rows, size_t* count,
                   bool* has_more) = 0;
  virtual AddStatus AddMessage(const char* id, const char* client_message_id, const char* text, size_t text_length,
                       const char* snapshot_json, size_t snapshot_json_length, const char* snapshot_hash,
                       char message_id[kIdBytes]) = 0;
  virtual bool LoadMessage(const char* id, const char* message_id, fortis::codec::tj::Arena& arena, MessageView* row,
                   const char** snapshot = nullptr, size_t* snapshot_length = nullptr) = 0;
  virtual bool ResetRunningJobs() = 0;
  // Claim atomically before returning true and publishing both identifiers.
  // An exception must occur before a durable claim, so callers can retry safely.
  virtual bool NextPendingJob(char conversation[kIdBytes], char user_message[kIdBytes]) = 0;
  virtual bool JobStatus(const char* id, const char* user_message_id, char* status, size_t capacity) = 0;
  // Serialize with SaveReply. A committed reply is terminal: late failure or
  // retry updates succeed as no-ops, never recreating its deleted job marker.
  // Read/write errors return false; callers must not infer that a frame rolled back.
  virtual bool SetJobStatus(const char* id, const char* user_message_id, const char* status) = 0;
  virtual bool FindReply(const char* id, const char* user_message_id, char reply_id[kIdBytes]) = 0;
  virtual bool LoadReplyContext(const char* id, const char* user_message_id, fortis::codec::tj::Arena& arena,
                        const fortis::codec::tj::Value** snapshot, Turn** turns, size_t* count) = 0;
  virtual bool SaveReply(const char* id, const char* user_message_id, const char* text, size_t length,
      char reply_id[kIdBytes], bool* created, const TokenUsage& usage = {}) = 0;
  virtual bool ConversationTimes(const char* id, int64_t* created_at_ms, int64_t* expires_at_ms) = 0;
 protected:
  ~ConversationRepository() = default;
};

class Store final : public ConversationRepository {
 public:
  Store() = default;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // `secret` (SUPPORT_AUTH_SECRET) keys the pseudonymous owner id.
  // `clock` (NowMs) is required; `new_id` null mints CSPRNG monotonic ULIDs.
  // Both must outlive the store.
  bool Start(storage::KvStore* kv, const char* secret, size_t secret_length, base::Clock* clock, base::IdSource* new_id);
  void Stop();
  int64_t Now() const override { return clock_->NowMs(); }

  // open(): verifies the signed proof and persists, recovers the owner's
  // thread with a rotated token, or returns the same-nonce replay.
  // False with *storage_error unset is PROOF_INVALID;
  // *storage_error marks a KV read/write failure or an undecodable record
  // (HTTP 500), including the replay's conversation, client index and first
  // message reads.
  bool Open(const OpenInput& input, Opened* opened, bool* storage_error) override;
  // authorize(): live conversation of this device whose token hash matches.
  // A malformed/missing credential is denied without a storage error; a KV
  // read or conversation decode failure sets storage_error for HTTP 500.
  bool Authorize(const char* id, const char* device_id, const char* token, size_t token_length,
                 bool* storage_error = nullptr) override;
  // messages(): the newest kMaxHistory messages, oldest first.
  bool Messages(const char* id, fortis::codec::tj::Arena& arena, MessageView** rows, size_t* count) override;
  // A bounded history page before an exclusive message id. The cursor is the
  // oldest returned id when has_more is true; reply context is independent.
  bool MessagePage(const char* id, const char* before_id, size_t limit,
                   fortis::codec::tj::Arena& arena, MessageView** rows, size_t* count,
                   bool* has_more) override;
  // addMessage(): the existing row for a repeated clientMessageId, else a
  // new redacted user row (with the snapshot update when one is given).
  // kInvalid is the source's null (SNAPSHOT_INVALID): hash mismatch, a
  // snapshot that is not JSON or fails the schema/links, no owning wallet,
  // an absent conversation, or a client index naming no message. kStorage
  // is a KV read/write failure or an undecodable stored record (HTTP 500).
  AddStatus AddMessage(const char* id, const char* client_message_id, const char* text, size_t text_length,
                       const char* snapshot_json, size_t snapshot_json_length, const char* snapshot_hash,
                       char message_id[kIdBytes]) override;
  // A stored message (and, with `snapshot`, its snapshot JSON text).
  bool LoadMessage(const char* id, const char* message_id, fortis::codec::tj::Arena& arena, MessageView* row,
                   const char** snapshot = nullptr, size_t* snapshot_length = nullptr) override;
  // replyNow() steps. FindReply: the persisted reply to `user_message_id`.
  // LoadReplyContext: the user turn's snapshot and the turns the model sees
  // (false: ReplyUnavailableError). SaveReply: persists one assistant row,
  // or answers the row another worker persisted first.
  // Durable job state is committed with the user row. A failed job is
  // retried when the same client message is posted again.
  bool ResetRunningJobs() override;
  bool NextPendingJob(char conversation[kIdBytes], char user_message[kIdBytes]) override;
  bool JobStatus(const char* id, const char* user_message_id, char* status, size_t capacity) override;
  bool SetJobStatus(const char* id, const char* user_message_id, const char* status) override;
  bool FindReply(const char* id, const char* user_message_id, char reply_id[kIdBytes]) override;
  bool LoadReplyContext(const char* id, const char* user_message_id, fortis::codec::tj::Arena& arena,
                        const fortis::codec::tj::Value** snapshot, Turn** turns, size_t* count) override;
  // `*created` is false when another worker's row answered.
  bool SaveReply(const char* id, const char* user_message_id, const char* text, size_t length,
                 char reply_id[kIdBytes], bool* created, const TokenUsage& usage = {}) override;
  // Zero expiry means a durable thread; older conversations retain their expiry.
  bool ConversationTimes(const char* id, int64_t* created_at_ms, int64_t* expires_at_ms) override;

 private:
  struct Conversation;
  enum class Lookup : uint8_t { kFound, kMissing, kError };
  bool LoadConversation(const char* id, int64_t now, fortis::codec::tj::Arena& arena, Conversation* out,
                        bool* storage_error = nullptr);
  Lookup GetTextStatus(const char* key, int64_t now, fortis::codec::tj::Arena& arena, const char** text, size_t* length);
  // A stored message; an unreadable or undecodable record is kError.
  Lookup GetMessage(const char* id, const char* message_id, int64_t now, fortis::codec::tj::Arena& arena, MessageView* row);
  bool GetText(const char* key, int64_t now, fortis::codec::tj::Arena& arena, const char** text, size_t* length);
  bool NewId(char output[kIdBytes]);

  storage::KvStore* kv_ = nullptr;
  char secret_[1024]{};
  size_t secret_length_ = 0;
  base::Clock* clock_ = nullptr;
  base::IdSource* new_id_ = nullptr;
  bool started_ = false;
  fortis::base::Mutex mutex_;
  // The CSPRNG ULID generator keeps ids strictly increasing within one
  // millisecond (the source's random ulid() does not), so message keys sort
  // in creation order.
  int64_t last_ms_ = -1;
  uint8_t last_random_[10]{};
};

// The monotonic ULID used in production.
bool NewMonotonicUlid(int64_t now_ms, int64_t* last_ms, uint8_t last_random[10], char output[kIdBytes]);

}  // namespace fortis::ai
