#pragma once

namespace fortis::operations {

inline constexpr char kCacheExplorerPage[] = R"FORTISHTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cache Explorer · FortisNet Node</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; background:#0b0b0d; color:#e2e2e2; }
    * { box-sizing: border-box; }
    body { margin:0; min-height:100vh; background:radial-gradient(circle at 75% 0%, #231336, #0b0b0d 55%); }
    header { padding:24px max(24px,calc((100vw - 1400px)/2)); border-bottom:1px solid #30283a; }
    a { color:#b990ff; text-decoration:none; }
    header a { font-size:14px; }
    h1 { margin:14px 0 4px; font-size:clamp(28px,4vw,42px); }
    h2 { font-size:17px; margin:0 0 8px; }
    p, .muted { color:#a1a1aa; }
    nav { display:flex; gap:8px; flex-wrap:wrap; margin-top:18px; }
    nav a { border:1px solid #554365; border-radius:8px; padding:7px 10px; background:#221b2a; font-size:13px; }
    nav a[aria-current="page"] { background:#50376a; color:white; }
    main { max-width:1448px; margin:auto; padding:24px; display:grid; grid-template-columns:minmax(270px,350px) minmax(0,1fr); gap:20px; }
    section { min-width:0; border:1px solid #30283a; border-radius:14px; background:#15131a; }
    .sidebar { display:flex; flex-direction:column; max-height:calc(100vh - 190px); }
    .sidehead, .detailhead { padding:20px; border-bottom:1px solid #30283a; }
    input { width:100%; border:1px solid #433951; border-radius:8px; background:#0d0c10; color:inherit; padding:10px 12px; font:inherit; }
    #namespaces { overflow:auto; min-height:180px; }
    .namespace { width:100%; text-align:left; border:0; border-bottom:1px solid #29242e; background:transparent; color:inherit; padding:12px 20px; cursor:pointer; }
    .namespace:hover, .namespace[aria-selected="true"] { background:#30233e; }
    .namespace strong, .namespace small { display:block; }
    .namespace small { color:#a1a1aa; margin-top:3px; overflow-wrap:anywhere; }
    .detailhead { display:flex; justify-content:space-between; gap:16px; flex-wrap:wrap; }
    .detailbody { padding:20px; }
    .summary { display:flex; gap:10px; flex-wrap:wrap; margin-bottom:18px; }
    .pill { border:1px solid #433951; border-radius:8px; padding:9px 12px; background:#100f14; font-size:13px; }
    .pill b { display:block; color:#b990ff; font-size:17px; }
    .record { border:1px solid #30283a; border-radius:10px; background:#100f14; padding:16px; margin-bottom:12px; }
    .record-title { font-weight:700; overflow-wrap:anywhere; }
    .record-meta { color:#a1a1aa; font-size:12px; margin-top:5px; }
    pre { overflow:auto; max-height:350px; padding:13px; margin:12px 0 0; border-radius:8px; background:#09090b; white-space:pre-wrap; overflow-wrap:anywhere; font-size:12px; line-height:1.5; }
    button.action { border:1px solid #554365; border-radius:8px; background:#221b2a; color:#ddd; padding:8px 12px; cursor:pointer; }
    #page-status { margin:0 0 14px; font-size:13px; }
    #more { display:none; }
    @media (max-width:760px) { main { grid-template-columns:1fr; } .sidebar { max-height:340px; } }
  </style>
</head>
<body>
  <header><a href="/">← FortisNet Node</a><h1 id="title">Cache Explorer</h1><p id="description"></p><p id="database-stats" class="muted"></p>
    <nav aria-label="Cache pages">
      <a href="/cache/inventory" data-group="inventory">Inventory</a>
      <a href="/cache/tokens" data-group="tokens">Token 360</a>
      <a href="/cache/prices" data-group="prices">Price Coverage</a>
      <a href="/cache/discovery" data-group="discovery">Discovery</a>
      <a href="/cache/news" data-group="news">News &amp; Media</a>
      <a href="/cache/address" data-group="address">Address &amp; Transactions</a>
      <a href="/networks">Networks</a><a href="/tokens">Token prices</a>
    </nav>
  </header>
  <main>
    <section class="sidebar" aria-label="KV namespaces">
      <div class="sidehead"><h2>KV namespaces <span id="namespace-count" class="muted"></span></h2><input id="search" type="search" placeholder="Search namespaces" aria-label="Search namespaces"></div>
      <div id="namespaces" role="listbox" aria-label="KV namespaces"></div>
    </section>
    <section aria-label="KV records">
      <div class="detailhead"><div><h2 id="selected">Select a namespace</h2><div id="prefix" class="muted"></div></div><button id="refresh" type="button" class="action">Refresh</button></div>
      <div class="detailbody">
        <p id="page-status">Each read examines at most 32 physical KV records. Counts and bytes describe this page, not the whole namespace.</p>
        <div class="summary"><div class="pill"><b id="scanned">—</b>records examined</div><div class="pill"><b id="live">—</b>live records shown</div><div class="pill"><b id="bytes">—</b>value bytes shown</div></div>
        <div id="records"></div><button id="more" type="button" class="action">Next page</button>
      </div>
    </section>
  </main>
  <script>
    const group = location.pathname.split('/').pop();
    const pages = {
      inventory: ['Cache Inventory', 'Inspect the allowlisted KV namespaces and overall store size. Private keys and values remain hidden.'],
      tokens: ['Token 360 Cache', 'Token facts, demand, fetch activity, liquidity observations and candles, comparisons, and holder history.'],
      prices: ['Price Coverage', 'Inspect stored quote, observation, candle, seal, and history-coverage records. Use the Token prices page for decoded charts.'],
      discovery: ['Discovery Cache', 'Browse stored market, coin-detail, analytics, search and network-overview results.'],
      news: ['News & Media Cache', 'Browse stored stories and indexes; media records show counts and sizes without their source URLs.'],
      address: ['Address & Transaction Cache', 'Inspect aggregate wallet and transaction storage without exposing addresses, identifiers or payloads.']
    };
    const title = pages[group] || pages.inventory;
    document.title = title[0] + ' · FortisNet Node';
    document.getElementById('title').textContent = title[0];
    document.getElementById('description').textContent = title[1];
    for (const link of document.querySelectorAll('nav a[data-group]'))
      if (link.dataset.group === group) link.setAttribute('aria-current', 'page');
    const list = document.getElementById('namespaces');
    const search = document.getElementById('search');
    const records = document.getElementById('records');
    const more = document.getElementById('more');
    const pageStatus = document.getElementById('page-status');
    let namespaces = [], selected = null, cursor = null, requestNumber = 0;
    const date = value => value ? new Date(value).toLocaleString() : '—';
    function renderList() {
      const query = search.value.trim().toLowerCase();
      const shown = namespaces.filter(item => (item.label + ' ' + item.prefix + ' ' + item.group).toLowerCase().includes(query));
      document.getElementById('namespace-count').textContent = `${shown.length} / ${namespaces.length}`;
      list.replaceChildren();
      for (const item of shown) {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'namespace';
        button.setAttribute('role', 'option'); button.setAttribute('aria-selected', String(item.slug === selected?.slug));
        const label = document.createElement('strong'); label.textContent = item.label;
        const hint = document.createElement('small'); hint.textContent = `${item.group} · ${item.prefix}`;
        button.append(label, hint); button.addEventListener('click', () => select(item)); list.append(button);
      }
      if (!shown.length) list.textContent = 'No matching namespace.';
    }
    function renderRecord(row, index, view) {
      const card = document.createElement('article'); card.className = 'record';
      const name = document.createElement('div'); name.className = 'record-title';
      name.textContent = row.key || `Record ${index + 1} · identifier hidden`;
      const meta = document.createElement('div'); meta.className = 'record-meta';
      const parts = [`${row.bytes.toLocaleString()} value bytes`];
      if (view === 'envelope') parts.push(row.corrupt ? 'invalid cache envelope' : `stored ${date(row.asOf)} · fresh until ${date(row.freshUntil)} · stale until ${date(row.staleUntil)}`);
      if (view === 'binary') parts.push('binary value · decoded details may be available on the feature page');
      if (view === 'metadata') parts.push('key and value hidden');
      if (row.truncated) parts.push('preview shortened');
      meta.textContent = parts.join(' · ');
      card.append(name, meta);
      if (row.preview != null && row.preview !== '') {
        const pre = document.createElement('pre');
        try { pre.textContent = JSON.stringify(JSON.parse(row.preview), null, 2); }
        catch { pre.textContent = row.preview; }
        card.append(pre);
      }
      return card;
    }
    async function loadPage(after = null) {
      if (!selected) return;
      const current = ++requestNumber;
      pageStatus.textContent = 'Reading a bounded KV page…'; more.style.display = 'none';
      const url = `/cache-data/${group}/${encodeURIComponent(selected.slug)}` + (after ? `?after=${encodeURIComponent(after)}` : '');
      try {
        const response = await fetch(url, {cache:'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json();
        if (current !== requestNumber) return;
        cursor = data.nextCursor;
        document.getElementById('scanned').textContent = data.scanned;
        document.getElementById('live').textContent = data.live;
        document.getElementById('bytes').textContent = data.bytes.toLocaleString();
        records.replaceChildren();
        data.rows.forEach((row, index) => records.append(renderRecord(row, index, data.view)));
        if (!data.rows.length) records.textContent = data.hasMore ? 'No live records in this page.' : 'No cached records in this namespace.';
        if (data.view === 'metadata') pageStatus.textContent = data.hasMore ? 'First 32 records sampled. Additional records exist; identifiers and values stay hidden.' : 'Only aggregate sizes are shown for this namespace.';
        else pageStatus.textContent = data.hasMore ? 'More records available.' : 'End of this namespace.';
        more.style.display = cursor ? 'inline-block' : 'none';
      } catch (error) {
        if (current !== requestNumber) return;
        pageStatus.textContent = 'Could not read this namespace: ' + error.message;
      }
    }
    function select(item) {
      selected = item; cursor = null; renderList();
      document.getElementById('selected').textContent = item.label;
      document.getElementById('prefix').textContent = item.prefix;
      history.replaceState(null, '', '#' + encodeURIComponent(item.slug));
      loadPage();
    }
    async function loadNamespaces() {
      try {
        const response = await fetch('/cache-data/' + group, {cache:'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const catalog = await response.json();
        namespaces = catalog.namespaces;
        if (catalog.stats) document.getElementById('database-stats').textContent =
          `${catalog.stats.keyCount.toLocaleString()} indexed keys · ${catalog.stats.liveBytes.toLocaleString()} live value bytes · ${catalog.stats.fileBytes.toLocaleString()} file bytes · ${catalog.stats.otherBytes.toLocaleString()} bytes of old data and framing`;
        renderList();
        const hash = decodeURIComponent(location.hash.slice(1));
        select(namespaces.find(item => item.slug === hash) || namespaces[0]);
      } catch (error) { list.textContent = 'Could not load namespaces: ' + error.message; }
    }
    search.addEventListener('input', renderList);
    document.getElementById('refresh').addEventListener('click', () => loadPage());
    more.addEventListener('click', () => { if (cursor) loadPage(cursor); });
    loadNamespaces();
  </script>
</body>
</html>)FORTISHTML";

}  // namespace fortis::operations
