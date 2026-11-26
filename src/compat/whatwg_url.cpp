#include "whatwg_url.h"

#include <string.h>

// A port of Ada v2.9.2's parse_url_impl<url_aggregator> (src/parser.cpp) and
// the url_aggregator buffer operations it calls (src/url_aggregator.cpp,
// include/ada/url_aggregator-inl.h, helpers.cpp, checkers.cpp, unicode.cpp,
// serializers.cpp). The aggregator edits one href buffer in place and keeps
// component offsets beside it; porting that model, rather than the spec's
// record, keeps Ada's observable quirks. The accessors mirror Node.js 22
// lib/internal/url.js getters over the same offsets.
namespace fortis::compat::url {
namespace {

constexpr uint32_t kOmit = kOmitted;

enum SchemeType : uint8_t { HTTP = 0, NOT_SPECIAL = 1, HTTPS = 2, WS = 3, FTP = 4, WSS = 5, FILE = 6 };
constexpr uint16_t kSpecialPorts[] = {80, 0, 443, 80, 21, 443, 0, 0};

SchemeType scheme_type(const char* s, size_t n) {
  struct Entry { const char* name; SchemeType type; };
  static const Entry kEntries[] = {{"http", HTTP}, {"https", HTTPS}, {"ws", WS},
                                   {"ftp", FTP},   {"wss", WSS},     {"file", FILE}};
  for (const Entry& e : kEntries)
    if (strlen(e.name) == n && memcmp(e.name, s, n) == 0) return e.type;
  return NOT_SPECIAL;
}

// ---- character classes (unicode.cpp, checkers.cpp) -------------------------
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) {
  const char l = static_cast<char>(c | 0x20);
  return l >= 'a' && l <= 'z';
}
bool is_alnum_plus(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' ||
         c == '-' || c == '.';
}
bool is_ascii_hex_digit(char c) {
  return is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
unsigned hex_value(char c) {
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}
bool is_c0_control_or_space(char c) { return static_cast<unsigned char>(c) <= ' '; }
bool is_tab_or_newline(char c) { return c == '\t' || c == '\n' || c == '\r'; }

bool is_forbidden_host(unsigned char c) {
  switch (c) {
    case '\0': case '\t': case '\n': case '\r': case ' ': case '#': case '/': case ':':
    case '<': case '>': case '?': case '@': case '[': case '\\': case ']': case '^': case '|':
      return true;
    default:
      return false;
  }
}
// unicode.cpp is_forbidden_domain_code_point_table (note: 0xFF is not set).
bool is_forbidden_domain(unsigned char c) {
  return is_forbidden_host(c) || c == '%' || c <= 32 || (c >= 127 && c < 255);
}

enum EncodeSet : uint8_t { kC0, kFragment, kQuery, kSpecialQuery, kPath, kUserinfo };
bool in_set(EncodeSet set, unsigned char c) {
  if (c < 0x20 || c > 0x7e) return true;
  const char* extra;
  switch (set) {
    case kC0: extra = ""; break;
    case kFragment: extra = " \"<>`"; break;
    case kQuery: extra = " \"#<>"; break;
    case kSpecialQuery: extra = " \"#'<>"; break;
    case kPath: extra = " \"#<>?`{}"; break;
    case kUserinfo: extra = " \"#/:;<=>?@[\\]^`{|}"; break;
    default: return false;
  }
  return c != 0 && strchr(extra, c) != nullptr;
}

bool is_windows_drive_letter(const char* s, size_t n) {
  return n >= 2 && is_alpha(s[0]) && (s[1] == ':' || s[1] == '|') &&
         (n == 2 || s[2] == '/' || s[2] == '\\' || s[2] == '?' || s[2] == '#');
}
bool is_normalized_windows_drive_letter(const char* s, size_t n) {
  return n >= 2 && is_alpha(s[0]) && s[1] == ':';
}
bool ieq_ascii(const char* s, size_t n, const char* lit) {
  if (strlen(lit) != n) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (c != lit[i]) return false;
  }
  return true;
}
bool is_double_dot(const char* s, size_t n) {
  return ieq_ascii(s, n, "..") || ieq_ascii(s, n, "%2e.") || ieq_ascii(s, n, ".%2e") ||
         ieq_ascii(s, n, "%2e%2e");
}
bool is_single_dot(const char* s, size_t n) {
  return (n == 1 && s[0] == '.') || (n == 3 && s[0] == '%' && s[1] == '2' &&
                                     (s[2] == 'e' || s[2] == 'E'));
}

uint8_t path_signature(const char* s, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c <= 0x20 || c == 0x22 || c == 0x23 || c == 0x3c || c == 0x3e || c == 0x3f ||
        c == 0x60 || c == 0x7b || c == 0x7d || c > 0x7e) acc |= 1;
    else if (c == 0x25) acc |= 8;
    else if (c == 0x2e) acc |= 4;
    else if (c == 0x5c) acc |= 2;
  }
  return acc;
}

size_t find_char(const char* s, size_t n, size_t from, char c) {
  for (size_t i = from; i < n; ++i) if (s[i] == c) return i;
  return SIZE_MAX;
}
size_t rfind_char(const char* s, size_t n, char c) {
  for (size_t i = n; i-- > 0;) if (s[i] == c) return i;
  return SIZE_MAX;
}

// A bounded byte string (a std::string stand-in for temporaries).
struct Str {
  char* data;
  size_t cap;
  size_t len;
  bool ok;
  void Clear() { len = 0; }
  void Push(char c) {
    if (len < cap) data[len++] = c;
    else ok = false;
  }
  void Append(const char* s, size_t n) {
    if (len + n > cap) { ok = false; return; }
    memmove(data + len, s, n);
    len += n;
  }
};

void append_hex_escape(Str* out, unsigned char c) {
  static const char kHex[] = "0123456789ABCDEF";
  out->Push('%');
  out->Push(kHex[c >> 4]);
  out->Push(kHex[c & 15]);
}

void percent_encode_into(Str* out, const char* s, size_t n, EncodeSet set) {
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (in_set(set, c)) append_hex_escape(out, c);
    else out->Push(static_cast<char>(c));
  }
}

bool needs_encoding(const char* s, size_t n, EncodeSet set) {
  for (size_t i = 0; i < n; ++i) if (in_set(set, static_cast<unsigned char>(s[i]))) return true;
  return false;
}

// ---- checkers::is_ipv4 -----------------------------------------------------
bool looks_ipv4(const char* s, size_t n) {
  if (n == 0) return false;
  char last = s[n - 1];
  if (last == '.') {
    --n;
    if (n == 0) return false;
    last = s[n - 1];
  }
  if (!((last >= '0' && last <= '9') || (last >= 'a' && last <= 'f') || last == 'x'))
    return false;
  size_t dot = rfind_char(s, n, '.');
  const char* v = dot == SIZE_MAX ? s : s + dot + 1;
  const size_t vn = dot == SIZE_MAX ? n : n - dot - 1;
  bool all_digits = true;
  for (size_t i = 0; i < vn; ++i) all_digits = all_digits && is_digit(v[i]);
  if (all_digits) return true;
  if (vn == 1) return false;
  if (!(v[0] == '0' && v[1] == 'x')) return false;
  if (vn == 2) return true;
  for (size_t i = 2; i < vn; ++i)
    if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return false;
  return true;
}

// std::from_chars for uint32_t in `base`: consumed count and status.
enum class FromChars { kOk, kInvalid, kRange };
FromChars from_chars_u32(const char* s, size_t n, int base, uint32_t* value, size_t* consumed) {
  uint64_t result = 0;
  size_t i = 0;
  bool overflow = false;
  for (; i < n; ++i) {
    const char c = s[i];
    int digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
    else break;
    if (digit >= base) break;
    result = result * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
    if (result > 0xffffffffull) { overflow = true; result = 0xffffffffull + 1; }
  }
  *consumed = i;
  if (i == 0) return FromChars::kInvalid;
  if (overflow) return FromChars::kRange;
  *value = static_cast<uint32_t>(result);
  return FromChars::kOk;
}

// -- helpers::get_host_delimiter_location ------------------------------------
size_t host_delimiter_location(bool special, const char* v, size_t n, bool* found_colon) {
  *found_colon = false;
  size_t location = 0;
  while (location < n) {
    const char c = v[location];
    if (c == ':' || c == '/' || c == '?' || c == '[' || (special && c == '\\')) {
      if (c == '[') {
        const size_t close = find_char(v, n, location, ']');
        if (close == SIZE_MAX) return n;
        location = close;
        continue;
      }
      *found_colon = c == ':';
      return location;
    }
    ++location;
  }
  return n;
}

// ---- the aggregator --------------------------------------------------------
struct Agg {
  char* buf = nullptr;
  uint32_t len = 0;
  uint32_t cap = 0;
  bool ok = true;  // buffer bound not exceeded
  uint32_t protocol_end = 0, username_end = 0, host_start = 0, host_end = 0, port = kOmit,
           pathname_start = 0, search_start = kOmit, hash_start = kOmit;
  SchemeType type = NOT_SPECIAL;
  bool has_opaque_path = false;
  bool is_valid = true;
  HostType host_type = HostType::kNone;
  UrlScratch* s = nullptr;

  bool is_special() const { return type != NOT_SPECIAL; }
  uint16_t default_port() const { return kSpecialPorts[type]; }

  // -- buffer primitives (std::string insert/erase/replace) --
  void insert(uint32_t pos, const char* data, size_t n) {
    if (!n) return;
    if (len + n > cap || pos > len) { ok = false; return; }
    char* tmp = nullptr;
    if (data >= buf && data < buf + cap) {  // source aliases the buffer
      if (n > sizeof(s->other)) { ok = false; return; }
      memcpy(s->other, data, n);
      tmp = s->other;
    }
    memmove(buf + pos + n, buf + pos, len - pos);
    memcpy(buf + pos, tmp ? tmp : data, n);
    len += static_cast<uint32_t>(n);
    buf[len] = '\0';
  }
  void insert(uint32_t pos, const char* text) { insert(pos, text, strlen(text)); }
  void erase(uint32_t pos, uint32_t n) {
    if (pos > len) return;
    if (n > len - pos) n = len - pos;
    memmove(buf + pos, buf + pos + n, len - pos - n);
    len -= n;
    buf[len] = '\0';
  }
  void resize_down(uint32_t n) {
    if (n < len) { len = n; buf[len] = '\0'; }
  }
  void append(const char* data, size_t n) { insert(len, data, n); }
  uint32_t replace_and_resize(uint32_t start, uint32_t end, const char* input, size_t n) {
    const uint32_t current = end - start;
    const uint32_t size = static_cast<uint32_t>(n);
    // Equivalent to Ada's four-way replace: erase the old span, insert new.
    if (input >= buf && input < buf + cap) {
      if (n > sizeof(s->other)) { ok = false; return 0; }
      memcpy(s->other, input, n);
      input = s->other;
    }
    erase(start, current);
    insert(start, input, n);
    return size - current;
  }
  void shift_after_host_end(uint32_t d) {
    pathname_start += d;
    if (search_start != kOmit) search_start += d;
    if (hash_start != kOmit) hash_start += d;
  }

  // -- getters (url_aggregator) --
  View get_pathname() const {
    uint32_t end = len;
    if (search_start != kOmit) end = search_start;
    else if (hash_start != kOmit) end = hash_start;
    return View{buf + pathname_start, end - pathname_start};
  }
  uint32_t get_pathname_length() const { return static_cast<uint32_t>(get_pathname().length); }
  View get_hostname() const {
    uint32_t start = host_start;
    if (host_end > host_start && buf[host_start] == '@') start++;
    return View{buf + start, host_end - start};
  }
  View get_host() const {
    uint32_t start = host_start;
    if (host_end > host_start && buf[host_start] == '@') start++;
    if (start == host_end) return View{buf + start, 0};
    return View{buf + start, pathname_start - start};
  }
  View get_search() const {
    if (search_start == kOmit) return View{buf, 0};
    uint32_t end = len;
    if (hash_start != kOmit) end = hash_start;
    if (end - search_start <= 1) return View{buf, 0};
    return View{buf + search_start, end - search_start};
  }
  View get_protocol() const { return View{buf, protocol_end}; }

  bool has_authority() const {
    return protocol_end + 2 <= host_start && buf[protocol_end] == '/' &&
           buf[protocol_end + 1] == '/';
  }
  bool has_non_empty_username() const { return protocol_end + 2 < username_end; }
  bool has_non_empty_password() const { return host_start - username_end > 0; }
  bool has_password() const { return host_start > username_end && buf[username_end] == ':'; }
  bool has_credentials() const { return has_non_empty_username() || has_non_empty_password(); }
  bool has_hostname() const { return has_authority(); }
  bool has_port() const { return has_hostname() && pathname_start != host_end; }
  bool cannot_have_credentials_or_port() const { return type == FILE || host_start == host_end; }
  bool has_dash_dot() const {
    return pathname_start == host_end + 2 && !has_opaque_path && buf[host_end] == '/' &&
           buf[host_end + 1] == '.';
  }
  bool is_at_path() const { return len == pathname_start; }

  void add_authority_slashes_if_needed() {
    if (has_authority()) return;
    insert(protocol_end, "//");
    username_end += 2;
    host_start += 2;
    host_end += 2;
    shift_after_host_end(2);
  }
  void delete_dash_dot() {
    erase(host_end, 2);
    pathname_start -= 2;
    if (search_start != kOmit) search_start -= 2;
    if (hash_start != kOmit) hash_start -= 2;
  }

  // -- scheme --
  void shift_all_after_protocol(uint32_t d) {
    username_end += d;
    host_start += d;
    host_end += d;
    shift_after_host_end(d);
  }
  void set_scheme_from_view_with_colon(const char* v, size_t n) {
    const uint32_t diff = static_cast<uint32_t>(n) - protocol_end;
    if (len == 0) {
      append(v, n);
    } else {
      erase(0, protocol_end);
      insert(0, v, n);
    }
    protocol_end += diff;
    shift_all_after_protocol(diff);
  }
  void set_scheme(const char* v, size_t n) {
    const uint32_t diff = static_cast<uint32_t>(n) - protocol_end + 1;
    type = scheme_type(v, n);
    if (len == 0) {
      append(v, n);
      append(":", 1);
    } else {
      erase(0, protocol_end);
      insert(0, ":", 1);
      insert(0, v, n);
    }
    protocol_end = static_cast<uint32_t>(n + 1);
    shift_all_after_protocol(diff);
  }
  void clear_port() {
    if (port == kOmit) return;
    const uint32_t length = pathname_start - host_end;
    erase(host_end, length);
    pathname_start -= length;
    if (search_start != kOmit) search_start -= length;
    if (hash_start != kOmit) hash_start -= length;
    port = kOmit;
  }
  // parse_scheme_with_colon<has_state_override>
  bool parse_scheme_with_colon(const char* with_colon, size_t n, bool override) {
    const char* input = with_colon;
    const size_t input_n = n - 1;
    const SchemeType parsed = scheme_type(input, input_n);
    const bool is_input_special = parsed != NOT_SPECIAL;
    if (is_input_special) {
      if (override) {
        if (is_special() != is_input_special) return false;
        if ((has_credentials() || port != kOmit) && parsed == FILE) return false;
        if (type == FILE && host_start == host_end) return false;
      }
      type = parsed;
      set_scheme_from_view_with_colon(with_colon, n);
      if (override) {
        const uint16_t urls_scheme_port = default_port();
        if (port == urls_scheme_port) clear_port();
      }
    } else {
      char lower[64];
      if (input_n > sizeof(lower)) {
        // Ada lowercases a heap copy; very long schemes are refused here.
        ok = false;
        return false;
      }
      for (size_t i = 0; i < input_n; ++i)
        lower[i] = (input[i] >= 'A' && input[i] <= 'Z') ? static_cast<char>(input[i] + 32) : input[i];
      if (override) {
        const bool buffer_special = scheme_type(lower, input_n) != NOT_SPECIAL;
        if (is_special() != buffer_special) return true;
        if ((has_credentials() || port != kOmit) && input_n == 4 && memcmp(lower, "file", 4) == 0)
          return true;
        if (type == FILE && host_start == host_end) return true;
      }
      set_scheme(lower, input_n);
      if (override) {
        const uint16_t urls_scheme_port = default_port();
        if (port == urls_scheme_port) clear_port();
      }
    }
    return true;
  }
  void copy_scheme(const Agg& u) {
    const uint32_t diff = u.protocol_end - protocol_end;
    type = u.type;
    erase(0, protocol_end);
    insert(0, u.buf, u.protocol_end);
    protocol_end = u.protocol_end;
    if (diff == 0) return;
    shift_all_after_protocol(diff);
  }
  void set_protocol_as_file() {
    type = FILE;
    const uint32_t diff = 5 - protocol_end;
    if (len == 0) {
      append("file:", 5);
    } else {
      erase(0, protocol_end);
      insert(0, "file:", 5);
    }
    protocol_end = 5;
    shift_all_after_protocol(diff);
  }

  // -- credentials --
  void append_base_username(const char* input, size_t n) {
    add_authority_slashes_if_needed();
    if (n == 0) return;
    uint32_t diff = static_cast<uint32_t>(n);
    insert(username_end, input, n);
    username_end += diff;
    host_start += diff;
    if (buf[host_start] != '@' && host_start != host_end) {
      insert(host_start, "@");
      diff++;
    }
    host_end += diff;
    shift_after_host_end(diff);
  }
  void append_base_password(const char* input, size_t n) {
    add_authority_slashes_if_needed();
    if (n == 0) return;
    uint32_t diff = static_cast<uint32_t>(n);
    if (has_password()) {
      insert(host_start, input, n);
    } else {
      diff++;
      insert(username_end, ":");
      insert(username_end + 1, input, n);
    }
    host_start += diff;
    if (buf[host_start] != '@') {
      insert(host_start, "@");
      diff++;
    }
    host_end += diff;
    shift_after_host_end(diff);
  }
  void update_base_authority(const Agg& base) {
    const char* input = base.buf + base.protocol_end;
    size_t input_n = base.host_start - base.protocol_end;
    const bool starts_with_dash = input_n >= 2 && input[0] == '/' && input[1] == '/';
    uint32_t diff = host_start - protocol_end;
    erase(protocol_end, host_start - protocol_end);
    username_end = protocol_end;
    if (starts_with_dash) {
      input += 2;
      input_n -= 2;
      diff += 2;
      insert(protocol_end, "//");
      username_end += 2;
    }
    const size_t delimiter = find_char(input, input_n, 0, ':');
    if (delimiter != SIZE_MAX) {
      const char* username = input;
      const size_t username_n = delimiter;
      const char* password = input + delimiter + 1;
      const size_t password_n = input_n - delimiter - 1;
      insert(protocol_end + diff, username, username_n);
      diff += static_cast<uint32_t>(username_n);
      insert(protocol_end + diff, ":");
      username_end = protocol_end + diff;
      insert(protocol_end + diff + 1, password, password_n);
      diff += static_cast<uint32_t>(password_n) + 1;
    } else if (input_n) {
      insert(protocol_end + diff, input, input_n);
      username_end = protocol_end + diff + static_cast<uint32_t>(input_n);
      diff += static_cast<uint32_t>(input_n);
    }
    host_start += diff;
    if (len > base.host_start && buf[base.host_start] != '@') {
      insert(host_start, "@");
      diff++;
    }
    host_end += diff;
    shift_after_host_end(diff);
  }

  // -- host --
  void update_base_hostname(const char* input, size_t n) {
    add_authority_slashes_if_needed();
    const bool credentials = protocol_end + 2 < host_start;
    uint32_t diff = replace_and_resize(host_start, host_end, input, n);
    if (credentials) {
      insert(host_start, "@");
      diff++;
    }
    host_end += diff;
    shift_after_host_end(diff);
  }
  void clear_hostname() {
    if (!has_authority()) return;
    uint32_t length = host_end - host_start;
    uint32_t start = host_start;
    if (length > 0 && buf[start] == '@') {
      start++;
      length--;
    }
    erase(start, length);
    host_end = start;
    pathname_start -= length;
    if (search_start != kOmit) search_start -= length;
    if (hash_start != kOmit) hash_start -= length;
  }
  void update_base_port(uint32_t input) {
    if (input == kOmit) {
      clear_port();
      return;
    }
    char value[16];
    value[0] = ':';
    char digits[12];
    size_t dn = 0;
    uint32_t v = input;
    do { digits[dn++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < dn; ++i) value[1 + i] = digits[dn - 1 - i];
    uint32_t diff = static_cast<uint32_t>(dn + 1);
    if (port != kOmit) {
      diff -= pathname_start - host_end;
      erase(host_end, pathname_start - host_end);
    }
    insert(host_end, value, dn + 1);
    shift_after_host_end(diff);
    port = input;
  }

  bool parse_ipv4(const char* in, size_t n, bool in_place) {
    const bool trailing_dot = n && in[n - 1] == '.';
    if (trailing_dot) --n;
    size_t digit_count = 0;
    int pure_decimal_count = 0;
    uint64_t ipv4 = 0;
    size_t pos = 0;
    for (; digit_count < 4 && pos < n; digit_count++) {
      uint32_t segment = 0;
      const char* view = in + pos;
      const size_t vn = n - pos;
      const bool is_hex = vn >= 2 && view[0] == '0' && (view[1] | 0x20) == 'x';
      if (is_hex && (vn == 2 || (vn > 2 && view[2] == '.'))) {
        segment = 0;
        pos += 2;
      } else {
        size_t consumed = 0;
        FromChars r;
        if (is_hex) {
          r = from_chars_u32(view + 2, vn - 2, 16, &segment, &consumed);
          consumed += 2;
        } else if (vn >= 2 && view[0] == '0' && is_digit(view[1])) {
          r = from_chars_u32(view + 1, vn - 1, 8, &segment, &consumed);
          consumed += 1;
        } else {
          pure_decimal_count++;
          r = from_chars_u32(view, vn, 10, &segment, &consumed);
        }
        if (r != FromChars::kOk) return is_valid = false;
        pos += consumed;
      }
      if (pos == n) {
        if (segment >= (uint64_t(1) << (32 - digit_count * 8))) return is_valid = false;
        ipv4 <<= (32 - digit_count * 8);
        ipv4 |= segment;
        goto final_step;
      }
      if (segment > 255 || in[pos] != '.') return is_valid = false;
      ipv4 <<= 8;
      ipv4 |= segment;
      pos++;
    }
    if (digit_count != 4 || pos != n) return is_valid = false;
  final_step:
    if (!(in_place && pure_decimal_count == 4 && !trailing_dot)) {
      char text[16];
      size_t tn = 0;
      for (int shift = 24; shift >= 0; shift -= 8) {
        const unsigned octet = static_cast<unsigned>((ipv4 >> shift) & 0xff);
        char d[3];
        size_t dn = 0;
        unsigned v = octet;
        do { d[dn++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (dn) text[tn++] = d[--dn];
        if (shift) text[tn++] = '.';
      }
      update_base_hostname(text, tn);
    }
    host_type = HostType::kIpv4;
    return true;
  }

  bool parse_ipv6(const char* in, size_t n) {
    if (n == 0) return is_valid = false;
    uint16_t address[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int piece_index = 0;
    int compress = -1;
    size_t p = 0;
    if (in[0] == ':') {
      if (n == 1 || in[1] != ':') return is_valid = false;
      p += 2;
      compress = ++piece_index;
    }
    while (p != n) {
      if (piece_index == 8) return is_valid = false;
      if (in[p] == ':') {
        if (compress != -1) return is_valid = false;
        p++;
        compress = ++piece_index;
        continue;
      }
      uint16_t value = 0, length = 0;
      while (length < 4 && p != n && is_ascii_hex_digit(in[p])) {
        value = static_cast<uint16_t>(value * 0x10 + hex_value(in[p]));
        p++;
        length++;
      }
      if (p != n && in[p] == '.') {
        if (length == 0) return is_valid = false;
        p -= length;
        if (piece_index > 6) return is_valid = false;
        int numbers_seen = 0;
        while (p != n) {
          int ipv4_piece = -1;
          if (numbers_seen > 0) {
            if (in[p] == '.' && numbers_seen < 4) p++;
            else return is_valid = false;
          }
          if (p == n || !is_digit(in[p])) return is_valid = false;
          while (p != n && is_digit(in[p])) {
            const int number = in[p] - '0';
            if (ipv4_piece == -1) ipv4_piece = number;
            else if (ipv4_piece == 0) return is_valid = false;
            else ipv4_piece = ipv4_piece * 10 + number;
            if (ipv4_piece > 255) return is_valid = false;
            p++;
          }
          address[piece_index] = static_cast<uint16_t>(address[piece_index] * 0x100 + ipv4_piece);
          numbers_seen++;
          if (numbers_seen == 2 || numbers_seen == 4) piece_index++;
        }
        if (numbers_seen != 4) return is_valid = false;
        break;
      } else if (p != n && in[p] == ':') {
        p++;
        if (p == n) return is_valid = false;
      } else if (p != n) {
        return is_valid = false;
      }
      address[piece_index] = value;
      piece_index++;
    }
    if (compress != -1) {
      int swaps = piece_index - compress;
      piece_index = 7;
      while (piece_index != 0 && swaps > 0) {
        const uint16_t t = address[piece_index];
        address[piece_index] = address[compress + swaps - 1];
        address[compress + swaps - 1] = t;
        piece_index--;
        swaps--;
      }
    } else if (piece_index != 8) {
      return is_valid = false;
    }
    // serializers::ipv6
    size_t compress_length = 0, compress_at = 0;
    for (size_t i = 0; i < 8; i++) {
      if (address[i] == 0) {
        size_t next = i + 1;
        while (next != 8 && address[next] == 0) ++next;
        const size_t count = next - i;
        if (compress_length < count) {
          compress_length = count;
          compress_at = i;
          if (next == 8) break;
          i = next;
        }
      }
    }
    if (compress_length <= 1) compress_at = compress_length = 8;
    char out[48];
    size_t on = 0;
    size_t piece = 0;
    out[on++] = '[';
    while (true) {
      if (piece == compress_at) {
        out[on++] = ':';
        if (piece == 0) out[on++] = ':';
        piece += compress_length;
        if (piece == 8) break;
      }
      static const char kHex[] = "0123456789abcdef";
      char d[4];
      size_t dn = 0;
      unsigned v = address[piece];
      do { d[dn++] = kHex[v & 15]; v >>= 4; } while (v);
      while (dn) out[on++] = d[--dn];
      piece++;
      if (piece == 8) break;
      out[on++] = ':';
    }
    out[on++] = ']';
    update_base_hostname(out, on);
    host_type = HostType::kIpv6;
    return true;
  }

  bool parse_opaque_host(const char* in, size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (is_forbidden_host(static_cast<unsigned char>(in[i]))) return is_valid = false;
    if (!needs_encoding(in, n, kC0)) {
      update_base_hostname(in, n);
    } else {
      Str out{s->host, sizeof(s->host), 0, true};
      percent_encode_into(&out, in, n, kC0);
      if (!out.ok) { ok = false; return is_valid = false; }
      update_base_hostname(out.data, out.len);
    }
    host_type = HostType::kOpaque;
    return true;
  }

  bool parse_host(const char* in, size_t n) {
    if (n == 0) return is_valid = false;
    if (in[0] == '[') {
      if (in[n - 1] != ']') return is_valid = false;
      return parse_ipv6(in + 1, n - 2);
    }
    if (!is_special()) return parse_opaque_host(in, n);
    bool forbidden_or_upper = false;
    for (size_t i = 0; i < n && !forbidden_or_upper; ++i) {
      const unsigned char c = static_cast<unsigned char>(in[i]);
      forbidden_or_upper = is_forbidden_domain(c) || (c >= 'A' && c <= 'Z');
    }
    bool has_xn = false;
    for (size_t i = 0; i + 3 <= n && !has_xn; ++i)
      has_xn = in[i] == 'x' && in[i + 1] == 'n' && in[i + 2] == '-';
    if (!forbidden_or_upper && !has_xn) {
      update_base_hostname(in, n);
      host_type = HostType::kDomain;
      const View hostname = get_hostname();
      if (looks_ipv4(hostname.data, hostname.length))
        return parse_ipv4(hostname.data, hostname.length, true);
      return true;
    }
    size_t ascii_n = 0;
    if (!DomainToAscii(in, n, s->ascii, sizeof(s->ascii), &ascii_n, s)) return is_valid = false;
    if (looks_ipv4(s->ascii, ascii_n)) return parse_ipv4(s->ascii, ascii_n, false);
    update_base_hostname(s->ascii, ascii_n);
    host_type = HostType::kDomain;
    return true;
  }

  // set_host_or_hostname<override_hostname>. The cleaned input is held in
  // s->setter (first half), the previous hostname in its second half and the
  // host view handed to parse_host in s->path's second half, none of which
  // parse_host or the buffer edits touch.
  bool set_host_or_hostname(const char* input, size_t input_n, bool override_hostname) {
    if (has_opaque_path) return false;
    const size_t half = sizeof(s->setter) / 2;
    const View prev = get_hostname();
    if (prev.length > half) { ok = false; return false; }
    char* previous_host = s->setter + half;
    memcpy(previous_host, prev.data, prev.length);
    const size_t previous_host_n = prev.length;
    const uint32_t previous_port = port;
    size_t end = find_char(input, input_n, 0, '#');
    if (end == SIZE_MAX) end = input_n;
    Str host{s->setter, half, 0, true};
    for (size_t i = 0; i < end; ++i) if (!is_tab_or_newline(input[i])) host.Push(input[i]);
    if (!host.ok) { ok = false; return false; }
    const char* nh = host.data;
    size_t nh_n = host.len;
    if (type != FILE) {
      bool found_colon = false;
      const size_t location = host_delimiter_location(is_special(), nh, nh_n, &found_colon);
      const size_t host_view_n = location;
      if (found_colon) {
        if (override_hostname) return false;
        if (location + 1 < nh_n) set_port(nh + location + 1, nh_n - location - 1);
      } else if (host_view_n == 0 && (is_special() || has_credentials() || has_port())) {
        return false;
      }
      if (host_view_n == 0 && !is_special()) {
        if (has_hostname()) {
          clear_hostname();
        } else if (has_dash_dot()) {
          add_authority_slashes_if_needed();
          delete_dash_dot();
        }
        return true;
      }
      const bool succeeded = parse_host_copy(nh, host_view_n);
      if (!succeeded) {
        update_base_hostname(previous_host, previous_host_n);
        update_base_port(previous_port);
      } else if (has_dash_dot()) {
        delete_dash_dot();
      }
      return succeeded;
    }
    for (size_t i = 0; i < nh_n; ++i)
      if (nh[i] == '/' || nh[i] == '\\' || nh[i] == '?') { nh_n = i; break; }
    if (nh_n == 0) {
      clear_hostname();
    } else {
      if (!parse_host_copy(nh, nh_n)) {
        update_base_hostname(previous_host, previous_host_n);
        update_base_port(previous_port);
        return false;
      }
      if (host_end - host_start == 9 && memcmp(buf + host_start, "localhost", 9) == 0)
        clear_hostname();
    }
    return true;
  }
  bool parse_host_copy(const char* host, size_t n) {
    const size_t half = sizeof(s->path) / 2;
    if (n > half) { ok = false; return is_valid = false; }
    char* copy = s->path + half;
    memmove(copy, host, n);
    return parse_host(copy, n);
  }

  size_t parse_port(const char* view, size_t n, bool check_trailing) {
    if (n && view[0] == '-') { is_valid = false; return 0; }
    // std::from_chars into uint16_t
    uint32_t parsed = 0;
    size_t consumed = 0;
    bool range = false, invalid = false;
    {
      uint64_t value = 0;
      size_t i = 0;
      for (; i < n && is_digit(view[i]); ++i) {
        value = value * 10 + static_cast<uint64_t>(view[i] - '0');
        if (value > 0xffff) range = true;
        if (value > 0xffffffffull) value = 0x10000;
      }
      consumed = i;
      invalid = i == 0;
      parsed = static_cast<uint32_t>(value);
    }
    if (range) { is_valid = false; return 0; }
    if (check_trailing) {
      is_valid = is_valid && (consumed == n || view[consumed] == '/' || view[consumed] == '?' ||
                              (is_special() && view[consumed] == '\\'));
    }
    if (is_valid) {
      const uint16_t dp = default_port();
      const bool port_valid = (dp == 0 && parsed == 0) || dp != parsed;
      if (!invalid && port_valid) update_base_port(parsed);
      else clear_port();
    }
    return consumed;
  }
  bool set_port(const char* input, size_t n) {
    if (cannot_have_credentials_or_port()) return false;
    char trimmed[64];
    size_t tn = 0;
    for (size_t i = 0; i < n; ++i) {
      if (is_tab_or_newline(input[i])) continue;
      if (tn == sizeof(trimmed)) return false;  // bound: long port text
      trimmed[tn++] = input[i];
    }
    if (tn == 0) { clear_port(); return true; }
    if (is_c0_control_or_space(trimmed[0])) return false;
    bool digit = false;
    for (size_t i = 0; i < n && !digit; ++i) digit = is_digit(input[i]);
    if (!digit) return false;
    const uint32_t previous = port;
    parse_port(trimmed, tn, false);
    if (is_valid) return true;
    update_base_port(previous);
    is_valid = true;
    return false;
  }

  // -- search, hash --
  void clear_search() {
    if (search_start == kOmit) return;
    if (hash_start == kOmit) {
      resize_down(search_start);
    } else {
      erase(search_start, hash_start - search_start);
      hash_start = search_start;
    }
    search_start = kOmit;
  }
  void update_base_search_plain(const char* input, size_t n) {
    if (n == 0) { clear_search(); return; }
    if (input[0] == '?') { input++; n--; }
    if (hash_start == kOmit) {
      if (search_start == kOmit) {
        search_start = len;
        append("?", 1);
      } else {
        resize_down(search_start + 1);
      }
      append(input, n);
    } else {
      if (search_start == kOmit) {
        search_start = hash_start;
      } else {
        erase(search_start, hash_start - search_start);
        hash_start = search_start;
      }
      insert(search_start, "?");
      insert(search_start + 1, input, n);
      hash_start += static_cast<uint32_t>(n + 1);
    }
  }
  void update_base_search_encoded(const char* input, size_t n, EncodeSet set) {
    Str enc{s->host, sizeof(s->host), 0, true};
    percent_encode_into(&enc, input, n, set);
    if (!enc.ok) { ok = false; return; }
    if (hash_start == kOmit) {
      if (search_start == kOmit) {
        search_start = len;
        append("?", 1);
      } else {
        resize_down(search_start + 1);
      }
      append(enc.data, enc.len);
    } else {
      if (search_start == kOmit) {
        search_start = hash_start;
      } else {
        erase(search_start, hash_start - search_start);
        hash_start = search_start;
      }
      insert(search_start, "?");
      insert(search_start + 1, enc.data, enc.len);
      hash_start += static_cast<uint32_t>(enc.len + 1);
    }
  }
  void update_unencoded_base_hash(const char* input, size_t n) {
    if (hash_start != kOmit) resize_down(hash_start);
    hash_start = len;
    append("#", 1);
    Str enc{s->host, sizeof(s->host), 0, true};
    percent_encode_into(&enc, input, n, kFragment);
    if (!enc.ok) { ok = false; return; }
    append(enc.data, enc.len);
  }

  // -- path --
  void update_base_pathname(const char* input, size_t n) {
    const bool begins_with_dashdash = n >= 2 && input[0] == '/' && input[1] == '/';
    if (!begins_with_dashdash && has_dash_dot()) delete_dash_dot();
    if (begins_with_dashdash && !has_opaque_path && !has_authority() && !has_dash_dot()) {
      insert(pathname_start, "/.");
      pathname_start += 2;
    }
    const uint32_t diff =
        replace_and_resize(pathname_start, pathname_start + get_pathname_length(), input, n);
    if (search_start != kOmit) search_start += diff;
    if (hash_start != kOmit) hash_start += diff;
  }
  void append_base_pathname(const char* input, size_t n) {
    uint32_t ending = len;
    if (search_start != kOmit) ending = search_start;
    else if (hash_start != kOmit) ending = hash_start;
    insert(ending, input, n);
    if (search_start != kOmit) search_start += static_cast<uint32_t>(n);
    if (hash_start != kOmit) hash_start += static_cast<uint32_t>(n);
  }
  void clear_pathname() {
    uint32_t ending = len;
    if (search_start != kOmit) ending = search_start;
    else if (hash_start != kOmit) ending = hash_start;
    const uint32_t length = ending - pathname_start;
    erase(pathname_start, length);
    uint32_t diff = length;
    if (pathname_start == host_end + 2 && buf[host_end] == '/' && buf[host_end + 1] == '.') {
      pathname_start -= 2;
      erase(host_end, 2);
      diff += 2;
    }
    if (search_start != kOmit) search_start -= diff;
    if (hash_start != kOmit) hash_start -= diff;
  }
  void set_host_from(const Agg& base) {
    const View h = base.get_host();
    set_host_or_hostname(h.data, h.length, false);
  }
  void set_hostname_from(const Agg& base) {
    const View h = base.get_hostname();
    set_host_or_hostname(h.data, h.length, true);
  }

  // helpers::shorten_path on a Str
  static bool shorten_path(Str* path, SchemeType t) {
    const size_t first = find_char(path->data, path->len, 1, '/');
    if (t == FILE && first == SIZE_MAX && path->len) {
      if (is_normalized_windows_drive_letter(path->data + 1, path->len - 1)) return false;
    }
    if (path->len) {
      const size_t slash = rfind_char(path->data, path->len, '/');
      if (slash != SIZE_MAX) {
        path->len = slash;
        return true;
      }
    }
    return false;
  }

  void consume_prepared_path(const char* input, size_t n) {
    const uint8_t acc = path_signature(input, n);
    const uint8_t need_encoding = 1, backslash_char = 2, dot_char = 4, percent_char = 8;
    const bool special = type != NOT_SPECIAL;
    const bool slow_file = type == FILE && is_windows_drive_letter(input, n);
    bool trivial = (special ? acc == 0 : (acc & (need_encoding | dot_char | percent_char)) == 0) &&
                   !slow_file;
    if (acc == dot_char && !slow_file) {
      if (input[0] != '.') {
        size_t slashdot = SIZE_MAX;
        for (size_t i = 0; i + 1 < n; ++i) if (input[i] == '/' && input[i + 1] == '.') { slashdot = i; break; }
        if (slashdot == SIZE_MAX) trivial = true;
        else trivial = !(slashdot + 2 == n || input[slashdot + 2] == '.' || input[slashdot + 2] == '/');
      }
    }
    if (trivial && is_at_path()) {
      append("/", 1);
      append(input, n);
      return;
    }
    const View current = get_pathname();
    Str path{s->path, sizeof(s->path) / 2, 0, true};
    path.Append(current.data, current.length);
    const bool fast = special && (acc & (need_encoding | backslash_char | percent_char)) == 0 &&
                      type != FILE;
    if (fast) {
      size_t previous = 0;
      while (true) {
        const size_t next = find_char(input, n, previous, '/');
        if (next == SIZE_MAX) {
          const char* pv = input + previous;
          const size_t pn = n - previous;
          if (pn == 2 && pv[0] == '.' && pv[1] == '.') {
            if (path.len == 0) {
              path.Push('/');
            } else if (path.data[path.len - 1] != '/') {
              path.len = rfind_char(path.data, path.len, '/') + 1;
            }
            break;
          }
          path.Push('/');
          if (!(pn == 1 && pv[0] == '.')) path.Append(pv, pn);
          break;
        }
        const char* pv = input + previous;
        const size_t pn = next - previous;
        previous = next + 1;
        if (pn == 2 && pv[0] == '.' && pv[1] == '.') {
          const size_t last = rfind_char(path.data, path.len, '/');
          if (last != SIZE_MAX) path.len = last;
        } else if (!(pn == 1 && pv[0] == '.')) {
          path.Push('/');
          path.Append(pv, pn);
        }
      }
    } else {
      const bool needs_percent = acc & 1;
      const char* rest = input;
      size_t rest_n = n;
      while (true) {
        size_t location = SIZE_MAX;
        for (size_t i = 0; i < rest_n; ++i)
          if (rest[i] == '/' || (special && (acc & 2) && rest[i] == '\\')) { location = i; break; }
        const char* pv = rest;
        size_t pn = rest_n;
        if (location != SIZE_MAX) {
          pn = location;
          rest += location + 1;
          rest_n -= location + 1;
        }
        Str enc{s->host, sizeof(s->host), 0, true};
        const char* pb = pv;
        size_t pbn = pn;
        if (needs_percent && needs_encoding(pv, pn, kPath)) {
          percent_encode_into(&enc, pv, pn, kPath);
          if (!enc.ok) { ok = false; return; }
          pb = enc.data;
          pbn = enc.len;
        }
        if (is_double_dot(pb, pbn)) {
          if ((shorten_path(&path, type) || special) && location == SIZE_MAX) path.Push('/');
        } else if (is_single_dot(pb, pbn) && location == SIZE_MAX) {
          path.Push('/');
        } else if (!is_single_dot(pb, pbn)) {
          if (type == FILE && path.len == 0 && is_windows_drive_letter(pb, pbn)) {
            path.Push('/');
            path.Push(pb[0]);
            path.Push(':');
            path.Append(pb + 2, pbn - 2);
          } else {
            path.Push('/');
            path.Append(pb, pbn);
          }
        }
        if (location == SIZE_MAX) break;
      }
    }
    if (!path.ok) { ok = false; return; }
    update_base_pathname(path.data, path.len);
  }
};

enum class State : uint8_t {
  AUTHORITY, SCHEME_START, SCHEME, HOST, NO_SCHEME, FRAGMENT, RELATIVE_SCHEME,
  RELATIVE_SLASH, FILE_STATE, FILE_HOST, FILE_SLASH, PATH_OR_AUTHORITY,
  SPECIAL_AUTHORITY_IGNORE_SLASHES, SPECIAL_AUTHORITY_SLASHES,
  SPECIAL_RELATIVE_OR_AUTHORITY, QUERY, PATH, PATH_START, OPAQUE_PATH, PORT
};

// WHATWG UTF-8 decode with replacement (USVString conversion of the input).
bool usv_utf8(const char* in, size_t n, char* out, size_t cap, size_t* out_n) {
  size_t o = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in);
  size_t i = 0;
  auto put = [&](const char* bytes, size_t k) {
    if (o + k > cap) return false;
    memcpy(out + o, bytes, k);
    o += k;
    return true;
  };
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      if (!put(reinterpret_cast<const char*>(p + i), 1)) return false;
      ++i;
      continue;
    }
    size_t need = 0;
    unsigned char lower = 0x80, upper = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) need = 1;
    else if (c >= 0xe0 && c <= 0xef) {
      need = 2;
      if (c == 0xe0) lower = 0xa0;
      if (c == 0xed) upper = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      need = 3;
      if (c == 0xf0) lower = 0x90;
      if (c == 0xf4) upper = 0x8f;
    } else {
      if (!put("\xef\xbf\xbd", 3)) return false;
      ++i;
      continue;
    }
    size_t k = 1;
    for (; k <= need; ++k) {
      if (i + k >= n) break;
      const unsigned char b = p[i + k];
      if (b < (k == 1 ? lower : 0x80) || b > (k == 1 ? upper : 0xbf)) break;
    }
    if (k == need + 1) {
      if (!put(reinterpret_cast<const char*>(p + i), need + 1)) return false;
      i += need + 1;
    } else {
      if (!put("\xef\xbf\xbd", 3)) return false;
      i += k;  // maximal subpart
    }
  }
  *out_n = o;
  return true;
}

bool parse_into(const char* user_input, size_t user_n, const Agg* base, Agg& url) {
  UrlScratch* s = url.s;
  // remove ASCII tab or newline (into s->input, which already holds the
  // USVString form when the caller converted it).
  size_t n = 0;
  char* data = s->input;
  for (size_t i = 0; i < user_n; ++i)
    if (!is_tab_or_newline(user_input[i])) data[n++] = user_input[i];
  // trim C0 control or space
  size_t begin = 0;
  while (begin < n && is_c0_control_or_space(data[begin])) ++begin;
  while (n > begin && is_c0_control_or_space(data[n - 1])) --n;
  const char* url_data = data + begin;
  size_t input_size = n - begin;
  // prune_hash
  const char* fragment = nullptr;
  size_t fragment_n = 0;
  bool has_fragment = false;
  {
    const size_t hash = find_char(url_data, input_size, 0, '#');
    if (hash != SIZE_MAX) {
      has_fragment = true;
      fragment = url_data + hash + 1;
      fragment_n = input_size - hash - 1;
      input_size = hash;
    }
  }
  auto finish_hash = [&]() {
    if (has_fragment) url.update_unencoded_base_hash(fragment, fragment_n);
  };
  State state = State::SCHEME_START;
  size_t input_position = 0;
  while (input_position <= input_size) {
    if (!url.ok) return false;
    switch (state) {
      case State::SCHEME_START:
        if (input_position != input_size && is_alpha(url_data[input_position])) {
          state = State::SCHEME;
          input_position++;
        } else {
          state = State::NO_SCHEME;
        }
        break;
      case State::SCHEME: {
        while (input_position != input_size && is_alnum_plus(url_data[input_position]))
          input_position++;
        if (input_position != input_size && url_data[input_position] == ':') {
          if (!url.parse_scheme_with_colon(url_data, input_position + 1, false)) return false;
          if (url.type == FILE) {
            state = State::FILE_STATE;
          } else if (url.is_special() && base && base->type == url.type) {
            state = State::SPECIAL_RELATIVE_OR_AUTHORITY;
          } else if (url.is_special()) {
            state = State::SPECIAL_AUTHORITY_SLASHES;
          } else if (input_position + 1 < input_size && url_data[input_position + 1] == '/') {
            state = State::PATH_OR_AUTHORITY;
            input_position++;
          } else {
            state = State::OPAQUE_PATH;
          }
        } else {
          state = State::NO_SCHEME;
          input_position = 0;
          break;
        }
        input_position++;
        break;
      }
      case State::NO_SCHEME:
        if (!base || (base->has_opaque_path && !has_fragment)) return false;
        if (base->has_opaque_path && has_fragment && input_position == input_size) {
          url.copy_scheme(*base);
          url.has_opaque_path = base->has_opaque_path;
          const View bp = base->get_pathname();
          url.update_base_pathname(bp.data, bp.length);
          const View bs = base->get_search();
          url.update_base_search_plain(bs.data, bs.length);
          url.update_unencoded_base_hash(fragment, fragment_n);
          return url.ok && url.is_valid;
        } else if (base->type != FILE) {
          state = State::RELATIVE_SCHEME;
        } else {
          state = State::FILE_STATE;
        }
        break;
      case State::AUTHORITY: {
        if (find_char(url_data, input_size, input_position, '@') == SIZE_MAX) {
          state = State::HOST;
          break;
        }
        bool at_sign_seen = false, password_token_seen = false;
        while (true) {
          const char* view = url_data + input_position;
          const size_t view_n = input_size - input_position;
          size_t location = view_n;
          for (size_t i = 0; i < view_n; ++i) {
            const char c = view[i];
            if (c == '@' || c == '/' || c == '?' || (url.is_special() && c == '\\')) { location = i; break; }
          }
          const size_t end_of_authority = input_position + location;
          if (end_of_authority != input_size && url_data[end_of_authority] == '@') {
            if (at_sign_seen) {
              if (password_token_seen) url.append_base_password("%40", 3);
              else url.append_base_username("%40", 3);
            }
            at_sign_seen = true;
            Str enc{s->host, sizeof(s->host), 0, true};
            if (!password_token_seen) {
              const size_t colon = find_char(view, location, 0, ':');
              password_token_seen = colon != SIZE_MAX;
              if (!password_token_seen) {
                percent_encode_into(&enc, view, location, kUserinfo);
                if (!enc.ok) return false;
                url.append_base_username(enc.data, enc.len);
              } else {
                percent_encode_into(&enc, view, colon, kUserinfo);
                if (!enc.ok) return false;
                url.append_base_username(enc.data, enc.len);
                enc.Clear();
                percent_encode_into(&enc, view + colon + 1, location - colon - 1, kUserinfo);
                if (!enc.ok) return false;
                url.append_base_password(enc.data, enc.len);
              }
            } else {
              percent_encode_into(&enc, view, location, kUserinfo);
              if (!enc.ok) return false;
              url.append_base_password(enc.data, enc.len);
            }
          } else if (end_of_authority == input_size || url_data[end_of_authority] == '/' ||
                     url_data[end_of_authority] == '?' ||
                     (url.is_special() && url_data[end_of_authority] == '\\')) {
            if (at_sign_seen && location == 0) return false;
            state = State::HOST;
            break;
          }
          if (end_of_authority == input_size) {
            finish_hash();
            return url.ok && url.is_valid;
          }
          input_position = end_of_authority + 1;
        }
        break;
      }
      case State::SPECIAL_RELATIVE_OR_AUTHORITY:
        if (input_size - input_position >= 2 && url_data[input_position] == '/' &&
            url_data[input_position + 1] == '/') {
          state = State::SPECIAL_AUTHORITY_IGNORE_SLASHES;
          input_position += 2;
        } else {
          state = State::RELATIVE_SCHEME;
        }
        break;
      case State::PATH_OR_AUTHORITY:
        if (input_position != input_size && url_data[input_position] == '/') {
          state = State::AUTHORITY;
          input_position++;
        } else {
          state = State::PATH;
        }
        break;
      case State::RELATIVE_SCHEME:
        url.copy_scheme(*base);
        if (input_position != input_size && url_data[input_position] == '/') {
          state = State::RELATIVE_SLASH;
        } else if (url.is_special() && input_position != input_size &&
                   url_data[input_position] == '\\') {
          state = State::RELATIVE_SLASH;
        } else {
          url.update_base_authority(*base);
          url.set_hostname_from(*base);
          url.update_base_port(base->port);
          url.has_opaque_path = base->has_opaque_path;
          {
            const View bp = base->get_pathname();
            url.update_base_pathname(bp.data, bp.length);
            const View bs = base->get_search();
            url.update_base_search_plain(bs.data, bs.length);
          }
          url.has_opaque_path = base->has_opaque_path;
          if (input_position != input_size && url_data[input_position] == '?') {
            state = State::QUERY;
          } else if (input_position != input_size) {
            url.clear_search();
            const View p = url.get_pathname();
            if (p.length > sizeof(s->path) / 2) return false;
            Str path{s->path, sizeof(s->path) / 2, 0, true};
            path.Append(p.data, p.length);
            if (Agg::shorten_path(&path, url.type)) url.update_base_pathname(path.data, path.len);
            state = State::PATH;
            break;
          }
        }
        input_position++;
        break;
      case State::RELATIVE_SLASH:
        if (url.is_special() && input_position != input_size &&
            (url_data[input_position] == '/' || url_data[input_position] == '\\')) {
          state = State::SPECIAL_AUTHORITY_IGNORE_SLASHES;
        } else if (input_position != input_size && url_data[input_position] == '/') {
          state = State::AUTHORITY;
        } else {
          url.update_base_authority(*base);
          url.set_hostname_from(*base);
          url.update_base_port(base->port);
          state = State::PATH;
          break;
        }
        input_position++;
        break;
      case State::SPECIAL_AUTHORITY_SLASHES:
        if (input_size - input_position >= 2 && url_data[input_position] == '/' &&
            url_data[input_position + 1] == '/')
          input_position += 2;
        [[fallthrough]];
      case State::SPECIAL_AUTHORITY_IGNORE_SLASHES:
        while (input_position != input_size &&
               (url_data[input_position] == '/' || url_data[input_position] == '\\'))
          input_position++;
        state = State::AUTHORITY;
        break;
      case State::QUERY:
        url.update_base_search_encoded(url_data + input_position, input_size - input_position,
                                       url.is_special() ? kSpecialQuery : kQuery);
        finish_hash();
        return url.ok && url.is_valid;
      case State::HOST: {
        const char* host_view = url_data + input_position;
        bool found_colon = false;
        const size_t location = host_delimiter_location(url.is_special(), host_view,
                                               input_size - input_position, &found_colon);
        const size_t host_n = location;
        input_position += location;
        // parse_host may clobber scratch; hosts live in the input buffer.
        if (found_colon) {
          if (!url.parse_host(host_view, host_n)) return false;
          state = State::PORT;
          input_position++;
        } else {
          if (url.is_special() && host_n == 0) return false;
          if (host_n == 0) {
            url.update_base_hostname("", 0);
          } else if (!url.parse_host(host_view, host_n)) {
            return false;
          }
          state = State::PATH_START;
        }
        break;
      }
      case State::OPAQUE_PATH: {
        const char* view = url_data + input_position;
        size_t view_n = input_size - input_position;
        const size_t location = find_char(view, view_n, 0, '?');
        if (location != SIZE_MAX) {
          view_n = location;
          state = State::QUERY;
          input_position += location + 1;
        } else {
          input_position = input_size + 1;
        }
        url.has_opaque_path = true;
        Str enc{s->path, sizeof(s->path), 0, true};
        percent_encode_into(&enc, view, view_n, kC0);
        if (!enc.ok) return false;
        url.update_base_pathname(enc.data, enc.len);
        break;
      }
      case State::PORT: {
        const size_t consumed = url.parse_port(url_data + input_position, input_size - input_position, true);
        input_position += consumed;
        if (!url.is_valid) return false;
        state = State::PATH_START;
      }
        [[fallthrough]];
      case State::PATH_START:
        if (url.is_special()) {
          state = State::PATH;
          if (input_position == input_size) {
            url.update_base_pathname("/", 1);
            finish_hash();
            return url.ok && url.is_valid;
          }
          if (url_data[input_position] != '/' && url_data[input_position] != '\\') break;
        } else if (input_position != input_size && url_data[input_position] == '?') {
          state = State::QUERY;
        } else if (input_position != input_size) {
          state = State::PATH;
          if (url_data[input_position] != '/') break;
        }
        input_position++;
        break;
      case State::PATH: {
        const char* view = url_data + input_position;
        size_t view_n = input_size - input_position;
        const size_t q = find_char(view, view_n, 0, '?');
        if (q != SIZE_MAX) {
          state = State::QUERY;
          view_n = q;
          input_position += q + 1;
        } else {
          input_position = input_size + 1;
        }
        url.consume_prepared_path(view, view_n);
        break;
      }
      case State::FILE_SLASH:
        if (input_position != input_size &&
            (url_data[input_position] == '/' || url_data[input_position] == '\\')) {
          state = State::FILE_HOST;
          input_position++;
        } else {
          if (base && base->type == FILE) {
            url.set_host_from(*base);
            const View bp = base->get_pathname();
            if (bp.length) {
              if (!is_windows_drive_letter(url_data + input_position, input_size - input_position)) {
                const char* first = bp.data + 1;
                size_t first_n = bp.length - 1;
                const size_t loc = find_char(first, first_n, 0, '/');
                if (loc != SIZE_MAX) first_n = loc;
                if (is_normalized_windows_drive_letter(first, first_n)) {
                  Str cat{s->path, sizeof(s->path) / 2, 0, true};
                  cat.Push('/');
                  cat.Append(first, first_n);
                  if (!cat.ok) return false;
                  url.append_base_pathname(cat.data, cat.len);
                }
              }
            }
          }
          state = State::PATH;
        }
        break;
      case State::FILE_HOST: {
        const char* view = url_data + input_position;
        const size_t view_n = input_size - input_position;
        size_t location = view_n;
        for (size_t i = 0; i < view_n; ++i)
          if (view[i] == '/' || view[i] == '\\' || view[i] == '?') { location = i; break; }
        if (is_windows_drive_letter(view, location)) {
          state = State::PATH;
        } else if (location == 0) {
          url.update_base_hostname("", 0);
          state = State::PATH_START;
        } else {
          input_position += location;
          if (!url.parse_host(view, location)) return false;
          const View h = url.get_hostname();
          if (h.length == 9 && memcmp(h.data, "localhost", 9) == 0) url.update_base_hostname("", 0);
          state = State::PATH_START;
        }
        break;
      }
      case State::FILE_STATE: {
        const char* file_view = url_data + input_position;
        const size_t file_n = input_size - input_position;
        url.set_protocol_as_file();
        url.update_base_hostname("", 0);
        if (input_position != input_size &&
            (url_data[input_position] == '/' || url_data[input_position] == '\\')) {
          state = State::FILE_SLASH;
        } else if (base && base->type == FILE) {
          url.set_hostname_from(*base);
          {
            const View bp = base->get_pathname();
            url.update_base_pathname(bp.data, bp.length);
            const View bs = base->get_search();
            url.update_base_search_plain(bs.data, bs.length);
          }
          url.has_opaque_path = base->has_opaque_path;
          if (input_position != input_size && url_data[input_position] == '?') {
            state = State::QUERY;
          } else if (input_position != input_size) {
            url.clear_search();
            if (!is_windows_drive_letter(file_view, file_n)) {
              const View p = url.get_pathname();
              if (p.length > sizeof(s->path) / 2) return false;
              Str path{s->path, sizeof(s->path) / 2, 0, true};
              path.Append(p.data, p.length);
              if (Agg::shorten_path(&path, url.type)) url.update_base_pathname(path.data, path.len);
            } else {
              url.clear_pathname();
              url.has_opaque_path = true;
            }
            state = State::PATH;
            break;
          }
        } else {
          state = State::PATH;
          break;
        }
        input_position++;
        break;
      }
      default:
        return false;
    }
  }
  finish_hash();
  return url.ok && url.is_valid;
}

void export_url(const Agg& a, Url* out) {
  out->href_length = a.len;
  out->href[a.len] = '\0';
  out->protocol_end = a.protocol_end;
  out->username_end = a.username_end;
  out->host_start = a.host_start;
  out->host_end = a.host_end;
  out->port = a.port == kOmit ? -1 : static_cast<int32_t>(a.port);
  out->pathname_start = a.pathname_start;
  out->search_start = a.search_start;
  out->hash_start = a.hash_start;
  out->special = a.type != NOT_SPECIAL;
  out->host_type = a.host_type;
  out->scheme_type_ = a.type;
  out->has_opaque_path_ = a.has_opaque_path;
}

void import_url(const Url& u, Agg* a, UrlScratch* s) {
  a->buf = const_cast<char*>(u.href);
  a->len = static_cast<uint32_t>(u.href_length);
  a->cap = static_cast<uint32_t>(kMaxHrefBytes - 1);
  a->protocol_end = u.protocol_end;
  a->username_end = u.username_end;
  a->host_start = u.host_start;
  a->host_end = u.host_end;
  a->port = u.port < 0 ? kOmit : static_cast<uint32_t>(u.port);
  a->pathname_start = u.pathname_start;
  a->search_start = u.search_start;
  a->hash_start = u.hash_start;
  a->type = static_cast<SchemeType>(u.scheme_type_);
  a->has_opaque_path = u.has_opaque_path_;
  a->host_type = u.host_type;
  a->s = s;
}

}  // namespace

bool DomainToAscii(const char* utf8, size_t length, char* output, size_t capacity,
                   size_t* output_length, UrlScratch* scratch) {
  // unicode::to_ascii: percent-decode when a '%' is present, then idna.
  const char* input = utf8;
  size_t n = length;
  const size_t first_percent = find_char(utf8, length, 0, '%');
  if (first_percent != SIZE_MAX) {
    if (length > sizeof(scratch->host)) return false;
    size_t o = 0;
    size_t i = 0;
    for (; i < first_percent; ++i) scratch->host[o++] = utf8[i];
    while (i < length) {
      const char ch = utf8[i];
      const size_t remaining = length - i - 1;
      if (ch != '%' || remaining < 2 || !is_ascii_hex_digit(utf8[i + 1]) ||
          !is_ascii_hex_digit(utf8[i + 2])) {
        scratch->host[o++] = ch;
        i++;
      } else {
        scratch->host[o++] = static_cast<char>(hex_value(utf8[i + 1]) * 16 + hex_value(utf8[i + 2]));
        i += 3;
      }
    }
    input = scratch->host;
    n = o;
  }
  size_t out_n = 0;
  if (!idna::ToAscii(input, n, output, capacity, &out_n, &scratch->idna) || out_n == 0)
    return false;
  for (size_t i = 0; i < out_n; ++i)
    if (is_forbidden_domain(static_cast<unsigned char>(output[i]))) return false;
  *output_length = out_n;
  return true;
}

bool Parse(const char* input, size_t length, const Url* base, Url* out, UrlScratch* scratch) {
  if (!input || !out || !scratch || length > kMaxInputBytes) return false;
  if (base == out) return false;
  // USVString conversion into s->path, then to the tail of s->input; the
  // parser compacts it (dropping tabs/newlines) into the head of s->input,
  // never writing past the byte it is reading.
  size_t usv_n = 0;
  if (!usv_utf8(input, length, scratch->path, sizeof(scratch->path), &usv_n)) return false;
  if (usv_n > sizeof(scratch->input)) return false;
  char* user = scratch->input + sizeof(scratch->input) - usv_n;
  memcpy(user, scratch->path, usv_n);
  Agg base_agg;
  if (base) import_url(*base, &base_agg, scratch);
  Agg url;
  url.buf = out->href;
  url.cap = static_cast<uint32_t>(kMaxHrefBytes - 1);
  url.len = 0;
  url.buf[0] = '\0';
  url.s = scratch;
  if (!parse_into(user, usv_n, base ? &base_agg : nullptr, url)) return false;
  export_url(url, out);
  return true;
}

View Href(const Url& u) { return View{u.href, u.href_length}; }
View Protocol(const Url& u) { return View{u.href, u.protocol_end}; }
View Username(const Url& u) {
  if (u.protocol_end + 2 < u.username_end)
    return View{u.href + u.protocol_end + 2, u.username_end - u.protocol_end - 2};
  return View{u.href, 0};
}
View Password(const Url& u) {
  if (u.host_start > u.username_end)
    return View{u.href + u.username_end + 1, u.host_start - u.username_end - 1};
  return View{u.href, 0};
}
View Hostname(const Url& u) {
  uint32_t start = u.host_start;
  if (start < u.href_length && u.href[start] == '@') start++;
  if (start > u.host_end) return View{u.href, 0};
  return View{u.href + start, u.host_end - start};
}
View Host(const Url& u) {
  uint32_t start = u.host_start;
  if (start < u.href_length && u.href[start] == '@') start++;
  if (start == u.host_end) return View{u.href, 0};
  return View{u.href + start, u.pathname_start - start};
}
View Port(const Url& u) {
  if (u.port < 0) return View{u.href, 0};
  return View{u.href + u.host_end + 1, u.pathname_start - u.host_end - 1};
}
View Pathname(const Url& u) {
  size_t end = u.href_length;
  if (u.search_start != kOmitted) end = u.search_start;
  else if (u.hash_start != kOmitted) end = u.hash_start;
  return View{u.href + u.pathname_start, end - u.pathname_start};
}
View Search(const Url& u) {
  if (u.search_start == kOmitted) return View{u.href, 0};
  size_t end = u.href_length;
  if (u.hash_start != kOmitted) end = u.hash_start;
  if (end - u.search_start <= 1) return View{u.href, 0};
  return View{u.href + u.search_start, end - u.search_start};
}
View Hash(const Url& u) {
  if (u.hash_start == kOmitted || u.href_length - u.hash_start <= 1) return View{u.href, 0};
  return View{u.href + u.hash_start, u.href_length - u.hash_start};
}

namespace {

// URLSearchParams over url.search, as Node.js 22 implements it: parseParams
// ('+' to space; querystring.unescape — percent-decoded bytes read as UTF-8
// with U+FFFD replacement — once any valid %XX was seen), name filtering, then
// serializeParams (application/x-www-form-urlencoded, uppercase hex).
bool param_name_deleted(const char* name, size_t n) {
  auto prefix = [&](const char* p) {
    const size_t k = strlen(p);
    if (n < k) return false;
    for (size_t i = 0; i < k; ++i) {
      char c = name[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      if (c != p[i]) return false;
    }
    return true;
  };
  return prefix("utm_") || ieq_ascii(name, n, "ref") || ieq_ascii(name, n, "source") ||
         ieq_ascii(name, n, "fbclid") || ieq_ascii(name, n, "gclid") || prefix("mc_");
}

void serialize_component(Str* out, const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ') out->Push('+');
    else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             c == '*' || c == '-' || c == '.' || c == '_') out->Push(static_cast<char>(c));
    else append_hex_escape(out, c);
  }
}

// One parseParams token: raw text with '+' as space, decoded when `encoded`.
// Writes the UTF-8 result to `out` (via `temp`).
bool decode_token(const char* raw, size_t n, bool encoded, Str* out, char* temp, size_t temp_cap) {
  size_t t = 0;
  for (size_t i = 0; i < n; ++i) {
    if (t == temp_cap) return false;
    const char c = raw[i];
    if (c == '+') { temp[t++] = ' '; continue; }
    if (encoded && c == '%' && i + 2 < n && is_ascii_hex_digit(raw[i + 1]) &&
        is_ascii_hex_digit(raw[i + 2])) {
      temp[t++] = static_cast<char>(hex_value(raw[i + 1]) * 16 + hex_value(raw[i + 2]));
      i += 2;
      continue;
    }
    temp[t++] = c;
  }
  size_t fixed = 0;
  char* dst = out->data + out->len;
  if (!usv_utf8(temp, t, dst, out->cap - out->len, &fixed)) return false;
  out->len += fixed;
  return true;
}

}  // namespace

bool CanonicalArticleUrl(const char* utf8, size_t length, char* output, size_t capacity,
                         size_t* output_length, UrlScratch* scratch) {
  Url* url = &scratch->canonical;
  if (!Parse(utf8, length, nullptr, url, scratch)) return false;
  Agg a;
  import_url(*url, &a, scratch);
  // parsed.hash = ''  (url_aggregator::set_hash with an empty input)
  if (a.hash_start != kOmit) {
    a.resize_down(a.hash_start);
    a.hash_start = kOmit;
  }
  if (a.has_opaque_path && a.search_start == kOmit) {
    const View p = a.get_pathname();
    size_t n = p.length;
    while (n && p.data[n - 1] == ' ') --n;
    if (n != p.length) {
      memcpy(scratch->path, p.data, n);
      a.update_base_pathname(scratch->path, n);
    }
  }
  // parsed.protocol = 'https:'  (set_protocol -> parse_scheme_with_colon<true>)
  a.parse_scheme_with_colon("https:", 6, true);
  if (!a.ok) return false;
  export_url(a, url);
  // parsed.hostname = parsed.hostname.toLowerCase().replace(/^www\./, '')
  {
    const View h = Hostname(*url);
    char* value = scratch->params;
    if (h.length > sizeof(scratch->params)) return false;
    size_t vn = 0;
    for (size_t i = 0; i < h.length; ++i) {
      char c = h.data[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      value[vn++] = c;
    }
    const char* v = value;
    if (vn >= 4 && memcmp(v, "www.", 4) == 0) { v += 4; vn -= 4; }
    // Node applies the setter to a fresh parse of the href and keeps the old
    // href on failure: snapshot the offsets and the buffer (the setter copies
    // its input into s->setter first, so `v` may stay in s->params' head).
    const Agg snapshot = a;
    const size_t half = sizeof(scratch->params) / 2;
    if (a.len > half || vn > half) return false;
    memcpy(scratch->params + half, url->href, a.len);
    if (!a.set_host_or_hostname(v, vn, true) || !a.ok || !a.is_valid) {
      a = snapshot;
      memcpy(url->href, scratch->params + half, snapshot.len);
      url->href[snapshot.len] = '\0';
    }
    export_url(a, url);
  }
  Str out{output, capacity, 0, true};
  const View hostname = Hostname(*url);
  out.Append(hostname.data, hostname.length);
  View path = Pathname(*url);
  while (path.length && path.data[path.length - 1] == '/') --path.length;
  out.Append(path.data, path.length);
  // searchParams from url.search
  const View search = Search(*url);
  const char* qs = search.data;
  const size_t qn = search.length;
  size_t i = qn && qs[0] == '?' ? 1 : 0;
  Str ser{scratch->ascii, sizeof(scratch->ascii), 0, true};
  bool first = true;
  char* name_buf = scratch->params;
  const size_t token_cap = sizeof(scratch->params) / 4;
  char* value_buf = scratch->params + token_cap;
  char* temp = scratch->params + 2 * token_cap;
  while (i <= qn) {
    // one pair: [i, amp)
    size_t amp = i;
    while (amp < qn && qs[amp] != '&') ++amp;
    if (amp > i) {
      size_t eq = i;
      while (eq < amp && qs[eq] != '=') ++eq;
      auto encoded_in = [&](size_t from, size_t to) {
        // parseParams' encodeCheck: a '%' followed by two hex digits, where
        // '+' neither counts nor resets.
        int check = 0;
        for (size_t k = from; k < to; ++k) {
          const char c = qs[k];
          if (c == '+') continue;
          if (c == '%') { check = 1; continue; }
          if (check > 0) {
            if (is_ascii_hex_digit(c)) { if (++check == 3) return true; }
            else check = 0;
          }
        }
        return false;
      };
      Str name{name_buf, token_cap, 0, true};
      Str value{value_buf, token_cap, 0, true};
      if (!decode_token(qs + i, eq - i, encoded_in(i, eq), &name, temp, token_cap)) return false;
      if (eq < amp &&
          !decode_token(qs + eq + 1, amp - eq - 1, encoded_in(eq + 1, amp), &value, temp, token_cap))
        return false;
      if (!param_name_deleted(name.data, name.len)) {
        if (!first) ser.Push('&');
        first = false;
        serialize_component(&ser, name.data, name.len);
        ser.Push('=');
        serialize_component(&ser, value.data, value.len);
      }
    }
    if (amp >= qn) break;
    i = amp + 1;
  }
  if (!ser.ok) return false;
  if (ser.len) {
    out.Push('?');
    out.Append(ser.data, ser.len);
  }
  if (!out.ok) return false;
  *output_length = out.len;
  return true;
}

}  // namespace fortis::compat::url
