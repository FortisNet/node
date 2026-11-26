#!/usr/bin/env python3
"""Compile-negative lifetime contracts under both strict compiler families."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = '#include "file_descriptor.h"\n#include "ownership.h"\n#include "status.h"\n#include "fixed_string.h"\n#include "threading.h"\nusing namespace fortis::base;\n'
CASES = [
    ('int main(){auto b=Buffer::Allocate(8);auto s=b.bytes();s[0]=1;Result<int> r(3);return r.value();}', None),
    ('int main(){auto s=Buffer::Allocate(8).bytes();}', 'deleted'),
    ('int main(){int fd=FileDescriptor(9).get();}', 'deleted'),
    ('int main(){auto p=Buffer::Allocate(8).data();}', 'deleted'),
    ('int main(){auto s=FixedString<8>{}.view();}', 'deleted'),
    ('int main(){auto s=FixedString<8>{}.data();}', 'deleted'),
    ('int main(){auto s=FixedString<8>{}.c_str();}', 'deleted'),
    ('int main(){auto&& v=Result<int>(1).value();}', 'deleted'),
    ('struct X{~X()noexcept(false){}};int main(){Result<X> r{X{}};}', 'Result cleanup must not throw'),
    ('struct F{F()=default;F(F&&)noexcept(false){};void operator()(){}};int main(){ThreadHandle h;F f;h.Start(std::move(f));}', 'thread capture must not throw'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', action='append')
    args = parser.parse_args()
    for compiler in args.compiler or ['clang++-18', 'g++-13']:
        with tempfile.TemporaryDirectory(prefix='fortis-compile-contracts-') as temporary:
            source = Path(temporary) / 'contract.cpp'
            for body, error in CASES:
                source.write_text(PREFIX + body + '\n')
                result = subprocess.run([*shlex.split(compiler), '-std=c++23', '-fno-rtti', '-Wall', '-Wextra', '-Werror',
                                         '-fsyntax-only', '-I' + str(ROOT / 'src/base'), str(source)], capture_output=True, text=True)
                if error is None:
                    if result.returncode:
                        raise RuntimeError(result.stderr)
                elif result.returncode == 0 or error.lower() not in result.stderr.lower():
                    raise RuntimeError('negative contract failed for ' + compiler + ': ' + body + '\n' + result.stderr)
    print('Lifetime/generic-type positive and negative compile contracts passed.')


if __name__ == '__main__':
    main()
