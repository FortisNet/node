#include "exception_boundary.h"
#include "address_store.h"

#include "crash_report_codec.h"
#include "device_auth.h"
#include "kv_store.h"
#include "threading.h"
#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::wallet::address {
namespace {

namespace tj = fortis::codec::tj;

constexpr int64_t kReadRefreshMs = 3600000;      // account_store READ_REFRESH_MS
constexpr int64_t kSyncRetainMs = 7LL * 86400000;  // account_sync_log RETAIN_MS
constexpr int64_t kWatchTtlMs = 15 * 60000;
constexpr int64_t kWatchRenewBeforeMs = 5 * 60000;
constexpr size_t kUpsertChunk = 200;
constexpr size_t kValuationChunk = 128;
constexpr size_t kPageScan = 256;
constexpr size_t kChunkArenaBytes = 8 * 1024 * 1024;
constexpr size_t kKeyBytes = 1025;
// A page position: an index root plus a display-cursor boundary.
constexpr size_t kPositionBytes = 8192;
constexpr size_t kScratchBytes = 1024 * 1024;

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

// A key component written into `out` (encodeURIComponent).
bool component(const char* text, char* out, size_t capacity) {
  if (!text) return false;
  chains::OutBuffer buffer{reinterpret_cast<uint8_t*>(out), capacity - 1, 0, true};
  if (!chains::EncodeUriComponent(text, strlen(text), buffer) || !buffer.ok) return false;
  out[buffer.used] = '\0';
  return true;
}

struct Key {
  char text[kKeyBytes]{};
  bool ok = true;
  size_t used = 0;
  void Raw(const char* s) {
    const size_t n = strlen(s);
    if (!ok || used + n >= sizeof(text)) { ok = false; return; }
    memcpy(text + used, s, n + 1);
    used += n;
  }
  void Part(const char* s) {
    char encoded[kKeyBytes];
    if (!ok || !component(s, encoded, sizeof(encoded))) { ok = false; return; }
    Raw(encoded);
  }
  void Pad(int64_t ms) {
    char padded[24];
    Store::Pad13(ms, padded);
    Raw(padded);
  }
};

// A scanned KV entry copied out of the visitor (the store mutex is held
// while it runs, so nothing may read the KV there).
struct Entry {
  char* key;
  char* value;
  size_t length;
};
struct Collected {
  Arena* arena;
  Entry* entries;
  size_t count;
  size_t capacity;
  bool ok;
};

bool collect(const char* key, const void* value, size_t size, Collected* c) {
  if (c->count == c->capacity) return false;
  Entry& e = c->entries[c->count];
  e.key = c->arena->CopyText(key, strlen(key));
  e.value = c->arena->CopyText(static_cast<const char*>(value), size);
  e.length = size;
  if (!e.key || !e.value) { c->ok = false; return false; }
  ++c->count;
  return true;
}

// One KV Page of at most kPageScan physical entries after `after`.
bool page(const storage::KvStore* kv, const char* prefix, const char* after, bool reverse, int64_t now_ms, Arena& arena,
          Collected* out, storage::KvPageResult* result) {
  out->arena = &arena;
  out->entries = static_cast<Entry*>(arena.Alloc(sizeof(Entry) * kPageScan));
  out->count = 0;
  out->capacity = kPageScan;
  out->ok = out->entries != nullptr;
  if (!out->ok) return false;
  *result = storage::KvPageResult{};
  if (!kv->Page(prefix, after, kPageScan, now_ms, reverse, [&, visitor_state = out](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }, result)) return false;
  return out->ok;
}

Value* parse_entry(Arena& arena, const Entry& e) {
  return tj::Parse(arena, reinterpret_cast<const uint8_t*>(e.value), e.length);
}

int64_t key_ms(const char* key, size_t offset) {
  // Number(key.slice(root.length, root.length + 13)).
  char digits[14];
  size_t n = 0;
  while (n < 13 && key[offset + n]) { digits[n] = key[offset + n]; ++n; }
  digits[n] = '\0';
  return strtoll(digits, nullptr, 10);
}

bool stringify(const Value* value, uint8_t* scratch, size_t capacity, size_t* length) {
  chains::OutBuffer out{scratch, capacity, 0, true};
  if (!tj::Stringify(value, out) || !out.ok) return false;
  *length = out.used;
  return true;
}

const char* string_member(const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  return tj::IsString(v) ? v->text : nullptr;
}

bool fill_account(const Value* row, Account* out) {
  const char* id = string_member(row, "id");
  const char* network = string_member(row, "network");
  const char* address = string_member(row, "address");
  if (!id || !network || !address || strlen(id) > kAccountIdBytes || strlen(network) >= sizeof(out->network) ||
      strlen(address) >= sizeof(out->address))
    return false;
  snprintf(out->id, sizeof(out->id), "%s", id);
  snprintf(out->network, sizeof(out->network), "%s", network);
  snprintf(out->address, sizeof(out->address), "%s", address);
  out->first_read_at = RowMs(row, "firstReadAt");
  out->last_read_at = RowMs(row, "lastReadAt");
  return true;
}

// padded(sequence): String(n).padStart(20, '0').
void padded_sequence(const chains::BigInt& value, char output[32]) {
  char digits[64];
  value.ToDecimal(digits, sizeof(digits));
  const size_t digit_length = strlen(digits);
  size_t used = 0;
  for (size_t z = digit_length; z < 20; ++z) output[used++] = '0';
  memcpy(output + used, digits, digit_length + 1);
}

// The journal head (sync2/meta/head), or zero when none was written.
bool read_head(const Store& store, int64_t now_ms, Arena& arena, chains::BigInt* head) {
  *head = chains::BigInt();
  Value* value = nullptr;
  const ReadStatus status = store.GetJson("sync2/meta/head", now_ms, arena, &value);
  if (status == ReadStatus::kError) return false;
  if (status == ReadStatus::kMissing) return true;
  if (!tj::IsString(value)) return false;
  return chains::BigInt::ParseDecimal(value->text, value->length, head);
}

bool is_unpriced(const Value* row) {
  return chains::Equals(tj::Get(row, "status"), "confirmed") && is_null(tj::Get(row, "fiatAtTime"));
}

}  // namespace

// ---- mutation batches ------------------------------------------------------------

struct Store::Mutations {
  Arena* arena;
  uint8_t* scratch;
  storage::KvMutation* items;
  size_t count = 0;
  size_t capacity = 0;
  bool ok = true;

  Mutations(Arena& a, uint8_t* s, size_t cap) : arena(&a), scratch(s), capacity(cap) {
    items = static_cast<storage::KvMutation*>(a.Alloc(sizeof(storage::KvMutation) * cap));
    ok = items != nullptr;
  }
  storage::KvMutation* Next() {
    if (!ok || count == capacity) { ok = false; return nullptr; }
    return &items[count++];
  }
  void PutText(const char* key, const char* text, size_t length, int64_t expires) {
    storage::KvMutation* m = Next();
    if (!m) return;
    char* k = arena->CopyText(key, strlen(key));
    char* v = arena->CopyText(text, length);
    if (!k || !v) { ok = false; return; }
    *m = storage::KvMutation{k, v, static_cast<uint32_t>(length), expires, false};
  }
  void Put(const char* key, const Value* value, int64_t expires = 0) {
    size_t length = 0;
    if (!ok || !stringify(value, scratch, kScratchBytes, &length)) { ok = false; return; }
    PutText(key, reinterpret_cast<const char*>(scratch), length, expires);
  }
  void PutString(const char* key, const char* text, int64_t expires = 0) {
    Value* v = tj::Str(*arena, text);
    if (!v) { ok = false; return; }
    Put(key, v, expires);
  }
  void Erase(const char* key) {
    storage::KvMutation* m = Next();
    if (!m) return;
    char* k = arena->CopyText(key, strlen(key));
    if (!k) { ok = false; return; }
    *m = storage::KvMutation{k, nullptr, 0, 0, true};
  }
};

Store::Store(storage::KvStore* kv) : kv_(kv) {

  recent_topics_ = base::AllocateZeroedStorage<RecentTopic[]>(kRecentTopics, sizeof(RecentTopic));
}

Store::~Store() {
  recent_topics_.reset();

}

bool Store::Component(const char* text, char* output, size_t capacity) { return component(text, output, capacity); }

void Store::Pad13(int64_t ms, char output[24]) {
  char text[24];
  snprintf(text, sizeof(text), "%lld", static_cast<long long>(ms));
  const size_t length = strlen(text);
  size_t used = 0;
  for (size_t i = length; i < 13; ++i) output[used++] = '0';
  memcpy(output + used, text, length + 1);
}

ReadStatus Store::GetJson(const char* key, int64_t now_ms, Arena& arena, Value** value) const {
  size_t size = 0;
  // Probe the KV index before reserving arena memory. A transfer page can
  // read 501 rows, so a fixed 64 KiB buffer per row exhausts the 8 MiB
  // foreground arena even when the serialized rows are small.
  const storage::KvReadStatus probe = kv_->GetStatus(key, now_ms, nullptr, 0, &size);
  if (probe == storage::KvReadStatus::Missing) return ReadStatus::kMissing;
  if (probe != storage::KvReadStatus::BufferTooSmall || !size) return ReadStatus::kError;
  size_t capacity = size;
  for (int round = 0; round < 3; ++round) {
    const tj::Mark mark = tj::MarkOf(arena);
    char* buffer = static_cast<char*>(arena.Alloc(capacity + 1, 1));
    if (!buffer) return ReadStatus::kError;
    const storage::KvReadStatus status = kv_->GetStatus(key, now_ms, buffer, capacity, &size);
    if (status == storage::KvReadStatus::Missing) { tj::Rewind(arena, mark); return ReadStatus::kMissing; }
    if (status == storage::KvReadStatus::BufferTooSmall) {
      tj::Rewind(arena, mark);
      capacity = size;
      continue;
    }
    if (status != storage::KvReadStatus::Found) return ReadStatus::kError;
    Value* parsed = tj::Parse(arena, reinterpret_cast<const uint8_t*>(buffer), size);
    if (!parsed) return ReadStatus::kError;
    *value = parsed;
    return ReadStatus::kFound;
  }
  return ReadStatus::kError;
}

// ---- accounts ----------------------------------------------------------------------

bool Store::Touch(const char* network, const char* address, int64_t now_ms, Account* out) {
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  auto memory_storage = base::AllocateStorage<uint8_t[]>(256 * 1024);
  auto* memory = memory_storage.get();
  if (!scratch || !memory) { scratch_storage.reset(); memory_storage.reset(); return false; }
  Arena arena{memory, 256 * 1024, 0, true};
  Key pair;
  pair.Raw("account/pair/");
  pair.Part(network);
  pair.Raw("/");
  pair.Part(address);
  bool ok = pair.ok;
  fortis::base::UniqueLock lock(mutex_);
  Value* id_value = nullptr;
  Value* old = nullptr;
  ReadStatus id_status = ok ? GetJson(pair.text, now_ms, arena, &id_value) : ReadStatus::kError;
  ok = id_status != ReadStatus::kError;
  if (ok && id_status == ReadStatus::kFound && tj::IsString(id_value)) {
    Key by_id;
    by_id.Raw("account/id/");
    by_id.Part(id_value->text);
    if (!by_id.ok || GetJson(by_id.text, now_ms, arena, &old) == ReadStatus::kError) ok = false;
  }
  Account account;
  if (ok && old && fill_account(old, &account) && now_ms - account.last_read_at < kReadRefreshMs) {
    *out = account;
    lock.Unlock();
    scratch_storage.reset();
    memory_storage.reset();
    return true;
  }
  if (ok) {
    Mutations batch(arena, scratch, 8);
    Value* row = nullptr;
    if (old && fill_account(old, &account)) {
      Key recent;
      recent.Raw("account/recent/");
      recent.Pad(account.last_read_at);
      recent.Raw("/");
      recent.Part(account.id);
      if (recent.ok) batch.Erase(recent.text);
      row = tj::Clone(arena, old);
      ok = row && tj::Set(arena, row, "lastReadAt", tj::Num(arena, static_cast<double>(now_ms)));
      account.last_read_at = now_ms;
    } else {
      uint8_t random[10];
      char id[codec::crash_report::kUlidLength + 1];
      ok = auth::SecureRandomBytes(random, sizeof(random)) && codec::crash_report::NewUlid(now_ms, random, id) &&
           strlen(network) < sizeof(account.network) && strlen(address) < sizeof(account.address);
      if (ok) {
        account = Account{};
        snprintf(account.id, sizeof(account.id), "%s", id);
        snprintf(account.network, sizeof(account.network), "%s", network);
        snprintf(account.address, sizeof(account.address), "%s", address);
        account.first_read_at = account.last_read_at = now_ms;
        row = tj::Obj(arena);
        ok = row && tj::Set(arena, row, "id", tj::Str(arena, id)) &&
             tj::Set(arena, row, "network", tj::Str(arena, network)) &&
             tj::Set(arena, row, "address", tj::Str(arena, address)) &&
             tj::Set(arena, row, "firstReadAt", tj::Num(arena, static_cast<double>(now_ms))) &&
             tj::Set(arena, row, "lastReadAt", tj::Num(arena, static_cast<double>(now_ms)));
      }
    }
    Key by_id, recent;
    by_id.Raw("account/id/");
    by_id.Part(account.id);
    recent.Raw("account/recent/");
    recent.Pad(now_ms);
    recent.Raw("/");
    recent.Part(account.id);
    ok = ok && by_id.ok && recent.ok;
    if (ok) {
      batch.Put(by_id.text, row);
      batch.PutString(pair.text, account.id);
      batch.PutString(recent.text, account.id);
      ok = batch.ok && kv_->Write(batch.items, batch.count);
    }
    if (ok) *out = account;
  }
  lock.Unlock();
  scratch_storage.reset();
  memory_storage.reset();
  return ok;
}

ReadStatus Store::ById(const char* id, int64_t now_ms, Account* out) const {
  constexpr size_t capacity = 86 * 1024;
  auto memory = base::AllocateStorage<uint8_t[]>(capacity);
  if (!memory) return ReadStatus::kError;
  Arena arena{memory.get(), capacity, 0, true};
  Key key;
  key.Raw("account/id/");
  key.Part(id);
  if (!key.ok) return ReadStatus::kError;
  Value* row = nullptr;
  const ReadStatus status = GetJson(key.text, now_ms, arena, &row);
  if (status != ReadStatus::kFound) return status;
  return fill_account(row, out) ? ReadStatus::kFound : ReadStatus::kError;
}

ReadStatus Store::Find(const char* network, const char* address, int64_t now_ms, Account* out) const {
  constexpr size_t capacity = 72 * 1024;
  auto memory = base::AllocateStorage<uint8_t[]>(capacity);
  if (!memory) return ReadStatus::kError;
  Arena arena{memory.get(), capacity, 0, true};
  Key pair;
  pair.Raw("account/pair/");
  pair.Part(network);
  pair.Raw("/");
  pair.Part(address);
  if (!pair.ok) return ReadStatus::kError;
  Value* id = nullptr;
  const ReadStatus status = GetJson(pair.text, now_ms, arena, &id);
  if (status != ReadStatus::kFound) return status;
  if (!tj::IsString(id)) return ReadStatus::kError;
  char copy[kAccountIdBytes * 3 + 1];
  snprintf(copy, sizeof(copy), "%s", id->text);
  return ById(copy, now_ms, out);
}

bool Store::RecentlyRead(int64_t since_ms, size_t limit, int64_t now_ms, Account* out, size_t capacity,
                         size_t* count) const {
  *count = 0;
  if (!limit) return true;
  auto memory_storage = base::AllocateStorage<uint8_t[]>(1024 * 1024);
  auto* memory = memory_storage.get();
  if (!memory) return false;
  const char* prefix = "account/recent/";
  const size_t prefix_length = strlen(prefix);
  char after[kKeyBytes] = {};
  bool has_after = false;
  bool ok = true;
  for (;;) {
    Arena arena{memory, 1024 * 1024, 0, true};
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, prefix, has_after ? after : nullptr, true, now_ms, arena, &entries, &result)) { ok = false; break; }
    bool stop = false;
    for (size_t i = 0; i < entries.count && !stop; ++i) {
      if (key_ms(entries.entries[i].key, prefix_length) < since_ms) { stop = true; break; }
      Value* id = parse_entry(arena, entries.entries[i]);
      Account account;
      if (tj::IsString(id) && ById(id->text, now_ms, &account) == ReadStatus::kFound) {
        if (*count < capacity) out[(*count)++] = account;
        if (*count >= limit || *count >= capacity) stop = true;
      }
    }
    if (stop || result.done || !result.next_cursor[0]) break;
    snprintf(after, sizeof(after), "%s", result.next_cursor);
    has_after = true;
  }
  memory_storage.reset();
  return ok;
}

// ---- balances -------------------------------------------------------------------

ReadStatus Store::GetBalance(const char* account_id, int64_t now_ms, Arena& arena, Value** payload,
                             int64_t* as_of_ms) const {
  Key key;
  key.Raw("balance/account/");
  key.Part(account_id);
  if (!key.ok) return ReadStatus::kError;
  Value* snapshot = nullptr;
  const ReadStatus status = GetJson(key.text, now_ms, arena, &snapshot);
  if (status != ReadStatus::kFound) return status;
  Value* body = tj::Get(snapshot, "payload");
  if (!tj::IsObject(body)) return ReadStatus::kError;
  *payload = body;
  *as_of_ms = RowMs(snapshot, "asOf");
  return ReadStatus::kFound;
}

bool Store::PutBalance(const char* account_id, const Value* incoming, const char* const* requested,
                       size_t requested_count, int64_t as_of_ms, Arena& arena) {
  Key key;
  key.Raw("balance/account/");
  key.Part(account_id);
  if (!key.ok) return false;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  fortis::base::UniqueLock lock(mutex_);
  Value* previous_payload = nullptr;
  int64_t previous_as_of = 0;
  const ReadStatus status = GetBalance(account_id, as_of_ms, arena, &previous_payload, &previous_as_of);
  bool ok = status != ReadStatus::kError;
  // new Map(previous tokens by id); requested-but-omitted ids deleted; the
  // incoming entries set (an existing key keeps its position).
  Value* tokens = tj::Obj(arena);
  ok = ok && tokens;
  const Value* old_tokens = status == ReadStatus::kFound ? tj::Get(previous_payload, "tokens") : nullptr;
  if (ok && tj::IsArray(old_tokens)) {
    for (uint32_t i = 0; i < old_tokens->count && ok; ++i) {
      const char* id = string_member(old_tokens->items[i], "tokenId");
      if (id) ok = tj::Set(arena, tokens, id, old_tokens->items[i]);
    }
  }
  const Value* new_tokens = tj::Get(incoming, "tokens");
  for (size_t r = 0; ok && r < requested_count; ++r) {
    bool returned = false;
    for (uint32_t i = 0; tj::IsArray(new_tokens) && i < new_tokens->count; ++i) {
      const char* id = string_member(new_tokens->items[i], "tokenId");
      if (id && !strcmp(id, requested[r])) { returned = true; break; }
    }
    if (!returned) tj::Remove(tokens, requested[r]);
  }
  for (uint32_t i = 0; ok && tj::IsArray(new_tokens) && i < new_tokens->count; ++i) {
    const char* id = string_member(new_tokens->items[i], "tokenId");
    if (id) ok = tj::Set(arena, tokens, id, new_tokens->items[i]);
  }
  Value* payload = ok ? tj::Clone(arena, incoming) : nullptr;
  Value* merged = payload ? tj::Arr(arena) : nullptr;
  ok = ok && merged;
  for (uint32_t i = 0; ok && i < tokens->count; ++i) ok = tj::Push(arena, merged, tokens->members[i].value);
  ok = ok && tj::Set(arena, payload, "tokens", merged);
  Value* snapshot = ok ? tj::Obj(arena) : nullptr;
  ok = ok && snapshot && tj::Set(arena, snapshot, "payload", payload) &&
       tj::Set(arena, snapshot, "asOf", tj::Num(arena, static_cast<double>(as_of_ms)));
  if (ok) {
    Mutations batch(arena, scratch, 1);
    batch.Put(key.text, snapshot);
    ok = batch.ok && kv_->Write(batch.items, batch.count);
  }
  lock.Unlock();
  scratch_storage.reset();
  return ok;
}

// ---- transfer sync state -----------------------------------------------------------

ReadStatus Store::GetSyncState(const char* account_id, int64_t now_ms, Arena& arena, Value** state) const {
  Key key;
  key.Raw("transfer/sync/");
  key.Part(account_id);
  if (!key.ok) return ReadStatus::kError;
  return GetJson(key.text, now_ms, arena, state);
}

bool Store::PatchSyncState(const char* account_id, const Value* fields, int64_t now_ms, Arena& arena) {
  Key key;
  key.Raw("transfer/sync/");
  key.Part(account_id);
  if (!key.ok) return false;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  fortis::base::UniqueLock lock(mutex_);
  Value* current = nullptr;
  const ReadStatus status = GetJson(key.text, now_ms, arena, &current);
  bool ok = status != ReadStatus::kError;
  Value* next = nullptr;
  if (ok && status == ReadStatus::kFound && tj::IsObject(current)) {
    next = current;
  } else if (ok) {
    next = tj::Obj(arena);
    ok = next && tj::Set(arena, next, "accountId", tj::Str(arena, account_id)) &&
         tj::Set(arena, next, "upstreamCursors", tj::Obj(arena)) &&
         tj::Set(arena, next, "backfillComplete", tj::Bool(arena, false)) &&
         tj::Set(arena, next, "backfillDepth", tj::Num(arena, 500)) &&
         tj::Set(arena, next, "lastSyncAt", tj::Null(arena)) && tj::Set(arena, next, "lastError", tj::Null(arena)) &&
         tj::Set(arena, next, "lastErrorAt", tj::Null(arena));
  }
  for (uint32_t i = 0; ok && tj::IsObject(fields) && i < fields->count; ++i)
    ok = tj::Set(arena, next, fields->members[i].key, fields->members[i].key_length, fields->members[i].value);
  if (ok) {
    Mutations batch(arena, scratch, 1);
    batch.Put(key.text, next);
    ok = batch.ok && kv_->Write(batch.items, batch.count);
  }
  lock.Unlock();
  scratch_storage.reset();
  return ok;
}

// ---- transfers ----------------------------------------------------------------------

namespace {

bool row_key(Key* key, const char* account, const char* record) {
  key->Raw("transfer/row/");
  key->Part(account);
  key->Raw("/");
  key->Part(record);
  return key->ok;
}

bool cursor_key(Key* key, const Value* row) {
  key->Raw("transfer/cursor/");
  key->Part(string_member(row, "accountId"));
  key->Raw("/");
  const char* cursor = string_member(row, "cursor");
  key->Raw(cursor ? cursor : "");
  return key->ok;
}

bool time_key(Key* key, const char* root, const Value* row) {
  key->Raw(root);
  key->Part(string_member(row, "accountId"));
  key->Raw("/");
  key->Pad(RowMs(row, "ts"));
  key->Raw("/");
  key->Part(string_member(row, "recordId"));
  return key->ok;
}

bool age_key(Key* key, const char* root, const Value* row) {
  key->Raw(root);
  key->Pad(RowMs(row, "ts"));
  key->Raw("/");
  key->Part(string_member(row, "accountId"));
  key->Raw("/");
  key->Part(string_member(row, "recordId"));
  return key->ok;
}

bool unpriced_key(Key* key, const Value* row) {
  key->Raw("transfer/unpriced/");
  key->Pad(RowMs(row, "fiatAttemptedAt"));
  key->Raw("/");
  key->Part(string_member(row, "accountId"));
  key->Raw("/");
  key->Part(string_member(row, "recordId"));
  return key->ok;
}

// tokenTimeKeys(row) under `root` (token-time or token-unpriced-time).
size_t token_time_keys(Key keys[2], const char* root, const Value* row) {
  const char* token = string_member(row, "tokenId");
  const char* to_token = string_member(row, "toTokenId");
  const char* tokens[2] = {token ? token : "", nullptr};
  size_t n = 1;
  if (to_token && token && strcmp(to_token, token) != 0) tokens[n++] = to_token;
  else if (to_token && !token) tokens[n++] = to_token;
  for (size_t i = 0; i < n; ++i) {
    keys[i] = Key{};
    keys[i].Raw(root);
    keys[i].Part(string_member(row, "accountId"));
    keys[i].Raw("/");
    keys[i].Part(tokens[i]);
    keys[i].Raw("/");
    keys[i].Pad(RowMs(row, "ts"));
    keys[i].Raw("/");
    keys[i].Part(string_member(row, "recordId"));
  }
  return n;
}

Value* demand_ref(Arena& arena, const Value* row) {
  Value* ref = tj::Obj(arena);
  auto copy = [&](const char* key) {
    const Value* v = tj::Get(row, key);
    return tj::Set(arena, ref, key, v ? tj::Clone(arena, v) : tj::Null(arena));
  };
  if (!ref || !copy("accountId") || !copy("recordId") || !copy("tokenId") || !copy("toTokenId") || !copy("feeRaw"))
    return nullptr;
  return ref;
}

Value* retention_ref(Arena& arena, const Value* row) {
  Value* ref = tj::Obj(arena);
  if (!ref || !tj::Set(arena, ref, "accountId", tj::Clone(arena, tj::Get(row, "accountId"))) ||
      !tj::Set(arena, ref, "recordId", tj::Clone(arena, tj::Get(row, "recordId"))))
    return nullptr;
  return ref;
}

// The in-commit overlay of rows written earlier in the same chunk (tx.get
// sees a transaction's own writes).
struct Overlay {
  const char* record_id;
  Value* row;
};

}  // namespace

ReadStatus Store::GetTransfer(const char* account_id, const char* record_id, int64_t now_ms, Arena& arena,
                              Value** row) const {
  Key key;
  if (!row_key(&key, account_id, record_id)) return ReadStatus::kError;
  return GetJson(key.text, now_ms, arena, row);
}

bool Store::UpsertTransfers(const char* account_id, const Network& network, const Value* records, int64_t now_ms,
                            Arena& arena, size_t* changed, bool* invalid_time) {
  (void)arena;
  *changed = 0;
  bool invalid = false;
  if (invalid_time) *invalid_time = false;
  if (!tj::IsArray(records)) return false;
  for (size_t offset = 0; offset < records->count; offset += kUpsertChunk) {
    const size_t end = offset + kUpsertChunk < records->count ? offset + kUpsertChunk : records->count;
    Arena chunk = tj::GrowingArena(kChunkArenaBytes);
  std::unique_ptr<Arena, tj::ArenaBlocksDeleter> chunk_storage(&chunk);
    const bool ok = UpsertChunk(account_id, network, records, offset, end, now_ms, chunk, changed, &invalid);
    tj::FreeArena(chunk);
    if (!ok) {
      if (invalid_time) *invalid_time = invalid;
      return false;
    }
  }
  return true;
}

bool Store::UpsertChunk(const char* account_id, const Network& network, const Value* records, size_t begin,
                        size_t end, int64_t now_ms, Arena& arena, size_t* changed, bool* invalid_time) {
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  fortis::base::UniqueLock lock(mutex_);
  // The sync journal head (sync2/meta/head), advanced per appended event.
  chains::BigInt head;
  bool ok = read_head(*this, now_ms, arena, &head);
  const size_t span = end - begin;
  Overlay* overlay = static_cast<Overlay*>(arena.Alloc(sizeof(Overlay) * span));
  size_t overlay_count = 0;
  Mutations batch(arena, scratch, span * 14 + 1);
  ok = ok && overlay && batch.ok;
  size_t count = 0;
  for (size_t i = begin; ok && i < end; ++i) {
    const Value* record = records->items[i];
    const char* record_id = string_member(record, "recordId");
    if (!record_id) { ok = false; break; }
    Key key;
    if (!row_key(&key, account_id, record_id)) { ok = false; break; }
    Value* old = nullptr;
    for (size_t o = overlay_count; o-- > 0;)
      if (!strcmp(overlay[o].record_id, record_id)) { old = overlay[o].row; break; }
    if (!old) {
      const ReadStatus status = GetJson(key.text, now_ms, arena, &old);
      if (status == ReadStatus::kError) { ok = false; break; }
      if (status == ReadStatus::kMissing) old = nullptr;
    }
    Value* input = InitialRow(arena, account_id, record, now_ms);
    const Value* restatable = tj::Get(record, "restatable");
    Value* candidate = !input ? nullptr
                       : old ? MergeRow(arena, old, input, restatable && restatable->type == tj::Type::kTrue, now_ms)
                             : input;
    if (!candidate) { ok = false; break; }
    // A merge keeps the stored ts; a new row's ts must be a valid Date or
    // toISOString throws RangeError and the whole commit rolls back.
    const Value* ts = tj::Get(candidate, "ts");
    if (!tj::IsNumber(ts) || !(fabs(ts->number) <= 8.64e15)) {
      *invalid_time = true;
      ok = false;
      break;
    }
    Value* current = TransferAppJson(arena, candidate, network, false, 0);
    if (!current) { ok = false; break; }
    if (old) {
      const tj::Mark comparison_mark = tj::MarkOf(arena);
      Value* previous = TransferAppJson(arena, old, network, false, 0);
      size_t a = 0, b = 0;
      uint8_t* left = static_cast<uint8_t*>(arena.Alloc(256 * 1024, 1));
      uint8_t* right = static_cast<uint8_t*>(arena.Alloc(256 * 1024, 1));
      if (!previous || !left || !right || !stringify(previous, left, 256 * 1024, &a) ||
          !stringify(current, right, 256 * 1024, &b)) {
        ok = false;
        break;
      }
      const bool same = a == b && memcmp(left, right, a) == 0;
      tj::Rewind(arena, comparison_mark);
      if (same) continue;
    }
    // appendInTransaction: the next padded sequence and the event row.
    Value* entity = tj::Obj(arena);
    char sequence[32];
    ok = entity && tj::Set(arena, entity, "id", tj::Str(arena, record_id)) &&
         QueueSyncEvent(batch, arena, head, account_id, entity, old ? "patch" : "upsert", current,
                        static_cast<double>(now_ms), now_ms, sequence);
    if (!ok) break;
    Value* row = candidate;
    ok = tj::Set(arena, row, "cursor", tj::Str(arena, sequence));
    if (!ok) break;
    if (old) {
      Key old_cursor;
      if (cursor_key(&old_cursor, old)) batch.Erase(old_cursor.text); else ok = false;
      if (is_unpriced(old)) {
        Key unpriced;
        if (unpriced_key(&unpriced, old)) batch.Erase(unpriced.text); else ok = false;
        Key keys[2];
        const size_t n = token_time_keys(keys, "transfer/token-unpriced-time/", old);
        for (size_t k = 0; k < n; ++k) if (keys[k].ok) batch.Erase(keys[k].text); else ok = false;
      }
      if (!chains::Equals(tj::Get(old, "status"), "pending")) {
        Key retention;
        if (age_key(&retention, "transfer/retention/", old)) batch.Erase(retention.text); else ok = false;
      }
      Key keys[2];
      const size_t n = token_time_keys(keys, "transfer/token-time/", old);
      for (size_t k = 0; k < n; ++k) if (keys[k].ok) batch.Erase(keys[k].text); else ok = false;
      if (RowMs(old, "ts") != RowMs(row, "ts")) {
        Key old_time, old_age;
        if (time_key(&old_time, "transfer/time/", old)) batch.Erase(old_time.text); else ok = false;
        if (age_key(&old_age, "transfer/age/", old)) batch.Erase(old_age.text); else ok = false;
      }
    }
    Key new_cursor, new_time, new_age;
    ok = ok && cursor_key(&new_cursor, row) && time_key(&new_time, "transfer/time/", row) &&
         age_key(&new_age, "transfer/age/", row);
    if (!ok) break;
    batch.Put(key.text, row);
    batch.PutString(new_cursor.text, record_id);
    batch.PutString(new_time.text, record_id);
    Key token_keys[2];
    const size_t n = token_time_keys(token_keys, "transfer/token-time/", row);
    for (size_t k = 0; k < n; ++k) if (token_keys[k].ok) batch.PutString(token_keys[k].text, record_id); else ok = false;
    batch.Put(new_age.text, demand_ref(arena, row));
    if (!chains::Equals(tj::Get(row, "status"), "pending")) {
      Key retention;
      if (age_key(&retention, "transfer/retention/", row)) batch.Put(retention.text, retention_ref(arena, row));
      else ok = false;
    }
    if (is_unpriced(row)) {
      Key unpriced;
      if (unpriced_key(&unpriced, row)) batch.Put(unpriced.text, demand_ref(arena, row)); else ok = false;
      Key keys[2];
      const size_t m = token_time_keys(keys, "transfer/token-unpriced-time/", row);
      for (size_t k = 0; k < m; ++k) if (keys[k].ok) batch.PutString(keys[k].text, record_id); else ok = false;
    }
    overlay[overlay_count++] = Overlay{arena.CopyText(record_id, strlen(record_id)), row};
    ++count;
    ok = ok && batch.ok;
  }
  if (ok && count) {
    char sequence[32];
    padded_sequence(head, sequence);
    batch.PutString("sync2/meta/head", sequence);
    ok = batch.ok && kv_->Write(batch.items, batch.count);
  }
  lock.Unlock();
  scratch_storage.reset();
  if (ok) *changed += count;
  return ok;
}

bool Store::QueueSyncEvent(Mutations& batch, Arena& arena, chains::BigInt& head, const char* account_id,
                           const Value* entity_key, const char* operation, Value* fields, double changed_at_ms,
                           int64_t now_ms, char sequence[32]) {
  if (!head.AddSmall(1)) return false;
  padded_sequence(head, sequence);
  Value* event = tj::Obj(arena);
  const bool ok = event && tj::Set(arena, event, "model", tj::Str(arena, "transfer")) &&
                  tj::Set(arena, event, "entityKey", tj::Clone(arena, entity_key)) &&
                  tj::Set(arena, event, "operation", tj::Str(arena, operation)) &&
                  tj::Set(arena, event, "fields", fields) &&
                  tj::Set(arena, event, "changedAt", tj::Num(arena, changed_at_ms)) &&
                  tj::Set(arena, event, "accountId", tj::Str(arena, account_id)) &&
                  tj::Set(arena, event, "sequence", tj::Str(arena, sequence)) &&
                  tj::Set(arena, event, "committedAt", tj::Num(arena, static_cast<double>(now_ms)));
  Key event_key;
  event_key.Raw("sync2/account/");
  event_key.Part(account_id);
  event_key.Raw("/");
  event_key.Raw(sequence);
  if (!ok || !event_key.ok) return false;
  batch.Put(event_key.text, event, now_ms + kSyncRetainMs);
  return batch.ok;
}

bool Store::AppendSyncEvents(const char* account_id, const Value* events, int64_t now_ms, bool commit, Arena& arena,
                             char head_out[32]) {
  if (!account_id || !tj::IsArray(events)) return false;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  fortis::base::UniqueLock lock(mutex_);
  chains::BigInt head;
  bool ok = read_head(*this, now_ms, arena, &head);
  Mutations batch(arena, scratch, events->count + 1);
  ok = ok && batch.ok;
  for (uint32_t i = 0; ok && i < events->count; ++i) {
    const Value* event = events->items[i];
    const Value* entity = tj::Get(event, "entityKey");
    const char* operation = string_member(event, "operation");
    const Value* fields = tj::Get(event, "fields");
    const Value* changed_at = tj::Get(event, "changedAt");
    char sequence[32];
    ok = tj::IsObject(entity) && operation && tj::IsObject(fields) && tj::IsNumber(changed_at) &&
         QueueSyncEvent(batch, arena, head, account_id, entity, operation, tj::Clone(arena, fields),
                        changed_at->number, now_ms, sequence);
  }
  if (ok && commit && events->count) {
    char sequence[32];
    padded_sequence(head, sequence);
    batch.PutString("sync2/meta/head", sequence);
    ok = batch.ok && kv_->Write(batch.items, batch.count);
  }
  chains::BigInt after;
  if (!read_head(*this, now_ms, arena, &after)) ok = false;
  padded_sequence(after, head_out);
  lock.Unlock();
  scratch_storage.reset();
  return ok;
}

bool Store::PageTransfers(const char* account_id, bool by_cursor, size_t limit, const char* after,
                          const PageOptions& options, RowMatcher matches, int64_t now_ms,
                          Arena& arena, Value** rows) const {
  if (!limit || limit > 501) return false;
  Key root;
  if (!by_cursor && options.token_id) {
    root.Raw("transfer/token-time/");
    root.Part(account_id);
    root.Raw("/");
    root.Part(options.token_id);
    root.Raw("/");
  } else {
    root.Raw(by_cursor ? "transfer/cursor/" : "transfer/time/");
    root.Part(account_id);
    root.Raw("/");
  }
  if (!root.ok) return false;
  const size_t root_length = strlen(root.text);
  // A crafted display cursor can compare beyond the stored key limit. Keep
  // the entire boundary: cutting it to a stored-key prefix would skip an
  // equal prefix during a reverse scan.
  char position[kPositionBytes] = {};
  bool has_position = false;
  if (after) {
    const size_t after_length = strlen(after);
    if (root_length + after_length >= sizeof(position)) return false;
    memcpy(position, root.text, root_length);
    memcpy(position + root_length, after, after_length + 1);
    has_position = true;
  }
  if (!by_cursor && options.has_to) {
    Key boundary;
    boundary.Raw(root.text);
    boundary.Pad(static_cast<int64_t>(options.to_ms + 1));
    if (!boundary.ok) return false;
    if (!has_position || strcmp(position, boundary.text) > 0) {
      snprintf(position, sizeof(position), "%s", boundary.text);
      has_position = true;
    }
  }
  Value* out = tj::Arr(arena);
  if (!out) return false;
  const bool reverse = !by_cursor;
  for (;;) {
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, root.text, has_position ? position : nullptr, reverse, now_ms, arena, &entries, &result))
      return false;
    for (size_t i = 0; i < entries.count; ++i) {
      if (!by_cursor && options.has_from &&
          static_cast<double>(key_ms(entries.entries[i].key, root_length)) < options.from_ms) {
        *rows = out;
        return true;
      }
      Value* record = parse_entry(arena, entries.entries[i]);
      if (!tj::IsString(record)) continue;
      Value* row = nullptr;
      const ReadStatus status = GetTransfer(account_id, record->text, now_ms, arena, &row);
      if (status == ReadStatus::kError) return false;
      if (status == ReadStatus::kFound && (!matches || matches(row)) && !tj::Push(arena, out, row))
        return false;
      if (out->count >= limit) {
        *rows = out;
        return true;
      }
    }
    if (result.done || !result.next_cursor[0]) break;
    snprintf(position, sizeof(position), "%s", result.next_cursor);
    has_position = true;
  }
  *rows = out;
  return true;
}

namespace {
struct LastKey {
  char key[kKeyBytes];
  bool found;
};
bool last_key(const char* key, const void*, size_t, LastKey* last) {
  snprintf(last->key, sizeof(last->key), "%s", key);
  last->found = true;
  return true;  // the scan limit of 1 stops it (a false return reads as a failure)
}
}  // namespace

base::Result<std::optional<TransferHighWater>> Store::HighWater(const char* account_id, int64_t now_ms) const {
  Key prefix;
  prefix.Raw("transfer/cursor/");
  prefix.Part(account_id);
  prefix.Raw("/");
  if (!prefix.ok) return base::Status(base::StatusCode::kInvalidArgument, "invalid transfer high-water key");
  LastKey last{};
  if (!kv_->Scan(prefix.text, nullptr, 1, now_ms, [&last](const char* key, const void* value, size_t size) {
        return last_key(key, value, size, &last);
      }, true))
    return base::Status(base::StatusCode::kUnavailable, "transfer high-water scan failed");
  if (!last.found) return std::optional<TransferHighWater>{};
  TransferHighWater value;
  snprintf(value.cursor, sizeof(value.cursor), "%s", last.key + strlen(prefix.text));
  return std::optional<TransferHighWater>{value};
}

bool Store::RecentTransfers(const char* account_id, size_t limit, int64_t now_ms, Arena& arena, Value** rows) const {
  Key prefix;
  prefix.Raw("transfer/time/");
  prefix.Part(account_id);
  prefix.Raw("/");
  if (!prefix.ok) return false;
  Value* out = tj::Arr(arena);
  if (!out) return false;
  char position[kKeyBytes] = {};
  bool has_position = false;
  while (out->count < limit) {
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, prefix.text, has_position ? position : nullptr, true, now_ms, arena, &entries, &result))
      return false;
    for (size_t i = 0; i < entries.count && out->count < limit; ++i) {
      Value* record = parse_entry(arena, entries.entries[i]);
      if (!tj::IsString(record)) continue;
      Value* row = nullptr;
      if (GetTransfer(account_id, record->text, now_ms, arena, &row) == ReadStatus::kFound &&
          !tj::Push(arena, out, row))
        return false;
    }
    if (result.done || !result.next_cursor[0]) break;
    snprintf(position, sizeof(position), "%s", result.next_cursor);
    has_position = true;
  }
  *rows = out;
  return true;
}

bool Store::UnpricedForToken(const char* account_id, const char* token_id, size_t limit, int64_t due_before_ms,
                             RowMatcher matches, int64_t now_ms, Arena& arena, Value** rows) const {
  Key root;
  root.Raw("transfer/token-unpriced-time/");
  root.Part(account_id);
  root.Raw("/");
  root.Part(token_id);
  root.Raw("/");
  if (!root.ok) return false;
  Value* out = tj::Arr(arena);
  if (!out) return false;
  char position[kKeyBytes] = {};
  bool has_position = false;
  for (;;) {
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, root.text, has_position ? position : nullptr, true, now_ms, arena, &entries, &result))
      return false;
    for (size_t i = 0; i < entries.count; ++i) {
      Value* record = parse_entry(arena, entries.entries[i]);
      if (!tj::IsString(record)) continue;
      Value* row = nullptr;
      if (GetTransfer(account_id, record->text, now_ms, arena, &row) != ReadStatus::kFound) continue;
      const Value* attempted = tj::Get(row, "fiatAttemptedAt");
      if (is_unpriced(row) && (is_null(attempted) || RowMs(row, "fiatAttemptedAt") <= due_before_ms) &&
          (!matches || matches(row)) && !tj::Push(arena, out, row))
        return false;
      if (out->count >= limit) { *rows = out; return true; }
    }
    if (result.done || !result.next_cursor[0]) break;
    snprintf(position, sizeof(position), "%s", result.next_cursor);
    has_position = true;
  }
  *rows = out;
  return true;
}

bool Store::ForEachUnpriced(RowVisitor visit, bool has_due_before, int64_t due_before_ms,
                            int64_t now_ms, Arena& arena) const {
  const char* root = "transfer/unpriced/";
  const size_t root_length = strlen(root);
  char position[kKeyBytes] = {};
  bool has_position = false;
  for (;;) {
    const tj::Mark mark = tj::MarkOf(arena);
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, root, has_position ? position : nullptr, false, now_ms, arena, &entries, &result)) return false;
    for (size_t i = 0; i < entries.count; ++i) {
      if (has_due_before && key_ms(entries.entries[i].key, root_length) > due_before_ms) {
        tj::Rewind(arena, mark);
        return true;
      }
      Value* ref = parse_entry(arena, entries.entries[i]);
      const char* account = string_member(ref, "accountId");
      const char* record = string_member(ref, "recordId");
      if (!account || !record) continue;
      Value* row = nullptr;
      if (GetTransfer(account, record, now_ms, arena, &row) == ReadStatus::kFound && is_unpriced(row) &&
          !visit(row, arena)) {
        tj::Rewind(arena, mark);
        return true;
      }
    }
    if (!result.done && result.next_cursor[0]) snprintf(position, sizeof(position), "%s", result.next_cursor);
    const bool more = !result.done && result.next_cursor[0];
    tj::Rewind(arena, mark);
    if (!more) break;
    has_position = true;
  }
  return true;
}

bool Store::RecordValuations(const ValuationTarget* targets, size_t count, int64_t at_ms, Arena& arena,
                             size_t* changed) {
  (void)arena;
  *changed = 0;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  bool ok = true;
  Arena chunk = tj::GrowingArena(kChunkArenaBytes);
  std::unique_ptr<Arena, tj::ArenaBlocksDeleter> chunk_storage(&chunk);
  for (size_t offset = 0; ok && offset < count; offset += kValuationChunk) {
    const size_t end = offset + kValuationChunk < count ? offset + kValuationChunk : count;
    tj::FreeArena(chunk);
    chunk = tj::GrowingArena(kChunkArenaBytes);
    fortis::base::UniqueLock lock(mutex_);
    Mutations batch(chunk, scratch, (end - offset) * 8 + 1);
    size_t written = 0;
    // Rows rewritten earlier in this commit (one transaction).
    Overlay* overlay = static_cast<Overlay*>(chunk.Alloc(sizeof(Overlay) * (end - offset)));
    size_t overlay_count = 0;
    ok = batch.ok && overlay;
    for (size_t i = offset; ok && i < end; ++i) {
      const ValuationTarget& t = targets[i];
      Key key;
      if (!row_key(&key, t.account_id, t.record_id)) { ok = false; break; }
      Value* old = nullptr;
      for (size_t o = overlay_count; o-- > 0;)
        if (!strcmp(overlay[o].record_id, key.text)) { old = overlay[o].row; break; }
      if (!old) {
        const ReadStatus status = GetJson(key.text, at_ms, chunk, &old);
        if (status == ReadStatus::kError) { ok = false; break; }
        if (status == ReadStatus::kMissing) continue;
      }
      Value* next = tj::Clone(chunk, old);
      if (!next) { ok = false; break; }
      ok = tj::Set(chunk, next, "fiatAttemptedAt", tj::Num(chunk, static_cast<double>(at_ms)));
      auto keep_or = [&](const char* field, Value* fallback) {
        const Value* existing = tj::Get(old, field);
        if (!is_null(existing)) return true;
        return tj::Set(chunk, next, field, fallback ? fallback : tj::Null(chunk));
      };
      ok = ok && keep_or("fiatAtTime", t.has_valuation ? tj::Str(chunk, t.fiat_at_time) : nullptr) &&
           keep_or("fiatSourceInterval", t.has_valuation ? tj::Str(chunk, t.interval) : nullptr) &&
           keep_or("fiatSourceId", t.has_valuation ? tj::Str(chunk, t.source_id) : nullptr) &&
           keep_or("fiatSourceTs", t.has_valuation ? tj::Num(chunk, static_cast<double>(t.source_ts)) : nullptr) &&
           tj::Set(chunk, next, "updatedAt", tj::Num(chunk, static_cast<double>(at_ms)));
      if (!ok) break;
      if (is_unpriced(old)) {
        Key unpriced;
        if (unpriced_key(&unpriced, old)) batch.Erase(unpriced.text); else ok = false;
        Key keys[2];
        const size_t n = token_time_keys(keys, "transfer/token-unpriced-time/", old);
        for (size_t k = 0; k < n; ++k) if (keys[k].ok) batch.Erase(keys[k].text); else ok = false;
      }
      batch.Put(key.text, next);
      if (is_unpriced(next)) {
        Key unpriced;
        if (unpriced_key(&unpriced, next)) batch.Put(unpriced.text, demand_ref(chunk, next)); else ok = false;
        Key keys[2];
        const size_t n = token_time_keys(keys, "transfer/token-unpriced-time/", next);
        for (size_t k = 0; k < n; ++k)
          if (keys[k].ok) batch.PutString(keys[k].text, t.record_id); else ok = false;
      }
      overlay[overlay_count++] = Overlay{chunk.CopyText(key.text, strlen(key.text)), next};
      ++written;
      ok = ok && batch.ok;
    }
    if (ok && batch.count) ok = kv_->Write(batch.items, batch.count);
    lock.Unlock();
    if (ok) *changed += written;
  }
  tj::FreeArena(chunk);
  scratch_storage.reset();
  return ok;
}

size_t Store::PruneTerminalBefore(int64_t before_ms, size_t limit, int64_t now_ms, Arena& arena) {
  (void)arena;
  const char* root = "transfer/retention/";
  const size_t root_length = strlen(root);
  size_t deleted = 0;
  char position[kKeyBytes] = {};
  bool has_position = false;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return 0;
  Arena chunk = tj::GrowingArena(kChunkArenaBytes);
  std::unique_ptr<Arena, tj::ArenaBlocksDeleter> chunk_storage(&chunk);
  while (deleted < limit) {
    tj::FreeArena(chunk);
    chunk = tj::GrowingArena(kChunkArenaBytes);
    Collected entries{};
    storage::KvPageResult result;
    const size_t want = limit - deleted < kPageScan ? limit - deleted : kPageScan;
    entries.arena = &chunk;
    entries.entries = static_cast<Entry*>(chunk.Alloc(sizeof(Entry) * kPageScan));
    entries.capacity = kPageScan;
    entries.ok = entries.entries != nullptr;
    if (!entries.ok || !kv_->Page(root, has_position ? position : nullptr, want, now_ms, false, [&, visitor_state = &entries](const char* key, const void* value, size_t size) { return (collect)(key, value, size, visitor_state); }, &result) || !entries.ok)
      break;
    size_t old_count = 0;
    while (old_count < entries.count && key_ms(entries.entries[old_count].key, root_length) < before_ms) ++old_count;
    if (old_count > 0) {
      fortis::base::UniqueLock lock(mutex_);
      Mutations batch(chunk, scratch, old_count * 14 + 1);
      size_t count = 0;
      for (size_t i = 0; batch.ok && i < old_count; ++i) {
        Value* ref = parse_entry(chunk, entries.entries[i]);
        const char* account = string_member(ref, "accountId");
        const char* record = string_member(ref, "recordId");
        Value* row = nullptr;
        if (account && record && GetTransfer(account, record, now_ms, chunk, &row) == ReadStatus::kFound &&
            !chains::Equals(tj::Get(row, "status"), "pending") && RowMs(row, "ts") < before_ms) {
          Key key, cursor, time, age;
          if (row_key(&key, account, record)) batch.Erase(key.text);
          if (cursor_key(&cursor, row)) batch.Erase(cursor.text);
          if (time_key(&time, "transfer/time/", row)) batch.Erase(time.text);
          Key keys[2];
          const size_t n = token_time_keys(keys, "transfer/token-time/", row);
          for (size_t k = 0; k < n; ++k) if (keys[k].ok) batch.Erase(keys[k].text);
          if (age_key(&age, "transfer/age/", row)) batch.Erase(age.text);
          if (is_unpriced(row)) {
            Key unpriced;
            if (unpriced_key(&unpriced, row)) batch.Erase(unpriced.text);
            Key ukeys[2];
            const size_t m = token_time_keys(ukeys, "transfer/token-unpriced-time/", row);
            for (size_t k = 0; k < m; ++k) if (ukeys[k].ok) batch.Erase(ukeys[k].text);
          }
          ++count;
        }
        batch.Erase(entries.entries[i].key);
      }
      const bool written = batch.ok && kv_->Write(batch.items, batch.count);
      lock.Unlock();
      if (!written) break;
      deleted += count;
    }
    if (old_count < entries.count) break;
    if (result.done || !result.next_cursor[0]) break;
    snprintf(position, sizeof(position), "%s", result.next_cursor);
    has_position = true;
  }
  tj::FreeArena(chunk);
  scratch_storage.reset();
  return deleted;
}

// ---- watches -----------------------------------------------------------------------

namespace {

bool watch_keys(Key* device_key, const char* device, const char* topic) {
  device_key->Raw("watch/device/");
  device_key->Part(device);
  device_key->Raw("/");
  device_key->Part(topic);
  return device_key->ok;
}

bool account_watch_key(Key* key, const char* account, const char* device, const char* topic) {
  key->Raw("watch/account/");
  key->Part(account);
  key->Raw("/");
  key->Part(device);
  key->Raw("/");
  key->Part(topic);
  return key->ok;
}

uint64_t topic_hash(const char* device, const char* topic) {
  uint64_t h = 1469598103934665603ULL;
  for (const char* p = device; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
  h = (h ^ 0) * 1099511628211ULL;
  for (const char* p = topic; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
  return h ? h : 1;
}

int compare_strings(const void* a, const void* b) {
  return strcmp(*static_cast<const char* const*>(a), *static_cast<const char* const*>(b));
}

}  // namespace

bool Store::PutWatch(const char* device_id, const char* topic, const char* account_id, const Value* tokens,
                     int64_t expires_at, int64_t now_ms, Arena& arena) {
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  Key key;
  bool ok = watch_keys(&key, device_id, topic);
  Value* old = nullptr;
  if (ok && GetJson(key.text, now_ms, arena, &old) == ReadStatus::kError) ok = false;
  Mutations batch(arena, scratch, 3);
  if (ok && old) {
    const char* old_account = string_member(old, "accountId");
    Key old_key;
    if (old_account && account_watch_key(&old_key, old_account, device_id, topic)) batch.Erase(old_key.text);
  }
  Value* watch = tj::Obj(arena);
  ok = ok && watch && tj::Set(arena, watch, "deviceId", tj::Str(arena, device_id)) &&
       tj::Set(arena, watch, "topic", tj::Str(arena, topic)) &&
       tj::Set(arena, watch, "accountId", account_id ? tj::Str(arena, account_id) : tj::Null(arena)) &&
       tj::Set(arena, watch, "trackedTokens", tokens ? tj::Clone(arena, tokens) : tj::Arr(arena)) &&
       tj::Set(arena, watch, "expiresAt", tj::Num(arena, static_cast<double>(expires_at)));
  if (ok) {
    batch.Put(key.text, watch, expires_at);
    Key index;
    if (account_id && account_watch_key(&index, account_id, device_id, topic)) batch.Put(index.text, watch, expires_at);
    ok = batch.ok && kv_->Write(batch.items, batch.count);
  }
  scratch_storage.reset();
  return ok;
}

bool Store::RememberTopic(const char* device_id, const char* topic, int64_t now_ms, Arena& arena) {
  const uint64_t hash = topic_hash(device_id, topic);
  // splitmix64's finalizer: FNV-1a's low bits alone collide on the
  // repeated-byte hashes wallets use.
  uint64_t mixed = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
  mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
  mixed ^= mixed >> 31;
  fortis::base::UniqueLock lock(mutex_);
  RecentTopic* set = recent_topics_
                         ? &recent_topics_[(mixed % (kRecentTopics / kRecentTopicWays)) * kRecentTopicWays]
                         : nullptr;
  RecentTopic* slot = nullptr;
  for (size_t way = 0; set && way < kRecentTopicWays && !slot; ++way)
    if (set[way].hash == hash) slot = &set[way];
  if (slot && now_ms - slot->renewed_at < kWatchTtlMs - kWatchRenewBeforeMs) return true;
  Key key;
  bool ok = watch_keys(&key, device_id, topic);
  Value* prior = nullptr;
  const ReadStatus status = ok ? GetJson(key.text, now_ms, arena, &prior) : ReadStatus::kError;
  ok = status != ReadStatus::kError;
  const char* account = status == ReadStatus::kFound ? string_member(prior, "accountId") : nullptr;
  const Value* tokens = status == ReadStatus::kFound ? tj::Get(prior, "trackedTokens") : nullptr;
  ok = ok && PutWatch(device_id, topic, account, tj::IsArray(tokens) ? tokens : nullptr, now_ms + kWatchTtlMs, now_ms,
                      arena);
  if (ok && set) {
    // The pair's own way, else the first empty one, else the oldest renewal.
    if (!slot) {
      slot = &set[0];
      for (size_t way = 0; way < kRecentTopicWays; ++way) {
        if (!set[way].hash) { slot = &set[way]; break; }
        if (set[way].renewed_at < slot->renewed_at) slot = &set[way];
      }
    }
    *slot = RecentTopic{hash, now_ms};
  }
  lock.Unlock();
  if (ok && status == ReadStatus::kMissing) TopicWritten(device_id);
  return ok;
}

bool Store::Remember(const char* device_id, const char* account_id, const char* network, const char* address,
                     const char* const* tracked, size_t tracked_count, int64_t now_ms, Arena& arena) {
  Value* topic_value = chains::Format(arena, "portfolio:%s:%s", network, address);
  if (!topic_value) return false;
  const char* topic = topic_value->text;
  // [...new Set(tracked.map(t => t.trim()).filter(Boolean))].sort()
  const char** list = tracked_count ? static_cast<const char**>(arena.Alloc(sizeof(char*) * tracked_count)) : nullptr;
  size_t n = 0;
  for (size_t i = 0; i < tracked_count; ++i) {
    size_t b = 0, e = 0;
    fortis::codec::provider_text::Trim(tracked[i], strlen(tracked[i]), &b, &e);
    if (e == b) continue;
    const char* trimmed = arena.CopyText(tracked[i] + b, e - b);
    if (!trimmed) return false;
    bool seen = false;
    for (size_t j = 0; j < n; ++j) if (!strcmp(list[j], trimmed)) { seen = true; break; }
    if (!seen) list[n++] = trimmed;
  }
  if (n > 1) qsort(reinterpret_cast<void*>(list), n, sizeof(char*), fortis::base::FatalCallback<compare_strings>);
  Value* tokens = tj::Arr(arena);
  for (size_t i = 0; tokens && i < n; ++i)
    if (!tj::Push(arena, tokens, tj::Str(arena, list[i]))) return false;
  if (!tokens) return false;
  fortis::base::UniqueLock lock(mutex_);
  Key key;
  bool ok = watch_keys(&key, device_id, topic);
  Value* previous = nullptr;
  const ReadStatus status = ok ? GetJson(key.text, now_ms, arena, &previous) : ReadStatus::kError;
  ok = status != ReadStatus::kError;
  bool changed = status != ReadStatus::kFound;
  if (ok && !changed) {
    const char* prev_account = string_member(previous, "accountId");
    if (!prev_account || strcmp(prev_account, account_id) != 0) changed = true;
    uint8_t a[16384], b[16384];
    size_t la = 0, lb = 0;
    const Value* prev_tokens = tj::Get(previous, "trackedTokens");
    if (!changed && (!prev_tokens || !stringify(prev_tokens, a, sizeof(a), &la) || !stringify(tokens, b, sizeof(b), &lb) ||
                     la != lb || memcmp(a, b, la) != 0))
      changed = true;
  }
  if (ok && !changed && RowMs(previous, "expiresAt") > now_ms + kWatchRenewBeforeMs) return true;
  ok = ok && PutWatch(device_id, topic, account_id, tokens, now_ms + kWatchTtlMs, now_ms, arena);
  lock.Unlock();
  if (ok && changed) TopicWritten(device_id);
  return ok;
}

bool Store::TokensForAccount(const char* account_id, int64_t now_ms, Arena& arena, Value** tokens) const {
  Key prefix;
  prefix.Raw("watch/account/");
  prefix.Part(account_id);
  prefix.Raw("/");
  if (!prefix.ok) return false;
  const char** list = static_cast<const char**>(arena.Alloc(sizeof(char*) * 1024));
  size_t n = 0;
  if (!list) return false;
  char position[kKeyBytes] = {};
  bool has_position = false;
  for (;;) {
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, prefix.text, has_position ? position : nullptr, false, now_ms, arena, &entries, &result))
      return false;
    for (size_t i = 0; i < entries.count; ++i) {
      Value* watch = parse_entry(arena, entries.entries[i]);
      const Value* tracked = tj::Get(watch, "trackedTokens");
      for (uint32_t t = 0; tj::IsArray(tracked) && t < tracked->count; ++t) {
        const Value* token = tracked->items[t];
        if (!tj::IsString(token)) continue;
        bool seen = false;
        for (size_t j = 0; j < n; ++j) if (!strcmp(list[j], token->text)) { seen = true; break; }
        if (!seen && n < 1024) list[n++] = token->text;
      }
    }
    if (result.done || !result.next_cursor[0]) break;
    snprintf(position, sizeof(position), "%s", result.next_cursor);
    has_position = true;
  }
  if (n > 1) qsort(reinterpret_cast<void*>(list), n, sizeof(char*), fortis::base::FatalCallback<compare_strings>);
  Value* out = tj::Arr(arena);
  for (size_t i = 0; out && i < n; ++i)
    if (!tj::Push(arena, out, tj::Str(arena, list[i]))) return false;
  if (!out) return false;
  *tokens = out;
  return true;
}


bool Store::ListDeviceWatches(const char* device_id, int64_t now_ms, Arena& arena, Value** rows) const {
  Key prefix;
  prefix.Raw("watch/device/");
  prefix.Part(device_id);
  prefix.Raw("/");
  Value* out = tj::Arr(arena);
  if (!prefix.ok || !out) return false;
  char position[kKeyBytes] = {};
  bool has_position = false;
  for (;;) {
    Collected entries{};
    storage::KvPageResult result;
    if (!page(kv_, prefix.text, has_position ? position : nullptr, false, now_ms, arena, &entries, &result))
      return false;
    for (size_t i = 0; i < entries.count; ++i) {
      Value* watch = parse_entry(arena, entries.entries[i]);
      if (!watch || !tj::Push(arena, out, watch)) return false;
    }
    if (result.done || !result.next_cursor[0]) break;
    snprintf(position, sizeof(position), "%s", result.next_cursor);
    has_position = true;
  }
  *rows = out;
  return true;
}

bool Store::RenewTopics(const char* device_id, const char* const* topics, size_t count, int64_t now_ms,
                        Arena& arena) {
  if (!count) return true;
  auto scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  auto* scratch = scratch_storage.get();
  if (!scratch) return false;
  fortis::base::UniqueLock lock(mutex_);
  Mutations batch(arena, scratch, count * 2);
  bool ok = batch.ok;
  const int64_t expires_at = now_ms + kWatchTtlMs;
  for (size_t i = 0; ok && i < count; ++i) {
    Key key;
    if (!watch_keys(&key, device_id, topics[i])) { ok = false; break; }
    // Each read reserves a value buffer: a scratch arena per topic keeps the
    // caller's arena to the queued rows, however many topics are due.
    Arena read = tj::GrowingArena(kChunkArenaBytes);
  std::unique_ptr<Arena, tj::ArenaBlocksDeleter> read_storage(&read);
    Value* old = nullptr;
    const ReadStatus status = GetJson(key.text, now_ms, read, &old);
    if (status == ReadStatus::kFound && RowMs(old, "expiresAt") <= now_ms + kWatchRenewBeforeMs) {
      ok = tj::Set(read, old, "expiresAt", tj::Num(read, static_cast<double>(expires_at)));
      if (ok) batch.Put(key.text, old, expires_at);
      const char* account = string_member(old, "accountId");
      Key index;
      if (ok && account && account_watch_key(&index, account, device_id, topics[i]))
        batch.Put(index.text, old, expires_at);
      else if (ok && account) ok = false;
    }
    if (status == ReadStatus::kError) ok = false;
    tj::FreeArena(read);
    ok = ok && batch.ok;
  }
  if (ok && batch.count) ok = kv_->Write(batch.items, batch.count);
  lock.Unlock();
  scratch_storage.reset();
  return ok;
}

bool Store::WriteWatch(const char* device_id, const char* topic, const char* account_id, int64_t expires_at,
                       int64_t now_ms, Arena& arena) {
  fortis::base::LockGuard lock(mutex_);
  const bool ok = PutWatch(device_id, topic, account_id, nullptr, expires_at, now_ms, arena);
  return ok;
}

}  // namespace fortis::wallet::address
