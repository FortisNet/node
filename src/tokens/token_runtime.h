#pragma once
#include "token_service.h"
#include "token_facts.h"
#include "token_queue.h"
#include "threading.h"
#include "price_jobs.h"
#include "coin_price_scheduler.h"

namespace fortis::tokens::token_swr { class Cache; }
namespace fortis::tokens::token_data { class TokenDataStore; }
namespace fortis::prices { class PriceSeriesStore; struct SpotProviderOwnerResult; }
namespace fortis::tokens {
namespace token_service_internal {
constexpr size_t kForegroundWorkers = 2;
constexpr size_t kQueueSlots = 16;
constexpr size_t kRevalidationSlots = 64;
}
struct TokenTask;
using token_facts::Work;

// One stop transition and one synchronization domain across request queues,
// revalidation admission, idle waits and scheduling. Locked access requires
// holding mutex(); Stop/IsStopping take that lock themselves.
class TokenRuntimeControl {
 public:
  base::Mutex& mutex() { return mutex_; }
  base::ConditionVariable& changed() { return changed_; }
  bool StoppingLocked() const { return stopping_; }
  void Stop() { base::LockGuard lock(mutex_); stopping_ = true; changed_.Broadcast(); }
  bool IsStopping() { base::LockGuard lock(mutex_); return stopping_; }
 private:
  base::Mutex mutex_;
  base::ConditionVariable changed_;
  bool stopping_ = false;
};

class TokenWorkspaces {
 public:
  explicit TokenWorkspaces(token_facts::Env& env) : env_(env) {}
  bool Make(Work* work, size_t arena_bytes, bool background, uint64_t budget_ms);
  static void Free(Work* work);
  bool MakeHistory(Work* work);
  static void FreeHistory(Work* work);
 private:
  token_facts::Env& env_;
};

class TokenPriceAccess final : public token_facts::TokenSpotReader, public token_facts::TokenSeriesReader {
 public:
  TokenPriceAccess(prices::SpotProviderOwner* spot, prices::PriceHistoryService* history,
                   TokenRuntimeControl& control) : spot_(spot), history_(history), control_(control) {}
  bool Quote(const char* price_id, uint64_t deadline_ms, token_facts::SpotQuote* quote) override;
  token_facts::SeriesStatus Read(Work& work, const char* price_id, int64_t from, int64_t to,
                                bool hourly, token_facts::SeriesRead* out) override;
 private:
  struct SpotWait {
    base::Mutex mutex;
    base::ConditionVariable done;
    bool finished = false, complete = false;
    std::unique_ptr<uint8_t[], base::StorageDeleter> body;
    size_t length = 0;
  };
  static bool Cancelled(void* context);
  static void Done(void* context, const prices::SpotProviderOwnerResult& result);
  prices::SpotProviderOwner* spot_;
  prices::PriceHistoryService* history_;
  TokenRuntimeControl& control_;
};

class TokenRequestWorker {
 public:
  TokenRequestWorker(TokenWorkspaces& workspaces, TokenRuntimeControl& control)
      : workspaces_(workspaces), control_(control) {}
  bool BindHandoff(http::HttpResponseHandoff& handoff) {
    if (handoff_) return false;
    handoff_ = &handoff;
    return true;
  }
  bool Start(const pthread_attr_t* attributes);
  void Join();
  void DiscardPending();
  bool Submit(TokenTask* task);
  void Complete(const TokenTask& task, const http::HttpResponse& response);
 private:
  static void* Entry(void* opaque);
  void Run(TokenTask* task, Work& work);
  void CompleteText(const TokenTask& task, int status, const char* reason, const char* body);
  void CompleteStatus(const TokenTask& task, token_facts::FactsOutcome status);
  void CompleteStructured(Work& work, const TokenTask& task, const char* name_space,
                          const codec::tj::Value* document, const codec::tj::Value* validator,
                          const char* cache_control, bool conditional);
  TokenWorkspaces& workspaces_;
  TokenRuntimeControl& control_;
  http::HttpResponseHandoff* handoff_ = nullptr;
  TokenQueue<TokenTask*, token_service_internal::kQueueSlots> tasks_;
  base::Thread workers_[token_service_internal::kForegroundWorkers];
  size_t started_ = 0;
};

class TokenRevalidator final : public token_facts::TokenRevalidationQueue {
 public:
  TokenRevalidator(TokenWorkspaces& workspaces, TokenRuntimeControl& control, token_facts::Env& env)
      : workspaces_(workspaces), control_(control), env_(env) {}
  bool Enqueue(const token_facts::Revalidation& task) override;
  bool Start(const pthread_attr_t* attributes);
  void Join();
  void DiscardPending();
  bool WaitIdle(uint32_t timeout_ms);
 private:
  static void* Entry(void* opaque);
  TokenWorkspaces& workspaces_;
  TokenRuntimeControl& control_;
  token_facts::Env& env_;
  TokenQueue<token_facts::Revalidation*, token_service_internal::kRevalidationSlots> tasks_;
  bool revalidating_ = false;
  base::Thread thread_;
  bool started_ = false;
};

struct TokenJobDependencies {
  TokenWorkspaces& workspaces;
  TokenRuntimeControl& control;
  token_jobs::TokenStats& stats;
  token_data::TokenDataStore& data;
  prices::PriceSeriesStore& series;
  token_swr::Cache& swr;
  storage::KvStore& kv;
  providers::TokenProviderOwner& owner;
  const config::ProviderRuntimeConfig& config;
  const TokenServiceSeams& seams;
  prices::SpotProviderOwner* spot = nullptr;
  discovery::CoinPopularity* popularity = nullptr;
  prices::PriceSpotCache* price_cache = nullptr;
};
class TokenJobScheduler {
 public:
  using Job = TokenService::Job;
  explicit TokenJobScheduler(const TokenJobDependencies& dependencies) : deps_(dependencies) {}
  bool Start(const pthread_attr_t* attributes);
  void Join();
  bool RunJob(Job job, char* summary, size_t capacity);
 private:
  double JitterSample() const;
  int64_t Now() const;
  static bool Stopping(void* opaque);
  bool RefreshActivePrices(int64_t now, price_jobs::RefreshResult* out);
  bool RunCoinPrices(prices::coin_prices::RunResult* out);
  price_jobs::SealResult SealPrices(int64_t now);
  static void* Entry(void* opaque);
  TokenJobDependencies deps_;
  base::Mutex job_mutex_;
  base::Thread thread_;
  bool started_ = false;
};

class TokenRoutes {
 public:
  TokenRoutes(token_facts::Env& env, TokenWorkspaces& workspaces, TokenRequestWorker& worker,
               bool attestation_required)
      : env_(env), workspaces_(workspaces), worker_(worker), attestation_required_(attestation_required) {}
  bool Register(http::HttpRouter& router);
  void HandlePage(int client, const http::HttpRequest& request, bool head);
  void HandleToken360(int client, const http::HttpRequest& request, bool head);
  void HandlePools(int client, const http::HttpRequest& request, bool head);
  void HandleHolders(int client, const http::HttpRequest& request, bool head);
  void HandleLiquidityHistory(int client, const http::HttpRequest& request, bool head);
  void HandleLiquiditySummary(int client, const http::HttpRequest& request, bool head);
 private:
  token_facts::Env& env_;
  TokenWorkspaces& workspaces_;
  TokenRequestWorker& worker_;
  bool attestation_required_;
};
}  // namespace fortis::tokens
