#include "ownership.h"
// Replays the TypeScript redactDiagnosticText corpus
// (tests/corpus/crash_redaction_corpus.json) through the native
// port and checks bounds the corpus does not reach.
#include <assert.h>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bounded_json.h"
#include "diagnostic_redaction.h"
#include "redaction_tables.h"

using namespace fortis::codec;
using namespace fortis::security;

namespace {

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file); free(data); return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

bool redact(const char* input, size_t length, std::unique_ptr<char[], fortis::base::StorageDeleter>* output, size_t* output_length) {
  return fortis::security::redaction::RedactDiagnosticText(input, length, output, output_length);
}

void expect(const char* input, const char* wanted) {
  std::unique_ptr<char[], fortis::base::StorageDeleter> output; size_t length = 0;
  assert(redact(input, strlen(input), &output, &length));
  if (strcmp(output.get(), wanted) != 0) {
    fprintf(stderr, "input:  %s\nwanted: %s\nactual: %s\n", input, wanted, output.get());
    assert(false);
  }
  output.reset();
}

double seconds_since(const timespec& start) {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<double>(now.tv_sec - start.tv_sec) +
         static_cast<double>(now.tv_nsec - start.tv_nsec) / 1e9;
}

// Worst-case shaped inputs at the crash report bound (24,000 UTF-16 units)
// must finish promptly and stay inside the output bound.
void check_bounds() {
  const size_t units = 24000;
  char* input = static_cast<char*>(malloc(units + 1));
  struct Shape { const char* unit; const char* name; };
  const Shape shapes[] = {
      {"1.1.1 ", "dotted digits (largest expansion)"},
      {"abandon ", "one long BIP-39 run"},
      {"-----BEGIN PRIVATE KEY----- ", "unterminated PEM headers"},
      {"{\"crypto\":{ ", "unterminated keystore objects"},
      {"<b> ", "markup separators"},
      {"aaaa", "one long word"},
  };
  for (const Shape& shape : shapes) {
    const size_t unit = strlen(shape.unit);
    size_t used = 0;
    while (used + unit <= units) { memcpy(input + used, shape.unit, unit); used += unit; }
    input[used] = '\0';
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    std::unique_ptr<char[], fortis::base::StorageDeleter> output; size_t length = 0;
    assert(redact(input, used, &output, &length));
    const double elapsed = seconds_since(start);
    printf("bound %-36s %6zu -> %7zu bytes in %.3f s\n", shape.name, used, length, elapsed);
    assert(elapsed < 5.0);
    assert(length < 4 * used + 64);
    output.reset();
  }
  free(input);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s crash_redaction_corpus.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* fixture = read_file(argv[1], &length);
  assert(fixture);
  static BoundedJsonToken tokens[kBoundedJsonMaxTokens];
  BoundedJsonDocument json;
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(json.Parse(reinterpret_cast<const unsigned char*>(fixture), length, tokens,
                    kBoundedJsonMaxTokens, &error));
  const uint32_t source = json.FindObjectValue(json.root(), "source");
  char unicode[32]; size_t unicode_length = 0;
  assert(json.DecodeString(json.FindObjectValue(source, "nodeUnicode"), unicode,
                           sizeof(unicode), &unicode_length));
  // The corpus and the generated NFKC tables must come from one Unicode version.
  assert(strcmp(unicode, kRedactionUnicodeVersion) == 0);
  const uint32_t cases = json.FindObjectValue(json.root(), "cases");
  assert(cases != kBoundedJsonNoToken);
  size_t count = 0, failures = 0;
  static char input[1 << 20], wanted[1 << 20];
  for (uint32_t item = json.token(cases)->first_child; item != kBoundedJsonNoToken;
       item = json.token(item)->next_sibling) {
    size_t input_length = 0, wanted_length = 0;
    assert(json.DecodeString(json.FindObjectValue(item, "input"), input, sizeof(input),
                             &input_length));
    assert(json.DecodeString(json.FindObjectValue(item, "output"), wanted, sizeof(wanted),
                             &wanted_length));
    std::unique_ptr<char[], fortis::base::StorageDeleter> output; size_t output_length = 0;
    assert(redact(input, input_length, &output, &output_length));
    if (output_length != wanted_length || memcmp(output.get(), wanted, wanted_length) != 0) {
      ++failures;
      fprintf(stderr, "case %zu mismatch\n  input:  %s\n  wanted: %s\n  actual: %s\n",
              count, input, wanted, output.get());
    }
    output.reset();
    ++count;
  }
  printf("redaction corpus: %zu cases, %zu mismatches\n", count, failures);
  assert(count >= 60 && failures == 0);

  // Invalid UTF-8 is refused rather than guessed at.
  std::unique_ptr<char[], fortis::base::StorageDeleter> output; size_t output_length = 0;
  assert(redact("previous", 8, &output, &output_length) && output);
  assert(redact(output.get(), output_length, &output, &output_length));
  assert(output_length == 8 && std::strcmp(output.get(), "previous") == 0);
  bool changed = false;
  assert(fortis::security::redaction::RedactSecrets(output.get(), output_length,
      &output, &output_length, &changed) && !changed);
  assert(output_length == 8 && std::strcmp(output.get(), "previous") == 0);
  assert(fortis::security::redaction::FoldForDetection(output.get(), output_length,
      &output, &output_length));
  assert(output_length == 8 && std::strcmp(output.get(), "previous") == 0);
  assert(!redact("\xff", 1, &output, &output_length));
  assert(!output && output_length == 0);
  assert(!redact("\xed\xa0\x80", 3, &output, &output_length));  // surrogate
  assert(!redact("\xc0\xaf", 2, &output, &output_length));      // overlong
  // NUL and other unstorable controls never survive.
  {
    const char text[] = "a\0b\x01" "c";
    assert(redact(text, sizeof(text) - 1, &output, &output_length));
    assert(output_length == 3 && memcmp(output.get(), "abc", 3) == 0);
    output.reset();
  }
  expect("", "");
  expect("fintel_dev_", "fintel_dev_");
  check_bounds();
  free(fixture);
  return 0;
}
