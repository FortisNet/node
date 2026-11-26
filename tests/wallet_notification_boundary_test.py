#!/usr/bin/env python3
"""The decentralized node must expose no centralized notification bridge."""
import os
import sys
import tempfile
from pathlib import Path
from device_challenge_race_test import exchange, free_port, server_env, start, stop


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix='fortis-no-notifications-') as directory:
        root = Path(directory)
        (root / 'kv').mkdir(mode=0o700)
        secret = root / 'retired-bridge-token'
        secret.write_text('retired-wallet-key-' + 'a' * 48)
        secret.chmod(0o600)
        port = free_port()
        env = server_env(str(root / 'kv'), NODE_ENV='development', ATTESTATION_MODE='development',
                         FORTIS_WALLET_BRIDGE_TOKEN_FILE=str(secret))
        server = start(binary, port, env)
        try:
            for method, path in [('GET', '/internal/wallet-devices'),
                                 ('GET', '/internal/wallet-devices/' + '0' * 26),
                                 ('POST', '/internal/wallet-devices/authorize'),
                                 ('GET', '/internal/wallet-news'),
                                 ('GET', '/internal/wallet-prices/bitcoin'),
                                 ('POST', '/v1/push-tokens'),
                                 ('POST', '/v1/installations'),
                                 ('GET', '/v1/notification-preferences')]:
                body = b'{}' if method == 'POST' else b''
                request = (f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'
                           f'Authorization: Bearer {secret.read_text()}\r\n'
                           'Content-Type: application/json\r\n'
                           f'Content-Length: {len(body)}\r\n\r\n').encode() + body
                status, _, _ = exchange(port, request)
                assert status == 404, (method, path, status)
        finally:
            stop(server)
    print('notification boundary: PASS (retired bridge cannot enable routes; no registration/settings/push endpoints)')


if __name__ == '__main__':
    main()
