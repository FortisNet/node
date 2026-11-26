#include "idna.h"

#include <string.h>

// Port of ada::idna (Ada v2.9.2): unicode_transcoding.cpp, mapping.cpp,
// normalization.cpp, punycode.cpp, validity.cpp and to_ascii.cpp. Comments
// name the Ada function each block mirrors; deviations are bounds only.
namespace fortis::compat::idna {
namespace {

namespace t = fortis::compat::idna_tables;

// ---- unicode_transcoding.cpp: utf8_to_utf32 --------------------------------
// Returns the number of code points, or 0 on invalid UTF-8 (Ada's error), or
// SIZE_MAX when the output bound is exceeded.
size_t utf8_to_utf32(const char* buf, size_t len, uint32_t* out, size_t capacity) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(buf);
  size_t pos = 0, written = 0;
  while (pos < len) {
    if (written == capacity) return SIZE_MAX;
    const uint8_t lead = data[pos];
    uint32_t code_point = 0;
    if (lead < 0x80) {
      code_point = lead;
      pos += 1;
    } else if ((lead & 0xe0) == 0xc0) {
      if (pos + 1 >= len) return 0;
      if ((data[pos + 1] & 0xc0) != 0x80) return 0;
      code_point = static_cast<uint32_t>(lead & 0x1f) << 6 | (data[pos + 1] & 0x3f);
      if (code_point < 0x80 || 0x7ff < code_point) return 0;
      pos += 2;
    } else if ((lead & 0xf0) == 0xe0) {
      if (pos + 2 >= len) return 0;
      if ((data[pos + 1] & 0xc0) != 0x80 || (data[pos + 2] & 0xc0) != 0x80) return 0;
      code_point = static_cast<uint32_t>(lead & 0x0f) << 12 |
                   static_cast<uint32_t>(data[pos + 1] & 0x3f) << 6 | (data[pos + 2] & 0x3f);
      if (code_point < 0x800 || 0xffff < code_point ||
          (0xd7ff < code_point && code_point < 0xe000)) return 0;
      pos += 3;
    } else if ((lead & 0xf8) == 0xf0) {
      if (pos + 3 >= len) return 0;
      if ((data[pos + 1] & 0xc0) != 0x80 || (data[pos + 2] & 0xc0) != 0x80 ||
          (data[pos + 3] & 0xc0) != 0x80) return 0;
      code_point = static_cast<uint32_t>(lead & 0x07) << 18 |
                   static_cast<uint32_t>(data[pos + 1] & 0x3f) << 12 |
                   static_cast<uint32_t>(data[pos + 2] & 0x3f) << 6 | (data[pos + 3] & 0x3f);
      if (code_point <= 0xffff || 0x10ffff < code_point) return 0;
      pos += 4;
    } else {
      return 0;
    }
    out[written++] = code_point;
  }
  return written;
}

// ---- mapping.cpp -----------------------------------------------------------
uint32_t find_range_index(uint32_t key) {
  const uint32_t len = static_cast<uint32_t>(t::kMappingTableCount / 2);
  uint32_t low = 0, high = len - 1;
  while (low <= high) {
    const uint32_t middle = (low + high) >> 1;
    const uint32_t value = t::kMappingTable[middle * 2];
    if (value < key) {
      low = middle + 1;
    } else if (value > key) {
      if (middle == 0) break;  // Ada's unsigned high underflows and ends the loop
      high = middle - 1;
    } else {
      return middle;
    }
  }
  return low == 0 ? 0 : low - 1;
}

// ada::idna::map: false on a disallowed code point (Ada returns "" then) or
// when the output bound is exceeded.
bool map(const uint32_t* input, size_t length, uint32_t* out, size_t capacity,
         size_t* out_length) {
  size_t written = 0;
  for (size_t i = 0; i < length; ++i) {
    const uint32_t x = input[i];
    const uint32_t descriptor = t::kMappingTable[find_range_index(x) * 2 + 1];
    const uint8_t code = static_cast<uint8_t>(descriptor);
    switch (code) {
      case 0: break;  // ignored
      case 1:
        if (written == capacity) return false;
        out[written++] = x;
        break;
      case 2: return false;  // disallowed
      default: {
        const size_t count = descriptor >> 24;
        const size_t index = static_cast<uint16_t>(descriptor >> 8);
        for (size_t k = index; k < index + count; ++k) {
          if (written == capacity) return false;
          out[written++] = t::kMappings[k];
        }
      }
    }
  }
  *out_length = written;
  return true;
}

// ---- normalization.cpp -------------------------------------------------------
constexpr uint32_t kHangulSBase = 0xAC00, kHangulTBase = 0x11A7, kHangulVBase = 0x1161,
                   kHangulLBase = 0x1100, kHangulLCount = 19, kHangulVCount = 21,
                   kHangulTCount = 28, kHangulNCount = kHangulVCount * kHangulTCount,
                   kHangulSCount = kHangulLCount * kHangulVCount * kHangulTCount;

const uint16_t* decomposition_entry(uint32_t c) {
  return t::kDecompositionBlock + static_cast<size_t>(t::kDecompositionIndex[c >> 8]) * 257 +
         (c % 256);
}

uint8_t get_ccc(uint32_t c) {
  return c < 0x110000 ? t::kCccBlock[static_cast<size_t>(t::kCccIndex[c >> 8]) * 256 + c % 256]
                      : 0;
}

// decompose_nfc: Ada's non-recursive decomposition, then sort_marks. `work`
// is scratch of the same capacity.
bool decompose_nfc(uint32_t* input, size_t* length, size_t capacity, uint32_t* work) {
  size_t written = 0;
  for (size_t i = 0; i < *length; ++i) {
    const uint32_t c = input[i];
    if (c >= kHangulSBase && c < kHangulSBase + kHangulSCount) {
      const uint32_t s_index = c - kHangulSBase;
      const size_t need = s_index % kHangulTCount != 0 ? 3 : 2;
      if (written + need > capacity) return false;
      work[written++] = kHangulLBase + s_index / kHangulNCount;
      work[written++] = kHangulVBase + (s_index % kHangulNCount) / kHangulTCount;
      if (s_index % kHangulTCount != 0) work[written++] = kHangulTBase + s_index % kHangulTCount;
    } else if (c < 0x110000) {
      const uint16_t* d = decomposition_entry(c);
      uint16_t dlen = static_cast<uint16_t>((d[1] >> 2) - (d[0] >> 2));
      if (dlen > 0 && (d[0] & 1)) dlen = 0;
      if (dlen > 0) {
        if (written + dlen > capacity) return false;
        for (uint16_t k = 0; k < dlen; ++k) work[written++] = t::kDecompositionData[(d[0] >> 2) + k];
      } else {
        if (written == capacity) return false;
        work[written++] = c;
      }
    } else {
      if (written == capacity) return false;
      work[written++] = c;
    }
  }
  memcpy(input, work, written * sizeof(uint32_t));
  *length = written;
  // sort_marks
  for (size_t idx = 1; idx < *length; ++idx) {
    const uint8_t ccc = get_ccc(input[idx]);
    if (ccc == 0) continue;
    const uint32_t current = input[idx];
    size_t back = idx;
    while (back != 0 && get_ccc(input[back - 1]) > ccc) {
      input[back] = input[back - 1];
      --back;
    }
    input[back] = current;
  }
  return true;
}

const uint16_t* composition_entry(uint32_t c) {
  return t::kCompositionBlock + static_cast<size_t>(t::kCompositionIndex[c >> 8]) * 257 +
         (c % 256);
}

void compose(uint32_t* input, size_t* length) {
  const size_t size = *length;
  size_t input_count = 0, composition_count = 0;
  for (; input_count < size; input_count++, composition_count++) {
    input[composition_count] = input[input_count];
    const uint32_t c = input[input_count];
    if (c >= kHangulLBase && c < kHangulLBase + kHangulLCount) {
      if (input_count + 1 < size && input[input_count + 1] >= kHangulVBase &&
          input[input_count + 1] < kHangulVBase + kHangulVCount) {
        input[composition_count] =
            kHangulSBase + ((c - kHangulLBase) * kHangulVCount + input[input_count + 1] -
                            kHangulVBase) * kHangulTCount;
        input_count++;
        if (input_count + 1 < size && input[input_count + 1] > kHangulTBase &&
            input[input_count + 1] < kHangulTBase + kHangulTCount) {
          input[composition_count] += input[++input_count] - kHangulTBase;
        }
      }
    } else if (c >= kHangulSBase && c < kHangulSBase + kHangulSCount) {
      if ((c - kHangulSBase) % kHangulTCount && input_count + 1 < size &&
          input[input_count + 1] > kHangulTBase &&
          input[input_count + 1] < kHangulTBase + kHangulTCount) {
        input[composition_count] += input[++input_count] - kHangulTBase;
      }
    } else if (c < 0x110000) {
      const uint16_t* composition = composition_entry(c);
      const size_t initial_composition_count = composition_count;
      for (int32_t previous_ccc = -1; input_count + 1 < size; input_count++) {
        const uint8_t ccc = get_ccc(input[input_count + 1]);
        if (composition[1] != composition[0] && previous_ccc < ccc) {
          uint16_t left = composition[0];
          uint16_t right = composition[1];
          while (left + 2 < right) {
            const uint16_t middle = static_cast<uint16_t>(left + (((right - left) >> 1) & ~1));
            if (t::kCompositionData[middle] <= input[input_count + 1]) left = middle;
            if (t::kCompositionData[middle] >= input[input_count + 1]) right = middle;
          }
          if (t::kCompositionData[left] == input[input_count + 1]) {
            input[initial_composition_count] = t::kCompositionData[left + 1];
            composition = composition_entry(t::kCompositionData[left + 1]);
            continue;
          }
        }
        if (ccc == 0) break;
        previous_ccc = ccc;
        input[++composition_count] = input[input_count + 1];
      }
    }
  }
  if (composition_count < input_count) *length = composition_count;
}

bool normalize(uint32_t* input, size_t* length, size_t capacity, uint32_t* work) {
  if (!decompose_nfc(input, length, capacity, work)) return false;
  compose(input, length);
  return true;
}

// ---- punycode.cpp ------------------------------------------------------------
constexpr int32_t kBase = 36, kTmin = 1, kTmax = 26, kSkew = 38, kDamp = 700,
                  kInitialBias = 72;
constexpr uint32_t kInitialN = 128;

int32_t char_to_digit_value(char value) {
  if (value >= 'a' && value <= 'z') return value - 'a';
  if (value >= '0' && value <= '9') return value - '0' + 26;
  return -1;
}

char digit_to_char(int32_t digit) {
  return digit < 26 ? static_cast<char>(digit + 97) : static_cast<char>(digit + 22);
}

int32_t adapt(int32_t d, int32_t n, bool firsttime) {
  d = firsttime ? d / kDamp : d / 2;
  d += d / n;
  int32_t k = 0;
  while (d > ((kBase - kTmin) * kTmax) / 2) {
    d /= kBase - kTmin;
    k += kBase;
  }
  return k + (((kBase - kTmin + 1) * d) / (d + kSkew));
}

// ---- validity.cpp ------------------------------------------------------------
enum Direction : uint8_t {
  NONE, BN, CS, ES, ON, EN, L, R, NSM, AL, AN, ET, WS, RLO, LRO, PDF, RLE, RLI, FSI, PDI,
  LRI, B, S, LRE
};

Direction find_direction(uint32_t code_point) {
  // std::lower_bound over final_code < code_point.
  size_t low = 0, high = t::kDirectionsCount / 3;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (t::kDirections[middle * 3 + 1] < code_point) low = middle + 1;
    else high = middle;
  }
  if (low == t::kDirectionsCount / 3) return NONE;
  if (code_point >= t::kDirections[low * 3]) return static_cast<Direction>(t::kDirections[low * 3 + 2]);
  return NONE;
}

bool sorted_contains(const uint32_t* table, size_t count, uint32_t value) {
  size_t low = 0, high = count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (table[middle] < value) low = middle + 1;
    else high = middle;
  }
  return low < count && table[low] == value;
}

bool is_l_or_d(uint32_t c) {
  return sorted_contains(t::kJoiningL, t::kJoiningLCount, c) ||
         sorted_contains(t::kJoiningD, t::kJoiningDCount, c);
}

bool is_r_or_d(uint32_t c) {
  return sorted_contains(t::kJoiningR, t::kJoiningRCount, c) ||
         sorted_contains(t::kJoiningD, t::kJoiningDCount, c);
}

bool is_label_valid(const uint32_t* label, size_t size) {
  if (size == 0) return true;
  if (sorted_contains(t::kCombining, t::kCombiningCount, label[0])) return false;
  for (size_t i = 0; i < size; i++) {
    const uint32_t c = label[i];
    if (c == 0x200c) {
      if (i > 0 && sorted_contains(t::kVirama, t::kViramaCount, label[i - 1])) return true;
      if (i == 0 || i + 1 >= size) return false;
      bool before = false, after = false;
      for (size_t k = 0; k < i && !before; ++k) before = is_l_or_d(label[k]);
      for (size_t k = i + 1; k < size && !after; ++k) after = is_r_or_d(label[k]);
      return before && after;
    } else if (c == 0x200d) {
      if (i > 0 && sorted_contains(t::kVirama, t::kViramaCount, label[i - 1])) return true;
      return false;
    }
  }
  // find_last_not_of_nsm
  size_t last_non_nsm = SIZE_MAX;
  for (size_t i = size; i-- > 0;) {
    if (find_direction(label[i]) != NSM) { last_non_nsm = i; break; }
  }
  if (last_non_nsm == SIZE_MAX) return false;
  // is_rtl_label
  bool rtl = false;
  for (size_t i = 0; i < size && !rtl; i++) {
    const Direction d = find_direction(label[i]);
    rtl = d == R || d == AL || d == AN;
  }
  if (!rtl) return true;
  if (find_direction(label[0]) == L) {
    for (size_t i = 0; i < last_non_nsm; i++) {
      const Direction d = find_direction(label[i]);
      if (!(d == L || d == EN || d == ES || d == CS || d == ET || d == ON || d == BN ||
            d == NSM)) return false;
      // Ada's (i == last_non_nsm_char) check here can never hold.
    }
    return true;
  }
  bool has_an = false, has_en = false;
  for (size_t i = 0; i <= last_non_nsm; i++) {
    const Direction d = find_direction(label[i]);
    if (d == EN) { has_en = true; if (has_an) return false; }
    if (d == AN) { has_an = true; if (has_en) return false; }
    if (!(d == R || d == AL || d == AN || d == EN || d == ES || d == CS || d == ET ||
          d == ON || d == BN || d == NSM)) return false;
    if (i == last_non_nsm && !(d == R || d == AL || d == AN || d == EN)) return false;
  }
  return true;
}

// ---- to_ascii.cpp ------------------------------------------------------------
struct Out {
  char* data;
  size_t capacity;
  size_t used;
  bool ok;
  void Push(char c) {
    if (used < capacity) data[used++] = c;
    else ok = false;
  }
  void Append(const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) Push(text[i]);
  }
};

bool equal32(const uint32_t* a, size_t a_length, const uint32_t* b, size_t b_length) {
  return a_length == b_length && memcmp(a, b, a_length * sizeof(uint32_t)) == 0;
}

// The xn-- label check shared by both to_ascii paths: `puny` is the label
// after "xn--", already appended to the output.
bool check_punycode_label(const char* puny, size_t puny_length, Scratch* s) {
  size_t decoded = 0;
  if (!PunycodeToUtf32(puny, puny_length, s->work, kMaxCodePoints, &decoded)) return false;
  size_t mapped = 0;
  if (!map(s->work, decoded, s->check, kMaxCodePoints, &mapped)) {
    // Ada's map returns "", which differs from a non-empty decoded label; an
    // empty decoded label maps to "" and fails the emptiness check below.
    return false;
  }
  if (!equal32(s->work, decoded, s->check, mapped)) return false;
  // pre_normal = post_map; normalize(post_map); compare. s->work keeps the
  // pre-normal copy (equal to s->check here); s->domain is not touched.
  static_assert(sizeof(s->work) == sizeof(s->check), "scratch shapes");
  // Normalization needs a work buffer distinct from the label: use a stack
  // window over s->work after the label's code points.
  size_t normalized = mapped;
  if (mapped * 2 > kMaxCodePoints) return false;
  if (!normalize(s->check, &normalized, kMaxCodePoints / 2, s->work + mapped)) return false;
  if (!equal32(s->work, decoded, s->check, normalized)) return false;
  if (normalized == 0) return false;
  return is_label_valid(s->check, normalized);
}

bool from_ascii_to_ascii(const char* input, size_t length, Out* out, Scratch* s) {
  size_t label_start = 0;
  while (label_start != length) {
    size_t dot = label_start;
    while (dot < length && input[dot] != '.') ++dot;
    const bool last = dot == length;
    const size_t label_size = dot - label_start;
    const char* label = input + label_start;
    label_start += last ? label_size : label_size + 1;
    // Lowercase as ascii_map does, while copying.
    const size_t out_label = out->used;
    for (size_t i = 0; i < label_size; ++i) {
      const char c = label[i];
      out->Push(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
    }
    if (!out->ok) return false;
    if (label_size >= 4 && memcmp(out->data + out_label, "xn--", 4) == 0 &&
        !check_punycode_label(out->data + out_label + 4, label_size - 4, s)) return false;
    if (!last) out->Push('.');
  }
  return out->ok;
}

}  // namespace

bool PunycodeToUtf32(const char* input, size_t length, uint32_t* out, size_t capacity,
                     size_t* out_length) {
  int32_t written_out = 0;
  uint32_t n = kInitialN;
  int32_t i = 0;
  int32_t bias = kInitialBias;
  size_t end_of_ascii = SIZE_MAX;
  for (size_t k = length; k-- > 0;) if (input[k] == '-') { end_of_ascii = k; break; }
  size_t position = 0;
  if (end_of_ascii != SIZE_MAX) {
    for (size_t k = 0; k < end_of_ascii; ++k) {
      const uint8_t c = static_cast<uint8_t>(input[k]);
      if (c >= 0x80) return false;
      if (static_cast<size_t>(written_out) == capacity) return false;
      out[written_out++] = c;
    }
    position = end_of_ascii + 1;
  }
  while (position < length) {
    const int32_t oldi = i;
    int32_t w = 1;
    for (int32_t k = kBase;; k += kBase) {
      if (position >= length) return false;
      const char code_point = input[position++];
      const int32_t digit = char_to_digit_value(code_point);
      if (digit < 0) return false;
      if (digit > (0x7fffffff - i) / w) return false;
      i = i + digit * w;
      const int32_t tt = k <= bias ? kTmin : k >= bias + kTmax ? kTmax : k - bias;
      if (digit < tt) break;
      if (w > 0x7fffffff / (kBase - tt)) return false;
      w = w * (kBase - tt);
    }
    bias = adapt(i - oldi, written_out + 1, oldi == 0);
    if (i / (written_out + 1) > static_cast<int32_t>(0x7fffffff - n)) return false;
    n = n + static_cast<uint32_t>(i / (written_out + 1));
    i = i % (written_out + 1);
    if (n < 0x80) return false;
    if (static_cast<size_t>(written_out) == capacity) return false;
    memmove(out + i + 1, out + i, static_cast<size_t>(written_out - i) * sizeof(uint32_t));
    out[i] = n;
    written_out++;
    ++i;
  }
  *out_length = static_cast<size_t>(written_out);
  return true;
}

bool Utf32ToPunycode(const uint32_t* input, size_t size, char* output, size_t capacity,
                     size_t* output_length) {
  Out out{output, capacity, 0, true};
  uint32_t n = kInitialN;
  int32_t d = 0;
  int32_t bias = kInitialBias;
  size_t h = 0;
  for (size_t k = 0; k < size; ++k) {
    const uint32_t c = input[k];
    if (c < 0x80) {
      ++h;
      out.Push(static_cast<char>(c));
    }
    if (c > 0x10ffff || (c >= 0xd880 && c < 0xe000)) return false;
  }
  const size_t b = h;
  if (b > 0) out.Push('-');
  while (h < size) {
    uint32_t m = 0x10FFFF;
    for (size_t k = 0; k < size; ++k)
      if (input[k] >= n && input[k] < m) m = input[k];
    if ((m - n) > (static_cast<size_t>(0x7fffffff - d)) / (h + 1)) return false;
    d = d + static_cast<int32_t>((m - n) * (h + 1));
    n = m;
    for (size_t k = 0; k < size; ++k) {
      const uint32_t c = input[k];
      if (c < n) {
        if (d == 0x7fffffff) return false;
        ++d;
      }
      if (c == n) {
        int32_t q = d;
        for (int32_t kk = kBase;; kk += kBase) {
          const int32_t tt = kk <= bias ? kTmin : kk >= bias + kTmax ? kTmax : kk - bias;
          if (q < tt) break;
          out.Push(digit_to_char(tt + ((q - tt) % (kBase - tt))));
          q = (q - tt) / (kBase - tt);
        }
        out.Push(digit_to_char(q));
        bias = adapt(d, static_cast<int32_t>(h + 1), h == b);
        d = 0;
        ++h;
      }
    }
    ++d;
    ++n;
  }
  if (!out.ok) return false;
  *output_length = out.used;
  return true;
}

bool ToAscii(const char* utf8, size_t length, char* output, size_t capacity,
             size_t* output_length, Scratch* s) {
  Out out{output, capacity, 0, true};
  bool ascii = true;
  for (size_t i = 0; i < length && ascii; ++i)
    ascii = static_cast<uint8_t>(utf8[i]) < 0x80;
  if (ascii) {
    if (!from_ascii_to_ascii(utf8, length, &out, s) || out.used == 0) return false;
    *output_length = out.used;
    return true;
  }
  const size_t decoded = utf8_to_utf32(utf8, length, s->work, kMaxCodePoints);
  if (decoded == 0 || decoded == SIZE_MAX) return false;
  size_t mapped = 0;
  if (!map(s->work, decoded, s->domain, kMaxCodePoints, &mapped)) return false;
  if (!normalize(s->domain, &mapped, kMaxCodePoints, s->work)) return false;
  size_t label_start = 0;
  while (label_start != mapped) {
    size_t dot = label_start;
    while (dot < mapped && s->domain[dot] != '.') ++dot;
    const bool last = dot == mapped;
    const size_t label_size = dot - label_start;
    const uint32_t* label = s->domain + label_start;
    label_start += last ? label_size : label_size + 1;
    if (label_size == 0) {
      // empty label: nothing to do
    } else if (label_size >= 4 && label[0] == 'x' && label[1] == 'n' && label[2] == '-' &&
               label[3] == '-') {
      const size_t out_label = out.used;
      for (size_t i = 0; i < label_size; ++i) {
        if (label[i] >= 0x80) return false;
        out.Push(static_cast<char>(label[i]));
      }
      if (!out.ok) return false;
      if (!check_punycode_label(out.data + out_label + 4, label_size - 4, s)) return false;
    } else {
      bool label_ascii = true;
      for (size_t i = 0; i < label_size && label_ascii; ++i) label_ascii = label[i] < 0x80;
      if (label_ascii) {
        for (size_t i = 0; i < label_size; ++i) out.Push(static_cast<char>(label[i]));
      } else {
        if (!is_label_valid(label, label_size)) return false;
        out.Append("xn--", 4);
        size_t written = 0;
        if (!out.ok || !Utf32ToPunycode(label, label_size, out.data + out.used,
                                        out.capacity - out.used, &written)) return false;
        out.used += written;
      }
    }
    if (!last) out.Push('.');
  }
  if (!out.ok || out.used == 0) return false;
  *output_length = out.used;
  return true;
}

}  // namespace fortis::compat::idna
