#!/usr/bin/env python3
"""Static ELF release admission, including static PIE without shared dependencies."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('native_release', Path(__file__).resolve().parents[1] / 'scripts/native_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def elf(kind=1, tags=()):
    data = bytearray(320)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HH', data, 16, 3, 62)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 2)
    struct.pack_into('<I', data, 64, kind)
    struct.pack_into('<Q', data, 72, 192)
    struct.pack_into('<Q', data, 96, len(tags) * 16)
    for i, tag in enumerate(tags):
        struct.pack_into('<qQ', data, 192 + i * 16, tag, 0)
    struct.pack_into('<II', data, 120, 0x6474e551, 6)
    struct.pack_into('<Q', data, 160, 2 * 1024 * 1024)
    return data


class NativeReleaseTest(unittest.TestCase):
    def test_static_and_static_pie(self):
        release.validate_elf(elf())
        release.validate_elf(elf(2, (7, 8, 0)))

    def test_interpreter_or_shared_library_refused(self):
        for data in [elf(3), elf(2, (1, 0)), elf(2, (15, 0)), elf(2, (29, 0))]:
            with self.assertRaises(ValueError):
                release.validate_elf(data)

    def test_bad_headers_or_architecture_refused(self):
        wrong_arch = elf()
        struct.pack_into('<H', wrong_arch, 18, 183)
        wrong_type = elf()
        struct.pack_into('<H', wrong_type, 16, 1)
        wrong_bounds = elf()
        struct.pack_into('<Q', wrong_bounds, 32, 250)
        for data in [b'not-an-elf', wrong_arch, wrong_type, wrong_bounds,
                     elf(2, (7,)), elf(2, ())]:
            with self.assertRaises(ValueError):
                release.validate_elf(data)

    def test_native_stack_budget_and_nx_required(self):
        for mode in ['missing', 'small', 'executable']:
            data = elf()
            if mode == 'missing':
                struct.pack_into('<I', data, 120, 1)
            elif mode == 'small':
                struct.pack_into('<Q', data, 160, 128 * 1024)
            else:
                struct.pack_into('<I', data, 124, 7)
            with self.assertRaises(ValueError):
                release.validate_elf(data)

    def test_manifest_and_version_validation(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / 'fortis-node').write_bytes(elf())
            text = release.manifest(directory, '1.2.3')
            self.assertIn('runtime=linux-musl-static-v1', text)
            self.assertIn('storage_epoch=1', text)
            self.assertEqual(len(text.splitlines()), 7)
            self.assertIn('architecture=x86_64', text)
            # The manifest names the architecture the program is really built for.
            with self.assertRaises(ValueError):
                release.manifest(directory, '1.2.3', architecture='aarch64')
            arm = elf(); struct.pack_into('<H', arm, 18, 183)
            (directory / 'fortis-node').write_bytes(arm)
            self.assertIn('architecture=aarch64', release.manifest(directory, '1.2.3', architecture='aarch64'))
            with self.assertRaises(ValueError):
                release.manifest(directory, '1.2.3')
            (directory / 'fortis-node').write_bytes(elf())
            for version in ['../bad', '..', '.', 'x\nformat=1', 'x' * 65]:
                with self.assertRaises(ValueError):
                    release.manifest(directory, version)

    def test_signed_source_provenance_format(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'fortis-node').write_bytes(elf())
            text = release.manifest(directory, 'new', 'a'*40, 'b'*64)
            self.assertTrue(text.startswith('format=2\n'))
            self.assertIn('source_commit=' + 'a'*40, text)
            self.assertIn('build_inputs_sha256=' + 'b'*64, text)
            with self.assertRaises(ValueError):
                release.manifest(directory, 'new', 'not-a-commit', 'b'*64)


if __name__ == '__main__':
    unittest.main()
