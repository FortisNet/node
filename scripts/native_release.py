#!/usr/bin/env python3
"""Build-time static ELF validation and deterministic release manifest generation."""
import hashlib
import json
import subprocess
import pathlib
import re
import struct
import sys


# ELF machine numbers of the architectures a release is built for.
MACHINES = {'x86_64': 62, 'aarch64': 183}


def validate_elf(binary, architecture='x86_64'):
    if len(binary) < 64 or binary[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('expected a little-endian ELF64 executable')
    if struct.unpack_from('<H', binary, 18)[0] != MACHINES[architecture]:
        raise ValueError('expected ' + architecture)
    if struct.unpack_from('<H', binary, 16)[0] not in (2, 3):
        raise ValueError('expected an executable ELF type')
    offset = struct.unpack_from('<Q', binary, 32)[0]
    size, count = struct.unpack_from('<HH', binary, 54)
    if size < 56 or offset + size * count > len(binary):
        raise ValueError('invalid program header bounds')
    stack_present = False
    for index in range(count):
        header = offset + index * size
        kind = struct.unpack_from('<I', binary, header)[0]
        if kind == 3:
            raise ValueError('ELF requires an external interpreter')
        if kind == 0x6474e551:  # PT_GNU_STACK: musl's default thread stack budget.
            stack_present = True
            flags = struct.unpack_from('<I', binary, header + 4)[0]
            stack_size = struct.unpack_from('<Q', binary, header + 40)[0]
            if flags & 1 or stack_size < 2 * 1024 * 1024:
                raise ValueError('native threads require a non-executable 2 MiB stack budget')
        if kind == 2:
            start = struct.unpack_from('<Q', binary, header + 8)[0]
            length = struct.unpack_from('<Q', binary, header + 32)[0]
            if length % 16 or start + length > len(binary):
                raise ValueError('invalid dynamic table bounds')
            entries = [struct.unpack_from('<qQ', binary, position)
                       for position in range(start, start + length, 16)]
            if not entries or not any(tag == 0 for tag, _ in entries):
                raise ValueError('unterminated dynamic table')
            if any(tag in (1, 15, 29) for tag, _ in entries):
                raise ValueError('ELF requires shared libraries or search paths')

    if not stack_present:
        raise ValueError('native thread stack budget is missing')


def manifest(directory, version, source=None, inputs=None, architecture='x86_64'):
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,64}', version) or version in ('.', '..'):
        raise ValueError('invalid release version')
    binary = (directory / 'fortis-node').read_bytes()
    validate_elf(binary, architecture)
    text = (f'format={2 if source is not None else 1}\nversion={version}\narchitecture={architecture}\n'
            f'runtime=linux-musl-static-v1\nstorage_epoch=1\n'
            f'sha256={hashlib.sha256(binary).hexdigest()}\nbytes={len(binary)}\n')
    if source is not None:
        if source != 'unknown' and not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', source):
            raise ValueError('invalid source commit')
        if not inputs or not re.fullmatch(r'[0-9a-f]{64}', inputs):
            raise ValueError('verified build input digest required')
        text += f'source_commit={source}\nbuild_inputs_sha256={inputs}\n'
    (directory / 'manifest').write_text(text)
    return text


# A passing report from either gate qualifies a release: the full `make verify`
# (seven stages) or `make lean-verify` (one compiler plus lint).
GATES = (('report.json', 7), ('report-lean.json', 2))


def passing_report(root):
    """The first complete, passing report whose inputs match the checkout."""
    stale = None
    for name, stages in GATES:
        path = root / 'build-verify' / name
        if not path.is_file():
            continue
        report = json.loads(path.read_text())
        if not report.get('complete') or not report.get('inputs_unchanged') or \
                len(report.get('stages', {})) != stages or \
                any(s['returncode'] for s in report['stages'].values()):
            continue
        changed = next((input_name for input_name, expected in report['input_sha256'].items()
                        if not (root / input_name).is_file() or
                        hashlib.sha256((root / input_name).read_bytes()).hexdigest() != expected), None)
        if changed is None:
            return report
        stale = stale or changed
    if stale:
        raise ValueError('source changed since verification: ' + stale)
    raise ValueError('a complete passing make verify or make lean-verify report is required before packaging')


def verified_inputs():
    root = pathlib.Path(__file__).resolve().parents[1]
    report = passing_report(root)
    digest = hashlib.sha256(json.dumps(report['input_sha256'], sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    changed = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=all', '--',
        *report['input_sha256']], cwd=root, text=True)
    source = 'unknown' if changed else subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    return source, digest


if __name__ == '__main__':
    source_commit, input_digest = verified_inputs()
    if sys.argv[1:] == ['--check-inputs']:
        print('Verified immutable release inputs: ' + input_digest)
    else:
        manifest(pathlib.Path(sys.argv[1]), sys.argv[2], source_commit, input_digest,
                 sys.argv[3] if len(sys.argv) > 3 else 'x86_64')
