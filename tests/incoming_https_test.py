#!/usr/bin/env python3
"""Direct ingress contracts; generated local CA only, never a public enrollment.

Also accepts --requests for reproducible persistent-connection latency samples.
Private fixture keys/stores are deleted on exit. Result JSON contains aggregates.
"""
import argparse
import concurrent.futures
from contextlib import closing
from email.utils import parsedate_to_datetime
import http.client
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import ssl
import statistics
import sys
import subprocess
import tempfile
import time
from tls_ca import create_certificates, free_port
import public_price_device_probe as device_probe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--handoff-binary')
    parser.add_argument('--wallet-packages', type=Path, help='optional wallet package_config.json for its real Dio/protobuf client')
    parser.add_argument('--burst', type=int, default=32)
    parser.add_argument('--dart', help='optional installed Dart SDK for wallet HTTP/1.1 transport check')
    parser.add_argument('--requests', type=int, default=100)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='fortis-incoming-tls-') as temporary:
        root = Path(temporary)
        ca, certificate, key, _ = create_certificates(root, 'DNS:localhost')
        certificates = root / 'certificates'
        generation = certificates / 'generation-1'
        generation.mkdir(parents=True)
        for name in ('domain', 'ip'):
            shutil.copyfile(certificate, generation / f'{name}.pem')
            shutil.copyfile(key, generation / f'{name}.key')
            (generation / f'{name}.key').chmod(0o600)
        # Real deployments may use RSA for the domain and ECDSA for the IP.
        subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout',
                        '-out', str(root / 'ip.key')], check=True, capture_output=True)
        subprocess.run(['openssl', 'req', '-new', '-key', str(root / 'ip.key'), '-subj', '/CN=127.0.0.1',
                        '-out', str(root / 'ip.csr')], check=True, capture_output=True)
        (root / 'ip.ext').write_text('basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:127.0.0.1\n')
        subprocess.run(['openssl', 'x509', '-req', '-in', str(root / 'ip.csr'), '-CA', str(ca),
                        '-CAkey', str(root / 'ca.key'), '-CAserial', str(root / 'ca.srl'), '-days', '2',
                        '-extfile', str(root / 'ip.ext'), '-out', str(generation / 'ip.pem')],
                       check=True, capture_output=True)
        shutil.copyfile(root / 'ip.key', generation / 'ip.key')
        (generation / 'ip.key').chmod(0o600)
        (certificates / 'current').symlink_to(generation.name)
        challenges = root / 'challenges'
        challenges.mkdir()
        (challenges / 'safe_token-1').write_text('challenge-value')
        (challenges / 'symlink').symlink_to(certificate)
        (root / 'kv').mkdir()
        port, redirect, health = free_port(), free_port(), free_port()
        env = dict(os.environ, FORTIS_HTTPS_CERT_DIR=str(certificates), FORTIS_HTTPS_DOMAIN='localhost',
                   FORTIS_HTTPS_IP='127.0.0.1', FORTIS_CA_BUNDLE_PATH=str(ca),
                   FORTIS_HTTP_REDIRECT_PORT=str(redirect), FORTIS_HEALTH_PORT=str(health),
                   FORTIS_ACME_CHALLENGE_DIR=str(challenges), FORTIS_KV_DIR=str(root / 'kv'),
                   FORTIS_ENABLE_DIAGNOSTIC_PROBES='1', FORTIS_NATIVE_PRICE_ROUTE_CANARY='1')
        context = ssl.create_default_context(cafile=str(ca))
        context.set_alpn_protocols(['h2', 'http/1.1'])
        log = (root / 'server.log').open('w+')
        server = subprocess.Popen([binary, str(port)], env=env, stdout=log, stderr=log)
        slow = []

        def connection():
            return http.client.HTTPSConnection('localhost', port, context=context, timeout=8)

        def get(path='/healthz'):
            with closing(connection()) as client:
                client.request('GET', path)
                response = client.getresponse()
                body = response.read()
                dates = [value for key, value in response.getheaders() if key.lower() == 'date']
                assert len(dates) == 1 and abs(parsedate_to_datetime(dates[0]).timestamp() - time.time()) < 10, dates
                return response.status, body

        def raw(wire, source=None):
            plain = socket.create_connection(('127.0.0.1', port), timeout=8,
                                             source_address=(source, 0) if source else None)
            with context.wrap_socket(plain, server_hostname='localhost') as client:
                client.sendall(wire)
                return client.recv(65536)

        try:
            for _ in range(200):
                if server.poll() is not None:
                    raise AssertionError('ingress startup failed')
                try:
                    status, body = get()
                    if status == 200:
                        break
                    raise AssertionError((status, body))
                except (OSError, http.client.HTTPException) as error:
                    if _ == 199:
                        raise AssertionError(repr(error)) from error
                    time.sleep(0.03)
            else:
                raise AssertionError('ingress did not start')
            checks = []
            assert get('/v1/prices?ids=bitcoin')[0] == 401
            checks.append('unauthenticated protobuf-price refusal')
            tunnel_target = f'https://localhost:{free_port()}'
            enrollment = device_probe.enrollment_tunnel_opener(tunnel_target, context, port)
            token = device_probe.enroll(enrollment, tunnel_target)
            from urllib.request import build_opener, HTTPSHandler, ProxyHandler
            direct = build_opener(ProxyHandler({}), HTTPSHandler(context=context), device_probe.NoRedirect())
            assert device_probe.call(direct, 'POST', f'https://localhost:{port}/v1/devices/revoke', token=token)[0] == 204
            checks.append('verified operator enrollment tunnel; direct credential revocation')

            with closing(connection()) as client:
                for _ in range(3):
                    client.request('GET', '/healthz')
                    response = client.getresponse()
                    assert response.status == 200
                    assert response.getheader('Connection') == 'keep-alive'
                    response.read()
                    assert client.sock.selected_alpn_protocol() == 'http/1.1'
                fd = client.sock.fileno()
                client.request('HEAD', '/healthz', headers={'Connection': 'keep-alive'})
                response = client.getresponse()
                assert response.status == 200 and response.read() == b''
                assert client.sock.fileno() == fd
            checks.append('ALPN, keep-alive and HEAD')
            # IP verification, with no DNS SNI, selects the default IP certificate.
            with context.wrap_socket(socket.create_connection(('127.0.0.1', port)), server_hostname='127.0.0.1') as client:
                client.sendall(b'GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n')
                assert b'200 OK' in client.recv(65536)
            checks.append('IP certificate coverage')
            for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                fixed = ssl.create_default_context(cafile=str(ca))
                fixed.minimum_version = fixed.maximum_version = version
                with fixed.wrap_socket(socket.create_connection(('127.0.0.1', port)), server_hostname='localhost') as client:
                    client.sendall(b'GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                    assert b'200 OK' in client.recv(65536)
            checks.append('TLS 1.2 and TLS 1.3; RSA domain and ECDSA IP certificates')

            for path in ('/probe', '/probe/representation', '/%70robe', '/probe%2fasync',
                         '//probe', '/./probe', '/x/../probe', '/%2570robe', '/probe%00', '/probe%5casync'):
                assert get(path)[0] in (400, 404), path
            checks.append('diagnostic exclusion including encoded paths')
            cases = [
                (b'Content-Length: 0\r\nContent-Length: 0', 400),
                (b'Transfer-Encoding: chunked', 400),
                (b'Content-Length: 524289', 413),
                (b'Content-Length: 2', 405),
                (b'Expect: 100-continue', 417),
            ]
            for header, status in cases:
                reply = raw(b'GET /healthz HTTP/1.1\r\nHost: localhost\r\n' + header + b'\r\n\r\n')
                assert f' {status} '.encode() in reply, (header, reply)
            assert b' 421 ' in raw(b'GET / HTTP/1.1\r\nHost: attacker.example\r\n\r\n')
            assert b' 431 ' in raw(b'GET / HTTP/1.1\r\nHost: localhost\r\nPadding: ' + b'x' * 16384)
            checks.append('framing, header/body limits, Host and Expect policy')
            wire = b'GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n'
            with context.wrap_socket(socket.create_connection(('127.0.0.1', port)), server_hostname='localhost') as client:
                client.sendall(wire + wire.replace(b'\r\n\r\n', b'\r\nConnection: close\r\n\r\n'))
                replies = b''
                while True:
                    part = client.recv(65536)
                    if not part:
                        break
                    replies += part
                assert replies.count(b'HTTP/1.1 200 OK') == 2
            checks.append('pipelined bytes retain framing and response order')
            for path, status, body in [('/.well-known/acme-challenge/safe_token-1', 200, b'challenge-value'),
                                        ('/.well-known/acme-challenge/symlink', 404, b''),
                                        ('/.well-known/acme-challenge/../domain.key', 404, b''),
                                        ('/healthz?x=1', 308, b'')]:
                with closing(http.client.HTTPConnection('localhost', redirect, timeout=4)) as client:
                    client.request('GET', path)
                    response = client.getresponse()
                    assert response.status == status, (path, response.status)
                    assert response.read() == body
                    if status == 308:
                        assert response.getheader('Location') == 'https://localhost/healthz?x=1'
            checks.append('redirects and ACME traversal/symlink refusal')
            # Uploads occupy ingress only, not route workers or health capacity.
            upload_count = 32 if sys.platform.startswith('linux') else 8
            for index in range(upload_count):
                source = f'127.0.0.{2 + index // 8}' if sys.platform.startswith('linux') else '127.0.0.1'
                plain = socket.create_connection(('127.0.0.1', port), source_address=(source, 0), timeout=4)
                client = context.wrap_socket(plain, server_hostname='localhost')
                client.sendall(b'POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Length: 524288\r\n\r\nx')
                slow.append(client)
            start = time.monotonic()
            assert get()[0] == 200
            assert time.monotonic() - start < 2
            checks.append(f'health responsive with {upload_count} incomplete uploads')
            for client in slow:
                client.close()
            slow.clear()
            # Slow ClientHellos cannot consume route workers; admission stays bounded.
            for index in range(24):
                client = socket.create_connection(('127.0.0.1', port), source_address=(f'127.0.0.{20 + index}' if sys.platform.startswith('linux') else '127.0.0.1', 0), timeout=4)
                client.sendall(b'\x16\x03')
                slow.append(client)
            assert get()[0] == 200
            for client in slow:
                client.close()
            slow.clear()
            checks.append('health responsive with incomplete TLS handshakes')
            # Invalid rotation retains active configuration and existing sessions.
            bad = certificates / 'generation-bad'
            shutil.copytree(generation, bad)
            (bad / 'domain.key').write_text('invalid key')
            selector = certificates / 'next'
            selector.symlink_to(bad.name)
            os.replace(selector, certificates / 'current')
            server.send_signal(signal.SIGHUP)
            time.sleep(0.2)
            assert get()[0] == 200
            selector.symlink_to(generation.name)
            os.replace(selector, certificates / 'current')
            server.send_signal(signal.SIGHUP)
            time.sleep(0.2)
            assert get()[0] == 200
            log.flush()
            log.seek(0)
            content = log.read()
            assert 'Certificate reload refused' in content and 'Certificate generation activated' in content
            checks.append('invalid reload retained; valid reload activated')
            if args.dart:
                subprocess.run([args.dart, str(Path(__file__).with_name('wallet_http1_client.dart')),
                                f'https://localhost:{port}', str(ca)], check=True, timeout=60)
                checks.append('wallet Dart HttpClient TLS and persistent HTTP/1.1 transport')
                if args.wallet_packages:
                    subprocess.run([args.dart, '--packages=' + str(args.wallet_packages),
                                    str(Path(__file__).with_name('wallet_dio_https_client.dart')),
                                    f'https://localhost:{port}', str(ca)], check=True, timeout=60)
                    checks.append('wallet installed Dio and generated protobuf enrollment/auth/revocation')
            samples = []
            with closing(connection()) as client:
                for _ in range(args.requests):
                    start = time.perf_counter()
                    client.request('GET', '/healthz')
                    response = client.getresponse()
                    assert response.status == 200
                    response.read()
                    samples.append((time.perf_counter() - start) * 1000)
            # Concurrent application requests exercise transport handoff/closure.
            with concurrent.futures.ThreadPoolExecutor(max_workers=min(200, args.burst)) as pool:
                assert all(status == 401 for status, _ in pool.map(get, ['/v1/prices?ids=bitcoin'] * args.burst))
            checks.append('concurrent application responses')
            if args.handoff_binary:
                fixture_port = free_port()
                fixture_env = dict(env, FORTIS_HTTP_REDIRECT_PORT=str(free_port()))
                fixture = subprocess.Popen([str(Path(args.handoff_binary).resolve()), str(fixture_port)],
                                           env=fixture_env, stdout=log, stderr=log)
                original_port = port
                port = fixture_port
                try:
                    for _ in range(100):
                        try:
                            if get()[0] == 200:
                                break
                        except OSError:
                            time.sleep(0.02)
                    else:
                        raise AssertionError('handoff fixture did not start')
                    with closing(connection()) as client:
                        for path, status, body in [('/deferred', 200, b'deferred'),
                                                   ('/reserved', 200, b'reserved'),
                                                   ('/timeout', 504, b'{"error":"TIMEOUT"}'),
                                                   ('/reserved', 200, b'reserved')]:
                            client.request('GET', path)
                            response = client.getresponse()
                            assert response.status == status, (path, response.status)
                            assert response.read() == body
                    # Abandoned reservations and shutdown exercise transferred TLS ownership.
                    abandoned = context.wrap_socket(socket.create_connection(('127.0.0.1', port)), server_hostname='localhost')
                    abandoned.sendall(b'GET /timeout HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    abandoned.close()
                    checks.append('TLS deferred/reserved replies, timeout, reuse and disconnect')
                finally:
                    fixture.terminate()
                    fixture.wait(timeout=15)
                    port = original_port
                assert fixture.returncode == 0
            result = {'checks': checks, 'requests': len(samples), 'median_ms': statistics.median(samples),
                      'p95_ms': sorted(samples)[int(len(samples) * 0.95)], 'maximum_ms': max(samples)}
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result, indent=2))
        except BaseException:
            server.terminate()
            server.wait(timeout=30)
            log.flush()
            log.seek(0)
            print(log.read()[-8000:])
            raise
        finally:
            for client in slow:
                client.close()
            server.terminate()
            try:
                server.wait(timeout=30)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            log.close()
        assert server.returncode == 0, server.returncode


if __name__ == '__main__':
    main()
