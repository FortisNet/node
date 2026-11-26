#pragma once

#include "idna.h"

#include <stddef.h>
#include <stdint.h>

// This server's URL parser: the WHATWG URL Standard as Ada 2.9.2 implements
// it (the parser behind `new URL(input, base?)` in Node.js 22, Unicode 15.1),
// which tests/whatwg_url_test.mjs pins against. The IDNA step is Ada's
// to_ascii (src/compat/idna.cpp, tables in src/compat/idna_tables.cpp
// generated from the pinned Ada source by scripts/generate_idna_tables.py).
//
// Security-relevant (docs/compat-decisions.md#whatwg-url): the parsed scheme,
// host type, host and port decide which feed, image and redirect targets the
// server may dial and which provider links reach the app. Never replace a
// call with ad-hoc string checks: IPv4 number forms and percent-encoded
// hosts must be normalised before they are judged.
//
// Allocation-free, no C++ standard library, guest-safe and thread-safe:
// every byte of working memory is the caller's UrlScratch.
namespace fortis::compat::url {

// Inputs longer than this are refused (a native bound; Ada has none).
constexpr size_t kMaxInputBytes = 8192;
// Serialized href capacity. A URL whose serialization does not fit is refused.
constexpr size_t kMaxHrefBytes = 16384;
constexpr uint32_t kOmitted = UINT32_MAX;

enum class HostType : uint8_t { kNone, kEmpty, kDomain, kIpv4, kIpv6, kOpaque };

// One parsed URL: the href serialization plus Ada url_aggregator-style
// component offsets into it.
struct Url {
  char href[kMaxHrefBytes];
  size_t href_length = 0;       // href[href_length] == '\0'
  uint32_t protocol_end = 0;    // href[0, protocol_end) is e.g. "https:"
  uint32_t username_end = 0;
  uint32_t host_start = 0;
  uint32_t host_end = 0;
  int32_t port = -1;            // -1 when absent; default ports are elided
  uint32_t pathname_start = 0;
  uint32_t search_start = kOmitted;  // offset of '?', or kOmitted
  uint32_t hash_start = kOmitted;    // offset of '#', or kOmitted
  HostType host_type = HostType::kNone;
  bool special = false;
  // Internal parser state carried so a Url can serve as a base or be edited.
  uint8_t scheme_type_ = 1;  // Ada scheme::type (1 = NOT_SPECIAL)
  bool has_opaque_path_ = false;
};

// Working memory for one Parse/DomainToAscii/CanonicalArticleUrl call (IDNA
// UTF-32 buffers, the USVString-converted input, percent-coding, host, path
// and setter state, and the URL CanonicalArticleUrl edits): about 200 KiB.
// Callers own it; one scratch must not be used by two threads at once.
struct UrlScratch {
  idna::Scratch idna;
  char input[3 * kMaxInputBytes];  // cleaned input after U+FFFD replacement
  char path[kMaxHrefBytes];        // pathname being rebuilt; host copies
  char host[kMaxHrefBytes];        // percent-decoded host / encoded pieces
  char ascii[kMaxHrefBytes];       // IDNA output
  char other[kMaxHrefBytes];       // aliasing copies inside buffer edits
  char setter[kMaxHrefBytes];      // setter input (host, previous host)
  char params[kMaxHrefBytes];      // URLSearchParams decode/serialize
  Url canonical;                   // the URL CanonicalArticleUrl edits
};

struct View {
  const char* data = nullptr;
  size_t length = 0;
};

// new URL(input, base): true when Ada would return a URL. `input` is UTF-8;
// invalid UTF-8 sequences are first replaced with U+FFFD as a USVString
// conversion would (callers holding UTF-16 with lone surrogates must
// replace those with U+FFFD before encoding). `base` may be null. `out` may
// not alias `base`.
bool Parse(const char* input, size_t length, const Url* base, Url* out,
           UrlScratch* scratch);

View Href(const Url& url);
View Protocol(const Url& url);  // "https:"
View Username(const Url& url);
View Password(const Url& url);
View Hostname(const Url& url);  // url.hostname; IPv6 keeps its brackets
View Host(const Url& url);      // url.host: hostname[:port]
View Port(const Url& url);      // url.port: "" or the digits
View Pathname(const Url& url);
View Search(const Url& url);    // url.search: "" or "?…" ("" for a lone "?")
View Hash(const Url& url);      // url.hash: "" or "#…" ("" for a lone "#")

// WHATWG "domain to ASCII" with beStrict false, as Ada's host parser runs it
// for a special URL: percent-decoding, ada::idna::to_ascii, the forbidden
// domain code point check. Writes the ASCII host (not NUL-terminated) and
// returns its length, or returns false on failure. IPv4 detection is not
// applied here.
bool DomainToAscii(const char* utf8, size_t length, char* output, size_t capacity,
                   size_t* output_length, UrlScratch* scratch);

// The TypeScript canonicalUrl for an input that `new URL` accepts:
//   hash cleared; protocol set to "https:"; hostname lowercased with one
//   leading "www." removed (through the hostname setter); every search
//   parameter whose name matches /^(?:utm_|ref$|source$|fbclid$|gclid$|mc_)/i
//   deleted; returns `${hostname}${pathname without trailing '/'+}` plus
//   `?${searchParams.toString()}` when that is non-empty.
// False when `new URL(input)` throws (the caller applies the TypeScript
// fallback `url.trim().toLowerCase()` itself) or the output does not fit.
bool CanonicalArticleUrl(const char* utf8, size_t length, char* output, size_t capacity,
                         size_t* output_length, UrlScratch* scratch);

}  // namespace fortis::compat::url
