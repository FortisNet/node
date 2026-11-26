#include "native_config.h"
#include "function_ref.h"
#include "http_server.h"
#include "token_service_internal.h"
#include "address_service_internal.h"
#include "device_auth.h"
#include "kv_runtime.h"
#include "unit_test_support.h"

#include <sys/socket.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace base = fortis::base;
namespace storage = fortis::storage;
namespace http = fortis::http;
using fortis::testing::Require;
int plus_one(int value) { return value + 1; }
base::FunctionRef<int(int)> pointer_value() {
  auto pointer = &plus_one;
  return base::FunctionRef<int(int)>(pointer);
}
void callable_contract() {
  using View = base::FunctionRef<int(int)>;
  static_assert(sizeof(View) == 2 * sizeof(void*), "two-word non-owning view");
  static_assert(!std::is_copy_assignable_v<View>, "borrowed views are not assignable");
  static_assert(std::is_trivially_destructible_v<View>, "no owned resource");
  View empty;
  Require(!empty, "empty callback is representable");
  Require(pointer_value()(3) == 4, "function pointer is stored by value");
  int sum = 0;
  auto mutable_call = [&](int value) { return sum += value; };
  View view(mutable_call);
  const View copy(view);
  Require(copy(4) == 4 && view(3) == 7, "copies borrow the same mutable callable");
  const auto constant = [](int value) { return value * 2; };
  Require(View(constant)(5) == 10, "const callable retains constness");
  auto move_only = [owned = std::make_unique<int>(11)](int value) { return *owned + value; };
  Require(View(move_only)(2) == 13, "callable is never copied or moved");
  auto consume = [](View callback) { return callback(8); };
  Require(consume([&sum](int value) { return sum + value; }) == 15, "temporary survives synchronous full expression");
  base::FunctionRef<void(int)> discard(mutable_call);
  discard(1);
  Require(sum == 8, "void view discards compatible return values");
  auto reference = [&]() -> int& { return sum; };
  base::FunctionRef<int&()> reference_view(reference);
  reference_view() = 19;
  Require(sum == 19, "reference return is preserved");
}
void scan_contract() {
  fortis::testing::TemporaryKv fixture;
  const storage::KvMutation mutations[] = {{"s/a", "a", 1, 1010, false}, {"s/b", "b", 1, 1000, false}, {"s/c", "c", 1, 0, false}};
  Require(fixture.kv.Write(mutations, 3), "seed live and expired records");
  std::vector<std::string> keys;
  Require(fixture.kv.Scan("s/", nullptr, 5, 1000, [&](const char* key, const void*, size_t) { keys.emplace_back(key); return true; }), "capture scan");
  Require(keys == std::vector<std::string>({"s/a", "s/c"}), "scan order and expiry unchanged");
  keys.clear();
  Require(!fixture.kv.Scan("s/", nullptr, 5, 1000, [&](const char* key, const void*, size_t) { keys.emplace_back(key); return false; }, true), "visitor can stop reverse scan");
  Require(keys == std::vector<std::string>({"s/c"}), "early stop visits once");
  storage::KvPageResult page;
  int visited = 0;
  Require(fixture.kv.Page("s/", nullptr, 2, 1000, false, nullptr, &page, [&](const char* key, const void* value, size_t size, int64_t expiry) {
        Require(!strcmp(key, "s/a") && size == 1 && *static_cast<const char*>(value) == 'a' && expiry == 1010, "record visitor gets expiry and bytes");
        ++visited; return true;
      }), "record-only page");
  Require(visited == 1 && page.scanned == 2 && !page.done && !strcmp(page.next_cursor, "s/b"), "physical page limit/cursor includes expired records");
  bool caught = false;
  try {
    fixture.kv.Scan("s/", nullptr, 5, 1000, [](const char*, const void*, size_t) -> bool { throw std::runtime_error("visitor failure"); });
  } catch (const std::runtime_error&) { caught = true; }
  Require(caught && fixture.kv.Scan("s/", nullptr, 1, 1000, [](const char*, const void*, size_t) { return true; }), "visitor exception propagates and releases scan lock");
  Require(!fixture.kv.Scan("s/", nullptr, 1, 1000, nullptr), "empty required visitor remains invalid");
}

struct Pair {
  int fds[2]{-1, -1};
  Pair() { Require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socket pair"); }
  ~Pair() { for (int fd : fds) if (fd >= 0) close(fd); }
  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;
};
std::string request(http::HttpRouter& router, const char* path, const char* authorization = nullptr, bool head = false) {
  Pair pair;
  timeval timeout{2, 0};
  Require(setsockopt(pair.fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "receive deadline");
  const std::string wire = std::string(head ? "HEAD " : "GET ") + path + " HTTP/1.1\r\nHost: unit\r\n" +
      (authorization ? std::string("Authorization: ") + authorization + "\r\n" : "") + "\r\n";
  Require(write(pair.fds[1], wire.data(), wire.size()) == static_cast<ssize_t>(wire.size()), "request bytes");
  Require(!router.Dispatch(pair.fds[0]), "local response retains close ownership");
  close(pair.fds[0]); pair.fds[0] = -1;
  std::string response;
  char bytes[2048];
  for (;;) {
    const ssize_t n = read(pair.fds[1], bytes, sizeof(bytes));
    if (n < 0 && errno == EINTR) continue;
    Require(n >= 0, "response completes");
    if (!n) break;
    response.append(bytes, static_cast<size_t>(n));
  }
  return response;
}
struct Endpoint {
  int status;
  Endpoint* operator&() { return nullptr; }
  void Reply(const http::HttpRequest&, http::HttpReply& reply) const {
    reply.Send(http::JsonResponse(status, "OK", "{}"));
  }
  void Raw(int client, const http::HttpRequest&, bool head) {
    http::WriteHttpResponse(client, http::JsonResponse(status, "OK", "{}"), head);
  }
};
void typed_binding() {
  http::HttpRouter first, second;
  const Endpoint immutable{201};
  Endpoint other{202};
  Require(first.Add<&Endpoint::Reply>("GET", "/reply", immutable), "bind const target");
  Require(second.Add<&Endpoint::Raw>("GET", "/reply", other), "bind legacy member target with overloaded address");
  Require(request(first, "/reply").find("HTTP/1.1 201 ") == 0, "first target stays bound");
  const auto head = request(second, "/reply", nullptr, true);
  Require(head.find("HTTP/1.1 202 ") == 0 && head.substr(head.find("\r\n\r\n") + 4).empty(), "raw binding preserves HEAD");
  other.status = 203;
  Require(request(second, "/reply").find("HTTP/1.1 203 ") == 0 && request(first, "/reply").find("HTTP/1.1 201 ") == 0, "independent instance state");
}
void real_service_instances() {
  fortis::testing::TemporaryDirectory directory;
  struct CloseRuntime { ~CloseRuntime() { if (auto* kv = storage::RuntimeKvStoreIfOpen()) kv->Close(); } } close_runtime;
  storage::RuntimeKvSetDirectory(directory.path());
  auto* kv = storage::RuntimeKvStore();
  Require(kv != nullptr, "runtime auth store");
  timespec now{};
  Require(clock_gettime(CLOCK_REALTIME, &now) == 0, "wall time for device credentials");
  fortis::auth::DeviceCredentials device;
  fortis::auth::DeviceAuthStore auth(kv);
  Require(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Ios, "unit", 4,
      static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000, 86400000, &device), "enroll development device");
  const std::string bearer = std::string("Bearer ") + device.token;
  fortis::config::NativeConfig allowed, denied;
  allowed.attestation_required = false;
  denied.attestation_required = true;
  // No workers or outbound transports start: accepted requests stop at the
  // deliberately absent handoff, after proving which instance authorized them.
  auto token_a = std::make_unique<fortis::tokens::TokenService::Impl>(allowed, kv, nullptr, nullptr, nullptr, *kv, nullptr, nullptr, nullptr);
  auto token_b = std::make_unique<fortis::tokens::TokenService::Impl>(denied, kv, nullptr, nullptr, nullptr, *kv, nullptr, nullptr, nullptr);
  auto wallet_a = std::make_unique<fortis::wallet::AddressService::Impl>(allowed, kv, nullptr, nullptr, nullptr, nullptr);
  auto wallet_b = std::make_unique<fortis::wallet::AddressService::Impl>(denied, kv, nullptr, nullptr, nullptr, nullptr);
  http::HttpRouter ta, tb, wa, wb;
  constexpr char token_path[] = "/v1/networks/eth-mainnet/token/native/page";
  constexpr char wallet_path[] = "/v1/networks/eth-mainnet/address/unit/nfts";
  Require(ta.Add<&fortis::tokens::TokenRoutes::HandlePage>("GET", "/v1/networks/:network/token/:tokenId/page", token_a->routes), "bind token A");
  Require(tb.Add<&fortis::tokens::TokenRoutes::HandlePage>("GET", "/v1/networks/:network/token/:tokenId/page", token_b->routes), "bind token B");
  Require(wa.Add<&fortis::wallet::AddressService::Impl::HandleNfts>("GET", "/v1/networks/:network/address/:address/nfts", *wallet_a), "bind wallet A");
  Require(wb.Add<&fortis::wallet::AddressService::Impl::HandleNfts>("GET", "/v1/networks/:network/address/:address/nfts", *wallet_b), "bind wallet B");
  Require(request(tb, token_path, bearer.c_str()).find("ATTESTATION_REQUIRED") != std::string::npos &&
              request(wb, wallet_path, bearer.c_str()).find("ATTESTATION_REQUIRED") != std::string::npos, "second instances enforce their policy");
  Require(request(ta, token_path, bearer.c_str()).find("HTTP/1.1 429 ") == 0 &&
              request(wa, wallet_path, bearer.c_str()).find("HTTP/1.1 429 ") == 0, "first instances retain development policy after second binding");
  token_b.reset(); wallet_b.reset();
  Require(request(ta, token_path, bearer.c_str()).find("HTTP/1.1 429 ") == 0 &&
              request(wa, wallet_path, bearer.c_str()).find("HTTP/1.1 429 ") == 0, "destroying second instances cannot clear first bindings");
}
}  // namespace
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("borrowed callable lifetime/types", callable_contract);
  suite.Run("KV scan/page/early stop/unwind", scan_contract);
  suite.Run("typed const/raw/HEAD binding", typed_binding);
  suite.Run("real token/wallet instance isolation", real_service_instances);
  return suite.Finish("callable_binding_test");
}
