#include <memory>
// R18 network overview units against
// tests/corpus/network_overview_edges.json (`units`, written by
// the earlier TypeScript implementation's tests):
//  - address::OverviewTokenAddress: overviewTokenAddress(network, raw) for
//    every family the popular-token facet serves (and two it does not), and
//    the token id the facet builds from it (overviewTokenId);
//  - address::OverviewIsNetworkNews: isNetworkNews(candidate, profile) for
//    each candidate headline against fifteen network profiles;
//  - address::OverviewProjectTrending: GeckoTerminalClient.trending's
//    projection of a trending_pools body, compared as JSON.stringify text;
//  - address::OverviewProtobuf: the NetworkOverviewResponse bytes protobuf-es
//    writes for a crafted payload, or its refusal (a holderCount that
//    BigInt() or the uint64 writer rejects).
// Usage: network_overview_test <network_overview_edges.json>
#include "../src/wallet/address_overview.h"
#include "../src/chains/chain_core.h"
#include "../src/codec/token_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::wallet;

using namespace fortis;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %.600s\n", what, detail);
}

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (data && fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    free(data);
    data = nullptr;
  }
  fclose(file);
  if (data) data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

struct Family {
  const char* id;
  chains::Family family;
  const char* prefix;
};
constexpr Family kFamilies[] = {
    {"eth-mainnet", chains::Family::kEvm, "erc20:"}, {"base", chains::Family::kEvm, "erc20:"},
    {"solana", chains::Family::kSolana, "spl:"},     {"tron", chains::Family::kTron, "trc20:"},
    {"algorand", chains::Family::kAlgorand, "asa:"}, {"cardano", chains::Family::kCardano, "cardano:"},
    {"aptos", chains::Family::kAptos, "aptos:"},     {"bitcoin", chains::Family::kBitcoin, nullptr},
    {"sui", chains::Family::kSui, nullptr},
};

bool network_for(const char* id, chains::Network* out, const char** prefix) {
  for (const Family& f : kFamilies) {
    if (strcmp(f.id, id) != 0) continue;
    *out = chains::Network{};
    snprintf(out->id, sizeof(out->id), "%s", id);
    out->family = f.family;
    if (prefix) *prefix = f.prefix;
    return true;
  }
  return false;
}

bool same(const tj::Value* a, const tj::Value* b) {
  return a && b && a->length == b->length && !memcmp(a->text, b->text, a->length);
}

void addresses(const tj::Value* groups, size_t* checked) {
  for (uint32_t g = 0; g < groups->count; ++g) {
    const tj::Value* group = groups->items[g];
    chains::Network network;
    const char* prefix = nullptr;
    const tj::Value* id = tj::Get(group, "network");
    if (!network_for(id->text, &network, &prefix)) { fail("addresses", id->text); continue; }
    const tj::Value* rows = tj::Get(group, "rows");
    for (uint32_t i = 0; i < rows->count; ++i) {
      tj::Arena arena = tj::GrowingArena(1 << 20);
      const tj::Value* input = tj::Get(rows->items[i], "input");
      const tj::Value* want = tj::Get(rows->items[i], "address");
      const tj::Value* want_id = tj::Get(rows->items[i], "tokenId");
      tj::Value* got = address::OverviewTokenAddress(arena, network, input->text, input->length);
      char label[160];
      snprintf(label, sizeof(label), "address %s[%u] %.40s", network.id, i, input->text);
      if (tj::IsString(want) != (got != nullptr) || (got && !same(got, want))) {
        fail(label, got ? got->text : "null");
      } else if (got) {
        // overviewTokenId: the family prefix + the canonical address.
        const size_t n = strlen(prefix);
        const bool id_ok = tj::IsString(want_id) && want_id->length == n + got->length &&
                           !memcmp(want_id->text, prefix, n) && !memcmp(want_id->text + n, got->text, got->length);
        if (!id_ok) fail(label, "token id");
      } else if (tj::IsString(want_id)) {
        fail(label, "token id without an address");
      }
      ++*checked;
      tj::FreeArena(arena);
    }
  }
}

void news(const tj::Value* rows, size_t* checked) {
  static const char* const kProfiles[] = {"eth-mainnet", "base", "bnb", "xrpl", "avalanche", "cosmos", "optimism", "sei",
                                          "robinhood", "bitcoin-cash", "bitcoin", "solana", "icp", "hyperevm", "tron"};
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* matches = tj::Get(row, "matches");
    for (const char* profile : kProfiles) {
      bool want = false;
      for (uint32_t m = 0; m < matches->count; ++m) want = want || tj::StringEquals(matches->items[m], profile);
      if (address::OverviewIsNetworkNews(row, profile) != want) {
        char label[200];
        snprintf(label, sizeof(label), "news[%u] %s", i, profile);
        fail(label, tj::Get(row, "headline")->text);
      }
      ++*checked;
    }
  }
}

void trending(const tj::Value* rows, size_t* checked) {
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    tj::Arena arena = tj::GrowingArena(4 << 20);
    chains::Network network;
    const tj::Value* id = tj::Get(row, "network");
    const tj::Value* text = tj::Get(row, "text");
    const tj::Value* output = tj::Get(row, "output");
    char label[64];
    snprintf(label, sizeof(label), "trending[%u] %s", i, id->text);
    const tj::Value* body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
    tj::Value* out = nullptr;
    if (!network_for(id->text, &network, nullptr) || !body ||
        !address::OverviewProjectTrending(network, body, arena, &out)) {
      fail(label, "projection failed");
    } else {
      static uint8_t buffer[1 << 20];
      tj::OutBuffer json{buffer, sizeof(buffer), 0, true};
      if (!tj::Stringify(out, json) || !json.ok || !tj::IsString(output) || json.used != output->length ||
          memcmp(buffer, output->text, json.used) != 0) {
        buffer[json.used < sizeof(buffer) ? json.used : sizeof(buffer) - 1] = 0;
        fail(label, reinterpret_cast<const char*>(buffer));
      }
    }
    ++*checked;
    tj::FreeArena(arena);
  }
}

int hex_digit(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

void protobuf(const tj::Value* rows, size_t* checked) {
  const size_t capacity = 4 << 20;
  auto buffer_owner = std::make_unique<uint8_t[]>(capacity);
  uint8_t* buffer = buffer_owner.get();
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    tj::Arena arena = tj::GrowingArena(8 << 20);
    const tj::Value* name = tj::Get(row, "name");
    const tj::Value* text = tj::Get(row, "payload");
    const tj::Value* wire = tj::Get(row, "wireHex");
    const tj::Value* payload = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
    tj::OutBuffer out{buffer, capacity, 0, true};
    bool refused = false;
    const bool ok = payload && address::OverviewProtobuf(payload, out, &refused);
    char label[96];
    snprintf(label, sizeof(label), "protobuf %s", name->text);
    if (!tj::IsString(wire)) {
      if (ok || !refused) fail(label, "TypeScript refuses this payload");
    } else if (!ok) {
      fail(label, refused ? "refused" : "encoding failed");
    } else {
      bool equal = out.used * 2 == wire->length;
      for (size_t b = 0; equal && b < out.used; ++b)
        equal = buffer[b] == (hex_digit(wire->text[2 * b]) << 4 | hex_digit(wire->text[2 * b + 1]));
      if (!equal) fail(label, "bytes differ");
    }
    // One byte short of the encoding is refused, never truncated.
    if (ok && out.used) {
      tj::OutBuffer small{buffer, out.used - 1, 0, true};
      if (address::OverviewProtobuf(payload, small) && small.ok) fail(label, "accepted a short buffer");
    }
    ++*checked;
    tj::FreeArena(arena);
  }

}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <network_overview_edges.json>\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* text = read_file(argv[1], &length);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(512u << 20);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  const tj::Value* units = tj::Get(fixture, "units");
  if (!units) {
    fprintf(stderr, "no units in %s\n", argv[1]);
    return 2;
  }
  size_t counts[4] = {0, 0, 0, 0};
  addresses(tj::Get(units, "addresses"), &counts[0]);
  news(tj::Get(units, "news"), &counts[1]);
  trending(tj::Get(units, "trending"), &counts[2]);
  protobuf(tj::Get(units, "protobuf"), &counts[3]);
  printf("network overview units: addresses %zu, news %zu, trending %zu, protobuf %zu; %d failure(s)\n", counts[0],
         counts[1], counts[2], counts[3], failures);
  tj::FreeArena(arena);
  free(text);
  return failures ? 1 : 0;
}
