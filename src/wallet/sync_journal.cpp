#include "ownership.h"
#include <vector>
#include "sync_journal.h"

#include "address_store.h"
#include "kv_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::wallet::sync {
namespace {

namespace tj = fortis::codec::tj;

constexpr size_t kPrefixBytes = 1100;
const char kZero[kSequenceDigits + 1] = "00000000000000000000";

bool digits(const char* text, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// The next event of one account: its key's sequence and a copy of the value.
struct Pending {
  size_t account = 0;
  char sequence[kSequenceDigits + 1]{};
  std::unique_ptr<char[], base::StorageDeleter> value;
  size_t length = 0;
};

struct Take {
  size_t prefix_length;
  Pending* node;
  bool found;
  bool ok;
};

bool take(const char* key, const void* value, size_t size, Take* take) {
  const char* suffix = key + take->prefix_length;
  if (strlen(suffix) != kSequenceDigits || !digits(suffix, kSequenceDigits)) {
    take->ok = false;
    return false;
  }
  auto copy = base::AllocateStorage<char[]>(size + 1);
  if (!copy) {
    take->ok = false;
    return false;
  }
  memcpy(copy.get(), value, size);
  copy[size] = '\0';
  memcpy(take->node->sequence, suffix, kSequenceDigits + 1);
  take->node->value = std::move(copy);
  take->node->length = size;
  take->found = true;
  return true;
}

// next(accountId, afterSequence, watermark, now): the first live event after
// `after` (exclusive) when it is within the watermark.
bool next_event(const storage::KvStore* kv, const char* prefix, const char* after, const char* watermark, int64_t now_ms,
                Pending* node, bool* found) {
  *found = false;
  char start[kPrefixBytes + 16 + kSequenceDigits + 1];
  const int written = snprintf(start, sizeof(start), "%s%s", prefix, after);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(start)) return false;
  Take state{strlen(prefix), node, false, true};
  if (!kv->Scan(prefix, start, 1, now_ms, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (take)(key, value, size, visitor_state); }) || !state.ok) {
    node->value.reset();
    return false;
  }
  if (!state.found) return true;
  if (strcmp(node->sequence, watermark) > 0) {
    node->value.reset();
    return true;
  }
  *found = true;
  return true;
}

// A binary min-heap on the 20-digit sequence (one pending event per account).
struct Heap {
  std::vector<Pending> nodes;
  size_t size = 0;

  void Push(Pending node) {
    size_t index = size++;
    while (index > 0) {
      const size_t parent = (index - 1) / 2;
      if (strcmp(nodes[parent].sequence, node.sequence) <= 0) break;
      nodes[index] = std::move(nodes[parent]);
      index = parent;
    }
    nodes[index] = std::move(node);
  }

  Pending Pop() {
    Pending first = std::move(nodes[0]);
    Pending last = std::move(nodes[--size]);
    size_t index = 0;
    for (;;) {
      const size_t left = index * 2 + 1;
      if (left >= size) break;
      const size_t right = left + 1;
      const size_t child =
          right < size && strcmp(nodes[right].sequence, nodes[left].sequence) < 0 ? right : left;
      if (strcmp(nodes[child].sequence, last.sequence) >= 0) break;
      nodes[index] = std::move(nodes[child]);
      index = child;
    }
    if (size) nodes[index] = std::move(last);
    return first;
  }
};

}  // namespace

void FormatCursor(const char* sequence, int64_t at_ms, char output[kCursorTextBytes]) {
  snprintf(output, kCursorTextBytes, "s2_%013lld_%s", static_cast<long long>(at_ms), sequence);
}

bool Journal::Head(int64_t now_ms, char head[kSequenceDigits + 1]) const {
  char value[64];
  size_t size = 0;
  const storage::KvReadStatus status = kv_->GetStatus("sync2/meta/head", now_ms, value, sizeof(value), &size);
  if (status == storage::KvReadStatus::Missing) {
    memcpy(head, kZero, sizeof(kZero));
    return true;
  }
  // The stored value is the JSON string of the padded sequence.
  if (status != storage::KvReadStatus::Found || size != kSequenceDigits + 2 || value[0] != '"' ||
      value[kSequenceDigits + 1] != '"' || !digits(value + 1, kSequenceDigits))
    return false;
  memcpy(head, value + 1, kSequenceDigits);
  head[kSequenceDigits] = '\0';
  return true;
}

bool Journal::Read(const char* const* account_ids, size_t account_count, const char* cursor, size_t cursor_length,
                   uint32_t limit, int64_t now_ms, bool has_after, double after_ms, EventVisitor visit, void* context,
                   tj::Arena& arena, PageResult* result) const {
  *result = PageResult{};
  if (limit < 1 || limit > 500) return false;
  char watermark[kSequenceDigits + 1];
  if (!Head(now_ms, watermark)) return false;
  FormatCursor(watermark, now_ms, result->cursor);
  if (!cursor) return true;
  // /^s2_([0-9]{13})_([0-9]{20})$/
  if (cursor_length != 37 || memcmp(cursor, "s2_", 3) != 0 || !digits(cursor + 3, 13) || cursor[16] != '_' ||
      !digits(cursor + 17, kSequenceDigits)) {
    result->reset = true;
    return true;
  }
  int64_t issued = 0;
  for (size_t i = 3; i < 16; ++i) issued = issued * 10 + (cursor[i] - '0');
  char since[kSequenceDigits + 1];
  memcpy(since, cursor + 17, kSequenceDigits);
  since[kSequenceDigits] = '\0';
  if (issued > now_ms || now_ms - issued > kCursorMaxAgeMs || strcmp(since, watermark) > 0) {
    result->reset = true;
    return true;
  }

  char** prefixes = account_count ? static_cast<char**>(arena.Alloc(sizeof(char*) * account_count)) : nullptr;
  Heap heap;
  if ((account_count && !prefixes) || account_count > heap.nodes.max_size()) return false;
  try { heap.nodes.resize(account_count); }
  catch (const std::bad_alloc&) { return false; }
  bool ok = true;
  for (size_t i = 0; ok && i < account_count; ++i) {
    char encoded[kPrefixBytes];
    ok = address::Store::Component(account_ids[i], encoded, sizeof(encoded) - 32);
    if (!ok) break;
    char prefix[kPrefixBytes + 16];  // "sync2/account/" + encoded + "/"
    snprintf(prefix, sizeof(prefix), "sync2/account/%s/", encoded);
    prefixes[i] = arena.CopyText(prefix, strlen(prefix));
    Pending node;
    node.account = i;
    bool found = false;
    ok = prefixes[i] && next_event(kv_, prefixes[i], since, watermark, now_ms, &node, &found);
    if (ok && found) heap.Push(std::move(node));
  }

  size_t accepted = 0;
  char last_sequence[kSequenceDigits + 1] = {};
  int64_t last_committed = 0;
  while (ok && heap.size) {
    Pending node = heap.Pop();
    const tj::Mark mark = tj::MarkOf(arena);
    const tj::Value* event = tj::Parse(arena, reinterpret_cast<const uint8_t*>(node.value.get()), node.length);
    const tj::Value* changed_at = tj::Get(event, "changedAt");
    const tj::Value* sequence = tj::Get(event, "sequence");
    const tj::Value* committed = tj::Get(event, "committedAt");
    ok = tj::IsObject(event) && tj::IsNumber(changed_at) && tj::IsString(sequence) &&
         sequence->length == kSequenceDigits && tj::IsNumber(committed);
    // after === undefined || changedAt.getTime() > after.getTime()
    if (ok && (!has_after || changed_at->number > after_ms)) {
      if (accepted == limit) {
        // The (limit + 1)th event only proves there is more.
        result->has_more = true;
        node.value.reset();
        tj::Rewind(arena, mark);
        break;
      }
      const Visit visited = visit(context, node.account, event, arena);
      if (visited == Visit::kFull && accepted) {
        result->has_more = true;
        node.value.reset();
        tj::Rewind(arena, mark);
        break;
      }
      ok = visited == Visit::kWritten;
      ++accepted;
      memcpy(last_sequence, sequence->text, kSequenceDigits + 1);
      last_committed = static_cast<int64_t>(committed->number);
    }
    tj::Rewind(arena, mark);
    node.value.reset();
    node.value = nullptr;
    if (!ok) break;
    char after[kSequenceDigits + 1];
    memcpy(after, node.sequence, sizeof(after));
    bool found = false;
    ok = next_event(kv_, prefixes[node.account], after, watermark, now_ms, &node, &found);
    if (ok && found) heap.Push(std::move(node));
  }
  heap.nodes.clear();
  if (!ok) return false;
  result->events = accepted;
  if (result->has_more) FormatCursor(last_sequence, last_committed, result->cursor);
  return true;
}

}  // namespace fortis::wallet::sync
