#include "release_routes.h"
#include "bounded_json.h"
#include "file_descriptor.h"
#include "query_string.h"
#include "threading.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace fortis::operations {
namespace {
constexpr size_t kCapacity = 64 * 1024;
constexpr size_t kHostPageCapacity = 512 * 1024;
constexpr int64_t kHealthFreshMs = 60 * 1000;
base::Mutex release_mutex;
std::array<char, 1024> release_directory{};
std::array<char, kCapacity> snapshot{};
std::array<codec::BoundedJsonToken, 8192> release_tokens{};
size_t snapshot_length = 0;
int snapshot_status = 503;
uint64_t cached_page = UINT64_MAX;
bool cached_history = false;
int64_t cached_second = -1;
const char kReleasePage[] =
#include "release_page.inc"
    ;

void Reply(int client, bool head, int status, const char* type, const char* data, size_t length) {
  http::HttpResponse response{status,
                              status == 200   ? "OK"
                              : status == 400 ? "Bad Request"
                              : status == 404 ? "Not Found"
                                              : "Service Unavailable",
                              type, reinterpret_cast<const uint8_t*>(data), length};
  response.cache_control = "no-store";
  WriteHttpResponse(client, response, head);
}
void HandlePage(int client, const http::HttpRequest&, bool head, void*) {
  Reply(client, head, 200, "text/html; charset=utf-8", kReleasePage, sizeof(kReleasePage) - 1);
}
void HandleSnapshot(int client, const http::HttpRequest& request, bool head, void*) {
  const bool history = std::strcmp(request.path, "/releasesz/history") == 0;
  uint64_t page = 0;
  const auto query = http::query_string::Query::FromTarget(request.raw_target ? request.raw_target : request.path);
  http::query_string::Component value;
  const bool has_page = query.First("page", &value);
  if (!query.Valid() || (has_page && (!history || !value.Unsigned(360287970189639ULL, &page))) ||
      (history && !has_page)) {
    constexpr char error[] = "{\"error\":\"invalid_query\"}";
    Reply(client, head, 400, "application/json; charset=utf-8", error, sizeof(error) - 1);
    return;
  }
  base::LockGuard guard(release_mutex);
  timespec now{};
  const bool clock_ok = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
  const int64_t second = clock_ok ? static_cast<int64_t>(now.tv_sec) : -1;
  if (second < 0 || second != cached_second || history != cached_history || page != cached_page) {
    snapshot_length = 0;
    snapshot_status = 503;
    char path[1200];
    const int length = history ? std::snprintf(path, sizeof(path), "%s/history-%llu.json", release_directory.data(),
                                               static_cast<unsigned long long>(page))
                               : std::snprintf(path, sizeof(path), "%s/current.json", release_directory.data());
    if (length > 0 && static_cast<size_t>(length) < sizeof(path)) {
      base::FileDescriptor file(::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
      if (file.get() < 0 && errno == ENOENT) snapshot_status = history ? 404 : 200;
      if (file.get() >= 0) {
        struct stat info {};
        bool ok = fstat(file.get(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
                  static_cast<uint64_t>(info.st_size) < snapshot.size();
        while (ok && snapshot_length < snapshot.size()) {
          const ssize_t count =
              ::read(file.get(), snapshot.data() + snapshot_length, snapshot.size() - snapshot_length);
          if (count < 0 && errno == EINTR) continue;
          if (count < 0) {
            ok = false;
            break;
          }
          if (!count) break;
          snapshot_length += static_cast<size_t>(count);
        }
        codec::BoundedJsonDocument document;
        codec::BoundedJsonError error{};
        ok = ok && snapshot_length < snapshot.size() &&
             document.Parse(reinterpret_cast<const unsigned char*>(snapshot.data()), snapshot_length,
                            release_tokens.data(), release_tokens.size(), &error);
        double schema = 0;
        ok = ok && document.Number(document.FindObjectValue(document.root(), "schemaVersion"), &schema) && schema == 1;
        if (ok) snapshot_status = 200;
        else snapshot_length = 0;
      }
    }
    cached_second = second;
    cached_history = history;
    cached_page = page;
  }
  if (snapshot_status == 200 && !snapshot_length) {
    constexpr char absent[] =
        "{\"schemaVersion\":1,\"managed\":false,\"current\":null,\"candidate\":null,\"latestPage\":null,"
        "\"observedAtMs\":null}";
    Reply(client, head, 200, "application/json; charset=utf-8", absent, sizeof(absent) - 1);
  } else if (snapshot_status == 200)
    Reply(client, head, 200, "application/json; charset=utf-8", snapshot.data(), snapshot_length);
  else {
    constexpr char missing[] = "{\"error\":\"history_not_found\"}";
    constexpr char failed[] = "{\"error\":\"release_metadata_unavailable\"}";
    Reply(client, head, snapshot_status, "application/json; charset=utf-8", snapshot_status == 404 ? missing : failed,
          snapshot_status == 404 ? sizeof(missing) - 1 : sizeof(failed) - 1);
  }
}
/** One regular file of the status directory, at most `limit` bytes. The
 * directory is fixed at registration, before any worker runs. */
bool ReadHostFile(const char* name, std::string* out, size_t limit) {
  char path[1200];
  const int length = std::snprintf(path, sizeof(path), "%s/%s", release_directory.data(), name);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) return false;
  base::FileDescriptor file(::open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat info {};
  if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<uint64_t>(info.st_size) > limit)
    return false;
  out->assign(static_cast<size_t>(info.st_size), '\0');
  size_t have = 0;
  while (have < out->size()) {
    const ssize_t count = ::read(file.get(), out->data() + have, out->size() - have);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    have += static_cast<size_t>(count);
  }
  return true;
}
void HandleHealth(int client, const http::HttpRequest& request, bool head, void*) {
  const bool json = std::strcmp(request.path, "/health.json") == 0;
  std::string body;
  if (!CopyHostPage(json ? "health.json" : "health.html", &body)) {
    // No orchestrator writes a report on this host: say so, as a failure,
    // without guessing at health this node cannot judge.
    constexpr char absent[] = "{\"error\":\"health_report_unavailable\"}";
    Reply(client, head, 503, "application/json; charset=utf-8", absent, sizeof(absent) - 1);
    return;
  }
  Reply(client, head, HostHealthStatus(), json ? "application/json; charset=utf-8" : "text/html; charset=utf-8",
        body.data(), body.size());
}
}  // namespace
bool CopyHostPage(const char* name, std::string* out) {
  return name && out && release_directory[0] == '/' && ReadHostFile(name, out, kHostPageCapacity);
}
int HostHealthStatus() {
  std::string line;
  if (release_directory[0] != '/' || !ReadHostFile("health.status", &line, 64) || line.back() != '\n') return 503;
  const auto space = line.find(' ');
  if (space == std::string::npos) return 503;
  int status = 0;
  int64_t written = 0;
  const char* begin = line.data();
  const auto first = std::from_chars(begin, begin + space, status);
  const auto second = std::from_chars(begin + space + 1, begin + line.size() - 1, written);
  if (first.ec != std::errc{} || first.ptr != begin + space || second.ec != std::errc{} ||
      second.ptr != begin + line.size() - 1)
    return 503;
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 503;
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
  return status == 200 && now_ms >= written && now_ms - written <= kHealthFreshMs ? 200 : 503;
}
bool RegisterReleaseRoutes(http::HttpRouter& router, std::string_view directory) {
  if (directory.empty() || directory.front() != '/' || directory.find('\0') != std::string_view::npos ||
      directory.size() >= release_directory.size())
    return false;
  std::memcpy(release_directory.data(), directory.data(), directory.size());
  release_directory[directory.size()] = '\0';
  cached_second = -1;
  return router.Add("GET", "/releases", HandlePage) && router.Add("GET", "/releasesz", HandleSnapshot) &&
         router.Add("GET", "/releasesz/history", HandleSnapshot) && router.Add("GET", "/health", HandleHealth) &&
         router.Add("GET", "/health.json", HandleHealth);
}
}  // namespace fortis::operations
