#pragma once
// Internal composition: all borrowed dependencies outlive workers and routes.
#include "ai_service.h"
#include "ownership.h"
#include "ai_store.h"
#include "device_enrollment.h"
#include "hourly_windows.h"
#include "threading.h"
namespace fortis::ai {
using HourlyWindows = fortis::ai::quota::HourlyWindows<32>;
struct Policy {
  int cost = -1;
  uint32_t remaining = 0;
};
struct GeneratedReply {
  const char* text = nullptr;  // caller's arena, or the static suppression reply
  size_t length = 0;
  TokenUsage usage{};
  base::ErrorDetails failure_details{};  // provider labels have static lifetime
  const char* failure = nullptr;  // stable public status; never provider text
};
struct Encoded {
  Encoded() = default;
  Encoded(const Encoded&) = delete;
  Encoded& operator=(const Encoded&) = delete;
  std::unique_ptr<uint8_t[], base::StorageDeleter> data;
  size_t length = 0;
};
struct TurnRequest {
  bool open = false;
  char conversation[ai::kIdBytes]{};
  char user_message[ai::kIdBytes]{};
  char access_token[56]{};
  int64_t expires_at_ms = 0;
  bool protobuf = false;
  bool head = false;
  Policy policy;
};
class ReplyGenerator {
 public:
  using Impl = AiConversationService::Impl;
  explicit ReplyGenerator(Impl& root) : root_(root) {}
  ReplyGenerator(const ReplyGenerator&) = delete;
  ReplyGenerator& operator=(const ReplyGenerator&) = delete;
  static bool ParseNumber(const char* text, int minimum, int maximum, int* out);
  bool ConfigureAnthropic();
  void GenerateReply(const codec::tj::Value* snapshot, const Turn* turns, size_t turn_count,
      codec::tj::Arena& arena, GeneratedReply* output);
 private:
  friend class AiConversationService;
  friend class ConversationService;
  Impl& root_;
  bool anthropic_ready = false;
  char anthropic_key[256]{};
  int ai_max_tokens = 2048;
  uint32_t ai_timeout_ms = 30000;
  size_t ai_concurrency = 1;
};
class ReplyJobQueue {
 public:
  using Impl = AiConversationService::Impl;
  explicit ReplyJobQueue(Impl& root) : root_(root) {}
  ReplyJobQueue(const ReplyJobQueue&) = delete;
  ReplyJobQueue& operator=(const ReplyJobQueue&) = delete;
  // False means no job claimed (including a reported repository failure).
  // Claimed jobs settle or retain their durable recovery state; never throw.
  bool ProcessPending() noexcept;
  static void* AsyncWorkerMain(void* opaque);
 private:
  friend class AiConversationService;
  Impl& root_;
  fortis::base::Mutex mutex;
  bool stopping = false;
  fortis::base::Thread workers[AiConversationService::kWorkers];
  size_t started_workers = 0;
  size_t active_jobs = 0;
};
class ConversationService {
 public:
  using Impl = AiConversationService::Impl;
  explicit ConversationService(Impl& root) : root_(root) {}
  ConversationService(const ConversationService&) = delete;
  ConversationService& operator=(const ConversationService&) = delete;
  void GenerateJob(const char* id, const char* user_message);
  void FailJob(const char* id, const char* message, const char* status, const char* code, const base::ErrorDetails& details = {}) noexcept;
  bool BuildResponse(const TurnRequest& turn, const char* reply_id, const char* fresh, size_t fresh_length,
                     http::HttpResponse* response, Encoded* encoded);
 private:
  Impl& root_;
};
class ConversationRoutes {
 public:
  using Impl = AiConversationService::Impl;
  explicit ConversationRoutes(Impl& root) : root_(root) {}
  ConversationRoutes(const ConversationRoutes&) = delete;
  ConversationRoutes& operator=(const ConversationRoutes&) = delete;
  static void HandleStats(int client, const http::HttpRequest&, bool head, void* opaque);
  Policy policy_of(const auth::DevicePolicyResult& result);
  bool OverQuota(char scope, const char* device, uint64_t limit);
  void Open(int client, const http::HttpRequest& request, bool head);
  bool ChatAuthorized(const http::HttpRequest& request, const char* id, const char* device,
                      bool* storage_error);
  void Messages(int client, const http::HttpRequest& request, bool head);
  void Retry(int client, const http::HttpRequest& request, bool head);
  void PostMessage(int client, const http::HttpRequest& request, bool head);
  static void HandleRetry(int client, const http::HttpRequest& request, bool head, void* context);
  static void HandleOpen(int client, const http::HttpRequest& request, bool head, void* context);
  static void HandleMessages(int client, const http::HttpRequest& request, bool head, void* context);
  static void HandlePost(int client, const http::HttpRequest& request, bool head, void* context);
 private:
  Impl& root_;
  HourlyWindows quotas;
};
struct AiConversationService::Impl {
  Impl() : store(stored), generator(*this), queue(*this), conversations(*this), routes(*this) {}
  explicit Impl(ConversationRepository& repository) : store(repository), generator(*this), queue(*this), conversations(*this), routes(*this) {}

  AiServiceConfig config{};
  fortis::storage::KvStore* kv = nullptr;
  AiServiceSeams seams{};
  bool have_seams = false;
  ai::Store stored;
  ConversationRepository& store;
  base::ErrorReporter* errors = nullptr;

  // The seam clock, else the system clock (a normal composition). Fixed at
  // Start, before the store or any route reads it.
  fortis::base::Clock& Clock() const { return have_seams && seams.clock ? *seams.clock : fortis::base::SystemClock(); }
  int64_t Now() const { return Clock().NowMs(); }
  // Routes acknowledge durable user turns immediately. Background workers claim
  // persisted jobs, save one reply or retryable failure, and resume after restart.
  ReplyGenerator generator;
  ReplyJobQueue queue;
  ConversationService conversations;
  ConversationRoutes routes;
};
}  // namespace fortis::ai
