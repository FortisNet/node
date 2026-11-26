#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, '..');
const sourcePath = path.join(scriptDir, '../contracts/network-catalog.json');
const outputPath = path.join(repoRoot, 'src/networks/network_registry.cpp');
const LIMITS = Object.freeze({
  networks: 43,
  idBytes: 64,
  familyBytes: 23,
  rpcUrls: 5,
  graphqlUrls: 3,
  urlBytes: 255,
  nativeSymbolBytes: 8,
  coingeckoIdBytes: 64,
  capabilityKeyBytes: 15,
  capabilityValueBytes: 4,
});
// Keep this in sync with kJsonCapacity in src/operations/network_route.cpp. The native
// writer needs one extra byte for its terminating NUL.
const NATIVE_JSON_CAPACITY = 32768;

export function readCatalog(source) {
  const catalog = JSON.parse(source);
  validateCatalog(catalog.seeds, catalog.capabilities);
  return catalog;
}

function utf8Length(value) { return Buffer.byteLength(value, 'utf8'); }
function assertString(value, label, maximum, { optional = false } = {}) {
  if (value === undefined && optional) return;
  if (typeof value !== 'string' || value.length === 0 || utf8Length(value) > maximum)
    throw new Error(`${label} must be a non-empty UTF-8 string of at most ${maximum} bytes`);
}

export function nativeJsonLength(seeds, capabilities) {
  const body = {
    networks: seeds.map((seed) => ({
      id: seed.id,
      family: seed.family,
      ...(seed.evmChainId !== undefined ? { evmChainId: seed.evmChainId } : {}),
      testnet: seed.testnet === true,
      ...(seed.requiredConfirmations !== undefined
        ? { requiredConfirmations: seed.requiredConfirmations } : {}),
      rpcUrls: seed.rpcUrls ?? [],
      ...(seed.graphqlUrls?.length ? { graphqlUrls: seed.graphqlUrls } : {}),
      ...(seed.native ? { native: seed.native } : {}),
      capabilities: Object.fromEntries(capabilities.map((name) => [name, 'none'])),
    })),
    prices: { spot: false, history: false,
      sources: { coingecko: 'keyless', jupiter: 'keyed', vestige: 'keyless' } },
  };
  // JSON.stringify uses two-byte short escapes for these five controls;
  // Writer::string uses six-byte \u00xx escapes. All other byte lengths match.
  let shortEscapes = 0;
  const visit = (value) => {
    if (typeof value === 'string') {
      for (const char of value)
        if ([8, 9, 10, 12, 13].includes(char.charCodeAt(0))) ++shortEscapes;
    } else if (Array.isArray(value)) value.forEach(visit);
    else if (value && typeof value === 'object') {
      for (const [key, item] of Object.entries(value)) {
        visit(key);
        visit(item);
      }
    }
  };
  visit(body);
  return Buffer.byteLength(JSON.stringify(body), 'utf8') + 4 * shortEscapes;
}

export function validateCatalog(seeds, capabilities) {
  if (!Array.isArray(seeds) || seeds.length === 0 || seeds.length > LIMITS.networks)
    throw new Error(`network count must be between 1 and ${LIMITS.networks}`);
  const ids = new Set();
  for (const seed of seeds) {
    assertString(seed.id, 'network id', LIMITS.idBytes);
    assertString(seed.family, `${seed.id}.family`, LIMITS.familyBytes);
    if (ids.has(seed.id)) throw new Error(`duplicate network id ${seed.id}`);
    ids.add(seed.id);
    for (const [field, maximum] of [['rpcUrls', LIMITS.rpcUrls],
                                    ['graphqlUrls', LIMITS.graphqlUrls]]) {
      const urls = seed[field] ?? [];
      if (!Array.isArray(urls) || urls.length > maximum)
        throw new Error(`${seed.id}.${field} exceeds ${maximum} entries`);
      urls.forEach((url, index) => assertString(url, `${seed.id}.${field}[${index}]`,
                                                LIMITS.urlBytes));
    }
    if (seed.evmChainId !== undefined &&
        (!Number.isInteger(seed.evmChainId) || seed.evmChainId < 0 || seed.evmChainId > 0xffffffff))
      throw new Error(`${seed.id}.evmChainId must fit uint32`);
    if (seed.requiredConfirmations !== undefined &&
        (!Number.isInteger(seed.requiredConfirmations) || seed.requiredConfirmations < 0 ||
         seed.requiredConfirmations > 0xffffffff))
      throw new Error(`${seed.id}.requiredConfirmations must fit uint32`);
    if (seed.native !== undefined) {
      assertString(seed.native.symbol, `${seed.id}.native.symbol`, LIMITS.nativeSymbolBytes);
      if (!Number.isInteger(seed.native.decimals) || seed.native.decimals < 0 ||
          seed.native.decimals > 0xffffffff)
        throw new Error(`${seed.id}.native.decimals must fit uint32`);
      assertString(seed.native.coingeckoId, `${seed.id}.native.coingeckoId`,
                   LIMITS.coingeckoIdBytes, { optional: true });
    }
  }
  if (!Array.isArray(capabilities) || capabilities.length !== 10 ||
      new Set(capabilities).size !== capabilities.length)
    throw new Error('NONE must define exactly ten unique capability names');
  for (const name of capabilities) assertString(name, 'capability name', LIMITS.capabilityKeyBytes);
  for (const status of ['none', 'ok', 'beta'])
    assertString(status, 'capability value', LIMITS.capabilityValueBytes);
  if (nativeJsonLength(seeds, capabilities) >= NATIVE_JSON_CAPACITY)
    throw new Error(`native network JSON exceeds ${NATIVE_JSON_CAPACITY - 1} bytes`);
  return true;
}

// Encode UTF-8 bytes with fixed-width octal escapes. C++ hex escapes can absorb
// following hex digits; octal escapes are always limited to three digits.
export function cppString(value) {
  let result = '"';
  for (const byte of Buffer.from(value, 'utf8')) {
    if (byte === 0x22) result += '\\"';
    else if (byte === 0x5c) result += '\\\\';
    else if (byte >= 0x20 && byte <= 0x7e) result += String.fromCharCode(byte);
    else result += `\\${byte.toString(8).padStart(3, '0')}`;
  }
  return result + '"';
}

function pointerArray(name, values) {
  if (!values?.length) return '';
  return `static const char* const ${name}[] = {\n${values.map((v) => `  ${cppString(v)},`).join('\n')}\n};\n`;
}

export function renderCatalog(seeds, capabilities) {
  validateCatalog(seeds, capabilities);
  let out = `// Generated by scripts/generate_network_catalog.mjs from\n// contracts/network-catalog.json. Do not edit by hand.\n#include "network_registry.h"\n\nnamespace fortis::networks {\n\nnamespace {\n`;
  seeds.forEach((seed, i) => {
    out += pointerArray(`kRpc${i}`, seed.rpcUrls ?? []);
    out += pointerArray(`kGraphql${i}`, seed.graphqlUrls ?? []);
  });
  out += '\nstatic const NativeNetworkConfig kNetworks[] = {\n';
  for (const [i, seed] of seeds.entries()) {
    const native = seed.native;
    const fields = [
      cppString(seed.id), cppString(seed.family),
      seed.evmChainId === undefined ? 'false' : 'true', `${seed.evmChainId ?? 0}U`,
      seed.testnet === true ? 'true' : 'false',
      seed.requiredConfirmations === undefined ? 'false' : 'true',
      `${seed.requiredConfirmations ?? 0}U`,
      seed.rpcUrls?.length ? `kRpc${i}` : 'nullptr', `${seed.rpcUrls?.length ?? 0}U`,
      seed.graphqlUrls?.length ? `kGraphql${i}` : 'nullptr', `${seed.graphqlUrls?.length ?? 0}U`,
      native ? cppString(native.symbol) : 'nullptr', `${native?.decimals ?? 0}U`,
      native?.coingeckoId === undefined ? 'nullptr' : cppString(native.coingeckoId),
      `{${capabilities.map(() => '"none"').join(', ')}}`,
    ];
    out += `  {${fields.join(', ')}},\n`;
  }
  out += '};\n}  // namespace\n\n';
  out += `const char* const kNetworkCapabilityNames[kNetworkCapabilityCount] = {\n${capabilities.map((name) => `  ${cppString(name)},`).join('\n')}\n};\n\n`;
  out += 'const NativeNetworkConfig* NativeNetworkRegistry::list(size_t* count) const {\n';
  out += '  if (count) *count = sizeof(kNetworks) / sizeof(kNetworks[0]);\n';
  out += '  return kNetworks;\n}\n\n}  // namespace fortis::networks\n';
  return out;
}

async function main() {
  const mode = process.argv[2];
  if (mode !== '--write' && mode !== '--check')
    throw new Error('usage: node scripts/generate_network_catalog.mjs --write|--check');
  const source = await readFile(sourcePath, 'utf8');
  const { seeds, capabilities } = readCatalog(source);
  const generated = renderCatalog(seeds, capabilities);
  if (mode === '--write') {
    await writeFile(outputPath, generated);
    process.stdout.write(`generated ${path.relative(repoRoot, outputPath)} (${seeds.length} networks)\n`);
    return;
  }
  const current = await readFile(outputPath, 'utf8');
  if (current !== generated) {
    process.stderr.write('native network catalog is stale; run make generate-network-catalog\n');
    process.exitCode = 1;
    return;
  }
  process.stdout.write(`native network catalog matches native contract (${seeds.length} networks)\n`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((error) => { console.error(error.message); process.exitCode = 1; });
}
