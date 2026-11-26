#!/usr/bin/env python3
"""Replay actual captured Flutter request bytes against production composition.
Synthetic public test inputs only; ephemeral development enrollment, KV and CA.
Compare refusal codes with a pre-reader-change run to catch new schema 400s.
Provider outcomes may vary; they are not successful transaction assertions.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import ai_open_route_test as wire
import discovery_route_harness as harness
import tls_ca

FIXTURE = Path(__file__).parent / 'fixtures/app_post_requests.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--strict-types', action='store_true')
    args = parser.parse_args()
    captured = json.loads(FIXTURE.read_text())['requests']
    rows = []
    with tempfile.TemporaryDirectory(prefix='fortis-app-post-wire-') as directory:
        root = Path(directory)
        (root / 'kv').mkdir()
        (root / 'tls').mkdir()
        ca, _, _, _ = tls_ca.create_certificates(root / 'tls', 'DNS:localhost')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('FORTIS_', 'AI_', 'OPENAI_', 'ANTHROPIC_', 'ALCHEMY_', 'TAOSTATS_', 'NODE_'))
               and k not in ('SUPPORT_AUTH_SECRET', 'ATTESTATION_MODE')}
        env.update(ATTESTATION_MODE='development', FORTIS_KV_DIR=str(root/'kv'),
                   FORTIS_CA_BUNDLE_PATH=str(ca), SUPPORT_AUTH_SECRET='fixture-support-secret-32-characters')
        for flag in ('PRICE_ROUTE', 'NEWS', 'ADDRESS', 'AI', 'CROSS_CHAIN', 'ROBINHOOD'):
            env[f'FORTIS_NATIVE_{flag}_CANARY'] = '1'
        server = type('Server', (), {'port': harness.free_port()})()
        with (root/'server.log').open('wb') as log:
            proc = subprocess.Popen([args.binary, str(server.port)], env=env, stdout=log, stderr=log)
            try:
                for _ in range(400):
                    if proc.poll() is not None:
                        raise AssertionError((root/'server.log').read_text())
                    try:
                        result = wire.call(server, path='/healthz', method='GET', device=None)
                        if result and result[0] == 200: break
                    except OSError: pass
                    time.sleep(.025)
                else: raise AssertionError('server startup timeout')
                status, _, body = wire.call(server, path='/v1/devices', device=None,
                                           payload={'platform': 'ios', 'appVersion': '1.0'})
                assert status == 201, ('development enrollment', status, body)
                token = json.loads(body)['deviceToken']
                if args.strict_types:
                    invalid_sync = [
                        {'cursor': 1}, {'cursor': None}, {'cursor': ['cursor']},
                        {'models': 'transfer'}, {'models': [1]}, {'models': [['transfer']]},
                        {'limit': '1'}, {'limit': True}, {'limit': [1]}, {'limit': None},
                        {'after': ['2026-01-01T00:00:00Z']}, {'after': False}]
                    for payload in invalid_sync:
                        code, _, result = wire.call(server, path='/v1/sync', device=token, payload=payload)
                        assert code == 400 and json.loads(result)['error'] == 'BAD_REQUEST', ('sync types', payload, code, result)
                    for value in ('1767225600', True, [1767225600], None):
                        payload = {'ids': ['bitcoin'], 'from': value, 'to': 1767229200}
                        code, _, result = wire.call(server, path='/v1/prices/history', device=token, payload=payload)
                        assert code == 400 and json.loads(result)['error'] == 'BAD_REQUEST', ('batch types', payload, code, result)
                    code, _, result = wire.call(server, path='/v1/sync', device=token,
                                                payload={'models': ['transfer'], 'limit': 1})
                    assert code == 200, ('typed sync succeeds', code, result)
                    print('strict JSON request types: 16 refusals and typed sync success', flush=True)
                # Credential mutations go last so every protected family uses
                # the same valid ephemeral installation credential.
                ordered = [r for r in captured if r['path'] not in ('/v1/devices/rotate','/v1/devices/revoke')]
                ordered += [r for r in captured if r['path'] in ('/v1/devices/rotate','/v1/devices/revoke')]
                for request in ordered:
                    path = request['path']
                    status, headers, body = wire.call(server, path=path, method='POST',
                        device=None if path in ('/v1/devices', '/v1/devices/challenge') else token,
                        body=bytes.fromhex(request['hex']), content_type=request['contentType'],
                        extra=('X-Intelligence-Authorization: Bearer fixture-session',))
                    parsed = json.loads(body) if body.startswith(b'{') else {}
                    if path == '/v1/devices/rotate' and status == 200: token = parsed['deviceToken']
                    assert status != 415 and (status != 404 or path.startswith('/v1/ai/conversations/')), (path, status, body[:300])
                    if path.startswith('/v1/ai/conversations/'):  # unknown synthetic thread is intentional
                        assert status in (401, 403, 404), (path, status, body[:300])
                    rows.append({'path': path, 'status': status, 'error': parsed.get('error'),
                                 'body_sha256': hashlib.sha256(bytes.fromhex(request['hex'])).hexdigest()})
                    print(path, status, parsed.get('error'), flush=True)
            finally:
                proc.terminate()
                try: proc.wait(timeout=10)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    if args.compare:
        previous = {r['path']: r for r in json.loads(args.compare.read_text())['requests']}
        for row in rows:
            old = previous[row['path']]
            assert row['body_sha256'] == old['body_sha256'], ('capture changed', row)
            assert (row['status'] == 400, row['error'] if row['status'] == 400 else None) == \
                   (old['status'] == 400, old['error'] if old['status'] == 400 else None), ('new schema verdict', old, row)
    if args.output: args.output.write_text(json.dumps({'requests': rows}, indent=2)+'\n')
    print(f'app POST wire replay: {len(rows)} captured requests, no new schema refusal')


if __name__ == '__main__': main()
