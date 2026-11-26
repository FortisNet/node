#pragma once
#include "ownership.h"

#include <stddef.h>
#include <stdint.h>

// This server's diagnostic redaction: the secret scanner (detection folding,
// the pattern rules, bare 32-byte hex keys, BIP-39 runs) followed by the
// wallet-address, token and user-directory scrubbing applied to crash
// reports. The pattern rules are hand-written matchers with these semantics:
// \b and \w are ASCII, \s is the ECMAScript WhiteSpace and LineTerminator
// set, and context windows are measured in UTF-16 code units.
//
// Detection folds the text with Unicode NFKC first, from generated Unicode
// data (see redaction_tables.h; the tables record their Unicode version).
//
// Security-relevant (docs/compat-decisions.md#redaction): this decides what
// is stored and what is sent to the model provider. Never weaken a rule.
namespace fortis::security::redaction {

// Hard bound on the code points one call may produce. Crash report fields
// are at most 24,000 UTF-16 units; the largest expansion
// ("1.1.1 " -> "[REDACTED_ADDRESS] ") is about 3.2x.
constexpr size_t kMaxCodePoints = 1u << 20;

// Redacts valid UTF-8 `input`. On success `*output` owns a NUL-terminated
// UTF-8 buffer of `*output_length` bytes. Returns false
// for invalid UTF-8, the size bound, or allocation failure.
bool RedactDiagnosticText(const char* input, size_t length, std::unique_ptr<char[], base::StorageDeleter>* output,
                          size_t* output_length);

// The secret scanner alone, as the AI conversation
// inbound path stores user text. `*redacted` is RedactionResult.redacted
// (some rule fired). Same ownership and failure contract as above.
bool RedactSecrets(const char* input, size_t length, std::unique_ptr<char[], base::StorageDeleter>* output, size_t* output_length,
                   bool* redacted);

// The detection folding (NFKC, invisibles and unstorable controls removed,
// short tags to spaces, confusables mapped). Same contract as above.
bool FoldForDetection(const char* input, size_t length, std::unique_ptr<char[], base::StorageDeleter>* output, size_t* output_length);

}  // namespace fortis::security::redaction
