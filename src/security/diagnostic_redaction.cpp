#include "ownership.h"
#include "diagnostic_redaction.h"

#include <stdlib.h>
#include <string.h>

#include "redaction_tables.h"

namespace fortis::security::redaction {
namespace {

// A owned code point string. Every append is bounded by
// kMaxCodePoints; a failed growth leaves `ok` false and stops appending.
struct Text {
  std::unique_ptr<uint32_t[], base::StorageDeleter> storage;
  uint32_t* data = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  bool ok = true;

  Text() = default;
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;


  bool Grow(size_t needed) {
    if (!ok) return false;
    if (needed <= capacity) return true;
    if (needed > kMaxCodePoints) { ok = false; return false; }
    size_t next = capacity ? capacity * 2 : 64;
    if (next < needed) next = needed;
    if (next > kMaxCodePoints) next = kMaxCodePoints;
    if (!base::ResizeStorage(storage, size * sizeof(uint32_t), next * sizeof(uint32_t))) { ok = false; return false; }
    data = storage.get();
    capacity = next;
    return true;
  }
  void Push(uint32_t cp) {
    if (Grow(size + 1)) data[size++] = cp;
  }
  void Append(const uint32_t* cps, size_t count) {
    if (!count || !Grow(size + count)) return;
    memcpy(data + size, cps, count * sizeof(uint32_t));
    size += count;
  }
  void Swap(Text& other) {
    storage.swap(other.storage);
    std::swap(data, other.data);
    std::swap(size, other.size);
    std::swap(capacity, other.capacity);
    std::swap(ok, other.ok);
  }
  uint32_t At(size_t index) const { return index < size ? data[index] : 0xFFFFFFFFu; }
};

// ── Character classes (ASCII \w, \d and \b; \s as below) ──

bool is_word(uint32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
bool is_alpha(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(uint32_t c) { return c >= '0' && c <= '9'; }
bool is_alnum(uint32_t c) { return is_alpha(c) || is_digit(c); }
bool is_hex(uint32_t c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_base58(uint32_t c) {
  return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') ||
         (c >= 'P' && c <= 'Z') || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}
// ECMAScript WhiteSpace and LineTerminator: the \s class.
bool is_space(uint32_t c) {
  return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0xa0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
         c == 0x205f || c == 0x3000 || c == 0xfeff;
}
uint32_t ascii_lower(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

// \b at `index` (between index - 1 and index).
bool boundary(const Text& text, size_t index) {
  const bool before = index > 0 && index <= text.size && is_word(text.data[index - 1]);
  const bool after = index < text.size && is_word(text.data[index]);
  return before != after;
}

bool starts_with(const Text& text, size_t index, const char* literal,
                 bool ignore_case = false) {
  for (size_t i = 0; literal[i]; ++i) {
    if (index + i >= text.size) return false;
    uint32_t actual = text.data[index + i];
    uint32_t wanted = static_cast<unsigned char>(literal[i]);
    if (ignore_case) { actual = ascii_lower(actual); wanted = ascii_lower(wanted); }
    if (actual != wanted) return false;
  }
  return true;
}

size_t run_length(const Text& text, size_t index, bool (*member)(uint32_t), size_t limit) {
  size_t length = 0;
  while (index + length < text.size && length < limit && member(text.data[index + length]))
    ++length;
  return length;
}

// ── UTF-8 ──

bool decode_utf8(const char* input, size_t length, Text* out) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(input);
  for (size_t i = 0; i < length;) {
    const unsigned char c = bytes[i];
    uint32_t cp = 0; size_t extra = 0; uint32_t minimum = 0;
    if (c < 0x80) { cp = c; }
    else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; minimum = 0x80; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; minimum = 0x800; }
    else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; minimum = 0x10000; }
    else return false;
    if (length - i - 1 < extra) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char next = bytes[i + k];
      if ((next & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (next & 0x3f);
    }
    if (extra && cp < minimum) return false;
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    out->Push(cp);
    i += extra + 1;
  }
  return out->ok;
}

bool encode_utf8(const Text& text, std::unique_ptr<char[], base::StorageDeleter>* output, size_t* output_length) {
  size_t bytes = 0;
  for (size_t i = 0; i < text.size; ++i) {
    const uint32_t cp = text.data[i];
    bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
  }
  auto out = base::AllocateStorage<char[]>(bytes + 1);
  if (!out) return false;
  size_t used = 0;
  for (size_t i = 0; i < text.size; ++i) {
    const uint32_t cp = text.data[i];
    if (cp < 0x80) {
      out[used++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      out[used++] = static_cast<char>(0xc0 | (cp >> 6));
      out[used++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out[used++] = static_cast<char>(0xe0 | (cp >> 12));
      out[used++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out[used++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out[used++] = static_cast<char>(0xf0 | (cp >> 18));
      out[used++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out[used++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out[used++] = static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  out[used] = '\0';
  *output = std::move(out);
  *output_length = used;
  return true;
}

void append_ascii(Text* out, const char* literal) {
  for (size_t i = 0; literal[i]; ++i) out->Push(static_cast<unsigned char>(literal[i]));
}

// ── foldForDetection ──

const NfkdMapping* find_nfkd(uint32_t cp) {
  size_t low = 0, high = kNfkdMappingCount;
  while (low < high) {
    const size_t middle = (low + high) / 2;
    if (kNfkdMappings[middle].code_point < cp) low = middle + 1;
    else high = middle;
  }
  return low < kNfkdMappingCount && kNfkdMappings[low].code_point == cp
      ? &kNfkdMappings[low] : nullptr;
}

// Canonical combining class rank; 0 for starters.
uint8_t combining_rank(uint32_t cp) {
  size_t low = 0, high = kCombiningRankCount;
  while (low < high) {
    const size_t middle = (low + high) / 2;
    if (kCombiningRanks[middle].code_point < cp) low = middle + 1;
    else high = middle;
  }
  return low < kCombiningRankCount && kCombiningRanks[low].code_point == cp
      ? kCombiningRanks[low].rank : 0;
}

constexpr uint32_t kSBase = 0xac00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11a7;
constexpr uint32_t kLCount = 19, kVCount = 21, kTCount = 28;
constexpr uint32_t kNCount = kVCount * kTCount, kSCount = kLCount * kNCount;

// The canonical primary composite of (starter, next), including Hangul.
bool compose(uint32_t starter, uint32_t next, uint32_t* composite) {
  if (starter >= kLBase && starter < kLBase + kLCount && next >= kVBase &&
      next < kVBase + kVCount) {
    *composite = kSBase + ((starter - kLBase) * kVCount + (next - kVBase)) * kTCount;
    return true;
  }
  if (starter >= kSBase && starter < kSBase + kSCount && (starter - kSBase) % kTCount == 0 &&
      next > kTBase && next < kTBase + kTCount) {
    *composite = starter + (next - kTBase);
    return true;
  }
  size_t low = 0, high = kCompositionPairCount;
  while (low < high) {
    const size_t middle = (low + high) / 2;
    const CompositionPair& pair = kCompositionPairs[middle];
    if (pair.first < starter || (pair.first == starter && pair.second < next)) low = middle + 1;
    else high = middle;
  }
  if (low < kCompositionPairCount && kCompositionPairs[low].first == starter &&
      kCompositionPairs[low].second == next) {
    *composite = kCompositionPairs[low].composite;
    return true;
  }
  return false;
}

void decompose(uint32_t cp, Text* out) {
  if (cp >= kSBase && cp < kSBase + kSCount) {
    const uint32_t index = cp - kSBase;
    out->Push(kLBase + index / kNCount);
    out->Push(kVBase + (index % kNCount) / kTCount);
    if (index % kTCount) out->Push(kTBase + index % kTCount);
    return;
  }
  const NfkdMapping* mapping = find_nfkd(cp);
  if (!mapping) { out->Push(cp); return; }
  out->Append(kNfkdPool + mapping->offset, mapping->length);
}

// String.prototype.normalize('NFKC'): full compatibility decomposition,
// canonical ordering of non-starter runs, then canonical composition with
// UAX #15 blocking.
bool nfkc(const Text& input, Text* output) {
  Text decomposed;
  for (size_t i = 0; i < input.size; ++i) decompose(input.data[i], &decomposed);
  if (!decomposed.ok) return false;
  auto ranks = base::AllocateStorage<uint8_t[]>(decomposed.size ? decomposed.size : 1);
  if (!ranks) return false;
  for (size_t i = 0; i < decomposed.size; ++i) ranks[i] = combining_rank(decomposed.data[i]);
  for (size_t i = 1; i < decomposed.size; ++i) {
    for (size_t j = i; j > 0 && ranks[j] && ranks[j - 1] > ranks[j]; --j) {
      const uint32_t cp = decomposed.data[j];
      decomposed.data[j] = decomposed.data[j - 1];
      decomposed.data[j - 1] = cp;
      const uint8_t rank = ranks[j];
      ranks[j] = ranks[j - 1];
      ranks[j - 1] = rank;
    }
  }
  if (decomposed.size) {
    size_t starter = 0;
    int last_class = ranks[0] ? 256 : 0;
    output->Push(decomposed.data[0]);
    for (size_t i = 1; i < decomposed.size; ++i) {
      const uint32_t cp = decomposed.data[i];
      const int cp_class = ranks[i];
      uint32_t composite = 0;
      if ((last_class < cp_class || last_class == 0) &&
          compose(output->data[starter], cp, &composite)) {
        output->data[starter] = composite;
        continue;
      }
      if (!cp_class) starter = output->size;
      last_class = cp_class;
      output->Push(cp);
    }
  }
  ranks.reset();
  return output->ok;
}

bool is_invisible(uint32_t cp) {
  for (size_t i = 0; i < kRedactionInvisibleCount; ++i)
    if (cp >= kRedactionInvisible[i].first && cp <= kRedactionInvisible[i].last) return true;
  return false;
}

// CONTROL_CHARS: U+0000-0008, U+000B, U+000C, U+000E-001F, U+007F.
bool is_unstorable_control(uint32_t cp) {
  return cp <= 0x08 || cp == 0x0b || cp == 0x0c || (cp >= 0x0e && cp <= 0x1f) || cp == 0x7f;
}

uint32_t unconfuse(uint32_t cp) {
  size_t low = 0, high = kRedactionConfusableCount;
  while (low < high) {
    const size_t middle = (low + high) / 2;
    if (kRedactionConfusables[middle].code_point < cp) low = middle + 1;
    else high = middle;
  }
  return low < kRedactionConfusableCount && kRedactionConfusables[low].code_point == cp
      ? static_cast<unsigned char>(kRedactionConfusables[low].ascii) : cp;
}

// /<\/?[a-zA-Z][a-zA-Z0-9]{0,14}\s*\/?>/
bool match_tag(const Text& text, size_t i, size_t* end) {
  if (text.At(i) != '<') return false;
  size_t j = i + 1;
  if (text.At(j) == '/') ++j;
  if (!is_alpha(text.At(j))) return false;
  ++j;
  j += run_length(text, j, is_alnum, 14);
  while (j < text.size && is_space(text.data[j])) ++j;
  if (text.At(j) == '/') ++j;
  if (text.At(j) != '>') return false;
  *end = j + 1;
  return true;
}

bool fold_for_detection(const Text& input, Text* output) {
  Text normalized;
  if (!nfkc(input, &normalized)) return false;
  Text visible;
  for (size_t i = 0; i < normalized.size; ++i) {
    const uint32_t cp = normalized.data[i];
    if (!is_invisible(cp) && !is_unstorable_control(cp)) visible.Push(cp);
  }
  for (size_t i = 0; i < visible.size;) {
    size_t end = 0;
    if (match_tag(visible, i, &end)) { output->Push(' '); i = end; continue; }
    output->Push(unconfuse(visible.data[i]));
    ++i;
  }
  return normalized.ok && visible.ok && output->ok;
}

// ── Global replacement ──

using Matcher = bool (*)(const Text& text, size_t index, size_t* end);

// String.prototype.replace with a global regex and a fixed replacement:
// leftmost matches, scanning resumes at each match end.
bool replace_all(const Text& input, Matcher matcher, const Text& replacement, Text* output) {
  for (size_t i = 0; i < input.size;) {
    size_t end = 0;
    if (matcher(input, i, &end) && end > i) {
      output->Append(replacement.data, replacement.size);
      i = end;
    } else {
      output->Push(input.data[i]);
      ++i;
    }
  }
  return input.ok && output->ok;
}

bool apply(Text* text, Matcher matcher, const Text& replacement) {
  Text next;
  if (!replace_all(*text, matcher, replacement, &next)) return false;
  text->Swap(next);
  return true;
}

// /\b[xyztuv](?:prv|pub)[1-9A-HJ-NP-Za-km-z]{100,112}\b/g
bool match_extended_key(const Text& text, size_t i, size_t* end) {
  const uint32_t c = text.At(i);
  if (!(c == 'x' || c == 'y' || c == 'z' || c == 't' || c == 'u' || c == 'v') ||
      !boundary(text, i) ||
      !(starts_with(text, i + 1, "prv") || starts_with(text, i + 1, "pub"))) return false;
  const size_t run = run_length(text, i + 4, is_base58, 113);
  if (run < 100 || run > 112 || !boundary(text, i + 4 + run)) return false;
  *end = i + 4 + run;
  return true;
}

// /\b[5KL9c][1-9A-HJ-NP-Za-km-z]{50,51}\b/g
bool match_wif(const Text& text, size_t i, size_t* end) {
  const uint32_t c = text.At(i);
  if (!(c == '5' || c == 'K' || c == 'L' || c == '9' || c == 'c') || !boundary(text, i))
    return false;
  const size_t run = run_length(text, i + 1, is_base58, 52);
  if (run < 50 || run > 51 || !boundary(text, i + 1 + run)) return false;
  *end = i + 1 + run;
  return true;
}

bool is_pem_label(uint32_t c) { return (c >= 'A' && c <= 'Z') || c == ' '; }

// `${prefix}(?:[A-Z ]*)PRIVATE KEY-----` at i; the greedy label backtracks
// to the longest label still followed by the suffix.
bool match_pem_line(const Text& text, size_t i, const char* prefix, size_t prefix_length,
                    size_t* end) {
  if (!starts_with(text, i, prefix)) return false;
  const size_t label = i + prefix_length;
  const size_t run = run_length(text, label, is_pem_label, text.size);
  for (size_t k = run + 1; k-- > 0;) {
    if (starts_with(text, label + k, "PRIVATE KEY-----")) {
      *end = label + k + 16;
      return true;
    }
  }
  return false;
}

// /-----BEGIN (?:[A-Z ]*)PRIVATE KEY-----[\s\S]*?-----END (?:[A-Z ]*)PRIVATE KEY-----/g
bool match_pem(const Text& text, size_t i, size_t* end) {
  if (!starts_with(text, i, "-----BEGIN ")) return false;
  const size_t label = i + 11;
  const size_t run = run_length(text, label, is_pem_label, text.size);
  for (size_t k = run + 1; k-- > 0;) {
    if (!starts_with(text, label + k, "PRIVATE KEY-----")) continue;
    for (size_t q = label + k + 16; q < text.size; ++q) {
      if (text.data[q] == '-' && match_pem_line(text, q, "-----END ", 9, end)) return true;
    }
  }
  return false;
}

size_t find_literal(const Text& text, size_t from, const char* literal, bool ignore_case) {
  for (size_t i = from; i < text.size; ++i)
    if (starts_with(text, i, literal, ignore_case)) return i;
  return text.size;
}

// /\{[^{}]*"crypto"\s*:\s*\{[\s\S]*?"ciphertext"[\s\S]*?\}[\s\S]*?\}/gi
bool match_keystore(const Text& text, size_t i, size_t* end) {
  if (text.At(i) != '{') return false;
  size_t j = i + 1;
  while (j < text.size && text.data[j] != '{' && text.data[j] != '}') ++j;
  if (text.At(j) != '{') return false;
  // [^{}]* must leave exactly `"crypto"\s*:\s*` before this brace.
  size_t k = j;
  while (k > i + 1 && is_space(text.data[k - 1])) --k;
  if (k <= i + 1 || text.data[k - 1] != ':') return false;
  --k;
  while (k > i + 1 && is_space(text.data[k - 1])) --k;
  if (k < i + 1 + 8 || !starts_with(text, k - 8, "\"crypto\"", true)) return false;
  const size_t cipher = find_literal(text, j + 1, "\"ciphertext\"", true);
  if (cipher == text.size) return false;
  size_t close = cipher + 12;
  while (close < text.size && text.data[close] != '}') ++close;
  if (close == text.size) return false;
  size_t second = close + 1;
  while (second < text.size && text.data[second] != '}') ++second;
  if (second == text.size) return false;
  *end = second + 1;
  return true;
}

// ── Bare 32-byte hex keys ──

// /\b(0x)?[0-9a-fA-F]{64}\b/
bool match_hex32(const Text& text, size_t i, size_t* end) {
  if (!boundary(text, i)) return false;
  if (text.At(i) == '0' && text.At(i + 1) == 'x' &&
      run_length(text, i + 2, is_hex, 64) == 64 && boundary(text, i + 66)) {
    *end = i + 66;
    return true;
  }
  if (run_length(text, i, is_hex, 64) == 64 && boundary(text, i + 64)) {
    *end = i + 64;
    return true;
  }
  return false;
}

size_t utf16_units(uint32_t cp) { return cp > 0xffff ? 2 : 1; }

// `${text.slice(start - 60, start)} ${text.slice(end, end + 40)}` measured in
// UTF-16 units. Half of a split surrogate pair counts as a lone surrogate:
// neither a word nor a space character, so U+FFFD stands in.
void hex_context(const Text& text, size_t start, size_t end, Text* context) {
  size_t first = start, units = 0;
  bool split_before = false;
  while (first > 0) {
    const size_t width = utf16_units(text.data[first - 1]);
    if (units + width > 60) { split_before = units < 60; break; }
    units += width;
    --first;
  }
  if (split_before) context->Push(0xfffd);
  context->Append(text.data + first, start - first);
  context->Push(' ');
  units = 0;
  size_t last = end;
  while (last < text.size) {
    const size_t width = utf16_units(text.data[last]);
    if (units + width > 40) {
      context->Append(text.data + end, last - end);
      if (units < 40) context->Push(0xfffd);
      return;
    }
    units += width;
    ++last;
  }
  context->Append(text.data + end, last - end);
}

// /\b(w1\s*w2|...)\b/i test: words separated by a space in `alternatives`
// accept any \s* run there.
bool context_has(const Text& context, const char* const* alternatives, size_t count) {
  for (size_t i = 0; i < context.size; ++i) {
    if (!boundary(context, i)) continue;
    for (size_t a = 0; a < count; ++a) {
      size_t j = i;
      bool matched = true;
      for (const char* p = alternatives[a]; *p; ++p) {
        if (*p == ' ') {
          while (j < context.size && is_space(context.data[j])) ++j;
          continue;
        }
        if (j >= context.size ||
            ascii_lower(context.data[j]) != static_cast<unsigned char>(*p)) {
          matched = false;
          break;
        }
        ++j;
      }
      if (matched && boundary(context, j)) return true;
    }
  }
  return false;
}

const char* const kKeyContext[] = {"private key", "privkey", "priv key", "secret key",
    "seed", "mnemonic", "recovery", "passphrase", "import", "wallet key"};
const char* const kHashContext[] = {"tx", "transaction", "hash", "txid", "txhash",
    "block", "blockhash", "digest", "signature", "sig", "salt", "commitment", "merkle", "root"};

bool redact_hex_keys(Text* text, const Text& placeholder) {
  Text next;
  for (size_t i = 0; i < text->size;) {
    size_t end = 0;
    if (!match_hex32(*text, i, &end)) {
      next.Push(text->data[i]);
      ++i;
      continue;
    }
    Text context;
    hex_context(*text, i, end, &context);
    if (!context.ok) return false;
    const bool key = context_has(context, kKeyContext, sizeof(kKeyContext) / sizeof(kKeyContext[0]));
    if (!key && context_has(context, kHashContext, sizeof(kHashContext) / sizeof(kHashContext[0])))
      next.Append(text->data + i, end - i);
    else
      next.Append(placeholder.data, placeholder.size);
    i = end;
  }
  if (!next.ok) return false;
  text->Swap(next);
  return true;
}

// ── BIP-39 runs ──

constexpr size_t kPartialRun = 7;

bool bip39_word(const Text& text, size_t start, size_t end) {
  const size_t length = end - start;
  if (length < 3 || length > 8) return false;
  char word[9];
  for (size_t i = 0; i < length; ++i) word[i] = static_cast<char>(ascii_lower(text.data[start + i]));
  word[length] = '\0';
  size_t low = 0, high = 2048;
  while (low < high) {
    const size_t middle = (low + high) / 2;
    const int order = strcmp(kBip39English[middle], word);
    if (order == 0) return true;
    if (order < 0) low = middle + 1;
    else high = middle;
  }
  return false;
}

// /^[\s,;:.!?'"“”‘’`´()[\]{}<>|/\\\-–—]*$/
bool plain_gap_char(uint32_t c) {
  if (is_space(c)) return true;
  switch (c) {
    case ',': case ';': case ':': case '.': case '!': case '?': case '\'': case '"':
    case 0x201c: case 0x201d: case 0x2018: case 0x2019: case '`': case 0xb4:
    case '(': case ')': case '[': case ']': case '{': case '}': case '<': case '>':
    case '|': case '/': case '\\': case '-': case 0x2013: case 0x2014:
      return true;
    default:
      return false;
  }
}

// /^[\s,\-–—]*[#(]?\d{1,3}[.):\]]?[\s]*$/
bool numbered_gap(const Text& text, size_t start, size_t end) {
  size_t i = start;
  while (i < end && (is_space(text.data[i]) || text.data[i] == ',' || text.data[i] == '-' ||
                     text.data[i] == 0x2013 || text.data[i] == 0x2014)) ++i;
  if (i < end && (text.data[i] == '#' || text.data[i] == '(')) ++i;
  size_t digits = 0;
  while (i < end && is_digit(text.data[i])) { ++i; ++digits; }
  if (digits < 1 || digits > 3) return false;
  if (i < end && (text.data[i] == '.' || text.data[i] == ')' || text.data[i] == ':' ||
                  text.data[i] == ']')) ++i;
  while (i < end && is_space(text.data[i])) ++i;
  return i == end;
}

bool gap_continues_phrase(const Text& text, size_t start, size_t end) {
  bool plain = true;
  for (size_t i = start; i < end && plain; ++i) plain = plain_gap_char(text.data[i]);
  return plain || numbered_gap(text, start, end);
}

struct Span { size_t start; size_t end; };

bool redact_mnemonics(Text* text, const Text& placeholder) {
  std::unique_ptr<Span[], base::StorageDeleter> spans;
  size_t span_count = 0, span_capacity = 0;
  bool ok = true;
  size_t run_start = SIZE_MAX, run_end = SIZE_MAX, run_count = 0;
  auto flush = [&]() {
    if (run_count >= kPartialRun && ok) {
      if (span_count == span_capacity) {
        const size_t next = span_capacity ? span_capacity * 2 : 8;
        if (!base::ResizeStorage(spans, span_count * sizeof(Span), next * sizeof(Span))) { ok = false; return; }
        span_capacity = next;
      }
      spans[span_count++] = {run_start, run_end};
    }
    run_start = run_end = SIZE_MAX;
    run_count = 0;
  };
  for (size_t i = 0; i < text->size;) {
    if (!is_alpha(text->data[i])) { ++i; continue; }
    const size_t start = i;
    while (i < text->size && is_alpha(text->data[i])) ++i;
    if (bip39_word(*text, start, i)) {
      if (run_end != SIZE_MAX && !gap_continues_phrase(*text, run_end, start)) flush();
      if (run_start == SIZE_MAX) run_start = start;
      run_end = i;
      ++run_count;
    } else {
      flush();
    }
  }
  flush();
  if (!ok) { spans.reset(); return false; }
  if (!span_count) return true;
  Text next;
  size_t cursor = 0;
  for (size_t s = 0; s < span_count; ++s) {
    next.Append(text->data + cursor, spans[s].start - cursor);
    next.Append(placeholder.data, placeholder.size);
    cursor = spans[s].end;
  }
  next.Append(text->data + cursor, text->size - cursor);
  spans.reset();
  if (!next.ok) return false;
  text->Swap(next);
  return true;
}

// ── redactDiagnosticText's identifier scrubbing ──

bool token_char(uint32_t c) { return is_alnum(c) || c == '_' || c == '-'; }

// /fintel_dev_[A-Za-z0-9_-]+/g
bool match_device_token(const Text& text, size_t i, size_t* end) {
  if (!starts_with(text, i, "fintel_dev_")) return false;
  const size_t run = run_length(text, i + 11, token_char, text.size);
  if (!run) return false;
  *end = i + 11 + run;
  return true;
}

// /0x[0-9a-fA-F]{32,}/g
bool match_long_hex(const Text& text, size_t i, size_t* end) {
  if (text.At(i) != '0' || text.At(i + 1) != 'x') return false;
  const size_t run = run_length(text, i + 2, is_hex, text.size);
  if (run < 32) return false;
  *end = i + 2 + run;
  return true;
}

// /\b(?:bc1|tb1|ltc1|addr1|cosmos1|osmo1|inj1)[a-zA-Z0-9]{20,}\b/g
bool match_bech32(const Text& text, size_t i, size_t* end) {
  if (!boundary(text, i)) return false;
  static const char* const kPrefixes[] = {"bc1", "tb1", "ltc1", "addr1", "cosmos1", "osmo1", "inj1"};
  for (const char* prefix : kPrefixes) {
    if (!starts_with(text, i, prefix)) continue;
    const size_t body = i + strlen(prefix);
    const size_t run = run_length(text, body, is_alnum, text.size);
    if (run >= 20 && boundary(text, body + run)) {
      *end = body + run;
      return true;
    }
  }
  return false;
}

// /\b[1-9A-HJ-NP-Za-km-z]{25,88}\b/g
bool match_base58(const Text& text, size_t i, size_t* end) {
  if (!boundary(text, i)) return false;
  const size_t run = run_length(text, i, is_base58, 89);
  if (run < 25 || run > 88 || !boundary(text, i + run)) return false;
  *end = i + run;
  return true;
}

// /\b\d+\.\d+\.\d+\b/g
bool match_dotted_digits(const Text& text, size_t i, size_t* end) {
  if (!is_digit(text.At(i)) || !boundary(text, i)) return false;
  size_t j = i;
  for (int part = 0; part < 3; ++part) {
    if (part) {
      if (text.At(j) != '.') return false;
      ++j;
    }
    const size_t run = run_length(text, j, is_digit, text.size);
    if (!run) return false;
    j += run;
  }
  if (!boundary(text, j)) return false;
  *end = j;
  return true;
}

bool not_slash_or_space(uint32_t c) { return c != '/' && !is_space(c); }
bool not_backslash_or_space(uint32_t c) { return c != '\\' && !is_space(c); }

// /\/Users\/[^/\s]+/g
bool match_unix_home(const Text& text, size_t i, size_t* end) {
  if (!starts_with(text, i, "/Users/")) return false;
  const size_t run = run_length(text, i + 7, not_slash_or_space, text.size);
  if (!run) return false;
  *end = i + 7 + run;
  return true;
}

// /\\Users\\[^\\\s]+/gi
bool match_windows_home(const Text& text, size_t i, size_t* end) {
  if (!starts_with(text, i, "\\Users\\", true)) return false;
  const size_t run = run_length(text, i + 7, not_backslash_or_space, text.size);
  if (!run) return false;
  *end = i + 7 + run;
  return true;
}

}  // namespace

namespace {

// redactSecrets: PATTERN_RULES in order, bare hex keys, then BIP-39 runs
// (last, so a placeholder's English words never extend a run).
bool redact_secrets(Text* text, const Text& placeholder) {
  return apply(text, match_extended_key, placeholder) &&
         apply(text, match_wif, placeholder) &&
         apply(text, match_pem, placeholder) &&
         apply(text, match_keystore, placeholder) &&
         redact_hex_keys(text, placeholder) &&
         redact_mnemonics(text, placeholder);
}

bool load_placeholder(Text* placeholder) {
  return decode_utf8(kRedactionPlaceholder, strlen(kRedactionPlaceholder), placeholder);
}

}  // namespace

bool RedactSecrets(const char* input, size_t length, std::unique_ptr<char[], base::StorageDeleter>* output, size_t* output_length,
                   bool* redacted) {
  if (!output || !output_length || !redacted || (!input && length)) return false;
  // Input views may point into the previous output; keep it alive while reading.
  const auto previous_output = std::move(*output);
  *output_length = 0;
  *redacted = false;
  Text placeholder, raw, text, before;
  if (!load_placeholder(&placeholder) || !decode_utf8(input, length, &raw) ||
      !fold_for_detection(raw, &text))
    return false;
  before.Append(text.data, text.size);
  if (!before.ok || !redact_secrets(&text, placeholder)) return false;
  // A rule replaced something exactly when the text changed: no secret
  // pattern matches the placeholder it is replaced with.
  *redacted = text.size != before.size ||
              (text.size && memcmp(text.data, before.data, text.size * sizeof(uint32_t)) != 0);
  return encode_utf8(text, output, output_length);
}

bool FoldForDetection(const char* input, size_t length, std::unique_ptr<char[], base::StorageDeleter>* output, size_t* output_length) {
  if (!output || !output_length || (!input && length)) return false;
  // Input views may point into the previous output; keep it alive while reading.
  const auto previous_output = std::move(*output);
  *output_length = 0;
  Text raw, text;
  if (!decode_utf8(input, length, &raw) || !fold_for_detection(raw, &text)) return false;
  return encode_utf8(text, output, output_length);
}

bool RedactDiagnosticText(const char* input, size_t length, std::unique_ptr<char[], base::StorageDeleter>* output,
                          size_t* output_length) {
  if (!output || !output_length || (!input && length)) return false;
  // Input views may point into the previous output; keep it alive while reading.
  const auto previous_output = std::move(*output);
  *output_length = 0;
  Text placeholder, token, hex, address, unix_home, windows_home;
  if (!load_placeholder(&placeholder)) return false;
  append_ascii(&token, "[REDACTED_TOKEN]");
  append_ascii(&hex, "[REDACTED_HEX]");
  append_ascii(&address, "[REDACTED_ADDRESS]");
  append_ascii(&unix_home, "/Users/[REDACTED]");
  append_ascii(&windows_home, "\\Users\\[REDACTED]");
  Text raw, text;
  if (!decode_utf8(input, length, &raw) || !fold_for_detection(raw, &text)) return false;
  if (!redact_secrets(&text, placeholder)) return false;
  // redactDiagnosticText's identifier replacements, in source order.
  if (!apply(&text, match_device_token, token) ||
      !apply(&text, match_long_hex, hex) ||
      !apply(&text, match_bech32, address) ||
      !apply(&text, match_base58, address) ||
      !apply(&text, match_dotted_digits, address) ||
      !apply(&text, match_unix_home, unix_home) ||
      !apply(&text, match_windows_home, windows_home)) return false;
  return encode_utf8(text, output, output_length);
}

}  // namespace fortis::security::redaction
