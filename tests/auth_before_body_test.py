#!/usr/bin/env python3
"""Exercise every protected POST family in the production composition.

Body/media/schema refusals follow device auth. HTTP message framing and
router path refusals happen before handlers, by design; this test uses valid
HTTP framing. No enrolled token or real provider connection is needed.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import ai_open_route_test as wire
import discovery_route_harness as harness
import tls_ca

POSTS = [
    '/v1/devices/rotate', '/v1/devices/revoke',
    '/v1/sync', '/v1/prices/history', '/v1/prices/markets',
    '/v1/bittensor/analytics', '/v1/tokens/compare', '/v1/crash-reports',
    '/v1/exchange/cross-chain/quotes', '/v1/exchange/cross-chain/status',
    '/v1/networks/robinhood/transactions/diagnose',
    '/v1/networks/robinhood/sponsored-trades/prepare',
    '/v1/networks/robinhood/sponsored-trades/submit',
    '/v1/ai/conversations',
    '/v1/ai/conversations/01JZ0000000000000000000000/messages',
    '/v1/ai/conversations/01JZ0000000000000000000000/messages/01JZ0000000000000000000001/retry',
]
DEPLOY_GETS = ['/v1/prices?ids=bitcoin&fiat=usd', '/v1/prices/snapshot', '/v1/news?limit=100',
               '/v1/networks/ethereum/address/0x0000000000000000000000000000000000000000/balances',
               '/v1/exchange/cross-chain/providers', '/v1/networks/robinhood/stock-tokens',
               '/v1/dashpay/names/resolve?name=example']


def main(binary):
    with tempfile.TemporaryDirectory(prefix='fortis-auth-order-') as directory:
        root = Path(directory)
        (root / 'kv').mkdir()
        (root / 'tls').mkdir()
        ca, _, _, _ = tls_ca.create_certificates(root / 'tls', 'DNS:localhost')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('FORTIS_', 'AI_', 'ANTHROPIC_', 'OPENAI_')) and k != 'SUPPORT_AUTH_SECRET'}
        env.update(FORTIS_KV_DIR=str(root / 'kv'), FORTIS_CA_BUNDLE_PATH=str(ca),
                   SUPPORT_AUTH_SECRET='fixture-support-secret-with-32-characters')
        env['FORTIS_PRICE_SNAPSHOT_ENABLED'] = '1'
        for flag in ('PRICE_ROUTE', 'NEWS', 'ADDRESS', 'AI', 'CROSS_CHAIN', 'ROBINHOOD', 'DASHPAY'):
            env[f'FORTIS_NATIVE_{flag}_CANARY'] = '1'
        port = harness.free_port()
        server = type('Server', (), {'port': port})()
        env['PORT'] = str(port)
        with (root / 'server.log').open('wb') as log:
            proc = subprocess.Popen([binary, str(port)], env=env, stdout=log, stderr=log)
            try:
                for _ in range(400):
                    if proc.poll() is not None:
                        raise AssertionError((root / 'server.log').read_text())
                    try:
                        result = wire.call(server, path='/healthz', method='GET', device=None)
                        if result and result[0] == 200:
                            break
                    except OSError:
                        pass
                    time.sleep(.025)
                else:
                    raise AssertionError('server startup timed out')
                count = 0
                for path in POSTS:
                    for media, body in [('application/json', b'{}'), ('application/json', b''),
                                        ('application/json', b'{'), ('text/plain', b'private body'),
                                        ('application/x-www-form-urlencoded', b'a=b'),
                                        ('application/protobuf', b'\xff')]:
                        for extra, status, code in [((), 401, 'UNAUTHORIZED'),
                                                   (('Origin: https://example.invalid',), 403, 'BROWSER_ORIGIN_FORBIDDEN')]:
                            result = wire.call(server, path=path, body=body, content_type=media, device=None, extra=extra)
                            assert result and result[0] == status, (path, media, result)
                            assert json.loads(result[2]) == {'error': code}, (path, result)
                            assert len(result[2]) < 1024 and 'x-rate-limit-cost' not in result[1]
                            count += 1
                for path in DEPLOY_GETS:
                    result = wire.call(server, path=path, method='GET', device=None)
                    assert result and result[0] == 401, (path, result)
                print(f'production auth-before-body: {count} checks across {len(POSTS)} POST routes; deployment GET probes passed')
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == '__main__':
    main(sys.argv[1])
