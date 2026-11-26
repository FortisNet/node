#include "token_comparison_store.h"

#include "js_case.h"
#include "kv_store.h"
#include "threading.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace fortis::tokens {
namespace {

using comparison::LiquidityStatus;
using comparison::OptDate;
using comparison::OptNumber;
using comparison::OptString;
using comparison::kMaxDateMs;
using comparison::kMaxStoredString;
using token_store::DatePatch;
using token_store::FieldState;
using token_store::NumberPatch;
using token_store::ReadStatus;
using token_store::StatusPatch;
using token_store::StringPatch;

constexpr char kMagic[4] = {'F', 'T', 'S', '1'};
constexpr size_t kMaxFields = 32;
constexpr size_t kMaxKeyBytes = 1024;  // KvStore kMaxKey

enum Type : uint8_t { kAbsent = 0, kNumber = 1, kDate = 2, kString = 3, kStatus = 4 };

// Field ids. Registry bucket:
enum IdentityField : uint8_t { kSymbol = 1, kName, kLogoMediaKey, kCoingeckoId };
// Comparison bucket:
enum ComparisonField : uint8_t {
  kFiat = 1, kMarketSourceId, kMarketPriceUsd, kChange1h, kChange24h, kChange7d, kChange30d,
  kMarketCapUsd, kFdvUsd, kVolume24hUsd, kMarketCapRank, kMarketAsOf, kMarketSource,
  kDexPriceUsd, kDexMarketCapUsd, kDexFdvUsd, kDexVolume24hUsd, kLiquidityUsd, kSpreadPct,
  kTopPoolShare, kPoolCount, kLiquidityStatus, kLiquidityCheckedAt, kLiquidityAsOf,
  kLiquiditySource, kUpdatedAt, kComparisonFieldEnd,
};

uint8_t identity_type(size_t id) { return id >= kSymbol && id <= kCoingeckoId ? kString : kAbsent; }

uint8_t comparison_type(size_t id) {
  switch (id) {
    case kFiat: case kMarketSourceId: case kMarketSource: case kLiquiditySource: return kString;
    case kMarketAsOf: case kLiquidityCheckedAt: case kLiquidityAsOf: case kUpdatedAt: return kDate;
    case kLiquidityStatus: return kStatus;
    default: return id > 0 && id < kComparisonFieldEnd ? kNumber : kAbsent;
  }
}

struct Field {
  uint8_t type = kAbsent;
  uint8_t status = 0;
  uint16_t length = 0;
  double number = 0;
  int64_t date = 0;
  char text[kMaxStoredString]{};
};

struct Doc {
  Field fields[kMaxFields];
};

bool valid_utf8(const char* text, size_t length) {
  return fortis::codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(text), length);
}

void put64(unsigned char* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<unsigned char>(value >> (8 * i));
}
uint64_t get64(const unsigned char* in) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | in[i];
  return value;
}

constexpr size_t kMaxDocBytes = 4 + kMaxFields * (2 + 2 + kMaxStoredString);

size_t encode(const Doc& doc, unsigned char* out) {
  size_t used = 0;
  memcpy(out, kMagic, 4); used = 4;
  for (size_t id = 1; id < kMaxFields; ++id) {
    const Field& f = doc.fields[id];
    if (f.type == kAbsent) continue;
    out[used++] = static_cast<unsigned char>(id);
    out[used++] = f.type;
    if (f.type == kNumber) {
      uint64_t bits = 0; memcpy(&bits, &f.number, 8);
      put64(out + used, bits); used += 8;
    } else if (f.type == kDate) {
      put64(out + used, static_cast<uint64_t>(f.date)); used += 8;
    } else if (f.type == kString) {
      out[used++] = static_cast<unsigned char>(f.length & 0xff);
      out[used++] = static_cast<unsigned char>(f.length >> 8);
      memcpy(out + used, f.text, f.length); used += f.length;
    } else {
      out[used++] = f.status;
    }
  }
  return used;
}

bool decode(const unsigned char* in, size_t length, uint8_t (*type_of)(size_t), Doc* doc) {
  *doc = Doc{};
  if (length < 4 || memcmp(in, kMagic, 4) != 0) return false;
  size_t position = 4, last_id = 0;
  while (position < length) {
    if (length - position < 2) return false;
    const size_t id = in[position++];
    const uint8_t type = in[position++];
    if (id <= last_id || id >= kMaxFields || type == kAbsent || type_of(id) != type) return false;
    last_id = id;
    Field& f = doc->fields[id];
    f.type = type;
    if (type == kNumber || type == kDate) {
      if (length - position < 8) return false;
      const uint64_t bits = get64(in + position); position += 8;
      if (type == kNumber) {
        memcpy(&f.number, &bits, 8);
        if (!isfinite(f.number)) return false;
      } else {
        f.date = static_cast<int64_t>(bits);
        if (f.date < 0 || f.date > kMaxDateMs) return false;
      }
    } else if (type == kString) {
      if (length - position < 2) return false;
      const size_t n = in[position] | (static_cast<size_t>(in[position + 1]) << 8);
      position += 2;
      if (n > kMaxStoredString || length - position < n ||
          !valid_utf8(reinterpret_cast<const char*>(in + position), n)) return false;
      memcpy(f.text, in + position, n); f.length = static_cast<uint16_t>(n);
      position += n;
    } else {
      if (length - position < 1) return false;
      f.status = in[position++];
      if (f.status < 1 || f.status > 4) return false;
    }
  }
  return true;
}

// encodeURIComponent of valid UTF-8.
bool append_component(char* out, size_t capacity, size_t* used, const char* text) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
    const bool keep = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
        (*c >= '0' && *c <= '9') || strchr("-_.!~*'()", *c) != nullptr;
    if (keep) {
      if (*used + 1 >= capacity) return false;
      out[(*used)++] = static_cast<char>(*c);
    } else {
      if (*used + 3 >= capacity) return false;
      out[(*used)++] = '%';
      out[(*used)++] = kHex[*c >> 4];
      out[(*used)++] = kHex[*c & 15];
    }
  }
  out[*used] = '\0';
  return true;
}

bool make_key(const char* bucket, const char* network, const char* token_id,
              char out[kMaxKeyBytes + 1]) {
  if (!network || !token_id || !valid_utf8(network, strlen(network)) ||
      !valid_utf8(token_id, strlen(token_id))) return false;
  const int prefix = snprintf(out, kMaxKeyBytes + 1, "token/%s/", bucket);
  if (prefix <= 0) return false;
  size_t used = static_cast<size_t>(prefix);
  if (!append_component(out, kMaxKeyBytes + 1, &used, network)) return false;
  if (used + 1 >= kMaxKeyBytes + 1) return false;
  out[used++] = '/';
  out[used] = '\0';
  return append_component(out, kMaxKeyBytes + 1, &used, token_id);
}

bool apply(Field& f, const StringPatch& patch) {
  if (patch.state == FieldState::kUnchanged) return true;
  if (patch.state == FieldState::kNull) { f = Field{}; return true; }
  if ((!patch.text && patch.length) || patch.length > kMaxStoredString ||
      !valid_utf8(patch.text, patch.length)) return false;
  f = Field{}; f.type = kString; f.length = static_cast<uint16_t>(patch.length);
  if (patch.length) memcpy(f.text, patch.text, patch.length);
  return true;
}
bool apply(Field& f, const NumberPatch& patch) {
  if (patch.state == FieldState::kUnchanged) return true;
  f = Field{};
  if (patch.state == FieldState::kValue && isfinite(patch.value)) {
    f.type = kNumber; f.number = patch.value;
  }
  return true;
}
bool apply(Field& f, const DatePatch& patch) {
  if (patch.state == FieldState::kUnchanged) return true;
  if (patch.state == FieldState::kNull) { f = Field{}; return true; }
  if (patch.ms < 0 || patch.ms > kMaxDateMs) return false;
  f = Field{}; f.type = kDate; f.date = patch.ms;
  return true;
}
bool apply(Field& f, const StatusPatch& patch) {
  if (patch.state == FieldState::kUnchanged) return true;
  f = Field{};
  if (patch.state == FieldState::kValue && patch.value != LiquidityStatus::kNone) {
    f.type = kStatus; f.status = static_cast<uint8_t>(patch.value);
  }
  return true;
}

fortis::base::Mutex writer_mutex;
fortis::base::Mutex reader_mutex;
Doc write_doc;
unsigned char write_buffer[kMaxDocBytes];
char write_upper[kMaxStoredString + 1];
Doc read_identity, read_metrics;
unsigned char read_buffer[kMaxDocBytes];

enum class Load : uint8_t { kFound, kMissing, kIoError, kCorrupt };

Load load(const storage::KvStore* kv, const char* key, int64_t now, uint8_t (*type_of)(size_t),
          unsigned char* buffer, Doc* doc, const storage::KvBatch* batch = nullptr) {
  size_t size = 0;
  const storage::KvReadStatus status = batch ? batch->Get(kv, key, now, buffer, kMaxDocBytes, &size)
                                    : kv->GetStatus(key, now, buffer, kMaxDocBytes, &size);
  if (status == storage::KvReadStatus::Missing) { *doc = Doc{}; return Load::kMissing; }
  if (status == storage::KvReadStatus::BufferTooSmall) return Load::kCorrupt;
  if (status != storage::KvReadStatus::Found) return Load::kIoError;
  return decode(buffer, size, type_of, doc) ? Load::kFound : Load::kCorrupt;
}

// Read-modify-write of one document into `batch`; the caller holds
// writer_mutex (write_doc and write_buffer are guarded by it).
template <typename Apply>
bool prepare_document(storage::KvStore* kv, storage::KvBatch& batch, const char* bucket, uint8_t (*type_of)(size_t),
                      const char* network, const char* token_id, int64_t now, Apply&& apply_all) {
  char key[kMaxKeyBytes + 1];
  if (!kv || !make_key(bucket, network, token_id, key)) return false;
  const Load loaded = load(kv, key, now, type_of, write_buffer, &write_doc, &batch);
  if ((loaded != Load::kFound && loaded != Load::kMissing) || !apply_all(write_doc)) return false;
  const size_t size = encode(write_doc, write_buffer);
  return batch.Put(key, write_buffer, size, 0);
}

template <typename Apply>
bool patch_document(storage::KvStore* kv, const char* bucket, uint8_t (*type_of)(size_t),
                    const char* network, const char* token_id, int64_t now, Apply&& apply_all) {
  if (!kv) return false;
  storage::KvBatch batch(kMaxDocBytes + kMaxKeyBytes + 1, 1);
  fortis::base::LockGuard writer_lock(writer_mutex);
  const bool ok = prepare_document(kv, batch, bucket, type_of, network, token_id, now, apply_all) &&
                  batch.Commit(kv);
  return ok;
}

void copy_string(const Field& f, OptString* out) {
  *out = OptString{};
  if (f.type != kString) return;
  out->present = true; out->length = f.length;
  memcpy(out->text, f.text, f.length); out->text[f.length] = '\0';
}
void copy_number(const Field& f, OptNumber* out) {
  *out = OptNumber{};
  if (f.type == kNumber) { out->present = true; out->value = f.number; }
}
void copy_date(const Field& f, OptDate* out) {
  *out = OptDate{};
  if (f.type == kDate) { out->present = true; out->ms = f.date; }
}

bool copy_text(char* out, size_t capacity, const char* text) {
  const size_t n = strlen(text);
  if (n >= capacity) return false;
  memcpy(out, text, n + 1);
  return true;
}

}  // namespace

bool TokenComparisonStore::PatchIdentity(const char* network, const char* token_id,
                                         const token_store::IdentityPatch& patch, int64_t now) {
  return patch_document(kv_, "registry", identity_type, network, token_id, now,
      [&](Doc& doc) {
        // The read side upper-cases the symbol (String.prototype.toUpperCase
        // can grow it, e.g. ß -> SS); a symbol whose mapped form would not
        // fit is refused. write_upper is guarded by the writer mutex.
        size_t length = 0;
        if (patch.symbol.state == FieldState::kValue &&
            (!patch.symbol.text || patch.symbol.length > kMaxStoredString ||
             !compat::JsToUpperCase(patch.symbol.text, patch.symbol.length, write_upper,
                            sizeof(write_upper), &length))) return false;
        return apply(doc.fields[kSymbol], patch.symbol) && apply(doc.fields[kName], patch.name) &&
            apply(doc.fields[kLogoMediaKey], patch.logo_media_key) &&
            apply(doc.fields[kCoingeckoId], patch.coingecko_id);
      });
}

TokenComparisonStore::WriterLock::WriterLock() : guard_(writer_mutex) {}

namespace {
bool apply_comparison(Doc& d, const token_store::ComparisonPatch& p) {
  Field* f = d.fields;
  return apply(f[kFiat], p.fiat) && apply(f[kMarketSourceId], p.market_source_id) &&
      apply(f[kMarketPriceUsd], p.market_price_usd) &&
      apply(f[kChange1h], p.price_change_1h_pct) &&
      apply(f[kChange24h], p.price_change_24h_pct) &&
      apply(f[kChange7d], p.price_change_7d_pct) &&
      apply(f[kChange30d], p.price_change_30d_pct) &&
      apply(f[kMarketCapUsd], p.market_cap_usd) && apply(f[kFdvUsd], p.fdv_usd) &&
      apply(f[kVolume24hUsd], p.volume_24h_usd) &&
      apply(f[kMarketCapRank], p.market_cap_rank) &&
      apply(f[kMarketAsOf], p.market_as_of) && apply(f[kMarketSource], p.market_source) &&
      apply(f[kDexPriceUsd], p.dex_price_usd) &&
      apply(f[kDexMarketCapUsd], p.dex_market_cap_usd) &&
      apply(f[kDexFdvUsd], p.dex_fdv_usd) &&
      apply(f[kDexVolume24hUsd], p.dex_volume_24h_usd) &&
      apply(f[kLiquidityUsd], p.liquidity_usd) && apply(f[kSpreadPct], p.spread_pct) &&
      apply(f[kTopPoolShare], p.top_pool_share) && apply(f[kPoolCount], p.pool_count) &&
      apply(f[kLiquidityStatus], p.liquidity_status) &&
      apply(f[kLiquidityCheckedAt], p.liquidity_checked_at) &&
      apply(f[kLiquidityAsOf], p.liquidity_as_of) &&
      apply(f[kLiquiditySource], p.liquidity_source) && apply(f[kUpdatedAt], p.updated_at);
}
}  // namespace

bool TokenComparisonStore::PatchComparison(const char* network, const char* token_id,
                                           const token_store::ComparisonPatch& p, int64_t now) {
  return patch_document(kv_, "comparison", comparison_type, network, token_id, now,
                        [&](Doc& d) { return apply_comparison(d, p); });
}

bool TokenComparisonStore::PrepareComparison(storage::KvBatch& batch, const char* network, const char* token_id,
                                             const token_store::ComparisonPatch& p, int64_t now) {
  return prepare_document(kv_, batch, "comparison", comparison_type, network, token_id, now,
                          [&](Doc& d) { return apply_comparison(d, p); });
}

ReadStatus TokenComparisonStore::Read(const token_store::Key* keys, size_t count,
                                      const char* fiat, int64_t now,
                                      comparison::Record* records) const {
  if (!kv_ || (count && (!keys || !records)) || !fiat) return ReadStatus::kIoError;
  ReadStatus result = ReadStatus::kOk;
  fortis::base::LockGuard reader_lock(reader_mutex);
  for (size_t i = 0; i < count && result == ReadStatus::kOk; ++i) {
    comparison::Record& r = records[i];
    r = comparison::Record{};
    if (!copy_text(r.network, sizeof(r.network), keys[i].network) ||
        !copy_text(r.token_id, sizeof(r.token_id), keys[i].token_id)) {
      result = ReadStatus::kIoError;
      break;
    }
    char key[kMaxKeyBytes + 1];
    Load identity = Load::kMissing, metrics = Load::kMissing;
    read_identity = Doc{}; read_metrics = Doc{};
    if (make_key("registry", keys[i].network, keys[i].token_id, key))
      identity = load(kv_, key, now, identity_type, read_buffer, &read_identity);
    if (make_key("comparison", keys[i].network, keys[i].token_id, key))
      metrics = load(kv_, key, now, comparison_type, read_buffer, &read_metrics);
    if (identity == Load::kIoError || metrics == Load::kIoError) { result = ReadStatus::kIoError; break; }
    if (identity == Load::kCorrupt || metrics == Load::kCorrupt) { result = ReadStatus::kCorrupt; break; }
    const Field* id = read_identity.fields;
    const Field* m = read_metrics.fields;
    // `stored?.fiat === fiat ? stored : null`
    const bool use_metrics = m[kFiat].type == kString && m[kFiat].length == strlen(fiat) &&
        memcmp(m[kFiat].text, fiat, m[kFiat].length) == 0;
    // symbol?.toUpperCase(): PatchIdentity guarantees the mapped form fits.
    // A display symbol, not a key: the Unicode mapping stays (js-case).
    if (id[kSymbol].type == kString) {
      size_t length = 0;
      if (!compat::JsToUpperCase(id[kSymbol].text, id[kSymbol].length, r.symbol.text,
                         sizeof(r.symbol.text), &length)) { result = ReadStatus::kCorrupt; break; }
      r.symbol.present = true;
      r.symbol.length = length;
    }
    copy_string(id[kName], &r.name);
    copy_string(id[kLogoMediaKey], &r.logo_media_key);
    // string(metric?.marketSourceId) ?? id?.coingeckoId ?? null
    if (use_metrics && m[kMarketSourceId].type == kString)
      copy_string(m[kMarketSourceId], &r.market_source_id);
    else
      copy_string(id[kCoingeckoId], &r.market_source_id);
    if (!use_metrics) continue;
    copy_number(m[kMarketPriceUsd], &r.market_price_usd);
    copy_number(m[kChange1h], &r.price_change_1h_pct);
    copy_number(m[kChange24h], &r.price_change_24h_pct);
    copy_number(m[kChange7d], &r.price_change_7d_pct);
    copy_number(m[kChange30d], &r.price_change_30d_pct);
    copy_number(m[kMarketCapUsd], &r.market_cap_usd);
    copy_number(m[kVolume24hUsd], &r.volume_24h_usd);
    copy_date(m[kMarketAsOf], &r.market_as_of);
    copy_string(m[kMarketSource], &r.market_source);
    copy_number(m[kDexPriceUsd], &r.dex_price_usd);
    copy_number(m[kDexMarketCapUsd], &r.dex_market_cap_usd);
    copy_number(m[kDexVolume24hUsd], &r.dex_volume_24h_usd);
    copy_number(m[kLiquidityUsd], &r.liquidity_usd);
    copy_date(m[kLiquidityAsOf], &r.liquidity_as_of);
    copy_date(m[kLiquidityCheckedAt], &r.liquidity_checked_at);
    copy_string(m[kLiquiditySource], &r.liquidity_source);
    r.liquidity_status = m[kLiquidityStatus].type == kStatus
        ? static_cast<LiquidityStatus>(m[kLiquidityStatus].status) : LiquidityStatus::kNone;
    copy_number(m[kSpreadPct], &r.spread_pct);
    copy_number(m[kTopPoolShare], &r.top_pool_share);
    copy_number(m[kPoolCount], &r.pool_count);
  }
  return result;
}

ReadStatus TokenComparisonStore::ReadIdentity(const char* network, const char* token_id, int64_t now,
                                              token_store::IdentityRecord* record) const {
  if (!kv_ || !record) return ReadStatus::kIoError;
  *record = token_store::IdentityRecord{};
  char key[kMaxKeyBytes + 1];
  if (!make_key("registry", network, token_id, key)) return ReadStatus::kOk;
  fortis::base::LockGuard reader_lock(reader_mutex);
  read_identity = Doc{};
  const Load loaded = load(kv_, key, now, identity_type, read_buffer, &read_identity);
  ReadStatus result = ReadStatus::kOk;
  if (loaded == Load::kIoError) result = ReadStatus::kIoError;
  else if (loaded == Load::kCorrupt) result = ReadStatus::kCorrupt;
  else if (loaded == Load::kFound) {
    record->found = true;
    copy_string(read_identity.fields[kSymbol], &record->symbol);
    copy_string(read_identity.fields[kName], &record->name);
    copy_string(read_identity.fields[kLogoMediaKey], &record->logo_media_key);
    copy_string(read_identity.fields[kCoingeckoId], &record->coingecko_id);
  }
  return result;
}

namespace {
struct ScanState {
  token_store::IdentityVisitor visitor;
  void* context;
  size_t limit;
  size_t visited;
  bool stopped;
  bool truncated;
};
Doc scan_doc;
bool scan_visit(const char*, const void* value, size_t size, ScanState* state) {
  if (state->visited >= state->limit) { state->truncated = true; return false; }
  ++state->visited;
  if (!decode(static_cast<const unsigned char*>(value), size, identity_type, &scan_doc)) return true;
  token_store::IdentityRecord row{};
  row.found = true;
  copy_string(scan_doc.fields[kSymbol], &row.symbol);
  copy_string(scan_doc.fields[kName], &row.name);
  copy_string(scan_doc.fields[kLogoMediaKey], &row.logo_media_key);
  copy_string(scan_doc.fields[kCoingeckoId], &row.coingecko_id);
  if (!state->visitor(state->context, row)) { state->stopped = true; return false; }
  return true;
}
}  // namespace

ReadStatus TokenComparisonStore::ScanIdentities(const char* network, size_t limit, int64_t now,
                                                token_store::IdentityVisitor visitor, void* context,
                                                bool* truncated) const {
  if (truncated) *truncated = false;
  if (!kv_ || !visitor) return ReadStatus::kIoError;
  char prefix[kMaxKeyBytes + 1];
  if (!make_key("registry", network, "", prefix)) return ReadStatus::kOk;
  fortis::base::UniqueLock reader_lock(reader_mutex);
  ScanState state{visitor, context, limit, 0, false, false};
  const bool ok = kv_->Scan(prefix, nullptr, limit + 1, now, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (scan_visit)(key, value, size, visitor_state); });
  reader_lock.Unlock();
  if (truncated) *truncated = state.truncated;
  return ok || state.stopped || state.truncated ? ReadStatus::kOk : ReadStatus::kIoError;
}

}  // namespace fortis::tokens
