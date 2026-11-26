"""Shared live fixtures for the durable, save-first AI HTTP contract.

The frozen AI fixture predates durable threads and asynchronous
Anthropic replies. It supplies signed-proof identities, not response goldens.
Provider behaviour is covered by ai_anthropic_async_route_test.py.
"""
from contextlib import contextmanager
import json
from pathlib import Path
import tempfile

import ai_conversations_route_test as replay
import ai_open_route_test as op
import discovery_route_harness as harness


@contextmanager
def running(binary):
    fixture = replay.load_fixture(replay.FIXTURE)
    op.DEVICES.update(fixture['devices'])
    with tempfile.TemporaryDirectory(prefix='fortis-ai-current-') as directory:
        root = Path(directory)
        (root / 'kv').mkdir()
        (root / 'tls').mkdir()
        upstream = replay.OpenAi(root / 'tls')
        harness.set_clock(root / 'clock', fixture['now'])
        # No configured provider: saving user turns must work independently.
        server = None
        try:
            server = replay.Server(binary, root, {'env': {'SUPPORT_AUTH_SECRET': fixture['env']['SUPPORT_AUTH_SECRET']}}, upstream)
            yield server, root, upstream, fixture['now']
            assert not upstream.calls(), 'save-only operations must never call the obsolete OpenAI provider'
        finally:
            if server is not None:
                server.stop()
            upstream.close()


def expect(result, status, code=None, charged=True, cache="no-store", content_type=op.JSON_TYPE):
    assert result is not None, 'connection closed without a response'
    got, headers, body = result
    assert got == status, (got, status, body[:300])
    assert headers.get('cache-control') == cache, headers
    assert headers.get('content-length') == str(len(body)), headers
    assert ('x-rate-limit-cost' in headers) == charged, headers
    if charged:
        assert headers['x-rate-limit-cost'] == '1', headers
    if code:
        assert body == json.dumps({'error': code}, separators=(',', ':')).encode(), body
        assert headers['content-type'] == content_type, headers
    return headers, body


def opened(server, now, name='first', **kwargs):
    request = op.make_open(now, name, **kwargs)
    headers, body = expect(op.call(server, request, device=kwargs.get('device', 'A')), 201)
    data = json.loads(body)
    assert set(data) == {'id', 'accessToken', 'message'}, data
    assert data['accessToken'] == request['accessToken']
    assert set(data['message']) == {'id', 'body', 'createdAt'}, data
    assert headers.get('vary') == 'Accept'
    return request, data


def message_path(opened):
    return f"{op.PATH}/{opened['id']}/messages"


def message(now, name, body='Next question'):
    snapshot = op.snapshot_json(now - 60_000)
    return {'clientMessageId': op.sha(name)[:32], 'body': body,
            'snapshotJson': snapshot, 'snapshotHash': op.sha(snapshot)}


def post(server, opened, request, **kwargs):
    return op.call(server, request, path=message_path(opened),
                   extra=(f"X-Intelligence-Authorization: Bearer {opened['accessToken']}",), **kwargs)


def auth_order(binary):
    with running(binary) as (server, root, upstream, now):
        paths = [op.PATH, f'{op.PATH}/{op.ulid(0)}/messages',
                 f'{op.PATH}/{op.ulid(0)}/messages/{op.ulid(1)}/retry']
        for path in paths:
            for content_type, body in [('application/json', b''), ('application/json', b'{'),
                                       ('text/plain', b'private body'), ('application/protobuf', b'\xff')]:
                expect(op.call(server, path=path, device=None, body=body, content_type=content_type),
                       401, 'UNAUTHORIZED', charged=False)
                expect(op.call(server, path=path, device=None, body=body, content_type=content_type,
                               extra=('Origin: https://example.invalid',)),
                       403, 'BROWSER_ORIGIN_FORBIDDEN', charged=False)
        for path in paths[:2]:
            for content_type, body, status, code in [
                ('application/json', b'', 400, 'BAD_REQUEST'),
                ('application/json', b'{', 400, 'BAD_REQUEST'),
                ('text/plain', b'private body', 415, 'UNSUPPORTED_MEDIA_TYPE'),
                ('application/x-www-form-urlencoded', b'a=b', 415, 'UNSUPPORTED_MEDIA_TYPE')]:
                expect(op.call(server, path=path, body=body, content_type=content_type), status, code)
        assert not (root / 'ids').exists(), 'refused framing/auth requests cannot draw a conversation ID'
    print('AI auth-before-body: all three protected writes; framing refusals charged once')
