#include "native_config.h"
#include "chain_family_drivers.h"
#include "chain_registry.h"
#include "chain_transport_policy.h"
#include "wallet_components.h"
#include "address_facets_internal.h"
#include "unit_test_support.h"
#include <memory>
#include <string>
#include <vector>

namespace {
namespace ch = fortis::chains;
namespace wa = fortis::wallet::address;
namespace tj = fortis::codec::tj;
using fortis::testing::Require;
using fortis::testing::JsonArena;
using fortis::config::NativeConfig;

tj::Value* Parse(tj::Arena& arena, const char* text) {
  auto* value = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), strlen(text));
  Require(value != nullptr, "parse fixture"); return value;
}
struct Clock final : fortis::base::Clock {
  int64_t now = 1700000000000LL;
  int64_t NowMs() override { return now; }
};
struct Transport final : ch::ChainTransport {
  std::vector<std::string> methods;
  bool outage=false;
  void Run(ch::Call* calls, size_t count, tj::Arena& arena, uint64_t deadline) override {
    Require(deadline >= ch::ChainMonotonicMs(), "bounded operation deadline reaches transport");
    for (size_t i = 0; i < count; ++i) {
      auto& c = calls[i]; const std::string method = c.method ? c.method : c.path ? c.path : "";
      methods.push_back(method);
      if (outage) {c.failure=ch::CallFailure::kTransport;continue;}
      c.failure = ch::CallFailure::kNone; c.settled_rank = static_cast<uint32_t>(i+1);
      const char* body = "null";
      if (method == "account_info") body = R"({"error":"actNotFound"})";
      else if (method == "eth_getTransactionCount") body = R"("0x2")";
      else if (method == "eth_getCode") body = R"("0x")";
      else if (method == "getSignatureStatuses") body = R"({"value":[null]})";
      else if (c.kind == ch::CallKind::kGet) body = R"({"data":[]})";
      c.value = Parse(arena, body);
    }
  }
};
struct Driver final : ch::ChainDriver {
  mutable size_t calls = 0;
  bool enabled = true;
  ch::Family family() const override { return ch::Family::kEvm; }
  bool Registered(const fortis::config::ProviderRuntimeConfig&) const override { return enabled; }
  bool Balances(ch::Context& ctx, const ch::Network&, const char*, const char* const*, size_t,
                tj::Value** out, ch::Error*) const override {
    ++calls;
    *out = Parse(*ctx.arena,R"({"native":{"balanceRaw":"2000000000000000000","decimals":18,"symbol":"ETH","source":"fake"},"tokens":[]})");
    return true;
  }
};
struct Source final : ch::TransferSource {
  mutable size_t calls = 0;
  bool enabled = true, supported = true, with_record = false, unavailable = false;
  ch::Family family() const override { return ch::Family::kEvm; }
  const char* Key() const override { return "fake-cursor"; }
  bool Registered(const fortis::config::ProviderRuntimeConfig&) const override { return enabled; }
  bool Supports(const fortis::config::ProviderRuntimeConfig&, const ch::Network&) const override { return supported; }
  bool Fetch(ch::Context& ctx, const ch::Network&, const char*, const tj::Value*, double,
              ch::SourceResult* out, ch::Error*) const override {
    ++calls;out->state=Parse(*ctx.arena,R"({"cursor":2})");out->backfill_complete=true;
    if (unavailable) {out->unavailable=true;snprintf(out->reason,sizeof(out->reason),"coverage gap");return true;}
    out->records=tj::Arr(*ctx.arena);
    if (with_record) {
      auto* r=ch::NewTransfer(*ctx.arena);
      for (auto item : {std::pair{"recordId","fake/0"}, {"status","confirmed"}, {"tokenId","native"},
                        {"hash","0xabc"}, {"direction","in"}, {"txType","transfer"}, {"amountRaw","250"}})
        Require(tj::Set(*ctx.arena,r,item.first,tj::Str(*ctx.arena,item.second)),"transfer fields");
      tj::Set(*ctx.arena,r,"ts",tj::Num(*ctx.arena,static_cast<double>(ctx.Now()-1000)));
      tj::Set(*ctx.arena,r,"amountDecimals",tj::Num(*ctx.arena,2));
      Require(tj::Push(*ctx.arena,out->records,r),"transfer row");
    }
    return true;
  }
};
struct Spot final : wa::WalletSpotReader {
  size_t calls=0;
  tj::Value* Read(tj::Arena& arena,const char* const* ids,size_t count) override {
    ++calls;Require(count==1 && !strcmp(ids[0],"ethereum"),"deduplicated native price ID");
    return Parse(arena,R"({"ethereum":3})");
  }
};
struct Series final : wa::WalletSeriesReader {
  bool available=true;
  mutable size_t calls=0;
  fortis::prices::PriceSeriesReadStatus Range(fortis::prices::PriceSeriesInterval,
      const char* fiat,const char* source,int64_t from,int64_t to,int64_t,
      fortis::prices::PriceSeriesCandle* output,size_t capacity,size_t* count) const override {
    ++calls;Require(!strcmp(fiat,"usd") && !strcmp(source,"ethereum") && capacity>0 && to>=from,"history read bounds");
    *count=available ? 1 : 0;
    if (available) {output[0]={};output[0].ts=1699999999000LL;output[0].close=7;}
    return available ? fortis::prices::PriceSeriesReadStatus::kFound : fortis::prices::PriceSeriesReadStatus::kMissing;
  }
};
bool News(void*,int64_t,tj::Arena& arena,tj::Value** out) {
  *out=Parse(arena,R"({"items":[],"asOf":null,"stale":false})");return true;
}
struct Fixture {
  fortis::testing::TemporaryKv storage;
  Clock clock;
  NativeConfig config{};
  ch::Catalog catalog;
  Transport transport;
  Driver driver;
  Source source;
  Spot spot;
  Series series;
  const ch::ChainDriver* drivers[1]{&driver};
  const ch::TransferSource* sources[1]{&source};
  ch::ChainRegistry registry{drivers,1,sources,1,nullptr,0};
  wa::Store store{&storage.kv};
  fortis::tokens::token_swr::Seams seams{&clock,nullptr};
  fortis::tokens::token_swr::Cache cache{&storage.kv,&seams};
  wa::ChangeTracker changes;
  wa::FacetDeps deps{};
  ch::Network network;
  Fixture() {
    Require(catalog.Init(config) && catalog.Resolve("eth-mainnet",11,&network),"network catalog");
    deps.config=&config;deps.catalog=&catalog;deps.transport=&transport;deps.chains=&registry;
    deps.store=&store;deps.swr=&cache;deps.kv=&storage.kv;deps.series=&series;deps.spot=&spot;
    deps.changes=&changes;deps.clock=&clock;deps.news_feed=News;
  }
};
void registry_contract() {
  Fixture f;Require(f.registry.DriverFor(f.config,ch::Family::kEvm)==&f.driver,"injected driver");
  f.driver.enabled=false;Require(!f.registry.DriverFor(f.config,ch::Family::kEvm),"registration gates driver");
  Require(!f.registry.ViewFor(ch::Family::kEvm),"optional view absent");
  Source fallback;const ch::TransferSource* sources[]{&f.source,&fallback};
  ch::ChainRegistry ordered(nullptr,0,sources,2,nullptr,0);
  f.source.supported=false;Require(!ordered.SourceFor(f.config,f.network),"first registered source preserves unsupported result");
  f.source.enabled=false;Require(ordered.SourceFor(f.config,f.network)==&fallback,"unregistered source skipped");
  ch::ChainRegistry native;size_t count=0;
  for (unsigned i=0;i<static_cast<unsigned>(ch::Family::kCount);++i)
    if (native.DriverFor(f.config,static_cast<ch::Family>(i))) ++count;
  Require(count>=18 && count<=21 && !native.DriverFor(f.config,ch::Family::kTon),"native families and absent TON");
  Require(!ch::kEvmDriver.Nfts(f.config),"unkeyed NFT capability absent");
  f.config.alchemy_api_key_configured=true;
  Require(ch::kEvmDriver.Nfts(f.config) && ch::kEvmDriver.Nfts(f.config)->SupportsNfts(f.network),"keyed NFT capability borrowed");
  Require(native.ViewFor(ch::Family::kEvm)==&ch::kEvmView,"native transaction view");
}
void family_transport() {
  Fixture f;JsonArena arena;ch::Context ctx{&f.transport,&arena.get(),&f.config,&f.clock};ch::Error error;tj::Value* result=nullptr;
  ch::Network xrpl;Require(f.catalog.Resolve("xrpl",4,&xrpl),"XRPL network");
  Require(ch::kXrplDriver.Balances(ctx,xrpl,"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",nullptr,0,&result,&error),"XRPL runs fake transport");
  Require(ch::Equals(tj::Get(tj::Get(result,"native"),"balanceRaw"),"0"),"unfunded XRPL is exact zero");
  Require(ch::kEvmDriver.Insight(ctx,f.network,"0x0000000000000000000000000000000000000001",&result,&error),"EVM insight fake transport");
  Require(tj::Get(result,"txCount")->number==2,"exact EVM nonce projection");
  ch::Network sol;Require(f.catalog.Resolve("solana",6,&sol),"Solana network");
  Require(ch::kSolanaDriver.TxStatus(ctx,sol,"unknown-signature",&result,&error) && result==nullptr,"unseen Solana signature");
  Require(f.transport.methods.size()==4 && f.transport.methods.front()=="account_info" && f.transport.methods.back()=="getSignatureStatuses","family request dispatch order");
}
void rpc_policy() {
  JsonArena a;ch::Call c;c.method="eth_call";auto* envelope=ch::JsonRpcClient::Envelope(a.get(),c);
  uint8_t bytes[256];tj::OutBuffer out{bytes,sizeof(bytes),0,true};Require(tj::Stringify(envelope,out),"serialize RPC envelope");
  Require(std::string(reinterpret_cast<char*>(bytes),out.used)==R"({"jsonrpc":"2.0","id":1,"method":"eth_call","params":[]})","RPC envelope golden bytes");
  Require(ch::JsonRpcClient::Interpret(c,Parse(a.get(),R"({"result":null})"))==ch::JsonRpcClient::Outcome::kSuccess,"null result valid");
  Require(ch::JsonRpcClient::Interpret(c,Parse(a.get(),R"({"error":{"code":-1}})"))==ch::JsonRpcClient::Outcome::kAnswer && c.has_code && c.code==-1,"answer error retains RPC code");
  Require(ch::JsonRpcClient::Interpret(c,Parse(a.get(),"null"))==ch::JsonRpcClient::Outcome::kTransport,"null envelope fails over");
  Require(ch::JsonRpcClient::Interpret(c,Parse(a.get(),R"({"error":false})"))==ch::JsonRpcClient::Outcome::kAnswer && std::string(c.message)=="eth_call: empty result","falsy error missing result");
  c.kind=ch::CallKind::kGet;Require(ch::ChainRetryPolicy::RetryParse(c,1) && !ch::ChainRetryPolicy::RetryParse(c,2),"GET parse retry bound");
  c.kind=ch::CallKind::kPost;Require(!ch::ChainRetryPolicy::RetryParse(c,0),"POST single shot");c.retry_post=true;
  Require(ch::ChainRetryPolicy::RetryParse(c,0) && ch::ChainRetryPolicy::ParseDelayMs(1)==250 && ch::ChainRetryPolicy::ParseDelayMs(2)==1000,"opted POST backoff");
  ch::EndpointFailover endpoints;Require(endpoints.Advance(3,9,10) && endpoints.index()==1,"next endpoint");
  Require(!endpoints.Advance(3,10,10) && endpoints.index()==1,"deadline refuses next endpoint");
  Require(endpoints.Advance(3,9,10) && !endpoints.Advance(3,9,10),"endpoint bound");
}
void cached_balance() {
  Fixture f;wa::WalletQueryService service(f.deps);JsonArena a;
  const char* address="0x0000000000000000000000000000000000000001";
  auto first=service.Balances(a.get(),"eth-mainnet",address,nullptr,0);
  Require(first.outcome==wa::Outcome::kOk,"balance through fake driver");
  const auto* native=tj::Get(first.payload,"native");Require(tj::Get(native,"fiat")->number==6,"native valuation joins spot price");
  auto second=service.Balances(a.get(),"eth-mainnet",address,nullptr,0);
  Require(second.outcome==wa::Outcome::kOk && f.driver.calls==1 && f.spot.calls==1,"cached reread avoids both providers");
}
void valuation() {
  Fixture f;wa::WalletValuation valuation(f.deps);JsonArena a;
  auto* row=Parse(a.get(),R"({"tokenId":"native","amountRaw":"250","amountDecimals":2,"ts":1699999999000})");
  valuation.Enrich(f.network,&row,1,nullptr,nullptr,f.clock.now,a.get());
  Require(tj::Get(row,"fiatAtTime")->number==17.5 && ch::Equals(tj::Get(row,"fiatSourceInterval"),"1h"),"hourly nearest fiat with exact amount units");
  Require(tj::Get(row,"fiatSourceTs")->number==1699999999000.0 && f.series.calls==2,"typed history reads both intervals");
}
void ingest_repair() {
  Fixture f;wa::WalletQueryService service(f.deps);JsonArena a;wa::Account account;
  const char* address="0x0000000000000000000000000000000000000001";
  Require(f.store.Touch(f.network.id,address,f.clock.now,&account),"account");
  f.source.with_record=true;f.series.available=false;
  service.EnsureFresh(account.id,f.network,address,1000);service.EnsureFresh(account.id,f.network,address,1000);
  Require(f.source.calls==1,"completed fresh ingest cursor suppresses repeat");
  tj::Value* state=nullptr;Require(f.store.GetSyncState(account.id,f.clock.now,a.get(),&state)==wa::ReadStatus::kFound,"durable sync state");
  Require(tj::Get(tj::Get(tj::Get(state,"upstreamCursors"),"fake-cursor"),"cursor")->number==2,"source key/cursor retained");
  f.series.available=true;size_t histories=0;
  auto result=service.RepairOperationCandles(a.get(),[&](const char* id,int64_t from,int64_t to,bool hourly) {
    ++histories;Require(!strcmp(id,"ethereum") && to>from && hourly,"repair groups history demand");return true;
  });
  Require(result.groups==1 && result.histories_ready==1 && result.transfers_repaired==1 && histories==1,"unpriced transfer durably repaired");
  auto again=service.RepairOperationCandles(a.get(),{});Require(again.groups==0,"repaired transfer removed from unpriced index");
  service.EnsureFresh(account.id,f.network,address,0);Require(f.source.calls==2,"force bypasses freshness");
}
void unavailable_source() {
  Fixture f;f.source.unavailable=true;wa::WalletQueryService service(f.deps);wa::Account account;JsonArena a;
  const char* address="0x0000000000000000000000000000000000000001";
  Require(f.store.Touch(f.network.id,address,f.clock.now,&account),"gap account");
  service.EnsureFresh(account.id,f.network,address,0);service.EnsureFresh(account.id,f.network,address,0);
  Require(f.source.calls==1,"source gap holds error cooldown even when forced");
  tj::Value* state=nullptr;Require(f.store.GetSyncState(account.id,f.clock.now,a.get(),&state)==wa::ReadStatus::kFound,"gap state");
  Require(ch::Equals(tj::Get(state,"lastError"),"coverage gap"),"source gap reason preserved");
}
void overview() {
  Fixture f;wa::WalletQueryService service(f.deps);JsonArena a;
  auto result=service.Overview(a.get(),"eth-mainnet");Require(result.outcome==wa::Outcome::kOk,"overview fake provider/news");
  Require(tj::Get(tj::Get(result.payload,"news"),"available")->type==tj::Type::kTrue,"news card retained");
  Require(tj::Get(tj::Get(result.payload,"popularTokens"),"items")->count==0 && f.transport.methods.size()==1,"trending projection through typed transport");
  auto again=service.Overview(a.get(),"eth-mainnet");Require(again.outcome==wa::Outcome::kOk && f.transport.methods.size()==1,"overview cache reread");
  Fixture down;down.transport.outage=true;wa::WalletQueryService degraded(down.deps);
  auto unavailable=degraded.Overview(a.get(),"eth-mainnet");
  Require(unavailable.outcome==wa::Outcome::kOk && tj::Get(tj::Get(unavailable.payload,"news"),"available")->type==tj::Type::kTrue,
          "trending outage retains news");
  Require(tj::Get(tj::Get(unavailable.payload,"popularTokens"),"stale")->type==tj::Type::kTrue,"outage marks popular card stale");
}
}
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("registry capability registration/order",registry_contract);
  suite.Run("real family drivers with fake transport",family_transport);
  suite.Run("RPC bytes and retry/failover policy",rpc_policy);
  suite.Run("wallet cached balance and spot join",cached_balance);
  suite.Run("wallet nearest historical valuation",valuation);
  suite.Run("durable transfer ingest and repair",ingest_repair);
  suite.Run("unavailable source cooldown",unavailable_source);
  suite.Run("overview composition and cached reread",overview);
  return suite.Finish("chain_wallet_components_test");
}
