#include <algorithm>
// R10 unit test of GET /v1/networks/robinhood/sponsored-trades/capabilities
// generation (robinhood_policy.cpp EncodeSponsoredCapabilities and
// SponsoredTradesEnabled, fed by LoadNativeConfig): the exact JSON and
// protobuf bytes of both states, the buffer boundaries, the enabling truth
// table and the configuration matrix of the TypeScript oracle
// (robinhood_capabilities_edges.json), including the values the native loader
// refuses at startup. The real-socket contract is
// tests/robinhood_capabilities_edges_test.py.
// Usage: robinhood_capabilities_test
#include "native_config.h"
#include "robinhood_policy.h"

#include <stdio.h>
#include <string.h>

using namespace fortis::config;

namespace rh = fortis::exchange::robinhood;

namespace {
int failures = 0;
int checks = 0;
bool check(bool condition, const char* what) {
  ++checks;
  if (!condition) { ++failures; fprintf(stderr, "FAIL: %s\n", what); }
  return condition;
}

// RobinhoodSponsoredTradeGateway.capabilities() as the oracle recorded it.
const char kEnabledJson[] =
    "{\"network\":\"robinhood\",\"chainId\":4663,\"enabled\":true,\"gasless\":true,\"sponsoredGas\":true,"
    "\"batchedCalls\":true,\"maxCalls\":2,\"accountStandard\":\"eip-7702\"}";
const char kDisabledJson[] =
    "{\"network\":\"robinhood\",\"chainId\":4663,\"enabled\":false,\"gasless\":false,\"sponsoredGas\":false,"
    "\"batchedCalls\":false,\"maxCalls\":2,\"accountStandard\":\"eip-7702\","
    "\"reason\":\"ALCHEMY_GAS_POLICY_NOT_CONFIGURED\"}";
const char kEnabledHex[] = "0a09726f62696e686f6f6410b7241801200128013001380242086569702d37373032";
const char kDisabledHex[] =
    "0a09726f62696e686f6f6410b724380242086569702d373730324a21414c4348454d595f4741535f504f4c4943595f4e4f545f"
    "434f4e46494755524544";

bool same_hex(const uint8_t* bytes, size_t length, const char* hex) {
  if (strlen(hex) != length * 2) return false;
  char pair[3] = {};
  for (size_t i = 0; i < length; ++i) {
    snprintf(pair, sizeof(pair), "%02x", bytes[i]);
    if (memcmp(pair, hex + 2 * i, 2) != 0) return false;
  }
  return true;
}

void encodings() {
  for (int state = 0; state < 2; ++state) {
    const bool enabled = state == 1;
    const char* json_expected = enabled ? kEnabledJson : kDisabledJson;
    const char* hex_expected = enabled ? kEnabledHex : kDisabledHex;
    const size_t json_size = strlen(json_expected), proto_size = strlen(hex_expected) / 2;
    char json[256];
    uint8_t proto[128];
    size_t json_length = 0, proto_length = 0;
    check(rh::EncodeSponsoredCapabilities(enabled, json, sizeof(json), &json_length, proto, sizeof(proto),
                                          &proto_length),
          "encode in the service's buffers");
    check(json_length == json_size && memcmp(json, json_expected, json_size) == 0 && json[json_size] == '\0',
          enabled ? "enabled JSON bytes" : "disabled JSON bytes");
    check(same_hex(proto, proto_length, hex_expected), enabled ? "enabled protobuf bytes" : "disabled protobuf bytes");
    // Exact capacities: the JSON needs its length + 1 (NUL), the protobuf
    // exactly its length; one byte less is refused, not truncated.
    check(rh::EncodeSponsoredCapabilities(enabled, json, json_size + 1, &json_length, proto, proto_size,
                                          &proto_length) &&
              json_length == json_size && proto_length == proto_size,
          "exact capacities accepted");
    size_t untouched_json = 777, untouched_proto = 777;
    check(!rh::EncodeSponsoredCapabilities(enabled, json, json_size, &untouched_json, proto, sizeof(proto),
                                           &untouched_proto) &&
              untouched_json == 777 && untouched_proto == 777,
          "JSON one byte short refused, lengths untouched");
    check(!rh::EncodeSponsoredCapabilities(enabled, json, sizeof(json), &untouched_json, proto, proto_size - 1,
                                           &untouched_proto) &&
              untouched_json == 777 && untouched_proto == 777,
          "protobuf one byte short refused, lengths untouched");
    check(!rh::EncodeSponsoredCapabilities(enabled, nullptr, 256, &json_length, proto, sizeof(proto), &proto_length),
          "null JSON buffer refused");
    check(!rh::EncodeSponsoredCapabilities(enabled, json, sizeof(json), &json_length, nullptr, 128, &proto_length),
          "null protobuf buffer refused");
    check(!rh::EncodeSponsoredCapabilities(enabled, json, 0, &json_length, proto, 0, &proto_length),
          "zero capacities refused");
    // The documents' only variable content is the one bit.
    check((strstr(json, "reason") != nullptr) == !enabled, "reason present exactly when disabled");
  }
  // The service's buffers keep headroom over the largest document.
  check(sizeof(kDisabledJson) <= 256 && strlen(kDisabledHex) / 2 <= 128, "service buffer headroom");
}

void truth_table() {
  check(!rh::SponsoredTradesEnabled(false, false), "neither: disabled");
  check(!rh::SponsoredTradesEnabled(true, false), "key only: disabled");
  check(!rh::SponsoredTradesEnabled(false, true), "policy only: disabled");
  check(rh::SponsoredTradesEnabled(true, true), "both: enabled");
  check(strcmp(rh::kSponsorshipDisabledReason, "ALCHEMY_GAS_POLICY_NOT_CONFIGURED") == 0, "reason constant");
}

struct Env { const char* pairs[4][2]; size_t count = 0; };
const char* lookup(void* context, const char* name) {
  const Env* env = static_cast<const Env*>(context);
  for (size_t i = 0; i < env->count; ++i)
    if (strcmp(env->pairs[i][0], name) == 0) return env->pairs[i][1];
  return nullptr;
}

// One row of the oracle's configuration matrix: -1 = native startup refusal
// (with `refusal` in the message), else TypeScript's `enabled`.
struct Row {
  const char* name;
  const char* key;
  const char* policy;
  int enabled;
  const char* refusal;
};

void configurations() {
  static char p255[256], k255[256], p256[257], k256[257], raw4096[4097], raw4097[4098];
  memset(p255, 'p', 255);
  memset(k255, 'k', 255);
  memset(p256, 'p', 256);
  memset(k256, 'k', 256);
  const char* key = "oracle-alchemy-secret-0001";
  const char* policy = "oracle-gas-policy-0001";
  memset(raw4096, ' ', 4096 - strlen(policy));
  std::copy_n(policy, strlen(policy), raw4096 + 4096 - strlen(policy));
  memset(raw4097, ' ', 4097 - strlen(key));
  std::copy_n(key, strlen(key), raw4097 + 4097 - strlen(key));
  const Row rows[] = {
      {"keyless", nullptr, nullptr, 0, nullptr},
      {"key-only", key, nullptr, 0, nullptr},
      {"policy-only", nullptr, policy, 0, nullptr},
      {"both", key, policy, 1, nullptr},
      {"key-blank", "", policy, 0, nullptr},
      {"key-whitespace", " \t ", policy, 0, nullptr},
      {"policy-blank", key, "", 0, nullptr},
      {"policy-whitespace", key, " \r\n\t ", 0, nullptr},
      {"both-blank", "", "", 0, nullptr},
      {"both-padded", "  oracle-alchemy-secret-0001\t", "\noracle-gas-policy-0001\r ", 1, nullptr},
      {"policy-255", key, p255, 1, nullptr},
      {"key-255", k255, policy, 1, nullptr},
      {"policy-raw-4096", key, raw4096, 1, nullptr},
      {"policy-with-symbols", key, "a1b2-c3d4_e5f6.~!$&'()*+,;=:@/?", 1, nullptr},
      {"policy-256", key, p256, -1, "ALCHEMY_GAS_POLICY_ID is too long"},
      {"key-256", k256, policy, -1, "ALCHEMY_API_KEY is too long"},
      {"key-raw-4097", raw4097, policy, -1, "ALCHEMY_API_KEY is too long"},
      {"policy-inner-space", key, "policy with space", -1, "ALCHEMY_GAS_POLICY_ID contains invalid characters"},
      {"policy-form-feed", key, "\foracle-gas-policy-0001", -1, "ALCHEMY_GAS_POLICY_ID contains invalid characters"},
      {"policy-nbsp", key, "\xc2\xa0oracle-gas-policy-0001\xc2\xa0", -1,
       "ALCHEMY_GAS_POLICY_ID contains invalid characters"},
      {"policy-non-ascii", key, "p\xc3\xb3licy-0001", -1, "ALCHEMY_GAS_POLICY_ID contains invalid characters"},
      {"key-only-blank-policy-nbsp", key, "\xc2\xa0", -1, "ALCHEMY_GAS_POLICY_ID contains invalid characters"},
  };
  for (const Row& row : rows) {
    Env env;
    if (row.key) { env.pairs[env.count][0] = "ALCHEMY_API_KEY"; env.pairs[env.count++][1] = row.key; }
    if (row.policy) { env.pairs[env.count][0] = "ALCHEMY_GAS_POLICY_ID"; env.pairs[env.count++][1] = row.policy; }
    static NativeConfig config;
    config = NativeConfig{};
    char error[256] = {};
    const bool loaded = LoadNativeConfig(lookup, &env, &config, error, sizeof(error));
    char what[160];
    if (row.enabled < 0) {
      snprintf(what, sizeof(what), "%s: startup refused with \"%s\" (got \"%s\")", row.name, row.refusal, error);
      check(!loaded && strstr(error, row.refusal) != nullptr, what);
      // The refusal names the variable, never its value.
      snprintf(what, sizeof(what), "%s: refusal message carries no credential", row.name);
      check(!strstr(error, "oracle-") && !strstr(error, "ppppp") && !strstr(error, "kkkkk"), what);
      continue;
    }
    snprintf(what, sizeof(what), "%s: loads (%s)", row.name, error);
    if (!check(loaded, what)) continue;
    const bool enabled = rh::SponsoredTradesEnabled(config.alchemy_api_key_configured,
                                                    config.alchemy_gas_policy_id_configured);
    snprintf(what, sizeof(what), "%s: enabled %d, TypeScript %d", row.name, enabled, row.enabled);
    check(enabled == (row.enabled == 1), what);
  }
  // Trimming keeps the value (padding is stripped, not sent upstream).
  Env padded;
  padded.pairs[0][0] = "ALCHEMY_GAS_POLICY_ID";
  padded.pairs[0][1] = "\noracle-gas-policy-0001\r ";
  padded.count = 1;
  static NativeConfig config;
  config = NativeConfig{};
  char error[256] = {};
  check(LoadNativeConfig(lookup, &padded, &config, error, sizeof(error)) &&
            strcmp(config.alchemy_gas_policy_id, "oracle-gas-policy-0001") == 0,
        "padded policy id is trimmed");
}
}  // namespace

int main() {
  encodings();
  truth_table();
  configurations();
  if (failures) {
    fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("robinhood capabilities: %d checks passed\n", checks);
  return 0;
}
