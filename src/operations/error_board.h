#pragma once

#include "clock.h"
#include "error_reporter.h"
#include "threading.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fortis::storage { class KvStore; }
namespace fortis::http { class HttpRouter; class HttpReply; struct HttpRequest; }

namespace fortis::operations {

/** Bounded public error summaries with asynchronous, atomic KV persistence.
 * Report and rendering access memory only. The operations sampler calls Flush;
 * destruction attempts a final flush. The borrowed KV and clock outlive this
 * object. Full boards replace the oldest summary of the same origin when
 * available; a newly observed origin claims the oldest slot. Load is a startup operation; Report, Flush and rendering are thread-safe.
 */
class ErrorBoard final : public base::ErrorReporter {
 public:
  static constexpr std::size_t kCapacity = 64;
  static constexpr std::size_t kRecordBytes = 384;
  static constexpr std::size_t kJsonCapacity = kCapacity * 512 + 128;

  explicit ErrorBoard(storage::KvStore& kv, base::Clock* clock = nullptr);
  ~ErrorBoard();
  ErrorBoard(const ErrorBoard&) = delete;
  ErrorBoard& operator=(const ErrorBoard&) = delete;

  /** Restore validated summaries; legacy records retain unclassified origin.
   * Legacy-readable base records and typed sidecars are committed atomically.
   * A sidecar is used only when its full base projection still matches, so a
   * rollback writer cannot cause stale classification to be applied.
   * Corrupt records are skipped and reported. Load is startup-only. */
  bool Load();
  void Report(std::string_view service, std::string_view code,
              std::string_view identifier = {}, base::ErrorDetails details = {}) noexcept override;
  /** Commit dirty generations in one frame; false keeps them pending for retry. */
  bool Flush() noexcept;
  /** Render a memory snapshot, or return zero when the output does not fit. */
  std::size_t RenderJson(char* output, std::size_t capacity);
  bool RegisterMissingIdentifiers(http::HttpRouter& router, bool attestation_required);

 private:
  struct Record {
    std::array<char, 25> service{};
    std::array<char, 49> code{};
    std::array<char, 101> identifier{};
    base::ErrorOrigin origin = base::ErrorOrigin::kNode;
    std::array<char, 33> provider{};
    std::uint16_t http_status = 0;
    std::uint64_t count = 0;
    std::int64_t first_ms = 0;
    std::int64_t last_ms = 0;
    std::uint64_t generation = 0;
    bool dirty = false;
  };

  static bool ValidField(std::string_view value, std::size_t maximum, bool empty = false);
  static bool Decode(std::string_view value, Record& record);
  static std::size_t Encode(const Record& record, char* output, std::size_t capacity);
  static std::size_t EncodeLegacy(const Record& record, char* output, std::size_t capacity);
  void MissingIdentifiers(const http::HttpRequest& request, http::HttpReply& reply);

  storage::KvStore& kv_;
  base::Clock& clock_;
  std::array<Record, kCapacity> records_{};
  bool write_failed_ = false;
  bool attestation_required_ = false;
  base::Mutex mutex_;
  base::Mutex flush_mutex_;
};

}  // namespace fortis::operations
