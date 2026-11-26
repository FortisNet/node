#include <algorithm>
#include "native_config.h"
#include "env_clock.h"
#include "kv_store.h"
#include "price_history_backfill_owner.h"
#include "price_series_store.h"
#include "provider_execution_coordinator.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::config;
using namespace fortis::prices;
using namespace fortis::providers;

namespace {
struct Completion {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  bool done = false;
  size_t calls = 0;
  PriceHistoryOwnerResult value{};
};
uint64_t now_ms() {
  timespec t{}; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return static_cast<uint64_t>(t.tv_sec) * 1000 + static_cast<uint64_t>(t.tv_nsec / 1000000);
}
bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }
struct StopContext { PriceHistoryBackfillOwner* owner; Completion* completion; };
void complete(void* context, const PriceHistoryOwnerResult& result) {
  auto* c = static_cast<Completion*>(context);
  pthread_mutex_lock(&c->mutex); c->value = result; ++c->calls; c->done = true;
  pthread_cond_broadcast(&c->changed); pthread_mutex_unlock(&c->mutex);
}
void complete_then_stop(void* context, const PriceHistoryOwnerResult& result) {
  auto* stop = static_cast<StopContext*>(context);
  complete(stop->completion, result);
  stop->owner->Stop(); // Must request owner-thread shutdown without self-joining.
}
void* stop_thread(void* context) {
  static_cast<PriceHistoryBackfillOwner*>(context)->Stop(); return nullptr;
}
void wait(Completion& c) {
  pthread_mutex_lock(&c.mutex); timespec until{}; clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 35;
  while (!c.done) assert(pthread_cond_timedwait(&c.changed, &c.mutex, &until) == 0);
  assert(c.calls == 1); pthread_mutex_unlock(&c.mutex);
}
void wait_file(const char* path) {
  for (int i = 0; i < 500 && access(path, F_OK) != 0; ++i) usleep(10000);
  if (access(path, F_OK) != 0) fprintf(stderr,"missing marker: %s\n",path);
  assert(access(path, F_OK) == 0);
}
void remove_store(const char* dir) {
  char path[512]; snprintf(path, sizeof(path), "%s/fortis.kv", dir); unlink(path);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", dir); unlink(path); rmdir(dir);
}
size_t frame_count(const char* dir) {
  char path[512]; snprintf(path, sizeof(path), "%s/fortis.kv", dir);
  FILE* file = fopen(path, "rb"); assert(file); size_t count = 0;
  unsigned char header[12];
  while (fread(header, 1, sizeof(header), file) == sizeof(header)) {
    assert(memcmp(header, "FKV1", 4) == 0);
    const uint32_t n = static_cast<uint32_t>(header[4]) |
      (static_cast<uint32_t>(header[5]) << 8) |
      (static_cast<uint32_t>(header[6]) << 16) |
      (static_cast<uint32_t>(header[7]) << 24);
    if (fseek(file, static_cast<long>(n), SEEK_CUR) != 0) break;
    ++count;
  }
  fclose(file); return count;
}
constexpr fortis::prices::PriceSeriesInterval kHour = fortis::prices::PriceSeriesInterval::k1h;
constexpr fortis::prices::PriceSeriesInterval kDay = fortis::prices::PriceSeriesInterval::k1d;

uint64_t submit(PriceHistoryBackfillOwner& owner, const char* coin,
    const char* source, int64_t from, int64_t to, Completion& completion_state,
    uint64_t deadline_offset = 25000, fortis::prices::PriceSeriesInterval interval = kHour) {
  uint64_t id = 0;
  assert(owner.Submit(coin, source, "usd", interval, from, to,
      now_ms() + deadline_offset, complete, &completion_state, &id) ==
      PriceHistoryOwnerSubmitStatus::kAccepted);
  assert(id); return id;
}
// Seconds, like the owner's flights and CandleHistory's coverage ranges.
bool covered(const fortis::prices::PriceSeriesStore& series, const char* source,
    int64_t from, int64_t to, int64_t now, fortis::prices::PriceSeriesInterval interval = kHour) {
  fortis::prices::HistoryCoverage coverage{};
  return series.ReadHistoryCoverage(interval, "usd", source, now, &coverage) ==
             fortis::prices::PriceSeriesReadStatus::kFound &&
         fortis::prices::HistoryCoverageContains(coverage, from, to);
}
void put_le(uint8_t* output, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) output[i] = static_cast<uint8_t>(value >> (8 * i));
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 4);
  // FORTIS_TEST_HISTORY_NOW_FILE / _MS move the history owner's clock.
  SetPriceHistoryCoverageClock(&HistoryCoverageEnvClock());
  char directory[] = "/tmp/fortis-history-owner-XXXXXX"; assert(mkdtemp(directory));
  constexpr int64_t from = 1767225600LL;
  constexpr int64_t fixture_to = from + 2 * 86400;
  // The existing large fixture contains 90 days of hourly samples. Keep its
  // requested coverage window aligned with those samples now that the owner
  // also accepts a separate 365-day daily fixture below.
  const int64_t ninety_day_to = from + 90 * 86400LL;
  constexpr int64_t daily_from = 1735689600LL;
  constexpr int64_t daily_to = daily_from + 365 * 86400LL;
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  char commit_marker[512]{};
  NativeConfig config{}; config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", argv[2]);
  {
    fortis::storage::KvStore disk; assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    PriceHistoryBackfillOwner owner(series, config);
    assert(owner.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));

    Completion a{}, b{};
    const uint64_t a_id = submit(owner, "Ethereum", "ethereum", from, fixture_to, a);
    const uint64_t b_id = submit(owner, "ethereum", "ethereum", from, fixture_to, b);
    assert(a_id != b_id); wait(a); wait(b);
    assert(a.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           b.value.status == PriceHistoryOwnerResultStatus::kComplete);
    assert(a.value.candle_rows == 10 && a.value.committed_chunks == 1);
    assert(a.value.coverage_committed && b.value.coverage_committed);
    // Coverage is keyed by interval, as CandleHistory's coverage key is.
    assert(covered(series, "ethereum", from, fixture_to, now));
    assert(!covered(series, "ethereum", from, fixture_to, now, kDay));
    assert(!owner.Cancel(a_id));
    fortis::prices::PriceSeriesCandle hourly[16]{}, daily[8]{}; size_t hc = 0, dc = 0;
    assert(series.Range(fortis::prices::PriceSeriesInterval::k1h,"usd","ethereum",
        from*1000,fixture_to*1000,now,hourly,16,&hc)==fortis::prices::PriceSeriesReadStatus::kFound);
    assert(series.Range(fortis::prices::PriceSeriesInterval::k1d,"usd","ethereum",
        from*1000,fixture_to*1000,now,daily,8,&dc)==fortis::prices::PriceSeriesReadStatus::kFound);
    assert(hc == 7 && dc == 3 && hourly[0].volume == 100 && daily[0].volume == 300);

    Completion daily_a{}, daily_b{};
    submit(owner, "daily-365", "daily-365", daily_from, daily_to, daily_a);
    submit(owner, "daily-365", "daily-365", daily_from, daily_to, daily_b);
    wait(daily_a); wait(daily_b);
    assert(daily_a.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           daily_b.value.status == PriceHistoryOwnerResultStatus::kComplete);
    assert(daily_a.value.candle_rows == 730 && daily_a.value.committed_chunks == 4);
    assert(covered(series, "daily-365", daily_from, daily_to, now));
    Completion oversized{}; uint64_t oversized_id = 0;
    assert(owner.Submit("too-wide", "too-wide", "usd", kHour, daily_from,
        daily_to + 1, now_ms() + 25000, complete, &oversized, &oversized_id) ==
        PriceHistoryOwnerSubmitStatus::kInvalidRequest);
    Completion five_minute{}; uint64_t five_minute_id = 0;
    assert(owner.Submit("ethereum", "ethereum", "usd", fortis::prices::PriceSeriesInterval::k5m,
        from, fixture_to, now_ms() + 25000, complete, &five_minute, &five_minute_id) ==
        PriceHistoryOwnerSubmitStatus::kInvalidRequest);

    // Exercise the deadline-after-commit race before the additional coverage
    // fixtures use the provider lane's burst tokens. Its 800 ms deadline is
    // deliberately shorter than the injected 1.2 s post-commit pause.
    Completion commit_deadline{};
    snprintf(commit_marker,sizeof(commit_marker),"%s/commit-deadline",argv[3]);
    assert(setenv("FORTIS_HISTORY_TEST_COMMIT_PAUSE_MS","1200",1)==0);
    assert(setenv("FORTIS_HISTORY_TEST_COMMIT_MARKER",commit_marker,1)==0);
    const uint64_t commit_deadline_id=submit(owner,"commit-deadline","commit-deadline",
        from,fixture_to,commit_deadline,800);
    wait_file(commit_marker);
    assert(!owner.Cancel(commit_deadline_id)); // Final completion is already linearized.
    wait(commit_deadline);
    assert(commit_deadline.value.status==PriceHistoryOwnerResultStatus::kComplete);
    assert(covered(series,"commit-deadline",from,fixture_to,now));
    assert(unsetenv("FORTIS_HISTORY_TEST_COMMIT_PAUSE_MS")==0);
    assert(unsetenv("FORTIS_HISTORY_TEST_COMMIT_MARKER")==0);

    // A 1h fetch leaves 1d uncovered; the 1d request fetches and marks 1d.
    // A whole-bucket window inside a covered range is answered from coverage.
    Completion split_hour{}, split_day{}, split_inner{};
    submit(owner, "interval-split", "interval-split", from, fixture_to, split_hour);
    wait(split_hour);
    assert(split_hour.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           split_hour.value.coverage_committed);
    assert(covered(series, "interval-split", from, fixture_to, now));
    assert(!covered(series, "interval-split", from, fixture_to, now, kDay));
    submit(owner, "interval-split", "interval-split", from, fixture_to, split_day, 25000, kDay);
    wait(split_day);
    assert(split_day.value.status == PriceHistoryOwnerResultStatus::kComplete);
    assert(covered(series, "interval-split", from, fixture_to, now, kDay));
    submit(owner, "interval-split", "interval-split", from + 3600, from + 86400, split_inner);
    wait(split_inner);
    assert(split_inner.value.status == PriceHistoryOwnerResultStatus::kCachedComplete);

    // 1h and 1d callers of one window share a flight; both intervals are marked.
    Completion mixed_hour{}, mixed_day{};
    submit(owner, "mixed-interval", "mixed-interval", from, fixture_to, mixed_hour);
    submit(owner, "mixed-interval", "mixed-interval", from, fixture_to, mixed_day, 25000, kDay);
    wait(mixed_hour); wait(mixed_day);
    assert(mixed_hour.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           mixed_day.value.status == PriceHistoryOwnerResultStatus::kComplete);
    assert(covered(series, "mixed-interval", from, fixture_to, now));
    assert(covered(series, "mixed-interval", from, fixture_to, now, kDay));

    // A corrupt record is uncovered: the provider is called and the record
    // is replaced by a valid one.
    const char corrupt_key[] = "price/history-coverage/v2/1h/usd/corrupt-coin";
    const char garbage[] = "not a coverage record";
    const fortis::storage::KvMutation corrupt{corrupt_key, garbage, sizeof(garbage), 0, false};
    assert(disk.Write(&corrupt, 1));
    fortis::prices::HistoryCoverage probe{};
    assert(series.ReadHistoryCoverage(kHour, "usd", "corrupt-coin", now, &probe) ==
           fortis::prices::PriceSeriesReadStatus::kCorrupt);
    Completion corrupt_fetch{};
    submit(owner, "corrupt-coin", "corrupt-coin", from, fixture_to, corrupt_fetch);
    wait(corrupt_fetch);
    assert(corrupt_fetch.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           corrupt_fetch.value.coverage_committed);
    assert(covered(series, "corrupt-coin", from, fixture_to, now));

    // A retired v1 exact-window marker is never read as coverage.
    char v1_key[160];
    snprintf(v1_key, sizeof(v1_key), "price/history-coverage/v1/usd/v1-coin/%019lld/%019lld",
             static_cast<long long>(from) * 1000, static_cast<long long>(fixture_to) * 1000);
    uint8_t v1_value[24]{};
    std::copy_n("HCV1", 4, v1_value);
    put_le(v1_value + 4, static_cast<uint64_t>(from * 1000), 8);
    put_le(v1_value + 12, static_cast<uint64_t>(fixture_to * 1000), 8);
    put_le(v1_value + 20, fortis::prices::HistoryCoverageCrc32(v1_value, 20), 4);
    const fortis::storage::KvMutation v1_marker{v1_key, v1_value, sizeof(v1_value),
                                       now + 6 * 60 * 60 * 1000LL, false};
    assert(disk.Write(&v1_marker, 1));
    Completion v1_fetch{};
    submit(owner, "v1-coin", "v1-coin", from, fixture_to, v1_fetch); wait(v1_fetch);
    assert(v1_fetch.value.status == PriceHistoryOwnerResultStatus::kComplete);
    assert(covered(series, "v1-coin", from, fixture_to, now));

    Completion retry{}; submit(owner,"server-retry","server-retry",from,fixture_to,retry); wait(retry);
    assert(retry.value.status == PriceHistoryOwnerResultStatus::kComplete && retry.value.attempts == 3);
    Completion retry_fail{}; submit(owner,"server-fails","server-fails",from,fixture_to,retry_fail); wait(retry_fail);
    assert(retry_fail.value.status == PriceHistoryOwnerResultStatus::kUpstreamFailure &&
           retry_fail.value.http_status == 503 && retry_fail.value.attempts == 3);
    assert(!covered(series,"server-fails",from,fixture_to,now));
    Completion malformed{}; submit(owner,"malformed","malformed",from,fixture_to,malformed); wait(malformed);
    assert(malformed.value.status == PriceHistoryOwnerResultStatus::kInvalidResponse);
    assert(!covered(series,"malformed",from,fixture_to,now));

    Completion canceled{}; const uint64_t cancel_id = submit(owner,"slow-cancel","slow-cancel",from,fixture_to,canceled);
    char marker[512]; snprintf(marker,sizeof(marker),"%s/slow-cancel",argv[3]); wait_file(marker);
    assert(owner.Cancel(cancel_id)); wait(canceled);
    assert(canceled.value.status == PriceHistoryOwnerResultStatus::kCancelled);
    usleep(1700000); // Let the canceled blocking TLS attempt release its lane lease.
    Completion daily_canceled{};
    const uint64_t daily_cancel_id = submit(owner,"slow-daily-cancel","slow-daily-cancel",
        daily_from,daily_to,daily_canceled);
    snprintf(marker,sizeof(marker),"%s/slow-daily-cancel",argv[3]); wait_file(marker);
    assert(owner.Cancel(daily_cancel_id)); wait(daily_canceled);
    assert(daily_canceled.value.status == PriceHistoryOwnerResultStatus::kCancelled);
    usleep(1700000);
    // Cancelled before commit: the late provider answer creates no coverage.
    assert(!covered(series,"slow-cancel",from,fixture_to,now));
    assert(!covered(series,"slow-daily-cancel",daily_from,daily_to,now));
    Completion timeout{}; submit(owner,"slow-timeout","slow-timeout",from,fixture_to,timeout,800);
    snprintf(marker,sizeof(marker),"%s/slow-timeout",argv[3]); wait_file(marker);
    wait(timeout); assert(timeout.value.status == PriceHistoryOwnerResultStatus::kTimeout);

    Completion queued[kPriceHistoryOwnerMaxRequests]{};
    uint64_t queued_ids[kPriceHistoryOwnerMaxRequests]{};
    char queue_coin[kPriceHistoryOwnerMaxRequests][32]{};
    for (size_t i=0;i<kPriceHistoryOwnerMaxRequests;++i) {
      snprintf(queue_coin[i],sizeof(queue_coin[i]),"slow-queue-%zu",i);
      queued_ids[i]=submit(owner,queue_coin[i],queue_coin[i],from,fixture_to,queued[i]);
    }
    Completion overflow{}; uint64_t overflow_id=0;
    assert(owner.Submit("queue-overflow","queue-overflow","usd",kHour,from,fixture_to,
        now_ms()+25000,complete,&overflow,&overflow_id)==PriceHistoryOwnerSubmitStatus::kBusy);
    for (uint64_t id : queued_ids) assert(owner.Cancel(id));
    for (Completion& state : queued) { wait(state); assert(state.value.status==PriceHistoryOwnerResultStatus::kCancelled); }
    assert(!covered(series,"slow-timeout",from,fixture_to,now));
    for (const auto& coin : queue_coin) assert(!covered(series,coin,from,fixture_to,now));

    Completion partial{};
    assert(setenv("FORTIS_KV_TEST_FAIL","write_before_fsync",1)==0);
    assert(setenv("FORTIS_KV_TEST_FAIL_ON_CALL","2",1)==0);
    submit(owner,"partial-large","partial-large",from,ninety_day_to,partial); wait(partial);
    assert(partial.value.status == PriceHistoryOwnerResultStatus::kStorageFailure);
    assert(partial.value.committed_chunks == 1 && partial.value.candle_rows == 200);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    assert(unsetenv("FORTIS_KV_TEST_FAIL_ON_CALL") == 0);
    Completion partial365{};
    assert(setenv("FORTIS_KV_TEST_FAIL","write_before_fsync",1)==0);
    assert(setenv("FORTIS_KV_TEST_FAIL_ON_CALL","1",1)==0);
    submit(owner,"partial-365","partial-365",daily_from,daily_to,partial365); wait(partial365);
    assert(partial365.value.status == PriceHistoryOwnerResultStatus::kStorageFailure);
    assert(partial365.value.committed_chunks == 0 && partial365.value.candle_rows == 0);
    assert(!covered(series,"partial-365",daily_from,daily_to,now));
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    assert(unsetenv("FORTIS_KV_TEST_FAIL_ON_CALL") == 0);
    owner.Stop();
  }
  {
    // The injected second-frame write failure closes the KV handle. Reopen it
    // to replay the committed prefix, then retry the same range idempotently.
    fortis::storage::KvStore disk; assert(disk.Open(directory)); fortis::prices::PriceSeriesStore series(disk);
    assert(!covered(series,"partial-large",from,ninety_day_to,now));
    // Coverage written before the restart is read back from disk.
    assert(covered(series,"interval-split",from,fixture_to,now));
    assert(covered(series,"interval-split",from,fixture_to,now,kDay));
    fortis::prices::PriceSeriesCandle partial_rows[2300]{}; size_t partial_count=0;
    const auto partial_read=series.Range(fortis::prices::PriceSeriesInterval::k1h,"usd","partial-large",
        from*1000,ninety_day_to*1000,now,partial_rows,2300,&partial_count);
    assert(partial_read==fortis::prices::PriceSeriesReadStatus::kFound && partial_count>0 && partial_count<2160);
    const size_t frames_before_recovery = frame_count(directory);
    PriceHistoryBackfillOwner owner(series,config);
    assert(owner.StartRedirected("localhost",argv[1],argv[2],allow_loopback,nullptr));
    Completion recovered{}; submit(owner,"partial-large","partial-large",from,ninety_day_to,recovered); wait(recovered);
    assert(recovered.value.status==PriceHistoryOwnerResultStatus::kComplete &&
           recovered.value.candle_rows==2250 && recovered.value.committed_chunks==12);
    assert(covered(series,"partial-large",from,ninety_day_to,now));
    const size_t full_frames=frame_count(directory);
    assert(full_frames == frames_before_recovery + 13); // 12 <=200-row frames + one coverage frame.
    fortis::prices::PriceSeriesCandle large_rows[2300]{}; size_t large_count=0;
    assert(series.Range(fortis::prices::PriceSeriesInterval::k1h,"usd","partial-large",
        from*1000,ninety_day_to*1000,now,large_rows,2300,&large_count)==fortis::prices::PriceSeriesReadStatus::kFound);
    assert(large_count==2160);
    assert(!covered(series,"partial-365",daily_from,daily_to,now));
    const size_t frames_before_daily_recovery = frame_count(directory);
    PriceHistoryBackfillOwner daily_owner(series,config);
    assert(daily_owner.StartRedirected("localhost",argv[1],argv[2],allow_loopback,nullptr));
    Completion daily_recovered{};
    submit(daily_owner,"partial-365","partial-365",daily_from,daily_to,daily_recovered);
    wait(daily_recovered);
    assert(daily_recovered.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           daily_recovered.value.candle_rows == 730 && daily_recovered.value.committed_chunks == 4);
    assert(covered(series,"partial-365",daily_from,daily_to,now));
    assert(frame_count(directory) == frames_before_daily_recovery + 5);
    daily_owner.Stop();
    Completion stop_race{}; StopContext stop_context{&owner,&stop_race};
    snprintf(commit_marker,sizeof(commit_marker),"%s/commit-stop",argv[3]);
    assert(setenv("FORTIS_HISTORY_TEST_COMMIT_PAUSE_MS","1200",1)==0);
    assert(setenv("FORTIS_HISTORY_TEST_COMMIT_MARKER",commit_marker,1)==0);
    uint64_t stop_race_id=0;
    assert(owner.Submit("commit-stop","commit-stop","usd",kHour,from,fixture_to,
        now_ms()+25000,complete_then_stop,&stop_context,&stop_race_id)==PriceHistoryOwnerSubmitStatus::kAccepted);
    wait_file(commit_marker);
    pthread_t stopping_thread{}; assert(pthread_create(&stopping_thread,nullptr,stop_thread,&owner)==0);
    wait(stop_race);
    assert(stop_race.value.status==PriceHistoryOwnerResultStatus::kComplete);
    assert(pthread_join(stopping_thread,nullptr)==0);
    assert(unsetenv("FORTIS_HISTORY_TEST_COMMIT_PAUSE_MS")==0);
    assert(unsetenv("FORTIS_HISTORY_TEST_COMMIT_MARKER")==0);
  }
  {
    fortis::storage::KvStore disk; assert(disk.Open(directory)); fortis::prices::PriceSeriesStore series(disk);
    assert(covered(series,"partial-large",from,ninety_day_to,now));
    PriceHistoryBackfillOwner owner(series,config);
    assert(owner.StartRedirected("localhost",argv[1],argv[2],allow_loopback,nullptr));
    Completion cached{}; submit(owner,"partial-large","partial-large",from,ninety_day_to,cached); wait(cached);
    assert(cached.value.status==PriceHistoryOwnerResultStatus::kCachedComplete);
    char future_now[32];
    snprintf(future_now,sizeof(future_now),"%lld",static_cast<long long>(now + 6*60*60*1000LL + 120000));
    assert(setenv("FORTIS_TEST_HISTORY_NOW_MS",future_now,1)==0);
    assert(!covered(series,"partial-large",from,ninety_day_to,strtoll(future_now,nullptr,10)));
    Completion expired{}; submit(owner,"partial-large","partial-large",from,ninety_day_to,expired); wait(expired);
    assert(expired.value.status==PriceHistoryOwnerResultStatus::kComplete &&
           expired.value.committed_chunks==12);
    assert(unsetenv("FORTIS_TEST_HISTORY_NOW_MS")==0);
    Completion rate{}; submit(owner,"rate-limit","rate-limit",from,fixture_to,rate); wait(rate);
    assert(rate.value.status == PriceHistoryOwnerResultStatus::kRateLimited &&
           rate.value.http_status == 429 && rate.value.attempts == 1 && rate.value.retry_after_ms == 7000);
    owner.Stop();
  }
  {
    // Every candle frame commits, then the coverage frame is torn. The result
    // stays complete (CandleHistory swallows its kv.set failure) but reports
    // that coverage was not committed.
    fortis::storage::KvStore disk; assert(disk.Open(directory)); fortis::prices::PriceSeriesStore series(disk);
    PriceHistoryBackfillOwner owner(series, config);
    assert(owner.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
    Completion torn{};
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(setenv("FORTIS_KV_TEST_FAIL_ON_CALL", "2", 1) == 0);
    submit(owner, "coverage-write-fail", "coverage-write-fail", from, fixture_to, torn);
    wait(torn);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    assert(unsetenv("FORTIS_KV_TEST_FAIL_ON_CALL") == 0);
    assert(torn.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           !torn.value.coverage_committed && torn.value.committed_chunks == 1);
    owner.Stop();
  }
  {
    // After replay the candles are present but the window is not covered, so
    // the next request refetches instead of trusting a torn write.
    fortis::storage::KvStore disk; assert(disk.Open(directory)); fortis::prices::PriceSeriesStore series(disk);
    assert(!covered(series, "coverage-write-fail", from, fixture_to, now));
    fortis::prices::PriceSeriesCandle rows[16]{}; size_t row_count = 0;
    assert(series.Range(fortis::prices::PriceSeriesInterval::k1h, "usd", "coverage-write-fail",
        from * 1000, fixture_to * 1000, now, rows, 16, &row_count) ==
        fortis::prices::PriceSeriesReadStatus::kFound && row_count == 7);
    PriceHistoryBackfillOwner owner(series, config);
    assert(owner.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
    Completion retried{};
    submit(owner, "coverage-write-fail", "coverage-write-fail", from, fixture_to, retried);
    wait(retried);
    assert(retried.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           retried.value.coverage_committed);
    assert(covered(series, "coverage-write-fail", from, fixture_to, now));
    owner.Stop();
  }
  {
    // Ten-year daily window: one provider request for the whole
    // TypeScript daily maximum, 3,661 daily points bucketed into 7,322 1h+1d
    // rows (37 frames), and a 1d coverage mark. Each owner below is private,
    // so its CoinGecko lane starts with a full burst.
    fortis::storage::KvStore disk; assert(disk.Open(directory)); fortis::prices::PriceSeriesStore series(disk);
    constexpr int64_t decade_from = 1451001600LL;  // 2026-01-01 - 3,660 days
    constexpr int64_t decade_to = decade_from + 10 * 366 * 86400LL;
    PriceHistoryBackfillOwner owner(series, config);
    assert(owner.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
    Completion decade{};
    submit(owner, "decade-coin", "decade-coin", decade_from, decade_to, decade, 25000, kDay);
    wait(decade);
    assert(decade.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           decade.value.coverage_committed);
    assert(decade.value.candle_rows == 7322 && decade.value.committed_chunks == 37);
    assert(covered(series, "decade-coin", decade_from, decade_to, now, kDay));
    assert(!covered(series, "decade-coin", decade_from, decade_to, now));
    auto* rows = static_cast<fortis::prices::PriceSeriesCandle*>(
        calloc(fortis::prices::kPriceSeriesMaxDailyRangeCandles, sizeof(fortis::prices::PriceSeriesCandle)));
    assert(rows);
    size_t row_count = 0;
    assert(series.Range(kDay, "usd", "decade-coin", decade_from * 1000, decade_to * 1000, now,
        rows, 3661, &row_count) == fortis::prices::PriceSeriesReadStatus::kFound && row_count == 3661);
    assert(rows[0].ts == decade_from * 1000 && rows[3660].ts == decade_to * 1000);

    // A daily window over 10 * 366 days plus one aligned bucket is refused.
    Completion too_wide{}; uint64_t too_wide_id = 0;
    assert(owner.Submit("too-wide", "too-wide", "usd", kDay, decade_from,
        decade_from + kPriceHistoryOwnerMaxDailyWindowSeconds + 1, now_ms() + 25000, complete,
        &too_wide, &too_wide_id) == PriceHistoryOwnerSubmitStatus::kInvalidRequest);

    // The demo-tier 401 past 365 days: one attempt, nothing stored, nothing
    // covered, so the next request asks again (CandleHistory's swallow).
    Completion locked{};
    submit(owner, "decade-401", "decade-401", decade_from, decade_to, locked, 25000, kDay);
    wait(locked);
    assert(locked.value.status == PriceHistoryOwnerResultStatus::kUpstreamFailure &&
           locked.value.http_status == 401 && locked.value.attempts == 1);
    assert(locked.value.candle_rows == 0 && locked.value.committed_chunks == 0);
    assert(!covered(series, "decade-401", decade_from, decade_to, now, kDay));
    assert(series.Range(kDay, "usd", "decade-401", decade_from * 1000, decade_to * 1000, now,
        rows, 3661, &row_count) == fortis::prices::PriceSeriesReadStatus::kMissing);
    owner.Stop();

    // Over the native provider-body bound (512 KiB): kTooLarge, not retried,
    // a failure rather than an empty success.
    PriceHistoryBackfillOwner bounds_owner(series, config);
    assert(bounds_owner.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
    Completion oversize{};
    submit(bounds_owner, "decade-oversize", "decade-oversize", decade_from, decade_to,
           oversize, 25000, kDay);
    wait(oversize);
    assert(oversize.value.status == PriceHistoryOwnerResultStatus::kUpstreamFailure &&
           oversize.value.attempts == 1 && oversize.value.candle_rows == 0);
    assert(!covered(series, "decade-oversize", decade_from, decade_to, now, kDay));
    // Over the 3,700-point workspace inside the body bound: invalid response.
    Completion too_many{};
    submit(bounds_owner, "decade-toomany", "decade-toomany", decade_from, decade_to,
           too_many, 25000, kDay);
    wait(too_many);
    assert(too_many.value.status == PriceHistoryOwnerResultStatus::kInvalidResponse &&
           too_many.value.candle_rows == 0);
    assert(!covered(series, "decade-toomany", decade_from, decade_to, now, kDay));
    assert(series.Range(kDay, "usd", "decade-toomany", decade_from * 1000, decade_to * 1000,
        now, rows, 3661, &row_count) == fortis::prices::PriceSeriesReadStatus::kMissing);
    // Cancelled while the decade is in flight: the late answer creates no
    // coverage.
    Completion decade_cancel{};
    const uint64_t decade_cancel_id = submit(bounds_owner, "slow-decade-cancel",
        "slow-decade-cancel", decade_from, decade_to, decade_cancel, 25000, kDay);
    char decade_marker[512];
    snprintf(decade_marker, sizeof(decade_marker), "%s/slow-decade-cancel", argv[3]);
    wait_file(decade_marker);
    assert(bounds_owner.Cancel(decade_cancel_id)); wait(decade_cancel);
    assert(decade_cancel.value.status == PriceHistoryOwnerResultStatus::kCancelled);
    usleep(1700000);  // Let the cancelled TLS attempt finish and be drained.
    assert(!covered(series, "slow-decade-cancel", decade_from, decade_to, now, kDay));
    bounds_owner.Stop();
    free(rows);
  }
  {
    // Shared mode must use the process coordinator rather than constructing a
    // second private adapter. Exercise completion, cancellation, and owner
    // shutdown while the coordinator remains alive.
    fortis::storage::KvStore disk; assert(disk.Open(directory)); fortis::prices::PriceSeriesStore series(disk);
    ProviderExecutionCoordinator coordinator;
    assert(coordinator.valid() && coordinator.Start());
    PriceHistoryBackfillOwner owner(series, config, &coordinator);
    assert(owner.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
    Completion shared{};
    submit(owner, "shared-success", "shared-success", from, fixture_to, shared);
    wait(shared);
    assert(shared.value.status == PriceHistoryOwnerResultStatus::kComplete);
    Completion shared_daily{};
    submit(owner, "shared-daily-365", "shared-daily-365", daily_from, daily_to, shared_daily);
    wait(shared_daily);
    assert(shared_daily.value.status == PriceHistoryOwnerResultStatus::kComplete &&
           shared_daily.value.candle_rows == 730 && shared_daily.value.committed_chunks == 4);

    Completion cancelled{};
    const uint64_t cancel_id = submit(owner, "shared-slow-cancel", "shared-slow-cancel",
        from, fixture_to, cancelled);
    char marker[512]; snprintf(marker, sizeof(marker), "%s/shared-slow-cancel", argv[3]);
    wait_file(marker);
    assert(owner.Cancel(cancel_id)); wait(cancelled);
    assert(cancelled.value.status == PriceHistoryOwnerResultStatus::kCancelled);

    Completion shutdown{};
    submit(owner, "shared-slow-shutdown", "shared-slow-shutdown", from,
        fixture_to, shutdown);
    snprintf(marker, sizeof(marker), "%s/shared-slow-shutdown", argv[3]);
    wait_file(marker);
    owner.Stop();
    wait(shutdown);
    assert(shutdown.value.status == PriceHistoryOwnerResultStatus::kShutdown);
    assert(coordinator.in_flight_count() == 0);
    assert(coordinator.Stop());
  }
  remove_store(directory);
  puts("native asynchronous CoinGecko history owner, single-flight, failures, chunks, cancellation, interval coverage, restart and ten-year daily bounds passed");
}
