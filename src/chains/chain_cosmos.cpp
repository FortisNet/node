#include "chain_family_drivers.h"
#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// Cosmos Hub (the TypeScript CosmosDriver and CosmosTransferSource): LCD
// bank balances with bounded IBC denom metadata, and the two-leg ascending
// tx search whose records are netted from the bank module's coin_received /
// coin_spent events. Guest-safe.
namespace fortis::chains {
namespace {

constexpr const char* kNativeDenom = "uatom";
constexpr size_t kMaxMetadataLookups = 10;
constexpr double kPageLimit = 50;
constexpr double kMaxPages = 3;

bool digits_only(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

bool is_string(const Value* v) { return tj::IsString(v); }

// /^cosmos1[0-9a-z]{38}$/
bool valid_address(const char* address) {
  const size_t length = strlen(address);
  if (length != 45 || strncmp(address, "cosmos1", 7) != 0) return false;
  for (size_t i = 7; i < length; ++i) {
    const char c = address[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) return false;
  }
  return true;
}

// Strict JavaScript `===` between two possibly-absent JSON values (objects
// and arrays compare by identity, so two distinct parsed ones never match).
bool same_value(const Value* a, const Value* b) {
  if (!a || !b) return !a && !b;
  if (a->type != b->type) return false;
  switch (a->type) {
    case tj::Type::kNull: case tj::Type::kTrue: case tj::Type::kFalse: return true;
    case tj::Type::kNumber: return a->number == b->number;
    case tj::Type::kString: return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
    default: return a == b;
  }
}

// application/x-www-form-urlencoded byte serializer (URLSearchParams).
bool form_append(OutBuffer& out, const char* text) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    const unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' ||
        c == '-' || c == '.' || c == '_') {
      out.Byte(c);
    } else if (c == ' ') {
      out.Byte('+');
    } else {
      out.Byte('%');
      out.Byte(static_cast<uint8_t>(kHex[c >> 4]));
      out.Byte(static_cast<uint8_t>(kHex[c & 15]));
    }
  }
  return out.ok;
}

// A trimmed coin `<digits><denom>` matched by /^(\d+)(.+)$/ (greedy digits,
// at least one denom character, backtracking into the digits when needed).
bool split_coin_loose(const char* coin, size_t length, size_t* digit_count) {
  size_t k = 0;
  while (k < length && coin[k] >= '0' && coin[k] <= '9') ++k;
  if (k == 0) return false;
  if (k == length) {
    if (k < 2) return false;
    k -= 1;
  }
  *digit_count = k;
  return true;
}

// /^(\d+)([a-zA-Z][a-zA-Z0-9/:._-]*)$/
bool split_coin_strict(const char* coin, size_t length, size_t* digit_count) {
  size_t k = 0;
  while (k < length && coin[k] >= '0' && coin[k] <= '9') ++k;
  if (k == 0 || k == length) return false;
  const char first = coin[k];
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) return false;
  for (size_t i = k + 1; i < length; ++i) {
    const char c = coin[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '/' || c == ':' || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  *digit_count = k;
  return true;
}

// Iterates `value.split(',')` with each piece JavaScript-trimmed.
struct CoinCursor {
  const char* text;
  size_t length;
  size_t position = 0;
  bool done = false;
  bool Next(const char** piece, size_t* piece_length) {
    if (done) return false;
    size_t end = position;
    while (end < length && text[end] != ',') ++end;
    size_t begin = 0, stop = 0;
    codec::provider_text::Trim(text + position, end - position, &begin, &stop);
    *piece = text + position + begin;
    *piece_length = stop - begin;
    if (end >= length) done = true;
    position = end + 1;
    return true;
  }
};

// ---- small ordered maps (Map<string, bigint>) ---------------------------------

struct Entry {
  const char* key;
  size_t key_length;
  BigInt amount;
};

struct Table {
  Entry* entries = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  Entry* Find(const char* key, size_t length) {
    for (size_t i = 0; i < count; ++i)
      if (entries[i].key_length == length && memcmp(entries[i].key, key, length) == 0) return &entries[i];
    return nullptr;
  }
  // map.set(key, (map.get(key) ?? 0n) + delta). False on arena/overflow failure.
  bool Add(Arena& arena, const char* key, size_t length, const BigInt& delta) {
    Entry* e = Find(key, length);
    if (!e) {
      if (count == capacity) {
        const size_t next = capacity ? capacity * 2 : 8;
        auto* grown = static_cast<Entry*>(arena.Alloc(sizeof(Entry) * next));
        if (!grown) return false;
        if (count) memcpy(static_cast<void*>(grown), entries, sizeof(Entry) * count);
        entries = grown;
        capacity = next;
      }
      e = &entries[count++];
      e->key = key;
      e->key_length = length;
      e->amount = BigInt();
    }
    return e->amount.Add(delta);
  }
  bool Set(Arena& arena, const char* key, size_t length, const BigInt& value) {
    Entry* e = Find(key, length);
    if (e) { e->amount = value; return true; }
    return Add(arena, key, length, value);
  }
};

struct Counterparty {
  const char* denom;
  size_t denom_length;
  const char* party;
  BigInt amount;
};

bool str_equals(const char* a, size_t a_length, const char* b) {
  return b && strlen(b) == a_length && memcmp(a, b, a_length) == 0;
}

const Value* attr_key(const Value* attribute) { return tj::Get(attribute, "key"); }

bool is_message(const Value* event) {
  const Value* attributes = tj::Get(event, "attributes");
  if (!tj::IsArray(attributes)) return false;
  for (uint32_t i = 0; i < attributes->count; ++i)
    if (tj::StringEquals(attr_key(attributes->items[i]), "msg_index")) return true;
  return false;
}

// TypeError guards for the shapes TypeScript would throw on.
bool events_well_formed(const Value* events) {
  for (uint32_t i = 0; i < events->count; ++i) {
    const Value* event = events->items[i];
    if (!event || event->type == tj::Type::kNull) return false;
    const Value* attributes = tj::Get(event, "attributes");
    if (attributes && attributes->type != tj::Type::kNull) {
      if (!tj::IsArray(attributes)) {
        // `?? []` keeps a non-array: .some()/for-of over an object throws,
        // a string iterates characters (which have no key/value).
        if (attributes->type != tj::Type::kString) return false;
        continue;
      }
      for (uint32_t j = 0; j < attributes->count; ++j) {
        const Value* attribute = attributes->items[j];
        if (!attribute || attribute->type == tj::Type::kNull) return false;
      }
    }
  }
  return true;
}

// attributesOf(event): the last value of `key`; absent -> null pointer.
const Value* attribute_of(const Value* event, const char* key) {
  const Value* attributes = tj::Get(event, "attributes");
  const Value* found = nullptr;
  if (!tj::IsArray(attributes)) return nullptr;
  for (uint32_t i = 0; i < attributes->count; ++i) {
    const Value* attribute = attributes->items[i];
    if (tj::StringEquals(attr_key(attribute), key)) found = tj::Get(attribute, "value");
  }
  return found;
}

// Network native coin defaults (network.native ?? {symbol: 'ATOM', decimals: 6}).
void native_of(const Network& network, const char** symbol, double* decimals) {
  *symbol = network.has_native ? network.native_symbol : "ATOM";
  *decimals = network.has_native ? network.native_decimals : 6;
}

// ---- driver -------------------------------------------------------------------

bool cosmos_present(const config::ProviderRuntimeConfig&) { return true; }

}  // namespace

bool CosmosDriver::Balances(Context& ctx, const Network& network, const char* address, const char* const*,
                     size_t, Value** result, Error* error) const {
  Arena& a = *ctx.arena;
  if (!valid_address(address)) {
    error->Range("not a Cosmos address: %s", address);
    return false;
  }
  const char* encoded = EncodeUri(a, address);
  const Value* bank_path = encoded ? Format(a, "/cosmos/bank/v1beta1/balances/%s", encoded) : nullptr;
  if (!bank_path) { error->Upstream("arena exhausted"); return false; }
  Call bank;
  bank.kind = CallKind::kGet;
  bank.urls = network.rpc_urls;
  bank.url_count = network.rpc_url_count;
  bank.path = bank_path->text;
  ctx.transport->Run(bank, a);
  if (!bank.ok()) { bank.ToError(error); return false; }
  if (!bank.value || bank.value->type == tj::Type::kNull) {
    error->Upstream("cosmos bank: null body");
    return false;
  }

  const char* native_raw = "0";
  size_t native_raw_length = 1;
  const Value* ibc[64];
  size_t ibc_count = 0;
  const Value* balances = tj::Get(bank.value, "balances");
  if (tj::IsObject(balances) || tj::IsNumber(balances) || tj::IsBool(balances)) {
    error->Upstream("cosmos bank: balances is not iterable");  // for-of throws
    return false;
  }
  if (tj::IsArray(balances)) {
    for (uint32_t i = 0; i < balances->count; ++i) {
      const Value* entry = balances->items[i];
      if (!entry || entry->type == tj::Type::kNull) { error->Upstream("cosmos bank: null entry"); return false; }
      const Value* denom = tj::Get(entry, "denom");
      const Value* amount = tj::Get(entry, "amount");
      if (!is_string(denom) || !is_string(amount)) continue;
      if (!digits_only(amount->text, amount->length)) continue;
      if (tj::StringEquals(denom, kNativeDenom)) {
        native_raw = amount->text;
        native_raw_length = amount->length;
      } else if (denom->length >= 4 && memcmp(denom->text, "ibc/", 4) == 0 &&
                 ibc_count < sizeof(ibc) / sizeof(ibc[0])) {
        ibc[ibc_count++] = entry;
      }
    }
  }

  // ibcDenoms.slice(0, MAX_METADATA_LOOKUPS), all at once.
  const size_t lookups = ibc_count < kMaxMetadataLookups ? ibc_count : kMaxMetadataLookups;
  Call calls[kMaxMetadataLookups];
  for (size_t i = 0; i < lookups; ++i) {
    const Value* denom = tj::Get(ibc[i], "denom");
    const char* denom_text = tj::CString(a, denom);
    const char* escaped = denom_text ? EncodeUri(a, denom_text) : nullptr;
    const Value* path = escaped ? Format(a, "/cosmos/bank/v1beta1/denoms_metadata/%s", escaped) : nullptr;
    if (!path) { error->Upstream("arena exhausted"); return false; }
    calls[i].kind = CallKind::kGet;
    calls[i].urls = network.rpc_urls;
    calls[i].url_count = network.rpc_url_count;
    calls[i].path = path->text;
  }
  if (lookups) ctx.transport->Run(calls, lookups, a);

  Value* tokens = tj::Arr(a);
  if (!tokens) { error->Upstream("arena exhausted"); return false; }
  for (size_t i = 0; i < lookups; ++i) {
    if (!calls[i].ok() || !calls[i].value) continue;  // no metadata: omitted honestly
    const Value* meta = calls[i].value;
    if (meta->type == tj::Type::kNull) continue;  // meta.metadata throws -> caught
    const Value* metadata = tj::Get(meta, "metadata");
    if (!tj::IsObject(metadata)) continue;
    const Value* display = tj::Get(metadata, "display");
    const Value* units = tj::Get(metadata, "denom_units");
    if (units && units->type != tj::Type::kNull && !tj::IsArray(units)) continue;  // .find throws
    const Value* exponent = nullptr;
    if (tj::IsArray(units)) {
      bool found = false;
      for (uint32_t u = 0; u < units->count && !found; ++u) {
        const Value* unit = units->items[u];
        if (!unit || unit->type == tj::Type::kNull) { exponent = nullptr; break; }  // throws
        if (same_value(tj::Get(unit, "denom"), display)) {
          found = true;
          exponent = tj::Get(unit, "exponent");
        }
      }
    }
    if (!tj::IsNumber(exponent)) continue;  // never mis-scale
    const Value* symbol = tj::Get(metadata, "symbol");
    Value* symbol_value = nullptr;
    const bool truthy = symbol && !(symbol->type == tj::Type::kNull || symbol->type == tj::Type::kFalse ||
                                    (symbol->type == tj::Type::kString && symbol->length == 0) ||
                                    (symbol->type == tj::Type::kNumber && (symbol->number == 0 || isnan(symbol->number))));
    if (truthy) {
      if (!is_string(symbol)) continue;  // sanitizeSymbol(non-string) throws -> null token
      symbol_value = SanitizeSymbol(a, symbol->text, symbol->length);
    } else {
      symbol_value = tj::Null(a);
    }
    const Value* denom = tj::Get(ibc[i], "denom");
    const Value* amount = tj::Get(ibc[i], "amount");
    Value* token = tj::Obj(a);
    const Value* token_id = Format(a, "denom:%.*s", static_cast<int>(denom->length), denom->text);
    if (!token || !token_id || !symbol_value ||
        !tj::Set(a, token, "tokenId", const_cast<Value*>(token_id)) ||
        !tj::Set(a, token, "balanceRaw", tj::Str(a, amount->text, amount->length)) ||
        !tj::Set(a, token, "decimals", tj::Num(a, exponent->number)) ||
        !tj::Set(a, token, "symbol", symbol_value) || !tj::Set(a, token, "source", tj::Str(a, "lcd")) ||
        !tj::Push(a, tokens, token)) {
      error->Upstream("arena exhausted");
      return false;
    }
  }

  const char* symbol = nullptr;
  double decimals = 0;
  native_of(network, &symbol, &decimals);
  Value* native = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!native || !out || !tj::Set(a, native, "balanceRaw", tj::Str(a, native_raw, native_raw_length)) ||
      !tj::Set(a, native, "decimals", tj::Num(a, decimals)) || !tj::Set(a, native, "symbol", tj::Str(a, symbol)) ||
      !tj::Set(a, native, "source", tj::Str(a, "lcd")) || !tj::Set(a, out, "native", native) ||
      !tj::Set(a, out, "tokens", tokens)) {
    error->Upstream("arena exhausted");
    return false;
  }
  *result = out;
  return true;
}

namespace {

// ---- transfer source ----------------------------------------------------------

bool cosmos_supports(const config::ProviderRuntimeConfig&, const Network& network) {
  return !network.testnet && network.rpc_url_count > 0;
}

// walk(): up to MAX_PAGES ascending pages of one attribute leg.
bool walk(Context& ctx, const Network& network, const char* attribute, const char* address,
          double since_height, double budget, Value* txs, bool* exhausted, Error* error) {
  Arena& a = *ctx.arena;
  char since[48];
  JsNumberText(since_height, since, sizeof(since));
  const Value* query = since_height > 0
      ? Format(a, "%s='%s' AND tx.height>%s", attribute, address, since)
      : Format(a, "%s='%s'", attribute, address);
  if (!query) { error->Upstream("arena exhausted"); return false; }
  for (size_t page = 1; static_cast<double>(page) <= kMaxPages; ++page) {
    uint8_t buffer[2048];
    OutBuffer out{buffer, sizeof(buffer), 0, true};
    char limit[24], offset[32];
    JsNumberText(kPageLimit, limit, sizeof(limit));
    JsNumberText((page - 1) * kPageLimit, offset, sizeof(offset));
    out.Text("/cosmos/tx/v1beta1/txs?query=");
    form_append(out, query->text);
    out.Text("&order_by=ORDER_BY_ASC&pagination.limit=");
    form_append(out, limit);
    out.Text("&pagination.offset=");
    form_append(out, offset);
    out.Byte(0);
    if (!out.ok) { error->Upstream("cosmos query too long"); return false; }
    const char* path = a.CopyText(reinterpret_cast<const char*>(buffer), out.used - 1);
    if (!path) { error->Upstream("arena exhausted"); return false; }
    Call call;
    call.kind = CallKind::kGet;
    call.urls = network.rpc_urls;
    call.url_count = network.rpc_url_count;
    call.path = path;
    ctx.transport->Run(call, a);
    if (!call.ok()) { call.ToError(error); return false; }
    const Value* rows = tj::Get(call.value, "tx_responses");
    const uint32_t count = tj::IsArray(rows) ? rows->count : 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (!tj::Push(a, txs, rows->items[i])) { error->Upstream("arena exhausted"); return false; }
    }
    if (count < kPageLimit) { *exhausted = true; return true; }
    if (txs->count >= budget) { *exhausted = false; return true; }
  }
  *exhausted = false;
  return true;
}

struct Leg {
  const char* denom;
  size_t denom_length;
  BigInt signed_amount;
  bool fee;
};

bool push_leg(Arena& a, Leg** legs, size_t* count, size_t* capacity, const Leg& leg) {
  if (*count == *capacity) {
    const size_t next = *capacity ? *capacity * 2 : 8;
    auto* grown = static_cast<Leg*>(a.Alloc(sizeof(Leg) * next));
    if (!grown) return false;
    if (*count) memcpy(static_cast<void*>(grown), *legs, sizeof(Leg) * *count);
    *legs = grown;
    *capacity = next;
  }
  (*legs)[(*count)++] = leg;
  return true;
}

// holderLegs(): the per-denom counterparty of the holder's largest message
// transfer (only lookups of the result matter).
bool counterparties_of(Arena& a, const char* address, const Value* events, bool tagged,
                       Counterparty** out, size_t* out_count, Error* error) {
  size_t capacity = 0;
  *out = nullptr;
  *out_count = 0;
  for (uint32_t e = 0; e < events->count; ++e) {
    const Value* event = events->items[e];
    if (tagged && !is_message(event)) continue;
    if (!tj::StringEquals(tj::Get(event, "type"), "transfer")) continue;
    const Value* attributes = tj::Get(event, "attributes");
    if (!tj::IsArray(attributes)) continue;
    const Value* recipient = nullptr;  // null
    const Value* sender = nullptr;
    for (uint32_t i = 0; i < attributes->count; ++i) {
      const Value* attribute = attributes->items[i];
      const Value* key = attr_key(attribute);
      const Value* value = tj::Get(attribute, "value");
      if (tj::StringEquals(key, "recipient")) {
        recipient = value && value->type != tj::Type::kNull ? value : nullptr;
      } else if (tj::StringEquals(key, "sender")) {
        sender = value && value->type != tj::Type::kNull ? value : nullptr;
      } else if (tj::StringEquals(key, "amount") && is_string(value)) {
        const bool received = tj::StringEquals(recipient, address);
        const bool sent = tj::StringEquals(sender, address);
        if (received != sent) {
          // denomsIn(value), then amountOfDenom(value, denom) for each.
          const char* seen[64];
          size_t seen_length[64];
          size_t seen_count = 0;
          CoinCursor outer{value->text, value->length};
          const char* coin;
          size_t coin_length;
          while (outer.Next(&coin, &coin_length)) {
            size_t k = 0;
            if (!split_coin_loose(coin, coin_length, &k)) continue;
            const char* denom = coin + k;
            const size_t denom_length = coin_length - k;
            bool dup = false;
            for (size_t s = 0; s < seen_count; ++s)
              if (seen_length[s] == denom_length && memcmp(seen[s], denom, denom_length) == 0) dup = true;
            if (dup || seen_count == 64) continue;
            seen[seen_count] = denom;
            seen_length[seen_count++] = denom_length;
          }
          for (size_t s = 0; s < seen_count; ++s) {
            BigInt total;
            bool any = false;
            CoinCursor inner{value->text, value->length};
            while (inner.Next(&coin, &coin_length)) {
              size_t k = 0;
              if (!split_coin_loose(coin, coin_length, &k)) continue;
              if (coin_length - k != seen_length[s] || memcmp(coin + k, seen[s], seen_length[s]) != 0) continue;
              BigInt part;
              if (!BigInt::ParseDecimal(coin, k, &part) || !total.Add(part)) {
                error->Upstream("cosmos amount overflow");
                return false;
              }
              any = true;
            }
            if (!any) continue;
            const Value* party = received ? sender : recipient;
            BigInt abs = total.Abs();
            if (!is_string(party)) continue;
            Counterparty* hit = nullptr;
            for (size_t c = 0; c < *out_count; ++c)
              if ((*out)[c].denom_length == seen_length[s] && memcmp((*out)[c].denom, seen[s], seen_length[s]) == 0)
                hit = &(*out)[c];
            const char* party_text = tj::CString(a, party);
            if (!party_text) { error->Upstream("arena exhausted"); return false; }
            if (!hit) {
              if (*out_count == capacity) {
                const size_t next = capacity ? capacity * 2 : 8;
                auto* grown = static_cast<Counterparty*>(a.Alloc(sizeof(Counterparty) * next));
                if (!grown) { error->Upstream("arena exhausted"); return false; }
                if (*out_count) memcpy(static_cast<void*>(grown), *out, sizeof(Counterparty) * *out_count);
                *out = grown;
                capacity = next;
              }
              Counterparty& c = (*out)[(*out_count)++];
              c.denom = seen[s];
              c.denom_length = seen_length[s];
              c.party = party_text;
              c.amount = abs;
            } else if (abs.Compare(hit->amount) > 0) {
              hit->party = party_text;
              hit->amount = abs;
            }
          }
        }
        recipient = nullptr;
        sender = nullptr;  // a triple is consumed by its amount
      }
    }
  }
  return true;
}

// toRecords(): one record per denom the holder's balance moved in, net.
bool to_records(Context& ctx, const Network& network, const char* address, const Value* tx, Value* records,
                Error* error) {
  Arena& a = *ctx.arena;
  const Value* hash = tj::Get(tx, "txhash");
  if (!is_string(hash) || hash->length == 0) return true;
  const Value* code = tj::Get(tx, "code");
  const bool failed = tj::IsNumber(code) && code->number != 0;
  const double height = JsNumber(tj::Get(tx, "height"));
  const Value* events_value = tj::Get(tx, "events");
  Value empty_events;
  empty_events.type = tj::Type::kArray;
  const Value* events = events_value && events_value->type != tj::Type::kNull ? events_value : &empty_events;
  if (!tj::IsArray(events) || !events_well_formed(events)) {
    error->Upstream("cosmos tx: malformed events");
    return false;
  }
  bool tagged = false;
  for (uint32_t i = 0; i < events->count && !tagged; ++i) tagged = is_message(events->items[i]);

  // tx.tx?.auth_info?.fee?.amount?.[0]
  const Value* fee_amounts = tj::Get(tj::Get(tj::Get(tj::Get(tx, "tx"), "auth_info"), "fee"), "amount");
  const Value* fee_coin = nullptr;
  if (tj::IsArray(fee_amounts)) fee_coin = fee_amounts->count ? fee_amounts->items[0] : nullptr;
  else if (tj::IsObject(fee_amounts)) fee_coin = tj::Get(fee_amounts, "0");
  BigInt signed_fee;
  if (fee_coin && tj::StringEquals(tj::Get(fee_coin, "denom"), kNativeDenom)) {
    const Value* raw = tj::Get(fee_coin, "amount");
    char number[64];
    const char* text = nullptr;
    size_t length = 0;
    if (is_string(raw)) { text = raw->text; length = raw->length; }
    else if (tj::IsNumber(raw)) { JsNumberText(raw->number, number, sizeof(number)); text = number; length = strlen(number); }
    else if (!raw || raw->type == tj::Type::kNull) { text = ""; length = 0; }
    if (text && digits_only(text, length) && !BigInt::ParseDecimal(text, length, &signed_fee)) {
      error->Upstream("cosmos fee overflow");
      return false;
    }
  }
  const BigInt zero;
  const bool by_signed_fee = !tagged && signed_fee.Compare(zero) > 0;

  Table nets, fees;
  BigInt holder_spent_native;
  const size_t address_length = strlen(address);
  for (uint32_t e = 0; e < events->count; ++e) {
    const Value* event = events->items[e];
    const Value* type = tj::Get(event, "type");
    const bool received = tj::StringEquals(type, "coin_received");
    if (!received && !tj::StringEquals(type, "coin_spent")) continue;
    const Value* party = attribute_of(event, received ? "receiver" : "spender");
    if (!party) continue;
    const Value* amounts = attribute_of(event, "amount");
    if (amounts && amounts->type != tj::Type::kNull && !is_string(amounts)) {
      error->Upstream("cosmos event: non-string amount");
      return false;
    }
    if (!is_string(amounts)) continue;
    const bool message = is_message(event);
    CoinCursor cursor{amounts->text, amounts->length};
    const char* coin;
    size_t coin_length;
    while (cursor.Next(&coin, &coin_length)) {
      size_t k = 0;
      if (!split_coin_strict(coin, coin_length, &k)) continue;
      BigInt amount;
      if (!BigInt::ParseDecimal(coin, k, &amount)) { error->Upstream("cosmos amount overflow"); return false; }
      const char* denom = coin + k;
      const size_t denom_length = coin_length - k;
      const bool party_is_holder = is_string(party) && party->length == address_length &&
                                   memcmp(party->text, address, address_length) == 0;
      if (!received && str_equals(denom, denom_length, kNativeDenom)) {
        if (!by_signed_fee && !message && is_string(party) &&
            !fees.Add(a, party->text, party->length, amount)) {
          error->Upstream("cosmos fee table");
          return false;
        }
        if (party_is_holder && !holder_spent_native.Add(amount)) { error->Upstream("overflow"); return false; }
      }
      if (!party_is_holder) continue;
      BigInt delta = amount;
      if (!received) delta.Negate();
      if (!nets.Add(a, denom, denom_length, delta)) { error->Upstream("cosmos net table"); return false; }
    }
  }
  if (by_signed_fee && holder_spent_native.Compare(signed_fee) >= 0 &&
      !fees.Set(a, address, address_length, signed_fee)) {
    error->Upstream("cosmos fee table");
    return false;
  }

  Counterparty* parties = nullptr;
  size_t party_count = 0;
  if (!counterparties_of(a, address, events, tagged, &parties, &party_count, error)) return false;

  const Entry* holder_fee_entry = fees.Find(address, address_length);
  const BigInt holder_fee = holder_fee_entry ? holder_fee_entry->amount : BigInt();
  const Entry* native_entry = nets.Find(kNativeDenom, strlen(kNativeDenom));
  const BigInt native_net = native_entry ? native_entry->amount : BigInt();
  Leg* legs = nullptr;
  size_t leg_count = 0, leg_capacity = 0;
  BigInt gross = native_net;
  if (!gross.Add(holder_fee)) { error->Upstream("overflow"); return false; }
  if (!gross.IsZero() && !push_leg(a, &legs, &leg_count, &leg_capacity, Leg{kNativeDenom, strlen(kNativeDenom), gross, false})) {
    error->Upstream("arena exhausted");
    return false;
  }
  for (size_t i = 0; i < nets.count; ++i) {
    const Entry& net = nets.entries[i];
    if (str_equals(net.key, net.key_length, kNativeDenom) || net.amount.IsZero()) continue;
    if (!push_leg(a, &legs, &leg_count, &leg_capacity, Leg{net.key, net.key_length, net.amount, false})) {
      error->Upstream("arena exhausted");
      return false;
    }
  }

  // The fee rides on exactly one record the holder sent.
  bool fee_only = false;
  if (holder_fee.Compare(zero) > 0) {
    Leg* carrier = nullptr;
    for (size_t i = 0; i < leg_count && !carrier; ++i)
      if (str_equals(legs[i].denom, legs[i].denom_length, kNativeDenom) && legs[i].signed_amount.negative())
        carrier = &legs[i];
    for (size_t i = 0; i < leg_count && !carrier; ++i)
      if (legs[i].signed_amount.negative()) carrier = &legs[i];
    if (carrier) {
      carrier->fee = true;
    } else {
      size_t native = leg_count;
      for (size_t i = 0; i < leg_count; ++i)
        if (str_equals(legs[i].denom, legs[i].denom_length, kNativeDenom)) { native = i; break; }
      if (native == leg_count) {
        fee_only = true;
      } else if (native_net.IsZero()) {
        for (size_t i = native + 1; i < leg_count; ++i) legs[i - 1] = legs[i];
        --leg_count;
      } else {
        legs[native] = Leg{kNativeDenom, strlen(kNativeDenom), native_net, false};
      }
    }
  }

  char fee_text[200] = {};
  const bool has_fee = holder_fee.Compare(zero) > 0;
  if (has_fee && !holder_fee.ToDecimal(fee_text, sizeof(fee_text))) { error->Upstream("fee text"); return false; }
  const double decimals = network.has_native ? network.native_decimals : 6;
  const char* symbol = network.has_native ? network.native_symbol : "ATOM";

  // memo / ts / error, with TypeScript's throw cases as failures.
  const Value* memo = tj::Get(tj::Get(tj::Get(tx, "tx"), "body"), "memo");
  Value* memo_value = nullptr;
  if (!memo || (is_string(memo) && memo->length == 0)) {
    memo_value = tj::Null(a);
  } else if (is_string(memo)) {
    memo_value = SanitizeText(a, memo->text, memo->length);
  } else {
    error->Upstream("cosmos tx: non-string memo");
    return false;
  }
  const Value* timestamp = tj::Get(tx, "timestamp");
  double ts = 0;
  if (timestamp) {
    if (is_string(timestamp)) {
      if (!ParseTime(timestamp->text, timestamp->length, &ts)) { error->Upstream("cosmos tx: invalid timestamp"); return false; }
    } else if (tj::IsNumber(timestamp) && isfinite(timestamp->number) && fabs(timestamp->number) <= 8.64e15) {
      ts = trunc(timestamp->number);
    } else if (timestamp->type == tj::Type::kNull) {
      ts = 0;
    } else {
      error->Upstream("cosmos tx: invalid timestamp");
      return false;
    }
  }
  Value* error_value = tj::Null(a);
  if (failed) {
    const Value* raw_log = tj::Get(tx, "raw_log");
    const Value* clean = nullptr;
    if (!raw_log || raw_log->type == tj::Type::kNull) clean = SanitizeText(a, "failed", 6);
    else if (is_string(raw_log)) clean = SanitizeText(a, raw_log->text, raw_log->length);
    else { error->Upstream("cosmos tx: non-string raw_log"); return false; }
    error_value = clean ? codec::provider_text::Slice(a, clean->text, clean->length, 0, 200) : nullptr;
  }
  if (!memo_value || !error_value) { error->Upstream("arena exhausted"); return false; }

  const char* hash_text = tj::CString(a, hash);
  auto make = [&](const Value* record_id, const char* tx_type, const char* direction, const Value* from,
                  const Value* to, const Value* counterparty, const char* token_id, Value* asset,
                  const Value* amount, bool carries_fee, Value* amount_decimals) -> bool {
    Value* r = NewTransfer(a);
    if (!r || !record_id) return false;
    Value* metadata = tj::Obj(a);
    const bool ok =
        metadata && tj::Set(a, r, "status", tj::Str(a, failed ? "failed" : "confirmed")) &&
        tj::Set(a, r, "ts", tj::Num(a, ts)) && tj::Set(a, r, "hash", tj::Str(a, hash->text, hash->length)) &&
        tj::Set(a, r, "blockNumber", isfinite(height) ? tj::Num(a, height) : tj::Null(a)) &&
        tj::Set(a, r, "memo", memo_value) && tj::Set(a, r, "method", tj::Null(a)) &&
        tj::Set(a, r, "error", error_value) && tj::Set(a, r, "metadata", metadata) &&
        tj::Set(a, r, "recordId", const_cast<Value*>(record_id)) && tj::Set(a, r, "txType", tj::Str(a, tx_type)) &&
        tj::Set(a, r, "direction", tj::Str(a, direction)) &&
        tj::Set(a, r, "fromAddress", from ? tj::Clone(a, from) : tj::Null(a)) &&
        tj::Set(a, r, "toAddress", to ? tj::Clone(a, to) : tj::Null(a)) &&
        tj::Set(a, r, "counterparty", counterparty ? tj::Clone(a, counterparty) : tj::Null(a)) &&
        tj::Set(a, r, "tokenId", tj::Str(a, token_id)) && tj::Set(a, r, "asset", asset ? asset : tj::Null(a)) &&
        tj::Set(a, r, "amountRaw", const_cast<Value*>(amount)) &&
        tj::Set(a, r, "amountDecimals", amount_decimals) &&
        tj::Set(a, r, "feeRaw", carries_fee ? tj::Str(a, fee_text) : tj::Null(a)) &&
        tj::Set(a, r, "feeDecimals", carries_fee ? tj::Num(a, decimals) : tj::Null(a)) &&
        tj::Set(a, r, "feeSymbol", carries_fee ? tj::Str(a, symbol) : tj::Null(a)) &&
        tj::Set(a, r, "restatable", tj::Bool(a, true)) && tj::Push(a, records, r);
    return ok;
  };

  Value* address_value = tj::Str(a, address);
  if (!hash_text || !address_value) { error->Upstream("arena exhausted"); return false; }
  for (size_t i = 0; i < leg_count; ++i) {
    const Leg& leg = legs[i];
    const bool native = str_equals(leg.denom, leg.denom_length, kNativeDenom);
    const bool incoming = !leg.signed_amount.negative() && !leg.signed_amount.IsZero();
    const char* direction = DirectionFromFlow(incoming, !incoming);
    const Value* counterparty = nullptr;
    for (size_t c = 0; c < party_count; ++c)
      if (parties[c].denom_length == leg.denom_length && memcmp(parties[c].denom, leg.denom, leg.denom_length) == 0)
        counterparty = tj::Str(a, parties[c].party);
    const char* denom_text = a.CopyText(leg.denom, leg.denom_length);
    const Value* record_id = denom_text ? TransferRecordId(a, network.id, hash_text, native ? nullptr : denom_text) : nullptr;
    if (record_id && record_id->type == tj::Type::kNull) { error->Upstream("cosmos tx: blank hash"); return false; }
    const Value* token_id = native ? nullptr : Format(a, "denom:%s", denom_text ? denom_text : "");
    BigInt abs = leg.signed_amount.Abs();
    char amount_text[200];
    if (!abs.ToDecimal(amount_text, sizeof(amount_text))) { error->Upstream("amount text"); return false; }
    if (!make(record_id, TypeFromDirection(direction), direction, incoming ? counterparty : address_value,
              incoming ? address_value : counterparty, counterparty, native ? "native" : token_id ? token_id->text : "",
              native ? NativeAssetJson(a, network) : nullptr, tj::Str(a, amount_text), leg.fee,
              native ? tj::Num(a, decimals) : tj::Null(a))) {
      error->Upstream("arena exhausted");
      return false;
    }
  }
  if (fee_only) {
    // Only the fee moved: keep the row, or the fee would be invisible.
    const Value* record_id = TransferRecordId(a, network.id, hash_text, nullptr);
    if (record_id && record_id->type == tj::Type::kNull) { error->Upstream("cosmos tx: blank hash"); return false; }
    if (!make(record_id, failed ? TypeFromDirection("outgoing") : "contractCall", "outgoing", address_value, nullptr,
              nullptr, "native", NativeAssetJson(a, network), tj::Str(a, "0"), true, tj::Num(a, decimals))) {
      error->Upstream("arena exhausted");
      return false;
    }
  }
  return true;
}

}  // namespace

bool CosmosSource::Fetch(Context& ctx, const Network& network, const char* address, const Value* state,
                  double backfill_depth, SourceResult* result, Error* error) const {
  Arena& a = *ctx.arena;
  const Value* last = tj::Get(state, "lastHeight");
  const double since_height = tj::IsNumber(last) ? last->number : 0;
  double budget = backfill_depth < kPageLimit * kMaxPages ? backfill_depth : kPageLimit * kMaxPages;
  if (!(budget >= 1)) budget = 1;  // Math.max(1, NaN) is NaN; depth is always numeric here

  Value* records = tj::Arr(a);
  if (!records) { error->Upstream("arena exhausted"); return false; }
  double high_water = since_height;
  bool has_resume = false;
  double resume_at = 0;
  bool complete = true;
  static const char* const kAttributes[] = {"transfer.recipient", "transfer.sender"};
  for (const char* attribute : kAttributes) {
    Value* txs = tj::Arr(a);
    bool exhausted = false;
    if (!txs || !walk(ctx, network, attribute, address, since_height, budget, txs, &exhausted, error)) {
      if (!txs) error->Upstream("arena exhausted");
      return false;
    }
    for (uint32_t i = 0; i < txs->count; ++i) {
      if (!txs->items[i] || txs->items[i]->type == tj::Type::kNull) {
        error->Upstream("cosmos tx: null row");  // tx.height on null throws
        return false;
      }
    }
    if (!exhausted) {
      complete = false;
      bool any = false;
      double max = 0;
      for (uint32_t i = 0; i < txs->count; ++i) {
        const double h = JsNumber(tj::Get(txs->items[i], "height"));
        if (!isfinite(h)) continue;
        if (!any || h > max) max = h;
        any = true;
      }
      if (any && (!has_resume || max < resume_at)) { resume_at = max; has_resume = true; }
    }
    for (uint32_t i = 0; i < txs->count; ++i) {
      const Value* tx = txs->items[i];
      const double h = JsNumber(tj::Get(tx, "height"));
      if (isfinite(h) && h > high_water) high_water = h;
      Value* produced = tj::Arr(a);
      if (!produced || !to_records(ctx, network, address, tx, produced, error)) {
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
  }

  // `tx.height>N` is exclusive: resuming at cut - 1 re-reads the cut height.
  double last_height = high_water;
  if (has_resume) {
    const double lower = since_height + 1;
    const double cut = high_water < resume_at - 1 ? high_water : resume_at - 1;
    last_height = cut > lower ? cut : lower;
  }
  Value* next = tj::IsObject(state) ? tj::Clone(a, state) : tj::Obj(a);
  if (!next || !tj::Set(a, next, "lastHeight", tj::Num(a, last_height))) {
    error->Upstream("arena exhausted");
    return false;
  }
  result->unavailable = false;
  result->records = records;
  result->state = next;
  result->backfill_complete = complete;
  return true;
}

const CosmosDriver kCosmosDriver{};

const CosmosSource kCosmosSource{};


bool CosmosDriver::Registered(const config::ProviderRuntimeConfig& config) const { return cosmos_present(config); }
bool CosmosSource::Registered(const config::ProviderRuntimeConfig& config) const { return cosmos_present(config); }
bool CosmosSource::Supports(const config::ProviderRuntimeConfig& config, const Network& network) const { return cosmos_supports(config, network); }

}  // namespace fortis::chains
