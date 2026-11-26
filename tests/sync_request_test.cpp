#include <memory>
#include "test_support.h"
// Replays tests/corpus/sync_datetime_corpus.json (written by
// the earlier TypeScript implementation's tests) against the
// POST /v1/sync `after` grammars in sync_request.cpp: Fastify's ajv-formats
// "date-time" (AjvDateTime), zod's iso.datetime({ offset: true })
// (ZodDateTime) and, for every zod-valid string, new Date(s).getTime()
// (provider_text::DateParse). The corpus hash is checked against its pin.
#include "sync_request.h"
#include "token_json.h"
#include "provider_text.h"

#include <mbedtls/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s sync_datetime_corpus.json\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* text = fixture_bytes.data();

  char pin_path[1024];
  snprintf(pin_path, sizeof(pin_path), "%s.sha256", argv[1]);
  std::ifstream pin_file(pin_path, std::ios::binary);
  char pin[65] = {};
  if (!pin_file.read(pin, 64)) { fprintf(stderr, "no pin %s\n", pin_path); return 2; }
  pin_file.close();
  unsigned char digest[32];
  mbedtls_sha256(text, static_cast<size_t>(size), digest, 0);
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (strcmp(hex, pin) != 0) {
    fprintf(stderr, "corpus sha256 %s does not match the pinned %s\n", hex, pin);
    return 1;
  }

  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* document = tj::Parse(arena, text, static_cast<size_t>(size));
  const tj::Value* corpus = tj::Get(document, "corpus");
  if (!tj::IsArray(corpus)) { fprintf(stderr, "no corpus\n"); return 2; }
  int failures = 0, checked = 0;
  for (uint32_t i = 0; i < corpus->count; ++i) {
    const tj::Value* entry = corpus->items[i];
    const tj::Value* input = tj::Get(entry, "input");
    const bool ajv = tj::Get(entry, "ajv")->type == tj::Type::kTrue;
    const bool zod = tj::Get(entry, "zod")->type == tj::Type::kTrue;
    const tj::Value* date = tj::Get(entry, "dateMs");
    const bool got_ajv = fortis::wallet::sync::AjvDateTime(input->text, input->length);
    const bool got_zod = fortis::wallet::sync::ZodDateTime(input->text, input->length);
    checked += 2;
    if (got_ajv != ajv || got_zod != zod) {
      if (++failures <= 40)
        fprintf(stderr, "%s: ajv %d (want %d) zod %d (want %d)\n", input->text, got_ajv, ajv, got_zod, zod);
    }
    if (zod) {
      ++checked;
      double ms = 0;
      const bool parsed = fortis::codec::provider_text::DateParse(input->text, input->length, &ms);
      const bool want = tj::IsNumber(date);
      if (parsed != want || (want && ms != date->number)) {
        if (++failures <= 40)
          fprintf(stderr, "%s: Date %d %.0f (want %d %.0f)\n", input->text, parsed, ms, want,
                  want ? date->number : 0.0);
      }
    }
  }
  const uint32_t strings = corpus->count;  // the corpus lives in the arena
  tj::FreeArena(arena);

  if (failures) {
    fprintf(stderr, "sync request grammars: %d failure(s) over %d checks\n", failures, checked);
    return 1;
  }
  printf("sync request grammars: %d checks over %u strings match ajv-formats, zod and Date\n", checked, strings);
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
