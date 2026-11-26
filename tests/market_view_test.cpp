// Line filter for tests/market_view_test.py:
//   "E <id hex> <row hex>" -> market entry JSON hex ("-" for a non-object row)
//   "D <hex>"              -> "1" when Date.parse would succeed, else "0"
//   "U <hex>"              -> "<0|1> <media key or ->"
#include "market_view.h"

#include <stdio.h>
#include <string.h>

using namespace fortis::codec;

namespace {
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
size_t unhex(const char* text, size_t length, unsigned char* output) {
  size_t bytes = 0;
  for (size_t i = 0; i + 1 < length; i += 2)
    output[bytes++] = static_cast<unsigned char>(nibble(text[i]) << 4 | nibble(text[i + 1]));
  return bytes;
}
}  // namespace

int main() {
  static char line[1 << 20];
  static unsigned char first[1 << 18], second[1 << 18];
  static BoundedJsonToken tokens[8192];
  static uint8_t out[1 << 18];
  static char image[1 << 14];
  static fortis::discovery::market_view::MarketViewScratch scratch;
  while (fgets(line, sizeof(line), stdin)) {
    const size_t length = strcspn(line, "\r\n");
    line[length] = '\0';
    const char kind = line[0];
    char* rest = line + 2;
    if (kind == 'D' || kind == 'U') {
      const size_t bytes = unhex(rest, strlen(rest), first);
      const char* text = reinterpret_cast<const char*>(first);
      if (kind == 'D') { puts(fortis::discovery::market_view::DateParseValid(text, bytes) ? "1" : "0"); continue; }
      const bool http = fortis::discovery::market_view::IsHttpUrl(text, bytes);
      char key[fortis::discovery::market_view::kMediaKeyChars + 1] = "-";
      if (http) fortis::discovery::market_view::MediaKeyFor(text, bytes, key);
      printf("%d %s\n", http ? 1 : 0, key);
      continue;
    }
    char* space = strchr(rest, ' ');
    if (kind != 'E' || !space) return 2;
    const size_t id_length = unhex(rest, static_cast<size_t>(space - rest), first);
    const size_t row_length = unhex(space + 1, strlen(space + 1), second);
    BoundedJsonDocument document;
    BoundedJsonError error{};
    if (!document.Parse(second, row_length, tokens, 8192, &error)) { puts("!"); continue; }
    const uint32_t row = document.root();
    if (document.token(row)->type != BoundedJsonType::kObject) { puts("-"); continue; }
    char key[fortis::discovery::market_view::kMediaKeyChars + 1];
    const char* logo = nullptr;
    size_t image_length = 0;
    if (fortis::discovery::market_view::RowImage(document, row, image, sizeof(image), &image_length) &&
        fortis::discovery::market_view::IsHttpUrl(image, image_length)) {
      fortis::discovery::market_view::MediaKeyFor(image, image_length, key);
      logo = key;
    }
    fortis::codec::history_batch::OutBuffer buffer{out, sizeof(out)};
    if (!fortis::discovery::market_view::AppendMarketEntry(document, row,
            reinterpret_cast<const char*>(first), id_length, logo, buffer, &scratch)) {
      puts("!");
      continue;
    }
    for (size_t i = 0; i < buffer.used; ++i) printf("%02x", out[i]);
    putchar('\n');
  }
  return 0;
}
