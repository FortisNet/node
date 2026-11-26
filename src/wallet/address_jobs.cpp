#include "fixed_string.h"
#include "ownership.h"
#include "exception_boundary.h"
#include "address_jobs.h"

#include "address_facets_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::wallet::address {
namespace {

// The TypeScript CHANGE_DETECTION policy.
constexpr int64_t kWatchReadWithinMs = 24 * 3600000LL;
constexpr size_t kBalanceBatch = 500;
constexpr size_t kKnownTokensPerAccount = 20;
constexpr int64_t kKnownTokensTtlMs = 15 * 60000;
constexpr double kMaxBlockRange = 2000;
constexpr size_t kIngestPerPass = 50;
constexpr size_t kMaxNetworks = 64;
constexpr size_t kRecentRows = 2000;

struct Watched {
  char id[kAccountIdBytes + 1];
  char network[96];
  char address[43];
};

struct Known {
  char id[kAccountIdBytes + 1];
  char contracts[kKnownTokensPerAccount][43];
  size_t count;
  int64_t at;
};

struct LastBlock {
  char network[96];
  uint64_t block;
};

bool lower_hex_address(const char* text) {
  if (strlen(text) != 42 || text[0] != '0' || text[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  return true;
}

int compare_contracts(const void* a, const void* b) {
  return strcmp(static_cast<const char*>(a), static_cast<const char*>(b));
}

struct TokenScan {
  Known* known;
  size_t rows;
};

// recent(id, 2000) folded into the first 20 distinct ERC-20 contracts; the
// row that completes the budget is "matched" so the page walk stops there.
bool collect_contracts(TokenScan* scan, const Value* row) {
  ++scan->rows;
  const Value* token = tj::Get(row, "tokenId");
  if (tj::IsString(token) && chains::StartsWith(token->text, "erc20:")) {
    const char* contract = token->text + 6;
    bool seen = false;
    for (size_t i = 0; i < scan->known->count && !seen; ++i) seen = !strcmp(scan->known->contracts[i], contract);
    if (lower_hex_address(contract) && !seen && scan->known->count < kKnownTokensPerAccount)
      snprintf(scan->known->contracts[scan->known->count++], 43, "%s", contract);
  }
  return scan->known->count >= kKnownTokensPerAccount || scan->rows >= kRecentRows;
}

// FNV-1a of the fingerprint string `${label}=${value}|…`.
struct Fingerprint {
  uint64_t hash = 1469598103934665603ULL;
  bool first = true;
  void Text(const char* s) {
    for (; *s; ++s) hash = (hash ^ static_cast<uint8_t>(*s)) * 1099511628211ULL;
  }
  void Part(const char* label, const char* value) {
    if (!first) Text("|");
    first = false;
    Text(label);
    Text("=");
    Text(value);
  }
};

}  // namespace

struct ChangeDetector::Impl {
  LastBlock blocks[kMaxNetworks]{};
  size_t block_count = 0;
  std::unique_ptr<Known[], base::StorageDeleter> known;
  size_t known_count = 0;
  std::unique_ptr<Watched[], base::StorageDeleter> watched;
  std::unique_ptr<Account[], base::StorageDeleter> accounts;

  Impl()
      : known(base::AllocateZeroedStorage<Known[]>(kMaxWatched, sizeof(Known))),
        watched(base::AllocateZeroedStorage<Watched[]>(kMaxWatched, sizeof(Watched))),
        accounts(base::AllocateZeroedStorage<Account[]>(kMaxWatched, sizeof(Account))) {}
  ~Impl() {
    known.reset();
    watched.reset();
    accounts.reset();
  }

  Known* KnownFor(const char* id) {
    for (size_t i = 0; i < known_count; ++i)
      if (!strcmp(known[i].id, id)) return &known[i];
    return nullptr;
  }
};

ChangeDetector::ChangeDetector() : impl_(fortis::base::New<Impl>()) {}

ChangeDetector::~ChangeDetector() {
  impl_.reset();
}

size_t ChangeDetector::Run(Facets& facets, const FacetDeps& deps, Arena& arena) {
  if (!impl_ || !impl_->known || !impl_->watched || !impl_->accounts) return 0;
  Impl& s = *impl_;
  // Networks: EVM, with RPC endpoints and an ingest source.
  const Network* networks[kMaxNetworks];
  size_t network_count = 0;
  for (size_t i = 0; i < deps.catalog->count() && network_count < kMaxNetworks; ++i) {
    const Network& n = deps.catalog->at(i);
    if (n.family == chains::Family::kEvm && n.rpc_url_count > 0 && facets.HasSource(n)) networks[network_count++] = &n;
  }
  if (!network_count) return 0;
  const int64_t now = facets.Now();
  size_t read = 0;
  if (!deps.store->RecentlyRead(now - kWatchReadWithinMs, kMaxWatched, now, s.accounts.get(), kMaxWatched, &read)) return 0;
  size_t watched_count = 0;
  for (size_t i = 0; i < read; ++i) {
    bool covered = false;
    for (size_t n = 0; n < network_count && !covered; ++n) covered = !strcmp(networks[n]->id, s.accounts[i].network);
    if (!covered || !lower_hex_address(s.accounts[i].address)) continue;
    Watched& w = s.watched[watched_count++];
    snprintf(w.id, sizeof(w.id), "%s", s.accounts[i].id);
    base::CopyCString(w.network, s.accounts[i].network);
    base::CopyCString(w.address, s.accounts[i].address);
  }
  // tracker.retainOnly(watched) and the known-token map with it.
  char (*ids)[kAccountIdBytes + 1] = static_cast<char (*)[kAccountIdBytes + 1]>(
      arena.Alloc(sizeof(char[kAccountIdBytes + 1]) * (watched_count + 1), 1));
  if (!ids) return 0;
  for (size_t i = 0; i < watched_count; ++i) memcpy(ids[i], s.watched[i].id, sizeof(ids[i]));
  deps.changes->RetainOnly(ids, watched_count);
  size_t kept = 0;
  for (size_t i = 0; i < s.known_count; ++i) {
    bool keep = false;
    for (size_t j = 0; j < watched_count && !keep; ++j) keep = !strcmp(s.known[i].id, s.watched[j].id);
    if (keep) s.known[kept++] = s.known[i];
  }
  s.known_count = kept;
  // refreshKnownTokens.
  for (size_t i = 0; i < watched_count; ++i) {
    Known* known = s.KnownFor(s.watched[i].id);
    if (known && now - known->at < kKnownTokensTtlMs) continue;
    if (!known) {
      if (s.known_count == kMaxWatched) continue;
      known = &s.known[s.known_count++];
    }
    *known = Known{};
    snprintf(known->id, sizeof(known->id), "%s", s.watched[i].id);
    const tj::Mark mark = tj::MarkOf(arena);
    TokenScan scan{known, 0};
    Value* ignored = nullptr;
    deps.store->PageTransfers(known->id, false, 1, nullptr, PageOptions{}, [&, visitor_state = &scan](const fortis::codec::tj::Value* row) { return (collect_contracts)(visitor_state, row); }, now, arena, &ignored);
    tj::Rewind(arena, mark);
    qsort(known->contracts, known->count, sizeof(known->contracts[0]), fortis::base::FatalCallback<compare_contracts>);
    known->at = now;
  }
  size_t observed = 0;
  for (size_t n = 0; n < network_count; ++n) {
    const Network& network = *networks[n];
    size_t list_count = 0;
    for (size_t i = 0; i < watched_count; ++i) list_count += !strcmp(s.watched[i].network, network.id);
    if (!list_count) continue;
    const tj::Mark mark = tj::MarkOf(arena);
    const int64_t at = facets.Now();
    chains::Call head;
    head.kind = chains::CallKind::kJsonRpc;
    head.urls = network.rpc_urls;
    head.url_count = network.rpc_url_count;
    head.method = "eth_blockNumber";
    head.background = true;
    deps.transport->Run(head, arena);
    chains::BigInt latest_big;
    if (!head.ok() || !tj::IsString(head.value) ||
        !chains::BigInt::Parse(head.value->text, head.value->length, &latest_big) || !latest_big.FitsU64()) {
      tj::Rewind(arena, mark);
      continue;  // nothing is vouched for this pass
    }
    const uint64_t latest = latest_big.LowU64();
    LastBlock* last = nullptr;
    for (size_t b = 0; b < s.block_count; ++b)
      if (!strcmp(s.blocks[b].network, network.id)) last = &s.blocks[b];
    const bool has_previous = last != nullptr;
    const uint64_t previous = last ? last->block : 0;
    if (!last && s.block_count < kMaxNetworks) {
      last = &s.blocks[s.block_count++];
      snprintf(last->network, sizeof(last->network), "%s", network.id);
    }
    if (last) last->block = latest;
    if (has_previous && latest <= previous) {
      for (size_t i = 0; i < watched_count; ++i) {
        if (strcmp(s.watched[i].network, network.id) != 0) continue;
        ChangeTracker::Observation o;
        o.observed = true;
        deps.changes->Observe(s.watched[i].id, o, at);
        ++observed;
      }
      tj::Rewind(arena, mark);
      continue;
    }
    char block_tag[24];
    snprintf(block_tag, sizeof(block_tag), "0x%llx", static_cast<unsigned long long>(latest));
    const bool continuous = has_previous && static_cast<double>(latest - previous) <= kMaxBlockRange;
    // Fingerprints in whole-account chunks of at most 500 sub-calls.
    bool* have = static_cast<bool*>(arena.Alloc(sizeof(bool) * watched_count + 1, 1));
    uint64_t* prints = static_cast<uint64_t*>(arena.Alloc(sizeof(uint64_t) * (watched_count + 1)));
    if (!have || !prints) { tj::Rewind(arena, mark); continue; }
    memset(have, 0, watched_count);
    size_t start = 0;
    while (start < watched_count) {
      size_t size = 0;
      size_t end = start;
      for (; end < watched_count; ++end) {
        if (strcmp(s.watched[end].network, network.id) != 0) continue;
        const Known* known = s.KnownFor(s.watched[end].id);
        const size_t calls = 1 + (known ? known->count : 0);
        if (size + calls > kBalanceBatch && size > 0) break;
        size += calls;
      }
      if (!size) break;
      chains::AbiCall3* calls = static_cast<chains::AbiCall3*>(arena.Alloc(sizeof(chains::AbiCall3) * size));
      size_t c = 0;
      for (size_t i = start; calls && i < end; ++i) {
        if (strcmp(s.watched[i].network, network.id) != 0) continue;
        calls[c++] = chains::AbiCall3{chains::kMulticall3Address, chains::AbiGetEthBalance(arena, s.watched[i].address)};
        const Known* known = s.KnownFor(s.watched[i].id);
        for (size_t t = 0; known && t < known->count; ++t)
          calls[c++] = chains::AbiCall3{known->contracts[t], chains::AbiBalanceOf(arena, s.watched[i].address)};
      }
      const char* data = calls ? chains::AbiAggregate3(arena, calls, c) : nullptr;
      Value* params = tj::Arr(arena);
      Value* target = tj::Obj(arena);
      if (data && params && target && tj::Set(arena, target, "to", tj::Str(arena, chains::kMulticall3Address)) &&
          tj::Set(arena, target, "data", tj::Str(arena, data)) && tj::Push(arena, params, target) &&
          tj::Push(arena, params, tj::Str(arena, block_tag))) {
        chains::Call call;
        call.kind = chains::CallKind::kJsonRpc;
        call.urls = network.rpc_urls;
        call.url_count = network.rpc_url_count;
        call.method = "eth_call";
        call.params = params;
        call.background = true;
        deps.transport->Run(call, arena);
        chains::AbiResult* decoded = nullptr;
        size_t decoded_count = 0;
        if (call.ok() && tj::IsString(call.value) &&
            chains::AbiDecodeAggregate3(arena, call.value->text, call.value->length, &decoded, &decoded_count) &&
            decoded_count == c) {
          size_t index = 0;
          for (size_t i = start; i < end; ++i) {
            if (strcmp(s.watched[i].network, network.id) != 0) continue;
            Fingerprint fp;
            const Known* known = s.KnownFor(s.watched[i].id);
            const size_t parts = 1 + (known ? known->count : 0);
            for (size_t p = 0; p < parts; ++p) {
              const chains::AbiResult& entry = decoded[index++];
              char value[200];
              if (entry.success) chains::AbiUint256(entry.data, entry.length).ToDecimal(value, sizeof(value));
              else snprintf(value, sizeof(value), "x");
              fp.Part(p == 0 ? "native" : known->contracts[p - 1], value);
            }
            have[i] = true;
            prints[i] = fp.hash;
          }
        }
      }
      start = end;
    }
    for (size_t i = 0; i < watched_count; ++i) {
      if (strcmp(s.watched[i].network, network.id) != 0) continue;
      ChangeTracker::Observation o;
      o.observed = continuous && have[i];
      o.has_fingerprint = have[i];
      o.fingerprint = have[i] ? prints[i] : 0;
      deps.changes->Observe(s.watched[i].id, o, at);
      ++observed;
    }
    tj::Rewind(arena, mark);
  }
  // ingestDirty: accounts that moved, within the pass budget.
  const int64_t at = facets.Now();
  char (*dirty)[kAccountIdBytes + 1] =
      static_cast<char (*)[kAccountIdBytes + 1]>(arena.Alloc(sizeof(char[kAccountIdBytes + 1]) * (watched_count + 1), 1));
  if (!dirty) return observed;
  const size_t dirty_count = deps.changes->Dirty(at, dirty, watched_count + 1);
  size_t due = 0;
  for (size_t d = 0; d < dirty_count && due < kIngestPerPass; ++d) {
    const Watched* account = nullptr;
    for (size_t i = 0; i < watched_count && !account; ++i)
      if (!strcmp(s.watched[i].id, dirty[d])) account = &s.watched[i];
    if (!account) continue;
    const Network* network = nullptr;
    for (size_t n = 0; n < network_count && !network; ++n)
      if (!strcmp(networks[n]->id, account->network)) network = networks[n];
    if (!network) continue;
    memcpy(dirty[due], account->id, sizeof(dirty[due]));
    ++due;
  }
  for (size_t d = 0; d < due; ++d) deps.changes->MarkIngestRequested(dirty[d], at);
  for (size_t d = 0; d < due; ++d) {
    const Watched* account = nullptr;
    for (size_t i = 0; i < watched_count && !account; ++i)
      if (!strcmp(s.watched[i].id, dirty[d])) account = &s.watched[i];
    for (size_t n = 0; account && n < network_count; ++n)
      if (!strcmp(networks[n]->id, account->network)) facets.EnsureFresh(account->id, *networks[n], account->address, 0);
  }
  return observed;
}

}  // namespace fortis::wallet::address
