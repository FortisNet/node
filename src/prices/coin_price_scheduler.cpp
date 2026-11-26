#include <memory>
#include "coin_price_scheduler.h"
#include "log.h"
#include "ownership.h"

#include "bounded_json.h"
#include "device_auth.h"
#include "discovery_cache.h"
#include "js_case.h"
#include "js_json.h"
#include "kv_store.h"
#include "market_view.h"
#include "media_registry.h"
#include "price_history_batch_codec.h"
#include "price_series_store.h"
#include "price_spot_cache.h"
#include "price_core_ids.h"
#include "js_date_parse.h"
#include "token_provider.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices::coin_prices {

namespace {

using codec::history_batch::OutBuffer;

constexpr char kPathPrefix[] = "/api/v3/coins/markets?vs_currency=";
constexpr char kPathSuffix[] = "&price_change_percentage=1h,24h,7d,14d,30d,200d,1y&sparkline=true";
constexpr size_t kRowTokens = 8192;
constexpr size_t kEntryBytes = 64 * 1024;
constexpr size_t kCacheScratchBytes = kEntryBytes + 256;
constexpr size_t kMaxBatch = kTypeScriptMarketBatchIds;

uint64_t monotonic_us() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + static_cast<uint64_t>(now.tv_nsec / 1000);
}

bool unreserved(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

size_t component_length(const char* text, size_t length) {
  size_t out = 0;
  for (size_t i = 0; i < length; ++i) out += unreserved(static_cast<unsigned char>(text[i])) ? 1 : 3;
  return out;
}

// encodeURIComponent bytes of `text` appended to `out`; false on overflow.
bool append_component(char* out, size_t capacity, size_t* used, const char* text, size_t length) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (unreserved(c)) {
      if (*used + 1 >= capacity) return false;
      out[(*used)++] = static_cast<char>(c);
    } else {
      if (*used + 3 >= capacity) return false;
      out[(*used)++] = '%';
      out[(*used)++] = kHex[c >> 4];
      out[(*used)++] = kHex[c & 15];
    }
  }
  return true;
}

struct Span {
  bool found;
  size_t start;
  size_t length;
};

// Per-run work buffers, allocated once.
struct Work {
  providers::TokenFetch fetch;
  discovery::RankedCoin selected[kBudgetPerRun];
  codec::BoundedJsonToken tokens[kRowTokens];
  Span spans[kMaxBatch];
  const discovery::RankedCoin* batch[kMaxBatch];
  discovery::CoinIdView refreshed_views[kBudgetPerRun];
  PriceSpotQuoteInput quotes[kPriceSpotMaxBatchItems];
  char quote_ids[kPriceSpotMaxBatchItems][kPriceSpotMaxSourceIdBytes + 1];
  discovery::market_view::MarketViewScratch view;
  unsigned char entry[kEntryBytes];
  unsigned char cache_scratch[kCacheScratchBytes];
  char image[16 * 1024];
  char row_id[discovery::kCoinPopularityMaxIdBytes + 1];
  char lowered[discovery::kCoinPopularityMaxIdBytes + 1];
};

struct ScanContext {
  Work* work;
  size_t batch_count;
  const unsigned char* base;
  bool null_row;
  bool failed;
};

// `new Map(rows.flatMap((row) => typeof row.id === 'string'
//   ? [[row.id.trim().toLowerCase(), row]] : []))` with ASCII lower-casing
// (the id is an identifier: js-case): a later row with the
// same id wins; a null element throws (the whole run rejects).
bool scan_row(void* opaque, const unsigned char* element, size_t length) {
  auto* scan = static_cast<ScanContext*>(opaque);
  Work& w = *scan->work;
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error{};
  if (!document.Parse(element, length, w.tokens, kRowTokens, &error)) {
    scan->failed = true;  // JSON.parse fails (or a native bound) for the whole body
    return false;
  }
  const codec::BoundedJsonToken* row = document.token(document.root());
  if (row->type == codec::BoundedJsonType::kNull) {
    scan->null_row = true;
    return false;
  }
  if (row->type != codec::BoundedJsonType::kObject) return true;
  const uint32_t id = document.FindObjectValue(document.root(), "id");
  if (id == codec::kBoundedJsonNoToken || document.token(id)->type != codec::BoundedJsonType::kString) return true;
  size_t decoded = 0;
  if (!document.DecodeString(id, w.row_id, sizeof(w.row_id), &decoded)) return true;
  size_t begin = 0, end = 0;
  codec::history_batch::JsTrim(w.row_id, decoded, &begin, &end);
  size_t lowered = 0;
  if (!compat::AsciiToLowerCase(w.row_id + begin, end - begin, w.lowered, sizeof(w.lowered), &lowered)) return true;
  for (size_t i = 0; i < scan->batch_count; ++i) {
    const discovery::RankedCoin& coin = *w.batch[i];
    if (coin.id_length == lowered && memcmp(coin.id, w.lowered, lowered) == 0)
      w.spans[i] = Span{true, static_cast<size_t>(element - scan->base), length};
  }
  return true;
}

// The spot store's source-id bound (printable ASCII without a slash or backslash,
// at most 512 bytes). A market id outside it gets its `mkt:` entry but no
// spot row (a native bound; /v1/prices accepts only [a-z0-9-] slugs).
bool spot_key_id(const discovery::RankedCoin& coin) {
  if (!coin.id_length || coin.id_length > kPriceSpotMaxSourceIdBytes) return false;
  for (size_t i = 0; i < coin.id_length; ++i) {
    const unsigned char c = static_cast<unsigned char>(coin.id[i]);
    if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') return false;
  }
  return true;
}

double jitter_sample(const Deps& deps) {
  double sample = 0.5;
  if (deps.jitter && deps.jitter->Sample(&sample)) return sample;
  uint32_t bits = 0;
  if (!auth::SecureRandomBytes(reinterpret_cast<uint8_t*>(&bits), sizeof(bits))) return 0.5;
  return static_cast<double>(bits) / 4294967295.0;
}

int64_t cache_now(const Deps& deps) {
  if (deps.clock) {
    const int64_t now = deps.clock->NowMs();
    if (now >= 0) return now;
  }
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool stop_requested(const Deps& deps) { return deps.stop && deps.stop(deps.stop_context); }

// One finite JSON number at `key` of the row object, as
// `typeof v === 'number' && Number.isFinite(v)`.
bool finite_number(const codec::BoundedJsonDocument& document, const char* key, double* value) {
  const uint32_t index = document.FindObjectValue(document.root(), key);
  return index != codec::kBoundedJsonNoToken && document.token(index)->type == codec::BoundedJsonType::kNumber &&
         document.Number(index, value) && isfinite(*value);
}

// The answered batch: market entries, logos and spot quotes. False when the
// run must reject (a null row) — nothing of this batch is stored then.
bool store_batch(const Deps& deps, Work& w, discovery::DiscoveryCache& cache, PriceSpotCache& spot, const char* fiat,
                 size_t count, const unsigned char* body, size_t length, RunResult* result, bool* parsed) {
  *parsed = false;
  bool write_failed = false;
  for (size_t i = 0; i < count; ++i) w.spans[i] = Span{false, 0, 0};
  if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) {
    body += 3;
    length -= 3;
  }
  bool is_array = false;
  ScanContext scan{&w, count, body, false, false};
  const bool walked = length && compat::js_json::ForEachTopLevelArrayElement(body, length, &is_array, scan_row, &scan);
  if (scan.null_row) return false;
  if (!walked || scan.failed) return true;  // http.json threw: the batch is skipped
  if (!is_array) {
    // `Array.isArray(body) ? body : []`: any other JSON lists nothing.
    codec::BoundedJsonDocument document;
    codec::BoundedJsonError error{};
    if (!document.ParseWithLimits(body, length, w.tokens, kRowTokens, kRowTokens,
                                  codec::kBoundedJsonMaxExpandedInputBytes, &error))
      return true;
  }
  *parsed = true;
  const int64_t now = cache_now(deps);
  size_t quote_count = 0;
  auto flush_quotes = [&]() {
    if (!quote_count) return;
    PriceSeriesStoreStatus series_status{};
    const PriceSpotStoreStatus status = spot.StoreProviderBatchWithSeries(
        *deps.series, fiat, "coingecko", w.quotes, quote_count, nullptr, 0, true, now, &series_status);
    if (status != PriceSpotStoreStatus::kStored) {
      ++result->spot_write_failures;
      write_failed = true;
      fortis::base::LogError("prices", "px cache write failed: coin price schedule");
    }
    result->quotes += quote_count;
    quote_count = 0;
  };
  for (size_t i = 0; i < count; ++i) {
    const discovery::RankedCoin& coin = *w.batch[i];
    const int64_t fresh_ms = FreshMsFor(coin.interval_ms);
    OutBuffer out{w.entry, sizeof(w.entry)};
    out.Text("{\"market\":");
    if (w.spans[i].found) {
      ++result->listed;
      codec::BoundedJsonDocument document;
      codec::BoundedJsonError error{};
      document.Parse(body + w.spans[i].start, w.spans[i].length, w.tokens, kRowTokens, &error);
      char logo[discovery::market_view::kMediaKeyChars + 1];
      const char* logo_key = nullptr;
      size_t image_length = 0;
      if (!deps.core_only && discovery::market_view::RowImage(document, document.root(), w.image, sizeof(w.image), &image_length) &&
          discovery::RegisterMediaSource(deps.kv, w.image, image_length, now, logo))
        logo_key = logo;
      if (!deps.core_only)
        discovery::market_view::AppendMarketEntry(document, document.root(), coin.id, coin.id_length, logo_key, out, &w.view);
      // The spot quote carried by the same row.
      bool snapshot_owned = false;
      if (!deps.core_only && deps.snapshot_owns_core_quotes && strcmp(fiat, "usd") == 0) {
        for (const char* core : kPriceCoreIds)
          snapshot_owned |= strcmp(coin.id, core) == 0;
        if (snapshot_owned) {
          // Keep the ranked fetch as a fallback if the dedicated warmer is
          // late or unavailable. A healthy core already has its own cadence.
          const auto cached = spot.Lookup(fiat, coin.id, now, true);
          const int64_t observed = cached.value.provider_observed_at_ms > 0
              ? cached.value.provider_observed_at_ms : cached.value.as_of_ms;
          snapshot_owned = cached.state == PriceSpotLookupState::kFresh &&
              cached.value.price > 0 && now - observed < 5 * kIntervalMs;
        }
      }
      double price = 0, change = 0, volume = 0;
      if (!snapshot_owned && finite_number(document, "current_price", &price) &&
          price > 0 && spot_key_id(coin)) {
        if (quote_count == kPriceSpotMaxBatchItems) flush_quotes();
        memcpy(w.quote_ids[quote_count], coin.id, coin.id_length + 1u);
        PriceSpotQuoteInput& quote = w.quotes[quote_count++];
        quote = PriceSpotQuoteInput{};
        quote.source_id = w.quote_ids[quote_count - 1];
        quote.price = price;
        quote.has_change24h_pct = finite_number(document, "price_change_percentage_24h", &change);
        quote.change24h_pct = quote.has_change24h_pct ? change : 0;
        quote.has_volume24h = finite_number(document, "total_volume", &volume);
        quote.volume24h = quote.has_volume24h ? volume : 0;
        quote.as_of_ms = now;
        const uint32_t updated = document.FindObjectValue(document.root(), "last_updated");
        char stamp[64]; size_t stamp_length = 0;
        if (updated != codec::kBoundedJsonNoToken && document.token(updated)->type == codec::BoundedJsonType::kString &&
            document.DecodeString(updated, stamp, sizeof(stamp), &stamp_length)) {
          uint16_t text[64];
          for (size_t n = 0; n < stamp_length; ++n) text[n] = static_cast<unsigned char>(stamp[n]);
          double parsed_ms = 0;
          if (compat::JsDateParse(text, stamp_length, &parsed_ms) && parsed_ms > 0 && parsed_ms <= now)
            quote.provider_observed_at_ms = static_cast<int64_t>(parsed_ms);
        }
        const int64_t jittered = discovery::OhlcJitteredFreshMs(fresh_ms, jitter_sample(deps));
        quote.fresh_ms = static_cast<uint32_t>(jittered);
      }
    } else {
      out.Text("null");
    }
    if (deps.core_only) continue;
    out.Byte('}');
    char key[16 + discovery::kCoinPopularityMaxIdBytes];
    const int key_length = snprintf(key, sizeof(key), "mkt:%s:%.*s", fiat, static_cast<int>(coin.id_length),
                                    coin.id);
    const bool stored = out.ok && key_length > 0 && static_cast<size_t>(key_length) < sizeof(key) &&
        cache.Write(key, discovery::DiscoveryCacheTtl{fresh_ms, kMarketStaleMs}, now, w.entry, out.used, nullptr);
    if (!stored) {
      ++result->market_write_failures;
      write_failed = true;
      fortis::base::LogError("prices", "market cache write failed: coin price schedule");
    }
  }
  flush_quotes();
  return !write_failed;
}

}  // namespace

int64_t FreshMsFor(int64_t interval_ms) {
  return static_cast<int64_t>(floor(static_cast<double>(interval_ms) * 1.5 + 0.5));
}

RunResult Run(const Deps& deps, const char* fiat) {
  RunResult result;
  const uint64_t started = monotonic_us();
  if (!deps.kv || !deps.series || !deps.owner || (!deps.core_only && !deps.popularity) || !fiat) {
    result.failed = true;
    return result;
  }
  discovery::RankedCoinList due;
  if (!deps.core_only &&
      !deps.popularity->Due(deps.budget < kBudgetPerRun ? deps.budget : kBudgetPerRun, &due)) {
    result.failed = true;
    return result;
  }
  result.due = due.count;
  size_t refreshed = 0;
  std::unique_ptr<Work, fortis::base::ObjectDeleter<Work>> work;
  if (due.count || deps.pinned_core || deps.core_only) {
    work.reset(fortis::base::New<Work>());
    if (!work) {
      due.Release();
      result.failed = true;
      return result;
    }
    const discovery::DiscoveryCacheSeams seams{deps.clock, deps.jitter};
    discovery::DiscoveryCache cache(deps.kv, work->cache_scratch, sizeof(work->cache_scratch), &seams);
    PriceSpotCache legacy_spot(*deps.kv);
    auto& spot = deps.spot_cache ? *deps.spot_cache : legacy_spot;
    // Pins use capacity within this run; the ranked tail retains every other
    // slot and existing quota. Do not record synthetic popularity or devices.
    size_t selected_count = 0;
    bool critical_pin = false;
    const size_t budget = deps.budget < kBudgetPerRun ? deps.budget : kBudgetPerRun;
    if (deps.pinned_core || deps.core_only) {
      const int64_t now = cache_now(deps);
      for (const char* id : kPriceCoreIds) {
        if (selected_count == budget) break;
        const auto cached = spot.Lookup(fiat, id, now, true);
        if (cached.state == PriceSpotLookupState::kNegative && !deps.core_only) continue;
        // Reserve a cadence of headroom for both the cache TTL and the app's
        // provider-age budget. A stale pin may queue under the same lane cap.
        constexpr int64_t kMaxObservationAgeMs = 5 * kIntervalMs;
        const int64_t observed = cached.value.provider_observed_at_ms > 0
            ? cached.value.provider_observed_at_ms : cached.value.as_of_ms;
        const bool fresh = cached.state == PriceSpotLookupState::kFresh &&
            cached.value.price > 0 && now - observed < kMaxObservationAgeMs;
        if (fresh && cached.value.fresh_until_ms - now > kIntervalMs &&
            now - observed < kMaxObservationAgeMs - kIntervalMs) continue;
        critical_pin |= !fresh;
        work->selected[selected_count++] = discovery::RankedCoin{id, strlen(id), 0, 0, kIntervalMs};
      }
    }
    const size_t pinned_count = selected_count;
    for (size_t i = 0; i < due.count && selected_count < budget; ++i) {
      bool duplicate = false;
      for (size_t n = 0; n < pinned_count; ++n)
        if (strcmp(work->selected[n].id, due.items[i].id) == 0) duplicate = true;
      if (!duplicate) work->selected[selected_count++] = due.items[i];
    }
    result.due = selected_count;
    const size_t batch_limit = deps.batch_ids && deps.batch_ids < kMaxBatch ? deps.batch_ids : kMaxBatch;
    size_t next = 0;
    size_t shed = 0;
    while (next < selected_count) {
      if (result.calls == deps.max_calls || result.calls == kMaxCallsPerRun) break;
      if (stop_requested(deps)) { result.stopped = true; break; }
      // As many ids as the batch size and the request path allow.
      providers::TokenFetch& fetch = work->fetch;
      TokenFetchRelease(&fetch);
      fetch = providers::TokenFetch{};
      fetch.upstream = providers::TokenUpstream::kCoinGecko;
      const int prefix = snprintf(fetch.path, sizeof(fetch.path), "%s%s&ids=", kPathPrefix, fiat);
      if (prefix <= 0 || static_cast<size_t>(prefix) >= sizeof(fetch.path)) { result.failed = true; break; }
      size_t used = static_cast<size_t>(prefix);
      const size_t suffix = deps.core_only ? 0 : sizeof(kPathSuffix) - 1;
      size_t count = 0;
      while (next < selected_count && count < batch_limit) {
        const discovery::RankedCoin& coin = work->selected[next];
        // encodeURIComponent(ids.join(',')): the comma is %2C.
        const size_t needed = (count ? 3 : 0) + component_length(coin.id, coin.id_length);
        if (used + needed + suffix + 1 > sizeof(fetch.path)) {
          if (!count) ++next;  // one id longer than the path bound is never requested
          break;
        }
        if (count) append_component(fetch.path, sizeof(fetch.path), &used, ",", 1);
        append_component(fetch.path, sizeof(fetch.path), &used, coin.id, coin.id_length);
        work->batch[count++] = &coin;
        ++next;
      }
      if (!count) continue;
      // Keep the ranked warmer's admission for any batch with ranked work.
      fetch.background = next <= pinned_count && !critical_pin && !deps.core_only;
      memcpy(fetch.path + used, kPathSuffix, suffix);
      fetch.path[used + suffix] = '\0';
      ++result.calls;
      deps.owner->FetchAll(&fetch, 1, deps.deadline_ms);
      result.last_provider_status = fetch.status;
      if (fetch.retry_after_ms > result.provider_retry_after_ms)
        result.provider_retry_after_ms = fetch.retry_after_ms;
      if (fetch.cancelled && stop_requested(deps)) { result.stopped = true; ++result.failed_calls; break; }
      if (!TokenFetchOk(fetch) || !fetch.body) {
        // provider busy or down: these coins keep their entries
        ++result.failed_calls;
        if (fetch.lane_rejected) {
          ++result.lane_rejected;
          if (deps.pinned_core) shed += count;
        }
        continue;
      }
      bool parsed = false;
      if (!store_batch(deps, *work, cache, spot, fiat, count, fetch.body.get(), fetch.body_length, &result, &parsed)) {
        result.aborted = true;
        break;
      }
      if (!parsed) {
        ++result.failed_calls;
        continue;
      }
      for (size_t i = 0; i < count; ++i)
        work->refreshed_views[refreshed++] = discovery::CoinIdView{work->batch[i]->id, work->batch[i]->id_length};
    }
    result.deferred = selected_count - next + shed;
    TokenFetchRelease(&work->fetch);
  }
  if (result.aborted) {
    // refreshMarkets rejected: nothing is marked and the dirty counters stay
    // for the next run, as the TypeScript job's onError path leaves them.
    due.Release();
    work.reset();
    result.elapsed_us = monotonic_us() - started;
    fortis::base::LogError("prices", "coin price schedule failed");
    return result;
  }
  if (refreshed && !deps.core_only)
    deps.popularity->MarkRefreshed(work->refreshed_views, refreshed);
  result.refreshed = refreshed;
  due.Release();
  work.reset();
  if (deps.core_only) {
    result.elapsed_us = monotonic_us() - started;
    return result;
  }
  discovery::CoinPopularity& popularity = *deps.popularity;
  discovery::PopularityRowList dirty;
  if (!popularity.TakeDirty(&dirty)) {
    result.failed = true;
  } else if (dirty.count) {
    const discovery::coin_popularity_store::SaveResult saved = discovery::coin_popularity_store::Save(*deps.kv, dirty.items.get(), dirty.count);
    result.saved = saved.rows;
    result.save_frames = saved.frames;
    result.save_bytes = saved.bytes;
    if (saved.failed) {
      result.failed = true;
      popularity.RequeueDirty(dirty.items.get(), dirty.count);
      fortis::base::LogError("prices", "coin popularity save failed");
    }
  }
  dirty.Release();
  result.elapsed_us = monotonic_us() - started;
  return result;
}

RestoreResult Restore(storage::KvStore& kv, discovery::CoinPopularity& popularity, int64_t now_ms) {
  RestoreResult result;
  const size_t rows = discovery::coin_popularity_store::LoadInto(kv, now_ms, popularity);
  if (rows == SIZE_MAX) {
    result.failed = true;
    fortis::base::LogError("prices", "coin popularity restore failed");
  } else {
    result.rows = rows;
  }
  return result;
}

PruneResult Prune(storage::KvStore& kv, discovery::CoinPopularity& popularity, int64_t now_ms) {
  PruneResult result;
  result.before = popularity.Size();
  popularity.Prune();
  result.after = popularity.Size();
  // TTL read checks already hide old coins; one bounded sweep removes at
  // most 256 expired records from disk.
  result.swept = kv.Sweep(now_ms, kPruneSweepLimit);
  return result;
}

}  // namespace fortis::prices::coin_prices
