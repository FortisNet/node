// Reads "<U|L> <hex UTF-8>" lines and writes the hex UTF-8 result of
// JsToUpperCase/JsToLowerCase per line ("!" when the input is refused).
// Driven by tests/js_case_test.mjs, which compares against Node.js.
// First it checks the identifier mapping (AsciiToUpperCase/AsciiToLowerCase,
// js-case) and exits 1 on a failure.
#include "js_case.h"

#include <stdio.h>
#include <string.h>

namespace {
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
}  // namespace

// Only A-Z / a-z change; non-ASCII is copied (no ß -> SS, no Kelvin sign ->
// k, no İ -> i̇); invalid UTF-8 and a short output are refused.
bool ascii_checks() {
  struct Case { bool upper; const char* in; const char* out; } cases[] = {
      {false, "BitCoin-0x_AB", "bitcoin-0x_ab"},
      {true, "01hzx-ulid", "01HZX-ULID"},
      {false, "\xe2\x84\xaa" "ASPA", "\xe2\x84\xaa" "aspa"},  // U+212A stays
      {true, "stra\xc3\x9f" "e", "STRA\xc3\x9f" "E"},             // ß stays
      {false, "\xc4\xb0", "\xc4\xb0"},                            // İ stays
      {true, "", ""},
  };
  char out[64];
  for (const Case& c : cases) {
    size_t used = 99;
    const bool ok = c.upper ? fortis::compat::AsciiToUpperCase(c.in, strlen(c.in), out, sizeof(out), &used)
                            : fortis::compat::AsciiToLowerCase(c.in, strlen(c.in), out, sizeof(out), &used);
    if (!ok || used != strlen(c.out) || strcmp(out, c.out) != 0) {
      fprintf(stderr, "FAIL: ascii case of \"%s\"\n", c.in);
      return false;
    }
  }
  size_t used = 0;
  if (fortis::compat::AsciiToLowerCase("\xed\xa0\x80", 3, out, sizeof(out), &used) ||
      fortis::compat::AsciiToLowerCase("\xc3", 1, out, sizeof(out), &used)) {
    fputs("FAIL: ascii case accepts invalid UTF-8\n", stderr);
    return false;
  }
  if (fortis::compat::AsciiToUpperCase("abc", 3, out, 3, &used) ||
      !fortis::compat::AsciiToUpperCase("abc", 3, out, 4, &used)) {
    fputs("FAIL: ascii case capacity (NUL included)\n", stderr);
    return false;
  }
  return true;
}

int main() {
  if (!ascii_checks()) return 1;
  static char line[1 << 16];
  static char input[1 << 15];
  static char output[1 << 16];
  while (fgets(line, sizeof(line), stdin)) {
    size_t length = strcspn(line, "\r\n");
    line[length] = '\0';
    if (length < 2 || (line[0] != 'U' && line[0] != 'L') || line[1] != ' ') return 2;
    size_t bytes = 0;
    for (size_t i = 2; i + 1 < length + 1 && line[i]; i += 2) {
      const int high = nibble(line[i]), low = nibble(line[i + 1]);
      if (high < 0 || low < 0 || bytes >= sizeof(input)) return 2;
      input[bytes++] = static_cast<char>(high << 4 | low);
    }
    size_t used = 0;
    const bool ok = line[0] == 'U'
        ? fortis::compat::JsToUpperCase(input, bytes, output, sizeof(output), &used)
        : fortis::compat::JsToLowerCase(input, bytes, output, sizeof(output), &used);
    if (!ok) { fputs("!\n", stdout); continue; }
    for (size_t i = 0; i < used; ++i)
      printf("%02x", static_cast<unsigned char>(output[i]));
    fputc('\n', stdout);
  }
  printf("unicode %s\n", fortis::compat::js_case_tables::kUnicodeVersion);
  return 0;
}
