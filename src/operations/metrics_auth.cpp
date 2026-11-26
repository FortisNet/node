#include "metrics_auth.h"

#include <string.h>

namespace fortis::operations {

bool MatchesMetricsBearer(const char* authorization, const char* token) {
  if (!authorization || !token || strncmp(authorization, "Bearer ", 7) != 0) return false;
  const size_t token_length = strnlen(token, kMetricsTokenMaxBytes + 1);
  if (token_length == 0 || token_length > kMetricsTokenMaxBytes) return false;
  const char* provided = authorization + 7;
  // One byte past the longest token is enough to prove a length mismatch.
  const size_t provided_length = strnlen(provided, kMetricsTokenMaxBytes + 1);
  unsigned difference = provided_length == token_length ? 0U : 1U;
  for (size_t i = 0; i < kMetricsTokenMaxBytes; ++i) {
    const unsigned char actual = i < provided_length
        ? static_cast<unsigned char>(provided[i]) : 0;
    const unsigned char expected = i < token_length
        ? static_cast<unsigned char>(token[i]) : 0;
    difference |= actual ^ expected;
  }
  return difference == 0;
}

}  // namespace fortis::operations
