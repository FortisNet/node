#include "native_config.h"
#include "token_runtime.h"
#include "token_facts_service.h"
#include "token_facet_fetcher.h"
#include "token_fact_writer.h"
#include "token_service_internal.h"
#include "token_facts_internal.h"
#include "unit_test_support.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace facts = fortis::tokens::token_facts;
namespace tj = fortis::codec::tj;
using namespace fortis::tokens;
using fortis::testing::Require;

struct Clock final : fortis::base::Clock {
  int64_t NowMs() override { return 1700000000000LL; }
};
struct Executor final : facts::TokenFactExecutor {
  size_t calls = 0;
  uint64_t deadline = 0;
  bool background = false;
  bool throw_after_response = false;
  struct Failure {};
  std::string path;
  void FetchAll(fortis::providers::TokenFetch* fetches, size_t count, uint64_t deadline_ms) override {
    ++calls; deadline = deadline_ms;
    for (size_t i = 0; i < count; ++i) {
      auto& fetch = fetches[i]; path = fetch.path; background = fetch.background;
      const char* body = R"([{"id":"bitcoin","symbol":"btc","name":"Bitcoin","current_price":42}])";
      fetch.body_length = strlen(body);
      fetch.body = fortis::base::AllocateStorage<uint8_t[]>(fetch.body_length);
      Require(fetch.body != nullptr, "allocate fake provider response");
      memcpy(fetch.body.get(), body, fetch.body_length);
      fetch.answered = true; fetch.status = 200;
      if (throw_after_response) throw Failure{};
    }
  }
};
struct Fixture {
  fortis::testing::TemporaryKv storage;
  Clock clock;
  token_swr::Seams seams{&clock, nullptr};
  token_swr::Cache cache{&storage.kv, &seams};
  token_data::TokenDataStore data{&storage.kv};
  Executor executor;
  facts::Env env;
  facts::Work work;
  std::vector<uint8_t> scratch = std::vector<uint8_t>(1024 * 1024 + 64);
  Fixture() {
    Require(data.valid(), "token store initialized");
    env.kv = &storage.kv; env.data = &data; env.swr = &cache; env.executor = &executor;
    work.env = &env; work.arena = tj::GrowingArena(16 * 1024 * 1024);
    work.scratch = scratch.data(); work.scratch_capacity = scratch.size(); work.deadline_ms = 123456;
  }
  ~Fixture() { tj::FreeArena(work.arena); }
};

void queue_model() {
  TokenQueue<int, 16> queue;
  std::deque<int> expected;
  uint32_t random = 1234567;
  for (size_t round = 0; round < 20000; ++round) {
    random = random * 1664525u + 1013904223u;
    if ((random % 3) == 0) {
      const int value = static_cast<int>(round + 1);
      const bool room = expected.size() < 16;
      Require(queue.Push(value) == room, "queue admission matches bounded model");
      if (room) expected.push_back(value);
    } else if ((random % 3) == 1) {
      const int value = expected.empty() ? 0 : expected.front();
      Require(queue.Pop() == value, "FIFO including wrap and empty sentinel");
      if (!expected.empty()) expected.pop_front();
    } else {
      const size_t limit = random % 7;
      int selected[16]{};
      const size_t count = queue.Select([](int value) { return value % 2 == 0; }, selected, limit);
      std::vector<int> want;
      std::deque<int> kept;
      for (int value : expected) {
        if (value % 2 == 0 && want.size() < limit) want.push_back(value);
        else kept.push_back(value);
      }
      Require(count == want.size(), "batch selection respects limit");
      for (size_t i = 0; i < count; ++i) Require(selected[i] == want[i], "selected entries retain arrival order");
      expected = kept;
    }
    Require(queue.size() == expected.size(), "queue size is consistent after compaction");
  }
  while (!expected.empty()) { Require(queue.Pop() == expected.front(), "retained FIFO order"); expected.pop_front(); }
}

void foreground_admission() {
  facts::Env env;
  TokenRuntimeControl control;
  TokenWorkspaces workspaces(env);
  TokenRequestWorker worker(workspaces, control);
  fortis::http::HttpResponseHandoff handoff;
  Require(worker.BindHandoff(handoff) && !worker.BindHandoff(handoff), "handoff binds once before startup");
  for (size_t i = 0; i < 16; ++i) {
    auto* task = fortis::base::New<TokenTask>(); Require(task != nullptr, "allocate task");
    Require(worker.Submit(task), "sixteen foreground admissions");
  }
  auto* full = fortis::base::New<TokenTask>(); Require(full != nullptr, "allocate rejected task");
  Require(!worker.Submit(full), "full queue refuses without taking ownership");
  fortis::base::Delete(full);
  control.Stop();
  auto* stopped = fortis::base::New<TokenTask>(); Require(stopped != nullptr, "allocate stop candidate");
  Require(!worker.Submit(stopped), "stop refuses new task ownership");
  fortis::base::Delete(stopped);
  worker.DiscardPending();
}

void revalidation_admission() {
  Fixture fixture;
  TokenRuntimeControl control;
  TokenWorkspaces workspaces(fixture.env);
  TokenRevalidator revalidator(workspaces, control, fixture.env);
  facts::Revalidation task;
  task.kind = facts::RevalidationKind::kResolution; task.key.Assign("identity");
  for (size_t i = 0; i < 64; ++i) Require(revalidator.Enqueue(task), "sixty-four revalidation admissions");
  Require(!revalidator.Enqueue(task) && !revalidator.WaitIdle(1), "full admission and nonidle queue");
  control.Stop();
  Require(!revalidator.Enqueue(task), "stop refuses revalidation and frees rejected copy");
  revalidator.DiscardPending();
  Require(revalidator.WaitIdle(1), "discarded queue is idle");
}

void worker_lifetimes() {
  facts::Env env;
  for (size_t iteration = 0; iteration < 30; ++iteration) {
    TokenRuntimeControl control;
    TokenWorkspaces workspaces(env);
    TokenRequestWorker worker(workspaces, control);
    TokenRevalidator revalidator(workspaces, control, env);
    pthread_attr_t attributes;
    Require(pthread_attr_init(&attributes) == 0, "initialize thread attributes");
    pthread_attr_setstacksize(&attributes, 512 * 1024);
    const bool requests = worker.Start(&attributes);
    const bool background = requests && revalidator.Start(&attributes);
    const bool refused_request_restart = !worker.Start(&attributes);
    const bool refused_background_restart = !revalidator.Start(&attributes);
    pthread_attr_destroy(&attributes);
    control.Stop();
    worker.Join(); revalidator.Join();
    worker.DiscardPending(); revalidator.DiscardPending();
    Require(requests && background && refused_request_restart && refused_background_restart, "startup, repeat refusal preserves handles, and stop joins");
  }
}

void fact_writer_contract() {
  Fixture f;
  facts::TokenFactWriter writer(f.work);
  auto* profile = tj::Obj(f.work.arena);
  Require(profile && tj::Set(f.work.arena, profile, "source", tj::Str(f.work.arena, "coingecko")), "profile fixture");
  auto change = writer.Change(profile, true, 1234, "fallback", "coin");
  Require(change.present && !strcmp(change.source, "coingecko") && !strcmp(change.source_id, "coin"), "embedded source wins and owns coin provenance");
  auto absent = writer.Change(nullptr, false, 0, "other", "ignored");
  Require(absent.present && !absent.source_id, "explicit null change differs from an omitted field");
  facts::TokenFactChange empty;
  Require(writer.Write("eth-mainnet", "token", empty, change, empty, empty, absent), "one write-through operation");
  tj::Value* document = nullptr;
  Require(f.data.ReadFacts("eth-mainnet", "token", f.clock.NowMs(), f.work.arena, &document) == token_data::DataOutcome::kOk, "read durable facts");
  Require(tj::Get(document, "profileAsOf")->number == 1234 && tj::StringEquals(tj::Get(document, "profileSourceId"), "coin"), "timestamp and source id persisted");
  Require(tj::Get(document, "safetyAsOf")->number == static_cast<double>(f.clock.NowMs()) && tj::Get(document, "safety")->type == tj::Type::kNull, "missing timestamp samples clock and explicit null is preserved");
}

void bound_fetcher() {
  Fixture f;
  fortis::networks::TokenNetwork network;
  fortis::networks::TokenRef ref;
  const fortis::networks::NativeNetworkConfig* seed = nullptr;
  Require(facts::Resolve("eth-mainnet", "cg:bitcoin", &network, &seed, &ref) == facts::FactsOutcome::kOk, "resolve bound token");
  facts::internal::FacetInputs input; input.network = &network; input.ref = &ref; input.seed = seed; input.coin_id = "bitcoin";
  facts::TokenFacetFetcher fetcher(f.work, input);
  auto fetch = std::make_unique<fortis::providers::TokenFetch>();
  tj::Value* value = nullptr; bool busy = false;
  f.work.background = true;
  Require(fetcher.Prepare(facts::kMarket, fetch.get(), &value, &busy) == facts::FacetFetchPlan::kRequest, "market request prepared for bound coin");
  f.env.executor->FetchAll(fetch.get(), 1, f.work.deadline_ms);
  Require(fetcher.Interpret(facts::kMarket, *fetch, &value), "prepared request interpreted by same context");
  fortis::providers::TokenFetchRelease(fetch.get());
  Require(f.executor.calls == 1 && f.executor.deadline == 123456 && f.executor.background && f.executor.path.find("ids=bitcoin") != std::string::npos, "typed executor retains id, deadline and background priority");
  const auto* price = tj::Get(value, "current_price");
  Require(tj::IsNumber(price) && price->number == 42, "provider body retains its market price for later composition");
}

void facts_workflow() {
  Fixture f;
  facts::TokenFactsService service(f.work);
  auto first = std::make_unique<facts::Facts>();
  Require(service.Compute("eth-mainnet", "cg:bitcoin", nullptr, facts::FacetBit(facts::kMarket), first.get()) == facts::FactsOutcome::kOk, "class coordinates cold facts");
  Require(f.executor.calls == 1 && first->resolution.has_coin, "identity and selected facet compose with fake provider");
  auto next = std::make_unique<facts::Facts>();
  Require(service.Compute("eth-mainnet", "cg:bitcoin", nullptr, facts::FacetBit(facts::kMarket), next.get()) == facts::FactsOutcome::kOk && f.executor.calls == 1, "second read uses durable SWR without provider");
}

void provider_exception_lifetime() {
  Fixture f;
  f.executor.throw_after_response = true;
  facts::TokenFactsService service(f.work);
  auto result = std::make_unique<facts::Facts>();
  bool propagated = false;
  try {
    service.Compute("eth-mainnet", "cg:bitcoin", nullptr, facts::FacetBit(facts::kMarket), result.get());
  } catch (const Executor::Failure&) {
    propagated = true;
  }
  Require(propagated, "provider exception after receiving an owned response propagates");
  // ASan/LSan also checks that unwinding released the response before the
  // fixture frees the arena containing its fetch records.
}

void scheduler_manual() {
  Fixture f;
  auto config = std::make_unique<fortis::config::NativeConfig>();
  fortis::providers::TokenProviderOwner owner(*config, nullptr);
  token_jobs::TokenStats stats;
  fortis::prices::PriceSeriesStore series(f.storage.kv);
  TokenRuntimeControl control;
  TokenWorkspaces workspaces(f.env);
  TokenServiceSeams seams; seams.manual_jobs = true;
  TokenJobScheduler scheduler({workspaces, control, stats, f.data, series, f.cache,
      f.storage.kv, owner, *config, seams});
  Require(scheduler.Start(nullptr), "manual scheduler starts no thread or provider");
  char summary[1024];
  Require(scheduler.RunJob(TokenService::Job::kStatsFlush, summary, sizeof(summary)), "serialized manual stats job");
  Require(strstr(summary, "\"failed\":false") && strstr(summary, "\"written\":0"), "manual job summary contract");
  control.Stop(); scheduler.Join();
  struct LateJitter final : fortis::base::JitterSource {
    bool Sample(double* value) override { *value = 0.99; return true; }
  } jitter;
  TokenRuntimeControl scheduled_control;
  TokenServiceSeams scheduled_seams; scheduled_seams.jitter = &jitter;
  TokenJobScheduler scheduled({workspaces, scheduled_control, stats, f.data, series, f.cache,
      f.storage.kv, owner, *config, scheduled_seams});
  pthread_attr_t attributes;
  Require(pthread_attr_init(&attributes) == 0, "scheduler attributes");
  pthread_attr_setstacksize(&attributes, 512 * 1024);
  const bool started = scheduled.Start(&attributes);
  const bool refused_restart = !scheduled.Start(&attributes);
  pthread_attr_destroy(&attributes);
  scheduled_control.Stop(); scheduled.Join();
  Require(started && refused_restart, "scheduler repeat refusal preserves join ownership");
}
}  // namespace
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("bounded FIFO and stable batch model", queue_model);
  suite.Run("foreground capacity and ownership", foreground_admission);
  suite.Run("revalidation capacity and stop", revalidation_admission);
  suite.Run("thirty runtime startup/stop cycles", worker_lifetimes);
  suite.Run("durable fact change and write contract", fact_writer_contract);
  suite.Run("bound fetch/interpret and typed executor", bound_fetcher);
  suite.Run("facts coordination and cached reread", facts_workflow);
  suite.Run("provider response ownership during exceptions", provider_exception_lifetime);
  suite.Run("manual scheduler execution", scheduler_manual);
  return suite.Finish("token_group1_test");
}
