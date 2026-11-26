#!/usr/bin/env python3
"""Reject missing/overridden compiler protections and malformed hardening ELF metadata."""
import copy
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hardening', ROOT / 'scripts/native_hardening.py')
hardening = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardening)


def elf():
    data = bytearray(1024)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHIQQ', data, 16, 3, 62, 1, 512, 64)
    struct.pack_into('<HHH', data, 52, 64, 56, 5)
    def header(i, kind, flags, offset, address, size, memory):
        struct.pack_into('<IIQQQQQQ', data, 64 + i * 56, kind, flags, offset, address, address, size, memory, 1)
    header(0, 1, 5, 0, 0, 600, 600)
    header(1, 1, 6, 600, 600, 424, 424)
    header(2, 0x6474e551, 6, 0, 0, 0, 2097152)
    header(3, 0x6474e552, 4, 600, 600, 100, 100)
    header(4, 2, 6, 720, 720, 32, 32)
    struct.pack_into('<qQqQ', data, 720, 30, 8, 0, 0)
    return data


class HardeningTest(unittest.TestCase):
    def test_exact_elf_properties(self):
        self.assertTrue(hardening.validate_elf(elf())['static_pie'])
        arm = elf(); struct.pack_into('<H', arm, 18, 183)
        self.assertTrue(hardening.validate_elf(arm, 'aarch64')['static_pie'])
        with self.assertRaises(ValueError):
            hardening.validate_elf(elf(), 'aarch64')
        mutations = [(16, '<H', 2), (18, '<H', 183), (32, '<Q', 1000),
                     (64 + 4, '<I', 7), (64 + 2 * 56 + 4, '<I', 7),
                     (64 + 3 * 56, '<I', 1), (720, '<q', 1), (728, '<Q', 4),
                     (64 + 4 * 56 + 32, '<Q', 33), (64 + 56 + 40, '<Q', (1 << 64) - 1)]
        for offset, fmt, value in mutations:
            data = elf(); struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                hardening.validate_elf(data)
        for length in (0, 63, 64, 200, 1023):
            with self.subTest(truncated=length), self.assertRaises(ValueError):
                hardening.validate_elf(elf()[:length])

    def test_audited_compile_coverage_and_overrides(self):
        flags = hardening.profile()
        paths = ['/work/src/app/main.cpp', '/work/.deps/mbedtls-3.6.7/library/ssl.c', '/work/third_party/nanopb/pb.c']
        records = [{'returncode': 0, 'arguments': ['cc', '-Os', '-c', *flags['cxx' if p.endswith('.cpp') else 'c']], 'sources': {p: 'hash'}} for p in paths]
        records += [{'returncode': 0, 'arguments': ['c++', *flags['cxx'], *flags['link'], '-o', 'build/fortis_node'], 'sources': {}}]
        expected = {paths[0]}
        self.assertEqual(hardening.validate_commands(records, expected)['production_sources'], 1)
        for change in ['missing', 'override-stack', 'override-clash', 'override-fortify', 'split-override', 'response-file', 'override-pie', 'override-assertions', 'missing-tu', 'missing-link', 'failed']:
            bad = copy.deepcopy(records)
            if change == 'missing': bad[0]['arguments'].remove('-fstack-protector-strong')
            elif change == 'override-stack': bad[0]['arguments'].append('-fno-stack-protector')
            elif change == 'override-clash': bad[0]['arguments'].append('-fno-stack-clash-protection')
            elif change == 'override-fortify': bad[0]['arguments'].append('-U_FORTIFY_SOURCE')
            elif change == 'split-override': bad[0]['arguments'] += ['-U', '_FORTIFY_SOURCE']
            elif change == 'response-file': bad[0]['arguments'].append('@hidden-flags')
            elif change == 'override-pie': bad[0]['arguments'].append('-fno-PIE')
            elif change == 'override-assertions': bad[0]['arguments'].append('-U_GLIBCXX_ASSERTIONS')
            elif change == 'missing-tu': bad.pop(0)
            elif change == 'missing-link': bad.pop()
            elif change == 'failed': bad[0]['returncode'] = 1
            with self.subTest(change=change), self.assertRaises(ValueError):
                hardening.validate_commands(bad, expected)


if __name__ == '__main__':
    unittest.main()
