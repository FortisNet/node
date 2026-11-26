// Unit checks of token_bittensor (the Bittensor driver helpers) that the
// fixture replay cannot reach cheaply: the driver's 256-netuid working set
// (15 s expiry, expired entries dropped first, then the oldest), the TAO/USD
// entry, the en-US formatter against values printed by V8
// (`toLocaleString('en-US', { maximumFractionDigits: 9 })`) and the netuid
// grammar. Usage: token_bittensor_unit_test
#include "token_bittensor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using fortis::tokens::token_bittensor::WorkingSet;
namespace tb = fortis::tokens::token_bittensor;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  ++failures;
  printf("FAIL: %s\n", what);
}

struct Scratch {
  uint8_t bytes[1 << 16];
  tj::Arena arena{bytes, sizeof(bytes), 0, true};
  void Reset() { arena.used = 0; arena.ok = true; }
};

tj::Value* pool_page(Scratch& s, uint32_t netuid, const char* symbol) {
  char body[512];
  snprintf(body, sizeof(body),
           "{\"data\":[{\"netuid\":%u,\"symbol\":\"%s\",\"name\":\"n\",\"price\":\"0.5\",\"alpha_staked\":\"-0\","
           "\"total_alpha\":\"1000000000\"}]}",
           netuid, symbol);
  return tb::PoolPage(s.arena, tj::Parse(s.arena, reinterpret_cast<const uint8_t*>(body), strlen(body)), netuid);
}

bool symbol_is(const tj::Value* page, const char* symbol) {
  const tj::Value* v = tj::Get(page, "symbol");
  return tj::IsString(v) && v->length == strlen(symbol) && memcmp(v->text, symbol, v->length) == 0;
}

void working_set() {
  Scratch s;
  WorkingSet* set = tb::NewWorkingSet();
  expect(set != nullptr, "allocate the working set");
  if (!set) return;
  const int64_t t0 = 1768176000000;
  tj::Value* page = nullptr;
  // Remembered pages answer for 15 s, with −0 kept.
  set->RememberPool(1, t0, pool_page(s, 1, "A"));
  expect(set->Pool(1, t0 + 14999, s.arena, &page) && symbol_is(page, "A"), "hit before 15 s");
  const tj::Value* staked = tj::Get(page, "alphaStaked");
  expect(tj::IsNumber(staked) && staked->number == 0 && signbit(staked->number), "-0 survives the copy");
  expect(!set->Pool(1, t0 + 15000, s.arena, &page), "expired at 15 s");
  // A JSON null page (no matching row) is kept too.
  set->RememberPool(2, t0, tj::Null(s.arena));
  expect(set->Pool(2, t0 + 1, s.arena, &page) && page->type == tj::Type::kNull, "null page kept");
  // 256 live netuids: a 257th drops the oldest; re-remembering moves an
  // entry last.
  s.Reset();
  WorkingSet* bound = tb::NewWorkingSet();
  for (uint32_t n = 1; n <= 256; ++n) {
    bound->RememberPool(n, t0 + n, pool_page(s, n, "B"));
    s.Reset();
  }
  expect(bound->pool_count() == 256, "256 entries");
  bound->RememberPool(1, t0 + 300, pool_page(s, 1, "C"));  // 1 is now the newest
  bound->RememberPool(1000, t0 + 301, pool_page(s, 1000, "D"));
  expect(bound->pool_count() == 256, "still 256 after the 257th netuid");
  expect(!bound->Pool(2, t0 + 302, s.arena, &page), "the oldest (2) was dropped");
  expect(bound->Pool(1, t0 + 302, s.arena, &page) && symbol_is(page, "C"), "the refreshed entry stays");
  expect(bound->Pool(1000, t0 + 302, s.arena, &page), "the new entry is kept");
  // Expired entries are dropped before the bound applies.
  bound->RememberPool(2000, t0 + 15004, pool_page(s, 2000, "E"));
  expect(bound->pool_count() < 256, "expired entries dropped on remember");
  printf("working set: %zu entries retain %zu bytes\n", bound->pool_count(), bound->retained_bytes());
  // TAO/USD: one entry, a null price included.
  s.Reset();
  expect(!set->Price(t0, s.arena, &page), "no price yet");
  set->RememberPrice(t0, tj::Null(s.arena));
  expect(set->Price(t0 + 1, s.arena, &page) && page->type == tj::Type::kNull, "null price kept");
  set->RememberPrice(t0 + 2, tj::Num(s.arena, 412.5));
  expect(set->Price(t0 + 15001, s.arena, &page) && page->number == 412.5, "price for 15 s");
  expect(!set->Price(t0 + 15002, s.arena, &page), "price expires");
  // An oversized page (an absurd supply string) is not kept.
  s.Reset();
  char* body = static_cast<char*>(malloc(8192));
  int used = snprintf(body, 8192, "{\"data\":[{\"netuid\":9,\"total_alpha\":\"");
  for (int i = 0; i < 4000; ++i) body[used++] = '7';
  used += snprintf(body + used, 8192 - static_cast<size_t>(used), "\"}]}");
  tj::Value* big = tb::PoolPage(s.arena, tj::Parse(s.arena, reinterpret_cast<const uint8_t*>(body), static_cast<size_t>(used)), 9);
  free(body);
  expect(big && tj::IsString(tj::Get(big, "totalSupply")), "a 4,000-digit supply projects");
  set->RememberPool(9, t0, big);
  expect(!set->Pool(9, t0 + 1, s.arena, &page), "an oversized page is not kept");
  tb::DeleteWorkingSet(bound);
  tb::DeleteWorkingSet(set);
}

void formatting() {
  // Expected strings printed by Node 25 (V8 14.1, ICU 78).
  struct Case { double value; const char* text; };
  static const Case kCases[] = {
      {0, "0"}, {-0.0, "-0"}, {1234.5678, "1,234.5678"}, {5e-10, "0.000000001"}, {-1e-12, "-0"},
      {1e21, "1,000,000,000,000,000,000,000"}, {1.5e-7, "0.00000015"}, {123456789.12345679, "123,456,789.12345679"},
      {0.30000000000000004, "0.3"}, {1.0000000005, "1.000000001"}, {9.9999999995, "10"},
      {1.2345678901234566e25, "12,345,678,901,234,566,000,000,000"}, {2.5e-9, "0.000000003"}, {5e-324, "0"},
      {0.1234567895, "0.12345679"}, {4.35, "4.35"}, {0.0123456789, "0.012345679"},
      {1234567.890123456, "1,234,567.890123456"}, {-0.25, "-0.25"}, {999.9999999996, "1,000"},
  };
  for (const Case& c : kCases) {
    char out[448];
    const bool ok = tb::FormatEnUs(c.value, out, sizeof(out));
    if (!ok || strcmp(out, c.text) != 0) {
      ++failures;
      printf("FAIL: format %.17g -> %s (expected %s)\n", c.value, ok ? out : "!", c.text);
    }
  }
  char out[448];
  expect(tb::FormatEnUs(1.7976931348623157e308, out, sizeof(out)) && strlen(out) == 411, "largest double fits");
  expect(!tb::FormatEnUs(INFINITY, out, sizeof(out)), "infinity refused");
}

void netuids() {
  uint32_t n = 0;
  expect(tb::SubnetNetuid("1", &n) && n == 1, "1");
  expect(tb::SubnetNetuid("00001", &n) && n == 1, "00001");
  expect(tb::SubnetNetuid("65535", &n) && n == 65535, "65535");
  expect(!tb::SubnetNetuid("0", &n), "0 is root (TAO)");
  expect(!tb::SubnetNetuid("65536", &n), "65536");
  expect(!tb::SubnetNetuid("000001", &n), "six digits");
  expect(!tb::SubnetNetuid("1a", &n) && !tb::SubnetNetuid("", &n) && !tb::SubnetNetuid(" 1", &n), "non-digits");
  char path[128];
  expect(tb::PoolPath(65535, path, sizeof(path)) &&
             strcmp(path, "/api/dtao/pool/latest/v1?network=finney&netuid=65535&limit=1") == 0,
         "pool path");
}

}  // namespace

int main() {
  working_set();
  formatting();
  netuids();
  if (failures) {
    printf("token_bittensor_unit_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("token_bittensor_unit_test: ok\n");
  return 0;
}
