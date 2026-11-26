#include <memory>
// Unit checks of GET /v1/exchange/cross-chain/providers (R06) below the HTTP
// layer (src/exchange/cross_chain_service.cpp), against the TypeScript oracle
// cross_chain_providers_edges.json (loadConfig + buildApp + real sockets):
//  - every configuration: LoadNativeConfig + LoadProviderSettings refuse
//    exactly when loadConfig threw, otherwise EncodeProviderDirectory gives
//    TypeScript's JSON text and CrossChainProvidersResponse wire byte for
//    byte (all 16 credential subsets with MAYA execution off and on, and the
//    credential/MAYA_NODE_URLS edges). One asserted difference: a credential
//    of only \v is unconfigured in TypeScript (JS trim) and refused natively;
//  - the request cases' 200 bodies are the all-configured directory;
//  - MAYA roots: apiRoots() trimming and first-root-per-origin;
//  - bounds: the largest bodies fit the static buffers, one byte less fails
//    without touching the lengths, null outputs refuse;
//  - no credential appears in any encoded body.
// Usage: cross_chain_providers_test cross_chain_providers_edges.json
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cross_chain_providers.h"
#include "cross_chain_service.h"
#include "module_configs.h"
#include "native_config.h"
#include "token_json.h"

using namespace fortis::app;
using namespace fortis::config;
using namespace fortis::exchange;

namespace {

namespace tj = fortis::codec::tj;
namespace cc = fortis::exchange::cross_chain;

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

void to_hex(const uint8_t* bytes, size_t length, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 15];
  }
  out[2 * length] = '\0';
}

const char* env_lookup(void* context, const char* name) {
  const tj::Value* value = tj::Get(static_cast<const tj::Value*>(context), name);
  return tj::IsString(value) ? value->text : nullptr;
}

struct Directory {
  bool refused = false;
  char json[cc::kDirectoryJsonBytes];
  size_t json_length = 0;
  uint8_t wire[cc::kDirectoryWireBytes];
  size_t wire_length = 0;
  char wire_hex[2 * cc::kDirectoryWireBytes + 1];
  cc::ProviderSettings settings;
};

NativeConfig* g_config = nullptr;
CrossChainConfig* g_cross_chain = nullptr;

// What the native process would serve for `env`, or refused (startup exits).
void build(const tj::Value* env, Directory* out) {
  char error[256];
  *g_config = NativeConfig{};
  out->refused = !LoadNativeConfig(env_lookup, const_cast<tj::Value*>(env), g_config, error, sizeof(error));
  if (!out->refused) {
    // The composition root's slice: what CrossChainService::Start receives.
    fortis::app::BuildCrossChainConfig(*g_config, g_cross_chain);
    out->refused = !cc::LoadProviderSettings(*g_cross_chain, &out->settings);
  }
  if (out->refused) return;
  const bool encoded = cc::EncodeProviderDirectory(out->settings, out->json, sizeof(out->json), &out->json_length,
                                                   out->wire, sizeof(out->wire), &out->wire_length);
  check(encoded, "encode", "the directory does not fit its buffers");
  out->json[encoded ? out->json_length : 0] = '\0';
  to_hex(out->wire, encoded ? out->wire_length : 0, out->wire_hex);
}

bool contains(const void* haystack, size_t length, const char* needle) {
  const size_t n = strlen(needle);
  if (!n || n > length) return false;
  const char* bytes = static_cast<const char*>(haystack);
  for (size_t i = 0; i + n <= length; ++i)
    if (memcmp(bytes + i, needle, n) == 0) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s cross_chain_providers_edges.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64u << 20);
  const tj::Value* root = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const tj::Value* configs = tj::Get(root, "configs");
  const tj::Value* cases = tj::Get(root, "cases");
  const tj::Value* secrets = tj::Get(root, "secrets");
  if (!root || !tj::IsArray(configs) || !tj::IsArray(cases) || !secrets) {
    fprintf(stderr, "malformed fixture\n");
    return 2;
  }
  auto config = std::make_unique<NativeConfig>();
  auto cross_chain = std::make_unique<CrossChainConfig>();
  auto directory = std::make_unique<Directory>();
  g_config = config.get();
  g_cross_chain = cross_chain.get();

  size_t matched = 0, refusals = 0, differences = 0, largest_json = 0, largest_wire = 0;
  for (uint32_t i = 0; i < configs->count; ++i) {
    const tj::Value* c = configs->items[i];
    const char* name = tj::Get(c, "name")->text;
    build(tj::Get(c, "env"), directory.get());
    const tj::Value* error = tj::Get(c, "error");
    if (strcmp(name, "vertical-tab-key") == 0) {
      // Difference (documented beside the handler): NativeConfig refuses a
      // credential holding a control character after its space/tab/CR/LF
      // trim; JS trim() strips \v and TypeScript reports LI.FI unconfigured.
      check(!error && directory->refused, name, "expected TS unconfigured, native refusal");
      ++differences;
      continue;
    }
    if (error) {
      check(directory->refused, name, "TypeScript refused this configuration; native served it");
      if (directory->refused) ++refusals;
      continue;
    }
    if (directory->refused) {
      check(false, name, "native refused a configuration TypeScript served");
      continue;
    }
    const tj::Value* text = tj::Get(c, "responseText");
    const tj::Value* wire = tj::Get(c, "wireHex");
    const bool json_ok = tj::IsString(text) && text->length == directory->json_length &&
                         memcmp(text->text, directory->json, directory->json_length) == 0;
    const bool wire_ok = tj::IsString(wire) && strcmp(wire->text, directory->wire_hex) == 0;
    check(json_ok, name, directory->json);
    check(wire_ok, name, directory->wire_hex);
    if (json_ok && wire_ok) ++matched;
    if (directory->json_length > largest_json) largest_json = directory->json_length;
    if (directory->wire_length > largest_wire) largest_wire = directory->wire_length;
    for (uint32_t k = 0; k < secrets->count; ++k) {
      const char* secret = secrets->members[k].value->text;
      check(!contains(directory->json, directory->json_length, secret) &&
                !contains(directory->wire, directory->wire_length, secret),
            name, "a credential reached the directory");
    }
  }

  // The request cases (all credentials, MAYA on two roots): every 200 body
  // is the directory of `mainEnv`.
  build(tj::Get(root, "mainEnv"), directory.get());
  check(!directory->refused, "mainEnv", "refused");
  size_t bodies = 0;
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    if (tj::Get(c, "status")->number != 200 || !tj::StringEquals(tj::Get(c, "method"), "GET")) continue;
    const char* name = tj::Get(c, "name")->text;
    const tj::Value* text = tj::Get(c, "responseText");
    const tj::Value* wire = tj::Get(c, "wireHex");
    if (tj::IsString(wire)) {
      check(strcmp(wire->text, directory->wire_hex) == 0, name, "protobuf body");
    } else {
      check(tj::IsString(text) && text->length == directory->json_length &&
                memcmp(text->text, directory->json, directory->json_length) == 0,
            name, "JSON body");
    }
    ++bodies;
  }

  // MAYA roots (apiRoots): trailing slashes removed; the first root of each
  // WHATWG origin kept; the path stays.
  {
    tj::Arena env_arena = tj::GrowingArena(1u << 20);
    const char* env_text =
        "{\"MAYA_DASH_EXECUTION_ENABLED\":\"true\",\"MAYA_NODE_URLS\":"
        "\"https://A.example:443/x///, https://a.example/y,https://b.example/,https://b.example:8443/z\"}";
    const tj::Value* env = tj::Parse(env_arena, reinterpret_cast<const uint8_t*>(env_text), strlen(env_text));
    build(env, directory.get());
    const cc::ProviderSettings& s = directory->settings;
    check(!directory->refused && s.maya_root_count == 3, "maya roots", "count");
    check(!directory->refused && !strcmp(s.maya_roots[0], "https://a.example/x"), "maya roots", s.maya_roots[0]);
    check(!directory->refused && !strcmp(s.maya_roots[1], "https://b.example"), "maya roots", s.maya_roots[1]);
    check(!directory->refused && !strcmp(s.maya_roots[2], "https://b.example:8443/z"), "maya roots",
          s.maya_roots[2]);
    tj::FreeArena(env_arena);
  }

  // Bounds: the largest directory (no credentials, MAYA off: every false is
  // one byte longer in JSON) and the largest wire (all configured) fit; one
  // byte less fails and leaves the lengths alone; null outputs refuse.
  {
    cc::ProviderSettings none{};
    cc::ProviderSettings all{};
    all.lifi_configured = all.squid_configured = all.simpleswap_configured = true;
    all.maya_execution_enabled = true;
    all.maya_root_count = 2;
    char json[cc::kDirectoryJsonBytes];
    uint8_t wire[cc::kDirectoryWireBytes];
    size_t json_length = 0, wire_length = 0;
    check(cc::EncodeProviderDirectory(none, json, sizeof(json), &json_length, wire, sizeof(wire), &wire_length),
          "bounds", "unconfigured directory");
    const size_t none_json = json_length;
    check(none_json == largest_json, "bounds", "the unconfigured directory is the largest JSON");
    check(cc::EncodeProviderDirectory(all, json, sizeof(json), &json_length, wire, sizeof(wire), &wire_length),
          "bounds", "configured directory");
    const size_t all_wire = wire_length;
    check(all_wire == largest_wire, "bounds", "the configured directory is the largest wire");
    size_t a = 7, b = 9;
    check(cc::EncodeProviderDirectory(none, json, none_json, &a, wire, sizeof(wire), &b) && a == none_json,
          "bounds", "exact JSON capacity");
    a = 7, b = 9;
    check(!cc::EncodeProviderDirectory(none, json, none_json - 1, &a, wire, sizeof(wire), &b) && a == 7 && b == 9,
          "bounds", "JSON one byte short");
    check(cc::EncodeProviderDirectory(all, json, sizeof(json), &a, wire, all_wire, &b) && b == all_wire,
          "bounds", "exact wire capacity");
    a = 7, b = 9;
    check(!cc::EncodeProviderDirectory(all, json, sizeof(json), &a, wire, all_wire - 1, &b) && a == 7 && b == 9,
          "bounds", "wire one byte short");
    check(!cc::EncodeProviderDirectory(all, nullptr, 0, &a, wire, sizeof(wire), &b) &&
              !cc::EncodeProviderDirectory(all, json, sizeof(json), nullptr, wire, sizeof(wire), &b) &&
              !cc::EncodeProviderDirectory(all, json, sizeof(json), &a, nullptr, 0, &b) &&
              !cc::EncodeProviderDirectory(all, json, sizeof(json), &a, wire, sizeof(wire), nullptr),
          "bounds", "null outputs");
    check(!cc::LoadProviderSettings(*g_cross_chain, nullptr), "bounds", "null settings");
    printf("cross-chain providers: largest JSON %zu bytes (buffer %zu), largest wire %zu bytes (buffer %zu)\n",
           largest_json, cc::kDirectoryJsonBytes, largest_wire, cc::kDirectoryWireBytes);
  }

  printf("cross-chain providers: %zu configurations byte-identical, %zu refusals matched, %zu asserted "
         "difference, %zu request bodies\n",
         matched, refusals, differences, bodies);
  tj::FreeArena(arena);
  free(data);
  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
