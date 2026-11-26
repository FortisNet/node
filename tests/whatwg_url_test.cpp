#include <algorithm>
// stdin/stdout driver for tests/whatwg_url_test.mjs. One request per line:
//   P <hex input> <hex base | ->  -> "null" | "basefail" | ten tab-separated
//        hex fields: href protocol username password host hostname port
//        pathname search hash
//   C <hex input>                 -> "null" | hex canonicalUrl
//   X <hex prefix> <hex suffix>   -> "<count> <sha256> <block sha256>..." over
//        `${cp.toString(16)} ${href ?? 'null'}\n` for every scalar value
//        U+0080..U+10FFFF except surrogates (blocks of 4096 code points)
//   Y <hex prefix> <hex suffix> <hex before> <hex after>  -> as X, over
//        `${prefix}${punycode(before + cp + after)}${suffix}` (an RFC 3492
//        encoder independent of src/compat/idna.cpp)
//   L/M ... <block>  -> one block's X/Y lines (diagnosis)
#include "../src/compat/whatwg_url.h"

#include "mbedtls/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

fortis::compat::url::UrlScratch g_scratch;
fortis::compat::url::Url g_url;
fortis::compat::url::Url g_base;
char g_line[1 << 20];
char g_a[1 << 18];
char g_b[1 << 18];
char g_out[1 << 16];

size_t unhex(const char* hex, size_t n, char* out) {
  size_t o = 0;
  for (size_t i = 0; i + 1 < n; i += 2) {
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    out[o++] = static_cast<char>(v(hex[i]) << 4 | v(hex[i + 1]));
  }
  return o;
}

void put_hex(const char* data, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    putchar(kHex[static_cast<unsigned char>(data[i]) >> 4]);
    putchar(kHex[static_cast<unsigned char>(data[i]) & 15]);
  }
}

size_t encode_cp(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xc0 | cp >> 6);
    out[1] = static_cast<char>(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xe0 | cp >> 12);
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out[2] = static_cast<char>(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = static_cast<char>(0xf0 | cp >> 18);
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
  out[3] = static_cast<char>(0x80 | (cp & 0x3f));
  return 4;
}

uint32_t g_before[16], g_after[16];
size_t g_before_n = 0, g_after_n = 0;
bool g_punycode = false;

size_t decode_utf8(const char* s, size_t n, uint32_t* out) {
  size_t o = 0;
  for (size_t i = 0; i < n;) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t k;
    if (c < 0x80) { cp = c; k = 1; }
    else if (c < 0xe0) { cp = c & 0x1f; k = 2; }
    else if (c < 0xf0) { cp = c & 0x0f; k = 3; }
    else { cp = c & 0x07; k = 4; }
    for (size_t j = 1; j < k; ++j) cp = cp << 6 | (static_cast<unsigned char>(s[i + j]) & 0x3f);
    out[o++] = cp;
    i += k;
  }
  return o;
}

// RFC 3492 encoding (no "xn--"), as the generator's JavaScript does it.
size_t punycode(const uint32_t* in, size_t count, char* out) {
  const uint32_t base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
  auto adapt = [&](uint32_t delta, uint32_t points, bool first) {
    delta = first ? delta / damp : delta >> 1;
    delta += delta / points;
    uint32_t k = 0;
    while (delta > ((base - tmin) * tmax) >> 1) { delta /= base - tmin; k += base; }
    return k + ((base - tmin + 1) * delta) / (delta + skew);
  };
  auto digit = [](uint32_t d) { return static_cast<char>(d < 26 ? d + 97 : d + 22); };
  size_t o = 0;
  for (size_t i = 0; i < count; ++i) if (in[i] < 0x80) out[o++] = static_cast<char>(in[i]);
  const size_t b = o;
  size_t h = b;
  if (b > 0) out[o++] = '-';
  uint32_t n = 128, delta = 0, bias = 72;
  while (h < count) {
    uint32_t m = 0xffffffffu;
    for (size_t i = 0; i < count; ++i) if (in[i] >= n && in[i] < m) m = in[i];
    delta += (m - n) * static_cast<uint32_t>(h + 1);
    n = m;
    for (size_t i = 0; i < count; ++i) {
      if (in[i] < n) delta++;
      if (in[i] == n) {
        uint32_t q = delta;
        for (uint32_t k = base;; k += base) {
          const uint32_t t = k <= bias ? tmin : k >= bias + tmax ? tmax : k - bias;
          if (q < t) break;
          out[o++] = digit(t + (q - t) % (base - t));
          q = (q - t) / (base - t);
        }
        out[o++] = digit(q);
        bias = adapt(delta, static_cast<uint32_t>(h + 1), h == b);
        delta = 0;
        h++;
      }
    }
    delta++;
    n++;
  }
  return o;
}

// Writes the probe line for `cp` into g_out; returns its length.
size_t probe_line(const char* prefix, size_t pn, const char* suffix, size_t sn, uint32_t cp) {
  char input[512];
  size_t n = 0;
  memcpy(input, prefix, pn);
  n += pn;
  if (g_punycode) {
    uint32_t label[40];
    size_t ln = 0;
    for (size_t i = 0; i < g_before_n; ++i) label[ln++] = g_before[i];
    label[ln++] = cp;
    for (size_t i = 0; i < g_after_n; ++i) label[ln++] = g_after[i];
    n += punycode(label, ln, input + n);
  } else {
    n += encode_cp(cp, input + n);
  }
  memcpy(input + n, suffix, sn);
  n += sn;
  const int head = snprintf(g_out, sizeof(g_out), "%x ", cp);
  size_t len = static_cast<size_t>(head);
  if (fortis::compat::url::Parse(input, n, nullptr, &g_url, &g_scratch)) {
    memcpy(g_out + len, g_url.href, g_url.href_length);
    len += g_url.href_length;
  } else {
    std::copy_n("null", 4, g_out + len);
    len += 4;
  }
  g_out[len++] = '\n';
  return len;
}

void hex_digest(const unsigned char digest[32], char out[65]) {
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 15];
  }
  out[64] = '\0';
}

}  // namespace

int main() {
  while (fgets(g_line, sizeof(g_line), stdin)) {
    size_t n = strlen(g_line);
    while (n && (g_line[n - 1] == '\n' || g_line[n - 1] == '\r')) g_line[--n] = '\0';
    char* fields[6] = {};
    size_t count = 0;
    char* save = nullptr;
    for (char* t = strtok_r(g_line, " ", &save); t && count < 6; t = strtok_r(nullptr, " ", &save))
      fields[count++] = t;
    if (!count) continue;
    const char mode = fields[0][0];
    if (mode == 'P' && count >= 2) {
      const size_t an = unhex(fields[1], strlen(fields[1]), g_a);
      const fortis::compat::url::Url* base = nullptr;
      if (count >= 3 && strcmp(fields[2], "-") != 0) {
        const size_t bn = unhex(fields[2], strlen(fields[2]), g_b);
        if (!fortis::compat::url::Parse(g_b, bn, nullptr, &g_base, &g_scratch)) {
          puts("basefail");
          continue;
        }
        base = &g_base;
      }
      if (!fortis::compat::url::Parse(g_a, an, base, &g_url, &g_scratch)) {
        puts("null");
        continue;
      }
      using fortis::compat::url::View;
      const View views[] = {fortis::compat::url::Href(g_url), fortis::compat::url::Protocol(g_url),
                            fortis::compat::url::Username(g_url), fortis::compat::url::Password(g_url),
                            fortis::compat::url::Host(g_url), fortis::compat::url::Hostname(g_url),
                            fortis::compat::url::Port(g_url), fortis::compat::url::Pathname(g_url),
                            fortis::compat::url::Search(g_url), fortis::compat::url::Hash(g_url)};
      for (size_t i = 0; i < 10; ++i) {
        if (i) putchar('\t');
        put_hex(views[i].data, views[i].length);
        if (!views[i].length) putchar('-');
      }
      putchar('\n');
    } else if (mode == 'C' && count >= 2) {
      const size_t an = unhex(fields[1], strlen(fields[1]), g_a);
      size_t out_n = 0;
      if (!fortis::compat::url::CanonicalArticleUrl(g_a, an, g_b, sizeof(g_b), &out_n, &g_scratch)) {
        puts("null");
        continue;
      }
      put_hex(g_b, out_n);
      if (!out_n) putchar('-');
      putchar('\n');
    } else if ((mode == 'X' || mode == 'L' || mode == 'Y' || mode == 'M') && count >= 3) {
      const size_t pn = unhex(fields[1], strlen(fields[1]), g_a);
      const size_t sn = strcmp(fields[2], "-") == 0 ? 0 : unhex(fields[2], strlen(fields[2]), g_b);
      g_punycode = mode == 'Y' || mode == 'M';
      size_t next = 3;
      if (g_punycode) {
        char tmp[64];
        g_before_n = count > 3 && strcmp(fields[3], "-") != 0
            ? decode_utf8(tmp, unhex(fields[3], strlen(fields[3]), tmp), g_before) : 0;
        g_after_n = count > 4 && strcmp(fields[4], "-") != 0
            ? decode_utf8(tmp, unhex(fields[4], strlen(fields[4]), tmp), g_after) : 0;
        next = 5;
      }
      if (mode == 'L' || mode == 'M') {
        const unsigned long block = count > next ? strtoul(fields[next], nullptr, 10) : 0;
        for (uint32_t cp = static_cast<uint32_t>(block << 12); cp < ((block + 1) << 12); ++cp) {
          if (cp < 0x80 || (cp >= 0xd800 && cp <= 0xdfff)) continue;
          fwrite(g_out, 1, probe_line(g_a, pn, g_b, sn, cp), stdout);
        }
        puts("end");
        continue;
      }
      mbedtls_sha256_context total;
      mbedtls_sha256_init(&total);
      mbedtls_sha256_starts(&total, 0);
      static char blocks[0x110][65];
      size_t probes = 0;
      for (uint32_t block = 0; block <= (0x10ffff >> 12); ++block) {
        mbedtls_sha256_context one;
        mbedtls_sha256_init(&one);
        mbedtls_sha256_starts(&one, 0);
        for (uint32_t cp = block << 12; cp < ((block + 1) << 12); ++cp) {
          if (cp < 0x80 || (cp >= 0xd800 && cp <= 0xdfff)) continue;
          const size_t len = probe_line(g_a, pn, g_b, sn, cp);
          mbedtls_sha256_update(&one, reinterpret_cast<unsigned char*>(g_out), len);
          mbedtls_sha256_update(&total, reinterpret_cast<unsigned char*>(g_out), len);
          ++probes;
        }
        unsigned char digest[32];
        mbedtls_sha256_finish(&one, digest);
        mbedtls_sha256_free(&one);
        hex_digest(digest, blocks[block]);
      }
      unsigned char digest[32];
      mbedtls_sha256_finish(&total, digest);
      mbedtls_sha256_free(&total);
      char hex[65];
      hex_digest(digest, hex);
      printf("%zu %s", probes, hex);
      for (uint32_t block = 0; block <= (0x10ffff >> 12); ++block) printf(" %s", blocks[block]);
      putchar('\n');
    } else {
      puts("error");
    }
    fflush(stdout);
  }
  return 0;
}
