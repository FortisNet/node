#!/usr/bin/env python3
"""Exercise the network catalog generator's escaping and capacity checks.

Covers the C++ string escaping, every field-size limit at and just past its
bound, and the prediction of the native network-list response size. Nothing
is read from or written to the repository.
"""
import copy
import importlib.util
import re
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'network_catalog_generator',
    Path(__file__).resolve().parents[1] / 'scripts/generate_network_catalog.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

capabilities = ['balances', 'transfers', 'nfts', 'insight', 'txStatus',
                'txDetail', 'token', 'trustlines', 'news', 'popularTokens']
seed = {
    'id': 'sample', 'family': 'evm', 'evmChainId': 1, 'testnet': False,
    'rpcUrls': ['https://rpc.example'], 'graphqlUrls': [],
    'native': {'symbol': 'ETH', 'decimals': 18, 'coingeckoId': 'ethereum'},
}
clone = copy.deepcopy


def equal(actual, expected):
    if actual != expected:
        raise AssertionError(f'expected {expected!r}, got {actual!r}')


def throws(action, pattern):
    try:
        action()
    except Exception as error:
        if not re.search(pattern, str(error)):
            raise AssertionError(f'error {str(error)!r} does not match {pattern!r}')
        return
    raise AssertionError(f'expected an error matching {pattern!r}')


def rejects(mutate, pattern):
    value = clone(seed)
    mutate(value)
    throws(lambda: generator.validate_catalog([value], capabilities), pattern)


def change(*path_and_value):
    """A mutation that sets one (possibly nested) field of a seed."""
    *path, last, new_value = path_and_value

    def mutate(value):
        for name in path:
            value = value[name]
        value[last] = new_value
    return mutate


equal(generator.cpp_string('q"\\\n\té'), '"q\\"\\\\\\012\\011\\303\\251"')
escaped = clone(seed)
escaped['id'] = 'line\n"quote'
rendered = generator.render_catalog([escaped], capabilities)
equal(re.search(r'"line\\012\\"quote"', rendered) is not None, True)
equal(re.search(r'// line', rendered) is None, True)

max_seed = clone(seed)
max_seed['id'] = 'i' * 64
max_seed['family'] = 'f' * 23
max_seed['rpcUrls'] = ['u' * 255, 'a', 'b', 'c', 'd']
max_seed['graphqlUrls'] = ['g' * 255, 'h', 'i']
max_seed['native']['symbol'] = 's' * 8
max_seed['native']['coingeckoId'] = 'c' * 64
equal(generator.validate_catalog([max_seed], capabilities), True)

rejects(change('id', 'i' * 65), r'network id')
rejects(change('family', 'f' * 24), r'family')
rejects(change('rpcUrls', ['https://rpc.example'] * 6), r'rpcUrls')
rejects(change('rpcUrls', ['u' * 256]), r'rpcUrls\[0\]')
rejects(change('graphqlUrls', ['https://g.example'] * 4), r'graphqlUrls')
rejects(change('native', 'symbol', 's' * 9), r'native.symbol')
rejects(change('native', 'coingeckoId', 'c' * 65), r'native.coingeckoId')
throws(lambda: generator.validate_catalog([seed] * 44, capabilities), r'network count')
throws(lambda: generator.validate_catalog([seed, clone(seed)], capabilities),
       r'duplicate network')
throws(lambda: generator.validate_catalog([seed], capabilities[1:]), r'exactly ten')
utf8 = clone(seed)
utf8['id'] = 'é' * 33
throws(lambda: generator.validate_catalog([utf8], capabilities), r'network id')
oversized = [{**clone(max_seed), 'id': f'oversized-{index}',
              'rpcUrls': ['\n' * 255] * 5,
              'graphqlUrls': ['\n' * 255] * 3} for index in range(43)]
throws(lambda: generator.validate_catalog(oversized, capabilities),
       r'native network JSON exceeds')
control_key = list(capabilities)
control_key[0] = 'a\n'
plain_key = list(capabilities)
plain_key[0] = 'aX'
equal(generator.native_json_length([seed], control_key) -
      generator.native_json_length([seed], plain_key), 5)

print('network catalog generator escaping, byte lengths, and native response bounds passed')
