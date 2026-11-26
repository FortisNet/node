// Unit test of Status and Result<T> (src/base/status.h).
#include <utility>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include "base/status.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {

using fortis::base::MakeUnique;
using fortis::base::Result;
using fortis::base::Status;
using fortis::base::StatusCode;

int g_live = 0;

struct Tracked {
  int tag;
  explicit Tracked(int tag_in) : tag(tag_in) { ++g_live; }
  ~Tracked() { --g_live; }
  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
};

// The pattern Result exists for: a private constructor and a fallible Create.
class Widget {
 public:
  static Result<std::unique_ptr<Widget, fortis::base::ObjectDeleter<Widget>>> Create(int size) {
    if (size <= 0) return Status(StatusCode::kInvalidArgument, "widget size must be positive");
    std::unique_ptr<Widget, fortis::base::ObjectDeleter<Widget>> widget(fortis::base::New<Widget>(size));
    if (!widget) return Status(StatusCode::kResourceExhausted, "widget allocation");
    return widget;
  }
  int size() const { return size_; }

 private:
  friend Widget* fortis::base::New<Widget, int&>(int&);
  explicit Widget(int size) : size_(size) {}
  int size_;
};

Result<int> Half(int value) {
  if (value % 2) return Status(StatusCode::kInvalidArgument, "odd");
  return value / 2;
}

void TestStatus() {
  const Status ok;
  assert(ok.ok() && ok.code() == StatusCode::kOk && ok == StatusCode::kOk);
  assert(strcmp(ok.message(), "ok") == 0);
  assert(Status::Ok().ok());

  const Status failed(StatusCode::kUnavailable, "backfill owner");
  assert(!failed.ok() && failed == StatusCode::kUnavailable && failed != StatusCode::kOk);
  assert(strcmp(failed.message(), "backfill owner") == 0);

  // Without a message the code names itself; the message is never null.
  const Status bare(StatusCode::kIoError);
  assert(strcmp(bare.message(), "I/O error") == 0);
  const Status null_message(StatusCode::kDataLoss, nullptr);
  assert(strcmp(null_message.message(), "data loss") == 0);

  // Every code has a distinct name.
  for (int left = 0; left <= static_cast<int>(StatusCode::kInternal); ++left)
    for (int right = left + 1; right <= static_cast<int>(StatusCode::kInternal); ++right)
      assert(strcmp(fortis::base::StatusCodeName(static_cast<StatusCode>(left)),
                    fortis::base::StatusCodeName(static_cast<StatusCode>(right))) != 0);

  // Two words, trivially copyable, usable in constant expressions.
  static_assert(sizeof(Status) <= 2 * sizeof(void*), "Status stays two words");
  static_assert(__is_trivially_copyable(Status), "Status is trivially copyable");
  constexpr Status constant(StatusCode::kNotFound, "constant");
  static_assert(!constant.ok() && constant.code() == StatusCode::kNotFound, "constexpr Status");
}

void TestResultValue() {
  Result<int> four = Half(8);
  assert(four.ok() && four && four.value() == 4 && four.status().ok());
  four.value() = 5;
  assert(four.TakeValue() == 5);

  Result<int> odd = Half(7);
  assert(!odd.ok() && !odd && odd.status() == StatusCode::kInvalidArgument);
  assert(strcmp(odd.status().message(), "odd") == 0);
  assert(std::move(odd).ValueOr(-1) == -1);
  assert(Half(6).ValueOr(-1) == 3);

  // An ok Status holds no value: the Result is a failure, not an empty success.
  Result<int> confused = Status::Ok();
  assert(!confused.ok() && confused.status() == StatusCode::kInternal);
}

void TestResultMoveOnly() {
  assert(g_live == 0);
  {
    Result<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>> made = MakeUnique<Tracked>(7);
    assert(made.ok() && made.value()->tag == 7 && g_live == 1);

    // Moving a Result moves the value; the source keeps a moved-from (empty) handle.
    Result<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>> moved = std::move(made);
    assert(moved.ok() && moved.value()->tag == 7 && g_live == 1);
    assert(made.ok() && !made.value());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): verifies the
                                         // documented empty source after a move.

    // Move assignment destroys the value held before.
    Result<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>> other = MakeUnique<Tracked>(9);
    assert(g_live == 2);
    other = std::move(moved);
    assert(g_live == 1 && other.value()->tag == 7);

    // A failure assigned over a value destroys the value.
    other = Result<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>>(
        Status(StatusCode::kCancelled, "stopped"));
    assert(g_live == 0 && !other.ok() && other.status() == StatusCode::kCancelled);

    // And a value assigned over a failure is held.
    other = Result<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>>(MakeUnique<Tracked>(3));
    assert(g_live == 1 && other.ok());
    std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>> taken = other.TakeValue();
    assert(taken->tag == 3 && g_live == 1);
  }
  // The Result's destructor releases whatever it still held.
  assert(g_live == 0);
  {
    Result<std::unique_ptr<Tracked, fortis::base::ObjectDeleter<Tracked>>> dropped = MakeUnique<Tracked>(1);
    assert(g_live == 1);
  }
  assert(g_live == 0);
}

int address_safe_live = 0;
struct AddressSafe {
  int value;
  explicit AddressSafe(int number) : value(number) { ++address_safe_live; }
  AddressSafe(const AddressSafe& other) : value(other.value) { ++address_safe_live; }
  AddressSafe(AddressSafe&& other) noexcept : value(std::exchange(other.value, -1)) { ++address_safe_live; }
  ~AddressSafe() { --address_safe_live; }
  AddressSafe* operator&() = delete;
  const AddressSafe* operator&() const = delete;
};

void TestResultWithOverloadedAddress() {
  // Generic lifetime management must obtain the real union-member address,
  // even when the value type disables the unary address-of operator.
  assert(address_safe_live == 0);
  {
    AddressSafe original(5);
    const AddressSafe constant(6);
    Result<AddressSafe> copy(original);
    Result<AddressSafe> const_copy(constant);
    Result<AddressSafe> moved(std::move(copy));
    Result<AddressSafe> assigned(AddressSafe(7));
    assigned = std::move(moved);
    const auto taken = assigned.TakeValue();
    assert(taken.value == 5 && const_copy.value().value == 6);
  }
  assert(address_safe_live == 0);
}

void TestCreatePattern() {
  Result<std::unique_ptr<Widget, fortis::base::ObjectDeleter<Widget>>> refused = Widget::Create(0);
  assert(!refused.ok() && refused.status() == StatusCode::kInvalidArgument);
  assert(strcmp(refused.status().message(), "widget size must be positive") == 0);

  Result<std::unique_ptr<Widget, fortis::base::ObjectDeleter<Widget>>> made = Widget::Create(12);
  assert(made.ok());
  std::unique_ptr<Widget, fortis::base::ObjectDeleter<Widget>> widget = made.TakeValue();
  assert(widget && widget->size() == 12);
}

struct ThrowingMove {
  static inline int live = 0;
  static inline bool fail = false;
  int value;
  explicit ThrowingMove(int number) : value(number) { ++live; }
  ThrowingMove(ThrowingMove&& other) noexcept(false) : value(std::exchange(other.value, -1)) {
    if (fail) throw std::runtime_error("move failed");
    ++live;
  }
  ThrowingMove(const ThrowingMove&) = delete;
  ThrowingMove& operator=(const ThrowingMove&) = delete;
  ThrowingMove& operator=(ThrowingMove&&) = delete;
  ~ThrowingMove() { --live; }
};

void TestThrowingResultMoves() {
  static_assert(std::is_nothrow_move_constructible_v<Result<int>>);
  static_assert(std::is_nothrow_move_assignable_v<Result<int>>);
  static_assert(!std::is_nothrow_move_constructible_v<Result<ThrowingMove>>);
  static_assert(!std::is_nothrow_move_assignable_v<Result<ThrowingMove>>);
  static_assert(std::is_nothrow_destructible_v<Result<ThrowingMove>>);
  assert(ThrowingMove::live == 0);
  {
    Result<ThrowingMove> source(ThrowingMove(7));
    ThrowingMove::fail = true;
    bool caught = false;
    try {
      Result<ThrowingMove> destination(std::move(source));
    } catch (const std::runtime_error&) { caught = true; }
    assert(caught && ThrowingMove::live == 1);
    ThrowingMove::fail = false;
    source = Result<ThrowingMove>(ThrowingMove(8));
    Result<ThrowingMove> destination(ThrowingMove(9));
    ThrowingMove::fail = true;
    caught = false;
    try {
      destination = std::move(source);
    } catch (const std::runtime_error&) { caught = true; }
    assert(caught && !destination.ok() && destination.status() == StatusCode::kInternal);
    assert(ThrowingMove::live == 1);
    // The source remains engaged with the value's documented moved-from state.
    // cppcheck-suppress accessMoved
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    assert(source.ok() && source.status().ok() && source.value().value == -1);
    ThrowingMove::fail = false;
    destination = Result<ThrowingMove>(ThrowingMove(11));
    assert(destination.ok() && destination.status().ok() && destination.value().value == 11);
    destination = Result<ThrowingMove>(Status(StatusCode::kCancelled));
    assert(!destination.ok() && destination.status() == StatusCode::kCancelled);
  }
  assert(ThrowingMove::live == 0);
}

}  // namespace

template <typename T>
concept TemporaryResultReference = requires(T value) {
  std::move(value).value();
};
static_assert(!TemporaryResultReference<Result<int>>);
void TestThrowingTakeAndSelfMove() {
  Result<ThrowingMove> source(ThrowingMove(19));
  ThrowingMove::fail = true;
  bool caught = false;
  try {
    auto value = source.TakeValue();
    (void)value;
  } catch (const std::runtime_error&) { caught = true; }
  assert(caught && source.ok() && source.status().ok() && ThrowingMove::live == 1);
  // T's throwing move provides its own basic guarantee; the Result remains engaged.
  ThrowingMove::fail = false;
  Result<ThrowingMove>& alias = source;
  source = std::move(alias);
  assert(source.ok() && ThrowingMove::live == 1);
}

int main() try {
  TestStatus();
  TestResultValue();
  TestResultMoveOnly();
  TestCreatePattern();
  TestResultWithOverloadedAddress();
  TestThrowingResultMoves();
  TestThrowingTakeAndSelfMove();
  puts("base status passed");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "base status failed: %s\n", error.what());
  return 1;
} catch (...) {
  fputs("base status failed: unexpected non-standard exception\n", stderr);
  return 1;
}
