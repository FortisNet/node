#pragma once

#include <memory>
#include "ownership.h"
#include "status.h"

#include <type_traits>

namespace fortis::base {

// Keep an existing outer object allocation before running a value factory.
// Storage is released on every factory failure/throw; the move cannot throw.
// Useful when a parent retains its historical separately allocated pimpl handle.
template <typename T, typename Factory>
Result<std::unique_ptr<T, fortis::base::ObjectDeleter<T>>> MakeOwned(Factory factory) {
  static_assert(alignof(T) <= alignof(max_align_t), "byte storage alignment");
  static_assert(std::is_nothrow_move_constructible_v<T>, "factory handle move must not throw");
  auto storage = AllocateStorage<T>(sizeof(T));
  if (!storage) return Status(StatusCode::kResourceExhausted, "factory handle allocation");
  auto made = factory();
  if (!made) return made.status();
  T* object = new (storage.get()) T(made.TakeValue());
  (void)storage.release();
  return std::unique_ptr<T, fortis::base::ObjectDeleter<T>>(object);
}

} // namespace fortis::base
