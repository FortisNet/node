#include "shutdown_signal.h"

#include <signal.h>
#include <atomic>

namespace fortis::app {
namespace {

std::atomic<bool> shutdown_requested{false};
std::atomic<bool> certificate_reload{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void request_reload(int) noexcept { certificate_reload.store(true, std::memory_order_relaxed); }

void request_shutdown(int) noexcept { shutdown_requested.store(true, std::memory_order_relaxed); }

}  // namespace

void InstallShutdownSignals() {
  signal(SIGPIPE, SIG_IGN);
  struct sigaction shutdown_action{};
  shutdown_action.sa_handler = request_shutdown;
  sigemptyset(&shutdown_action.sa_mask);
  sigaction(SIGINT, &shutdown_action, nullptr);
  sigaction(SIGTERM, &shutdown_action, nullptr);
  shutdown_action.sa_handler = request_reload;
  sigaction(SIGHUP, &shutdown_action, nullptr);
}

bool TakeCertificateReload() {
  return certificate_reload.exchange(false, std::memory_order_relaxed);
}

bool ShutdownRequested() { return shutdown_requested.load(std::memory_order_relaxed); }

void MarkShutdown() { shutdown_requested.store(true, std::memory_order_relaxed); }

}  // namespace fortis::app
