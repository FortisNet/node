// Unit test of the process logger (src/base/log.h).
#include "base/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <new>
#include <stdexcept>

// stderr is redirected for the whole test, so a failed check reports on stdout.
#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      fflush(stdout); \
      abort(); \
    } \
  } while (0)

namespace {

using fortis::base::LogLevel;
using fortis::base::LogRecord;
using fortis::base::LogSink;

// Captures records. Write runs on any thread, so the state is locked.
class CaptureSink final : public LogSink {
 public:
  explicit CaptureSink(bool consume) : consume_(consume) {}
  bool Write(const LogRecord& record) override {
    pthread_mutex_lock(&mutex_);
    if (count_ < kCapacity) {
      Line& line = lines_[count_];
      line.level = record.level;
      snprintf(line.component, sizeof(line.component), "%s", record.component);
      snprintf(line.text, sizeof(line.text), "%s", record.text);
      line.length = record.length;
      line.terminated = record.text[record.length] == '\0';
    }
    ++count_;
    pthread_mutex_unlock(&mutex_);
    return consume_;
  }
  struct Line {
    LogLevel level;
    char component[32];
    char text[fortis::base::kLogLineCapacity + 8];
    size_t length;
    bool terminated;
  };
  static constexpr size_t kCapacity = 16;
  size_t count() const { return count_; }
  const Line& line(size_t index) const { return lines_[index]; }
  void Clear() { count_ = 0; }

 private:
  bool consume_;
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  size_t count_ = 0;
  Line lines_[kCapacity]{};
};

// Points fd 2 at a temporary file for the lifetime of the object.
class StderrFile {
 public:
  StderrFile() {
    snprintf(path_, sizeof(path_), "/tmp/fortis_base_log_test.%d.XXXXXX", static_cast<int>(getpid()));
    const int file = mkstemp(path_);
    CHECK(file >= 0);
    saved_ = dup(STDERR_FILENO);
    CHECK(saved_ >= 0 && dup2(file, STDERR_FILENO) == STDERR_FILENO);
    close(file);
  }
  ~StderrFile() {
    dup2(saved_, STDERR_FILENO);
    close(saved_);
    unlink(path_);
  }
  // Everything written so far, NUL-terminated.
  const char* Read() {
    const int file = open(path_, O_RDONLY);
    CHECK(file >= 0);
    size_t length = 0;
    for (;;) {
      const ssize_t count = read(file, text_ + length, sizeof(text_) - 1 - length);
      if (count <= 0) break;
      length += static_cast<size_t>(count);
    }
    close(file);
    text_[length] = '\0';
    length_ = length;
    return text_;
  }
  size_t length() const { return length_; }
  void Truncate() { CHECK(truncate(path_, 0) == 0 && lseek(STDERR_FILENO, 0, SEEK_SET) == 0); }

 private:
  char path_[64];
  int saved_ = -1;
  size_t length_ = 0;
  char text_[256 * 1024]{};
};

StderrFile* g_stderr = nullptr;

void TestLevels() {
  CHECK(fortis::base::kDefaultLogLevel == LogLevel::kInfo);
  CHECK(fortis::base::LogThreshold() == LogLevel::kInfo);
  CHECK(fortis::base::LogEnabled(LogLevel::kError) && fortis::base::LogEnabled(LogLevel::kWarn) &&
         fortis::base::LogEnabled(LogLevel::kInfo) && !fortis::base::LogEnabled(LogLevel::kDebug));

  CHECK(strcmp(fortis::base::LogLevelName(LogLevel::kError), "error") == 0);
  CHECK(strcmp(fortis::base::LogLevelName(LogLevel::kWarn), "warn") == 0);
  CHECK(strcmp(fortis::base::LogLevelName(LogLevel::kInfo), "info") == 0);
  CHECK(strcmp(fortis::base::LogLevelName(LogLevel::kDebug), "debug") == 0);

  const LogLevel levels[] = {LogLevel::kError, LogLevel::kWarn, LogLevel::kInfo, LogLevel::kDebug};
  for (const LogLevel level : levels) {
    const auto parsed = fortis::base::ParseLogLevel(fortis::base::LogLevelName(level));
    CHECK(parsed.ok() && parsed.value() == level);
  }
  const char* const refused[] = {"", "ERROR", "Info", " info", "info ", "warning", "trace", "0", "debugging"};
  for (const char* text : refused) {
    const auto parsed = fortis::base::ParseLogLevel(text);
    CHECK(!parsed.ok() && parsed.status() == fortis::base::StatusCode::kInvalidArgument);
  }
  CHECK(!fortis::base::ParseLogLevel(nullptr).ok());
}

// The stderr text is the message and one newline: no prefix of any kind.
void TestStderrText() {
  g_stderr->Truncate();
  fortis::base::LogError("kv", "KV lock open failed: errno=%d", 13);
  fortis::base::LogWarn("providers", "CoinGecko HTTP 429: paused lane for %u ms (%s)", 60000u, "keyless");
  fortis::base::LogInfo("app", "HTTP admission: workers=%zu pending=%zu health_port=%u", static_cast<size_t>(8),
                  static_cast<size_t>(64), 0u);
  fortis::base::LogDebug("app", "not printed at the default threshold");
  fortis::base::Log(LogLevel::kError, "app", "%s", "plain");
  fortis::base::LogError("app", "%s", "");
  CHECK(strcmp(g_stderr->Read(),
                "KV lock open failed: errno=13\n"
                "CoinGecko HTTP 429: paused lane for 60000 ms (keyless)\n"
                "HTTP admission: workers=8 pending=64 health_port=0\n"
                "plain\n"
                "\n") == 0);
}

void TestThreshold() {
  g_stderr->Truncate();
  fortis::base::SetLogThreshold(LogLevel::kDebug);
  CHECK(fortis::base::LogThreshold() == LogLevel::kDebug && fortis::base::LogEnabled(LogLevel::kDebug));
  fortis::base::LogDebug("app", "debug %d", 1);
  fortis::base::SetLogThreshold(LogLevel::kWarn);
  CHECK(!fortis::base::LogEnabled(LogLevel::kInfo) && fortis::base::LogEnabled(LogLevel::kWarn));
  fortis::base::LogInfo("app", "info hidden");
  fortis::base::LogWarn("app", "warn shown");
  fortis::base::SetLogThreshold(LogLevel::kError);
  fortis::base::LogWarn("app", "warn hidden");
  fortis::base::LogErrno(LogLevel::kWarn, "app", "hidden", ENOENT);
  fortis::base::LogError("app", "error shown");
  fortis::base::SetLogThreshold(fortis::base::kDefaultLogLevel);
  CHECK(strcmp(g_stderr->Read(), "debug 1\nwarn shown\nerror shown\n") == 0);
}

// LogErrno prints what perror(prefix) prints, and logging preserves errno.
void TestErrno() {
  g_stderr->Truncate();
  errno = ENOENT;
  fortis::base::LogErrno(LogLevel::kError, "storage", "storage probe: open temp", errno);
  CHECK(errno == ENOENT);
  errno = EACCES;
  perror("storage probe: open temp");
  errno = EACCES;
  fortis::base::LogErrno(LogLevel::kError, "storage", "storage probe: open temp", errno);
  char expected[512];
  char first[128];
  char second_message[128];
  snprintf(first, sizeof(first), "%s", strerror(ENOENT));
  snprintf(second_message, sizeof(second_message), "%s", strerror(EACCES));
  snprintf(expected, sizeof(expected), "storage probe: open temp: %s\nstorage probe: open temp: %s\n"
           "storage probe: open temp: %s\n", first, second_message, second_message);
  CHECK(strcmp(g_stderr->Read(), expected) == 0);

  // A number the C library has no text for prints what perror prints.
  g_stderr->Truncate();
  const int unknown = 99999;
  errno = unknown;
  perror("accept");
  fortis::base::LogErrno(LogLevel::kError, "app", "accept", unknown);
  const char* both = g_stderr->Read();
  const char* second = strchr(both, '\n');
  CHECK(second && second - both > 8 && strncmp(both, "accept: ", 8) == 0);
  const size_t line_length = static_cast<size_t>(second - both) + 1;
  CHECK(g_stderr->length() == 2 * line_length && memcmp(both, second + 1, line_length) == 0);

  errno = EINTR;
  fortis::base::LogError("app", "errno survives %d", 1);
  CHECK(errno == EINTR);
  errno = 0;
}

void TestStatusLine() {
  g_stderr->Truncate();
  fortis::base::LogStatus("app", "start", fortis::base::Status::Ok());
  fortis::base::LogStatus("app", "Failed to start the native price history",
                    fortis::base::Status(fortis::base::StatusCode::kUnavailable, "backfill owner"));
  CHECK(strcmp(g_stderr->Read(), "Failed to start the native price history: backfill owner\n") == 0);
}

// A line that does not fit is cut at the capacity and still ends in a newline.
void TestTruncation() {
  g_stderr->Truncate();
  char longest[fortis::base::kLogLineCapacity * 2];
  memset(longest, 'x', sizeof(longest) - 1);
  longest[sizeof(longest) - 1] = '\0';
  fortis::base::LogError("app", "%s", longest);
  const char* text = g_stderr->Read();
  CHECK(g_stderr->length() == fortis::base::kLogLineCapacity);
  CHECK(text[fortis::base::kLogLineCapacity - 1] == '\n');
  for (size_t index = 0; index + 1 < fortis::base::kLogLineCapacity; ++index) CHECK(text[index] == 'x');

  // The longest line that fits is not cut.
  g_stderr->Truncate();
  longest[fortis::base::kLogLineCapacity - 1] = '\0';
  fortis::base::LogError("app", "%s", longest);
  g_stderr->Read();
  CHECK(g_stderr->length() == fortis::base::kLogLineCapacity);
  g_stderr->Truncate();
  longest[fortis::base::kLogLineCapacity - 2] = '\0';
  fortis::base::LogError("app", "%s", longest);
  g_stderr->Read();
  CHECK(g_stderr->length() == fortis::base::kLogLineCapacity - 1);
}

void TestSink() {
  g_stderr->Truncate();
  // A consuming sink replaces stderr and sees level, component and text.
  CaptureSink capture(true);
  CHECK(fortis::base::SetLogSink(&capture) == nullptr);
  fortis::base::LogError("kv", "KV write failed: stage=%s errno=%d", "fsync", 5);
  fortis::base::LogWarn("auth", "refused");
  fortis::base::LogInfo(nullptr, "no component");
  fortis::base::LogDebug("kv", "below the threshold: never reaches the sink");
  fortis::base::LogErrno(LogLevel::kError, "app", "accept", EMFILE);
  CHECK(capture.count() == 4);
  CHECK(capture.line(0).level == LogLevel::kError && strcmp(capture.line(0).component, "kv") == 0 &&
         strcmp(capture.line(0).text, "KV write failed: stage=fsync errno=5") == 0 &&
         capture.line(0).length == strlen("KV write failed: stage=fsync errno=5") && capture.line(0).terminated);
  CHECK(capture.line(1).level == LogLevel::kWarn && strcmp(capture.line(1).component, "auth") == 0 &&
         strcmp(capture.line(1).text, "refused") == 0);
  CHECK(capture.line(2).level == LogLevel::kInfo && strcmp(capture.line(2).component, "") == 0);
  char accept_line[160];
  snprintf(accept_line, sizeof(accept_line), "accept: %s", strerror(EMFILE));
  CHECK(strcmp(capture.line(3).text, accept_line) == 0);
  CHECK(g_stderr->Read()[0] == '\0');

  // An observing sink sees the record and the line still goes to stderr.
  CaptureSink observe(false);
  CHECK(fortis::base::SetLogSink(&observe) == &capture);
  fortis::base::LogError("app", "both %d", 2);
  CHECK(observe.count() == 1 && strcmp(observe.line(0).text, "both 2") == 0);
  CHECK(strcmp(g_stderr->Read(), "both 2\n") == 0);

  // Removing the sink restores stderr only.
  CHECK(fortis::base::SetLogSink(nullptr) == &observe);
  fortis::base::LogError("app", "alone");
  CHECK(observe.count() == 1 && capture.count() == 4);
  CHECK(strcmp(g_stderr->Read(), "both 2\nalone\n") == 0);
}

constexpr int kThreads = 8;
constexpr int kLinesPerThread = 150;

void TestThrowingSinkFallback() {
  static_assert(noexcept(fortis::base::LogError("test", "bounded error")));
  struct Throwing final : LogSink {
    int kind = 0;
    int calls = 0;
    bool Write(const LogRecord&) override {
      ++calls;
      if (kind == 0) throw std::bad_alloc();
      if (kind == 1) throw std::runtime_error("fintel_dev_private");
      throw 7;
    }
  } sink;
  auto* previous = fortis::base::SetLogSink(&sink);
  for (int kind = 0; kind < 3; ++kind) {
    sink.kind = kind;
    g_stderr->Truncate();
    errno = ENOMEM;
    fortis::base::LogError("test", "safe error %d", kind);
    CHECK(errno == ENOMEM && sink.calls == kind + 1);
    const char* text = g_stderr->Read();
    CHECK(strstr(text, "logging failed: sink exception") && strstr(text, "safe error"));
    CHECK(!strstr(text, "fintel_dev_private"));
  }
  fortis::base::SetLogSink(previous);
}

void* Hammer(void* opaque) {
  const int thread = *static_cast<const int*>(opaque);
  // 200 bytes of one letter per line: an interleaved line would mix letters.
  char payload[201];
  memset(payload, 'a' + thread, sizeof(payload) - 1);
  payload[sizeof(payload) - 1] = '\0';
  for (int line = 0; line < kLinesPerThread; ++line) fortis::base::LogInfo("test", "%d %s", thread, payload);
  return nullptr;
}

// Lines from concurrent threads arrive whole: one write() each.
void TestNoInterleaving() {
  g_stderr->Truncate();
  pthread_t threads[kThreads];
  int thread_indices[kThreads]{};
  for (int index = 0; index < kThreads; ++index) {
    thread_indices[index] = index;
    CHECK(pthread_create(&threads[index], nullptr, Hammer, &thread_indices[index]) == 0);
  }
  for (int index = 0; index < kThreads; ++index) pthread_join(threads[index], nullptr);
  const char* text = g_stderr->Read();
  int seen[kThreads] = {};
  size_t lines = 0;
  while (*text) {
    const char* end = strchr(text, '\n');
    CHECK(end && end - text == 202);
    const int thread = text[0] - '0';
    CHECK(thread >= 0 && thread < kThreads && text[1] == ' ');
    for (const char* cursor = text + 2; cursor < end; ++cursor) CHECK(*cursor == 'a' + thread);
    ++seen[thread];
    ++lines;
    text = end + 1;
  }
  CHECK(lines == static_cast<size_t>(kThreads) * kLinesPerThread);
  for (int index = 0; index < kThreads; ++index) CHECK(seen[index] == kLinesPerThread);
}

}  // namespace

int main() {
  static StderrFile redirected;
  g_stderr = &redirected;
  TestLevels();
  TestStderrText();
  TestThreshold();
  TestErrno();
  TestStatusLine();
  TestTruncation();
  TestSink();
  TestThrowingSinkFallback();
  TestNoInterleaving();
  puts("base log passed");
  return 0;
}
