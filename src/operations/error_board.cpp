#include "error_board.h"

#include "fixed_string.h"
#include "kv_store.h"
#include "network_registry.h"
#include "log.h"
#include "price_history_batch_codec.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>
#include <type_traits>

namespace fortis::operations {
namespace {
constexpr char kPrefix[] = "ops/error/v1/";
constexpr char kMetadataPrefix[] = "ops/error-meta/v2/";
constexpr std::int64_t kMaxTimestampMs = 253402300799999LL;

template <typename Integer>
bool number(std::string_view text, Integer& value) {
  if (text.empty()) return false;
  // Stored counters and timestamps are nonnegative. Parsing unsigned also
  // avoids libc++ 14's signed from_chars/GCC maybe-uninitialized diagnostic.
  std::make_unsigned_t<Integer> unsigned_value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), unsigned_value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      unsigned_value > static_cast<std::make_unsigned_t<Integer>>(std::numeric_limits<Integer>::max())) return false;
  value = static_cast<Integer>(unsigned_value);
  return true;
}

void key_for(std::size_t index, char (&key)[40]) {
  std::snprintf(key, sizeof(key), "%s%zu", kPrefix, index);
}

void unsigned_number(codec::history_batch::OutBuffer& out, std::uint64_t value) {
  char text[32];
  const auto converted = std::to_chars(text, text + sizeof(text), value);
  if (converted.ec != std::errc{}) { out.ok = false; return; }
  out.Append(text, static_cast<std::size_t>(converted.ptr - text));
}
}  // namespace

ErrorBoard::ErrorBoard(storage::KvStore& kv, base::Clock* clock)
    : kv_(kv), clock_(clock ? *clock : base::SystemClock()) {}

ErrorBoard::~ErrorBoard() { (void)Flush(); }

bool ErrorBoard::ValidField(std::string_view value, std::size_t maximum, bool empty) {
  return (empty || !value.empty()) && value.size() <= maximum &&
      std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
            byte == '.' || byte == ':';
      });
}

bool ErrorBoard::Decode(std::string_view value, Record& record) {
  std::array<std::string_view, 10> fields{};
  std::size_t count = 0;
  while (count < fields.size()) {
    const auto separator = value.find('|');
    fields[count++] = value.substr(0, separator);
    if (separator == std::string_view::npos) break;
    value.remove_prefix(separator + 1);
    if (count == fields.size()) return false;
  }
  if (!((count == 7 && fields[0] == "1") || (count == 10 && fields[0] == "2"))) return false;
  Record parsed{};
  if (!ValidField(fields[1], 24) || !ValidField(fields[2], 48) ||
      !ValidField(fields[3], 100, true) || !number(fields[4], parsed.count) ||
      !number(fields[5], parsed.first_ms) || !number(fields[6], parsed.last_ms) ||
      !parsed.count || parsed.first_ms < 0 || parsed.last_ms < parsed.first_ms || parsed.last_ms > kMaxTimestampMs) return false;
  parsed.origin = base::ErrorOrigin::kUnclassified;
  if (count == 10) {
    if (fields[7] == "node") parsed.origin = base::ErrorOrigin::kNode;
    else if (fields[7] == "third_party") parsed.origin = base::ErrorOrigin::kThirdParty;
    else if (fields[7] != "unclassified") return false;
    if (!ValidField(fields[8], 32, parsed.origin != base::ErrorOrigin::kThirdParty) ||
        !number(fields[9], parsed.http_status) ||
        (parsed.http_status != 0 && (parsed.http_status < 100 || parsed.http_status > 599))) return false;
    base::CopyCString(parsed.provider.data(), parsed.provider.size(), fields[8]);
  }
  // Preserve old evidence without guessing its cause; the next flush upgrades it.
  parsed.dirty = count == 7;
  base::CopyCString(parsed.service.data(), parsed.service.size(), fields[1]);
  base::CopyCString(parsed.code.data(), parsed.code.size(), fields[2]);
  base::CopyCString(parsed.identifier.data(), parsed.identifier.size(), fields[3]);
  parsed.generation = 1;
  record = parsed;
  return true;
}

std::size_t ErrorBoard::Encode(const Record& record, char* output, std::size_t capacity) {
  const int written = std::snprintf(output, capacity, "2|%s|%s|%s|%llu|%lld|%lld|%s|%s|%u",
      record.service.data(), record.code.data(), record.identifier.data(),
      static_cast<unsigned long long>(record.count), static_cast<long long>(record.first_ms),
      static_cast<long long>(record.last_ms), base::ErrorOriginName(record.origin),
      record.provider.data(), static_cast<unsigned>(record.http_status));
  return written >= 0 && static_cast<std::size_t>(written) < capacity
      ? static_cast<std::size_t>(written) : 0;
}

std::size_t ErrorBoard::EncodeLegacy(const Record& record, char* output, std::size_t capacity) {
  const int written = std::snprintf(output, capacity, "1|%s|%s|%s|%llu|%lld|%lld",
      record.service.data(), record.code.data(), record.identifier.data(),
      static_cast<unsigned long long>(record.count), static_cast<long long>(record.first_ms),
      static_cast<long long>(record.last_ms));
  return written >= 0 && static_cast<std::size_t>(written) < capacity ? static_cast<std::size_t>(written) : 0;
}

bool ErrorBoard::Load() {
  bool corrupt = false;
  bool loaded = false;
  {
    base::LockGuard lock(mutex_);
    std::array<bool, kCapacity> needs_projection{};
    loaded = kv_.Scan(kPrefix, nullptr, kCapacity + 1, clock_.NowMs(),
        [&](const char* key, const void* data, std::size_t size) {
          std::size_t index = 0;
          const std::string_view suffix(key + sizeof(kPrefix) - 1);
          Record record{};
          if (!number(suffix, index) || index >= kCapacity || size > kRecordBytes ||
              !Decode({static_cast<const char*>(data), size}, record)) {
            corrupt = true;
          } else {
            needs_projection[index] = size && static_cast<const char*>(data)[0] == '2';
            record.dirty = record.dirty || needs_projection[index];
            records_[index] = record;
          }
          return true;
        }, false);
    // Scan visitors run under the KV mutex; sidecar reads must follow the scan.
    if (loaded) for (std::size_t index = 0; index < records_.size(); ++index) {
      if (!records_[index].count) continue;
      char key[40], value[kRecordBytes]; std::size_t length = 0;
      std::snprintf(key, sizeof(key), "%s%zu", kMetadataPrefix, index);
      const auto status = kv_.GetStatus(key, clock_.NowMs(), value, sizeof(value), &length);
      if (status == storage::KvReadStatus::Missing) continue;
      if (status == storage::KvReadStatus::IoError) { loaded = false; break; }
      Record typed{};
      if (status != storage::KvReadStatus::Found || !Decode({value, length}, typed) || !length || value[0] != '2') {
        corrupt = true; continue;
      }
      char wanted[kRecordBytes], actual[kRecordBytes];
      const auto wanted_length = EncodeLegacy(records_[index], wanted, sizeof(wanted));
      const auto actual_length = EncodeLegacy(typed, actual, sizeof(actual));
      if (wanted_length && wanted_length == actual_length && std::memcmp(wanted, actual, wanted_length) == 0) {
        typed.dirty = needs_projection[index]; records_[index] = typed;
      }
      // A valid but nonmatching sidecar is stale after a legacy writer. Keep
      // the base evidence unclassified and replace its sidecar on next flush.
    }
    write_failed_ = !loaded;
  }
  if (corrupt) Report("operations", "invalid_error_record");
  if (!loaded) Report("operations", "error_board_read_failed");
  return loaded;
}

void ErrorBoard::Report(std::string_view service, std::string_view code,
                        std::string_view identifier, base::ErrorDetails details) noexcept {
  if (!ValidField(service, 24) || !ValidField(code, 48) || !ValidField(identifier, 100, true) ||
      !ValidField(details.provider, 32, details.origin != base::ErrorOrigin::kThirdParty) ||
      (details.origin != base::ErrorOrigin::kNode && details.origin != base::ErrorOrigin::kThirdParty &&
       details.origin != base::ErrorOrigin::kUnclassified) ||
      (details.http_status && (details.http_status < 100 || details.http_status > 599))) return;
  // A rejected, unrecognised slug is still untrusted client input; it may be
  // a wallet address or credential submitted as an ID by mistake. Public
  // telemetry follows the same native-catalog allowlist as missing-ID reports.
  if (service == "prices" && code == "provider_rejected_coin_id" && !identifier.empty()) {
    std::size_t count = 0;
    const auto* catalog = networks::NativeNetworkRegistry{}.list(&count);
    if (!std::any_of(catalog, catalog + count,
                     [&](const auto& row) { return row.native_coingecko_id && identifier == row.native_coingecko_id; }))
      identifier = {};
  }
  try {
    const auto now = std::clamp<std::int64_t>(clock_.NowMs(), 0, kMaxTimestampMs);
    base::LockGuard lock(mutex_);
    auto record = std::find_if(records_.begin(), records_.end(), [&](const Record& entry) {
      return entry.count && service == entry.service.data() && code == entry.code.data() &&
          identifier == entry.identifier.data() && details.origin == entry.origin &&
          details.provider == entry.provider.data() && details.http_status == entry.http_status;
    });
    const bool fresh = record == records_.end();
    if (fresh) {
      record = std::find_if(records_.begin(), records_.end(), [](const Record& entry) { return !entry.count; });
      if (record == records_.end()) {
        // Repeated dependency failures must not evict every node incident.
        for (auto candidate = records_.begin(); candidate != records_.end(); ++candidate) {
          if (candidate->origin == details.origin &&
              (record == records_.end() || candidate->last_ms < record->last_ms)) record = candidate;
        }
        if (record == records_.end()) {
          record = std::min_element(records_.begin(), records_.end(), [](const Record& left, const Record& right) {
            return left.last_ms < right.last_ms;
          });
        }
      }
    }
    const auto generation = record->generation + 1;
    if (fresh) {
      *record = Record{};
      base::CopyCString(record->service.data(), record->service.size(), service);
      base::CopyCString(record->code.data(), record->code.size(), code);
      base::CopyCString(record->identifier.data(), record->identifier.size(), identifier);
      record->origin = details.origin;
      record->http_status = details.http_status;
      base::CopyCString(record->provider.data(), record->provider.size(), details.provider);
      record->first_ms = now;
    }
    record->generation = generation ? generation : 1;
    if (record->count != std::numeric_limits<std::uint64_t>::max()) ++record->count;
    record->first_ms = std::min(record->first_ms, now);
    record->last_ms = std::max(record->last_ms, now);
    record->dirty = true;
  } catch (...) {
    base::LogError("operations", "Unable to record a node error");
  }
}

bool ErrorBoard::Flush() noexcept {
  try {
    base::LockGuard flush(flush_mutex_);
    std::array<Record, kCapacity> snapshot;
    {
      base::LockGuard lock(mutex_);
      snapshot = records_;
    }
    storage::KvBatch batch(kCapacity * kRecordBytes * 2, kCapacity * 2);
    bool pending = false;
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
      if (!snapshot[index].dirty) continue;
      pending = true;
      char key[40], value[kRecordBytes], legacy[kRecordBytes], metadata_key[40];
      key_for(index, key);
      std::snprintf(metadata_key, sizeof(metadata_key), "%s%zu", kMetadataPrefix, index);
      const auto length = Encode(snapshot[index], value, sizeof(value));
      const auto legacy_length = EncodeLegacy(snapshot[index], legacy, sizeof(legacy));
      if (!length || !legacy_length || !batch.Put(key, legacy, legacy_length, 0) ||
          !batch.Put(metadata_key, value, length, 0)) {
        base::LockGuard lock(mutex_);
        write_failed_ = true;
        return false;
      }
    }
    if (!pending) return true;
    const bool stored = batch.Commit(&kv_);
    {
      base::LockGuard lock(mutex_);
      write_failed_ = !stored;
      if (stored) {
        for (std::size_t index = 0; index < snapshot.size(); ++index) {
          if (snapshot[index].dirty && snapshot[index].generation == records_[index].generation)
            records_[index].dirty = false;
        }
      }
    }
    if (stored) {
      for (const auto& record : snapshot) {
        if (!record.dirty) continue;
        base::LogError("operations", "operational_error origin=%s service=%s provider=%s code=%s http_status=%u total=%llu",
            base::ErrorOriginName(record.origin), record.service.data(), record.provider.data(),
            record.code.data(), static_cast<unsigned>(record.http_status),
            static_cast<unsigned long long>(record.count));
      }
    }
    return stored;
  } catch (...) {
    base::LockGuard lock(mutex_);
    write_failed_ = true;
    base::LogError("operations", "Unable to persist node errors");
    return false;
  }
}

std::size_t ErrorBoard::RenderJson(char* output, std::size_t capacity) {
  codec::history_batch::OutBuffer out{reinterpret_cast<std::uint8_t*>(output), capacity};
  base::LockGuard lock(mutex_);
  const bool pending = std::any_of(records_.begin(), records_.end(), [](const Record& record) { return record.dirty; });
  out.Text("{\"persisted\":");
  out.Text(!pending && !write_failed_ ? "true" : "false");
  out.Text(",\"writeFailed\":");
  out.Text(write_failed_ ? "true" : "false");
  out.Text(",\"records\":[");
  bool first = true;
  for (const auto& record : records_) {
    if (!record.count) continue;
    if (!first) out.Byte(',');
    first = false;
    out.Text("{\"origin\":\""); out.Text(base::ErrorOriginName(record.origin));
    out.Text("\",\"provider\":\""); out.Text(record.provider.data());
    out.Text("\",\"httpStatus\":");
    if (record.http_status) unsigned_number(out, record.http_status); else out.Text("null");
    out.Text(",\"service\":\""); out.Text(record.service.data());
    out.Text("\",\"code\":\""); out.Text(record.code.data());
    out.Text("\",\"identifier\":\""); out.Text(record.identifier.data());
    out.Text("\",\"count\":"); unsigned_number(out, record.count);
    out.Text(",\"firstSeenMs\":"); unsigned_number(out, static_cast<std::uint64_t>(record.first_ms));
    out.Text(",\"lastSeenMs\":"); unsigned_number(out, static_cast<std::uint64_t>(record.last_ms));
    out.Byte('}');
  }
  out.Text("]}");
  return out.ok ? out.used : 0;
}

}  // namespace fortis::operations
