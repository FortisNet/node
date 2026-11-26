#!/usr/bin/env python3
"""Bounded authenticated diagnostic reports, public redaction and restart durability."""
import json
from pathlib import Path
import sys
import tempfile
import operations_routes_test as harness


def main(binary):
    with tempfile.TemporaryDirectory(prefix='fortis-errors-route-') as directory:
        root = Path(directory) / 'kv'
        process, port = harness.start(binary, root, {'FORTIS_ENABLE_TEST_OPERATIONS': '1'})
        try:
            path = '/v1/prices/missing-identifiers'
            valid = json.dumps({'symbols': ['FLR', 'HYPE', 'PRIVATE123']}).encode()
            headers = {'Authorization': f'Bearer {harness.TOKEN}', 'Content-Type': 'application/json'}
            assert harness.request(port, path, 'POST', body=valid)[0] == 401
            assert harness.request(port, path, 'POST', headers | {'Origin': 'https://browser.test'}, valid)[0] == 403
            assert harness.request(port, path, 'POST', {'Authorization': headers['Authorization']}, valid)[0] == 415
            for body in ({'symbols': []}, {'symbols': ['FLR', 'private/value']},
                         {'symbols': ['FLR'], 'extra': True}, {'symbols': ['FLR'] * 17}):
                assert harness.request(port, path, 'POST', headers, json.dumps(body).encode())[0] == 400
            assert harness.request(port, path, 'POST', headers, valid)[0] == 204
            status, _, body = harness.request(port, '/errorsz')
            assert status == 200
            board = json.loads(body)['operational']
            assert board['persisted'] and not board['writeFailed']
            rows = board['records']
            assert {row['identifier'] for row in rows} == {'FLR', 'HYPE', ''}, rows
            assert all(row['code'] == 'client_identifier_missing' and row['count'] == 1 and
                       row['origin'] == 'unclassified' and row['provider'] == '' and row['httpStatus'] is None for row in rows)
            assert b'PRIVATE123' not in body and harness.TOKEN.encode() not in body
            assert harness.request(port, '/error')[2] == harness.request(port, '/errors')[2]
        finally:
            harness.stop(process)
        process, port = harness.start(binary, root)
        try:
            board = json.loads(harness.request(port, '/errorsz')[2])['operational']
            assert board['persisted'] and {row['identifier'] for row in board['records']} == {'FLR', 'HYPE', ''}
        finally:
            harness.stop(process)
    print('error_board_route_test: authentication, bounds, privacy and restart passed')


if __name__ == '__main__':
    main(sys.argv[1])
