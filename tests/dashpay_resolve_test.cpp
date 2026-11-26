#include <cassert>
#include <algorithm>
#include <cstdio>
// Unit checks of GET /v1/dashpay/names/resolve (R05) below the HTTP layer
// (src/exchange/dashpay_service.cpp), against TypeScript oracles:
//  - ParseNameQuery on every request target of dashpay_resolve_edges.json
//    (real Fastify over real sockets: fast-querystring, zod trim/1..69
//    UTF-16 units, normalizeDashPayName) and dashpay_route.json: valid
//    exactly when TypeScript called the resolver, with the same canonical
//    name;
//  - EncodeNameResponse against every TypeScript 200 body (JSON text and
//    DashPayNameResponse wire, found and absent);
//  - ValidPlatformIdentifier against validDashPlatformIdentifier;
//  - bounds: long padded values (past the old 8 KiB scratch), the largest
//    body, refused labels/identities/capacities, a target without a query.
// Usage: dashpay_resolve_test dashpay_resolve_edges.json dashpay_route.json
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashpay_service.h"
#include "token_json.h"

namespace {

namespace tj = fortis::codec::tj;
namespace dp = fortis::exchange::dashpay;

int g_failures = 0;
void check(bool condition, const char* what, const char* detail = "") {
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

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Hex → bytes (NUL-terminated); returns the length.
size_t from_hex(const char* hex, size_t hex_length, char* out) {
  size_t used = 0;
  for (size_t i = 0; i + 1 < hex_length; i += 2)
    out[used++] = static_cast<char>(hex_value(hex[i]) << 4 | hex_value(hex[i + 1]));
  out[used] = '\0';
  return used;
}

bool is_router_level(const char* name) {
  // The native request line refuses `#` and targets over 12 500 bytes before
  // any handler (documented differences; see the route test).
  return !strcmp(name, "fragment-in-query") || !strcmp(name, "fragment-before-query") ||
         !strcmp(name, "target-13000");
}

const char* identity_for(const tj::Value* verified, const char* name) {
  const tj::Value* identity = tj::Get(verified, name);
  return tj::IsString(identity) ? identity->text : nullptr;
}

// One TypeScript-observed request: the query verdict and, for a GET 200,
// the body bytes.
void check_case(const char* label_name, const char* target, const char* asked, const tj::Value* verified,
                const tj::Value* c, size_t* parsed, size_t* encoded) {
  char label[dp::kMaxLabelBytes + 1];
  size_t label_length = 0;
  const dp::NameQuery query = dp::ParseNameQuery(target, label, &label_length);
  ++*parsed;
  // query-string: first value wins; malformed unknown keys are refused.
  if (!strcmp(label_name, "repeated") || !strcmp(label_name, "repeated-key-only") ||
      !strcmp(label_name, "repeated-encoded-key")) {
    check(query == dp::NameQuery::kValid && !strcmp(label, "dash"),
          "first repeated name wins", label_name);
    return;
  }
  if (!strcmp(label_name, "malformed-key-escape")) {
    check(query == dp::NameQuery::kInvalid, "malformed key refused", label_name);
    return;
  }
  if (!asked) {
    check(query == dp::NameQuery::kInvalid, "TypeScript 400 parsed as valid", label_name);
    return;
  }
  char expected[dp::kMaxLabelBytes + 6];
  snprintf(expected, sizeof(expected), "%s", asked);
  const size_t expected_length = strlen(expected) - 5;
  check(query == dp::NameQuery::kValid && label_length == expected_length &&
            memcmp(label, expected, expected_length) == 0 && label[label_length] == '\0',
        "canonical name differs", label_name);
  if (query != dp::NameQuery::kValid) return;
  const tj::Value* status = tj::Get(c, "status");
  const tj::Value* method = tj::Get(c, "method");
  if (!tj::IsNumber(status) || status->number != 200 || !tj::StringEquals(method, "GET")) return;
  const tj::Value* wire = tj::Get(c, "wireHex");
  const tj::Value* text = tj::Get(c, "responseText");
  const char* identity = identity_for(verified, asked);
  uint8_t body[dp::kNameResponseBytes];
  const size_t length = dp::EncodeNameResponse(label, label_length, identity, tj::IsString(wire), body, sizeof(body));
  if (tj::IsString(wire)) {
    char bytes[512];
    const size_t want = from_hex(wire->text, wire->length, bytes);
    check(length == want && memcmp(body, bytes, want) == 0, "protobuf body differs", label_name);
  } else {
    check(tj::IsString(text) && length == text->length && memcmp(body, text->text, length) == 0,
          "JSON body differs", label_name);
  }
  ++*encoded;
}

void edges(const tj::Value* root, size_t* parsed, size_t* encoded) {
  const tj::Value* verified = tj::Get(root, "verified");
  const tj::Value* cases = tj::Get(root, "cases");
  check(tj::IsObject(verified) && tj::IsArray(cases) && cases->count > 100, "edges fixture shape");
  if (!tj::IsArray(cases)) return;
  char* target = static_cast<char*>(malloc(64 * 1024));
  size_t router_level = 0;
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    const tj::Value* name = tj::Get(c, "name");
    const char* case_name = tj::IsString(name) ? name->text : "?";
    if (tj::StringEquals(tj::Get(c, "group"), "concurrent")) {
      const tj::Value* labels = tj::Get(c, "labels");
      const tj::Value* bodies = tj::Get(c, "bodies");
      for (uint32_t k = 0; tj::IsArray(labels) && k < labels->count; ++k) {
        char asked[96];
        snprintf(asked, sizeof(asked), "%s.dash", labels->items[k]->text);
        uint8_t body[dp::kNameResponseBytes];
        const size_t length = dp::EncodeNameResponse(labels->items[k]->text, labels->items[k]->length,
                                                     identity_for(verified, asked), false, body, sizeof(body));
        check(length == bodies->items[k]->length && memcmp(body, bodies->items[k]->text, length) == 0,
              "concurrent body differs", asked);
        ++*encoded;
      }
      continue;
    }
    if (is_router_level(case_name)) { ++router_level; continue; }
    const tj::Value* status = tj::Get(c, "status");
    if (!tj::IsNumber(status) || status->number == 401 || status->number == 403 || status->number == 429) continue;
    const tj::Value* hex = tj::Get(c, "targetHex");
    from_hex(hex->text, hex->length, target);
    const tj::Value* asked = tj::Get(c, "resolverAsked");
    const char* first = tj::IsArray(asked) && asked->count ? asked->items[0]->text : nullptr;
    check_case(case_name, target, first, verified, c, parsed, encoded);
  }
  check(router_level == 3, "router-level cases not found");
  free(target);

  const tj::Value* identifiers = tj::Get(root, "identifiers");
  check(tj::IsArray(identifiers) && identifiers->count >= 10, "identifier oracle shape");
  for (uint32_t i = 0; tj::IsArray(identifiers) && i < identifiers->count; ++i) {
    const tj::Value* row = identifiers->items[i];
    const tj::Value* text = tj::Get(row, "text");
    const bool valid = tj::Get(row, "valid")->type == tj::Type::kTrue;
    check(dp::ValidPlatformIdentifier(text->text) == valid, "identifier verdict differs", text->text);
  }
}

// dashpay_route.json: the resolver-outcome contract's query verdicts.
void route_fixture(const tj::Value* root, size_t* parsed) {
  const tj::Value* cases = tj::Get(root, "cases");
  check(tj::IsArray(cases) && cases->count >= 40, "route fixture shape");
  for (uint32_t i = 0; tj::IsArray(cases) && i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    const double status = tj::Get(c, "status")->number;
    if (status == 401 || status == 403) continue;
    const tj::Value* path = tj::Get(c, "path");
    char label[dp::kMaxLabelBytes + 1];
    size_t label_length = 0;
    const dp::NameQuery query = dp::ParseNameQuery(path->text, label, &label_length);
    ++*parsed;
    const bool first_repeated = tj::StringEquals(tj::Get(c, "name"), "invalid-repeated");
    check((query == dp::NameQuery::kValid) == (status != 400 || first_repeated), "route fixture verdict differs", path->text);
    const tj::Value* text = tj::Get(c, "responseText");
    if (status == 200 && tj::IsString(text) && text->length && query == dp::NameQuery::kValid) {
      char prefix[96];
      snprintf(prefix, sizeof(prefix), "{\"name\":\"%s.dash\"", label);
      check(strncmp(text->text, prefix, strlen(prefix)) == 0, "route fixture name differs", path->text);
    }
  }
}

void bounds() {
  char label[dp::kMaxLabelBytes + 1];
  size_t length = 99;
  check(dp::ParseNameQuery(nullptr, label, &length) == dp::NameQuery::kInvalid && length == 0, "null target");
  check(dp::ParseNameQuery("/v1/dashpay/names/resolve", label, &length) == dp::NameQuery::kInvalid, "no query");
  check(dp::ParseNameQuery("/v1/dashpay/names/resolve?name=dash", label, &length) == dp::NameQuery::kValid &&
            length == 4 && !strcmp(label, "dash"),
        "plain name");

  // Padding past the old 8 KiB scratch, up to the request line's 12 500.
  static char target[13000];
  const char* prefix = "/v1/dashpay/names/resolve?name=";
  static const size_t kPaddings[] = {8160, 8191, 8192, 8193, 12400};
  for (size_t padding : kPaddings) {
    snprintf(target, sizeof(target), "%s", prefix);
    size_t at = strlen(prefix);
    memset(target + at, '+', padding);
    at += padding;
    memcpy(target + at, "Dash", 5);
    char what[64];
    snprintf(what, sizeof(what), "%zu-byte plus padding", padding);
    check(dp::ParseNameQuery(target, label, &length) == dp::NameQuery::kValid && !strcmp(label, "dash"), what);
  }
  snprintf(target, sizeof(target), "%s", prefix);
  size_t escaped_at = strlen(target);
  assert(escaped_at + 4000 * 3 + 5 <= sizeof(target));
  for (size_t i = 0; i < 4000; ++i) {
    std::copy_n("%20", 3, target + escaped_at);
    escaped_at += 3;
  }
  std::copy_n("dash", 5, target + escaped_at);
  check(dp::ParseNameQuery(target, label, &length) == dp::NameQuery::kValid, "4000 escaped spaces");
  // A long value that trims to 70 units is still too long.
  snprintf(target, sizeof(target), "%s", prefix);
  memset(target + strlen(prefix), '+', 9000);
  snprintf(target + strlen(prefix) + 9000, 100, "%%40%s.dashx", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  check(dp::ParseNameQuery(target, label, &length) == dp::NameQuery::kInvalid, "padded 70 units");
  // Raw bytes >= 0x80 anywhere in the target (Node refuses the request).
  check(dp::ParseNameQuery("/v1/dashpay/names/resolve?name=dash&x=\xc3\xa4", label, &length) ==
            dp::NameQuery::kInvalid,
        "raw UTF-8 elsewhere in the query");

  // The encoder's bounds.
  const char* a63 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char* id50 = "11111111111111111111111111111111111111111111111111";
  uint8_t body[dp::kNameResponseBytes];
  const size_t json = dp::EncodeNameResponse(a63, 63, id50, false, body, sizeof(body));
  check(json == 9 + 68 + 16 + 50 + 2 && body[json - 1] == '}', "largest JSON body");
  const size_t wire = dp::EncodeNameResponse(a63, 63, id50, true, body, sizeof(body));
  check(wire == 2 + 68 + 2 + 50 && body[0] == 0x0a && body[1] == 68 && body[70] == 0x12 && body[71] == 50,
        "largest protobuf body");
  check(dp::EncodeNameResponse("abc", 3, nullptr, true, body, sizeof(body)) == 10 && body[1] == 8,
        "absence has no identity_id");
  check(dp::EncodeNameResponse("ab", 2, nullptr, false, body, sizeof(body)) == 0, "2-byte label refused");
  check(dp::EncodeNameResponse("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 64, nullptr, false,
                               body, sizeof(body)) == 0,
        "64-byte label refused");
  check(dp::EncodeNameResponse("abc", 3, "111111111111111111111111111111111111111111111111111", false, body,
                               sizeof(body)) == 0,
        "51-char identity refused");
  check(dp::EncodeNameResponse("abc", 3, nullptr, false, body, dp::kNameResponseBytes - 1) == 0,
        "short capacity refused");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s dashpay_resolve_edges.json dashpay_route.json\n", argv[0]);
    return 2;
  }
  size_t parsed = 0, encoded = 0;
  for (int f = 1; f <= 2; ++f) {
    size_t length = 0;
    char* data = read_file(argv[f], &length);
    if (!data) {
      fprintf(stderr, "cannot read %s\n", argv[f]);
      return 2;
    }
    tj::Arena arena = tj::GrowingArena(64u << 20);
    const tj::Value* root = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
    check(root != nullptr, "fixture JSON");
    if (root) {
      if (f == 1) edges(root, &parsed, &encoded);
      else route_fixture(root, &parsed);
    }
    tj::FreeArena(arena);
    free(data);
  }
  bounds();
  if (g_failures) {
    fprintf(stderr, "dashpay_resolve_test: %d failures\n", g_failures);
    return 1;
  }
  printf("dashpay_resolve_test: %zu query verdicts and %zu bodies match TypeScript; bounds OK\n", parsed, encoded);
  return 0;
}
