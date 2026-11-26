#include "token_json.h"
#include "js_json.h"
#include "token_text.h"
#include "text_sanitize.h"
#include "bounded_json.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;
int main() {
  auto arena = tj::GrowingArena(1024 * 1024);
  int failures = 0;
  auto check = [&](bool ok, const char* name) { if (!ok) { fprintf(stderr, "FAIL: %s\n", name); ++failures; } };
  const char input[] = "[\"\\ud800x\\udc00\",\"\\ud83d\\ude00\",\"\\ud800\\u0061\"]";
  auto* doc = tj::Parse(arena, reinterpret_cast<const uint8_t*>(input), strlen(input));
  check(tj::IsArray(doc) && doc->count == 3, "lone escapes are valid JSON input");
  uint8_t bytes[512];
  tj::OutBuffer out{bytes, sizeof(bytes)};
  const char expected[] = "[\"\xef\xbf\xbdx\xef\xbf\xbd\",\"\xf0\x9f\x98\x80\",\"\xef\xbf\xbd" "a\"]";
  check(doc && tj::Stringify(doc, out) && out.used == strlen(expected) && !memcmp(bytes, expected, out.used),
        "one Unicode scalar representation for escaped lone surrogates");
  fortis::codec::BoundedJsonDocument bounded;
  fortis::codec::BoundedJsonToken tokens[64];
  fortis::codec::BoundedJsonError error{};
  check(bounded.Parse(reinterpret_cast<const uint8_t*>(input), strlen(input), tokens, 64, &error),
        "bounded JSON shares replacement policy");
  char decoded[32]; size_t length = 0;
  const auto* root = bounded.token(bounded.root());
  check(root && bounded.DecodeString(root->first_child, decoded, sizeof(decoded), &length) &&
        length == 7 && !memcmp(decoded, "\xef\xbf\xbdx\xef\xbf\xbd", 7), "bounded decoded bytes agree");
  const char emoji[] = "a\xf0\x9f\x98\x80" "b";
  auto* first = fortis::compat::token_text::Slice(arena, emoji, strlen(emoji), 0, 2);
  auto* last = fortis::compat::token_text::Slice(arena, emoji, strlen(emoji), 2, 4);
  check(tj::StringEquals(first, "a") && tj::StringEquals(last, "b"), "slice never emits half a code point");
  char sanitized[64];
  const char symbol[] = "abcdefghijklmn\xf0\x9f\x98\x80zz";
  length = fortis::compat::SanitizeSymbol(symbol, strlen(symbol), sanitized, sizeof(sanitized));
  check(length == 17 && !memcmp(sanitized, "abcdefghijklmn\xe2\x80\xa6", 17),
        "display truncation drops partial emoji before ellipsis");
  const char raw[] = "\xff";
  auto* text = tj::Str(arena, raw, 1);
  check(text && text->length == 3 && fortis::codec::utf8::Valid(reinterpret_cast<const uint8_t*>(text->text), text->length),
        "builder strings normalize malformed UTF-8");
  auto* bom = tj::Str(arena, "\xef\xbb\xbf" "x", 4);
  check(bom && bom->length == 4, "builder content retains BOM");
  const char source_order[] = "{\"b\":1,\"10\":2,\"2\":3,\"b\":4}";
  const char ordered[] = "{\"b\":4,\"10\":2,\"2\":3}";
  auto* object = tj::Parse(arena, reinterpret_cast<const uint8_t*>(source_order), strlen(source_order));
  tj::OutBuffer ordered_out{bytes, sizeof(bytes)};
  check(object && tj::Stringify(object, ordered_out) && ordered_out.used == strlen(ordered) &&
        !memcmp(bytes, ordered, ordered_out.used), "DOM source order and last duplicate value");
  check(bounded.Parse(reinterpret_cast<const uint8_t*>(source_order), strlen(source_order), tokens, 64, &error),
        "bounded source-order input");
  uint32_t scratch[512]{};
  ordered_out.used = 0;
  check(fortis::compat::js_json::Stringify(bounded, bounded.root(), ordered_out, {scratch, 512}) &&
        ordered_out.used == strlen(ordered) && !memcmp(bytes, ordered, ordered_out.used), "bounded source order");
  tj::FreeArena(arena);
  if (failures) return 1;
  puts("unicode_contract_test: DOM/bounded decode, builders and code-point-safe truncation OK");
  return 0;
}
