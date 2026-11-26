#pragma once

#include <stddef.h>
#include <stdint.h>

// This server's regular expression engine: a small backtracking matcher for
// a fixed subset of ECMAScript (non-unicode) syntax over UTF-16 code units.
// It runs the news pipeline's patterns (news_text.cpp) and the AI reply
// filter's secret-request patterns (ai_reply.cpp); the latter makes it
// security-relevant (docs/compat-decisions.md#js-regex).
// Supported syntax, and nothing else (Compile refuses the rest):
//   literals; escapes \b \B \s \S \d \D \w \W \t \n \r \f \v \xHH \uHHHH and
//   escaped punctuation; classes with ranges, negation and class escapes;
//   '.'; '^' and '$' (no m flag); capturing and (?:) groups; alternation;
//   greedy/lazy * + ? {m} {m,} {m,n} on a single-character atom; greedy/lazy
//   '?' on any non-nullable atom or group; (?=X) where X is one character
//   or class; flags i and g.
// Under the i flag every letter must be ASCII (non-unicode Canonicalize then
// is ASCII case folding: no non-ASCII code unit canonicalizes to an ASCII
// one); the only non-ASCII pattern code units accepted under i are the
// caseless U+00BB and U+201C the news patterns use.
// Matching is bounded by kStepBudget steps and kMaxFrames backtrack frames
// per Exec; exhausting either is reported as kBudget (no news pattern comes
// near it on a 4 MiB feed).
// Allocation-free and guest-safe; a compiled Program is immutable and may be
// shared between threads.
namespace fortis::compat::js_regex {

constexpr size_t kMaxInstructions = 1024;  // per program
constexpr size_t kMaxClasses = 16;         // per program
constexpr size_t kMaxClassRanges = 24;
constexpr size_t kMaxGroups = 10;  // group 0 plus nine capturing groups
constexpr size_t kMaxFrames = 1024;
constexpr uint64_t kStepBudget = 400000000ULL;

struct Range {
  uint16_t first;
  uint16_t last;
};

struct CharClass {
  Range ranges[kMaxClassRanges];
  uint8_t range_count;
  bool negated;
};

struct Instruction {
  uint8_t op;
  uint8_t flags;
  uint16_t x;
  uint16_t y;
  uint16_t z;
};

// Caller storage that compiled programs are carved from, so each program
// takes exactly the instructions and classes it needs.
struct ProgramPool {
  Instruction* code = nullptr;
  size_t code_capacity = 0;
  size_t code_used = 0;
  CharClass* classes = nullptr;
  size_t class_capacity = 0;
  size_t class_used = 0;
};

struct Program {
  const Instruction* code = nullptr;
  uint16_t code_length = 0;
  const CharClass* classes = nullptr;
  uint8_t class_count = 0;
  uint8_t group_count = 1;  // including group 0
  bool global = false;
  bool ignore_case = false;
};

// False when the pattern uses unsupported syntax or exceeds a bound (the
// pool is then left unchanged). `source` is UTF-8; `flags` is a subset of "gi".
bool Compile(const char* source, const char* flags, ProgramPool* pool, Program* program);

struct Match {
  size_t start = 0;
  size_t end = 0;
  // Group k spans [group_start[k], group_end[k]) when group_matched[k].
  size_t group_start[kMaxGroups]{};
  size_t group_end[kMaxGroups]{};
  bool group_matched[kMaxGroups]{};
};

enum class MatchOutcome : uint8_t { kNoMatch, kMatch, kBudget };

// The first match starting at or after `from`.
MatchOutcome Exec(const Program& program, const uint16_t* text, size_t length, size_t from,
            Match* match);

}  // namespace fortis::compat::js_regex
