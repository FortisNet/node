#pragma once

#include <stddef.h>
#include <stdint.h>

// The ownership boundary of signed AI conversations, ported from the
// earlier TypeScript implementation:
// the seven-line proof message, SHA-256 token/snapshot hashes, the
// pseudonymous owner id, ethers' getAddress and verifyMessage, and zod's
// datetime({ offset: true }) format. Guest-safe; nothing here logs.
namespace fortis::ai {

constexpr size_t kHashHexBytes = 65;     // 64 hex digits + NUL
constexpr size_t kAddressBytes = 43;     // 0x + 40 hex digits + NUL

// hashToken: SHA-256 of the UTF-8 bytes, lowercase hex.
void Sha256Hex(const void* data, size_t length, char output[kHashHexBytes]);

// HMAC-SHA256(key, message): the raw digest, and as lowercase hex.
void HmacSha256(const void* key, size_t key_length, const void* message, size_t message_length,
                uint8_t output[32]);
void HmacSha256Hex(const void* key, size_t key_length, const void* message, size_t message_length,
                   char output[kHashHexBytes]);

// intelligenceOwnerId(secret, address): HMAC-SHA256(secret,
// `intelligence-owner:${lower(address)}`). `address` is WTF-8 (a lone
// surrogate encodes as U+FFFD); the lower-casing is ASCII only. False when the address does not fit
// the bounded workspace (snapshot addresses are at most 256 UTF-16 units).
bool OwnerId(const char* secret, size_t secret_length, const char* address, size_t address_length,
             char output[kHashHexBytes]);

// The ASCII lower-cased address equals `lower_ascii`.
bool LowercaseEquals(const char* address, size_t address_length, const char* lower_ascii);

// ethers getAddress() of a string already matching /^0x[0-9a-fA-F]{40}$/:
// the EIP-55 checksum form, or false for a mixed-case input whose checksum
// is wrong.
bool ChecksumAddress(const char* address, char output[kAddressBytes]);

// ethers verifyMessage(message, signature) for a /^0x[0-9a-fA-F]{130}$/
// signature, as a checksummed address. ethers' rules on top of the shared
// recovery primitive: v 0/27 and 1/28, any v >= 35 by parity (EIP-155), any
// other v rejected; s must have its top bit clear ("non-canonical s").
bool VerifyMessage(const char* message, size_t message_length, const char* signature,
                   char output[kAddressBytes]);

// intelligenceProofMessage(...): the seven lines joined by "\n". Returns the
// length written (excluding the NUL), or 0 when it does not fit.
size_t ProofMessage(const char* address, const char* device_id, const char* nonce,
                    const char* timestamp, const char* snapshot_hash,
                    const char* access_token_hash, char* output, size_t capacity);

// z.string().datetime({ offset: true }) (zod 4.4 regex): date with the
// Gregorian leap rule, 'T', hh:mm[:ss[.fraction]], then 'Z' or ±hh:mm.
bool IsZodDatetime(const char* text, size_t length);

}  // namespace fortis::ai
