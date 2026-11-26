#include "test_support.h"
#include "token_comparison_seed.h"

#include "kv_store.h"
#include "price_history_batch_codec.h"
#include "token_comparison_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

namespace {

using fortis::tokens::token_store::ComparisonPatch;
using fortis::tokens::token_store::DatePatch;
using fortis::tokens::token_store::FieldState;
using fortis::tokens::token_store::IdentityPatch;
using fortis::tokens::token_store::NumberPatch;
using fortis::tokens::token_store::StatusPatch;
using fortis::tokens::token_store::StringPatch;

struct Value {
  enum Kind { kString, kNumber, kDate, kNull } kind = kNull;
  char text[1024]{};
  size_t length = 0;
  double number = 0;
  long long ms = 0;
};

bool parse_value(const char* kind, const char* raw, Value* out) {
  if (strcmp(kind, "null") == 0) { out->kind = Value::kNull; return true; }
  if (strcmp(kind, "num") == 0) {
    char* end = nullptr;
    out->number = strtod(raw, &end);
    out->kind = Value::kNumber;
    return end && end != raw && !*end;
  }
  if (strcmp(kind, "date") == 0) {
    char* end = nullptr;
    out->ms = strtoll(raw, &end, 10);
    out->kind = Value::kDate;
    return end && end != raw && !*end;
  }
  if (strcmp(kind, "str") != 0) return false;
  const size_t hex = strlen(raw);
  if (hex % 2 || hex / 2 >= sizeof(out->text)) return false;
  for (size_t i = 0; i < hex / 2; ++i) {
    unsigned byte = 0;
    if (fortis::test::ParseHexByte(raw + 2 * i, &byte) != 1) return false;
    out->text[i] = static_cast<char>(byte);
  }
  out->length = hex / 2;
  out->kind = Value::kString;
  return true;
}

// The repository's string(): strings only.
StringPatch as_string(const Value& v) {
  StringPatch patch;
  if (v.kind == Value::kString) {
    patch.state = FieldState::kValue; patch.text = v.text; patch.length = v.length;
  } else {
    patch.state = FieldState::kNull;
  }
  return patch;
}

// The repository's number(): Number(string) or a number, finite only.
NumberPatch as_number(const Value& v) {
  NumberPatch patch;
  patch.state = FieldState::kNull;
  double value = 0;
  if (v.kind == Value::kNumber) value = v.number;
  else if (v.kind == Value::kString) {
    static char scratch[2048];
    value = fortis::codec::history_batch::JsStringToNumber(v.text, v.length, scratch, sizeof(scratch));
  } else {
    return patch;
  }
  patch.state = FieldState::kValue;
  patch.value = value;  // the store drops a non-finite value
  return patch;
}

// The repository's date(): Date instances only.
DatePatch as_date(const Value& v) {
  DatePatch patch;
  if (v.kind == Value::kDate) { patch.state = FieldState::kValue; patch.ms = v.ms; }
  else patch.state = FieldState::kNull;
  return patch;
}

StatusPatch as_status(const Value& v) {
  using fortis::tokens::comparison::LiquidityStatus;
  StatusPatch patch;
  patch.state = FieldState::kNull;
  if (v.kind != Value::kString) return patch;
  const struct { const char* name; LiquidityStatus status; } names[] = {
    {"ok", LiquidityStatus::kOk}, {"not_found", LiquidityStatus::kNotFound},
    {"unsupported", LiquidityStatus::kUnsupported}, {"error", LiquidityStatus::kError},
  };
  for (const auto& entry : names)
    if (strlen(entry.name) == v.length && memcmp(entry.name, v.text, v.length) == 0) {
      patch.state = FieldState::kValue; patch.value = entry.status;
    }
  return patch;
}

bool comparison_patch(const char* field, const Value& v, ComparisonPatch* p) {
  const struct { const char* name; StringPatch ComparisonPatch::*member; } strings[] = {
    {"fiat", &ComparisonPatch::fiat}, {"marketSourceId", &ComparisonPatch::market_source_id},
    {"marketSource", &ComparisonPatch::market_source},
    {"liquiditySource", &ComparisonPatch::liquidity_source},
  };
  const struct { const char* name; NumberPatch ComparisonPatch::*member; } numbers[] = {
    {"marketPriceUsd", &ComparisonPatch::market_price_usd},
    {"priceChange1hPct", &ComparisonPatch::price_change_1h_pct},
    {"priceChange24hPct", &ComparisonPatch::price_change_24h_pct},
    {"priceChange7dPct", &ComparisonPatch::price_change_7d_pct},
    {"priceChange30dPct", &ComparisonPatch::price_change_30d_pct},
    {"marketCapUsd", &ComparisonPatch::market_cap_usd}, {"fdvUsd", &ComparisonPatch::fdv_usd},
    {"volume24hUsd", &ComparisonPatch::volume_24h_usd},
    {"marketCapRank", &ComparisonPatch::market_cap_rank},
    {"dexPriceUsd", &ComparisonPatch::dex_price_usd},
    {"dexMarketCapUsd", &ComparisonPatch::dex_market_cap_usd},
    {"dexFdvUsd", &ComparisonPatch::dex_fdv_usd},
    {"dexVolume24hUsd", &ComparisonPatch::dex_volume_24h_usd},
    {"liquidityUsd", &ComparisonPatch::liquidity_usd}, {"spreadPct", &ComparisonPatch::spread_pct},
    {"topPoolShare", &ComparisonPatch::top_pool_share},
    {"poolCount", &ComparisonPatch::pool_count},
  };
  const struct { const char* name; DatePatch ComparisonPatch::*member; } dates[] = {
    {"marketAsOf", &ComparisonPatch::market_as_of},
    {"liquidityCheckedAt", &ComparisonPatch::liquidity_checked_at},
    {"liquidityAsOf", &ComparisonPatch::liquidity_as_of},
    {"updatedAt", &ComparisonPatch::updated_at},
  };
  for (const auto& entry : strings)
    if (strcmp(entry.name, field) == 0) { p->*entry.member = as_string(v); return true; }
  for (const auto& entry : numbers)
    if (strcmp(entry.name, field) == 0) { p->*entry.member = as_number(v); return true; }
  for (const auto& entry : dates)
    if (strcmp(entry.name, field) == 0) { p->*entry.member = as_date(v); return true; }
  if (strcmp(field, "liquidityStatus") == 0) { p->liquidity_status = as_status(v); return true; }
  return false;
}

bool identity_patch(const char* field, const Value& v, IdentityPatch* p) {
  const struct { const char* name; StringPatch IdentityPatch::*member; } strings[] = {
    {"symbol", &IdentityPatch::symbol}, {"name", &IdentityPatch::name},
    {"logoMediaKey", &IdentityPatch::logo_media_key},
    {"coingeckoId", &IdentityPatch::coingecko_id},
  };
  for (const auto& entry : strings)
    if (strcmp(entry.name, field) == 0) { p->*entry.member = as_string(v); return true; }
  return false;
}

bool plain_component(const char* text) {
  for (const char* c = text; *c; ++c)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
          *c == '-' || *c == '_' || *c == '.')) return false;
  return *text != '\0';
}

}  // namespace

bool LoadTokenComparisonSeedFile(fortis::storage::KvStore& kv, const char* path) {
  FILE* file = path ? fopen(path, "r") : nullptr;
  if (!file) return false;
  timeval now{};
  gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  fortis::tokens::TokenComparisonStore store(&kv);
  static char line[4096];
  bool ok = true;
  while (ok && fgets(line, sizeof(line), file)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
    if (!length) continue;
    char* parts[6];
    size_t count = 0;
    for (char* cursor = line; count < 6;) {
      parts[count++] = cursor;
      char* tab = strchr(cursor, '\t');
      if (!tab) break;
      *tab = '\0';
      cursor = tab + 1;
    }
    if (count != 6) { ok = false; break; }
    const char* bucket = parts[0];
    const char* network = parts[1];
    const char* token_id = parts[2];
    if (strcmp(parts[4], "corrupt") == 0) {
      // Only plain test ids are supported, so the key needs no escaping.
      if (!plain_component(network) || !plain_component(token_id)) { ok = false; break; }
      char key[256];
      snprintf(key, sizeof(key), "token/%s/%s/%s", bucket, network, token_id);
      const char garbage[] = "not a token document";
      const fortis::storage::KvMutation mutation{key, garbage, sizeof(garbage), 0, false};
      ok = kv.Write(&mutation, 1);
      continue;
    }
    static Value value;
    value = Value{};
    if (!parse_value(parts[4], parts[5], &value)) { ok = false; break; }
    if (strcmp(bucket, "registry") == 0) {
      IdentityPatch patch;
      ok = identity_patch(parts[3], value, &patch) &&
          store.PatchIdentity(network, token_id, patch, now_ms);
    } else if (strcmp(bucket, "comparison") == 0) {
      static ComparisonPatch patch;
      patch = ComparisonPatch{};
      ok = comparison_patch(parts[3], value, &patch) &&
          store.PatchComparison(network, token_id, patch, now_ms);
    } else {
      ok = false;
    }
    if (!ok) fprintf(stderr, "token comparison seed failed: %s %s %s %s\n", bucket, network,
                     token_id, parts[3]);
  }
  fclose(file);
  return ok;
}
