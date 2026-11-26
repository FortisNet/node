#include "chain_family_drivers.h"
#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// Sui (the TypeScript SuiDriver, SuiTransferSource and coin type
// normalization) over the network's GraphQL endpoints only: paginated
// `Address.balances` with bounded
// `coinMetadata` reads, and the newest-first `transactions(relation:
// AFFECTED)` walk whose records come from the netted `balanceChangesJson`
// effects plus address-owned object (NFT) moves. Query text is byte-identical
// to the TypeScript templates. Guest-safe.
namespace fortis::chains {
namespace {

constexpr const char* kSuiCoinType = "0x2::sui::SUI";
constexpr const char* kDeepBookBalanceManagerType =
    "0x2c8d603bc51326b8c13cef9dd07031a408a48dddb541963357661df5d3204809::balance_manager::BalanceManager";
constexpr size_t kMaxMetadataLookups = 20;
constexpr int kBalancePageSize = 50;
constexpr int kBalanceMaxPages = 20;
constexpr double kPageLimit = 50;
constexpr int kMaxPages = 4;
constexpr size_t kQueryBytes = 16384;
// canonicalSuiMoveType recursion bound (TypeScript has none; deeper generic
// nesting is returned trimmed, as for unrecognizable input).
constexpr int kMaxTypeDepth = 32;

bool hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

// canonicalSuiCoinType over [s, s+n) already trimmed; false = null.
bool coin_type_trimmed(const char* s, size_t n, OutBuffer& out) {
  if (n < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
  size_t h = 2;
  while (h < n && hex_char(s[h])) ++h;
  const size_t hex_length = h - 2;
  if (hex_length < 1 || hex_length > 64) return false;
  if (h + 2 > n || s[h] != ':' || s[h + 1] != ':') return false;
  const size_t m = h + 2;
  if (m >= n || !ident_start(s[m])) return false;
  size_t me = m + 1;
  while (me < n && ident_char(s[me])) ++me;
  if (me + 2 > n || s[me] != ':' || s[me + 1] != ':') return false;
  const size_t t = me + 2;
  if (t >= n || !ident_start(s[t])) return false;
  size_t te = t + 1;
  while (te < n && ident_char(s[te])) ++te;
  if (te != n) return false;
  size_t z = 2;
  while (z < h && s[z] == '0') ++z;
  out.Text("0x");
  if (z == h) out.Byte('0');
  for (; z < h; ++z) {
    const char c = s[z];
    out.Byte(static_cast<uint8_t>((c >= 'A' && c <= 'F') ? c - 'A' + 'a' : c));
  }
  out.Text("::");
  out.Append(s + m, me - m);
  out.Text("::");
  out.Append(s + t, te - t);
  return true;
}

void trim(const char* text, size_t length, const char** s, size_t* n) {
  size_t b = 0, e = 0;
  compat::token_text::Trim(text, length, &b, &e);
  *s = text + b;
  *n = e - b;
}

// canonicalSuiMoveType, appended to `out`.
void move_type(const char* text, size_t length, OutBuffer& out, int depth) {
  const char* s;
  size_t n;
  trim(text, length, &s, &n);
  const char* lt = static_cast<const char*>(memchr(s, '<', n));
  if (!lt) {
    const size_t before = out.used;
    if (!coin_type_trimmed(s, n, out)) { out.used = before; out.Append(s, n); }
    return;
  }
  if (n == 0 || s[n - 1] != '>' || depth >= kMaxTypeDepth) { out.Append(s, n); return; }
  const size_t head_length = static_cast<size_t>(lt - s);
  // canonicalSuiCoinType(trimmed.slice(0, lt)) trims the head again.
  const char* hs;
  size_t hn;
  trim(s, head_length, &hs, &hn);
  const size_t before = out.used;
  if (!coin_type_trimmed(hs, hn, out)) { out.used = before; out.Append(s, n); return; }
  out.Byte('<');
  // splitTopLevelTypeArgs(trimmed.slice(lt + 1, -1)).
  const char* args = lt + 1;
  const size_t args_length = n - head_length - 2;
  int level = 0;
  size_t start = 0;
  bool first = true;
  for (size_t i = 0; i <= args_length; ++i) {
    const bool end = i == args_length;
    const char c = end ? ',' : args[i];
    if (!end && c == '<') ++level;
    else if (!end && c == '>') --level;
    else if (c == ',' && (end || level == 0)) {
      if (!first) out.Text(", ");
      first = false;
      move_type(args + start, i - start, out, depth + 1);
      start = i + 1;
    }
  }
  out.Byte('>');
}

// An arena copy of canonicalSuiMoveType(text).
const char* canonical_move_type(Arena& a, const char* text, size_t length) {
  const size_t capacity = length * 2 + 64;
  auto* bytes = static_cast<uint8_t*>(a.Alloc(capacity, 1));
  if (!bytes) return nullptr;
  OutBuffer out{bytes, capacity, 0, true};
  move_type(text, length, out, 0);
  if (!out.ok) return nullptr;
  return a.CopyText(reinterpret_cast<const char*>(bytes), out.used);
}

// canonicalSuiCoinType(text) as an arena copy, or null.
const char* canonical_coin_type(Arena& a, const char* text, size_t length) {
  const char* s;
  size_t n;
  trim(text, length, &s, &n);
  uint8_t buffer[256];
  OutBuffer out{buffer, sizeof(buffer), 0, true};
  if (!coin_type_trimmed(s, n, out) || !out.ok) return nullptr;
  return a.CopyText(reinterpret_cast<const char*>(buffer), out.used);
}

bool same_move_type(Arena& a, const Value* v, const char* canonical) {
  if (!tj::IsString(v)) return false;
  const char* c = canonical_move_type(a, v->text, v->length);
  return c && strcmp(c, canonical) == 0;
}

// sameAddress(a, b): lowercase, strip /^0x0*/. `*failed` where TypeScript
// throws (a non-string reaching toLowerCase).
bool same_address(const Value* a, const char* b, bool* failed) {
  if (!a || a->type == tj::Type::kNull) return false;
  if (!tj::IsString(a)) { *failed = true; return false; }
  auto norm = [](const char* text, size_t length, size_t* begin) {
    // Lowercase first, then /^0x0*/ (an uppercase "0X" lowercases to "0x").
    size_t i = 0;
    if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      i = 2;
      while (i < length && text[i] == '0') ++i;
    }
    *begin = i;
  };
  const size_t bl = strlen(b);
  size_t ai = 0, bi = 0;
  norm(a->text, a->length, &ai);
  norm(b, bl, &bi);
  if (a->length - ai != bl - bi) return false;
  for (size_t i = 0; i < bl - bi; ++i) {
    char x = a->text[ai + i], y = b[bi + i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

bool digits(const char* text, size_t length, bool allow_sign) {
  size_t i = 0;
  if (allow_sign && length && text[0] == '-') i = 1;
  if (i >= length) return false;
  for (; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// JSON.stringify(text) appended to `out`.
void json_string(Arena& a, OutBuffer& out, const char* text) {
  Value* v = tj::Str(a, text);
  if (!v) { out.ok = false; return; }
  tj::Stringify(v, out);
}

const char* finish(Arena& a, uint8_t* bytes, OutBuffer& out) {
  if (!out.ok) return nullptr;
  return a.CopyText(reinterpret_cast<const char*>(bytes), out.used);
}

// A GraphQL POST {query} with HttpClient's read retry; `data` receives
// body.data, false with `error` where TypeScript throws.
bool graphql(Context& ctx, const Network& network, const char* query, const char* label, const Value** data,
             Error* error) {
  Arena& a = *ctx.arena;
  Value* body = tj::Obj(a);
  if (!body || !tj::Set(a, body, "query", tj::Str(a, query))) { error->Upstream("arena exhausted"); return false; }
  Call call;
  call.kind = CallKind::kPost;
  call.urls = network.graphql_urls;
  call.url_count = network.graphql_url_count;
  call.path = "";
  call.body = body;
  call.retry_post = true;
  ctx.transport->Run(call, a);
  if (!call.ok()) { call.ToError(error); return false; }
  if (!call.value || call.value->type == tj::Type::kNull) {
    error->Upstream("%s: null body", label);
    return false;
  }
  const Value* errors = tj::Get(call.value, "errors");
  if (tj::IsArray(errors) && errors->count > 0) {
    error->Upstream("%s: graphql errors", label);
    return false;
  }
  *data = tj::Get(call.value, "data");
  return true;
}

// ---- driver -------------------------------------------------------------------

bool sui_present(const config::ProviderRuntimeConfig&) { return true; }

bool valid_address(const char* address) {
  const size_t length = strlen(address);
  if (length < 3 || length > 66 || address[0] != '0' || address[1] != 'x') return false;
  for (size_t i = 2; i < length; ++i)
    if (!hex_char(address[i])) return false;
  return true;
}

}  // namespace

bool SuiDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*, size_t,
                  Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!valid_address(address)) {
    error->Range("not a Sui address: %s", address);
    return false;
  }
  // allBalances(): every Coin<T> the owner holds, page by page.
  constexpr size_t kMaxEntries = static_cast<size_t>(kBalancePageSize) * kBalanceMaxPages;
  const Value** entries = static_cast<const Value**>(a.Alloc(sizeof(Value*) * kMaxEntries * 2));
  if (!entries) { error->Upstream("arena exhausted"); return false; }
  const Value** others = entries + kMaxEntries;
  size_t entry_count = 0;
  const char* cursor = nullptr;
  bool done = false;
  for (int page = 0; page < kBalanceMaxPages && !done; ++page) {
    auto* bytes = static_cast<uint8_t*>(a.Alloc(kQueryBytes, 1));
    if (!bytes) { error->Upstream("arena exhausted"); return false; }
    OutBuffer q{bytes, kQueryBytes, 0, true};
    char size[16];
    snprintf(size, sizeof(size), "%d", kBalancePageSize);
    q.Text("query FortisBalances {\n        address(address: ");
    json_string(a, q, address);
    q.Text(") {\n          balances(first: ");
    q.Text(size);
    if (cursor) { q.Text(", after: "); json_string(a, q, cursor); }
    q.Text(") {\n            pageInfo { hasNextPage endCursor }\n            nodes { coinType { repr } totalBalance }\n"
           "          }\n        }\n      }");
    const char* query = finish(a, bytes, q);
    if (!query) { error->Upstream("query too long"); return false; }
    const Value* data = nullptr;
    if (!graphql(ctx, network, query, "Sui GraphQL balances", &data, error)) return false;
    const Value* connection = tj::Get(tj::Get(data, "address"), "balances");
    const Value* nodes = tj::Get(connection, "nodes");
    if (!connection || connection->type == tj::Type::kNull || !tj::IsArray(nodes)) {
      error->Upstream("Sui GraphQL balances: malformed response");
      return false;
    }
    for (uint32_t i = 0; i < nodes->count; ++i) {
      const Value* node = nodes->items[i];
      if (!node || node->type == tj::Type::kNull) { error->Upstream("Sui GraphQL balances: null node"); return false; }
      const Value* coin_type = tj::Get(tj::Get(node, "coinType"), "repr");
      const Value* total = tj::Get(node, "totalBalance");
      if (!tj::IsString(coin_type) || !tj::IsString(total)) continue;
      if (entry_count == kMaxEntries) {
        error->Upstream("Sui GraphQL balances: too many coins");
        return false;
      }
      entries[entry_count++] = node;
    }
    const Value* info = tj::Get(connection, "pageInfo");
    const Value* more = tj::Get(info, "hasNextPage");
    if (!more || more->type != tj::Type::kTrue) { done = true; break; }
    const Value* next = tj::Get(info, "endCursor");
    if (!tj::IsString(next) || next->length == 0 || (cursor && tj::StringEquals(next, cursor))) {
      error->Upstream("Sui GraphQL balances: invalid cursor");
      return false;
    }
    cursor = tj::CString(a, next);
  }
  if (!done) { error->Upstream("Sui GraphQL balances: pagination limit exceeded"); return false; }

  const char* native_raw = "0";
  size_t native_raw_length = 1;
  size_t other_count = 0;
  for (size_t i = 0; i < entry_count; ++i) {
    const Value* coin_type = tj::Get(tj::Get(entries[i], "coinType"), "repr");
    const Value* total = tj::Get(entries[i], "totalBalance");
    if (!digits(total->text, total->length, false)) continue;
    const char* canonical = canonical_coin_type(a, coin_type->text, coin_type->length);
    if (canonical && strcmp(canonical, kSuiCoinType) == 0) {
      native_raw = total->text;
      native_raw_length = total->length;
    } else {
      others[other_count++] = entries[i];
    }
  }

  const size_t lookups = other_count < kMaxMetadataLookups ? other_count : kMaxMetadataLookups;
  Value* bodies[kMaxMetadataLookups];
  Call calls[kMaxMetadataLookups];
  for (size_t i = 0; i < lookups; ++i) {
    const char* coin_type = tj::CString(a, tj::Get(tj::Get(others[i], "coinType"), "repr"));
    auto* bytes = static_cast<uint8_t*>(a.Alloc(kQueryBytes, 1));
    if (!coin_type || !bytes) { error->Upstream("arena exhausted"); return false; }
    OutBuffer q{bytes, kQueryBytes, 0, true};
    q.Text("query FortisCoinMetadata {\n      coinMetadata(coinType: ");
    json_string(a, q, coin_type);
    q.Text(") { decimals symbol name }\n    }");
    const char* query = finish(a, bytes, q);
    bodies[i] = tj::Obj(a);
    if (!query || !bodies[i] || !tj::Set(a, bodies[i], "query", tj::Str(a, query))) {
      error->Upstream("arena exhausted");
      return false;
    }
    calls[i].kind = CallKind::kPost;
    calls[i].urls = network.graphql_urls;
    calls[i].url_count = network.graphql_url_count;
    calls[i].path = "";
    calls[i].body = bodies[i];
    calls[i].retry_post = true;
  }
  if (lookups) ctx.transport->Run(calls, lookups, a);

  Value* tokens = tj::Arr(a);
  if (!tokens) { error->Upstream("arena exhausted"); return false; }
  for (size_t i = 0; i < lookups; ++i) {
    // Any failure (outage, in-band GraphQL error, malformed body) omits the
    // coin rather than mis-scaling it.
    if (!calls[i].ok() || !calls[i].value || calls[i].value->type == tj::Type::kNull) continue;
    const Value* errors = tj::Get(calls[i].value, "errors");
    if (tj::IsArray(errors) && errors->count > 0) continue;
    const Value* meta = tj::Get(tj::Get(calls[i].value, "data"), "coinMetadata");
    if (!meta || meta->type == tj::Type::kNull) continue;
    const Value* decimals = tj::Get(meta, "decimals");
    if (!tj::IsNumber(decimals)) continue;  // never mis-scale
    const Value* symbol = tj::Get(meta, "symbol");
    Value* symbol_value = nullptr;
    const bool truthy = symbol && !(symbol->type == tj::Type::kNull || symbol->type == tj::Type::kFalse ||
                                    (symbol->type == tj::Type::kString && symbol->length == 0) ||
                                    (symbol->type == tj::Type::kNumber && (symbol->number == 0 || isnan(symbol->number))));
    if (truthy) {
      if (!tj::IsString(symbol)) continue;  // sanitizeSymbol throws -> caught
      symbol_value = SanitizeSymbol(a, symbol->text, symbol->length);
    } else {
      symbol_value = tj::Null(a);
    }
    const Value* coin_type = tj::Get(tj::Get(others[i], "coinType"), "repr");
    const Value* total = tj::Get(others[i], "totalBalance");
    const char* canonical = canonical_move_type(a, coin_type->text, coin_type->length);
    const Value* token_id = canonical ? Format(a, "coin:%s", canonical) : nullptr;
    Value* token = tj::Obj(a);
    if (!token || !token_id || !symbol_value || !tj::Set(a, token, "tokenId", const_cast<Value*>(token_id)) ||
        !tj::Set(a, token, "balanceRaw", tj::Str(a, total->text, total->length)) ||
        !tj::Set(a, token, "decimals", tj::Num(a, decimals->number)) || !tj::Set(a, token, "symbol", symbol_value) ||
        !tj::Set(a, token, "source", tj::Str(a, "sui-graphql")) || !tj::Push(a, tokens, token)) {
      error->Upstream("arena exhausted");
      return false;
    }
  }

  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!native || !out || !tj::Set(a, native, "balanceRaw", tj::Str(a, native_raw, native_raw_length)) ||
      !tj::Set(a, native, "decimals", tj::Num(a, network.has_native ? network.native_decimals : 9)) ||
      !tj::Set(a, native, "symbol", tj::Str(a, network.has_native ? network.native_symbol : "SUI")) ||
      !tj::Set(a, native, "source", tj::Str(a, "sui-graphql")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", tokens)) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

// ---- transfer source ----------------------------------------------------------

bool sui_supports(const config::ProviderRuntimeConfig&, const Network& network) {
  return !network.testnet && network.graphql_url_count > 0;
}

constexpr const char kObjectNodes[] =
    "                  nodes {\n"
    "                    address\n"
    "                    idCreated\n"
    "                    inputState {\n"
    "                      owner {\n"
    "                        ... on AddressOwner { address { address } }\n"
    "                      }\n"
    "                    }\n"
    "                    outputState {\n"
    "                      owner {\n"
    "                        ... on AddressOwner { address { address } }\n"
    "                      }\n"
    "                      asMoveObject {\n"
    "                        contents {\n"
    "                          type { repr }\n"
    "                          display { output }\n"
    "                        }\n"
    "                      }\n"
    "                    }\n"
    "                  }\n";

const char* transactions_query(Arena& a, const char* address, double page_size, const char* before) {
  auto* bytes = static_cast<uint8_t*>(a.Alloc(kQueryBytes, 1));
  if (!bytes) return nullptr;
  OutBuffer q{bytes, kQueryBytes, 0, true};
  char size[32];
  JsNumberText(page_size, size, sizeof(size));
  q.Text("query FortisSuiTransfers {\n        address(address: ");
  json_string(a, q, address);
  q.Text(") {\n          transactions(last: ");
  q.Text(size);
  if (before) { q.Text(", before: "); json_string(a, q, before); }
  q.Text(", relation: AFFECTED) {\n"
         "            pageInfo { hasPreviousPage startCursor }\n"
         "            nodes {\n"
         "              digest\n"
         "              sender { address }\n"
         "              gasInput { gasSponsor { address } }\n"
         "              kind { __typename }\n"
         "              effects {\n"
         "                status\n"
         "                executionError { message }\n"
         "                timestamp\n"
         "                checkpoint { sequenceNumber }\n"
         "                balanceChangesJson\n"
         "                objectChanges(first: 100) {\n"
         "                  pageInfo { hasNextPage endCursor }\n");
  q.Text(kObjectNodes);
  q.Text("                }\n"
         "                gasEffects {\n"
         "                  gasSummary { computationCost storageCost storageRebate }\n"
         "                }\n"
         "              }\n"
         "            }\n"
         "          }\n"
         "        }\n"
         "      }");
  return finish(a, bytes, q);
}

const char* object_changes_query(Arena& a, const char* digest, const char* cursor) {
  auto* bytes = static_cast<uint8_t*>(a.Alloc(kQueryBytes, 1));
  if (!bytes) return nullptr;
  OutBuffer q{bytes, kQueryBytes, 0, true};
  q.Text("query FortisSuiObjectChanges {\n        transaction(digest: ");
  json_string(a, q, digest);
  q.Text(") {\n          effects {\n            objectChanges(first: 100, after: ");
  json_string(a, q, cursor);
  q.Text(") {\n"
         "              pageInfo { hasNextPage endCursor }\n"
         "              nodes {\n"
         "                address\n"
         "                idCreated\n"
         "                inputState { owner { ... on AddressOwner { address { address } } } }\n"
         "                outputState {\n"
         "                  owner { ... on AddressOwner { address { address } } }\n"
         "                  asMoveObject {\n"
         "                    contents { type { repr } display { output } }\n"
         "                  }\n"
         "                }\n"
         "              }\n"
         "            }\n"
         "          }\n"
         "        }\n"
         "      }");
  return finish(a, bytes, q);
}

// completeObjectChanges(): finish the nested object-change connection.
bool complete_object_changes(Context& ctx, const Network& network, Value* block, Error* error) {
  Arena& a = *ctx.arena;
  Value* connection = tj::Get(tj::Get(block, "effects"), "objectChanges");
  const Value* more = tj::Get(tj::Get(connection, "pageInfo"), "hasNextPage");
  if (!more || more->type != tj::Type::kTrue) return true;
  const Value* digest = tj::Get(block, "digest");
  if (!tj::IsString(digest)) return true;
  Value* existing = tj::Get(connection, "nodes");
  Value* nodes = tj::Arr(a);
  if (!nodes) { error->Upstream("arena exhausted"); return false; }
  if (tj::IsArray(existing))
    for (uint32_t i = 0; i < existing->count; ++i)
      if (!tj::Push(a, nodes, existing->items[i])) { error->Upstream("arena exhausted"); return false; }
  const Value* cursor = tj::Get(tj::Get(connection, "pageInfo"), "endCursor");
  if (!tj::IsString(cursor) || cursor->length == 0) {
    error->Upstream("Sui GraphQL object changes: invalid cursor");
    return false;
  }
  const char* digest_text = tj::CString(a, digest);
  const char* cursor_text = tj::CString(a, cursor);
  for (int page = 1; page < 100; ++page) {
    const char* query = object_changes_query(a, digest_text, cursor_text);
    if (!query) { error->Upstream("query too long"); return false; }
    const Value* data = nullptr;
    if (!graphql(ctx, network, query, "Sui GraphQL object changes", &data, error)) return false;
    Value* next_connection = tj::Get(tj::Get(tj::Get(data, "transaction"), "effects"), "objectChanges");
    const Value* next_nodes = tj::Get(next_connection, "nodes");
    if (!next_connection || next_connection->type == tj::Type::kNull || !tj::IsArray(next_nodes)) {
      error->Upstream("Sui GraphQL object changes: malformed response");
      return false;
    }
    for (uint32_t i = 0; i < next_nodes->count; ++i)
      if (!tj::Push(a, nodes, next_nodes->items[i])) { error->Upstream("arena exhausted"); return false; }
    Value* info = tj::Get(next_connection, "pageInfo");
    const Value* next_more = tj::Get(info, "hasNextPage");
    if (!next_more || next_more->type != tj::Type::kTrue) {
      if (!tj::Set(a, connection, "nodes", nodes) ||
          !(info ? tj::Set(a, connection, "pageInfo", info) : (tj::Remove(connection, "pageInfo"), true))) {
        error->Upstream("arena exhausted");
        return false;
      }
      return true;
    }
    const Value* next = tj::Get(info, "endCursor");
    if (!tj::IsString(next) || next->length == 0 || tj::StringEquals(next, cursor_text)) {
      error->Upstream("Sui GraphQL object changes: invalid cursor");
      return false;
    }
    cursor_text = tj::CString(a, next);
  }
  error->Upstream("Sui GraphQL object changes: pagination limit exceeded");
  return false;
}

struct Walked {
  Value* blocks = nullptr;
  const char* newest = nullptr;
  bool reached_seen = false;
  const char* next_before = nullptr;
};

bool walk_affected(Context& ctx, const Network& network, const char* address, const char* seen_digest,
                   double budget, const char* initial_before, Walked* out, Error* error) {
  Arena& a = *ctx.arena;
  out->blocks = tj::Arr(a);
  if (!out->blocks) { error->Upstream("arena exhausted"); return false; }
  const char* cursor = initial_before;
  for (int page = 0; page < kMaxPages && out->blocks->count < budget; ++page) {
    const double remaining = budget - out->blocks->count;
    const double page_size = kPageLimit < remaining ? kPageLimit : remaining;
    const char* query = transactions_query(a, address, page_size, cursor);
    if (!query) { error->Upstream("query too long"); return false; }
    const Value* data = nullptr;
    if (!graphql(ctx, network, query, "Sui GraphQL transactions", &data, error)) return false;
    const Value* connection = tj::Get(tj::Get(data, "address"), "transactions");
    const Value* nodes = tj::Get(connection, "nodes");
    if (!connection || connection->type == tj::Type::kNull || !tj::IsArray(nodes)) {
      error->Upstream("Sui GraphQL transactions: malformed response");
      return false;
    }
    // Relay returns this page oldest-to-newest; cursor ingestion is newest-first.
    for (uint32_t k = nodes->count; k-- > 0;) {
      Value* block = nodes->items[k];
      if (!block || block->type == tj::Type::kNull) {
        error->Upstream("Sui GraphQL transactions: null node");
        return false;
      }
      const Value* digest = tj::Get(block, "digest");
      if (!tj::IsString(digest)) continue;
      if (!out->newest) out->newest = tj::CString(a, digest);
      if (seen_digest && tj::StringEquals(digest, seen_digest)) {
        out->reached_seen = true;
        break;
      }
      if (out->blocks->count < budget) {
        if (!complete_object_changes(ctx, network, block, error)) return false;
        if (!tj::Push(a, out->blocks, block)) { error->Upstream("arena exhausted"); return false; }
      }
    }
    if (out->reached_seen) break;
    const Value* info = tj::Get(connection, "pageInfo");
    const Value* previous = tj::Get(info, "hasPreviousPage");
    if (!previous || previous->type != tj::Type::kTrue) {
      out->reached_seen = true;
      break;
    }
    const Value* next = tj::Get(info, "startCursor");
    if (!tj::IsString(next) || next->length == 0 || (cursor && tj::StringEquals(next, cursor))) {
      error->Upstream("Sui GraphQL transactions: invalid cursor");
      return false;
    }
    cursor = tj::CString(a, next);
  }
  out->next_before = out->reached_seen ? nullptr : cursor;
  return true;
}

// String.prototype.trim() !== '' for a string member.
const Value* nonblank(const Value* v) {
  if (!tj::IsString(v)) return nullptr;
  size_t b = 0, e = 0;
  compat::token_text::Trim(v->text, v->length, &b, &e);
  return e > b ? v : nullptr;
}

// decimalBigInt(value): String(value) digits -> BigInt, else 0.
bool decimal_big(const Value* v, BigInt* out) {
  *out = BigInt();
  if (!v) return true;  // undefined -> '0'
  if (tj::IsString(v)) {
    if (digits(v->text, v->length, false)) return BigInt::ParseDecimal(v->text, v->length, out);
    return true;
  }
  if (tj::IsNumber(v)) {
    char text[64];
    JsNumberText(v->number, text, sizeof(text));
    if (digits(text, strlen(text), false)) return BigInt::ParseDecimal(text, strlen(text), out);
  }
  return true;
}

// Object.keys(display).length === 0 for the JSON shapes GraphQL can return.
bool display_empty(const Value* display) {
  if (tj::IsObject(display) || tj::IsArray(display)) return display->count == 0;
  if (tj::IsString(display)) return display->length == 0;
  return true;  // numbers and booleans have no own keys
}

// netByType / gainersByType: per raw coin-type string, in first-insert order.
struct Net {
  const Value* coin_type;  // string
  BigInt amount;
};
struct Gainers {
  const Value* coin_type;
  const Value* first;  // the first distinct gaining address
  size_t count;        // distinct gaining addresses
};

bool same_text(const Value* a, const Value* b) {
  return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

// toRecords(network, address, block): appends to `records`.
bool to_records(Context& ctx, const Network& network, const char* address, const Value* block, Value* records,
                Error* error) {
  Arena& a = *ctx.arena;
  const Value* digest = tj::Get(block, "digest");
  if (!tj::IsString(digest)) return true;
  const Value* effects = tj::Get(block, "effects");
  const Value* status = tj::Get(effects, "status");
  const Value* timestamp = tj::Get(effects, "timestamp");
  if (!tj::IsString(status) || !tj::IsString(timestamp)) return true;
  const bool failed = !tj::StringEquals(status, "SUCCESS");
  const Value* sender_raw = tj::Get(tj::Get(block, "sender"), "address");
  const Value* sender = sender_raw && sender_raw->type != tj::Type::kNull ? sender_raw : nullptr;  // ?? null
  bool threw = false;
  const bool holder_sent = same_address(sender, address, &threw);
  double ts = 0;
  if (!ParseTime(timestamp->text, timestamp->length, &ts)) return true;  // Invalid Date: skipped
  const Value* kind_raw = tj::Get(tj::Get(block, "kind"), "__typename");
  const Value* kind = kind_raw && kind_raw->type != tj::Type::kNull ? kind_raw : nullptr;
  if (kind && !tj::IsString(kind)) threw = true;  // sanitizeText(non-string) throws
  const double checkpoint = JsNumber(tj::Get(tj::Get(effects, "checkpoint"), "sequenceNumber"));
  const bool has_checkpoint = isfinite(checkpoint);

  // Sponsored transactions bill the sponsor, never the affected recipient.
  const Value* gas = tj::Get(tj::Get(effects, "gasEffects"), "gasSummary");
  const Value* gas_payer = tj::Get(tj::Get(tj::Get(block, "gasInput"), "gasSponsor"), "address");
  const bool paid_gas = !gas_payer ? holder_sent : same_address(gas_payer, address, &threw);
  const bool has_gas = gas && !(gas->type == tj::Type::kNull || gas->type == tj::Type::kFalse ||
                                (gas->type == tj::Type::kString && gas->length == 0) ||
                                (gas->type == tj::Type::kNumber && (gas->number == 0 || isnan(gas->number))));
  BigInt gas_cost;
  if (has_gas) {
    BigInt computation, storage, rebate;
    if (!decimal_big(tj::Get(gas, "computationCost"), &computation) ||
        !decimal_big(tj::Get(gas, "storageCost"), &storage) || !decimal_big(tj::Get(gas, "storageRebate"), &rebate) ||
        !computation.Add(storage) || !computation.Sub(rebate)) {
      error->Upstream("Sui gas overflow");
      return false;
    }
    gas_cost = computation;
  }
  // A net storage rebate is not a fee: it stays in the SUI delta.
  const bool has_fee = paid_gas && has_gas && !gas_cost.negative() && !gas_cost.IsZero();
  char fee_text[200] = {};
  if (has_fee && !gas_cost.ToDecimal(fee_text, sizeof(fee_text))) { error->Upstream("fee text"); return false; }

  const Value* changes = tj::Get(effects, "balanceChangesJson");
  if (changes && changes->type != tj::Type::kNull && !tj::IsArray(changes)) threw = true;  // for-of throws
  if (threw) { error->Upstream("Sui transaction: malformed field"); return false; }
  const uint32_t change_count = tj::IsArray(changes) ? changes->count : 0;
  Net* nets = static_cast<Net*>(a.Alloc(sizeof(Net) * (change_count ? change_count : 1)));
  Gainers* gainers = static_cast<Gainers*>(a.Alloc(sizeof(Gainers) * (change_count ? change_count : 1)));
  if (!nets || !gainers) { error->Upstream("arena exhausted"); return false; }
  size_t net_count = 0, gainer_count = 0;
  for (uint32_t i = 0; i < change_count; ++i) {
    const Value* change = changes->items[i];
    if (!change || change->type == tj::Type::kNull) { error->Upstream("Sui transaction: null change"); return false; }
    const Value* coin_type = tj::Get(change, "coinType");
    const Value* amount = tj::Get(change, "amount");
    if (!tj::IsString(coin_type) || !tj::IsString(amount)) continue;
    if (!digits(amount->text, amount->length, true)) continue;
    BigInt value;
    if (!BigInt::Parse(amount->text, amount->length, &value)) { error->Upstream("Sui amount overflow"); return false; }
    const Value* owner = tj::Get(change, "address");
    const bool holder = same_address(owner, address, &threw);
    if (threw) { error->Upstream("Sui transaction: malformed owner"); return false; }
    if (holder) {
      Net* net = nullptr;
      for (size_t k = 0; k < net_count && !net; ++k)
        if (same_text(nets[k].coin_type, coin_type)) net = &nets[k];
      if (!net) { net = &nets[net_count++]; net->coin_type = coin_type; net->amount = BigInt(); }
      if (!net->amount.Add(value)) { error->Upstream("Sui amount overflow"); return false; }
    } else if (tj::IsString(owner) && !value.negative() && !value.IsZero()) {
      Gainers* g = nullptr;
      for (size_t k = 0; k < gainer_count && !g; ++k)
        if (same_text(gainers[k].coin_type, coin_type)) g = &gainers[k];
      if (!g) { g = &gainers[gainer_count++]; g->coin_type = coin_type; g->first = owner; g->count = 1; continue; }
      // A Set of addresses: only whether exactly one distinct gainer exists matters.
      if (g->count == 1 && !same_text(g->first, owner)) g->count = 2;
    }
  }

  Value* address_value = tj::Str(a, address);
  Value* method = kind ? SanitizeText(a, kind->text, kind->length) : tj::Null(a);
  Value* error_value = tj::Null(a);
  if (failed) {
    const Value* message = tj::Get(tj::Get(effects, "executionError"), "message");
    if (message && message->type != tj::Type::kNull && !tj::IsString(message)) {
      error->Upstream("Sui transaction: non-string error");
      return false;
    }
    error_value = tj::IsString(message) ? SanitizeText(a, message->text, message->length) : SanitizeText(a, "failed", 6);
  }
  if (!address_value || !method || !error_value) { error->Upstream("arena exhausted"); return false; }
  Value* block_number = has_checkpoint ? tj::Num(a, checkpoint) : tj::Null(a);
  const double native_decimals = network.has_native ? network.native_decimals : 9;
  const char* native_symbol = network.has_native ? network.native_symbol : "SUI";

  const size_t first_record = records->count;
  const char* canonical_balance_manager =
      canonical_move_type(a, kDeepBookBalanceManagerType, strlen(kDeepBookBalanceManagerType));
  Value* created_managers = tj::Arr(a);
  if (!canonical_balance_manager || !created_managers) { error->Upstream("arena exhausted"); return false; }
  const Value* object_nodes = tj::Get(tj::Get(effects, "objectChanges"), "nodes");
  if (object_nodes && object_nodes->type != tj::Type::kNull && !tj::IsArray(object_nodes)) {
    error->Upstream("Sui transaction: malformed object changes");
    return false;
  }
  const uint32_t object_count = tj::IsArray(object_nodes) ? object_nodes->count : 0;
  for (uint32_t i = 0; i < object_count; ++i) {
    const Value* change = object_nodes->items[i];
    if (!change || change->type == tj::Type::kNull) { error->Upstream("Sui transaction: null object change"); return false; }
    const Value* object_id = tj::Get(change, "address");
    const Value* move = tj::Get(tj::Get(tj::Get(tj::Get(change, "outputState"), "asMoveObject"), "contents"), "type");
    const Value* repr = tj::Get(move, "repr");
    const Value* created = tj::Get(change, "idCreated");
    if (created && created->type == tj::Type::kTrue && tj::IsString(object_id) && tj::IsString(repr) &&
        same_move_type(a, repr, canonical_balance_manager)) {
      if (!tj::Push(a, created_managers, tj::Clone(a, object_id))) { error->Upstream("arena exhausted"); return false; }
    }
  }

  // Address-owned object (NFT) moves.
  for (uint32_t i = 0; i < object_count; ++i) {
    const Value* change = object_nodes->items[i];
    const Value* object_id = tj::Get(change, "address");
    const Value* input_raw = tj::Get(tj::Get(tj::Get(tj::Get(change, "inputState"), "owner"), "address"), "address");
    const Value* output_raw = tj::Get(tj::Get(tj::Get(tj::Get(change, "outputState"), "owner"), "address"), "address");
    const Value* contents = tj::Get(tj::Get(tj::Get(change, "outputState"), "asMoveObject"), "contents");
    const Value* display = tj::Get(tj::Get(contents, "display"), "output");
    const Value* move_repr = tj::Get(tj::Get(contents, "type"), "repr");
    const bool in_holder = same_address(input_raw, address, &threw);
    const bool out_holder = same_address(output_raw, address, &threw);
    if (threw) { error->Upstream("Sui transaction: malformed owner"); return false; }
    const bool incoming = out_holder && !in_holder;
    const bool outgoing = in_holder && !out_holder;
    if (!tj::IsString(object_id) || (!incoming && !outgoing) || !display || display->type == tj::Type::kNull ||
        display_empty(display))
      continue;
    const char* direction = DirectionFromFlow(incoming, outgoing);
    auto display_text = [&](const char* key) -> Value* {
      const Value* v = nonblank(tj::Get(display, key));
      return v ? SanitizeText(a, v->text, v->length) : nullptr;
    };
    auto display_name = [&](const char* key) -> Value* {
      const Value* v = nonblank(tj::Get(display, key));
      return v ? SanitizeName(a, v->text, v->length) : nullptr;
    };
    Value* package_id = nullptr;
    Value* struct_name = nullptr;
    if (tj::IsString(move_repr)) {
      const char* t = move_repr->text;
      const size_t n = move_repr->length;
      if (n >= 2 && t[0] == '0' && t[1] == 'x') {
        size_t end = 0;
        while (end + 1 < n && !(t[end] == ':' && t[end + 1] == ':')) ++end;
        if (end + 1 >= n) end = n;
        package_id = tj::Str(a, t, end);
      }
      // split('::').at(-1)?.split('<')[0]?.trim() || null
      size_t last = 0;
      for (size_t k = 0; k + 1 < n; ++k)
        if (t[k] == ':' && t[k + 1] == ':') { last = k + 2; ++k; }  // non-overlapping, as split()
      const char* segment = t + last;
      size_t segment_length = n - last;
      const char* lt = static_cast<const char*>(memchr(segment, '<', segment_length));
      if (lt) segment_length = static_cast<size_t>(lt - segment);
      size_t b = 0, e = 0;
      compat::token_text::Trim(segment, segment_length, &b, &e);
      if (e > b) struct_name = tj::Str(a, segment + b, e - b);
    }
    Value* name = display_name("name");
    Value* collection = display_name("collection");
    if (!collection) collection = struct_name;
    Value* description = display_text("description");
    Value* image = display_text("image_url");
    if (!image) image = display_text("image");
    if (!image) image = display_text("img_url");
    Value* external = display_text("project_url");
    if (!external) external = display_text("link");
    const Value* input_owner = input_raw && input_raw->type != tj::Type::kNull ? input_raw : nullptr;
    const Value* output_owner = output_raw && output_raw->type != tj::Type::kNull ? output_raw : nullptr;
    const Value* received_from = input_owner ? input_owner : sender;

    Value* nft = tj::Obj(a);
    bool ok = nft && tj::Set(a, nft, "chainId", tj::Str(a, network.id)) &&
              tj::Set(a, nft, "tokenId", tj::Clone(a, object_id)) &&
              (!package_id || tj::Set(a, nft, "contractAddress", package_id)) &&
              tj::Set(a, nft, "standard", tj::Str(a, "Sui Object")) && (!name || tj::Set(a, nft, "name", name)) &&
              (!collection || tj::Set(a, nft, "collectionName", collection)) &&
              (!description || tj::Set(a, nft, "description", description)) &&
              (!image || tj::Set(a, nft, "imageUrl", image)) && (!external || tj::Set(a, nft, "externalUrl", external));
    if (ok && incoming) {
      ok = tj::Set(a, nft, "receivedTxHash", tj::Clone(a, digest)) &&
           (!received_from || tj::Set(a, nft, "receivedFrom", tj::Clone(a, received_from))) &&
           tj::Set(a, nft, "receivedAt", IsoValue(a, static_cast<int64_t>(ts)));
    }
    if (ok && move_repr) {
      Value* details = tj::Arr(a);
      Value* detail = tj::Obj(a);
      ok = details && detail && tj::Set(a, detail, "label", tj::Str(a, "Object type")) &&
           tj::Set(a, detail, "value", tj::Clone(a, move_repr)) &&
           tj::Set(a, detail, "kind", tj::Str(a, "identifier")) && tj::Push(a, details, detail) &&
           tj::Set(a, nft, "details", details);
    }
    Value* asset = tj::Obj(a);
    Value* metadata = tj::Obj(a);
    ok = ok && asset && metadata && tj::Set(a, asset, "chainId", tj::Str(a, network.id)) &&
         tj::Set(a, asset, "symbol", tj::Str(a, "NFT")) &&
         tj::Set(a, asset, "name", name ? tj::Clone(a, name) : collection ? tj::Clone(a, collection) : tj::Str(a, "Sui NFT")) &&
         tj::Set(a, asset, "decimals", tj::Num(a, 0)) && tj::Set(a, asset, "contractAddress", tj::Clone(a, object_id)) &&
         tj::Set(a, metadata, "nft", nft) && tj::Set(a, metadata, "suiObjectId", tj::Clone(a, object_id)) &&
         tj::Set(a, metadata, "moveType", move_repr ? tj::Clone(a, move_repr) : tj::Null(a));
    const Value* record_id = Format(a, "%s:%s:nft:%s", network.id, tj::CString(a, digest), tj::CString(a, object_id));
    const Value* token_id = Format(a, "nft:%s", tj::CString(a, object_id));
    const bool mint = incoming && tj::Get(change, "idCreated") && tj::Get(change, "idCreated")->type == tj::Type::kTrue;
    const Value* from = incoming ? received_from : address_value;
    const Value* to = incoming ? address_value : output_owner;
    const Value* counterparty = incoming ? received_from : output_owner;
    Value* r = NewTransfer(a);
    ok = ok && r && record_id && token_id && tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) &&
         tj::Set(a, r, "txType", tj::Str(a, mint ? "mint" : TypeFromDirection(direction))) &&
         tj::Set(a, r, "status", tj::Str(a, failed ? "failed" : "confirmed")) &&
         tj::Set(a, r, "direction", tj::Str(a, direction)) && tj::Set(a, r, "ts", tj::Num(a, ts)) &&
         tj::Set(a, r, "hash", tj::Clone(a, digest)) && tj::Set(a, r, "blockNumber", tj::Clone(a, block_number)) &&
         tj::Set(a, r, "fromAddress", from ? tj::Clone(a, from) : tj::Null(a)) &&
         tj::Set(a, r, "toAddress", to ? tj::Clone(a, to) : tj::Null(a)) &&
         tj::Set(a, r, "counterparty", counterparty ? tj::Clone(a, counterparty) : tj::Null(a)) &&
         tj::Set(a, r, "tokenId", const_cast<Value*>(token_id)) && tj::Set(a, r, "asset", asset) &&
         tj::Set(a, r, "amountRaw", tj::Str(a, "1")) && tj::Set(a, r, "amountDecimals", tj::Num(a, 0)) &&
         tj::Set(a, r, "feeRaw", tj::Null(a)) && tj::Set(a, r, "feeDecimals", tj::Null(a)) &&
         tj::Set(a, r, "feeSymbol", tj::Null(a)) && tj::Set(a, r, "memo", tj::Null(a)) &&
         tj::Set(a, r, "method", tj::Clone(a, method)) && tj::Set(a, r, "error", tj::Clone(a, error_value)) &&
         tj::Set(a, r, "metadata", metadata) && tj::Push(a, records, r);
    if (!ok) { error->Upstream("arena exhausted"); return false; }
  }

  // Coin deltas, netted by Sui per owner and coin type.
  BigInt fee;
  if (has_fee && !BigInt::ParseDecimal(fee_text, strlen(fee_text), &fee)) { error->Upstream("fee"); return false; }
  Value* native_record = nullptr;
  BigInt native_net;
  for (size_t k = 0; k < net_count; ++k) {
    const Net* net = &nets[k];
    const Value* coin_type = net->coin_type;
    const bool is_native = same_move_type(a, coin_type, kSuiCoinType);
    if (is_native && !native_net.Add(net->amount)) { error->Upstream("overflow"); return false; }
    // The native owner delta includes gas: add it back.
    BigInt moved = net->amount;
    if (is_native && has_fee && !moved.Add(fee)) { error->Upstream("overflow"); return false; }
    if (moved.IsZero()) continue;
    const bool incoming = !moved.negative();
    const char* direction = DirectionFromFlow(incoming, !incoming);
    const Value* recipient = nullptr;
    for (size_t g = 0; !incoming && g < gainer_count; ++g)
      if (same_text(gainers[g].coin_type, coin_type) && gainers[g].count == 1) recipient = gainers[g].first;
    // A holder's own transaction output came from a contract, not itself.
    const Value* source = holder_sent ? nullptr : sender;
    const Value* record_id = is_native
        ? Format(a, "%s:%s", network.id, tj::CString(a, digest))
        : Format(a, "%s:%s:%s", network.id, tj::CString(a, digest), tj::CString(a, coin_type));
    const char* canonical = is_native ? nullptr : canonical_move_type(a, coin_type->text, coin_type->length);
    const Value* token_id = is_native ? tj::Str(a, "native") : canonical ? Format(a, "coin:%s", canonical) : nullptr;
    char amount_text[200];
    if (!moved.Abs().ToDecimal(amount_text, sizeof(amount_text))) { error->Upstream("amount"); return false; }
    const Value* from = incoming ? source : address_value;
    const Value* to = incoming ? address_value : recipient;
    const Value* counterparty = incoming ? source : recipient;
    Value* r = NewTransfer(a);
    const bool ok =
        r && record_id && token_id && tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) &&
        tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection(direction))) &&
        tj::Set(a, r, "status", tj::Str(a, failed ? "failed" : "confirmed")) &&
        tj::Set(a, r, "direction", tj::Str(a, direction)) && tj::Set(a, r, "ts", tj::Num(a, ts)) &&
        tj::Set(a, r, "hash", tj::Clone(a, digest)) && tj::Set(a, r, "blockNumber", tj::Clone(a, block_number)) &&
        tj::Set(a, r, "fromAddress", from ? tj::Clone(a, from) : tj::Null(a)) &&
        tj::Set(a, r, "toAddress", to ? tj::Clone(a, to) : tj::Null(a)) &&
        tj::Set(a, r, "counterparty", counterparty ? tj::Clone(a, counterparty) : tj::Null(a)) &&
        tj::Set(a, r, "tokenId", const_cast<Value*>(token_id)) &&
        tj::Set(a, r, "asset", is_native ? NativeAssetJson(a, network) : tj::Null(a)) &&
        tj::Set(a, r, "amountRaw", tj::Str(a, amount_text)) &&
        tj::Set(a, r, "amountDecimals", is_native ? tj::Num(a, native_decimals) : tj::Null(a)) &&
        tj::Set(a, r, "feeRaw", tj::Null(a)) && tj::Set(a, r, "feeDecimals", tj::Null(a)) &&
        tj::Set(a, r, "feeSymbol", tj::Null(a)) && tj::Set(a, r, "memo", tj::Null(a)) &&
        tj::Set(a, r, "method", tj::Clone(a, method)) && tj::Set(a, r, "error", tj::Clone(a, error_value)) &&
        tj::Set(a, r, "metadata", tj::Obj(a)) && tj::Push(a, records, r);
    if (!ok) { error->Upstream("arena exhausted"); return false; }
    if (is_native) native_record = r;
  }

  // The gas rides on exactly one record the holder sent.
  auto outgoing = [](const Value* r) { return tj::StringEquals(tj::Get(r, "direction"), "outgoing"); };
  bool gas_only = false;
  if (has_fee) {
    Value* carrier = native_record && outgoing(native_record) ? native_record : nullptr;
    for (uint32_t i = first_record; !carrier && i < records->count; ++i) {
      const Value* token = tj::Get(records->items[i], "tokenId");
      if (outgoing(records->items[i]) && !(token->length >= 4 && memcmp(token->text, "nft:", 4) == 0))
        carrier = records->items[i];
    }
    for (uint32_t i = first_record; !carrier && i < records->count; ++i)
      if (outgoing(records->items[i])) carrier = records->items[i];
    if (carrier) {
      if (!tj::Set(a, carrier, "feeRaw", tj::Str(a, fee_text)) ||
          !tj::Set(a, carrier, "feeDecimals", tj::Num(a, native_decimals)) ||
          !tj::Set(a, carrier, "feeSymbol", tj::Str(a, native_symbol))) {
        error->Upstream("arena exhausted");
        return false;
      }
    } else if (native_record) {
      if (native_net.IsZero()) {
        for (uint32_t i = first_record; i < records->count; ++i) {
          if (records->items[i] != native_record) continue;
          for (uint32_t j = i + 1; j < records->count; ++j) records->items[j - 1] = records->items[j];
          --records->count;
          break;
        }
      } else {
        char net_text[200];
        if (!native_net.Abs().ToDecimal(net_text, sizeof(net_text))) { error->Upstream("amount"); return false; }
        bool ok = true;
        if (native_net.negative()) {
          // Received less than the gas: what it cost, net.
          ok = tj::Set(a, native_record, "direction", tj::Str(a, "outgoing")) &&
               tj::Set(a, native_record, "txType", tj::Str(a, TypeFromDirection("outgoing"))) &&
               tj::Set(a, native_record, "fromAddress", tj::Clone(a, address_value)) &&
               tj::Set(a, native_record, "toAddress", tj::Null(a)) &&
               tj::Set(a, native_record, "counterparty", tj::Null(a));
        }
        ok = ok && tj::Set(a, native_record, "amountRaw", tj::Str(a, net_text));
        if (!ok) { error->Upstream("arena exhausted"); return false; }
      }
    } else {
      gas_only = true;
    }
  }

  // A gas-only, failed or shared-state-only transaction keeps one row.
  if (gas_only || (records->count == first_record && holder_sent)) {
    const Value* record_id = holder_sent ? Format(a, "%s:%s", network.id, tj::CString(a, digest))
                                         : Format(a, "%s:%s:gas", network.id, tj::CString(a, digest));
    Value* r = NewTransfer(a);
    Value* metadata = tj::Obj(a);
    bool ok = r && record_id && metadata && tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) &&
              tj::Set(a, r, "ts", tj::Num(a, ts)) && tj::Set(a, r, "hash", tj::Clone(a, digest)) &&
              tj::Set(a, r, "blockNumber", tj::Clone(a, block_number)) &&
              tj::Set(a, r, "fromAddress", tj::Clone(a, address_value)) && tj::Set(a, r, "toAddress", tj::Null(a)) &&
              tj::Set(a, r, "counterparty", tj::Null(a)) && tj::Set(a, r, "tokenId", tj::Str(a, "native")) &&
              tj::Set(a, r, "feeRaw", has_fee ? tj::Str(a, fee_text) : tj::Null(a)) &&
              tj::Set(a, r, "feeDecimals", has_fee ? tj::Num(a, native_decimals) : tj::Null(a)) &&
              tj::Set(a, r, "feeSymbol", has_fee ? tj::Str(a, native_symbol) : tj::Null(a)) &&
              tj::Set(a, r, "memo", tj::Null(a)) && tj::Set(a, r, "method", tj::Clone(a, method));
    if (failed) {
      ok = ok && tj::Set(a, r, "txType", tj::Str(a, TypeFromDirection("outgoing"))) &&
           tj::Set(a, r, "status", tj::Str(a, "failed")) && tj::Set(a, r, "direction", tj::Str(a, "outgoing")) &&
           tj::Set(a, r, "asset", NativeAssetJson(a, network)) && tj::Set(a, r, "amountRaw", tj::Str(a, "0")) &&
           tj::Set(a, r, "amountDecimals", tj::Num(a, native_decimals)) &&
           tj::Set(a, r, "error", tj::Clone(a, error_value));
    } else {
      ok = ok && tj::Set(a, r, "txType", tj::Str(a, "contractCall")) &&
           tj::Set(a, r, "status", tj::Str(a, "confirmed")) &&
           tj::Set(a, r, "direction", tj::Str(a, "selfTransfer")) && tj::Set(a, r, "asset", tj::Null(a)) &&
           tj::Set(a, r, "amountRaw", tj::Null(a)) && tj::Set(a, r, "amountDecimals", tj::Null(a)) &&
           tj::Set(a, r, "error", tj::Null(a)) &&
           (!kind || tj::Set(a, metadata, "suiTransactionKind", tj::Clone(a, method)));
    }
    ok = ok && tj::Set(a, r, "metadata", metadata) && tj::Push(a, records, r);
    if (!ok) { error->Upstream("arena exhausted"); return false; }
  }

  if (!failed && holder_sent && created_managers->count > 0) {
    for (uint32_t i = first_record; i < records->count; ++i) {
      Value* metadata = tj::Get(records->items[i], "metadata");
      if (!tj::Set(a, metadata, "protocol", tj::Str(a, "DeepBook V3")) ||
          !tj::Set(a, metadata, "deepBookAction", tj::Str(a, "createManager")) ||
          !tj::Set(a, metadata, "activityLabel", tj::Str(a, "Create DeepBook trading account")) ||
          !tj::Set(a, metadata, "createdBalanceManagerIds", tj::Clone(a, created_managers))) {
        error->Upstream("arena exhausted");
        return false;
      }
    }
  }
  for (uint32_t i = first_record; i < records->count; ++i)
    if (!tj::Set(a, records->items[i], "restatable", tj::Bool(a, true))) { error->Upstream("arena exhausted"); return false; }
  return true;
}

}  // namespace

bool SuiSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
               double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  double budget = backfill_depth < kPageLimit * kMaxPages ? backfill_depth : kPageLimit * kMaxPages;
  if (!(budget >= 1)) budget = 1;
  auto string_state = [&](const char* key) -> const char* {
    const Value* v = tj::Get(state, key);
    return tj::IsString(v) ? tj::CString(a, v) : nullptr;
  };
  const char* seen_digest = string_state("lastDigest");
  const char* pending_newest = string_state("pendingNewest");
  const char* before_cursor = string_state("beforeCursor");
  Walked walked;
  if (!walk_affected(ctx, network, address, seen_digest, budget, before_cursor, &walked, error)) return false;

  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("arena exhausted"); return false; }
  for (uint32_t b = 0; b < walked.blocks->count; ++b) {
    Value* produced = tj::Arr(a);
    if (!produced || !to_records(ctx, network, address, walked.blocks->items[b], produced, error)) {
      if (!produced) error->Upstream("arena exhausted");
      return false;
    }
    for (uint32_t r = 0; r < produced->count; ++r) {
      const Value* id = tj::Get(produced->items[r], "recordId");
      bool seen = false;
      for (uint32_t k = 0; k < records->count && !seen; ++k) {
        const Value* other = tj::Get(records->items[k], "recordId");
        seen = other->length == id->length && memcmp(other->text, id->text, id->length) == 0;
      }
      if (!seen && !tj::Push(a, records, produced->items[r])) { error->Upstream("arena exhausted"); return false; }
    }
  }

  const char* crawl_newest = pending_newest ? pending_newest : walked.newest;
  Value* next = tj::Obj(a);
  bool ok = next != nullptr;
  if (walked.reached_seen) {
    ok = ok && (!crawl_newest || tj::Set(a, next, "lastDigest", tj::Str(a, crawl_newest)));
  } else {
    ok = ok && (!seen_digest || tj::Set(a, next, "lastDigest", tj::Str(a, seen_digest))) &&
         (!crawl_newest || tj::Set(a, next, "pendingNewest", tj::Str(a, crawl_newest))) &&
         (!walked.next_before || tj::Set(a, next, "beforeCursor", tj::Str(a, walked.next_before)));
  }
  if (!ok) { error->Upstream("arena exhausted"); return false; }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = walked.reached_seen;
  return true;
}

const SuiDriver kSuiDriver{};

const SuiSource kSuiSource{};


bool SuiDriver::Registered(const config::ProviderRuntimeConfig& config) const { return sui_present(config); }
bool SuiSource::Registered(const config::ProviderRuntimeConfig& config) const { return sui_present(config); }
bool SuiSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return sui_supports(config, network); }

}  // namespace fortis::chains
