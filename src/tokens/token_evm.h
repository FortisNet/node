#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The TypeScript EVM driver's token reads for the token page (contractFacts /
// tokenMetadata / tokenInsight and the driver's ABI helpers):
// totalSupply, decimals, symbol and name in one Multicall3 aggregate3
// eth_call. Guest-safe.
namespace fortis::tokens::token_evm {

// The JSON-RPC request body for contractFacts(address): JSON.stringify of
// {jsonrpc:'2.0', id:1, method:'eth_call', params:[{to, data}, 'latest']}.
// False when `address` is not 0x + 40 hex (TypeScript then reads nothing).
bool ContractFactsRequest(const char* address, char* output, size_t capacity, size_t* length);

enum class FactsStatus : uint8_t { kFacts, kNone, kFailed };

// Interprets a parsed JSON-RPC response body. kFailed where TypeScript
// throws (an RPC error, no result, a non-string result, a malformed uint
// word); kNone where contractFacts returns null.
FactsStatus ContractFacts(codec::tj::Arena& arena, const codec::tj::Value* body, codec::tj::Value** facts);

// tokenMetadata(...) from contract facts: the chain metadata object, or
// JSON null when the contract stated nothing.
codec::tj::Value* TokenMetadata(codec::tj::Arena& arena, const codec::tj::Value* facts);
// tokenInsight(...) from contract facts: the section array (maybe empty).
codec::tj::Value* TokenInsight(codec::tj::Arena& arena, const codec::tj::Value* facts);

}  // namespace fortis::tokens::token_evm
