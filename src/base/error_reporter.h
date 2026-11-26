#pragma once

#include <string_view>
#include <cstdint>

namespace fortis::base {

/** Observed origin, not a claim about the ultimate root cause. */
enum class ErrorOrigin : std::uint8_t { kNode, kThirdParty, kUnclassified };

/** Borrowed public metadata copied synchronously by the sink.
 * provider is a stable configured provider label, never a host, URL or user ID.
 * http_status is an observed upstream status (100..599), or zero if unavailable.
 */
struct ErrorDetails {
  ErrorOrigin origin = ErrorOrigin::kNode;
  std::string_view provider{};
  std::uint16_t http_status = 0;
};

/** Return the public spelling; invalid values have the unclassified spelling. */
inline constexpr const char* ErrorOriginName(ErrorOrigin origin) {
  switch (origin) {
    case ErrorOrigin::kNode: return "node";
    case ErrorOrigin::kThirdParty: return "third_party";
    case ErrorOrigin::kUnclassified: return "unclassified";
  }
  return "unclassified";
}

/** Borrowed sink for bounded, public operational diagnostics.
 * Report must not perform network/storage I/O or throw. Callers supply stable
 * classifications and public market identifiers, never upstream text, tokens,
 * wallet contents or AI messages. The sink copies fields before returning.
 */
class ErrorReporter {
 public:
  virtual void Report(std::string_view service, std::string_view code, std::string_view identifier = {},
                      ErrorDetails details = {}) noexcept = 0;

 protected:
  ~ErrorReporter() = default;
};

}  // namespace fortis::base
