#include <memory>
#include "test_support.h"
// Atomic token writes (token_writes.h): every liquidity outcome, write-through
// and market chunk is one KV frame. Injects KV write failures, hard kills
// (before the body, before fsync, after the acknowledged fsync, during
// replay) and torn tails at every frame boundary, then reopens the store
// and checks that facts, comparison, observation and pair rows describe one
// generation. Also checks concurrent read-modify-writes, the full-block
// observation bound and the market frame split. `--measure` prints frame,
// write-count, allocation and latency figures for realistic batches.
#include "../src/storage/kv_store.h"
#include "../src/tokens/token_comparison_store.h"
#include "../src/tokens/token_data.h"
#include "../src/codec/token_json.h"
#include "../src/tokens/token_writes.h"

#include <atomic>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::codec;

using namespace fortis::storage;
using namespace fortis::tokens;

using namespace fortis;
namespace td = fortis::tokens::token_data;
namespace tw = fortis::tokens::token_writes;
using token_store::ComparisonPatch;
using token_store::FieldState;

#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                        \
    }                                                                                 \
  } while (0)

namespace {

constexpr const char* kNet = "eth-mainnet";
constexpr const char* kToken = "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48";
// A 6-hour block boundary, so generations 1..71 share one observation block.
constexpr int64_t kBase = 1760011200000LL;
constexpr int64_t kStep = 300000;  // one observation bucket per generation
constexpr int64_t kReadNow = kBase + 86400000LL;

int64_t at(int generation) { return kBase + generation * kStep; }
int generation_of(int64_t ms) { return static_cast<int>((ms - kBase) / kStep); }

struct ArenaBox {
  uint8_t* memory;
  tj::Arena arena;
  explicit ArenaBox(size_t bytes) : memory(static_cast<uint8_t*>(malloc(bytes))), arena{memory, bytes, 0, true} {
    CHECK(memory);
  }
  ~ArenaBox() { free(memory); }
  void Reset() { arena.used = 0; arena.ok = true; }
};

struct World {
  KvStore kv;
  td::TokenDataStore* data = nullptr;
  explicit World(const char* directory) {
    CHECK(kv.Open(directory));
    data = new td::TokenDataStore(&kv);
    CHECK(data->valid());
  }
  World(const World&) = delete;
  World& operator=(const World&) = delete;
  ~World() {
    delete data;
    kv.Close();
  }
};

void make_directory(char* out, size_t capacity) {
  snprintf(out, capacity, "/tmp/fortis-token-atomicity-XXXXXX");
  CHECK(mkdtemp(out));
}

void remove_directory(const char* directory) {
  char path[512];
  const char* names[] = {"fortis.kv", "fortis.kv.lock", "fortis.kv.compact"};
  for (const char* name : names) {
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    unlink(path);
  }
  rmdir(directory);
}

uint64_t log_size(const char* directory) {
  char path[512];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  struct stat info {};
  return stat(path, &info) == 0 ? static_cast<uint64_t>(info.st_size) : 0;
}

// ---- one liquidity generation ------------------------------------------------

struct Liquidity {
  tj::Value* facts = nullptr;
  ComparisonPatch patch;
  td::Observation observation;
  char pad[256];
};

// `kind`: 0 ok, 1 not_found, 2 diagnostic (error).
void build_liquidity(tj::Arena& a, int generation, int kind, Liquidity* out, size_t snapshot_pad = 0) {
  const int64_t t = at(generation);
  out->facts = tj::Obj(a);
  const char* status = kind == 0 ? "ok" : kind == 1 ? "not_found" : "error";
  if (kind == 0) {
    tj::Value* snapshot = tj::Obj(a);
    CHECK(tj::Set(a, snapshot, "totalUsd", tj::Num(a, generation * 1000.0)));
    if (snapshot_pad) {
      char* text = static_cast<char*>(a.Alloc(snapshot_pad + 1));
      CHECK(text);
      memset(text, 'p', snapshot_pad);
      text[snapshot_pad] = '\0';
      CHECK(tj::Set(a, snapshot, "pad", tj::Str(a, text, snapshot_pad)));
    }
    CHECK(tj::Set(a, out->facts, "liquidity", snapshot));
    CHECK(tj::Set(a, out->facts, "liquidityAsOf", tj::Num(a, static_cast<double>(t))));
  }
  CHECK(tj::Set(a, out->facts, "liquidityStatus", tj::Str(a, status)));
  CHECK(tj::Set(a, out->facts, "liquidityCheckedAt", tj::Num(a, static_cast<double>(t))));
  CHECK(tj::Set(a, out->facts, "liquiditySource", tj::Str(a, "dexscreener")));
  out->patch = ComparisonPatch{};
  out->patch.fiat = {FieldState::kValue, "usd", 3};
  out->patch.liquidity_status = {FieldState::kValue,
                                 kind == 0 ? comparison::LiquidityStatus::kOk
                                 : kind == 1 ? comparison::LiquidityStatus::kNotFound
                                             : comparison::LiquidityStatus::kError};
  out->patch.liquidity_checked_at = {FieldState::kValue, t};
  out->patch.liquidity_source = {FieldState::kValue, "dexscreener", 11};
  out->patch.updated_at = {FieldState::kValue, t};
  if (kind == 0) {
    out->patch.liquidity_usd = {FieldState::kValue, generation * 1000.0};
    out->patch.liquidity_as_of = {FieldState::kValue, t};
  }
  out->observation = td::Observation{};
  out->observation.ts = t;
  out->observation.ok = kind == 0;
  snprintf(out->observation.source, sizeof(out->observation.source), "dexscreener");
  out->observation.has_sampling = true;
  out->observation.sampling_seconds = 300;
  out->observation.has_liquidity = kind == 0;
  out->observation.liquidity = generation * 1000.0;
  out->observation.pool_count = kind == 0 ? generation : 0;
}

bool record(World& w, tj::Arena& a, int generation, int kind = 0, tw::FrameStats* stats = nullptr,
            const char* token = kToken, size_t snapshot_pad = 0) {
  Liquidity l{};
  build_liquidity(a, generation, kind, &l, snapshot_pad);
  tw::LiquidityOutcome outcome{};
  outcome.network = kNet;
  outcome.token_id = token;
  outcome.facts = l.facts;
  outcome.comparison = &l.patch;
  outcome.observation = kind == 2 ? nullptr : &l.observation;
  return tw::RecordLiquidity(*w.data, outcome, at(generation) + 1000, stats);
}

// ---- the related view ----------------------------------------------------------

struct View {
  int facts = -1;        // facts.liquidityCheckedAt
  int comparison = -1;   // comparison liquidity_checked_at
  int observation = -1;  // newest raw row
  size_t rows = 0;
  int pair = -1;         // liqpair lastTs
};

View read_view(World& w, const char* token = kToken) {
  View v{};
  ArenaBox box(4 << 20);
  tj::Value* facts = nullptr;
  if (w.data->ReadFacts(kNet, token, kReadNow, box.arena, &facts) == td::DataOutcome::kOk) {
    const tj::Value* checked = tj::Get(facts, "liquidityCheckedAt");
    if (tj::IsNumber(checked)) v.facts = generation_of(static_cast<int64_t>(checked->number));
  }
  TokenComparisonStore store(&w.kv);
  token_store::Key key{kNet, token};
  auto* record = static_cast<comparison::Record*>(malloc(sizeof(comparison::Record)));
  CHECK(record);
  CHECK(store.Read(&key, 1, "usd", kReadNow, record) == token_store::ReadStatus::kOk);
  if (record->liquidity_checked_at.present) v.comparison = generation_of(record->liquidity_checked_at.ms);
  free(record);
  td::Observation rows[128];
  td::Rows<td::Observation> out{rows, 128, 0, false};
  CHECK(w.data->Observations(kNet, token, 0, kBase * 2, kReadNow, &out) == td::DataOutcome::kOk);
  v.rows = out.count;
  if (out.count) v.observation = generation_of(rows[out.count - 1].ts);
  char pair_key[256];
  snprintf(pair_key, sizeof(pair_key), "token/liqpair/%s/%s", kNet, token);
  char buffer[1024];
  size_t size = 0;
  if (w.kv.Get(pair_key, kReadNow, buffer, sizeof(buffer), &size)) {
    box.Reset();
    tj::Value* pair = tj::Parse(box.arena, reinterpret_cast<const uint8_t*>(buffer), size);
    const tj::Value* last = tj::Get(pair, "lastTs");
    if (tj::IsNumber(last)) v.pair = generation_of(static_cast<int64_t>(last->number));
  }
  return v;
}

// One generation everywhere, `rows` observation rows (ok/not_found outcomes).
void expect_generation(World& w, int generation, size_t rows) {
  const View v = read_view(w);
  if (v.facts != generation || v.comparison != generation || v.observation != generation ||
      v.pair != generation || v.rows != rows) {
    fprintf(stderr, "view: facts %d comparison %d observation %d pair %d rows %zu, expected %d/%zu\n", v.facts,
            v.comparison, v.observation, v.pair, v.rows, generation, rows);
    abort();
  }
}

void expect_empty(World& w) {
  const View v = read_view(w);
  CHECK(v.facts == -1 && v.comparison == -1 && v.observation == -1 && v.pair == -1 && v.rows == 0);
}

// ---- KvBatch ---------------------------------------------------------------------

void test_batch() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  {
    KvStore kv;
    CHECK(kv.Open(directory));
    const KvMutation seed{"a", "stored", 6, 0, false};
    CHECK(kv.Write(&seed, 1));
    KvBatch batch(64, 4);
    char out[32];
    size_t size = 0;
    CHECK(batch.Get(&kv, "a", 0, out, sizeof(out), &size) == KvReadStatus::Found && size == 6);
    CHECK(batch.Put("a", "staged", 6, 0));
    CHECK(batch.Get(&kv, "a", 0, out, sizeof(out), &size) == KvReadStatus::Found && memcmp(out, "staged", 6) == 0);
    CHECK(batch.Put("b", "x", 1, 50));
    CHECK(batch.Get(&kv, "b", 49, out, sizeof(out), &size) == KvReadStatus::Found);
    CHECK(batch.Get(&kv, "b", 50, out, sizeof(out), &size) == KvReadStatus::Missing);
    const KvBatch::Mark mark = batch.Position();
    CHECK(batch.Erase("a"));
    CHECK(batch.Get(&kv, "a", 0, out, sizeof(out), &size) == KvReadStatus::Missing);
    batch.Rollback(mark);
    CHECK(batch.Get(&kv, "a", 0, out, sizeof(out), &size) == KvReadStatus::Found && memcmp(out, "staged", 6) == 0);
    // Byte and count bounds report full(); invalid input does not.
    char big[80];
    memset(big, 'z', sizeof(big));
    CHECK(!batch.Put("c", big, sizeof(big), 0) && batch.full());
    batch.Rollback(batch.Position());
    CHECK(!batch.full());
    CHECK(!batch.Put("", "v", 1, 0) && !batch.full());
    CHECK(!batch.Erase(nullptr) && !batch.full());
    CHECK(batch.Put("c", "1", 1, 0) && batch.Put("d", "2", 1, 0));
    CHECK(!batch.Put("e", "3", 1, 0) && batch.full());
    CHECK(batch.count() == 4);
    CHECK(batch.FrameBytes() == 16 + 4 * 15 + batch.bytes() - 4);
    const uint64_t before = log_size(directory);
    CHECK(batch.Commit(&kv) && batch.count() == 0);
    CHECK(log_size(directory) - before == 16 + 4 * 15 + (2 + 6) + (2 + 1) + (2 + 1) + (2 + 1) - 4);
    CHECK(kv.Get("a", 0, out, sizeof(out), &size) && memcmp(out, "staged", 6) == 0);
    // Duplicate keys in one frame: the later mutation wins, also on replay.
    CHECK(batch.Put("dup", "one", 3, 0) && batch.Put("dup", "two", 3, 0) && batch.Commit(&kv));
    CHECK(kv.Get("dup", 0, out, sizeof(out), &size) && memcmp(out, "two", 3) == 0);
  }
  {
    KvStore kv;
    CHECK(kv.Open(directory));
    char out[32];
    size_t size = 0;
    CHECK(kv.Get("dup", 0, out, sizeof(out), &size) && memcmp(out, "two", 3) == 0);
    CHECK(!kv.Get("b", 100, out, sizeof(out), &size));
  }
  remove_directory(directory);
  puts("  KvBatch staging, read-your-writes, bounds and duplicate keys");
}

// ---- liquidity outcomes: one frame each -----------------------------------------

void test_liquidity_frames() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  ArenaBox box(8 << 20);
  {
    World w(directory);
    tw::FrameStats stats{};
    CHECK(record(w, box.arena, 1, 0, &stats));
    CHECK(stats.frames == 1 && stats.mutations == 4 && stats.failed_frames == 0);
    expect_generation(w, 1, 1);
    box.Reset();
    stats = tw::FrameStats{};
    CHECK(record(w, box.arena, 2, 1, &stats));  // not_found
    CHECK(stats.frames == 1 && stats.mutations == 4);
    expect_generation(w, 2, 2);
    box.Reset();
    stats = tw::FrameStats{};
    CHECK(record(w, box.arena, 3, 2, &stats));  // diagnostic: facts + comparison
    CHECK(stats.frames == 1 && stats.mutations == 2);
    const View v = read_view(w);
    CHECK(v.facts == 3 && v.comparison == 3 && v.observation == 2 && v.pair == 2 && v.rows == 2);
    // A refused document (facts over the 256 KiB bound) stages nothing.
    box.Reset();
    stats = tw::FrameStats{};
    CHECK(!record(w, box.arena, 4, 0, &stats, kToken, td::kMaxDocumentBytes));
    CHECK(stats.frames == 0);
    const View after = read_view(w);
    CHECK(after.facts == 3 && after.comparison == 3 && after.observation == 2 && after.rows == 2);
    // A refused comparison patch (an over-long string) holds back the facts
    // patch and the observation too.
    box.Reset();
    Liquidity l{};
    build_liquidity(box.arena, 5, 0, &l);
    char long_source[300];
    memset(long_source, 's', sizeof(long_source));
    l.patch.liquidity_source = {FieldState::kValue, long_source, sizeof(long_source)};
    tw::LiquidityOutcome outcome{kNet, kToken, l.facts, &l.patch, &l.observation};
    CHECK(!tw::RecordLiquidity(*w.data, outcome, at(5) + 1000));
    const View refused = read_view(w);
    CHECK(refused.facts == 3 && refused.observation == 2 && refused.rows == 2);
  }
  {
    World w(directory);
    const View v = read_view(w);
    CHECK(v.facts == 3 && v.comparison == 3 && v.observation == 2 && v.pair == 2 && v.rows == 2);
  }
  remove_directory(directory);
  puts("  liquidity ok/not_found/diagnostic are one frame each; refusals stage nothing");
}

void test_corrupt_pair_refuses_transaction() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  ArenaBox box(8 << 20);
  {
    World w(directory);
    char pair_key[256];
    snprintf(pair_key, sizeof(pair_key), "token/liqpair/%s/%s", kNet, kToken);
    const KvMutation bad{pair_key, "{", 1, 0, false};
    CHECK(w.kv.Write(&bad, 1));
    const uint64_t before = log_size(directory);
    CHECK(!record(w, box.arena, 1));
    CHECK(log_size(directory) == before);
    const View view = read_view(w);
    CHECK(view.facts == -1 && view.comparison == -1 && view.observation == -1 && view.rows == 0);
  }
  remove_directory(directory);
  puts("  corrupt pair row refuses the whole liquidity transaction");
}

// ---- injected write failures ---------------------------------------------------------

void test_write_failures() {
  struct Case {
    const char* stage;
    int expected;  // generation visible after restart
  };
  // Before the commit barrier, recovery keeps the acknowledged generation.
  // An index failure after the barrier leaves the new generation durable.
  const Case cases[] = {{"write_after_header", 1}, {"write_before_fsync", 1}, {"write_after_fsync", 2}};
  for (int kind = 0; kind < 3; ++kind) {
    for (const Case& c : cases) {
      char directory[64];
      make_directory(directory, sizeof(directory));
      ArenaBox box(4 << 20);
      {
        World w(directory);
        CHECK(record(w, box.arena, 1, 0));
        box.Reset();
        CHECK(setenv("FORTIS_KV_TEST_FAIL", c.stage, 1) == 0);
        CHECK(!record(w, box.arena, 2, kind));
        CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
        // The store closed itself: nothing more is acknowledged.
        box.Reset();
        CHECK(!record(w, box.arena, 3, 0));
      }
      {
        World w(directory);
        const View v = read_view(w);
        if (kind == 2) {
          // Diagnostic: facts + comparison move together; raw rows stay at 1.
          CHECK(v.facts == c.expected && v.comparison == c.expected);
          CHECK(v.observation == 1 && v.pair == 1 && v.rows == 1);
        } else {
          expect_generation(w, c.expected, static_cast<size_t>(c.expected));
        }
        // The next write after restart is acknowledged and visible.
        box.Reset();
        CHECK(record(w, box.arena, 4, 0));
        if (kind == 2) {
          const View next = read_view(w);
          CHECK(next.facts == 4 && next.comparison == 4 && next.observation == 4);
        } else {
          expect_generation(w, 4, static_cast<size_t>(c.expected) + 1);
        }
      }
      remove_directory(directory);
    }
  }
  puts("  injected header/body/fsync failures: one generation visible, acknowledged write kept");
}

// ---- hard kills ------------------------------------------------------------------------

// Runs `body` in a child that must die by SIGKILL at the KV kill point.
template <typename Body>
void killed_child(const char* point, const char* on_call, Body body) {
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    setenv("FORTIS_KV_TEST_KILL", point, 1);
    if (on_call) setenv("FORTIS_KV_TEST_KILL_ON_CALL", on_call, 1);
    body();
    _exit(3);  // the kill point was not reached
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    fprintf(stderr, "child for %s exited with status %d\n", point, status);
    abort();
  }
}

void test_hard_kills() {
  struct Case {
    const char* point;
    int expected;
  };
  const Case cases[] = {{"write_after_header", 1}, {"write_before_fsync", 1}, {"write_acknowledged", 2}};
  for (const Case& c : cases) {
    char directory[64];
    make_directory(directory, sizeof(directory));
    {
      ArenaBox box(4 << 20);
      World w(directory);
      CHECK(record(w, box.arena, 1, 0));
    }
    killed_child(c.point, nullptr, [&]() {
      ArenaBox box(4 << 20);
      World w(directory);
      record(w, box.arena, 2, 0);
    });
    {
      World w(directory);
      expect_generation(w, c.expected, static_cast<size_t>(c.expected));
    }
    remove_directory(directory);
  }
  // A kill during restart replay (every frame position) changes nothing.
  char directory[64];
  make_directory(directory, sizeof(directory));
  {
    ArenaBox box(4 << 20);
    World w(directory);
    for (int g = 1; g <= 3; ++g) {
      box.Reset();
      CHECK(record(w, box.arena, g, 0));
    }
  }
  for (int frame = 1; frame <= 3; ++frame) {
    char call[8];
    snprintf(call, sizeof(call), "%d", frame);
    killed_child("replay_frame", call, [&]() {
      KvStore kv;
      kv.Open(directory);
    });
    World w(directory);
    expect_generation(w, 3, 3);
  }
  remove_directory(directory);
  puts("  SIGKILL before body/fsync, after acknowledgement and during replay: no half view");
}

// ---- torn tails ----------------------------------------------------------------------------

void copy_prefix(const char* from, const char* to_directory, uint64_t length) {
  char source[512], target[512];
  snprintf(source, sizeof(source), "%s/fortis.kv", from);
  snprintf(target, sizeof(target), "%s/fortis.kv", to_directory);
  const int in = open(source, O_RDONLY);
  const int out = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(in >= 0 && out >= 0);
  char buffer[65536];
  uint64_t left = length;
  while (left) {
    const ssize_t n = read(in, buffer, left < sizeof(buffer) ? left : sizeof(buffer));
    CHECK(n > 0);
    CHECK(write(out, buffer, static_cast<size_t>(n)) == n);
    left -= static_cast<uint64_t>(n);
  }
  close(in);
  close(out);
}

void test_torn_tails() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  uint64_t ends[4]{};
  {
    ArenaBox box(4 << 20);
    World w(directory);
    for (int g = 1; g <= 3; ++g) {
      box.Reset();
      CHECK(record(w, box.arena, g, g == 2 ? 1 : 0));
      ends[g] = log_size(directory);
    }
  }
  size_t checked = 0;
  for (int g = 1; g <= 3; ++g) {
    const uint64_t start = ends[g - 1], end = ends[g];
    for (uint64_t length = start; length <= end; ++length) {
      // Every header byte, every 23rd body byte, and the final bytes.
      const bool sample = length - start < 16 || end - length < 16 || (length - start) % 23 == 0;
      if (!sample) continue;
      char torn[64];
      make_directory(torn, sizeof(torn));
      copy_prefix(directory, torn, length);
      {
        World w(torn);
        const int visible = length == end ? g : g - 1;
        if (visible == 0) expect_empty(w);
        else expect_generation(w, visible, static_cast<size_t>(visible));
      }
      remove_directory(torn);
      ++checked;
    }
  }
  remove_directory(directory);
  printf("  torn tail at %zu offsets across 3 frames: all-or-nothing\n", checked);
}

// ---- write-through ---------------------------------------------------------------------------

void test_write_through() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  ArenaBox box(4 << 20);
  {
    World w(directory);
    tj::Arena& a = box.arena;
    tj::Value* facts = tj::Obj(a);
    tj::Value* market = tj::Obj(a);
    CHECK(tj::Set(a, market, "current_price", tj::Num(a, 1.0)));
    CHECK(tj::Set(a, facts, "market", market) && tj::Set(a, facts, "marketAsOf", tj::Num(a, static_cast<double>(at(1)))));
    ComparisonPatch patch{};
    patch.fiat = {FieldState::kValue, "usd", 3};
    patch.market_as_of = {FieldState::kValue, at(1)};
    patch.market_price_usd = {FieldState::kValue, 1.0};
    tw::FrameStats stats{};
    CHECK(tw::WriteThrough(*w.data, kNet, kToken, facts, &patch, at(1), &stats));
    CHECK(stats.frames == 1 && stats.mutations == 2);
    // Without a market change only facts are staged.
    stats = tw::FrameStats{};
    CHECK(tw::WriteThrough(*w.data, kNet, kToken, facts, nullptr, at(1), &stats));
    CHECK(stats.frames == 1 && stats.mutations == 1);
    // A comparison patch the native bounds refuse leaves the facts patch to
    // commit alone (TypeScript's separate facts transaction).
    tj::Value* later = tj::Obj(a);
    CHECK(tj::Set(a, later, "marketAsOf", tj::Num(a, static_cast<double>(at(2)))));
    char long_source[300];
    memset(long_source, 's', sizeof(long_source));
    ComparisonPatch refused{};
    refused.market_source = {FieldState::kValue, long_source, sizeof(long_source)};
    refused.market_as_of = {FieldState::kValue, at(2)};
    stats = tw::FrameStats{};
    CHECK(!tw::WriteThrough(*w.data, kNet, kToken, later, &refused, at(2), &stats));
    CHECK(stats.frames == 1 && stats.mutations == 1);
    box.Reset();
    tj::Value* doc = nullptr;
    CHECK(w.data->ReadFacts(kNet, kToken, kReadNow, a, &doc) == td::DataOutcome::kOk);
    CHECK(generation_of(static_cast<int64_t>(tj::Get(doc, "marketAsOf")->number)) == 2);
    TokenComparisonStore store(&w.kv);
    token_store::Key key{kNet, kToken};
    auto* r = static_cast<comparison::Record*>(malloc(sizeof(comparison::Record)));
    CHECK(store.Read(&key, 1, "usd", kReadNow, r) == token_store::ReadStatus::kOk);
    CHECK(r->market_as_of.present && generation_of(r->market_as_of.ms) == 1);
    free(r);
    // A failed frame commits neither.
    box.Reset();
    tj::Value* third = tj::Obj(a);
    CHECK(tj::Set(a, third, "marketAsOf", tj::Num(a, static_cast<double>(at(3)))));
    ComparisonPatch third_patch{};
    third_patch.market_as_of = {FieldState::kValue, at(3)};
    CHECK(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    CHECK(!tw::WriteThrough(*w.data, kNet, kToken, third, &third_patch, at(3)));
    CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    World w(directory);
    box.Reset();
    tj::Value* doc = nullptr;
    CHECK(w.data->ReadFacts(kNet, kToken, kReadNow, box.arena, &doc) == td::DataOutcome::kOk);
    CHECK(generation_of(static_cast<int64_t>(tj::Get(doc, "marketAsOf")->number)) == 2);
    TokenComparisonStore store(&w.kv);
    token_store::Key key{kNet, kToken};
    auto* r = static_cast<comparison::Record*>(malloc(sizeof(comparison::Record)));
    CHECK(store.Read(&key, 1, "usd", kReadNow, r) == token_store::ReadStatus::kOk);
    CHECK(r->market_as_of.present && generation_of(r->market_as_of.ms) == 1);
    free(r);
  }
  remove_directory(directory);
  puts("  write-through: facts + market comparison in one frame; native refusal keeps facts");
}

// ---- market chunks -------------------------------------------------------------------------------

struct MarketItems {
  char tokens[200][48];
  ComparisonPatch patches[200];
  tw::MarketRefresh items[200];
};

void build_market(tj::Arena& a, MarketItems* m, size_t count, int generation, size_t market_pad) {
  for (size_t i = 0; i < count; ++i) {
    snprintf(m->tokens[i], sizeof(m->tokens[i]), "0x%040zx", i + 1);
    tj::Value* market = tj::Obj(a);
    CHECK(tj::Set(a, market, "current_price", tj::Num(a, generation)));
    if (market_pad) {
      char* text = static_cast<char*>(a.Alloc(market_pad + 1));
      CHECK(text);
      memset(text, 'm', market_pad);
      text[market_pad] = '\0';
      CHECK(tj::Set(a, market, "description", tj::Str(a, text, market_pad)));
    }
    tj::Value* facts = tj::Obj(a);
    CHECK(tj::Set(a, facts, "market", market) &&
          tj::Set(a, facts, "marketAsOf", tj::Num(a, static_cast<double>(at(generation)))) &&
          tj::Set(a, facts, "marketSource", tj::Str(a, "coingecko")) &&
          tj::Set(a, facts, "marketSourceId", tj::Str(a, "usd-coin")));
    tj::Value* demand = tj::Obj(a);
    CHECK(tj::Set(a, demand, "lastRefreshedAt", tj::Num(a, static_cast<double>(at(generation)))));
    ComparisonPatch& p = m->patches[i];
    p = ComparisonPatch{};
    p.fiat = {FieldState::kValue, "usd", 3};
    p.market_price_usd = {FieldState::kValue, static_cast<double>(generation)};
    p.market_as_of = {FieldState::kValue, at(generation)};
    p.market_source = {FieldState::kValue, "coingecko", 9};
    p.updated_at = {FieldState::kValue, at(generation)};
    m->items[i] = tw::MarketRefresh{};
    m->items[i].network = kNet;
    m->items[i].token_id = m->tokens[i];
    m->items[i].facts = facts;
    m->items[i].comparison = &p;
    m->items[i].demand = demand;
  }
}

// facts.marketAsOf, comparison market_as_of and demand.lastRefreshedAt.
void market_view(World& w, const char* token, int* facts, int* comparison, int* demand) {
  ArenaBox box(1 << 20);
  *facts = *comparison = *demand = -1;
  tj::Value* doc = nullptr;
  if (w.data->ReadFacts(kNet, token, kReadNow, box.arena, &doc) == td::DataOutcome::kOk)
    *facts = generation_of(static_cast<int64_t>(tj::Get(doc, "marketAsOf")->number));
  if (w.data->ReadDemand(kNet, token, kReadNow, box.arena, &doc) == td::DataOutcome::kOk)
    *demand = generation_of(static_cast<int64_t>(tj::Get(doc, "lastRefreshedAt")->number));
  TokenComparisonStore store(&w.kv);
  token_store::Key key{kNet, token};
  auto* r = static_cast<comparison::Record*>(malloc(sizeof(comparison::Record)));
  CHECK(store.Read(&key, 1, "usd", kReadNow, r) == token_store::ReadStatus::kOk);
  if (r->market_as_of.present) *comparison = generation_of(r->market_as_of.ms);
  free(r);
}

void test_market_chunks() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  auto market_owner = std::make_unique<MarketItems>();
  auto* m = market_owner.get();
  CHECK(m);
  ArenaBox box(64 << 20);
  constexpr size_t kCount = 130;
  {
    World w(directory);
    build_market(box.arena, m, kCount, 1, 0);
    tw::FrameStats stats{};
    CHECK(tw::RecordMarketRefreshes(*w.data, m->items, kCount, at(1), &stats) == kCount);
    CHECK(stats.frames == 3 && stats.mutations == 3 * kCount && stats.splits == 0);
    // Generation 2: the second chunk's frame tears; chunk 1 is kept, chunks
    // 2 and 3 stay at generation 1 in every bucket.
    box.Reset();
    build_market(box.arena, m, kCount, 2, 0);
    CHECK(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    CHECK(setenv("FORTIS_KV_TEST_FAIL_ON_CALL", "2", 1) == 0);
    stats = tw::FrameStats{};
    CHECK(tw::RecordMarketRefreshes(*w.data, m->items, kCount, at(2), &stats) == 64);
    CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0 && unsetenv("FORTIS_KV_TEST_FAIL_ON_CALL") == 0);
    CHECK(stats.frames == 1 && stats.failed_frames == 1);
    for (size_t i = 0; i < kCount; ++i) CHECK(m->items[i].committed == (i < 64));
  }
  {
    World w(directory);
    for (size_t i = 0; i < kCount; ++i) {
      int facts, comparison, demand;
      market_view(w, m->tokens[i], &facts, &comparison, &demand);
      const int expected = i < 64 ? 2 : 1;
      CHECK(facts == expected && comparison == expected && demand == expected);
    }
    // A token refused by the native bounds is rolled out of its frame; the
    // rest of the chunk commits.
    box.Reset();
    build_market(box.arena, m, 10, 3, 0);
    char long_source[300];
    memset(long_source, 's', sizeof(long_source));
    m->patches[4].market_source = {FieldState::kValue, long_source, sizeof(long_source)};
    tw::FrameStats stats{};
    CHECK(tw::RecordMarketRefreshes(*w.data, m->items, 10, at(3), &stats) == 9);
    CHECK(stats.frames == 1 && stats.mutations == 27 && !m->items[4].committed);
    int facts, comparison, demand;
    market_view(w, m->tokens[4], &facts, &comparison, &demand);
    CHECK(facts == 2 && comparison == 2 && demand == 2);
    market_view(w, m->tokens[5], &facts, &comparison, &demand);
    CHECK(facts == 3 && comparison == 3 && demand == 3);
    // The frame bound: 64 tokens with ~120 KiB facts each exceed 4 MiB, so the
    // chunk splits at token boundaries and every group stays whole.
    box.Reset();
    build_market(box.arena, m, 64, 4, 120 * 1024);
    stats = tw::FrameStats{};
    CHECK(tw::RecordMarketRefreshes(*w.data, m->items, 64, at(4), &stats) == 64);
    CHECK(stats.splits >= 1 && stats.frames == stats.splits + 1);
    CHECK(stats.max_frame_bytes <= tw::kMarketFrameBytes + 64 * 3 * KvBatch::kRecordOverhead + 16);
    CHECK(stats.mutations == 64 * 3);
    for (size_t i = 0; i < 64; ++i) {
      market_view(w, m->tokens[i], &facts, &comparison, &demand);
      CHECK(facts == 4 && comparison == 4 && demand == 4);
    }
  }

  remove_directory(directory);
  puts("  market chunks: 64 tokens per frame, torn chunk invisible, refusals and splits per token");
}

// ---- full observation block ----------------------------------------------------------------------

void test_full_block_bound() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  ArenaBox box(16 << 20);
  {
    World w(directory);
    // 71 rows fill the 6-hour block (one per 5-minute bucket) with every
    // optional field present.
    for (int g = 1; g <= 71; ++g) {
      td::Observation o{};
      o.ts = at(g);
      snprintf(o.source, sizeof(o.source), "dexscreener");
      o.has_sampling = o.has_liquidity = o.has_price = o.has_spread = o.has_spread_pools = o.has_top_share =
          o.has_fdv = o.has_market_cap = o.has_volume_h24 = o.has_buys_h24 = o.has_sells_h24 = true;
      o.sampling_seconds = 300;
      o.liquidity = o.price = o.spread = o.spread_pools = o.top_share = o.fdv = o.market_cap = o.volume_h24 =
          o.buys_h24 = o.sells_h24 = 123456789.123456789;
      o.pool_count = 30;
      CHECK(w.data->PutObservation(kNet, kToken, o, at(g) + 1000));
    }
    // The rewrite at the biggest facts document the store accepts.
    tw::FrameStats stats{};
    box.Reset();
    CHECK(record(w, box.arena, 71, 0, &stats, kToken, td::kMaxDocumentBytes - 512));
    CHECK(stats.frames == 1 && stats.mutations == 4);
    CHECK(stats.max_frame_bytes < tw::kLiquidityFrameBytes);
    printf("  full block + max facts frame: %zu bytes (bound %zu, KV frame limit %zu)\n", stats.max_frame_bytes,
           tw::kLiquidityFrameBytes, KvBatch::kMaxFrameBytes);
    const View v = read_view(w);
    CHECK(v.rows == 71 && v.observation == 71 && v.facts == 71 && v.comparison == 71 && v.pair == 71);
  }
  remove_directory(directory);
}

// ---- concurrency ----------------------------------------------------------------------------------

struct Shared {
  World* world;
  int iterations;
  int liquidity_last = 0;
  int market_last = 0;
};

void* liquidity_thread(void* opaque) {
  auto* s = static_cast<Shared*>(opaque);
  ArenaBox box(4 << 20);
  for (int g = 1; g <= s->iterations; ++g) {
    box.Reset();
    CHECK(record(*s->world, box.arena, g, g % 3 == 0 ? 1 : 0));
    s->liquidity_last = g;
  }
  return nullptr;
}

void* market_thread(void* opaque) {
  auto* s = static_cast<Shared*>(opaque);
  ArenaBox box(4 << 20);
  for (int g = 1; g <= s->iterations; ++g) {
    box.Reset();
    tj::Arena& a = box.arena;
    tj::Value* facts = tj::Obj(a);
    CHECK(tj::Set(a, facts, "marketAsOf", tj::Num(a, static_cast<double>(at(g)))));
    ComparisonPatch patch{};
    patch.fiat = {FieldState::kValue, "usd", 3};
    patch.market_as_of = {FieldState::kValue, at(g)};
    if (g % 2) {
      CHECK(tw::WriteThrough(*s->world->data, kNet, kToken, facts, &patch, at(g)));
    } else {
      tj::Value* demand = tj::Obj(a);
      CHECK(tj::Set(a, demand, "lastRefreshedAt", tj::Num(a, static_cast<double>(at(g)))));
      tw::MarketRefresh item{};
      item.network = kNet;
      item.token_id = kToken;
      item.facts = facts;
      item.comparison = &patch;
      item.demand = demand;
      CHECK(tw::RecordMarketRefreshes(*s->world->data, &item, 1, at(g)) == 1);
    }
    s->market_last = g;
  }
  return nullptr;
}

// Single-document writers that take one lock each: they must interleave
// with the transactions without deadlock or lost fields.
void* single_thread(void* opaque) {
  auto* s = static_cast<Shared*>(opaque);
  ArenaBox box(1 << 20);
  TokenComparisonStore store(&s->world->kv);
  for (int i = 0; i < s->iterations; ++i) {
    box.Reset();
    token_store::IdentityPatch identity{};
    identity.symbol = {FieldState::kValue, "USDC", 4};
    CHECK(store.PatchIdentity(kNet, kToken, identity, kBase));
    ComparisonPatch rank{};
    rank.market_cap_rank = {FieldState::kValue, static_cast<double>(i)};
    CHECK(store.PatchComparison(kNet, kToken, rank, kBase));
    tj::Value* fields = tj::Obj(box.arena);
    CHECK(tj::Set(box.arena, fields, "pageTier", tj::Num(box.arena, i)));
    CHECK(s->world->data->PatchDemand(kNet, kToken, fields, kBase));
    tj::Value* profile = tj::Obj(box.arena);
    CHECK(tj::Set(box.arena, profile, "profileAsOf", tj::Num(box.arena, i)));
    CHECK(s->world->data->PatchFacts(kNet, kToken, profile, kBase));
  }
  return nullptr;
}

void test_concurrency() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  constexpr int kIterations = 60;
  {
    World w(directory);
    Shared s{&w, kIterations};
    alarm(120);  // a lock-order inversion would hang here
    pthread_t threads[4];
    CHECK(pthread_create(&threads[0], nullptr, liquidity_thread, &s) == 0);
    CHECK(pthread_create(&threads[1], nullptr, market_thread, &s) == 0);
    CHECK(pthread_create(&threads[2], nullptr, single_thread, &s) == 0);
    CHECK(pthread_create(&threads[3], nullptr, single_thread, &s) == 0);
    for (pthread_t t : threads) CHECK(pthread_join(t, nullptr) == 0);
    alarm(0);
  }
  {
    // No lost update: each writer's last fields survive in every document.
    World w(directory);
    const View v = read_view(w);
    CHECK(v.facts == kIterations && v.comparison == kIterations && v.observation == kIterations &&
          v.pair == kIterations && v.rows == static_cast<size_t>(kIterations));
    ArenaBox box(1 << 20);
    tj::Value* doc = nullptr;
    CHECK(w.data->ReadFacts(kNet, kToken, kReadNow, box.arena, &doc) == td::DataOutcome::kOk);
    CHECK(generation_of(static_cast<int64_t>(tj::Get(doc, "marketAsOf")->number)) == kIterations);
    CHECK(tj::Get(doc, "profileAsOf")->number == kIterations - 1);
    CHECK(w.data->ReadDemand(kNet, kToken, kReadNow, box.arena, &doc) == td::DataOutcome::kOk);
    CHECK(generation_of(static_cast<int64_t>(tj::Get(doc, "lastRefreshedAt")->number)) == kIterations);
    CHECK(tj::Get(doc, "pageTier")->number == kIterations - 1);
    TokenComparisonStore store(&w.kv);
    token_store::Key key{kNet, kToken};
    auto* r = static_cast<comparison::Record*>(malloc(sizeof(comparison::Record)));
    CHECK(store.Read(&key, 1, "usd", kReadNow, r) == token_store::ReadStatus::kOk);
    CHECK(r->market_as_of.present && generation_of(r->market_as_of.ms) == kIterations);
    CHECK(r->symbol.present && strcmp(r->symbol.text, "USDC") == 0);
    free(r);
  }
  remove_directory(directory);
  puts("  concurrent transactions and single-document writers: no deadlock, no lost field");
}

// ---- compaction ----------------------------------------------------------------------------------

struct CompactorState {
  KvStore* kv;
  std::atomic<bool> stop;  // set by the main thread, polled by the compactor
  int runs;
};

void* compactor_thread(void* opaque) {
  auto* s = static_cast<CompactorState*>(opaque);
  while (!s->stop) {
    CHECK(s->kv->Compact());
    ++s->runs;
  }
  return nullptr;
}

// The compactor rewrites the log while transactions commit; every frame it
// copies (snapshot or verbatim tail) must keep the related view whole.
void test_compaction_interleave() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  constexpr int kGenerations = 40;
  CompactorState state{nullptr, false, 0};
  {
    World w(directory);
    state.kv = &w.kv;
    pthread_t compactor;
    CHECK(pthread_create(&compactor, nullptr, compactor_thread, &state) == 0);
    ArenaBox box(4 << 20);
    for (int g = 1; g <= kGenerations; ++g) {
      box.Reset();
      CHECK(record(w, box.arena, g, g % 4 == 0 ? 1 : 0));
      expect_generation(w, g, static_cast<size_t>(g));
    }
    state.stop = true;
    CHECK(pthread_join(compactor, nullptr) == 0);
    CHECK(state.runs > 0);
    CHECK(w.kv.Compact());
  }
  {
    World w(directory);
    expect_generation(w, kGenerations, kGenerations);
  }
  remove_directory(directory);
  printf("  %d compactions interleaved with %d outcome frames: view whole after reopen\n", state.runs, kGenerations);
}

// ---- measurements ------------------------------------------------------------------------------

double now_ms() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// A realistic prior facts document: the fixture-sized sections (market
// 2.8 KiB, liquidity 6 KiB, contract 3 KiB, safety 0.4 KiB) plus a 4 KiB
// profile description, about 17 KiB serialized.
tj::Value* realistic_facts(tj::Arena& a) {
  auto pad = [&](size_t n, char c) {
    char* text = static_cast<char*>(a.Alloc(n + 1));
    memset(text, c, n);
    text[n] = '\0';
    return tj::Str(a, text, n);
  };
  tj::Value* facts = tj::Obj(a);
  const struct { const char* name; size_t bytes; } sections[] = {
      {"market", 2800}, {"profile", 4600}, {"liquidity", 6000}, {"contract", 3000}, {"safety", 400}, {"holders", 90}};
  for (const auto& s : sections) {
    tj::Value* section = tj::Obj(a);
    CHECK(tj::Set(a, section, "blob", pad(s.bytes, 'x')));
    CHECK(tj::Set(a, facts, s.name, section));
  }
  return facts;
}

struct Timing {
  double total = 0, max = 0;
};

void measure() {
  char directory[64];
  make_directory(directory, sizeof(directory));
  auto market_owner = std::make_unique<MarketItems>();
  auto* m = market_owner.get();
  ArenaBox box(64 << 20);
  World w(directory);
  // Seed 175 tokens with realistic facts, demand and a nearly full raw block.
  for (size_t i = 0; i < 175; ++i) {
    snprintf(m->tokens[i], sizeof(m->tokens[i]), "0x%040zx", i + 1);
    box.Reset();
    CHECK(w.data->PatchFacts(kNet, m->tokens[i], realistic_facts(box.arena), kBase));
    for (int g = 1; g <= 70; ++g) {
      td::Observation o{};
      o.ts = at(g);
      snprintf(o.source, sizeof(o.source), "dexscreener");
      o.has_liquidity = o.has_price = o.has_spread = o.has_top_share = o.has_fdv = o.has_market_cap =
          o.has_volume_h24 = o.has_buys_h24 = o.has_sells_h24 = o.has_sampling = true;
      o.liquidity = 1234567.89;
      o.price = 0.99987;
      o.pool_count = 12;
      o.sampling_seconds = 300;
      if (i < 120) CHECK(w.data->PutObservation(kNet, m->tokens[i], o, at(g) + 1000));
    }
  }
  const uint64_t seeded = log_size(directory);
  // Market broad (175) and active (50): the new chunked frames vs. the
  // previous three separate commits per token.
  const size_t counts[] = {175, 50};
  for (size_t count : counts) {
    box.Reset();
    build_market(box.arena, m, count, 71, 2800);
    tw::FrameStats stats{};
    const uint64_t before = log_size(directory);
    double start = now_ms();
    CHECK(tw::RecordMarketRefreshes(*w.data, m->items, count, at(71), &stats) == count);
    const double atomic_ms = now_ms() - start;
    const uint64_t atomic_bytes = log_size(directory) - before;
    start = now_ms();
    for (size_t i = 0; i < count; ++i) {
      CHECK(w.data->PatchFacts(kNet, m->tokens[i], m->items[i].facts, at(71)));
      TokenComparisonStore store(&w.kv);
      CHECK(store.PatchComparison(kNet, m->tokens[i], m->patches[i], at(71)));
      CHECK(w.data->PatchDemand(kNet, m->tokens[i], m->items[i].demand, at(71)));
    }
    const double separate_ms = now_ms() - start;
    printf("market %3zu tokens: %zu frames (was %zu), %zu mutations, %.1f KiB written (max frame %.1f KiB, "
           "staging %.1f KiB), %.1f ms vs %.1f ms separate\n",
           count, stats.frames, count * 3, stats.mutations, atomic_bytes / 1024.0, stats.max_frame_bytes / 1024.0,
           stats.max_staging_bytes / 1024.0, atomic_ms, separate_ms);
  }
  // Liquidity: 120 tokens (the warmer cap), each one outcome frame.
  Timing timing{};
  tw::FrameStats stats{};
  const uint64_t before = log_size(directory);
  for (size_t i = 0; i < 120; ++i) {
    box.Reset();
    Liquidity l{};
    build_liquidity(box.arena, 71, i % 5 == 0 ? 1 : 0, &l, 6000);
    tw::LiquidityOutcome outcome{kNet, m->tokens[i], l.facts, &l.patch, &l.observation};
    const double start = now_ms();
    CHECK(tw::RecordLiquidity(*w.data, outcome, at(71) + 1000, &stats));
    const double took = now_ms() - start;
    timing.total += took;
    if (took > timing.max) timing.max = took;
  }
  printf("liquidity 120 tokens: %zu frames (was 480), %zu mutations, %.1f KiB written (max frame %.1f KiB, "
         "staging %.1f KiB), %.1f ms total, %.2f ms mean, %.2f ms max per outcome\n",
         stats.frames, stats.mutations, (log_size(directory) - before) / 1024.0, stats.max_frame_bytes / 1024.0,
         stats.max_staging_bytes / 1024.0, timing.total, timing.total / 120, timing.max);
  printf("seeded log %.1f MiB\n", seeded / 1048576.0);

  remove_directory(directory);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc > 1 && strcmp(argv[1], "--measure") == 0) {
    measure();
    return 0;
  }
  test_batch();
  test_liquidity_frames();
  test_corrupt_pair_refuses_transaction();
  test_write_failures();
  test_hard_kills();
  test_torn_tails();
  test_write_through();
  test_market_chunks();
  test_full_block_bound();
  test_concurrency();
  test_compaction_interleave();
  puts("native token atomicity test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
