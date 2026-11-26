#pragma once

#include <memory>
#include "clock.h"
#include "error_reporter.h"
#include "ownership.h"
#include "status.h"
#include "ai_config.h"
#include "http_server.h"
#include "seams.h"
#include "outbound_https.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

namespace fortis::ai {

// Test seams. A normal composition passes none: the wall clock, CSPRNG
// monotonic ULIDs and https://api.openai.com with the validated CA bundle.
// Borrowed synchronous effect: request/body remain valid only for this call.
// Implementations preserve the request's deadline, size and address policy.
class ReplyTransport {
 public:
  virtual http::OutboundResponse Execute(const http::OutboundRequest& request,
      uint8_t* response, size_t capacity) = 0;
 protected:
  ~ReplyTransport() = default;
};

struct AiServiceSeams {
  ReplyTransport* transport = nullptr;
  fortis::base::Clock* clock = nullptr;  // NowMs: the wall clock
  fortis::base::IdSource* new_id = nullptr;
  // Replaces the OpenAI endpoint (a local HTTPS fixture).
  const char* upstream_host = nullptr;
  const char* upstream_port = nullptr;
  const char* upstream_ca = nullptr;
  bool (*allow_address)(void* context, uint32_t ipv4_host_order) = nullptr;
  // Replaces the 30-second provider deadline.
  uint32_t provider_timeout_ms = 0;
  void* context = nullptr;  // allow_address's context
};

// Durable wallet-owned AI conversations behind FORTIS_NATIVE_AI_CANARY=1.
// The HTTP route commits the user turn and a pending job in one KV frame,
// then returns 201. Bounded workers use Anthropic Messages when explicitly
// configured, save one assistant reply or a retryable failure, and resume
// unfinished jobs after restart. History pagination is unchanged; its newest
// page carries reply status headers. No wallet body or provider key is logged.
struct AiServiceDeps { storage::KvStore& kv; base::ErrorReporter* errors = nullptr; };

/** Own bounded background generation workers and durable conversation routes.
 * Dependencies and optional test seams are borrowed and must outlive this object.
 * Destruction stops and joins workers before releasing implementation storage.
 * HTTP 201 acknowledges durable acceptance; it does not promise a provider reply.
 */
class AiConversationService {
 public:
  static constexpr size_t kWorkers = 4;
  static constexpr size_t kSlots = 8;

  AiConversationService(AiConversationService&&) noexcept;
  AiConversationService& operator=(AiConversationService&&) = delete;
  ~AiConversationService();
  AiConversationService(const AiConversationService&) = delete;
  AiConversationService& operator=(const AiConversationService&) = delete;

  /** Validate configuration and construct the service, returning a failure status.
   * No conversation credentials, user messages or provider secrets enter logs.
   * @param seams Optional synchronous effects used by deterministic tests.
   */
  static base::Result<AiConversationService> Create(const AiServiceConfig& config, AiServiceDeps deps,
                                                     const AiServiceSeams* seams = nullptr);
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  // Waits until no generation is queued or running (tests).
  bool WaitIdle(uint32_t timeout_ms);

  struct Impl;

 private:
  explicit AiConversationService(Impl* impl);
  void Stop();
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

struct MessageView;

// The pure pieces of GET /v1/ai/conversations/:id/messages (contract beside
// AiConversationService::Impl::Messages), exposed for unit tests.

// paramsSchema: /^[0-9A-HJKMNP-TV-Z]{26}$/ (Crockford base32, upper case).
bool IsConversationId(const char* id);

// authorized()'s conversation bearer: the text after a case-sensitive
// "Bearer " prefix of the one x-intelligence-authorization header (the
// parser has already trimmed its surrounding spaces/tabs, as Node does).
// "" for a missing or differently prefixed header (a repeated one never
// reaches the route: the request parser refuses it); the store then refuses
// "" by shape. Never null.
const char* ChatBearerToken(const char* header);

// The 200 body for `rows` (oldest first): JSON
// {"messages":[{"id","role","body","createdAt"},…]} (JSON.stringify bytes)
// or the ConversationMessagesResponse protobuf wire (field 1 repeated
// ConversationMessage{1 id, 2 role, 3 body, 4 createdAt}, empty strings
// omitted, WTF-8 lone surrogates written as U+FFFD). Keeps the newest rows
// whose whole body fits `limit` bytes, dropping the oldest first; `*first`
// is the index of the first row kept (== count when none fits; the empty
// JSON envelope, 15 bytes, is written even under a smaller limit). `*body` is
// a standard owning buffer (at least one byte). False only when
// memory runs out.
bool EncodeMessagesBody(const MessageView* rows, size_t count, bool protobuf, size_t limit, std::unique_ptr<uint8_t[], base::StorageDeleter>* body,
                        size_t* length, size_t* first);

}  // namespace fortis::ai
