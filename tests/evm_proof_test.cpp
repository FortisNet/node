#include "../src/auth/evm_proof.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::auth;

int main() {
  // Independently generated with ethers Wallet.signMessage and hashMessage.
  constexpr char message[] = "FortisNet native proof";
  constexpr char signature[] =
      "0x16467bd18cd195a85b71883fc36e55c3248928f0e9113cf3f20c670be21c83f8"
      "1b331e42b83f7d524157cbef6611dfbf737c80657b0d7ab40e2127b12bbc6fe01c";
  uint8_t digest[32];
  evm_keccak256(reinterpret_cast<const uint8_t*>(""), 0, digest);
  constexpr uint8_t expected[32] = {
      0xc5, 0xd2, 0x46, 0x01, 0x86, 0xf7, 0x23, 0x3c,
      0x92, 0x7e, 0x7d, 0xb2, 0xdc, 0xc7, 0x03, 0xc0,
      0xe5, 0x00, 0xb6, 0x53, 0xca, 0x82, 0x27, 0x3b,
      0x7b, 0xfa, 0xd8, 0x04, 0x5d, 0x85, 0xa4, 0x70,
  };
  assert(memcmp(digest, expected, sizeof(digest)) == 0);
  char address[43];
  assert(evm_recover_address(reinterpret_cast<const uint8_t*>(message),
                             strlen(message), signature, address));
  assert(strcmp(address, "0xfcad0b19bb29d4674531d6f115237e16afce377c") == 0);
  char normalized_v[sizeof(signature)];
  memcpy(normalized_v, signature, sizeof(signature));
  normalized_v[130] = '0';
  normalized_v[131] = '1';
  assert(evm_recover_address(reinterpret_cast<const uint8_t*>(message),
                             strlen(message), normalized_v, address));
  assert(strcmp(address, "0xfcad0b19bb29d4674531d6f115237e16afce377c") == 0);
  char long_message[301];
  memset(long_message, 'x', 300);
  long_message[300] = '\0';
  constexpr char long_signature[] =
      "0x769e6e55407d78bb28f6b5d72256ac49f7108e2888b4958fc52c1361770be627"
      "41b2cdc1251a1c7923eee5084410f6bc363f8380a73937ebf2b810bc9694ecad1b";
  assert(evm_recover_address(reinterpret_cast<const uint8_t*>(long_message),
                             300, long_signature, address));
  assert(strcmp(address, "0xfcad0b19bb29d4674531d6f115237e16afce377c") == 0);
  char changed[sizeof(signature)];
  memcpy(changed, signature, sizeof(signature));
  changed[8] = changed[8] == '0' ? '1' : '0';
  assert(!evm_recover_address(reinterpret_cast<const uint8_t*>(message),
                              strlen(message), changed, address) ||
         strcmp(address, "0xfcad0b19bb29d4674531d6f115237e16afce377c") != 0);
  changed[8] = 'z';
  assert(!evm_recover_address(reinterpret_cast<const uint8_t*>(message),
                              strlen(message), changed, address));
  puts("EIP-191 secp256k1 recovery and Keccak vectors passed");
}
