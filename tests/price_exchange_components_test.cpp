#include "native_config.h"
#include "spot_provider_owner_internal.h"
#include "history_query_service.h"
#include "price_history_backfill.h"
#include "price_history_view.h"
#include "cross_chain_components.h"
#include "chain_transport.h"
#include "unit_test_support.h"
#include <array>
#include <atomic>
#include <memory>
#include <type_traits>
#include <string>
#include <vector>

namespace {
namespace pr = fortis::prices;
namespace cc = fortis::exchange::cross_chain;
namespace tj = fortis::codec::tj;
using fortis::testing::Require;
static_assert(!std::is_copy_constructible<pr::SpotRequestProcessor>::value && !std::is_move_constructible<pr::SpotRequestProcessor>::value, "SpotRequestProcessor keeps one runtime/state identity");
static_assert(!std::is_copy_constructible<pr::FlightTable>::value && !std::is_move_constructible<pr::FlightTable>::value, "FlightTable keeps one runtime/state identity");
static_assert(!std::is_copy_constructible<pr::SpotBatchScheduler>::value && !std::is_move_constructible<pr::SpotBatchScheduler>::value, "SpotBatchScheduler keeps one runtime/state identity");
static_assert(!std::is_copy_constructible<pr::JupiterSpotBatch>::value && !std::is_move_constructible<pr::JupiterSpotBatch>::value, "JupiterSpotBatch keeps one runtime/state identity");
static_assert(!std::is_copy_constructible<pr::RobinhoodSpotBatch>::value && !std::is_move_constructible<pr::RobinhoodSpotBatch>::value, "RobinhoodSpotBatch keeps one runtime/state identity");
struct Clock final : fortis::base::Clock {
  std::atomic<uint64_t> monotonic{1000};
  int64_t NowMs() override { return 1700000000000LL; }
  uint64_t MonotonicMs() override { return monotonic.load(std::memory_order_relaxed); }
};
tj::Value* Parse(tj::Arena& arena,const char* text) {
  auto* v=tj::Parse(arena,reinterpret_cast<const uint8_t*>(text),strlen(text));Require(v!=nullptr,"fixture JSON");return v;
}
struct Completion {
  size_t count=0,length=0;pr::SpotProviderOwnerResultStatus status{};char body[4096]{};
  static void Done(void* opaque,const pr::SpotProviderOwnerResult& r) {
    auto& self=*static_cast<Completion*>(opaque);++self.count;self.status=r.status;
    self.length=r.body_length<sizeof(self.body)-1?r.body_length:sizeof(self.body)-1;
    if (self.length) { memcpy(self.body,r.body,self.length); }
    self.body[self.length]='\0';
  }
};
struct SpotFixture {
  fortis::testing::TemporaryKv storage;Clock clock;
  pr::PriceSpotCache cache{storage.kv};pr::PriceSeriesStore series{storage.kv};
  fortis::config::NativeConfig config{};
  std::unique_ptr<pr::SpotRuntime> runtime=std::make_unique<pr::SpotRuntime>(cache,series,config,nullptr,clock);
  SpotFixture() {runtime->clock=&clock;runtime->owner_started=true;}
  ~SpotFixture() {
    runtime->owner_started=false;
    if(runtime->adapter){runtime->adapter->BeginShutdown();runtime->adapter->Poll();runtime->adapter->JoinWorkers();}
  }
  pr::SpotProviderOwnerSubmission Submit(Completion& out) {
    const std::string_view id="bitcoin";
    return runtime->requests.Submit(&id,1,"usd",pr::SpotProviderOwnerFormat::kJson,2000,
        Completion::Done,&out,pr::SpotSubmitMode::kForeground,nullptr,0);
  }
  void Answer() {
    fortis::providers::CoinGeckoSpotResult quotes{};quotes.count=1;
    snprintf(quotes.quotes[0].id,sizeof(quotes.quotes[0].id),"bitcoin");quotes.quotes[0].price=42;
    runtime->flight_table.CompleteFlight(0,true,&quotes,clock.NowMs());
  }
};
void shared_flights() {
  SpotFixture f;Completion a,b;Require(f.Submit(a).ticket()!=nullptr && f.Submit(b).ticket()!=nullptr,"admit two requests");
  f.runtime->requests.ProcessQueued(0);f.runtime->requests.ProcessQueued(1);
  const auto generation=f.runtime->flight_table.at(0).generation;
  Require(f.runtime->flight_table.at(0).waiter_count==2 && f.runtime->flight_table.at(0).foreground_waiters==2,"shared foreground flight");
  f.Answer();f.Answer();Require(a.count==1 && b.count==1 && a.status==pr::SpotProviderOwnerResultStatus::kComplete,"one completion per waiter");
  fortis::testing::JsonArena arena;const auto* value=Parse(arena.get(),a.body);
  Require(tj::Get(tj::Get(tj::Get(value,"prices"),"bitcoin"),"price")->number==42,"shared provider quote serialized");
  Require(f.runtime->flight_table.at(0).state==pr::SpotTypes::FlightState::kFree,"flight released after fanout");
  Completion c;Require(f.Submit(c).ticket()!=nullptr,"reuse request slot");f.runtime->requests.ProcessQueued(0);
  Require(f.runtime->flight_table.at(0).generation!=generation,"reuse advances generation");f.Answer();Require(c.count==1,"reused slot completion");
}
void cancel_waiter() {
  SpotFixture f;Completion a,b;auto first=f.Submit(a);Require(first.ticket()!=nullptr && f.Submit(b).ticket()!=nullptr,"shared cancel setup");
  f.runtime->requests.ProcessQueued(0);f.runtime->requests.ProcessQueued(1);
  Require(f.runtime->requests.Cancel(first.ticket()->request_id),"cancel one waiter");f.runtime->requests.CancelOrExpire(1000,false);
  Require(a.count==1 && a.status==pr::SpotProviderOwnerResultStatus::kCancelled && b.count==0,"cancel only its callback");
  Require(f.runtime->flight_table.at(0).waiter_count==1,"other waiter retains flight");f.Answer();Require(b.count==1 && b.status==pr::SpotProviderOwnerResultStatus::kComplete,"remaining waiter completes");
}
void deadline_fallback() {
  SpotFixture f;Completion out;Require(f.Submit(out).ticket()!=nullptr,"deadline setup");f.runtime->requests.ProcessQueued(0);
  f.runtime->requests.CancelOrExpire(2000,false);
  Require(out.count==1 && out.status==pr::SpotProviderOwnerResultStatus::kComplete,"planned deadline completes through missing/floor policy");
  Require(std::string(out.body).find("bitcoin")!=std::string::npos && f.runtime->flight_table.at(0).state==pr::SpotTypes::FlightState::kFree,"deadline releases flight");
}
void flight_capacity() {
  SpotFixture f;const auto kind=fortis::providers::PriceIdKind::kCoinGecko;
  auto& table=f.runtime->flight_table;
  Require(table.FindOrCreateFlight("usd","bitcoin",kind,true)==0,"first background flight");
  f.clock.monotonic=1010;
  Require(table.FindOrCreateFlight("usd","ethereum",kind,true)==1 && table.at(1).window_deadline_ms==1020,
          "new peer shares the open twenty millisecond window");
  Require(table.FindOrCreateFlight("usd","bitcoin",kind,true)==0,"background coalescing");
  for(size_t i=2;i<pr::kMaxBackgroundOnlyFlights;++i) {
    char id[40];snprintf(id,sizeof(id),"asset-%zu",i);
    Require(table.FindOrCreateFlight("usd",id,kind,true)==i,"bounded background admission");
  }
  Require(table.FindOrCreateFlight("usd","over-cap",kind,true)==pr::kMaxSharedFlights,"background cap");
  Require(table.FindOrCreateFlight("usd","foreground",kind,false)==pr::kMaxBackgroundOnlyFlights,"foreground reserve survives background saturation");
}
struct SparseRepository final : pr::PriceSeriesRepository {
  mutable size_t calls=0;
  pr::PriceSeriesReadStatus Range(pr::PriceSeriesInterval,const char*,const char*,int64_t from,int64_t,int64_t,
      pr::PriceSeriesCandle* out,size_t cap,size_t* count) const override {
    ++calls;*count=0;if(!cap)return pr::PriceSeriesReadStatus::kCapacityExceeded;
    out[0]={};out[0].ts=from;*count=1;return pr::PriceSeriesReadStatus::kFound;
  }
  pr::PriceSeriesReadStatus ReadHistoryCoverage(pr::PriceSeriesInterval,const char*,const char*,int64_t,pr::HistoryCoverage*) const override {
    return pr::PriceSeriesReadStatus::kMissing;
  }
};
void chunked_repository() {
  SparseRepository repo;std::array<pr::PriceSeriesCandle,4> rows{};size_t count=0;
  constexpr int64_t width=3600000;
  Require(repo.ReadWindow(pr::PriceSeriesInterval::k1h,"bitcoin",0,7200*width,width,0,rows.data(),4,&count)==pr::PriceSeriesReadStatus::kFound,
          "chunked repository read");
  Require(repo.calls==4 && count==4 && rows[1].ts==2400*width && rows[3].ts==7200*width,"inclusive nonoverlapping chunks");
  rows[3].close=99;
  Require(repo.ReadWindow(pr::PriceSeriesInterval::k1h,"bitcoin",0,7200*width,width,0,rows.data(),3,&count)==pr::PriceSeriesReadStatus::kCapacityExceeded,
          "overflow probe reports capacity without writing outside storage");
  Require(rows[3].close==99,"overflow sentinel retained");
}
struct Repository final : pr::PriceSeriesRepository {
  bool filled=false,broken=false;mutable size_t reads=0;
  pr::PriceSeriesReadStatus Range(pr::PriceSeriesInterval,const char*,const char*,int64_t from,int64_t to,int64_t,
      pr::PriceSeriesCandle* out,size_t cap,size_t* count) const override {
    ++reads;*count=0;if(broken)return pr::PriceSeriesReadStatus::kIoError;
    for (int i=0;i<3;++i) {
      if (i == 1 && !filled) continue;
      const int64_t ts = i * 3600000LL;
      if (ts < from || ts > to) continue;
      if(*count==cap)return pr::PriceSeriesReadStatus::kCapacityExceeded;
      out[*count]={};out[*count].ts=ts;out[(*count)++].close=i+1;
    }
    return *count?pr::PriceSeriesReadStatus::kFound:pr::PriceSeriesReadStatus::kMissing;
  }
  pr::PriceSeriesReadStatus ReadHistoryCoverage(pr::PriceSeriesInterval,const char*,const char*,int64_t,pr::HistoryCoverage*) const override {
    return pr::PriceSeriesReadStatus::kMissing;
  }
};
struct Gaps final : pr::HistoryGapSource {
  Repository& repo;bool busy=false,defer=false;size_t submitted=0,cancelled=0;pr::PriceHistoryOwnerCompletionFn callback=nullptr;void* context=nullptr;
  explicit Gaps(Repository& r):repo(r){}
  pr::PriceHistoryOwnerSubmitStatus Submit(const char*,const char*,const char*,pr::PriceSeriesInterval,int64_t from,int64_t to,
      uint64_t,pr::PriceHistoryOwnerCompletionFn done,void* opaque,uint64_t* id) override {
    Require(from==3600 && to==7200,"exact uncovered hourly gap");++submitted;if(busy)return pr::PriceHistoryOwnerSubmitStatus::kBusy;
    *id=7;callback=done;context=opaque;
    if(!defer){repo.filled=true;pr::PriceHistoryOwnerResult r{};r.status=pr::PriceHistoryOwnerResultStatus::kComplete;done(opaque,r);}
    return pr::PriceHistoryOwnerSubmitStatus::kAccepted;
  }
  bool Cancel(uint64_t id) override {
    Require(id==7,"cancel accepted request");++cancelled;pr::PriceHistoryOwnerResult r{};r.status=pr::PriceHistoryOwnerResultStatus::kCancelled;callback(context,r);return true;
  }
};
void query_history() {
  Repository repo;Gaps gaps(repo);pr::HistoryQueryService service(repo,gaps);std::vector<pr::PriceSeriesCandle> rows(4);
  pr::PriceHistorySeriesQuery query{};query.price_id="bitcoin";query.from_seconds=0;query.to_seconds=7200;query.hourly=true;
  query.deadline_monotonic_ms=fortis::base::SystemClock().MonotonicMs()+10000;query.candles=rows.data();query.capacity=rows.size();
  pr::PriceHistorySeriesResult result;
  Require(service.ReadSeries(query,&result)==pr::PriceHistorySeriesStatus::kOk && result.count==3,"fill then reread fake repository");
  Require(result.cached==2 && !result.hit && result.gaps_planned==1 && result.gaps_filled==1 && gaps.submitted==1,"initial provenance and completed gap");
  repo.filled=false;gaps.busy=true;Require(service.ReadSeries(query,&result)==pr::PriceHistorySeriesStatus::kOk && result.count==2 && result.hit,"busy backfill retains cached points");
  repo.broken=true;Require(service.ReadSeries(query,&result)==pr::PriceHistorySeriesStatus::kUnavailable,"storage failures remain unavailable");
}
void cancelled_history() {
  Repository repo;Gaps gaps(repo);gaps.defer=true;pr::HistoryQueryService service(repo,gaps);pr::PriceSeriesCandle rows[4]{};
  pr::PriceHistorySeriesQuery query{};query.price_id="bitcoin";query.from_seconds=0;query.to_seconds=7200;query.hourly=true;
  query.deadline_monotonic_ms=fortis::base::SystemClock().MonotonicMs()+10000;query.candles=rows;query.capacity=4;
  query.cancelled=[](void*){return true;};pr::PriceHistorySeriesResult result;
  Require(service.ReadSeries(query,&result)==pr::PriceHistorySeriesStatus::kUnavailable && gaps.cancelled==1,"cancellation waits for owned callback before stack exit");
}
void aggregate_candles() {
  auto workspace=std::make_unique<pr::PriceHistoryWorkspace>();const char* text=R"({"prices":[[0,2],[3600000,3]],"total_volumes":[]})";
  Require(pr::CandleAggregator::Parse(reinterpret_cast<const uint8_t*>(text),strlen(text),workspace.get())==pr::PriceHistoryParseStatus::kOk,"parse history");
  Require(workspace->hourly_count==2 && workspace->daily_count==1 && workspace->daily[0].candle.close==3,"two-resolution buckets");
  pr::PriceHistoryCandles candles;size_t hourly=0,daily=0;candles.count=pr::CandleAggregator::BuildChunk(*workspace,&hourly,&daily,candles.items);
  Require(candles.count==3 && hourly==2 && daily==1,"mixed chunk advances both cursors");
  fortis::testing::TemporaryKv storage;pr::PriceSeriesStore store(storage.kv);
  Require(pr::CandleAggregator::Store(store,"usd","bitcoin",candles,86400000)==pr::PriceSeriesStoreStatus::kStored,"persist mixed resolutions");
  pr::StoredPriceSeriesRepository repository(store);pr::PriceSeriesCandle out[4];size_t count=0;
  Require(repository.ReadWindow(pr::PriceSeriesInterval::k1h,"bitcoin",0,3600000,3600000,86400000,out,4,&count)==pr::PriceSeriesReadStatus::kFound && count==2 && out[1].close==3,"repository reads stored aggregation");
  pr::history_batch::HistoryWindow window{3600,0,20};uint8_t bitmap[3]{};
  for(size_t bit=0;bit<20;bit+=2) bitmap[bit/8]|=static_cast<uint8_t>(1u<<(bit%8));
  int64_t planned[8][2]{};Require(pr::HistoryBackfillPlanner::Plan(window,bitmap,nullptr,planned)==1 && planned[0][0]==0 && planned[0][1]==72000,"over-eight gaps collapse to full aligned window");
}
struct Exchange final : cc::ExchangeProvider {
  const char* id;mutable size_t quotes=0,statuses=0;mutable bool prepared=false;
  explicit Exchange(const char* name):id(name){}
  const char* Id() const override{return id;}const char* Name() const override{return id;}
  const char* Website() const override{return "https://example.com";}const char* Custody() const override{return "non-custodial";}
  cc::Outcome Quote(cc::ProviderContext&,cc::Arena& arena,const cc::QuoteRequest& request,cc::Value** out) const override {
    ++quotes;prepared=request.prepare;*out=Parse(arena,R"([{"id":"first","amountOutMin":"10000000000000000001","amountOut":"10000000000000000002","feeUsd":null,"etaSeconds":null}])");return cc::Outcome::Ok();
  }
  cc::Outcome Status(cc::ProviderContext&,cc::Arena& arena,const cc::StatusRequest&,cc::Value** out) const override {
    ++statuses;*out=Parse(arena,R"({"status":"pending"})");return cc::Outcome::Ok();
  }
};
void exchange_workflows() {
  Exchange a("z-provider"),b("a-provider");const cc::ExchangeProvider* providers[]{&a,&b};cc::ExchangeProviderRegistry registry(providers,2);
  Require(registry.Index("a-provider",10)==1 && &registry.At(99)==&b,"registry lookup and last-index fallback");size_t order[2];registry.ById(order);Require(order[0]==1 && order[1]==0,"directory order");
  cc::CrossChainQuoteService quotes(registry);cc::DepositTracker deposits(registry);fortis::testing::JsonArena arena;cc::ProviderContext context;cc::QuoteRequest request;cc::Value* results[2]{};
  Require(quotes.Quote(0,context,arena.get(),request,&results[0]).failure==cc::Failure::kNone && !a.prepared,"passive quote stays passive");
  request.prepare=true;quotes.Quote(1,context,arena.get(),request,&results[1]);Require(b.prepared,"explicit preparation propagated");
  tj::Set(arena.get(),results[1]->items[0],"id",tj::Str(arena.get(),"second"));
  const uint8_t indices[]{0,1};const bool configured[]{true,true};cc::Outcome outcomes[2]{};cc::QuoteInputs inputs{2,indices,configured,outcomes,results};cc::Value* output=nullptr;cc::ProviderSettings settings;
  Require(quotes.Aggregate(arena.get(),settings,inputs,&output),"aggregate fake providers");
  Require(cc::Is(tj::Get(output,"bestRouteId"),"first") && tj::Get(tj::Get(output,"routes")->items[1],"rank")->number==2,"big-integer ties preserve selection order");
  outcomes[0].failure=cc::Failure::kLaneBusy;Require(quotes.Aggregate(arena.get(),settings,inputs,&output) && cc::Is(tj::Get(output,"bestRouteId"),"second"),"failed provider isolated from successful quote");
  cc::StatusRequest status;cc::Value* tracked=nullptr;Require(deposits.Track(1,context,arena.get(),status,&tracked).failure==cc::Failure::kNone && b.statuses==1 && cc::Is(tj::Get(tracked,"status"),"pending"),"typed deposit status dispatch");
  Require(context.near_catalog==&deposits.Catalog(),"tracker owns catalog context");deposits.Catalog().arena=tj::GrowingArena(2*1024*1024);deposits.Catalog().loaded=true;
  Parse(deposits.Catalog().arena,"[]");
  deposits.Reset();deposits.Reset();
}
}
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("shared spot flights and generations",shared_flights);
  suite.Run("cancel shared spot waiter",cancel_waiter);
  suite.Run("spot deadline fallback",deadline_fallback);
  suite.Run("flight window/background capacity",flight_capacity);
  suite.Run("chunked history capacity",chunked_repository);
  suite.Run("history reader gaps and provenance",query_history);
  suite.Run("history cancellation callback lifetime",cancelled_history);
  suite.Run("candle aggregation/repository/planner",aggregate_candles);
  suite.Run("quote aggregation and deposit tracking",exchange_workflows);
  return suite.Finish("price_exchange_components_test");
}
