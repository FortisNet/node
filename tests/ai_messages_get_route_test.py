#!/usr/bin/env python3
"""Durable history: auth, paging, JSON/protobuf, HEAD and malformed paths."""
import json
import sys

import ai_open_route_test as op
from ai_current_contract import running, expect, opened, message, message_path, post


def main(binary):
    with running(binary) as (server, root, upstream, now):
        _, thread = opened(server, now)
        for i in range(23):
            expect(post(server, thread, message(now, f'page-{i}', f'question {i}')), 201)
        path = message_path(thread)
        chat = (f"X-Intelligence-Authorization: Bearer {thread['accessToken']}",)
        def get(target=path, **kwargs):
            return op.call(server, path=target, method='GET', extra=chat, **kwargs)
        _, payload = expect(get(), 200, cache="private, no-store")
        all_rows = json.loads(payload)['messages']
        assert len(all_rows) == 24
        headers, payload = expect(get(path + '?limit=20'), 200, cache="private, no-store")
        latest = json.loads(payload)['messages']
        cursor = headers['x-next-cursor']
        assert len(latest) == 20 and cursor == latest[0]['id']
        headers, payload = expect(get(path + '?limit=20&before=' + cursor), 200, cache="private, no-store")
        earlier = json.loads(payload)['messages']
        assert earlier + latest == all_rows and 'x-next-cursor' not in headers
        for query in ('?limit=0', '?limit=101', '?before=invalid'):
            expect(get(path + query), 400, 'BAD_REQUEST')
        headers, payload = expect(get(accept='application/protobuf'), 200, cache="private, no-store")
        assert headers['content-type'] == 'application/protobuf'
        # Repeated message submessages (tag 1), independently counted.
        count, offset = 0, 0
        while offset < len(payload):
            assert payload[offset] == 10
            offset += 1
            size, shift = 0, 0
            while True:
                byte = payload[offset]
                offset += 1
                size |= (byte & 127) << shift
                shift += 7
                if not byte & 128:
                    break
            fields = op.wire_fields(payload[offset:offset + size])
            assert fields[2] == b'user'
            assert fields[1].decode() == all_rows[count]['id']
            assert fields[3].decode() == all_rows[count]['body']
            assert fields[4].decode() == all_rows[count]['createdAt']
            offset += size
            count += 1
        assert count == len(all_rows)
        status, headers, body = op.call(server, path=path, method='HEAD', extra=chat)
        assert status == 200 and not body and int(headers['content-length']) > 0
        expect(op.call(server, path=path, method='GET'), 404, 'NOT_FOUND')
        expect(op.call(server, path=path, method='GET', device=None), 401, 'UNAUTHORIZED', charged=False)
        # Credential ambiguity is rejected by the HTTP parser before hooks.
        expect(op.call(server, path=path, method='GET', extra=chat + chat), 400, 'BAD_REQUEST', charged=False, content_type='application/json')
        for segment in ('', '%00', '%1F', 'a' * 513):
            result = get(f'{op.PATH}/{segment}/messages')
            status, headers, body = result
            assert status == 404 and body == b'{"error":"NOT_FOUND"}'
            assert 'x-rate-limit-cost' not in headers
        expect(get(f'{op.PATH}/' + 'a' * 101 + '/messages'), 414, 'URI_TOO_LONG', charged=False)
    print('AI history: paging, both representations, HEAD, auth and router refusals passed')


if __name__ == '__main__':
    main(sys.argv[1])
