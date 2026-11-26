import assert from 'node:assert/strict';
import { cppString, nativeJsonLength, renderCatalog, validateCatalog } from '../scripts/generate_network_catalog.mjs';

const capabilities = ['balances', 'transfers', 'nfts', 'insight', 'txStatus',
  'txDetail', 'token', 'trustlines', 'news', 'popularTokens'];
const seed = {
  id: 'sample', family: 'evm', evmChainId: 1, testnet: false,
  rpcUrls: ['https://rpc.example'], graphqlUrls: [],
  native: { symbol: 'ETH', decimals: 18, coingeckoId: 'ethereum' },
};
const clone = (value) => structuredClone(value);
const rejects = (mutate, pattern) => {
  const value = clone(seed);
  mutate(value);
  assert.throws(() => validateCatalog([value], capabilities), pattern);
};

assert.equal(cppString('q"\\\n\té'), '"q\\"\\\\\\012\\011\\303\\251"');
const escaped = clone(seed);
escaped.id = 'line\n"quote';
const rendered = renderCatalog([escaped], capabilities);
assert.match(rendered, /"line\\012\\"quote"/);
assert.doesNotMatch(rendered, /\/\/ line/);

const maxSeed = clone(seed);
maxSeed.id = 'i'.repeat(64);
maxSeed.family = 'f'.repeat(23);
maxSeed.rpcUrls = ['u'.repeat(255), 'a', 'b', 'c', 'd'];
maxSeed.graphqlUrls = ['g'.repeat(255), 'h', 'i'];
maxSeed.native.symbol = 's'.repeat(8);
maxSeed.native.coingeckoId = 'c'.repeat(64);
assert.equal(validateCatalog([maxSeed], capabilities), true);

rejects((value) => { value.id = 'i'.repeat(65); }, /network id/);
rejects((value) => { value.family = 'f'.repeat(24); }, /family/);
rejects((value) => { value.rpcUrls = Array(6).fill('https://rpc.example'); }, /rpcUrls/);
rejects((value) => { value.rpcUrls = ['u'.repeat(256)]; }, /rpcUrls\[0\]/);
rejects((value) => { value.graphqlUrls = Array(4).fill('https://g.example'); }, /graphqlUrls/);
rejects((value) => { value.native.symbol = 's'.repeat(9); }, /native.symbol/);
rejects((value) => { value.native.coingeckoId = 'c'.repeat(65); }, /native.coingeckoId/);
assert.throws(() => validateCatalog(Array(44).fill(seed), capabilities), /network count/);
assert.throws(() => validateCatalog([seed, clone(seed)], capabilities), /duplicate network/);
assert.throws(() => validateCatalog([seed], capabilities.slice(1)), /exactly ten/);
const utf8 = clone(seed);
utf8.id = 'é'.repeat(33);
assert.throws(() => validateCatalog([utf8], capabilities), /network id/);
const oversized = Array.from({ length: 43 }, (_, index) => ({
  ...clone(maxSeed), id: `oversized-${index}`,
  rpcUrls: Array(5).fill('\n'.repeat(255)),
  graphqlUrls: Array(3).fill('\n'.repeat(255)),
}));
assert.throws(() => validateCatalog(oversized, capabilities), /native network JSON exceeds/);
const controlKey = [...capabilities];
controlKey[0] = 'a\n';
const plainKey = [...capabilities];
plainKey[0] = 'aX';
assert.equal(nativeJsonLength([seed], controlKey) - nativeJsonLength([seed], plainKey), 5);

process.stdout.write('network catalog generator escaping, byte lengths, and native response bounds passed\n');
