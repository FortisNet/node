#!/usr/bin/env python3
"""Bounded public release metadata, immutable cursors and safe missing/error behavior."""
import json
from pathlib import Path
import sys
import tempfile
import time
from operations_routes_test import start, stop, request, SECURITY


def publish(path, payload):
    temporary = path.with_suffix('.new')
    temporary.write_text(json.dumps(payload))
    temporary.replace(path)
    time.sleep(1.05)


def main():
    with tempfile.TemporaryDirectory(prefix='fortis-release-routes-') as temporary:
        root = Path(temporary)
        public = root / 'public'
        public.mkdir()
        process, port = start(sys.argv[1], root / 'kv', {'FORTIS_RELEASE_STATUS_DIR': str(public)})
        try:
            status, headers, page = request(port, '/releases')
            assert status == 200 and b'Versions &amp; deployments.' in page
            assert b'textContent' in page and b'No confirmed end' in page
            assert b'href="/assets/fortis-favicon.svg"' in page
            for key, value in SECURITY.items():
                assert headers[key] == value
            assert request(port, '/releases', method='HEAD')[2] == b''
            assert request(port, '/releases', method='POST')[0] == 404
            status, headers, body = request(port, '/releasesz')
            assert status == 200 and not json.loads(body)['managed']
            assert headers['cache-control'] == 'no-store'
            assert request(port, '/releasesz/history?page=0')[0] == 404
            current = {'schemaVersion': 1, 'managed': True, 'observedAtMs': 1, 'latestPage': 0,
                       'current': {'version': '<img src=x onerror=alert(1)>', 'sha256': 'a' * 64}, 'candidate': None}
            publish(public / 'current.json', current)
            status, headers, body = request(port, '/releasesz')
            assert status == 200 and json.loads(body) == current
            assert headers['content-length'] == str(len(body))
            assert request(port, '/releasesz', method='HEAD')[1]['content-length'] == str(len(body))
            history = {'schemaVersion': 1, 'page': 0, 'previousPage': None, 'records': []}
            publish(public / 'history-0.json', history)
            assert json.loads(request(port, '/releasesz/history?page=0')[2]) == history
            for query in ['page=-1', 'page=18446744073709551616', 'page=../../etc/passwd', 'page=%00', 'page=%xx', 'page=']:
                assert request(port, '/releasesz/history?' + query)[0] == 400, query
            assert request(port, '/releasesz/history')[0] == 400
            assert request(port, '/releasesz?page=1')[0] == 400
            (public / 'current.json').write_text('{broken')
            time.sleep(1.05)
            assert request(port, '/releasesz')[0] == 503
            assert str(public).encode() not in request(port, '/releasesz')[2]
            (public / 'current.json').unlink()
            (public / 'current.json').symlink_to('/etc/passwd')
            time.sleep(1.05)
            assert request(port, '/releasesz')[0] == 503
            (public / 'current.json').unlink()
            (public / 'current.json').write_bytes(b' ' * 65536)
            time.sleep(1.05)
            assert request(port, '/releasesz')[0] == 503
            (public / 'current.json').unlink()
            (public / 'current.json').mkdir()
            time.sleep(1.05)
            assert request(port, '/releasesz')[0] == 503
            # The contract every node release keeps with the host orchestrator:
            # its pages are handed out unchanged at /, /health and /health.json.
            status, headers, built_in = request(port, '/')
            assert status == 200 and b'FortisNet' in built_in  # unmanaged: the node's own page
            status, _, body = request(port, '/health')
            assert status == 503 and json.loads(body) == {'error': 'health_report_unavailable'}
            assert request(port, '/health.json')[0] == 503
            status, _, body = request(port, '/node.json')
            assert status == 404 and json.loads(body) == {'error': 'node_report_unavailable'}
            now = lambda: int(time.time() * 1000)
            first_page = b'<!DOCTYPE html><title>the host page</title>' + b'x' * 300000
            (public / 'index.html').write_bytes(first_page)
            (public / 'health.html').write_bytes(b'<!DOCTYPE html><title>health</title>')
            (public / 'health.json').write_bytes(b'{"schemaVersion":1,"status":200}\n')
            (public / 'health.status').write_text(f'200 {now()}\n')
            status, headers, body = request(port, '/')
            assert status == 200 and body == first_page
            assert headers['content-type'] == 'text/html; charset=utf-8' and headers['cache-control'] == 'no-store'
            for key, value in SECURITY.items():
                assert headers[key] == value
            assert request(port, '/', method='HEAD')[2] == b''
            status, headers, body = request(port, '/health')
            assert status == 200 and body == b'<!DOCTYPE html><title>health</title>'
            assert headers['cache-control'] == 'no-store'
            status, headers, body = request(port, '/health.json')
            assert status == 200 and body == b'{"schemaVersion":1,"status":200}\n'
            assert headers['content-type'] == 'application/json; charset=utf-8'
            # The host's report about the node is handed out as written.
            node_report = b'{"schemaVersion":1,"running":{"version":"one"},"update":{"state":"idle"}}\n'
            (public / 'node.json').write_bytes(node_report)
            status, headers, body = request(port, '/node.json')
            assert status == 200 and body == node_report
            assert headers['content-type'] == 'application/json; charset=utf-8' and headers['cache-control'] == 'no-store'
            assert request(port, '/node.json', method='HEAD')[2] == b''
            # The status the report states, and no belief in a report nobody refreshes.
            (public / 'health.status').write_text(f'503 {now()}\n')
            assert request(port, '/health')[0] == 503 and request(port, '/health.json')[0] == 503
            assert request(port, '/health.json')[2] == b'{"schemaVersion":1,"status":200}\n'
            for stale in (f'200 {now() - 120000}\n', f'200 {now() + 3600000}\n', '', '200\n', 'garbage\n',
                          '200 soon\n', f'200 {now()}'):
                (public / 'health.status').write_text(stale)
                assert request(port, '/health')[0] == 503, repr(stale)
            # The existing health check is untouched by any of this.
            assert request(port, '/healthz')[0] == 200
            # Unusable files fall back instead of being served or followed.
            (public / 'index.html').unlink()
            (public / 'index.html').symlink_to('/etc/passwd')
            assert request(port, '/')[2] == built_in
            (public / 'index.html').unlink()
            (public / 'index.html').write_bytes(b'x' * (512 * 1024 + 1))
            assert request(port, '/')[2] == built_in
            (public / 'index.html').unlink()
            (public / 'index.html').mkdir()
            assert request(port, '/')[2] == built_in
            (public / 'health.html').unlink()
            (public / 'health.html').symlink_to('/etc/passwd')
            assert request(port, '/health')[0] == 503 and b'root:' not in request(port, '/health')[2]
        finally:
            stop(process)
    print('Release routes: missing/stale metadata, HEAD, MIME/security, bounds, symlinks, corruption and the host page contract passed')


if __name__ == '__main__':
    main()
