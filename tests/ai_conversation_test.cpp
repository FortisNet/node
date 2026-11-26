#include "test_support.h"
#include "ownership.h"
// Replays the TypeScript AI conversation unit corpus
// (tests/corpus/ai_conversation_corpus.json, written by
// the earlier TypeScript implementation's tests) through the
// native ports: zod datetime/trim, the snapshot schema with its normalized
// output and portfolio context, the owner HMAC, getAddress, verifyMessage,
// redactSecrets plus outbound suppression, OpenAI reply parsing and the
// exact OpenAI request bodies.
#include "strict_unit_expectations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "ai_proof.h"
#include "ai_reply.h"
#include "ai_snapshot.h"
#include "diagnostic_redaction.h"
#include "js_date_parse.h"
#include "token_json.h"

namespace {

namespace tj = fortis::codec::tj;
using tj::Value;

int g_failures = 0;
size_t g_checks = 0;

void fail(const char* group, size_t index, const char* message) {
  if (++g_failures <= 40) fprintf(stderr, "%s[%zu]: %s\n", group, index, message);
}

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file);
    free(data);
    return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

bool same(const Value* expected, const char* text, size_t length) {
  return tj::IsString(expected) && expected->length == length && memcmp(expected->text, text, length) == 0;
}

void show_difference(const Value* expected, const char* actual, size_t length) {
  size_t i = 0;
  while (i < length && i < expected->length && actual[i] == expected->text[i]) ++i;
  const size_t from = i > 80 ? i - 80 : 0;
  fprintf(stderr, "  differs at byte %zu\n  expected …%.*s\n  actual   …%.*s\n", i,
          static_cast<int>(expected->length - from < 200 ? expected->length - from : 200), expected->text + from,
          static_cast<int>(length - from < 200 ? length - from : 200), actual + from);
}

void datetimes(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* text = tj::Get(row, "text");
    const bool valid = tj::Get(row, "valid")->type == tj::Type::kTrue;
    ++g_checks;
    if (fortis::ai::IsZodDatetime(text->text, text->length) != valid) fail("datetime", i, text->text);
    const Value* ms = tj::Get(row, "ms");
    if (valid && tj::IsNumber(ms)) {
      uint16_t wide[128];
      size_t n = 0;
      for (size_t k = 0; k < text->length && n < 128; ++k) wide[n++] = static_cast<uint8_t>(text->text[k]);
      double parsed = 0;
      ++g_checks;
      if (!fortis::compat::JsDateParse(wide, n, &parsed) || parsed != ms->number) fail("datetime-ms", i, text->text);
    }
  }
}

void bodies(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* text = tj::Get(row, "text");
    const Value* value = tj::Get(row, "value");
    size_t begin = 0, end = 0;
    const bool ok = fortis::ai::ZodTrimmedBody(text->text, text->length, &begin, &end);
    ++g_checks;
    if (ok != tj::IsString(value) || (ok && !same(value, text->text + begin, end - begin))) fail("body", i, "trim");
  }
}

void snapshots(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* name = tj::Get(row, "name");
    const Value* text = tj::Get(row, "text");
    // compat-decisions.md#js-prototype-keys: an own "__proto__"
    // key is an ordinary unknown key, which the strict snapshot schema
    // refuses (zod skipped it).
    const bool proto_key = strcmp(name->text, "protoKey") == 0 || strcmp(name->text, "protoKeyNested") == 0;
    const bool schema = tj::Get(row, "schema")->type == tj::Type::kTrue && !proto_key;
    const bool links = tj::Get(row, "links")->type == tj::Type::kTrue;
    tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
    const Value* parsed = fortis::ai::ParseSnapshot(text->text, text->length, arena);
    ++g_checks;
    if (!!parsed != schema) {
      fail("snapshot", i, name->text);
      tj::FreeArena(arena);
      continue;
    }
    if (parsed) {
      tj::OutBuffer counter{};
      tj::Stringify(parsed, counter);
      char* normalized = static_cast<char*>(malloc(counter.used + 1));
      tj::OutBuffer out{reinterpret_cast<uint8_t*>(normalized), counter.used + 1};
      tj::Stringify(parsed, out);
      ++g_checks;
      if (!same(tj::Get(row, "normalized"), normalized, out.used)) {
        fail("snapshot-normalized", i, name->text);
        show_difference(tj::Get(row, "normalized"), normalized, out.used);
      }
      free(normalized);
      ++g_checks;
      if (fortis::ai::SnapshotLinksValid(parsed) != links) fail("snapshot-links", i, name->text);
      if (links) {
        const char* context = nullptr;
        size_t context_length = 0;
        ++g_checks;
        if (!fortis::ai::PortfolioContext(parsed, arena, &context, &context_length) ||
            !same(tj::Get(row, "context"), context, context_length)) {
          fail("snapshot-context", i, name->text);
          if (context) show_difference(tj::Get(row, "context"), context, context_length);
        }
      }
    }
    tj::FreeArena(arena);
  }
}

void owners(const Value* list, const Value* secret) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* address = tj::Get(row, "address");
    char owner[65];
    ++g_checks;
    bool ascii = true;
    for (uint32_t k = 0; k < address->length; ++k) ascii = ascii && static_cast<unsigned char>(address->text[k]) < 0x80;
    if (!ascii) {
      // js-case: the owner id lower-cases ASCII only, so for a
      // non-ASCII address (never a valid EVM owner) the recorded TypeScript
      // id no longer applies; it is the HMAC over the ASCII-lowered address.
      std::string message = "intelligence-owner:";
      for (uint32_t k = 0; k < address->length; ++k) {
        const char c = address->text[k];
        message += c >= 'A' && c <= 'Z' ? static_cast<char>(c | 0x20) : c;
      }
      char want[65];
      fortis::ai::HmacSha256Hex(secret->text, secret->length, message.data(), message.size(), want);
      if (!fortis::ai::OwnerId(secret->text, secret->length, address->text, address->length, owner) ||
          strcmp(owner, want) != 0)
        fail("owner", i, address->text);
      continue;
    }
    if (!fortis::ai::OwnerId(secret->text, secret->length, address->text, address->length, owner) ||
        !same(tj::Get(row, "owner"), owner, 64))
      fail("owner", i, address->text);
  }
}

void checksums(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* expected = tj::Get(row, "checksum");
    char output[43];
    const bool ok = fortis::ai::ChecksumAddress(tj::Get(row, "address")->text, output);
    ++g_checks;
    if (ok != tj::IsString(expected) || (ok && !same(expected, output, 42))) fail("checksum", i, output);
  }
}

void signatures(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* message = tj::Get(row, "message");
    const Value* expected = tj::Get(row, "recovered");
    char output[43];
    const bool ok = fortis::ai::VerifyMessage(message->text, message->length, tj::Get(row, "signature")->text, output);
    ++g_checks;
    if (ok != tj::IsString(expected) || (ok && !same(expected, output, 42))) fail("signature", i, output);
  }
}

void redactions(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* input = tj::Get(row, "input");
    std::unique_ptr<char[], fortis::base::StorageDeleter> output;
    size_t length = 0;
    bool redacted = false;
    ++g_checks;
    if (!fortis::security::redaction::RedactSecrets(input->text, input->length, &output, &length, &redacted) ||
        !same(tj::Get(row, "text"), output.get(), length) ||
        redacted != (tj::Get(row, "redacted")->type == tj::Type::kTrue)) {
      fail("redact", i, input->text);
    }
    output.reset();
    ++g_checks;
    if (fortis::ai::OutboundSuppressed(input->text, input->length) !=
        (tj::Get(row, "suppressed")->type == tj::Type::kTrue))
      fail("outbound", i, input->text);
  }
}

void replies(const Value* list) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* text = tj::Get(row, "text");
    const Value* expected = tj::Get(row, "result");
    tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
    const char* reply = nullptr;
    size_t length = 0;
    const bool ok = fortis::ai::ParseOpenAiReply(static_cast<int>(tj::Get(row, "status")->number),
                                                 reinterpret_cast<const uint8_t*>(text->text), text->length, arena,
                                                 &reply, &length);
    ++g_checks;
    if (ok != tj::IsString(expected) || (ok && !same(expected, reply, length))) fail("reply", i, text->text);
    tj::FreeArena(arena);
  }
}

void requests(const Value* list, const Value* model) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* row = list->items[i];
    const Value* snapshot_text = tj::Get(row, "snapshot");
    const Value* turns = tj::Get(row, "turns");
    tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
    const Value* snapshot = fortis::ai::ParseSnapshot(snapshot_text->text, snapshot_text->length, arena);
    fortis::ai::Turn list_turns[64];
    for (uint32_t j = 0; j < turns->count && j < 64; ++j) {
      const Value* body = tj::Get(turns->items[j], "body");
      list_turns[j] = {tj::StringEquals(tj::Get(turns->items[j], "role"), "assistant"), body->text, body->length};
    }
    const uint8_t* body = nullptr;
    size_t length = 0;
    ++g_checks;
    if (!snapshot || !fortis::ai::BuildOpenAiRequest(model->text, model->length, snapshot, list_turns, turns->count,
                                                     arena, &body, &length) ||
        !same(tj::Get(row, "body"), reinterpret_cast<const char*>(body), length)) {
      fail("request", i, "body");
      if (body) show_difference(tj::Get(row, "body"), reinterpret_cast<const char*>(body), length);
    }
    tj::FreeArena(arena);
  }
}

void version_two_and_anthropic() {
  const char snapshot[] =
      "{\"version\":2,\"capturedAt\":\"2026-11-09T10:00:00Z\",\"balancesComplete\":false,"
      "\"wallets\":[{\"id\":\"w\",\"address\":\"0x1111111111111111111111111111111111111111\","
      "\"family\":\"evm\",\"networkIds\":[\"eth\"],\"watchOnly\":false}],"
      "\"tokens\":[{\"assetId\":\"eth:native\",\"networkId\":\"eth\",\"symbol\":\"ETH\","
      "\"decimals\":18,\"source\":\"native\"}],"
      "\"holdings\":[],\"coverage\":[{\"walletId\":\"w\",\"networkId\":\"eth\","
      "\"status\":\"unknown\",\"knownZero\":false,\"observedAt\":null,\"source\":\"unknown\",\"stale\":true}],"
      "\"prices\":[]}";
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  const Value* parsed = fortis::ai::ParseSnapshot(snapshot, sizeof(snapshot) - 1, arena);
  ++g_checks;
  if (!parsed || !fortis::ai::SnapshotLinksValid(parsed)) fail("v2", 0, "unknown coverage must validate");
  if (parsed) {
    const fortis::ai::Turn turn{false, "What is my ETH balance?", 23};
    const uint8_t* body = nullptr;
    size_t length = 0;
    ++g_checks;
    if (!fortis::ai::BuildAnthropicRequest("claude-opus-4-6", "low", 2048, parsed, &turn, 1,
                                          arena, &body, &length) ||
        !strstr(reinterpret_cast<const char*>(body), "\"output_config\":{\"effort\":\"low\"}") ||
        !strstr(reinterpret_cast<const char*>(body), "\\\"status\\\":\\\"unknown\\\""))
      fail("anthropic", 0, "request and unknown coverage");
    ++g_checks;
    if (fortis::ai::BuildAnthropicRequest("claude-opus-4-6", "adaptive", 2048, parsed, &turn, 1,
                                          arena, &body, &length))
      fail("anthropic", 1, "unsupported effort");
  }
  std::string estimated_snapshot(snapshot);
  const std::string empty_holdings = "\"holdings\":[]";
  const size_t holding_offset = estimated_snapshot.find(empty_holdings);
  if (holding_offset != std::string::npos)
    estimated_snapshot.replace(holding_offset, empty_holdings.size(),
        "\"holdings\":[{\"walletId\":\"w\",\"assetId\":\"eth:native\","
        "\"rawAmount\":null,\"displayUnits\":\"0.5\",\"precision\":\"estimated\"}]");
  const Value* estimated = fortis::ai::ParseSnapshot(estimated_snapshot.c_str(), estimated_snapshot.size(), arena);
  ++g_checks;
  if (!estimated || !fortis::ai::SnapshotLinksValid(estimated))
    fail("v2", 2, "estimated holding may coexist with unknown coverage");
  const char priced_snapshot[] =
      "{\"version\":2,\"capturedAt\":\"2026-11-09T10:00:00Z\",\"balancesComplete\":true,"
      "\"wallets\":[{\"id\":\"w\",\"address\":\"0x1111111111111111111111111111111111111111\","
      "\"family\":\"evm\",\"networkIds\":[\"eth\"],\"watchOnly\":false}],"
      "\"tokens\":[{\"assetId\":\"eth:native\",\"networkId\":\"eth\",\"symbol\":\"ETH\","
      "\"decimals\":18,\"source\":\"native\"}],"
      "\"holdings\":[{\"walletId\":\"w\",\"assetId\":\"eth:native\",\"rawAmount\":\"1500000000000000000\","
      "\"displayUnits\":\"1.5\",\"precision\":\"exact\"}],"
      "\"coverage\":[{\"walletId\":\"w\",\"networkId\":\"eth\",\"status\":\"observed\","
      "\"knownZero\":false,\"observedAt\":\"2026-11-09T10:00:00Z\",\"source\":\"direct\",\"stale\":false}],"
      "\"prices\":[{\"assetId\":\"eth:native\",\"usd\":\"2.25\",\"lookup\":\"symbol:ETH\","
      "\"source\":\"fixture\",\"observedAt\":null,\"stale\":true}]}";
  const Value* priced = fortis::ai::ParseSnapshot(priced_snapshot, sizeof(priced_snapshot) - 1, arena);
  const char* context = nullptr;
  size_t context_length = 0;
  ++g_checks;
  if (!priced || !fortis::ai::SnapshotLinksValid(priced) ||
      !fortis::ai::PortfolioContext(priced, arena, &context, &context_length) ||
      !strstr(context, "\"cachedPricedTotalUsd\":\"3.38\"") ||
      !strstr(context, "\"totalPartial\":true"))
    fail("v2", 1, "deterministic cents and stale partial label");
  const char reply[] = "{\"type\":\"message\",\"stop_reason\":\"end_turn\","
                       "\"content\":[{\"type\":\"text\",\"text\":\"Balances are unknown.\"}]}";
  const char* text = nullptr;
  size_t text_length = 0;
  ++g_checks;
  if (!fortis::ai::ParseAnthropicReply(200, reinterpret_cast<const uint8_t*>(reply),
                                       sizeof(reply) - 1, arena, &text, &text_length) ||
      text_length != strlen("Balances are unknown.") ||
      memcmp(text, "Balances are unknown.", text_length) != 0)
    fail("anthropic", 2, "natural text reply");
  tj::FreeArena(arena);
}

}  // namespace

int main(int argc, char** argv) try {
  const char* path = argc > 1 ? argv[1] : "tests/corpus/ai_conversation_corpus.json";
  size_t length = 0;
  char* data = read_file(path, &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", path);
    return 1;
  }
  if (!fortis::ai::OutboundPatternsReady()) {
    fprintf(stderr, "outbound patterns did not compile\n");
    return 1;
  }
  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const Value* corpus = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const auto* text_changes = strict::UnitExpectations(arena, "tests/fixtures/strict_text_expectations.json");
  strict::PatchIndexRows(arena, tj::Get(corpus, "snapshots"), tj::Get(text_changes, "ai_snapshots"));
  strict::PatchIndexRows(arena, tj::Get(corpus, "replies"), tj::Get(text_changes, "ai_replies"));
  strict::PatchIndexRows(arena, tj::Get(corpus, "requests"), tj::Get(text_changes, "ai_requests"));
  if (!corpus) {
    fprintf(stderr, "corpus is not JSON\n");
    return 1;
  }
  version_two_and_anthropic();
  datetimes(tj::Get(corpus, "datetimes"));
  bodies(tj::Get(corpus, "bodies"));
  snapshots(tj::Get(corpus, "snapshots"));
  owners(tj::Get(corpus, "owners"), tj::Get(corpus, "secret"));
  checksums(tj::Get(corpus, "checksums"));
  signatures(tj::Get(corpus, "signatures"));
  redactions(tj::Get(corpus, "redactions"));
  replies(tj::Get(corpus, "replies"));
  requests(tj::Get(corpus, "requests"), tj::Get(corpus, "model"));
  tj::FreeArena(arena);
  free(data);
  if (g_failures) {
    fprintf(stderr, "ai_conversation_test: %d of %zu checks failed\n", g_failures, g_checks);
    return 1;
  }
  printf("ai_conversation_test: %zu checks passed\n", g_checks);
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
