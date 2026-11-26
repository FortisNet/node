#include <cstdio>
#include <algorithm>
#include "ownership.h"
#include "price_series_store.h"

#include "threading.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {
namespace {
constexpr char kSpotPrefix[] = "price/series/v1/";
constexpr char kOpenPrefix[] = "price/open/v1/";
constexpr char kBlockPrefix[] = "price/block/v1/";
constexpr char kSealPrefix[] = "price/seal/v1/";
// v1 held one exact-window marker per key. It is retired, never read again,
// and its keys expire through their own six-hour KV TTL.
constexpr char kCoveragePrefix[] = "price/history-coverage/v2/";
constexpr size_t kKeyCapacity = 600;
constexpr int64_t kDay = 86'400'000;
constexpr int64_t kWeek = 7 * kDay;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kHasChange = 1, kHasVolume = 2;
constexpr size_t kSpotCrc = 80, kOpenBytes = 76, kOpenCrc = 72;
constexpr size_t kMarkerBytes = 28, kMarkerCrc = 24;
constexpr size_t kBlockHeader = 20, kBlockMaxSlots = 168, kCandleBytes = 65;
constexpr size_t kBlockMaxBytes =
    kBlockHeader + kBlockMaxSlots * kCandleBytes + 4;
constexpr size_t kQuoteMaxMutations = kPriceSeriesMaxBatchItems * 7;
constexpr size_t kCombinedProviderMaxMutations =
    kPriceProviderBatchMaxMutations;
constexpr size_t kSealFrameMaxSegments = 16;
constexpr size_t kSealFrameMaxRows = 200;
constexpr size_t kSealMaxMutations =
    kSealFrameMaxRows + 2 * kSealFrameMaxSegments;
constexpr size_t kSealMaxPerCall = 1000;
constexpr size_t kUpsertMaxMutations = kPriceSeriesMaxUpsertItems * 2;
constexpr size_t kUpsertMaxBlockBytes =
    kPriceSeriesMaxUpsertItems * kBlockMaxBytes;
static_assert(kSpotCrc + 4 == kPriceSeriesEncodedBytes,
              "spot series record size mismatch");
static_assert(kQuoteMaxMutations *
                          (15 + kKeyCapacity + kPriceSeriesEncodedBytes) +
                      4 <
                  8 * 1024 * 1024,
              "series provider batch must fit one KV frame");
static_assert(kQuoteMaxMutations *
                      (15 + kKeyCapacity + kPriceSeriesEncodedBytes) +
                  kPriceSpotMaxBatchItems *
                      (15 + kKeyCapacity + kPriceSpotEncodedBytes) <
                  8 * 1024 * 1024,
              "combined provider batch must fit one KV frame");
static_assert(kSealMaxMutations * (15 + kKeyCapacity) +
                      kSealFrameMaxSegments * kBlockMaxBytes <
                  8 * 1024 * 1024,
              "sealer frame must fit KV frame cap");
static_assert(kUpsertMaxMutations * (15 + kKeyCapacity) + kUpsertMaxBlockBytes <
                  8 * 1024 * 1024,
              "upsert frame must fit KV frame cap");

struct Candle {
  int64_t ts = 0, first = 0, last = 0;
  double open = 0, high = 0, low = 0, close = 0, volume = 0;
  bool has_volume = false, present = false;
};
struct Spot {
  uint8_t flags = 0;
  int64_t as_of = 0, expires = 0;
  double price = 0, change = 0, volume = 0;
  char provider[kPriceSpotMaxProviderBytes + 1]{};
};
struct Block {
  size_t interval = 0;
  int64_t start = 0;
  Candle slots[kBlockMaxSlots]{};
};
struct Marker {
  size_t interval = 0;
  int64_t start = 0, expires = 0;
  uint16_t next_slot = 0;
};
struct Workspace {
  storage::KvMutation mutations[kQuoteMaxMutations]{};
  char keys[kQuoteMaxMutations][kKeyCapacity]{};
  uint8_t values[kQuoteMaxMutations][kPriceSeriesEncodedBytes]{};
  uint8_t marker_values[kQuoteMaxMutations][kMarkerBytes]{};
};
struct OpenPending {
  char key[kKeyCapacity]{};
  Candle candle{};
  int64_t expiry = 0;
  uint8_t encoded[kOpenBytes]{};
};
struct BlockPending {
  char key[kKeyCapacity]{};
  Block block{};
  int64_t expiry = 0;
  uint8_t *encoded = nullptr;
  size_t encoded_size = 0;
};
struct UpsertWorkspace {
  OpenPending opens[kPriceSeriesMaxUpsertItems]{};
  BlockPending blocks[kPriceSeriesMaxUpsertItems]{};
  uint8_t block_values[kUpsertMaxBlockBytes]{};
  storage::KvMutation mutations[kUpsertMaxMutations]{};
  char block_keys[kPriceSeriesMaxUpsertItems][kKeyCapacity]{};
  char marker_keys[kPriceSeriesMaxUpsertItems][kKeyCapacity]{};
  uint8_t marker_values[kPriceSeriesMaxUpsertItems][kMarkerBytes]{};
  size_t open_count = 0, block_count = 0, marker_count = 0, mutation_count = 0,
         block_value_offset = 0;
};
struct SealDesc {
  char key[kKeyCapacity]{};
  char fiat[6]{};
  char source_id[kPriceSpotMaxSourceIdBytes + 1]{};
  size_t interval = 0;
  int64_t due = 0, start = 0;
  Marker marker{};
};
struct SealWorkspace {
  SealDesc segments[kSealFrameMaxSegments]{};
  storage::KvMutation mutations[kSealMaxMutations]{};
  char keys[kSealMaxMutations][kKeyCapacity]{};
  uint8_t block_values[kSealFrameMaxSegments][kBlockMaxBytes]{};
  uint8_t marker_values[kSealFrameMaxSegments][kMarkerBytes]{};
  Candle open_rows[kBlockMaxSlots]{};
  Block block{};
};
struct MarkerCollect {
  SealDesc *items;
  size_t capacity = 0, count = 0;
  int64_t now = 0;
  bool invalid = false, future = false, more = false;
};
struct OpenCollect {
  Candle *items;
  size_t capacity = 0, count = 0;
  size_t interval = 0;
  int64_t segment_start = 0;
  bool has_more = false, reached_end = false;
  int64_t next_ts = 0;
  bool invalid = false;
};
struct CandleCollect {
  PriceSeriesCandle *items;
  size_t capacity = 0, count = 0;
  size_t interval = 0;
  int64_t from = 0, to = 0;
  int64_t last_ts = 0;
  bool has_last = false, overflow = false, invalid = false;
};
struct EdgeCollect {
  size_t interval = 0;
  bool found = false, invalid = false;
  int64_t min_ts = INT64_MAX, max_ts = 0;
};

// A single lock covers the complete read/merge/write sequence across wrappers
// sharing this KV instance. KvStore itself serializes individual operations.
fortis::base::Mutex g_series_mutex;
bool FiatOk(const char *s) {
  if (!s)
    return false;
  const size_t n = strnlen(s, 6);
  if (n < 3 || n > 5)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (s[i] < 'a' || s[i] > 'z')
      return false;
  return true;
}
bool IdOk(const char *s) {
  if (!s)
    return false;
  const size_t n = strnlen(s, kPriceSpotMaxSourceIdBytes + 1);
  if (!n || n > kPriceSpotMaxSourceIdBytes || s[0] == ' ' || s[n - 1] == ' ')
    return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x21 || c > 0x7e || c == '/' || c == '\\')
      return false;
  }
  return true;
}
bool ProviderOk(const char *s) {
  if (!s)
    return false;
  const size_t n = strnlen(s, kPriceSpotMaxProviderBytes + 1);
  if (!n || n > kPriceSpotMaxProviderBytes)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
          s[i] == '-'))
      return false;
  return true;
}
void Put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t Get32(const uint8_t *p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
void Put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
uint16_t Get16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}
void Put64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint64_t Get64(const uint8_t *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}
void PutDouble(uint8_t *p, double v) {
  uint64_t u = 0;
  memcpy(&u, &v, 8);
  Put64(p, u);
}
double GetDouble(const uint8_t *p) {
  uint64_t u = Get64(p);
  double v = 0;
  memcpy(&v, &u, 8);
  return v;
}
uint32_t Crc(const uint8_t *p, size_t n) {
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (unsigned b = 0; b < 8; ++b)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}
bool ParseInterval(const char *s, size_t *interval) {
  if (!strcmp(s, "5m"))
    *interval = 0;
  else if (!strcmp(s, "1h"))
    *interval = 1;
  else if (!strcmp(s, "1d"))
    *interval = 2;
  else
    return false;
  return true;
}
const char *IntervalName(size_t i) {
  return i == 0 ? "5m" : i == 1 ? "1h" : "1d";
}
int64_t Width(size_t i) { return i == 0 ? 300'000 : i == 1 ? 3'600'000 : kDay; }
int64_t Retain(size_t i) {
  return i == 0 ? 7 * kDay : i == 1 ? 90 * kDay : 366 * kDay;
}
size_t SlotCount(size_t i) { return i == 0 ? 12 : i == 1 ? 168 : 31; }
int64_t Bucket(size_t i, int64_t ts) { return ts / Width(i) * Width(i); }
int64_t SegmentStart(size_t i, int64_t ts) {
  if (i == 0)
    return ts / 3'600'000 * 3'600'000;
  if (i == 1)
    return ts / kWeek * kWeek;
  time_t t = static_cast<time_t>(ts / 1000);
  struct tm d{};
  if (!gmtime_r(&t, &d))
    return ts;
  d.tm_mday = 1;
  d.tm_hour = d.tm_min = d.tm_sec = 0;
  return static_cast<int64_t>(timegm(&d)) * 1000;
}
int64_t SegmentEnd(size_t i, int64_t ts) {
  const int64_t start = SegmentStart(i, ts);
  if (i == 0)
    return start + 3'600'000;
  if (i == 1)
    return start + kWeek;
  time_t t = static_cast<time_t>(start / 1000);
  struct tm d{};
  if (!gmtime_r(&t, &d))
    return start + 32 * kDay;
  ++d.tm_mon;
  return static_cast<int64_t>(timegm(&d)) * 1000;
}
int64_t CandleExpiry(size_t i, int64_t ts, int64_t now) {
  const int64_t end = SegmentEnd(i, ts);
  return now + 7 * kDay > end + Retain(i) ? now + 7 * kDay : end + Retain(i);
}
size_t SlotOf(size_t i, int64_t ts, int64_t start) {
  return static_cast<size_t>((ts - start) / Width(i));
}
size_t SegmentCount(size_t i, int64_t from, int64_t to) {
  const int64_t first = SegmentStart(i, from), last = SegmentStart(i, to);
  if (i < 2)
    return static_cast<size_t>((last - first) / (i == 0 ? 3'600'000 : kWeek) +
                               1);
  time_t a = static_cast<time_t>(first / 1000),
         b = static_cast<time_t>(last / 1000);
  struct tm da{}, db{};
  if (!gmtime_r(&a, &da) || !gmtime_r(&b, &db))
    return 5000;
  const int64_t months = static_cast<int64_t>(db.tm_year - da.tm_year) * 12 +
                         db.tm_mon - da.tm_mon + 1;
  return months > 0 && months <= INT32_MAX ? static_cast<size_t>(months) : 5000;
}
size_t BucketSpan(size_t i, int64_t from, int64_t to) {
  return static_cast<size_t>((Bucket(i, to) - Bucket(i, from)) / Width(i) + 1);
}
bool SpotKey(const char *fiat, const char *id, char *out) {
  if (!FiatOk(fiat) || !IdOk(id))
    return false;
  const int n = snprintf(out, kKeyCapacity, "%s%s/%s", kSpotPrefix, fiat, id);
  return n > 0 && static_cast<size_t>(n) < kKeyCapacity;
}

// Coverage exists only for the provider-backed 1h and 1d resolutions.
bool HistoryCoverageKey(PriceSeriesInterval interval, const char *fiat,
                        const char *id, char *out, size_t *length) {
  if ((interval != PriceSeriesInterval::k1h &&
       interval != PriceSeriesInterval::k1d) ||
      !FiatOk(fiat) || !IdOk(id))
    return false;
  const int n = snprintf(out, kKeyCapacity, "%s%s/%s/%s", kCoveragePrefix,
                         IntervalName(static_cast<size_t>(interval)), fiat, id);
  if (n <= 0 || static_cast<size_t>(n) >= kKeyCapacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}
bool OpenPrefix(size_t i, const char *fiat, const char *id, char *out) {
  if (i > 2 || !FiatOk(fiat) || !IdOk(id))
    return false;
  const int n = snprintf(out, kKeyCapacity, "%s%s/%s/%s/", kOpenPrefix,
                         IntervalName(i), fiat, id);
  return n > 0 && static_cast<size_t>(n) < kKeyCapacity;
}
bool AppendTimestamp(const char *prefix, int64_t ts, char *out,
                     size_t capacity) {
  if (ts < 0)
    return false;
  const size_t prefix_size = strlen(prefix);
  if (prefix_size > capacity || capacity - prefix_size < 20)
    return false;
  std::copy_n(prefix, prefix_size, out);
  const int written = snprintf(out + prefix_size, capacity - prefix_size,
                               "%019llu", static_cast<unsigned long long>(ts));
  return written == 19;
}
bool OpenKey(size_t i, const char *fiat, const char *id, int64_t ts,
             char *out) {
  char prefix[kKeyCapacity];
  if (!OpenPrefix(i, fiat, id, prefix))
    return false;
  return AppendTimestamp(prefix, ts, out, kKeyCapacity);
}
bool BlockPrefix(size_t i, const char *fiat, const char *id, char *out) {
  if (i > 2 || !FiatOk(fiat) || !IdOk(id))
    return false;
  const int n = snprintf(out, kKeyCapacity, "%s%s/%s/%s/", kBlockPrefix,
                         IntervalName(i), fiat, id);
  return n > 0 && static_cast<size_t>(n) < kKeyCapacity;
}
bool BlockKey(size_t i, const char *fiat, const char *id, int64_t start,
              char *out) {
  char prefix[kKeyCapacity];
  if (!BlockPrefix(i, fiat, id, prefix))
    return false;
  return AppendTimestamp(prefix, start, out, kKeyCapacity);
}
bool SealKey(size_t i, const char *fiat, const char *id, int64_t start,
             char *out) {
  if (i > 2 || !FiatOk(fiat) || !IdOk(id))
    return false;
  const int n =
      snprintf(out, kKeyCapacity, "%s%019lld/%s/%s/%s/%019lld", kSealPrefix,
               static_cast<long long>(SegmentEnd(i, start)), IntervalName(i),
               fiat, id, static_cast<long long>(start));
  return n > 0 && static_cast<size_t>(n) < kKeyCapacity;
}
void EncodeSpot(const Spot &s, uint8_t *out) {
  memset(out, 0, kPriceSeriesEncodedBytes);
  std::copy_n("FPSP", 4, out);
  out[4] = kVersion;
  out[5] = s.flags;
  out[6] = static_cast<uint8_t>(strlen(s.provider));
  Put64(out + 8, static_cast<uint64_t>(s.as_of));
  Put64(out + 16, static_cast<uint64_t>(s.expires));
  PutDouble(out + 24, s.price);
  PutDouble(out + 32, s.change);
  PutDouble(out + 40, s.volume);
  memcpy(out + 48, s.provider, strlen(s.provider));
  Put32(out + kSpotCrc, Crc(out, kSpotCrc));
}
bool DecodeSpot(const uint8_t *in, Spot *s) {
  if (memcmp(in, "FPSP", 4) != 0 || in[4] != kVersion || (in[5] & ~3u) || in[7] || !in[6] || in[6] > 31 ||
      Get32(in + kSpotCrc) != Crc(in, kSpotCrc))
    return false;
  for (size_t i = 0; i < 32; ++i)
    if (i < in[6] ? in[48 + i] == 0 : in[48 + i] != 0)
      return false;
  *s = Spot{};
  s->flags = in[5];
  s->as_of = static_cast<int64_t>(Get64(in + 8));
  s->expires = static_cast<int64_t>(Get64(in + 16));
  s->price = GetDouble(in + 24);
  s->change = GetDouble(in + 32);
  s->volume = GetDouble(in + 40);
  memcpy(s->provider, in + 48, in[6]);
  return s->as_of >= 0 && s->expires > s->as_of && ProviderOk(s->provider) &&
         isfinite(s->price) && isfinite(s->change) && isfinite(s->volume);
}
void EncodeCandle(const Candle &c, uint8_t *out) {
  memset(out, 0, kOpenBytes);
  std::copy_n("FPSO", 4, out);
  out[4] = kVersion;
  out[5] = c.has_volume ? 1 : 0;
  Put64(out + 8, static_cast<uint64_t>(c.ts));
  Put64(out + 16, static_cast<uint64_t>(c.first));
  Put64(out + 24, static_cast<uint64_t>(c.last));
  PutDouble(out + 32, c.open);
  PutDouble(out + 40, c.high);
  PutDouble(out + 48, c.low);
  PutDouble(out + 56, c.close);
  PutDouble(out + 64, c.volume);
  Put32(out + kOpenCrc, Crc(out, kOpenCrc));
}
bool DecodeCandle(const uint8_t *in, size_t size, Candle *c) {
  if (size != kOpenBytes || memcmp(in, "FPSO", 4) != 0 || in[4] != kVersion || (in[5] & ~1u) || in[6] || in[7] ||
      Get32(in + kOpenCrc) != Crc(in, kOpenCrc))
    return false;
  c->ts = static_cast<int64_t>(Get64(in + 8));
  c->first = static_cast<int64_t>(Get64(in + 16));
  c->last = static_cast<int64_t>(Get64(in + 24));
  c->open = GetDouble(in + 32);
  c->high = GetDouble(in + 40);
  c->low = GetDouble(in + 48);
  c->close = GetDouble(in + 56);
  c->volume = GetDouble(in + 64);
  c->has_volume = in[5] != 0;
  return c->ts >= 0 && c->first >= 0 && c->last >= c->first &&
         isfinite(c->open) && isfinite(c->high) && isfinite(c->low) &&
         isfinite(c->close) && isfinite(c->volume);
}
void EncodeMarker(const Marker &m, uint8_t *out) {
  memset(out, 0, kMarkerBytes);
  std::copy_n("FPSM", 4, out);
  out[4] = kVersion;
  out[5] = static_cast<uint8_t>(m.interval);
  Put16(out + 6, m.next_slot);
  Put64(out + 8, static_cast<uint64_t>(m.start));
  Put64(out + 16, static_cast<uint64_t>(m.expires));
  Put32(out + kMarkerCrc, Crc(out, kMarkerCrc));
}
bool DecodeMarker(const uint8_t *in, size_t size, Marker *m) {
  if (size != kMarkerBytes || memcmp(in, "FPSM", 4) != 0 || in[4] != kVersion || in[5] > 2 ||
      Get32(in + kMarkerCrc) != Crc(in, kMarkerCrc))
    return false;
  m->interval = in[5];
  m->next_slot = Get16(in + 6);
  m->start = static_cast<int64_t>(Get64(in + 8));
  m->expires = static_cast<int64_t>(Get64(in + 16));
  return m->next_slot <= SlotCount(m->interval) && m->start >= 0 &&
         m->expires > m->start &&
         SegmentStart(m->interval, m->start) == m->start;
}
void EncodePackedCandle(const Candle &c, uint8_t *out) {
  memset(out, 0, kCandleBytes);
  if (!c.present)
    return;
  Put64(out, static_cast<uint64_t>(c.ts));
  Put64(out + 8, static_cast<uint64_t>(c.first));
  Put64(out + 16, static_cast<uint64_t>(c.last));
  PutDouble(out + 24, c.open);
  PutDouble(out + 32, c.high);
  PutDouble(out + 40, c.low);
  PutDouble(out + 48, c.close);
  PutDouble(out + 56, c.volume);
  out[64] = c.has_volume ? 2 : 1;
}
bool DecodePackedCandle(const uint8_t *in, Candle *c, bool *present) {
  if (in[64] > 2)
    return false;
  *present = in[64] != 0;
  if (!*present) {
    for (size_t i = 0; i < kCandleBytes; ++i)
      if (in[i])
        return false;
    *c = Candle{};
    return true;
  }
  c->ts = static_cast<int64_t>(Get64(in));
  c->first = static_cast<int64_t>(Get64(in + 8));
  c->last = static_cast<int64_t>(Get64(in + 16));
  c->open = GetDouble(in + 24);
  c->high = GetDouble(in + 32);
  c->low = GetDouble(in + 40);
  c->close = GetDouble(in + 48);
  c->volume = GetDouble(in + 56);
  c->has_volume = in[64] == 2;
  c->present = true;
  return c->ts >= 0 && c->first >= 0 && c->last >= c->first &&
         isfinite(c->open) && isfinite(c->high) && isfinite(c->low) &&
         isfinite(c->close) && isfinite(c->volume);
}
size_t BlockBytes(size_t i) {
  return kBlockHeader + SlotCount(i) * kCandleBytes + 4;
}
void EncodeBlock(const Block &b, uint8_t *out) {
  const size_t bytes = BlockBytes(b.interval);
  memset(out, 0, bytes);
  std::copy_n("FPSB", 4, out);
  out[4] = kVersion;
  out[5] = static_cast<uint8_t>(b.interval);
  Put16(out + 6, static_cast<uint16_t>(SlotCount(b.interval)));
  Put64(out + 8, static_cast<uint64_t>(b.start));
  for (size_t i = 0; i < SlotCount(b.interval); ++i)
    EncodePackedCandle(b.slots[i], out + kBlockHeader + i * kCandleBytes);
  Put32(out + bytes - 4, Crc(out, bytes - 4));
}
bool DecodeBlock(size_t interval, int64_t start, const uint8_t *in,
                 size_t bytes, Block *b) {
  if (interval > 2 || bytes != BlockBytes(interval) || memcmp(in, "FPSB", 4) != 0 || in[4] != kVersion ||
      in[5] != interval || Get16(in + 6) != SlotCount(interval) || static_cast<int64_t>(Get64(in + 8)) != start ||
      Get32(in + bytes - 4) != Crc(in, bytes - 4))
    return false;
  *b = Block{};
  b->interval = interval;
  b->start = start;
  for (size_t i = 0; i < SlotCount(interval); ++i) {
    bool present = false;
    if (!DecodePackedCandle(in + kBlockHeader + i * kCandleBytes, &b->slots[i],
                            &present))
      return false;
    if (present &&
        b->slots[i].ts != start + static_cast<int64_t>(i) * Width(interval))
      return false;
  }
  return true;
}
bool ReadSpot(storage::KvStore &store, const char *key, int64_t now, Spot *s,
              PriceSeriesReadStatus *status) {
  uint8_t bytes[kPriceSeriesEncodedBytes]{};
  size_t size = 0;
  const storage::KvReadStatus r = store.GetStatus(key, now, bytes, sizeof(bytes), &size);
  if (r == storage::KvReadStatus::Missing) {
    *status = PriceSeriesReadStatus::kMissing;
    return false;
  }
  if (r == storage::KvReadStatus::IoError) {
    *status = PriceSeriesReadStatus::kIoError;
    return false;
  }
  if (r != storage::KvReadStatus::Found || size != sizeof(bytes) ||
      !DecodeSpot(bytes, s)) {
    *status = PriceSeriesReadStatus::kCorrupt;
    return false;
  }
  *status = PriceSeriesReadStatus::kFound;
  return true;
}
bool ReadCandle(storage::KvStore &store, const char *key, int64_t now, Candle *c,
                PriceSeriesReadStatus *status) {
  uint8_t bytes[kOpenBytes]{};
  size_t size = 0;
  const storage::KvReadStatus r = store.GetStatus(key, now, bytes, sizeof(bytes), &size);
  if (r == storage::KvReadStatus::Missing) {
    *status = PriceSeriesReadStatus::kMissing;
    return false;
  }
  if (r == storage::KvReadStatus::IoError) {
    *status = PriceSeriesReadStatus::kIoError;
    return false;
  }
  if (r != storage::KvReadStatus::Found || !DecodeCandle(bytes, size, c)) {
    *status = PriceSeriesReadStatus::kCorrupt;
    return false;
  }
  *status = PriceSeriesReadStatus::kFound;
  return true;
}
bool ReadBlock(storage::KvStore &store, const char *key, size_t interval, int64_t start,
               int64_t now, Block *b, PriceSeriesReadStatus *status) {
  uint8_t bytes[kBlockMaxBytes]{};
  size_t size = 0;
  const storage::KvReadStatus r = store.GetStatus(key, now, bytes, sizeof(bytes), &size);
  if (r == storage::KvReadStatus::Missing) {
    *status = PriceSeriesReadStatus::kMissing;
    return false;
  }
  if (r == storage::KvReadStatus::IoError) {
    *status = PriceSeriesReadStatus::kIoError;
    return false;
  }
  if (r != storage::KvReadStatus::Found ||
      !DecodeBlock(interval, start, bytes, size, b)) {
    *status = PriceSeriesReadStatus::kCorrupt;
    return false;
  }
  *status = PriceSeriesReadStatus::kFound;
  return true;
}
bool ReadMarker(storage::KvStore &store, const char *key, int64_t now, Marker *m,
                PriceSeriesReadStatus *status) {
  uint8_t bytes[kMarkerBytes]{};
  size_t size = 0;
  const storage::KvReadStatus r = store.GetStatus(key, now, bytes, sizeof(bytes), &size);
  if (r == storage::KvReadStatus::Missing) {
    *status = PriceSeriesReadStatus::kMissing;
    return false;
  }
  if (r == storage::KvReadStatus::IoError) {
    *status = PriceSeriesReadStatus::kIoError;
    return false;
  }
  if (r != storage::KvReadStatus::Found || !DecodeMarker(bytes, size, m)) {
    *status = PriceSeriesReadStatus::kCorrupt;
    return false;
  }
  *status = PriceSeriesReadStatus::kFound;
  return true;
}
bool ParseTimestampSuffix(const char *key, int64_t *timestamp) {
  const char *tail = strrchr(key, '/');
  if (!tail || !tail[1])
    return false;
  char *end = nullptr;
  const long long value = strtoll(tail + 1, &end, 10);
  if (!end || *end || value < 0)
    return false;
  *timestamp = static_cast<int64_t>(value);
  return true;
}
bool ParseSealKey(const char *key, SealDesc *out) {
  const size_t prefix = sizeof(kSealPrefix) - 1;
  if (strncmp(key, kSealPrefix, prefix) != 0)
    return false;
  const char *p = key + prefix;
  char *end = nullptr;
  const long long due = strtoll(p, &end, 10);
  if (!end || end == p || *end != '/' || strlen(p) < 21 || due < 0)
    return false;
  out->due = due;
  p = end + 1;
  const char *slash = strchr(p, '/');
  if (!slash || static_cast<size_t>(slash - p) >= 4)
    return false;
  char interval[4]{};
  memcpy(interval, p, static_cast<size_t>(slash - p));
  if (!ParseInterval(interval, &out->interval))
    return false;
  p = slash + 1;
  slash = strchr(p, '/');
  if (!slash || slash - p < 3 || slash - p > 5)
    return false;
  memcpy(out->fiat, p, static_cast<size_t>(slash - p));
  out->fiat[slash - p] = '\0';
  if (!FiatOk(out->fiat))
    return false;
  p = slash + 1;
  slash = strchr(p, '/');
  if (!slash || slash == p ||
      slash - p > static_cast<ptrdiff_t>(kPriceSpotMaxSourceIdBytes))
    return false;
  memcpy(out->source_id, p, static_cast<size_t>(slash - p));
  out->source_id[slash - p] = '\0';
  if (!IdOk(out->source_id) || strlen(slash + 1) != 19)
    return false;
  end = nullptr;
  const long long start = strtoll(slash + 1, &end, 10);
  if (!end || *end || start < 0)
    return false;
  out->start = start;
  return SegmentStart(out->interval, out->start) == out->start &&
         SegmentEnd(out->interval, out->start) == out->due;
}
bool MarkerMatches(const uint8_t *bytes, size_t size, const SealDesc &desc,
                   Marker *marker) {
  if (!DecodeMarker(bytes, size, marker))
    return false;
  return marker->interval == desc.interval && marker->start == desc.start &&
         marker->expires > desc.due &&
         marker->next_slot <= SlotCount(desc.interval);
}
bool VisitMarker(const char *key, const void *value, size_t size,
                 void *opaque) {
  auto *ctx = static_cast<MarkerCollect *>(opaque);
  if (ctx->count == ctx->capacity) {
    ctx->more = true;
    return false;
  }
  SealDesc desc{};
  Marker marker{};
  if (!ParseSealKey(key, &desc)) {
    ctx->invalid = true;
    return false;
  }
  if (desc.due > ctx->now) {
    ctx->future = true;
    return false;
  }
  if (!MarkerMatches(static_cast<const uint8_t *>(value), size, desc,
                     &marker)) {
    ctx->invalid = true;
    return false;
  }
  memcpy(desc.key, key, strlen(key) + 1);
  desc.marker = marker;
  ctx->items[ctx->count++] = desc;
  return true;
}
bool VisitOpenForSeal(const char *key, const void *value, size_t size,
                      void *opaque) {
  auto *ctx = static_cast<OpenCollect *>(opaque);
  Candle candle{};
  int64_t key_ts = 0;
  if (!DecodeCandle(static_cast<const uint8_t *>(value), size, &candle) ||
      !ParseTimestampSuffix(key, &key_ts) || key_ts != candle.ts) {
    ctx->invalid = true;
    return false;
  }
  const int64_t end = SegmentEnd(ctx->interval, ctx->segment_start);
  if (candle.ts >= end) {
    ctx->reached_end = true;
    return false;
  }
  if (candle.ts < ctx->segment_start) {
    ctx->invalid = true;
    return false;
  }
  if (ctx->count == ctx->capacity) {
    ctx->has_more = true;
    ctx->next_ts = candle.ts;
    return false;
  }
  ctx->items[ctx->count++] = candle;
  return true;
}
bool AppendRange(const PriceSeriesCandle& candle, CandleCollect *ctx) {
  if (candle.ts < ctx->from || candle.ts > ctx->to)
    return true;
  if (ctx->has_last && candle.ts < ctx->last_ts) {
    ctx->invalid = true;
    return false;
  }
  ctx->last_ts = candle.ts;
  ctx->has_last = true;
  if (ctx->count == ctx->capacity) {
    ctx->overflow = true;
    return false;
  }
  ctx->items[ctx->count++] = candle;
  return true;
}
bool VisitPackedRange(const char *key, const void *value, size_t size,
                      void *opaque) {
  auto *ctx = static_cast<CandleCollect *>(opaque);
  int64_t start = 0;
  if (!ParseTimestampSuffix(key, &start)) {
    ctx->invalid = true;
    return false;
  }
  Block block{};
  if (!DecodeBlock(ctx->interval, start, static_cast<const uint8_t *>(value),
                   size, &block)) {
    ctx->invalid = true;
    return false;
  }
  for (size_t i = 0; i < SlotCount(ctx->interval); ++i) {
    const Candle &c = block.slots[i];
    if (!c.present)
      continue;
    if (!AppendRange({c.ts, c.open, c.high, c.low, c.close, c.volume,
                      c.has_volume, c.first, c.last},
                     ctx))
      return false;
  }
  return true;
}
bool VisitOpenRange(const char *key, const void *value, size_t size,
                    void *opaque) {
  auto *ctx = static_cast<CandleCollect *>(opaque);
  Candle c{};
  int64_t key_ts = 0;
  if (!DecodeCandle(static_cast<const uint8_t *>(value), size, &c) ||
      !ParseTimestampSuffix(key, &key_ts) || key_ts != c.ts) {
    ctx->invalid = true;
    return false;
  }
  return AppendRange({c.ts, c.open, c.high, c.low, c.close, c.volume,
                      c.has_volume, c.first, c.last},
                     ctx);
}
bool VisitEdgeBlock(const char *, const void *value, size_t size,
                    void *opaque) {
  auto *ctx = static_cast<EdgeCollect *>(opaque);
  Block block{};
  // The key is not needed to choose the edge candle because the block value
  // contains its segment start.
  const uint8_t *bytes = static_cast<const uint8_t *>(value);
  if (size < kBlockHeader || memcmp(bytes, "FPSB", 4) != 0 || bytes[5] != ctx->interval) {
    ctx->invalid = true;
    return false;
  }
  const int64_t start = static_cast<int64_t>(Get64(bytes + 8));
  if (!DecodeBlock(ctx->interval, start, bytes, size, &block)) {
    ctx->invalid = true;
    return false;
  }
  for (size_t i = 0; i < SlotCount(ctx->interval); ++i) {
    const Candle &c = block.slots[i];
    if (!c.present)
      continue;
    ctx->found = true;
    if (c.ts < ctx->min_ts)
      ctx->min_ts = c.ts;
    if (c.ts > ctx->max_ts)
      ctx->max_ts = c.ts;
  }
  return true;
}
bool VisitEdgeOpen(const char *key, const void *value, size_t size,
                   void *opaque) {
  auto *ctx = static_cast<EdgeCollect *>(opaque);
  Candle candle{};
  int64_t key_ts = 0;
  if (!DecodeCandle(static_cast<const uint8_t *>(value), size, &candle) ||
      !ParseTimestampSuffix(key, &key_ts) || key_ts != candle.ts) {
    ctx->invalid = true;
    return false;
  }
  ctx->found = true;
  if (candle.ts < ctx->min_ts)
    ctx->min_ts = candle.ts;
  if (candle.ts > ctx->max_ts)
    ctx->max_ts = candle.ts;
  return true;
}
void Merge(Candle *c, bool existing, int64_t ts, int64_t at, double price,
           double volume, bool has_volume) {
  if (!existing) {
    *c = Candle{};
    c->present = true;
    c->ts = ts;
    c->first = c->last = at;
    c->open = c->high = c->low = c->close = price;
    c->volume = volume;
    c->has_volume = has_volume;
    return;
  }
  if (at < c->first)
    c->open = price;
  if (price > c->high)
    c->high = price;
  if (price < c->low)
    c->low = price;
  if (at >= c->last) {
    c->last = at;
    c->close = price;
    if (has_volume) {
      c->volume = volume;
      c->has_volume = true;
    }
  }
  if (at < c->first)
    c->first = at;
}
bool ValidInput(const PriceSeriesCandleInput &c, size_t interval, int64_t now) {
  return c.ts >= 0 && c.ts <= now && Bucket(interval, c.ts) == c.ts &&
         isfinite(c.open) && isfinite(c.high) && isfinite(c.low) &&
         isfinite(c.close) && isfinite(c.volume);
}
} // namespace

PriceSeriesStore::PriceSeriesStore(storage::KvStore &store) : store_(store) {}

PriceSeriesStoreStatus
PriceSeriesStore::RecordSpots(const char *fiat, const char *provider,
                              const PriceSpotQuoteInput *quotes, size_t count,
                              int64_t now) {
  return RecordSpotsWithAdditional(fiat, provider, quotes, count, now,
                                   nullptr, 0);
}

PriceSeriesStoreStatus PriceSeriesStore::RecordSpotsWithAdditional(
    const char *fiat, const char *provider,
    const PriceSpotQuoteInput *quotes, size_t count, int64_t now,
    const storage::KvMutation *additional, size_t additional_count) {
  if (!FiatOk(fiat) || !ProviderOk(provider) || now < 0 || (count && !quotes) ||
      count > kPriceSeriesMaxBatchItems ||
      (additional_count && !additional) ||
      additional_count > 2 * kPriceSpotMaxBatchItems)
    return PriceSeriesStoreStatus::kInvalidArguments;
  if (!count && !additional_count)
    return PriceSeriesStoreStatus::kNoItems;
  for (size_t i = 0; i < count; ++i) {
    const auto &q = quotes[i];
    if (!IdOk(q.source_id) || !isfinite(q.price) ||
        (q.has_change24h_pct && !isfinite(q.change24h_pct)) ||
        (q.has_volume24h && !isfinite(q.volume24h)) || q.as_of_ms < 0 ||
        q.as_of_ms > now || now > INT64_MAX - 367 * kDay)
      return PriceSeriesStoreStatus::kInvalidArguments;
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(q.source_id, quotes[j].source_id))
        return PriceSeriesStoreStatus::kInvalidArguments;
  }
  // Coordinated writes acquire cache batch mutex, then this series mutex,
  // then KvStore's mutex for the single commit. No series path acquires the
  // cache mutex, so the global order is stable.
  fortis::base::LockGuard guard(g_series_mutex);
  if (!guard.locked())
    return PriceSeriesStoreStatus::kWriteFailure;
  auto w_storage = base::AllocateZeroedStorage<Workspace>(1, sizeof(Workspace));
  auto* w = w_storage.get();
  if (!w)
    return PriceSeriesStoreStatus::kAllocationFailure;
  size_t n = 0;
  PriceSeriesStoreStatus result = PriceSeriesStoreStatus::kStored;
  for (size_t q = 0; q < count && result == PriceSeriesStoreStatus::kStored;
       ++q) {
    char sk[kKeyCapacity];
    Spot s{};
    PriceSeriesReadStatus rs{};
    bool had = false;
    if (!SpotKey(fiat, quotes[q].source_id, sk)) {
      result = PriceSeriesStoreStatus::kInvalidArguments;
      break;
    }
    had = ReadSpot(store_, sk, now, &s, &rs);
    if (!had && rs != PriceSeriesReadStatus::kMissing) {
      result = PriceSeriesStoreStatus::kReadFailure;
      break;
    }
    if (had && s.as_of > quotes[q].as_of_ms)
      continue;
    const bool hv = quotes[q].has_volume24h || (had && (s.flags & kHasVolume));
    const double vol = quotes[q].has_volume24h ? quotes[q].volume24h
                       : had                   ? s.volume
                                               : 0;
    s.as_of = quotes[q].as_of_ms;
    s.expires = now + 7 * kDay;
    s.price = quotes[q].price;
    s.change = quotes[q].has_change24h_pct ? quotes[q].change24h_pct : 0;
    s.volume = hv ? vol : 0;
    s.flags =
        (quotes[q].has_change24h_pct ? kHasChange : 0) | (hv ? kHasVolume : 0);
    std::snprintf(s.provider, sizeof(s.provider), "%s", provider);
    size_t ix = n++;
    std::snprintf(w->keys[ix], sizeof(w->keys[ix]), "%s", sk);
    EncodeSpot(s, w->values[ix]);
    w->mutations[ix] = {w->keys[ix], w->values[ix], kPriceSeriesEncodedBytes,
                        s.expires, false};
    for (size_t r = 0; r < 3; ++r) {
      int64_t ts = Bucket(r, quotes[q].as_of_ms);
      char ok[kKeyCapacity], bk[kKeyCapacity], mk[kKeyCapacity];
      if (!OpenKey(r, fiat, quotes[q].source_id, ts, ok) ||
          !BlockKey(r, fiat, quotes[q].source_id, SegmentStart(r, ts), bk) ||
          !SealKey(r, fiat, quotes[q].source_id, SegmentStart(r, ts), mk)) {
        result = PriceSeriesStoreStatus::kInvalidArguments;
        break;
      }
      Candle c{};
      PriceSeriesReadStatus cs{};
      bool found = ReadCandle(store_, ok, now, &c, &cs);
      if (!found && cs != PriceSeriesReadStatus::kMissing) {
        result = PriceSeriesStoreStatus::kReadFailure;
        break;
      }
      if (!found) {
        Block b{};
        PriceSeriesReadStatus bs{};
        bool bh = ReadBlock(store_, bk, r, SegmentStart(r, ts), now, &b, &bs);
        if (!bh && bs != PriceSeriesReadStatus::kMissing) {
          result = PriceSeriesStoreStatus::kReadFailure;
          break;
        }
        if (bh)
          c = b.slots[SlotOf(r, ts, b.start)];
        found = c.present;
      }
      Merge(&c, found, ts, quotes[q].as_of_ms, quotes[q].price, vol, hv);
      ix = n++;
      std::snprintf(w->keys[ix], sizeof(w->keys[ix]), "%s", ok);
      EncodeCandle(c, w->values[ix]);
      w->mutations[ix] = {w->keys[ix], w->values[ix], kOpenBytes,
                          CandleExpiry(r, ts, now), false};
      Marker m{};
      PriceSeriesReadStatus ms{};
      bool mh = ReadMarker(store_, mk, now, &m, &ms);
      if (!mh && ms != PriceSeriesReadStatus::kMissing) {
        result = PriceSeriesStoreStatus::kReadFailure;
        break;
      }
      const int64_t expiry = CandleExpiry(r, ts, now);
      const size_t slot = SlotOf(r, ts, SegmentStart(r, ts));
      if (!mh || m.next_slot > slot || m.expires < expiry) {
        m.interval = r;
        m.start = SegmentStart(r, ts);
        if (expiry > m.expires)
          m.expires = expiry;
        if (!mh || m.next_slot > slot)
          m.next_slot = slot;
        ix = n++;
        std::snprintf(w->keys[ix], sizeof(w->keys[ix]), "%s", mk);
        EncodeMarker(m, w->marker_values[ix]);
        w->mutations[ix] = {w->keys[ix], w->marker_values[ix], kMarkerBytes,
                            m.expires, false};
      }
    }
  }
  if (result == PriceSeriesStoreStatus::kStored) {
    if (!n && !additional_count)
      result = PriceSeriesStoreStatus::kNoItems;
    else if (!additional_count) {
      if (!store_.Write(w->mutations, n))
        result = PriceSeriesStoreStatus::kWriteFailure;
    } else {
      storage::KvMutation combined[kCombinedProviderMaxMutations]{};
      if (n) memcpy(combined, w->mutations, n * sizeof(storage::KvMutation));
      memcpy(combined + n, additional,
             additional_count * sizeof(storage::KvMutation));
      if (!store_.Write(combined, n + additional_count))
        result = PriceSeriesStoreStatus::kWriteFailure;
    }
  }
  w_storage.reset();
  return result;
}

PriceSeriesReadStatus PriceSeriesStore::GetSpot(const char *fiat,
                                                const char *id, int64_t now,
                                                PriceSeriesSpot *out) const {
  char key[kKeyCapacity];
  if (!out || now < 0 || !SpotKey(fiat, id, key))
    return PriceSeriesReadStatus::kInvalidKey;
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked())
    return PriceSeriesReadStatus::kIoError;
  Spot s{};
  PriceSeriesReadStatus st{};
  if (!ReadSpot(store_, key, now, &s, &st))
    return st;
  if (now >= s.expires)
    return PriceSeriesReadStatus::kMissing;
  std::snprintf(out->source, sizeof(out->source), "%s", s.provider);
  out->price = s.price;
  out->change24h_pct = s.change;
  out->volume24h = s.volume;
  out->as_of_ms = s.as_of;
  out->has_change24h_pct = (s.flags & kHasChange) != 0;
  out->has_volume24h = (s.flags & kHasVolume) != 0;
  return PriceSeriesReadStatus::kFound;
}

PriceSeriesStoreStatus PriceSeriesStore::UpsertCandles(
    PriceSeriesInterval interval, const char *fiat, const char *id,
    const PriceSeriesCandleInput *inputs, size_t count, int64_t now) {
  const size_t r = static_cast<size_t>(interval);
  if (r > 2 || !FiatOk(fiat) || !IdOk(id) || now < 0 || (count && !inputs) ||
      count > kPriceSeriesMaxUpsertItems || now > INT64_MAX - 367 * kDay)
    return PriceSeriesStoreStatus::kInvalidArguments;
  if (!count) return PriceSeriesStoreStatus::kNoItems;
  PriceSeriesCandleBatchInput batch[kPriceSeriesMaxUpsertItems]{};
  for (size_t i = 0; i < count; ++i) batch[i] = {interval, inputs[i]};
  return UpsertCandlesAtomic(fiat, id, batch, count, now);
}

PriceSeriesStoreStatus PriceSeriesStore::UpsertCandlesAtomic(
    const char *fiat, const char *id,
    const PriceSeriesCandleBatchInput *inputs, size_t count, int64_t now) {
  if (!FiatOk(fiat) || !IdOk(id) || now < 0 || (count && !inputs) ||
      count > kPriceSeriesMaxUpsertItems || now > INT64_MAX - 367 * kDay)
    return PriceSeriesStoreStatus::kInvalidArguments;
  if (!count) return PriceSeriesStoreStatus::kNoItems;
  for (size_t i = 0; i < count; ++i) {
    const size_t r = static_cast<size_t>(inputs[i].interval);
    if (r > 2 || !ValidInput(inputs[i].candle, r, now))
      return PriceSeriesStoreStatus::kInvalidArguments;
  }
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked())
    return PriceSeriesStoreStatus::kWriteFailure;
  auto w_storage = base::AllocateZeroedStorage<UpsertWorkspace>(1, sizeof(UpsertWorkspace));
  auto* w = w_storage.get();
  if (!w)
    return PriceSeriesStoreStatus::kAllocationFailure;
  PriceSeriesStoreStatus result = PriceSeriesStoreStatus::kStored;
  for (size_t i = 0; i < count && result == PriceSeriesStoreStatus::kStored;
       ++i) {
    const size_t r = static_cast<size_t>(inputs[i].interval);
    const auto &in = inputs[i].candle;
    char ok[kKeyCapacity];
    if (!OpenKey(r, fiat, id, in.ts, ok)) {
      result = PriceSeriesStoreStatus::kInvalidArguments;
      break;
    }
    Candle c{};
    PriceSeriesReadStatus st{};
    OpenPending *pending = nullptr;
    for (size_t j = 0; j < w->open_count; ++j)
      if (!strcmp(w->opens[j].key, ok)) {
        pending = &w->opens[j];
        break;
      }
    bool has = pending != nullptr;
    if (has)
      c = pending->candle;
    else {
      has = ReadCandle(store_, ok, now, &c, &st);
      if (!has && st != PriceSeriesReadStatus::kMissing) {
        result = PriceSeriesStoreStatus::kReadFailure;
        break;
      }
    }
    if (has) {
      c = Candle{};
      c.present = true;
      c.ts = in.ts;
      c.first = c.last = in.ts;
      c.open = in.open;
      c.high = in.high;
      c.low = in.low;
      c.close = in.close;
      c.volume = in.volume;
      c.has_volume = in.has_volume;
      OpenPending *p = pending;
      if (!p) {
        p = &w->opens[w->open_count++];
        std::snprintf(p->key, sizeof(p->key), "%s", ok);
      }
      p->candle = c;
      p->expiry = CandleExpiry(r, in.ts, now);
      EncodeCandle(c, p->encoded);
    } else {
      const int64_t start = SegmentStart(r, in.ts);
      char bk[kKeyCapacity];
      if (!BlockKey(r, fiat, id, start, bk)) {
        result = PriceSeriesStoreStatus::kInvalidArguments;
        break;
      }
      BlockPending *p = nullptr;
      for (size_t j = 0; j < w->block_count; ++j)
        if (!strcmp(w->blocks[j].key, bk)) {
          p = &w->blocks[j];
          break;
        }
      if (!p) {
        p = &w->blocks[w->block_count++];
        std::snprintf(p->key, sizeof(p->key), "%s", bk);
        p->block.interval = r;
        p->block.start = start;
        PriceSeriesReadStatus bs{};
        bool bh = ReadBlock(store_, bk, r, start, now, &p->block, &bs);
        if (!bh && bs != PriceSeriesReadStatus::kMissing) {
          result = PriceSeriesStoreStatus::kReadFailure;
          break;
        }
      }
      Candle prev = p->block.slots[SlotOf(r, in.ts, start)];
      c = Candle{};
      c.present = true;
      c.ts = in.ts;
      c.first = c.last = in.ts;
      c.open = in.open;
      c.high = in.high;
      c.low = in.low;
      c.close = in.close;
      c.volume = in.has_volume ? in.volume : prev.present ? prev.volume : 0;
      c.has_volume = in.has_volume || (prev.present && prev.has_volume);
      p->block.slots[SlotOf(r, in.ts, start)] = c;
      const int64_t expiry = CandleExpiry(r, in.ts, now);
      if (expiry > p->expiry)
        p->expiry = expiry;
    }
    if (has) {
      const int64_t start = SegmentStart(r, in.ts);
      char mk[kKeyCapacity];
      if (!SealKey(r, fiat, id, start, mk)) {
        result = PriceSeriesStoreStatus::kInvalidArguments;
        break;
      }
      size_t mi = 0;
      for (; mi < w->marker_count; ++mi)
        if (!strcmp(w->marker_keys[mi], mk))
          break;
      Marker m{};
      PriceSeriesReadStatus ms{};
      bool mh = false;
      if (mi < w->marker_count)
        mh = DecodeMarker(w->marker_values[mi], kMarkerBytes, &m);
      else {
        mh = ReadMarker(store_, mk, now, &m, &ms);
        if (!mh && ms != PriceSeriesReadStatus::kMissing) {
          result = PriceSeriesStoreStatus::kReadFailure;
          break;
        }
      }
      const size_t slot = SlotOf(r, in.ts, start);
      const int64_t expiry = CandleExpiry(r, in.ts, now);
      if (!mh || m.next_slot > slot || m.expires < expiry) {
        if (mi == w->marker_count) {
          std::snprintf(w->marker_keys[mi], sizeof(w->marker_keys[mi]), "%s", mk);
          ++w->marker_count;
        }
        m.interval = r;
        m.start = start;
        if (expiry > m.expires)
          m.expires = expiry;
        if (!mh || m.next_slot > slot)
          m.next_slot = static_cast<uint16_t>(slot);
        EncodeMarker(m, w->marker_values[mi]);
      }
    }
  }
  for (size_t i = 0;
       i < w->open_count && result == PriceSeriesStoreStatus::kStored; ++i) {
    OpenPending &p = w->opens[i];
    size_t n = w->mutation_count++;
    w->mutations[n] = {p.key, p.encoded, kOpenBytes, p.expiry, false};
  }
  for (size_t i = 0;
       i < w->block_count && result == PriceSeriesStoreStatus::kStored; ++i) {
    BlockPending &p = w->blocks[i];
    p.encoded = w->block_values + w->block_value_offset;
    const size_t r = p.block.interval;
    p.encoded_size = BlockBytes(r);
    w->block_value_offset += p.encoded_size;
    EncodeBlock(p.block, p.encoded);
    size_t n = w->mutation_count++;
    w->mutations[n] = {p.key, p.encoded, static_cast<uint32_t>(p.encoded_size),
                       p.expiry, false};
  }
  for (size_t i = 0;
       i < w->marker_count && result == PriceSeriesStoreStatus::kStored; ++i) {
    Marker m{};
    DecodeMarker(w->marker_values[i], kMarkerBytes, &m);
    size_t n = w->mutation_count++;
    w->mutations[n] = {w->marker_keys[i], w->marker_values[i], kMarkerBytes,
                       m.expires, false};
  }
  if (result == PriceSeriesStoreStatus::kStored) {
    if (!w->mutation_count)
      result = PriceSeriesStoreStatus::kNoItems;
    else if (!store_.Write(w->mutations, w->mutation_count))
      result = PriceSeriesStoreStatus::kWriteFailure;
  }
  w_storage.reset();
  return result;
}

namespace {
// Caller holds g_series_mutex. Expired records read as missing even if the
// KV sweep has not removed them yet.
PriceSeriesReadStatus ReadCoverageLocked(const storage::KvStore &store, const char *key,
                                         size_t key_length, uint8_t interval,
                                         int64_t now, HistoryCoverage *out) {
  uint8_t encoded[kHistoryCoverageMaxBytes]{};
  size_t size = 0;
  const storage::KvReadStatus read =
      store.GetStatus(key, now, encoded, sizeof(encoded), &size);
  if (read == storage::KvReadStatus::Missing) return PriceSeriesReadStatus::kMissing;
  if (read == storage::KvReadStatus::IoError) return PriceSeriesReadStatus::kIoError;
  // An oversized value cannot be a record this code wrote.
  if (read != storage::KvReadStatus::Found) return PriceSeriesReadStatus::kCorrupt;
  HistoryCoverage decoded{};
  if (!DecodeHistoryCoverage(encoded, size, interval,
                             HistoryCoverageCrc32(key, key_length), &decoded))
    return PriceSeriesReadStatus::kCorrupt;
  if (decoded.expires_ms <= now) return PriceSeriesReadStatus::kMissing;
  *out = decoded;
  return PriceSeriesReadStatus::kFound;
}
}  // namespace

PriceSeriesReadStatus PriceSeriesStore::ReadHistoryCoverage(
    PriceSeriesInterval interval, const char *fiat, const char *id,
    int64_t now, HistoryCoverage *output) const {
  char key[kKeyCapacity];
  size_t key_length = 0;
  if (!output || now < 0 ||
      !HistoryCoverageKey(interval, fiat, id, key, &key_length))
    return PriceSeriesReadStatus::kInvalidKey;
  *output = HistoryCoverage{};
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked()) return PriceSeriesReadStatus::kIoError;
  return ReadCoverageLocked(store_, key, key_length,
                            static_cast<uint8_t>(interval), now, output);
}

PriceSeriesStoreStatus PriceSeriesStore::MarkHistoryCoverageRange(
    PriceSeriesInterval interval, const char *fiat, const char *id,
    int64_t from, int64_t to, int64_t now) {
  char key[kKeyCapacity];
  size_t key_length = 0;
  if (!HistoryCoverageKey(interval, fiat, id, key, &key_length) || now < 0 ||
      now > INT64_MAX - kHistoryCoverageTtlMs || from < 0 || to <= from ||
      to > INT64_MAX / 1000)
    return PriceSeriesStoreStatus::kInvalidArguments;
  const uint8_t code = static_cast<uint8_t>(interval);
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked()) return PriceSeriesStoreStatus::kWriteFailure;
  HistoryCoverage coverage{};
  const PriceSeriesReadStatus read =
      ReadCoverageLocked(store_, key, key_length, code, now, &coverage);
  // Without the current record a rewrite could only drop coverage; skip it.
  if (read == PriceSeriesReadStatus::kIoError)
    return PriceSeriesStoreStatus::kReadFailure;
  if (read != PriceSeriesReadStatus::kFound) coverage = HistoryCoverage{};
  coverage.interval = code;
  if (!InsertHistoryCoverage(&coverage, from, to))
    return PriceSeriesStoreStatus::kInvalidArguments;
  // markCoveredRange rewrites the whole list with a fresh TTL.
  coverage.expires_ms = now + kHistoryCoverageTtlMs;
  uint8_t encoded[kHistoryCoverageMaxBytes]{};
  const size_t size = EncodeHistoryCoverage(
      coverage, HistoryCoverageCrc32(key, key_length), encoded, sizeof(encoded));
  if (!size) return PriceSeriesStoreStatus::kInvalidArguments;
  storage::KvMutation mutation{key, encoded, static_cast<uint32_t>(size),
                      coverage.expires_ms, false};
  return store_.Write(&mutation, 1) ? PriceSeriesStoreStatus::kStored
                                    : PriceSeriesStoreStatus::kWriteFailure;
}

PriceSeriesReadStatus PriceSeriesStore::Range(PriceSeriesInterval interval,
                                              const char *fiat, const char *id,
                                              int64_t from, int64_t to,
                                              int64_t now,
                                              PriceSeriesCandle *out,
                                              size_t cap, size_t *count) const {
  if (count)
    *count = 0;
  size_t r = static_cast<size_t>(interval);
  // The two scratch arrays below are heap-allocated per call (at most
  // 2 x 3,700 x sizeof(PriceSeriesCandle), about 520 KiB, for 1d) and the
  // series mutex serializes callers.
  if (!count || !out || !cap || r > 2 || cap > PriceSeriesMaxRangeCandles(interval) ||
      now < 0 || from < 0 || from > to || !FiatOk(fiat) || !IdOk(id))
    return PriceSeriesReadStatus::kInvalidKey;
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked())
    return PriceSeriesReadStatus::kIoError;
  auto scratch_storage = base::AllocateZeroedStorage<PriceSeriesCandle[]>(cap, sizeof(PriceSeriesCandle));
  auto* scratch = scratch_storage.get();
  auto open_values_storage = base::AllocateZeroedStorage<PriceSeriesCandle[]>(cap, sizeof(PriceSeriesCandle));
  auto* open_values = open_values_storage.get();
  if (!scratch || !open_values) {
    scratch_storage.reset();
    open_values_storage.reset();
    return PriceSeriesReadStatus::kIoError;
  }
  CandleCollect packed{scratch, cap, 0, r, from, to};
  char pref[kKeyCapacity], after[kKeyCapacity];
  BlockPrefix(r, fiat, id, pref);
  int64_t block_start = SegmentStart(r, from);
  if (block_start) {
    if (!AppendTimestamp(pref, block_start - 1, after, sizeof(after))) {
      scratch_storage.reset();
      open_values_storage.reset();
      return PriceSeriesReadStatus::kInvalidKey;
    }
  } else {
    std::snprintf(after, sizeof(after), "%s", pref);
  }
  bool ok = store_.Scan(pref, after, SegmentCount(r, from, to) + 2, now, [&, visitor_state = &packed](const char* key, const void* value, size_t size) { return (VisitPackedRange)(key, value, size, visitor_state); });
  CandleCollect opens{open_values, cap, 0, r, from, to};
  OpenPrefix(r, fiat, id, pref);
  int64_t bucket_start = Bucket(r, from);
  if (bucket_start) {
    if (!AppendTimestamp(pref, bucket_start - 1, after, sizeof(after))) {
      scratch_storage.reset();
      open_values_storage.reset();
      return PriceSeriesReadStatus::kInvalidKey;
    }
  } else {
    std::snprintf(after, sizeof(after), "%s", pref);
  }
  ok = ok && store_.Scan(pref, after, BucketSpan(r, from, to) + 2, now, [&, visitor_state = &opens](const char* key, const void* value, size_t size) { return (VisitOpenRange)(key, value, size, visitor_state); });
  if (packed.invalid || opens.invalid) {
    scratch_storage.reset();
    open_values_storage.reset();
    return PriceSeriesReadStatus::kCorrupt;
  }
  if (packed.overflow || opens.overflow) {
    scratch_storage.reset();
    open_values_storage.reset();
    return PriceSeriesReadStatus::kCapacityExceeded;
  }
  if (!ok) {
    scratch_storage.reset();
    open_values_storage.reset();
    return PriceSeriesReadStatus::kIoError;
  }
  // Merge both sorted streams; coalesce repeated timestamps and let open win.
  size_t a = 0, b = 0, n = 0;
  while (a < packed.count || b < opens.count) {
    const int64_t ts = b == opens.count                    ? scratch[a].ts
                       : a == packed.count                 ? open_values[b].ts
                       : scratch[a].ts < open_values[b].ts ? scratch[a].ts
                                                           : open_values[b].ts;
    PriceSeriesCandle c{};
    bool have = false;
    while (a < packed.count && scratch[a].ts == ts) {
      c = scratch[a++];
      have = true;
    }
    while (b < opens.count && open_values[b].ts == ts) {
      c = open_values[b++];
      have = true;
    }
    if (have) {
      if (n == cap) {
        scratch_storage.reset();
        open_values_storage.reset();
        return PriceSeriesReadStatus::kCapacityExceeded;
      }
      out[n++] = c;
    }
  }
  *count = n;
  scratch_storage.reset();
  open_values_storage.reset();
  return n ? PriceSeriesReadStatus::kFound : PriceSeriesReadStatus::kMissing;
}

PriceSeriesStoreStatus PriceSeriesStore::SealDue(int64_t now, size_t limit,
                                                 size_t *sealed) {
  if (sealed)
    *sealed = 0;
  if (!sealed || now < 0 || limit > kSealMaxPerCall)
    return PriceSeriesStoreStatus::kInvalidArguments;
  if (!limit)
    return PriceSeriesStoreStatus::kNoItems;
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked())
    return PriceSeriesStoreStatus::kWriteFailure;
  auto w_storage = base::AllocateZeroedStorage<SealWorkspace>(1, sizeof(SealWorkspace));
  auto* w = w_storage.get();
  if (!w)
    return PriceSeriesStoreStatus::kAllocationFailure;
  MarkerCollect mc{w->segments, kSealFrameMaxSegments, 0, now};
  const bool marker_scan = store_.Scan(kSealPrefix, kSealPrefix, kSealFrameMaxSegments, now, [&, visitor_state = &mc](const char* key, const void* value, size_t size) { return (VisitMarker)(key, value, size, visitor_state); });
  if ((!marker_scan && !mc.future && !mc.more) || mc.invalid) {
    w_storage.reset();
    return PriceSeriesStoreStatus::kReadFailure;
  }
  size_t total = 0, muts = 0, segs = 0;
  const size_t row_limit =
      limit < kSealFrameMaxRows ? limit : kSealFrameMaxRows;
  for (size_t si = 0;
       si < mc.count && total < row_limit && segs < kSealFrameMaxSegments;
       ++si) {
    SealDesc &d = w->segments[si];
    char op[kKeyCapacity], after[kKeyCapacity];
    OpenPrefix(d.interval, d.fiat, d.source_id, op);
    if (d.start) {
      if (!AppendTimestamp(op, d.start - 1, after, sizeof(after))) {
        w_storage.reset();
        return PriceSeriesStoreStatus::kReadFailure;
      }
    } else {
      std::snprintf(after, sizeof(after), "%s", op);
    }
    const size_t rem = row_limit - total,
                 cap = rem < kBlockMaxSlots ? rem : kBlockMaxSlots;
    OpenCollect oc{w->open_rows, cap, 0, d.interval, d.start};
    const bool open_scan =
        store_.Scan(op, after, cap + 1, now, [&, visitor_state = &oc](const char* key, const void* value, size_t size) { return (VisitOpenForSeal)(key, value, size, visitor_state); });
    if (oc.invalid || (!open_scan && !oc.has_more && !oc.reached_end)) {
      w_storage.reset();
      return PriceSeriesStoreStatus::kReadFailure;
    }
    if (!oc.count) {
      size_t m = muts++;
      std::snprintf(w->keys[m], sizeof(w->keys[m]), "%s", d.key);
      w->mutations[m] = {w->keys[m], nullptr, 0, 0, true};
      ++segs;
      continue;
    }
    char bk[kKeyCapacity];
    BlockKey(d.interval, d.fiat, d.source_id, d.start, bk);
    Block block{};
    PriceSeriesReadStatus bs{};
    bool bh = ReadBlock(store_, bk, d.interval, d.start, now, &block, &bs);
    if (!bh && bs != PriceSeriesReadStatus::kMissing) {
      w_storage.reset();
      return PriceSeriesStoreStatus::kReadFailure;
    }
    if (!bh) {
      block.interval = d.interval;
      block.start = d.start;
    }
    for (size_t j = 0; j < oc.count; ++j) {
      Candle c = oc.items[j];
      Candle &old = block.slots[SlotOf(d.interval, c.ts, d.start)];
      if (!c.has_volume && old.present && old.has_volume) {
        c.volume = old.volume;
        c.has_volume = true;
      }
      c.present = true;
      old = c;
      char key[kKeyCapacity];
      OpenKey(d.interval, d.fiat, d.source_id, c.ts, key);
      size_t m = muts++;
      std::snprintf(w->keys[m], sizeof(w->keys[m]), "%s", key);
      w->mutations[m] = {w->keys[m], nullptr, 0, 0, true};
    }
    uint8_t *bv = w->block_values[segs];
    EncodeBlock(block, bv);
    size_t m = muts++;
    std::snprintf(w->keys[m], sizeof(w->keys[m]), "%s", bk);
    w->mutations[m] = {w->keys[m], bv,
                       static_cast<uint32_t>(BlockBytes(d.interval)),
                       CandleExpiry(d.interval, d.start, now), false};
    if (oc.has_more) {
      Marker mm = d.marker;
      mm.next_slot =
          static_cast<uint16_t>(SlotOf(d.interval, oc.next_ts, d.start));
      EncodeMarker(mm, w->marker_values[segs]);
      m = muts++;
      std::snprintf(w->keys[m], sizeof(w->keys[m]), "%s", d.key);
      w->mutations[m] = {w->keys[m], w->marker_values[segs], kMarkerBytes,
                         mm.expires, false};
    } else {
      m = muts++;
      std::snprintf(w->keys[m], sizeof(w->keys[m]), "%s", d.key);
      w->mutations[m] = {w->keys[m], nullptr, 0, 0, true};
    }
    total += oc.count;
    ++segs;
  }
  PriceSeriesStoreStatus st = PriceSeriesStoreStatus::kNoItems;
  if (muts) {
    st = store_.Write(w->mutations, muts)
             ? PriceSeriesStoreStatus::kStored
             : PriceSeriesStoreStatus::kWriteFailure;
    if (st == PriceSeriesStoreStatus::kStored)
      *sealed = total;
  }
  w_storage.reset();
  return st;
}

namespace {
// Next byte of encodeURIComponent(text) (ASCII source IDs and fiats).
struct EncodedCursor {
  const char *text;
  char pending[3];
  size_t pending_used = 0, pending_size = 0;
  int Next() {
    if (pending_used < pending_size)
      return static_cast<unsigned char>(pending[pending_used++]);
    const unsigned char c = static_cast<unsigned char>(*text);
    if (!c)
      return -1;
    ++text;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || strchr("-_.!~*'()", c))
      return c;
    static const char kHex[] = "0123456789ABCDEF";
    pending[0] = kHex[c >> 4];
    pending[1] = kHex[c & 15];
    pending_used = 0;
    pending_size = 2;
    return '%';
  }
};
// enc(fiat) + '/' + enc(id) byte order.
int CompareRecentKey(const PriceSeriesRecentSpot &a,
                     const PriceSeriesRecentSpot &b) {
  EncodedCursor parts[2][2] = {{{a.fiat, {}}, {a.source_id, {}}},
                               {{b.fiat, {}}, {b.source_id, {}}}};
  size_t part[2] = {0, 0};
  for (;;) {
    int next[2];
    for (size_t side = 0; side < 2; ++side) {
      next[side] = part[side] < 2 ? parts[side][part[side]].Next() : -1;
      if (next[side] < 0 && part[side] == 0) {
        part[side] = 1;
        next[side] = '/';
      } else if (next[side] < 0) {
        part[side] = 2;
      }
    }
    if (next[0] != next[1])
      return next[0] < next[1] ? -1 : 1;
    if (next[0] < 0)
      return 0;
  }
}
bool RecentBefore(const PriceSeriesRecentSpot &a,
                  const PriceSeriesRecentSpot &b) {
  if (a.as_of_ms != b.as_of_ms)
    return a.as_of_ms < b.as_of_ms;
  return CompareRecentKey(a, b) < 0;
}
struct RecentCollect {
  PriceSeriesRecentSpot *slots;
  uint16_t *order;  // slot indexes, oldest first
  size_t capacity, count;
  int64_t since;
  size_t visited = 0, corrupt = 0;
  char last[kKeyCapacity]{};
  PriceSeriesRecentSpot candidate{};
};
bool VisitRecentSpot(const char *key, const void *value, size_t size,
                     void *opaque) {
  auto *ctx = static_cast<RecentCollect *>(opaque);
  ++ctx->visited;
  snprintf(ctx->last, sizeof(ctx->last), "%s", key);
  const char *rest = key + sizeof(kSpotPrefix) - 1;
  const char *slash = strchr(rest, '/');
  Spot spot{};
  PriceSeriesRecentSpot &c = ctx->candidate;
  if (!slash || slash - rest < 3 || slash - rest > 5 ||
      size != kPriceSeriesEncodedBytes ||
      !DecodeSpot(static_cast<const uint8_t *>(value), &spot)) {
    ++ctx->corrupt;
    return true;
  }
  memcpy(c.fiat, rest, static_cast<size_t>(slash - rest));
  c.fiat[slash - rest] = '\0';
  snprintf(c.source_id, sizeof(c.source_id), "%s", slash + 1);
  if (!FiatOk(c.fiat) || !IdOk(c.source_id)) {
    ++ctx->corrupt;
    return true;
  }
  c.as_of_ms = spot.as_of;
  c.price = spot.price;
  if (c.as_of_ms < ctx->since)
    return true;
  if (ctx->count == ctx->capacity &&
      !RecentBefore(c, ctx->slots[ctx->order[ctx->count - 1]]))
    return true;
  // Binary search for the first entry the candidate precedes.
  size_t low = 0, high = ctx->count;
  while (low < high) {
    const size_t mid = (low + high) / 2;
    if (RecentBefore(c, ctx->slots[ctx->order[mid]]))
      high = mid;
    else
      low = mid + 1;
  }
  uint16_t slot;
  if (ctx->count == ctx->capacity) {
    slot = ctx->order[ctx->count - 1];
    --ctx->count;
  } else {
    slot = static_cast<uint16_t>(ctx->count);
  }
  memmove(ctx->order + low + 1, ctx->order + low,
          (ctx->count - low) * sizeof(uint16_t));
  ctx->order[low] = slot;
  ctx->slots[slot] = c;
  ++ctx->count;
  return true;
}
}  // namespace

PriceSeriesReadStatus PriceSeriesStore::RecentSpots(
    int64_t since, int64_t now, PriceSeriesRecentSpot *output,
    size_t capacity, size_t *count, size_t *scanned, size_t *corrupt) const {
  if (count)
    *count = 0;
  if (scanned)
    *scanned = 0;
  if (corrupt)
    *corrupt = 0;
  if (!count || (!output && capacity) || capacity > kPriceSeriesMaxRecentSpots ||
      now < 0)
    return PriceSeriesReadStatus::kInvalidKey;
  if (!capacity)
    return PriceSeriesReadStatus::kMissing;
  constexpr size_t kPage = 256;
  auto slots_storage = base::AllocateZeroedStorage<PriceSeriesRecentSpot[]>(capacity, sizeof(PriceSeriesRecentSpot));
  auto* slots = slots_storage.get();
  auto order_storage = base::AllocateZeroedStorage<uint16_t[]>(capacity, sizeof(uint16_t));
  auto* order = order_storage.get();
  auto ctx_storage = base::AllocateZeroedStorage<RecentCollect>(1, sizeof(RecentCollect));
  auto* ctx = ctx_storage.get();
  if (!slots || !order || !ctx) {
    slots_storage.reset();
    order_storage.reset();
    ctx_storage.reset();
    return PriceSeriesReadStatus::kIoError;
  }
  ctx->slots = slots;
  ctx->order = order;
  ctx->capacity = capacity;
  ctx->since = since;
  char after[kKeyCapacity];
  std::snprintf(after, sizeof(after), "%s", kSpotPrefix);
  PriceSeriesReadStatus status = PriceSeriesReadStatus::kFound;
  for (;;) {
    const size_t before = ctx->visited;
    bool ok;
    {
      fortis::base::LockGuard g(g_series_mutex);
      ok = g.locked() &&
           store_.Scan(kSpotPrefix, after, kPage, now, [&, visitor_state = ctx](const char* key, const void* value, size_t size) { return (VisitRecentSpot)(key, value, size, visitor_state); });
    }
    if (!ok) {
      status = PriceSeriesReadStatus::kIoError;
      break;
    }
    if (ctx->visited - before < kPage)
      break;
    std::snprintf(after, sizeof(after), "%s", ctx->last);
  }
  if (status == PriceSeriesReadStatus::kFound) {
    for (size_t i = 0; i < ctx->count; ++i)
      output[i] = slots[order[i]];
    *count = ctx->count;
    if (!ctx->count)
      status = PriceSeriesReadStatus::kMissing;
  }
  if (scanned)
    *scanned = ctx->visited;
  if (corrupt)
    *corrupt = ctx->corrupt;
  slots_storage.reset();
  order_storage.reset();
  ctx_storage.reset();
  return status;
}

PriceSeriesReadStatus
PriceSeriesStore::Availability(PriceSeriesInterval interval, const char *fiat,
                               const char *id, int64_t now,
                               PriceSeriesAvailability *out) const {
  const size_t r = static_cast<size_t>(interval);
  if (!out || r > 2 || now < 0 || !FiatOk(fiat) || !IdOk(id))
    return PriceSeriesReadStatus::kInvalidKey;
  fortis::base::LockGuard g(g_series_mutex);
  if (!g.locked())
    return PriceSeriesReadStatus::kIoError;
  EdgeCollect edge{};
  edge.interval = r;
  char p[kKeyCapacity];
  BlockPrefix(r, fiat, id, p);
  bool ok = store_.Scan(p, p, 1, now, [&, visitor_state = &edge](const char* key, const void* value, size_t size) { return (VisitEdgeBlock)(key, value, size, visitor_state); }) &&
            store_.Scan(p, nullptr, 1, now, [&, visitor_state = &edge](const char* key, const void* value, size_t size) { return (VisitEdgeBlock)(key, value, size, visitor_state); }, true);
  OpenPrefix(r, fiat, id, p);
  ok = ok && store_.Scan(p, p, 1, now, [&, visitor_state = &edge](const char* key, const void* value, size_t size) { return (VisitEdgeOpen)(key, value, size, visitor_state); }) &&
       store_.Scan(p, nullptr, 1, now, [&, visitor_state = &edge](const char* key, const void* value, size_t size) { return (VisitEdgeOpen)(key, value, size, visitor_state); }, true);
  if (!ok)
    return edge.invalid ? PriceSeriesReadStatus::kCorrupt
                        : PriceSeriesReadStatus::kIoError;
  if (!edge.found)
    return PriceSeriesReadStatus::kMissing;
  out->oldest = edge.min_ts;
  out->newest = edge.max_ts;
  return PriceSeriesReadStatus::kFound;
}
} // namespace fortis::prices
