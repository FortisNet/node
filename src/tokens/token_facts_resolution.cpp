// Request-local production adapters for TokenIdentityResolver. The domain
// workflow lives in token_identity_resolver.cpp; these preserve KV scan,
// provider workspace/release and SWR admission behavior.
#include "token_facts_internal.h"
#include "token_identity_resolver.h"

namespace tj = fortis::codec::tj;
namespace fortis::tokens::token_facts {
namespace {
struct SymbolScan {
  Arena* arena;
  const char* symbol;
  char found[kCoinIdBytes];
  bool hit;
};

bool symbol_visit(void* context, const token_store::IdentityRecord& row) {
  auto* scan = static_cast<SymbolScan*>(context);
  if (!row.symbol.present || !row.coingecko_id.present || !row.coingecko_id.length) return true;
  const tj::Mark mark = tj::MarkOf(*scan->arena);
  Value* upper = compat::token_text::ToUpperCase(*scan->arena, row.symbol.text, row.symbol.length);
  const bool same = upper && upper->length == strlen(scan->symbol) &&
                    memcmp(upper->text, scan->symbol, upper->length) == 0;
  tj::Rewind(*scan->arena, mark);
  if (!same || row.coingecko_id.length >= sizeof(scan->found)) return true;
  memcpy(scan->found, row.coingecko_id.text, row.coingecko_id.length);
  scan->found[row.coingecko_id.length] = '\0';
  scan->hit = true;
  return false;
}

class StoredIdentityRegistry final : public TokenIdentityRegistry {
 public:
  explicit StoredIdentityRegistry(storage::KvStore* kv) : store_(kv) {}
  void FindCoin(Arena& a, const networks::TokenNetwork& network, const networks::TokenRef& ref,
                int64_t now, char (&coin_id)[kCoinIdBytes]) override {
    if (ref.kind == networks::TokenRefKind::kSymbol) {
      SymbolScan scan{&a, ref.symbol, {}, false};
      bool truncated = false;
      store_.ScanIdentities(network.id, 1024, now, symbol_visit, &scan, &truncated);
      if (scan.hit) snprintf(coin_id, sizeof(coin_id), "%s", scan.found);
    } else {
      token_store::IdentityRecord row{};
      if (store_.ReadIdentity(network.id, ref.token_id, now, &row) == token_store::ReadStatus::kOk && row.found &&
          row.coingecko_id.present && row.coingecko_id.length && row.coingecko_id.length < sizeof(coin_id)) {
        memcpy(coin_id, row.coingecko_id.text, row.coingecko_id.length);
        coin_id[row.coingecko_id.length] = '\0';
      }
    }
  }
  void Patch(const networks::TokenNetwork& network, const networks::TokenRef& ref,
             const token_store::IdentityPatch& patch, int64_t now) override {
    store_.PatchIdentity(network.id, ref.token_id, patch, now);
  }
 private:
  TokenComparisonStore store_;
};

class ProviderIdentityLookup final : public TokenIdentityProvider {
 public:
  explicit ProviderIdentityLookup(Work& work) : work_(work) {}
  bool BeginLookup() override {
    fetch_storage_ = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(work_.arena.Alloc(sizeof(providers::TokenFetch))), 1);
    fetch_ = fetch_storage_.get();
    if (!fetch_) return false;
    *fetch_ = providers::TokenFetch{};
    fetch_->background = work_.background;
    return true;
  }
  bool Search(Arena& arena, const char* symbol, Value** hits) override {
    if (!token_fetch::Search(fetch_, symbol)) return false;
    work_.env->executor->FetchAll(fetch_, 1, work_.deadline_ms);
    const bool ok = token_fetch::SearchHits(arena, *fetch_, hits) == token_fetch::Outcome::kValue;
    TokenFetchRelease(fetch_);
    return ok;
  }
  bool Contract(Arena& arena, const char* platform, const char* address, Value** info) override {
    if (!token_fetch::ContractInfo(fetch_, platform, address)) return false;
    work_.env->executor->FetchAll(fetch_, 1, work_.deadline_ms);
    const bool ok = token_fetch::ContractInfoBody(arena, *fetch_, info) == token_fetch::Outcome::kValue;
    TokenFetchRelease(fetch_);
    return ok;
  }
 private:
  Work& work_;
  std::unique_ptr<providers::TokenFetch[], providers::ArenaFetchDeleter> fetch_storage_;
  providers::TokenFetch* fetch_ = nullptr;
};

class ResolutionCache final : public TokenIdentityCache {
 public:
  explicit ResolutionCache(Work& work) : work_(work) {}
  int64_t Now() const override { return work_.Now(); }
  token_swr::State Read(const char* key, int64_t now, Arena& arena, Value** value) override {
    int64_t as_of = 0;
    return work_.env->swr->Read(key, now, work_.scratch, work_.scratch_capacity, arena, value, &as_of);
  }
  void RequestRefresh(const char* key, const networks::TokenNetwork& network,
                      const networks::TokenRef& ref, int64_t now) override {
    if (work_.env->revalidation_queue && work_.env->swr->BeginRevalidate(key, now)) {
      Revalidation task{};
      task.kind = RevalidationKind::kResolution;
      task.key.Assign(key);
      task.network.Assign(network.id);
      task.token_id.Assign(ref.token_id);
      if (!work_.env->revalidation_queue->Enqueue(task))
        work_.env->swr->EndRevalidate(key, now, false, false);
    }
  }

  bool MissHeld(const char* key, int64_t now) override { return work_.env->swr->MissHeld(key, now); }
  void Store(const char* key, int64_t now, const Value* value) override {
    work_.env->swr->Write(key, token_swr::kTokenIdentity, now, value, work_.scratch, work_.scratch_capacity, nullptr);
  }
  void HoldMiss(const char* key, int64_t now) override { work_.env->swr->HoldMiss(key, now, false); }
 private:
  Work& work_;
};

}  // namespace
namespace internal {

void ResolveTokenIdentity(Work& work, const networks::TokenNetwork& network,
                          const networks::TokenRef& ref, const char* hint, Resolution* resolution) {
  StoredIdentityRegistry registry(work.env->kv);
  ProviderIdentityLookup provider(work);
  ResolutionCache cache(work);
  TokenIdentityResolver resolver(work.arena, registry, provider, cache);
  resolver.Resolve(network, ref, hint, resolution);
}

bool ResolveTokenIdentityLive(Work& work, const networks::TokenNetwork& network,
                              const networks::TokenRef& ref, Value** out) {
  StoredIdentityRegistry registry(work.env->kv);
  ProviderIdentityLookup provider(work);
  ResolutionCache cache(work);
  TokenIdentityResolver resolver(work.arena, registry, provider, cache);
  return resolver.ResolveLive(network, ref, out);
}

}  // namespace internal
}  // namespace fortis::tokens::token_facts
