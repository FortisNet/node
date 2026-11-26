#pragma once

// Internal to the news read routes (news_routes.cpp, news_reactions.cpp,
// news_reaction_prices.cpp): the constants, reply helpers and reaction state
// they share. Nothing outside src/news/news_r*.cpp includes this.
#include "news_routes.h"

#include "device_enrollment.h"
#include "js_case.h"
#include "js_json.h"
#include "proto_wire.h"
#include "news_json.h"
#include "price_history_batch_codec.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace fortis::news {

namespace news_routes_internal {

using fortis::news::Arena;
using fortis::news::Cluster;
using fortis::news::OutBuffer;
using fortis::news::StoreStatus;
using fortis::news::Topic;
using fortis::news::U16;
constexpr uint32_t kCost = 1;
constexpr char kJsonType[] = "application/json; charset=utf-8";
constexpr char kFeedCache[] = "private, max-age=60";
constexpr discovery::DiscoveryCacheTtl kTtlReactions{60000, 120000};
constexpr discovery::DiscoveryCacheTtl kTtlSamples{60000, 300000};
constexpr int64_t kDayMs = 86400000;
constexpr uint32_t kHandoffTimeoutMs = 30000;
constexpr uint64_t kOwnerDeadlineMs = 25000;
constexpr size_t kReactionJobs = 8;
constexpr size_t kReactionWaiters = 8;
constexpr size_t kReactionCoins = 7;    // six tagged assets plus the benchmark

// Native per-coin ceilings on retained samples. The reaction window is
// [published - 30 min, published + 24 h] (24.5 h, at most three UTC days);
// CoinGecko's market_chart/range answers a one-day range at 5-minute
// granularity (about 295 samples in the window), and even minutely data
// stays under 1,500. A coin whose samples would exceed either ceiling is
// treated like an unreadable day (no points), so one job holds at most
// 7 x (256 KiB raw + 4,096 x 24 B samples + 4,096 x 8 B order) = 2.5 MiB and
// the eight jobs at most 20 MiB, independent of the 512 KiB provider body cap.
constexpr size_t kReactionMaxSamples = 4096;
constexpr size_t kReactionMaxRawBytes = 256 * 1024;
constexpr int kWindows[4] = {300, 900, 3600, 86400};  // REACTION_WINDOWS

void send_json(int client, int status, const char* reason, const char* body, bool head,
               int cost, uint32_t remaining);
http::HttpResponse json_response(int status, const char* reason, const char* body);
int64_t route_now(const NewsRouteContext* route);
void append_iso(OutBuffer& out, int64_t ms);
void append_component(OutBuffer& out, const char* text, size_t length);
bool normalize_id(const http::HttpRequest& request, char output[27]);
bool reject_raw_non_ascii(int client, const http::HttpRequest& request, bool head);
bool reject_long_id(int client, const http::HttpRequest& request, bool head);

// ---- protobuf from the cached JSON payload --------------------------------------

struct Proto {
  Arena* arena;
  bool ok = true;

  OutBuffer sub(size_t capacity) {
    uint8_t* data = static_cast<uint8_t*>(arena->Alloc(capacity ? capacity : 1, 1));
    if (!data) ok = false;
    return OutBuffer{data, data ? capacity : 0};
  }
  void string(OutBuffer& out, uint32_t field, const json::Value* value, bool implicit) {
    if (!value || value->type != json::Type::kString) return;
    if (implicit && !value->string.length) return;
    OutBuffer text = sub(value->string.length * 3 + 1);
    fortis::news::AppendUtf8(text, value->string);
    if (!text.ok) { ok = false; return; }
    codec::proto_wire::PutBytes(out, field, text.data, text.used);
  }
  void message(OutBuffer& out, uint32_t field, const OutBuffer& body) {
    if (!body.ok) { ok = false; return; }
    codec::proto_wire::PutBytes(out, field, body.data, body.used);
  }
  void optional_double(OutBuffer& out, uint32_t field, const json::Value* value) {
    if (value && value->type == json::Type::kNumber) codec::proto_wire::PutDouble(out, field, value->number);
  }
  void varint(OutBuffer& out, uint32_t field, uint64_t value) {
    if (!value) return;
    codec::proto_wire::PutTag(out, field, 0);
    codec::proto_wire::PutVarint(out, value);
  }
};

using ProtoFn = bool (*)(const json::Value&, Arena*, OutBuffer&);

http::HttpResponse conditional(const http::HttpRequest& request, const char* name_space,
                         const uint8_t* payload, size_t length, ProtoFn proto,
                         NewsWorkspace* workspace, Arena* arena, const char* cache_control);

}  // namespace news_routes_internal
using namespace news_routes_internal;

// ---- reactions ------------------------------------------------------------------

namespace news_routes_internal {
void handle_reactions(int client, const http::HttpRequest& request, bool head, void* opaque);
}

class ReactionService {
 public:
  ReactionService() = default;
  ~ReactionService();
  ReactionService(const ReactionService&) = delete;
  ReactionService& operator=(const ReactionService&) = delete;
  struct Sample {
    double at = 0;
    double price = 0;
    uint32_t raw_offset = 0;
    uint32_t raw_length = 0;
  };
  struct Coin {
    char id[160]{};
    int64_t day = 0;
    bool done = false;
    bool failed = false;
    bool waiting = false;
    uint64_t request_id = 0;
    char key[200]{};
    std::unique_ptr<Sample[], base::StorageDeleter> samples;
    size_t sample_count = 0;
    size_t sample_capacity = 0;
    std::unique_ptr<char[], base::StorageDeleter> raw;
    size_t raw_used = 0;
    size_t raw_capacity = 0;
  };
  struct TickerCopy {
    std::unique_ptr<uint16_t[], base::StorageDeleter> symbol;
    size_t symbol_length = 0;
    bool has_coin_id = false;
    char coin_id[160]{};
  };
  struct Waiter {
    uint64_t token = 0;
    bool protobuf = false;
    int cost = -1;
    uint32_t remaining = 0;
    // The waiter's If-None-Match, so a cold measurement still answers 304
    // to a client that already holds the same representation.
    bool has_if_none_match = false;
    char if_none_match[1024]{};
  };
  struct Job;
  struct Pending {
    ReactionService* owner = nullptr;
    Job* job = nullptr;
    size_t coin = 0;
    bool refresh = false;  // a background revalidation of a stale sample day
    char key[200]{};
  };
  struct Job {
    bool used = false;
    bool background = false;
    char cache_key[128]{};
    char id[27]{};
    int64_t published_ms = 0;
    int64_t now_ms = 0;
    int64_t from_ms = 0;
    int64_t to_ms = 0;
    std::unique_ptr<TickerCopy[]> tickers;
    size_t ticker_count = 0;
    Coin coins[kReactionCoins]{};
    size_t coin_count = 0;
    Waiter waiters[kReactionWaiters]{};
    size_t waiter_count = 0;
    size_t outstanding = 0;
  };
  Pending* AcquirePending();
  bool Submit(Job& job, size_t index, const char* key, const char* path,
            bool refresh);
  static void OnSample(void* context, const discovery::DiscoveryProviderResult& result, const unsigned char* body,
               size_t body_length);
  void Advance(Job& job, size_t index);
  void Finish(Job& job);
  void StartJob(Job& job, const Cluster& cluster, int64_t now);
  Job* AcquireJob();
  NewsRouteContext* route = nullptr;
 private:
  friend void news_routes_internal::handle_reactions(int, const http::HttpRequest&, bool, void*);
  Job jobs[kReactionJobs]{};
  Pending pending[kReactionJobs * kReactionCoins * 2]{};
};

extern NewsReactionsState* g_news_reactions;

namespace news_routes_internal {

bool prices_value(const unsigned char* body, size_t length, uint8_t* output, size_t capacity,
                  size_t* used, Arena* arena);
void handle_reactions(int client, const http::HttpRequest& request, bool head, void* opaque);

}  // namespace news_routes_internal

}  // namespace fortis::news
