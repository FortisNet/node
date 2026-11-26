#include "token_service_internal.h"
#include "log.h"
namespace tj = fortis::codec::tj;
namespace fortis::tokens {
namespace {
constexpr size_t kForegroundArenaBytes = 8 * 1024 * 1024 - 64;
const char* const kPageCacheControl = "private, max-age=15";
}
// ---- foreground workers ----------------------------------------------------------
bool TokenRequestWorker::Submit(TokenTask* task) {
  fortis::base::LockGuard lock(control_.mutex());
  if (control_.StoppingLocked() || !tasks_.Push(task)) return false;
  control_.changed().Broadcast();
  return true;
}

void* TokenRequestWorker::Entry(void* opaque) {
  auto* self = static_cast<TokenRequestWorker*>(opaque);
  Work work{};
  bool ready = self->workspaces_.Make(&work, kForegroundArenaBytes, false, kTaskBudgetMs);
  for (;;) {
    fortis::base::UniqueLock lock(self->control_.mutex());
    while (!self->control_.StoppingLocked() && self->tasks_.size() == 0) self->control_.changed().WaitLocked(self->control_.mutex());
    if (self->tasks_.size() == 0) break;
    TokenTask* task = self->tasks_.Pop();
    lock.Unlock();
    // A fixed 8 MiB allocation on *every* page read eventually fails in the
    // page-based guest allocator even while total free RAM remains ample.
    // Reuse one workspace per worker; retry allocation if startup was tight.
    if (!ready) ready = self->workspaces_.Make(&work, kForegroundArenaBytes, false, kTaskBudgetMs);
    if (ready) self->Run(task, work);
    else self->CompleteText(*task, 502, "Bad Gateway", kUpstreamUnavailable);
    fortis::base::Delete(task);
  }
  if (ready) TokenWorkspaces::Free(&work);
  return nullptr;
}

void TokenRequestWorker::Complete(const TokenTask& task, const http::HttpResponse& response) {
  http::HttpResponse copy = response;
  copy.rate_limit_cost = task.cost;
  copy.rate_limit_remaining = task.remaining;
  if (!handoff_->Complete(task.token, copy)) {
    // The deadline passed or the handoff stopped: nothing to answer.
  }
}

void TokenRequestWorker::CompleteText(const TokenTask& task, int status, const char* reason, const char* body) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  Complete(task, response);
}

// runFacet's error contract for a facts status.
void TokenRequestWorker::CompleteStatus(const TokenTask& task, FactsOutcome status) {
  if (status == FactsOutcome::kUnknownNetwork || status == FactsOutcome::kUnsupportedRef) {
    char detail[1200];
    if (status == FactsOutcome::kUnknownNetwork)
      snprintf(detail, sizeof(detail), "unknown network: %.*s", static_cast<int>(task.network_length), task.network.data());
    else
      snprintf(detail, sizeof(detail), "unsupported token ref: %.*s", static_cast<int>(task.ref_length), task.ref.data());
    char body[4096];
    const size_t length = bad_request_body(detail, body, sizeof(body));
    http::HttpResponse response{400, "Bad Request", kJsonType, reinterpret_cast<const uint8_t*>(body), length};
    Complete(task, response);
    return;
  }
  CompleteText(task, 502, "Bad Gateway", kUpstreamUnavailable);
}

// sendConditionalStructured: JSON or StructuredReadResponse protobuf with
// the TypeScript weak validator; `validator` replaces the JSON payload in
// the validator when given.
void TokenRequestWorker::CompleteStructured(Work& work, const TokenTask& task, const char* name_space,
                                            const Value* document, const Value* validator, const char* cache_control,
                                            bool conditional) {
  (void)work;
  // Response buffers live outside the work arena; Complete copies the body.
  auto body_storage = base::AllocateStorage<uint8_t[]>(kResponseBytes);
  auto stable_storage = validator ? base::AllocateStorage<uint8_t[]>(kResponseBytes) : nullptr;
  auto* body = body_storage.get();
  auto* stable = stable_storage.get();
  if (!body || (validator && !stable)) { CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return; }
  tj::OutBuffer out{body, kResponseBytes, 0, true};
  tj::OutBuffer stable_out{stable, stable ? kResponseBytes : 0, 0, true};
  bool ok;
  if (task.protobuf) {
    ok = tj::AppendStructuredReadResponse(document, out) && out.ok;
    if (ok && validator) ok = tj::AppendStructuredReadResponse(validator, stable_out) && stable_out.ok;
  } else {
    ok = tj::Stringify(document, out) && out.ok;
    if (ok && validator) ok = tj::Stringify(validator, stable_out) && stable_out.ok;
  }
  if (!ok) { CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return; }
  http::HttpResponse selected{200, "OK", task.protobuf ? "application/protobuf" : kJsonType, body, out.used};
  if (!conditional) {
    // sendStructuredOrJson: Vary: Accept on both representations, no ETag.
    selected.vary_accept = true;
    selected.cache_control = cache_control;
    Complete(task, selected);
    return;
  }
  http::HttpRequest request{};
  char accept[] = "application/protobuf";
  request.accept = task.protobuf ? accept : nullptr;
  request.if_none_match = task.has_if_none_match ? task.if_none_match.data() : nullptr;
  http::HttpResponse response{};
  const uint8_t* validator_bytes = validator ? stable : nullptr;
  const size_t validator_length = validator ? stable_out.used : 0;
  if (!ConditionalStructuredResponse(request, name_space, selected, selected, &response, validator_bytes,
                                     validator_length, validator_bytes, validator_length)) {
    CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  response.cache_control = cache_control;
  Complete(task, response);
}

void TokenRequestWorker::Run(TokenTask* task, Work& work) {
  work.arena.used = 0;
  work.arena.ok = true;
  work.deadline_ms = providers::TokenMonotonicMs() + kTaskBudgetMs;
  Value* document = nullptr;
  switch (task->kind) {
    case TokenTask::kPage: {
      task->page.coingecko_hint = task->has_hint ? task->hint.data() : nullptr;
      const FactsOutcome status = fortis::tokens::token_page::PageDocument(work, task->network.data(), task->network_length, task->ref.data(),
                                                             task->ref_length, task->page, &document);
      if (status != FactsOutcome::kOk) {
        if (status == FactsOutcome::kInternal)
          fortis::base::LogError("tokens", "token page composition failed: arena_ok=%d arena_used=%zu",
                  work.arena.ok, work.arena.used);
        CompleteStatus(*task, status);
      }
      else CompleteStructured(work, *task, "token-page", document, nullptr, kPageCacheControl, true);
      break;
    }
    case TokenTask::kToken360: {
      const FactsOutcome status = fortis::tokens::token_page::Token360Document(work, task->network.data(), task->network_length,
                                                                 task->ref.data(), task->ref_length, &document);
      if (status != FactsOutcome::kOk) CompleteStatus(*task, status);
      else CompleteStructured(work, *task, "token360", document, nullptr, nullptr, false);
      break;
    }
    case TokenTask::kPools: {
      const FactsOutcome status = fortis::tokens::token_facts::LiquidityDetails(work, std::string_view{task->network.data(), task->network_length}, std::string_view{task->ref.data(), task->ref_length}, &document);
      if (status != FactsOutcome::kOk) CompleteStatus(*task, status);
      else CompleteStructured(work, *task, "token-liquidity-pools", document, nullptr, kMinuteCacheControl, true);
      break;
    }
  }
}

bool TokenRequestWorker::Start(const pthread_attr_t* attributes) {
  if (started_) return false;
  bool ok = true;
  for (size_t i = 0; i < kForegroundWorkers && ok; ++i) {
    ok = workers_[i].StartEntry<Entry>(this, attributes);
    if (ok) ++started_;
  }
  return ok;
}
void TokenRequestWorker::Join() { for (size_t i = 0; i < started_; ++i) workers_[i].Join(); }
void TokenRequestWorker::DiscardPending() { while (tasks_.size()) base::Delete(tasks_.Pop()); }
}  // namespace fortis::tokens
