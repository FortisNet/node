#!/usr/bin/env python3
"""Explicit cache warmup is separate from measured coverage and fails closed."""
from types import SimpleNamespace
from concurrent.futures import ThreadPoolExecutor
import unittest
import ssl
from unittest.mock import patch
import portfolio_load_test as load


class Clock:
    now = 0
    def monotonic(self): return self.now
    def sleep(self, duration): self.now += duration


class Client:
    def __init__(self, replies):
        self.replies = {route: list(rows) for route, rows in replies.items()}
    def measure(self, route):
        rows = self.replies[route]
        sample = rows.pop(0) if len(rows) > 1 else rows[0]
        return sample, None


def sample(ok):
    return load.Sample(200, 5, ids=frozenset({'bitcoin'}) if ok else frozenset(),
                       expected_ids=frozenset({'bitcoin'}), result='ok' if ok else 'incomplete')


class WarmupTests(unittest.TestCase):
    def warm(self, client, *, required=True):
        clock = Clock()
        args = SimpleNamespace(mode='mixed', require_snapshot=required, warmup_seconds=2)
        with patch.object(load.time, 'monotonic', clock.monotonic), patch.object(load.time, 'sleep', clock.sleep):
            return load.warm_prices(args, client)

    def test_retries_incomplete_coverage_and_reports_warmup_separately(self):
        report = self.warm(Client({'snapshot': [sample(True)], 'spot': [sample(False), sample(True)]}))
        self.assertTrue(report['passed'])
        self.assertEqual(report['routes']['spot']['outcome'], {'incomplete': 1, 'ok': 1})
        self.assertEqual(report['routes']['spot']['unavailable_by_id'], {'bitcoin': 1})
        self.assertEqual(report['elapsed_s'], 1)

    def test_persistent_incomplete_coverage_cannot_pass(self):
        report = self.warm(Client({'snapshot': [sample(True)], 'spot': [sample(False)]}))
        self.assertFalse(report['passed'])
        self.assertEqual(report['routes']['spot']['outcome'], {'incomplete': 2})

    def test_optional_snapshot_does_not_mask_spot_failure(self):
        self.assertTrue(self.warm(Client({'snapshot': [sample(False)], 'spot': [sample(True)]}), required=False)['passed'])
        self.assertFalse(self.warm(Client({'snapshot': [sample(True)], 'spot': [sample(False)]}), required=False)['passed'])


class EnrollmentTests(unittest.TestCase):
    def enroll(self, replies):
        args = SimpleNamespace(users=5, base_url='http://fixture', context=None, timeout=1)
        with ThreadPoolExecutor(max_workers=2) as pool, \
                patch.object(load, 'enroll_user', side_effect=replies), \
                patch.object(load.time, 'sleep') as sleep:
            clients, samples = load.enroll_clients(args, pool)
        return clients, samples, sleep.call_count

    def test_retries_only_rate_limited_setup_and_keeps_every_attempt(self):
        accepted = (object(), load.Sample(201, 1, result='ok'))
        limited = (None, load.Sample(429, 1, result='rate_limited'))
        clients, samples, waits = self.enroll([limited] * 2 + [accepted] * 5)
        self.assertEqual((len(clients), len(samples), waits), (5, 7, 1))
        self.assertEqual(sum(sample.status == 429 for sample in samples), 2)

    def test_non_quota_failure_is_not_retried_or_hidden(self):
        accepted = (object(), load.Sample(201, 1, result='ok'))
        failed = (None, load.Sample(503, 1, result='enrollment_error'))
        clients, samples, waits = self.enroll([failed] + [accepted] * 4)
        self.assertEqual((len(clients), len(samples), waits), (4, 5, 0))

    def test_permanent_quota_refusal_has_a_finite_setup_budget(self):
        clients, samples, waits = self.enroll(lambda *args: (None, load.Sample(429, 1, result='rate_limited')))
        self.assertEqual((len(clients), len(samples), waits), (0, 15, 2))


class SustainedTests(unittest.TestCase):
    def run_fixture(self, fail_sustained=False):
        args = SimpleNamespace(users=1, enroll_workers=1, warmup=False, mode='mixed', base_url='http://fixture',
                               rounds=2, interval=30, sustained_rounds=3, sustained_interval=2,
                               require_snapshot=True, max_core_p95_ms=500, core_deadline_ms=None)
        client = SimpleNamespace(request=lambda *args, **kwargs: (204, None))
        row = {'users_with_all_core_quotes': 1, 'users_with_all_legacy_contract_quotes': 1,
               'routes': {'spot': {'outcome': {'ok': 1}}, 'snapshot': {'outcome': {'ok': 1}}},
               'all_six_core_quotes_ms': {'p95': 1}}
        rows = [row] * 5
        if fail_sustained:
            rows[3] = {**row, 'users_with_all_core_quotes': 0}
        clock = Clock()
        with patch.object(load, 'enroll_clients', return_value=([client], [])), \
                patch.object(load, 'round_load', side_effect=rows) as measure, \
                patch.object(load.time, 'sleep', clock.sleep), \
                patch.object(load.time, 'monotonic', clock.monotonic):
            result = load.run(args)
        self.assertEqual(measure.call_count, 5)
        self.assertEqual((len(result['rounds']), len(result['sustained_rounds'])), (2, 3))
        self.assertEqual(result['revocation_status'], {'204': 1})
        self.assertEqual(clock.now, 36)
        return result

    def test_baseline_and_sustained_results_remain_separate(self):
        self.assertTrue(self.run_fixture()['passed'])

    def test_sustained_failure_fails_run_and_devices_are_revoked(self):
        self.assertFalse(self.run_fixture(fail_sustained=True)['passed'])


class TlsContextTests(unittest.TestCase):
    def test_connection_reuses_verified_context_without_loading_default_roots(self):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        client = load.Client('https://localhost', context, 1)
        handler = load.TimedHTTPSHandler(context, client)

        def construct(connection_type, request, **options):
            self.assertIs(options.get('context'), context)
            return connection_type('localhost', **options)

        with patch.object(handler, 'do_open', side_effect=construct), \
                patch.object(ssl, '_create_default_https_context', side_effect=AssertionError('unused root-store load')):
            connection = handler.https_open(load.Request('https://localhost/healthz'))
        self.assertIs(connection._context, context)
        connection.close()


if __name__ == '__main__':
    unittest.main()
