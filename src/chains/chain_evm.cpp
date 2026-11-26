// The EVM family of the address, transaction and network routes:
//  - kEvmDriver: the TypeScript EvmDriver (balances pinned to one block
//    with the parent-block tip-race retry, Multicall3 token balances with the
//    sequential fallback, insight, latestBlockHeight, txStatus and the keyed
//    Alchemy NFT inventory);
//  - kEvmSource: the TypeScript FailoverTransferSource('evm',
//    'etherscan', [EtherscanTransferSource, AlchemyTransferSource],
//    'etherscan') over ports of the two transfer sources.
// Guest-safe: no C++ standard-library headers.
#include "chain_family_drivers.h"
#include "threading.h"

#include "token_text.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

using tj::Type;

constexpr size_t kMaxTokens = 200;
constexpr char kEtherscanBase[] = "https://api.etherscan.io/v2/api";
constexpr char kZeroAddress[] = "0x0000000000000000000000000000000000000000";
constexpr char kDeadAddress[] = "0x000000000000000000000000000000000000dead";

struct Slug {
  const char* network;
  const char* slug;
};
// EvmDriver ALCHEMY_NFT_SLUGS.
constexpr Slug kNftSlugs[] = {
    {"eth-mainnet", "eth-mainnet"}, {"base", "base-mainnet"},
    {"polygon", "polygon-mainnet"}, {"bnb", "bnb-mainnet"},
};
// The Alchemy transfer source's ALCHEMY_SLUGS.
constexpr Slug kTransferSlugs[] = {
    {"eth-mainnet", "eth-mainnet"}, {"base", "base-mainnet"},   {"polygon", "polygon-mainnet"},
    {"bnb", "bnb-mainnet"},         {"arbitrum", "arb-mainnet"}, {"optimism", "opt-mainnet"},
};

template <size_t N>
const char* slug_for(const Slug (&table)[N], const char* network) {
  for (const Slug& entry : table)
    if (strcmp(entry.network, network) == 0) return entry.slug;
  return nullptr;
}

// ---- value helpers -------------------------------------------------------------------

bool nullish(const Value* v) { return !v || v->type == Type::kNull; }
bool is_string(const Value* v) { return v && v->type == Type::kString; }
bool is_object(const Value* v) { return v && v->type == Type::kObject; }

// JavaScript truthiness of a member (absent = undefined = false).
bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case Type::kNull: case Type::kFalse: return false;
    case Type::kNumber: return v->number != 0 && !isnan(v->number);
    case Type::kString: return v->length != 0;
    default: return true;
  }
}

bool equals(const Value* v, const char* text) {
  const size_t length = strlen(text);
  return is_string(v) && v->length == length && memcmp(v->text, text, length) == 0;
}

bool digits(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// /^\d+$/.test(value ?? '') for a string member (providers send strings).
bool digit_string(const Value* v) { return is_string(v) && digits(v->text, v->length); }

bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// /^0x[0-9a-fA-F]{40}$/
bool evm_address(const char* text, size_t length) {
  if (length != 42 || text[0] != '0' || text[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i)
    if (!is_hex(text[i])) return false;
  return true;
}

// ASCII case-insensitive substring search (the case-insensitive regexes).
bool contains_ci(const char* text, size_t length, const char* needle) {
  const size_t n = strlen(needle);
  if (n > length) return false;
  for (size_t i = 0; i + n <= length; ++i) {
    size_t j = 0;
    while (j < n) {
      char a = text[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) break;
      ++j;
    }
    if (j == n) return true;
  }
  return false;
}

// (value ?? '').toLowerCase(): false where JavaScript throws (a non-string).
bool lower_or_empty(Arena& a, const Value* v, Value** out) {
  if (nullish(v)) { *out = tj::Str(a, "", 0); return *out != nullptr; }
  if (!is_string(v)) return false;
  *out = compat::token_text::ToLowerCase(a, v->text, v->length);
  return *out != nullptr;
}

bool same(const Value* a, const char* text) { return equals(a, text); }

// `x ?? null` for a member.
Value* or_null(Arena& a, const Value* v) { return nullish(v) ? tj::Null(a) : const_cast<Value*>(v); }

// `0x${value.toString(16)}` for a non-negative integral double.
const char* number_hex(Arena& a, double value) {
  BigInt big;
  char buffer[200];
  if (!BigInt::FromDouble(value, &big) || !big.ToHex(buffer, sizeof(buffer))) return nullptr;
  char out[204];
  snprintf(out, sizeof(out), "0x%s", buffer);
  return a.CopyText(out, strlen(out));
}

Value* empty_array(Arena& a) { return tj::Arr(a); }

// One JSON-RPC call over the network's endpoints.
void rpc(Call& call, const Network& network, const char* method, const Value* params) {
  call = Call{};
  call.kind = CallKind::kJsonRpc;
  call.urls = network.rpc_urls;
  call.url_count = network.rpc_url_count;
  call.method = method;
  call.params = params;
}

// DriverTransport.withFailover's RangeError for an empty endpoint list.
bool endpoints(const Network& network, Error* error) {
  if (network.rpc_url_count) return true;
  error->Range("no endpoints configured");
  return false;
}

bool oom(Error* error) {
  error->Upstream("arena exhausted");
  return false;
}

// isTipPropagationRace: an UpstreamAnswerError whose message matches
// /(?:requested height .*not yet available|safe latest|block height not yet available)/i.
bool tip_race(const Call& call) {
  if (!call.answer()) return false;
  const char* m = call.message;
  const size_t length = strlen(m);
  if (contains_ci(m, length, "safe latest") || contains_ci(m, length, "block height not yet available"))
    return true;
  static const char kHead[] = "requested height ";
  const size_t head = sizeof(kHead) - 1;
  for (size_t i = 0; i + head <= length; ++i) {
    if (!contains_ci(m + i, head, kHead)) continue;
    if (contains_ci(m + i + head, length - i - head, "not yet available")) return true;
  }
  return false;
}

// `rpc:${new URL(rpcUrls[0]).host}`.
const char* rpc_source(Arena& a, const Network& network) {
  char host[300];
  if (!UrlHost(network.rpc_urls[0], host, sizeof(host))) return nullptr;
  char out[310];
  snprintf(out, sizeof(out), "rpc:%s", host);
  return a.CopyText(out, strlen(out));
}

// {to, data} eth_call parameters at `block_tag`.
Value* eth_call_params(Arena& a, const char* to, const char* data, const char* block_tag) {
  Value* call = tj::Obj(a);
  Value* params = tj::Arr(a);
  if (!call || !params || !tj::Set(a, call, "to", tj::Str(a, to)) ||
      !tj::Set(a, call, "data", tj::Str(a, data)) || !tj::Push(a, params, call) ||
      !tj::Push(a, params, tj::Str(a, block_tag)))
    return nullptr;
  return params;
}

Value* string_params(Arena& a, const char* first, const char* second) {
  Value* params = tj::Arr(a);
  if (!params || !tj::Push(a, params, tj::Str(a, first))) return nullptr;
  if (second && !tj::Push(a, params, tj::Str(a, second))) return nullptr;
  return params;
}

// ---- balances ----------------------------------------------------------------------------

struct Contract {
  const char* address;  // the 0x... part of the tracked token id, as given
};

// A failed balanceAt step: `race` reports isTipPropagationRace.
struct Step {
  bool race = false;
};

bool token_balance_entry(Arena& a, const char* contract, const BigInt& raw, double decimals,
                         Value* symbol, const char* source, Value* tokens) {
  Value* entry = tj::Obj(a);
  Value* lowered = compat::token_text::ToLowerCase(a, contract, strlen(contract));
  if (!entry || !lowered) return false;
  Value* token_id = Format(a, "erc20:%.*s", static_cast<int>(lowered->length), lowered->text);
  return token_id && tj::Set(a, entry, "tokenId", token_id) &&
         tj::Set(a, entry, "balanceRaw", raw.ToValue(a)) &&
         tj::Set(a, entry, "decimals", tj::Num(a, decimals)) && tj::Set(a, entry, "symbol", symbol) &&
         tj::Set(a, entry, "source", tj::Str(a, source)) && tj::Push(a, tokens, entry);
}

// tokenBalancesSequential: one balanceOf eth_call per contract; any failure
// omits the token (unknown, never zero).
bool tokens_sequential(Context& ctx, const Network& network, const char* holder, const Contract* contracts,
                       size_t count, const char* block_tag, const char* source, Value* tokens,
                       Error* error) {
  Arena& a = *ctx.arena;
  for (size_t i = 0; i < count; ++i) {
    Value* params = eth_call_params(a, contracts[i].address, AbiBalanceOf(a, holder), block_tag);
    if (!params) return oom(error);
    Call call;
    rpc(call, network, "eth_call", params);
    ctx.transport->Run(call, a);
    if (!call.ok() || !is_string(call.value)) continue;
    const BigInt raw = AbiUint256(call.value->text, call.value->length);
    if (!token_balance_entry(a, contracts[i].address, raw, 18, tj::Null(a), source, tokens))
      return oom(error);
  }
  return true;
}

// tokenBalances: one aggregate3 with [balanceOf, decimals, symbol] per token.
bool tokens_multicall(Context& ctx, const Network& network, const char* holder, const Contract* contracts,
                      size_t count, const char* block_tag, Value* tokens, Step* step, Error* error) {
  Arena& a = *ctx.arena;
  auto* calls = static_cast<AbiCall3*>(a.Alloc(sizeof(AbiCall3) * count * 3));
  if (!calls) return oom(error);
  const char* decimals = AbiDecimals(a);
  const char* symbol = AbiSymbol(a);
  for (size_t i = 0; i < count; ++i) {
    calls[i * 3] = AbiCall3{contracts[i].address, AbiBalanceOf(a, holder)};
    calls[i * 3 + 1] = AbiCall3{contracts[i].address, decimals};
    calls[i * 3 + 2] = AbiCall3{contracts[i].address, symbol};
  }
  const char* data = AbiAggregate3(a, calls, count * 3);
  Value* params = data ? eth_call_params(a, kMulticall3Address, data, block_tag) : nullptr;
  if (!params) return oom(error);
  Call call;
  rpc(call, network, "eth_call", params);
  ctx.transport->Run(call, a);
  if (!call.ok()) {
    step->race = tip_race(call);
    call.ToError(error);
    return false;
  }
  // decodeAggregate3(result) calls result.trim(): a non-string result throws.
  if (!is_string(call.value)) {
    error->Upstream("eth_call: result is not a string");
    return false;
  }
  const char* source = rpc_source(a, network);
  if (!source) {
    error->Upstream("invalid RPC URL");
    return false;
  }
  AbiResult* results = nullptr;
  size_t decoded = 0;
  if (!AbiDecodeAggregate3(a, call.value->text, call.value->length, &results, &decoded) ||
      decoded != count * 3)
    return tokens_sequential(ctx, network, holder, contracts, count, block_tag, source, tokens, error);
  for (size_t i = 0; i < count; ++i) {
    const AbiResult& balance = results[i * 3];
    const AbiResult& dec = results[i * 3 + 1];
    const AbiResult& sym = results[i * 3 + 2];
    if (!balance.success) continue;  // unknown, never zero
    Value* raw_symbol = sym.success ? AbiString(a, sym.data, sym.length) : tj::Null(a);
    if (!raw_symbol) return oom(error);
    Value* symbol_value = tj::Null(a);
    if (raw_symbol->type == Type::kString && raw_symbol->length)
      symbol_value = SanitizeSymbol(a, raw_symbol->text, raw_symbol->length);
    const double decimals_value = dec.success ? AbiUint256(dec.data, dec.length).ToDouble() : 18;
    if (!symbol_value ||
        !token_balance_entry(a, contracts[i].address, AbiUint256(balance.data, balance.length),
                             decimals_value, symbol_value, source, tokens))
      return oom(error);
  }
  return true;
}

// balanceAt: eth_getBalance, then the token reads, at one block.
bool balance_at(Context& ctx, const Network& network, const char* holder, const Contract* contracts,
                size_t count, const char* block_tag, Value** balance_hex, Value** tokens, Step* step,
                Error* error) {
  Arena& a = *ctx.arena;
  Value* params = string_params(a, holder, block_tag);
  if (!params) return oom(error);
  Call call;
  rpc(call, network, "eth_getBalance", params);
  ctx.transport->Run(call, a);
  if (!call.ok()) {
    step->race = tip_race(call);
    call.ToError(error);
    return false;
  }
  *balance_hex = call.value;
  *tokens = tj::Arr(a);
  if (!*tokens) return oom(error);
  if (!count) return true;
  return tokens_multicall(ctx, network, holder, contracts, count, block_tag, *tokens, step, error);
}

bool parse_bigint(const Value* v, BigInt* out) {
  return is_string(v) && BigInt::Parse(v->text, v->length, out);
}

}  // namespace

bool EvmDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const* tokens,
                  size_t token_count, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!evm_address(address, strlen(address))) {
    error->Range("not an EVM address: %s", address);
    return false;
  }
  if (!network.rpc_url_count) {
    error->Range("no RPC configured for %s", network.id);
    return false;
  }
  const char* native_symbol = network.has_native ? network.native_symbol : "";
  const double native_decimals = network.has_native ? network.native_decimals : 18;

  Contract* contracts = static_cast<Contract*>(a.Alloc(sizeof(Contract) * (kMaxTokens + 1)));
  if (!contracts) return oom(error);
  size_t count = 0;
  for (size_t i = 0; i < token_count && count < kMaxTokens; ++i) {
    const char* token = tokens[i];
    if (strncmp(token, "erc20:", 6) != 0 || !evm_address(token + 6, strlen(token + 6))) continue;
    contracts[count++] = Contract{token + 6};
  }

  Call head;
  Value* no_params = empty_array(a);
  if (!no_params) return oom(error);
  rpc(head, network, "eth_blockNumber", no_params);
  ctx.transport->Run(head, a);
  if (!head.ok()) { head.ToError(error); return false; }
  BigInt block;
  if (!parse_bigint(head.value, &block)) {
    error->Upstream("eth_blockNumber: not a quantity");
    return false;
  }
  const char* block_tag = tj::CString(a, head.value);
  Value* balance_hex = nullptr;
  Value* token_list = nullptr;
  Step step;
  if (!block_tag) return oom(error);
  if (!balance_at(ctx, network, address, contracts, count, block_tag, &balance_hex, &token_list, &step,
                  error)) {
    if (!step.race || block.IsZero()) return false;
    // Fast-finality gateways can publish eth_blockNumber one slot before their
    // state serves it: retry the whole snapshot at the parent block.
    error->Clear();
    BigInt parent = block;
    if (!parent.Sub(BigInt::FromU64(1))) return oom(error);
    char hex[200];
    char tag[204];
    if (!parent.ToHex(hex, sizeof(hex))) return oom(error);
    snprintf(tag, sizeof(tag), "0x%s", hex);
    block_tag = a.CopyText(tag, strlen(tag));
    block = parent;
    Step retry;
    if (!block_tag ||
        !balance_at(ctx, network, address, contracts, count, block_tag, &balance_hex, &token_list, &retry,
                    error))
      return block_tag ? false : oom(error);
  }
  BigInt native;
  if (!parse_bigint(balance_hex, &native)) {
    error->Upstream("eth_getBalance: not a quantity");
    return false;
  }
  const char* source = rpc_source(a, network);
  if (!source) {
    error->Upstream("invalid RPC URL");
    return false;
  }
  Value* out = tj::Obj(a);
  Value* native_entry = tj::Obj(a);
  Value* reference = tj::Obj(a);
  if (!out || !native_entry || !reference || !tj::Set(a, native_entry, "balanceRaw", native.ToValue(a)) ||
      !tj::Set(a, native_entry, "decimals", tj::Num(a, native_decimals)) ||
      !tj::Set(a, native_entry, "symbol", tj::Str(a, native_symbol)) ||
      !tj::Set(a, native_entry, "source", tj::Str(a, source)) || !tj::Set(a, out, "native", native_entry) ||
      !tj::Set(a, out, "tokens", token_list) || !tj::Set(a, reference, "kind", tj::Str(a, "block")) ||
      !tj::Set(a, reference, "value", block.ToValue(a)) || !tj::Set(a, out, "reference", reference) ||
      !tj::Set(a, out, "finality", tj::Str(a, "latest")))
    return oom(error);
  *result = out;
  return true;
}

namespace {

// ---- insight / height / status ---------------------------------------------------------

}  // namespace

bool EvmDriver::Insight(Context& ctx, const Network& network, const char* address, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!evm_address(address, strlen(address))) {
    error->Range("not an EVM address: %s", address);
    return false;
  }
  if (!endpoints(network, error)) return false;
  Call calls[2];
  Value* code_params = string_params(a, address, "latest");
  Value* nonce_params = string_params(a, address, "latest");
  if (!code_params || !nonce_params) return oom(error);
  rpc(calls[0], network, "eth_getCode", code_params);
  rpc(calls[1], network, "eth_getTransactionCount", nonce_params);
  ctx.transport->Run(calls, 2, a);
  // Promise.all rejects with the call that failed first.
  if (const Call* failed = FirstSettledFailure(calls, 2)) {
    failed->ToError(error);
    return false;
  }
  double tx_count = 0;
  if (!BigIntNumber(a, calls[1].value, &tx_count, error)) {
    // A TypeError/SyntaxError is a failed read; BigInt's RangeError is the
    // route's 400 with its message.
    if (error->kind != ErrorKind::kRange) error->Upstream("eth_getTransactionCount: not a BigInt");
    return false;
  }
  // code !== '0x' && code !== '0x0' (a non-string code compares unequal).
  const bool contract = !equals(calls[0].value, "0x") && !equals(calls[0].value, "0x0");
  Value* out = tj::Obj(a);
  if (!out || !tj::Set(a, out, "exists", tj::Bool(a, contract || tx_count > 0)) ||
      !tj::Set(a, out, "kind", tj::Str(a, contract ? "contract" : "eoa")) ||
      !tj::Set(a, out, "txCount", tj::Num(a, tx_count)) ||
      !tj::Set(a, out, "txCountFloor", tj::Bool(a, false)))
    return oom(error);
  *result = out;
  return true;
}

bool EvmDriver::LatestHeight(Context& ctx, const Network& network, bool* has_height, double* height,
                       Error* error) const {
  *has_height = false;
  if (!network.rpc_url_count) return true;
  Arena& a = *ctx.arena;
  Value* params = empty_array(a);
  if (!params) return oom(error);
  Call call;
  rpc(call, network, "eth_blockNumber", params);
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  // Number(BigInt(result)): a hex or decimal string, an integral number, a
  // boolean or a one-element array; null, a fraction or junk throw (the
  // transfers read then falls back to one confirmation).
  double number = 0;
  if (!BigIntNumber(a, call.value, &number, error)) return false;
  if (number >= 0 && number <= 9007199254740991.0 && floor(number) == number) {
    *has_height = true;
    *height = number;
  }
  return true;
}

bool EvmDriver::TxStatus(Context& ctx, const Network& network, const char* hash, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!endpoints(network, error)) return false;
  Value* params = string_params(a, hash, nullptr);
  if (!params) return oom(error);
  Call call;
  rpc(call, network, "eth_getTransactionByHash", params);
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  Value* tx = call.value;
  if (nullish(tx)) { *result = nullptr; return true; }  // unknown to the chain (yet)
  if (nullish(tj::Get(tx, "blockNumber"))) return EvmTxStatusOf(a, network, tx, nullptr, nullptr, result, error);
  Call calls[2];
  Value* receipt_params = string_params(a, hash, nullptr);
  Value* tip_params = empty_array(a);
  if (!receipt_params || !tip_params) return oom(error);
  rpc(calls[0], network, "eth_getTransactionReceipt", receipt_params);
  rpc(calls[1], network, "eth_blockNumber", tip_params);
  ctx.transport->Run(calls, 2, a);
  // Promise.all: the call that failed first decides.
  if (const Call* failed = FirstSettledFailure(calls, 2)) { failed->ToError(error); return false; }
  return EvmTxStatusOf(a, network, tx, calls[0].value, calls[1].value, result, error);
}

namespace {

// ---- Alchemy NFTs ----------------------------------------------------------------------

// sanitizeName(value) for a truthy value; false where JavaScript throws.
bool truthy_name(Arena& a, const Value* v, Value** out) {
  if (!truthy(v)) { *out = tj::Null(a); return *out != nullptr; }
  if (!is_string(v)) return false;
  *out = SanitizeName(a, v->text, v->length);
  return *out != nullptr;
}

}  // namespace

bool EvmDriver::ReadNfts(Context& ctx, const Network& network, const char* address, Value** items, Error* error) const {
  Arena& a = *ctx.arena;
  if (!evm_address(address, strlen(address))) {
    error->Range("not an EVM address: %s", address);
    return false;
  }
  const char* slug = slug_for(kNftSlugs, network.id);
  if (!slug) {
    error->Range("no Alchemy NFT coverage for %s", network.id);
    return false;
  }
  char base[128];
  snprintf(base, sizeof(base), "https://%s.g.alchemy.com", slug);
  char path[1024];
  snprintf(path, sizeof(path), "/nft/v3/%s/getNFTsForOwner?owner=%s&withMetadata=true&pageSize=100",
           ctx.config->alchemy_api_key, address);
  const char* urls[1] = {base};
  Call call;
  call.kind = CallKind::kGet;
  call.urls = urls;
  call.url_count = 1;
  call.path = path;
  call.lane = "alchemy";
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  if (!call.value || call.value->type == Type::kNull) {
    // `body.ownedNfts` on a JSON null body throws.
    error->Upstream("Cannot read properties of null (reading 'ownedNfts')");
    return false;
  }
  Value* out = tj::Arr(a);
  if (!out) return oom(error);
  const Value* owned = tj::Get(call.value, "ownedNfts");
  if (nullish(owned) || is_string(owned)) {
    // `?? []`; a string iterates characters that carry no NFT fields.
    *items = out;
    return true;
  }
  if (owned->type != Type::kArray) {
    error->Upstream("ownedNfts is not iterable");
    return false;
  }
  for (uint32_t i = 0; i < owned->count; ++i) {
    const Value* nft = owned->items[i];
    if (nft->type == Type::kNull) {
      error->Upstream("ownedNfts entry is null");
      return false;
    }
    const Value* contract_object = tj::Get(nft, "contract");
    const Value* address_value = tj::Get(contract_object, "address");
    Value* contract = nullptr;
    if (!nullish(address_value)) {
      if (!is_string(address_value)) {
        error->Upstream("contract.address is not a string");
        return false;
      }
      contract = compat::token_text::ToLowerCase(a, address_value->text, address_value->length);
      if (!contract) return oom(error);
    }
    const Value* token_id = tj::Get(nft, "tokenId");
    if (!contract || !contract->length || !is_string(token_id)) continue;
    const Value* token_type = tj::Get(nft, "tokenType");
    Value* standard = tj::Null(a);
    if (!nullish(token_type)) {
      if (!is_string(token_type)) {
        error->Upstream("tokenType is not a string");
        return false;
      }
      standard = compat::token_text::ToLowerCase(a, token_type->text, token_type->length);
    }
    Value* name = nullptr;
    Value* collection = nullptr;
    if (!truthy_name(a, tj::Get(nft, "name"), &name) ||
        !truthy_name(a, tj::Get(contract_object, "name"), &collection)) {
      error->Upstream("NFT name is not a string");
      return false;
    }
    const Value* image = tj::Get(nft, "image");
    const Value* cached = tj::Get(image, "cachedUrl");
    const Value* original = tj::Get(image, "originalUrl");
    Value* image_url = !nullish(cached) ? const_cast<Value*>(cached)
                       : !nullish(original) ? const_cast<Value*>(original) : tj::Null(a);
    Value* flags = tj::Obj(a);
    Value* item = tj::Obj(a);
    if (!standard || !flags || !item || !image_url) return oom(error);
    if (truthy(tj::Get(contract_object, "isSpam")) && !tj::Set(a, flags, "spam", tj::Bool(a, true)))
      return oom(error);
    Value* nft_id = Format(a, "%s:%.*s:%.*s", network.id, static_cast<int>(contract->length), contract->text,
                           static_cast<int>(token_id->length), token_id->text);
    if (!nft_id || !tj::Set(a, item, "nftId", nft_id) || !tj::Set(a, item, "contractAddress", contract) ||
        !tj::Set(a, item, "tokenId", const_cast<Value*>(token_id)) || !tj::Set(a, item, "standard", standard) ||
        !tj::Set(a, item, "name", name) || !tj::Set(a, item, "collectionName", collection) ||
        !tj::Set(a, item, "imageUrl", image_url) || !tj::Set(a, item, "flags", flags) ||
        !tj::Push(a, out, item))
      return oom(error);
  }
  *items = out;
  return true;
}

namespace {

bool driver_present(const config::ProviderRuntimeConfig&) { return true; }
bool nfts_present(const config::ProviderRuntimeConfig& config) { return config.alchemy_api_key_configured; }
bool supports_nfts(const Network& network) { return slug_for(kNftSlugs, network.id) != nullptr; }

// ---- transfer records (shared by both providers) -----------------------------------------

// evmTokenTransferType(direction, from, to).
const char* token_transfer_type(const char* direction, const Value* from, const Value* to) {
  if (strcmp(direction, "incoming") == 0 && same(from, kZeroAddress)) return "mint";
  if (strcmp(direction, "outgoing") == 0 && (same(to, kZeroAddress) || same(to, kDeadAddress)))
    return "burn";
  return TypeFromDirection(direction);
}

bool set_all(Arena& a, Value* object, const char* const* keys, Value* const* values, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (!values[i] || !tj::Set(a, object, keys[i], values[i])) return false;
  return true;
}

// The record id, or a failure where transferRecordId throws.
Value* record_id(Arena& a, const Network& network, const Value* hash, const char* unit) {
  const char* text = tj::CString(a, hash);
  if (!text) return nullptr;
  Value* id = TransferRecordId(a, network.id, text, unit);
  return id && id->type == Type::kString ? id : nullptr;
}

// ---- Etherscan ----------------------------------------------------------------------------

constexpr size_t kUnsupportedCapacity = 64;
fortis::base::Mutex g_unsupported_mutex;
char g_unsupported[kUnsupportedCapacity][kMaxNetworkIdBytes + 1];
size_t g_unsupported_count = 0;
size_t g_unsupported_next = 0;

// The process-lifetime unsupportedNetworks set, bounded: a full table
// replaces its oldest entry.
bool etherscan_unsupported(const char* network) {
  fortis::base::LockGuard g_unsupported_lock(g_unsupported_mutex);
  bool found = false;
  for (size_t i = 0; i < g_unsupported_count && !found; ++i) found = strcmp(g_unsupported[i], network) == 0;
  return found;
}

void etherscan_mark_unsupported(const char* network) {
  fortis::base::LockGuard g_unsupported_lock(g_unsupported_mutex);
  bool found = false;
  for (size_t i = 0; i < g_unsupported_count && !found; ++i) found = strcmp(g_unsupported[i], network) == 0;
  if (!found) {
    const size_t slot = g_unsupported_count < kUnsupportedCapacity ? g_unsupported_count++ : g_unsupported_next;
    if (g_unsupported_count == kUnsupportedCapacity)
      g_unsupported_next = (slot + 1) % kUnsupportedCapacity;
    snprintf(g_unsupported[slot], sizeof(g_unsupported[slot]), "%s", network);
  }
}

bool etherscan_supports(const config::ProviderRuntimeConfig& config, const Network& network) {
  return config.etherscan_api_key_configured && network.has_evm_chain_id && !network.testnet &&
         !etherscan_unsupported(network.id);
}

struct EtherscanCursor {
  double startblock = 0;
  double page = 1;
  bool has_last = false;
  double last_block = 0;
};

EtherscanCursor etherscan_cursor(const Value* raw, double fallback) {
  EtherscanCursor out;
  out.startblock = fallback;
  // `raw && typeof raw === 'object'` (arrays included).
  if (raw && (raw->type == Type::kObject || raw->type == Type::kArray)) {
    const Value* start = tj::Get(raw, "startblock");
    const Value* page = tj::Get(raw, "page");
    const Value* last = tj::Get(raw, "lastBlock");
    if (start && start->type == Type::kNumber) out.startblock = start->number;
    if (page && page->type == Type::kNumber && page->number >= 1) out.page = page->number;
    if (last && last->type == Type::kNumber) { out.has_last = true; out.last_block = last->number; }
  }
  return out;
}

// advance(): the next cursor object.
Value* etherscan_advance(Arena& a, const EtherscanCursor& current, const Value* rows, double limit) {
  double max_block = current.has_last ? current.last_block : current.startblock - 1;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const double block = JsNumber(tj::Get(rows->items[i], "blockNumber"));
    if (isfinite(block) && block > max_block) max_block = block;
  }
  Value* cursor = tj::Obj(a);
  if (!cursor) return nullptr;
  if (rows->count >= limit) {
    if (!tj::Set(a, cursor, "startblock", tj::Num(a, current.startblock)) ||
        !tj::Set(a, cursor, "page", tj::Num(a, current.page + 1)) ||
        !tj::Set(a, cursor, "lastBlock", tj::Num(a, max_block)))
      return nullptr;
    return cursor;
  }
  const double next = max_block + 1 > current.startblock ? max_block + 1 : current.startblock;
  if (!tj::Set(a, cursor, "startblock", tj::Num(a, next)) || !tj::Set(a, cursor, "page", tj::Num(a, 1)))
    return nullptr;
  return cursor;
}

enum class PageOutcome : uint8_t { kRows, kUnavailable, kFailed };

struct EtherscanPage {
  PageOutcome outcome = PageOutcome::kFailed;
  const Value* rows = nullptr;
  Error error;
  char reason[512]{};
};

// page(): interprets one txlist/tokentx answer.
void etherscan_page(Arena& a, const Network& network, const char* action, const Call& call,
                    EtherscanPage* page) {
  if (!call.ok()) { call.ToError(&page->error); return; }
  const Value* body = call.value;
  if (!body || body->type == Type::kNull) {
    page->error.Upstream("etherscan %s: null body", action);
    return;
  }
  const Value* result = tj::Get(body, "result");
  if (result && result->type == Type::kArray) {
    page->outcome = PageOutcome::kRows;
    page->rows = result;
    return;
  }
  const Value* message = tj::Get(body, "message");
  if (!nullish(message) && !is_string(message)) {
    page->error.Upstream("etherscan %s: message is not a string", action);
    return;
  }
  if (is_string(message) && message->length >= 15 && memcmp(message->text, "No transactions", 15) == 0) {
    page->rows = tj::Arr(a);
    if (!page->rows) {
      page->error.Upstream("arena exhausted");
      return;
    }
    page->outcome = PageOutcome::kRows;
    return;
  }
  size_t begin = 0, end = 0;
  if (is_string(result)) compat::token_text::Trim(result->text, result->length, &begin, &end);
  const char* detail = is_string(result) ? result->text + begin : "";
  const size_t detail_length = is_string(result) ? end - begin : 0;
  if (contains_ci(detail, detail_length, "free api access is not supported for this chain") ||
      contains_ci(detail, detail_length, "missing or unsupported chainid parameter")) {
    etherscan_mark_unsupported(network.id);
    char chain[48];
    JsNumberText(network.evm_chain_id, chain, sizeof(chain));
    snprintf(page->reason, sizeof(page->reason), "Etherscan does not cover %s (chain %s)", network.id, chain);
    page->outcome = PageOutcome::kUnavailable;
    return;
  }
  const char* message_text = is_string(message) ? message->text : "malformed result";
  const int message_length = is_string(message) ? static_cast<int>(message->length)
                                                : static_cast<int>(strlen(message_text));
  if (detail_length)
    page->error.Upstream("etherscan %s: %.*s: %.*s", action, message_length, message_text,
                         static_cast<int>(detail_length), detail);
  else
    page->error.Upstream("etherscan %s: %.*s", action, message_length, message_text);
}

// feeFields(): the fee only on the paying side.
bool etherscan_fee(Arena& a, const Network& network, bool paid_by_holder, const Value* tx, Value* record) {
  const Value* gas_used = tj::Get(tx, "gasUsed");
  const Value* gas_price = tj::Get(tx, "gasPrice");
  if (!paid_by_holder || !digit_string(gas_used) || !digit_string(gas_price)) {
    return tj::Set(a, record, "feeRaw", tj::Null(a)) && tj::Set(a, record, "feeDecimals", tj::Null(a)) &&
           tj::Set(a, record, "feeSymbol", tj::Null(a));
  }
  BigInt fee, price;
  if (!BigInt::ParseDecimal(gas_used->text, gas_used->length, &fee) ||
      !BigInt::ParseDecimal(gas_price->text, gas_price->length, &price) || !fee.Mul(price))
    return false;
  return tj::Set(a, record, "feeRaw", fee.ToValue(a)) &&
         tj::Set(a, record, "feeDecimals", tj::Num(a, network.has_native ? network.native_decimals : 18)) &&
         tj::Set(a, record, "feeSymbol", tj::Str(a, network.has_native ? network.native_symbol : ""));
}

// Number(tx.blockNumber) || null
Value* block_or_null(Arena& a, const Value* v) {
  const double block = JsNumber(v);
  return block != 0 && !isnan(block) ? tj::Num(a, block) : tj::Null(a);
}

// new Date(Number(tx.timeStamp) * 1000)
Value* seconds_ts(Arena& a, const Value* v) { return tj::Num(a, JsNumber(v) * 1000); }

// kEmptyId: transferRecordId's "chainId and hash must be non-empty" (a hash
// that trims to nothing).
enum class RecordOutcome : uint8_t { kRecord, kSkip, kFailed, kEmptyId };

bool blank_hash(const Value* hash) {
  if (!is_string(hash)) return false;
  size_t begin = 0, end = 0;
  compat::token_text::Trim(hash->text, hash->length, &begin, &end);
  return begin == end;
}

RecordOutcome etherscan_native(Arena& a, const Network& network, const char* holder, const Value* tx,
                               Value** out) {
  if (tx->type == Type::kNull) return RecordOutcome::kFailed;
  const Value* hash = tj::Get(tx, "hash");
  const Value* time = tj::Get(tx, "timeStamp");
  if (!truthy(hash) || !truthy(time)) return RecordOutcome::kSkip;
  if (!is_string(hash)) return RecordOutcome::kFailed;
  Value* from = nullptr;
  Value* to = nullptr;
  if (!lower_or_empty(a, tj::Get(tx, "from"), &from) || !lower_or_empty(a, tj::Get(tx, "to"), &to))
    return RecordOutcome::kFailed;
  const char* direction = DirectionFromFlow(same(to, holder), same(from, holder));
  const bool failed = equals(tj::Get(tx, "isError"), "1") || equals(tj::Get(tx, "txreceipt_status"), "0");
  const Value* raw_value = tj::Get(tx, "value");
  Value* value = digit_string(raw_value) ? const_cast<Value*>(raw_value) : tj::Str(a, "0");
  const Value* input = tj::Get(tx, "input");
  const bool is_call = equals(value, "0") && !(nullish(input) ? true : equals(input, "0x"));
  Value* native = NativeAssetJson(a, network);
  Value* id = record_id(a, network, hash, nullptr);
  if (!id) return blank_hash(hash) ? RecordOutcome::kEmptyId : RecordOutcome::kFailed;
  const Value* function_name = tj::Get(tx, "functionName");
  Value* method = tj::Null(a);
  if (!nullish(function_name)) {
    if (!is_string(function_name)) return RecordOutcome::kFailed;
    const char* paren = static_cast<const char*>(memchr(function_name->text, '(', function_name->length));
    const size_t length = paren ? static_cast<size_t>(paren - function_name->text) : function_name->length;
    if (length) method = tj::Str(a, function_name->text, length);
  }
  const Value* tx_from = tj::Get(tx, "from");
  const Value* tx_to = tj::Get(tx, "to");
  const bool incoming = strcmp(direction, "incoming") == 0;
  Value* record = NewTransfer(a);
  if (!record || !native || !value || !method) return RecordOutcome::kFailed;
  const char* const keys[] = {"recordId", "txType", "status", "direction", "ts", "hash", "blockNumber",
                              "fromAddress", "toAddress", "counterparty", "tokenId", "asset", "amountRaw",
                              "amountDecimals"};
  Value* const values[] = {id,
                           tj::Str(a, is_call ? "contractCall" : TypeFromDirection(direction)),
                           tj::Str(a, failed ? "failed" : "confirmed"),
                           tj::Str(a, direction),
                           seconds_ts(a, time),
                           const_cast<Value*>(hash),
                           block_or_null(a, tj::Get(tx, "blockNumber")),
                           or_null(a, tx_from),
                           or_null(a, tx_to),
                           or_null(a, incoming ? tx_from : tx_to),
                           tj::Str(a, "native"),
                           native,
                           value,
                           tj::Num(a, network.has_native ? network.native_decimals : 18)};
  if (!set_all(a, record, keys, values, sizeof(keys) / sizeof(keys[0])) ||
      !etherscan_fee(a, network, same(from, holder), tx, record) ||
      !tj::Set(a, record, "memo", tj::Null(a)) || !tj::Set(a, record, "method", method) ||
      !tj::Set(a, record, "error", failed ? tj::Str(a, "reverted") : tj::Null(a)) ||
      !tj::Set(a, record, "metadata", tj::Obj(a)))
    return RecordOutcome::kFailed;
  *out = record;
  return RecordOutcome::kRecord;
}

// sanitizeSymbol(value ?? '') for a string-or-nullish value.
Value* symbol_of(Arena& a, const Value* v) {
  if (!is_string(v)) return SanitizeSymbol(a, "", 0);
  return SanitizeSymbol(a, v->text, v->length);
}

RecordOutcome etherscan_token(Arena& a, const Network& network, const char* holder, const Value* tx,
                              Value** out) {
  if (tx->type == Type::kNull) return RecordOutcome::kFailed;
  const Value* hash = tj::Get(tx, "hash");
  const Value* time = tj::Get(tx, "timeStamp");
  const Value* contract_value = tj::Get(tx, "contractAddress");
  if (!truthy(hash) || !truthy(time) || !truthy(contract_value)) return RecordOutcome::kSkip;
  if (!is_string(hash) || !is_string(contract_value)) return RecordOutcome::kFailed;
  Value* from = nullptr;
  Value* to = nullptr;
  if (!lower_or_empty(a, tj::Get(tx, "from"), &from) || !lower_or_empty(a, tj::Get(tx, "to"), &to))
    return RecordOutcome::kFailed;
  const char* direction = DirectionFromFlow(same(to, holder), same(from, holder));
  Value* contract = compat::token_text::ToLowerCase(a, contract_value->text, contract_value->length);
  if (!contract) return RecordOutcome::kFailed;
  const double decimals = JsNumber(tj::Get(tx, "tokenDecimal"));
  if (!isfinite(decimals) || floor(decimals) != decimals || decimals < 0) return RecordOutcome::kSkip;
  const Value* log_index = tj::Get(tx, "logIndex");
  const char* contract_text = tj::CString(a, contract);
  const char* unit = contract_text;
  if (!nullish(log_index)) {
    // `${contract}:${tx.logIndex}`
    Value* joined = nullptr;
    if (is_string(log_index)) {
      joined = Format(a, "%s:%.*s", contract_text, static_cast<int>(log_index->length), log_index->text);
    } else if (log_index->type == Type::kNumber) {
      char number[48];
      JsNumberText(log_index->number, number, sizeof(number));
      joined = Format(a, "%s:%s", contract_text, number);
    } else {
      const char* text = log_index->type == Type::kTrue    ? "true"
                         : log_index->type == Type::kFalse ? "false"
                         : log_index->type == Type::kArray ? nullptr
                                                           : "[object Object]";
      // String([...]) joins the elements; providers never send one.
      if (!text) return RecordOutcome::kFailed;
      joined = Format(a, "%s:%s", contract_text, text);
    }
    unit = tj::CString(a, joined);
  }
  if (!contract_text || !unit) return RecordOutcome::kFailed;
  Value* id = record_id(a, network, hash, unit);
  if (!id) return blank_hash(hash) ? RecordOutcome::kEmptyId : RecordOutcome::kFailed;
  const Value* symbol_raw = tj::Get(tx, "tokenSymbol");
  const Value* name_raw = tj::Get(tx, "tokenName");
  Value* asset = tj::Obj(a);
  if (!asset || !tj::Set(a, asset, "chainId", tj::Str(a, network.id)) ||
      !tj::Set(a, asset, "symbol", symbol_of(a, symbol_raw)) ||
      !tj::Set(a, asset, "name", symbol_of(a, nullish(name_raw) ? symbol_raw : name_raw)) ||
      !tj::Set(a, asset, "decimals", tj::Num(a, decimals)) || !tj::Set(a, asset, "contractAddress", contract))
    return RecordOutcome::kFailed;
  const Value* raw_value = tj::Get(tx, "value");
  Value* value = digit_string(raw_value) ? const_cast<Value*>(raw_value) : tj::Str(a, "0");
  Value* token_id = Format(a, "erc20:%s", contract_text);
  const Value* tx_from = tj::Get(tx, "from");
  const Value* tx_to = tj::Get(tx, "to");
  const bool incoming = strcmp(direction, "incoming") == 0;
  Value* record = NewTransfer(a);
  if (!record || !value || !token_id) return RecordOutcome::kFailed;
  const char* const keys[] = {"recordId", "txType", "status", "direction", "ts", "hash", "blockNumber",
                              "fromAddress", "toAddress", "counterparty", "tokenId", "asset", "amountRaw",
                              "amountDecimals"};
  Value* const values[] = {id,
                           tj::Str(a, token_transfer_type(direction, from, to)),
                           tj::Str(a, "confirmed"),
                           tj::Str(a, direction),
                           seconds_ts(a, time),
                           const_cast<Value*>(hash),
                           block_or_null(a, tj::Get(tx, "blockNumber")),
                           or_null(a, tx_from),
                           or_null(a, tx_to),
                           or_null(a, incoming ? tx_from : tx_to),
                           token_id,
                           asset,
                           value,
                           tj::Num(a, decimals)};
  if (!set_all(a, record, keys, values, sizeof(keys) / sizeof(keys[0])) ||
      !etherscan_fee(a, network, same(from, holder), tx, record) ||
      !tj::Set(a, record, "memo", tj::Null(a)) || !tj::Set(a, record, "method", tj::Null(a)) ||
      !tj::Set(a, record, "error", tj::Null(a)) || !tj::Set(a, record, "metadata", tj::Obj(a)))
    return RecordOutcome::kFailed;
  *out = record;
  return RecordOutcome::kRecord;
}

bool etherscan_fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                     double backfill_depth, SourceResult* result, Error* error) {
  Arena& a = *ctx.arena;
  const EtherscanCursor native_cursor = etherscan_cursor(tj::Get(state, "native"), 0);
  const EtherscanCursor token_cursor = etherscan_cursor(tj::Get(state, "token"), 0);
  const double limit = backfill_depth < 500 ? backfill_depth : 500;
  const char* actions[2] = {"txlist", "tokentx"};
  const EtherscanCursor* cursors[2] = {&native_cursor, &token_cursor};
  const char* urls[1] = {kEtherscanBase};
  Call calls[2];
  char chain[48], limit_text[48];
  JsNumberText(network.evm_chain_id, chain, sizeof(chain));
  JsNumberText(limit, limit_text, sizeof(limit_text));
  for (int i = 0; i < 2; ++i) {
    char start[48], page[48];
    JsNumberText(cursors[i]->startblock, start, sizeof(start));
    JsNumberText(cursors[i]->page, page, sizeof(page));
    Value* path = Format(a,
                         "?chainid=%s&module=account&action=%s&address=%s&startblock=%s&endblock=latest"
                         "&page=%s&offset=%s&sort=asc&apikey=%s",
                         chain, actions[i], address, start, page, limit_text, ctx.config->etherscan_api_key);
    const char* wire = path ? FetchWirePath(a, tj::CString(a, path)) : nullptr;
    if (!wire) return oom(error);
    calls[i].kind = CallKind::kGet;
    calls[i].urls = urls;
    calls[i].url_count = 1;
    calls[i].path = wire;
    calls[i].lane = "etherscan";
  }
  ctx.transport->Run(calls, 2, a);
  // Promise.all: both answers are interpreted (each may record a coverage
  // gap), then a failure of either rejects the fetch.
  EtherscanPage pages[2];
  for (int i = 0; i < 2; ++i) etherscan_page(a, network, actions[i], calls[i], &pages[i]);
  for (int i = 0; i < 2; ++i)
    if (pages[i].outcome == PageOutcome::kFailed) { *error = pages[i].error; return false; }
  for (int i = 0; i < 2; ++i)
    if (pages[i].outcome == PageOutcome::kUnavailable) {
      result->unavailable = true;
      snprintf(result->reason, sizeof(result->reason), "%s", pages[i].reason);
      return true;
    }
  Value* holder_value = compat::token_text::ToLowerCase(a, address, strlen(address));
  const char* holder = holder_value ? tj::CString(a, holder_value) : nullptr;
  Value* records = tj::Arr(a);
  if (!holder || !records) return oom(error);
  for (int i = 0; i < 2; ++i) {
    for (uint32_t r = 0; r < pages[i].rows->count; ++r) {
      Value* record = nullptr;
      const RecordOutcome outcome = i == 0 ? etherscan_native(a, network, holder, pages[i].rows->items[r], &record)
                                           : etherscan_token(a, network, holder, pages[i].rows->items[r], &record);
      if (outcome == RecordOutcome::kFailed) {
        error->Upstream("etherscan %s: malformed row", actions[i]);
        return false;
      }
      if (outcome == RecordOutcome::kEmptyId) {
        error->Upstream("chainId and hash must be non-empty");
        return false;
      }
      if (outcome == RecordOutcome::kRecord && !tj::Push(a, records, record)) return oom(error);
    }
  }
  Value* next_state = tj::Obj(a);
  Value* next_native = etherscan_advance(a, native_cursor, pages[0].rows, limit);
  Value* next_token = etherscan_advance(a, token_cursor, pages[1].rows, limit);
  if (!next_state || !next_native || !next_token || !tj::Set(a, next_state, "native", next_native) ||
      !tj::Set(a, next_state, "token", next_token))
    return oom(error);
  result->records = records;
  result->state = next_state;
  result->backfill_complete = pages[0].rows->count < limit && pages[1].rows->count < limit;
  return true;
}

// ---- Alchemy transfers ---------------------------------------------------------------------

bool alchemy_supports(const config::ProviderRuntimeConfig& config, const Network& network) {
  return config.alchemy_api_key_configured && !network.testnet &&
         slug_for(kTransferSlugs, network.id) != nullptr;
}

struct AlchemyCursor {
  double from_block = 0;
  const Value* page_key = nullptr;  // a non-empty string
  bool has_last = false;
  double last_block = 0;
};

AlchemyCursor alchemy_cursor(const Value* raw) {
  AlchemyCursor out;
  if (!is_object(raw)) return out;
  const Value* from = tj::Get(raw, "fromBlock");
  const Value* key = tj::Get(raw, "pageKey");
  const Value* last = tj::Get(raw, "lastBlock");
  if (from && from->type == Type::kNumber && from->number >= 0) out.from_block = from->number;
  if (is_string(key) && key->length) out.page_key = key;
  if (last && last->type == Type::kNumber && last->number >= 0) { out.has_last = true; out.last_block = last->number; }
  return out;
}

// hexInt(): a safe integer from a number or /^(?:0x[0-9a-f]+|\d+)$/i string.
bool hex_int(const Value* v, double* out) {
  if (v && v->type == Type::kNumber) {
    if (!isfinite(v->number) || floor(v->number) != v->number || fabs(v->number) > 9007199254740991.0)
      return false;
    *out = v->number;
    return true;
  }
  if (!is_string(v) || !v->length) return false;
  const bool hex = v->length > 2 && v->text[0] == '0' && (v->text[1] == 'x' || v->text[1] == 'X');
  for (size_t i = hex ? 2 : 0; i < v->length; ++i)
    if (hex ? !is_hex(v->text[i]) : (v->text[i] < '0' || v->text[i] > '9')) return false;
  BigInt big;
  if (!BigInt::Parse(v->text, v->length, &big)) return false;
  const double number = big.ToDouble();
  if (number > 9007199254740991.0) return false;
  *out = number;
  return true;
}

// unsignedRaw(): the decimal string of a non-negative integer value.
Value* unsigned_raw(Arena& a, const Value* v) {
  if (!is_string(v) || !v->length) return nullptr;
  const bool hex = v->length > 2 && v->text[0] == '0' && (v->text[1] == 'x' || v->text[1] == 'X');
  for (size_t i = hex ? 2 : 0; i < v->length; ++i)
    if (hex ? !is_hex(v->text[i]) : (v->text[i] < '0' || v->text[i] > '9')) return nullptr;
  BigInt big;
  if (!BigInt::Parse(v->text, v->length, &big) || big.negative()) return nullptr;
  return big.ToValue(a);
}

Value* alchemy_advance(Arena& a, const AlchemyCursor& cursor, const Value* rows, const Value* page_key) {
  double max_block = cursor.has_last ? cursor.last_block : cursor.from_block - 1;
  for (uint32_t i = 0; i < rows->count; ++i) {
    double block = 0;
    if (hex_int(tj::Get(rows->items[i], "blockNum"), &block) && block > max_block) max_block = block;
  }
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  if (page_key) {
    if (!tj::Set(a, out, "fromBlock", tj::Num(a, cursor.from_block)) ||
        !tj::Set(a, out, "pageKey", const_cast<Value*>(page_key)) ||
        !tj::Set(a, out, "lastBlock", tj::Num(a, max_block)))
      return nullptr;
    return out;
  }
  const double next = max_block + 1 > cursor.from_block ? max_block + 1 : cursor.from_block;
  if (!tj::Set(a, out, "fromBlock", tj::Num(a, next))) return nullptr;
  return out;
}

RecordOutcome alchemy_record(Arena& a, const Network& network, const char* holder, const Value* row,
                             Value** out) {
  if (row->type == Type::kNull) return RecordOutcome::kFailed;
  const Value* hash = tj::Get(row, "hash");
  const Value* metadata = tj::Get(row, "metadata");
  const Value* block_timestamp = tj::Get(metadata, "blockTimestamp");
  if (!truthy(hash) || !truthy(block_timestamp)) return RecordOutcome::kSkip;
  if (!is_string(hash)) return RecordOutcome::kFailed;
  Value* from = nullptr;
  Value* to = nullptr;
  if (!lower_or_empty(a, tj::Get(row, "from"), &from) || !lower_or_empty(a, tj::Get(row, "to"), &to))
    return RecordOutcome::kFailed;
  const bool incoming = same(to, holder);
  const bool outgoing = same(from, holder);
  if (!incoming && !outgoing) return RecordOutcome::kSkip;
  const char* direction = DirectionFromFlow(incoming, outgoing);
  const Value* raw_contract = tj::Get(row, "rawContract");
  Value* amount = unsigned_raw(a, tj::Get(raw_contract, "value"));
  if (!amount) return RecordOutcome::kSkip;
  // new Date(blockTimestamp): a string is parsed; a number is epoch ms.
  double ts = NAN;
  if (is_string(block_timestamp)) {
    if (!ParseTime(block_timestamp->text, block_timestamp->length, &ts)) return RecordOutcome::kSkip;
  } else if (block_timestamp->type == Type::kNumber) {
    ts = block_timestamp->number;
    if (!isfinite(ts) || fabs(ts) > 8.64e15) return RecordOutcome::kSkip;
    ts = trunc(ts);
  } else {
    return RecordOutcome::kSkip;
  }
  double block = 0;
  Value* block_number = hex_int(tj::Get(row, "blockNum"), &block) ? tj::Num(a, block) : tj::Null(a);
  const Value* row_from = tj::Get(row, "from");
  const Value* row_to = tj::Get(row, "to");
  const Value* category = tj::Get(row, "category");
  const char* const keys[] = {"recordId", "txType", "status", "direction", "ts", "hash", "blockNumber",
                              "fromAddress", "toAddress", "counterparty", "tokenId", "asset", "amountRaw",
                              "amountDecimals", "feeRaw", "feeDecimals", "feeSymbol", "memo", "method",
                              "error", "metadata"};
  Value* record = NewTransfer(a);
  if (!record) return RecordOutcome::kFailed;
  if (equals(category, "external")) {
    Value* native = NativeAssetJson(a, network);
    Value* id = record_id(a, network, hash, nullptr);
    if (!native || !id) return RecordOutcome::kFailed;
    Value* const values[] = {id,
                             tj::Str(a, TypeFromDirection(direction)),
                             tj::Str(a, "confirmed"),
                             tj::Str(a, direction),
                             tj::Num(a, ts),
                             const_cast<Value*>(hash),
                             block_number,
                             or_null(a, row_from),
                             or_null(a, row_to),
                             or_null(a, strcmp(direction, "incoming") == 0 ? row_from : row_to),
                             tj::Str(a, "native"),
                             native,
                             amount,
                             tj::Num(a, network.has_native ? network.native_decimals : 18),
                             tj::Null(a), tj::Null(a), tj::Null(a), tj::Null(a), tj::Null(a), tj::Null(a),
                             tj::Obj(a)};
    if (!set_all(a, record, keys, values, sizeof(keys) / sizeof(keys[0]))) return RecordOutcome::kFailed;
    *out = record;
    return RecordOutcome::kRecord;
  }
  const Value* contract_value = tj::Get(raw_contract, "address");
  if (!equals(category, "erc20") || !truthy(contract_value)) return RecordOutcome::kSkip;
  if (!is_string(contract_value)) return RecordOutcome::kFailed;
  Value* contract = compat::token_text::ToLowerCase(a, contract_value->text, contract_value->length);
  const char* contract_text = contract ? tj::CString(a, contract) : nullptr;
  if (!contract_text) return RecordOutcome::kFailed;
  double decimals = 0;
  if (!hex_int(tj::Get(raw_contract, "decimal"), &decimals) || decimals < 0 || decimals > 24)
    return RecordOutcome::kSkip;
  const Value* asset_name = tj::Get(row, "asset");
  if (!nullish(asset_name) && !is_string(asset_name)) return RecordOutcome::kFailed;
  Value* symbol = symbol_of(a, asset_name);
  // uniqueId?.split(':').at(-1)
  const Value* unique_id = tj::Get(row, "uniqueId");
  const char* unit = contract_text;
  if (!nullish(unique_id)) {
    if (!is_string(unique_id)) return RecordOutcome::kFailed;
    size_t start = 0;
    for (size_t i = 0; i < unique_id->length; ++i)
      if (unique_id->text[i] == ':') start = i + 1;
    if (start < unique_id->length) {
      Value* joined = Format(a, "%s:%.*s", contract_text, static_cast<int>(unique_id->length - start),
                             unique_id->text + start);
      unit = joined ? tj::CString(a, joined) : nullptr;
    }
  }
  Value* id = unit ? record_id(a, network, hash, unit) : nullptr;
  Value* asset = tj::Obj(a);
  if (!id || !symbol || !asset || !tj::Set(a, asset, "chainId", tj::Str(a, network.id)) ||
      !tj::Set(a, asset, "symbol", symbol) || !tj::Set(a, asset, "name", symbol) ||
      !tj::Set(a, asset, "decimals", tj::Num(a, decimals)) || !tj::Set(a, asset, "contractAddress", contract))
    return RecordOutcome::kFailed;
  Value* const values[] = {id,
                           tj::Str(a, token_transfer_type(direction, from, to)),
                           tj::Str(a, "confirmed"),
                           tj::Str(a, direction),
                           tj::Num(a, ts),
                           const_cast<Value*>(hash),
                           block_number,
                           or_null(a, row_from),
                           or_null(a, row_to),
                           or_null(a, strcmp(direction, "incoming") == 0 ? row_from : row_to),
                           Format(a, "erc20:%s", contract_text),
                           asset,
                           amount,
                           tj::Num(a, decimals),
                           tj::Null(a), tj::Null(a), tj::Null(a), tj::Null(a), tj::Null(a), tj::Null(a),
                           tj::Obj(a)};
  if (!set_all(a, record, keys, values, sizeof(keys) / sizeof(keys[0]))) return RecordOutcome::kFailed;
  *out = record;
  return RecordOutcome::kRecord;
}

bool record_ids_equal(const Value* left, const Value* right) {
  const Value* a = tj::Get(left, "recordId");
  const Value* b = tj::Get(right, "recordId");
  return is_string(a) && is_string(b) && a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

bool alchemy_fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                   double backfill_depth, SourceResult* result, Error* error) {
  Arena& a = *ctx.arena;
  const char* slug = slug_for(kTransferSlugs, network.id);
  if (!slug) {
    error->Range("Alchemy transfers not served for %s", network.id);
    return false;
  }
  const AlchemyCursor cursors[2] = {alchemy_cursor(tj::Get(state, "incoming")),
                                    alchemy_cursor(tj::Get(state, "outgoing"))};
  double limit = backfill_depth > 1 ? backfill_depth : 1;
  if (limit > 1000) limit = 1000;
  Value* url_value = Format(a, "https://%s.g.alchemy.com/v2/%s", slug, ctx.config->alchemy_api_key);
  const char** urls = static_cast<const char**>(a.Alloc(sizeof(const char*)));
  if (!url_value || !urls) return oom(error);
  urls[0] = tj::CString(a, url_value);
  const char* max_count = number_hex(a, limit);
  Call calls[2];
  for (int side = 0; side < 2; ++side) {
    const char* from_block = number_hex(a, cursors[side].from_block);
    Value* params = tj::Obj(a);
    Value* category = tj::Arr(a);
    Value* list = tj::Arr(a);
    Value* body = tj::Obj(a);
    if (!from_block || !max_count || !params || !category || !list || !body ||
        !tj::Set(a, params, "fromBlock", tj::Str(a, from_block)) ||
        !tj::Set(a, params, "toBlock", tj::Str(a, "latest")) ||
        !tj::Set(a, params, side == 0 ? "toAddress" : "fromAddress", tj::Str(a, address)) ||
        !tj::Push(a, category, tj::Str(a, "external")) || !tj::Push(a, category, tj::Str(a, "erc20")) ||
        !tj::Set(a, params, "category", category) || !tj::Set(a, params, "withMetadata", tj::Bool(a, true)) ||
        !tj::Set(a, params, "excludeZeroValue", tj::Bool(a, true)) ||
        !tj::Set(a, params, "order", tj::Str(a, "asc")) || !tj::Set(a, params, "maxCount", tj::Str(a, max_count)) ||
        (cursors[side].page_key && !tj::Set(a, params, "pageKey", const_cast<Value*>(cursors[side].page_key))) ||
        !tj::Push(a, list, params) || !tj::Set(a, body, "jsonrpc", tj::Str(a, "2.0")) ||
        !tj::Set(a, body, "id", tj::Num(a, 1)) ||
        !tj::Set(a, body, "method", tj::Str(a, "alchemy_getAssetTransfers")) || !tj::Set(a, body, "params", list))
      return oom(error);
    calls[side].kind = CallKind::kPost;
    calls[side].urls = urls;
    calls[side].url_count = 1;
    calls[side].path = "";
    calls[side].body = body;
    calls[side].lane = "alchemy";
  }
  ctx.transport->Run(calls, 2, a);
  const Value* rows[2] = {nullptr, nullptr};
  const Value* keys[2] = {nullptr, nullptr};
  for (int side = 0; side < 2; ++side) {
    if (!calls[side].ok()) { calls[side].ToError(error); return false; }
    const Value* body = calls[side].value;
    if (!body || body->type == Type::kNull) {
      error->Upstream("Alchemy transfers: null body");
      return false;
    }
    if (tj::Get(body, "error")) {
      const Value* message = tj::Get(tj::Get(body, "error"), "message");
      if (is_string(message)) {
        Value* cut = compat::token_text::Slice(a, message->text, message->length, 0, 240);
        error->Upstream("Alchemy transfers: %.*s", cut ? static_cast<int>(cut->length) : 0, cut ? cut->text : "");
      } else {
        error->Upstream("Alchemy transfers: request rejected");
      }
      return false;
    }
    const Value* transfers = tj::Get(tj::Get(body, "result"), "transfers");
    if (!transfers || transfers->type != Type::kArray) {
      error->Upstream("Alchemy transfers: malformed result");
      return false;
    }
    rows[side] = transfers;
    const Value* key = tj::Get(tj::Get(body, "result"), "pageKey");
    keys[side] = is_string(key) && key->length ? key : nullptr;
  }
  Value* holder_value = compat::token_text::ToLowerCase(a, address, strlen(address));
  const char* holder = holder_value ? tj::CString(a, holder_value) : nullptr;
  Value* records = tj::Arr(a);
  if (!holder || !records) return oom(error);
  for (int side = 0; side < 2; ++side) {
    for (uint32_t i = 0; i < rows[side]->count; ++i) {
      Value* record = nullptr;
      const RecordOutcome outcome = alchemy_record(a, network, holder, rows[side]->items[i], &record);
      if (outcome == RecordOutcome::kFailed) {
        error->Upstream("Alchemy transfers: malformed row");
        return false;
      }
      if (outcome != RecordOutcome::kRecord) continue;
      // new Map(records.map(r => [r.recordId, r])): the first position, the
      // last value (self-transfers come back from both sides).
      bool replaced = false;
      for (uint32_t j = 0; j < records->count && !replaced; ++j)
        if (record_ids_equal(records->items[j], record)) { records->items[j] = record; replaced = true; }
      if (!replaced && !tj::Push(a, records, record)) return oom(error);
    }
  }
  Value* next_state = tj::Obj(a);
  Value* incoming = alchemy_advance(a, cursors[0], rows[0], keys[0]);
  Value* outgoing = alchemy_advance(a, cursors[1], rows[1], keys[1]);
  if (!next_state || !incoming || !outgoing || !tj::Set(a, next_state, "incoming", incoming) ||
      !tj::Set(a, next_state, "outgoing", outgoing))
    return oom(error);
  result->records = records;
  result->state = next_state;
  result->backfill_complete = !keys[0] && !keys[1];
  return true;
}

// ---- failover --------------------------------------------------------------------------------

struct Provider {
  const char* key;
  bool (*supports)(const config::ProviderRuntimeConfig&, const Network&);
  bool (*fetch)(Context&, const Network&, const char*, const Value*, double, SourceResult*, Error*);
};
constexpr Provider kProviders[] = {
    {"etherscan", etherscan_supports, etherscan_fetch},
    {"alchemy-transfers", alchemy_supports, alchemy_fetch},
};

// record(): a plain object, else {}.
const Value* record_or_empty(Arena& a, const Value* v) { return is_object(v) ? v : tj::Obj(a); }

bool source_present(const config::ProviderRuntimeConfig& config) {
  return config.etherscan_api_key_configured || config.alchemy_api_key_configured;
}

bool source_supports(const config::ProviderRuntimeConfig& config, const Network& network) {
  for (const Provider& provider : kProviders)
    if (provider.supports(config, network)) return true;
  return false;
}

}  // namespace

bool EvmSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* encoded = record_or_empty(a, tj::Get(state, "sources"));
  if (!encoded) return oom(error);
  const bool has_envelope = encoded->count > 0;
  Value* states = tj::Clone(a, encoded);
  if (!states) return oom(error);
  char reasons[512] = {};
  size_t reasons_used = 0;
  bool failed = false;
  Error last;
  for (const Provider& provider : kProviders) {
    if (!provider.supports(*ctx.config, network)) continue;
    const Value* stored = tj::Get(states, provider.key);
    const Value* source_state = record_or_empty(
        a, !nullish(stored) ? stored : (!has_envelope && strcmp(provider.key, "etherscan") == 0 ? state : nullptr));
    if (!source_state) return oom(error);
    SourceResult attempt;
    Error attempt_error;
    if (!provider.fetch(ctx, network, address, source_state, backfill_depth, &attempt, &attempt_error)) {
      // A transient primary failure must not block a healthy secondary.
      failed = true;
      last = attempt_error;
      continue;
    }
    if (attempt.unavailable) {
      const int written = snprintf(reasons + reasons_used, sizeof(reasons) - reasons_used, "%s%s",
                                   reasons_used ? "; " : "", attempt.reason);
      if (written > 0) {
        reasons_used += static_cast<size_t>(written);
        if (reasons_used >= sizeof(reasons)) reasons_used = sizeof(reasons) - 1;
      }
      continue;
    }
    Value* next = tj::Obj(a);
    if (!next || !tj::Set(a, states, provider.key, attempt.state) || !tj::Set(a, next, "sources", states))
      return oom(error);
    *result = attempt;
    result->state = next;
    return true;
  }
  if (failed) {
    *error = last;
    return false;
  }
  result->unavailable = true;
  if (reasons_used)
    snprintf(result->reason, sizeof(result->reason), "%s", reasons);
  else
    snprintf(result->reason, sizeof(result->reason), "no transfer source supports %s", network.id);
  return true;
}

bool EvmTxStatusOf(Arena& a, const Network& network, Value* tx, Value* receipt, const Value* tip, Value** result,
                   Error* error) {
  Value* out = tj::Obj(a);
  if (!out) return oom(error);
  // tx.blockNumber == null: in the mempool (a non-object transaction too).
  if (nullish(tj::Get(tx, "blockNumber"))) {
    if (!tj::Set(a, out, "status", tj::Str(a, "pending")) || !tj::Set(a, out, "blockNumber", tj::Null(a)) ||
        !tj::Set(a, out, "confirmedAt", tj::Null(a)) || !tj::Set(a, out, "detail", tj::Obj(a)) ||
        !tj::Set(a, out, "raw", tx))
      return oom(error);
    *result = out;
    return true;
  }
  // receipt !== null: any non-null answer counts as a receipt ('x', false).
  const bool has_receipt = !nullish(receipt);
  const bool failed = has_receipt && equals(tj::Get(receipt, "status"), "0x0");
  // Number(BigInt(String(tx.blockNumber))) then Number(BigInt(tipHex)), in
  // that order: the block's SyntaxError wins over a bad tip; a fractional
  // tip is BigInt's RangeError (the route's 400 with its message).
  double number = 0, height = 0;
  if (!StringBigIntNumber(a, tj::Get(tx, "blockNumber"), &number, error)) return false;
  if (!BigIntNumber(a, tip, &height, error)) {
    if (error->kind != ErrorKind::kRange) error->Upstream("eth_blockNumber: not a BigInt");
    return false;
  }
  // Plain double arithmetic: Infinity confirmations write as JSON null.
  const double confirmations = height >= number ? height - number + 1 : 0;
  const double required = network.has_required_confirmations ? network.required_confirmations : 3;
  const bool confirmed = has_receipt && confirmations >= required;
  const Value* gas_used = tj::Get(receipt, "gasUsed");
  const Value* gas_price = tj::Get(receipt, "effectiveGasPrice");
  Value* detail = tj::Obj(a);
  Value* raw = tj::Obj(a);
  if (!detail || !raw || !tj::Set(a, out, "status", tj::Str(a, failed ? "failed" : confirmed ? "confirmed" : "pending")) ||
      !tj::Set(a, out, "blockNumber", tj::Num(a, number)) || !tj::Set(a, out, "confirmedAt", tj::Null(a)) ||
      !tj::Set(a, detail, "confirmations", tj::Num(a, confirmations)) ||
      !tj::Set(a, detail, "requiredConfirmations", tj::Num(a, required)) ||
      !tj::Set(a, detail, "gasUsed", is_string(gas_used) ? const_cast<Value*>(gas_used) : tj::Null(a)) ||
      !tj::Set(a, detail, "effectiveGasPrice",
               is_string(gas_price) ? const_cast<Value*>(gas_price) : tj::Null(a)) ||
      !tj::Set(a, out, "detail", detail) || !tj::Set(a, raw, "transaction", tx) ||
      !tj::Set(a, raw, "receipt", receipt ? receipt : tj::Null(a)) || !tj::Set(a, out, "raw", raw))
    return oom(error);
  *result = out;
  return true;
}

const EvmDriver kEvmDriver{};

const EvmSource kEvmSource{};

// FetchWirePath (chain_driver.h): an Etherscan or Blockscout URL string as
// fetch() puts it on the wire.
const char* FetchWirePath(Arena& a, const char* path) {
  static const char kHex[] = "0123456789ABCDEF";
  const size_t length = strcspn(path, "#");
  char* out = static_cast<char*>(a.Alloc(length * 3 + 1, 1));
  if (!out) return nullptr;
  size_t used = 0;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(path[i]);
    if (c <= 0x20 || c == '"' || c == '\'' || c == '<' || c == '>' || c >= 0x7F) {
      out[used++] = '%';
      out[used++] = kHex[c >> 4];
      out[used++] = kHex[c & 15];
    } else {
      out[used++] = static_cast<char>(c);
    }
  }
  out[used] = '\0';
  return out;
}


bool EvmDriver::Registered(const config::ProviderRuntimeConfig& config) const { return driver_present(config); }
const NftReader* EvmDriver::Nfts(const config::ProviderRuntimeConfig& config) const { return nfts_present(config) ? this : nullptr; }
bool EvmDriver::SupportsNfts(const Network& network) const { return supports_nfts(network); }
bool EvmSource::Registered(const config::ProviderRuntimeConfig& config) const { return source_present(config); }
bool EvmSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return source_supports(config, network); }

}  // namespace fortis::chains
