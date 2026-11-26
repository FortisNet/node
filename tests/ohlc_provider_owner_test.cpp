#include "native_config.h"
#include "ohlc_provider_owner.h"
#include "provider_execution_coordinator.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::config;
using namespace fortis::prices;
using namespace fortis::providers;

namespace {
struct Completion {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  bool done = false;
  bool cancelled = false;
};

uint64_t now_ms() {
  timespec now{};
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void cancel_during_admission(void* context, uint64_t id) {
  assert(static_cast<OhlcProviderOwner*>(context)->Cancel(id));
}

void complete(void* context, const OhlcProviderResult& result,
              const unsigned char*, size_t) {
  auto* state = static_cast<Completion*>(context);
  pthread_mutex_lock(&state->mutex);
  state->cancelled = result.cancelled;
  state->done = true;
  pthread_cond_signal(&state->changed);
  pthread_mutex_unlock(&state->mutex);
}

void wait(Completion& state) {
  pthread_mutex_lock(&state.mutex);
  timespec deadline{};
  assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 3;
  while (!state.done)
    assert(pthread_cond_timedwait(&state.changed, &state.mutex, &deadline) == 0);
  assert(state.cancelled);
  pthread_mutex_unlock(&state.mutex);
}
}  // namespace

int main() {
  alarm(10);  // A leaked flight used to make Stop block indefinitely.
  NativeConfig config{};
  config.ca_bundle_configured = true;
  config.ca_bundle_path[0] = 'x';
  ProviderExecutionCoordinator coordinator;
  assert(coordinator.valid() && coordinator.Start());
  OhlcProviderOwner owner(config, &coordinator);
  owner.SetAdmissionHookForTest(cancel_during_admission, &owner);
  assert(owner.Start());
  for (int i = 0; i < 9; ++i) {
    Completion state{};
    uint64_t id = 0;
    assert(owner.Submit("ethereum", 7, now_ms() + 5000, complete, &state, &id));
    wait(state);
  }
  owner.Stop();
  assert(coordinator.Stop());
  alarm(0);
}
