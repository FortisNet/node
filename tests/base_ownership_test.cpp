// Unit test of the shared ownership helpers (src/base/ownership.h).
#include <utility>
#include <memory>
#include "base/ownership.h"
#include "base/stdlib_check.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

namespace {

using fortis::base::Buffer;
using fortis::base::Delete;
using fortis::base::MakeUnique;
using fortis::base::New;
using fortis::base::NewDefaultInit;

int g_constructed = 0;
int g_destroyed = 0;
// The destruction log: tags in the order the destructors ran.
int g_order[8];
int g_order_count = 0;

struct Tracked {
  int tag;
  int extra;
  Tracked() : tag(0), extra(0) { ++g_constructed; }
  Tracked(int tag_in, int extra_in) : tag(tag_in), extra(extra_in) { ++g_constructed; }
  ~Tracked() {
    ++g_destroyed;
    if (g_order_count < 8) g_order[g_order_count++] = tag;
  }
  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
};

void ResetCounters() {
  g_constructed = 0;
  g_destroyed = 0;
  g_order_count = 0;
}

struct Plain {
  int value = 7;
  char text[16];
};

// Takes a move-only argument and an lvalue reference: New must forward both.
struct Forwarded {
  std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> owned;
  int* counter;
  Forwarded(std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>&& owned_in, int& counter_in)
      : owned(std::move(owned_in)), counter(&counter_in) {}
};

void TestNewDelete() {
  ResetCounters();
  Tracked* object = New<Tracked>(3, 4);
  assert(object && object->tag == 3 && object->extra == 4);
  assert(g_constructed == 1 && g_destroyed == 0);
  Delete(object);
  assert(g_destroyed == 1);

  // Null-safe, and a const pointer is accepted.
  Delete(static_cast<Tracked*>(nullptr));
  const Tracked* constant = New<Tracked>();
  assert(constant && constant->tag == 0);
  Delete(constant);
  assert(g_constructed == 2 && g_destroyed == 2);

  // No arguments value-initializes: members without an initializer are zero.
  Plain* plain = New<Plain>();
  assert(plain && plain->value == 7);
  for (size_t i = 0; i < sizeof(plain->text); ++i) assert(plain->text[i] == 0);
  Delete(plain);

  // Default-initialization still runs member initializers and constructors.
  Plain* raw = NewDefaultInit<Plain>();
  assert(raw && raw->value == 7);
  Delete(raw);
  Tracked* tracked = NewDefaultInit<Tracked>();
  assert(tracked && g_constructed == 3);
  Delete(tracked);
  assert(g_destroyed == 3);

  // A scalar works too.
  uint64_t* number = New<uint64_t>(UINT64_C(41));
  assert(number && *number == 41);
  Delete(number);
}

void TestForwarding() {
  ResetCounters();
  int counter = 0;
  std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> inner = MakeUnique<Tracked>(9, 0);
  Forwarded* outer = New<Forwarded>(std::move(inner), counter);
  // The moved-from empty state is the contract being tested here.
  // cppcheck-suppress accessMoved
  assert(outer && !inner && outer->owned && outer->owned->tag == 9);
  assert(outer->counter == &counter);
  Delete(outer);
  assert(g_constructed == 1 && g_destroyed == 1);
}

void TestUniquePtr() {
  static_assert(
      std::is_same_v<decltype(MakeUnique<Tracked>()), std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>>);
  static_assert(!std::is_copy_constructible_v<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>>);
  static_assert(std::is_nothrow_move_constructible_v<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>>);
  ResetCounters();
  {
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> empty;
    assert(!empty && empty.get() == nullptr && empty == nullptr);
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> null_built = nullptr;
    assert(!null_built);

    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> first(New<Tracked>(1, 10));
    assert(first && first != nullptr && first->extra == 10 && (*first).tag == 1);

    // Move construction and assignment transfer ownership without destroying.
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> second(std::move(first));
    // The moved-from empty state is the contract being tested here.
    // cppcheck-suppress accessMoved
    assert(!first && second && second->tag == 1 && g_destroyed == 0);
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> third;
    third = std::move(second);
    // The moved-from empty state is the contract being tested here.
    // cppcheck-suppress accessMoved
    assert(!second && third->tag == 1 && g_destroyed == 0);

    // Move assignment over a held object destroys that object.
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> other(New<Tracked>(2, 20));
    third = std::move(other);
    // The moved-from empty state is the contract being tested here.
    // cppcheck-suppress accessMoved
    assert(g_destroyed == 1 && g_order[0] == 1 && third->tag == 2 && !other);

    // Self move-assignment keeps the object.
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>& alias = third;
    third = std::move(alias);
    assert(third && third->tag == 2 && g_destroyed == 1);

    // release() hands the object over undestroyed.
    Tracked* released = third.release();
    assert(!third && released->tag == 2 && g_destroyed == 1);
    third.reset(released);
    assert(third.get() == released);

    // reset() replaces, reset() with nothing empties, = nullptr empties.
    third.reset(New<Tracked>(3, 30));
    assert(g_destroyed == 2 && g_order[1] == 2 && third->tag == 3);
    third.reset();
    assert(!third && g_destroyed == 3 && g_order[2] == 3);
    third.reset();
    assert(g_destroyed == 3);
    third.reset(New<Tracked>(4, 40));
    third = nullptr;
    assert(!third && g_destroyed == 4);

    third = MakeUnique<Tracked>(5, 50);
    assert(third && third->tag == 5);
  }
  // Leaving scope destroyed the one object still held.
  assert(g_constructed == 5 && g_destroyed == 5 && g_order[4] == 5);
  static_assert(sizeof(std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>) == sizeof(Tracked*),
                "the handle is one pointer wide");
}

// Standard reset publishes the replacement before invoking its old deleter.
// Do not depend on an owning handle's state during the handle destructor.
struct Observer;
std::unique_ptr<Observer, fortis::base::ObjectDeleter<Observer>>* g_handle = nullptr;
const Observer* g_seen = nullptr;
bool g_looked = false;
struct Observer {
  ~Observer() {
    if (g_handle) g_seen = g_handle->get();
    g_looked = true;
  }
};

void TestDestructionOrder() {
  ResetCounters();
  {
    // Members destruct in reverse declaration order.
    struct Owner {
      std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> a{New<Tracked>(1, 0)};
      std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> b{New<Tracked>(2, 0)};
      std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> c{New<Tracked>(3, 0)};
    } owner;
    assert(owner.a && owner.b && owner.c);
  }
  assert(g_order_count == 3 && g_order[0] == 3 && g_order[1] == 2 && g_order[2] == 1);

  ResetCounters();
  {
    // An explicit reset() sequence overrides it.
    struct Owner {
      std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> a{New<Tracked>(1, 0)};
      std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> b{New<Tracked>(2, 0)};
      std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> c{New<Tracked>(3, 0)};
      ~Owner() {
        b.reset();
        a.reset();
      }
    } owner;
    assert(owner.c);
  }
  assert(g_order_count == 3 && g_order[0] == 2 && g_order[1] == 1 && g_order[2] == 3);

  std::unique_ptr<Observer, fortis::base::ObjectDeleter<Observer>> handle(New<Observer>());
  g_handle = &handle;
  handle.reset();
  assert(g_looked && g_seen == nullptr && !handle);
  // Replacement is visible during destruction as well; old object destruction
  // never reads a dangling value from its owner.
  handle.reset(New<Observer>());
  Observer* replacement = New<Observer>();
  g_looked = false;
  handle.reset(replacement);
  assert(g_looked && g_seen == replacement && handle.get() == replacement);
  // Explicit release also detaches before invoking the deleter.
  handle.reset(New<Observer>());
  g_looked = false;
  Delete(handle.release());
  assert(g_looked && g_seen == nullptr);
  g_looked = false;
  {
    std::unique_ptr<Observer, fortis::base::ObjectDeleter<Observer>> scoped(New<Observer>());
    g_handle = nullptr;
  }
  assert(g_looked);
}

int g_custom_deleted = 0;
struct CountingDeleter {
  void operator()(Plain* object) const {
    ++g_custom_deleted;
    Delete(object);
  }
};

void TestCustomDeleter() {
  {
    std::unique_ptr<Plain, CountingDeleter> handle(New<Plain>());
    assert(handle && handle->value == 7);
    handle.reset(New<Plain>());
    assert(g_custom_deleted == 1);
    std::unique_ptr<Plain, CountingDeleter> moved(std::move(handle));
    // The moved-from empty state is the contract being tested here.
    // cppcheck-suppress accessMoved
    assert(!handle && moved);
  }
  assert(g_custom_deleted == 2);

  // StorageDeleter owns a raw block.
  auto block = fortis::base::AllocateStorage<char>(32);
  assert(block);
  memset(block.get(), 'x', 32);
}

void TestBuffer() {
  Buffer empty;
  assert(!empty && empty.data() == nullptr && empty.size() == 0);
  empty.reset();

  Buffer buffer = Buffer::Allocate(64);
  assert(buffer && buffer.size() == 64);
  memset(buffer.data(), 0xAB, buffer.size());
  assert(static_cast<void*>(buffer.chars()) == static_cast<void*>(buffer.data()));

  Buffer zeroed = Buffer::AllocateZeroed(48);
  assert(zeroed && zeroed.size() == 48);
  for (size_t i = 0; i < zeroed.size(); ++i) assert(zeroed.data()[i] == 0);

  // Moves transfer the block and empty the source.
  unsigned char* block = buffer.data();
  Buffer moved(std::move(buffer));
  // The moved-from empty state is the contract being tested here.
  // cppcheck-suppress accessMoved
  // NOLINTNEXTLINE(bugprone-use-after-move): verifies the documented empty source after a move.
  assert(!buffer && buffer.size() == 0 && moved.data() == block && moved.size() == 64);
  zeroed = std::move(moved);
  // The moved-from empty state is the contract being tested here.
  // cppcheck-suppress accessMoved
  // NOLINTNEXTLINE(bugprone-use-after-move): verifies the documented empty source after a move.
  assert(!moved && zeroed.data() == block && zeroed.size() == 64);
  Buffer& alias = zeroed;
  zeroed = std::move(alias);
  assert(zeroed.data() == block && zeroed.size() == 64);

  // release() hands the block over; Adopt() takes one.
  unsigned char* released = zeroed.release();
  assert(released == block && !zeroed && zeroed.size() == 0);
  Buffer adopted = Buffer::Adopt(released, 64);
  assert(adopted.data() == block && adopted.size() == 64 && adopted.data()[0] == 0xAB);
  adopted.reset();
  assert(!adopted && adopted.size() == 0);

  // A null block has no size.
  Buffer null_adopted = Buffer::Adopt(nullptr, 10);
  assert(!null_adopted && null_adopted.size() == 0);
}

}  // namespace

// The start-up runtime self-check (src/base/stdlib_check.h) passes on the host:
// the allocator, std::array / std::unique_ptr and a caught std::out_of_range.
void TestStandardLibraryCheck() {
  assert(fortis::base::StandardLibraryUsable());
  assert(fortis::base::StandardLibraryUsable());
}

namespace {
int construction_live = 0;
struct ConstructionMember {
  ConstructionMember() { ++construction_live; }
  ~ConstructionMember() { --construction_live; }
};
struct ThrowingConstruction {
  ConstructionMember member;
  ThrowingConstruction() { throw std::bad_alloc(); }
};
void TestThrowingConstructionStorage() {
  for (bool default_init : {false, true}) {
    bool caught = false;
    try {
      if (default_init) (void)fortis::base::NewDefaultInit<ThrowingConstruction>();
      else (void)fortis::base::New<ThrowingConstruction>();
    } catch (const std::bad_alloc&) { caught = true; }
    assert(caught && construction_live == 0);
  }
}

void TestRawStorageOwners() {
  auto bytes = fortis::base::AllocateStorage<unsigned char[]>(16);
  static_assert(std::is_same_v<decltype(bytes), std::unique_ptr<unsigned char[], fortis::base::StorageDeleter>>);
  assert(bytes);
  for (size_t i = 0; i < 16; ++i) bytes[i] = static_cast<unsigned char>(i);
  const auto address = reinterpret_cast<uintptr_t>(bytes.get());
  assert(fortis::base::ResizeStorage(bytes, 16, 64));
  assert(reinterpret_cast<uintptr_t>(bytes.get()) != address);
  for (size_t i = 0; i < 16; ++i) assert(bytes[i] == i);
  auto* preserved = bytes.get();
  assert(!fortis::base::ResizeStorage(bytes, 16, SIZE_MAX) && bytes.get() == preserved && errno == ENOMEM);
  for (size_t i = 0; i < 16; ++i) assert(bytes[i] == i);
  assert(fortis::base::ResizeStorage(bytes, 16, 8));
  for (size_t i = 0; i < 8; ++i) assert(bytes[i] == i);
  assert(fortis::base::ResizeStorage(bytes, 8, 0) && !bytes);
  assert(fortis::base::ResizeStorage(bytes, 0, 8) && bytes);
  struct alignas(max_align_t) Aligned {
    unsigned char data[sizeof(max_align_t)];
  };
  auto aligned = fortis::base::MakeUnique<Aligned>();
  assert(aligned && reinterpret_cast<uintptr_t>(aligned.get()) % alignof(Aligned) == 0);
  auto zeroed = fortis::base::AllocateZeroedStorage<uint64_t[]>(4, sizeof(uint64_t));
  assert(zeroed);
  for (size_t i = 0; i < 4; ++i) assert(zeroed[i] == 0);
  assert(!fortis::base::AllocateZeroedStorage<uint64_t[]>(SIZE_MAX, sizeof(uint64_t)) && errno == ENOMEM);
}
}  // namespace

template <typename T>
concept TemporaryBytes = requires(T value) {
  std::move(value).bytes();
};
template <typename T>
concept TemporaryData = requires(T value) {
  std::move(value).data();
};
void TestBorrowedBytes() {
  static_assert(!TemporaryBytes<Buffer> && !TemporaryData<Buffer>);
  static_assert(std::is_same_v<decltype(std::declval<const Buffer&>().bytes()), std::span<const unsigned char>>);
  static_assert(std::is_same_v<decltype(std::declval<const Buffer&>().data()), const unsigned char*>);
  auto owner = Buffer::AllocateZeroed(16);
  auto borrowed = owner.bytes();
  assert(borrowed.size() == 16 && borrowed.data() == owner.data());
  borrowed[15] = 42;
  const Buffer& immutable = owner;
  assert(immutable.bytes()[15] == 42 && immutable.bytes().size() == 16);
  auto moved = std::move(owner);
  assert(moved.bytes().data() == borrowed.data() && moved.bytes()[15] == 42);
  // Moving preserves the allocation, but all subsequent access uses its new owner.
  assert(moved.size() == 16);
  moved.reset();
  assert(moved.bytes().empty());  // Never dereference the invalidated old span.
  assert(!Buffer::Allocate(SIZE_MAX));
}

int main() {
  TestThrowingConstructionStorage();
  TestRawStorageOwners();
  TestStandardLibraryCheck();
  TestNewDelete();
  TestForwarding();
  TestUniquePtr();
  TestDestructionOrder();
  TestCustomDeleter();
  TestBuffer();
  TestBorrowedBytes();
  printf("base_ownership_test: ok\n");
  return 0;
}
