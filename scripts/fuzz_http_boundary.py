#!/usr/bin/env python3
"""Run the parser's shared libFuzzer entry point under ASan/UBSan, locally only."""
import argparse
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=60)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error('seconds must be 1..3600')
    output = ROOT / 'build-fuzz-http'
    corpus = output / 'corpus'
    corpus.mkdir(parents=True, exist_ok=True)
    for index, request in enumerate((b'GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n',
                                     b'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 524288\r\n\r\n',
                                     b'GET /%70robe%2fasync HTTP/1.1\r\nHost: localhost\r\n\r\n')):
        (corpus / f'seed-{index}').write_bytes(request)
    image = 'fortis-node-sanitizers:' + hashlib.sha256((ROOT / 'Dockerfile.sanitizers').read_bytes()).hexdigest()[:12]
    with (output / 'fuzzer.log').open('w') as log:
        # Use the same pinned sanitizer toolchain as scripts/sanitize.py.
        command = ['docker', 'run', '--rm', '--init', '-v', f'{ROOT}:/work', '-w', '/work', image]
        subprocess.run(command + ['clang++-18', '-std=c++23', '-fno-rtti', '-pthread', '-g', '-O1',
                                   '-DFORTIS_LIBFUZZER', '-fsanitize=fuzzer,address,undefined',
                                   '-fno-sanitize=vptr', '-fno-sanitize-recover=all',
                                   '-Isrc/http', '-Isrc/base', 'tests/http_boundary_fuzz_test.cpp',
                                   'src/http/http_request.cpp', '-o', 'build-fuzz-http/parser'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(command + ['build-fuzz-http/parser', 'build-fuzz-http/corpus',
                                   f'-max_total_time={args.seconds}', '-max_len=16385', '-timeout=5',
                                   '-rss_limit_mb=512', '-artifact_prefix=build-fuzz-http/'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    print(f'Parser libFuzzer/ASan/UBSan passed; evidence: {output / "fuzzer.log"}')


if __name__ == '__main__':
    main()
