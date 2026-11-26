#include "exception_boundary.h"
#include "news_text.h"

#include "js_case.h"
#include "js_date_parse.h"
#include "js_regex.h"
#include "network_registry.h"
#include "threading.h"
#include "whatwg_url.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

namespace fortis::news {

using compat::js_regex::Match;
using compat::js_regex::Program;
using compat::js_regex::MatchOutcome;

// ---- arena, strings ---------------------------------------------------------

void* Arena::Alloc(size_t bytes, size_t align) {
  if (!ok) return nullptr;
  if (align == 0) align = 1;
  const uintptr_t current = reinterpret_cast<uintptr_t>(base) + used;
  const size_t padding = (align - (current % align)) % align;
  if (padding > capacity - used || bytes > capacity - used - padding) {
    ok = false;
    return nullptr;
  }
  void* result = base + used + padding;
  used += padding + bytes;
  return result;
}

U16 Arena::Copy(U16 text) {
  if (text.length == 0) return U16{};
  auto* data = static_cast<uint16_t*>(Alloc(text.length * 2, 2));
  if (!data) return U16{};
  memcpy(data, text.data, text.length * 2);
  return U16{data, text.length};
}

namespace {

// A growable UTF-16 buffer; grows in place while it is the arena's last
// allocation.
struct Builder {
  Arena* arena;
  uint16_t* data = nullptr;
  size_t length = 0;
  size_t capacity = 0;

  explicit Builder(Arena* a) : arena(a) {}
  bool Reserve(size_t extra) {
    if (!arena->ok) return false;
    if (extra <= capacity - length) return true;
    size_t wanted = capacity ? capacity * 2 : 64;
    while (wanted - length < extra) wanted *= 2;
    uint8_t* end = data ? reinterpret_cast<uint8_t*>(data + capacity) : nullptr;
    if (data && end == arena->base + arena->used &&
        (wanted - capacity) * 2 <= arena->capacity - arena->used) {
      arena->used += (wanted - capacity) * 2;
      capacity = wanted;
      return true;
    }
    auto* next = static_cast<uint16_t*>(arena->Alloc(wanted * 2, 2));
    if (!next) return false;
    if (length) memcpy(next, data, length * 2);
    data = next;
    capacity = wanted;
    return true;
  }
  void Push(uint16_t c) {
    if (Reserve(1)) data[length++] = c;
  }
  void Append(const uint16_t* units, size_t count) {
    if (count && !units) { arena->ok = false; return; }
    if (count && Reserve(count)) {
      memcpy(data + length, units, count * 2);
      length += count;
    }
  }
  void Append(U16 text) { Append(text.data, text.length); }
  void Ascii(const char* text) {
    for (; *text; ++text) Push(static_cast<uint8_t>(*text));
  }
  void CodePoint(uint32_t cp) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      Push(static_cast<uint16_t>(0xD800 + (cp >> 10)));
      Push(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      Push(static_cast<uint16_t>(cp));
    }
  }
  U16 Done() const { return arena->ok ? U16{data, length} : U16{}; }
};

// String.prototype.slice(start, end) with non-negative arguments.
U16 slice(U16 text, size_t start, size_t end) {
  if (start > text.length) start = text.length;
  if (end > text.length) end = text.length;
  if (end < start) return U16{text.data + start, 0};
  if (start > 0 && start < text.length && text.data[start] >= 0xdc00 && text.data[start] <= 0xdfff &&
      text.data[start - 1] >= 0xd800 && text.data[start - 1] <= 0xdbff) ++start;
  if (end > 0 && end < text.length && text.data[end - 1] >= 0xd800 && text.data[end - 1] <= 0xdbff &&
      text.data[end] >= 0xdc00 && text.data[end] <= 0xdfff) --end;
  return U16{text.data + start, end > start ? end - start : 0};
}

bool is_space(uint16_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 ||
         c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

U16 ascii16(Arena* arena, const char* text) {
  Builder b(arena);
  b.Ascii(text);
  return b.Done();
}

bool starts_with_ascii(U16 text, const char* prefix) {
  size_t i = 0;
  for (; prefix[i]; ++i)
    if (i >= text.length || text.data[i] != static_cast<uint8_t>(prefix[i])) return false;
  return true;
}

bool ends_with_ascii(U16 text, const char* suffix) {
  const size_t n = strlen(suffix);
  if (text.length < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (text.data[text.length - n + i] != static_cast<uint8_t>(suffix[i])) return false;
  return true;
}

// String.prototype.indexOf(search, from).
size_t index_of(U16 haystack, U16 needle, size_t from, bool* found) {
  *found = false;
  if (from > haystack.length) from = haystack.length;
  if (needle.length == 0) { *found = true; return from; }
  if (needle.length > haystack.length) return 0;
  for (size_t i = from; i + needle.length <= haystack.length; ++i) {
    if (haystack.data[i] != needle.data[0]) continue;
    if (memcmp(haystack.data + i, needle.data, needle.length * 2) == 0) {
      *found = true;
      return i;
    }
  }
  return 0;
}

// ---- case mapping (js_case tables over UTF-16) ------------------------------

const compat::JsCaseMapping* find_mapping(const compat::JsCaseMapping* table, size_t count, uint32_t value) {
  size_t low = 0, high = count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (table[middle].code_point < value) low = middle + 1;
    else high = middle;
  }
  return low < count && table[low].code_point == value ? &table[low] : nullptr;
}

bool in_code_ranges(const compat::JsCodePointRange* table, size_t count, uint32_t value) {
  size_t low = 0, high = count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (table[middle].last < value) low = middle + 1;
    else high = middle;
  }
  return low < count && table[low].first <= value;
}

// The code point at `i` (a lone surrogate is its own code point).
uint32_t code_point_at(U16 text, size_t i, size_t* width) {
  const uint16_t c = text.data[i];
  if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.length && text.data[i + 1] >= 0xDC00 &&
      text.data[i + 1] <= 0xDFFF) {
    *width = 2;
    return 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (text.data[i + 1] - 0xDC00);
  }
  *width = 1;
  return c;
}

uint32_t code_point_before(U16 text, size_t end, size_t* start) {
  size_t i = end - 1;
  if (i > 0 && text.data[i] >= 0xDC00 && text.data[i] <= 0xDFFF && text.data[i - 1] >= 0xD800 &&
      text.data[i - 1] <= 0xDBFF)
    --i;
  size_t width = 0;
  const uint32_t cp = code_point_at(text, i, &width);
  *start = i;
  return cp;
}

bool is_surrogate(uint32_t cp) { return cp >= 0xD800 && cp <= 0xDFFF; }

bool ignorable(uint32_t cp) {
  return !is_surrogate(cp) && in_code_ranges(compat::js_case_tables::kCaseIgnorable,
                                             compat::js_case_tables::kCaseIgnorableCount, cp);
}
bool cased(uint32_t cp) {
  return !is_surrogate(cp) && in_code_ranges(compat::js_case_tables::kCased, compat::js_case_tables::kCasedCount, cp);
}

bool final_sigma(U16 text, size_t start, size_t end) {
  bool preceded = false;
  for (size_t cursor = start; cursor > 0;) {
    size_t previous = 0;
    const uint32_t cp = code_point_before(text, cursor, &previous);
    cursor = previous;
    if (ignorable(cp)) continue;
    preceded = cased(cp);
    break;
  }
  if (!preceded) return false;
  for (size_t cursor = end; cursor < text.length;) {
    size_t width = 0;
    const uint32_t cp = code_point_at(text, cursor, &width);
    cursor += width;
    if (ignorable(cp)) continue;
    return !cased(cp);
  }
  return true;
}

U16 case_map(U16 text, Arena* arena, bool upper) {
  // Fast path: nothing to change returns the input view.
  const compat::JsCaseMapping* table = upper ? compat::js_case_tables::kUpper : compat::js_case_tables::kLower;
  const size_t count = upper ? compat::js_case_tables::kUpperCount : compat::js_case_tables::kLowerCount;
  size_t first_change = text.length;
  for (size_t i = 0; i < text.length;) {
    size_t width = 0;
    const uint32_t cp = code_point_at(text, i, &width);
    if ((!upper && cp == 0x03A3) || (!is_surrogate(cp) && find_mapping(table, count, cp))) {
      first_change = i;
      break;
    }
    i += width;
  }
  if (first_change == text.length) return text;
  Builder out(arena);
  out.Append(text.data, first_change);
  for (size_t i = first_change; i < text.length;) {
    size_t width = 0;
    const uint32_t cp = code_point_at(text, i, &width);
    if (!upper && cp == 0x03A3 && final_sigma(text, i, i + width)) {
      out.Push(0x03C2);
      i += width;
      continue;
    }
    const compat::JsCaseMapping* mapping = is_surrogate(cp) ? nullptr : find_mapping(table, count, cp);
    if (!mapping) out.Append(text.data + i, width);
    else for (uint8_t k = 0; k < mapping->count; ++k) out.CodePoint(mapping->output[k]);
    i += width;
  }
  return out.Done();
}

// ---- regular expressions ------------------------------------------------------

constexpr size_t kPoolInstructions = 12288;
constexpr size_t kPoolClasses = 256;
compat::js_regex::Instruction g_code_pool[kPoolInstructions];
compat::js_regex::CharClass g_class_pool[kPoolClasses];

const char* const kRawNames[] = {"title", "guid", "id", "link", "description", "summary",
                                 "content:encoded", "content", "pubDate", "published",
                                 "updated", "dc:date", "dc:creator", "author", "category"};
enum RawName {
  kTitle, kGuid, kId, kLink, kDescription, kSummary, kContentEncoded, kContent, kPubDate,
  kPublished, kUpdated, kDcDate, kDcCreator, kAuthor, kCategory, kRawNameCount
};
const char* const kAttributeNames[] = {"href", "url", "medium", "type"};
enum AttributeName { kHref, kUrl, kMedium, kType, kAttributeNameCount };

struct PatternTopic {
  const char* source;
  const char* flags;
  Topic topic;
};
// The TypeScript CATEGORY_TOPICS and TEXT_RULES, verbatim (`.source`, `.flags`).
const PatternTopic kCategoryTopics[] = {
    {"\\b(?:policy|regulation|regulatory|legal|government|court)\\b", "", Topic::kPolicy},
    {"\\b(?:security|hack|exploit|scam|fraud)\\b", "", Topic::kSecurity},
    {"\\b(?:macro|economy|economics|inflation|monetary)\\b", "", Topic::kMacro},
    {"\\b(?:etf|fund|flows|institutional)\\b", "", Topic::kFlows},
    {"\\b(?:defi|protocol|layer\\s?2|infrastructure)\\b", "", Topic::kProtocol},
    {"\\b(?:bitcoin|ethereum|solana|blockchain|mining|nodes?)\\b", "", Topic::kChains},
};
const PatternTopic kTextRules[] = {
    {"\\b(?:exploit(?:ed|s)?|hacked?|hacks|drained?|rug ?pull|phishing|vulnerabilit(?:y|ies)|breach(?:ed|es)?|stolen|theft|malware|backdoor|compromised)\\b", "i", Topic::kSecurity},
    {"\\b(?:sec|cftc|esma|mica|fca|finra|doj|lawsuit|sue[ds]?|court|judge|ruling|subpoena|indict(?:ed|ment)?|settle(?:d|ment)?|regulat(?:e|ed|ion|ory|or|ors)|legislation|bill|sanction(?:s|ed)?|ban(?:s|ned)?|licen[cs]e[ds]?|compliance)\\b", "i", Topic::kPolicy},
    {"\\b(?:fed|federal reserve|fomc|ecb|boj|bank of england|cpi|ppi|inflation|deflation|interest rates?|rate cut|rate hike|yields?|treasur(?:y|ies)|payrolls?|jobless|gdp|recession|dollar index|dxy|nasdaq|s&p ?500)\\b", "i", Topic::kMacro},
    {"\\b(?:etf|inflows?|outflows?|net flows?|unlock(?:s|ed|ing)?|vesting|whale|reserves?|treasury (?:buy|purchase)|accumulat(?:e|ed|ion)|allocation|aum|institutional (?:buying|demand))\\b", "i", Topic::kFlows},
    {"\\b(?:upgrade[ds]?|hard ?fork|soft ?fork|halving|mainnet|testnet|validators?|node client|consensus|finality|block time|throughput|tps|outage|halted?|reorg|staking|slash(?:ed|ing)?)\\b", "i", Topic::kChains},
    {"\\b(?:protocol|dao|governance (?:vote|proposal)|tokenomics|airdrop|launch(?:es|ed)?|integrat(?:e|ed|ion)|partnership|roadmap|audit(?:ed)?|tvl|liquidity pool|bridge)\\b", "i", Topic::kProtocol},
    {"\\b(?:rall(?:y|ies|ied)|surge[ds]?|soar(?:s|ed)?|jump(?:s|ed)?|plunge[ds]?|slump(?:s|ed)?|crash(?:es|ed)?|tumble[ds]?|selloff|sell-off|all-time high|record high|breaks? (?:above|below)|price (?:target|action)|bull(?:ish)?|bear(?:ish)?)\\b", "i", Topic::kTrend},
};
struct Pattern {
  const char* source;
  const char* flags;
};
// The TypeScript WATCH_PATTERNS and BOILERPLATE, verbatim.
const Pattern kWatchPatterns[] = {
    {"\\b(?:unlock(?:s|ed|ing)?|vesting|cliff)\\b", "i"},
    {"\\b(?:deadline|expires?|expiry|sunset|cut-?off|window closes)\\b", "i"},
    {"\\b(?:migration|migrate|swap deadline|snapshot|redenomination)\\b", "i"},
    {"\\b(?:delisting|delist(?:s|ed)?|suspend(?:s|ed|ing)? (?:trading|withdrawals?|deposits?))\\b", "i"},
};
const Pattern kBoilerplate[] = {
    {"\\bthe post\\b[\\s\\S]*?\\bappeared first on\\b[\\s\\S]*$", "i"},
    {"\\bthis (?:article|post) (?:originally )?appeared\\b[\\s\\S]*$", "i"},
    {"\\bcontinue reading\\b[\\s\\S]*$", "i"},
    {"\\bread more\\b\\s*(?:[.:>\xc2\xbb-]|$)[\\s\\S]*$", "i"},
    {"\\bsource:\\s*\\S+\\s*$", "i"},
    {"\\bimage:\\s*[\\s\\S]*$", "i"},
    {"\\bphoto by\\b[\\s\\S]*$", "i"},
    {"\\bshare this article\\b[\\s\\S]*$", "i"},
    {"\\bsubscribe (?:to|now)\\b[\\s\\S]*$", "i"},
};
constexpr size_t kCategoryRuleCount = sizeof(kCategoryTopics) / sizeof(kCategoryTopics[0]);
constexpr size_t kTextRuleCount = sizeof(kTextRules) / sizeof(kTextRules[0]);
constexpr size_t kWatchCount = sizeof(kWatchPatterns) / sizeof(kWatchPatterns[0]);
constexpr size_t kBoilerplateCount = sizeof(kBoilerplate) / sizeof(kBoilerplate[0]);

struct Programs {
  Program comments, cdata, tags, entities, collapse, http, image_ext;
  Program link_tag, rel_alternate, rel_any, media_content, enclosure, img_src, thumbnail_tag;
  Program open_item, open_entry;
  Program raw[kRawNameCount];
  Program attribute[kAttributeNameCount];
  Program category[kCategoryRuleCount];
  Program text_rule[kTextRuleCount];
  Program watch[kWatchCount];
  Program boilerplate[kBoilerplateCount];
  Program sentence;
  Program ticker;
};
Programs g_programs;
bool g_programs_ok = false;
pthread_once_t g_programs_once = PTHREAD_ONCE_INIT;

void compile_programs() {
  compat::js_regex::ProgramPool pool{g_code_pool, kPoolInstructions, 0, g_class_pool, kPoolClasses, 0};
  Programs& p = g_programs;
  bool ok = true;
  auto c = [&](const char* source, const char* flags, Program* program) {
    ok = compat::js_regex::Compile(source, flags, &pool, program) && ok;
  };
  // Feed parsing.
  c("<!--[\\s\\S]*?-->", "g", &p.comments);
  c("<!\\[CDATA\\[([\\s\\S]*?)\\]\\]>", "g", &p.cdata);
  c("<[^>]*>", "g", &p.tags);
  c("&(#x[0-9a-f]+|#\\d+|[a-z]+);", "gi", &p.entities);
  c("[\\s\\x00-\\x1F]+", "g", &p.collapse);
  c("^https?:\\/\\/\\S+$", "i", &p.http);
  c("\\.(?:png|jpe?g|gif|webp)(?:$|[?#])", "i", &p.image_ext);
  c("<link\\b([^>]*)\\/?>", "gi", &p.link_tag);
  c("rel\\s*=\\s*[\"']alternate[\"']", "i", &p.rel_alternate);
  c("rel\\s*=\\s*[\"']", "i", &p.rel_any);
  c("<media:content\\b([^>]*)\\/?>", "gi", &p.media_content);
  c("<enclosure\\b([^>]*)\\/?>", "gi", &p.enclosure);
  c("<img\\b[^>]*\\bsrc\\s*=\\s*[\"']([^\"']+)[\"']", "i", &p.img_src);
  c("<media:thumbnail\\b([^>]*)\\/?>", "i", &p.thumbnail_tag);
  c("<item\\b[^>]*>", "gi", &p.open_item);
  c("<entry\\b[^>]*>", "gi", &p.open_entry);
  char source[256];
  for (size_t i = 0; i < kRawNameCount; ++i) {
    // `<${name}\\b[^>]*>([\\s\\S]*?)</${name}>` (escapeName is a no-op here).
    const size_t n = strlen(kRawNames[i]);
    char* w = source;
    *w++ = '<'; memcpy(w, kRawNames[i], n); w += n;
    memcpy(w, "\\b[^>]*>([\\s\\S]*?)</", 20); w += 20;
    memcpy(w, kRawNames[i], n); w += n;
    *w++ = '>'; *w = '\0';
    c(source, "gi", &p.raw[i]);
  }
  for (size_t i = 0; i < kAttributeNameCount; ++i) {
    // `\\b${name}\\s*=\\s*["']([^"']*)["']`
    const size_t n = strlen(kAttributeNames[i]);
    char* w = source;
    memcpy(w, "\\b", 2); w += 2;
    memcpy(w, kAttributeNames[i], n); w += n;
    const char* tail = "\\s*=\\s*[\"']([^\"']*)[\"']";
    memcpy(w, tail, strlen(tail) + 1);
    c(source, "i", &p.attribute[i]);
  }
  for (size_t i = 0; i < kCategoryRuleCount; ++i)
    c(kCategoryTopics[i].source, kCategoryTopics[i].flags, &p.category[i]);
  for (size_t i = 0; i < kTextRuleCount; ++i)
    c(kTextRules[i].source, kTextRules[i].flags, &p.text_rule[i]);
  for (size_t i = 0; i < kWatchCount; ++i)
    c(kWatchPatterns[i].source, kWatchPatterns[i].flags, &p.watch[i]);
  for (size_t i = 0; i < kBoilerplateCount; ++i)
    c(kBoilerplate[i].source, kBoilerplate[i].flags, &p.boilerplate[i]);
  // The TypeScript splitSentences; the bare-symbol ticker scan.
  c("([.!?])\\s+(?=[A-Z\"'\xe2\x80\x9c(])", "g", &p.sentence);
  c("\\$?\\b([A-Z][A-Z0-9]{1,9})\\b", "g", &p.ticker);
  g_programs_ok = ok;
}

const Programs* programs() {
  pthread_once(&g_programs_once, fortis::base::FatalCallback<compile_programs>);
  return g_programs_ok ? &g_programs : nullptr;
}

MatchOutcome exec(const Program& program, U16 text, size_t from, Match* match, Arena* arena) {
  const MatchOutcome status = compat::js_regex::Exec(program, text.data, text.length, from, match);
  if (status == MatchOutcome::kBudget && arena) arena->regex_budget = true;
  return status;
}

bool test(const Program& program, U16 text, Arena* arena) {
  Match match;
  return exec(program, text, 0, &match, arena) == MatchOutcome::kMatch;
}

U16 group(U16 text, const Match& match, size_t index) {
  if (!match.group_matched[index]) return U16{};
  return U16{text.data + match.group_start[index], match.group_end[index] - match.group_start[index]};
}

using ReplacementFn = void (*)(void* context, U16 subject, const Match& match, Builder* out);

// String.prototype.replace(regexp, replacement): every match for a global
// program (empty matches advance lastIndex by one), else the first.
U16 replace(const Program& program, U16 text, ReplacementFn replacement, void* context,
            Arena* arena) {
  Builder out(arena);
  size_t from = 0, last = 0;
  bool any = false;
  for (;;) {
    Match match;
    if (exec(program, text, from, &match, arena) != MatchOutcome::kMatch) break;
    any = true;
    out.Append(text.data + last, match.start - last);
    replacement(context, text, match, &out);
    last = match.end;
    if (!program.global) break;
    from = match.end == match.start ? match.end + 1 : match.end;
    if (from > text.length) break;
  }
  if (!any) return text;
  out.Append(text.data + last, text.length - last);
  return out.Done();
}

void replace_empty(void*, U16, const Match&, Builder*) {}
void replace_space(void*, U16, const Match&, Builder* out) { out->Push(' '); }
void replace_group1(void*, U16 subject, const Match& match, Builder* out) {
  out->Append(group(subject, match, 1));
}

// matchAll: calls `visit` for each match until it returns false.
template <typename Visit>
void match_all(const Program& program, U16 text, Arena* arena, Visit visit) {
  size_t from = 0;
  for (;;) {
    Match match;
    if (exec(program, text, from, &match, arena) != MatchOutcome::kMatch) return;
    if (!visit(match)) return;
    from = match.end == match.start ? match.end + 1 : match.end;
    if (from > text.length) return;
  }
}

struct OptU16 {
  bool has = false;
  U16 value;
};

}  // namespace

U16 Arena::FromUtf8(const char* text, size_t length) {
  // The WHATWG UTF-8 decoder (TextDecoder / Response.text()): maximal
  // subparts become U+FFFD and one leading BOM is removed.
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  size_t start = 0;
  if (length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) start = 3;
  auto* out = static_cast<uint16_t*>(Alloc((length - start) * 2 + 2, 2));
  if (!out) return U16{};
  size_t n = 0;
  uint32_t cp = 0;
  int needed = 0, seen = 0;
  unsigned lower = 0x80, upper = 0xBF;
  auto emit = [&](uint32_t value) {
    if (value >= 0x10000) {
      value -= 0x10000;
      out[n++] = static_cast<uint16_t>(0xD800 + (value >> 10));
      out[n++] = static_cast<uint16_t>(0xDC00 + (value & 0x3FF));
    } else {
      out[n++] = static_cast<uint16_t>(value);
    }
  };
  for (size_t i = start; i < length;) {
    const unsigned b = bytes[i];
    if (needed == 0) {
      ++i;
      if (b <= 0x7F) emit(b);
      else if (b >= 0xC2 && b <= 0xDF) { needed = 1; cp = b & 0x1F; }
      else if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) lower = 0xA0;
        if (b == 0xED) upper = 0x9F;
        needed = 2; cp = b & 0x0F;
      } else if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) lower = 0x90;
        if (b == 0xF4) upper = 0x8F;
        needed = 3; cp = b & 0x07;
      } else {
        emit(0xFFFD);
      }
      continue;
    }
    if (b < lower || b > upper) {
      cp = 0; needed = 0; seen = 0; lower = 0x80; upper = 0xBF;
      emit(0xFFFD);
      continue;  // reprocess this byte
    }
    ++i;
    lower = 0x80; upper = 0xBF;
    cp = cp << 6 | (b & 0x3F);
    if (++seen == needed) {
      emit(cp);
      cp = 0; needed = 0; seen = 0;
    }
  }
  if (needed) emit(0xFFFD);
  // Return the unused tail of the conservative allocation.
  const size_t reserved = (length - start) * 2 + 2;
  if (reinterpret_cast<uint8_t*>(out) + reserved == base + used) used -= reserved - n * 2;
  return U16{out, n};
}

bool AppendUtf8(OutBuffer& out, U16 text) {
  for (size_t i = 0; i < text.length;) {
    size_t width = 0;
    uint32_t cp = code_point_at(text, i, &width);
    i += width;
    if (is_surrogate(cp)) cp = 0xFFFD;
    uint8_t bytes[4];
    size_t count;
    if (cp < 0x80) { bytes[0] = static_cast<uint8_t>(cp); count = 1; }
    else if (cp < 0x800) {
      bytes[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
      bytes[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F)); count = 2;
    } else if (cp < 0x10000) {
      bytes[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
      bytes[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      bytes[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F)); count = 3;
    } else {
      bytes[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
      bytes[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
      bytes[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      bytes[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F)); count = 4;
    }
    out.Append(bytes, count);
  }
  return out.ok;
}

namespace {

// JSON.stringify's QuoteJSONString, emitting code units to `sink`.
template <typename Sink>
void quote_json(U16 text, Sink sink) {
  static const char kHex[] = "0123456789abcdef";
  sink('"');
  for (size_t i = 0; i < text.length; ++i) {
    const uint16_t c = text.data[i];
    switch (c) {
      case 0x22: sink('\\'); sink('"'); continue;
      case 0x5C: sink('\\'); sink('\\'); continue;
      case 0x08: sink('\\'); sink('b'); continue;
      case 0x0C: sink('\\'); sink('f'); continue;
      case 0x0A: sink('\\'); sink('n'); continue;
      case 0x0D: sink('\\'); sink('r'); continue;
      case 0x09: sink('\\'); sink('t'); continue;
      default: break;
    }
    bool escape = c < 0x20;
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < text.length && text.data[i + 1] >= 0xDC00 && text.data[i + 1] <= 0xDFFF) {
        sink(c);
        sink(text.data[++i]);
        continue;
      }
      sink(0xfffd); continue;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      sink(0xfffd); continue;
    }
    if (escape) {
      sink('\\'); sink('u');
      sink(static_cast<uint16_t>(kHex[c >> 12])); sink(static_cast<uint16_t>(kHex[(c >> 8) & 15]));
      sink(static_cast<uint16_t>(kHex[(c >> 4) & 15])); sink(static_cast<uint16_t>(kHex[c & 15]));
      continue;
    }
    sink(c);
  }
  sink('"');
}

}  // namespace

void AppendJsonString(OutBuffer& out, U16 text) {
  // Pairs are handed over together so AppendUtf8 sees them adjacent.
  uint16_t pending = 0;
  auto sink = [&](uint16_t c) {
    if (pending) {
      const uint16_t pair[2] = {pending, c};
      pending = 0;
      AppendUtf8(out, U16{pair, 2});
      return;
    }
    if (c >= 0xD800 && c <= 0xDBFF) { pending = c; return; }
    AppendUtf8(out, U16{&c, 1});
  };
  quote_json(text, sink);
}

bool Equal(U16 a, U16 b) {
  return a.length == b.length && (a.length == 0 || memcmp(a.data, b.data, a.length * 2) == 0);
}

bool EqualAscii(U16 a, const char* ascii) {
  const size_t n = strlen(ascii);
  if (a.length != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (a.data[i] != static_cast<uint8_t>(ascii[i])) return false;
  return true;
}

int Compare(U16 a, U16 b) {
  const size_t n = a.length < b.length ? a.length : b.length;
  for (size_t i = 0; i < n; ++i)
    if (a.data[i] != b.data[i]) return a.data[i] < b.data[i] ? -1 : 1;
  return a.length == b.length ? 0 : a.length < b.length ? -1 : 1;
}

const Topic kTopicOrder[kTopicCount] = {Topic::kTrend, Topic::kMacro, Topic::kChains,
                                        Topic::kPolicy, Topic::kSecurity, Topic::kFlows,
                                        Topic::kProtocol};

const char* TopicName(Topic topic) {
  switch (topic) {
    case Topic::kTrend: return "trend";
    case Topic::kMacro: return "macro";
    case Topic::kChains: return "chains";
    case Topic::kPolicy: return "policy";
    case Topic::kSecurity: return "security";
    case Topic::kFlows: return "flows";
    case Topic::kProtocol: return "protocol";
  }
  return "trend";
}

bool TopicFromName(U16 name, Topic* topic) {
  for (Topic t : kTopicOrder) {
    if (EqualAscii(name, TopicName(t))) { *topic = t; return true; }
  }
  return false;
}

const char* ImpactName(Impact impact) {
  switch (impact) {
    case Impact::kNormal: return "normal";
    case Impact::kWatch: return "watch";
    case Impact::kBreaking: return "breaking";
  }
  return "normal";
}

bool ImpactFromName(U16 name, Impact* impact) {
  static const Impact kImpacts[] = {Impact::kNormal, Impact::kWatch, Impact::kBreaking};
  for (Impact i : kImpacts) {
    if (EqualAscii(name, ImpactName(i))) { *impact = i; return true; }
  }
  return false;
}

U16 JsToLowerCase(U16 text, Arena* arena) { return case_map(text, arena, false); }

U16 JsTrim(U16 text) {
  size_t begin = 0, end = text.length;
  while (begin < end && is_space(text.data[begin])) ++begin;
  while (end > begin && is_space(text.data[end - 1])) --end;
  return U16{text.data + begin, end - begin};
}

bool NewsTextInitialize() { return programs() != nullptr; }

size_t PatternCount(PatternTable table) {
  switch (table) {
    case PatternTable::kCategory: return kCategoryRuleCount;
    case PatternTable::kText: return kTextRuleCount;
    case PatternTable::kWatch: return kWatchCount;
    case PatternTable::kBoilerplate: return kBoilerplateCount;
  }
  return 0;
}

bool PatternAt(PatternTable table, size_t index, const char** source, const char** flags,
               Topic* topic) {
  if (index >= PatternCount(table)) return false;
  *topic = Topic::kTrend;
  switch (table) {
    case PatternTable::kCategory:
      *source = kCategoryTopics[index].source;
      *flags = kCategoryTopics[index].flags;
      *topic = kCategoryTopics[index].topic;
      return true;
    case PatternTable::kText:
      *source = kTextRules[index].source;
      *flags = kTextRules[index].flags;
      *topic = kTextRules[index].topic;
      return true;
    case PatternTable::kWatch:
      *source = kWatchPatterns[index].source;
      *flags = kWatchPatterns[index].flags;
      return true;
    case PatternTable::kBoilerplate:
      *source = kBoilerplate[index].source;
      *flags = kBoilerplate[index].flags;
      return true;
  }
  return false;
}

// ---- feed parsing ----------------------------------------------------------------

namespace {

U16 strip_comments(const Programs& p, U16 xml, Arena* arena) {
  return replace(p.comments, xml, replace_empty, nullptr, arena);
}
U16 unwrap_cdata(const Programs& p, U16 value, Arena* arena) {
  return replace(p.cdata, value, replace_group1, nullptr, arena);
}

void decode_entity(void*, U16 subject, const Match& match, Builder* out) {
  const U16 whole = group(subject, match, 0);
  const U16 body = group(subject, match, 1);
  if (!body.data) { out->Append(whole); return; }
  // NAMED_ENTITIES[body.toLowerCase()] (body is ASCII here). An own key, or
  // a lower-case Object.prototype key: only `constructor` is reachable by
  // [a-z]+, and String(Object) is what replace() then inserts.
  char lower[16];
  bool named_candidate = body.length < sizeof(lower);
  for (size_t i = 0; named_candidate && i < body.length; ++i) {
    uint16_t c = body.data[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<uint16_t>(c | 0x20);
    lower[i] = static_cast<char>(c);
  }
  if (named_candidate) {
    lower[body.length] = '\0';
    static const char* const kNames[][2] = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""},
                                            {"apos", "'"}, {"nbsp", " "},
                                            {"constructor", "function Object() { [native code] }"}};
    for (const auto& entry : kNames) {
      if (strcmp(lower, entry[0]) == 0) { out->Ascii(entry[1]); return; }
    }
  }
  if (body.length && body.data[0] == '#') {
    const bool hex = body.length > 1 && (body.data[1] | 0x20) == 'x';
    // Number.parseInt: the digits are all valid here; saturate past U+10FFFF.
    uint64_t code = 0;
    for (size_t i = hex ? 2 : 1; i < body.length; ++i) {
      const uint16_t c = body.data[i];
      const unsigned digit = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
      code = code * (hex ? 16 : 10) + digit;
      if (code > 0x10FFFF) { code = 0x110000; }
    }
    if (code > 0 && code <= 0x10FFFF) {
      out->CodePoint(code >= 0xd800 && code <= 0xdfff ? 0xfffd : static_cast<uint32_t>(code));
      return;
    }
  }
  out->Append(whole);
}

}  // namespace

U16 DecodeEntities(U16 text, Arena* arena) {
  const Programs* p = programs();
  if (!p) { arena->ok = false; return U16{}; }
  return replace(p->entities, text, decode_entity, nullptr, arena);
}

U16 StripTags(U16 text, Arena* arena) {
  const Programs* p = programs();
  if (!p) { arena->ok = false; return U16{}; }
  return replace(p->tags, text, replace_space, nullptr, arena);
}

U16 Collapse(U16 text, Arena* arena) {
  const Programs* p = programs();
  if (!p) { arena->ok = false; return U16{}; }
  return JsTrim(replace(p->collapse, text, replace_space, nullptr, arena));
}

bool ParseJsDate(U16 text, double* epoch_ms) {
  return compat::JsDateParse(text.data, text.length, epoch_ms);
}

namespace {

U16 clean_text(U16 raw, Arena* arena) {
  return Collapse(DecodeEntities(StripTags(raw, arena), arena), arena);
}

OptU16 raw_text(const Programs& p, U16 block, RawName name, Arena* arena) {
  OptU16 result;
  Match match;
  if (exec(p.raw[name], block, 0, &match, arena) == MatchOutcome::kMatch) {
    result.has = true;
    result.value = unwrap_cdata(p, group(block, match, 1), arena);
  }
  return result;
}

OptU16 text_of(const Programs& p, U16 block, RawName name, Arena* arena) {
  OptU16 raw = raw_text(p, block, name, arena);
  if (raw.has) raw.value = clean_text(raw.value, arena);
  return raw;
}

bool is_http_url(const Programs& p, U16 value, Arena* arena) { return test(p.http, value, arena); }
bool looks_like_image_url(const Programs& p, U16 value, Arena* arena) {
  return test(p.image_ext, value, arena);
}

OptU16 attribute(const Programs& p, U16 attrs, AttributeName name, Arena* arena) {
  OptU16 result;
  Match match;
  if (exec(p.attribute[name], attrs, 0, &match, arena) != MatchOutcome::kMatch) return result;
  const U16 value = JsTrim(DecodeEntities(group(attrs, match, 1), arena));
  if (value.length == 0) return result;
  result.has = true;
  result.value = value;
  return result;
}

OptU16 link_of(const Programs& p, U16 block, Arena* arena) {
  const OptU16 body = text_of(p, block, kLink, arena);
  if (body.has && is_http_url(p, body.value, arena)) return body;
  // links.find(alternate) ?? links.find(no rel) over every <link …> tag.
  bool have_alternate = false, have_plain = false;
  U16 alternate, plain;
  match_all(p.link_tag, block, arena, [&](const Match& m) {
    const U16 attrs = group(block, m, 1);
    if (!have_alternate && test(p.rel_alternate, attrs, arena)) {
      have_alternate = true;
      alternate = attrs;
    }
    if (!have_plain && !test(p.rel_any, attrs, arena)) {
      have_plain = true;
      plain = attrs;
    }
    return !have_alternate;
  });
  if (!have_alternate && !have_plain) return OptU16{};
  const OptU16 href = attribute(p, have_alternate ? alternate : plain, kHref, arena);
  if (href.has && is_http_url(p, href.value, arena)) return href;
  return OptU16{};
}

bool date_of(const Programs& p, U16 block, double* ms, Arena* arena) {
  // `updated` is an edit time, not evidence of the initial release time.
  static const RawName kTags[] = {kPubDate, kPublished, kDcDate};
  for (RawName tag : kTags) {
    const OptU16 raw = text_of(p, block, tag, arena);
    if (!raw.has || raw.value.length == 0) continue;
    double at = 0;
    if (!ParseJsDate(raw.value, &at)) continue;
    if (at < 946684800000.0) continue;  // Date.UTC(2000, 0, 1)
    *ms = at;
    return true;
  }
  return false;
}

OptU16 image_of(const Programs& p, U16 block, Arena* arena) {
  OptU16 found;
  match_all(p.media_content, block, arena, [&](const Match& m) {
    const U16 attrs = group(block, m, 1);
    const OptU16 url = attribute(p, attrs, kUrl, arena);
    if (!url.has || !is_http_url(p, url.value, arena)) return true;
    const OptU16 medium = attribute(p, attrs, kMedium, arena);
    const OptU16 type = attribute(p, attrs, kType, arena);
    const bool declares_image = (medium.has && EqualAscii(medium.value, "image")) ||
                                (type.has && starts_with_ascii(type.value, "image/"));
    if (declares_image || (!medium.has && !type.has && looks_like_image_url(p, url.value, arena))) {
      found = url;
      return false;
    }
    return true;
  });
  if (found.has) return found;

  Match thumb;
  if (exec(p.thumbnail_tag, block, 0, &thumb, arena) == MatchOutcome::kMatch) {
    const OptU16 url = attribute(p, group(block, thumb, 1), kUrl, arena);
    if (url.has && is_http_url(p, url.value, arena)) return url;
  }

  match_all(p.enclosure, block, arena, [&](const Match& m) {
    const U16 attrs = group(block, m, 1);
    const OptU16 url = attribute(p, attrs, kUrl, arena);
    if (!url.has || !is_http_url(p, url.value, arena)) return true;
    const OptU16 type = attribute(p, attrs, kType, arena);
    if ((type.has && starts_with_ascii(type.value, "image/")) ||
        looks_like_image_url(p, url.value, arena)) {
      found = url;
      return false;
    }
    return true;
  });
  if (found.has) return found;

  OptU16 description = raw_text(p, block, kDescription, arena);
  if (!description.has) description = raw_text(p, block, kContentEncoded, arena);
  const U16 html = description.has ? description.value : U16{};
  Match img;
  if (exec(p.img_src, html, 0, &img, arena) == MatchOutcome::kMatch) {
    const U16 src = DecodeEntities(group(html, img, 1), arena);
    if (is_http_url(p, src, arena)) {
      found.has = true;
      found.value = src;
    }
  }
  return found;
}

bool parse_entry(const Programs& p, U16 block, Arena* arena, FeedEntry* entry) {
  const OptU16 title = text_of(p, block, kTitle, arena);
  if (!title.has || title.value.length == 0) return false;
  *entry = FeedEntry{};
  entry->title = title.value;
  OptU16 guid = text_of(p, block, kGuid, arena);
  if (!guid.has) guid = text_of(p, block, kId, arena);
  entry->has_guid = guid.has;
  entry->guid = guid.value;
  const OptU16 link = link_of(p, block, arena);
  entry->has_link = link.has;
  entry->link = link.value;
  static const RawName kSummaries[] = {kDescription, kSummary, kContentEncoded, kContent};
  for (RawName name : kSummaries) {
    const OptU16 summary = raw_text(p, block, name, arena);
    if (summary.has) {
      entry->has_summary_html = true;
      entry->summary_html = summary.value;
      break;
    }
  }
  entry->has_published = date_of(p, block, &entry->published_ms, arena);
  OptU16 author = text_of(p, block, kDcCreator, arena);
  if (!author.has) author = text_of(p, block, kAuthor, arena);
  entry->has_author = author.has;
  entry->author = author.value;
  // allText(block, 'category'): every non-empty cleaned body.
  size_t count = 0, capacity = 0;
  U16* categories = nullptr;
  match_all(p.raw[kCategory], block, arena, [&](const Match& m) {
    const U16 value = clean_text(unwrap_cdata(p, group(block, m, 1), arena), arena);
    if (value.length == 0) return true;
    if (count == capacity) {
      const size_t next = capacity ? capacity * 2 : 4;
      auto* grown = static_cast<U16*>(arena->Alloc(next * sizeof(U16), alignof(U16)));
      if (!grown) return false;
      for (size_t i = 0; i < count; ++i) grown[i] = categories[i];
      categories = grown;
      capacity = next;
    }
    categories[count++] = value;
    return true;
  });
  entry->categories = categories;
  entry->category_count = count;
  const OptU16 image = image_of(p, block, arena);
  entry->has_image = image.has;
  entry->image_url = image.value;
  return arena->ok;
}

}  // namespace

bool ParseFeed(U16 xml, Arena* arena, FeedEntry* entries, size_t capacity, size_t* count) {
  *count = 0;
  const Programs* p = programs();
  if (!p) { arena->ok = false; return false; }
  if (xml.length > kMaxFeedUnits) return true;
  const U16 document = strip_comments(*p, xml, arena);
  const U16 lower = JsToLowerCase(document, arena);
  if (!arena->ok) return false;
  // [...elementBlocks(document, 'item'), ...elementBlocks(document, 'entry')]
  // truncated to MAX_ENTRIES before parsing.
  size_t blocks = 0;
  for (int which = 0; which < 2 && blocks < kMaxFeedEntries; ++which) {
    const Program& open = which == 0 ? p->open_item : p->open_entry;
    uint16_t close_units[8];
    const char* close = which == 0 ? "</item>" : "</entry>";
    const size_t close_length = strlen(close);
    for (size_t i = 0; i < close_length; ++i) close_units[i] = static_cast<uint8_t>(close[i]);
    const U16 close_text{close_units, close_length};
    match_all(open, document, arena, [&](const Match& m) {
      const U16 tag = group(document, m, 0);
      if (ends_with_ascii(tag, "/>")) return true;
      const size_t start = m.end;
      bool found = false;
      const size_t end = index_of(lower, close_text, start, &found);
      if (!found) return true;
      const U16 block = slice(document, start, end);
      ++blocks;
      if (*count < capacity) {
        FeedEntry entry;
        if (parse_entry(*p, block, arena, &entry)) entries[(*count)++] = entry;
      }
      return blocks < kMaxFeedEntries && arena->ok;
    });
  }
  return arena->ok;
}

// ---- feed registry and feed entries ---------------------------------------------

namespace {
const Topic kMacroOnly[] = {Topic::kMacro};
const Topic kPolicyOnly[] = {Topic::kPolicy};
}  // namespace

const FeedRegistration kNewsFeeds[] = {
    {"federal-reserve", "Federal Reserve", "https://www.federalreserve.gov/feeds/press_all.xml",
     kMacroOnly, 1, 10},
    {"sec", "U.S. SEC", "https://www.sec.gov/news/pressreleases.rss", kPolicyOnly, 1, 10},
    {"coindesk", "CoinDesk", "https://www.coindesk.com/arc/outboundfeeds/rss/", nullptr, 0, 8},
    {"the-block", "The Block", "https://www.theblock.co/rss.xml", nullptr, 0, 8},
    {"decrypt", "Decrypt", "https://decrypt.co/feed", nullptr, 0, 6},
    {"cointelegraph", "Cointelegraph", "https://cointelegraph.com/rss", nullptr, 0, 6},
};
const size_t kNewsFeedCount = sizeof(kNewsFeeds) / sizeof(kNewsFeeds[0]);

namespace {
constexpr double kMaxEntryAgeMs = 7.0 * 24 * 60 * 60 * 1000;
}

bool ToArticle(const FeedRegistration& feed, const FeedEntry& entry, double now_ms, Arena* arena,
               Article* output) {
  if (!entry.has_link || entry.title.length == 0) return false;
  if (!entry.has_published) return false;
  const double age = now_ms - entry.published_ms;
  if (age > kMaxEntryAgeMs) return false;
  if (age < -60.0 * 60 * 1000) return false;
  *output = Article{};
  output->source_id = ascii16(arena, feed.id);
  output->publisher = arena->FromUtf8(feed.publisher, strlen(feed.publisher));
  output->trust = feed.trust;
  output->external_id = entry.has_guid ? entry.guid : CanonicalUrl(entry.link, arena);
  output->title = entry.title;
  output->summary = entry.has_summary_html ? entry.summary_html : U16{};
  output->url = entry.link;
  output->has_image = entry.has_image;
  output->image_url = entry.image_url;
  output->published_ms = entry.published_ms;
  if (entry.category_count) {
    output->categories =
        static_cast<U16*>(arena->Alloc(entry.category_count * sizeof(U16), alignof(U16)));
    if (output->categories) {
      for (size_t i = 0; i < entry.category_count; ++i)
        output->categories[i] = JsToLowerCase(entry.categories[i], arena);
      output->category_count = entry.category_count;
    }
  }
  if (feed.declared_topic_count) {
    output->declared_topics =
        static_cast<Topic*>(arena->Alloc(feed.declared_topic_count * sizeof(Topic), alignof(Topic)));
    if (output->declared_topics) {
      memcpy(output->declared_topics, feed.declared_topics, feed.declared_topic_count * sizeof(Topic));
      output->declared_topic_count = feed.declared_topic_count;
    }
  }
  return arena->ok;
}

U16 ArticleKey(U16 source_id, U16 external_id, Arena* arena) {
  Builder out(arena);
  auto sink = [&](uint16_t c) { out.Push(c); };
  out.Push('[');
  quote_json(source_id, sink);
  out.Push(',');
  quote_json(external_id, sink);
  out.Push(']');
  return out.Done();
}

// ---- summaries ----------------------------------------------------------------------

namespace {

constexpr size_t kMaxSentences = 3;
constexpr size_t kMaxChars = 320;

bool is_alnum_lower(uint16_t c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

// value.toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim()
U16 normalize_for_compare(U16 value, Arena* arena) {
  const U16 lower = JsToLowerCase(value, arena);
  Builder out(arena);
  bool in_run = false;
  for (size_t i = 0; i < lower.length; ++i) {
    if (is_alnum_lower(lower.data[i])) { out.Push(lower.data[i]); in_run = false; }
    else if (!in_run) { out.Push(' '); in_run = true; }
  }
  return JsTrim(out.Done());
}

// text.split-sentence parts, filtered of empties.
size_t split_sentences(const Programs& p, U16 text, U16* parts, size_t capacity, Arena* arena) {
  size_t count = 0, start = 0;
  match_all(p.sentence, text, arena, [&](const Match& m) {
    const size_t end = m.start + 1;
    const U16 part = JsTrim(slice(text, start, end));
    if (part.length && count < capacity) parts[count++] = part;
    start = end + (m.end - m.start) - 1;
    return true;
  });
  const U16 tail = JsTrim(slice(text, start, text.length));
  if (tail.length && count < capacity) parts[count++] = tail;
  return count;
}

U16 clamp_to_sentences(const Programs& p, U16 text, Arena* arena) {
  U16 parts[kMaxSentences];
  const size_t count = split_sentences(p, text, parts, kMaxSentences, arena);
  Builder joined(arena);
  for (size_t i = 0; i < count; ++i) {
    if (i) joined.Push(' ');
    joined.Append(parts[i]);
  }
  U16 out = JsTrim(joined.Done());
  if (out.length == 0) out = text;
  if (out.length <= kMaxChars) return out;
  const U16 cut = slice(out, 0, kMaxChars);
  size_t last_space = 0;
  bool have_space = false;
  for (size_t i = cut.length; i > 0; --i)
    if (cut.data[i - 1] == ' ') { last_space = i - 1; have_space = true; break; }
  U16 body = have_space && static_cast<double>(last_space) > kMaxChars * 0.6
      ? slice(cut, 0, last_space) : cut;
  // .replace(/[\s,;:.]+$/, ''): strip the trailing run of those characters.
  while (body.length) {
    const uint16_t c = body.data[body.length - 1];
    if (!(is_space(c) || c == ',' || c == ';' || c == ':' || c == '.')) break;
    --body.length;
  }
  Builder result(arena);
  result.Append(body);
  result.Push(0x2026);
  return result.Done();
}

}  // namespace

U16 Summarize(bool has_html, U16 html, U16 fallback_title, Arena* arena) {
  const Programs* p = programs();
  if (!p) { arena->ok = false; return U16{}; }
  if (!has_html) return U16{};
  const U16 plain = clean_text(html, arena);
  if (plain.length == 0) return U16{};
  U16 text = plain;
  for (size_t i = 0; i < kBoilerplateCount; ++i)
    text = replace(p->boilerplate[i], text, replace_empty, nullptr, arena);
  text = Collapse(text, arena);
  if (text.length == 0) return U16{};
  const U16 trimmed = clamp_to_sentences(*p, text, arena);
  if (Equal(normalize_for_compare(trimmed, arena), normalize_for_compare(fallback_title, arena)))
    return U16{};
  return trimmed;
}

// ---- classification -------------------------------------------------------------------

size_t ClassifyTopics(U16 title, U16 summary, const U16* categories, size_t category_count,
                      const Topic* declared, size_t declared_count, Topic output[kMaxTopics],
                      Arena* scratch_arena) {
  const Programs* p = programs();
  Topic ordered[kTopicCount];
  size_t count = 0;
  auto add = [&](Topic topic) {
    for (size_t i = 0; i < count; ++i) if (ordered[i] == topic) return;
    if (count < kTopicCount) ordered[count++] = topic;
  };
  for (size_t i = 0; i < declared_count; ++i) add(declared[i]);
  Arena& arena = *scratch_arena;
  const size_t mark = arena.used;
  if (p) {
    for (size_t i = 0; i < category_count; ++i) {
      const U16 normalized = JsToLowerCase(categories[i], &arena);
      Topic topic;
      if (TopicFromName(normalized, &topic)) { add(topic); continue; }
      for (size_t r = 0; r < kCategoryRuleCount; ++r)
        if (test(p->category[r], normalized, &arena)) add(kCategoryTopics[r].topic);
    }
    Builder text(&arena);
    text.Append(title);
    text.Push(' ');
    text.Append(summary);
    const U16 joined = text.Done();
    for (size_t r = 0; r < kTextRuleCount; ++r)
      if (test(p->text_rule[r], joined, &arena)) add(kTextRules[r].topic);
  }
  if (arena.ok) arena.used = mark;
  if (count == 0) add(Topic::kTrend);
  const size_t n = count < kMaxTopics ? count : kMaxTopics;
  for (size_t i = 0; i < n; ++i) output[i] = ordered[i];
  return n;
}

// ---- tickers ------------------------------------------------------------------------

namespace {

const char* const kNeverATicker[] = {
    "SEC", "CEO", "CFO", "COO", "CTO", "ETF", "ETFS", "API", "NFT", "NFTS", "DAO", "DEX", "CEX",
    "CPI", "PPI", "GDP", "FED", "FOMC", "ECB", "IMF", "USD", "EUR", "GBP", "JPY", "AI", "IPO",
    "SPAC", "KYC", "AML", "DOJ", "CFTC", "FCA", "ESMA", "MICA", "TVL", "APR", "APY", "ATH", "FUD",
    "DEFI", "NEWS", "US", "UK", "EU", "UN", "ETH2", "Q1", "Q2", "Q3", "Q4"};

bool never_a_ticker(U16 symbol) {
  for (const char* word : kNeverATicker) if (EqualAscii(symbol, word)) return true;
  return false;
}

bool word_char(const uint16_t* c) { return c && is_alnum_lower(*c); }

// indexOfWord(haystack, word).
bool index_of_word(U16 haystack, U16 word, size_t* at) {
  size_t from = 0;
  for (;;) {
    bool found = false;
    const size_t i = index_of(haystack, word, from, &found);
    if (!found) return false;
    const uint16_t* before = i == 0 ? nullptr : &haystack.data[i - 1];
    const uint16_t* after = i + word.length < haystack.length ? &haystack.data[i + word.length] : nullptr;
    if (!word_char(before) && !word_char(after)) { *at = i; return true; }
    from = i + 1;
  }
}

}  // namespace

bool BuildTickerDictionary(const networks::NativeNetworkRegistry& registry, Arena* arena,
                           TickerDictionary* output) {
  *output = TickerDictionary{};
  size_t count = 0;
  const networks::NativeNetworkConfig* networks = registry.list(&count);
  output->entries = static_cast<TickerEntry*>(arena->Alloc(count * sizeof(TickerEntry) + 1, alignof(TickerEntry)));
  output->aliases = static_cast<TickerAlias*>(arena->Alloc(count * 2 * sizeof(TickerAlias) + 1, alignof(TickerAlias)));
  if (!output->entries || !output->aliases) return false;
  for (size_t n = 0; n < count; ++n) {
    const networks::NativeNetworkConfig& network = networks[n];
    if (network.testnet || !network.native_symbol) continue;
    const char* coin = network.native_coingecko_id;
    const U16 raw_symbol = arena->FromUtf8(network.native_symbol, strlen(network.native_symbol));
    const U16 symbol = case_map(JsTrim(raw_symbol), arena, true);
    if (symbol.length == 0) continue;
    TickerEntry entry{};
    entry.symbol = symbol;
    entry.has_coin_id = coin != nullptr;
    if (coin) entry.coin_id = arena->FromUtf8(coin, strlen(coin));
    entry.primary = true;
    for (size_t i = 0; i < output->entry_count; ++i)
      if (Equal(output->entries[i].symbol, symbol)) { entry.primary = false; break; }
    const size_t index = output->entry_count++;
    output->entries[index] = entry;
    const char* aliases[2] = {coin ? coin : "", network.id};
    for (const char* alias : aliases) {
      if (!alias || !*alias) continue;
      const U16 key = JsToLowerCase(JsTrim(arena->FromUtf8(alias, strlen(alias))), arena);
      if (key.length == 0) continue;
      bool known = false;
      for (size_t i = 0; i < output->alias_count; ++i)
        if (Equal(output->aliases[i].alias, key)) { known = true; break; }
      if (!known) output->aliases[output->alias_count++] = TickerAlias{key, index};
    }
  }
  return arena->ok;
}

size_t ExtractTickers(U16 text, const TickerDictionary& dictionary, Ticker output[kMaxTickers],
                      Arena* arena_pointer) {
  const Programs* p = programs();
  struct Found {
    const TickerEntry* entry;
    size_t at;
  };
  static constexpr size_t kMaxFound = 256;
  Found found[kMaxFound]{};
  size_t found_count = 0;
  auto record = [&](const TickerEntry* entry, size_t at) {
    for (size_t i = 0; i < found_count; ++i) {
      if (Equal(found[i].entry->symbol, entry->symbol)) {
        if (at < found[i].at) found[i] = Found{entry, at};  // Map.set keeps the position
        return;
      }
    }
    if (found_count < kMaxFound) found[found_count++] = Found{entry, at};
  };
  if (p) {
    match_all(p->ticker, text, arena_pointer, [&](const Match& m) {
      const U16 symbol = group(text, m, 1);
      if (symbol.length < 3 || never_a_ticker(symbol)) return true;
      for (size_t i = 0; i < dictionary.entry_count; ++i) {
        const TickerEntry& entry = dictionary.entries[i];
        if (entry.primary && Equal(entry.symbol, symbol)) { record(&entry, m.start); break; }
      }
      return true;
    });
  }
  Arena& arena = *arena_pointer;
  const size_t mark = arena.used;
  const U16 lower = JsToLowerCase(text, &arena);
  for (size_t a = 0; a < dictionary.alias_count; ++a) {
    size_t at = 0;
    if (index_of_word(lower, dictionary.aliases[a].alias, &at))
      record(&dictionary.entries[dictionary.aliases[a].entry], at);
  }
  if (arena.ok) arena.used = mark;
  // Stable sort by first mention.
  for (size_t i = 1; i < found_count; ++i) {
    const Found current = found[i];
    size_t j = i;
    while (j > 0 && found[j - 1].at > current.at) { found[j] = found[j - 1]; --j; }
    found[j] = current;
  }
  const size_t n = found_count < kMaxTickers ? found_count : kMaxTickers;
  for (size_t i = 0; i < n; ++i) {
    output[i] = Ticker{};
    output[i].symbol = found[i].entry->symbol;
    output[i].has_coin_id = found[i].entry->has_coin_id;
    output[i].coin_id = found[i].entry->coin_id;
  }
  return n;
}

// ---- impact --------------------------------------------------------------------------

Impact ScoreImpact(const Cluster& cluster, double now_ms, Arena* arena) {
  for (size_t i = 0; i < cluster.topic_count; ++i)
    if (cluster.topics[i] == Topic::kSecurity) return Impact::kWatch;
  const Programs* p = programs();
  if (p) {
    const size_t mark = arena->used;
    Builder text(arena);
    text.Append(cluster.headline);
    text.Push(' ');
    text.Append(cluster.summary);
    const U16 joined = text.Done();
    bool watch = false;
    for (size_t i = 0; i < kWatchCount && !watch; ++i) watch = test(p->watch[i], joined, arena);
    if (arena->ok) arena->used = mark;
    if (watch) return Impact::kWatch;
  }
  size_t publishers = 0;
  for (size_t i = 0; i < cluster.article_count && publishers < 2; ++i) {
    bool seen = false;
    for (size_t j = 0; j < i; ++j)
      if (Equal(cluster.articles[j].publisher, cluster.articles[i].publisher)) { seen = true; break; }
    if (!seen) ++publishers;
  }
  const double age = now_ms - cluster.published_ms;
  const bool recent = age >= 0 && age <= 90.0 * 60 * 1000;
  if (recent && publishers >= 2) return Impact::kBreaking;
  return Impact::kNormal;
}

// ---- clustering ----------------------------------------------------------------------

namespace {

constexpr double kOverlapThreshold = 0.6;
constexpr double kSimilarityThreshold = 0.5;
constexpr size_t kMinOverlapWords = 4;
constexpr size_t kShingleSize = 3;

const char* const kStopwords[] = {
    "a", "an", "the", "and", "or", "but", "of", "to", "in", "on", "at", "for", "with", "by",
    "from", "as", "is", "are", "was", "were", "be", "been", "it", "its", "this", "that", "these",
    "those", "has", "have", "had", "will", "would", "can", "could", "may", "might", "says", "said",
    "after", "over", "amid", "into", "up", "down", "new", "more", "than", "about", "us", "first",
    "now", "next", "just", "not", "no", "you", "your", "how", "why", "what", "who", "when", "top",
    "big", "key", "set", "sets", "get", "gets", "still", "back", "out", "off", "per", "via",
    "ahead"};

bool stopword(U16 word) {
  for (const char* s : kStopwords) if (EqualAscii(word, s)) return true;
  return false;
}

struct WordSet {
  U16* items = nullptr;
  size_t count = 0;
};

bool set_has(const WordSet& set, U16 value) {
  for (size_t i = 0; i < set.count; ++i) if (Equal(set.items[i], value)) return true;
  return false;
}

// headline.toLowerCase().replace(/[^a-z0-9$ ]+/g, ' ').split(/\s+/): the
// replaced text holds no whitespace but U+0020, so the words are its maximal
// runs of other characters.
size_t headline_words(U16 headline, Arena* arena, U16** words) {
  const U16 lower = JsToLowerCase(headline, arena);
  auto* buffer = static_cast<uint16_t*>(arena->Alloc(lower.length * 2 + 2, 2));
  U16* list = static_cast<U16*>(arena->Alloc((lower.length / 1 + 1) * sizeof(U16), alignof(U16)));
  if (!buffer || !list) { *words = nullptr; return 0; }
  size_t n = 0;
  bool in_run = false;
  for (size_t i = 0; i < lower.length; ++i) {
    const uint16_t c = lower.data[i];
    if (is_alnum_lower(c) || c == '$' || c == ' ') { buffer[n++] = c; in_run = false; }
    else if (!in_run) { buffer[n++] = ' '; in_run = true; }
  }
  size_t count = 0;
  for (size_t i = 0; i < n;) {
    while (i < n && buffer[i] == ' ') ++i;
    const size_t start = i;
    while (i < n && buffer[i] != ' ') ++i;
    if (i > start) list[count++] = U16{buffer + start, i - start};
  }
  *words = list;
  return count;
}

WordSet shingle(U16 headline, Arena* arena) {
  U16* words = nullptr;
  const size_t raw = headline_words(headline, arena, &words);
  WordSet out;
  size_t kept = 0;
  for (size_t i = 0; i < raw; ++i)
    if (!stopword(words[i])) words[kept++] = words[i];
  if (kept == 0) return out;
  out.items = static_cast<U16*>(arena->Alloc(kept * sizeof(U16), alignof(U16)));
  if (!out.items) return WordSet{};
  if (kept < kShingleSize) {
    for (size_t i = 0; i < kept; ++i)
      if (!set_has(out, words[i])) out.items[out.count++] = words[i];
    return out;
  }
  for (size_t i = 0; i + kShingleSize <= kept; ++i) {
    Builder b(arena);
    for (size_t k = 0; k < kShingleSize; ++k) {
      if (k) b.Push(' ');
      b.Append(words[i + k]);
    }
    const U16 value = b.Done();
    if (!set_has(out, value)) out.items[out.count++] = value;
  }
  return out;
}

WordSet significant_words(U16 headline, Arena* arena) {
  U16* words = nullptr;
  const size_t raw = headline_words(headline, arena, &words);
  WordSet out;
  out.items = words;
  for (size_t i = 0; i < raw; ++i)
    if (words[i].length > 2 && !stopword(words[i]) && !set_has(out, words[i]))
      out.items[out.count++] = words[i];
  return out;
}

double jaccard(const WordSet& a, const WordSet& b) {
  if (a.count == 0 || b.count == 0) return 0;
  size_t shared = 0;
  for (size_t i = 0; i < a.count; ++i) if (set_has(b, a.items[i])) ++shared;
  return static_cast<double>(shared) / static_cast<double>(a.count + b.count - shared);
}

double overlap_coefficient(const WordSet& a, const WordSet& b) {
  if (a.count < kMinOverlapWords || b.count < kMinOverlapWords) return 0;
  size_t shared = 0;
  for (size_t i = 0; i < a.count; ++i) if (set_has(b, a.items[i])) ++shared;
  return static_cast<double>(shared) / static_cast<double>(a.count < b.count ? a.count : b.count);
}

double story_score(const WordSet& significant_a, const WordSet& significant_b,
                   const WordSet& shingles_a, const WordSet& shingles_b) {
  const double overlap = overlap_coefficient(significant_a, significant_b);
  if (overlap >= kOverlapThreshold) return overlap;
  const double shingled = jaccard(shingles_a, shingles_b);
  return shingled >= kSimilarityThreshold ? shingled : 0;
}

struct Work {
  Cluster cluster;
  U16* canonical;  // canonicalUrl of each member url (parallel to articles)
  size_t article_capacity;
  size_t topic_capacity;
  size_t ticker_capacity;
  WordSet shingles;             // of the headline at creation; never refreshed
  WordSet significant;          // of `significant_for`
  U16 significant_for;
  bool dirty;
  bool existing;
  size_t existing_index;
};

template <typename T>
bool grow(Arena* arena, T** items, size_t count, size_t* capacity, size_t wanted) {
  if (wanted <= *capacity) return true;
  size_t next = *capacity ? *capacity * 2 : 4;
  while (next < wanted) next *= 2;
  auto* grown = static_cast<T*>(arena->Alloc(next * sizeof(T), alignof(T)));
  if (!grown) return false;
  for (size_t i = 0; i < count; ++i) grown[i] = (*items)[i];
  *items = grown;
  *capacity = next;
  return true;
}

// a.trust desc, then a.publishedAt asc: `b.trust - a.trust || a.publishedAt - b.publishedAt`.
double member_order(const Article& a, const Article& b) {
  const double trust = b.trust - a.trust;
  if (trust != 0 && !isnan(trust)) return trust;
  return a.published_ms - b.published_ms;
}

compat::url::UrlScratch g_canonical_scratch;
char g_canonical_output[compat::url::kMaxHrefBytes];
uint8_t g_canonical_input[3 * compat::url::kMaxInputBytes + 4];
fortis::base::Mutex g_canonical_mutex;

}  // namespace

U16 CanonicalUrl(U16 url, Arena* arena) {
  fortis::base::UniqueLock g_canonical_lock(g_canonical_mutex);
  codec::history_batch::OutBuffer in{g_canonical_input, sizeof(g_canonical_input) - 1};
  AppendUtf8(in, url);  // lone surrogates -> U+FFFD (USVString)
  size_t length = 0;
  const bool ok = in.ok && compat::url::CanonicalArticleUrl(
      reinterpret_cast<const char*>(g_canonical_input), in.used, g_canonical_output,
      sizeof(g_canonical_output), &length, &g_canonical_scratch);
  U16 result;
  if (ok) result = arena->FromUtf8(g_canonical_output, length);
  g_canonical_lock.Unlock();
  if (ok) return result;
  return JsToLowerCase(JsTrim(url), arena);
}

bool ClusterArticles(const ClusterInput* inputs, size_t input_count, const Cluster* existing,
                     size_t existing_count, base::IdSource& new_id, Arena* arena,
                     Cluster** output, size_t* output_count, bool* existing_touched) {
  *output = nullptr;
  *output_count = 0;
  if (existing_touched) for (size_t i = 0; i < existing_count; ++i) existing_touched[i] = false;
  const size_t work_capacity = existing_count + input_count;
  Work* work = static_cast<Work*>(arena->Alloc((work_capacity + 1) * sizeof(Work), alignof(Work)));
  if (!work) return false;
  size_t work_count = 0;
  for (size_t e = 0; e < existing_count; ++e) {
    Work& w = work[work_count++];
    w = Work{};
    w.cluster = existing[e];
    w.existing = true;
    w.existing_index = e;
    const Cluster& c = existing[e];
    w.article_capacity = c.article_count + 1;
    w.cluster.articles = static_cast<Article*>(arena->Alloc(w.article_capacity * sizeof(Article), alignof(Article)));
    w.canonical = static_cast<U16*>(arena->Alloc(w.article_capacity * sizeof(U16), alignof(U16)));
    w.topic_capacity = c.topic_count + kTopicCount;
    w.cluster.topics = static_cast<Topic*>(arena->Alloc(w.topic_capacity * sizeof(Topic), alignof(Topic)));
    w.ticker_capacity = c.ticker_count + kMaxTickers;
    w.cluster.tickers = static_cast<Ticker*>(arena->Alloc(w.ticker_capacity * sizeof(Ticker), alignof(Ticker)));
    if (!w.cluster.articles || !w.canonical || !w.cluster.topics || !w.cluster.tickers) return false;
    for (size_t i = 0; i < c.article_count; ++i) {
      w.cluster.articles[i] = c.articles[i];
      w.canonical[i] = CanonicalUrl(c.articles[i].url, arena);
    }
    for (size_t i = 0; i < c.topic_count; ++i) w.cluster.topics[i] = c.topics[i];
    for (size_t i = 0; i < c.ticker_count; ++i) w.cluster.tickers[i] = c.tickers[i];
    w.shingles = shingle(c.headline, arena);
  }

  // Oldest first (a stable sort of the inputs by publish time).
  size_t* order = static_cast<size_t*>(arena->Alloc((input_count + 1) * sizeof(size_t), alignof(size_t)));
  if (!order) return false;
  for (size_t i = 0; i < input_count; ++i) {
    size_t j = i;
    while (j > 0 && inputs[order[j - 1]].article.published_ms - inputs[i].article.published_ms > 0) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = i;
  }

  for (size_t k = 0; k < input_count && arena->ok; ++k) {
    const ClusterInput& input = inputs[order[k]];
    const WordSet shingles = shingle(input.article.title, arena);
    const U16 url = CanonicalUrl(input.article.url, arena);
    const WordSet input_significant = significant_words(input.article.title, arena);
    // findMatch.
    Work* match = nullptr;
    Work* best = nullptr;
    double best_score = 0;
    for (size_t i = 0; i < work_count && !match; ++i) {
      Work& w = work[i];
      for (size_t a = 0; a < w.cluster.article_count; ++a)
        if (Equal(w.canonical[a], url)) { match = &w; break; }
      if (match) break;
      const double apart = fabs(w.cluster.published_ms - input.article.published_ms);
      if (apart > kClusterWindowMs) continue;
      if (!Equal(w.significant_for, w.cluster.headline) || !w.significant.items) {
        w.significant = significant_words(w.cluster.headline, arena);
        w.significant_for = w.cluster.headline;
      }
      const double score = story_score(input_significant, w.significant, shingles, w.shingles);
      if (score > 0 && (!best || score > best_score)) {
        best = &w;
        best_score = score;
      }
    }
    if (!match) match = best;
    if (!match) {
      // seedCluster.
      if (work_count >= work_capacity) { arena->ok = false; break; }
      Work& w = work[work_count++];
      w = Work{};
      char id[27];
      if (!new_id.NewId(id)) return false;
      w.cluster.id = ascii16(arena, id);
      w.cluster.headline = input.article.title;
      w.cluster.summary = input.summary;
      w.topic_capacity = kTopicCount;
      w.cluster.topics = static_cast<Topic*>(arena->Alloc(w.topic_capacity * sizeof(Topic), alignof(Topic)));
      w.ticker_capacity = input.ticker_count + kMaxTickers;
      w.cluster.tickers = static_cast<Ticker*>(arena->Alloc(w.ticker_capacity * sizeof(Ticker), alignof(Ticker)));
      w.article_capacity = 2;
      w.cluster.articles = static_cast<Article*>(arena->Alloc(w.article_capacity * sizeof(Article), alignof(Article)));
      w.canonical = static_cast<U16*>(arena->Alloc(w.article_capacity * sizeof(U16), alignof(U16)));
      if (!w.cluster.topics || !w.cluster.tickers || !w.cluster.articles || !w.canonical) return false;
      for (size_t i = 0; i < input.topic_count; ++i) w.cluster.topics[w.cluster.topic_count++] = input.topics[i];
      w.cluster.impact = Impact::kNormal;
      for (size_t i = 0; i < input.ticker_count; ++i) w.cluster.tickers[w.cluster.ticker_count++] = input.tickers[i];
      w.cluster.publisher = input.article.publisher;
      w.cluster.url = input.article.url;
      w.cluster.has_image = input.article.has_image;
      w.cluster.image_url = input.article.image_url;
      w.cluster.published_ms = input.article.published_ms;
      w.cluster.articles[0] = input.article;
      w.canonical[0] = url;
      w.cluster.article_count = 1;
      w.shingles = shingles;
      w.dirty = true;
      continue;
    }
    // join.
    Cluster& c = match->cluster;
    bool duplicate = false;
    for (size_t a = 0; a < c.article_count; ++a)
      if (Equal(match->canonical[a], url)) { duplicate = true; break; }
    if (duplicate) continue;
    if (c.article_count + 1 > match->article_capacity) {
      size_t capacity = match->article_capacity;
      size_t canonical_capacity = match->article_capacity;
      if (!grow(arena, &c.articles, c.article_count, &capacity, c.article_count + 1) ||
          !grow(arena, &match->canonical, c.article_count, &canonical_capacity, c.article_count + 1))
        return false;
      match->article_capacity = capacity;
    }
    // push, then Array.prototype.sort (stable) by trust desc, time asc.
    c.articles[c.article_count] = input.article;
    match->canonical[c.article_count] = url;
    ++c.article_count;
    for (size_t i = 1; i < c.article_count; ++i) {
      const Article moving = c.articles[i];
      const U16 moving_url = match->canonical[i];
      size_t j = i;
      while (j > 0 && member_order(c.articles[j - 1], moving) > 0) {
        c.articles[j] = c.articles[j - 1];
        match->canonical[j] = match->canonical[j - 1];
        --j;
      }
      c.articles[j] = moving;
      match->canonical[j] = moving_url;
    }
    if (c.article_count > kMaxClusterMembers) c.article_count = kMaxClusterMembers;
    const Article& lead = c.articles[0];
    if (!Equal(lead.url, c.url)) {
      c.headline = lead.title;
      c.publisher = lead.publisher;
      c.url = lead.url;
    }
    if (c.summary.length == 0 && input.summary.length != 0) c.summary = input.summary;
    if (!c.has_image && input.article.has_image) {
      c.has_image = true;
      c.image_url = input.article.image_url;
    }
    if (input.article.published_ms < c.published_ms) c.published_ms = input.article.published_ms;
    for (size_t t = 0; t < input.topic_count; ++t) {
      bool present = false;
      for (size_t i = 0; i < c.topic_count; ++i) if (c.topics[i] == input.topics[t]) present = true;
      if (present) continue;
      if (!grow(arena, &c.topics, c.topic_count, &match->topic_capacity, c.topic_count + 1)) return false;
      c.topics[c.topic_count++] = input.topics[t];
    }
    for (size_t t = 0; t < input.ticker_count; ++t) {
      bool present = false;
      for (size_t i = 0; i < c.ticker_count; ++i)
        if (Equal(c.tickers[i].symbol, input.tickers[t].symbol)) present = true;
      if (present) continue;
      if (!grow(arena, &c.tickers, c.ticker_count, &match->ticker_capacity, c.ticker_count + 1)) return false;
      c.tickers[c.ticker_count++] = input.tickers[t];
    }
    match->dirty = true;
  }
  if (!arena->ok) return false;
  size_t dirty = 0;
  for (size_t i = 0; i < work_count; ++i) if (work[i].dirty) ++dirty;
  Cluster* result = static_cast<Cluster*>(arena->Alloc((dirty + 1) * sizeof(Cluster), alignof(Cluster)));
  if (!result) return false;
  size_t n = 0;
  for (size_t i = 0; i < work_count; ++i) {
    if (!work[i].dirty) continue;
    result[n++] = work[i].cluster;
    if (work[i].existing && existing_touched) existing_touched[work[i].existing_index] = true;
  }
  *output = result;
  *output_count = n;
  return true;
}

size_t SourceCount(const Cluster& cluster, Arena* arena) {
  size_t distinct = 0;
  const size_t mark = arena->used;
  U16* seen = static_cast<U16*>(arena->Alloc((cluster.article_count + 1) * sizeof(U16), alignof(U16)));
  if (!seen) return 1;
  for (size_t i = 0; i < cluster.article_count; ++i) {
    const U16 lower = JsToLowerCase(cluster.articles[i].publisher, arena);
    bool known = false;
    for (size_t k = 0; k < distinct; ++k) if (Equal(seen[k], lower)) { known = true; break; }
    if (!known) seen[distinct++] = lower;
  }
  if (arena->ok) arena->used = mark;
  return distinct > 1 ? distinct : 1;
}

}  // namespace fortis::news
