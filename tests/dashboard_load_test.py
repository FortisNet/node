#!/usr/bin/env python3
"""Local-only exact-release dashboard/API load measurement; never a public probe.

The node has no external network. A separate client shares its loopback namespace.
TLS uses a generated trusted CA; quotas, auth and connection bounds stay enabled.
Timing is recorded as evidence, never asserted as a portable performance promise.
"""
import argparse
import asyncio
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import platform
import ssl
import subprocess
import time
import uuid

from tls_ca import create_certificates

ROOT = Path(__file__).resolve().parents[1]
BUILD_IMAGE = 'fortis-node-native-build:local'


def docker(*args):
    return subprocess.check_output(['docker', *map(str, args)], text=True, timeout=180).strip()


def setup():
    fixtures = Path('/data/fixtures')
    fixtures.mkdir()
    _, cert, key, _ = create_certificates(fixtures, 'DNS:localhost,IP:127.0.0.1')
    generation = Path('/data/certificates/generation-1')
    generation.mkdir(parents=True)
    for profile in ('domain', 'ip'):
        (generation / (profile + '.pem')).write_bytes(cert.read_bytes())
        target = generation / (profile + '.key')
        target.write_bytes(key.read_bytes())
        os.chown(target, 10001, 10001)
        target.chmod(0o400)
    Path('/data/certificates/current').symlink_to('generation-1')
    for name in ('kv', 'acme'):
        directory = Path('/data') / name
        directory.mkdir()
        os.chown(directory, 10001, 10001)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[int((len(ordered) - 1) * fraction)], 3)


async def scenario(context, connections, seconds, workload, reuse, distinct_peers=True, population=0, peer_rate=None):
    # Fixed bounds prevent a fast run or a broken server from growing client RAM.
    statuses = Counter()
    failures = Counter()
    latency = []
    counts = []
    stop = time.monotonic() + seconds

    async def worker(index):
        reader = writer = None
        count = 0
        next_request = time.monotonic()
        try:
            while time.monotonic() < stop:
                if peer_rate:
                    await asyncio.sleep(max(0, next_request - time.monotonic()))
                    if time.monotonic() >= stop:
                        break
                    next_request = time.monotonic() + 1 / peer_rate
                started = time.monotonic()
                path = '/v1/networks'
                if workload == 'dashboard-mix':
                    path = ('/statusz', '/statsz', '/status-history')[count % 10] if count % 10 < 3 else path
                try:
                    if writer is None:
                        reader, writer = await asyncio.wait_for(asyncio.open_connection(
                            '127.0.0.1', 8080, ssl=context, server_hostname='localhost',
                            local_addr=(f'127.0.{population}.{index + 2}' if distinct_peers else f'127.0.{population}.1', 0)), 5)
                    connection = 'keep-alive' if reuse else 'close'
                    writer.write(f'GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: {connection}\r\n\r\n'.encode())
                    await writer.drain()
                    header = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 5)
                    lines = header.decode('latin1').split('\r\n')
                    status = int(lines[0].split()[1])
                    headers = dict(line.lower().split(': ', 1) for line in lines[1:] if ': ' in line)
                    length = int(headers['content-length'])
                    if not 0 <= length <= 2 * 1024 * 1024:
                        raise ValueError('Response outside declared server bound')
                    await asyncio.wait_for(reader.readexactly(length), 5)
                    statuses[f'{path}:{status}'] += 1
                    # 11 is coprime with the 10-request route mix; offsets also
                    # avoid over-sampling simultaneous first TLS handshakes.
                    if count % 11 == index % 11 and len(latency) < connections * 4096:
                        latency.append(1000 * (time.monotonic() - started))
                    count += 1
                    if not reuse or headers.get('connection') != 'keep-alive':
                        writer.close()
                        await writer.wait_closed()
                        reader = writer = None
                except (OSError, ValueError, asyncio.IncompleteReadError, asyncio.LimitOverrunError) as error:
                    failures[type(error).__name__] += 1
                    if writer:
                        writer.close()
                    reader = writer = None
                    # A bounded backoff prevents an unavailable listener spin.
                    await asyncio.sleep(.001)
        finally:
            if writer:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass
            counts.append(count)

    started = time.monotonic()
    await asyncio.gather(*(worker(i) for i in range(connections)))
    elapsed = time.monotonic() - started
    return {'connections': connections, 'workload': workload, 'tlsReuse': reuse,
            'peerAddresses': 'one per client' if distinct_peers else 'shared NAT',
            'population': population, 'targetRequestsPerPeerPerSec': peer_rate,
            'seconds': round(elapsed, 3), 'responses': sum(counts),
            'responsesPerSec': round(sum(counts) / elapsed, 1),
            'latencySampleEvery': 11, 'latencySampleOffset': 'worker index modulo 11',
            'latencySampleLimit': connections * 4096,
            'latencySampleLimitReached': len(latency) == connections * 4096, 'latencySamples': len(latency),
            'p50Ms': percentile(latency, .50), 'p95Ms': percentile(latency, .95),
            'p99Ms': percentile(latency, .99), 'statuses': dict(statuses), 'transportFailures': dict(failures)}


async def drive(seconds):
    context = ssl.create_default_context(cafile='/data/fixtures/ca.pem')
    for _ in range(120):
        try:
            reader, writer = await asyncio.open_connection('127.0.0.1', 8080, ssl=context, server_hostname='localhost')
            writer.close()
            await writer.wait_closed()
            break
        except OSError:
            await asyncio.sleep(.25)
    else:
        raise RuntimeError('Exact release failed verified TLS startup')
    results = []
    # Warm-up is deliberately separate from the published measurements.
    await scenario(context, 1, 1, 'api-refusal', True)
    for population, (reuse, workload, connections) in enumerate((
            (True, 'api-refusal', 1), (True, 'api-refusal', 16), (True, 'api-refusal', 64),
            (True, 'dashboard-mix', 16), (True, 'dashboard-mix', 64), (False, 'api-refusal', 16)), 1):
        results.append(await scenario(context, connections, seconds, workload, reuse, population=population))
    results.append(await scenario(context, 16, seconds, 'api-refusal', True, False, population=7))
    results.append(await scenario(context, 64, seconds, 'api-refusal', True, population=8, peer_rate=50))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--seconds', type=int, choices=range(2, 31), default=5)
    parser.add_argument('--driver', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--setup', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.setup:
        setup()
        return
    if args.driver:
        print(json.dumps(asyncio.run(drive(args.seconds))))
        return
    if args.release is None or args.output is None:
        parser.error('--release and --output are required')
    release = args.release.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    name = 'fortis-dashboard-load-' + uuid.uuid4().hex[:10]
    volume = name + '-data'
    report = {'passed': False, 'releaseSha256': hashlib.sha256((release / 'fortis-node').read_bytes()).hexdigest(),
              'nodeLimits': {'cpuCores': 2, 'memoryBytes': 512 * 1024 * 1024},
              'clientCpuLimit': 2, 'host': platform.platform(), 'volumeRetained': volume,
              'dockerServerArchitecture': docker('info', '--format', '{{.Architecture}}'),
              'clientAndNodeArchitecture': 'linux/amd64',
              'scope': 'Local TLS, read-only dashboard aggregates and unauthenticated Networks refusals; no providers or durable writes'}
    try:
        docker('volume', 'create', volume)
        common = ['--platform', 'linux/amd64', '-v', f'{ROOT}:/input:ro', '-v', f'{volume}:/data']
        docker('run', '--rm', *common, BUILD_IMAGE, 'python3', '/input/tests/dashboard_load_test.py', '--setup')
        docker('run', '-d', '--platform', 'linux/amd64', '--name', name, '--network', 'none',
               '--read-only', '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
               '--cpus', '2', '--memory', '512m', '--pids-limit', '128',
               '-v', f'{release}:/app/current:ro', '-v', f'{volume}:/data',
               '-e', 'NODE_ENV=development', '-e', 'FORTIS_KV_DIR=/data/kv',
               '-e', 'FORTIS_HEALTH_PORT=8081', '-e', 'FORTIS_CA_BUNDLE_PATH=/data/fixtures/ca.pem',
               '-e', 'FORTIS_HTTPS_CERT_DIR=/data/certificates',
               '-e', 'FORTIS_HTTPS_DOMAIN=localhost', '-e', 'FORTIS_HTTPS_IP=127.0.0.1',
               '-e', 'FORTIS_HTTP_REDIRECT_PORT=8082', '-e', 'FORTIS_ACME_CHALLENGE_DIR=/data/acme',
               'fortis-node-native:1')
        raw = docker('run', '--rm', *common, '--network', 'container:' + name, '--cpus', '2',
                     '--memory', '512m', BUILD_IMAGE, 'python3', '/input/tests/dashboard_load_test.py',
                     '--driver', '--seconds', str(args.seconds))
        report['scenarios'] = json.loads(raw)
        for scenario_result in report['scenarios']:
            scenario_result['rateLimitedResponses'] = sum(n for key, n in scenario_result['statuses'].items() if key.endswith(':429'))
            scenario_result['qualifiedAtIllustrative100MsP99'] = not scenario_result['rateLimitedResponses'] and not scenario_result['transportFailures'] and scenario_result['p99Ms'] <= 100
        report['passed'] = all(s['responses'] and not s['transportFailures'] and all(
            key.endswith((':200', ':429')) if not key.startswith('/v1/') else key.endswith((':401', ':429'))
            for key in s['statuses']) for s in report['scenarios'])
        report['containerStats'] = json.loads(docker('stats', '--no-stream', '--format', '{{json .}}', name))
    finally:
        log = subprocess.run(['docker', 'logs', name], capture_output=True, text=True)
        (args.output / 'node.log').write_text(log.stdout + log.stderr)
        subprocess.run(['docker', 'rm', '-f', name], capture_output=True, check=False)
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
