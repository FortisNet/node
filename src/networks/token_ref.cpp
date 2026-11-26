#include "token_ref.h"

#include "network_registry.h"
#include "price_history_batch_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::networks {

namespace {

// A bounded view of the caller's bytes.
struct Span {
  const char* data = nullptr;
  size_t length = 0;
};

bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_hex(unsigned char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }
// [1-9A-HJ-NP-Za-km-z]
bool is_base58(unsigned char c) {
  return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') ||
         (c >= 'P' && c <= 'Z') || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}
unsigned char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c;
}
unsigned char ascii_upper(unsigned char c) {
  return c >= 'a' && c <= 'z' ? static_cast<unsigned char>(c - 32) : c;
}

template <typename Predicate>
bool all_of(Span s, Predicate predicate) {
  for (size_t i = 0; i < s.length; ++i)
    if (!predicate(static_cast<unsigned char>(s.data[i]))) return false;
  return true;
}

bool equals(Span s, const char* literal) {
  const size_t n = strlen(literal);
  return s.length == n && memcmp(s.data, literal, n) == 0;
}

bool starts_with(Span s, const char* literal) {
  const size_t n = strlen(literal);
  return s.length >= n && memcmp(s.data, literal, n) == 0;
}

// Bounded string builder into a fixed field.
struct Builder {
  char* out;
  size_t capacity;  // including the terminator
  size_t used = 0;
  bool ok = true;
  Builder(char* output, size_t cap) : out(output), capacity(cap) { out[0] = '\0'; }
  void add(const char* data, size_t length) {
    if (!ok || length >= capacity - used) { ok = false; return; }
    memcpy(out + used, data, length);
    used += length;
    out[used] = '\0';
  }
  void add(const char* text) { add(text, strlen(text)); }
  void add(Span s) { add(s.data, s.length); }
  void add_lower(Span s) {
    for (size_t i = 0; i < s.length && ok; ++i) {
      const char c = static_cast<char>(ascii_lower(static_cast<unsigned char>(s.data[i])));
      add(&c, 1);
    }
  }
  void add_upper(Span s) {
    for (size_t i = 0; i < s.length && ok; ++i) {
      const char c = static_cast<char>(ascii_upper(static_cast<unsigned char>(s.data[i])));
      add(&c, 1);
    }
  }
};

bool set_field(char* field, size_t capacity, Span value, bool* flag = nullptr) {
  Builder b(field, capacity);
  b.add(value);
  if (flag) *flag = b.ok;
  return b.ok;
}

// ── TypeScript regular expressions (all ASCII) ──

bool evm_address(Span s) {  // /^0[xX][0-9a-fA-F]{40}$/
  return s.length == 42 && s.data[0] == '0' && (s.data[1] == 'x' || s.data[1] == 'X') &&
         all_of(Span{s.data + 2, 40}, is_hex);
}
bool tron_address(Span s) {  // /^T[base58]{33}$/
  return s.length == 34 && s.data[0] == 'T' && all_of(Span{s.data + 1, 33}, is_base58);
}
bool digits(Span s, size_t min, size_t max) {
  return s.length >= min && s.length <= max && all_of(s, is_digit);
}
bool spl_mint(Span s) { return s.length >= 32 && s.length <= 44 && all_of(s, is_base58); }
bool cardano_unit(Span s) { return s.length >= 56 && s.length <= 120 && all_of(s, is_hex); }
bool stellar_issuer(Span s) {  // /^G[A-Z2-7]{55}$/
  if (s.length != 56 || s.data[0] != 'G') return false;
  for (size_t i = 1; i < 56; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
  }
  return true;
}
bool stellar_code(Span s) { return s.length >= 1 && s.length <= 12 && all_of(s, is_alnum); }
bool xrpl_issuer(Span s) {  // /^r[base58]{24,34}$/
  return s.length >= 25 && s.length <= 35 && s.data[0] == 'r' &&
         all_of(Span{s.data + 1, s.length - 1}, is_base58);
}
bool xrpl_code(Span s) {  // /^([A-Za-z0-9]{1,3}|[0-9A-Fa-f]{40})$/
  return (s.length >= 1 && s.length <= 3 && all_of(s, is_alnum)) ||
         (s.length == 40 && all_of(s, is_hex));
}
bool ticker(Span s, size_t max) { return s.length >= 1 && s.length <= max && all_of(s, is_alnum); }
bool coingecko_id(Span s) {  // /^[a-z0-9][a-z0-9-]{0,99}$/
  if (s.length < 1 || s.length > 100) return false;
  for (size_t i = 0; i < s.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    const bool ok = (c >= 'a' && c <= 'z') || is_digit(c) || (i > 0 && c == '-');
    if (!ok) return false;
  }
  return true;
}
bool cosmos_denom(Span s) {  // /^[a-zA-Z][a-zA-Z0-9/:._-]{2,127}$/
  if (s.length < 3 || s.length > 128 || !is_alpha(static_cast<unsigned char>(s.data[0])))
    return false;
  for (size_t i = 1; i < s.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    if (!(is_alnum(c) || c == '/' || c == ':' || c == '.' || c == '_' || c == '-'))
      return false;
  }
  return true;
}
// [A-Za-z_][A-Za-z0-9_]*
bool move_identifier(Span s) {
  if (!s.length) return false;
  const unsigned char first = static_cast<unsigned char>(s.data[0]);
  if (!(is_alpha(first) || first == '_')) return false;
  for (size_t i = 1; i < s.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    if (!(is_alnum(c) || c == '_')) return false;
  }
  return true;
}

// String.prototype.toLowerCase restricted to what can reach an ASCII-only
// pattern: ASCII letters and U+212A KELVIN SIGN (-> 'k'). Any other non-ASCII
// code point lowercases to a non-ASCII string, so the result cannot match;
// false reports that.
bool js_lower_ascii(Span s, char* out, size_t capacity, size_t* length) {
  size_t used = 0;
  for (size_t i = 0; i < s.length;) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    char next;
    if (c < 0x80) { next = static_cast<char>(ascii_lower(c)); ++i; }
    else if (c == 0xe2 && i + 2 < s.length && static_cast<unsigned char>(s.data[i + 1]) == 0x84 &&
             static_cast<unsigned char>(s.data[i + 2]) == 0xaa) { next = 'k'; i += 3; }
    else return false;
    if (used + 1 >= capacity) return false;
    out[used++] = next;
  }
  out[used] = '\0';
  *length = used;
  return true;
}

// Unsigned decimal of a digit run without leading zeros (BigInt(x).toString()).
Span strip_leading_zeros(Span s) {
  size_t first = 0;
  while (first + 1 < s.length && s.data[first] == '0') ++first;
  return Span{s.data + first, s.length - first};
}

// /^\d{1,10}\.\d{1,10}\.\d{1,12}$/
bool hedera_entity_shape(Span s, Span parts[3]) {
  const size_t limits[3] = {10, 10, 12};
  size_t start = 0;
  for (int part = 0; part < 3; ++part) {
    size_t end = start;
    while (end < s.length && is_digit(static_cast<unsigned char>(s.data[end]))) ++end;
    const size_t n = end - start;
    if (n < 1 || n > limits[part]) return false;
    parts[part] = Span{s.data + start, n};
    if (part < 2) {
      if (end >= s.length || s.data[end] != '.') return false;
      start = end + 1;
    } else if (end != s.length) {
      return false;
    }
  }
  return true;
}

uint64_t hex_value(Span s) {
  uint64_t value = 0;
  for (size_t i = 0; i < s.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    value = (value << 4) | static_cast<uint64_t>(is_digit(c) ? c - '0' : ascii_lower(c) - 'a' + 10);
  }
  return value;
}

// The TypeScript canonicalHederaEntityId. `value` is trimmed.
bool hedera_entity_id(Span value, char* out, size_t capacity) {
  Span parts[3];
  if (hedera_entity_shape(value, parts)) {
    Builder b(out, capacity);
    for (int i = 0; i < 3; ++i) {
      if (i) b.add(".");
      b.add(strip_leading_zeros(parts[i]));
    }
    return b.ok;
  }
  // /^(?:0x)?([0-9a-fA-F]{40})$/ (lowercase x only)
  Span hex = value;
  if (hex.length == 42 && hex.data[0] == '0' && hex.data[1] == 'x') hex = Span{hex.data + 2, 40};
  if (hex.length != 40 || !all_of(hex, is_hex)) return false;
  char candidate[96];
  snprintf(candidate, sizeof(candidate), "%llu.%llu.%llu",
           static_cast<unsigned long long>(hex_value(Span{hex.data, 8})),
           static_cast<unsigned long long>(hex_value(Span{hex.data + 8, 16})),
           static_cast<unsigned long long>(hex_value(Span{hex.data + 24, 16})));
  Span check{candidate, strlen(candidate)};
  if (!hedera_entity_shape(check, parts)) return false;
  Builder b(out, capacity);
  b.add(check);
  return b.ok;
}

// The 0x... address of a Move type, canonicalized: leading zeros stripped,
// lowercase, "0x0" for zero. `hex` excludes the prefix.
bool canonical_move_address(Span hex, Builder& b) {
  size_t first = 0;
  while (first < hex.length && hex.data[first] == '0') ++first;
  b.add("0x");
  if (first == hex.length) b.add("0");
  else b.add_lower(Span{hex.data + first, hex.length - first});
  return b.ok;
}

// /^(0x|0[xX])[0-9a-fA-F]{1,64}::ident::ident$/ (prefix per `upper_x`).
bool move_coin_type(Span s, bool upper_x, char* out, size_t capacity) {
  if (s.length < 3 || s.data[0] != '0' || !(s.data[1] == 'x' || (upper_x && s.data[1] == 'X')))
    return false;
  size_t end = 2;
  while (end < s.length && is_hex(static_cast<unsigned char>(s.data[end]))) ++end;
  const Span hex{s.data + 2, end - 2};
  if (hex.length < 1 || hex.length > 64) return false;
  if (s.length - end < 2 || s.data[end] != ':' || s.data[end + 1] != ':') return false;
  const size_t module_start = end + 2;
  const char* sep = nullptr;
  for (size_t i = module_start; i + 1 < s.length; ++i)
    if (s.data[i] == ':' && s.data[i + 1] == ':') { sep = s.data + i; break; }
  if (!sep) return false;
  const Span module{s.data + module_start, static_cast<size_t>(sep - (s.data + module_start))};
  const Span name{sep + 2, static_cast<size_t>(s.data + s.length - (sep + 2))};
  if (!move_identifier(module) || !move_identifier(name)) return false;
  Builder b(out, capacity);
  canonical_move_address(hex, b);
  b.add("::"); b.add(module); b.add("::"); b.add(name);
  return b.ok;
}

// canonicalAptosTokenIdentifier(raw).value; raw is already trimmed.
bool aptos_identifier(Span s, char* out, size_t capacity) {
  if (move_coin_type(s, false, out, capacity)) return true;
  if (s.length < 3 || s.length > 66 || s.data[0] != '0' || s.data[1] != 'x' ||
      !all_of(Span{s.data + 2, s.length - 2}, is_hex)) return false;
  Builder b(out, capacity);
  return canonical_move_address(Span{s.data + 2, s.length - 2}, b);
}

// canonicalSuiCoinType(raw); raw is already trimmed.
bool sui_coin_type(Span s, char* out, size_t capacity) {
  return move_coin_type(s, true, out, capacity);
}

bool chain_shaped(Span s) {
  char scratch[96];
  return evm_address(s) || tron_address(s) || spl_mint(s) || cardano_unit(s) ||
         hedera_entity_id(s, scratch, sizeof(scratch)) || stellar_issuer(s) ||
         xrpl_issuer(s);
}

enum class FamilyKind : uint8_t { kNone, kErc20, kSpl, kAsa, kCardano, kStellar, kHts, kXrpl,
                                  kTrc20, kBittensorAlpha, kAptos, kSui, kDenom };

FamilyKind family_kind(const char* family) {
  if (!family) return FamilyKind::kNone;
  struct Entry { const char* family; FamilyKind kind; };
  static const Entry kEntries[] = {
    {"evm", FamilyKind::kErc20}, {"vechain", FamilyKind::kErc20},
    {"solana", FamilyKind::kSpl}, {"algorand", FamilyKind::kAsa},
    {"cardano", FamilyKind::kCardano}, {"stellar", FamilyKind::kStellar},
    {"hedera", FamilyKind::kHts}, {"xrpl", FamilyKind::kXrpl},
    {"tron", FamilyKind::kTrc20}, {"bittensor", FamilyKind::kBittensorAlpha},
    {"aptos", FamilyKind::kAptos}, {"sui", FamilyKind::kSui}, {"cosmos", FamilyKind::kDenom},
  };
  for (const Entry& entry : kEntries)
    if (strcmp(entry.family, family) == 0) return entry.kind;
  return FamilyKind::kNone;
}

const char* family_prefix(FamilyKind kind) {
  switch (kind) {
    case FamilyKind::kErc20: return "erc20";
    case FamilyKind::kSpl: return "spl";
    case FamilyKind::kAsa: return "asa";
    case FamilyKind::kCardano: return "cardano";
    case FamilyKind::kStellar: return "stellar";
    case FamilyKind::kHts: return "hts";
    case FamilyKind::kXrpl: return "xrpl";
    case FamilyKind::kTrc20: return "trc20";
    case FamilyKind::kBittensorAlpha: return "bittensor-alpha";
    case FamilyKind::kAptos: return "aptos";
    case FamilyKind::kSui: return "sui";
    case FamilyKind::kDenom: return "denom";
    case FamilyKind::kNone: break;
  }
  return nullptr;
}

bool family_prefix_known(const char* prefix) {
  static const char* const kPrefixes[] = {
    "erc20", "trc10", "trc20", "spl", "asa", "cardano", "stellar", "hts", "xrpl",
    "bittensor-alpha", "aptos", "sui", "denom",
  };
  for (const char* known : kPrefixes) if (strcmp(known, prefix) == 0) return true;
  return false;
}

bool family_is(const TokenNetwork* network, const char* family) {
  return network && network->family && strcmp(network->family, family) == 0;
}

bool finish(TokenRef* out, TokenRefKind kind, bool ok) {
  out->kind = kind;
  return ok;
}

// symbolRef(network, raw, base).
bool symbol_ref(const TokenNetwork* network, Span raw, TokenRef* out) {
  if (!ticker(raw, 12)) return false;
  char symbol[16];
  Builder s(symbol, sizeof(symbol));
  s.add_upper(raw);
  if (network && network->native_symbol) {
    char native[64];
    Builder n(native, sizeof(native));
    n.add_upper(Span{network->native_symbol, strlen(network->native_symbol)});
    if (n.ok && strcmp(native, symbol) == 0) {
      bool ok = set_field(out->token_id, sizeof(out->token_id), Span{"native", 6}) &&
                set_field(out->symbol, sizeof(out->symbol), Span{symbol, strlen(symbol)},
                          &out->has_symbol);
      if (network->native_coingecko_id) {
        const Span id{network->native_coingecko_id, strlen(network->native_coingecko_id)};
        ok = ok && set_field(out->coingecko_id, sizeof(out->coingecko_id), id,
                             &out->has_coingecko_id) &&
             set_field(out->price_id, sizeof(out->price_id), id, &out->has_price_id);
      }
      return finish(out, TokenRefKind::kNative, ok);
    }
  }
  Builder id(out->token_id, sizeof(out->token_id));
  id.add("symbol:"); id.add(symbol);
  const bool ok = id.ok &&
      set_field(out->symbol, sizeof(out->symbol), Span{symbol, strlen(symbol)}, &out->has_symbol);
  return finish(out, TokenRefKind::kSymbol, ok);
}

bool coingecko_ref(Span lowered, TokenRef* out) {
  if (!coingecko_id(lowered)) return false;
  Builder id(out->token_id, sizeof(out->token_id));
  id.add("cg:"); id.add(lowered);
  const bool ok = id.ok &&
      set_field(out->coingecko_id, sizeof(out->coingecko_id), lowered, &out->has_coingecko_id) &&
      set_field(out->price_id, sizeof(out->price_id), lowered, &out->has_price_id);
  return finish(out, TokenRefKind::kCoinGecko, ok);
}

// Sets token_id = prefix + ":" + value, and address = value.
bool prefixed_id(TokenRef* out, const char* prefix, Span value) {
  Builder id(out->token_id, sizeof(out->token_id));
  id.add(prefix); id.add(":"); id.add(value);
  return id.ok && set_field(out->address, sizeof(out->address), value, &out->has_address);
}

bool price_id(TokenRef* out, const char* a, const char* b, Span c) {
  Builder p(out->price_id, sizeof(out->price_id));
  if (a) p.add(a);
  p.add(b); p.add(c);
  out->has_price_id = p.ok;
  return p.ok;
}

// The family switch. Returns 1 for a match, 0 for "break" (fall through to the
// bare forms), -1 for an internal bound failure.
int family_ref(const TokenNetwork* network, FamilyKind kind, Span body, TokenRef* out) {
  char scratch[kTokenRefMaxInputBytes + 8];
  switch (kind) {
    case FamilyKind::kErc20: {
      if (!evm_address(body)) return 0;
      char address[43];
      Builder a(address, sizeof(address));
      a.add_lower(body);
      const Span value{address, 42};
      const bool ok = prefixed_id(out, "erc20", value) &&
          price_id(out, network->id, ":erc20:", value);
      return finish(out, TokenRefKind::kErc20, ok) ? 1 : -1;
    }
    case FamilyKind::kTrc20: {
      if (!tron_address(body)) return 0;
      const bool ok = prefixed_id(out, "trc20", body) &&
          price_id(out, network->id, ":trc20:", body);
      return finish(out, TokenRefKind::kTrc20, ok) ? 1 : -1;
    }
    case FamilyKind::kSpl: {
      if (!spl_mint(body)) return 0;
      const bool ok = prefixed_id(out, "spl", body) && price_id(out, nullptr, "spl:", body);
      return finish(out, TokenRefKind::kSpl, ok) ? 1 : -1;
    }
    case FamilyKind::kAsa: {
      if (!digits(body, 1, 20)) return 0;
      const bool ok = prefixed_id(out, "asa", body) && price_id(out, nullptr, "asa:", body);
      return finish(out, TokenRefKind::kAsa, ok) ? 1 : -1;
    }
    case FamilyKind::kDenom: {
      if (!cosmos_denom(body)) return 0;
      const bool ibc_hash = body.length == 68 && starts_with(body, "ibc/") &&
          all_of(Span{body.data + 4, 64}, is_hex);
      if (starts_with(body, "ibc/") && !ibc_hash) return 0;
      Builder d(scratch, sizeof(scratch));
      if (ibc_hash) { d.add("ibc/"); d.add_upper(Span{body.data + 4, 64}); }
      else d.add(body);
      const bool ok = d.ok && prefixed_id(out, "denom", Span{scratch, d.used});
      return finish(out, TokenRefKind::kDenom, ok) ? 1 : -1;
    }
    case FamilyKind::kCardano: {
      if (!cardano_unit(body)) return 0;
      Builder u(scratch, sizeof(scratch));
      u.add_lower(body);
      const bool ok = u.ok && prefixed_id(out, "cardano", Span{scratch, u.used});
      return finish(out, TokenRefKind::kCardano, ok) ? 1 : -1;
    }
    case FamilyKind::kStellar: {
      const char* dash = static_cast<const char*>(memchr(body.data, '-', body.length));
      if (!dash) {
        if (!stellar_issuer(body)) return 0;
        const bool ok = prefixed_id(out, "stellar", body) &&
            set_field(out->issuer, sizeof(out->issuer), body, &out->has_issuer);
        return finish(out, TokenRefKind::kStellar, ok) ? 1 : -1;
      }
      const Span code{body.data, static_cast<size_t>(dash - body.data)};
      const Span issuer{dash + 1, body.length - code.length - 1};
      if (!stellar_code(code) || !stellar_issuer(issuer)) return 0;
      Builder c(scratch, sizeof(scratch));
      c.add_upper(code); c.add("-"); c.add(issuer);
      const bool ok = c.ok && prefixed_id(out, "stellar", Span{scratch, c.used}) &&
          set_field(out->symbol, sizeof(out->symbol), Span{scratch, code.length},
                    &out->has_symbol) &&
          set_field(out->issuer, sizeof(out->issuer), issuer, &out->has_issuer);
      return finish(out, TokenRefKind::kStellar, ok) ? 1 : -1;
    }
    case FamilyKind::kHts: {
      if (!hedera_entity_id(body, scratch, sizeof(scratch))) return 0;
      const bool ok = prefixed_id(out, "hts", Span{scratch, strlen(scratch)});
      return finish(out, TokenRefKind::kHts, ok) ? 1 : -1;
    }
    case FamilyKind::kXrpl: {
      const char* dot = static_cast<const char*>(memchr(body.data, '.', body.length));
      if (!dot) {
        if (!xrpl_issuer(body)) return 0;
        const bool ok = prefixed_id(out, "xrpl", body) &&
            set_field(out->issuer, sizeof(out->issuer), body, &out->has_issuer);
        return finish(out, TokenRefKind::kXrpl, ok) ? 1 : -1;
      }
      const Span code{body.data, static_cast<size_t>(dot - body.data)};
      const Span issuer{dot + 1, body.length - code.length - 1};
      if (!xrpl_code(code) || !xrpl_issuer(issuer)) return 0;
      Builder c(scratch, sizeof(scratch));
      c.add_upper(code); c.add("."); c.add(issuer);
      const bool ok = c.ok && prefixed_id(out, "xrpl", Span{scratch, c.used}) &&
          set_field(out->symbol, sizeof(out->symbol), Span{scratch, code.length},
                    &out->has_symbol) &&
          set_field(out->issuer, sizeof(out->issuer), issuer, &out->has_issuer);
      return finish(out, TokenRefKind::kXrpl, ok) ? 1 : -1;
    }
    case FamilyKind::kBittensorAlpha: {
      if (!digits(body, 1, 5)) return 0;
      char value[8];
      long parsed = 0;  // at most five digits
      for (size_t i = 0; i < body.length; ++i) parsed = parsed * 10 + (body.data[i] - '0');
      if (parsed <= 0 || parsed > 65535) return 0;
      snprintf(value, sizeof(value), "%ld", parsed);
      const bool ok = prefixed_id(out, "bittensor-alpha", Span{value, strlen(value)});
      return finish(out, TokenRefKind::kBittensorAlpha, ok) ? 1 : -1;
    }
    case FamilyKind::kAptos: {
      if (!aptos_identifier(body, scratch, sizeof(scratch))) return 0;
      const bool ok = prefixed_id(out, "aptos", Span{scratch, strlen(scratch)});
      return finish(out, TokenRefKind::kAptos, ok) ? 1 : -1;
    }
    case FamilyKind::kSui: {
      if (!sui_coin_type(body, scratch, sizeof(scratch))) return 0;
      const bool ok = prefixed_id(out, "sui", Span{scratch, strlen(scratch)});
      return finish(out, TokenRefKind::kSui, ok) ? 1 : -1;
    }
    case FamilyKind::kNone:
      break;
  }
  return 0;
}

bool contains_line_terminator(Span s) {
  for (size_t i = 0; i < s.length; ++i) {
    const unsigned char c = static_cast<unsigned char>(s.data[i]);
    if (c == '\n' || c == '\r') return true;
    if (c == 0xe2 && i + 2 < s.length && static_cast<unsigned char>(s.data[i + 1]) == 0x80 &&
        (static_cast<unsigned char>(s.data[i + 2]) == 0xa8 ||
         static_cast<unsigned char>(s.data[i + 2]) == 0xa9)) return true;
  }
  return false;
}

Span js_trim(Span s) {
  size_t begin = 0, end = 0;
  fortis::codec::history_batch::JsTrim(s.data, s.length, &begin, &end);
  return Span{s.data + begin, end - begin};
}

}  // namespace

const char* TokenRefKindName(TokenRefKind kind) {
  switch (kind) {
    case TokenRefKind::kNative: return "native";
    case TokenRefKind::kErc20: return "erc20";
    case TokenRefKind::kTrc10: return "trc10";
    case TokenRefKind::kTrc20: return "trc20";
    case TokenRefKind::kSpl: return "spl";
    case TokenRefKind::kAsa: return "asa";
    case TokenRefKind::kCardano: return "cardano";
    case TokenRefKind::kStellar: return "stellar";
    case TokenRefKind::kHts: return "hts";
    case TokenRefKind::kXrpl: return "xrpl";
    case TokenRefKind::kBittensorAlpha: return "bittensor-alpha";
    case TokenRefKind::kAptos: return "aptos";
    case TokenRefKind::kSui: return "sui";
    case TokenRefKind::kDenom: return "denom";
    case TokenRefKind::kSymbol: return "symbol";
    case TokenRefKind::kCoinGecko: return "coingecko";
  }
  return "";
}

bool ResolveTokenNetwork(const char* id, size_t length, TokenNetwork* out) {
  if (!id || !out) return false;
  *out = TokenNetwork{};
  NativeNetworkRegistry registry;
  size_t count = 0;
  const NativeNetworkConfig* networks = registry.list(&count);
  const Span value{id, length};
  for (size_t i = 0; i < count; ++i) {
    if (!equals(value, networks[i].id)) continue;
    out->id = networks[i].id; out->family = networks[i].family;
    out->native_symbol = networks[i].native_symbol;
    out->native_coingecko_id = networks[i].native_coingecko_id;
    return true;
  }
  // /^eip155:(\d+)$/
  if (!starts_with(value, "eip155:") || value.length == 7 ||
      !all_of(Span{id + 7, length - 7}, is_digit)) return false;
  // Number(digits): strtod is correctly rounded like ToNumber.
  if (length - 7 > 400) return false;
  char number[408];
  memcpy(number, id + 7, length - 7);
  number[length - 7] = '\0';
  const double chain = strtod(number, nullptr);
  for (size_t i = 0; i < count; ++i) {
    if (!networks[i].has_evm_chain_id ||
        static_cast<double>(networks[i].evm_chain_id) != chain) continue;
    out->id = networks[i].id; out->family = networks[i].family;
    out->native_symbol = networks[i].native_symbol;
    out->native_coingecko_id = networks[i].native_coingecko_id;
    return true;
  }
  if (length > kTokenNetworkMaxGenericIdBytes) return false;
  memcpy(out->generic_id, id, length);
  out->generic_id[length] = '\0';
  out->id = out->generic_id;
  out->family = "evm";
  return true;
}

bool ParseTokenRef(const TokenNetwork* network, const char* raw, size_t length,
                   TokenRef* out) {
  if (!raw || !out) return false;
  *out = TokenRef{};
  if (!fortis::codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(raw), length))
    return false;
  const Span input = js_trim(Span{raw, length});
  if (!input.length || input.length > kTokenRefMaxInputBytes) return false;

  // ── Universal forms ──
  char lowered[kTokenRefMaxInputBytes + 1];
  size_t lowered_length = 0;
  const bool lowered_ok = js_lower_ascii(input, lowered, sizeof(lowered), &lowered_length);
  if (lowered_ok && equals(Span{lowered, lowered_length}, "native")) {
    if (!network) return false;
    bool ok = set_field(out->token_id, sizeof(out->token_id), Span{"native", 6});
    if (network->native_symbol)
      ok = ok && set_field(out->symbol, sizeof(out->symbol),
                           Span{network->native_symbol, strlen(network->native_symbol)},
                           &out->has_symbol);
    if (network->native_coingecko_id) {
      const Span cg{network->native_coingecko_id, strlen(network->native_coingecko_id)};
      ok = ok && set_field(out->coingecko_id, sizeof(out->coingecko_id), cg,
                           &out->has_coingecko_id) &&
           set_field(out->price_id, sizeof(out->price_id), cg, &out->has_price_id);
    }
    return finish(out, TokenRefKind::kNative, ok);
  }

  if (family_is(network, "aptos") && starts_with(input, "0x")) {
    char value[kTokenRefMaxInputBytes + 8];
    if (!aptos_identifier(input, value, sizeof(value))) return false;
    return finish(out, TokenRefKind::kAptos,
                  prefixed_id(out, "aptos", Span{value, strlen(value)}));
  }
  if (family_is(network, "sui") && (starts_with(input, "0x") || starts_with(input, "0X"))) {
    char value[kTokenRefMaxInputBytes + 8];
    if (!sui_coin_type(input, value, sizeof(value))) return false;
    return finish(out, TokenRefKind::kSui,
                  prefixed_id(out, "sui", Span{value, strlen(value)}));
  }

  // /^([a-z0-9-]+):(.+)$/i
  bool prefixed = false;
  char prefix[kTokenRefMaxInputBytes + 1];
  Span rest{};
  {
    size_t colon = 0;
    while (colon < input.length) {
      const unsigned char c = static_cast<unsigned char>(input.data[colon]);
      if (!(is_alnum(c) || c == '-')) break;
      ++colon;
    }
    if (colon > 0 && colon < input.length && input.data[colon] == ':' &&
        colon + 1 < input.length) {
      const Span tail{input.data + colon + 1, input.length - colon - 1};
      if (!contains_line_terminator(tail)) {
        prefixed = true;
        for (size_t i = 0; i < colon; ++i)
          prefix[i] = static_cast<char>(ascii_lower(static_cast<unsigned char>(input.data[i])));
        prefix[colon] = '\0';
        rest = js_trim(tail);
      }
    }
  }

  if (prefixed && (strcmp(prefix, "symbol") == 0 || strcmp(prefix, "sym") == 0))
    return symbol_ref(network, rest, out);
  if (prefixed && (strcmp(prefix, "coingecko") == 0 || strcmp(prefix, "cg") == 0)) {
    char id[kTokenRefMaxInputBytes + 1];
    size_t id_length = 0;
    if (!js_lower_ascii(rest, id, sizeof(id), &id_length)) return false;
    return coingecko_ref(Span{id, id_length}, out);
  }

  // ── Family forms ──
  const FamilyKind kind = network ? family_kind(network->family) : FamilyKind::kNone;
  const bool tron_trc10 = family_is(network, "tron") &&
      ((prefixed && strcmp(prefix, "trc10") == 0) || (!prefixed && digits(input, 1, 20)));
  if (tron_trc10) {
    const Span asset = prefixed ? rest : input;
    if (!digits(asset, 1, 20)) return false;
    const Span id = strip_leading_zeros(asset);
    if (equals(id, "0")) return false;
    const bool ok = prefixed_id(out, "trc10", id) && price_id(out, nullptr, "tron:trc10:", id);
    return finish(out, TokenRefKind::kTrc10, ok);
  }
  const char* kind_prefix = family_prefix(kind);
  if (prefixed && family_prefix_known(prefix) &&
      (kind == FamilyKind::kNone || strcmp(prefix, kind_prefix) != 0)) return false;
  Span body{};
  if (prefixed && kind_prefix && strcmp(prefix, kind_prefix) == 0) body = rest;
  else if (!prefixed) body = input;
  else return false;

  const int matched = family_ref(network, kind, body, out);
  if (matched > 0) return true;
  if (matched < 0) return false;
  *out = TokenRef{};

  // ── Bare fallbacks ──
  if (prefixed) return false;
  if (ticker(input, 5)) return symbol_ref(network, input, out);
  if (kind == FamilyKind::kNone && !chain_shaped(input) && lowered_ok)
    return coingecko_ref(Span{lowered, lowered_length}, out);
  return false;
}

bool CanonicalHederaEntityId(const char* input, size_t length, char* out, size_t capacity) {
  return hedera_entity_id(js_trim(Span{input, length}), out, capacity);
}

}  // namespace fortis::networks
