#pragma once

namespace fortis::operations {

// Standalone, same-origin operations page. Data comes only from the durable
// price cache through /tokens/data; browser code never calls protected /v1.
inline constexpr char kTokenPricesPage[] = R"FORTISHTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cached Tokens · FortisNet Node</title>
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
    .sidehead, .charthead { padding: 20px; border-bottom: 1px solid #30283a; }
    h2 { font-size: 17px; margin: 0 0 10px; }
    input { width: 100%; border: 1px solid #433951; border-radius: 8px; background: #0d0c10; color: inherit; padding: 10px 12px; font: inherit; }
    #token-list { overflow: auto; min-height: 180px; }
    .token { display: block; width: 100%; padding: 13px 20px; border: 0; border-bottom: 1px solid #29242e; background: transparent; color: inherit; text-align: left; cursor: pointer; }
    .token:hover, .token[aria-selected="true"] { background: #30233e; }
    .token-id { display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-weight: 650; }
    .token-price, .token-date { display: block; margin-top: 3px; color: #a1a1aa; font-size: 12px; }
    .charthead { display: flex; justify-content: space-between; gap: 16px; flex-wrap: wrap; align-items: start; }
    #selected-id { overflow-wrap: anywhere; }
    #latest-price { font-size: 24px; font-weight: 750; color: #b990ff; }
    .ranges { display: flex; gap: 6px; flex-wrap: wrap; }
    .ranges button { padding: 7px 11px; border-radius: 8px; border: 1px solid #554365; background: #221b2a; color: #ddd; cursor: pointer; }
    .ranges button[aria-pressed="true"] { border-color: #b990ff; background: #50376a; color: white; }
    .chartbody { padding: 20px; }
    #chart { width: 100%; height: auto; display: block; overflow: visible; }
    #chart-status { min-height: 24px; margin: 12px 0 0; color: #a1a1aa; font-size: 13px; }
    #chart .grid { stroke: #3a3442; stroke-width: 1; }
    #chart .line { fill: none; stroke: #b990ff; stroke-width: 2.5; stroke-linejoin: round; stroke-linecap: round; }
    #chart .dot { fill: #e3cfff; }
    #chart text { fill: #a1a1aa; font: 12px system-ui, sans-serif; }
    #chart .area { fill: #654195; opacity: .18; }
    @media (max-width: 760px) { main { grid-template-columns: 1fr; } .sidebar { max-height: 340px; } }
  </style>
</head>
<body>
  <header><a href="/">← FortisNet Node</a><h1>Cached Tokens</h1><p>USD prices already stored by this node. Select a token to inspect its recent price history.</p></header>
  <main>
    <section class="sidebar" aria-label="Cached tokens">
      <div class="sidehead"><h2>Tokens <span id="token-count" class="muted"></span></h2><input id="search" type="search" placeholder="Search token ID" aria-label="Search cached tokens"></div>
      <div id="token-list" role="listbox" aria-label="Cached token prices"></div>
    </section>
    <section aria-label="Token price chart">
      <div class="charthead"><div><h2 id="selected-id">Select a token</h2><div id="latest-price">—</div><div id="last-updated" class="muted"></div></div><div class="ranges" aria-label="Chart time window">
        <button data-range="5m" aria-pressed="false">5m</button><button data-range="15m" aria-pressed="false">15m</button><button data-range="30m" aria-pressed="false">30m</button><button data-range="1h" aria-pressed="true">1h</button><button data-range="8h" aria-pressed="false">8h</button>
      </div></div>
      <div class="chartbody"><svg id="chart" viewBox="0 0 900 400" role="img" aria-label="Price over time"></svg><p id="chart-status" aria-live="polite">Select a cached token.</p><p class="muted" style="font-size:12px">The chart uses cached five-minute price buckets. It does not request a new price or backfill missing points.</p></div>
    </section>
  </main>
  <script>
    const list = document.getElementById('token-list');
    const chart = document.getElementById('chart');
    const status = document.getElementById('chart-status');
    const search = document.getElementById('search');
    let tokens = [], selected = null, range = '1h', requestNumber = 0;
    const money = n => Number.isFinite(n) ? '$' + n.toLocaleString(undefined, {maximumSignificantDigits: 8}) : '—';
    const time = ms => new Date(ms).toLocaleTimeString(undefined, {hour:'2-digit', minute:'2-digit'});
    const svg = (name, attributes) => { const element = document.createElementNS('http://www.w3.org/2000/svg', name); for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value); return element; };
    function draw(points) {
      chart.replaceChildren();
      if (!points.length) { status.textContent = 'No cached price points in this window.'; return; }
      const width = 900, height = 400, left = 78, right = 24, top = 25, bottom = 46;
      const values = points.map(point => point[1]);
      let min = Math.min(...values), max = Math.max(...values);
      const padding = Math.max((max - min) * .12, Math.abs(max) * .002, 1e-12);
      min -= padding; max += padding;
      const first = points[0][0], last = points[points.length - 1][0];
      const x = value => left + (last === first ? (width - left - right) / 2 : (value - first) * (width - left - right) / (last - first));
      const y = value => top + (max - value) * (height - top - bottom) / (max - min);
      for (let i = 0; i <= 4; i++) {
        const value = min + (max - min) * (4 - i) / 4;
        const yy = top + i * (height - top - bottom) / 4;
        chart.append(svg('line', {x1:left, x2:width-right, y1:yy, y2:yy, class:'grid'}));
        const label = svg('text', {x:left-10, y:yy+4, 'text-anchor':'end'}); label.textContent = money(value); chart.append(label);
      }
      const ticks = points.length === 1 ? [points[0]] : [points[0], points[Math.floor((points.length - 1)/2)], points[points.length - 1]];
      for (const point of ticks) { const label = svg('text', {x:x(point[0]), y:height-12, 'text-anchor':'middle'}); label.textContent = time(point[0]); chart.append(label); }
      if (points.length > 1) {
        const path = points.map((point, i) => (i ? 'L' : 'M') + x(point[0]) + ' ' + y(point[1])).join(' ');
        chart.append(svg('path', {d:path + ' L' + x(last) + ' ' + (height-bottom) + ' L' + x(first) + ' ' + (height-bottom) + ' Z', class:'area'}));
        chart.append(svg('path', {d:path, class:'line'}));
      }
      for (const point of points) { const dot = svg('circle', {cx:x(point[0]), cy:y(point[1]), r:points.length < 12 ? 4 : 2, class:'dot'}); const title = svg('title', {}); title.textContent = time(point[0]) + ' · ' + money(point[1]); dot.append(title); chart.append(dot); }
      status.textContent = points.length + (points.length === 1 ? ' cached point' : ' cached points') + ' · five-minute buckets';
    }
    async function loadChart() {
      if (!selected) return;
      const current = ++requestNumber;
      status.textContent = 'Loading cached prices…';
      try {
        const response = await fetch('/tokens/data/' + encodeURIComponent(selected) + '?range=' + range, {cache:'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json();
        if (current !== requestNumber) return;
        const now = data.now, windowMs = ({'5m':5,'15m':15,'30m':30,'1h':60,'8h':480})[range] * 60000;
        const points = data.points.filter(point => Number.isFinite(point[0]) && Number.isFinite(point[1]));
        if (data.spot && data.spot.asOf >= now - windowMs && Number.isFinite(data.spot.price) && (!points.length || data.spot.asOf > points[points.length-1][0])) points.push([data.spot.asOf, data.spot.price]);
        draw(points);
        document.getElementById('latest-price').textContent = data.spot ? money(data.spot.price) : '—';
        document.getElementById('last-updated').textContent = data.spot ? 'Last quote ' + new Date(data.spot.asOf).toLocaleString() : 'No current spot quote';
      } catch (error) { if (current === requestNumber) { chart.replaceChildren(); status.textContent = 'Could not read cached prices: ' + error.message; } }
    }
    function renderList() {
      const needle = search.value.trim().toLowerCase();
      list.replaceChildren();
      const filtered = tokens.filter(token => token.id.toLowerCase().includes(needle));
      document.getElementById('token-count').textContent = '(' + tokens.length + ')';
      if (!filtered.length) { const empty = document.createElement('p'); empty.className = 'muted'; empty.style.padding = '16px 20px'; empty.textContent = tokens.length ? 'No matching tokens.' : 'No cached USD tokens yet.'; list.append(empty); return; }
      for (const token of filtered) {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'token'; button.setAttribute('role', 'option'); button.setAttribute('aria-selected', String(token.id === selected));
        const id = document.createElement('span'); id.className = 'token-id'; id.textContent = token.id;
        const price = document.createElement('span'); price.className = 'token-price'; price.textContent = money(token.price);
        const date = document.createElement('span'); date.className = 'token-date'; date.textContent = 'Last quote ' + new Date(token.asOf).toLocaleString();
        button.append(id, price, date); button.onclick = () => { selected = token.id; document.getElementById('selected-id').textContent = token.id; renderList(); loadChart(); }; list.append(button);
      }
    }
    async function loadList() {
      try {
        const response = await fetch('/tokens/data', {cache:'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json(); tokens = data.tokens || [];
        if (selected && !tokens.some(token => token.id === selected)) selected = null;
        if (!selected && tokens.length) { selected = tokens[0].id; document.getElementById('selected-id').textContent = selected; loadChart(); }
        renderList();
      } catch (error) { list.textContent = 'Could not read token cache: ' + error.message; }
    }
    search.addEventListener('input', renderList);
    document.querySelectorAll('[data-range]').forEach(button => button.onclick = () => { range = button.dataset.range; document.querySelectorAll('[data-range]').forEach(item => item.setAttribute('aria-pressed', String(item === button))); loadChart(); });
    loadList(); setInterval(loadList, 60000); setInterval(loadChart, 30000);
  </script>
</body>
</html>)FORTISHTML";

}  // namespace fortis::operations
