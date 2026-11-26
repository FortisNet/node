#include "sandbox_control.h"
#include "sandbox_socket.h"
#include "file_descriptor.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fortis::operations {
namespace {
void Require(bool value) {
  if (!value) throw std::runtime_error("sandbox control refused");
}
std::string ReadSpec(const char* path) {
  base::FileDescriptor input(open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  struct stat info {};
  Require(input.get() >= 0 && fstat(input.get(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
          info.st_size <= static_cast<off_t>(codec::sandbox::kRequestLimit));
  std::string data(static_cast<size_t>(info.st_size), '\0');
  size_t received = 0;
  while (received < data.size()) {
    const ssize_t n = read(input.get(), data.data() + received, data.size() - received);
    if (n < 0 && errno == EINTR) continue;
    Require(n > 0);
    received += static_cast<size_t>(n);
  }
  char extra = 0;
  Require(read(input.get(), &extra, 1) == 0);
  return data;
}
}  // namespace
std::optional<int> SandboxControlMain(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "sandbox") return std::nullopt;
  try {
    Require(argc == 5);
    const std::string_view verb(argv[3]);
    codec::sandbox::Operation operation{};
    if (verb == "start") operation = codec::sandbox::Operation::Start;
    else if (verb == "status") operation = codec::sandbox::Operation::Status;
    else if (verb == "stop") operation = codec::sandbox::Operation::Stop;
    else Require(false);
    const auto payload = operation == codec::sandbox::Operation::Start ? ReadSpec(argv[4]) : std::string(argv[4]);
    const auto reply = sandbox::Request(argv[2], operation, payload);
    (void)std::fwrite(reply.payload.data(), 1, reply.payload.size(), stdout);
    (void)std::fputc('\n', stdout);
    return reply.code == codec::sandbox::ReplyCode::Ok || reply.code == codec::sandbox::ReplyCode::Accepted ? 0 : 1;
  } catch (const std::exception&) {
    (void)std::fputs("sandbox control unavailable or invalid local request\n", stderr);
    return 2;
  }
}
}  // namespace fortis::operations
