#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::auth {

// EIP-191 personal_sign recovery. Output is a lowercase 0x-prefixed address.
// The caller must validate message fields, timestamp, nonce, and wallet link.
bool evm_recover_address(const uint8_t* message, size_t message_length,
                         const char* signature, char address[43]);

void evm_keccak256(const uint8_t* data, size_t length, uint8_t digest[32]);

}  // namespace fortis::auth
