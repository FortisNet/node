#pragma once
#include "chain_transport.h"
#include "provider_execution.h"

namespace fortis::chains {
// Policy only: the coordinator owns transport retry execution; malformed JSON
// retries are scheduled by Transport's existing event loop.
class ChainRetryPolicy {
 public:
  static unsigned MaxAttempts(bool post, bool retry_post) {
    return !post || retry_post ? providers::kProviderExecutionMaxAttempts : 1;
  }
  static bool RetryParse(const Call& call, size_t retries) {
    return (call.kind == CallKind::kGet || (call.kind == CallKind::kPost && call.retry_post)) && retries < 2;
  }
  static uint32_t ParseDelayMs(size_t completed_retries) { return completed_retries == 1 ? 250 : 1000; }
};

class EndpointFailover {
 public:
  size_t index() const { return index_; }
  bool Advance(size_t count, uint64_t now, uint64_t deadline) {
    if (index_ + 1 >= count || now >= deadline) return false;
    ++index_;
    return true;
  }
 private:
  size_t index_ = 0;
};

class JsonRpcClient {
 public:
  enum class Outcome : uint8_t { kSuccess, kAnswer, kTransport };
  static Value* Envelope(Arena& arena, const Call& call);
  static Outcome Interpret(Call& call, Value* body);
};
}  // namespace fortis::chains
