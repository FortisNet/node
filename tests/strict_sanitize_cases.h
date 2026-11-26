#pragma once
#include <string.h>
// Generated independent Unicode-boundary expectation deltas.
namespace strict {
inline bool SanitizeCase(const char* input, const char* kind, const char** json, const char** hex) {
  struct Row { const char* input; const char* kind; const char* json; const char* hex; };
  static const Row rows[] = {
    {"f09f9880f09f9880f09f9880f09f9880f09f9880f09f9880f09f9880f09f9880f09f9880", "sanitizeSymbol", "\"😀😀😀😀😀😀😀…\"", "f09f9880f09f9880f09f9880f09f9880f09f9880f09f9880f09f9880e280a6"},
    {"6162636465666768696a6b6c6d6ef09f988078", "sanitizeSymbol", "\"abcdefghijklmn…\"", "6162636465666768696a6b6c6d6ee280a6"},
    {"7878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878f09f98807a", "sanitizeName", "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…\"", "7878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878787878e280a6"},
  };
  for (const auto& row : rows) if (!strcmp(input, row.input) && !strcmp(kind, row.kind)) {
    *json = row.json; *hex = row.hex; return true;
  }
  return false;
}
}
