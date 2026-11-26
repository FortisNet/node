// Reads hex-encoded JSON documents, one per line, and prints for each
// "<stringify hex> <protobuf Value hex> <array element count or -1>" (or "!"
// for a refused document). Driven by tests/js_json_test.py, which compares
// against JSON.stringify(JSON.parse()) and protobuf-es toBinary(fromJson()).
#include "js_json.h"

#include <stdio.h>
#include <string.h>

using namespace fortis::codec;

namespace {
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
bool count_element(void* context, const unsigned char*, size_t) {
  ++*static_cast<long*>(context);
  return true;
}
void hex(const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) printf("%02x", bytes[i]);
}
}  // namespace

int main() {
  static char line[1 << 20];
  static unsigned char input[1 << 19];
  static BoundedJsonToken tokens[32768];
  static uint32_t words[32768 * fortis::compat::js_json::kScratchWordsPerToken];
  static uint8_t json[1 << 20];
  static uint8_t proto[1 << 20];
  const fortis::compat::js_json::Scratch scratch{words, sizeof(words) / sizeof(words[0])};
  while (fgets(line, sizeof(line), stdin)) {
    size_t length = strcspn(line, "\r\n");
    size_t bytes = 0;
    for (size_t i = 0; i + 1 < length; i += 2) {
      const int high = nibble(line[i]), low = nibble(line[i + 1]);
      if (high < 0 || low < 0) return 2;
      input[bytes++] = static_cast<unsigned char>(high << 4 | low);
    }
    BoundedJsonDocument document;
    BoundedJsonError error{};
    if (!document.ParseWithLimits(input, bytes, tokens, 32768, 32768, sizeof(input), &error)) {
      puts("!");
      continue;
    }
    fortis::codec::history_batch::OutBuffer json_out{json, sizeof(json)};
    fortis::codec::history_batch::OutBuffer proto_out{proto, sizeof(proto)};
    if (!fortis::compat::js_json::Stringify(document, document.root(), json_out, scratch) ||
        !fortis::compat::js_json::AppendProtoValue(document, document.root(), proto_out, scratch)) {
      puts("!");
      continue;
    }
    bool is_array = false;
    long elements = 0;
    if (!fortis::compat::js_json::ForEachTopLevelArrayElement(input, bytes, &is_array,
                                                      count_element, &elements)) {
      puts("!");
      continue;
    }
    hex(json, json_out.used); putchar(' ');
    hex(proto, proto_out.used); printf(" %ld\n", is_array ? elements : -1L);
  }
  return 0;
}
