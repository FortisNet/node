"""Explicitly test additional empty fallback calls without rewriting frozen oracles."""
import re
import time

CHAINS = {'eth-mainnet': 'ethereum', 'eip155:1': 'ethereum', 'base': 'base', 'eip155:8453': 'base',
          'bnb': 'bsc', 'eip155:56': 'bsc', 'arbitrum': 'arbitrum', 'eip155:42161': 'arbitrum',
          'polygon': 'polygon', 'eip155:137': 'polygon', 'optimism': 'optimism', 'eip155:10': 'optimism',
          'avalanche': 'avax', 'eip155:43114': 'avax', 'cronos': 'cronos', 'eip155:25': 'cronos',
          'xdc': 'xdc', 'eip155:50': 'xdc'}

def fallback_key(raw):
    value = raw.strip().lower()
    if re.fullmatch(r'[a-z0-9][a-z0-9-]{0,99}', value): return 'coingecko:' + value
    network, separator, address = value.partition(':erc20:')
    if separator and network in CHAINS and re.fullmatch(r'0x[0-9a-f]{40}', address):
        return CHAINS[network] + ':' + address
    return None

def empty_fallback_reply(handler, path, observed, lock):
    if not path.startswith('/prices/current/'): return False
    assert not handler.headers.get('x-cg-demo-api-key'), 'CoinGecko credential leaked to fallback'
    assert not handler.headers.get('x-cg-pro-api-key'), 'CoinGecko credential leaked to fallback'
    keys = path.removeprefix('/prices/current/').split(',')
    assert 1 <= len(keys) <= 16 and len(keys) == len(set(keys)), path
    assert len(path.encode()) <= 2048, path
    with lock: observed.append(path)
    body = b'{"coins":{}}'
    handler.send_response(200)
    handler.send_header('Content-Type', 'application/json')
    handler.send_header('Content-Length', str(len(body)))
    handler.send_header('Connection', 'close')
    handler.end_headers()
    try: handler.wfile.write(body)
    except OSError: pass
    handler.close_connection = True
    return True

def assert_fallback_for_case(upstream, case, index, name):
    body = case['body']
    if isinstance(body, dict): body = body.get('decoded', body)
    fiat = body.get('fiat', '') if isinstance(body, dict) else ''
    expected = {key for raw in body.get('missing', []) if (key := fallback_key(raw))} if fiat == 'usd' else set()
    deadline = time.monotonic() + 10
    while True:
        with upstream.lock: calls = list(upstream.fallback_observed[index:])
        keys = [key for path in calls for key in path.removeprefix('/prices/current/').split(',')]
        if set(keys) == expected or time.monotonic() >= deadline: break
        time.sleep(.01)
    assert set(keys) == expected and len(keys) == len(set(keys)), (name, keys, expected)
    return index + len(calls)
