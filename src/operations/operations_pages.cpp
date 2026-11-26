// Native-owned HTML templates. Edit this C++ file to change the pages.
// Only the LandingSlot entries below are injected by RenderLandingPage.
#include "operations_pages.h"

namespace fortis::operations {

#include "node_home.inc"

const char kOperationsPackageVersion[] = "0.1.0";

#define FORTIS_TEXT(value) {LandingSlot::kText, value, sizeof(value) - 1}
const LandingSegment kLandingSegments[] = {
#include "dashboard_page.inc"
};
#undef FORTIS_TEXT
const size_t kLandingSegmentCount = sizeof(kLandingSegments) / sizeof(kLandingSegments[0]);

const char kLandingDocsEnabled[] = R"FDOC(<a href="/doc">API reference</a>)FDOC";
const char kLandingDocsDisabled[] = R"FDOC(<span aria-disabled="true">API reference disabled</span>)FDOC";

const char kErrorsPageHtml[] = R"FERROR(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>Node and third-party errors | FortisNet</title>
    <style>
        :root {
            --bg: #0b0b0d;
            --primary: #a970ff;
            --on-surface: #e2e2e2;
            --on-surface-variant: #a1a1aa;
            --green: #34d399;
            --amber: #fbbf24;
            --red: #f87171;
        }
        *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            min-height: 100dvh;
            background: var(--bg);
            color: var(--on-surface);
            font-family: system-ui, -apple-system, 'Segoe UI', sans-serif;
            -webkit-font-smoothing: antialiased;
            padding: 2.5rem 1.5rem 4rem;
        }
        .wrap { max-width: 62rem; margin: 0 auto; }
        .crumb { font-size: 0.75rem; margin-bottom: 1.5rem; }
        .crumb a { color: var(--primary); text-decoration: none; }
        .crumb span { color: var(--on-surface-variant); }
        h1 {
            font-size: 1.6rem;
            font-weight: 800;
            letter-spacing: -0.03em;
            margin-bottom: 0.4rem;
        }
        .sub { font-size: 0.85rem; color: var(--on-surface-variant); margin-bottom: 1.75rem; }
        .chips { display: flex; gap: 1.5rem; flex-wrap: wrap; margin-bottom: 2rem; }
        .chip { display: flex; align-items: baseline; gap: 0.4rem; }
        .chip-val { font-size: 1.1rem; font-weight: 800; font-variant-numeric: tabular-nums; }
        .chip-key {
            font-size: 0.6rem; font-weight: 700; letter-spacing: 0.12em;
            text-transform: uppercase; color: var(--on-surface-variant);
        }
        .card {
            background: linear-gradient(145deg, rgba(24,24,27,0.8) 0%, rgba(11,11,13,0.8) 100%);
            border: 1px solid rgba(255, 255, 255, 0.06);
            border-radius: 0.75rem;
            padding: 1.25rem 1.5rem;
            margin-bottom: 1.5rem;
            overflow-x: auto;
        }
        .card-title {
            font-size: 0.65rem; font-weight: 700; letter-spacing: 0.15em;
            text-transform: uppercase; color: var(--primary); margin-bottom: 0.75rem;
        }
        table { width: 100%; border-collapse: collapse; font-size: 0.8rem; }
        th {
            text-align: left; font-size: 0.6rem; font-weight: 700;
            letter-spacing: 0.12em; text-transform: uppercase;
            color: var(--on-surface-variant);
            padding: 0.35rem 0.75rem 0.35rem 0;
            border-bottom: 1px solid rgba(255,255,255,0.08);
        }
        td {
            padding: 0.45rem 0.75rem 0.45rem 0;
            border-bottom: 1px solid rgba(255,255,255,0.04);
            font-variant-numeric: tabular-nums;
        }
        td.num, th.num { text-align: right; }
        .route { color: var(--on-surface-variant); word-break: break-all; }
        .badge {
            display: inline-block; font-size: 0.6rem; font-weight: 700;
            letter-spacing: 0.08em; text-transform: uppercase;
            border-radius: 9999px; padding: 0.15rem 0.6rem;
        }
        .badge.over { color: var(--red); border: 1px solid rgba(248,113,113,0.5); }
        .badge.under { color: var(--amber); border: 1px solid rgba(251,191,36,0.4); }
        .empty { color: var(--green); font-size: 0.85rem; padding: 0.5rem 0; }
        .status-5xx { color: var(--red); font-weight: 700; }
    </style>
</head>
<body>
    <div class="wrap">
        <div class="crumb"><a href="/">FortisNet Intelligence</a> <span>/ errors</span></div>
        <h1>Errors &amp; dependencies</h1>
        <p class="sub">HTTP 5xx responses (all causes) per service over the last <span id="e-window">15</span> minutes.
           A service crosses the threshold at <span id="e-threshold">5</span> errors in the window.
           Aggregate counts and route templates only — no request data.</p>

        <div class="chips">
            <span class="chip"><span class="chip-val" id="e-total">-</span><span class="chip-key">HTTP 5xx responses</span></span>
            <span class="chip"><span class="chip-val" id="e-over">-</span><span class="chip-key">routes at or above threshold</span></span>
            <span class="chip"><span class="chip-val" id="e-requests">-</span><span class="chip-key">requests in window</span></span>
        </div>

        <p class="sub">The panels retain bounded summaries across restarts. Their counts are observations
          at different layers, not unique failed requests. HTTP 5xx totals above include all causes.</p>
        <div class="card">
            <div class="card-title">Node errors</div>
            <p class="sub">Failures recorded inside the node, such as storage or job processing errors.</p>
            <div id="e-node"></div>
        </div>
        <div class="card">
            <div class="card-title">Third-party errors</div>
            <p class="sub">Failed responses from external services. For example, CoinGecko HTTP 429
              means a provider rate or quota limit; it does not by itself distinguish the two.</p>
            <div id="e-third-party"></div>
        </div>
        <div class="card">
            <div class="card-title" id="incorrect-coin-ids">Incorrect coin IDs</div>
            <p class="sub">Individual coin lookups rejected by the provider (HTTP 404), or invalid identifier syntax.
              These are observations to investigate, not proof that an asset does not exist. Quota limits (429),
              missing quotes and temporary outages are excluded. This view is read-only; there is no database edit control.
              Only catalogued public coin IDs are named; unrecognised identifiers are omitted for privacy and shown as a dash.</p>
            <div id="e-incorrect-ids"></div>
        </div>
        <div class="card">
            <div class="card-title">Unclassified outcomes &amp; client reports</div>
            <p class="sub">Missing prices, network/TLS failures and older summaries do not prove a node
              or provider fault. Client mapping reports are also shown here, without wallet contents.</p>
            <div id="e-unclassified"></div>
            <p class="sub" id="e-persistence"></p>
        </div>

        <div class="card">
            <div class="card-title">HTTP 5xx by service (all causes)</div>
            <div id="e-routes"></div>
        </div>

        <div class="card">
            <div class="card-title">Recent HTTP 5xx responses (all causes)</div>
            <div id="e-recent"></div>
        </div>
    </div>

    <script>
        (function() {
            function $(id) { return document.getElementById(id); }
            function esc(s) {
                return String(s).replace(/[&<>"]/g, function(c) {
                    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
                });
            }
            function hhmmss(sec) {
                var t = new Date(sec * 1000);
                function p(n) { return ('0' + n).slice(-2); }
                return p(t.getHours()) + ':' + p(t.getMinutes()) + ':' + p(t.getSeconds());
            }

            function render(data) {
                var operational = data.operational || {records:[],persisted:true,writeFailed:false};
                var incidents = operational.records || [];
                function incorrect(e) { return e.code === 'provider_rejected_coin_id' || e.code === 'invalid_identifier'; }
                function table(origin, empty) {
                    var selected = incidents.filter(function(e) {
                        var known = e.origin === 'node' || e.origin === 'third_party';
                        return origin === 'incorrect' ? incorrect(e) : !incorrect(e) && (known ? e.origin : 'unclassified') === origin;
                    });
                    if (!selected.length) return '<div class="empty">'+esc(empty)+'</div>';
                    return '<table><thead><tr><th>Service</th><th>Provider</th><th>HTTP</th><th>Code</th><th>Market identifier</th><th class="num">Count</th><th>Last seen</th></tr></thead><tbody>'+
                        selected.slice().sort(function(a,b){return b.lastSeenMs-a.lastSeenMs;}).map(function(e){
                            return '<tr><td>'+esc(e.service)+'</td><td>'+esc(e.provider || '—')+'</td><td>'+esc(e.httpStatus || '—')+
                                '</td><td>'+esc(e.code)+'</td><td>'+esc(e.identifier || '—')+'</td><td class="num">'+esc(e.count)+
                                '</td><td>'+esc(new Date(e.lastSeenMs).toISOString())+'</td></tr>';
                        }).join('')+'</tbody></table>';
                }
                $('e-incorrect-ids').innerHTML = table('incorrect', 'No incorrect coin IDs reported.');
                $('e-node').innerHTML = table('node', 'No node errors recorded.');
                $('e-third-party').innerHTML = table('third_party', 'No third-party errors recorded.');
                $('e-unclassified').innerHTML = table('unclassified', 'No unclassified outcomes recorded.');
                $('e-persistence').textContent = operational.writeFailed ? 'Persistence failed; summaries remain visible in memory and will be retried.' :
                  operational.persisted ? 'Summaries saved to persistent storage.' : 'Latest summaries are waiting for the next storage flush.';
                $('e-window').textContent = data.windowMinutes;
                $('e-threshold').textContent = data.threshold;
                $('e-total').textContent = data.totalErrors;
                $('e-requests').textContent = data.totalRequests;
                var over = data.routes.filter(function(r) { return r.overThreshold; }).length;
                $('e-over').textContent = over;
                $('e-over').style.color = over > 0 ? 'var(--red)' : 'var(--green)';

                if (data.routes.length === 0) {
                    $('e-routes').innerHTML = '<div class="empty">No server errors in the window.</div>';
                } else {
                    var rows = data.routes.map(function(r) {
                        var rate = r.requests > 0 ? Math.round(100 * r.errors / r.requests) : 0;
                        return '<tr><td>' + esc(r.service) + '</td>' +
                            '<td class="route">' + esc(r.route) + '</td>' +
                            '<td class="num">' + r.requests + '</td>' +
                            '<td class="num status-5xx">' + r.errors + '</td>' +
                            '<td class="num">' + rate + '%</td>' +
                            '<td><span class="badge ' + (r.overThreshold ? 'over">over threshold' : 'under">below threshold') + '</span></td></tr>';
                    }).join('');
                    $('e-routes').innerHTML = '<table><thead><tr><th>Service</th><th>Route</th>' +
                        '<th class="num">Requests</th><th class="num">Errors</th><th class="num">Rate</th><th>State</th></tr></thead>' +
                        '<tbody>' + rows + '</tbody></table>';
                }

                if (data.recent.length === 0) {
                    $('e-recent').innerHTML = '<div class="empty">Nothing recorded.</div>';
                } else {
                    var recent = data.recent.map(function(e) {
                        return '<tr><td>' + hhmmss(e.sec) + '</td>' +
                            '<td>' + esc(e.service) + '</td>' +
                            '<td class="route">' + esc(e.route) + '</td>' +
                            '<td class="num status-5xx">' + e.status + '</td></tr>';
                    }).join('');
                    $('e-recent').innerHTML = '<table><thead><tr><th>Time</th><th>Service</th><th>Route</th>' +
                        '<th class="num">HTTP</th></tr></thead><tbody>' + recent + '</tbody></table>';
                }
            }

            function poll() {
                fetch('/errorsz').then(function(r) { return r.json(); }).then(render).catch(function() {});
            }
            poll();
            setInterval(poll, 5000);
        })();
    </script>
</body>
</html>)FERROR";
const size_t kErrorsPageHtmlLength = sizeof(kErrorsPageHtml) - 1;

// The embedded report history and HTML form one static response. Clang
// supports this size but warns when it exceeds the standard's minimum.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"
#endif
const char kBenchmarkPageHtml[] = R"BENCH(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Benchmark Stats | FortisNet</title>
<style>
:root{color-scheme:light;font:16px/1.55 system-ui;background:#f3f6f7;color:#18343d}
*{box-sizing:border-box}body{margin:0}main{max-width:74rem;margin:auto;padding:2rem 1rem 4rem}
nav{display:flex;gap:1.2rem;flex-wrap:wrap}a{color:#007f69}h1{font-size:clamp(2rem,5vw,3rem);margin:.7em 0 .15em}
h2{margin:0 0 .4em;font-size:1.3rem}p{margin:.45em 0 1em;color:#536873}
section{background:#fff;border:1px solid #dce5e9;border-radius:.8rem;padding:1rem;margin-top:1.25rem}
.scroll{overflow-x:auto}table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:.9rem}
th,td{padding:.55rem .65rem;border-bottom:1px solid #dce5e9;text-align:right;white-space:nowrap}
th:first-child,td:first-child{text-align:left}th{color:#536873}tbody tr:last-child td{border-bottom:0}
label{display:block;font-weight:600;margin-bottom:.4rem}input,select{max-width:100%;font:inherit}details{margin:.8rem 0}
#report-meta,#live-state{overflow-wrap:anywhere}output{display:block;color:#007f69}
@media(max-width:600px){main{padding:1rem .7rem 2rem}section{padding:.75rem}}
</style>
<main>
<nav aria-label="Node pages"><a href="/">Node status</a><a href="/stats">Request statistics</a></nav>
<h1>Benchmark Stats</h1>
<p>Historical workload evidence and a separate view of live internal timings.</p>
<section aria-labelledby="interpret-heading"><h2 id="interpret-heading">How to read this page</h2><p><strong>Published runs are historical experiments, not a live capacity promise.</strong> Their workload, environment and timing boundaries matter. A fast local KV run cannot be treated as the number of real wallet requests this node can serve.</p><p><strong>p95</strong> means about 95 out of 100 measured operations finished within that time. Client benchmark latency may include network and connection setup; the dashboard's route latency is measured on the server. Compare matching workloads and boundaries.</p><p><a href="/#guide">Read the plain-language measurement guide</a> · <a href="/stats">Inspect current API performance</a></p></section>
<section aria-labelledby="run-heading">
<h2 id="run-heading">Benchmark results</h2>
<p id="durability-scope">The mixed canary test measures wallet and device traffic. The local runs cover direct KV and emulated guest capacity. Abrupt host failure was outside these runs.</p>
<label for="published-run">Published run</label><select id="published-run"></select>
<details><summary>Open another benchmark report</summary>
<p>JSONL files are read in this browser.</p>
<label for="report">Benchmark report</label><input id="report" type="file" accept=".jsonl,.ndjson,application/x-ndjson,text/plain">
</details>
<output id="report-meta" role="status"></output>
<div class="scroll" tabindex="0" aria-label="Benchmark results table">
<table id="runs-table" hidden><thead><tr><th scope="col">Workload</th><th scope="col">Offered</th><th scope="col">Completed</th><th scope="col">Driver rejects</th><th scope="col">Other failures</th><th scope="col">Read p95</th><th scope="col">Write p95</th><th scope="col">Health</th></tr></thead><tbody id="runs"></tbody></table>
</div>
<div id="canary-results" hidden>
<p>Latency includes client connection time. Sync counts include background storage activity during each burst. Incomplete prices count separately from request and authorization errors.</p>
<div class="scroll" tabindex="0" aria-label="Canary write and flush counts">
<table><thead><tr><th scope="col">Mode</th><th scope="col">Readers</th><th scope="col">Bursts</th><th scope="col">Durable write frames</th><th scope="col">File and directory syncs</th></tr></thead><tbody id="canary-writes"></tbody></table>
</div>
<div class="scroll" tabindex="0" aria-label="Canary route latency and errors">
<table><thead><tr><th scope="col">Mode</th><th scope="col">Route</th><th scope="col">Requests</th><th scope="col">p95</th><th scope="col">p99</th><th scope="col">Errors</th><th scope="col">Incomplete prices</th></tr></thead><tbody id="canary-routes"></tbody></table>
</div>
</div>
<div id="run-details"></div>
</section>
<section aria-labelledby="live-heading">
<h2 id="live-heading">Live node timings</h2>
<p>Changes between successive samples. Latency percentiles show histogram bucket bounds.</p>
<output id="live-state" role="status">Waiting for two samples…</output>
<div id="live-rate-chart" aria-label="Measured internal operations per second"></div><p>Bars show measured internal phase events per second, not unique user requests or maximum capacity. KV means the node's local key-value database. A lock is exclusive access to shared data; wait time is time spent queuing for it. Phases can overlap; do not add their rates. Bucket timings in the table are estimates.</p>
<div class="scroll" tabindex="0" aria-label="Live timing table">
<table><thead><tr><th scope="col">Phase</th><th scope="col">Samples</th><th scope="col">Rate / s</th><th scope="col">p95</th><th scope="col">p99</th><th scope="col">Max bucket</th></tr></thead><tbody id="live"></tbody></table>
</div></section>
</main>
<script type="application/json" id="published-reports">)BENCH"
#include "benchmark_reports.inc"
R"BENCH(</script>
<script>
'use strict';
const byId = id => document.getElementById(id);
const format = new Intl.NumberFormat();
const count = n => Number.isFinite(n) ? format.format(n) : '—';
const ms = n => Number.isFinite(n) ? (n / 1000).toLocaleString(undefined, {maximumFractionDigits: 3}) + ' ms' : '—';
function row(target, values) {
  const tr = document.createElement('tr');
  for (const value of values) {
    const td = document.createElement('td');
    td.textContent = value;
    tr.appendChild(td);
  }
  target.appendChild(tr);
}
function note(target, value) {
  const p = document.createElement('p');
  p.textContent = value;
  target.appendChild(p);
}
function subtract(current, previous, bounds) {
  if (!Array.isArray(current) || !Array.isArray(previous) ||
      current.length !== previous.length || current.length !== bounds.length + 1) return null;
  const buckets = current.map((value, i) => value - previous[i]);
  if (buckets.some(value => !Number.isFinite(value) || value < 0)) return null;
  const total = buckets.reduce((sum, value) => sum + value, 0);
  function quantile(p) {
    if (!total) return '—';
    let rank = Math.ceil(total * p);
    const bucket = buckets.findIndex(value => (rank -= value) <= 0);
    return bucket < bounds.length ? '≤ ' + ms(bounds[bucket]) : '> ' + ms(bounds[bounds.length - 1]);
  }
  return [total, quantile(.95), quantile(.99), quantile(1)];
}
const series = {
  databaseActivity: {
    reads: 'KV read calls', writes: 'KV write calls', lockWait: 'KV mutex wait',
    readHold: 'Point read mutex hold', scanHold: 'Scan mutex hold', writeHold: 'Write mutex hold',
    sweepHold: 'Expiry sweep mutex hold', compactionHold: 'Compaction mutex hold',
    recoveryHold: 'Recovery mutex hold', fileRead: 'File read', fileSync: 'File sync',
    samplerWakeLag: 'Timer wake lag'
  },
  httpTiming: {
    dispatch: 'HTTP dispatch queue', readLockWait: 'Read route lock wait',
    readLockHold: 'Read route lock hold', mutationLockWait: 'Write route lock wait',
    mutationLockHold: 'Write route lock hold'
  }
};
let last;
async function refresh() {
  try {
    const response = await fetch('/statusz', {cache:'no-store'});
    if (!response.ok) throw new Error('status');
    const current = await response.json();
    const activity = current.databaseActivity;
    const previous = last && last.databaseActivity;
    const previousStatus = last;
    if (!activity) {
      last = undefined;
      byId('live').replaceChildren();
      byId('live-state').textContent = 'Database timing is unavailable.';
      return;
    }
    if (previous && activity.sampleMs === previous.sampleMs) return;
    last = current;
    byId('live').replaceChildren();
    if (!previous || activity.sampleMs < previous.sampleMs) {
      byId('live-state').textContent = 'Waiting for the next sample…';
      return;
    }
    const seconds = (activity.sampleMs - previous.sampleMs) / 1000;
    const rateBars = [];
    for (const [group, labels] of Object.entries(series)) {
      const values = current[group], prior = previousStatus[group];
      if (!values || !prior || !Array.isArray(values.boundsUs) ||
          JSON.stringify(values.boundsUs) !== JSON.stringify(prior.boundsUs)) continue;
      for (const [key, label] of Object.entries(labels)) {
        const delta = subtract(values[key], prior[key], values.boundsUs);
        if (delta) {
          row(byId('live'), [label, count(delta[0]), (delta[0] / seconds).toFixed(1), ...delta.slice(1)]);
          rateBars.push([label, delta[0] / seconds]);
        }
      }
    }
    const chart = byId('live-rate-chart');
    chart.replaceChildren();
    const active = rateBars.filter(entry => entry[1] > 0).sort((a,b) => b[1] - a[1]).slice(0,8);
    const peak = active.length ? active[0][1] : 0;
    if (!active.length) note(chart, 'No internal phase events recorded in this sample interval.');
    for (const [label, rate] of active) {
      const item = document.createElement('div');
      const text = document.createElement('p');
      text.textContent = label + ' · ' + rate.toFixed(1) + ' events / s';
      const bar = document.createElement('div');
      bar.style.cssText = 'height:7px;background:#007f69;border-radius:4px;max-width:100%';
      bar.style.width = (100 * rate / peak) + '%';
      item.append(text, bar); chart.append(item);
    }
    const auth = current.snapshotAuth;
    const snapshot = current.priceSnapshot;
    const publication = snapshot ? ' · Price publication: ' + (snapshot.ready ? 'ready' : 'unavailable') +
      ', generation ' + count(snapshot.sequence) + ', age ' + count(snapshot.ageMs) +
      ' ms, ' + count(snapshot.unavailable) + ' unavailable reads' : '';
    const cache = auth ? ' · Snapshot auth since boot: ' + count(auth.hits) + ' hits, ' +
      count(auth.misses) + ' misses, ' + count(auth.invalidations) + ' invalidations' : '';
    byId('live-state').textContent = seconds.toFixed(1) + ' s window · KV waiters: ' +
      count(activity.lockWaiters) + ' · Peak since boot: ' + count(activity.peakLockWaiters) + cache + publication;
  } catch {
    last = undefined;
    byId('live').replaceChildren();
    byId('live-state').textContent = 'Live status unavailable. Retrying…';
  } finally {
    setTimeout(refresh, 5000);
  }
}
function guestSummary(run) {
  const status = run.statuses || {};
  const reads = status.read || {}, writes = status.write || {}, health = status.health || {};
  const total = values => Object.values(values).reduce((sum, value) =>
    Number.isFinite(value) ? sum + value : sum, 0);
  const completed = (reads[200] || 0) + (writes[200] || 0);
  const rejected = (reads.driver_rejected || 0) + (writes.driver_rejected || 0);
  const transport = (reads[0] || 0) + (writes[0] || 0);
  const errors = total(reads) + total(writes) - completed - rejected - transport;
  return [count(run.offered_reads + run.offered_writes), count(completed), count(rejected),
    count(errors) + ' HTTP / ' + count(transport) + ' transport',
    ms(run.read_us?.p95), ms(run.write_us?.p95),
    total(health) ? count(health[200] || 0) + '/' + count(total(health)) + ' HTTP 200' : '—'];
}
function hostSummary(run) {
  return [count(run.submitted_reads + run.submitted_writes), count(run.read_ok + run.write_ok),
    count(run.read_rejected + run.write_rejected), count(run.errors),
    ms(run.read_us?.p95), ms(run.write_us?.p95), '—'];
}
function clearReport() {
  byId('runs').replaceChildren();
  byId('canary-writes').replaceChildren();
  byId('canary-routes').replaceChildren();
  byId('run-details').replaceChildren();
  byId('runs-table').hidden = true;
  byId('canary-results').hidden = true;
  byId('report-meta').textContent = '';
}
function renderReport(records) {
  clearReport();
  const target = byId('runs'), details = byId('run-details');
  let environment, method, stages = 0, canaryRuns = 0;
  for (const record of records) {
    if (!record || typeof record !== 'object' || Array.isArray(record)) throw new Error('Invalid record');
    if (typeof record.environment === 'string') {
      environment = [record.environment, record.storage, record.runtime_image].filter(Boolean).join(' · ');
      method = record.method;
    }
    if (record.mode === 'burst' || record.mode === 'sustained') {
      const guest = !!record.statuses;
      const label = record.mode === 'burst' ? 'Burst' : guest ? count(record.offered_rate) + '/s offered' :
        count((record.submitted_reads + record.submitted_writes) / record.offered_seconds) + '/s offered';
      row(target, [label + (record.batch_limit ? ' · batch ' + record.batch_limit : ''),
        ...(guest ? guestSummary(record) : hostSummary(record))]);
      const duration = record.elapsed_s ?? (Number.isFinite(record.elapsed_us) ? record.elapsed_us / 1000000 : null);
      const fields = [
        ['Elapsed including drain', Number.isFinite(duration) ? count(duration) + ' s' : null],
        ['Read p99', ms(record.read_us?.p99)], ['Write p99', ms(record.write_us?.p99)],
        ['Driver start lag p95', ms(record.driver_start_lag_us?.p95)],
        ['Durable frames', Number.isFinite(record.write_frames) ? count(record.write_frames) : null],
        ['Compactions completed', Number.isFinite(record.compactions_completed) ? count(record.compactions_completed) : null]
      ].filter(([, value]) => value && value !== '—');
      if (fields.length) note(details, label + ' · ' + fields.map(([key, value]) => key + ': ' + value).join(' · '));
      const timing = record.database_activity_delta;
      if (timing && typeof timing === 'object') {
        const parts = Object.entries(timing).filter(([, value]) => value?.p95_us != null)
          .map(([name, value]) => (series.databaseActivity[name] || name) + ' p95 ' +
            String(value.p95_us).replace('<=', '≤ ') + ' μs');
        if (parts.length) note(details, label + ' · ' + parts.join(' · '));
      }
      const http = record.http_timing_delta;
      if (http && typeof http === 'object') {
        const parts = Object.entries(http).filter(([, value]) => value?.p95_us != null)
          .map(([name, value]) => (series.httpTiming[name] || name) + ' p95 ' +
            String(value.p95_us).replace('<=', '≤ ') + ' μs');
        if (parts.length) note(details, label + ' · ' + parts.join(' · '));
      }
      ++stages;
    } else if (record.mode === 'group-commit-canary') {
      const mode = record.label || (record.price_store ? 'Separate price store' :
        record.group_commit ? 'Group commit on' : 'Group commit off');
      row(byId('canary-writes'), [mode, count(record.users), count(record.rounds),
        count(record.frames), count(record.flushes)]);
      if (record.external_health) {
        const health = record.external_health;
        const statuses = Object.entries(health.status || {}).map(([status, total]) =>
          count(total) + (status === '0' ? ' transport errors' : ' HTTP ' + status));
        note(details, mode + ' independent health probes · ' + statuses.join(' · ') +
          ' · p95 ' + ms(health.all_latency_ms?.p95 * 1000) +
          ' · p99 ' + ms(health.all_latency_ms?.p99 * 1000));
      }
      if (record.price_store) {
        const prices = record.price_store;
        note(details, 'Price-store activity · ' + count(prices.memoryHits) + ' memory hits · ' +
          count(prices.contractMemoryHits) + ' contract and chain-token hits · ' +
          count(prices.writeFrames) + ' price frames · ' + count(prices.fileSyncs) +
          ' price syncs. The table shows device-store frames and syncs.');
      }
      if (record.preconditions) {
        const warmup = Array.isArray(record.preconditions) ? record.preconditions : [record.preconditions];
        note(details, mode + ' warm-up fresh prices · snapshot ' +
          warmup.map(sample => count(sample.snapshot_fresh_ids) + '/6').join(', ') +
          ' · spot ' + warmup.map(sample => count(sample.spot_fresh_ids) + '/12').join(', '));
      }
      if (record.freshness_soak) {
        const soak = record.freshness_soak;
        const coverage = bins => Object.entries(bins).map(([fresh, samples]) =>
          fresh + '/6 fresh in ' + count(samples) + ' samples').join(', ');
        note(details, mode + ' freshness check · ' + count(soak.samples) + ' samples, ' +
          count(soak.interval_seconds) + ' s apart');
        note(details, 'Core snapshot · ' + coverage(soak.snapshot_fresh));
        note(details, 'Spot contract and chain tokens · ' + coverage(soak.spot_contract_fresh));
      }
      if (record.ingress) {
        const ingress = record.ingress;
        note(details, mode + ' · QEMU listener backlog ' + count(ingress.listen_backlog) +
          ' · Linux listen overflows ' + count(ingress.listen_overflows) +
          ' · listen drops ' + count(ingress.listen_drops));
        for (const [route, timing] of Object.entries(ingress.routes || {})) {
          note(details, route + ' · Nginx upstream header p95 ' + ms(timing.upstream_header_us?.p95) +
            ', p99 ' + ms(timing.upstream_header_us?.p99) +
            ' · guest app p95 ' + ms(timing.guest_app_us?.p95) +
            ', p99 ' + ms(timing.guest_app_us?.p99));
        }
      }
      if (record.public_ingress) {
        const ingress = record.public_ingress;
        note(details, mode + ' · Nginx recorded ' + count(ingress.nginx_seen) + ' of ' +
          count(ingress.requests) + ' benchmark request IDs · ' +
          count(ingress.transport_with_nginx_record) + ' transport failures with an Nginx access record');
        if (Number.isFinite(ingress.listen_overflows)) {
          note(details, 'Host kernel deltas · ' + count(ingress.listen_overflows) + ' listen overflows · ' +
            count(ingress.listen_drops) + ' listen drops · ' + count(ingress.syncookies_sent) + ' SYN cookies');
        }
        if (ingress.protocol) note(details, mode + ' · Nginx protocol ' + ingress.protocol);
        if (Number.isFinite(ingress.max_conntrack)) {
          note(details, 'Host tracked connections · sampled peak ' + count(ingress.max_conntrack));
        }
        for (const [origin, statuses] of Object.entries(ingress.external_health || {})) {
          note(details, origin + ' independent health probes · ' + Object.entries(statuses)
            .map(([status, total]) => count(total) + (status === '0' ? ' transport errors' : ' HTTP ' + status))
            .join(' · '));
        }
        for (const [route, timing] of Object.entries(ingress.routes || {})) {
          note(details, route + ' · Nginx upstream headers p95 ' + ms(timing.upstream_header_us?.p95) +
            ', p99 ' + ms(timing.upstream_header_us?.p99));
          if (timing.accept_to_request_ms) {
            note(details, route + ' · Nginx accept to first HTTP request (approx.) p99 ' +
              ms(timing.accept_to_request_ms.p99 * 1000));
          }
        }
      }
      for (const [name, result] of Object.entries(record.http_controls || {})) {
        note(details, name + ' · negotiated HTTP/' + result.protocol + ' · ' + count(result.requests) +
          ' requests · ' + count(result.statuses?.[200] ?? 0) + ' HTTP 200 · ' +
          count(result.new_connections) + ' new connections · p95 ' + ms(result.latency_ms?.p95 * 1000) +
          ' · p99 ' + ms(result.latency_ms?.p99 * 1000));
      }
      for (const [name, results] of Object.entries(record.tls_controls || {})) {
        note(details, name + ' · TLS handshake p99 · ' + results.map(result =>
          count(result.concurrency) + ' clients: ' + ms(result.handshake_ms?.p99 * 1000) +
          ' (' + count(result.completed) + ' HTTP 200)').join(' · '));
      }
      for (const [name, result] of Object.entries(record.wiki_cat_soak || {})) {
        note(details, name + ' · WikiCat · ' + count(result.samples) + ' samples · ' +
          count(result.stale_after_first) + ' stale after first read · ' + count(result.missing) +
          ' missing · maximum cache age ' + ms(result.cache_age_ms?.p100 * 1000));
      }
      if (record.wiki_cat_provider) {
        const probe = record.wiki_cat_provider;
        note(details, 'WikiCat direct CoinGecko probe · ' + Object.entries(probe.status)
          .map(([status, total]) => count(total) + ' HTTP ' + status).join(' · ') +
          ' · maximum provider observation age ' + count(probe.observation_age_s?.p100) + ' s');
      }
      if (record.provider) {
        const provider = record.provider;
        const apiRateLimits = Object.values(record.routes || {})
          .reduce((total, route) => total + (route.status?.[429] || 0), 0);
        note(details, 'CoinGecko ' + provider.access + ' · ' + count(provider.http_429) +
          ' upstream HTTP 429s over ' + count(provider.observed_seconds) + ' s');
        if (Object.values(record.routes || {}).every(route => route.status))
          note(details, mode + ' · ' + count(apiRateLimits) + ' HTTP 429s returned by the canary');
      }
      if (record.skipped_rotations) note(details, mode + ' skipped rotations after credential loss · ' + count(record.skipped_rotations));
      for (const [route, result] of Object.entries(record.routes || {})) {
        const latency = result.all_latency_ms || {};
        row(byId('canary-routes'), [mode, route, count(result.requests),
          ms(latency.p95 * 1000), ms(latency.p99 * 1000), count(record.errors?.[route]),
          route === 'spot' || route === 'snapshot' ? count(result.outcome?.incomplete ?? 0) : '—']);
        const milestones = [
          ['TCP connected', result.milestones_ms?.tcp],
          ['TLS ready', result.milestones_ms?.tls],
          ['HTTP headers received', result.milestones_ms?.headers],
          ['HTTP body received', result.milestones_ms?.body]
        ].filter(([, values]) => values);
        if (milestones.length) note(details, mode + ' ' + route + ' client milestones from request start · ' +
          milestones.map(([name, values]) => name + ' p95 ' + ms(values.p95 * 1000) +
            ', p99 ' + ms(values.p99 * 1000)).join(' · '));
        const handshake = result.tls_handshake_ms;
        if (handshake) note(details, mode + ' ' + route + ' TLS handshake after TCP · p95 ' +
          ms(handshake.p95 * 1000) + ' · p99 ' + ms(handshake.p99 * 1000));
        const failures = Object.entries(result.transport_failures || {});
        if (failures.length) note(details, mode + ' ' + route + ' transport failures · ' +
          failures.map(([reason, total]) => reason + ' ' + count(total)).join(', '));
        const reasons = Object.entries(result.error_codes || {});
        if (reasons.length) note(details, mode + ' ' + route + ' errors · ' +
          reasons.map(([reason, total]) => reason + ' ' + count(total)).join(', '));
        if (Number.isFinite(result.responses_with_stale)) {
          note(details, mode + ' ' + route + ' freshness · ' + count(result.responses_with_stale) +
            ' responses with stale quotes · ' + count(result.responses_with_missing) +
            ' with missing or invalid IDs');
        }
        if (result.fresh_by_id) {
          const breakdown = document.createElement('details');
          const summary = document.createElement('summary');
          summary.textContent = mode + ' ' + route + ' fresh quotes by asset';
          breakdown.appendChild(summary);
          for (const [id, fresh] of Object.entries(result.fresh_by_id))
            note(breakdown, id + ' · ' + count(fresh) + ' / ' + count(result.requests));
          details.appendChild(breakdown);
        }
      }
      ++canaryRuns;
    } else if (record.mode === 'guest-crash-replay') {
      note(details, 'Guest restart: ' + count(record.restart_seconds) + ' s · Counter before kill: ' +
        count(record.counter_before_crash) + ' · Recovered counter: ' + count(record.counter));
    } else if (record.mode === 'process-crash') {
      note(details, 'Process crash replay: ' +
        Object.entries(record).filter(([key, value]) => key.endsWith('_us') && Number.isFinite(value))
          .map(([key, value]) => key.replace(/_us$/, '').replaceAll('_', ' ') + ' ' + ms(value)).join(' · '));
    } else if (record.mode === 'device-contract' && record.requests && typeof record.requests === 'object') {
      note(details, 'Device contract: ' + Object.entries(record.requests).map(([key, value]) =>
        key.replaceAll('_', ' ') + ' HTTP ' + count(value?.status) + ' (' + ms(value?.latency_us) + ')').join(' · '));
    }
  }
  if (!stages && !canaryRuns && !details.childElementCount) throw new Error('No benchmark results');
  byId('runs-table').hidden = !stages;
  byId('canary-results').hidden = !canaryRuns;
  if (canaryRuns && typeof method === 'string') note(details, method);
  const summary = [];
  if (environment) summary.push(environment);
  if (stages) summary.push(count(stages) + ' workload stages');
  if (canaryRuns) summary.push(count(canaryRuns) + ' canary runs');
  byId('report-meta').textContent = summary.join(' · ');
}
let importId = 0;
const published = JSON.parse(byId('published-reports').textContent);
const publishedRun = byId('published-run');
for (const [index, report] of published.entries()) {
  const option = document.createElement('option');
  option.value = String(index);
  option.textContent = report.label;
  publishedRun.appendChild(option);
}
function showPublished() {
  ++importId;
  const report = published[Number(publishedRun.value)];
  if (report) renderReport(report.records);
}
publishedRun.addEventListener('change', showPublished);
showPublished();
byId('report').addEventListener('change', async event => {
  const id = ++importId;
  const file = event.target.files[0];
  if (!file) { showPublished(); return; }
  try {
    if (file.size > 2 * 1024 * 1024) throw new Error('File too large');
    const raw = await file.text();
    if (id !== importId) return;
    const lines = raw.split(/\r?\n/).filter(line => line.trim());
    if (lines.length > 250) throw new Error('Too many records');
    renderReport(lines.map(line => JSON.parse(line)));
  } catch {
    if (id !== importId) return;
    clearReport();
    byId('report-meta').textContent = 'Could not read the benchmark report. Use runner JSONL under 2 MB and 250 records.';
  }
});
refresh();
</script></html>)BENCH";

const size_t kBenchmarkPageHtmlLength = sizeof(kBenchmarkPageHtml) - 1;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

}  // namespace fortis::operations
