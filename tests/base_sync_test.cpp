// Unit test of the shared concurrency primitives (src/base/threading.h).
#include "base/threading.h"

#include <assert.h>
#include <mutex>
#include <type_traits>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

namespace {

using fortis::base::ConditionVariable;
using fortis::base::LockGuard;
using fortis::base::Mutex;
using fortis::base::Thread;
using fortis::base::UniqueLock;

// Constant-initialized: usable from a static constructor of another unit.
Mutex g_mutex;
ConditionVariable g_changed;

timespec RealtimeAfterMs(long ms) {
  timespec at{};
  clock_gettime(CLOCK_REALTIME, &at);
  at.tv_sec += ms / 1000;
  at.tv_nsec += (ms % 1000) * 1000000L;
  if (at.tv_nsec >= 1000000000L) {
    ++at.tv_sec;
    at.tv_nsec -= 1000000000L;
  }
  return at;
}

// True when another thread can take the mutex right now.
struct Probe {
  pthread_mutex_t* mutex;
  bool free;
};
void ProbeMain(Probe* probe) {
  probe->free = pthread_mutex_trylock(probe->mutex) == 0;
  if (probe->free) pthread_mutex_unlock(probe->mutex);
}
bool IsFree(pthread_mutex_t* mutex) {
  Probe probe{mutex, false};
  Thread thread;
  assert(thread.Start(ProbeMain, &probe));
  thread.Join();
  return probe.free;
}

void TestMutex() {
  Mutex mutex;
  assert(mutex.Lock());
  assert(!IsFree(mutex.native()));
  mutex.Unlock();
  assert(IsFree(mutex.native()));
  assert(mutex.TryLock());
  assert(!IsFree(mutex.native()));
  mutex.Unlock();
  // The static mutex needs no initialization call.
  assert(g_mutex.TryLock());
  g_mutex.Unlock();
}

void TestLockGuard() {
  Mutex mutex;
  {
    LockGuard guard(mutex);
    assert(guard.locked());
    assert(!IsFree(mutex.native()));
  }
  assert(IsFree(mutex.native()));

  // A mutex with an explicit lifecycle stays a pthread_mutex_t.
  pthread_mutex_t raw;
  assert(pthread_mutex_init(&raw, nullptr) == 0);
  {
    LockGuard guard(raw);
    assert(guard.locked());
    assert(!IsFree(&raw));
  }
  assert(IsFree(&raw));
  assert(pthread_mutex_destroy(&raw) == 0);
}

int EarlyReturn(Mutex& mutex, bool early) {
  UniqueLock lock(mutex);
  if (early) return 1;
  lock.Unlock();
  return IsFree(mutex.native()) ? 2 : 3;
}

void TestUniqueLock() {
  Mutex mutex;
  {
    UniqueLock lock(mutex);
    assert(lock.locked());
    assert(lock.native() == mutex.native());
    assert(!IsFree(mutex.native()));
    lock.Unlock();
    assert(!lock.locked());
    assert(IsFree(mutex.native()));
    lock.Unlock();  // A second release is a no-op, not a double unlock.
    assert(IsFree(mutex.native()));
    assert(lock.Lock());
    assert(lock.locked());
    assert(!IsFree(mutex.native()));
    assert(lock.Lock());  // Already held: no second pthread_mutex_lock.
    lock.Unlock();
    assert(IsFree(mutex.native()));
  }
  // Destroyed unlocked: the destructor did not unlock a mutex it did not hold.
  assert(mutex.TryLock());
  mutex.Unlock();
  {
    UniqueLock lock(mutex);
    lock.Unlock();
    assert(lock.Lock());
  }
  assert(IsFree(mutex.native()));  // Destroyed locked: released.
  assert(EarlyReturn(mutex, true) == 1);
  assert(IsFree(mutex.native()));
  assert(EarlyReturn(mutex, false) == 2);
  assert(IsFree(mutex.native()));

  pthread_mutex_t raw = PTHREAD_MUTEX_INITIALIZER;
  {
    UniqueLock lock(raw);
    assert(lock.native() == &raw);
    assert(!IsFree(&raw));
  }
  assert(IsFree(&raw));
}

struct Queue {
  Mutex mutex;
  ConditionVariable changed;
  int produced = 0;
  int consumed = 0;
  bool done = false;
};

void Consumer(Queue* queue) {
  UniqueLock lock(queue->mutex);
  for (;;) {
    while (queue->consumed == queue->produced && !queue->done) queue->changed.Wait(lock);
    if (queue->consumed == queue->produced) return;
    ++queue->consumed;
    queue->changed.Broadcast();
  }
}

void TestConditionVariable() {
  Queue queue;
  Thread consumers[3];
  for (Thread& consumer : consumers) assert(consumer.Start(Consumer, &queue, 256 * 1024));
  for (int i = 0; i < 2000; ++i) {
    UniqueLock lock(queue.mutex);
    ++queue.produced;
    queue.changed.Signal();
    // Hand-off: wait until this item was taken before producing the next.
    while (queue.consumed != queue.produced) queue.changed.Wait(lock);
  }
  {
    LockGuard guard(queue.mutex);
    queue.done = true;
  }
  queue.changed.Broadcast();
  for (Thread& consumer : consumers) consumer.Join();
  assert(queue.consumed == 2000);

  // A timed wait with no signal times out, holding the lock again on return.
  Mutex mutex;
  ConditionVariable never;
  UniqueLock lock(mutex);
  const timespec past = RealtimeAfterMs(-1000);
  assert(never.WaitUntil(lock, past) == ETIMEDOUT);
  const timespec soon = RealtimeAfterMs(30);
  int result = 0;
  while (result == 0) result = never.WaitUntil(lock, soon);  // Spurious wakes re-wait.
  assert(result == ETIMEDOUT);
  assert(lock.locked());
  assert(!IsFree(mutex.native()));
  lock.Unlock();

  // The static condition variable needs no initialization call.
  UniqueLock global(g_mutex);
  assert(g_changed.WaitUntil(global, past) == ETIMEDOUT);
}

struct Counter {
  Mutex mutex;
  uint64_t value = 0;
  pthread_t ran_on{};
  const Thread* owner = nullptr;
  bool owner_saw_current = false;
};

void Count(Counter* counter) {
  for (int i = 0; i < 20000; ++i) {
    LockGuard guard(counter->mutex);
    ++counter->value;
  }
}

void RecordThread(Counter* counter) {
  LockGuard guard(counter->mutex);  // The starter holds it until Start returned.
  counter->ran_on = pthread_self();
  counter->owner_saw_current = counter->owner->IsCurrent();
}

void ConstEntry(const int* value) { assert(*value == 7); }

void TestThread() {
  Thread idle;
  assert(!idle.started());
  assert(!idle.IsCurrent());
  idle.Join();  // Nothing started: a no-op.

  Counter counter;
  {
    Thread threads[4];
    for (Thread& thread : threads) assert(thread.Start(Count, &counter));
    assert(threads[0].started());
    assert(!threads[0].Start(Count, &counter));  // One thread per owner.
    // No Join: the destructors join.
  }
  assert(counter.value == 80000);

  Thread thread;
  counter.owner = &thread;
  {
    LockGuard guard(counter.mutex);
    assert(thread.Start(RecordThread, &counter, 128 * 1024));
    assert(!thread.IsCurrent());
  }
  const pthread_t native = thread.native();
  thread.Join();
  assert(!thread.started());
  assert(pthread_equal(native, counter.ran_on));
  assert(counter.owner_saw_current);
  thread.Join();  // Idempotent.

  // A joined owner can start again, and a const context is typed too.
  const int seven = 7;
  assert(thread.Start(ConstEntry, &seven));
  thread.Join();
  assert(thread.Start(Count, &counter));
  thread.Join();
  assert(counter.value == 100000);

  // An impossible stack size fails without leaving a thread to join.
  Thread refused;
  assert(!refused.Start(Count, &counter, 1));
  assert(!refused.started());
  refused.Join();
  assert(counter.value == 100000);
}

void TestCallableAndStandardLocks() {
  static_assert(std::is_trivially_destructible_v<fortis::base::ThreadHandle>,
                "static thread handles must not join at exit");
  static_assert(std::is_trivially_destructible_v<Mutex>);
  static_assert(std::is_trivially_destructible_v<ConditionVariable>);
  Mutex mutex;
  Thread thread;
  int observed = 0;
  {
    std::unique_lock<Mutex> hold(mutex);
    {
      int copied = 37;
      const bool started = thread.Start(
          [&, copied] {
            std::lock_guard<Mutex> lock(mutex);
            observed = copied;
          },
          256 * 1024);
      assert(started);
    }  // The temporary callable and copied local have gone away.
    assert(!thread.Start([] {}));
    hold.unlock();
  }
  thread.Join();
  assert(observed == 37);
  const bool restarted = thread.Start([&] { ++observed; });
  assert(restarted);
  thread.Join();
  assert(observed == 38);
  try {
    std::lock_guard<Mutex> hold(mutex);
    throw 1;
  } catch (int value) { assert(value == 1); }
  assert(IsFree(mutex.native()));
  std::unique_lock<Mutex> try_lock(mutex, std::try_to_lock);
  assert(try_lock.owns_lock());
  try_lock.unlock();
  assert(!thread.Start([] {}, 1));
  assert(!thread.started());
}

}  // namespace

void TestImmediateIdentityAndSelfJoin() {
  static_assert(!std::is_convertible_v<Thread*, fortis::base::ThreadHandle*>);
  static_assert(sizeof(Thread) == sizeof(fortis::base::ThreadHandle));
  for (unsigned i = 0; i < 200; ++i) {
    fortis::base::ThreadHandle owner;
    bool identity = false, retained = false;
    const bool started = owner.Start([&] {
      identity = owner.IsCurrent();
      retained = !owner.Join();  // Must not mark a still-executing handle idle.
    });
    assert(started);
    const bool joined = owner.Join();
    assert(joined);
    assert(identity && retained && !owner.started() && !owner.IsCurrent());
  }
  // Stop/wake precedes joining: callback may still borrow the owner's state.
  Mutex mutex;
  ConditionVariable available;
  bool stop = false, exited = false;
  fortis::base::Thread worker;
  const bool started = worker.Start([&] {
    UniqueLock lock(mutex);
    while (!stop) available.Wait(lock);
    exited = true;
  });
  assert(started);
  {
    LockGuard lock(mutex);
    stop = true;
    available.Broadcast();
  }
  const bool joined = worker.Join();
  assert(joined && exited);
}

int main() {
  TestMutex();
  TestLockGuard();
  TestUniqueLock();
  TestConditionVariable();
  TestThread();
  TestImmediateIdentityAndSelfJoin();
  TestCallableAndStandardLocks();
  puts("base sync primitives passed");
  return 0;
}
