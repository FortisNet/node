#pragma once

#include <memory>
#include <stdlib.h>
#include <type_traits>
#include <utility>

namespace fortis::base {

// A non-owning, synchronous callable view. Never store or return a borrowed
// view: its callable must remain alive until the consuming call returns.
// Temporaries are safe as arguments for that full expression only. Function
// pointers are held by value, so pointer variables need not outlive the view.
template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
 public:
  FunctionRef(std::nullptr_t = nullptr) noexcept {}
  FunctionRef(R (*function)(Args...)) noexcept : invoke_(function ? InvokeFunction : nullptr) {
    target_.function = function;
  }
  template <typename F, typename = std::enable_if_t<
      !std::is_same_v<std::decay_t<F>, FunctionRef> &&
      !std::is_pointer_v<std::decay_t<F>> &&
      std::is_invocable_r_v<R, F&, Args...>>>
  FunctionRef(F&& callable) noexcept : invoke_(InvokeObject<std::remove_reference_t<F>>) {
    target_.object = const_cast<void*>(static_cast<const void*>(std::addressof(callable)));
  }
  FunctionRef(const FunctionRef&) noexcept = default;
  FunctionRef& operator=(const FunctionRef&) = delete;
  explicit operator bool() const noexcept { return invoke_ != nullptr; }
  R operator()(Args... args) const {
    if (!invoke_) abort();
    return invoke_(target_, std::forward<Args>(args)...);
  }

 private:
  union Target {
    void* object = nullptr;
    R (*function)(Args...);
  };
  static R InvokeFunction(Target target, Args... args) {
    return target.function(std::forward<Args>(args)...);
  }
  template <typename F>
  static R InvokeObject(Target target, Args... args) {
    if constexpr (std::is_void_v<R>) {
      (*static_cast<F*>(target.object))(std::forward<Args>(args)...);
    } else {
      return (*static_cast<F*>(target.object))(std::forward<Args>(args)...);
    }
  }
  Target target_;
  R (*invoke_)(Target, Args...) = nullptr;
};

}  // namespace fortis::base
