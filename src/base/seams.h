#pragma once

#include <stddef.h>
#include <stdint.h>

// Typed dependency seams shared by several modules (the clock is clock.h).
// Each replaces a `fn(void* context, ...)` pointer and its context with one
// object the compiler checks. As with Clock, no method is pure and no
// destructor is virtual: the default body is the answer of "no implementation"
// (documented per method), and objects are never deleted through an interface.
namespace fortis::base {

// A uniform sample for a jittered delay or TTL.
class JitterSource {
 public:
  // Writes a sample in [0, 1) and returns true; false means "no sample" and
  // the caller falls back to its own default (the base body).
  virtual bool Sample(double* sample) { (void)sample; return false; }

 protected:
  constexpr JitterSource() = default;
  ~JitterSource() = default;
};

// Random bytes (tickets, nonces). Production code that has no source injected
// uses the CSPRNG (crypto/secure_random.h) directly.
class RandomSource {
 public:
  // Fills `output[0, length)`; false when no bytes are available (the base).
  virtual bool Bytes(uint8_t* output, size_t length) { (void)output; (void)length; return false; }

 protected:
  constexpr RandomSource() = default;
  ~RandomSource() = default;
};

// 26-character identifiers (ULIDs) written NUL-terminated.
class IdSource {
 public:
  static constexpr size_t kIdBytes = 27;
  // Writes a new identifier; false when none can be made (the base).
  virtual bool NewId(char output[kIdBytes]) { (void)output; return false; }

 protected:
  constexpr IdSource() = default;
  ~IdSource() = default;
};

// Starts a dependency a service owns (a transport, a provider owner) in place
// of its normal Start(): a replay binary points it at a local fixture host.
// A service given none calls the dependency's own Start().
template <typename T>
class Starter {
 public:
  // True when `target` is started; the base starts nothing and fails.
  virtual bool Start(T& target) { (void)target; return false; }

 protected:
  constexpr Starter() = default;
  ~Starter() = default;
};

}  // namespace fortis::base
