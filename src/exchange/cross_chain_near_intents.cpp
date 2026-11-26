#include "ownership.h"
// NEAR Intents 1Click (port of the TypeScript adapter): a
// deposit-address provider. Passive pricing never creates an order; only a
// prepared quote allocates a quote-bound deposit target, which the adapter
// re-validates against the request before the wallet may sign anything.
#include "cross_chain_deposit_internal.h"
#include "threading.h"

#include "whatwg_url.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::cross_chain::detail {
using namespace deposit;
namespace {
constexpr const char* kNearBase = "https://1click.chaindefuser.com";
constexpr const char* kNearProvider = "near-intents";
constexpr const char* kAptCoin = "0x1::aptos_coin::AptosCoin";

const char* near_blockchain(const Value* chain_id) {
  static const char* kMap[][2] = {{"bitcoin", "btc"}, {"bitcoin-cash", "bch"}, {"dash", "dash"},
                                  {"zcash", "zec"},   {"1", "eth"},             {"8453", "base"},
                                  {"56", "bsc"},      {"aptos", "aptos"},       {"near", "near"},
                                  {"solana-mainnet-beta", "sol"},               {"ton", "ton"}};
  for (const auto& entry : kMap)
    if (Is(chain_id, entry[0])) return entry[1];
  return nullptr;
}

const char* near_native_symbol(const Value* chain_id) {
  static const char* kMap[][2] = {{"bitcoin", "BTC"}, {"bitcoin-cash", "BCH"}, {"dash", "DASH"},
                                  {"zcash", "ZEC"},   {"1", "ETH"},             {"8453", "ETH"},
                                  {"56", "BNB"},      {"aptos", "APT"},         {"near", "NEAR"},
                                  {"solana-mainnet-beta", "SOL"},               {"ton", "GRAM"}};
  for (const auto& entry : kMap)
    if (Is(chain_id, entry[0])) return entry[1];
  return "";
}

const char* near_native_asset(const Value* chain_id) {
  static const char* kMap[][2] = {{"bitcoin", "nep141:btc.omft.near"},
                                  {"bitcoin-cash", "nep141:bch.omft.near"},
                                  {"dash", "nep141:dash.omft.near"},
                                  {"zcash", "nep141:zec.omft.near"},
                                  {"1", "nep141:eth.omft.near"},
                                  {"8453", "nep141:base.omft.near"},
                                  {"solana-mainnet-beta", "nep141:sol.omft.near"},
                                  {"56", "nep245:v2_1.omni.hot.tg:56_11111111111111111111"},
                                  {"aptos", "nep141:aptos.omft.near"},
                                  {"ton", "nep245:v2_1.omni.hot.tg:1117_"}};
  for (const auto& entry : kMap)
    if (Is(chain_id, entry[0])) return entry[1];
  return nullptr;
}

bool near_is_native(Arena& arena, const Value* chain_id, const Value* token) {
  if (Is(chain_id, "bitcoin")) return lower_is(arena, token, "bitcoin");
  if (Is(chain_id, "bitcoin-cash")) return lower_is(arena, token, "bitcoin-cash");
  if (Is(chain_id, "dash")) return lower_is(arena, token, "dash");
  if (Is(chain_id, "zcash")) return lower_is(arena, token, "zcash");
  if (Is(chain_id, "1") || Is(chain_id, "8453") || Is(chain_id, "56")) return lower_is(arena, token, kNativeEvm);
  if (Is(chain_id, "aptos")) return lower_is(arena, token, "0x1::aptos_coin::aptoscoin");
  if (Is(chain_id, "solana-mainnet-beta")) return Is(token, "11111111111111111111111111111111");
  if (Is(chain_id, "ton")) return lower_is(arena, token, "ton");
  return false;
}

bool near_valid_address(const Value* chain_id, const Value* address) {
  if (Is(chain_id, "bitcoin")) return ValidBitcoinAddress(address);
  if (Is(chain_id, "bitcoin-cash")) return ValidBitcoinCashAddress(address);
  if (Is(chain_id, "dash")) return ValidDashAddress(address);
  if (Is(chain_id, "zcash")) return ValidZcashTransparentAddress(address);
  if (Is(chain_id, "1") || Is(chain_id, "8453") || Is(chain_id, "56")) return ValidEvmAddress(address);
  if (Is(chain_id, "aptos")) return ValidAptosAddress(address);
  if (Is(chain_id, "near")) return ValidNearAccount(address);
  if (Is(chain_id, "solana-mainnet-beta")) return ValidSolanaAddress(address);
  if (Is(chain_id, "ton")) return ValidTonFriendlyAddress(address);
  return false;
}

const Value* near_normalized(Arena& arena, const Value* chain_id, const Value* address) {
  if (Is(chain_id, "1") || Is(chain_id, "8453") || Is(chain_id, "56") || Is(chain_id, "aptos") ||
      Is(chain_id, "bitcoin-cash"))
    return Lower(arena, address);
  return address;
}

struct NearAsset {
  const Value* asset_id = nullptr;
  const Value* symbol = nullptr;
};

bool catalog_asset(Arena& arena, const NearToken* tokens, size_t count, const Endpoint& endpoint, NearAsset* out) {
  const char* blockchain = near_blockchain(endpoint.chain_id);
  if (!blockchain) return false;
  const bool native = near_is_native(arena, endpoint.chain_id, endpoint.token);
  auto candidate = [&](const NearToken& token) {
    return Is(token.blockchain, blockchain) && token.decimals == endpoint.decimals;
  };
  const NearToken* match = nullptr;
  size_t matches = 0;
  if (native) {
    const char* canonical = near_native_asset(endpoint.chain_id);
    const char* symbol = near_native_symbol(endpoint.chain_id);
    if (canonical) {
      for (size_t i = 0; i < count; ++i) {
        if (!candidate(tokens[i]) || !Is(tokens[i].asset_id, canonical)) continue;
        const Value* upper = Upper(arena, tokens[i].symbol);
        if (upper && Is(upper, symbol)) { match = &tokens[i]; ++matches; }
      }
    }
    if (matches > 1) return false;
    if (matches == 0) {
      if (Is(endpoint.chain_id, "ton")) return false;
      for (size_t i = 0; i < count; ++i) {
        if (!candidate(tokens[i]) || tokens[i].contract) continue;
        const Value* upper = Upper(arena, tokens[i].symbol);
        if (upper && Is(upper, symbol)) { match = &tokens[i]; ++matches; }
      }
    }
  } else {
    for (size_t i = 0; i < count; ++i)
      if (candidate(tokens[i]) && tokens[i].contract && same_lower(arena, tokens[i].contract, endpoint.token)) {
        match = &tokens[i];
        ++matches;
      }
  }
  if (matches != 1) return false;
  out->asset_id = tj::Clone(arena, match->asset_id);
  out->symbol = tj::Clone(arena, match->symbol);
  return out->asset_id && out->symbol;
}

// parseTokenCatalog() into the catalog's own arena.
bool parse_catalog(Arena& arena, const Value* payload, NearToken** tokens, size_t* count) {
  *tokens = nullptr;
  *count = 0;
  if (!payload || payload->type != tj::Type::kArray) return true;
  const size_t limit = payload->count < kNearCatalogMax ? payload->count : kNearCatalogMax;
  NearToken* list = static_cast<NearToken*>(arena.Alloc(sizeof(NearToken) * (limit ? limit : 1)));
  if (!list) return false;
  size_t used = 0;
  for (size_t i = 0; i < limit; ++i) {
    const Value* token = AsRecord(payload->items[i]);
    const Value* asset_id = StringValue(Field(token, "assetId"));
    const Value* chain = StringValue(Field(token, "blockchain"));
    const Value* symbol = StringValue(Field(token, "symbol"));
    const double decimals = JsNumber(Field(token, "decimals"));
    const Value* raw_contract = Field(token, "contractAddress");
    const Value* contract = Nullish(raw_contract) ? nullptr : StringValue(raw_contract);
    if (!asset_id || !chain || !symbol || !isfinite(decimals) || trunc(decimals) != decimals || decimals < 0 ||
        decimals > 36 || (!Nullish(raw_contract) && !contract))
      continue;
    NearToken& entry = list[used];
    entry.asset_id = tj::Clone(arena, asset_id);
    entry.blockchain = Lower(arena, chain);
    entry.symbol = tj::Clone(arena, symbol);
    entry.decimals = static_cast<uint32_t>(decimals);
    entry.contract = contract ? tj::Clone(arena, contract) : nullptr;
    if (!entry.asset_id || !entry.blockchain || !entry.symbol || (contract && !entry.contract)) return false;
    ++used;
  }
  *tokens = list;
  *count = used;
  return true;
}

const char* near_auth(const ProviderContext& context) {
  return context.settings->near_jwt_configured ? context.settings->near_authorization : nullptr;
}

// resolveAssets(): false -> no route; `*outcome` carries a thrown error.
bool near_resolve(ProviderContext& context, Arena& arena, const QuoteRequest& request, double now, NearAsset* source,
                  NearAsset* destination, Outcome* outcome) {
  *outcome = Outcome::Ok();
  if (!near_valid_address(request.from.chain_id, request.from.address) ||
      !near_valid_address(request.to.chain_id, request.to.address))
    return false;
  NearCatalog& catalog = *context.near_catalog;
  fortis::base::UniqueLock lock(catalog.mutex);
  if (catalog.loaded && catalog.expires_at > now) {
    const bool found = catalog_asset(arena, catalog.tokens, catalog.count, request.from, source) &&
                       catalog_asset(arena, catalog.tokens, catalog.count, request.to, destination);
    return found;
  }
  lock.Unlock();
  Fetch fetch;
  fetch.lane = kNearProvider;
  fetch.base = kNearBase;
  fetch.path = "/v0/tokens";
  fetch.header_name = near_auth(context) ? "authorization" : nullptr;
  fetch.header_value = near_auth(context);
  fetch.timeout_ms = 12000;
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) { *outcome = fetch.error(); return false; }
  tj::Arena fresh = tj::GrowingArena(16 * 1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> fresh_blocks(&fresh);
  NearToken* tokens = nullptr;
  size_t count = 0;
  if (!parse_catalog(fresh, fetch.value, &tokens, &count) || !fresh.ok) {
    tj::FreeArena(fresh);
    *outcome = Error();
    return false;
  }
  if (!count) { tj::FreeArena(fresh); return false; }
  lock.Lock();
  tj::Arena old = catalog.arena;
  const bool had = catalog.loaded;
  catalog.arena = *fresh_blocks.release();
  catalog.tokens = tokens;
  catalog.count = count;
  catalog.loaded = true;
  catalog.expires_at = now + 5 * 60000;
  const bool found = catalog_asset(arena, catalog.tokens, catalog.count, request.from, source) &&
                     catalog_asset(arena, catalog.tokens, catalog.count, request.to, destination);
  lock.Unlock();
  if (had) tj::FreeArena(old);
  return found;
}

const Value* route_symbol(Arena& arena, const Endpoint& endpoint, const NearAsset& asset) {
  return Is(endpoint.chain_id, "near") && lower_is(arena, endpoint.token, "wrap.near") ? str(arena, "NEAR") : asset.symbol;
}

Value* parse_near_route(Arena& arena, const Value* payload, const QuoteRequest& request, double now,
                        const NearAsset& source, const NearAsset& destination) {
  const Value* root = AsRecord(payload);
  const Value* quote = AsRecord(Field(root, "quote"));
  const Value* echoed = AsRecord(Field(root, "quoteRequest"));
  if (!root || !quote || !echoed) return nullptr;
  const char* amount_in = SafePositiveInteger(arena, tj::Get(quote, "amountIn"));
  const char* amount_out = SafePositiveInteger(arena, tj::Get(quote, "amountOut"));
  const char* amount_out_min = SafePositiveInteger(arena, tj::Get(quote, "minAmountOut"));
  const bool prepared = request.prepare;
  const Value* deposit_address = StringValue(tj::Get(quote, "depositAddress"));
  const Value* raw_memo = tj::Get(quote, "depositMemo");
  const Value* deposit_memo = Nullish(raw_memo) ? nullptr : StringValue(raw_memo);
  const Value* refund_to = JsString(arena, tj::Get(echoed, "refundTo"));
  const Value* recipient = JsString(arena, tj::Get(echoed, "recipient"));
  if (!refund_to || !recipient) return nullptr;
  if (!amount_in || !Is(request.amount_in, amount_in) || !amount_out || !amount_out_min ||
      CompareDecimal(amount_out_min, amount_out) > 0 ||
      (prepared && (!deposit_address || !near_valid_address(request.from.chain_id, deposit_address))) ||
      (prepared && !Nullish(raw_memo) && (!deposit_memo || Units(deposit_memo) > 256)) ||
      !SameText(tj::Get(echoed, "originAsset"), source.asset_id) ||
      !SameText(tj::Get(echoed, "destinationAsset"), destination.asset_id) ||
      !SameText(tj::Get(echoed, "amount"), request.amount_in) || !Is(tj::Get(echoed, "swapType"), "EXACT_INPUT") ||
      !Is(tj::Get(echoed, "depositType"), "ORIGIN_CHAIN") || !Is(tj::Get(echoed, "refundType"), "ORIGIN_CHAIN") ||
      !Is(tj::Get(echoed, "recipientType"), "DESTINATION_CHAIN") ||
      !SameText(near_normalized(arena, request.from.chain_id, refund_to),
                near_normalized(arena, request.from.chain_id, request.from.address)) ||
      !SameText(near_normalized(arena, request.to.chain_id, recipient),
                near_normalized(arena, request.to.chain_id, request.to.address)) ||
      JsNumber(tj::Get(echoed, "slippageTolerance")) != static_cast<double>(request.slippage_bps))
    return nullptr;
  const char* reported_at = SafeTimestamp(arena, tj::Get(root, "timestamp"));
  double reported_ms = now;
  if (reported_at && !DateParse(reported_at, strlen(reported_at), &reported_ms)) return nullptr;
  Validity prepared_validity, passive_validity;
  if (!QuoteValidity(arena,
                     Coalesce(Coalesce(tj::Get(quote, "deadline"), tj::Get(quote, "timeWhenInactive")),
                              tj::Get(echoed, "deadline")),
                     reported_ms, 5 * 60000, &prepared_validity))
    return nullptr;
  if (!prepared && !QuoteValidity(arena, nullptr, now, 60000, &passive_validity)) return nullptr;
  const bool evm_source = prepared && deposit_address && ValidEvmAddress(deposit_address);
  ExecutionFields execution;
  execution.memo_key = true;
  if (prepared) {
    execution.kind = evm_source ? "evm-transfer" : "deposit";
    execution.to = deposit_address;
    execution.memo = deposit_memo;
    execution.data = evm_source ? str(arena, "0x") : nullptr;
    execution.value = request.amount_in;
    execution.gas_limit = evm_source ? str(arena, "21000") : nullptr;
  }
  Value* steps = tj::Arr(arena);
  Value* step = tj::Obj(arena);
  if (!steps || !step || !SetStr(arena, step, "kind", "bridge") || !SetStr(arena, step, "tool", "NEAR Intents 1Click") ||
      !SetVal(arena, step, "fromChainId", request.from.chain_id) || !SetVal(arena, step, "toChainId", request.to.chain_id) ||
      !SetVal(arena, step, "fromToken", route_symbol(arena, request.from, source)) ||
      !SetVal(arena, step, "toToken", route_symbol(arena, request.to, destination)) || !tj::Push(arena, steps, step))
    return nullptr;
  RouteFields f;
  f.provider_id = kNearIntentsProvider.Id();
  f.provider_name = kNearIntentsProvider.Name();
  f.provider_url = kNearIntentsProvider.Website();
  f.provider_quote_id = prepared && deposit_address ? near_normalized(arena, request.from.chain_id, deposit_address)
                                                    : nullptr;
  f.bridge_id = str(arena, "near-intents-1click");
  f.bridge = str(arena, "NEAR Intents 1Click");
  f.bridge_url = BridgeWebsiteUrl(arena, f.bridge_id);
  f.risk_key = "near-intents-1click";
  f.custody = kNearIntentsProvider.Custody();
  f.amount_in = request.amount_in;
  f.amount_out = amount_out;
  f.amount_out_min = amount_out_min;
  f.decimals_in = request.from.decimals;
  f.decimals_out = request.to.decimals;
  const Value* eta[] = {tj::Get(quote, "timeEstimate")};
  f.has_eta = SafeEta(eta, 1, &f.eta_seconds);
  const Validity& validity = prepared ? prepared_validity : passive_validity;
  f.quoted_at = validity.quoted_at;
  f.expires_at = validity.expires_at;
  f.steps = steps;
  f.execution = Execution(arena, execution);
  if (!f.execution) return nullptr;
  return Route(arena, f);
}

struct NearDestination {
  bool ok = false;
  const Value* token = nullptr;
  const Value* symbol = nullptr;
  uint32_t decimals = 0;
};

NearDestination near_native_destination(Arena& arena, const Value* chain_id) {
  static const struct { const char* chain; const char* token; const char* symbol; uint32_t decimals; } kMap[] = {
      {"bitcoin", "bitcoin", "BTC", 8},
      {"bitcoin-cash", "bitcoin-cash", "BCH", 8},
      {"dash", "dash", "DASH", 8},
      {"zcash", "zcash", "ZEC", 8},
      {"1", kNativeEvm, "ETH", 18},
      {"8453", kNativeEvm, "ETH", 18},
      {"56", kNativeEvm, "BNB", 18},
      {"aptos", kAptCoin, "APT", 8},
      {"near", "wrap.near", "NEAR", 24},
      {"solana-mainnet-beta", "11111111111111111111111111111111", "SOL", 9},
      {"ton", "ton", "GRAM", 9}};
  NearDestination out;
  for (const auto& entry : kMap)
    if (Is(chain_id, entry.chain)) {
      out.ok = true;
      out.token = str(arena, entry.token);
      out.symbol = str(arena, entry.symbol);
      out.decimals = entry.decimals;
    }
  return out;
}

NearDestination near_status_destination(Arena& arena, const StatusRequest& request) {
  NearDestination fallback = near_native_destination(arena, request.to_chain_id);
  if (!request.to_symbol || !request.has_to_decimals) return fallback;
  const Value* token = request.to_token ? request.to_token : fallback.ok ? fallback.token : nullptr;
  NearDestination out;
  if (!token) return out;
  out.ok = true;
  out.token = token;
  out.symbol = request.to_symbol;
  out.decimals = request.to_decimals;
  return out;
}

struct NearTx {
  const Value* hash = nullptr;
  const Value* url = nullptr;
};

// safeHttps(): new URL(value), kept only for https: and serialized.
const Value* safe_https(Arena& arena, const Value* value) {
  if (!value) return nullptr;
  auto scratch_storage = base::AllocateStorage<compat::url::UrlScratch>(sizeof(compat::url::UrlScratch));
  auto* scratch = scratch_storage.get();
  auto parsed_storage = base::AllocateStorage<compat::url::Url>(sizeof(compat::url::Url));
  auto* parsed = parsed_storage.get();
  const Value* result = nullptr;
  if (scratch && parsed) {
    size_t clean_length = 0;
    const char* clean = WellFormed(arena, value->text, value->length, &clean_length);
    if (clean && compat::url::Parse(clean, clean_length, nullptr, parsed, scratch)) {
      const compat::url::View protocol = compat::url::Protocol(*parsed);
      if (protocol.length == 6 && memcmp(protocol.data, "https:", 6) == 0) {
        const compat::url::View href = compat::url::Href(*parsed);
        result = tj::Str(arena, href.data, href.length);
      }
    }
  }
  scratch_storage.reset();
  parsed_storage.reset();
  return result;
}

NearTx first_transaction(Arena& arena, const Value* value) {
  NearTx out;
  if (!value || value->type != tj::Type::kArray || value->count == 0) return out;
  const Value* first = value->items[0];
  if (first->type == tj::Type::kString) { out.hash = StringValue(first); return out; }
  const Value* tx = AsRecord(first);
  out.hash = StringValue(Coalesce(Field(tx, "hash"), Field(tx, "txHash")));
  out.url = safe_https(arena, StringValue(Coalesce(Field(tx, "explorerUrl"), Field(tx, "url"))));
  return out;
}

Value* near_step(Arena& arena, const char* kind, const Value* label, const char* state, const Value* chain_id,
                 const Value* tx_hash, const Value* explorer) {
  char id[48];
  snprintf(id, sizeof(id), "near-intents-%s", kind);
  Value* step = ProgressStep(arena, id, kind, label, state, chain_id, tx_hash);
  const bool problem = !strcmp(state, "failed") || !strcmp(state, "needs-action");
  if (!step || !SetVal(arena, step, "explorerUrl", explorer) || !SetStr(arena, step, "errorCode", problem ? state : nullptr))
    return nullptr;
  return step;
}

Value* parse_near_status(Arena& arena, const Value* payload, const StatusRequest& request) {
  const Value* root = AsRecord(payload);
  const Value* details = AsRecord(Field(root, "swapDetails"));
  const Value* raw_status = StringValue(Field(root, "status"));
  const Value* status = raw_status ? Upper(arena, raw_status) : str(arena, "PENDING_DEPOSIT");
  if (!status) return nullptr;
  const char* state = Is(status, "SUCCESS") ? "completed" : Is(status, "REFUNDED") ? "refunded"
                      : Is(status, "FAILED") ? "failed" : Is(status, "INCOMPLETE_DEPOSIT") ? "needs-action" : "pending";
  const NearTx source = first_transaction(arena, Field(details, "originChainTxHashes"));
  const NearTx destination_tx = first_transaction(arena, Field(details, "destinationChainTxHashes"));
  const Value* source_hash = source.hash ? source.hash : request.tx_hash;
  const char* updated_at = SafeTimestamp(arena, Field(root, "updatedAt"));
  const char* quoted_at = SafeTimestamp(arena, Field(Field(root, "quoteResponse"), "timestamp"));
  const Value* deposit_address = request.provider_quote_id;
  const Value* explorer = nullptr;
  if (deposit_address && near_valid_address(request.from_chain_id, deposit_address)) {
    Text url;
    if (!url.Init(arena, deposit_address->length * 3 + 96)) return nullptr;
    url.out.Text(Is(status, "PENDING_DEPOSIT") ? "https://explorer.near-intents.org/?search="
                                               : "https://explorer.near-intents.org/transactions/");
    if (!EncodeComponent(deposit_address, url.out)) return nullptr;
    const char* text = url.Finish();
    if (!text) return nullptr;
    explorer = str(arena, text);
  }
  const char* amount_out = SafePositiveInteger(arena, Field(details, "amountOut"));
  const NearDestination asset = near_status_destination(arena, request);
  const Value* symbol = asset.ok ? asset.symbol : nullptr;
  // statusMessage(status, symbol = 'assets').
  const Value* message_symbol = symbol ? symbol : str(arena, "assets");
  Value* message = nullptr;
  if (Is(status, "PENDING_DEPOSIT")) message = str(arena, "Waiting for the source-chain deposit.");
  else if (Is(status, "KNOWN_DEPOSIT_TX")) message = str(arena, "Deposit found; waiting for confirmation.");
  else if (Is(status, "PROCESSING")) message = concat(arena, "Swapping and delivering ", message_symbol, " to the destination wallet.");
  else if (Is(status, "SUCCESS")) message = concat(arena, "", message_symbol, " was delivered to the destination wallet.");
  else if (Is(status, "INCOMPLETE_DEPOSIT")) message = str(arena, "The deposited amount did not match the quote.");
  else if (Is(status, "REFUNDED")) message = str(arena, "The source deposit was refunded.");
  else if (Is(status, "FAILED")) message = str(arena, "The intent could not be completed.");
  else message = str(arena, "The bridge is processing the deposit.");
  double elapsed = 0;
  const bool has_elapsed = SecondsBetween(quoted_at, updated_at, &elapsed);
  Value* received = tj::Null(arena);
  if (!strcmp(state, "completed") && amount_out && asset.ok) {
    received = tj::Obj(arena);
    if (!received || !SetVal(arena, received, "chainId", request.to_chain_id) ||
        !SetStr(arena, received, "amount", amount_out) || !SetVal(arena, received, "tokenAddress", asset.token) ||
        !SetVal(arena, received, "symbol", asset.symbol) || !SetNum(arena, received, "decimals", asset.decimals))
      return nullptr;
  }
  // progressForStatus(..., destinationSymbol = native symbol ?? 'Assets').
  const Value* destination_symbol = symbol;
  if (!destination_symbol) {
    const NearDestination native = near_native_destination(arena, request.to_chain_id);
    destination_symbol = native.ok ? native.symbol : str(arena, "Assets");
  }
  const bool deposited = !Is(status, "PENDING_DEPOSIT");
  const bool complete = Is(status, "SUCCESS");
  const char* bridge_state = complete ? "completed" : Is(status, "INCOMPLETE_DEPOSIT") ? "needs-action"
                             : Is(status, "FAILED") ? "failed" : Is(status, "REFUNDED") ? "refunded"
                             : deposited ? "pending" : "not-started";
  Value* progress = tj::Arr(arena);
  Value* source_step = near_step(arena, "source", str(arena, "Deposit sent"), deposited ? "completed" : "pending",
                                 request.from_chain_id, source_hash, source.url);
  Value* bridge_step = near_step(arena, "bridge", str(arena, "Swap and bridge"), bridge_state, nullptr, nullptr, nullptr);
  Value* destination_step = near_step(arena, "destination", concat(arena, "", destination_symbol, " received"),
                                      complete ? "completed" : "not-started", request.to_chain_id, destination_tx.hash,
                                      destination_tx.url);
  if (!progress || !source_step || !bridge_step || !destination_step || !tj::Push(arena, progress, source_step) ||
      !tj::Push(arena, progress, bridge_step) || !tj::Push(arena, progress, destination_step))
    return nullptr;
  if (Is(status, "REFUNDED")) {
    Value* refund = near_step(arena, "refund", str(arena, "Deposit refunded"), "refunded", request.from_chain_id,
                              nullptr, nullptr);
    if (!refund || !tj::Push(arena, progress, refund)) return nullptr;
  }
  Value* recovery = tj::Null(arena);
  if (Is(status, "PENDING_DEPOSIT") || Is(status, "KNOWN_DEPOSIT_TX") || Is(status, "PROCESSING")) {
    recovery = Recovery(arena, "wait", str(arena, "Bridge in progress"),
                        str(arena, "Keep this transaction in your history while settlement completes."),
                        explorer ? str(arena, "Track intent") : nullptr, explorer);
  } else if (Is(status, "REFUNDED")) {
    recovery = Recovery(arena, "track-refund", str(arena, "Deposit refunded"),
                        str(arena, "Check the source wallet for the returned native coin."),
                        explorer ? str(arena, "Track refund") : nullptr, explorer);
  } else if (Is(status, "INCOMPLETE_DEPOSIT") || Is(status, "FAILED")) {
    recovery = Recovery(arena, "contact-support",
                        str(arena, Is(status, "INCOMPLETE_DEPOSIT") ? "Deposit amount mismatch" : "Bridge failed"),
                        str(arena, "Open the intent tracker and contact support with the deposit address."),
                        explorer ? str(arena, "Open tracker") : nullptr, explorer);
  }
  Value* out = tj::Obj(arena);
  if (!message || !recovery || !received || !out || !SetStr(arena, out, "providerId", kNearProvider) ||
      !SetStr(arena, out, "state", state) || !SetVal(arena, out, "substatus", status) ||
      !SetVal(arena, out, "message", message) || !SetVal(arena, out, "sourceHash", source_hash) ||
      !SetVal(arena, out, "destinationHash", destination_tx.hash) ||
      !SetVal(arena, out, "providerExplorerUrl", explorer) || !SetVal(arena, out, "bridgeExplorerUrl", explorer) ||
      !SetNumOrNull(arena, out, "elapsedSeconds", has_elapsed, elapsed) ||
      !tj::Set(arena, out, "actualReceived", received) || !tj::Set(arena, out, "progress", progress) ||
      !tj::Set(arena, out, "recovery", recovery) ||
      !tj::Set(arena, out, "providerData", BoundedProviderSnapshot(arena, root ? root : nullptr)))
    return nullptr;
  return out;
}

}  // namespace
}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome NearIntentsProvider::Quote(ProviderContext& context, Arena& arena, const QuoteRequest& request, Value** routes) const {
  using namespace detail;
  const double now = context.Now();
  NearAsset source, destination;
  Outcome outcome;
  if (!near_resolve(context, arena, request, now, &source, &destination, &outcome)) {
    if (outcome.failure != Failure::kNone) return outcome;
    *routes = Routes(arena, nullptr);
    return *routes ? Outcome::Ok() : Error();
  }
  const char* deadline = IsoText(arena, now + 10 * 60000);
  Value* body = tj::Obj(arena);
  if (!deadline || !body || !SetBool(arena, body, "dry", !request.prepare) ||
      !SetStr(arena, body, "swapType", "EXACT_INPUT") || !SetNum(arena, body, "slippageTolerance", request.slippage_bps) ||
      !SetVal(arena, body, "originAsset", source.asset_id) || !SetStr(arena, body, "depositType", "ORIGIN_CHAIN") ||
      !SetVal(arena, body, "destinationAsset", destination.asset_id) || !SetVal(arena, body, "amount", request.amount_in) ||
      !SetVal(arena, body, "refundTo", near_normalized(arena, request.from.chain_id, request.from.address)) ||
      !SetStr(arena, body, "refundType", "ORIGIN_CHAIN") ||
      !SetVal(arena, body, "recipient", near_normalized(arena, request.to.chain_id, request.to.address)) ||
      !SetStr(arena, body, "recipientType", "DESTINATION_CHAIN") || !SetStr(arena, body, "deadline", deadline) ||
      !SetNum(arena, body, "quoteWaitingTimeMs", 5000))
    return Error();
  Fetch fetch;
  fetch.lane = kNearProvider;
  fetch.base = kNearBase;
  fetch.path = "/v0/quote";
  fetch.post = true;
  fetch.body = body;
  fetch.header_name = near_auth(context) ? "authorization" : nullptr;
  fetch.header_value = near_auth(context);
  fetch.timeout_ms = 15000;
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  *routes = Routes(arena, parse_near_route(arena, fetch.value, request, now, source, destination));
  return *routes ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
Outcome NearIntentsProvider::Status(ProviderContext& context, Arena& arena, const StatusRequest& request, Value** status) const {
  using namespace detail;
  const Value* deposit = request.provider_quote_id;
  if (!deposit || !near_valid_address(request.from_chain_id, deposit)) return Error();
  Text path;
  if (!path.Init(arena, value_bytes(deposit) + value_bytes(request.deposit_memo) + 64)) return Error();
  path.out.Text("/v0/status?");
  QueryPair pairs[] = {{"depositAddress", deposit}, {"depositMemo", request.deposit_memo}};
  FormEncode(pairs, request.deposit_memo ? 2 : 1, path.out);
  Fetch fetch;
  fetch.lane = kNearProvider;
  fetch.base = kNearBase;
  fetch.path = path.Finish();
  fetch.header_name = near_auth(context) ? "authorization" : nullptr;
  fetch.header_value = near_auth(context);
  fetch.timeout_ms = 12000;
  if (!fetch.path) return Error();
  RunFetch(context, arena, fetch);
  if (!fetch.ok()) return fetch.error();
  *status = parse_near_status(arena, fetch.value, request);
  return *status ? Outcome::Ok() : Error();
}
}  // namespace fortis::exchange::cross_chain

namespace fortis::exchange::cross_chain::detail {


}  // namespace fortis::exchange::cross_chain::detail

namespace fortis::exchange::cross_chain {
const NearIntentsProvider kNearIntentsProvider{};

}  // namespace fortis::exchange::cross_chain
