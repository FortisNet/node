#include "token_facts.h"
#include "token_provider.h"
namespace fortis::tokens::token_facts {
void ProviderOwnerExecution::FetchAll(providers::TokenFetch* fetches, size_t count, uint64_t deadline_ms) {
  owner_.FetchAll(fetches, count, deadline_ms);
}
}  // namespace fortis::tokens::token_facts
