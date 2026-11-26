#include "conversation_components_internal.h"
#include "exception_boundary.h"
#include "feed_query_service.h"
#include "news_ingest.h"
#include "news_routes_internal.h"
#include "media_components_internal.h"
#include "unit_test_support.h"
#include <atomic>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <string>
#include <type_traits>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
namespace {
namespace ai=fortis::ai;
namespace news=fortis::news;
namespace tj=fortis::codec::tj;
namespace http=fortis::http;
using fortis::testing::Require;
struct Clock final : fortis::base::Clock {
  std::atomic<int64_t> now{1700000000000LL};
  int64_t NowMs() override {return now.load(std::memory_order_relaxed);}
};
enum class ReplyFailure { None, Allocation, Standard, Unknown };
void InjectReplyFailure(ReplyFailure failure) {
  if (failure == ReplyFailure::Allocation) throw std::bad_alloc();
  if (failure == ReplyFailure::Standard) throw std::runtime_error("Bearer fintel_dev_private upstream body");
  if (failure == ReplyFailure::Unknown) throw 7;
}
struct MemoryConversations final : ai::ConversationRepository {
  using AddStatus=ai::AddStatus;using OpenInput=ai::OpenInput;using Opened=ai::Opened;
  using MessageView=ai::MessageView;using Turn=ai::Turn;using TokenUsage=ai::TokenUsage;
  static constexpr size_t kIdBytes=ai::kIdBytes;
  bool replied=false, pending=false, context_ok=true, save_ok=true;
  ReplyFailure before_save=ReplyFailure::None, after_save=ReplyFailure::None;
  ReplyFailure status_failure=ReplyFailure::None, claim_failure=ReplyFailure::None;
  size_t saved=0;std::string body,status;ai::TokenUsage usage{};
  int64_t Now() const override {return 1700000000000LL;}
  bool FindReply(const char*,const char*,char id[kIdBytes]) override {
    if (replied) { snprintf(id,kIdBytes,"assistant"); }
    return replied;
  }
  bool NextPendingJob(char id[kIdBytes],char message[kIdBytes]) override {
    InjectReplyFailure(claim_failure);
    if (!pending) return false;
    pending=false;snprintf(id,kIdBytes,"conversation");snprintf(message,kIdBytes,"user");return true;
  }
  bool LoadReplyContext(const char*,const char*,tj::Arena& arena,const tj::Value** snapshot,ai::Turn** turns,size_t* count) override {
    constexpr char json[]=R"({"version":1,"capturedAt":"2026-01-01T00:00:00Z","balancesComplete":false,"wallets":[],"tokens":[],"holdings":[]})";
    *snapshot=tj::Parse(arena,reinterpret_cast<const uint8_t*>(json),sizeof(json)-1);
    *turns=nullptr;*count=0;return context_ok&&*snapshot;
  }
  bool SetJobStatus(const char*,const char*,const char* value) override {
    InjectReplyFailure(status_failure);
    if (!replied) status=value;
    return true;
  }
  bool SaveReply(const char*,const char*,const char* value,size_t length,char id[kIdBytes],bool* created,const TokenUsage& u) override {
    InjectReplyFailure(before_save);
    if (!save_ok) return false;
    body.assign(value,length);usage=u;++saved;replied=true;status="complete";*created=true;snprintf(id,kIdBytes,"assistant");
    InjectReplyFailure(after_save);
    return true;
  }
  bool LoadMessage(const char*,const char* id,tj::Arena&,MessageView* row,const char** snapshot,size_t* length) override {
    *row={};snprintf(row->id,sizeof(row->id),"%s",id);snprintf(row->role,sizeof(row->role),"%s",!strcmp(id,"assistant")?"assistant":"user");
    row->body=!strcmp(id,"assistant")?body.c_str():"hello";row->body_length=strlen(row->body);row->created_at_ms=Now();
    if (snapshot) *snapshot = nullptr;
    if (length) *length = 0;
    return true;
  }
bool Open(const OpenInput& input, Opened* opened, bool* storage_error) override {(void)input;(void)opened;(void)storage_error;return false;}
bool Authorize(const char* id, const char* device_id, const char* token, size_t token_length,
                 bool* storage_error) override {(void)id;(void)device_id;(void)token;(void)token_length;(void)storage_error;return false;}
bool Messages(const char* id, fortis::codec::tj::Arena& arena, MessageView** rows, size_t* count) override {(void)id;(void)arena;(void)rows;(void)count;return false;}
bool MessagePage(const char* id, const char* before_id, size_t limit,
                   fortis::codec::tj::Arena& arena, MessageView** rows, size_t* count,
                   bool* has_more) override {(void)id;(void)before_id;(void)limit;(void)arena;(void)rows;(void)count;(void)has_more;return false;}
AddStatus AddMessage(const char* id, const char* client_message_id, const char* text, size_t text_length,
                       const char* snapshot_json, size_t snapshot_json_length, const char* snapshot_hash,
                       char message_id[kIdBytes]) override {(void)id;(void)client_message_id;(void)text;(void)text_length;(void)snapshot_json;(void)snapshot_json_length;(void)snapshot_hash;(void)message_id;return ai::AddStatus::kStorage;}
bool ResetRunningJobs() override {return false;}
bool JobStatus(const char* id, const char* user_message_id, char* status, size_t capacity) override {(void)id;(void)user_message_id;(void)status;(void)capacity;return false;}
bool ConversationTimes(const char* id, int64_t* created_at_ms, int64_t* expires_at_ms) override {(void)id;(void)created_at_ms;(void)expires_at_ms;return false;}
};
struct ReplyTransport final : ai::ReplyTransport {
  ReplyFailure failure=ReplyFailure::None;
  int status=200;size_t calls=0;std::string text="A bounded reply.";
  http::OutboundResponse Execute(const http::OutboundRequest& request,uint8_t* output,size_t capacity) override {
    const auto injected=std::exchange(failure,ReplyFailure::None);
    InjectReplyFailure(injected);
    ++calls;Require(!strcmp(request.path,"/v1/messages"),"Anthropic path");
    Require(request.timeout_ms==1000&&request.max_response_bytes==2*1024*1024&&request.header_count==2,"provider budgets/headers");
    Require(request.body&&request.body_length,"request body lifetime");
    const std::string answer="{\"type\":\"message\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\""+text+"\"}],\"usage\":{\"input_tokens\":7,\"output_tokens\":3}}";
    Require(answer.size()<capacity,"reply capacity");memcpy(output,answer.data(),answer.size());output[answer.size()]=0;http::OutboundResponse r{};r.error=http::OutboundError::kNone;r.status=status;r.body_length=answer.size();return r;
  }
};
struct AiFixture {
  MemoryConversations repository;ReplyTransport transport;Clock clock;
  ai::AiConversationService::Impl runtime{repository};char path[64]="/tmp/fortis-ai-component-XXXXXX";
  AiFixture() {
    const int fd=mkstemp(path);Require(fd>=0,"key fixture");Require(write(fd,"fixture-key-only",16)==16,"key write");close(fd);
    auto& c=runtime.config;
    c.ai_provider_configured=c.ai_model_configured=c.ai_effort_configured=c.ai_max_output_tokens_configured=c.ai_timeout_ms_configured=c.ai_concurrency_configured=c.anthropic_api_key_path_configured=true;
    snprintf(c.ai_provider,sizeof(c.ai_provider),"anthropic");snprintf(c.ai_model,sizeof(c.ai_model),"claude-opus-test");snprintf(c.ai_effort,sizeof(c.ai_effort),"low");
    snprintf(c.ai_max_output_tokens,sizeof(c.ai_max_output_tokens),"256");snprintf(c.ai_timeout_ms,sizeof(c.ai_timeout_ms),"1000");snprintf(c.ai_concurrency,sizeof(c.ai_concurrency),"1");snprintf(c.anthropic_api_key_path,sizeof(c.anthropic_api_key_path),"%s",path);
    runtime.have_seams=true;runtime.seams.transport=&transport;runtime.seams.clock=&clock;
    Require(runtime.generator.ConfigureAnthropic(),"valid provider settings");
  }
  ~AiFixture(){unlink(path);}
};
void ReplyWorkflow() {
  AiFixture f;f.repository.pending=true;Require(f.runtime.queue.ProcessPending(),"pending job processed");
  Require(f.repository.saved==1&&f.repository.body=="A bounded reply."&&f.repository.usage.input==7,"persist body and usage");
  Require(!f.runtime.queue.ProcessPending(),"empty queue");f.runtime.conversations.GenerateJob("conversation","user");Require(f.transport.calls==1,"existing reply suppresses another provider call");
  ai::TurnRequest turn;snprintf(turn.conversation,sizeof(turn.conversation),"conversation");snprintf(turn.user_message,sizeof(turn.user_message),"user");
  http::HttpResponse response;ai::Encoded encoded;Require(f.runtime.conversations.BuildResponse(turn,"assistant",nullptr,0,&response,&encoded)&&response.status==201,"saved turn response");
}
struct Errors final : fortis::base::ErrorReporter {
  std::vector<std::string> codes;
  bool record_failed = false;
  fortis::base::ErrorOrigin origin = fortis::base::ErrorOrigin::kNode;
  std::string provider;
  std::uint16_t http_status = 0;
  void Report(std::string_view, std::string_view code, std::string_view, fortis::base::ErrorDetails details) noexcept override {
    try { codes.emplace_back(code); origin = details.origin; provider = details.provider; http_status = details.http_status; } catch (...) { record_failed = true; }
  }
};
struct ExceptionLogs final : fortis::base::LogSink {
  std::string messages;
  bool Write(const fortis::base::LogRecord& record) override {
    messages.append(record.text,record.length);
    return true;
  }
};
void ReplyWorkerRecovery() {
  ExceptionLogs logs;
  auto* previous=fortis::base::SetLogSink(&logs);
  const auto restore=[previous](fortis::base::LogSink*) noexcept {fortis::base::SetLogSink(previous);};
  std::unique_ptr<fortis::base::LogSink,decltype(restore)> log_scope(&logs,restore);
  for (auto failure : {ReplyFailure::Allocation,ReplyFailure::Standard,ReplyFailure::Unknown}) {
    for (bool storage : {false,true}) {
      AiFixture fixture;
      Errors errors;
      fixture.runtime.errors=&errors;
      if (storage) fixture.repository.before_save=failure;
      else fixture.transport.failure=failure;
      fixture.repository.pending=true;
      Require(fixture.runtime.queue.ProcessPending(),"throwing job consumed");
      Require(fixture.repository.status=="failed" && errors.codes==std::vector<std::string>{"provider_exception"},"exception classified and persisted");
      fixture.repository.before_save=ReplyFailure::None;
      fixture.repository.pending=true;
      Require(fixture.runtime.queue.ProcessPending(),"worker accepts next job");
      Require(fixture.repository.saved==1 && !errors.record_failed,"next job completes after exception");
    }
    AiFixture claim;
    Errors claim_errors;
    claim.runtime.errors=&claim_errors;
    claim.repository.claim_failure=failure;
    claim.repository.pending=true;
    Require(!claim.runtime.queue.ProcessPending() && claim.repository.pending,"failed claim leaves pending job intact");
    Require(claim_errors.codes==std::vector<std::string>{"job_queue_read_failed"},"claim exception reported without claiming work");
    claim.repository.claim_failure=ReplyFailure::None;
    Require(claim.runtime.queue.ProcessPending() && claim.repository.saved==1,"claim retry succeeds without lost job");
    AiFixture committed;
    committed.repository.after_save=failure;
    committed.repository.pending=true;
    Require(committed.runtime.queue.ProcessPending(),"post-commit failure consumed");
    Require(committed.repository.saved==1 && committed.repository.status=="complete","late failure preserves committed reply");
    committed.repository.pending=true;
    Require(committed.runtime.queue.ProcessPending(),"replayed job consumed");
    Require(committed.repository.saved==1 && committed.transport.calls==1,"no duplicate reply or provider call after committed failure");

    AiFixture unavailable;
    Errors errors;
    unavailable.runtime.errors=&errors;
    unavailable.transport.failure=failure;
    unavailable.repository.status_failure=failure;
    unavailable.repository.pending=true;
    Require(unavailable.runtime.queue.ProcessPending(),"status-write exception contained");
    Require(errors.codes==std::vector<std::string>({"provider_exception","job_status_write_failed"}),"failure persistence reported truthfully");
    unavailable.repository.status_failure=ReplyFailure::None;
    unavailable.repository.pending=true;
    Require(unavailable.runtime.queue.ProcessPending() && unavailable.repository.saved==1,"status-write failure releases job accounting");
  }
  Require(logs.messages.find("std::bad_alloc")!=std::string::npos && logs.messages.find("unknown exception")!=std::string::npos &&
      logs.messages.find("fintel_dev_private")==std::string::npos,"job logs classify without copying exception payloads");
}

struct ReplyIds final : fortis::base::IdSource {
  unsigned long long next=0;
  bool NewId(char output[kIdBytes]) override {
    snprintf(output,kIdBytes,"%026llu",++next);
    return true;
  }
};
void DurableReplyStatusIsTerminal() {
  fortis::testing::TemporaryKv fixture;
  Clock clock;
  ai::Store store;
  ReplyIds ids;
  Require(store.Start(&fixture.kv,"fixture",7,&clock,&ids),"start durable reply fixture");
  constexpr char conversation[]=R"({"ownerId":"0000000000000000000000000000000000000000000000000000000000000000","deviceId":"device","proofNonceHash":"nonce","accessTokenHash":"token","snapshotHash":"snapshot","createdAt":0,"updatedAt":0,"expiresAt":0})";
  const fortis::storage::KvMutation seed{"ai/conversation/thread",conversation,sizeof(conversation)-1,0,false};
  Require(fixture.kv.Write(&seed,1),"seed private repository fixture");
  Require(store.SetJobStatus("thread","user","pending"),"pending durable job");
  char reply[ai::kIdBytes];
  bool created=false;
  Require(store.SaveReply("thread","user","answer",6,reply,&created) && created,"reply and job deletion committed atomically");
  for (const char* status : {"failed","pending","running"}) Require(store.SetJobStatus("thread","user",status),"late status is harmless");
  char status[32];
  Require(store.JobStatus("thread","user",status,sizeof(status)) && !strcmp(status,"complete"),"late failure cannot recreate job");
  store.Stop();
  fixture.kv.Close();
  Require(fixture.kv.Open(fixture.directory.path()) && store.Start(&fixture.kv,"fixture",7,&clock,&ids),"reopen durable completion");
  char winner[ai::kIdBytes];
  Require(store.FindReply("thread","user",winner) && !strcmp(reply,winner),"same assistant survives reopen");
  Require(store.JobStatus("thread","user",status,sizeof(status)) && !strcmp(status,"complete"),"completed status survives reopen");
}
void ReplyFailuresAndQuotas() {
  AiFixture f; Errors errors; f.runtime.errors=&errors; f.transport.status=429;f.runtime.conversations.GenerateJob("c","u");Require(f.repository.status=="rate_limited"&&!f.repository.saved,"stable provider failure status");
  Require(errors.origin==fortis::base::ErrorOrigin::kThirdParty && errors.provider=="anthropic" &&
          errors.http_status==429,"provider refusal remains distinct from local failure");
  f.transport.status=200;f.repository.save_ok=false;f.runtime.conversations.GenerateJob("c","u");Require(f.repository.status=="failed","save failure status");
  Require(errors.origin==fortis::base::ErrorOrigin::kNode && errors.provider.empty(),"local persistence failure stays a node error");
  f.repository.context_ok=false;const auto calls=f.transport.calls;f.runtime.conversations.GenerateJob("c","u");Require(f.transport.calls==calls,"unreadable context never sent");
  for (int i=0; i<20; ++i) { Require(!f.runtime.routes.OverQuota('o',"device",20),"quota first20"); }
  Require(f.runtime.routes.OverQuota('o',"device",20),"quota21");
  Require(!f.runtime.routes.OverQuota('m',"device",120),"distinct domain quota");f.clock.now.fetch_add(3600000);Require(!f.runtime.routes.OverQuota('o',"device",20),"window edge resets");
}
struct MemoryNews final : news::NewsRepository {
  mutable size_t reads=0,persists=0,prunes=0;bool read_ok=true,persist_ok=true,prune_ok=true;mutable std::vector<int> order;
  news::StoreStatus KnownArticleKeys(const char* const*,size_t,int64_t,int64_t,news::Arena*,news::U16** keys,size_t* count) const override {order.push_back(1);*keys=nullptr;*count=0;return read_ok?news::StoreStatus::kOk:news::StoreStatus::kIoError;}
  news::StoreStatus RecentClusters(int64_t,int64_t,news::Arena*,news::Cluster** clusters,size_t* count) const override {order.push_back(2);*clusters=nullptr;*count=0;return news::StoreStatus::kOk;}
  news::StoreStatus Persist(news::Cluster* const*,size_t,fortis::base::Clock&,news::Arena*) const override {++persists;return persist_ok?news::StoreStatus::kOk:news::StoreStatus::kIoError;}
  news::StoreStatus Prune(int64_t,news::Arena*,size_t* removed) const override {order.push_back(3);++prunes;*removed=2;return prune_ok?news::StoreStatus::kOk:news::StoreStatus::kIoError;}
  news::StoreStatus Feed(const news::Topic*,size_t,size_t,int64_t,news::Arena*,news::Cluster**,size_t*) const override{return news::StoreStatus::kIoError;}
  news::StoreStatus Item(const char*,int64_t,news::Arena*,news::Cluster*,bool* found) const override {*found=false;return news::StoreStatus::kOk;}
  news::StoreStatus LastIngestAt(int64_t,bool*,int64_t*) const override{return news::StoreStatus::kIoError;}
  news::StoreStatus FeedSnapshot(const news::Topic*,size_t,size_t,bool,int64_t,const char*,int64_t,news::Arena*,news::Cluster** clusters,size_t* count,bool* found,int64_t* last) const override {++reads;*clusters=nullptr;*count=0;*found=false;*last=0;return read_ok?news::StoreStatus::kOk:news::StoreStatus::kIoError;}
};
void NewsWorkflows() {
  MemoryNews repository;Clock clock;news::TickerDictionary dictionary{};
  std::vector<uint8_t> run(news::kIngestArenaBytes),scratch(news::kFeedScratchBytes),body(news::kMaxFeedBodyBytes),out(4096);
  news::IngestDeps deps;deps.repository=&repository;deps.clock=&clock;deps.dictionary=&dictionary;
  deps.fetch=[](void*,const news::FeedRegistration&,uint8_t*,size_t,size_t*){return false;};
  news::IngestResult result;Require(news::NewsIngestor(deps).Run(run.data(),scratch.data(),body.data(),&result),"failed feeds isolated");
  Require(result.feeds_failed==news::kNewsFeedCount&&result.pruned==2&&repository.order==std::vector<int>({1,2,3}),"ingest read/prune ordering");
  struct FeedFixture {size_t calls=0;} feed;
  deps.fetch_context=&feed;
  deps.fetch=[](void* opaque,const news::FeedRegistration&,uint8_t* output,size_t capacity,size_t* length){
    auto& fixture=*static_cast<FeedFixture*>(opaque);
    if(fixture.calls++)return false;
    constexpr char xml[]="<rss><channel><item><guid>component-story</guid><title>Bitcoin network security update</title><link>https://example.org/story</link><description>A public Bitcoin network security update with details.</description><pubDate>Tue, 14 Nov 2023 12:00:00 GMT</pubDate></item></channel></rss>";
    if (sizeof(xml)-1 > capacity) return false;
    memcpy(output,xml,sizeof(xml)-1);*length=sizeof(xml)-1;return true;
  };
  repository.persist_ok=false;Require(news::NewsIngestor(deps).Run(run.data(),scratch.data(),body.data(),&result),"persist failure is an ingest outcome");
  Require(result.fetched==1&&result.fresh==1&&result.persist_failed&&repository.persists==1&&repository.prunes==1,"fresh story pipeline skips prune after failed persist");
  repository.read_ok=false;Require(!news::NewsIngestor(deps).Run(run.data(),scratch.data(),body.data(),&result),"repository read failure stops ingest");
  repository.read_ok=true;news::Arena arena{run.data(),run.size()};news::OutBuffer output{out.data(),out.size()};news::FeedQuery query;
  news::FeedQueryService service(repository);Require(service.Feed(query,clock.NowMs(),&arena,output),"feed fake repository");
  Require(std::string(reinterpret_cast<char*>(out.data()),output.used)=="{\"items\":[],\"asOf\":null,\"stale\":true}","empty feed bytes");
  output={out.data(),out.size()};Require(service.Detail("unknown",clock.NowMs(),&arena,output)&&output.used==4&&!memcmp(out.data(),"null",4),"unknown detail");
}
struct MediaTransport final : news::MediaTransport {
  size_t calls=0;bool redirect=false;
  http::OutboundResponse Execute(const http::OutboundRequest& request,uint8_t* response,size_t capacity) override {
    ++calls;Require(request.allow_address&&request.cancel,"pinned addresses and cancellation");Require(capacity==news::kMediaMaxBytes,"media bound");
    http::OutboundResponse r{};r.error=http::OutboundError::kNone;r.status=redirect?302:200;
    if(redirect){r.has_location=true;snprintf(r.location,sizeof(r.location),"http://127.0.0.1/private");return r;}
    const uint8_t png[12]={0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0};memcpy(response,png,sizeof(png));r.body_length=sizeof(png);return r;
  }
};
void MediaAndReactions() {
  fortis::testing::TemporaryKv storage;Clock clock;MediaTransport transport;
  auto runtime=std::make_unique<news::MediaRuntime>();runtime->kv=&storage.kv;runtime->have_seams=true;runtime->seams.clock=&clock;runtime->seams.transport=&transport;
  runtime->seams.resolve=[](void*,const char*,uint32_t* addresses,size_t* count){addresses[0]=0x08080808;*count=1;return true;};
  Require(runtime->fetcher.AllocateBuffers(),"media owned buffers");
  const char* key="11111111111111111111111111111111";const char* url="https://example.org/image.png";news::MediaRecord record;snprintf(record.key,sizeof(record.key),"%s",key);record.source_url=url;record.source_url_length=strlen(url);record.updated_at_ms=clock.NowMs();
  Require(runtime->repository.WriteRecord(record,clock.NowMs(),nullptr,false,false),"media registration");
  auto answer=runtime->fetcher.Process(key);Require(answer.found&&answer.length==12&&!strcmp(answer.content_type,"image/png")&&transport.calls==1,"verified media saved");
  answer=runtime->fetcher.Process(key);Require(answer.found&&transport.calls==1,"cached bytes avoid fetch");
  size_t orphans=99;Require(runtime->pruner.Prune(clock.NowMs(),&orphans)==0&&orphans==0,"fresh media retained");
  transport.redirect=true;const char* blocked="22222222222222222222222222222222";snprintf(record.key,sizeof(record.key),"%s",blocked);Require(runtime->repository.WriteRecord(record,clock.NowMs(),nullptr,false,false),"redirect record");
  answer=runtime->fetcher.Process(blocked);Require(!answer.found&&transport.calls==2,"private redirect refused before transport");
  const char* expired="33333333333333333333333333333333";
  snprintf(record.key,sizeof(record.key),"%s",expired);record.status=news::MediaStatus::kOk;record.has_bytes=true;record.chunks=1;record.length=12;record.content_type="image/png";
  record.has_fetched_at=true;record.fetched_at_ms=clock.NowMs()-news::kMediaTtlMs-1;record.blob_expires_ms=clock.NowMs()+news::kMediaTtlMs;
  const uint8_t png[12]={0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0};
  Require(runtime->repository.WriteRecord(record,clock.NowMs(),png,true,false),"old unviewed bytes");
  const char orphan[]="media/blob/44444444444444444444444444444444/0";
  const fortis::storage::KvMutation mutation{orphan,png,sizeof(png),record.blob_expires_ms,false};Require(storage.kv.Write(&mutation,1),"orphan chunk");
  Require(runtime->pruner.Prune(clock.NowMs(),&orphans)==1&&orphans==1,"old bytes and orphan removed in bounded pages");
  runtime->fetcher.Cancel();Require(runtime->fetcher.cancelled(),"cancel flag owned by fetcher");
  auto reactions=std::make_unique<news::ReactionService>();for(size_t i=0;i<8;++i)Require(reactions->AcquireJob()!=nullptr,"bounded reaction job");Require(!reactions->AcquireJob(),"reaction capacity");
}
static_assert(!std::is_copy_constructible<ai::ReplyJobQueue>::value&&!std::is_move_constructible<ai::ReplyJobQueue>::value,"queue identity");
static_assert(!std::is_copy_constructible<ai::Encoded>::value,"response owns one buffer");
static_assert(!std::is_copy_constructible<news::MediaFetcher>::value&&!std::is_move_constructible<news::MediaFetcher>::value,"media identity");
static_assert(!std::is_copy_constructible<news::ReactionService>::value,"callback owner identity");
}  // namespace
int main() try {ReplyWorkflow();ReplyWorkerRecovery();DurableReplyStatusIsTerminal();ReplyFailuresAndQuotas();NewsWorkflows();MediaAndReactions();puts("content components: 6 cases passed");} catch (const std::exception& e) {fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
