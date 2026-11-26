#include "test_support.h"
// Unit checks of GET /v1/media/{key} (R08) below the HTTP layer
// (src/news/media_service.cpp), against the TypeScript oracle
// media_get_edges.json (its `unit` member):
//  - MediaAddressBlocked against isBlockedAddress over a grid of
//    IPv4 addresses (every first octet x boundary second/third octets). The
//    asserted difference: the four special-use /24s the outbound transport
//    refuses for every caller (192.88.99, 192.175.48, 192.31.196, 192.52.193)
//    are blocked natively only;
//  - SniffImageType against sniffImageType (every signature truncated at
//    each length, near misses);
//  - MediaKeyValid against /^[0-9a-f]{32}$/, MediaKeyFor against mediaKeyFor;
//  - MediaResolveLocation + MediaRequestTarget against
//    new URL(location, base) and pinnedFetch's pathname + search, and
//    MediaUrlAllowed against checkPublicUrl. Asserted difference: an IPv6
//    literal is refused natively (TypeScript allows a public one);
//  - MediaClassifyHop: fetchAndStore's decision table, in its order (3xx
//    first, the declared length before the status, the streamed cap only for
//    2xx, 4xx permanent, other non-2xx/transport faults transient, guard
//    refusals permanent, magic bytes last). The route replay
//    (tests/media_get_route_test.py) checks each row end to end;
//  - bounds of the request target and Location buffers.
// Usage: media_get_test media_get_edges.json
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "market_view.h"
#include "media_service.h"
#include "token_json.h"
#include "whatwg_url.h"

using namespace fortis::http;
using namespace fortis::news;

namespace {

namespace tj = fortis::codec::tj;
namespace url = fortis::compat::url;

int g_failures = 0;
int g_checks = 0;
void check(bool condition, const char* what, const char* detail = "") {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s %s\n", what, detail);
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

size_t from_hex(const char* hex, unsigned char* out, size_t capacity) {
  const size_t length = strlen(hex) / 2;
  if (length > capacity) return 0;
  for (size_t i = 0; i < length; ++i) {
    unsigned value = 0;
    if (!fortis::test::ParseHexByte(hex + 2 * i, &value)) throw std::runtime_error("invalid hex in test fixture");
    out[i] = static_cast<unsigned char>(value);
  }
  return length;
}

const char* text_of(const tj::Value* value) { return tj::IsString(value) ? value->text : nullptr; }
bool is_true(const tj::Value* value) { return value && value->type == tj::Type::kTrue; }

bool native_extra_block(uint32_t ip) {
  const unsigned a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
  return a == 192 && ((b == 88 && c == 99) || (b == 175 && c == 48) || (b == 31 && c == 196) ||
                      (b == 52 && c == 193));
}

void blocked_grid(const tj::Value* grid) {
  const tj::Value* bs = tj::Get(grid, "b");
  const tj::Value* cs = tj::Get(grid, "c");
  const char* flags = text_of(tj::Get(grid, "flags"));
  size_t index = 0, differences = 0, blocked = 0;
  for (unsigned a = 0; a < 256; ++a) {
    for (uint32_t i = 0; i < bs->count; ++i) {
      for (uint32_t j = 0; j < cs->count; ++j, ++index) {
        const uint32_t ip = a << 24 | static_cast<uint32_t>(bs->items[i]->number) << 16 |
                            static_cast<uint32_t>(cs->items[j]->number) << 8 | 1;
        const bool expected = flags[index] == '1';
        const bool actual = MediaAddressBlocked(ip);
        blocked += expected;
        if (native_extra_block(ip)) {
          ++differences;
          check(!expected && actual, "native extra block");
          continue;
        }
        char detail[64];
        snprintf(detail, sizeof(detail), "%u.%u.%u.1", a, ip >> 16 & 255, ip >> 8 & 255);
        check(actual == expected, "isBlockedAddress", detail);
      }
    }
  }
  check(index == strlen(flags), "grid size");
  printf("  blocked grid: %zu IPv4 addresses (%zu blocked in TypeScript), %zu asserted native-only "
         "special-use blocks\n", index, blocked, differences);
}

void sniff(const tj::Value* list) {
  unsigned char bytes[128];
  for (uint32_t i = 0; i < list->count; ++i) {
    const char* hex = text_of(tj::Get(list->items[i], "hex"));
    const size_t length = from_hex(hex, bytes, sizeof(bytes));
    const char* expected = text_of(tj::Get(list->items[i], "type"));
    const char* actual = SniffImageType(length ? bytes : reinterpret_cast<const unsigned char*>(""),
                                        length);
    check(expected ? actual && strcmp(actual, expected) == 0 : actual == nullptr, "sniffImageType",
          hex);
  }
  printf("  sniffImageType: %u byte strings\n", list->count);
}

void keys(const tj::Value* list, const tj::Value* media_keys) {
  for (uint32_t i = 0; i < list->count; ++i) {
    const tj::Value* key = tj::Get(list->items[i], "key");
    // A key with a NUL cannot reach the handler as a C string; none here.
    check(MediaKeyValid(key->text) == is_true(tj::Get(list->items[i], "valid")), "key grammar",
          key->text);
  }
  check(!MediaKeyValid(nullptr), "null key");
  for (uint32_t i = 0; i < media_keys->count; ++i) {
    const tj::Value* source = tj::Get(media_keys->items[i], "url");
    char key[fortis::discovery::market_view::kMediaKeyChars + 1];
    fortis::discovery::market_view::MediaKeyFor(source->text, source->length, key);
    check(strcmp(key, text_of(tj::Get(media_keys->items[i], "key"))) == 0, "mediaKeyFor", source->text);
  }
  printf("  key grammar: %u strings; mediaKeyFor: %u URLs\n", list->count, media_keys->count);
}

void resolutions(const tj::Value* list) {
  url::UrlScratch* scratch = static_cast<url::UrlScratch*>(malloc(sizeof(url::UrlScratch)));
  url::Url* base = static_cast<url::Url*>(malloc(sizeof(url::Url)));
  url::Url* next = static_cast<url::Url*>(malloc(sizeof(url::Url)));
  size_t refused_v6 = 0, allowed = 0, refused = 0, errors = 0;
  for (uint32_t i = 0; i < list->count; ++i) {
    const tj::Value* entry = list->items[i];
    const char* base_text = text_of(tj::Get(entry, "base"));
    unsigned char location[2048];
    const size_t length = from_hex(text_of(tj::Get(entry, "locationHex")), location, sizeof(location) - 1);
    location[length] = '\0';
    char detail[4096];
    snprintf(detail, sizeof(detail), "%s + %s", base_text, reinterpret_cast<char*>(location));
    check(url::Parse(base_text, strlen(base_text), nullptr, base, scratch), "base", base_text);
    const bool parsed = MediaResolveLocation(reinterpret_cast<char*>(location), *base, next, scratch);
    if (is_true(tj::Get(entry, "error"))) {
      ++errors;
      check(!parsed, "new URL throws", detail);
      continue;
    }
    check(parsed, "new URL parses", detail);
    if (!parsed) continue;
    const url::View href = url::Href(*next);
    const char* expected_href = text_of(tj::Get(entry, "href"));
    check(href.length == strlen(expected_href) && memcmp(href.data, expected_href, href.length) == 0,
          "href", detail);
    char target[8192];
    check(MediaRequestTarget(*next, target, sizeof(target)) &&
              strcmp(target, text_of(tj::Get(entry, "target"))) == 0,
          "pinnedFetch target", detail);
    const bool expected_allowed = is_true(tj::Get(entry, "allowed"));
    const bool actual = MediaUrlAllowed(*next);
    if (next->host_type == url::HostType::kIpv6) {
      ++refused_v6;
      check(!actual, "IPv6 literal refused natively", detail);
      continue;
    }
    expected_allowed ? ++allowed : ++refused;
    check(actual == expected_allowed, "checkPublicUrl", detail);
  }
  printf("  redirect resolution: %u (base, Location) pairs: %zu throw, %zu allowed, %zu refused, "
         "%zu IPv6 literals refused natively\n", list->count, errors, allowed, refused, refused_v6);

  // Bounds: the request target must fit with its NUL; the Location buffer
  // holds a 1023-byte all-Latin-1 value (2046 UTF-8 bytes).
  const char* plain = "https://cdn.example/abc?d";
  check(url::Parse(plain, strlen(plain), nullptr, base, scratch), "bound base");
  char small[8];
  check(MediaRequestTarget(*base, small, 7) && strcmp(small, "/abc?d") == 0, "target exact fit");
  check(!MediaRequestTarget(*base, small, 6), "target one byte short");
  char wide[1024];
  wide[0] = '/';
  memset(wide + 1, 0xff, 1022);
  wide[1023] = '\0';
  check(MediaResolveLocation(wide, *base, next, scratch), "1023-byte Latin-1 Location");
  check(url::Pathname(*next).length == 1 + 1022 * 6, "Latin-1 bytes percent-encoded as UTF-8");
  free(scratch);
  free(base);
  free(next);
}

struct Row {
  const char* name;
  int status;
  OutboundError error;
  bool location;
  bool declared;
  const char* body;  // hex
  MediaHopVerdict verdict;
  const char* type;
};

void classification() {
  const char* png = "89504e470d0a1a0a0001020304050607";
  const Row rows[] = {
      // No answer: the guard's refusals are permanent, anything else threw.
      {"dns failure", 0, OutboundError::kResolve, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"non-public address", 0, OutboundError::kPrivateAddress, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"unnameable host", 0, OutboundError::kInvalidRequest, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"timeout", 0, OutboundError::kTimeout, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"connect", 0, OutboundError::kConnect, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"tls", 0, OutboundError::kTls, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"reset", 0, OutboundError::kHttp, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"header overflow", 0, OutboundError::kTooLarge, false, false, "", MediaHopVerdict::kTransient, nullptr},
      // Redirects come first (redirectRelative, redirectNoLocation, upstream304).
      {"301 + Location", 301, OutboundError::kNone, true, false, "", MediaHopVerdict::kRedirect, nullptr},
      {"302 no Location", 302, OutboundError::kNone, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"304", 304, OutboundError::kNone, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"307 declared huge", 307, OutboundError::kTooLarge, true, true, "", MediaHopVerdict::kRedirect, nullptr},
      // Declared length over 2 MiB whatever the status (upstream503Declared3MiB, over2MiBDeclared).
      {"200 declared huge", 200, OutboundError::kTooLarge, false, true, "", MediaHopVerdict::kPermanent, nullptr},
      {"503 declared huge", 503, OutboundError::kTooLarge, false, true, "", MediaHopVerdict::kPermanent, nullptr},
      {"404 declared huge", 404, OutboundError::kTooLarge, false, true, "", MediaHopVerdict::kPermanent, nullptr},
      // A streamed non-2xx body is never read in TypeScript (upstream503Streamed3MiB).
      {"503 streamed huge", 503, OutboundError::kTooLarge, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"500", 500, OutboundError::kNone, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"502 body timeout", 502, OutboundError::kTimeout, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"404", 404, OutboundError::kNone, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"410", 410, OutboundError::kNone, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"429", 429, OutboundError::kNone, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"101", 101, OutboundError::kNone, false, false, "", MediaHopVerdict::kTransient, nullptr},
      // 2xx (over2MiBStreamed, transport faults, magic bytes).
      {"200 streamed huge", 200, OutboundError::kTooLarge, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"200 body timeout", 200, OutboundError::kTimeout, false, false, png, MediaHopVerdict::kTransient, nullptr},
      {"200 bad framing", 200, OutboundError::kUnsupportedFraming, false, false, "", MediaHopVerdict::kTransient, nullptr},
      {"200 png", 200, OutboundError::kNone, false, false, png, MediaHopVerdict::kServe, "image/png"},
      {"206 png", 206, OutboundError::kNone, false, false, png, MediaHopVerdict::kServe, "image/png"},
      {"200 + Location png", 200, OutboundError::kNone, true, false, png, MediaHopVerdict::kServe, "image/png"},
      {"200 eleven bytes", 200, OutboundError::kNone, false, false, "89504e470d0a1a0a000102", MediaHopVerdict::kPermanent, nullptr},
      {"204 empty", 204, OutboundError::kNone, false, false, "", MediaHopVerdict::kPermanent, nullptr},
      {"200 svg", 200, OutboundError::kNone, false, false, "3c737667206f6e6c6f61643d", MediaHopVerdict::kPermanent, nullptr},
  };
  for (const Row& row : rows) {
    OutboundResponse response{};
    response.status = row.status;
    response.error = row.error;
    response.has_location = row.location;
    if (row.location) snprintf(response.location, sizeof(response.location), "/next");
    response.declared_too_large = row.declared;
    unsigned char body[64];
    response.body_length = from_hex(row.body, body, sizeof(body));
    const char* type = "unset";
    const MediaHopVerdict verdict = MediaClassifyHop(response, body, &type);
    check(verdict == row.verdict, "classification", row.name);
    check(row.type ? type && strcmp(type, row.type) == 0 : type == nullptr, "classification type", row.name);
  }
  printf("  fetchAndStore decision table: %zu rows\n", sizeof(rows) / sizeof(rows[0]));
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: media_get_test media_get_edges.json\n");
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(256u << 20);
  const tj::Value* root = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const tj::Value* unit = tj::Get(root, "unit");
  if (!root || !tj::IsObject(unit)) {
    fprintf(stderr, "fixture unreadable\n");
    return 2;
  }
  blocked_grid(tj::Get(unit, "blockedGrid"));
  sniff(tj::Get(unit, "sniff"));
  keys(tj::Get(unit, "keys"), tj::Get(unit, "mediaKeys"));
  resolutions(tj::Get(unit, "resolutions"));
  classification();
  tj::FreeArena(arena);
  free(data);
  if (g_failures) {
    fprintf(stderr, "media_get_test: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  printf("media_get_test: %d checks passed\n", g_checks);
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
