#include "test_support.h"
// Unit checks of GET /v1/networks/{network}/tokens/search (R25) below the
// HTTP layer, against the TypeScript oracle token_search_edges.json:
//  - every request record's target through the handler's q parse
//    (TokenSearchTerm: fast-querystring, JS trim, 1..100 UTF-16 units, then
//    trim().toLowerCase()) gives TypeScript's `unit.term` byte for byte, or
//    refuses exactly where Zod refused q (unit.term null);
//  - the cache key (SearchCacheKey) of every distinct term is distinct, holds
//    no NUL and is `search:<id>:<term>` wherever the term has no NUL or '%';
//  - every 200 JSON answer, rebuilt as the native stored record from
//    TypeScript's items, renders TypeScript's JSON text byte for byte with
//    its provenance, and its protobuf twin (the same term read as
//    application/protobuf) TypeScript's TokenSearchResponse wire, or is
//    refused where protobuf-es threw (the 500); one byte short is refused.
// Usage: token_search_test token_search_edges.json
#include "strict_unit_expectations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>

#include "token_json.h"
#include "token_search_route.h"

using namespace fortis::discovery;

namespace {

namespace tj = fortis::codec::tj;

int g_failures = 0;
int g_checks = 0;
void check(bool condition, const char* what, const char* detail = "") {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s %s\n", what, detail);
}

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file);
    free(data);
    return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

const tj::Value* at(const tj::Value* array, uint32_t index) {
  return tj::IsArray(array) && index < array->count ? array->items[index] : nullptr;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_iso(const tj::Value* text, int64_t* out) {
  int year, month, day, hour, minute, second, millis;
  if (!tj::IsString(text) ||
      !fortis::test::ParseTimestampParts(text->text, &year, &month, &day, &hour, &minute, &second, &millis))
    return false;
  *out = ((days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 24 + hour) * 60 + minute) *
             60000 + second * 1000 + millis;
  return true;
}

std::string target_of(const tj::Value* record) {
  const tj::Value* hex = tj::Get(record, "targetHex");
  std::string target;
  for (uint32_t i = 0; tj::IsString(hex) && i + 1 < hex->length; i += 2)
    target.push_back(static_cast<char>(hex_digit(hex->text[i]) * 16 + hex_digit(hex->text[i + 1])));
  return target;
}

// ---- the q parse and the cache key ----------------------------------------------------------------------

struct TermCounts { int accepted = 0, refused = 0, keys = 0; };

void run_term(const tj::Value* record, std::map<std::string, std::string>* keys, TermCounts* counts) {
  const tj::Value* unit = tj::Get(record, "unit");
  if (!tj::IsObject(unit)) return;
  const tj::Value* name = tj::Get(record, "name");
  char label[200];
  snprintf(label, sizeof(label), "term %s", tj::IsString(name) ? name->text : "?");
  const tj::Value* expected = tj::Get(unit, "term");
  const std::string target = target_of(record);
  char term[1024];
  size_t length = 0;
  const bool accepted = TokenSearchTerm(target.c_str(), term, sizeof(term), &length);
  if (!tj::IsString(expected)) {
    check(!accepted, label, "Zod refused q: the native parse must refuse it");
    ++counts->refused;
    return;
  }
  check(accepted && length == expected->length && memcmp(term, expected->text, length) == 0, label,
        "the term TypeScript searched by");
  ++counts->accepted;
  if (!accepted) return;
  char key[2048];
  size_t key_length = 0;
  check(SearchCacheKey("eth-mainnet", term, length, key, sizeof(key), &key_length), label, "cache key");
  check(strlen(key) == key_length, label, "cache key holds no NUL");
  const std::string term_text(term, length);
  if (term_text.find('\0') == std::string::npos && term_text.find('%') == std::string::npos)
    check(std::string(key, key_length) == "search:eth-mainnet:" + term_text, label, "keys.tokenSearch spelling");
  const std::string key_text(key, key_length);
  auto found = keys->find(key_text);
  check(found == keys->end() || found->second == term_text, label, "distinct terms share a cache key");
  if (found == keys->end()) { (*keys)[key_text] = term_text; ++counts->keys; }
}

// ---- the stored record and the encoders -------------------------------------------------------------------

void put_string(std::string& out, const tj::Value* value) {
  const size_t length = tj::IsString(value) ? value->length : 0;
  out.push_back(static_cast<char>(length & 0xff));
  out.push_back(static_cast<char>((length >> 8) & 0xff));
  out.push_back(static_cast<char>((length >> 16) & 0xff));
  if (length) out.append(value->text, length);
}

void put_double(std::string& out, double value) {
  char bytes[8];
  memcpy(bytes, &value, 8);
  out.append(bytes, 8);
}

// TypeScript's items as the native `TSR1` record (token_search_route.cpp
// write_record): count, then per item network, tokenId, address, symbol and
// name (24-bit length + WTF-8), decimals, verified, holders, logo.
bool build_record(const tj::Value* items, std::string* record) {
  if (!tj::IsArray(items) || items->count > 20) return false;
  record->assign("TSR1");
  record->push_back(static_cast<char>(items->count));
  for (uint32_t i = 0; i < items->count; ++i) {
    const tj::Value* item = items->items[i];
    for (const char* name : {"network", "tokenId", "address", "symbol", "name"}) put_string(*record, tj::Get(item, name));
    const tj::Value* decimals = tj::Get(item, "decimals");
    if (!tj::IsNumber(decimals)) return false;
    put_double(*record, decimals->number);
    const tj::Value* verified = tj::Get(item, "verified");
    record->push_back(verified && verified->type == tj::Type::kTrue ? 1 : 0);
    const tj::Value* holders = tj::Get(item, "holderCount");
    record->push_back(tj::IsNumber(holders) ? 1 : 0);
    put_double(*record, tj::IsNumber(holders) ? holders->number : 0);
    const tj::Value* logo = tj::Get(item, "logoMediaKey");
    record->push_back(tj::IsString(logo) ? 1 : 0);
    if (tj::IsString(logo)) {
      if (logo->length != 32) return false;
      record->append(logo->text, 32);
    }
  }
  return true;
}

std::string hex_of(const uint8_t* bytes, size_t length) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 15]);
  }
  return out;
}

struct RenderCounts { int json = 0, protobuf = 0, refused = 0; };

constexpr size_t kOut = 1024 * 1024;

void run_render(const tj::Value* record, const tj::Value* cases, uint8_t* out, RenderCounts* counts) {
  const tj::Value* name = tj::Get(record, "name");
  const tj::Value* status = tj::Get(record, "status");
  const tj::Value* method = tj::Get(record, "method");
  const tj::Value* text = tj::Get(record, "responseText");
  if (!tj::IsString(name) || !tj::IsNumber(status) || status->number != 200 || !tj::IsString(method) ||
      strcmp(method->text, "GET") != 0 || !tj::IsString(text) || !text->length)
    return;
  char label[200];
  snprintf(label, sizeof(label), "render %s", name->text);
  tj::Arena a = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* body = tj::Parse(a, reinterpret_cast<const uint8_t*>(text->text), text->length);
  std::string stored;
  const bool built = body && build_record(tj::Get(body, "items"), &stored);
  check(built, label, "TypeScript's items as a record");
  if (!built) { tj::FreeArena(a); return; }
  const tj::Value* cache = tj::Get(body, "cache");
  const bool hit = tj::Get(cache, "hit") && tj::Get(cache, "hit")->type == tj::Type::kTrue;
  int64_t as_of = -1;
  if (hit) check(parse_iso(tj::Get(cache, "timestamp"), &as_of), label, "cache timestamp");
  size_t used = 0;
  const auto* bytes = reinterpret_cast<const unsigned char*>(stored.data());
  const bool rendered = TokenSearchRender(bytes, stored.size(), hit, as_of, false, out, kOut, &used);
  check(rendered && used == text->length && memcmp(out, text->text, used) == 0, label, "JSON text");
  if (rendered && used > 1) {
    size_t short_used = 0;
    check(!TokenSearchRender(bytes, stored.size(), hit, as_of, false, out, used - 1, &short_used), label,
          "JSON one byte short is refused");
  }
  ++counts->json;
  // The protobuf twin: `<name>-protobuf`, or `<stem>-protobuf` for a
  // `<stem>-json` record, the same term.
  std::string stem = name->text;
  if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "-json") == 0) stem.resize(stem.size() - 5);
  const std::string twin_name = stem + "-protobuf";
  for (uint32_t i = 0; cases && i < cases->count; ++i) {
    const tj::Value* twin = cases->items[i];
    const tj::Value* twin_label = tj::Get(twin, "name");
    const tj::Value* twin_method = tj::Get(twin, "method");
    if (!tj::IsString(twin_label) || twin_name != twin_label->text || !tj::IsString(twin_method) ||
        strcmp(twin_method->text, "GET") != 0)
      continue;
    const tj::Value* twin_status = tj::Get(twin, "status");
    const tj::Value* wire = tj::Get(twin, "wireHex");
    const tj::Value* decoded = tj::Get(twin, "decoded");
    const tj::Value* twin_cache = tj::Get(decoded, "cache");
    const bool twin_hit = tj::Get(twin_cache, "hit") && tj::Get(twin_cache, "hit")->type == tj::Type::kTrue;
    int64_t twin_as_of = -1;
    if (twin_hit) parse_iso(tj::Get(twin_cache, "timestamp"), &twin_as_of);
    const bool encoded = TokenSearchRender(bytes, stored.size(), twin_hit, twin_as_of, true, out, kOut, &used);
    char twin_label_text[220];
    snprintf(twin_label_text, sizeof(twin_label_text), "render %s", twin_label->text);
    if (tj::IsNumber(twin_status) && twin_status->number == 500) {
      check(!encoded, twin_label_text, "protobuf-es refused a value: the native encoder must refuse it");
      ++counts->refused;
    } else {
      check(encoded && tj::IsString(wire) && hex_of(out, used) == std::string(wire->text, wire->length),
            twin_label_text, "TokenSearchResponse wire");
      if (encoded && used > 1) {
        size_t short_used = 0;
        check(!TokenSearchRender(bytes, stored.size(), twin_hit, twin_as_of, true, out, used - 1, &short_used),
              twin_label_text, "protobuf one byte short is refused");
      }
      ++counts->protobuf;
    }
  }
  tj::FreeArena(a);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s token_search_edges.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* text = read_file(argv[1], &length);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena a = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(a, reinterpret_cast<const uint8_t*>(text), length);
  if (!strict::ContractExpectations(a, fixture, "token_search_edges.json")) return 2;
  strict::PatchRows(a, tj::Get(fixture, "cases"), tj::Get(strict::UnitExpectations(a), "search"), "name", "unit");
  if (!fixture) {
    fprintf(stderr, "cannot parse %s\n", argv[1]);
    return 2;
  }
  const tj::Value* cases = tj::Get(fixture, "cases");
  std::map<std::string, std::string> keys;
  TermCounts terms;
  RenderCounts renders;
  uint8_t* out = static_cast<uint8_t*>(malloc(kOut));
  for (uint32_t i = 0; cases && i < cases->count; ++i) {
    const tj::Value* record = at(cases, i);
    if (tj::Get(record, "state") || tj::Get(record, "concurrent")) continue;
    run_term(record, &keys, &terms);
    run_render(record, cases, out, &renders);
  }
  // NUL and '%' terms the fixture does not reach: the escape stays injective.
  static const char* const kTerms[] = {"%00a", "\0a", "%2500a", "%", "%25", "a\0", "a%00"};
  static const size_t kLengths[] = {4, 2, 6, 1, 3, 2, 4};
  std::map<std::string, std::string> extra;
  for (size_t i = 0; i < sizeof(kTerms) / sizeof(kTerms[0]); ++i) {
    char key[256];
    size_t key_length = 0;
    check(SearchCacheKey("base", kTerms[i], kLengths[i], key, sizeof(key), &key_length) && strlen(key) == key_length,
          "escape", kTerms[i]);
    const std::string term(kTerms[i], kLengths[i]);
    auto found = extra.find(std::string(key, key_length));
    check(found == extra.end() || found->second == term, "escape", "two terms share a key");
    extra[std::string(key, key_length)] = term;
  }
  char empty_capacity[1]{};
  size_t empty_length = 0;
  check(!SearchCacheKey("base", "a", 1, empty_capacity, 0, &empty_length), "cache key", "zero capacity");
  free(out);
  printf("token search units: %d accepted terms, %d refusals, %d distinct keys; %d JSON answers, %d protobuf twins, "
         "%d protobuf refusals; %d checks, %d failures\n",
         terms.accepted, terms.refused, terms.keys, renders.json, renders.protobuf, renders.refused, g_checks,
         g_failures);
  tj::FreeArena(a);
  free(text);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
