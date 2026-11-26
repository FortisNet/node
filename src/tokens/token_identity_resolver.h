#pragma once

#include "token_facts.h"
#include "token_swr.h"

namespace fortis::tokens::token_store { struct IdentityPatch; }

namespace fortis::tokens::token_facts {

// Borrowed operation dependencies. Implementations are owned by composition;
// they are never deleted through these interfaces. Documents and patch strings
// borrow the request arena and must not be retained by an implementation.
class TokenIdentityRegistry {
 public:
  // Misses/read failures leave coin_id empty; the resolver may try a provider.
  virtual void FindCoin(Arena& arena, const networks::TokenNetwork& network,
                        const networks::TokenRef& ref, int64_t now,
                        char (&coin_id)[kCoinIdBytes]) = 0;
  virtual void Patch(const networks::TokenNetwork& network, const networks::TokenRef& ref,
                     const token_store::IdentityPatch& patch, int64_t now) = 0;
 protected:
  ~TokenIdentityRegistry() = default;
};

class TokenIdentityProvider {
 public:
  // Reserve lookup workspace after the registry miss, even when the network
  // subsequently has no contract platform. False retains the old OOM failure.
  virtual bool BeginLookup() = 0;
  // Require a successful BeginLookup. Success returns a validated search array
  // with string id/symbol fields, or the parsed contract body, in arena.
  virtual bool Search(Arena& arena, const char* symbol, Value** hits) = 0;
  virtual bool Contract(Arena& arena, const char* platform, const char* address, Value** info) = 0;
 protected:
  ~TokenIdentityProvider() = default;
};

class TokenIdentityCache {
 public:
  virtual int64_t Now() const = 0;
  virtual token_swr::State Read(const char* key, int64_t now, Arena& arena, Value** value) = 0;
  // Admit/queue a stale resolution refresh; a rejected enqueue releases the
  // claim without recording a failure. An absent queue makes this a no-op.
  virtual void RequestRefresh(const char* key, const networks::TokenNetwork& network,
                              const networks::TokenRef& ref, int64_t now) = 0;
  virtual bool MissHeld(const char* key, int64_t now) = 0;
  virtual void Store(const char* key, int64_t now, const Value* value) = 0;
  virtual void HoldMiss(const char* key, int64_t now) = 0;
 protected:
  ~TokenIdentityCache() = default;
};

// Request-scoped identity workflow. Dependencies and arena must outlive this
// object. Ref/cache/registry/provider/hint precedence and ambiguity live here;
// transport, KV and queue mechanics stay in adapters.
class TokenIdentityResolver {
 public:
  TokenIdentityResolver(Arena& arena, TokenIdentityRegistry& registry,
                        TokenIdentityProvider& provider, TokenIdentityCache& cache)
      : arena_(arena), registry_(registry), provider_(provider), cache_(cache) {}
  void Resolve(const networks::TokenNetwork& network, const networks::TokenRef& ref,
               const char* hint, Resolution* resolution);
  // Bypass the resolution cache for an admitted background revalidation.
  // False means a failed lookup, distinct from a successful unresolved result.
  bool ResolveLive(const networks::TokenNetwork& network, const networks::TokenRef& ref, Value** out);

 private:
  Arena& arena_;
  TokenIdentityRegistry& registry_;
  TokenIdentityProvider& provider_;
  TokenIdentityCache& cache_;
};

}  // namespace fortis::tokens::token_facts
