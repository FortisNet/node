#include "price_id_parser.h"

#include "js_case.h"

#include <string.h>

namespace fortis::providers {

namespace {
struct Slice { size_t begin; size_t length; };

// String.prototype.trim: the JavaScript WhiteSpace and LineTerminator set.
Slice trim_js(const char* input, Slice value) {
  while (size_t width = fortis::providers::JsWhitespaceAtStart(input + value.begin, value.length)) {
    value.begin += width;
    value.length -= width;
  }
  while (size_t width = fortis::providers::JsWhitespaceAtEnd(input + value.begin, value.length))
    value.length -= width;
  return value;
}

bool starts(const char* input, Slice value, const char* prefix, Slice* rest) {
  const size_t length = strlen(prefix);
  if (value.length < length || memcmp(input + value.begin, prefix, length) != 0)
    return false;
  if (rest) *rest = {value.begin + length, value.length - length};
  return true;
}

char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
}

bool copy_lower_ascii(const char* input, Slice value, char* output, size_t capacity) {
  if (value.length >= capacity) return false;
  for (size_t i = 0; i < value.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(input[value.begin + i]);
    if (c >= 0x80) return false;
    output[i] = ascii_lower(c);
  }
  output[value.length] = '\0';
  return true;
}

bool copy_exact(const char* input, Slice value, char* output, size_t capacity) {
  if (value.length >= capacity) return false;
  if (value.length) memcpy(output, input + value.begin, value.length);
  output[value.length] = '\0';
  return true;
}

bool append_text(char* output, size_t capacity, size_t* cursor,
                 const char* text, size_t length) {
  if (*cursor >= capacity || length > capacity - *cursor - 1) return false;
  memcpy(output + *cursor, text, length);
  *cursor += length;
  output[*cursor] = '\0';
  return true;
}

bool set_source(ParsedPriceId* output, const char* prefix, const char* value) {
  size_t cursor = 0;
  output->source_id[0] = '\0';
  return append_text(output->source_id, sizeof(output->source_id), &cursor,
                     prefix, strlen(prefix)) &&
         append_text(output->source_id, sizeof(output->source_id), &cursor,
                     value, strlen(value));
}

bool decimal(Slice value, const char* input, size_t maximum) {
  if (!value.length || value.length > maximum) return false;
  for (size_t i = 0; i < value.length; ++i) {
    const char c = input[value.begin + i];
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool base58_char(unsigned char c) {
  return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') ||
         (c >= 'J' && c <= 'N') || (c >= 'P' && c <= 'Z') ||
         (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}

bool valid_base58(const char* input, Slice value) {
  if (!value.length) return false;
  for (size_t i = 0; i < value.length; ++i)
    if (!base58_char(static_cast<unsigned char>(input[value.begin + i]))) return false;
  return true;
}

bool hex_char(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool valid_evm_address(const char* input, Slice value) {
  if (value.length != 42 || input[value.begin] != '0' || input[value.begin + 1] != 'x')
    return false;
  for (size_t i = 2; i < value.length; ++i)
    if (!hex_char(static_cast<unsigned char>(input[value.begin + i]))) return false;
  return true;
}

bool valid_identifier(const char* input, Slice value) {
  if (!value.length) return false;
  const char first = input[value.begin];
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
    return false;
  for (size_t i = 1; i < value.length; ++i) {
    const char c = input[value.begin + i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool canonical_aptos_address(const char* input, Slice address, char* output,
                            size_t capacity) {
  if (address.length < 3 || address.length > 66 ||
      input[address.begin] != '0' || input[address.begin + 1] != 'x') return false;
  Slice hex{address.begin + 2, address.length - 2};
  if (!hex.length || hex.length > 64) return false;
  for (size_t i = 0; i < hex.length; ++i)
    if (!hex_char(static_cast<unsigned char>(input[hex.begin + i]))) return false;
  size_t first_nonzero = 0;
  while (first_nonzero < hex.length && input[hex.begin + first_nonzero] == '0')
    ++first_nonzero;
  if (first_nonzero == hex.length) {
    if (capacity < 4) return false;
    memcpy(output, "0x0", 4);
    return true;
  }
  const size_t digits = hex.length - first_nonzero;
  if (digits + 3 > capacity) return false;
  output[0] = '0'; output[1] = 'x';
  for (size_t i = 0; i < digits; ++i)
    output[2 + i] = ascii_lower(static_cast<unsigned char>(input[hex.begin + first_nonzero + i]));
  output[2 + digits] = '\0';
  return true;
}

bool parse_aptos(const char* input, Slice payload, ParsedPriceId* output) {
  payload = trim_js(input, payload);  // TS canonicalAptosTokenIdentifier trims again.
  size_t first = payload.length;
  size_t second = payload.length;
  for (size_t i = 0; i + 1 < payload.length; ++i) {
    if (input[payload.begin + i] == ':' && input[payload.begin + i + 1] == ':') {
      if (first == payload.length) first = i;
      else if (second == payload.length) second = i;
      else return false;
      ++i;
    }
  }
  Slice address{payload.begin, first == payload.length ? payload.length : first};
  char canonical_address[67]{};
  if (!canonical_aptos_address(input, address, canonical_address,
                               sizeof(canonical_address))) return false;
  char canonical_value[kPriceIdMaxSourceIdBytes + 1]{};
  size_t cursor = 0;
  if (!append_text(canonical_value, sizeof(canonical_value), &cursor,
                   canonical_address, strlen(canonical_address))) return false;
  if (first != payload.length) {
    if (second == payload.length) return false;
    Slice module{payload.begin + first + 2, second - first - 2};
    Slice structure{payload.begin + second + 2,
                    payload.length - second - 2};
    if (!valid_identifier(input, module) || !valid_identifier(input, structure) ||
        !append_text(canonical_value, sizeof(canonical_value), &cursor, "::", 2) ||
        !append_text(canonical_value, sizeof(canonical_value), &cursor,
                     input + module.begin, module.length) ||
        !append_text(canonical_value, sizeof(canonical_value), &cursor, "::", 2) ||
        !append_text(canonical_value, sizeof(canonical_value), &cursor,
                     input + structure.begin, structure.length)) return false;
  } else if (second != payload.length) {
    return false;
  }
  output->kind = PriceIdKind::kContract;
  if (!copy_exact("aptos", {0, 5}, output->network, sizeof(output->network)) ||
      !copy_exact("aptos", {0, 5}, output->standard, sizeof(output->standard)) ||
      !copy_exact(canonical_value, {0, strlen(canonical_value)}, output->address,
                  sizeof(output->address)) ||
      !set_source(output, "aptos:", canonical_value)) return false;
  return true;
}

bool parse_contract(const char* input, Slice id, ParsedPriceId* output) {
  size_t marker = id.length;
  size_t standard_length = 0;
  const char* standard = nullptr;
  for (size_t i = id.length; i > 0; --i) {
    const size_t at = i - 1;
    if (at + 7 <= id.length && memcmp(input + id.begin + at, ":erc20:", 7) == 0) {
      marker = at; standard = "erc20"; standard_length = 5; break;
    }
    if (at + 7 <= id.length && memcmp(input + id.begin + at, ":trc20:", 7) == 0) {
      marker = at; standard = "trc20"; standard_length = 5; break;
    }
  }
  if (!standard || marker == 0) return false;
  Slice network_input{id.begin, marker};
  Slice address_input{id.begin + marker + 7, id.length - marker - 7};
  if (!address_input.length) return false;
  for (size_t i = 0; i < network_input.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(input[network_input.begin + i]);
    // JavaScript RegExp `.` (no s flag) excludes the line terminators LF,
    // CR, U+2028 and U+2029.
    if (c == '\n' || c == '\r') return false;
    if (c == 0xe2 && i + 2 < network_input.length &&
        static_cast<unsigned char>(input[network_input.begin + i + 1]) == 0x80 &&
        (static_cast<unsigned char>(input[network_input.begin + i + 2]) == 0xa8 ||
         static_cast<unsigned char>(input[network_input.begin + i + 2]) == 0xa9)) return false;
  }
  if (strcmp(standard, "erc20") == 0) {
    if (!valid_evm_address(input, address_input)) return false;
  } else {
    if (network_input.length != 4 ||
        ascii_lower(input[network_input.begin]) != 't' ||
        ascii_lower(input[network_input.begin + 1]) != 'r' ||
        ascii_lower(input[network_input.begin + 2]) != 'o' ||
        ascii_lower(input[network_input.begin + 3]) != 'n' ||
        address_input.length != 34 || input[address_input.begin] != 'T' ||
        !valid_base58(input, address_input)) return false;
  }
  // The network id, ASCII lower-cased (an identifier: js-case).
  size_t network_length = 0;
  if (!fortis::compat::AsciiToLowerCase(input + network_input.begin, network_input.length,
                             output->network, sizeof(output->network),
                             &network_length)) return false;
  if (strcmp(standard, "erc20") == 0) {
    if (!copy_lower_ascii(input, address_input, output->address,
                          sizeof(output->address))) return false;
  } else if (!copy_exact(input, address_input, output->address,
                         sizeof(output->address))) return false;
  memcpy(output->standard, standard, standard_length);
  output->standard[standard_length] = '\0';
  output->kind = PriceIdKind::kContract;
  size_t cursor = 0;
  output->source_id[0] = '\0';
  return append_text(output->source_id, sizeof(output->source_id), &cursor,
                     output->network, strlen(output->network)) &&
         append_text(output->source_id, sizeof(output->source_id), &cursor, ":", 1) &&
         append_text(output->source_id, sizeof(output->source_id), &cursor,
                     output->standard, strlen(output->standard)) &&
         append_text(output->source_id, sizeof(output->source_id), &cursor, ":", 1) &&
         append_text(output->source_id, sizeof(output->source_id), &cursor,
                     output->address, strlen(output->address));
}
bool parse_price_id_impl(const char* raw, size_t raw_length, ParsedPriceId* output) {
  if (!raw || raw_length > kPriceIdMaxRawBytes) return false;
  for (size_t i = 0; i < raw_length; ++i)
    if (raw[i] == '\0') return false;
  Slice id = trim_js(raw, {0, raw_length});
  if (!id.length) return false;

  bool has_colon = false;
  for (size_t i = 0; i < id.length; ++i)
    if (raw[id.begin + i] == ':') { has_colon = true; break; }
  if (!has_colon) {
    // The id ASCII lower-cased (an identifier: js-case), then
    // /^[a-z0-9][a-z0-9-]{0,99}$/: any non-ASCII character fails.
    size_t length = 0;
    for (size_t i = 0; i < id.length; ++i) {
      const unsigned char c = static_cast<unsigned char>(raw[id.begin + i]);
      if (c >= 0x80) return false;
      const char lowered = ascii_lower(c);
      if (length == 100) return false;
      if (!((lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9') ||
            (length > 0 && lowered == '-'))) return false;
      output->address[length++] = lowered;
    }
    output->address[length] = '\0';
    output->kind = PriceIdKind::kCoinGecko;
    copy_exact("coingecko", {0, 9}, output->network, sizeof(output->network));
    return set_source(output, "", output->address);
  }

  Slice payload{};
  if (starts(raw, id, "spl:", &payload)) {
    if (payload.length < 32 || payload.length > 44 || !valid_base58(raw, payload) ||
        !copy_exact(raw, payload, output->address, sizeof(output->address)) ||
        !copy_exact("solana", {0, 6}, output->network, sizeof(output->network))) return false;
    output->kind = PriceIdKind::kSpl;
    return set_source(output, "spl:", output->address);
  }
  if (starts(raw, id, "asa:", &payload)) {
    if (!decimal(payload, raw, 20) ||
        !copy_exact(raw, payload, output->address, sizeof(output->address)) ||
        !copy_exact("algorand", {0, 8}, output->network, sizeof(output->network))) return false;
    output->kind = PriceIdKind::kAsa;
    return set_source(output, "asa:", output->address);
  }
  if (starts(raw, id, "aptos:", &payload)) return parse_aptos(raw, payload, output);
  if (starts(raw, id, "tron:trc10:", &payload)) {
    if (!decimal(payload, raw, 20)) return false;
    size_t first_nonzero = 0;
    while (first_nonzero < payload.length && raw[payload.begin + first_nonzero] == '0')
      ++first_nonzero;
    if (first_nonzero == payload.length) return false;
    Slice canonical{payload.begin + first_nonzero, payload.length - first_nonzero};
    if (!copy_exact(raw, canonical, output->address, sizeof(output->address)) ||
        !copy_exact("tron", {0, 4}, output->network, sizeof(output->network))) return false;
    output->kind = PriceIdKind::kTrc10;
    return set_source(output, "tron:trc10:", output->address);
  }
  return parse_contract(raw, id, output);
}
}  // namespace

const char* CoinGeckoPlatformForNetwork(const char* network) {
  if (!network) return nullptr;
  struct Mapping { const char* network; const char* platform; const char* chain; };
  static constexpr Mapping mappings[] = {
      {"eth-mainnet", "ethereum", "1"}, {"bnb", "binance-smart-chain", "56"},
      {"solana", "solana", nullptr}, {"cardano", "cardano", nullptr},
      {"algorand", "algorand", nullptr}, {"base", "base", "8453"},
      {"polygon", "polygon-pos", "137"}, {"avalanche", "avalanche", "43114"},
      {"cronos", "cronos", "25"}, {"arbitrum", "arbitrum-one", "42161"},
      {"optimism", "optimistic-ethereum", "10"}, {"xdc", "xdc-network", "50"},
      {"sui", "sui", nullptr}, {"tron", "tron", nullptr},
      {"vechain", "vechain", nullptr}, {"aptos", "aptos", nullptr},
  };
  if (strncmp(network, "eip155:", 7) == 0) {
    const char* chain = network + 7;
    // TypeScript converts the decimal suffix with Number(), so leading zeroes
    // are insignificant (for example eip155:01 aliases chain 1).
    while (*chain == '0' && chain[1] != '\0') ++chain;
    for (const char* digit = chain; *digit; ++digit)
      if (*digit < '0' || *digit > '9') return nullptr;
    for (const Mapping& mapping : mappings)
      if (mapping.chain && strcmp(mapping.chain, chain) == 0) return mapping.platform;
    return nullptr;
  }
  for (const Mapping& mapping : mappings)
    if (strcmp(mapping.network, network) == 0) return mapping.platform;
  return nullptr;
}

bool ParsePriceId(const char* raw, size_t raw_length, ParsedPriceId* output) {
  if (!output) return false;
  *output = ParsedPriceId{};
  ParsedPriceId parsed{};
  if (!parse_price_id_impl(raw, raw_length, &parsed)) return false;
  *output = parsed;
  return true;
}

size_t JsWhitespaceAtStart(const char* text, size_t length) {
  if (!length) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  if ((p[0] >= 0x09 && p[0] <= 0x0d) || p[0] == 0x20) return 1;
  if (length >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (length < 3) return 0;
  const uint32_t code = (p[0] == 0xe1 || p[0] == 0xe2 || p[0] == 0xe3 || p[0] == 0xef)
      ? (static_cast<uint32_t>(p[0] & 0x0f) << 12) |
        (static_cast<uint32_t>(p[1] & 0x3f) << 6) | (p[2] & 0x3f)
      : 0;
  if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
  if (code == 0x1680 || (code >= 0x2000 && code <= 0x200a) || code == 0x2028 ||
      code == 0x2029 || code == 0x202f || code == 0x205f || code == 0x3000 ||
      code == 0xfeff) return 3;
  return 0;
}

size_t JsWhitespaceAtEnd(const char* text, size_t length) {
  if (!length) return 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  const unsigned char last = p[length - 1];
  if ((last >= 0x09 && last <= 0x0d) || last == 0x20) return 1;
  if (length >= 2 && JsWhitespaceAtStart(text + length - 2, 2) == 2) return 2;
  if (length >= 3 && JsWhitespaceAtStart(text + length - 3, 3) == 3) return 3;
  return 0;
}

}  // namespace fortis::providers
