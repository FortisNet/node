#pragma once

#include "log.h"
#include "status.h"

#include <optional>
#include <utility>

namespace fortis::app::detail {

/** Lazily construct a service only when its slot is empty and dependencies ready.
 * Failure preserves an empty slot. A throwing factory propagates to the caller's
 * exception boundary; successfully constructed services are owned by the slot.
 */
template <typename Service, typename Factory>
base::Status EmplaceService(std::optional<Service>& service, Factory&& create, bool ready = true) {
  if (service || !ready) return base::Status(base::StatusCode::kFailedPrecondition, "composition");
  auto created = std::forward<Factory>(create)();
  if (!created) return created.status();
  service.emplace(created.TakeValue());
  return base::Status::Ok();
}

template <typename Service, typename Factory>
bool StartService(std::optional<Service>& service, const char* failure, Factory&& create, bool ready = true) {
  const auto status = EmplaceService(service, std::forward<Factory>(create), ready);
  if (!status.ok()) base::LogError("app", "%s", failure);
  return status.ok();
}

/** Register present services in order, stopping at the first failed route set.
 * Router, handoff and services are borrowed. Registration is startup-only;
 * previously registered routes remain installed when a later service fails.
 */
template <typename Router, typename Handoff, typename... Services>
bool RegisterServices(Router& router, Handoff& handoff, std::optional<Services>&... services) {
  return ((!services || services->Register(router, handoff)) && ...);
}

}  // namespace fortis::app::detail
