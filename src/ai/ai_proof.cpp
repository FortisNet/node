#include "utf8.h"
#include "ai_proof.h"

#include "evm_proof.h"
#include "js_case.h"

#include <mbedtls/sha256.h>

#include <stdio.h>
#include <string.h>

namespace fortis::ai {
namespace {

constexpr char kHex[] = "0123456789abcdef";

void to_hex(const uint8_t* bytes, size_t length, char* output) {
  for (size_t i = 0; i < length; ++i) {
    output[2 * i] = kHex[bytes[i] >> 4];
    output[2 * i + 1] = kHex[bytes[i] & 15];
  }
  output[2 * length] = '\0';
}

int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool digit(char c) { return c >= '0' && c <= '9'; }

// Two ASCII digits as a number, or -1.
int two(const char* text) { return digit(text[0]) && digit(text[1]) ? (text[0] - '0') * 10 + (text[1] - '0') : -1; }

// Copies WTF-8 `text`, writing each lone surrogate (ED A0..BF xx) as U+FFFD,
// as Node's UTF-8 encoder does for a string holding one.
bool copy_utf8(const char* text, size_t length, char* output, size_t capacity,
                             size_t* output_length) {
  *output_length = codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, output, capacity);
  return *output_length <= capacity;
}

// ASCII lower-casing of a WTF-8 address, lone surrogates as U+FFFD. For
// every address that can own a conversation (0x + 40 hex) this equals the
// Unicode mapping the owner ids were first derived with: no non-ASCII
// character lower-cases to a hex digit or `x`.
bool lower_wtf8(const char* text, size_t length, char* output, size_t capacity, size_t* output_length) {
  char valid[4096];
  size_t valid_length = 0;
  return copy_utf8(text, length, valid, sizeof(valid), &valid_length) &&
         compat::AsciiToLowerCase(valid, valid_length, output, capacity, output_length);
}

}  // namespace

void Sha256Hex(const void* data, size_t length, char output[kHashHexBytes]) {
  uint8_t digest[32];
  mbedtls_sha256(static_cast<const uint8_t*>(data), length, digest, 0);
  to_hex(digest, sizeof(digest), output);
}

void HmacSha256(const void* key, size_t key_length, const void* message, size_t message_length,
                uint8_t output[32]) {
  uint8_t block[64]{};
  if (key_length > sizeof(block)) mbedtls_sha256(static_cast<const uint8_t*>(key), key_length, block, 0);
  else if (key_length) memcpy(block, key, key_length);
  uint8_t pad[64];
  uint8_t inner[32];
  mbedtls_sha256_context context;
  for (size_t i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x36;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(&context, pad, sizeof(pad));
  mbedtls_sha256_update(&context, static_cast<const uint8_t*>(message), message_length);
  mbedtls_sha256_finish(&context, inner);
  for (size_t i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x5c;
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(&context, pad, sizeof(pad));
  mbedtls_sha256_update(&context, inner, sizeof(inner));
  mbedtls_sha256_finish(&context, output);
  mbedtls_sha256_free(&context);
  memset(block, 0, sizeof(block));
  memset(pad, 0, sizeof(pad));
}

void HmacSha256Hex(const void* key, size_t key_length, const void* message, size_t message_length,
                   char output[kHashHexBytes]) {
  uint8_t digest[32];
  HmacSha256(key, key_length, message, message_length, digest);
  to_hex(digest, sizeof(digest), output);
}

bool OwnerId(const char* secret, size_t secret_length, const char* address, size_t address_length,
             char output[kHashHexBytes]) {
  static const char kPrefix[] = "intelligence-owner:";
  char message[sizeof(kPrefix) + 4096];
  memcpy(message, kPrefix, sizeof(kPrefix) - 1);
  size_t lower_length = 0;
  if (!lower_wtf8(address, address_length, message + sizeof(kPrefix) - 1, 4096, &lower_length)) return false;
  HmacSha256Hex(secret, secret_length, message, sizeof(kPrefix) - 1 + lower_length, output);
  return true;
}

bool LowercaseEquals(const char* address, size_t address_length, const char* lower_ascii) {
  char lower[4096];
  size_t lower_length = 0;
  return lower_wtf8(address, address_length, lower, sizeof(lower), &lower_length) &&
         lower_length == strlen(lower_ascii) && memcmp(lower, lower_ascii, lower_length) == 0;
}

bool ChecksumAddress(const char* address, char output[kAddressBytes]) {
  if (!address || strnlen(address, 43) != 42 || address[0] != '0' || address[1] != 'x') return false;
  char lower[41];
  bool has_upper = false, has_lower = false;
  for (size_t i = 0; i < 40; ++i) {
    const char c = address[2 + i];
    if (nibble(c) < 0) return false;
    has_upper |= c >= 'A' && c <= 'F';
    has_lower |= c >= 'a' && c <= 'f';
    lower[i] = c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
  }
  lower[40] = '\0';
  uint8_t digest[32];
  auth::evm_keccak256(reinterpret_cast<const uint8_t*>(lower), 40, digest);
  output[0] = '0';
  output[1] = 'x';
  for (size_t i = 0; i < 40; ++i) {
    const char c = lower[i];
    const unsigned bits = i % 2 ? digest[i / 2] & 15 : digest[i / 2] >> 4;
    output[2 + i] = c >= 'a' && c <= 'f' && bits >= 8 ? static_cast<char>(c - 'a' + 'A') : c;
  }
  output[42] = '\0';
  // ethers: /([A-F].*[a-f])|([a-f].*[A-F])/ (mixed case) must checksum exactly.
  return !(has_upper && has_lower) || memcmp(output, address, 42) == 0;
}

bool VerifyMessage(const char* message, size_t message_length, const char* signature,
                   char output[kAddressBytes]) {
  if (!signature || strnlen(signature, 133) != 132 || signature[0] != '0' || signature[1] != 'x') return false;
  for (size_t i = 2; i < 132; ++i)
    if (nibble(signature[i]) < 0) return false;
  // Signature.from(): v normalization, then the `s` getter's canonical check.
  const unsigned v = static_cast<unsigned>(nibble(signature[130]) * 16 + nibble(signature[131]));
  unsigned normalized = 0;
  if (v == 0 || v == 27) normalized = 27;
  else if (v == 1 || v == 28) normalized = 28;
  else if (v >= 35) normalized = v & 1 ? 27 : 28;
  else return false;
  if (nibble(signature[66]) >= 8) return false;
  char canonical[133];
  memcpy(canonical, signature, 130);
  canonical[130] = '1';
  canonical[131] = normalized == 27 ? 'b' : 'c';
  canonical[132] = '\0';
  char recovered[43];
  if (!auth::evm_recover_address(reinterpret_cast<const uint8_t*>(message), message_length, canonical, recovered))
    return false;
  return ChecksumAddress(recovered, output);
}

size_t ProofMessage(const char* address, const char* device_id, const char* nonce,
                    const char* timestamp, const char* snapshot_hash,
                    const char* access_token_hash, char* output, size_t capacity) {
  const int written = snprintf(output, capacity,
      "FortisNet Intelligence conversation v1\nAddress: %s\nDevice: %s\nNonce: %s\nTimestamp: %s\n"
      "Snapshot SHA-256: %s\nSession token SHA-256: %s",
      address, device_id, nonce, timestamp, snapshot_hash, access_token_hash);
  return written > 0 && static_cast<size_t>(written) < capacity ? static_cast<size_t>(written) : 0;
}

bool IsZodDatetime(const char* text, size_t length) {
  // date: \d{4}-MM-DD, 'T' at 10, hh:mm at 11..15.
  if (!text || length < 17) return false;
  for (size_t i = 0; i < 4; ++i)
    if (!digit(text[i])) return false;
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':') return false;
  const int month = two(text + 5), day = two(text + 8);
  if (month < 1 || month > 12 || day < 1) return false;
  const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (day > kDays[month - 1] && !(month == 2 && day == 29 && leap)) return false;
  const int hour = two(text + 11), minute = two(text + 14);
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
  size_t i = 16;
  if (i < length && text[i] == ':') {
    if (i + 3 > length) return false;
    const int second = two(text + i + 1);
    if (second < 0 || second > 59) return false;
    i += 3;
    if (i < length && text[i] == '.') {
      ++i;
      const size_t start = i;
      while (i < length && digit(text[i])) ++i;
      if (i == start) return false;
    }
  }
  if (i < length && text[i] == 'Z') return i + 1 == length;
  if (i < length && (text[i] == '+' || text[i] == '-')) {
    if (i + 6 != length || text[i + 3] != ':') return false;
    const int offset_hour = two(text + i + 1), offset_minute = two(text + i + 4);
    return offset_hour >= 0 && offset_hour <= 23 && offset_minute >= 0 && offset_minute <= 59;
  }
  return false;
}

}  // namespace fortis::ai
