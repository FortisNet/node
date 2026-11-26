#include <algorithm>
#include "ownership.h"
#include "address_facets_internal.h"

#include "address_overview.h"
#include "json_document_writer.h"
#include "proto_wire.h"
#include "kv_store.h"
#include "media_registry.h"
#include "threading.h"
#include "token_ref.h"
#include "provider_text.h"
#include "whatwg_url.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// The TypeScript NetworkOverviewFacet over the network overview profiles,
// the GeckoTerminal client and the TokenHolderSource composition
// (EvmTokenHolderSource: Ethplorer for eth-mainnet, Etherscan V2 elsewhere;
// DriverTokenHolderSource: Tron with a TronScan key, Cardano via Koios).
namespace fortis::wallet::address {
namespace {

namespace jj = fortis::codec::json_document_writer;
namespace pw = fortis::codec::proto_wire;
using chains::OutBuffer;

struct Profile {
  const char* id;
  const char* name;
  const char* aliases[4];
  const char* website;
};

constexpr Profile kProfiles[] = {
    {"bitcoin", "Bitcoin", {"Bitcoin", "Bitcoin network"}, "https://bitcoin.org"},
    {"bitcoin-cash", "Bitcoin Cash", {"Bitcoin Cash", "BCH network"}, "https://bitcoincash.org"},
    {"eth-mainnet", "Ethereum", {"Ethereum", "Ethereum mainnet"}, "https://ethereum.org"},
    {"bnb", "BNB Chain", {"BNB Chain", "BNB Smart Chain", "BSC"}, "https://www.bnbchain.org"},
    {"solana", "Solana", {"Solana", "Solana network"}, "https://solana.com"},
    {"xrpl", "XRP Ledger", {"XRP Ledger", "XRPL"}, "https://xrpl.org"},
    {"hedera", "Hedera", {"Hedera", "Hedera network"}, "https://hedera.com"},
    {"cardano", "Cardano", {"Cardano", "Cardano network"}, "https://cardano.org"},
    {"stellar", "Stellar", {"Stellar", "Stellar network"}, "https://stellar.org"},
    {"algorand", "Algorand", {"Algorand", "Algorand network"}, "https://algorand.co"},
    {"base", "Base", {"Base mainnet", "Base network", "Base chain", "Coinbase L2"}, "https://base.org"},
    {"polygon", "Polygon", {"Polygon", "Polygon PoS"}, "https://polygon.technology"},
    {"avalanche", "Avalanche", {"Avalanche", "Avalanche C-Chain"}, "https://www.avax.network"},
    {"cronos", "Cronos", {"Cronos", "Cronos chain"}, "https://cronos.org"},
    {"arbitrum", "Arbitrum", {"Arbitrum", "Arbitrum One"}, "https://arbitrum.io"},
    {"optimism", "Optimism", {"Optimism", "OP Mainnet"}, "https://www.optimism.io"},
    {"hyperevm", "HyperEVM", {"HyperEVM", "Hyperliquid EVM"}, "https://hyperliquid.xyz"},
    {"robinhood", "Robinhood Chain", {"Robinhood Chain"}, nullptr},
    {"sei", "Sei", {"Sei network", "Sei blockchain", "Sei v2"}, "https://www.sei.io"},
    {"xdc", "XDC Network", {"XDC Network", "XinFin"}, "https://xdc.org"},
    {"flare", "Flare", {"Flare network", "Flare blockchain"}, "https://flare.network"},
    {"dogecoin", "Dogecoin", {"Dogecoin", "Dogecoin network"}, "https://dogecoin.com"},
    {"dash", "Dash", {"Dash network", "Dash blockchain"}, "https://www.dash.org"},
    {"litecoin", "Litecoin", {"Litecoin", "Litecoin network"}, "https://litecoin.org"},
    {"zcash", "Zcash", {"Zcash", "Zcash network"}, "https://z.cash"},
    {"sui", "Sui", {"Sui network", "Sui blockchain", "Sui mainnet"}, "https://sui.io"},
    {"cosmos", "Cosmos Hub", {"Cosmos Hub", "Cosmos network"}, "https://cosmos.network"},
    {"near", "NEAR", {"NEAR Protocol", "NEAR network"}, "https://near.org"},
    {"tron", "TRON", {"TRON network", "TRON blockchain"}, "https://tron.network"},
    {"vechain", "VeChain", {"VeChain", "VeChainThor"}, "https://www.vechain.org"},
    {"aptos", "Aptos", {"Aptos", "Aptos network"}, "https://aptosfoundation.org"},
    {"bittensor", "Bittensor", {"Bittensor", "Bittensor network"}, "https://bittensor.com"},
    {"icp", "Internet Computer", {"Internet Computer", "ICP network"}, "https://internetcomputer.org"},
};

struct GeckoId {
  const char* network;
  const char* gecko;
};
constexpr GeckoId kGeckoIds[] = {
    {"eth-mainnet", "eth"}, {"bnb", "bsc"},      {"solana", "solana"},   {"base", "base"},
    {"polygon", "polygon_pos"}, {"avalanche", "avax"}, {"cronos", "cro"},   {"arbitrum", "arbitrum"},
    {"optimism", "optimism"}, {"hyperevm", "hyperevm"}, {"robinhood", "robinhood"}, {"sei", "sei-network"},
    {"xdc", "xdc"},          {"flare", "flare"},  {"tron", "tron"},       {"cardano", "cardano"},
    {"aptos", "aptos"},
};

// Etherscan V2 tokenholdercount chain coverage.
constexpr double kEtherscanHolderChains[] = {1, 10, 50, 56, 137, 999, 1329, 8453, 42161, 43114};

const Profile* profile_for(const char* id) {
  for (const Profile& p : kProfiles)
    if (!strcmp(p.id, id)) return &p;
  return nullptr;
}

const char* gecko_id(const char* network) {
  for (const GeckoId& g : kGeckoIds)
    if (!strcmp(g.network, network)) return g.gecko;
  return nullptr;
}

bool popular_family(chains::Family f) {
  return f == chains::Family::kEvm || f == chains::Family::kSolana || f == chains::Family::kTron ||
         f == chains::Family::kAlgorand || f == chains::Family::kCardano || f == chains::Family::kAptos;
}

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

bool ascii_alnum(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

uint8_t ascii_lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? static_cast<uint8_t>(c - 'A' + 'a') : c; }

// /(?:^|[^A-Za-z0-9])alias(?:$|[^A-Za-z0-9])/i over UTF-8: the alias is
// ASCII and non-ASCII characters never case-fold to ASCII without `u`.
bool contains_phrase(const char* text, size_t length, const char* phrase) {
  const size_t n = strlen(phrase);
  for (size_t i = 0; i + n <= length; ++i) {
    if (i > 0 && ascii_alnum(static_cast<uint8_t>(text[i - 1]))) continue;
    bool match = true;
    for (size_t j = 0; j < n && match; ++j)
      match = ascii_lower(static_cast<uint8_t>(text[i + j])) == ascii_lower(static_cast<uint8_t>(phrase[j]));
    if (!match) continue;
    if (i + n < length && ascii_alnum(static_cast<uint8_t>(text[i + n]))) continue;
    return true;
  }
  return false;
}

// isNetworkNews(candidate, profile).
bool network_news(const Value* item, const Profile& profile) {
  const Value* topics = tj::Get(item, "topics");
  bool chain_topic = false;
  for (uint32_t i = 0; tj::IsArray(topics) && i < topics->count && !chain_topic; ++i)
    chain_topic = chains::Equals(topics->items[i], "chains") || chains::Equals(topics->items[i], "protocol") ||
                  chains::Equals(topics->items[i], "security");
  if (!chain_topic) return false;
  const Value* headline = tj::Get(item, "headline");
  const Value* summary = tj::Get(item, "summary");
  const size_t a = tj::IsString(headline) ? headline->length : 0, b = tj::IsString(summary) ? summary->length : 0;
  auto text_storage = base::AllocateStorage<char[]>(a + b + 2);
  auto* text = text_storage.get();
  if (!text) return false;
  if (a) memcpy(text, headline->text, a);
  text[a] = ' ';
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  if (b) memcpy(text + a + 1, summary->text, b);
  bool found = false;
  for (const char* alias : profile.aliases)
    if (alias && contains_phrase(text, a + b + 1, alias)) { found = true; break; }
  text_storage.reset();
  return found;
}

// text(value): a string trimmed (String.prototype.trim), else "". Lengths
// are carried, so an escaped NUL inside a provider string survives.
Value* trimmed(Arena& arena, const Value* v) {
  if (!tj::IsString(v)) return tj::Str(arena, "", 0);
  size_t b = 0, e = 0;
  fortis::codec::provider_text::Trim(v->text, v->length, &b, &e);
  return tj::Str(arena, v->text + b, e - b);
}

bool same_text(const Value* a, const Value* b) {
  return tj::IsString(a) && tj::IsString(b) && a->length == b->length && !memcmp(a->text, b->text, a->length);
}

bool finite_of(const Value* v, double* out) {
  double parsed = NAN;
  if (tj::IsNumber(v)) parsed = v->number;
  else if (tj::IsString(v)) parsed = fortis::codec::provider_text::ToNumber(v->text, v->length);
  if (!isfinite(parsed)) return false;
  *out = parsed;
  return true;
}

bool hex_only(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
  return true;
}

bool base58_only(const char* s, size_t n) {
  static const char kAlphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  for (size_t i = 0; i < n; ++i)
    if (std::find(kAlphabet, kAlphabet + 58, s[i]) == kAlphabet + 58 || !s[i]) return false;
  return true;
}

// overviewTokenAddress(network, address): null where the app has no grammar.
Value* overview_address(Arena& arena, const Network& network, const char* raw, size_t raw_length) {
  size_t b = 0, e = 0;
  fortis::codec::provider_text::Trim(raw, raw_length, &b, &e);
  const char* value = raw + b;
  const size_t n = e - b;
  if (!n) return nullptr;
  switch (network.family) {
    case chains::Family::kEvm: {
      if (n != 42 || value[0] != '0' || value[1] != 'x' || !hex_only(value + 2, 40)) return nullptr;
      return fortis::codec::provider_text::ToLowerCase(arena, value, n);
    }
    case chains::Family::kSolana:
      return n >= 32 && n <= 44 && base58_only(value, n) ? tj::Str(arena, value, n) : nullptr;
    case chains::Family::kTron:
      return n == 34 && value[0] == 'T' && base58_only(value + 1, 33) ? tj::Str(arena, value, n) : nullptr;
    case chains::Family::kAlgorand: {
      if (n < 1 || n > 20) return nullptr;
      for (size_t i = 0; i < n; ++i) if (value[i] < '0' || value[i] > '9') return nullptr;
      chains::BigInt id;
      if (!chains::BigInt::ParseDecimal(value, n, &id)) return nullptr;
      return id.ToValue(arena);
    }
    case chains::Family::kCardano: {
      if (n < 56 || !hex_only(value, n)) return nullptr;
      return fortis::codec::provider_text::ToLowerCase(arena, value, n);
    }
    case chains::Family::kAptos:
      return fortis::codec::provider_text::Utf16Length(value, n) <= 300 ? tj::Str(arena, value, n) : nullptr;
    default:
      return nullptr;
  }
}

const char* overview_token_prefix(chains::Family family) {
  switch (family) {
    case chains::Family::kEvm: return "erc20:";
    case chains::Family::kSolana: return "spl:";
    case chains::Family::kTron: return "trc20:";
    case chains::Family::kAlgorand: return "asa:";
    case chains::Family::kCardano: return "cardano:";
    case chains::Family::kAptos: return "aptos:";
    default: return nullptr;
  }
}

// new URL(raw): https only, else null (url.toString()).
const char* https_url(Arena& arena, const char* raw, size_t length) {
  if (!length) return nullptr;
  auto url_storage = base::AllocateStorage<fortis::compat::url::Url>(sizeof(fortis::compat::url::Url));
  auto* url = url_storage.get();
  auto scratch_storage = base::AllocateStorage<fortis::compat::url::UrlScratch>(sizeof(fortis::compat::url::UrlScratch));
  auto* scratch = scratch_storage.get();
  const char* out = nullptr;
  if (url && scratch) {
    new (url) fortis::compat::url::Url();
    if (fortis::compat::url::Parse(raw, length, nullptr, url, scratch)) {
      const fortis::compat::url::View protocol = fortis::compat::url::Protocol(*url);
      if (protocol.length == 6 && !memcmp(protocol.data, "https:", 6)) {
        const fortis::compat::url::View href = fortis::compat::url::Href(*url);
        out = arena.CopyText(href.data, href.length);
      }
    }
  }
  url_storage.reset();
  scratch_storage.reset();
  return out;
}

}  // namespace

// GeckoTerminalClient.trending's projection of one trending_pools body.
bool OverviewProjectTrending(const Network& network, const Value* body, Arena& arena, Value** rows) {
  Value* out = tj::Arr(arena);
  if (!out) return false;
  *rows = out;
  if (!gecko_id(network.id)) return true;
  const Value* root = tj::IsObject(body) ? body : nullptr;
  const Value* pools = root ? tj::Get(root, "data") : nullptr;
  const Value* included = root ? tj::Get(root, "included") : nullptr;
  for (uint32_t index = 0; tj::IsArray(pools) && index < pools->count; ++index) {
    const Value* pool = tj::IsObject(pools->items[index]) ? pools->items[index] : nullptr;
    const Value* attributes = tj::IsObject(tj::Get(pool, "attributes")) ? tj::Get(pool, "attributes") : nullptr;
    const Value* relationships = tj::IsObject(tj::Get(pool, "relationships")) ? tj::Get(pool, "relationships") : nullptr;
    const Value* base_token = tj::IsObject(tj::Get(relationships, "base_token")) ? tj::Get(relationships, "base_token") : nullptr;
    const Value* base_data = tj::IsObject(tj::Get(base_token, "data")) ? tj::Get(base_token, "data") : nullptr;
    const Value* base_id = trimmed(arena, tj::Get(base_data, "id"));
    if (!base_id) return false;
    // tokens.get(id): the LAST included token resource with that id wins.
    const Value* token = nullptr;
    for (uint32_t i = 0; tj::IsArray(included) && i < included->count; ++i) {
      const Value* resource = tj::IsObject(included->items[i]) ? included->items[i] : nullptr;
      const Value* id = trimmed(arena, tj::Get(resource, "id"));
      if (!id) return false;
      if (id->length && chains::Equals(tj::Get(resource, "type"), "token") && same_text(id, base_id)) token = resource;
    }
    const Value* token_attributes = tj::IsObject(tj::Get(token, "attributes")) ? tj::Get(token, "attributes") : nullptr;
    Value* address = trimmed(arena, tj::Get(token_attributes, "address"));
    const Value* name_raw = trimmed(arena, tj::Get(token_attributes, "name"));
    const Value* symbol_raw = trimmed(arena, tj::Get(token_attributes, "symbol"));
    if (!address || !name_raw || !symbol_raw) return false;
    Value* name = chains::SanitizeName(arena, name_raw->text, name_raw->length);
    Value* symbol_clean = chains::SanitizeSymbol(arena, symbol_raw->text, symbol_raw->length);
    Value* symbol = symbol_clean ? fortis::codec::provider_text::ToUpperCase(arena, symbol_clean->text, symbol_clean->length) : nullptr;
    double decimals = 0, market_cap = 0;
    const bool has_decimals = finite_of(tj::Get(token_attributes, "decimals"), &decimals) && decimals >= 0 &&
                              trunc(decimals) == decimals && decimals <= 9007199254740991.0;
    const bool has_cap = finite_of(tj::Get(attributes, "market_cap_usd"), &market_cap) && market_cap > 0;
    bool seen = false;
    for (uint32_t r = 0; r < out->count && !seen; ++r) seen = same_text(tj::Get(out->items[r], "address"), address);
    if (!address->length || !name || !name->length || !symbol || !symbol->length || !has_decimals || decimals > 255 ||
        !has_cap || seen)
      continue;
    const Value* change = tj::IsObject(tj::Get(attributes, "price_change_percentage"))
                              ? tj::Get(attributes, "price_change_percentage")
                              : nullptr;
    double price = 0, change24 = 0;
    const bool has_price = finite_of(tj::Get(attributes, "base_token_price_usd"), &price) && price >= 0;
    const bool has_change = finite_of(tj::Get(change, "h24"), &change24);
    const Value* image = trimmed(arena, tj::Get(token_attributes, "image_url"));
    const Value* coin_raw = trimmed(arena, tj::Get(token_attributes, "coingecko_coin_id"));
    if (!image || !coin_raw) return false;
    const char* logo = https_url(arena, image->text, image->length);
    Value* coin = fortis::codec::provider_text::ToLowerCase(arena, coin_raw->text, coin_raw->length);
    Value* row = tj::Obj(arena);
    if (!row || !tj::Set(arena, row, "address", address) || !tj::Set(arena, row, "name", name) ||
        !tj::Set(arena, row, "symbol", symbol) || !tj::Set(arena, row, "decimals", tj::Num(arena, decimals)) ||
        !tj::Set(arena, row, "logoUrl", logo ? tj::Str(arena, logo) : tj::Null(arena)) ||
        !tj::Set(arena, row, "coinGeckoId", coin && coin->length ? coin : tj::Null(arena)) ||
        !tj::Set(arena, row, "marketCapUsd", tj::Num(arena, market_cap)) ||
        !tj::Set(arena, row, "priceUsd", has_price ? tj::Num(arena, price) : tj::Null(arena)) ||
        !tj::Set(arena, row, "change24hPct", has_change ? tj::Num(arena, change24) : tj::Null(arena)) ||
        !tj::Set(arena, row, "providerRank", tj::Num(arena, index + 1)) || !tj::Push(arena, out, row))
      return false;
  }
  return true;
}

namespace {

// GeckoTerminalClient.trending(network): the lane call, then the projection.
bool trending(WalletQueryContext* impl, const Network& network, Arena& arena, Value** rows, chains::Error* error) {
  const char* gecko = gecko_id(network.id);
  if (!gecko) return OverviewProjectTrending(network, nullptr, arena, rows) || (error->Upstream("arena"), false);
  static const char* const kBase[] = {"https://api.geckoterminal.com"};
  const char* encoded = chains::EncodeUri(arena, gecko);
  Value* path = encoded ? chains::Format(arena, "/api/v2/networks/%s/trending_pools?include=base_token&duration=24h&page=1",
                                        encoded)
                        : nullptr;
  if (!path) { error->Upstream("arena"); return false; }
  chains::Call call;
  call.kind = chains::CallKind::kGet;
  call.urls = kBase;
  call.url_count = 1;
  call.path = path->text;
  call.lane = "geckoterminal";
  call.headers[0] = chains::CallHeader{"accept", "application/json;version=20230203"};
  call.header_count = 1;
  impl->deps.transport->Run(call, arena);
  if (!call.ok()) {
    call.ToError(error);
    if (call.answer()) error->Upstream("geckoterminal %d", call.http_status);
    return false;
  }
  if (!OverviewProjectTrending(network, call.value, arena, rows)) { error->Upstream("arena"); return false; }
  return true;
}

}  // namespace

// ---- holder counts (TokenHolderSource) -------------------------------------------------

namespace {

// Number(value) for a JSON value: null 0, booleans 0/1, strings by
// StringToNumber, an array through its join (one element, or none),
// objects NaN.
double js_number(const Value* v, bool in_array = false) {
  if (!v) return NAN;
  switch (v->type) {
    case tj::Type::kNull: return 0;
    case tj::Type::kTrue: return in_array ? NAN : 1;
    case tj::Type::kFalse: return in_array ? NAN : 0;
    case tj::Type::kNumber: return v->number;
    case tj::Type::kString: return fortis::codec::provider_text::ToNumber(v->text, v->length);
    case tj::Type::kArray:
      if (v->count == 0) return 0;
      return v->count == 1 ? js_number(v->items[0], true) : NAN;
    default: return NAN;
  }
}

bool contains_free_api(const Value* detail) {
  static const char kNeedle[] = "free api access is not supported for this chain";
  const size_t needle = sizeof(kNeedle) - 1;
  for (size_t i = 0; tj::IsString(detail) && i + needle <= detail->length; ++i) {
    bool same = true;
    for (size_t k = 0; k < needle && same; ++k) {
      char c = detail->text[i + k];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      same = c == kNeedle[k];
    }
    if (same) return true;
  }
  return false;
}

}  // namespace

bool WalletOverviewBuilder::EtherscanHolderUnsupported(double chain_id) {
  const auto id = chains::ChainId::FromNumber(chain_id);
  return id && etherscan_unsupported_.Contains(*id);
}

void WalletOverviewBuilder::RememberEtherscanHolderUnsupported(double chain_id) {
  // Holder requests are gated by kEtherscanHolderChains (small integer IDs).
  if (const auto id = chains::ChainId::FromNumber(chain_id)) etherscan_unsupported_.Insert(*id);
}

bool WalletOverviewBuilder::HolderFetch(const Network& network, const char* token_id, const char* address, Arena& arena,
                               Value** value, chains::Error* error, bool check_memo) {
  (void)token_id;
  chains::Context ctx = context_.ChainContext(arena);
  if (network.family == chains::Family::kEvm) {
    const bool ethplorer = !strcmp(network.id, "eth-mainnet");
    // EtherscanHolderClient.holders: `if (!this.supports(chainId)) return null`
    // (a revalidation after the chain was found uncovered).
    if (!ethplorer && check_memo && EtherscanHolderUnsupported(network.evm_chain_id)) {
      *value = tj::Null(arena);
      return true;
    }
    Value* lowered = fortis::codec::provider_text::ToLowerCase(arena, address, strlen(address));
    const char* encoded = lowered ? chains::EncodeUri(arena, lowered->text) : nullptr;
    if (!encoded) { error->Upstream("arena"); return false; }
    chains::Call call;
    call.kind = chains::CallKind::kGet;
    call.url_count = 1;
    const char* source = nullptr;
    if (ethplorer) {
      static const char* const kEthplorer[] = {"https://api.ethplorer.io"};
      call.urls = kEthplorer;
      Value* path = chains::Format(arena, "/getTokenInfo/%s?apiKey=%s", encoded,
                                  context_.deps.config->ethplorer_api_key_configured ? context_.deps.config->ethplorer_api_key : "freekey");
      call.path = path ? path->text : nullptr;
      call.lane = "ethplorer";
      source = "ethplorer";
    } else {
      static const char* const kEtherscan[] = {"https://api.etherscan.io/v2/api"};
      call.urls = kEtherscan;
      char chain_id[32];
      chains::JsNumberText(network.evm_chain_id, chain_id, sizeof(chain_id));
      // new URLSearchParams({chainid, module, action, contractaddress, apikey}).
      Value* path = chains::Format(arena, "?chainid=%s&module=token&action=tokenholdercount&contractaddress=%s&apikey=%s",
                                  chain_id, encoded, context_.deps.config->etherscan_api_key);
      call.path = path ? path->text : nullptr;
      call.lane = "etherscan";
      source = "etherscan";
    }
    context_.deps.transport->Run(call, arena);
    if (!call.ok()) {
      call.ToError(error);
      return false;
    }
    // `body.holdersCount` / `body.status` on a JSON null body throws.
    if (!call.value || call.value->type == tj::Type::kNull) {
      error->Upstream("TypeError: Cannot read properties of null");
      return false;
    }
    Value* count = tj::Null(arena);
    if (ethplorer) {
      const Value* holders = tj::Get(call.value, "holdersCount");
      if (tj::IsNumber(holders)) count = tj::Num(arena, holders->number);
    } else {
      const Value* status = tj::Get(call.value, "status");
      const Value* result = tj::Get(call.value, "result");
      if (!chains::Equals(status, "1")) {
        // body.result?.trim(): a non-string, non-nullish result throws too.
        if (contains_free_api(result)) {
          RememberEtherscanHolderUnsupported(network.evm_chain_id);
          *value = tj::Null(arena);
          return true;
        }
        error->Upstream("Etherscan tokenholdercount: NOTOK");
        return false;
      }
      // Number(body.result) (undefined: NaN), a non-negative safe integer.
      const double parsed = result ? js_number(result) : NAN;
      if (isfinite(parsed) && trunc(parsed) == parsed && parsed >= 0 && parsed <= 9007199254740991.0)
        count = tj::Num(arena, parsed);
    }
    if (count->type == tj::Type::kNull) {
      *value = count;
      return true;
    }
    Value* out = tj::Obj(arena);
    if (!out || !tj::Set(arena, out, "count", count) || !tj::Set(arena, out, "atLeast", tj::Bool(arena, false)) ||
        !tj::Set(arena, out, "source", tj::Str(arena, source))) {
      error->Upstream("arena");
      return false;
    }
    *value = out;
    return true;
  }
  if (network.family == chains::Family::kTron) return chains::TronTokenHolders(ctx, network, address, value, error);
  if (network.family == chains::Family::kCardano) return chains::CardanoTokenHolders(ctx, network, address, value, error);
  *value = tj::Null(arena);
  return true;
}

namespace {

struct HolderContext {
  const Network* network;
  const char* token_id;
  const char* address;
  // Whether the fetch re-checks the per-chain Etherscan memo. A foreground
  // overview decides support for all of its tokens before any request (the
  // TypeScript Promise.all issues every request before the first answer).
  bool check_memo;
};

bool holder_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  auto* c = static_cast<HolderContext*>(opaque);
  return impl->overview.HolderFetch(*c->network, c->token_id, c->address, arena, value, error, c->check_memo);
}

Revalidation* holder_revalidation(void* opaque, const char* key) {
  auto* c = static_cast<HolderContext*>(opaque);
  auto* job = fortis::base::New<Revalidation>();
  if (!job) return nullptr;
  job->kind = Revalidation::Kind::kHolders;
  job->key.Assign(key);
  job->network.Assign(c->network->id);
  job->subject.Assign(c->token_id);
  job->extra.Assign(c->address);
  return job;
}

}  // namespace

Value* WalletOverviewBuilder::OverviewHolderCount(const Network& network, const char* token_id, const char* address,
                                         Arena& arena, bool etherscan_uncovered) {
  // parseTokenRef(network, address) and source.supports(network, ref).
  networks::TokenNetwork resolved;
  if (!ResolveTokenNetwork(network.id, strlen(network.id), &resolved)) return nullptr;
  networks::TokenRef ref;
  if (!ParseTokenRef(&resolved, address, strlen(address), &ref)) return nullptr;
  bool supported = false;
  if (network.family == chains::Family::kEvm) {
    if (ref.kind == networks::TokenRefKind::kErc20) {
      if (!strcmp(network.id, "eth-mainnet")) supported = true;
      else if (context_.deps.config->etherscan_api_key_configured && network.has_evm_chain_id && !etherscan_uncovered)
        for (double id : kEtherscanHolderChains) supported = supported || id == network.evm_chain_id;
    }
  } else if (network.family == chains::Family::kTron) {
    supported = ref.has_address && chains::TronTokenHoldersPresent(*context_.deps.config);
  } else if (network.family == chains::Family::kCardano) {
    supported = ref.has_address;
  }
  if (!supported) return nullptr;
  char key[1200];
  if (snprintf(key, sizeof(key), "tf:%s:%s:holders", network.id, token_id) >= static_cast<int>(sizeof(key))) return nullptr;
  HolderContext context{&network, token_id, ref.has_address ? ref.address : address, false};
  SwrFetcher fetcher;
  fetcher.fetch = holder_fetch;
  fetcher.context = &context;
  fetcher.revalidation = holder_revalidation;
  SwrResult swr;
  if (!context_.SwrRead(key, kTtlTokenHolders, fetcher, arena, &swr)) return nullptr;  // .catch(() => null)
  return tj::IsObject(swr.value) ? swr.value : nullptr;
}

bool WalletOverviewBuilder::OverviewTokens(const Network& network, Arena& arena, Value** rows, chains::Error* error) {
  Value* trending_rows = nullptr;
  if (!trending(&context_, network, arena, &trending_rows, error)) return false;
  const char* prefix = overview_token_prefix(network.family);
  struct Selected {
    const Value* row;
    Value* token_id;
    Value* address;
  };
  Selected selected[5];
  size_t count = 0;
  const size_t prefix_length = prefix ? strlen(prefix) : 0;
  for (uint32_t i = 0; prefix && i < trending_rows->count && count < 5; ++i) {
    const Value* row = trending_rows->items[i];
    const Value* raw = tj::Get(row, "address");
    Value* address = overview_address(arena, network, raw->text, raw->length);
    if (!address) continue;
    char* text = static_cast<char*>(arena.Alloc(prefix_length + address->length + 1, 1));
    if (!text) { error->Upstream("arena"); return false; }
    memcpy(text, prefix, prefix_length);
    memcpy(text + prefix_length, address->text, address->length);
    text[prefix_length + address->length] = '\0';
    Value* token_id = tj::Str(arena, text, prefix_length + address->length);
    if (!token_id) { error->Upstream("arena"); return false; }
    bool seen = false;
    for (size_t s = 0; s < count && !seen; ++s) seen = same_text(selected[s].token_id, token_id);
    if (!seen) selected[count++] = Selected{row, token_id, address};
  }
  Value* out = tj::Arr(arena);
  if (!out) { error->Upstream("arena"); return false; }
  const int64_t now = context_.Now();
  // Promise.all([media.register(...), ...holderCount]): every token's support
  // is decided before the first holder request, so a "free api access" answer
  // only stops the tokens of later overviews.
  const bool etherscan_uncovered = network.family == chains::Family::kEvm && network.has_evm_chain_id &&
                                   EtherscanHolderUnsupported(network.evm_chain_id);
  for (size_t i = 0; i < count; ++i) {
    const Value* row = selected[i].row;
    const Value* logo = tj::Get(row, "logoUrl");
    Value* media_key = tj::Null(arena);
    if (tj::IsString(logo)) {
      char key[fortis::discovery::market_view::kMediaKeyChars + 1];
      if (discovery::RegisterMediaSource(context_.deps.kv, logo->text, logo->length, now, key)) media_key = tj::Str(arena, key);
    }
    Value* holders = OverviewHolderCount(network, selected[i].token_id->text, selected[i].address->text, arena,
                                         etherscan_uncovered);
    const Value* holder_count = holders ? tj::Get(holders, "count") : nullptr;
    const Value* at_least = holders ? tj::Get(holders, "atLeast") : nullptr;
    Value* item = tj::Obj(arena);
    auto copy = [&](const char* to, const char* from) {
      const Value* v = tj::Get(row, from);
      return tj::Set(arena, item, to, v ? tj::Clone(arena, v) : tj::Null(arena));
    };
    if (!item || !tj::Set(arena, item, "tokenId", selected[i].token_id) ||
        !tj::Set(arena, item, "address", selected[i].address) || !copy("symbol", "symbol") ||
        !copy("name", "name") || !copy("decimals", "decimals") || !tj::Set(arena, item, "logoMediaKey", media_key) ||
        !copy("logoUrl", "logoUrl") || !copy("coinGeckoId", "coinGeckoId") || !copy("marketCapUsd", "marketCapUsd") ||
        !tj::Set(arena, item, "holderCount", tj::IsNumber(holder_count) ? tj::Num(arena, holder_count->number) : tj::Null(arena)) ||
        !tj::Set(arena, item, "holderCountAtLeast", tj::Bool(arena, at_least && at_least->type == tj::Type::kTrue)) ||
        !copy("priceUsd", "priceUsd") || !copy("change24hPct", "change24hPct") || !copy("rank", "providerRank") ||
        !tj::Set(arena, item, "source", tj::Str(arena, "coingecko")) || !tj::Push(arena, out, item)) {
      error->Upstream("arena");
      return false;
    }
  }
  *rows = out;
  return true;
}

namespace {

struct TokensContext {
  const Network* network;
};

bool tokens_fetch(WalletQueryContext* impl, void* opaque, Arena& arena, Value** value, chains::Error* error) {
  return impl->overview.OverviewTokens(*static_cast<TokensContext*>(opaque)->network, arena, value, error);
}

Revalidation* tokens_revalidation(void* opaque, const char* key) {
  auto* c = static_cast<TokensContext*>(opaque);
  auto* job = fortis::base::New<Revalidation>();
  if (!job) return nullptr;
  job->kind = Revalidation::Kind::kOverviewTokens;
  job->key.Assign(key);
  job->network.Assign(c->network->id);
  return job;
}

Value* popular_section(Arena& arena, bool available, Value* items, bool has_as_of, int64_t as_of, bool stale,
                       bool has_source) {
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "available", tj::Bool(arena, available)) ||
      !tj::Set(arena, out, "items", items ? items : tj::Arr(arena)) ||
      !tj::Set(arena, out, "asOf", has_as_of ? chains::IsoValue(arena, as_of) : tj::Null(arena)) ||
      !tj::Set(arena, out, "stale", tj::Bool(arena, stale)) ||
      !tj::Set(arena, out, "metric", tj::Str(arena, "on_chain_holders")) ||
      !tj::Set(arena, out, "source", has_source ? tj::Str(arena, "coingecko") : tj::Null(arena)))
    return nullptr;
  return out;
}

}  // namespace

void RunOverviewRevalidation(WalletQueryContext* impl, Revalidation* job, const Network& network, Arena& arena) {
  SwrFetcher fetcher;
  if (job->kind == Revalidation::Kind::kOverviewTokens) {
    TokensContext context{&network};
    fetcher.fetch = tokens_fetch;
    fetcher.context = &context;
    impl->Revalidate(job->key.data(), kTtlNetworkOverview, fetcher, arena);
    return;
  }
  HolderContext context{&network, job->subject.data(), job->extra.data(), true};
  fetcher.fetch = holder_fetch;
  fetcher.context = &context;
  impl->Revalidate(job->key.data(), kTtlTokenHolders, fetcher, arena);
}

Value* OverviewTokenAddress(Arena& arena, const Network& network, const char* raw, size_t length) {
  return overview_address(arena, network, raw, length);
}

bool OverviewIsNetworkNews(const Value* item, const char* network_id) {
  const Profile* profile = profile_for(network_id);
  return profile && network_news(item, *profile);
}

bool OverviewSupportsNews(const Network& network) {
  return !network.testnet && profile_for(network.id) != nullptr;
}

bool OverviewSupportsPopularTokens(const Network& network) {
  return !network.testnet && profile_for(network.id) && popular_family(network.family) && gecko_id(network.id);
}

FacetReply WalletOverviewBuilder::Build(Arena& arena, const char* network_id) {
  FacetReply result;
  auto resolved_network = context_.Resolve(network_id);
  if (const auto* failure = std::get_if<FacetReply>(&resolved_network)) return *failure;
  Network& network = std::get<Network>(resolved_network);
  const Profile* profile = profile_for(network.id);
  if (!profile || network.testnet) {
    result.outcome = Outcome::kBadRequest;
    snprintf(result.detail, sizeof(result.detail), "network overview is unavailable: %s", network_id);
    return result;
  }
  // Promise.all([networkNews, popularTokens]): the news feed may throw (502).
  Value* feed = nullptr;
  if (!context_.deps.news_feed || !context_.deps.news_feed(context_.deps.news_context, context_.Now(), arena, &feed) ||
      !tj::IsObject(feed)) {
    result.outcome = Outcome::kUpstream;
    return result;
  }
  Value* news_items = tj::Arr(arena);
  const Value* items = tj::Get(feed, "items");
  for (uint32_t i = 0; news_items && tj::IsArray(items) && i < items->count && news_items->count < 4; ++i)
    if (network_news(items->items[i], *profile) && !tj::Push(arena, news_items, items->items[i])) return result;
  const Value* feed_as_of = tj::Get(feed, "asOf");
  const Value* feed_stale = tj::Get(feed, "stale");
  Value* news = tj::Obj(arena);
  if (!news || !news_items || !tj::Set(arena, news, "available", tj::Bool(arena, true)) ||
      !tj::Set(arena, news, "items", news_items) ||
      !tj::Set(arena, news, "asOf", feed_as_of ? tj::Clone(arena, feed_as_of) : tj::Null(arena)) ||
      !tj::Set(arena, news, "stale", tj::Bool(arena, feed_stale && feed_stale->type == tj::Type::kTrue)))
    return result;
  Value* popular = nullptr;
  if (!popular_family(network.family) || !gecko_id(network.id)) {
    popular = popular_section(arena, false, nullptr, false, 0, false, false);
  } else {
    char key[256];
    snprintf(key, sizeof(key), "netoverview:v3:%s", network.id);
    TokensContext context{&network};
    SwrFetcher fetcher;
    fetcher.fetch = tokens_fetch;
    fetcher.context = &context;
    fetcher.revalidation = tokens_revalidation;
    SwrResult swr;
    if (!context_.SwrRead(key, kTtlNetworkOverview, fetcher, arena, &swr)) {
      // A provider outage must not blank the news card.
      popular = popular_section(arena, true, nullptr, false, 0, true, true);
    } else {
      popular = popular_section(arena, true, tj::Clone(arena, swr.value), true, swr.as_of_ms, swr.stale, true);
    }
  }
  Value* network_section = tj::Obj(arena);
  Value* description = chains::Format(arena, "%s network", profile->name);
  Value* out = tj::Obj(arena);
  if (!popular || !network_section || !description || !out ||
      !tj::Set(arena, network_section, "id", tj::Str(arena, network.id)) ||
      !tj::Set(arena, network_section, "name", tj::Str(arena, profile->name)) ||
      !tj::Set(arena, network_section, "description", description) ||
      !tj::Set(arena, network_section, "websiteUrl", profile->website ? tj::Str(arena, profile->website) : tj::Null(arena)) ||
      !tj::Set(arena, out, "network", network_section) || !tj::Set(arena, out, "news", news) ||
      !tj::Set(arena, out, "popularTokens", popular))
    return result;
  result.payload = out;
  result.outcome = Outcome::kOk;
  return result;
}
FacetReply WalletQueryService::Overview(Arena& arena, const char* network_id) {
  return impl_->overview.Build(arena, network_id);
}


// ---- protobuf (fortisnet.intelligence.v1 NetworkOverviewResponse) -------------------

namespace {

// A string field as protobuf-es writes it: UTF-8, a lone surrogate as U+FFFD.
void put_string(OutBuffer& out, uint32_t field, const Value* v, bool optional) {
  if (!tj::IsString(v) || (!optional && v->length == 0)) return;
  pw::PutBytes(out, field, v->text, v->length);
}

void put_bool(OutBuffer& out, uint32_t field, const Value* v) {
  if (v && v->type == tj::Type::kTrue) {
    pw::PutTag(out, field, 0);
    pw::PutVarint(out, 1);
  }
}

void put_uint(OutBuffer& out, uint32_t field, const Value* v, bool optional) {
  if (!tj::IsNumber(v) || v->number < 0) return;
  const uint64_t value = static_cast<uint64_t>(v->number);
  if (!optional && value == 0) return;
  pw::PutTag(out, field, 0);
  pw::PutVarint(out, value);
}

void put_double(OutBuffer& out, uint32_t field, const Value* v, bool optional) {
  if (!tj::IsNumber(v)) return;
  if (!optional && v->number == 0) return;
  pw::PutDouble(out, field, v->number);
}

void put_message(OutBuffer& out, uint32_t field, const OutBuffer& body) {
  pw::PutBytes(out, field, body.data, body.used);
}

struct Sub {
  std::unique_ptr<uint8_t[], base::StorageDeleter> data;
  OutBuffer out;
  explicit Sub(size_t capacity) : data(base::AllocateStorage<uint8_t[]>(capacity)), out{data.get(), data ? capacity : 0, 0, data != nullptr} {}
};

// An upper bound of the protobuf bytes any message built from `v` takes:
// every string at most its UTF-8 length (a lone surrogate's three bytes
// become U+FFFD's three), every scalar or nested header at most 16 bytes.
size_t encoded_bound(const Value* v) {
  if (!v) return 16;
  size_t total = 16;
  switch (v->type) {
    case tj::Type::kString: return total + v->length;
    case tj::Type::kArray:
      for (uint32_t i = 0; i < v->count; ++i) total += encoded_bound(v->items[i]);
      return total;
    case tj::Type::kObject:
      for (uint32_t i = 0; i < v->count; ++i) total += encoded_bound(v->members[i].value);
      return total;
    default: return total;
  }
}

// BigInt(holderCount) then the uint64 writer: a fraction or non-finite number
// throws RangeError, a negative or >= 2^64 one "invalid uint64".
bool valid_uint64(const Value* v) {
  return tj::IsNumber(v) && isfinite(v->number) && trunc(v->number) == v->number && v->number >= 0 &&
         v->number < 18446744073709551616.0;
}

bool news_item(OutBuffer& out, const Value* item) {
  put_string(out, 1, tj::Get(item, "id"), false);
  put_string(out, 2, tj::Get(item, "headline"), false);
  put_string(out, 3, tj::Get(item, "summary"), false);
  const Value* topics = tj::Get(item, "topics");
  for (uint32_t i = 0; tj::IsArray(topics) && i < topics->count; ++i) {
    // repeated string: every element, empty ones included.
    put_string(out, 4, topics->items[i], true);
  }
  put_string(out, 5, tj::Get(item, "impact"), false);
  const Value* tickers = tj::Get(item, "tickers");
  for (uint32_t i = 0; tj::IsArray(tickers) && i < tickers->count; ++i) {
    const Value* ticker = tickers->items[i];
    Sub body(encoded_bound(ticker));
    put_string(body.out, 1, tj::Get(ticker, "symbol"), false);
    const Value* coin = tj::Get(ticker, "coinId");
    if (!is_null(coin)) put_string(body.out, 2, coin, true);
    const Value* change = tj::Get(ticker, "change24hPct");
    if (!is_null(change)) put_double(body.out, 3, change, true);
    if (!body.out.ok) return false;
    put_message(out, 6, body.out);
  }
  put_string(out, 7, tj::Get(item, "publisher"), false);
  put_string(out, 8, tj::Get(item, "url"), false);
  const Value* image = tj::Get(item, "imageMediaKey");
  if (!is_null(image)) put_string(out, 9, image, true);
  put_string(out, 10, tj::Get(item, "publishedAt"), false);
  put_uint(out, 11, tj::Get(item, "sourceCount"), false);
  return out.ok;
}

}  // namespace

bool OverviewProtobuf(const Value* payload, OutBuffer& out, bool* refused) {
  if (refused) *refused = false;
  const Value* network = tj::Get(payload, "network");
  const Value* news = tj::Get(payload, "news");
  const Value* popular = tj::Get(payload, "popularTokens");
  // create(...) evaluates BigInt(holderCount) for every token before any
  // byte is written: such a payload has no protobuf form (500 INTERNAL).
  const Value* tokens = tj::Get(popular, "items");
  for (uint32_t i = 0; tj::IsArray(tokens) && i < tokens->count; ++i) {
    const Value* holders = tj::Get(tokens->items[i], "holderCount");
    if (!is_null(holders) && !valid_uint64(holders)) {
      if (refused) *refused = true;
      return false;
    }
  }
  {
    Sub body(encoded_bound(network));
    put_string(body.out, 1, tj::Get(network, "id"), false);
    put_string(body.out, 2, tj::Get(network, "name"), false);
    put_string(body.out, 3, tj::Get(network, "description"), false);
    const Value* site = tj::Get(network, "websiteUrl");
    if (!is_null(site)) put_string(body.out, 4, site, true);
    if (!body.out.ok) return false;
    put_message(out, 1, body.out);
  }
  {
    Sub body(encoded_bound(news));
    put_bool(body.out, 1, tj::Get(news, "available"));
    const Value* items = tj::Get(news, "items");
    for (uint32_t i = 0; tj::IsArray(items) && i < items->count; ++i) {
      Sub item(encoded_bound(items->items[i]));
      if (!news_item(item.out, items->items[i])) return false;
      put_message(body.out, 2, item.out);
    }
    const Value* as_of = tj::Get(news, "asOf");
    if (!is_null(as_of)) put_string(body.out, 3, as_of, true);
    put_bool(body.out, 4, tj::Get(news, "stale"));
    if (!body.out.ok) return false;
    put_message(out, 2, body.out);
  }
  {
    Sub body(encoded_bound(popular));
    put_bool(body.out, 1, tj::Get(popular, "available"));
    const Value* items = tj::Get(popular, "items");
    for (uint32_t i = 0; tj::IsArray(items) && i < items->count; ++i) {
      const Value* token = items->items[i];
      Sub item(encoded_bound(token));
      OutBuffer& t = item.out;
      put_string(t, 1, tj::Get(token, "tokenId"), false);
      put_string(t, 2, tj::Get(token, "address"), false);
      put_string(t, 3, tj::Get(token, "symbol"), false);
      put_string(t, 4, tj::Get(token, "name"), false);
      put_uint(t, 5, tj::Get(token, "decimals"), false);
      const char* const optional_strings[] = {"logoMediaKey", "logoUrl", "coinGeckoId"};
      for (uint32_t f = 0; f < 3; ++f) {
        const Value* v = tj::Get(token, optional_strings[f]);
        if (!is_null(v)) put_string(t, 6 + f, v, true);
      }
      put_double(t, 9, tj::Get(token, "marketCapUsd"), false);
      const Value* holders = tj::Get(token, "holderCount");
      if (!is_null(holders)) put_uint(t, 10, holders, true);
      put_bool(t, 11, tj::Get(token, "holderCountAtLeast"));
      const Value* price = tj::Get(token, "priceUsd");
      if (!is_null(price)) put_double(t, 12, price, true);
      const Value* change = tj::Get(token, "change24hPct");
      if (!is_null(change)) put_double(t, 13, change, true);
      put_uint(t, 14, tj::Get(token, "rank"), false);
      put_string(t, 15, tj::Get(token, "source"), false);
      if (!t.ok) return false;
      put_message(body.out, 2, t);
    }
    const Value* as_of = tj::Get(popular, "asOf");
    if (!is_null(as_of)) put_string(body.out, 3, as_of, true);
    put_bool(body.out, 4, tj::Get(popular, "stale"));
    put_string(body.out, 5, tj::Get(popular, "metric"), false);
    const Value* source = tj::Get(popular, "source");
    if (!is_null(source)) put_string(body.out, 6, source, true);
    if (!body.out.ok) return false;
    put_message(out, 3, body.out);
  }
  return out.ok;
}

}  // namespace fortis::wallet::address
