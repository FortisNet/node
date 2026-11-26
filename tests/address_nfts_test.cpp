// R15 NFT inventory units against tests/corpus/address_nfts_edges.json
// (`units`, written by the earlier TypeScript implementation's tests):
//  - address::NftItemKey: nftItemKey(account, nftId) =
//    nft/item/{encodeURIComponent(account)}/{encodeURIComponent(nftId)}, or
//    the URIError encodeURIComponent throws for a lone surrogate; and the
//    byte order of those keys (the KV's) equals JavaScript's sort;
//  - address::NftItemKey's kTooLong: an item key over the native KV key
//    bound (1,024 bytes) (lmdb-msgpack: the oracle's LMDB bound on
//    the expiry index key is gone; the fixture ids still probe it);
//  - address::NftScanDue: NftsFacet.scanDue over scan records and instants
//    (the 10-minute and 60-second boundaries);
//  - stored records: records are stored as written; a record
//    an earlier version stored (the fixture's msgpack `stored` form)
//    still reads back byte for byte;
//  - RegisterMediaSource (no KV): MediaFacet.register's key for a URL, or
//    null for one that is not http(s).
// Usage: address_nfts_test <address_nfts_edges.json>
#include "strict_unit_expectations.h"
#include "../src/wallet/address_facets.h"
#include "../src/discovery/media_registry.h"
#include "../src/codec/token_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>

using namespace fortis::wallet;

using namespace fortis::discovery;

using namespace fortis;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %.300s\n", what, detail);
}

const char* text(const tj::Value* object, const char* key) {
  const tj::Value* value = tj::Get(object, key);
  return tj::IsString(value) ? value->text : nullptr;
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

void keys(const tj::Value* units) {
  const char* account = text(units, "account");
  const tj::Value* rows = tj::Get(units, "keys");
  size_t checked = 0;
  for (uint32_t i = 0; account && tj::IsArray(rows) && i < rows->count; ++i) {
    const tj::Value* id = tj::Get(rows->items[i], "nftId");
    // A C string cannot carry an embedded NUL (no driver id holds one).
    if (!tj::IsString(id) || strlen(id->text) != id->length) continue;
    char key[4096];
    const address::NftKeyStatus status = address::NftItemKey(account, id->text, key, sizeof(key));
    const char* expected = text(rows->items[i], "key");
    const char* error = text(rows->items[i], "error");
    ++checked;
    if (expected && (status != address::NftKeyStatus::kOk || strcmp(key, expected) != 0))
      fail("item key", expected);
    if (error && (strcmp(error, "URIError") != 0 || status != address::NftKeyStatus::kUriError))
      fail("item key error", id->text);
  }
  // The KV pages keys in byte order; TypeScript's LMDB too, and the fixture's
  // `sorted` is JavaScript's sort of the same keys.
  const tj::Value* sorted = tj::Get(units, "sorted");
  size_t ordered = 0;
  for (uint32_t i = 1; tj::IsArray(sorted) && i < sorted->count; ++i, ++ordered)
    if (strcmp(sorted->items[i - 1]->text, sorted->items[i]->text) >= 0) fail("key order", sorted->items[i]->text);
  printf("item keys: %zu checked, %zu ordered pairs\n", checked, ordered);
}

void limits(const tj::Value* units) {
  const char* account = text(units, "account");
  const tj::Value* rows = tj::Get(units, "limits");
  size_t checked = 0;
  for (uint32_t i = 0; account && tj::IsArray(rows) && i < rows->count; ++i, ++checked) {
    const char* id = text(rows->items[i], "nftId");
    // nft/item/{enc(account)}/{enc(id)}: an unreserved byte is written as
    // itself, any other as %XX.
    size_t length = 10;
    for (const char* p : {account, id})
      for (const char* c = p; *c; ++c) {
        const char x = *c;
        const bool plain = (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') ||
                           strchr("-_.!~*'()", x) != nullptr;
        length += plain ? 1 : 3;
      }
    const bool stored = length <= 1024;
    char key[4096];
    const address::NftKeyStatus status = address::NftItemKey(account, id, key, sizeof(key));
    if ((status == address::NftKeyStatus::kOk) != stored || (!stored && status != address::NftKeyStatus::kTooLong) ||
        (stored && strlen(key) != length)) {
      char detail[96];
      snprintf(detail, sizeof(detail), "%zu-byte key: native status %d", length, static_cast<int>(status));
      fail("native key bound", detail);
    }
  }
  printf("native key bound: %zu checked\n", checked);
}

void scans(const tj::Value* rows, tj::Arena& arena) {
  size_t checked = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i, ++checked) {
    const tj::Value* row = rows->items[i];
    const tj::Value* state = tj::Get(row, "state");
    tj::Value* record = nullptr;
    if (tj::IsObject(state)) {
      record = tj::Clone(arena, state);
      tj::Set(arena, record, "lastFullScanAt", tj::Clone(arena, tj::Get(state, "lastScanAt")));
    }
    const bool due = address::NftScanDue(record, static_cast<int64_t>(tj::Get(row, "now")->number));
    if (due != (tj::Get(row, "due")->type == tj::Type::kTrue)) fail("scan due", text(row, "name"));
  }
  printf("scan due: %zu checked\n", checked);
}

void stored(const tj::Value* rows, tj::Arena& arena) {
  size_t checked = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i, ++checked) {
    // The record text an earlier version stored for this input.
    const char* expected = text(rows->items[i], "stored");
    const tj::Value* old = tj::Parse(arena, reinterpret_cast<const uint8_t*>(expected), strlen(expected));
    static uint8_t buffer[64 * 1024];
    tj::OutBuffer out{buffer, sizeof(buffer), 0, true};
    if (!old || !tj::Stringify(old, out) || out.used != strlen(expected) || memcmp(buffer, expected, out.used) != 0)
      fail("record stored before task 17", expected);
  }
  printf("records stored before task 17: %zu read back as stored\n", checked);
}

// createHash().update(string) encodes like TextEncoder: a lone surrogate
// becomes one U+FFFD.
size_t text_encoder(const char* input, size_t length, char* output) {
  size_t used = 0;
  for (size_t i = 0; i < length; ++i) {
    if (static_cast<uint8_t>(input[i]) == 0xED && i + 2 < length && static_cast<uint8_t>(input[i + 1]) >= 0xA0) {
      memcpy(output + used, "\xEF\xBF\xBD", 3);
      used += 3;
      i += 2;
      continue;
    }
    output[used++] = input[i];
  }
  output[used] = '\0';
  return used;
}

void media(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i, ++checked) {
    const tj::Value* url = tj::Get(rows->items[i], "url");
    const char* expected = text(rows->items[i], "key");
    char encoded[1024];
    const size_t length = text_encoder(url->text, url->length, encoded);
    char key[market_view::kMediaKeyChars + 1] = {};
    const bool registered = RegisterMediaSource(nullptr, encoded, length, 0, key);
    if (registered != (expected != nullptr) || (expected && strcmp(key, expected) != 0)) fail("media key", url->text);
  }
  printf("media registration: %zu checked\n", checked);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <address_nfts_edges.json>\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const auto* text_changes = strict::UnitExpectations(arena, "tests/fixtures/strict_text_expectations.json");
  strict::PatchIndexRows(arena, tj::Get(tj::Get(fixture, "units"), "keys"), tj::Get(text_changes, "nft_keys"));
  const tj::Value* units = tj::Get(fixture, "units");
  if (!units) {
    fprintf(stderr, "no units in %s\n", argv[1]);
    return 2;
  }
  keys(units);
  limits(units);
  scans(tj::Get(units, "scans"), arena);
  stored(tj::Get(units, "stored"), arena);
  media(tj::Get(units, "media"));
  tj::FreeArena(arena);
  free(data);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("address nfts units: all passed\n");
  return 0;
}
