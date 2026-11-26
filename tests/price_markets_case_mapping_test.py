#!/usr/bin/env python3
"""Isolate the js-case Unicode-cache miss from unrelated replay lane pressure."""
import sys
import tempfile
from urllib.parse import parse_qs, urlsplit

import discovery_post_edges as edges
import discovery_route_harness as harness
import price_markets_edges_test as markets


def main(binary):
    fixture = edges.load_fixture(markets.FIXTURE)
    cases = {c['name']: c for c in fixture['cases'] if c['group'] == 'main'}
    with tempfile.TemporaryDirectory(prefix='fortis-market-case-') as directory:
        h = markets.Harness(binary, directory, fixture)
        server = h.server('case', {}, cases['unicode']['at'])
        try:
            # Two distinct spellings: the first has ASCII-preserved capital
            # dotted I; the second has ASCII i + combining dot. Neither is a
            # real CoinGecko ID, but the markets fixture intentionally uses it.
            for name, spelling in [('unicode', 'İstanbul'), ('unicode-protobuf', 'i\u0307stanbul')]:
                case = cases[name]
                harness.set_clock(h.clock, case['at'])
                h.lanes.reserve([{'url': 'https://api.coingecko.com/api/v3/coins/markets'}])
                before = len(h.upstream.calls())
                status, headers, body = edges.raw(server.port, case['method'], case['target'],
                                             markets.request_headers(case), markets.body_of(case))
                observed = edges.settle(h.upstream, before, 1)
                assert status == 200 and len(observed) == 1, (name, status, observed, body[:200])
                ids = parse_qs(urlsplit(observed[0][0]).query)['ids'][0].split(',')
                assert spelling in ids, (name, ids)
                expected = markets.ascii_identifier_case(h, case)
                assert edges.compare(name, expected, status, headers, body)
                assert markets.compare_upstream(h, name, expected, observed)
                assert edges.compare_state(name, server, expected['kvWrites'])
                if name == 'unicode':
                    assert 'i\u0307stanbul' not in ids
            # The validated loopback composition uses the same four-token
            # lane as the replay model. Reserve the next token without an
            # ad hoc sleep or changes to fixture time.
            case = cases['escaping-ids']
            assert markets.replay_case(h, server, case), 'escaping IDs changed even with a full provider budget'
            assert not edges.FAILURES, edges.FAILURES
        finally:
            server.stop()
            h.upstream.close()
    print('market case mapping: distinct Unicode cache keys cause one read each; escaped IDs match with correctly paced lane admission')


if __name__ == '__main__':
    main(sys.argv[1])
