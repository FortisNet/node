#pragma once

namespace fortis::operations {

inline constexpr char kNetworkPreviewPage[] = R"FORTISHTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Networks · FortisNet Node</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; background: #0b0b0d; color: #e2e2e2; }
    * { box-sizing: border-box; }
    body { margin: 0; min-height: 100vh; background: radial-gradient(circle at 75% 0%, #231336, #0b0b0d 55%); }
    header { padding: 24px max(24px, calc((100vw - 1400px) / 2)); border-bottom: 1px solid #30283a; }
    header a { color: #b990ff; text-decoration: none; font-size: 14px; }
    h1 { margin: 14px 0 4px; font-size: clamp(28px, 4vw, 42px); }
    header p, .muted { color: #a1a1aa; }
    main { max-width: 1448px; margin: auto; padding: 24px; display: grid; grid-template-columns: minmax(260px, 350px) minmax(0, 1fr); gap: 20px; }
    section { min-width: 0; border: 1px solid #30283a; border-radius: 14px; background: #15131a; }
    .sidebar { display: flex; flex-direction: column; max-height: calc(100vh - 190px); }
    .sidehead, .detailhead { padding: 20px; border-bottom: 1px solid #30283a; }
    h2 { font-size: 18px; margin: 0 0 8px; overflow-wrap: anywhere; }
    h3 { font-size: 15px; margin: 0 0 12px; }
    input { width: 100%; border: 1px solid #433951; border-radius: 8px; background: #0d0c10; color: inherit; padding: 10px 12px; font: inherit; }
    #network-list { overflow: auto; min-height: 180px; }
    .network { display: block; width: 100%; padding: 12px 20px; border: 0; border-bottom: 1px solid #29242e; background: transparent; color: inherit; text-align: left; cursor: pointer; }
    .network:hover, .network[aria-selected="true"] { background: #30233e; }
    .network strong, .network small { display: block; }
    .network small { margin-top: 3px; color: #a1a1aa; }
    .detailhead { display: flex; justify-content: space-between; align-items: start; gap: 16px; flex-wrap: wrap; }
    .detailbody { padding: 20px; }
    .sectionbox { border: 1px solid #30283a; border-radius: 10px; padding: 18px; margin-bottom: 16px; background: #100f14; }
    .facts { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px 24px; }
    .fact-label { display: block; color: #a1a1aa; font-size: 12px; margin-bottom: 3px; }
    .fact-value { overflow-wrap: anywhere; }
    .caps { display: flex; gap: 7px; flex-wrap: wrap; }
    .cap { border: 1px solid #45404b; color: #aaa; border-radius: 999px; padding: 5px 9px; font-size: 12px; }
    .cap.ok { color: #79e5bd; border-color: #22634f; background: #122a22; }
    .state { color: #b990ff; font-weight: 700; }
    .state.fresh { color: #79e5bd; }
    .state.error { color: #fca5a5; }
    .endpoints { margin: 8px 0 0; padding-left: 18px; overflow-wrap: anywhere; }
    .endpoints li { margin: 5px 0; }
    pre { overflow: auto; max-height: 420px; margin: 12px 0 0; padding: 14px; border-radius: 8px; background: #09090b; font-size: 12px; line-height: 1.5; white-space: pre-wrap; overflow-wrap: anywhere; }
    button.refresh { border: 1px solid #554365; border-radius: 8px; background: #221b2a; color: #ddd; padding: 8px 12px; cursor: pointer; }
    @media (max-width: 760px) { main { grid-template-columns: 1fr; } .sidebar { max-height: 340px; } .facts { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <header><a href="/">← FortisNet Node</a><h1>Networks</h1><p>Browse the network catalogue and inspect details already held by this node.</p></header>
  <main>
    <section class="sidebar" aria-label="Network catalogue">
      <div class="sidehead"><h2>Networks <span id="network-count" class="muted"></span></h2><input id="search" type="search" placeholder="Search network or family" aria-label="Search networks"></div>
      <div id="network-list" role="listbox" aria-label="Available networks"></div>
    </section>
    <section aria-label="Network details">
      <div class="detailhead"><div><h2 id="selected-id">Select a network</h2><span id="selected-type" class="muted"></span></div><button id="refresh" class="refresh" type="button">Refresh cache view</button></div>
      <div class="detailbody">
        <div class="sectionbox"><h3>Catalogue details</h3><div id="facts" class="facts"></div></div>
        <div class="sectionbox"><h3>Capabilities served by this node</h3><div id="capabilities" class="caps"></div></div>
        <div class="sectionbox"><h3>Public endpoints</h3><div id="endpoints" class="muted">—</div></div>
        <div class="sectionbox"><h3>Cached native asset price</h3><div id="native-price" class="muted">Select a network</div></div>
        <div class="sectionbox"><h3>Cached popular tokens</h3><div id="cache-state" class="state muted">Select a network</div><div id="cache-times" class="muted"></div><pre id="cache-body" hidden></pre><p class="muted" style="font-size:12px">Key: <span id="cache-key">—</span>. This is a stored snapshot only; viewing it never fetches a provider or writes to the cache. Other network data may live under separate keys.</p></div>
      </div>
    </section>
  </main>
  <script>
    const list = document.getElementById('network-list');
    const search = document.getElementById('search');
    const facts = document.getElementById('facts');
    const capabilities = document.getElementById('capabilities');
    const endpoints = document.getElementById('endpoints');
    const cacheState = document.getElementById('cache-state');
    const cacheTimes = document.getElementById('cache-times');
    const cacheBody = document.getElementById('cache-body');
    let networks = [], selected = null, requestNumber = 0;
    const date = value => value ? new Date(value).toLocaleString() : '—';
    function addFact(label, value) {
      const cell = document.createElement('div');
      const name = document.createElement('span'); name.className = 'fact-label'; name.textContent = label;
      const text = document.createElement('span'); text.className = 'fact-value'; text.textContent = value == null ? '—' : String(value);
      cell.append(name, text); facts.append(cell);
    }
    function renderList() {
      const query = search.value.trim().toLowerCase();
      const shown = networks.filter(n => (n.id + ' ' + n.family + ' ' + (n.native?.symbol || '')).toLowerCase().includes(query));
      document.getElementById('network-count').textContent = `${shown.length} / ${networks.length}`;
      list.replaceChildren();
      for (const network of shown) {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'network';
        button.setAttribute('role', 'option'); button.setAttribute('aria-selected', String(network.id === selected));
        const title = document.createElement('strong'); title.textContent = network.id;
        const meta = document.createElement('small');
        const served = Object.values(network.capabilities || {}).filter(value => value === 'ok').length;
        meta.textContent = `${network.family} · ${network.testnet ? 'testnet' : 'mainnet'} · ${served} capabilities`;
        button.append(title, meta); button.addEventListener('click', () => select(network.id)); list.append(button);
      }
      if (!shown.length) list.textContent = 'No matching networks.';
    }
    async function loadCache() {
      if (!selected) return;
      const current = ++requestNumber;
      cacheState.textContent = 'Reading cache…'; cacheState.className = 'state muted';
      cacheTimes.textContent = ''; cacheBody.hidden = true;
      try {
        const response = await fetch('/networks/data/' + encodeURIComponent(selected), {cache:'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json();
        if (current !== requestNumber) return;
        cacheState.textContent = data.state === 'missing' ? 'No cached popular-token snapshot' : `Cache: ${data.state}`;
        cacheState.className = 'state ' + (data.state === 'fresh' ? 'fresh' : data.state === 'error' ? 'error' : '');
        document.getElementById('cache-key').textContent = data.key;
        cacheTimes.textContent = data.asOf ? `Stored ${date(data.asOf)} · fresh until ${date(data.freshUntil)} · expires ${date(data.staleUntil)}` : '';
        document.getElementById('native-price').textContent = data.nativePrice ?
          `${new Intl.NumberFormat(undefined, {style:'currency', currency:'USD', maximumSignificantDigits:8}).format(data.nativePrice.price)} · stored ${date(data.nativePrice.asOf)}` :
          'No cached USD price for this network’s native asset.';
        if (data.body !== null) {
          try { cacheBody.textContent = JSON.stringify(JSON.parse(data.body), null, 2); }
          catch { cacheBody.textContent = data.body; }
          cacheBody.hidden = false;
        }
      } catch (error) {
        if (current !== requestNumber) return;
        cacheState.textContent = 'Could not read cache: ' + error.message;
        cacheState.className = 'state error';
      }
    }
    function select(id) {
      const network = networks.find(item => item.id === id);
      if (!network) return;
      selected = id; history.replaceState(null, '', '#' + encodeURIComponent(id)); renderList();
      document.getElementById('selected-id').textContent = id;
      document.getElementById('selected-type').textContent = `${network.family} · ${network.testnet ? 'testnet' : 'mainnet'}`;
      facts.replaceChildren();
      addFact('EVM chain ID', network.evmChainId);
      addFact('Required confirmations', network.requiredConfirmations);
      addFact('Native currency', network.native?.symbol);
      addFact('Native decimals', network.native?.decimals);
      addFact('Native price ID', network.native?.coingeckoId);
      capabilities.replaceChildren();
      for (const [name, value] of Object.entries(network.capabilities || {})) {
        const badge = document.createElement('span'); badge.className = 'cap' + (value === 'ok' ? ' ok' : '');
        badge.textContent = `${name}: ${value}`; capabilities.append(badge);
      }
      endpoints.replaceChildren();
      const urls = [...(network.rpcUrls || []), ...(network.graphqlUrls || [])];
      if (urls.length) {
        const ul = document.createElement('ul'); ul.className = 'endpoints';
        for (const url of urls) { const li = document.createElement('li'); li.textContent = url; ul.append(li); }
        endpoints.append(ul);
      } else endpoints.textContent = 'No public endpoint in the catalogue.';
      document.getElementById('cache-key').textContent = 'netoverview:v3:' + id;
      loadCache();
    }
    async function loadCatalog() {
      try {
        const response = await fetch('/networks/data', {cache:'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json(); networks = data.networks || [];
        renderList();
        const hash = decodeURIComponent(location.hash.slice(1));
        if (networks.length) select(networks.some(item => item.id === hash) ? hash : networks[0].id);
      } catch (error) { list.textContent = 'Could not load networks: ' + error.message; }
    }
    search.addEventListener('input', renderList);
    document.getElementById('refresh').addEventListener('click', loadCache);
    setInterval(loadCache, 60000);
    loadCatalog();
  </script>
</body>
</html>)FORTISHTML";

}  // namespace fortis::operations
