#!/usr/bin/env node
// Build a self-contained, read-only HTML reference and the source OpenAPI YAML
// for the native image. The spec is the reviewed API contract owned by this native project.
import { readFile, writeFile, readdir } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { routeBindings } from './route_bindings.mjs';

const native = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = native;
const source = path.join(native, 'contracts/openapi.base.yaml');
const nativeOverlay = path.join(native, 'openapi.native.yaml');
const output = path.join(native, 'src/operations/api_docs_generated.cpp');
const require = createRequire(import.meta.url);
const yaml = require('yaml');
const escape = (value) => String(value ?? '').replace(/[&<>"']/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const pretty = (value) => escape(JSON.stringify(value, null, 2));
const methods = new Set(['get', 'post', 'put', 'patch', 'delete', 'head']);
const routeKey = (method, route) => `${method.toUpperCase()} ${route.replace(/:([\w]+)/g, '{$1}')}`;

async function validateNativeCoverage(document) {
  const spec = new Set(Object.entries(document.paths).flatMap(([route, operations]) =>
    Object.keys(operations).filter(m => methods.has(m)).map(m => routeKey(m, route))));
  const sourceFiles = async dir => {
    const entries = await readdir(dir, { withFileTypes: true });
    const nested = await Promise.all(entries.map(entry => {
      const file = path.join(dir, entry.name);
      return entry.isDirectory() ? sourceFiles(file) : Promise.resolve(entry.name.endsWith('.cpp') ? [file] : []);
    }));
    return nested.flat();
  };
  const files = await sourceFiles(path.join(native, 'src'));
  const missing = [];
  for (const file of files) {
    const source = await readFile(file, 'utf8');
    for (const [method, route] of routeBindings(source)) {
      if (route.startsWith('/v1/') && !route.startsWith('/v1/test-operations/') &&
          !spec.has(routeKey(method, route))) missing.push(`${method} ${route} (${path.relative(native, file)})`);
    }
  }
  if (missing.length) throw new Error(`Native routes missing from OpenAPI:\n${missing.join('\n')}`);
}

function render(document) {
  const byTag = new Map();
  for (const [route, operations] of Object.entries(document.paths)) {
    for (const [method, operation] of Object.entries(operations)) {
      if (!methods.has(method)) continue;
      const tag = operation.tags?.[0] ?? 'Other';
      if (!byTag.has(tag)) byTag.set(tag, []);
      byTag.get(tag).push({ route, method, operation });
    }
  }
  const groups = [...byTag.entries()].map(([tag, operations]) => {
    const cards = operations.map(({route, method, operation}) => {
      const id = escape(operation.operationId ?? `${method}-${route}`);
      const details = [];
      if (operation.parameters?.length) details.push(`<details><summary>Parameters</summary><pre>${pretty(operation.parameters)}</pre></details>`);
      if (operation.requestBody) details.push(`<details><summary>Request body</summary><pre>${pretty(operation.requestBody)}</pre></details>`);
      if (operation.responses) details.push(`<details><summary>Responses</summary><pre>${pretty(operation.responses)}</pre></details>`);
      const security = operation.security ?? document.security;
      return `<article id="${id}" data-search="${escape(`${tag} ${method} ${route} ${operation.summary ?? ''}`.toLowerCase())}">
<h3><span class="method ${method}">${escape(method.toUpperCase())}</span><code>${escape(route)}</code></h3>
<p class="summary">${escape(operation.summary ?? '')}</p><p>${escape(operation.description ?? '').replace(/\n/g, '<br>')}</p>
<p class="meta">Operation: <code>${id}</code> · Security: ${escape(security?.length ? security.map(s => Object.keys(s).join(', ')).join('; ') : 'public')}</p>${details.join('')}</article>`;
    }).join('\n');
    return `<section id="tag-${escape(tag.replace(/\W+/g, '-'))}"><h2>${escape(tag)} <small>${operations.length}</small></h2>${cards}</section>`;
  }).join('\n');
  const schemaCards = Object.entries(document.components?.schemas ?? {}).map(([name, schema]) =>
    `<details class="schema" data-search="${escape(name.toLowerCase())}"><summary>${escape(name)}</summary><pre>${pretty(schema)}</pre></details>`).join('\n');
  const nav = [...byTag.keys()].map(tag => `<a href="#tag-${escape(tag.replace(/\W+/g, '-'))}">${escape(tag)}</a>`).join('');
  const count = [...byTag.values()].reduce((sum, group) => sum + group.length, 0);
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>${escape(document.info.title)} — API Reference</title>
<style>:root{color-scheme:dark;font:14px/1.6 system-ui,sans-serif;background:#101116;color:#e8e8ee}*{box-sizing:border-box}body{margin:0;display:grid;grid-template-columns:230px minmax(0,1fr)}aside{position:sticky;top:0;height:100vh;overflow:auto;padding:1.5rem;background:#171920;border-right:1px solid #30343d}aside a{display:block;color:#b7c2d9;text-decoration:none;padding:.3rem 0}aside a:hover{color:#fff}main{max-width:1100px;width:100%;padding:2rem 3rem}h1{font-size:2rem}h2{margin-top:2.5rem;border-bottom:1px solid #343943;padding-bottom:.3rem}h2 small{color:#8793a7;font-size:.75rem}h3{display:flex;align-items:center;gap:1rem;flex-wrap:wrap}article,.schema{background:#1a1d25;border:1px solid #343943;border-radius:10px;padding:1rem 1.4rem;margin:1rem 0}article p{white-space:normal;color:#bdc4cf}code,pre{font-family:ui-monospace,SFMono-Regular,monospace}code{overflow-wrap:anywhere}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#11131a;border-radius:6px;padding:1rem;max-height:28rem;overflow:auto}details{margin:.75rem 0}summary{cursor:pointer;color:#c5aaff;font-weight:600}.method{border-radius:4px;padding:.2rem .5rem;color:#081011;background:#69d8a0;font-size:.7rem}.post{background:#e5b86b}.delete{background:#fa9292}.put,.patch{background:#80baf1}.meta{font-size:.8rem;color:#9da6b5!important}.summary{font-weight:700;color:#f0f1f6!important}input{width:100%;padding:.7rem;background:#20242d;color:#fff;border:1px solid #444c58;border-radius:6px}a{color:#bc9fff}@media(max-width:700px){body{display:block}aside{position:relative;height:auto}main{padding:1rem}} </style></head><body>
<aside><strong>FortisNet API</strong><p>${count} operations · ${Object.keys(document.components?.schemas ?? {}).length} schemas</p><input id="search" type="search" placeholder="Filter operations" aria-label="Filter operations"><a href="/">← Home</a><a href="/openapi.yaml" download>Download OpenAPI YAML</a><hr>${nav}<a href="#schemas">Schemas</a></aside>
<main><h1>${escape(document.info.title)}</h1><p>Version ${escape(document.info.version)} · OpenAPI ${escape(document.openapi)} · Read-only reference</p><p>${escape(document.info.description ?? '').replace(/\n/g,'<br>')}</p>${groups}<section id="schemas"><h2>Schemas</h2>${schemaCards}</section></main>
<script>document.getElementById('search').addEventListener('input',function(){var q=this.value.trim().toLowerCase();document.querySelectorAll('[data-search]').forEach(function(el){el.hidden=!!q&&!el.dataset.search.includes(q)})})</script></body></html>`;
}

const mode = process.argv[2];
if (!['--write', '--check'].includes(mode)) throw new Error('usage: node scripts/generate_api_docs.mjs --write|--check');
// Keep native-only operations out of the shared TypeScript contract. The
// generated /openapi.yaml and /doc are the merged, accurate C++ surface.
const merged = yaml.parseDocument(await readFile(source, 'utf8'));
const overlay = yaml.parse(await readFile(nativeOverlay, 'utf8'));
for (const route of overlay.excludedPaths ?? []) merged.deleteIn(['paths', route]);
for (const name of overlay.excludedSecuritySchemes ?? [])
  merged.deleteIn(['components', 'securitySchemes', name]);
for (const name of overlay.excludedSchemas ?? []) merged.deleteIn(['components', 'schemas', name]);
for (const [route, operations] of Object.entries(overlay.paths ?? {}))
  merged.setIn(['paths', route], operations);
for (const [name, schema] of Object.entries(overlay.components?.schemas ?? {}))
  merged.setIn(['components', 'schemas', name], schema);
const document = merged.toJS();
const raw = merged.toString();
if (!document.openapi?.startsWith('3.') || !document.paths) throw new Error('Expected an OpenAPI 3 document');
await validateNativeCoverage(document);
const html = render(document);
// The source contract is 187 KiB, so generated literals intentionally exceed
// the standard's minimum guaranteed literal length. The host build disables
// that warning for this generated translation unit only.
const literal = (value, delimiter) => `R"${delimiter}(${value})${delimiter}"`;
const generated = `// Generated by scripts/generate_api_docs.mjs from contracts/openapi.base.yaml\n// plus openapi.native.yaml. Do not edit; run make generate-api-docs.\n#include "api_docs_generated.h"\nnamespace fortis::operations {\n\nconst char kApiDocsHtml[] = ${literal(html,'FORTIS_HTML')};\nconst size_t kApiDocsHtmlLength = sizeof(kApiDocsHtml) - 1;\nconst char kApiDocsYaml[] = ${literal(raw,'FORTIS_YAML')};\nconst size_t kApiDocsYamlLength = sizeof(kApiDocsYaml) - 1;\n\n}  // namespace fortis::operations\n`;
if (mode === '--write') { await writeFile(output, generated); console.log(`generated ${path.relative(root, output)} (${countOperations(document)} operations)`); }
else if (await readFile(output, 'utf8').catch(() => '') !== generated) { console.error('API docs are stale; run make generate-api-docs'); process.exitCode = 1; }
else console.log('API docs are current');
function countOperations(document) { return Object.values(document.paths).reduce((n, p) => n + Object.keys(p).filter(k => methods.has(k)).length, 0); }
