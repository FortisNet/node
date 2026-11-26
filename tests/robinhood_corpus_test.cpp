#include "test_support.h"
// Checks the Robinhood policy against the TypeScript oracle corpus
// (tests/corpus/robinhood_corpus.json, written by
// the earlier TypeScript implementation's tests): stockLogoUrl, the
// trade-call policy over router calldata (validateTradeCalls), revert
// decoding (through a simulated status-0x0 row whose `error` is
// "fallback error") and prepared-call validation through the gateway. It
// also drives the pending-ticket store: device binding, the two-minute
// lifetime, in-flight exclusion and the 1,000-ticket cap.
#include "strict_unit_expectations.h"
#include "robinhood_policy.h"
#include "robinhood_tickets.h"
#include "token_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rh = fortis::exchange::robinhood;
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

const char* stringify(tj::Arena& arena, const tj::Value* v) {
  if (!v) return "<null>";
  const size_t capacity = 1024 * 1024;
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
  tj::OutBuffer out{buffer, capacity, 0, true};
  if (!buffer || !tj::Stringify(v, out) || !out.ok) return "<stringify failed>";
  return arena.CopyText(reinterpret_cast<const char*>(buffer), out.used);
}

bool same(tj::Arena& arena, const tj::Value* a, const tj::Value* b) {
  return strcmp(stringify(arena, a), stringify(arena, b)) == 0;
}

const char* name_of(const tj::Value* entry) {
  const tj::Value* name = tj::Get(entry, "name");
  return name && name->type == tj::Type::kString ? name->text : "?";
}

void check_tickets() {
  rh::TicketStore store;
  const int64_t t0 = 1000000;
  const char* device = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
  const char* other = "01ARZ3NDEKTSV4RRFFQ69G5FB2";
  const char* text = "{\"type\":\"user-operation-v070\"}";
  char ticket[33];
  memcpy(ticket, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 33);
  check(store.Insert(ticket, device, t0 + 120000, text, strlen(text), 1, t0) == rh::TicketStore::kInserted,
        "tickets", "insert");
  rh::TicketStore::Claim claim;
  check(store.Begin(ticket, other, t0, 1, &claim) == rh::TicketStore::kNotFound, "tickets", "other device");
  check(store.Begin("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", device, t0, 1, &claim) == rh::TicketStore::kNotFound, "tickets",
        "unknown");
  check(store.Begin(ticket, device, t0 + 119999, 2, &claim) == rh::TicketStore::kWrongCount, "tickets",
        "count before expiry");
  check(store.Begin(ticket, device, t0 + 1, 1, &claim) == rh::TicketStore::kClaimed && claim.length == strlen(text) &&
            memcmp(claim.prepared.get(), text, claim.length) == 0,
        "tickets", "claim");
  rh::TicketStore::Claim second;
  check(store.Begin(ticket, device, t0 + 1, 1, &second) == rh::TicketStore::kNotFound, "tickets", "in flight");
  store.Finish(claim, false);
  claim.prepared.reset();
  check(store.Begin(ticket, device, t0 + 2, 1, &claim) == rh::TicketStore::kClaimed, "tickets", "retry after failure");
  store.Finish(claim, true);
  claim.prepared.reset();
  check(store.Begin(ticket, device, t0 + 3, 1, &claim) == rh::TicketStore::kNotFound, "tickets", "replay");
  memcpy(ticket, "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC", 33);
  check(store.Insert(ticket, device, t0 + 120000, text, strlen(text), 1, t0) == rh::TicketStore::kInserted,
        "tickets", "insert second");
  check(store.Begin(ticket, device, t0 + 120000, 1, &claim) == rh::TicketStore::kNotFound, "tickets",
        "expired at the instant");
  // A ticket that expires while in flight is not resurrected by a failure.
  memcpy(ticket, "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD", 33);
  store.Insert(ticket, device, t0 + 120000, text, strlen(text), 1, t0);
  check(store.Begin(ticket, device, t0, 1, &claim) == rh::TicketStore::kClaimed, "tickets", "claim before expiry");
  check(store.Pending(t0 + 120000) == 0, "tickets", "pruned in flight");
  store.Finish(claim, false);
  claim.prepared.reset();
  check(store.Begin(ticket, device, t0 + 1, 1, &second) == rh::TicketStore::kNotFound, "tickets", "not resurrected");
  // The 1,000 pending cap counts unexpired tickets, in flight or not.
  for (int i = 0; i < 1000; ++i) {
    snprintf(ticket, sizeof(ticket), "T%031d", i);
    if (store.Insert(ticket, device, t0 + 120000, text, strlen(text), 1, t0) != rh::TicketStore::kInserted) {
      check(false, "tickets", "fill");
      break;
    }
  }
  check(store.Insert("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", device, t0 + 120000, text, strlen(text), 1, t0) ==
            rh::TicketStore::kFull,
        "tickets", "cap");
  check(store.Insert("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", device, t0 + 240000, text, strlen(text), 1, t0 + 120000) ==
            rh::TicketStore::kInserted,
        "tickets", "pruned after expiry");
  check(store.Pending(t0 + 120000) == 1, "tickets", "pending after prune");
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s robinhood_corpus.json\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* data = fixture_bytes.data();
  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* corpus = tj::Parse(arena, data, static_cast<size_t>(size));
  const auto* text_changes = strict::UnitExpectations(arena, "tests/fixtures/strict_text_expectations.json");
  strict::PatchIndexRows(arena, tj::Get(corpus, "reverts"), tj::Get(text_changes, "robinhood_reverts"));
  if (!corpus) { fprintf(stderr, "corpus does not parse\n"); return 2; }

  const tj::Value* logos = tj::Get(corpus, "stockLogos");
  for (uint32_t i = 0; logos && i < logos->count; ++i) {
    const tj::Value* entry = logos->items[i];
    const tj::Value* symbol = tj::Get(entry, "symbol");
    const tj::Value* url = tj::Get(entry, "url");
    const char* mine = rh::StockLogoUrl(arena, symbol->text, symbol->length);
    const bool ok = url->type == tj::Type::kNull ? mine == nullptr
                                                 : mine && strlen(mine) == url->length && memcmp(mine, url->text, url->length) == 0;
    check(ok, "stockLogoUrl", symbol->text);
  }

  const tj::Value* trades = tj::Get(corpus, "trades");
  for (uint32_t i = 0; trades && i < trades->count; ++i) {
    const tj::Value* entry = trades->items[i];
    const tj::Value* addresses = tj::Get(entry, "stockAddresses");
    rh::StockToken tokens[8];
    size_t count = 0;
    for (uint32_t k = 0; k < addresses->count && count < 8; ++k, ++count) {
      const tj::Value* a = addresses->items[k];
      for (size_t c = 0; c < 42; ++c) {
        const char ch = a->text[c];
        tokens[count].address[c] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch | 0x20) : ch;
      }
      tokens[count].address[42] = '\0';
    }
    rh::StockSet stocks{tokens, count};
    const tj::Value* from = tj::Get(entry, "from");
    bool exhausted = false;
    tj::Value* normalized = rh::ValidateTradeCalls(arena, tj::Get(entry, "calls"), stocks, from->text, from->length,
                                                   &exhausted);
    const bool expected = tj::Get(entry, "ok")->type == tj::Type::kTrue;
    if ((normalized != nullptr) != expected) {
      check(false, "validateTradeCalls", name_of(entry));
      continue;
    }
    check(!expected || same(arena, normalized, tj::Get(entry, "normalized")), "validateTradeCalls normalized",
          name_of(entry));
  }

  const tj::Value* reverts = tj::Get(corpus, "reverts");
  for (uint32_t i = 0; reverts && i < reverts->count; ++i) {
    const tj::Value* entry = reverts->items[i];
    tj::Value* reason = rh::DecodeRevertReason(arena, tj::Get(entry, "data"));
    if (!reason) reason = tj::Str(arena, "fallback error");
    char label[64];
    snprintf(label, sizeof(label), "#%u", i);
    check(same(arena, reason, tj::Get(entry, "reason")), "decodeRevertReason", label);
  }

  const tj::Value* prepareds = tj::Get(corpus, "prepared");
  for (uint32_t i = 0; prepareds && i < prepareds->count; ++i) {
    const tj::Value* entry = prepareds->items[i];
    tj::Value* requests = nullptr;
    size_t nodes = 0;
    const bool ok = rh::ValidatePrepared(arena, tj::Get(entry, "prepared"),
                                         "0x2222222222222222222222222222222222222222", &requests, &nodes);
    const bool expected = tj::Get(entry, "ok")->type == tj::Type::kTrue;
    check(ok == expected, "validatePrepared", name_of(entry));
    if (ok && expected)
      check(same(arena, requests, tj::Get(entry, "signatureRequests")), "validatePrepared requests", name_of(entry));
  }

  check_tickets();
  tj::FreeArena(arena);

  printf("robinhood corpus: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
