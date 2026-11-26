#include "robinhood_price.h"
#include "bounded_json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::providers;

int main() {
  BoundedJsonToken tokens[kBoundedJsonMaxTokens]{};
  const char assets[] = R"({"assets":[{"id":"0xabababababababababababababababababababababababababababababababab","tokenSymbol":"BRK.B/US","tokenName":"Stock","currentMultiplier":"0.5","deployments":[{"chainId":1,"contractAddress":"0x1111111111111111111111111111111111111111"},{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]},{"id":"0xcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd","tokenSymbol":"BAD","tokenName":"Bad decimal","currentMultiplier":".5","deployments":[{"chainId":4663,"contractAddress":"0x2222222222222222222222222222222222222222"}]},{"id":"0xefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef","tokenSymbol":"DUP","tokenName":"Duplicate","currentMultiplier":"3","deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]}]})";
  RobinhoodAssetCatalog catalog{};
  assert(ExtractRobinhoodAssets(reinterpret_cast<const unsigned char*>(assets), strlen(assets), tokens, kBoundedJsonMaxTokens, &catalog) == RobinhoodError::kNone);
  assert(catalog.count == 1 && strcmp(catalog.assets[0].symbol, "BRK.B/US") == 0 && catalog.assets[0].multiplier == 0.5);
  char path[kRobinhoodPricePathBytes]{};
  assert(BuildRobinhoodPricePath("BRK.B/US", path, sizeof(path)));
  assert(strcmp(path, "/rhj/prices/BRK.B%2FUS") == 0);
  const char quote[] = R"({"quotes":[{"tokenSymbol":"BRK.B/US","currency":"USD","bid":"200","ask":204,"generatedAt":"2026-01-02T03:04:04Z","isTradingHalt":true,"deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]}]})";
  double midpoint = 0;
  assert(ExtractRobinhoodQuote(reinterpret_cast<const unsigned char*>(quote), strlen(quote), catalog.assets[0], tokens, kBoundedJsonMaxTokens, &midpoint) == RobinhoodError::kNone);
  assert(midpoint == 101);
  const char non_usd[] = R"({"quotes":[{"tokenSymbol":"BRK.B/US","currency":"EUR","bid":200,"ask":204,"generatedAt":"2026-01-02T03:04:04Z","deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]}]})";
  assert(ExtractRobinhoodQuote(reinterpret_cast<const unsigned char*>(non_usd), strlen(non_usd), catalog.assets[0], tokens, kBoundedJsonMaxTokens, &midpoint) == RobinhoodError::kInvalidQuote);
  const char boolean_bid[] = R"({"quotes":[{"tokenSymbol":"BRK.B/US","currency":"USD","bid":true,"ask":204,"generatedAt":"2026-01-02T03:04:04Z","deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]}]})";
  assert(ExtractRobinhoodQuote(reinterpret_cast<const unsigned char*>(boolean_bid), strlen(boolean_bid), catalog.assets[0], tokens, kBoundedJsonMaxTokens, &midpoint) == RobinhoodError::kInvalidQuote);
  const char date_parse_but_not_rfc3339[] = R"({"quotes":[{"tokenSymbol":"BRK.B/US","currency":"USD","bid":200,"ask":204,"generatedAt":"Jan 2, 2026","deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]}]})";
  assert(ExtractRobinhoodQuote(reinterpret_cast<const unsigned char*>(date_parse_but_not_rfc3339), strlen(date_parse_but_not_rfc3339), catalog.assets[0], tokens, kBoundedJsonMaxTokens, &midpoint) == RobinhoodError::kInvalidQuote);
  const char bad_first_date[] = R"({"quotes":[{"tokenSymbol":"BRK.B/US","currency":"USD","bid":200,"ask":204,"generatedAt":"bad","deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]},{"tokenSymbol":"BRK.B/US","currency":"USD","bid":200,"ask":204,"generatedAt":"2026-01-02T03:04:04Z","deployments":[{"chainId":4663,"contractAddress":"0x1111111111111111111111111111111111111111"}]}]})";
  assert(ExtractRobinhoodQuote(reinterpret_cast<const unsigned char*>(bad_first_date), strlen(bad_first_date), catalog.assets[0], tokens, kBoundedJsonMaxTokens, &midpoint) == RobinhoodError::kInvalidQuote);
  puts("native Robinhood parser and request tests passed");
}
