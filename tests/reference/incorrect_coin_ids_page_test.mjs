import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source = readFileSync(new URL('../../src/operations/operations_pages.cpp', import.meta.url), 'utf8');
const html = source.split('R"FERROR(')[1].split(')FERROR"')[0];
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const element = id => {if (!elements.has(id)) elements.set(id, {innerHTML:'',textContent:''}); return elements.get(id);};
const records = [
 {service:'prices',code:'provider_rejected_coin_id',origin:'third_party',provider:'coingecko',httpStatus:404,identifier:'old-coin',count:2,lastSeenMs:1700000000000},
 {service:'prices',code:'invalid_identifier',origin:'unclassified',identifier:'',count:1,lastSeenMs:1700000000000},
 {service:'providers',code:'http_rate_limited',origin:'third_party',provider:'coingecko',httpStatus:429,count:1,lastSeenMs:1700000000000},
 {service:'prices',code:'quote_unavailable',origin:'unclassified',identifier:'bitcoin',count:1,lastSeenMs:1700000000000},
];
vm.runInNewContext(script, {
 document:{getElementById:element}, setInterval:()=>{},
 fetch:async()=>({json:async()=>({operational:{records,persisted:true},routes:[],recent:[]})}),
});
await new Promise(resolve=>setImmediate(resolve));
const incorrect = element('e-incorrect-ids').innerHTML;
assert.match(html, /Incorrect coin IDs/);
assert.match(html, /read-only/);
assert.match(incorrect, /old-coin/);
assert.match(incorrect, /invalid_identifier/);
assert.doesNotMatch(incorrect, /http_rate_limited|quote_unavailable/);
assert.match(element('e-third-party').innerHTML, /http_rate_limited/);
assert.doesNotMatch(element('e-third-party').innerHTML, /old-coin/);
assert.match(element('e-unclassified').innerHTML, /quote_unavailable/);
assert.doesNotMatch(html, /<form|method="post"/i);
console.log('Incorrect coin IDs page: read-only classification passed.');
