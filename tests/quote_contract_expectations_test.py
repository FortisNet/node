"""The migration adapter changes exact contract URLs, never provider payloads."""
import copy
import unittest
from quote_contract_expectations import observation_url, retarget


class ContractExpectationsTests(unittest.TestCase):
    def test_adds_observation_without_reencoding_existing_query(self):
        url = 'https://api.coingecko.com/api/v3/simple/token_price/ethereum?contract_addresses=0xAA%2C0xBB&vs_currencies=usd'
        self.assertEqual(observation_url(url), url + '&include_last_updated_at=true')
        self.assertEqual(observation_url(observation_url(url)), observation_url(url))

    def test_keeps_explicit_flags_and_non_contract_routes(self):
        for url in ('/api/v3/simple/price?ids=ethereum', '/v1/prices?ids=ethereum',
                    '/api/v3/simple/token_price/ethereum?include_last_updated_at=false'):
            self.assertEqual(observation_url(url), url)

    def test_preserves_payload_and_identities(self):
        source = {'providerRequests': [{'url': '/api/v3/simple/token_price/ethereum?vs_currencies=usd',
                                       'body': {'price': 1, 'url': '/api/v3/simple/token_price/private-payload'}, 'sourceId': 'ethereum'}],
                  'cases': [{'upstream': [{'path': '/api/v3/simple/token_price/ethereum'}]}]}
        original = copy.deepcopy(source)
        retarget(source)
        self.assertEqual(source['providerRequests'][0]['body'], original['providerRequests'][0]['body'])
        self.assertEqual(source['providerRequests'][0]['sourceId'], 'ethereum')
        self.assertEqual(source['cases'][0]['upstream'][0]['path'],
                         '/api/v3/simple/token_price/ethereum?include_last_updated_at=true')


if __name__ == '__main__':
    unittest.main()
