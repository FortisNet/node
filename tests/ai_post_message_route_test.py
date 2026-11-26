#!/usr/bin/env python3
"""Current save-first message writes: validation, idempotency, redaction, quota."""
import json
import sys
import threading

import ai_open_route_test as op
from ai_current_contract import running, expect, opened, message, message_path, post


def main(binary):
    with running(binary) as (server, root, upstream, now):
        request, thread = opened(server, now)
        path = message_path(thread)
        for value in ({}, {'clientMessageId': 4, 'body': 'hi'}, message(now, 'blank', ' '),
                      message(now, 'long', 'x' * 8001)):
            expect(post(server, thread, value), 400, 'BAD_REQUEST')
        expect(op.call(server, message(now, 'no-chat'), path=path), 404, 'NOT_FOUND')
        expect(op.call(server, message(now, 'wrong-chat'), path=path,
                       extra=('X-Intelligence-Authorization: Bearer fintel_chat_' + 'Z' * 43,)),
               404, 'NOT_FOUND')
        value = message(now, 'repeat')
        _, payload = expect(post(server, thread, value), 201)
        saved = json.loads(payload)
        assert set(saved) == {'message'} and saved['message']['body'] == value['body']
        assert saved['message']['portfolioSnapshot']['wallets']
        for _ in range(2):
            _, repeated = expect(post(server, thread, value), 201)
            assert repeated == payload, 'same client message ID must replay exact bytes'
        results = [None, None]
        value = message(now, 'parallel')
        def send(index):
            results[index] = post(server, thread, value)
        workers = [threading.Thread(target=send, args=(i,)) for i in range(2)]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join()
        for result in results:
            expect(result, 201)
        assert results[0][2] == results[1][2]
        rows = op.history(server, thread['id'], thread['accessToken'])
        assert len(rows) == 3 and sum(row[2] == 'Next question' for row in rows) == 2
        value = message(now, 'proto', 'Protobuf question')
        wire = b''.join(bytes([i << 3 | 2]) + op.varint(len(value[key].encode())) + value[key].encode()
                        for i, key in enumerate(('clientMessageId', 'body', 'snapshotJson', 'snapshotHash'), 1))
        _, payload = expect(post(server, thread, None, body=wire, content_type='application/protobuf',
                                 accept='application/protobuf'), 201)
        fields = op.wire_fields(payload)
        assert set(fields) == {1}, 'no synchronous assistant in protobuf ACK'
        assert op.wire_fields(fields[1])[3].decode() == value['body']
        value = message(now, 'redact', 'my phrase is abandon ability able about above absent absorb abstract '
                        'absurd abuse access accident')
        _, payload = expect(post(server, thread, value), 201)
        assert b'abandon ability' not in payload and b'redacted' in payload
        value = message(now, 'bad-snapshot')
        value['snapshotHash'] = '0' * 64
        expect(post(server, thread, value), 400, 'SNAPSHOT_INVALID')
        # Parsing/schema still follows device authentication but precedes chat
        # authorization for framing, so malformed JSON returns a charged 400.
        expect(post(server, thread, None, body=b'{'), 400, 'BAD_REQUEST')
    with running(binary) as (server, root, upstream, now):
        _, thread = opened(server, now, 'post-quota')
        value = message(now, 'quota-repeat')
        for _ in range(120):
            expect(post(server, thread, value), 201)
        headers, _ = expect(post(server, thread, value), 429, 'RATE_LIMITED')
        assert headers['retry-after'] == '3600'
        assert len(op.history(server, thread['id'], thread['accessToken'])) == 2
    print('AI message writes: validation, both bearers, concurrent idempotency, protobuf, redaction and quota passed')


if __name__ == '__main__':
    main(sys.argv[1])
