#include "js_regex.h"

#include "threading.h"

#include <pthread.h>
#include <string.h>

namespace fortis::compat::js_regex {
namespace {

enum Op : uint8_t {
  kOpChar, kOpAny, kOpClass, kOpBol, kOpEol, kOpWordB, kOpNotWordB, kOpLook,
  kOpSplit, kOpJmp, kOpSave, kOpStar, kOpMatch,
};
// Instruction flags.
constexpr uint8_t kFlagFold = 1;       // ASCII case-insensitive literal / class
constexpr uint8_t kFlagLazy = 2;       // kOpStar
constexpr uint8_t kMatcherChar = 0;    // flags >> 4: the kOpStar/kOpLook matcher
constexpr uint8_t kMatcherAny = 1;
constexpr uint8_t kMatcherClass = 2;
constexpr uint16_t kInfinite = 0xFFFF;

// ECMAScript WhiteSpace and LineTerminator (\s).
constexpr Range kSpace[] = {{0x09, 0x0D}, {0x20, 0x20}, {0xA0, 0xA0}, {0x1680, 0x1680},
                            {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F},
                            {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF}};
constexpr Range kDigit[] = {{'0', '9'}};
constexpr Range kWord[] = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};

bool in_ranges(const Range* ranges, size_t count, uint16_t c) {
  for (size_t i = 0; i < count; ++i)
    if (c >= ranges[i].first && c <= ranges[i].last) return true;
  return false;
}

bool is_word(uint16_t c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool is_ascii_letter(uint16_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool is_line_terminator(uint16_t c) {
  return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

// ---- compiler ------------------------------------------------------------

enum Kind : uint8_t {
  kEmpty, kChar, kAny, kClass, kBol, kEol, kWordB, kNotWordB, kLook, kGroup, kAlt, kSeq, kQuant,
};

struct Node {
  uint8_t kind = kEmpty;
  uint8_t flags = 0;    // kFlagFold for kChar; kFlagLazy for kQuant; matcher for kLook
  uint16_t value = 0;   // code unit, class index, or capture index (0xFFFF none)
  uint16_t min = 0;
  uint16_t max = 0;
  int16_t child = -1;   // first child
  int16_t next = -1;    // next sibling
};

constexpr size_t kMaxNodes = 1024;

struct Compiler {
  const uint16_t* source;
  size_t length;
  size_t position = 0;
  Program* program;
  Instruction* code;     // pool storage for this program
  size_t code_capacity;
  CharClass* classes;
  size_t class_capacity;
  Node nodes[kMaxNodes];
  size_t node_count = 0;
  bool ok = true;

  int16_t add(const Node& node) {
    if (node_count >= kMaxNodes) { ok = false; return -1; }
    nodes[node_count] = node;
    return static_cast<int16_t>(node_count++);
  }
  bool at_end() const { return position >= length; }
  uint16_t peek() const { return position < length ? source[position] : 0; }

  bool fold_allowed(uint16_t c) const {
    if (!program->ignore_case || c < 0x80) return true;
    return c == 0x00BB || c == 0x201C;
  }

  int class_add(CharClass* cls, uint16_t first, uint16_t last) {
    if (cls->range_count >= kMaxClassRanges) { ok = false; return 0; }
    cls->ranges[cls->range_count++] = Range{first, last};
    return 1;
  }
  void class_add_set(CharClass* cls, const Range* ranges, size_t count, bool complement) {
    if (!complement) {
      for (size_t i = 0; i < count; ++i) class_add(cls, ranges[i].first, ranges[i].last);
      return;
    }
    uint32_t next = 0;
    for (size_t i = 0; i < count; ++i) {
      if (ranges[i].first > next)
        class_add(cls, static_cast<uint16_t>(next), static_cast<uint16_t>(ranges[i].first - 1));
      next = static_cast<uint32_t>(ranges[i].last) + 1;
    }
    if (next <= 0xFFFF) class_add(cls, static_cast<uint16_t>(next), 0xFFFF);
  }

  int hex_digit(uint16_t c) const {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  bool read_hex(size_t digits, uint16_t* value) {
    uint32_t v = 0;
    for (size_t i = 0; i < digits; ++i) {
      const int d = position < length ? hex_digit(source[position]) : -1;
      if (d < 0) return false;
      v = v * 16 + static_cast<uint32_t>(d);
      ++position;
    }
    *value = static_cast<uint16_t>(v);
    return true;
  }

  // One escaped character (after '\'), for the literal escapes shared by
  // atoms and classes. Returns false for an unsupported escape.
  bool escape_char(uint16_t e, uint16_t* out) {
    switch (e) {
      case 't': *out = 0x09; return true;
      case 'n': *out = 0x0A; return true;
      case 'v': *out = 0x0B; return true;
      case 'f': *out = 0x0C; return true;
      case 'r': *out = 0x0D; return true;
      case 'x': return read_hex(2, out);
      case 'u': return read_hex(4, out);
      default: break;
    }
    // Identity escapes of syntax characters and '/', '-'.
    if (strchr("^$\\.*+?()[]{}|/-", static_cast<int>(e)) && e < 0x80) { *out = e; return true; }
    return false;
  }

  int16_t parse_class() {
    // After '['.
    if (program->class_count >= class_capacity) { ok = false; return -1; }
    const uint16_t index = program->class_count++;
    CharClass* cls = &classes[index];
    cls->range_count = 0;
    cls->negated = false;
    if (peek() == '^') { cls->negated = true; ++position; }
    bool first = true;
    while (ok) {
      if (at_end()) { ok = false; return -1; }
      uint16_t c = source[position];
      if (c == ']' && !first) { ++position; break; }
      if (c == ']' && first) { ok = false; return -1; }  // [] / [^] unsupported
      first = false;
      ++position;
      uint16_t low = 0;
      bool single = true;
      if (c == '\\') {
        if (at_end()) { ok = false; return -1; }
        const uint16_t e = source[position++];
        switch (e) {
          case 's': class_add_set(cls, kSpace, sizeof(kSpace) / sizeof(kSpace[0]), false); single = false; break;
          case 'S': class_add_set(cls, kSpace, sizeof(kSpace) / sizeof(kSpace[0]), true); single = false; break;
          case 'd': class_add_set(cls, kDigit, 1, false); single = false; break;
          case 'D': class_add_set(cls, kDigit, 1, true); single = false; break;
          case 'w': class_add_set(cls, kWord, 4, false); single = false; break;
          case 'W': class_add_set(cls, kWord, 4, true); single = false; break;
          case 'b': low = 0x08; break;
          default:
            if (!escape_char(e, &low)) { ok = false; return -1; }
        }
      } else {
        low = c;
      }
      if (!single) continue;
      if (!fold_allowed(low)) { ok = false; return -1; }
      // A range a-b (a '-' before ']' is literal).
      if (peek() == '-' && position + 1 < length && source[position + 1] != ']') {
        ++position;
        uint16_t high = source[position++];
        if (high == '\\') {
          if (at_end()) { ok = false; return -1; }
          const uint16_t e = source[position++];
          if (!escape_char(e, &high)) { ok = false; return -1; }
        }
        if (high < low || !fold_allowed(high)) { ok = false; return -1; }
        // A folded range must not straddle letters partially into non-ASCII.
        if (program->ignore_case && high >= 0x80 && low < 0x80) { ok = false; return -1; }
        class_add(cls, low, high);
      } else {
        class_add(cls, low, low);
      }
    }
    Node node;
    node.kind = kClass;
    node.value = index;
    return add(node);
  }

  int16_t parse_atom() {
    const uint16_t c = source[position++];
    Node node;
    switch (c) {
      case '^': node.kind = kBol; return add(node);
      case '$': node.kind = kEol; return add(node);
      case '.': node.kind = kAny; return add(node);
      case '[': return parse_class();
      case '(': {
        bool capture = true, look = false;
        if (peek() == '?') {
          if (position + 1 >= length) { ok = false; return -1; }
          const uint16_t k = source[position + 1];
          if (k == ':') capture = false;
          else if (k == '=') { capture = false; look = true; }
          else { ok = false; return -1; }
          position += 2;
        }
        if (look) {
          // (?=X) with X one character or class.
          if (at_end()) { ok = false; return -1; }
          const int16_t inner = parse_atom();
          if (!ok || inner < 0 || peek() != ')') { ok = false; return -1; }
          ++position;
          const Node& in = nodes[inner];
          if (in.kind != kChar && in.kind != kClass) { ok = false; return -1; }
          node.kind = kLook;
          node.flags = static_cast<uint8_t>((in.kind == kChar ? kMatcherChar : kMatcherClass) << 4 |
                                            (in.flags & kFlagFold));
          node.value = in.value;
          return add(node);
        }
        uint16_t index = 0xFFFF;
        if (capture) {
          if (program->group_count >= kMaxGroups) { ok = false; return -1; }
          index = program->group_count++;
        }
        const int16_t child = parse_alternation();
        if (!ok || peek() != ')') { ok = false; return -1; }
        ++position;
        node.kind = kGroup;
        node.value = index;
        node.child = child;
        return add(node);
      }
      case '\\': {
        if (at_end()) { ok = false; return -1; }
        const uint16_t e = source[position++];
        if (e == 'b') { node.kind = kWordB; return add(node); }
        if (e == 'B') { node.kind = kNotWordB; return add(node); }
        const Range* set = nullptr;
        size_t set_count = 0;
        bool complement = false;
        switch (e) {
          case 's': set = kSpace; set_count = sizeof(kSpace) / sizeof(kSpace[0]); break;
          case 'S': set = kSpace; set_count = sizeof(kSpace) / sizeof(kSpace[0]); complement = true; break;
          case 'd': set = kDigit; set_count = 1; break;
          case 'D': set = kDigit; set_count = 1; complement = true; break;
          case 'w': set = kWord; set_count = 4; break;
          case 'W': set = kWord; set_count = 4; complement = true; break;
          default: break;
        }
        if (set) {
          if (program->class_count >= class_capacity) { ok = false; return -1; }
          const uint16_t index = program->class_count++;
          CharClass* cls = &classes[index];
          cls->range_count = 0;
          cls->negated = false;
          class_add_set(cls, set, set_count, complement);
          node.kind = kClass;
          node.value = index;
          return add(node);
        }
        uint16_t literal = 0;
        if (!escape_char(e, &literal)) { ok = false; return -1; }
        return literal_node(literal);
      }
      case '*': case '+': case '?': case '{': case ')': case '|': case ']': case '}':
        ok = false;
        return -1;
      default:
        return literal_node(c);
    }
  }

  int16_t literal_node(uint16_t c) {
    if (!fold_allowed(c)) { ok = false; return -1; }
    Node node;
    node.kind = kChar;
    if (program->ignore_case && is_ascii_letter(c)) {
      node.flags = kFlagFold;
      c = static_cast<uint16_t>(c | 0x20);
    }
    node.value = c;
    return add(node);
  }

  bool read_number(uint32_t* value) {
    if (at_end() || peek() < '0' || peek() > '9') return false;
    uint32_t v = 0;
    while (!at_end() && peek() >= '0' && peek() <= '9') {
      v = v * 10 + (peek() - '0');
      if (v > 0xFFFE) return false;
      ++position;
    }
    *value = v;
    return true;
  }

  bool nullable(int16_t index) const {
    if (index < 0) return true;
    const Node& n = nodes[index];
    switch (n.kind) {
      case kEmpty: case kBol: case kEol: case kWordB: case kNotWordB: case kLook: return true;
      case kChar: case kAny: case kClass: return false;
      case kGroup: return nullable(n.child);
      case kAlt:
        for (int16_t c = n.child; c >= 0; c = nodes[c].next) if (nullable(c)) return true;
        return false;
      case kSeq:
        for (int16_t c = n.child; c >= 0; c = nodes[c].next) if (!nullable(c)) return false;
        return true;
      case kQuant: return n.min == 0 || nullable(n.child);
    }
    return true;
  }

  int16_t parse_term() {
    const int16_t atom = parse_atom();
    if (!ok || atom < 0) return -1;
    uint32_t min = 0, max = 0;
    bool quantified = true;
    const uint16_t q = peek();
    if (q == '*') { min = 0; max = kInfinite; ++position; }
    else if (q == '+') { min = 1; max = kInfinite; ++position; }
    else if (q == '?') { min = 0; max = 1; ++position; }
    else if (q == '{') {
      ++position;
      if (!read_number(&min)) { ok = false; return -1; }
      if (peek() == ',') {
        ++position;
        if (peek() == '}') max = kInfinite;
        else if (!read_number(&max)) { ok = false; return -1; }
      } else {
        max = min;
      }
      if (peek() != '}' || max < min) { ok = false; return -1; }
      ++position;
    } else {
      quantified = false;
    }
    if (!quantified) return atom;
    const uint8_t kind = nodes[atom].kind;
    const bool single = kind == kChar || kind == kAny || kind == kClass;
    if (kind == kBol || kind == kEol || kind == kWordB || kind == kNotWordB || kind == kLook) {
      ok = false;
      return -1;
    }
    if (!single && (max != 1 || min != 0 || nullable(atom))) { ok = false; return -1; }
    Node node;
    node.kind = kQuant;
    node.min = static_cast<uint16_t>(min);
    node.max = static_cast<uint16_t>(max);
    node.child = atom;
    if (peek() == '?') { node.flags = kFlagLazy; ++position; }
    if (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{') { ok = false; return -1; }
    return add(node);
  }

  int16_t parse_sequence() {
    Node seq;
    seq.kind = kSeq;
    const int16_t index = add(seq);
    if (!ok) return -1;
    int16_t last = -1;
    while (ok && !at_end() && peek() != '|' && peek() != ')') {
      const int16_t term = parse_term();
      if (!ok) return -1;
      if (last < 0) nodes[index].child = term;
      else nodes[last].next = term;
      last = term;
    }
    return index;
  }

  int16_t parse_alternation() {
    const int16_t first = parse_sequence();
    if (!ok || peek() != '|') return first;
    Node alt;
    alt.kind = kAlt;
    const int16_t index = add(alt);
    if (!ok) return -1;
    nodes[index].child = first;
    int16_t last = first;
    while (ok && peek() == '|') {
      ++position;
      const int16_t next = parse_sequence();
      if (!ok) return -1;
      nodes[last].next = next;
      last = next;
    }
    return index;
  }

  // ---- code generation ----
  bool emit(uint8_t op, uint8_t flags, uint16_t x, uint16_t y, uint16_t z, uint16_t* at = nullptr) {
    if (program->code_length >= code_capacity) { ok = false; return false; }
    if (at) *at = program->code_length;
    code[program->code_length++] = Instruction{op, flags, x, y, z};
    return true;
  }

  void gen(int16_t index) {
    if (!ok || index < 0) return;
    const Node& n = nodes[index];
    switch (n.kind) {
      case kEmpty: return;
      case kChar: emit(kOpChar, n.flags, n.value, 0, 0); return;
      case kAny: emit(kOpAny, 0, 0, 0, 0); return;
      case kClass: emit(kOpClass, 0, n.value, 0, 0); return;
      case kBol: emit(kOpBol, 0, 0, 0, 0); return;
      case kEol: emit(kOpEol, 0, 0, 0, 0); return;
      case kWordB: emit(kOpWordB, 0, 0, 0, 0); return;
      case kNotWordB: emit(kOpNotWordB, 0, 0, 0, 0); return;
      case kLook: emit(kOpLook, n.flags, n.value, 0, 0); return;
      case kGroup:
        if (n.value != 0xFFFF) emit(kOpSave, 0, static_cast<uint16_t>(n.value * 2), 0, 0);
        gen(n.child);
        if (n.value != 0xFFFF) emit(kOpSave, 0, static_cast<uint16_t>(n.value * 2 + 1), 0, 0);
        return;
      case kSeq:
        for (int16_t c = n.child; c >= 0 && ok; c = nodes[c].next) gen(c);
        return;
      case kAlt: {
        uint16_t jumps[256];
        size_t jump_count = 0;
        for (int16_t c = n.child; c >= 0 && ok; c = nodes[c].next) {
          if (nodes[c].next >= 0) {
            uint16_t split = 0;
            if (!emit(kOpSplit, 0, 0, 0, 0, &split)) return;
            code[split].x = static_cast<uint16_t>(split + 1);
            gen(c);
            if (jump_count >= 256) { ok = false; return; }
            if (!emit(kOpJmp, 0, 0, 0, 0, &jumps[jump_count++])) return;
            code[split].y = program->code_length;
          } else {
            gen(c);
          }
        }
        for (size_t i = 0; i < jump_count; ++i) code[jumps[i]].x = program->code_length;
        return;
      }
      case kQuant: {
        const Node& child = nodes[n.child];
        if (child.kind == kChar || child.kind == kAny || child.kind == kClass) {
          const uint8_t matcher = child.kind == kChar ? kMatcherChar
                                : child.kind == kAny ? kMatcherAny : kMatcherClass;
          emit(kOpStar, static_cast<uint8_t>(matcher << 4 | (child.flags & kFlagFold) |
                                             (n.flags & kFlagLazy)),
               child.value, n.min, n.max);
          return;
        }
        // '?' on a group.
        uint16_t split = 0;
        if (!emit(kOpSplit, 0, 0, 0, 0, &split)) return;
        gen(n.child);
        const uint16_t body = static_cast<uint16_t>(split + 1);
        const uint16_t after = program->code_length;
        if (n.flags & kFlagLazy) { code[split].x = after; code[split].y = body; }
        else { code[split].x = body; code[split].y = after; }
        return;
      }
    }
  }
};

// UTF-8 pattern source to code units (BMP only).
bool decode_source(const char* source, uint16_t* out, size_t capacity, size_t* length) {
  size_t n = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(source);
  while (*p) {
    uint32_t c = *p;
    size_t extra = 0;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else return false;
    ++p;
    for (size_t i = 0; i < extra; ++i) {
      if ((*p & 0xC0) != 0x80) return false;
      c = c << 6 | (*p++ & 0x3F);
    }
    if (n >= capacity) return false;
    out[n++] = static_cast<uint16_t>(c);
  }
  *length = n;
  return true;
}

// ---- matcher ----------------------------------------------------------------

bool class_match(const Program& program, uint16_t index, bool fold, uint16_t c) {
  const CharClass& cls = program.classes[index];
  bool raw = in_ranges(cls.ranges, cls.range_count, c);
  if (!raw && fold && is_ascii_letter(c))
    raw = in_ranges(cls.ranges, cls.range_count, static_cast<uint16_t>(c ^ 0x20));
  return cls.negated ? !raw : raw;
}

bool single_match(const Program& program, uint8_t matcher, bool fold, uint16_t value, uint16_t c) {
  switch (matcher) {
    case kMatcherChar:
      if (fold && is_ascii_letter(c)) return static_cast<uint16_t>(c | 0x20) == value;
      return c == value;
    case kMatcherAny: return !is_line_terminator(c);
    default: return class_match(program, value, program.ignore_case, c);
  }
}

enum FrameKind : uint8_t { kBranch, kRestore, kGreedy, kLazy };
struct Frame {
  uint8_t kind;
  uint16_t pc;
  size_t a;
  size_t b;
};

constexpr size_t kUnset = static_cast<size_t>(-1);

MatchOutcome run(const Program& program, const uint16_t* text, size_t length, size_t start,
           uint64_t* steps, Match* match) {
  Frame frames[kMaxFrames];
  size_t depth = 0;
  size_t captures[kMaxGroups * 2];
  for (size_t i = 0; i < kMaxGroups * 2; ++i) captures[i] = kUnset;
  uint16_t pc = 0;
  size_t pos = start;
  for (;;) {
    if (++*steps > kStepBudget) return MatchOutcome::kBudget;
    const Instruction& in = program.code[pc];
    bool failed = false;
    switch (in.op) {
      case kOpChar: {
        if (pos < length) {
          const uint16_t c = text[pos];
          const bool eq = (in.flags & kFlagFold) && is_ascii_letter(c)
              ? static_cast<uint16_t>(c | 0x20) == in.x : c == in.x;
          if (eq) { ++pos; ++pc; break; }
        }
        failed = true;
        break;
      }
      case kOpAny:
        if (pos < length && !is_line_terminator(text[pos])) { ++pos; ++pc; } else failed = true;
        break;
      case kOpClass:
        if (pos < length && class_match(program, in.x, program.ignore_case, text[pos])) {
          ++pos; ++pc;
        } else {
          failed = true;
        }
        break;
      case kOpBol: if (pos == 0) ++pc; else failed = true; break;
      case kOpEol: if (pos == length) ++pc; else failed = true; break;
      case kOpWordB: case kOpNotWordB: {
        const bool before = pos > 0 && is_word(text[pos - 1]);
        const bool after = pos < length && is_word(text[pos]);
        if ((before != after) == (in.op == kOpWordB)) ++pc; else failed = true;
        break;
      }
      case kOpLook:
        if (pos < length && single_match(program, static_cast<uint8_t>(in.flags >> 4),
                                         in.flags & kFlagFold, in.x, text[pos])) ++pc;
        else failed = true;
        break;
      case kOpSplit:
        if (depth >= kMaxFrames) return MatchOutcome::kBudget;
        frames[depth++] = Frame{kBranch, in.y, pos, 0};
        pc = in.x;
        break;
      case kOpJmp: pc = in.x; break;
      case kOpSave:
        if (depth >= kMaxFrames) return MatchOutcome::kBudget;
        frames[depth++] = Frame{kRestore, in.x, captures[in.x], 0};
        captures[in.x] = pos;
        ++pc;
        break;
      case kOpStar: {
        const uint8_t matcher = static_cast<uint8_t>(in.flags >> 4);
        const bool fold = in.flags & kFlagFold;
        const size_t max = in.z == kInfinite ? static_cast<size_t>(-1) : in.z;
        size_t p = pos, count = 0;
        if (!(in.flags & kFlagLazy)) {
          while (count < max && p < length && single_match(program, matcher, fold, in.x, text[p])) {
            ++p; ++count;
            if ((count & 0xFFF) == 0) {
              *steps += 0x1000;
              if (*steps > kStepBudget) return MatchOutcome::kBudget;
            }
          }
          if (count < in.y) { failed = true; break; }
          if (count > in.y) {
            if (depth >= kMaxFrames) return MatchOutcome::kBudget;
            frames[depth++] = Frame{kGreedy, static_cast<uint16_t>(pc + 1), pos + in.y, p};
          }
        } else {
          while (count < in.y) {
            if (!(p < length && single_match(program, matcher, fold, in.x, text[p]))) break;
            ++p; ++count;
          }
          if (count < in.y) { failed = true; break; }
          if (count < max) {
            if (depth >= kMaxFrames) return MatchOutcome::kBudget;
            frames[depth++] = Frame{kLazy, pc, p, count};
          }
        }
        pos = p;
        ++pc;
        break;
      }
      case kOpMatch:
        match->start = start;
        match->end = pos;
        for (size_t g = 0; g < kMaxGroups; ++g) {
          if (g == 0) {
            match->group_start[0] = start;
            match->group_end[0] = pos;
            match->group_matched[0] = true;
            continue;
          }
          const size_t s = captures[g * 2], e = captures[g * 2 + 1];
          match->group_matched[g] = g < program.group_count && s != kUnset && e != kUnset;
          match->group_start[g] = match->group_matched[g] ? s : 0;
          match->group_end[g] = match->group_matched[g] ? e : 0;
        }
        return MatchOutcome::kMatch;
    }
    if (!failed) continue;
    // Backtrack.
    bool resumed = false;
    while (depth > 0 && !resumed) {
      Frame& frame = frames[depth - 1];
      switch (frame.kind) {
        case kBranch:
          pc = frame.pc;
          pos = frame.a;
          --depth;
          resumed = true;
          break;
        case kRestore:
          captures[frame.pc] = frame.a;
          --depth;
          break;
        case kGreedy:
          --frame.b;
          pos = frame.b;
          pc = frame.pc;
          if (frame.b <= frame.a) --depth;
          resumed = true;
          break;
        case kLazy: {
          const Instruction& star = program.code[frame.pc];
          const size_t max = star.z == kInfinite ? static_cast<size_t>(-1) : star.z;
          if (frame.b < max && frame.a < length &&
              single_match(program, static_cast<uint8_t>(star.flags >> 4), star.flags & kFlagFold,
                           star.x, text[frame.a])) {
            ++frame.a;
            ++frame.b;
            pos = frame.a;
            pc = static_cast<uint16_t>(frame.pc + 1);
            if (frame.b >= max) --depth;
            resumed = true;
          } else {
            --depth;
          }
          break;
        }
      }
      if (resumed && ++*steps > kStepBudget) return MatchOutcome::kBudget;
    }
    if (!resumed) return MatchOutcome::kNoMatch;
  }
}

Compiler g_compiler_storage;  // too large for small thread stacks
fortis::base::Mutex g_compiler_mutex;

}  // namespace

bool Compile(const char* source, const char* flags, ProgramPool* pool, Program* program) {
  if (!source || !program || !pool || pool->code_used > pool->code_capacity ||
      pool->class_used > pool->class_capacity) return false;
  program->code_length = 0;
  program->class_count = 0;
  program->group_count = 1;
  program->global = false;
  program->ignore_case = false;
  for (const char* f = flags ? flags : ""; *f; ++f) {
    if (*f == 'g' && !program->global) program->global = true;
    else if (*f == 'i' && !program->ignore_case) program->ignore_case = true;
    else return false;
  }
  uint16_t units[2048];
  size_t length = 0;
  if (!decode_source(source, units, sizeof(units) / sizeof(units[0]), &length)) return false;
  size_t code_capacity = pool->code_capacity - pool->code_used;
  if (code_capacity > kMaxInstructions) code_capacity = kMaxInstructions;
  size_t class_capacity = pool->class_capacity - pool->class_used;
  if (class_capacity > kMaxClasses) class_capacity = kMaxClasses;
  Instruction* code = pool->code + pool->code_used;
  CharClass* classes = pool->classes + pool->class_used;
  fortis::base::UniqueLock g_compiler_lock(g_compiler_mutex);
  Compiler& compiler = g_compiler_storage;
  compiler.source = units;
  compiler.length = length;
  compiler.position = 0;
  compiler.program = program;
  compiler.code = code;
  compiler.code_capacity = code_capacity;
  compiler.classes = classes;
  compiler.class_capacity = class_capacity;
  compiler.node_count = 0;
  compiler.ok = true;
  const int16_t root = compiler.parse_alternation();
  bool ok = compiler.ok && compiler.at_end();
  if (ok) compiler.gen(root);
  ok = ok && compiler.ok && program->code_length < code_capacity;
  compiler.source = nullptr;
  compiler.program = nullptr;
  compiler.code = nullptr;
  compiler.classes = nullptr;
  g_compiler_lock.Unlock();
  if (!ok) return false;
  code[program->code_length++] = Instruction{kOpMatch, 0, 0, 0, 0};
  program->code = code;
  program->classes = classes;
  pool->code_used += program->code_length;
  pool->class_used += program->class_count;
  return true;
}

MatchOutcome Exec(const Program& program, const uint16_t* text, size_t length, size_t from,
            Match* match) {
  uint64_t steps = 0;
  if (from > length) return MatchOutcome::kNoMatch;
  const Instruction& first = program.code[0];
  const bool anchored = first.op == kOpBol;
  const bool literal = first.op == kOpChar;
  for (size_t start = from; start <= length; ++start) {
    if (anchored && start > 0) return MatchOutcome::kNoMatch;
    if (literal) {
      // Skip to the next code unit that can begin a match.
      while (start < length) {
        const uint16_t c = text[start];
        if ((first.flags & kFlagFold) && is_ascii_letter(c) ? (c | 0x20) == first.x : c == first.x)
          break;
        ++start;
      }
      if (start >= length) return MatchOutcome::kNoMatch;
    }
    const MatchOutcome status = run(program, text, length, start, &steps, match);
    if (status != MatchOutcome::kNoMatch) return status;
  }
  return MatchOutcome::kNoMatch;
}

}  // namespace fortis::compat::js_regex
