#pragma once

#include "kv_store.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::ai {

// Aggregates start when this schema is first written. Older conversations do
// not contain provider usage, so they cannot be silently counted as zero.
struct StatsCounts {
  uint64_t users = 0;
  uint64_t user_messages = 0;
  uint64_t assistant_messages = 0;
  uint64_t input_tokens = 0;
  uint64_t output_tokens = 0;
  uint64_t measured_replies = 0;
  int64_t since_ms = 0;
};

struct TokenUsage {
  bool available = false;
  uint64_t input = 0;  // includes cache creation and cache reads
  uint64_t output = 0;
};

enum class StatsEvent : uint8_t { UserMessage, AssistantMessage };

// Prepared under ai::Store's mutex and appended to the message's existing KV
// frame. Markers are pseudonymous HMAC owner IDs, never wallet addresses.
struct StatsWrites {
  char keys[6][192]{};
  char values[3][160]{};
  storage::KvMutation mutations[6]{};
  size_t count = 0;
};

bool PrepareStatsWrites(storage::KvStore& kv, const char* owner_id, int64_t now_ms,
                        StatsEvent event, const TokenUsage& usage, StatsWrites* out);
bool ReadStats(storage::KvStore& kv, const char* bucket, int64_t now_ms, StatsCounts* out);
size_t RenderAiStatsPage(storage::KvStore& kv, int64_t now_ms, char* output, size_t capacity);
void HourBucket(int64_t now_ms, char out[24]);
void DayBucket(int64_t now_ms, char out[24]);

}  // namespace fortis::ai
