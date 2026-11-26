#include <cmath>
#include "test_support.h"
// Seven-day retention soak for automatic KV compaction. A deterministic
// simulated clock drives representative domain workloads (key shapes, value
// sizes, TTLs and multi-record frames from the native stores) while a
// maintenance thread runs the production sweep + MaintainCompaction policy
// concurrently with a foreground reader. Every seven simulated days the store
// restarts and the whole acknowledged model is compared, including related
// index records. Reports volume size, write amplification, foreground
// latency percentiles, mutex holds, RSS and allocator peak.
//
// Usage: kv_compaction_soak [days=28] [scale=1.0] [directory]
#include "../src/storage/kv_store.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

namespace {

using fortis::storage::KvCompactionPolicy;
using fortis::storage::KvCompactionStats;
using fortis::storage::KvMutation;
using fortis::storage::KvStore;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

constexpr int64_t kMinute = 60 * 1000;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kDay = 24 * kHour;
constexpr int64_t kStart = 1767225600000;  // 2026-01-01T00:00:00Z

uint64_t NowUs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000u + now.tv_nsec / 1000u;
}

uint64_t Fnv(const std::string& text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
  return hash;
}

void FillValue(const std::string& key, uint32_t version, uint32_t size, std::string* out) {
  if (size < 8) size = 8;
  out->resize(size);
  memcpy(&(*out)[0], &version, 4);
  memcpy(&(*out)[4], &size, 4);
  uint64_t state = Fnv(key) ^ (static_cast<uint64_t>(version) * 0x9e3779b97f4a7c15ull);
  char* data = &(*out)[0];
  for (uint32_t i = 8; i < size; ++i) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    data[i] = static_cast<char>(state);
  }
}

bool ValidValue(const std::string& key, const char* data, size_t size, uint32_t* version) {
  if (size < 8) return false;
  uint32_t claimed = 0;
  memcpy(version, data, 4);
  memcpy(&claimed, data + 4, 4);
  if (claimed != size) return false;
  std::string expected;
  FillValue(key, *version, claimed, &expected);
  return memcmp(expected.data(), data, size) == 0;
}

struct Entry {
  uint32_t version;
  uint32_t size;
  int64_t expiry;
};

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed) {}
  uint32_t Next() {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return static_cast<uint32_t>(state >> 11);
  }
  uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

struct Latency {
  std::vector<uint32_t> all, compacting;
  void Add(uint64_t us, bool during) {
    const uint32_t value = us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(us);
    all.push_back(value);
    if (during) compacting.push_back(value);
  }
};

double Percentile(std::vector<uint32_t> values, double p) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const size_t index = std::min(values.size() - 1,
                                static_cast<size_t>(p * static_cast<double>(values.size())));
  return values[index];
}

void PrintLatency(const char* name, const std::vector<uint32_t>& values) {
  if (values.empty()) { printf("  %-28s n=0\n", name); return; }
  printf("  %-28s n=%zu p50=%.0f p95=%.0f p99=%.0f max=%u us\n", name, values.size(),
         Percentile(values, 0.50), Percentile(values, 0.95), Percentile(values, 0.99),
         *std::max_element(values.begin(), values.end()));
}

struct Memory { uint64_t rss = 0, rss_peak = 0, heap = 0; };
Memory SampleMemory() {
  Memory memory;
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    memory.rss = info.resident_size;
    memory.rss_peak = info.resident_size_max;
  }
  malloc_statistics_t stats{};
  malloc_zone_statistics(nullptr, &stats);
  memory.heap = stats.size_in_use;
#elif defined(__linux__)
  if (FILE* status = fopen("/proc/self/status", "r")) {
    char line[256];
    while (fgets(line, sizeof(line), status)) {
      unsigned long long kib = 0;
      if (strncmp(line, "VmRSS:", 6) == 0) {
        kib = fortis::test::ParseInteger<unsigned long long>(line + 6);
        memory.rss = kib * 1024;
      }
      if (strncmp(line, "VmHWM:", 6) == 0) {
        kib = fortis::test::ParseInteger<unsigned long long>(line + 6);
        memory.rss_peak = kib * 1024;
      }
    }
    fclose(status);
  }
  memory.heap = mallinfo2().uordblks;
#endif
  return memory;
}

class Soak {
 public:
  Soak(const std::string& directory, double scale) : directory_(directory), scale_(scale) {}

  // ---- Workload -----------------------------------------------------------
  struct Frame {
    std::vector<std::string> keys, values;
    std::vector<int64_t> expiries;
    std::vector<bool> erases;
    std::vector<uint32_t> versions;
    void Put(const std::string& key, uint32_t version, uint32_t size, int64_t expiry) {
      keys.push_back(key);
      values.emplace_back();
      FillValue(key, version, size, &values.back());
      expiries.push_back(expiry);
      erases.push_back(false);
      versions.push_back(version);
    }
    void Erase(const std::string& key) {
      keys.push_back(key); values.emplace_back(); expiries.push_back(0);
      erases.push_back(true); versions.push_back(0);
    }
  };

  void Commit(Frame& frame, const char* domain) {
    if (frame.keys.empty()) return;
    std::vector<KvMutation> mutations(frame.keys.size());
    uint64_t encoded = 16;
    for (size_t i = 0; i < frame.keys.size(); ++i) {
      mutations[i] = {frame.keys[i].c_str(), frame.erases[i] ? nullptr : frame.values[i].data(),
                      static_cast<uint32_t>(frame.values[i].size()), frame.expiries[i],
                      frame.erases[i]};
      encoded += 15 + frame.keys[i].size() + frame.values[i].size();
    }
    const bool during = compacting_.load(std::memory_order_relaxed);
    const uint64_t start = NowUs();
    CHECK(store_.Write(mutations.data(), mutations.size()));
    const uint64_t took = NowUs() - start;
    write_latency_.Add(took, during);
    if (took > slowest_write_us_) {
      slowest_write_us_ = took;
      slowest_write_at_ = now_;
      slowest_write_during_ = during;
      slowest_write_domain_ = domain;
    }
    foreground_bytes_ += encoded;
    ++frames_;
    domain_bytes_[domain] += encoded;
    for (size_t i = 0; i < frame.keys.size(); ++i) {
      if (frame.erases[i]) model_.erase(frame.keys[i]);
      else model_[frame.keys[i]] = {frame.versions[i],
                                    static_cast<uint32_t>(frame.values[i].size()),
                                    frame.expiries[i]};
    }
    frame = Frame();
  }

  uint32_t Version() { return ++version_; }
  unsigned Scaled(unsigned base) const {
    const unsigned value = static_cast<unsigned>(std::floor(base * scale_ + 0.5));
    return value ? value : 1;
  }

  static std::string Ts(int64_t ms, int width) {
    char text[32];
    snprintf(text, sizeof(text), "%0*lld", width, static_cast<long long>(ms));
    return text;
  }

  // price/spot, open candles and seal markers every 5 minutes per coin;
  // hourly seal folds the 5m opens into a block and rewrites the growing
  // 1h week block; the 1d month block is rewritten daily.
  void Prices(int64_t now) {
    const unsigned coins = Scaled(60);
    if (now % (5 * kMinute) == 0) {
      for (unsigned coin = 0; coin < coins; ++coin) {
        const std::string id = "coin" + std::to_string(coin);
        Frame frame;
        frame.Put("price/spot/v1/usd/" + id, Version(), 172, now + 7 * kDay);
        const int64_t bucket5 = now - now % (5 * kMinute);
        const int64_t hour = now - now % kHour;
        const int64_t day = now - now % kDay;
        frame.Put("price/open/v1/5m/usd/" + id + "/" + Ts(bucket5, 19), Version(), 76,
                  now + 7 * kDay);
        frame.Put("price/open/v1/1h/usd/" + id + "/" + Ts(hour, 19), Version(), 76,
                  now + 7 * kDay);
        frame.Put("price/open/v1/1d/usd/" + id + "/" + Ts(day, 19), Version(), 76,
                  now + 7 * kDay);
        frame.Put("price/seal/v1/" + Ts(hour + kHour, 19) + "/5m/usd/" + id + "/" +
                  Ts(hour, 19), Version(), 28, hour + kHour + 7 * kDay);
        Commit(frame, "prices");
      }
    }
    if (now % kHour == 0 && now > kStart) {
      const int64_t hour = now - kHour;
      const int64_t week = hour - (hour - kStart) % (7 * kDay);
      const unsigned hours_in_week = static_cast<unsigned>((hour - week) / kHour) + 1;
      for (unsigned coin = 0; coin < coins; ++coin) {
        const std::string id = "coin" + std::to_string(coin);
        Frame frame;
        frame.Put("price/block/v1/5m/usd/" + id + "/" + Ts(hour, 19), Version(), 804,
                  hour + kHour + 7 * kDay);
        for (int64_t bucket = hour; bucket < hour + kHour; bucket += 5 * kMinute)
          frame.Erase("price/open/v1/5m/usd/" + id + "/" + Ts(bucket, 19));
        frame.Erase("price/open/v1/1h/usd/" + id + "/" + Ts(hour, 19));
        frame.Erase("price/seal/v1/" + Ts(hour + kHour, 19) + "/5m/usd/" + id + "/" +
                    Ts(hour, 19));
        frame.Put("price/block/v1/1h/usd/" + id + "/" + Ts(week, 19), Version(),
                  20 + 65 * hours_in_week + 4, week + 7 * kDay + 90 * kDay);
        if (now % kDay == 0) {
          const int64_t month = now - (now - kStart) % (30 * kDay);
          frame.Put("price/block/v1/1d/usd/" + id + "/" + Ts(month, 19), Version(),
                    20 + 65 * static_cast<unsigned>((now - month) / kDay + 1) + 4,
                    month + 30 * kDay + 366 * kDay);
          frame.Erase("price/open/v1/1d/usd/" + id + "/" + Ts(now - kDay, 19));
        }
        Commit(frame, "prices");
      }
    }
  }

  // Token liquidity: hourly observation rewrites the 6 h raw block, the pair
  // pointer and the 1h candle block atomically; every 6 h facts and the
  // comparison doc join the same frame. SWR market caches every 5 minutes.
  void Tokens(int64_t now) {
    const unsigned tokens = Scaled(40);
    if (now % (5 * kMinute) == 0) {
      for (unsigned token = 0; token < tokens; ++token) {
        Frame frame;
        frame.Put("tf:eth-mainnet:t" + std::to_string(token) + ":market", Version(), 900,
                  now + 16 * kMinute);
        Commit(frame, "tokens");
      }
    }
    if (now % kHour != 0) return;
    for (unsigned token = 0; token < tokens; ++token) {
      const std::string id = "eth-mainnet/0xtoken" + std::to_string(token);
      const int64_t block = now - now % (6 * kHour);
      const unsigned rows = static_cast<unsigned>((now - block) / kHour) + 1;
      const int64_t candle = now - (now - kStart) % (4 * kDay);
      Frame frame;
      frame.Put("token/liqraw/" + id + "/" + Ts(block, 13), Version(), 40 + 120 * rows,
                now + 7 * kDay);
      frame.Put("token/liqpair/" + id, Version(), 48, now + 7 * kDay);
      frame.Put("token/liqcandle/" + id + "/1h/" + Ts(candle, 13), Version(),
                60 + 90 * static_cast<unsigned>((now - candle) / kHour + 1),
                now + 7 * kDay);
      if (now % (6 * kHour) == 0) {
        frame.Put("token/facts/" + id, Version(), 4096, 0);
        frame.Put("token/comparison/" + id, Version(), 300, 0);
      }
      Commit(frame, "tokens");
    }
  }

  // Transfers: hourly chunks write each row with its cursor/time indexes and
  // sync event plus the head pointer in one frame; rows past seven days are
  // pruned with their indexes (PruneTerminalBefore).
  void Transfers(int64_t now) {
    if (now % kHour != 0) return;
    const unsigned accounts = Scaled(30);
    const unsigned per_hour = Scaled(40);
    Frame frame;
    for (unsigned i = 0; i < per_hour; ++i) {
      const unsigned account = rng_.Below(accounts);
      const std::string acct = "acct" + std::to_string(account);
      const std::string record = Ts(now, 13) + "-" + std::to_string(i);
      frame.Put(std::string("transfer/row/").append(acct).append("/").append(record), Version(), 600 + rng_.Below(800), 0);
      frame.Put(std::string("transfer/cursor/").append(acct).append("/").append(record), Version(), 26, 0);
      frame.Put(std::string("transfer/time/").append(acct).append("/").append(Ts(now, 13)).append("/").append(record), Version(), 26, 0);
      frame.Put("sync2/account/" + acct + "/" + Ts(++sync_sequence_, 20), Version(), 300,
                now + 7 * kDay);
      transfers_.push_back({now, acct, record});
    }
    frame.Put("sync2/meta/head", Version(), 32, 0);
    Commit(frame, "transfers");
    Frame prune;
    while (!transfers_.empty() && transfers_.front().at <= now - 7 * kDay) {
      const TransferRef& old = transfers_.front();
      prune.Erase("transfer/row/" + old.account + "/" + old.record);
      prune.Erase("transfer/cursor/" + old.account + "/" + old.record);
      prune.Erase("transfer/time/" + old.account + "/" + Ts(old.at, 13) + "/" + old.record);
      transfers_.pop_front();
      if (prune.keys.size() >= 600) Commit(prune, "transfers");
    }
    Commit(prune, "transfers");
  }

  // News: hourly batch persists stories with all their indexes (TTL
  // published + 7 d) and moves the `updated` index of recent stories.
  // Media: record + two 64 KiB blob chunks per frame, hourly touches.
  void News(int64_t now) {
    if (now % kHour != 0) return;
    const unsigned stories = Scaled(12);
    Frame frame;
    for (unsigned i = 0; i < stories; ++i) {
      const std::string id = "n" + Ts(now, 13) + "-" + std::to_string(i);
      const int64_t expiry = now + 7 * kDay;
      frame.Put("news/item/" + id, Version(), 1200 + rng_.Below(800), expiry);
      frame.Put("news/time/" + Ts(now, 13) + "/" + id, Version(), 16, expiry);
      frame.Put("news/updated/" + Ts(now, 13) + "/" + id, Version(), 16, expiry);
      frame.Put("news/source/src" + std::to_string(i % 5) + "/" + Ts(now, 13) + "/" +
                std::to_string(Fnv(id)), Version(), 16, expiry);
      frame.Put("news/article_owner/" + std::to_string(Fnv(id + "o")), Version(), 30, expiry);
      stories_.push_back({now, id, now});
    }
    // Re-persist a fifth of the last day's stories: the updated index moves.
    for (Story& story : stories_) {
      if (story.published < now - kDay || rng_.Below(5)) continue;
      const int64_t expiry = story.published + 7 * kDay;
      frame.Erase("news/updated/" + Ts(story.updated, 13) + "/" + story.id);
      story.updated = now;
      frame.Put("news/updated/" + Ts(now, 13) + "/" + story.id, Version(), 16, expiry);
      frame.Put("news/item/" + story.id, Version(), 1200 + rng_.Below(800), expiry);
      if (frame.keys.size() > 160) Commit(frame, "news");
    }
    Commit(frame, "news");
    while (!stories_.empty() && stories_.front().published <= now - 7 * kDay)
      stories_.pop_front();
    for (unsigned i = 0; i < Scaled(3); ++i) {
      const std::string key = std::to_string(Fnv("media" + Ts(now, 13) + std::to_string(i)));
      Frame media;
      media.Put("media/item/" + key, Version(), 300, now + 7 * kDay);
      media.Put("media/blob/" + key + "/0", Version(), 64 * 1024, now + 7 * kDay);
      media.Put("media/blob/" + key + "/1", Version(), 64 * 1024, now + 7 * kDay);
      Commit(media, "media");
      media_.push_back({now, key});
    }
    while (!media_.empty() && media_.front().first <= now - 7 * kDay)
      media_.pop_front();
    for (size_t i = 0; i < media_.size(); i += 7) {
      Frame touch;  // lastAccessAt / fetchCount rewrite.
      touch.Put("media/item/" + media_[i].second, Version(), 300, media_[i].first + 7 * kDay);
      Commit(touch, "media");
    }
  }

  // Support (permanent): tickets with activity/owner indexes and messages;
  // each message moves the indexes and rewrites the ticket.
  // AI (fixed 7-day lifetime): six-record open frames, message frames that
  // rewrite the conversation and add snapshots. Crash reports and device
  // token rotation round out the mix.
  void Accounts(int64_t now) {
    if (now % kHour != 0) return;
    if (now % (6 * kHour) == 0) {
      for (unsigned i = 0; i < Scaled(5); ++i) {
        const std::string id = "t" + Ts(now, 13) + std::to_string(i);
        Frame frame;
        frame.Put("support/ticket/" + id, Version(), 1000, 0);
        frame.Put("support/activity/" + Ts(9999999999999 - now, 13) + "/" + id, Version(), 26, 0);
        frame.Put("support/owner/dev" + std::to_string(i) + "/" + Ts(now, 13) + "/" + id,
                  Version(), 26, 0);
        frame.Put("support/message/" + id + "/" + Ts(now, 13) + "/m0", Version(), 500, 0);
        Commit(frame, "support");
        tickets_.push_back({id, now});
      }
    }
    for (unsigned i = 0; i < Scaled(2) && !tickets_.empty(); ++i) {
      Ticket& ticket = tickets_[rng_.Below(static_cast<uint32_t>(tickets_.size()))];
      Frame frame;
      frame.Put("support/message/" + ticket.id + "/" + Ts(now, 13) + "/m" + std::to_string(i),
                Version(), 500, 0);
      frame.Erase("support/activity/" + Ts(9999999999999 - ticket.last, 13) + "/" + ticket.id);
      frame.Put("support/activity/" + Ts(9999999999999 - now, 13) + "/" + ticket.id, Version(),
                26, 0);
      frame.Put("support/ticket/" + ticket.id, Version(), 1000, 0);
      ticket.last = now;
      Commit(frame, "support");
    }
    for (unsigned i = 0; i < Scaled(4); ++i) {
      const std::string id = "c" + Ts(now, 13) + std::to_string(i);
      const int64_t expiry = now + 7 * kDay;
      Frame frame;
      frame.Put("ai/conversation/" + id, Version(), 800, expiry);
      frame.Put("ai/nonce/" + std::to_string(Fnv(id + "n")), Version(), 26, expiry);
      frame.Put("ai/token/" + std::to_string(Fnv(id + "t")), Version(), 26, expiry);
      frame.Put("ai/message/" + id + "/m0", Version(), 600, expiry);
      frame.Put("ai/client/" + id + "/c0", Version(), 26, expiry);
      frame.Put("ai/snapshot/" + id + "/s0", Version(), 2048, expiry);
      Commit(frame, "ai");
      conversations_.push_back({now, id});
    }
    while (!conversations_.empty() && conversations_.front().first <= now - 7 * kDay)
      conversations_.pop_front();
    for (unsigned i = 0; i < Scaled(12) && !conversations_.empty(); ++i) {
      const auto& conversation =
          conversations_[rng_.Below(static_cast<uint32_t>(conversations_.size()))];
      const int64_t expiry = conversation.first + 7 * kDay;
      const std::string message = "m" + Ts(now, 13) + std::to_string(i);
      Frame frame;
      frame.Put("ai/message/" + conversation.second + "/" + message, Version(), 600, expiry);
      frame.Put("ai/client/" + conversation.second + "/" + message, Version(), 26, expiry);
      frame.Put("ai/conversation/" + conversation.second, Version(), 800, expiry);
      if (i % 3 == 0)
        frame.Put("ai/snapshot/" + conversation.second + "/" + message, Version(), 2048, expiry);
      Commit(frame, "ai");
    }
    for (unsigned i = 0; i < Scaled(2); ++i) {
      Frame frame;
      frame.Put("crash/report/" + Ts(now, 13) + std::to_string(i), Version(), 2048,
                now + 7 * kDay);
      Commit(frame, "crash");
    }
    if (now % kDay == 0) {
      for (unsigned device = 0; device < Scaled(50); ++device) {
        const std::string id = "device/id/d" + std::to_string(device);
        Frame frame;
        if (device_hash_.size() <= device) device_hash_.resize(device + 1);
        if (!device_hash_[device].empty()) frame.Erase(device_hash_[device]);
        device_hash_[device] = "device/hash/" + std::to_string(Fnv(id + Ts(now, 13)));
        frame.Put(id, Version(), 138, now + 37 * kDay);
        frame.Put(device_hash_[device], Version(), 26, now + 37 * kDay);
        Commit(frame, "device");
      }
    }
  }

  // ---- Verification -------------------------------------------------------
  void Verify(int64_t now) {
    std::vector<char> buffer(1 << 20);
    size_t visible = 0;
    for (const auto& item : model_) {
      size_t size = 0;
      const auto status = store_.GetStatus(item.first.c_str(), now, buffer.data(),
                                           buffer.size(), &size);
      if (item.second.expiry && item.second.expiry <= now) {
        CHECK(status == fortis::storage::KvReadStatus::Missing);
        continue;
      }
      ++visible;
      if (status != fortis::storage::KvReadStatus::Found) {
        fprintf(stderr, "missing acknowledged %s\n", item.first.c_str());
        CHECK(false);
      }
      uint32_t version = 0;
      CHECK(ValidValue(item.first, buffer.data(), size, &version));
      CHECK(version == item.second.version && size == item.second.size);
    }
    struct Count { size_t n = 0; } count;
    CHECK(store_.Scan("", nullptr, SIZE_MAX, now, [context = &count](const char*, const void*, size_t) {
      ++static_cast<Count*>(context)->n;
      return true;
    }));
    CHECK(count.n == visible);  // Nothing fabricated or resurrected.
    // Related records (indexes, sync events, blobs) are ordinary model
    // keys, so a lost or resurrected half of any frame fails above.
  }

  uint64_t DiskBytes() const {
    uint64_t total = 0;
    for (const char* name : {"/fortis.kv", "/fortis.kv.compact"}) {
      struct stat info {};
      if (stat((directory_ + name).c_str(), &info) == 0) total += static_cast<uint64_t>(info.st_size);
    }
    return total;
  }

  // ---- Driver -------------------------------------------------------------
  int Run(int days) {
    CHECK(store_.Open(directory_.c_str()));
    std::atomic<int64_t> sim_now{kStart};
    std::atomic<bool> done{false};
    std::mutex tick_mutex;
    std::condition_variable tick_signal;
    int64_t tick_now = kStart;
    uint64_t tick_serial = 0;
    std::atomic<uint64_t> peak_disk{0};
    std::atomic<uint64_t> heap_peak{0};
    std::atomic<uint64_t> maintenance_ticks{0};
    std::atomic<uint64_t> maintenance_us_max{0};
    // Shared by the reader and maintenance threads; the restart is exclusive.
    std::shared_mutex store_mutex;

    // Production policy: production thresholds, cadence on the simulated
    // clock (startup 5 min, interval 15 min), production slice sizes.
    KvCompactionPolicy policy;
    // Experiment knobs (defaults = production policy).
    if (const char* value = getenv("FORTIS_SOAK_SYNC_EVERY_BYTES"))
      policy.sync_every_bytes = strtoull(value, nullptr, 10);
    if (const char* value = getenv("FORTIS_SOAK_SLICE_BYTES"))
      policy.slice_bytes = strtoull(value, nullptr, 10);
    // Baseline: the same workload and sweep with compaction never started.
    const bool no_compaction = getenv("FORTIS_SOAK_NO_COMPACTION") != nullptr;
    if (no_compaction) puts("baseline: compaction disabled");
    printf("policy: slice %zu B, swap tail %zu B, sync every %llu B, tick budget %u ms\n",
           policy.slice_bytes, policy.swap_tail_bytes,
           static_cast<unsigned long long>(policy.sync_every_bytes), policy.tick_budget_ms);
    std::thread maintenance([&] {
      uint64_t seen = 0;
      while (!done) {
        int64_t now;
        {
          std::unique_lock<std::mutex> lock(tick_mutex);
          tick_signal.wait(lock, [&] { return done || tick_serial != seen; });
          if (done) break;
          seen = tick_serial;
          now = tick_now;
        }
        std::shared_lock<std::shared_mutex> guard(store_mutex);
        const uint64_t start = NowUs();
        store_.Sweep(now, 256);
        // Foreground samples taken while this call runs count as "during
        // compaction" (a not-due call returns in microseconds).
        compacting_ = true;
        const bool running = !no_compaction && store_.MaintainCompaction(now, now, policy);
        compacting_ = false;
        run_open_ = running;
        const uint64_t took = NowUs() - start;
        if (took > maintenance_us_max) maintenance_us_max = took;
        ++maintenance_ticks;
        const uint64_t disk = DiskBytes();
        if (disk > peak_disk) peak_disk = disk;
        const uint64_t heap = SampleMemory().heap;
        if (heap > heap_peak) heap_peak = heap;
      }
    });
    std::thread reader([&] {
      Rng rng(99);
      std::vector<char> buffer(1 << 20);
      while (!done) {
        usleep(100);  // Outside the lock: the restart gets its turn.
        std::shared_lock<std::shared_mutex> guard(store_mutex);
        const int64_t now = sim_now.load();
        const bool during = compacting_.load(std::memory_order_relaxed);
        char key[96];
        const unsigned kind = rng.Below(3);
        if (kind == 0) {
          snprintf(key, sizeof(key), "price/spot/v1/usd/coin%u", rng.Below(Scaled(60)));
        } else if (kind == 1) {
          snprintf(key, sizeof(key), "token/facts/eth-mainnet/0xtoken%u", rng.Below(Scaled(40)));
        } else {
          snprintf(key, sizeof(key), "sync2/meta/head");
        }
        size_t size = 0;
        const uint64_t start = NowUs();
        const auto status = store_.GetStatus(key, now, buffer.data(), buffer.size(), &size);
        read_latency_.Add(NowUs() - start, during);
        uint32_t version = 0;
        if (status == fortis::storage::KvReadStatus::Found)
          CHECK(ValidValue(key, buffer.data(), size, &version));
        if (rng.Below(16) == 0) {
          fortis::storage::KvPageResult page;
          const uint64_t page_start = NowUs();
          CHECK(store_.Page("news/time/", nullptr, 64, now, true, [](const char*, const void*, size_t) { return true; }, &page));
          page_latency_.Add(NowUs() - page_start, during);
        }
      }
    });

    const Memory baseline = SampleMemory();
    printf("soak: %d days, scale %.2f, directory %s\n", days, scale_, directory_.c_str());
    int64_t now = kStart;
    const int64_t end = kStart + days * kDay;
    uint64_t cycle_start_disk = DiskBytes();
    KvCompactionStats last{};
    while (now < end) {
      now_ = now;
      Prices(now);
      Tokens(now);
      Transfers(now);
      News(now);
      Accounts(now);
      now += kMinute;
      sim_now = now;
      {
        std::lock_guard<std::mutex> lock(tick_mutex);
        tick_now = now;
        ++tick_serial;
      }
      tick_signal.notify_one();
      if ((now - kStart) % (7 * kDay) == 0) {
        // Let a running compaction finish with extra ticks at the same
        // instant, then restart and compare the whole model.
        for (int spin = 0; spin < 2000 && run_open_; ++spin) {
          usleep(2000);
          std::lock_guard<std::mutex> lock(tick_mutex);
          ++tick_serial;
          tick_signal.notify_one();
        }
        KvCompactionStats stats;
        uint64_t before_restart = 0, after_restart = 0;
        {
          std::unique_lock<std::shared_mutex> guard(store_mutex);
          store_.CompactionStats(&stats);
          before_restart = DiskBytes();
          store_.Close();
          CHECK(store_.Open(directory_.c_str()));
          after_restart = DiskBytes();
          Verify(now);
        }
        const Memory memory = SampleMemory();
        printf("day %3lld: disk start %.1f MiB, end %.1f MiB (restart %.1f), live %.1f MiB, "
               "peak disk %.1f MiB, runs %llu (+%llu), reclaimed %.1f MiB, model keys %zu, "
               "store keys %zu, rss %.1f MiB, heap %.1f MiB\n",
               static_cast<long long>((now - kStart) / kDay), cycle_start_disk / 1048576.0,
               before_restart / 1048576.0, after_restart / 1048576.0,
               stats.live_bytes / 1048576.0, peak_disk.load() / 1048576.0,
               static_cast<unsigned long long>(stats.runs_completed),
               static_cast<unsigned long long>(stats.runs_completed - last.runs_completed),
               (stats.bytes_reclaimed - last.bytes_reclaimed) / 1048576.0, model_.size(),
               store_.Size(), memory.rss / 1048576.0, memory.heap / 1048576.0);
        fflush(stdout);
        CHECK(stats.runs_failed == 0);
        last = stats;
        cycle_start_disk = after_restart;
      }
    }
    done = true;
    tick_signal.notify_all();
    maintenance.join();
    reader.join();
    KvCompactionStats stats;
    store_.CompactionStats(&stats);
    const Memory memory = SampleMemory();
    const uint64_t compaction_bytes = stats.bytes_copied + stats.tail_bytes_copied;
    printf("summary:\n");
    printf("  foreground frames %llu, foreground bytes %.1f MiB, compaction bytes %.1f MiB "
           "(copied %.1f, tail %.1f), write amplification %.3f\n",
           static_cast<unsigned long long>(frames_), foreground_bytes_ / 1048576.0,
           compaction_bytes / 1048576.0, stats.bytes_copied / 1048576.0,
           stats.tail_bytes_copied / 1048576.0,
           static_cast<double>(foreground_bytes_ + compaction_bytes) /
               static_cast<double>(foreground_bytes_));
    for (const auto& item : domain_bytes_)
      printf("  domain %-10s %.1f MiB written\n", item.first.c_str(), item.second / 1048576.0);
    printf("  runs started %llu completed %llu failed %llu, expired dropped %llu, "
           "reclaimed %.1f MiB, compaction fsyncs %llu\n",
           static_cast<unsigned long long>(stats.runs_started),
           static_cast<unsigned long long>(stats.runs_completed),
           static_cast<unsigned long long>(stats.runs_failed),
           static_cast<unsigned long long>(stats.expired_dropped),
           stats.bytes_reclaimed / 1048576.0, static_cast<unsigned long long>(stats.fsyncs));
    printf("  disk: final %.1f MiB, live %.1f MiB, peak (log + temporary) %.1f MiB\n",
           DiskBytes() / 1048576.0, stats.live_bytes / 1048576.0, peak_disk.load() / 1048576.0);
    printf("  compaction pauses %llu, mean %.1f us, max %llu us, swap max %llu us; "
           "longest store mutex hold %llu us; longest maintenance tick %.1f ms\n",
           static_cast<unsigned long long>(stats.pauses),
           stats.pauses ? static_cast<double>(stats.pause_us_total) / stats.pauses : 0.0,
           static_cast<unsigned long long>(stats.pause_us_max),
           static_cast<unsigned long long>(stats.swap_pause_us_max),
           static_cast<unsigned long long>(stats.lock_hold_us_max),
           maintenance_us_max.load() / 1000.0);
    printf("  slowest write %llu us at sim minute %lld (%s, maintenance call %s)\n",
           static_cast<unsigned long long>(slowest_write_us_),
           static_cast<long long>((slowest_write_at_ - kStart) / kMinute),
           slowest_write_domain_.c_str(), slowest_write_during_ ? "running" : "idle");
    PrintLatency("write (all)", write_latency_.all);
    PrintLatency("write (compaction running)", write_latency_.compacting);
    PrintLatency("get (all)", read_latency_.all);
    PrintLatency("get (compaction running)", read_latency_.compacting);
    PrintLatency("page 64 (all)", page_latency_.all);
    PrintLatency("page 64 (compaction running)", page_latency_.compacting);
    const size_t model_bytes = model_.size() * (sizeof(Entry) + 96);
    printf("  memory: rss %.1f MiB (peak %.1f, baseline %.1f), heap in use %.1f MiB "
           "(sampled peak %.1f); harness model ~%.1f MiB and latency samples %.1f MiB "
           "are included\n",
           memory.rss / 1048576.0, memory.rss_peak / 1048576.0, baseline.rss / 1048576.0,
           memory.heap / 1048576.0, heap_peak.load() / 1048576.0, model_bytes / 1048576.0,
           (write_latency_.all.capacity() + write_latency_.compacting.capacity() +
            read_latency_.all.capacity() + read_latency_.compacting.capacity()) * 4 / 1048576.0);
    store_.Close();
    return 0;
  }

 private:
  struct TransferRef { int64_t at; std::string account, record; };
  struct Story { int64_t published; std::string id; int64_t updated; };
  struct Ticket { std::string id; int64_t last; };

  std::string directory_;
  double scale_;
  KvStore store_;
  std::unordered_map<std::string, Entry> model_;
  std::deque<TransferRef> transfers_;
  std::deque<Story> stories_;
  std::deque<std::pair<int64_t, std::string>> media_;
  std::vector<Ticket> tickets_;
  std::deque<std::pair<int64_t, std::string>> conversations_;
  std::vector<std::string> device_hash_;
  std::unordered_map<std::string, uint64_t> domain_bytes_;
  Rng rng_{12345};
  uint32_t version_ = 0;
  int64_t now_ = kStart;
  uint64_t sync_sequence_ = 0;
  uint64_t foreground_bytes_ = 0;
  uint64_t frames_ = 0;
  std::atomic<bool> compacting_{false};
  std::atomic<bool> run_open_{false};
  uint64_t slowest_write_us_ = 0;
  int64_t slowest_write_at_ = 0;
  bool slowest_write_during_ = false;
  std::string slowest_write_domain_;
  Latency write_latency_, read_latency_, page_latency_;
};

}  // namespace

int main(int argc, char** argv) try {
  const int days = argc > 1 ? fortis::test::ParseInteger<int>(argv[1]) : 28;
  const double scale = argc > 2 ? fortis::test::ParseDouble(argv[2]) : 1.0;
  std::string directory;
  if (argc > 3) {
    directory = argv[3];
    mkdir(directory.c_str(), 0700);
  } else {
    char path[] = "/tmp/fortis-kv-soak-XXXXXX";
    CHECK(mkdtemp(path));
    directory = path;
  }
  if (days < 7 || days % 7 || scale <= 0) {
    fprintf(stderr, "usage: %s [days (multiple of 7)] [scale] [directory]\n", argv[0]);
    return 2;
  }
  Soak soak(directory, scale);
  const int result = soak.Run(days);
  if (argc <= 3) {
    for (const char* name : {"/fortis.kv", "/fortis.kv.lock", "/fortis.kv.compact"})
      unlink((directory + name).c_str());
    rmdir(directory.c_str());
  }
  puts("native KV seven-day retention compaction soak passed");
  return result;
} catch (...) {
  return fortis::test::ReportException();
}
