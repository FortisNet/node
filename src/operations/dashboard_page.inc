// Native-owned dashboard template. Static text and numeric capability slots only.
FORTIS_TEXT(R"FDASH(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"><link rel="icon" href="/assets/fortis-favicon.svg" type="image/svg+xml"><meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='7' fill='%23007f69'/%3E%3Ctext x='10' y='24' fill='white' font-size='24'%3EF%3C/text%3E%3C/svg%3E"><meta name="color-scheme" content="light"><title>FortisNet · Node observability</title>
<style>
:root{--ink:#18343d;--muted:#536873;--line:#dce5e9;--bg:#f3f6f7;--card:#fff;--primary:#007f69;--blue:#376dc9;--red:#be3d49;--amber:#986013;--nav:#122c35;font:14px/1.55 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:var(--ink);background:var(--bg)}
*{box-sizing:border-box}body{margin:0}a{color:var(--primary);text-underline-offset:3px}button,input,select{font:inherit}button,select{cursor:pointer}button:disabled{cursor:default}button:focus-visible,a:focus-visible,summary:focus-visible,input:focus-visible,select:focus-visible,[tabindex]:focus-visible{outline:3px solid #438dd1;outline-offset:3px}button{border:1px solid var(--line);background:white;color:var(--ink);padding:.5rem .8rem;border-radius:7px}button[aria-pressed=true]{background:var(--ink);color:white;border-color:var(--ink)}.skip{position:fixed;top:-100px;z-index:99;background:white;padding:1rem}.skip:focus{top:0}.sidebar{position:fixed;inset:0 auto 0 0;width:222px;background:var(--nav);color:#eff8f7;padding:28px 20px;display:flex;flex-direction:column;z-index:5}.brand{font-size:21px;font-weight:750;letter-spacing:-.6px;color:white;text-decoration:none;display:flex;align-items:center;gap:9px}.mark{background:#41c6a8;color:#12382e;border-radius:9px;width:32px;height:32px;display:grid;place-items:center;font-size:18px}.side-note{font-size:12px;color:#a7bfc4;margin:8px 0 28px}.nav-label{text-transform:uppercase;letter-spacing:1.5px;font-size:10px;color:#9cb6be;margin:18px 12px 8px}.sidebar nav{display:grid;gap:5px}.sidebar nav a{color:#cfdee2;text-decoration:none;padding:10px 12px;border-radius:7px;display:flex;align-items:center;gap:12px}.sidebar nav a[aria-current=page]{color:#fff;background:#28505c}.sidebar nav a:hover{background:#20424e}.nav-icon{width:18px;text-align:center;color:#63cbb3}.side-bottom{margin-top:auto;padding-top:25px;font-size:12px;color:#a7bfc4}.side-bottom a{color:#b3e7dc}.workspace{margin-left:222px;min-height:100vh}.topbar{height:69px;border-bottom:1px solid var(--line);background:#fff;display:flex;align-items:center;justify-content:space-between;padding:0 34px;gap:15px}.crumb{font-size:12px;color:var(--muted)}.crumb b{color:var(--ink);font-weight:600}.live{display:flex;align-items:center;gap:8px;font-size:12px}.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#73838c}.live[data-state=live] .dot{background:#00866a}.live[data-state=stale] .dot{background:var(--red)}main{max-width:1600px;margin:auto;padding:30px 34px 50px}h1{font-size:clamp(25px,2.5vw,34px);line-height:1.2;letter-spacing:-.9px;margin:0 0 10px}h2{font-size:16px;letter-spacing:-.2px;margin:0 0 5px}h3{font-size:14px;margin:0 0 5px}p{margin:5px 0 14px}.muted,.desc{color:var(--muted)}.desc{font-size:12px}.eyebrow{color:var(--primary);text-transform:uppercase;letter-spacing:1.8px;font-size:10px;font-weight:750;margin-bottom:10px}.page-head{display:flex;align-items:flex-end;justify-content:space-between;gap:22px;margin-bottom:22px}.page-head p{margin-bottom:0;max-width:760px}.toolbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.updated{font-size:11px;color:var(--muted)}.notice{padding:12px 16px;border:1px solid #b8d4e8;background:#edf7ff;border-radius:8px;margin-bottom:18px;font-size:13px}.notice.error{background:#fff0f0;border-color:#e4b6bb;color:#8f2533}[hidden]{display:none!important}.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:16px;margin-bottom:22px}.card,.panel{border:1px solid var(--line);background:var(--card);border-radius:11px;box-shadow:0 2px 4px #18343d03}.card{padding:18px 19px}.card-label{font-size:12px;color:var(--muted);display:flex;justify-content:space-between;gap:8px}.metric{font-size:clamp(25px,2.8vw,35px);font-weight:680;letter-spacing:-1px;line-height:1.25;margin:12px 0 8px;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}.unit{font-size:14px;color:var(--muted);font-weight:450;letter-spacing:0}.card .desc{margin:0;font-size:11px}.pill{font-size:10px;border-radius:4px;padding:2px 6px;color:var(--primary);background:#e6f5ef;white-space:nowrap}.panel{padding:21px;margin-bottom:18px;min-width:0}.panel-head{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;margin-bottom:14px}.panel-head p{margin:0}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 18px}.range{display:flex;gap:4px}.range button{font-size:11px;padding:4px 9px;white-space:nowrap}.chart{position:relative;min-height:210px}.chart svg{display:block;width:100%;height:185px;overflow:visible}.chart-note{font-size:11px;color:var(--muted);margin:5px 0 0;min-height:18px}.chart-tip{position:absolute;top:4px;left:60px;padding:6px 9px;background:var(--nav);color:white;border-radius:5px;font-size:11px;pointer-events:none;max-width:85%;z-index:2}.legend{font-size:11px;color:var(--muted);display:flex;gap:15px;flex-wrap:wrap}.swatch{display:inline-block;width:8px;height:8px;margin-right:5px;border-radius:50%;background:var(--primary)}.swatch.red{background:var(--red)}.swatch.blue{background:var(--blue)}.scroll{overflow:auto;max-width:100%}table{border-collapse:collapse;width:100%;font-size:12px;font-variant-numeric:tabular-nums}th{font-size:10px;text-transform:uppercase;letter-spacing:.6px;color:var(--muted);font-weight:600;white-space:nowrap}th,td{padding:12px 10px;border-bottom:1px solid #e8eef0;text-align:right}th:first-child,td:first-child{text-align:left}tbody tr:last-child td{border-bottom:0}td code{font-size:11px;white-space:nowrap}td strong{font-weight:650}.bar-cell{min-width:100px}.bar-track{height:5px;background:#e8eff1;border-radius:6px;margin-top:7px;overflow:hidden}.bar-fill{height:100%;width:0;background:var(--primary);border-radius:6px}.table-tools{display:flex;gap:10px;justify-content:space-between;margin:12px 0}.table-tools input{max-width:100%;width:310px;border:1px solid var(--line);border-radius:6px;padding:7px 10px}.table-tools label{font-size:11px;color:var(--muted)}.table-tools select{border:1px solid var(--line);background:white;padding:6px;border-radius:6px}.definition-list{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:9px 18px;font-size:12px;margin:12px 0}.definition-list dt{color:var(--muted)}.definition-list dd{margin:0;text-align:right;font-variant-numeric:tabular-nums;overflow-wrap:anywhere;max-width:100%}.storage-number{font-size:25px;letter-spacing:-.6px;font-weight:650;margin:10px 0}.storage-number small{font-size:13px;font-weight:400;color:var(--muted)}.storage-meter{height:10px;background:#e7eef0;border-radius:6px;overflow:hidden;margin:12px 0}.storage-meter>div{height:100%;background:var(--primary);width:0}.explain{padding:14px 16px;background:#f4f8f8;border-left:3px solid #8bc8ba;border-radius:0 6px 6px 0;font-size:12px;margin-top:14px;color:#3a555f}.section-head{display:flex;align-items:center;justify-content:space-between;margin:28px 0 12px;gap:12px}.link-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}.link-card{display:block;padding:15px;border:1px solid var(--line);border-radius:8px;text-decoration:none;color:var(--ink)}.link-card:hover{border-color:#6bad9d;background:#f7fcfa}.link-card strong{display:block;font-size:13px;margin-bottom:4px}.link-card span{display:block;font-size:11px;color:var(--muted)}.guide-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}.guide-grid article{padding:18px;background:white;border:1px solid var(--line);border-radius:9px}.guide-grid p,.guide-grid li{font-size:13px;color:var(--muted)}.flow{display:flex;align-items:stretch;gap:12px;margin:20px 0}.flow>div{flex:1;padding:16px;background:#edf5f3;border-radius:8px;font-size:12px}.flow b{display:block;margin:5px 0}.flow .step{font-size:10px;letter-spacing:1px;color:var(--primary)}details{margin-top:16px}summary{cursor:pointer;font-size:12px;font-weight:600}.empty{text-align:center;padding:32px 10px;color:var(--muted);font-size:13px}.empty small{display:block}.footnote{font-size:11px;color:var(--muted);border-top:1px solid var(--line);padding-top:16px;margin-top:25px}.health-note{font-size:12px;color:var(--muted)}.bad{color:var(--red)}.section-view{scroll-margin-top:15px}noscript{display:block;padding:20px;background:#fff4df}.no-wrap{white-space:nowrap}
@media(min-width:1500px){main{padding-top:38px}.card{padding:23px}.panel{padding:25px}}
@media(max-width:1100px){.sidebar{width:185px;padding:24px 12px}.workspace{margin-left:185px}.topbar{padding:0 22px}main{padding:25px 22px}.cards{gap:10px}.card{padding:14px}.metric{font-size:27px}.link-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:780px){.sidebar{position:static;width:100%;padding:15px 18px;display:block}.brand{font-size:19px}.side-note,.nav-label,.side-bottom{display:none}.sidebar nav{display:flex;flex-wrap:wrap;gap:4px;margin-top:12px}.sidebar nav a{font-size:12px;padding:7px 9px}.nav-icon{display:none}.workspace{margin-left:0}.topbar{height:auto;min-height:56px;padding:12px 18px;flex-wrap:wrap;gap:8px}main{padding:23px 17px 35px}.page-head{display:block}.page-head .toolbar{margin-top:15px}.cards{grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.card{padding:15px}.metric{font-size:29px}.grid,.guide-grid{grid-template-columns:1fr}.panel{padding:17px}.link-grid{grid-template-columns:1fr 1fr}.flow{display:grid;grid-template-columns:1fr 1fr}.panel-head{flex-wrap:wrap}.definition-list{gap:8px}.table-tools{flex-wrap:wrap}.section-head{align-items:flex-start;flex-wrap:wrap}.chart svg{height:185px}}
@media(max-width:390px){.link-grid,.flow{grid-template-columns:1fr}.card-label{display:block;min-height:40px}.pill{display:inline-block;margin-top:3px}.metric{font-size:25px}.live{font-size:11px}}
@media(prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;animation:none!important;transition:none!important}}
</style>
</head>
<body>
<a class="skip" href="#content">Skip to dashboard</a>
<aside class="sidebar"><a class="brand" href="/"><span class="mark" aria-hidden="true">F</span> FortisNet</a><p class="side-note">OPEN NODE · OBSERVABILITY</p><div class="nav-label">Explore this node</div>
<nav aria-label="Dashboard views"><a href="/dashboard#overview" data-view="overview"><span class="nav-icon" aria-hidden="true">▦</span>Overview</a><a href="/stats" data-view="traffic"><span class="nav-icon" aria-hidden="true">↗</span>API performance</a><a href="/dashboard#resources" data-view="resources"><span class="nav-icon" aria-hidden="true">▥</span>Resources &amp; storage</a><a href="/dashboard#guide" data-view="guide"><span class="nav-icon" aria-hidden="true">◎</span>Understanding the node</a></nav>
<div class="nav-label">Inspect &amp; verify</div><nav aria-label="Diagnostic pages"><a id="release-link" href="/releases">Versions &amp; deployments</a><a href="/errors">Error board</a><a href="/benchmark">Benchmark reports</a>)FDASH"),
{LandingSlot::kDocsLink, nullptr, 0},
FORTIS_TEXT(R"FDASH(</nav><div class="side-bottom">Open-source infrastructure.<br>Aggregate measurements, open methods.<p><a href="https://github.com/FortisNet/node" rel="noopener noreferrer">View source ↗</a></p></div></aside>
<div class="workspace"><header class="topbar"><div class="crumb">FortisNet / <b id="breadcrumb">Node overview</b></div><div class="live" id="live-state" data-state="loading" role="status"><span class="dot" aria-hidden="true"></span><span id="connection">Connecting to node…</span></div></header>
<main id="content">
<div class="page-head"><div><div class="eyebrow">Transparent by design</div><h1 id="page-title">Inside the node.</h1><p class="muted" id="page-subtitle">See the work this node is doing, the resources it uses, and how quickly it responds.</p></div><div class="toolbar"><span class="updated" id="updated">Waiting for measurements</span><button type="button" id="pause" aria-pressed="false">Pause updates</button></div></div>
<div id="fetch-notice" class="notice" role="status">Loading live aggregates. A blank measurement means it is not available, never zero.</div>
<noscript><style>section[data-section][hidden]{display:block!important}#fetch-notice,#pause,.live{display:none!important}</style>Live charts need JavaScript. Raw aggregate data is available at <a href="/statusz">/statusz</a>, <a href="/statsz">/statsz</a> and <a href="/status-history">/status-history</a>. The explanations and diagnostic links below remain readable.</noscript>
<section class="section-view" id="overview" data-section="overview">
<div class="cards"><article class="card"><div class="card-label">API requests <span class="pill">5 min</span></div><div class="metric" id="requests-now">—</div><p class="desc">Completed API responses, including refusals.</p></article><article class="card"><div class="card-label">Server error rate <span class="pill">5 min</span></div><div class="metric" id="error-rate">—</div><p class="desc" id="error-detail">5xx responses / all API responses.</p></article><article class="card"><div class="card-label">Process memory <span class="pill">Current</span></div><div class="metric" id="r-memory-now">—</div><p class="desc" id="memory-scope">Resident memory; not the host's total usage.</p></article><article class="card"><div class="card-label">CPU activity <span class="pill">Current</span></div><div class="metric" id="r-cpu-now">—</div><p class="desc" id="cpu-scope">100% means one logical CPU core is busy.</p></article></div>
<div class="section-head"><div><h2>Work over time</h2><span class="desc">One-minute aggregates · gaps mean no measurement</span></div><div class="range" id="history-periods" role="group" aria-label="Chart time range"><button data-minutes="15" aria-pressed="true">15 min</button><button data-minutes="60" aria-pressed="false">1 hour</button><button data-minutes="480" aria-pressed="false">8 hours</button></div></div>
<div class="grid"><article class="panel"><div class="panel-head"><div><h2>API traffic</h2><p class="desc">Responses completed in each minute.</p></div><div class="legend"><span><i class="swatch"></i>Requests</span><span><i class="swatch red"></i>Server errors</span></div></div><div class="chart" id="traffic-chart" data-chart="traffic"></div></article><article class="panel"><div class="panel-head"><div><h2>Response time</h2><p class="desc">Server-side time in milliseconds, per minute.</p></div><div class="legend"><span><i class="swatch blue"></i>Average</span><span><i class="swatch red"></i>Slowest</span></div></div><div class="chart" id="latency-chart" data-chart="latency"></div></article><article class="panel"><div class="panel-head"><div><h2>Memory footprint</h2><p class="desc">Average resident MiB, or tracked guest allocations.</p></div></div><div class="chart" id="r-memory-chart" data-chart="memory"></div></article><article class="panel"><div class="panel-head"><div><h2>CPU work</h2><p class="desc">Average busy time · 100% = one core on native Linux.</p></div></div><div class="chart" id="r-cpu-chart" data-chart="cpu"></div></article></div>
<article class="panel"><div class="panel-head"><div><h2>What the node is doing</h2><p class="desc">API groups · last 15 minute buckets · server-side response timings</p></div><a href="/stats" class="desc">Explore every route →</a></div><div class="scroll" tabindex="0" aria-label="API group performance"><table><thead><tr><th scope="col">API group</th><th scope="col">Responses</th><th scope="col">Average</th><th scope="col">95% within*</th><th scope="col">5xx errors</th><th scope="col">4xx refusals</th></tr></thead><tbody id="overview-groups"></tbody></table></div><p class="desc">*Histogram estimate, not an exact percentile. No traffic means no latency measurement. A quick response can still be an error or contain incomplete upstream data.</p></article>
<div class="grid"><article class="panel"><h2>Data storage</h2><p class="desc" id="disk-summary">Waiting for a filesystem sample.</p><div class="storage-number" id="disk-now">—</div><div class="storage-meter" role="img" aria-label="Filesystem usage" id="disk-meter"><div></div></div><p class="desc" id="disk-free">Capacity is shown only when the operating system reports it.</p><div class="explain">Disk usage describes the data filesystem, which may be shared with other files. The database file shown in Resources is only one part of it. Used disk space is not disk speed.</div><p><a href="/dashboard#resources">Inspect resources and database →</a></p></article><article class="panel"><h2>Serving wallets, keeping work local</h2><p class="muted">This node gathers blockchain and market information, reuses cached results and serves wallet apps. When outside providers are slow, response times and data freshness can change.</p><dl class="definition-list"><dt>Runtime</dt><dd id="runtime">—</dd><dt>Time since process start</dt><dd id="uptime">—</dd><dt>Identified devices in 5 min</dt><dd id="identified-devices">—</dd><dt>Price publication</dt><dd id="publication">Not reported</dd><dt>Published snapshot age</dt><dd id="publication-age">—</dd></dl><p class="desc">Device counts are bounded and exclude unattributed requests; they do not count people. Connected means this dashboard can read the node. It does not certify every upstream provider or every asset is available.</p></article></div>
</section>
<section class="section-view" id="traffic" data-section="traffic" hidden>
<article class="panel"><h2>Node request statistics</h2><p class="muted">Every completed response tells part of the story: what was requested, how long it took, and whether the node refused it or encountered a server error.</p><div class="explain">All tables below use the same 15 one-minute buckets, including the current partial minute. Group percentiles merge the original latency histograms; they are not averages of route percentiles. Polling this dashboard does not inflate API request counts.</div></article>
<article class="panel"><div class="panel-head"><div><h2>API group performance</h2><p class="desc">A shared view of each type of work. Groups with no samples remain visible.</p></div></div><div class="chart" id="group-chart"></div><div class="scroll" tabindex="0" aria-label="Detailed group performance"><table><thead><tr><th scope="col">API group</th><th scope="col">Responses</th><th scope="col">Avg ms</th><th scope="col">p50*</th><th scope="col">p95*</th><th scope="col">p99*</th><th scope="col">Slowest</th><th scope="col">5xx</th><th scope="col">4xx</th><th scope="col">429</th><th scope="col">Avg queue</th></tr></thead><tbody id="detail-groups"></tbody></table></div></article>
<article class="panel"><h2>Individual routes</h2><p class="desc">Names are templates such as <code>:network</code>, never a visitor's wallet address or complete URL.</p><div class="table-tools"><label>Filter routes <input id="route-filter" type="search" placeholder="Search API group, method or route" autocomplete="off"></label><label>Sort by <select id="route-sort"><option value="requests">Most requests</option><option value="p95Ms">Highest p95</option><option value="errors5xx">Most server errors</option><option value="maxMs">Slowest response</option></select></label></div><div class="scroll" tabindex="0" aria-label="Individual API route performance"><table><thead><tr><th scope="col">Method / route</th><th scope="col">Group</th><th scope="col">Responses</th><th scope="col">Avg ms</th><th scope="col">p50*</th><th scope="col">p95*</th><th scope="col">p99*</th><th scope="col">Slowest</th><th scope="col">5xx</th><th scope="col">4xx</th><th scope="col">429</th><th scope="col">Avg queue</th><th scope="col">Max queue</th><th scope="col">Owner samples</th><th scope="col">Avg owner</th><th scope="col">Max owner</th></tr></thead><tbody id="detail-routes"></tbody></table></div><div class="toolbar"><button id="routes-prev" type="button" disabled>Previous 50</button><button id="routes-next" type="button" disabled>Next 50</button></div><p class="desc" id="route-count">Waiting for request statistics.</p><p class="desc">Queue measurements apply where the HTTP admission path records them. Spot-owner time covers price planning and provider waits; it is only shown when owner samples exist. These timings overlap and must not be added together.</p></article>
<div class="grid"><article class="panel"><h2>Devices by operating system</h2><p class="desc">Response counts, not a census of users. Platform is known only where enrolled-device metadata was recorded.</p><div id="platforms"></div></article><article class="panel"><h2>Capacity events</h2><p class="desc">Lifetime counters since this process started, not the 15-minute traffic window. One request may contribute to more than one diagnostic counter.</p><div id="admission"></div><p class="desc">A refusal can protect the node from overload. See the <a href="/errors">error board</a> for 5xx outcomes and operational incident summaries.</p></article></div>
<p class="desc">*p50 is the middle response, p95 covers about 95 in 100 responses, and p99 about 99 in 100. Values are histogram estimates. See <a href="/dashboard#guide">the measurement guide</a>.</p>
</section>
<section class="section-view" id="resources" data-section="resources" hidden>
<div class="grid"><article class="panel"><h2>Process &amp; workload</h2><p class="desc">Latest OS sample: <span id="resource-age">not yet available</span></p><dl class="definition-list" id="process-details"></dl><div class="explain">Resident memory is RAM currently mapped into this process. Virtual memory is reserved address space, not actual RAM consumption. A cgroup is an OS resource-sharing group; its figures may include file cache and other processes.</div></article><article class="panel"><h2>Filesystem &amp; database</h2><p class="desc">Filesystem sample: <span id="disk-age">not yet available</span></p><dl class="definition-list" id="storage-details"></dl><div class="explain">The node periodically compacts its database: it copies useful records and removes obsolete versions. This reclaims space; a brief increase during compaction does not automatically mean a leak. Cached database counters have their own sample age.</div></article><article class="panel"><h2>Stored data over time</h2><p class="desc">Used bytes on the data filesystem, or allocated bytes in the data tree when capacity is unavailable. Samples every 30 seconds, grouped by minute. Only samples matching the current storage scope are plotted.</p><div class="range" role="group" aria-label="Storage chart time range"><button data-minutes="15" aria-pressed="true">15 min</button><button data-minutes="60" aria-pressed="false">1 hour</button><button data-minutes="480" aria-pressed="false">8 hours</button></div><div class="chart" id="disk-history-chart" data-chart="disk"></div><p class="desc">Chart axes scale to the observed range. A larger database is not necessarily less efficient. Compare its live bytes, file size, compaction activity and traffic together.</p></article><article class="panel"><h2>Storage I/O &amp; connections</h2><p class="desc">Disk I/O counters are cumulative process-accounted storage bytes, not network traffic or disk bandwidth. Cached reads may add no physical read bytes.</p><dl class="definition-list" id="io-details"></dl><div class="explain">Threads are workers available to run tasks. Database lock waiters are tasks waiting for shared access; lock hold time measures how long an operation keeps that access exclusive. Open descriptors are handles to files, sockets and other OS objects. Their counts alone do not indicate overload. Guest socket slots are shown only where that runtime exports them.</div></article></div>
<article class="panel"><h2>Freshness and cache reuse</h2><dl class="definition-list" id="cache-details"></dl><p class="desc">Cache hits mean previously prepared information was reused. A hit count is not a hit rate without a matching request denominator. A published snapshot can be old or incomplete; HTTP success alone does not prove asset coverage.</p><p><a href="/tokens">Browse token freshness</a> · <a href="/cache/inventory">Inspect aggregate cache inventory</a> · <a href="/benchmark">Inspect live storage timing buckets</a></p></article>
</section>
<section class="section-view" id="guide" data-section="guide" hidden>
<article class="panel"><h2>What does a node actually do?</h2><p class="muted">Think of the node as a shared research desk for wallet apps. It looks up information, organizes it, remembers reusable results and sends a response. It does not need your wallet seed or private key to do this work.</p><div class="flow"><div><span class="step">01 · REQUEST</span><b>A wallet asks a question</b>For example: what is this asset worth, or what changed on this address?</div><div><span class="step">02 · CHECK &amp; REUSE</span><b>The node validates access</b>It checks authorization and limits, and reuses suitable cached information.</div><div><span class="step">03 · LOOK UP</span><b>Providers fill the gaps</b>Blockchain and market services supply information that is not already available locally.</div><div><span class="step">04 · RESPOND &amp; SAVE</span><b>The result goes back</b>Responses finish at different times. AI turns are saved first with a durable 201 acknowledgement; generation finishes asynchronously.</div></div></article>
<div class="guide-grid"><article><h2>How fast is fast?</h2><p>A millisecond (ms) is one thousandth of a second: 100 ms is a tenth of a second; 1,000 ms is one second. Average time describes the typical workload but can hide a few very slow replies.</p><p>p95 means roughly 95 out of 100 measured responses fell within that time. This node stores timing buckets, so percentiles are estimates of bucket boundaries, not exact ranked measurements. The final bucket uses the observed maximum.</p><p>These are server-side completion timings, not the full phone experience: network travel, TCP/TLS setup and client rendering are not all included. Benchmark reports may measure a different boundary.</p></article><article><h2>What makes a node efficient?</h2><p>Charts automatically scale their axes to the selected data range; a tall line does not by itself mean a resource is full. Look for useful responses served quickly, without rising errors, long queues or resource exhaustion. Low CPU on an idle node does not demonstrate capacity; it can also mean requests are waiting on disk or a provider. High CPU under useful load is not automatically a problem.</p><p>Compare equal workloads and time windows. The dashboard does not invent an efficiency score, energy usage, guaranteed requests per second or a global availability percentage.</p><p>To investigate slow requests, compare route latency, queue time and storage timings with CPU and memory trends. Do not add overlapping timing phases together.</p></article><article><h2>Errors and refusals are different</h2><p>4xx responses usually mean a request was not accepted: missing authorization (401), an invalid request (400), or a rate limit (429). They are not counted as server failures here.</p><p>5xx means the server could not complete the request as expected. This can include an unavailable outside provider or a capacity limit. A response with no 5xx error can still contain partial or stale data.</p><p>The error board counts failures over a time window and shows persisted operational incidents. Its threshold is a count, not an uptime guarantee or a severity verdict for each visitor.</p></article><article><h2>CPU, RAM and disk in everyday language</h2><p>CPU is the work being done now. On native Linux, 100% means one logical core; a multithreaded process can exceed 100%. In the guest it is scheduler busy time for the configured guest CPU. The current CPU card compares successive status reads, while the chart averages background samples within each minute; their values need not match.</p><p>RAM is the desk space used while working. A MiB is 1,048,576 bytes, about a million; a GiB is 1,024 MiB, about a billion bytes. Native resident memory includes more than heap allocations. A guest may instead report only tracked allocator bytes, which must not be compared as if it were RSS.</p><p>Disk is the filing cabinet. Used filesystem space can include other files. The database file is only part of that cabinet. No physical HDD/SSD type, drive temperature, wear or energy measurement is inferred.</p></article><article><h2>Where do these numbers come from?</h2><p>Request counters and latency histograms are recorded when API responses complete. Group histograms are merged on the server. The 15-minute tables use 15 one-minute buckets, including a partial current minute.</p><p>Charts refresh about every 30 seconds; status and traffic tables refresh about every ten seconds. The background sampler keeps up to eight hours of one-minute resource and traffic history in memory. OS counters are read about every five seconds; the filesystem is sampled about every 30 seconds. History is lost on restart. Incidents may be persisted separately.</p><p>Linux resource details come from procfs and the process's unified cgroup when accessible. A reported local cgroup quota may be further constrained by an ancestor. Missing files, unsupported runtimes and unbounded quotas remain unavailable, never fabricated zeros.</p></article><article><h2>What is counted, and what stays private?</h2><p>API traffic covers tracked <code>/v1/</code> responses. Dashboard, health, documentation and root-scoped media reads do not inflate those numbers. Platform attribution can be Unknown; missing labels do not mean the request did not happen.</p><p>Statistics publish aggregate counts and registered route templates. They do not publish device identifiers, tokens, wallet addresses, account balances, conversations, provider credentials or request bodies.</p><p>Tracking is bounded. If route samples are omitted, a visible coverage warning appears; per-route and group totals may then undercount. Distinct-client counts are bounded estimates/lower bounds, not a count of human beings.</p></article></div>
</section>
<section class="panel"><div class="panel-head"><div><h2>Explore the evidence</h2><p class="desc">Each page answers a different question.</p></div></div><div class="link-grid"><a class="link-card" href="/errors"><strong>Error board</strong><span>Which responses failed, and which operational incidents were recorded?</span></a><a class="link-card" href="/benchmark"><strong>Benchmark reports</strong><span>Historical workload tests and live timing buckets. Published runs are not current capacity guarantees.</span></a><a class="link-card" href="/networks"><strong>Supported networks</strong><span>)FDASH"),
{LandingSlot::kNetworks, nullptr, 0},
FORTIS_TEXT(R"FDASH( networks · )FDASH"),
{LandingSlot::kFamilies, nullptr, 0},
FORTIS_TEXT(R"FDASH( chain families. Support is not a live provider-health test.</span></a><a class="link-card" href="/tokens"><strong>Token freshness</strong><span>How recently cached token facts were refreshed.</span></a><a class="link-card" href="/ai"><strong>AI model inventory</strong><span>Configured models and safe public settings, not conversation contents.</span></a><a class="link-card" href="/cache/inventory"><strong>Cache inventory</strong><span>Inspect stored categories; private categories expose metadata only.</span></a></div><details><summary>Capability counts and raw aggregate feeds</summary><p class="desc">Configured network coverage, derived from the same registry as the network API. This is not a promise that every provider is available right now.</p><p class="desc">Balances )FDASH"),
{LandingSlot::kBalances, nullptr, 0},
FORTIS_TEXT(R"FDASH( · Transfers )FDASH"),
{LandingSlot::kTransfers, nullptr, 0},
FORTIS_TEXT(R"FDASH( · NFTs )FDASH"),
{LandingSlot::kNfts, nullptr, 0},
FORTIS_TEXT(R"FDASH( · Address insight )FDASH"),
{LandingSlot::kInsight, nullptr, 0},
FORTIS_TEXT(R"FDASH( · Transaction status )FDASH"),
{LandingSlot::kTxStatus, nullptr, 0},
FORTIS_TEXT(R"FDASH( · Token data )FDASH"),
{LandingSlot::kToken, nullptr, 0},
FORTIS_TEXT(R"FDASH( · Trustlines )FDASH"),
{LandingSlot::kTrustlines, nullptr, 0},
FORTIS_TEXT(R"FDASH( · Transaction detail )FDASH"),
{LandingSlot::kTxDetail, nullptr, 0},
FORTIS_TEXT(R"FDASH(</p><p>)FDASH"),
{LandingSlot::kDocsLink, nullptr, 0},
FORTIS_TEXT(R"FDASH( · <a href="/statusz">Current status JSON</a> · <a href="/statsz">API statistics JSON</a> · <a href="/status-history">History JSON</a></p></details></section>
<footer class="footnote">Public observability, without private user data. Measurements are process-local unless their label says filesystem or cgroup. <span id="version"></span></footer>
</main></div>
<script>
'use strict';
(function () {
  const $ = id => document.getElementById(id);
  const state = {status:null, stats:null, history:null, failures:[], paused:false, running:false, timer:null, range:15, page:0, historyAt:0};
  const numeric = x => typeof x === 'number' && Number.isFinite(x) && x >= 0;
  const nf = new Intl.NumberFormat(undefined, {maximumFractionDigits:2});
  const count = x => numeric(x) ? nf.format(x) : 'Not reported';
  const ms = x => numeric(x) ? count(x) + ' ms' : '—';
  function bytes(x) {
    if (!numeric(x)) return 'Not reported';
    const units = ['B','KiB','MiB','GiB','TiB','PiB'];
    let index = 0;
    while (x >= 1024 && index < units.length - 1) { x /= 1024; index++; }
    return count(x) + ' ' + units[index];
  }
  function age(timestamp) {
    if (!numeric(timestamp) || !timestamp) return 'not yet reported';
    const seconds = (Date.now() - timestamp) / 1000;
    if (seconds < -5) return 'timestamp is ahead of this browser';
    return Math.max(0, Math.floor(seconds)) + ' seconds ago';
  }
  function duration(seconds) {
    if (!numeric(seconds)) return 'Not reported';
    const days = Math.floor(seconds / 86400), hours = Math.floor(seconds / 3600) % 24, minutes = Math.floor(seconds / 60) % 60;
    return (days ? days + 'd ' : '') + hours + 'h ' + minutes + 'm';
  }
  function cell(row, value, code) {
    const td = document.createElement('td');
    if (code) { const c = document.createElement('code'); c.textContent = value; td.append(c); }
    else td.textContent = value;
    row.append(td);
    return td;
  }
  function emptyRow(target, columns, message) {
    const row = document.createElement('tr'), td = cell(row, message);
    td.colSpan = columns; td.className = 'empty'; target.replaceChildren(row);
  }
  function definitions(id, entries) {
    const fragment = document.createDocumentFragment();
    for (const [label, value] of entries) {
      const dt = document.createElement('dt'), dd = document.createElement('dd');
      dt.textContent = label; dd.textContent = value; fragment.append(dt, dd);
    }
    $(id).replaceChildren(fragment);
  }
  const views = {overview:['Node overview','Inside the node.','See the work this node is doing, the resources it uses, and how quickly it responds.'], traffic:['API performance','Every request tells a story.','Explore responses, delays and errors by API group, route and device platform.'], resources:['Resources & storage','The resources behind the work.','Understand process memory, CPU activity, storage and cache reuse without confusing their scopes.'], guide:['Measurement guide','Make sense of the numbers.','A practical guide to what the node does, how it is measured, and what these statistics can and cannot prove.']};
  let lastReleaseCheck = -Infinity;
  function refreshReleaseIdentity() {
    if (Date.now() - lastReleaseCheck < 10000) return;
    lastReleaseCheck = Date.now();
    fetch('/releasesz', {cache:'no-store'}).then(response => response.ok ? response.json() : null).then(data => {
    const current = data && data.managed === true ? data.current : null;
    if (current && /^[a-f0-9]{64}$/.test(current.sha256)) {
      const link = document.getElementById('release-link');
      link.textContent = 'Release ' + current.sha256.slice(0, 10) + '…';
      link.title = 'Controller-observed executable SHA-256: ' + current.sha256;
    }
  }).catch(() => {});
  }
  function currentView() { const hash = location.hash.slice(1); return views[hash] ? hash : location.pathname === '/stats' ? 'traffic' : 'overview'; }
  function showView() {
    const view = currentView(), [crumb, title, subtitle] = views[view];
    $('breadcrumb').textContent = crumb; $('page-title').textContent = title; $('page-subtitle').textContent = subtitle;
    document.title = crumb + ' · FortisNet';
    document.querySelectorAll('[data-section]').forEach(section => { section.hidden = section.dataset.section !== view; });
    document.querySelectorAll('[data-view]').forEach(link => { if (link.dataset.view === view) link.setAttribute('aria-current','page'); else link.removeAttribute('aria-current'); });
    renderCharts();
  }
  document.querySelectorAll('[data-view], a[href="/dashboard#resources"], a[href="/dashboard#guide"], a[href="/stats"]').forEach(link => link.addEventListener('click', event => {
    if (event.button || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
    const url = new URL(link.href);
    if (url.origin !== location.origin) return;
    event.preventDefault(); history.pushState(null,'',url.pathname + url.hash); showView(); window.scrollTo({top:0,behavior:'instant'});
  }));
  addEventListener('hashchange', showView); addEventListener('popstate', showView);
  function renderStatus() {
    const data = state.status;
    if (!data) return;
    refreshReleaseIdentity();
    const totals = data.stats?.window5m;
    $('requests-now').textContent = count(totals?.requests);
    $('error-rate').textContent = totals?.requests > 0 && numeric(totals.errors) ? count(100 * totals.errors / totals.requests) + '%' : 'No traffic';
    $('error-detail').textContent = totals?.requests > 0 ? count(totals.errors) + ' server errors in ' + count(totals.requests) + ' responses.' : 'No responses to calculate an error rate.';
    const rss = data.memory?.rss, heap = data.memory?.heapUsed;
    $('r-memory-now').textContent = bytes(numeric(rss) ? rss : heap);
    $('memory-scope').textContent = numeric(rss) ? 'Resident process memory; not whole-host usage.' : numeric(heap) ? 'Tracked guest allocations; not RSS or all guest memory.' : 'This runtime does not expose process memory.';
    $('r-cpu-now').textContent = numeric(data.cpu?.percent) ? count(data.cpu.percent) + '%' : 'Not reported';
    const guest = String(data.node || '').includes('unikraft');
    $('cpu-scope').textContent = guest ? 'Guest scheduler busy time, not host/QEMU CPU.' : '100% = one logical CPU core. Can exceed 100%.';
    $('runtime').textContent = typeof data.node === 'string' ? data.node : 'Not reported';
    $('uptime').textContent = duration(data.uptimeSec);
    $('identified-devices').textContent = count(totals?.clients);
    $('version').textContent = typeof data.version === 'string' ? 'Node version ' + data.version + '.' : '';
    const resource = data.resources || {}, disk = data.disk, db = data.database || {}, io = data.databaseActivity || {};
    const scope = resource.diskScope || (disk?.total > 0 ? 'filesystem' : disk ? 'data-tree' : 'unavailable');
    const capacity = scope === 'filesystem' && numeric(disk?.total) && disk.total > 0;
    $('disk-now').textContent = disk && numeric(disk.used) ? bytes(disk.used) + (capacity ? ' / ' + bytes(disk.total) : ' allocated') : 'Not reported';
    $('disk-summary').textContent = scope === 'filesystem' ? 'Used capacity on the filesystem containing node data.' : scope === 'data-tree' ? 'Allocated bytes in the bounded data-tree scan; total capacity unavailable.' : 'Filesystem data is unavailable on this runtime.';
    const percent = capacity && numeric(disk.used) ? disk.used / disk.total * 100 : null;
    $('disk-meter').hidden = !numeric(percent);
    $('disk-meter').setAttribute('aria-label', numeric(percent) ? count(percent) + '% of filesystem capacity used' : 'Filesystem capacity unavailable');
    $('disk-meter').firstElementChild.style.width = numeric(percent) ? Math.min(100,percent) + '%' : '0%';
    $('disk-free').textContent = numeric(resource.diskAvailableBytes) ? bytes(resource.diskAvailableBytes) + ' available to this process (reserved blocks excluded).' : 'Available writable space is not reported; no capacity percentage is inferred.';
    $('resource-age').textContent = age(resource.sampledAtMs); $('disk-age').textContent = age(resource.diskSampledAtMs);
    definitions('process-details', [['Resident process memory',bytes(rss)],['Tracked guest allocations',bytes(heap)],['Peak resident memory',bytes(resource.peakRssBytes)],['Virtual address space',bytes(resource.virtualBytes)],['Threads',count(resource.threads)],['Cgroup memory (may include cache)',bytes(resource.cgroupMemoryBytes)],['Reported local cgroup memory limit',bytes(resource.cgroupMemoryLimitBytes)],['Reported local CPU quota',numeric(resource.cpuQuotaCores) ? count(resource.cpuQuotaCores) + ' cores' : 'No quota reported / unavailable']]);
    definitions('storage-details', [['Measurement scope',scope === 'filesystem' ? 'Data filesystem' : scope === 'data-tree' ? 'Allocated data-tree bytes' : 'Not reported'],['Allocated / used storage',bytes(disk?.used)],['Filesystem capacity',capacity ? bytes(disk.total) : 'Not reported'],['Available writable space',bytes(resource.diskAvailableBytes)],['Database file size',bytes(db.fileBytes)],['Estimated live database bytes',bytes(db.liveBytes)],['Indexed database keys',count(db.indexedKeys)],['Compaction state',typeof db.compactionPhase === 'string' ? db.compactionPhase : 'Not reported'],['Compactions completed / failed',count(db.compactionsCompleted) + ' / ' + count(db.compactionsFailed)],['Database snapshot age',numeric(db.sampleAgeSec) ? count(db.sampleAgeSec) + ' seconds' : 'Not reported'],['Recovery pending',typeof db.recoveryPending === 'boolean' ? (db.recoveryPending ? 'Yes' : 'No') : 'Not reported']]);
    definitions('io-details', [['Process storage reads since start',bytes(resource.readBytesSinceBoot)],['Process storage writes since start',bytes(resource.writtenBytesSinceBoot)],['Open process descriptors',count(resource.openDescriptors)],['Guest socket slots in use',count(data.sockets?.used)],['Guest socket slot limit',count(data.sockets?.limit)],['Peak sampled guest socket slots',count(data.sockets?.sampledPeak)],['Database lock waiters now',count(io.lockWaiters)],['Peak database lock waiters',count(io.peakLockWaiters)],['Main database write frames',count(io.writeFrames)],['Maximum recorded compaction lock hold',numeric(db.maxLockHoldUs) ? ms(db.maxLockHoldUs / 1000) : 'Not reported']]);
    const publication = data.priceSnapshot;
    $('publication').textContent = publication ? (publication.ready ? 'Published snapshot ready' : 'Snapshot not ready') : 'Not reported';
    $('publication-age').textContent = ms(publication?.ageMs);
    definitions('cache-details', [['Published price snapshot',publication ? (publication.ready ? 'Ready' : 'Not ready') : 'Not reported'],['Snapshot age',ms(publication?.ageMs)],['Snapshot generation',count(publication?.sequence)],['Unavailable snapshot reads since start',count(publication?.unavailable)],['Price memory-cache hits since start',count(data.priceStore?.memoryHits)],['Contract-price memory-cache hits',count(data.priceStore?.contractMemoryHits)],['Snapshot authentication cache hits',count(data.snapshotAuth?.hits)],['Snapshot authentication cache misses',count(data.snapshotAuth?.misses)],['Snapshot authentication invalidations',count(data.snapshotAuth?.invalidations)]]);
  }
  const detailColumns = ['requests','avgMs','p50Ms','p95Ms','p99Ms','maxMs','errors5xx','errors4xx','rateLimited','avgQueueMs'];
  function value(row, key) { return ['requests','errors5xx','errors4xx','rateLimited','ownerSamples'].includes(key) ? count(row[key]) : row.requests > 0 && numeric(row[key]) ? ms(row[key]) : '—'; }
  const groupMeaning={'Prices':'Current and historical asset values.','Token Discovery':'Search for assets and market listings.','Token 360':'Facts and history about an individual asset.','Address Data':'Blockchain balances, transfers and address activity.','Devices':'Device enrollment and access management.','Exchange':'Exchange routes, instruments and quotes.','Bittensor':'Network and subnet analytics.','Crash Reports':'Application diagnostic reporting.','Sync':'Synchronize wallet-app data.','Networks':'Describe supported blockchains.','AI':'Save conversation turns and track generated replies.','News':'Blockchain and market news.','API':'Other tracked API operations.'};
  function groupCell(row,group) { const td=cell(row,'');const name=document.createElement('strong');name.textContent=String(group.service);const explanation=document.createElement('small');explanation.className='desc';explanation.style.display='block';explanation.style.minWidth='145px';explanation.textContent=groupMeaning[group.service] || 'Tracked API responses.';td.append(name,explanation); }
  function renderGroups() {
    const groups = Array.isArray(state.stats?.groups) ? [...state.stats.groups].sort((a,b) => b.requests-a.requests || String(a.service).localeCompare(String(b.service))) : [];
    $('overview-groups').replaceChildren(); $('detail-groups').replaceChildren();
    for (const group of groups) {
      const row = document.createElement('tr'); groupCell(row,group); detailColumns.forEach(key => cell(row,value(group,key))); $('detail-groups').append(row);
    }
    for (const group of groups.filter(g => g.requests > 0).slice(0,8)) {
      const row = document.createElement('tr'); groupCell(row,group); [count(group.requests),value(group,'avgMs'),value(group,'p95Ms'),count(group.errors5xx),count(group.errors4xx)].forEach(v => cell(row,v)); $('overview-groups').append(row);
    }
    if (!$('overview-groups').children.length) emptyRow($('overview-groups'),6,groups.length ? 'No completed API responses in this window.' : 'Group statistics are not reported by this node version.');
    if (!groups.length) emptyRow($('detail-groups'),11,'Group statistics are not available. Individual route data may still be available below.');
    const container = $('group-chart'); container.replaceChildren();
    const active = groups.filter(g => g.requests > 0).slice(0,7), total = groups.reduce((sum,g) => sum + (numeric(g.requests) ? g.requests : 0),0);
    if (!active.length) { container.className = 'empty'; container.textContent = 'Waiting for API traffic. No sample bars are invented.'; return; }
    container.className = '';
    for (const group of active) {
      const line = document.createElement('div'); line.style.marginBottom = '10px';
      const label = document.createElement('div'); label.className='desc'; label.textContent=String(group.service) + ' · ' + count(group.requests) + ' responses · ' + count(100*group.requests/total) + '% of recorded group traffic';
      const track=document.createElement('div'), bar=document.createElement('div'); track.className='bar-track'; bar.className='bar-fill'; bar.style.width=(100*group.requests/total)+'%'; track.append(bar); line.append(label,track); container.append(line);
    }
  }
  const routeColumns = [...detailColumns,'maxQueueMs','ownerSamples','avgOwnerMs','maxOwnerMs'];
  function renderRoutes() {
    const filter = $('route-filter').value.toLowerCase(), sort = $('route-sort').value;
    const rows = (Array.isArray(state.stats?.routes) ? state.stats.routes : []).filter(r => [r.method,r.route,r.service].join(' ').toLowerCase().includes(filter)).sort((a,b) => (numeric(b[sort]) ? b[sort] : 0) - (numeric(a[sort]) ? a[sort] : 0) || String(a.route).localeCompare(String(b.route)));
    const limit=50; state.page=Math.min(state.page,Math.max(0,Math.ceil(rows.length/limit)-1));
    const target=$('detail-routes'); target.replaceChildren();
    for (const entry of rows.slice(state.page*limit,(state.page+1)*limit)) {
      const row=document.createElement('tr'); cell(row,String(entry.method)+' '+String(entry.route),true); cell(row,String(entry.service || 'Not reported'));
      for (const key of routeColumns) cell(row,(key==='avgOwnerMs' || key==='maxOwnerMs') && !entry.ownerSamples ? '—' : value(entry,key));
      target.append(row);
    }
    if (!rows.length) emptyRow(target,16,filter ? 'No routes match this filter.' : 'No route responses recorded in this window.');
    $('route-count').textContent = rows.length ? 'Showing '+(state.page*limit+1)+'–'+Math.min((state.page+1)*limit,rows.length)+' of '+rows.length+' matching routes. Percentiles include errors and refusals.' : 'No matching route samples.';
    $('routes-prev').disabled=state.page===0; $('routes-next').disabled=(state.page+1)*limit>=rows.length;
  }
  const admissionLabels={connectionQueue:'Connection queue full',spotSlots:'Price request slots full',responseSlots:'Response slots full',spotOwner:'Price owner declined work',snapshotUnavailable:'Price snapshot unavailable',acceptResourceExhaustion:'Socket resources exhausted',snapshotAuthorityBusy:'Snapshot authority busy',connectionQueueExpired:'Waiting connection expired'};
  function renderStats() {
    if (!state.stats) return;
    renderGroups(); renderRoutes();
    const platforms=Array.isArray(state.stats.platforms) ? state.stats.platforms : [];
    const platformList=document.createElement('dl'); platformList.className='definition-list';
    for (const entry of platforms) { const dt=document.createElement('dt'),dd=document.createElement('dd'); dt.textContent=entry.os==='unknown' ? 'Unknown / not attributed' : String(entry.os); dd.textContent=count(entry.requests)+' responses'; platformList.append(dt,dd); }
    $('platforms').replaceChildren(platformList);
    const list=document.createElement('dl'); list.className='definition-list';
    for (const [key,n] of Object.entries(state.stats.admissionSinceBoot || {})) { const dt=document.createElement('dt'),dd=document.createElement('dd'); dt.textContent=admissionLabels[key] || key; dd.textContent=count(n); list.append(dt,dd); }
    $('admission').replaceChildren(list);
  }
  $('route-filter').addEventListener('input',()=>{state.page=0;renderRoutes();}); $('route-sort').addEventListener('change',()=>{state.page=0;renderRoutes();});
  $('routes-prev').addEventListener('click',()=>{state.page--;renderRoutes();}); $('routes-next').addEventListener('click',()=>{state.page++;renderRoutes();});
  function svgNode(name,attrs,text) { const e=document.createElementNS('http://www.w3.org/2000/svg',name); for (const [key,v] of Object.entries(attrs || {})) e.setAttribute(key,String(v)); if (text!==undefined) e.textContent=text; return e; }
  const chartSpec={traffic:{title:'API responses per minute',unit:'responses',series:[s=>s.requests?.total,s=>s.requests?.errors],colors:['#007f69','#be3d49']},latency:{title:'Server response times per minute',unit:'ms',series:[s=>s.requests?.total>0 ? s.requests.avgDurationMs : null,s=>s.requests?.total>0 ? s.requests.maxDurationMs : null],colors:['#376dc9','#be3d49']},memory:{title:'Memory footprint',unit:'MiB',series:[s=>s.memoryMb?.avg],colors:['#007f69']},cpu:{title:'CPU busy time',unit:'%',series:[s=>s.cpuPercent?.avg],colors:['#376dc9']},disk:{title:'Used storage',unit:'MiB',series:[s=>s.diskScope === state.status?.resources?.diskScope ? s.diskUsedMb?.avg : null],colors:['#007f69']}};
  function drawChart(container) {
    if (container.closest('[hidden]')) return;
    const spec=chartSpec[container.dataset.chart], history=state.history?.slots;
    const slots=Array.isArray(history) ? history.slice(-state.range) : [];
    const observed=s=>typeof s.observed==='boolean' ? s.observed : Boolean(s.memoryMb || s.cpuPercent || s.requests?.total);
    const series=spec.series.map(fn=>slots.map(s=>observed(s)&&numeric(fn(s)) ? fn(s) : null));
    const values=series.flat().filter(numeric), width=Math.max(240,container.clientWidth), height=185, left=50, right=14, top=14, bottom=30;
    container.replaceChildren(); container.tabIndex=0; container.setAttribute('role','img'); container.setAttribute('aria-label',spec.title+'. Use left and right arrow keys to inspect minute samples.');
    if (!values.length) { const message=document.createElement('div'); message.className='empty'; message.textContent='No measured samples in this range.'; const detail=document.createElement('small'); detail.textContent='History starts with this process. Missing values are not plotted as zero.'; message.append(detail); container.append(message); return; }
    const largest=Math.max(1,...values), max=container.dataset.chart==='traffic' ? Math.max(2,Math.ceil(largest/2)*2) : largest, x=i=>left+(width-left-right)*(slots.length<2 ? .5 : i/(slots.length-1)), y=n=>top+(height-top-bottom)*(1-n/max);
    const svg=svgNode('svg',{viewBox:'0 0 '+width+' '+height,'aria-hidden':'true'});
    for (let tick=0;tick<=2;tick++) { const value=max*tick/2, yy=y(value); svg.append(svgNode('line',{x1:left,x2:width-right,y1:yy,y2:yy,stroke:'#e5ecef','stroke-dasharray':tick?'3 4':'0'})); svg.append(svgNode('text',{x:left-8,y:yy+4,'text-anchor':'end',fill:'#607580','font-size':10},new Intl.NumberFormat(undefined,{notation:'compact',maximumFractionDigits:1}).format(value)+(spec.unit==='%'?'%':''))); }
    series.map((points,index)=>({points,index})).reverse().forEach(({points,index})=>{
      let segment=[];
      function finish() { if (!segment.length) return; if (segment.length>1) svg.append(svgNode('polyline',{points:segment.join(' '),fill:'none',stroke:spec.colors[index],'stroke-width':2,'stroke-linejoin':'round'})); segment=[]; }
      points.forEach((n,i)=>{if(n===null){finish();return;}segment.push(x(i)+','+y(n));}); finish();
      const last=points.findLastIndex(n=>n!==null); if(last>=0) svg.append(svgNode('circle',{cx:x(last),cy:y(points[last]),r:3,fill:spec.colors[index]}));
    });
    const time=s=>new Date(s.created*1000).toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit'});
    svg.append(svgNode('text',{x:left,y:height-5,fill:'#607580','font-size':10},slots[0]?time(slots[0]):'')); svg.append(svgNode('text',{x:width-right,y:height-5,fill:'#607580','font-size':10,'text-anchor':'end'},slots.length?time(slots[slots.length-1]):''));
    const note=document.createElement('p'); note.className='chart-note'; const first=series[0], latest=first.findLastIndex(n=>n!==null);
    note.textContent=first.filter(n=>n!==null).length+' measured minute slots · '+(latest>=0?'Latest '+count(first[latest])+' '+spec.unit+' at '+time(slots[latest]):'No primary samples');
    const tip=document.createElement('div');tip.className='chart-tip';tip.hidden=true;container.append(svg,tip,note);
    let selected=slots.length-1;
    const inspect=i=>{selected=Math.max(0,Math.min(slots.length-1,i));tip.textContent=time(slots[selected])+' · '+series.map(p=>p[selected]===null?'No measurement':count(p[selected])+' '+spec.unit).join(' / ');tip.hidden=false;};
    container.onpointermove=e=>inspect(Math.round((e.clientX-container.getBoundingClientRect().left-left)/(width-left-right)*(slots.length-1)));
    container.onpointerleave=()=>{tip.hidden=true;}; container.onblur=()=>{tip.hidden=true;};
    container.onkeydown=e=>{if(e.key==='ArrowLeft'||e.key==='ArrowRight'){e.preventDefault();inspect(selected+(e.key==='ArrowLeft'?-1:1));tip.setAttribute('role','status');}if(e.key==='Escape')tip.hidden=true;};
  }
  function renderCharts() { document.querySelectorAll('[data-chart]').forEach(drawChart); }
  document.querySelectorAll('[data-minutes]').forEach(button=>button.addEventListener('click',()=>{state.range=Number(button.dataset.minutes);document.querySelectorAll('[data-minutes]').forEach(b=>b.setAttribute('aria-pressed',String(Number(b.dataset.minutes)===state.range)));renderCharts();}));
  let resizeFrame;
  addEventListener('resize',()=>{cancelAnimationFrame(resizeFrame);resizeFrame=requestAnimationFrame(renderCharts);});
  function freshness() {
    const omissions=state.stats?.omittedRouteSamples;
    const issues=[];
    if (state.paused) issues.push('Updates are paused. Values shown are the last readings, not live measurements.');
    if (state.failures.length) issues.push('Could not refresh '+state.failures.join(', ')+'. Last known values remain visible and may be stale.');
    if (numeric(omissions)&&omissions>0) issues.push(count(omissions)+' route samples were omitted by tracking bounds. Route and group tables undercount; platform totals can differ.');
    $('fetch-notice').hidden=!issues.length; $('fetch-notice').textContent=issues.join(' '); $('fetch-notice').className='notice'+(state.failures.length?' error':'');
    const connected=Boolean(state.status), mode=state.paused?'paused':state.failures.length?'stale':connected?'live':'loading';
    $('live-state').dataset.state=mode; $('connection').textContent=state.paused?'Updates paused':state.failures.length?'Partial or stale data':connected?'Connected · refreshes every 10s':'Waiting for node';
  }
  async function read(path) {
    const controller=new AbortController(), timer=setTimeout(()=>controller.abort(),8000);
    try { const response=await fetch(path,{cache:'no-store',signal:controller.signal}); if(!response.ok)throw new Error('HTTP '+response.status);return await response.json(); } finally {clearTimeout(timer);}
  }
  function valid(key,data) {
    if(!data || typeof data!=='object')return false;
    if(key==='status')return data.ok===true && numeric(data.uptimeSec) && data.memory && data.cpu && data.stats;
    if(key==='stats')return data.windowMinutes===15 && Array.isArray(data.routes) && Array.isArray(data.platforms) && numeric(data.omittedRouteSamples);
    return data.maxMinutes===480 && Array.isArray(data.slots) && data.slots.length===480 && data.slots.every(s=>s && numeric(s.created) && s.requests);
  }
  async function poll() {
    if(state.running||state.paused)return;state.running=true;clearTimeout(state.timer);
    try {
      const endpoints=[['status','Current status','/statusz'],['stats','API statistics','/statsz']];
      if(!state.history || Date.now()-state.historyAt>=30000 || state.failures.includes('History')) endpoints.push(['history','History','/status-history']);
      const results=await Promise.allSettled(endpoints.map(e=>read(e[2])));
      if(state.paused)return;
      state.failures=[];let historyChanged=false;
      results.forEach((r,i)=>{const key=endpoints[i][0];if(r.status==='fulfilled'&&valid(key,r.value)){state[key]=r.value;if(key==='history'){state.historyAt=Date.now();historyChanged=true;}}else state.failures.push(endpoints[i][1]);});
      renderStatus();renderStats();if(historyChanged)renderCharts();
      $('updated').textContent=(state.failures.length?'Last refresh attempt ':'Updated ')+new Date().toLocaleTimeString();freshness();
    } catch (_) { state.failures=['Dashboard rendering'];freshness(); }
    finally {state.running=false;if(!state.paused)state.timer=setTimeout(poll,10000);}
  }
  $('pause').addEventListener('click',()=>{state.paused=!state.paused;$('pause').setAttribute('aria-pressed',String(state.paused));$('pause').textContent=state.paused?'Resume updates':'Pause updates';clearTimeout(state.timer);freshness();if(!state.paused)poll();});
  document.addEventListener('visibilitychange',()=>{if(!document.hidden&&!state.paused)poll();});
  showView();poll();
})();
</script>
</body>
</html>
)FDASH"),
