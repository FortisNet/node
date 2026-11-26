// Unit checks of the Robinhood token-page seams that the fixture replay
// cannot reach cheaply: StockClient over a FetchFn (the token canary's
// client; the read's lane priority reaches the fetch), the catalog shared
// with the price route's spot owner (FreshCatalog / AdoptCatalog: expiry on
// the client clock, a repeated contract keeps its last row, an over-long
// symbol is skipped, a failed parse keeps the previous catalog, an oversized
// catalog is kept for the page but does not flatten) and the token_robinhood
// projections. Usage: token_robinhood_unit_test
#include "fake_seams.h"
#include "robinhood_markets.h"
#include "robinhood_price.h"
#include "token_robinhood.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::providers;

namespace rh = fortis::exchange::robinhood;
namespace tr = fortis::tokens::token_robinhood;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  ++failures;
  printf("FAIL: %s\n", what);
}

int64_t g_now = 1768176000000;
fortis::testing::PointerClock g_clock(&g_now);

struct Fake final : rh::StockClient::Fetcher {
  const char* body = nullptr;
  int calls = 0;
  bool background = false;
  rh::Outcome Fetch(tj::Arena& arena, uint64_t deadline_ms, const char* path, bool background,
                    tj::Value** body) override;
};

rh::Outcome Fake::Fetch(tj::Arena& arena, uint64_t, const char*, bool background, tj::Value** body) {
  Fake* fake = this;
  ++fake->calls;
  fake->background = background;
  rh::Outcome outcome;
  *body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(fake->body), strlen(fake->body));
  if (!*body) outcome.failure = rh::Failure::kError;
  return outcome;
}

// An asset row; `symbol` and `multiplier` vary.
void asset(char* out, size_t capacity, int id, const char* symbol, const char* contract, const char* multiplier) {
  snprintf(out, capacity,
           "{\"id\":\"0x%064x\",\"tokenSymbol\":\"%s\",\"tokenName\":\"Name %d\",\"deployments\":"
           "[{\"chainId\":4663,\"contractAddress\":\"%s\"}],\"currentMultiplier\":\"%s\",\"pendingMultiplier\":\"\","
           "\"logoUrl\":\"https://cdn.example/%d.png\",\"status\":\"ASSET_STATUS_ACTIVE\"}",
           id, symbol, id, contract, multiplier, id);
}

const RobinhoodAsset* find(const RobinhoodAssetCatalog& catalog, const char* address) {
  for (size_t i = 0; i < catalog.count; ++i)
    if (strcmp(catalog.assets[i].address, address) == 0) return &catalog.assets[i];
  return nullptr;
}

uint64_t far_deadline() { return static_cast<uint64_t>(-1) / 2; }

}  // namespace

int main() {
  const char* upper = "0xABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD";
  const char* lower = "0xabcdefabcdefabcdefabcdefabcdefabcdefabcd";
  char rows[4][512];
  // Sorted by symbol: AAA then ZZZ share a contract (the later row, ZZZ,
  // is the price route's); a 17 x U+03A9 symbol (34 bytes, 17 UTF-16 units)
  // is listed but too long for the flat record.
  asset(rows[0], sizeof(rows[0]), 1, "ZZZ", upper, "3");
  asset(rows[1], sizeof(rows[1]), 2, "AAA", upper, "2");
  asset(rows[2], sizeof(rows[2]), 3, "ΩΩΩΩΩΩΩΩΩΩΩΩΩΩ"
        "ΩΩΩ", "0x1111111111111111111111111111111111111111", "1");
  asset(rows[3], sizeof(rows[3]), 4, "TSLA", "0x2222222222222222222222222222222222222222", "0.5");
  char catalog[4096];
  snprintf(catalog, sizeof(catalog), "{\"assets\":[%s,%s,%s,%s]}", rows[0], rows[1], rows[2], rows[3]);

  Fake fake;
  fake.body = catalog;
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  RobinhoodAssetCatalog flat{};
  expect(!client.FreshCatalog(&flat), "no catalog before the first read");

  // The page reads the catalog (background revalidation priority passes through).
  std::shared_ptr<const rh::StockClient::Snapshot> held;
  expect(client.Assets(far_deadline(), &held, true).ok() && fake.calls == 1 && fake.background,
         "Assets reads through the FetchFn at the read's priority");
  expect(rh::StockClient::Document(held.get())->count == 4, "the parsed catalog keeps every listed row");
  held.reset();
  expect(client.FreshCatalog(&flat) && flat.count == 2, "the flat catalog: one row per contract, long symbol skipped");
  const RobinhoodAsset* dup = find(flat, lower);
  expect(dup && strcmp(dup->symbol, "ZZZ") == 0 && dup->multiplier == 3, "a repeated contract keeps its last row");
  expect(find(flat, "0x1111111111111111111111111111111111111111") == nullptr, "an over-long symbol is skipped");
  const RobinhoodAsset* tsla = find(flat, "0x2222222222222222222222222222222222222222");
  expect(tsla && tsla->multiplier == 0.5, "multiplier");

  // Expiry on the client clock; the page's next read fetches again.
  g_now += 15 * 60000;
  expect(!client.FreshCatalog(&flat), "the catalog expires after 15 minutes");

  // The spot owner adopts a body it fetched itself: the page then reads it.
  char second[2048];
  snprintf(second, sizeof(second), "{\"assets\":[%s]}", rows[3]);
  expect(client.AdoptCatalog(reinterpret_cast<const uint8_t*>(second), strlen(second), &flat) && flat.count == 1,
         "AdoptCatalog flattens what it keeps");
  held = nullptr;
  expect(client.Assets(far_deadline(), &held).ok() && fake.calls == 1 &&
             rh::StockClient::Document(held.get())->count == 1,
         "the page reads the adopted catalog without a request");
  held.reset();

  // A body parseAssets rejects (or invalid JSON) is not adopted.
  const char* bad[] = {"{\"assets\":\"none\"}", "null", "<html>", "[]"};
  for (const char* body : bad)
    expect(!client.AdoptCatalog(reinterpret_cast<const uint8_t*>(body), strlen(body), &flat), "bad body refused");
  expect(client.FreshCatalog(&flat) && flat.count == 1, "a refused body keeps the previous catalog");

  // More contracts than the flat record holds: kept for the page, not for
  // the price route.
  const size_t big_capacity = (kRobinhoodMaxAssets + 1) * 512 + 64;
  char* big = static_cast<char*>(malloc(big_capacity));
  size_t used = static_cast<size_t>(snprintf(big, big_capacity, "{\"assets\":["));
  for (size_t i = 0; i <= kRobinhoodMaxAssets; ++i) {
    char contract[43];
    snprintf(contract, sizeof(contract), "0x%040zx", i + 1);
    char sym[16];
    snprintf(sym, sizeof(sym), "S%zu", i);
    char row[512];
    asset(row, sizeof(row), static_cast<int>(i + 10), sym, contract, "1");
    used += static_cast<size_t>(snprintf(big + used, big_capacity - used, "%s%s", i ? "," : "", row));
  }
  used += static_cast<size_t>(snprintf(big + used, big_capacity - used, "]}"));
  expect(!client.AdoptCatalog(reinterpret_cast<const uint8_t*>(big), used, &flat), "an oversized catalog does not flatten");
  held = nullptr;
  expect(client.Assets(far_deadline(), &held).ok() && fake.calls == 1 &&
             rh::StockClient::Document(held.get())->count == kRobinhoodMaxAssets + 1,
         "the page still reads the oversized catalog");
  held.reset();
  free(big);

  // The projections over one asset and its quote.
  uint8_t bytes[1 << 16];
  tj::Arena arena{bytes, sizeof(bytes), 0, true};
  const tj::Value* listed = tj::Parse(arena, reinterpret_cast<const uint8_t*>(rows[3]), strlen(rows[3]));
  tj::Value* parsed_assets = nullptr;
  {
    char one[1024];
    snprintf(one, sizeof(one), "{\"assets\":[%s]}", rows[3]);
    parsed_assets = rh::ParseAssets(arena, tj::Parse(arena, reinterpret_cast<const uint8_t*>(one), strlen(one)));
  }
  expect(listed && parsed_assets && parsed_assets->count == 1, "ParseAssets");
  const tj::Value* row = parsed_assets->items[0];
  const char* quote_text = "{\"midpointUsd\":201}";
  const tj::Value* quote = tj::Parse(arena, reinterpret_cast<const uint8_t*>(quote_text), strlen(quote_text));
  auto json = [&](const tj::Value* value) {
    static char out[2048];
    tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(out), sizeof(out) - 1, 0, true};
    tj::Stringify(value, buffer);
    out[buffer.used] = '\0';
    return out;
  };
  expect(strcmp(json(tr::MarketRow(arena, row, quote)),
                "{\"symbol\":\"TSLA\",\"name\":\"Name 4\",\"current_price\":201,\"total_volume\":null,"
                "\"high_24h\":null,\"low_24h\":null}") == 0,
         "market row");
  const char* meta = json(tr::ChainMeta(arena, row, nullptr, g_now));
  const char* meta_prefix = "{\"symbol\":\"TSLA\",\"name\":\"Name 4\",\"decimals\":18,\"verified\":true,"
                            "\"holderCount\":null,\"logoMediaKey\":\"";
  expect(strncmp(meta, meta_prefix, strlen(meta_prefix)) == 0 && strlen(meta) == strlen(meta_prefix) + 34,
         "chain meta");
  const char* profile = json(tr::Profile(arena, row, nullptr, g_now));
  expect(strstr(profile, "{\"description\":\"Name 4 is an issuer-registered tokenized debt security") == profile &&
             strstr(profile, "\"categories\":[\"Tokenized security\",\"Stock Token\"],\"links\":{\"homepage\":"
                             "\"https://docs.robinhood.com/chain/stock-tokens\"},\"logoMediaKey\":\"") &&
             strstr(profile, "\"marketsCount\":null,\"sentimentUpPct\":null}"),
         "profile");

  if (failures) {
    printf("token_robinhood_unit_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("token_robinhood_unit_test: ok\n");
  return 0;
}
