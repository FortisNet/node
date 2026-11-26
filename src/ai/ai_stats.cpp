#include <cstdlib>
#include <cerrno>
#include "ai_stats.h"
#include "price_history_batch_codec.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::ai {
namespace {
constexpr int64_t kHourMs = 3600000;
constexpr int64_t kDayMs = 86400000;
constexpr int64_t kHourRetentionMs = 3 * kDayMs;
constexpr int64_t kDayRetentionMs = 32 * kDayMs;

bool parse(const char* text, StatsCounts* out) {
  unsigned long long v[6]{};
  const char* cursor = text;
  for (auto& value : v) {
    char* end = nullptr;
    errno = 0;
    value = std::strtoull(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || *end != ',') return false;
    cursor = end + 1;
  }
  char* end = nullptr;
  errno = 0;
  const long long since = std::strtoll(cursor, &end, 10);
  if (end == cursor || errno == ERANGE || *end || since < 0) return false;
  *out = {v[0], v[1], v[2], v[3], v[4], v[5], since};
  return true;
}

bool load(storage::KvStore& kv, const char* key, int64_t now, StatsCounts* out, bool* found) {
  char value[160]{};
  size_t length = 0;
  const storage::KvReadStatus status = kv.GetStatus(key, now, value, sizeof(value) - 1, &length);
  if (status == storage::KvReadStatus::Missing) { *out = {}; *found = false; return true; }
  if (status != storage::KvReadStatus::Found || length >= sizeof(value)) return false;
  value[length] = 0;
  *found = true;
  return parse(value, out);
}

bool present(storage::KvStore& kv, const char* key, int64_t now, bool* found) {
  char value[1];
  size_t length = 0;
  const storage::KvReadStatus status = kv.GetStatus(key, now, value, sizeof(value), &length);
  if (status == storage::KvReadStatus::IoError) return false;
  *found = status != storage::KvReadStatus::Missing;
  return true;
}

bool plus(uint64_t* value, uint64_t amount) {
  if (amount > UINT64_MAX - *value) return false;
  *value += amount;
  return true;
}

bool encode(const StatsCounts& value, char out[160], uint32_t* length) {
  const int n = snprintf(out, 160, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
      ",%" PRIu64 ",%" PRIu64 ",%" PRId64, value.users, value.user_messages,
      value.assistant_messages, value.input_tokens, value.output_tokens,
      value.measured_replies, value.since_ms);
  if (n < 0 || n >= 160) return false;
  *length = static_cast<uint32_t>(n);
  return true;
}
}  // namespace

void HourBucket(int64_t now_ms, char out[24]) {
  snprintf(out, 24, "h/%" PRId64, now_ms / kHourMs);
}
void DayBucket(int64_t now_ms, char out[24]) {
  snprintf(out, 24, "d/%" PRId64, now_ms / kDayMs);
}

bool ReadStats(storage::KvStore& kv, const char* bucket, int64_t now_ms, StatsCounts* out) {
  if (!bucket || !out) return false;
  char key[96];
  const int n = snprintf(key, sizeof(key), "ai/stats/v1/%s", bucket);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(key)) return false;
  bool found = false;
  return load(kv, key, now_ms, out, &found);
}

bool PrepareStatsWrites(storage::KvStore& kv, const char* owner_id, int64_t now_ms,
                        StatsEvent event, const TokenUsage& usage, StatsWrites* out) {
  if (!owner_id || strlen(owner_id) != 64 || now_ms < 0 || !out) return false;
  char hour[24], day[24];
  HourBucket(now_ms, hour);
  DayBucket(now_ms, day);
  const char* buckets[3] = {"all", hour, day};
  const int64_t expiry[3] = {0, (now_ms / kHourMs + 1) * kHourMs + kHourRetentionMs,
                              (now_ms / kDayMs + 1) * kDayMs + kDayRetentionMs};
  out->count = 0;
  for (size_t i = 0; i < 3; ++i) {
    const int kn = snprintf(out->keys[i], sizeof(out->keys[i]), "ai/stats/v1/%s", buckets[i]);
    const int mn = snprintf(out->keys[i + 3], sizeof(out->keys[i + 3]),
                            "ai/stats/v1/member/%s/%s", buckets[i], owner_id);
    if (kn < 0 || mn < 0 || static_cast<size_t>(kn) >= sizeof(out->keys[i]) ||
        static_cast<size_t>(mn) >= sizeof(out->keys[i + 3])) return false;
    StatsCounts counts;
    bool counter_found = false, member_found = false;
    if (!load(kv, out->keys[i], now_ms, &counts, &counter_found) ||
        !present(kv, out->keys[i + 3], now_ms, &member_found)) return false;
    if (!counter_found) counts.since_ms = now_ms;
    if ((!member_found && !plus(&counts.users, 1)) ||
        (event == StatsEvent::UserMessage && !plus(&counts.user_messages, 1)) ||
        (event == StatsEvent::AssistantMessage && !plus(&counts.assistant_messages, 1)) ||
        (usage.available && (!plus(&counts.input_tokens, usage.input) ||
                             !plus(&counts.output_tokens, usage.output) ||
                             !plus(&counts.measured_replies, 1)))) return false;
    uint32_t value_length = 0;
    if (!encode(counts, out->values[i], &value_length)) return false;
    out->mutations[out->count++] = {out->keys[i], out->values[i], value_length, expiry[i], false};
    if (!member_found)
      out->mutations[out->count++] = {out->keys[i + 3], "1", 1, expiry[i], false};
  }
  return true;
}

size_t RenderAiStatsPage(storage::KvStore& kv, int64_t now_ms, char* output, size_t capacity) {
  if (!output || !capacity || now_ms < 0) return 0;
  StatsCounts total;
  StatsCounts hourly[24]{};
  StatsCounts daily[7]{};
  if (!ReadStats(kv, "all", now_ms, &total)) return 0;
  for (int period = 0; period < 2; ++period) {
    const int count = period == 0 ? 24 : 7;
    const int64_t width = period == 0 ? kHourMs : kDayMs;
    for (int i = 0; i < count; ++i) {
      const int64_t start = (now_ms / width - i) * width;
      char bucket[24];
      if (period == 0) HourBucket(start, bucket); else DayBucket(start, bucket);
      if (!ReadStats(kv, bucket, now_ms, period == 0 ? &hourly[i] : &daily[i])) return 0;
    }
  }
  fortis::codec::history_batch::OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity};
  out.Text(R"FST(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>AI statistics | FortisNet</title><style>body{font:16px system-ui,sans-serif;background:#0b1020;color:#e9edf7;margin:0;padding:3rem 1rem}main{max-width:1100px;margin:auto}a{color:#8db5ff}h1{font-size:2rem}p{color:#b8c2d7}table{width:100%;border-collapse:collapse;background:#151d32;border-radius:12px;overflow:hidden;margin:1.25rem 0 2rem}th,td{text-align:right;padding:.8rem;border-bottom:1px solid #303a52}th:first-child,td:first-child{text-align:left}th{color:#b8c2d7}small{color:#b8c2d7}.chart-panel{background:#151d32;border:1px solid #303a52;border-radius:12px;padding:1rem;margin:1.5rem 0}.chart-head{display:flex;align-items:center;justify-content:space-between;gap:1rem;flex-wrap:wrap}.chart-head h2{margin:.2rem 0}.chart-controls{display:flex;gap:.35rem;flex-wrap:wrap}.chart-controls button{background:#222d49;border:1px solid #43506e;border-radius:7px;color:#dce6fa;padding:.35rem .65rem;cursor:pointer}.chart-controls button[aria-pressed=true]{background:#34518b;border-color:#74a8ff;color:#fff}.resource-chart{display:block;width:100%;height:auto;margin-top:.6rem}@media(max-width:760px){table{display:block;overflow-x:auto}}</style></head><body><main><a href="/ai">&larr; AI models</a><h1>AI statistics</h1><p>Durable aggregates from this server. Times are UTC. Unique users are pseudonymous wallet owners active in each period. Token totals use provider-reported usage from completed Anthropic replies, including cached input tokens.</p>)FST");
  char line[512];
  auto row = [&](const char* label, const StatsCounts& c) {
    const int n = snprintf(line, sizeof(line),
        "<tr><td>%s</td><td>%llu</td><td>%llu</td><td>%llu</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>",
        label, static_cast<unsigned long long>(c.users),
        static_cast<unsigned long long>(c.user_messages),
        static_cast<unsigned long long>(c.assistant_messages),
        static_cast<unsigned long long>(c.input_tokens),
        static_cast<unsigned long long>(c.output_tokens),
        static_cast<unsigned long long>(c.measured_replies));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(line)) out.ok = false;
    else out.Append(line, static_cast<size_t>(n));
  };
  const char* head = "<table><thead><tr><th>Period</th><th>Users</th><th>User messages</th><th>AI messages</th><th>Input tokens</th><th>Output tokens</th><th>Measured replies</th></tr></thead><tbody>";
  out.Text("<h2>Since statistics began</h2>");
  if (total.since_ms) {
    time_t seconds = static_cast<time_t>(total.since_ms / 1000);
    tm utc{};
    char since[40]{};
    if (!gmtime_r(&seconds, &utc) || !strftime(since, sizeof(since), "%Y-%m-%d %H:%M UTC", &utc)) return 0;
    out.Text("<p>Recording since "); out.Text(since); out.Text(".</p>");
  } else out.Text("<p>No recorded AI activity yet.</p>");
  out.Text(head); row("All recorded", total); out.Text("</tbody></table>");
  out.Text(R"FST(<section class="chart-panel"><div class="chart-head"><h2>Activity over time</h2><div class="chart-controls" aria-label="Chart metric"><button type="button" data-metric="users" aria-pressed="false">Users</button><button type="button" data-metric="messages" aria-pressed="true">Messages</button><button type="button" data-metric="tokens" aria-pressed="false">Tokens</button></div><div class="chart-controls" aria-label="Chart period"><button type="button" data-period="hour" aria-pressed="true">24 hours</button><button type="button" data-period="day" aria-pressed="false">7 days</button></div></div><svg id="ai-chart" class="resource-chart" viewBox="0 0 600 180" role="img" aria-label="AI messages per hour over the last 24 hours"></svg></section>)FST");
  for (int period = 0; period < 2; ++period) {
    out.Text(period == 0 ? "<h2>Last 24 hours</h2>" : "<h2>Last 7 days</h2>");
    out.Text(head);
    const int count = period == 0 ? 24 : 7;
    const int64_t width = period == 0 ? kHourMs : kDayMs;
    for (int i = 0; i < count; ++i) {
      const int64_t start = (now_ms / width - i) * width;
      const StatsCounts& counts = period == 0 ? hourly[i] : daily[i];
      time_t seconds = static_cast<time_t>(start / 1000);
      tm utc{};
      char label[40]{};
      if (!gmtime_r(&seconds, &utc) ||
          !strftime(label, sizeof(label), period == 0 ? "%Y-%m-%d %H:00" : "%Y-%m-%d", &utc)) return 0;
      row(label, counts);
    }
    out.Text("</tbody></table>");
  }
  out.Text("<script>const aiHours=[");
  for (int period = 0; period < 2; ++period) {
    if (period) out.Text("];const aiDays=[");
    const int count = period == 0 ? 24 : 7;
    const StatsCounts* counts = period == 0 ? hourly : daily;
    for (int i = count - 1; i >= 0; --i) {
      const StatsCounts& c = counts[i];
      const int n = snprintf(line, sizeof(line), "%s[%llu,%llu,%llu,%llu,%llu]",
          i == count - 1 ? "" : ",", static_cast<unsigned long long>(c.users),
          static_cast<unsigned long long>(c.user_messages),
          static_cast<unsigned long long>(c.assistant_messages),
          static_cast<unsigned long long>(c.input_tokens),
          static_cast<unsigned long long>(c.output_tokens));
      if (n < 0 || static_cast<size_t>(n) >= sizeof(line)) return 0;
      out.Append(line, static_cast<size_t>(n));
    }
  }
  out.Text(R"FST(];(function(){let metric='messages',period='hour';const svg=document.getElementById('ai-chart');function niceTop(v){if(v<=0)return 1;const scale=Math.pow(10,Math.floor(Math.log10(v)));const n=v/scale;return(n<=1?1:n<=2?2:n<=5?5:10)*scale}function value(row){return metric==='users'?row[0]:metric==='messages'?row[1]+row[2]:row[3]+row[4]}function draw(){const data=period==='hour'?aiHours:aiDays;const values=data.map(value),top=niceTop(Math.max(...values)),parts=[];for(let tick=0;tick<=4;tick++){const y=140-tick*28.75;parts.push('<line x1="55" y1="'+y+'" x2="590" y2="'+y+'" stroke="rgba(255,255,255,.12)"/>');parts.push('<text x="49" y="'+(y+3)+'" text-anchor="end" fill="#a1a1aa" font-size="10">'+(top*tick/4).toLocaleString()+'</text>')}let path='';values.forEach((v,i)=>{const x=55+i/Math.max(1,values.length-1)*535,y=140-v/top*115;path+=(i?' L':'M')+x.toFixed(1)+','+y.toFixed(1)});parts.push('<path d="'+path+'" fill="none" stroke="#74a8ff" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>');values.forEach((v,i)=>{const x=55+i/Math.max(1,values.length-1)*535,y=140-v/top*115;parts.push('<circle cx="'+x.toFixed(1)+'" cy="'+y.toFixed(1)+'" r="3" fill="#74a8ff"><title>'+v.toLocaleString()+' '+metric+' · '+(period==='hour'?(values.length-1-i)+' hours ago':(values.length-1-i)+' days ago')+'</title></circle>')});parts.push('<text x="55" y="169" fill="#a1a1aa" font-size="10">'+(period==='hour'?'23h ago':'6d ago')+'</text><text x="590" y="169" text-anchor="end" fill="#a1a1aa" font-size="10">now</text>');svg.innerHTML=parts.join('');svg.setAttribute('aria-label','AI '+metric+' per '+(period==='hour'?'hour over the last 24 hours':'day over the last 7 days'))}document.querySelectorAll('[data-metric],[data-period]').forEach(button=>button.addEventListener('click',()=>{const key=button.dataset.metric?'metric':'period';document.querySelectorAll('[data-'+key+']').forEach(peer=>peer.setAttribute('aria-pressed',peer===button?'true':'false'));if(key==='metric')metric=button.dataset.metric;else period=button.dataset.period;draw()}));draw()})();</script>)FST");
  out.Text("<small>Statistics start with this release; earlier activity is excluded. Usage may undercount provider calls that failed or were interrupted before a completed reply was stored.</small></main></body></html>");
  return out.ok ? out.used : 0;
}
}  // namespace fortis::ai
