#pragma once
#include "token_facts.h"
namespace fortis::tokens::token_facts {
struct TokenFactChange {
  bool present = false;
  Value* value = nullptr;
  bool has_as_of = false;
  int64_t as_of = 0;
  const char* source = nullptr;
  const char* source_id = nullptr;
};
// All values/strings borrow the work arena. Writes retain existing frame,
// source-id and observation/invalidation order; the writer owns no storage.
class TokenFactWriter {
 public:
  explicit TokenFactWriter(Work& work) : work_(work) {}
  TokenFactChange Change(Value* value, bool has_as_of, int64_t as_of,
                         const char* fallback, const char* candidate_source_id);
  bool Write(const char* network, const char* token_id, const TokenFactChange& market,
             const TokenFactChange& profile, const TokenFactChange& holders,
             const TokenFactChange& contract, const TokenFactChange& safety);
  void PersistRevalidated(Facet facet, const networks::TokenNetwork& network,
                          const networks::NativeNetworkConfig* seed, const char* token_id,
                          const char* coin_id, Value* value, int64_t as_of);
 private:
  Work& work_;
};
}  // namespace fortis::tokens::token_facts
