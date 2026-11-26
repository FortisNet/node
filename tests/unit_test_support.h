#pragma once

#include "kv_store.h"
#include "token_json.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <exception>
#include <stdexcept>

namespace fortis::testing {

inline void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Each case owns its directory; destruction closes KV before removing files.
class TemporaryDirectory {
 public:
  TemporaryDirectory() { Require(mkdtemp(path_) != nullptr, "create temporary directory"); }
  ~TemporaryDirectory() { Remove(path_); }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  const char* path() const { return path_; }

 private:
  static void Remove(const char* path) {
    DIR* directory = opendir(path);
    if (!directory) return;
    while (dirent* entry = readdir(directory)) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      char child[4096];
      const int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
      if (length < 0 || static_cast<size_t>(length) >= sizeof(child)) continue;
      struct stat info {};
      if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) Remove(child);
      else (void)unlink(child);
    }
    closedir(directory);
    (void)rmdir(path);
  }
  char path_[64] = "/tmp/fortis-class-unit-XXXXXX";
};

struct TemporaryKv {
  TemporaryDirectory directory;
  storage::KvStore kv;
  TemporaryKv() { Require(kv.Open(directory.path()), "open temporary KV"); }
};

class JsonArena {
 public:
  JsonArena() : value_(codec::tj::GrowingArena(8 * 1024 * 1024)) {}
  ~JsonArena() { codec::tj::FreeArena(value_); }
  JsonArena(const JsonArena&) = delete;
  JsonArena& operator=(const JsonArena&) = delete;
  codec::tj::Arena& get() { return value_; }

 private:
  codec::tj::Arena value_;
};

class UnitSuite {
 public:
  template <typename Test>
  void Run(const char* name, Test test) noexcept {
    ++cases_;
    try {
      test();
    } catch (const std::exception& error) {
      ++failures_;
      fprintf(stderr, "FAIL %s: %s\n", name, error.what());
    } catch (...) {
      ++failures_;
      fprintf(stderr, "FAIL %s: unknown exception\n", name);
    }
  }
  int Finish(const char* name) const {
    printf("%s: %u cases, %u failures\n", name, cases_, failures_);
    return failures_ ? 1 : 0;
  }

 private:
  unsigned cases_ = 0;
  unsigned failures_ = 0;
};

}  // namespace fortis::testing
