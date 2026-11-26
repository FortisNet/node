#include "exception_boundary.h"
#include "provider_diagnostics.h"
#include "provider_execution_coordinator.h"
#include "ownership.h"

#include "threading.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::providers {

namespace {
constexpr uint64_t kCoordinatorPollCeilingMs = 100;

uint64_t monotonic_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

timespec realtime_after_ms(uint64_t milliseconds) {
  timespec result{}; clock_gettime(CLOCK_REALTIME, &result);
  result.tv_sec += static_cast<time_t>(milliseconds / 1000);
  result.tv_nsec += static_cast<long>((milliseconds % 1000) * 1000000);
  if (result.tv_nsec >= 1000000000L) {
    ++result.tv_sec; result.tv_nsec -= 1000000000L;
  }
  return result;
}
}

struct ProviderExecutionCoordinator::Impl {
  enum class CommandKind : uint8_t { kSubmit, kCancel };
  struct Owner {
    bool used = false;
    uint64_t id = 0;
    ProviderCoordinatorWakeFn wake = nullptr;
    void* context = nullptr;
  };
  struct Job {
    bool used = false;
    bool ready = false;
    uint64_t owner_id = 0;
    uint64_t id = 0; // Coordinator-visible identity used for cancellation.
    uint64_t adapter_id = 0;
    void* context = nullptr;
    char lane[kProviderLaneNameCapacity]{};
    char coin_profile_id[101]{};
    ProviderExecutionResult result{};
    unsigned char body[kProviderExecutionMaxResponseBody]{};
  };
  static void ResetJob(Job& job) {
    job.used = false;
    job.ready = false;
    job.owner_id = 0;
    job.id = 0;
    job.adapter_id = 0;
    job.context = nullptr;
    memset(job.lane, 0, sizeof(job.lane));
    memset(job.coin_profile_id, 0, sizeof(job.coin_profile_id));
    job.result = ProviderExecutionResult{};
  }
  struct Command {
    bool used = false;
    bool processing = false;
    bool done = false;
    CommandKind kind = CommandKind::kSubmit;
    uint64_t owner_id = 0;
    uint64_t job_id = 0;
    char lane[kProviderLaneNameCapacity]{};
    http::OutboundRequest request{}; // pointers remain valid until the synchronous caller returns
    ProviderExecutionOptions options{};
    void* context = nullptr;
    ProviderExecutionSubmission submission{};
    bool cancel_result = false;
  };

  base::ErrorReporter* errors;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  ProviderLaneRegistry lanes;
  std::optional<ProviderExecutionAdapter> adapter;
  fortis::base::Thread thread;
  bool started = false;
  bool stopping = false;
  bool shutdown_started = false;
  bool joined = false;
  uint64_t next_owner_id = 1;
  uint64_t next_job_id = 1;
  Owner owners[kProviderCoordinatorMaxOwners]{};
  Job jobs[kProviderExecutionMaxJobs]{};
  unsigned char scratch_body[kProviderExecutionMaxResponseBody]{};
  Command commands[kProviderCoordinatorCommandCapacity]{};

  explicit Impl(bool keyless_coingecko, base::ErrorReporter* reporter) : errors(reporter), lanes(&fortis::base::SystemClock(), keyless_coingecko),
           adapter([&]() -> std::optional<ProviderExecutionAdapter> {
          auto made = ProviderExecutionAdapter::Create(lanes, fortis::base::SystemClock(), WakeCoordinator, this);
          if (!made) return std::nullopt;
          return made.TakeValue();
        }()) {}

  static void WakeCoordinator(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    fortis::base::LockGuard lock(self->mutex);
    self->changed.Signal();
  }
  static void* Entry(void* opaque) {
    static_cast<Impl*>(opaque)->Loop(); return nullptr;
  }
  size_t OwnerCountLocked() const {
    size_t count = 0;
    for (const Owner& owner : owners) count += owner.used;
    return count;
  }
  Owner* FindOwnerLocked(uint64_t id) {
    for (Owner& owner : owners) if (owner.used && owner.id == id) return &owner;
    return nullptr;
  }
  Job* FindJobLocked(uint64_t owner_id, uint64_t job_id) {
    for (Job& job : jobs)
      if (job.used && job.owner_id == owner_id && job.id == job_id) return &job;
    return nullptr;
  }
  size_t LiveJobsLocked(uint64_t owner_id) const {
    size_t count = 0;
    for (const Job& job : jobs)
      count += job.used && (!owner_id || job.owner_id == owner_id);
    return count;
  }

  void FinishCommand(size_t index, const ProviderExecutionSubmission& result) {
    fortis::base::LockGuard lock(mutex);
    commands[index].submission = result;
    commands[index].done = true;
    commands[index].processing = false;
    changed.Broadcast();
  }
  void FinishCancel(size_t index, bool result) {
    fortis::base::LockGuard lock(mutex);
    commands[index].cancel_result = result;
    commands[index].done = true;
    commands[index].processing = false;
    changed.Broadcast();
  }

  bool ProcessOneCommand() {
    Command copy{};
    size_t selected = kProviderCoordinatorCommandCapacity;
    fortis::base::UniqueLock lock(mutex);
    for (size_t i = 0; i < kProviderCoordinatorCommandCapacity; ++i) {
      if (commands[i].used && !commands[i].processing && !commands[i].done) {
        selected = i; commands[i].processing = true; copy = commands[i]; break;
      }
    }
    lock.Unlock();
    if (selected == kProviderCoordinatorCommandCapacity) return false;

    if (copy.kind == CommandKind::kSubmit) {
      ProviderExecutionSubmission failure{};
      failure.status = ProviderExecutionSubmitStatus::kBusy;
      Job* record = nullptr;
      lock.Lock();
      if (!stopping && FindOwnerLocked(copy.owner_id)) {
        for (Job& job : jobs) if (!job.used) {
          record = &job; ResetJob(job); job.used = true;
          job.owner_id = copy.owner_id; job.context = copy.context;
          memcpy(job.lane, copy.lane, sizeof(job.lane));
          if (strcmp(copy.lane, "coingecko") == 0 && copy.request.path) {
            const auto id = CoinGeckoProfileId(copy.request.path);
            if (!id.empty()) memcpy(job.coin_profile_id, id.data(), id.size());
          }
          job.id = next_job_id++;
          if (!job.id) job.id = next_job_id++;
          break;
        }
      }
      lock.Unlock();
      if (!record) { FinishCommand(selected, failure); return true; }

      const auto submitted = adapter->Submit(copy.lane, copy.request, record,
                                             copy.options);
      lock.Lock();
      if (submitted.status == ProviderExecutionSubmitStatus::kAccepted) {
        record->adapter_id = submitted.job_id;
        failure = submitted;
        failure.job_id = record->id;
      } else {
        ResetJob(*record);
        failure = submitted;
      }
      commands[selected].submission = failure;
      commands[selected].done = true;
      commands[selected].processing = false;
      changed.Broadcast();
      return true;
    }

    bool cancelled = false;
    uint64_t adapter_id = 0;
    lock.Lock();
    Job* record = FindJobLocked(copy.owner_id, copy.job_id);
    if (record && !record->ready) adapter_id = record->adapter_id;
    lock.Unlock();
    if (adapter_id) cancelled = adapter->Cancel(adapter_id);
    FinishCancel(selected, cancelled);
    return true;
  }

  bool DrainOneResult() {
    fortis::base::UniqueLock lock(mutex);
    bool has_active = false;
    for (const Job& job : jobs) has_active |= job.used && !job.ready && job.adapter_id;
    lock.Unlock();
    if (!has_active) return false;

    ProviderExecutionResult result{};
    const auto taken = adapter->TryTakeResult(&result, scratch_body,
                                              sizeof(scratch_body));
    if (taken == ProviderExecutionTakeStatus::kNone) return false;
    if (taken != ProviderExecutionTakeStatus::kTaken) return false;

    ProviderCoordinatorWakeFn wake = nullptr; void* wake_context = nullptr;
    lock.Lock();
    auto* record = static_cast<Job*>(result.context);
    if (record < &jobs[0] || record >= &jobs[kProviderExecutionMaxJobs] ||
        !record->used || record->ready || record->adapter_id != result.job_id) {
      lock.Unlock();
      abort();
    }
    Owner* owner = FindOwnerLocked(record->owner_id);
    if (!owner) {
      lock.Unlock();
      abort(); // owner leases cannot be released while a result is outstanding
    }
    if (result.body_length) memcpy(record->body, scratch_body, result.body_length);
    wake = owner->wake; wake_context = owner->context;
    result.job_id = record->id;
    result.context = record->context;
    record->result = result;
    record->ready = true;
    changed.Broadcast();
    char lane[kProviderLaneNameCapacity]{};
    memcpy(lane, record->lane, sizeof(lane));
    char coin_profile_id[101]{};
    memcpy(coin_profile_id, record->coin_profile_id, sizeof(coin_profile_id));
    lock.Unlock();
    // Owners may consume/reuse the job immediately; the label is a local copy.
    const auto failure = DescribeProviderFailure(lane, result);
    if (errors && failure.code) {
      if (result.response.status == 404 && failure.details.origin == base::ErrorOrigin::kThirdParty && coin_profile_id[0])
        errors->Report("prices", "provider_rejected_coin_id", coin_profile_id, failure.details);
      else errors->Report("providers", failure.code, {}, failure.details);
    }
    if (wake) wake(wake_context);
    return true;
  }

  void Loop() {
    for (;;) {
      bool changed_any = false;
      while (ProcessOneCommand()) changed_any = true;
      fortis::base::UniqueLock lock(mutex);
      const bool stopping_now = stopping;
      lock.Unlock();
      if (stopping_now && !shutdown_started) {
        adapter->BeginShutdown(); shutdown_started = true;
      }
      changed_any |= adapter->Poll() != 0;
      while (DrainOneResult()) changed_any = true;

      lock.Lock();
      // Stop() may set `stopping` after this iteration sampled it above. Never
      // exit before the adapter has begun shutdown, or JoinWorkers() returns
      // early and ~ProviderExecutionAdapter aborts on live workers.
      const bool done = stopping && shutdown_started &&
          LiveJobsLocked(0) == 0 && OwnerCountLocked() == 0;
      const bool have_command = [&] {
        for (const Command& command : commands)
          if (command.used && !command.processing && !command.done) return true;
        return false;
      }();
      if (done) break;
      if (have_command || (stopping && !shutdown_started)) continue;
      const uint64_t now = monotonic_ms();
      const uint64_t next = adapter->NextWakeupMs();
      const uint64_t wait = changed_any || next <= now ? 1 :
          next == UINT64_MAX || next - now > kCoordinatorPollCeilingMs
              ? kCoordinatorPollCeilingMs : next - now;
      const timespec wake = realtime_after_ms(wait);
      changed.WaitUntil(lock, wake);
    }
    lanes.Close();
  }
};

ProviderExecutionCoordinator::ProviderExecutionCoordinator(bool keyless_coingecko, base::ErrorReporter* errors)
    : impl_(fortis::base::New<Impl>(keyless_coingecko, errors)), keyless_coingecko_(keyless_coingecko) {}

ProviderExecutionCoordinator::~ProviderExecutionCoordinator() {
  if (!impl_) return;
  if (impl_->started && !impl_->joined && !Stop()) abort();
  impl_.reset();
}

bool ProviderExecutionCoordinator::valid() const {
  return impl_ && impl_->lanes.valid() && impl_->adapter.has_value();
}

bool ProviderExecutionCoordinator::Start() {
  if (!valid()) return false;
  fortis::base::UniqueLock lock(impl_->mutex);
  if (impl_->started || impl_->stopping) {
    const bool already = impl_->started && !impl_->stopping;
    return already;
  }
  lock.Unlock();
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  if (pthread_attr_setstacksize(&attributes, 128 * 1024) != 0) {
    pthread_attr_destroy(&attributes); return false;
  }
  const int result = (impl_->thread.StartEntry<Impl::Entry>(impl_.get(), &attributes) ? 0 : -1);
  pthread_attr_destroy(&attributes);
  if (result != 0) return false;
  lock.Lock(); impl_->started = true;
  return true;
}

bool ProviderExecutionCoordinator::Stop() {
  if (!impl_) return true;
  fortis::base::UniqueLock lock(impl_->mutex);
  // Stop is a lifecycle operation. The coordinator thread cannot join itself,
  // and must never mark its own storage safe to destroy.
  if (impl_->started && pthread_equal(pthread_self(), impl_->thread.native())) return false;
  if (impl_->joined) return true;
  if (!impl_->started || impl_->OwnerCountLocked() != 0 ||
      impl_->LiveJobsLocked(0) != 0) {
    return false;
  }
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  if (!pthread_equal(pthread_self(), impl_->thread.native())) impl_->thread.Join();
  impl_->adapter->JoinWorkers();
  lock.Lock(); impl_->joined = true;
  return true;
}

uint64_t ProviderExecutionCoordinator::RegisterOwner(
    ProviderCoordinatorWakeFn wake, void* context) {
  if (!impl_ || !wake) return 0;
  fortis::base::LockGuard lock(impl_->mutex);
  if (!impl_->started || impl_->stopping) return 0;
  for (auto& owner : impl_->owners) if (!owner.used) {
    owner.used = true; owner.id = impl_->next_owner_id++;
    if (!owner.id) owner.id = impl_->next_owner_id++;
    owner.wake = wake; owner.context = context;
    const uint64_t id = owner.id;
    return id;
  }
  return 0;
}

bool ProviderExecutionCoordinator::UnregisterOwner(uint64_t owner_id) {
  if (!impl_ || !owner_id) return false;
  fortis::base::LockGuard lock(impl_->mutex);
  auto* owner = impl_->FindOwnerLocked(owner_id);
  if (!owner || impl_->LiveJobsLocked(owner_id)) return false;
  *owner = Impl::Owner{};
  impl_->changed.Broadcast();
  return true;
}

ProviderExecutionSubmission ProviderExecutionCoordinator::Submit(
    uint64_t owner_id, const char* lane, const http::OutboundRequest& request,
    void* context, const ProviderExecutionOptions& options) {
  ProviderExecutionSubmission rejected{};
  rejected.status = ProviderExecutionSubmitStatus::kStopping;
  if (!impl_ || !owner_id || !lane || !*lane ||
      strnlen(lane, kProviderLaneNameCapacity) >= kProviderLaneNameCapacity) {
    rejected.status = ProviderExecutionSubmitStatus::kInvalidRequest; return rejected;
  }
  fortis::base::UniqueLock lock(impl_->mutex);
  if (!impl_->FindOwnerLocked(owner_id) || impl_->stopping) return rejected;
  size_t index = kProviderCoordinatorCommandCapacity;
  for (size_t i = 0; i < kProviderCoordinatorCommandCapacity; ++i)
    if (!impl_->commands[i].used) { index = i; break; }
  while (index == kProviderCoordinatorCommandCapacity &&
         impl_->FindOwnerLocked(owner_id) && !impl_->stopping) {
    impl_->changed.Wait(lock);
    for (size_t i = 0; i < kProviderCoordinatorCommandCapacity; ++i)
      if (!impl_->commands[i].used) { index = i; break; }
  }
  if (index == kProviderCoordinatorCommandCapacity) return rejected;
  auto& command = impl_->commands[index]; command = Impl::Command{};
  command.used = true; command.kind = Impl::CommandKind::kSubmit;
  command.owner_id = owner_id; command.request = request;
  command.options = options; command.context = context;
  // Adapter retries reuse the original lane debit. Anonymous CoinGecko
  // needs each physical request to pass the pacing gate.
  if (keyless_coingecko_ && strcmp(lane, "coingecko") == 0 &&
      command.options.max_attempts > 1 &&
      command.options.max_attempts <= kProviderExecutionMaxAttempts)
    command.options.max_attempts = 1;
  snprintf(command.lane, sizeof(command.lane), "%s", lane);
  impl_->changed.Broadcast();
  while (!command.done) impl_->changed.Wait(lock);
  const auto result = command.submission;
  command = Impl::Command{};
  impl_->changed.Broadcast();
  return result;
}

bool ProviderExecutionCoordinator::Cancel(uint64_t owner_id, uint64_t job_id) {
  if (!impl_ || !owner_id || !job_id) return false;
  fortis::base::UniqueLock lock(impl_->mutex);
  if (!impl_->FindOwnerLocked(owner_id) || impl_->stopping) return false;
  size_t index = kProviderCoordinatorCommandCapacity;
  for (size_t i = 0; i < kProviderCoordinatorCommandCapacity; ++i)
    if (!impl_->commands[i].used) { index = i; break; }
  while (index == kProviderCoordinatorCommandCapacity &&
         impl_->FindOwnerLocked(owner_id) && !impl_->stopping) {
    impl_->changed.Wait(lock);
    for (size_t i = 0; i < kProviderCoordinatorCommandCapacity; ++i)
      if (!impl_->commands[i].used) { index = i; break; }
  }
  if (index == kProviderCoordinatorCommandCapacity) return false;
  auto& command = impl_->commands[index]; command = Impl::Command{};
  command.used = true; command.kind = Impl::CommandKind::kCancel;
  command.owner_id = owner_id; command.job_id = job_id;
  impl_->changed.Broadcast();
  while (!command.done) impl_->changed.Wait(lock);
  const bool result = command.cancel_result;
  command = Impl::Command{};
  impl_->changed.Broadcast();
  return result;
}

ProviderExecutionTakeStatus ProviderExecutionCoordinator::TryTakeResult(
    uint64_t owner_id, ProviderExecutionResult* output,
    unsigned char* response_body, size_t response_capacity) {
  if (!impl_ || !owner_id || !output) return ProviderExecutionTakeStatus::kNone;
  fortis::base::LockGuard lock(impl_->mutex);
  for (auto& job : impl_->jobs) {
    if (!job.used || !job.ready || job.owner_id != owner_id) continue;
    if (job.result.body_length > response_capacity ||
        (job.result.body_length && !response_body)) {
      return ProviderExecutionTakeStatus::kBufferTooSmall;
    }
    if (job.result.body_length)
      memcpy(response_body, job.body, job.result.body_length);
    *output = job.result;
    Impl::ResetJob(job);
    impl_->changed.Broadcast();
    return ProviderExecutionTakeStatus::kTaken;
  }
  return ProviderExecutionTakeStatus::kNone;
}

size_t ProviderExecutionCoordinator::live_job_count(uint64_t owner_id) const {
  if (!impl_ || !owner_id) return 0;
  fortis::base::LockGuard lock(impl_->mutex);
  const size_t count = impl_->LiveJobsLocked(owner_id);
  return count;
}

size_t ProviderExecutionCoordinator::in_flight_count() const {
  if (!impl_) return 0;
  fortis::base::LockGuard lock(impl_->mutex);
  const size_t count = impl_->LiveJobsLocked(0);
  return count;
}

}  // namespace fortis::providers
