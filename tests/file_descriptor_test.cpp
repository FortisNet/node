#include "file_descriptor.h"
#include "unit_test_support.h"
#include <fcntl.h>
#include <stdexcept>
#include <type_traits>

namespace {
using fortis::base::FileDescriptor;
using fortis::testing::Require;
int NewFile() {
  const int fd = open("/dev/null", O_RDONLY);
  Require(fd >= 0, "open fixture");
  return fd;
}
bool Closed(int fd) { return fcntl(fd, F_GETFD) < 0 && errno == EBADF; }
void MovesAndBorrowing() {
  static_assert(!std::is_copy_constructible_v<FileDescriptor>);
  static_assert(std::is_nothrow_move_constructible_v<FileDescriptor>);
  static_assert(std::is_nothrow_move_assignable_v<FileDescriptor>);
  static_assert(std::is_nothrow_destructible_v<FileDescriptor>);
  FileDescriptor invalid(-9);
  Require(invalid.get() == -1, "negative descriptors normalize to empty");
  const int original = NewFile();
  {
    FileDescriptor owner(original);
    const int borrowed = owner.get();
    FileDescriptor next(std::move(owner));
    Require(next.get() == borrowed, "move preserves descriptor");
    FileDescriptor& self = next;
    next = std::move(self);
    Require(next.get() == borrowed, "self move retains ownership");
    next.reset(borrowed);
    Require(!Closed(borrowed), "self reset does not close a borrowed descriptor");
    const int replaced = NewFile();
    FileDescriptor replacement(replaced);
    replacement = std::move(next);
    Require(Closed(replaced) && replacement.get() == borrowed, "move assignment closes previous ownership");
    const int released = replacement.release();
    Require(replacement.get() == -1 && released == original, "release transfers");
    FileDescriptor adopted(released);
  }
  Require(Closed(original), "sole owner closes once");
}
void UnwindAndErrno() {
  const int fd = NewFile();
  bool caught = false;
  try {
    FileDescriptor guard(fd);
    throw std::bad_alloc();
  } catch (const std::bad_alloc&) { caught = true; }
  Require(caught && Closed(fd), "allocation failure after acquisition cannot leak descriptor");
  const int value = NewFile();
  errno = EDOM;
  {
    FileDescriptor guard(value);
    errno = EDOM;
  }
  Require(errno == EDOM, "cleanup preserves failure classification errno");
  // A close interrupted or reported failed must not be retried against fd reuse.
  FileDescriptor invalid(1000000);
  errno = EAGAIN;
  invalid.reset();
  Require(errno == EAGAIN && invalid.get() == -1, "failed close leaves owner empty and preserves errno");
}
}  // namespace
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("move/self move/borrow/reset/release", MovesAndBorrowing);
  suite.Run("exception unwind/errno", UnwindAndErrno);
  return suite.Finish("file_descriptor_test");
}
