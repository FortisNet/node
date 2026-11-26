#pragma once

#include "token_json.h"
#include "token_ref.h"

#include <stddef.h>
#include <stdint.h>

// Pure ports for the token page `contract` section: the TypeScript
// GoPlus client (chain routing, URL, response classification and
// pickToken) and the TypeScript normalizeContractSecurity. The
// output object has exactly the TypeScript key order (the `empty()`
// literal). Guest-safe.
namespace fortis::tokens::token_contract {

// goPlusChainId(network) for the catalogued network; null when GoPlus does
// not index it. `evm_chain_id` is 0 when the network has none.
const char* GoPlusChainId(const networks::TokenNetwork& network, uint32_t evm_chain_id, char buffer[16]);

// The request path (with query) for tokenSecurity(chain, address), false
// when the chain has no dialect or the path does not fit.
bool GoPlusPath(const char* chain, const char* address, char* output, size_t capacity);

enum class GoPlusStatus : uint8_t { kRow, kAbsent, kFailed };

// GoPlusClient.tokenSecurity over a parsed 200 body: kRow with *token set,
// kAbsent (a cacheable null: no row, or a documented coverage code), or
// kFailed where TypeScript throws.
GoPlusStatus ClassifyGoPlusBody(const codec::tj::Value* body, const char* address, const codec::tj::Value** token);

// normalizeContractSecurity({dialect, chain, token}, address, now). Null on
// arena exhaustion (the normalizer itself never returns null).
codec::tj::Value* NormalizeContractSecurity(codec::tj::Arena& arena, const codec::tj::Value* token, const char* chain,
                                     const char* address, int64_t now_ms);

// The TypeScript isBurnAddress.
bool IsBurnAddress(codec::tj::Arena& arena, const char* address, size_t length);

}  // namespace fortis::tokens::token_contract
