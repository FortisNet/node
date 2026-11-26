#include "test_support.h"
// Checks the cross-chain helpers against the TypeScript oracle
// corpus (tests/corpus/cross_chain_corpus.json, written by
// the earlier TypeScript implementation's tests): the address
// validators, normalizedRiskKey and the bridge directory, safeTimestamp,
// quoteValidity, routeId and boundedProviderSnapshot.
#include "strict_unit_expectations.h"
#include "cross_chain_support.h"
#include "token_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace cc = fortis::exchange::cross_chain;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* what, const char* detail = "") {
  ++checks;
  if (!ok) {
    ++failures;
    fprintf(stderr, "FAIL %s %s\n", what, detail);
  }
}

bool text_equals(const tj::Value* v, const char* text) {
  if (!text) return !v || v->type == tj::Type::kNull;
  return v && v->type == tj::Type::kString && strlen(text) == v->length && memcmp(v->text, text, v->length) == 0;
}

const char* stringify(tj::Arena& arena, const tj::Value* v) {
  const size_t capacity = 4 * 1024 * 1024;
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
  tj::OutBuffer out{buffer, capacity, 0, true};
  if (!buffer || !tj::Stringify(v, out) || !out.ok) return "<stringify failed>";
  return arena.CopyText(reinterpret_cast<const char*>(buffer), out.used);
}

const tj::Value* str_or_null(const tj::Value* v) {
  return v && v->type == tj::Type::kString ? v : nullptr;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s cross_chain_corpus.json\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* data = fixture_bytes.data();
  tj::Arena arena = tj::GrowingArena(512 * 1024 * 1024);
  const tj::Value* corpus = tj::Parse(arena, data, static_cast<size_t>(size));
  const auto* text_changes = strict::UnitExpectations(arena, "tests/fixtures/strict_text_expectations.json");
  strict::PatchRows(arena, tj::Get(corpus, "snapshots"), tj::Get(text_changes, "cross_snapshots"), "name");
  if (!corpus) { fprintf(stderr, "corpus does not parse\n"); return 2; }
  const double now = tj::Get(corpus, "now")->number;

  const tj::Value* addresses = tj::Get(corpus, "addresses");
  for (uint32_t i = 0; i < addresses->count; ++i) {
    const tj::Value* entry = addresses->items[i];
    const tj::Value* kind = tj::Get(entry, "kind");
    const tj::Value* value = tj::Get(entry, "value");
    const bool want = tj::Get(entry, "valid")->type == tj::Type::kTrue;
    bool got = false;
    if (cc::Is(kind, "ton")) got = cc::ValidTonFriendlyAddress(value);
    else if (cc::Is(kind, "monero")) got = cc::ValidMoneroAddress(value);
    else if (cc::Is(kind, "dash")) got = cc::ValidDashAddress(value);
    else if (cc::Is(kind, "zcash")) got = cc::ValidZcashTransparentAddress(value);
    else if (cc::Is(kind, "bitcoinCash")) got = cc::ValidBitcoinCashAddress(value);
    else if (cc::Is(kind, "solana")) got = cc::ValidSolanaAddress(value);
    else if (cc::Is(kind, "bitcoin")) got = cc::ValidBitcoinAddress(value);
    char detail[256];
    snprintf(detail, sizeof(detail), "%s %.120s want %d", kind->text, value->text, want);
    check(got == want, "address", detail);
  }

  const tj::Value* keys = tj::Get(corpus, "riskKeys");
  for (uint32_t i = 0; i < keys->count; ++i) {
    const tj::Value* entry = keys->items[i];
    const tj::Value* value = tj::Get(entry, "value");
    const char* risk = cc::NormalizedRiskKey(arena, value);
    const char* url = cc::BridgeWebsiteUrl(arena, value);
    char detail[256];
    snprintf(detail, sizeof(detail), "%.60s -> %s / %s", value->text, risk ? risk : "(null)", url ? url : "(null)");
    check(risk && text_equals(tj::Get(entry, "riskKey"), risk), "riskKey", detail);
    check(text_equals(tj::Get(entry, "bridgeUrl"), url), "bridgeUrl", detail);
  }

  // LI.FI Mayan variants: display names (toolDetails.name) and bare keys
  // (tool fallback) must both link to Mayan, never to MAYAChain.
  const char* mayan_aliases[] = {"Mayan (Swift)", "Mayan (Wormhole)", "CCTP + Mayan", "CCTPv2 + Mayan",
                                 "mayan",         "mayanWH",          "mayanMCTP",    "mayanFastMCTP"};
  for (const char* alias : mayan_aliases) {
    const char* url = cc::BridgeWebsiteUrl(arena, tj::Str(arena, alias, strlen(alias)));
    check(url && strcmp(url, "https://mayan.finance/") == 0, "mayanBridgeUrl", alias);
  }
  {
    const char* url = cc::BridgeWebsiteUrl(arena, tj::Str(arena, "Maya", 4));
    check(url && strcmp(url, "https://www.mayaprotocol.com/") == 0, "mayaBridgeUrl", "Maya");
  }

  const tj::Value* timestamps = tj::Get(corpus, "timestamps");
  for (uint32_t i = 0; i < timestamps->count; ++i) {
    const tj::Value* entry = timestamps->items[i];
    const char* iso = cc::SafeTimestamp(arena, tj::Get(entry, "value"));
    const tj::Value* want = str_or_null(tj::Get(entry, "iso"));
    char detail[256];
    snprintf(detail, sizeof(detail), "#%u -> %s", i, iso ? iso : "(null)");
    check(want ? (iso && text_equals(want, iso)) : !iso, "safeTimestamp", detail);
  }

  const tj::Value* validity = tj::Get(corpus, "validity");
  for (uint32_t i = 0; i < validity->count; ++i) {
    const tj::Value* entry = validity->items[i];
    cc::Validity got;
    const tj::Value* value = tj::Get(entry, "value");
    const bool ok = cc::QuoteValidity(arena, value && value->type == tj::Type::kNull ? nullptr : value, now, 30000, &got);
    char detail[256];
    snprintf(detail, sizeof(detail), "#%u -> %s %s", i, got.quoted_at ? got.quoted_at : "-",
             got.expires_at ? got.expires_at : "-");
    check(ok && text_equals(tj::Get(entry, "quotedAt"), got.quoted_at) &&
              text_equals(tj::Get(entry, "expiresAt"), got.expires_at),
          "quoteValidity", detail);
  }

  const tj::Value* ids = tj::Get(corpus, "routeIds");
  for (uint32_t i = 0; i < ids->count; ++i) {
    const tj::Value* entry = ids->items[i];
    const tj::Value* id = cc::RouteId(arena, tj::Get(entry, "providerId")->text, str_or_null(tj::Get(entry, "quoteId")),
                                      tj::Get(entry, "bridge"), tj::Get(entry, "amountOutMin")->text,
                                      str_or_null(tj::Get(entry, "to")), str_or_null(tj::Get(entry, "data")));
    char detail[256];
    snprintf(detail, sizeof(detail), "#%u -> %s", i, id ? id->text : "(null)");
    check(id && text_equals(tj::Get(entry, "id"), id->text), "routeId", detail);
  }

  const tj::Value* snapshots = tj::Get(corpus, "snapshots");
  for (uint32_t i = 0; i < snapshots->count; ++i) {
    const tj::Value* entry = snapshots->items[i];
    const tj::Value* text = tj::Get(entry, "text");
    const tj::Value* parsed = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
    const char* got = parsed ? stringify(arena, cc::BoundedProviderSnapshot(arena, parsed)) : "<parse failed>";
    const tj::Value* want = tj::Get(entry, "snapshot");
    char detail[512];
    snprintf(detail, sizeof(detail), "%s: %.200s", tj::Get(entry, "name")->text, got);
    check(text_equals(want, got), "snapshot", detail);
  }

  tj::FreeArena(arena);

  printf("cross_chain_corpus_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
