#!/usr/bin/env python3
"""Validate compiler mechanisms in disposable children, never by corrupting the node."""
import importlib.util
import json
from pathlib import Path
import resource
import platform
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hardening', ROOT / 'scripts/native_hardening.py')
hardening = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardening)


def main():
    output = Path(sys.argv[1])
    flags = hardening.profile()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='fortis-hardening-probe-') as name:
        root = Path(name)
        binary = root / 'probe'
        subprocess.run(['gcc', '-Os', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', *flags['c'], *flags['link'], str(ROOT / 'tests/native_hardening_probe.c'), '-o', str(binary)], check=True)
        disassembly = subprocess.check_output(['objdump', '-d', '--disassemble=clash_probe', str(binary)], text=True)
        # A probe touches the stack as it grows, so a large frame cannot step
        # over the guard page: on x86_64 an "or" into (%rsp) every 0x1000
        # bytes, on aarch64 a store of xzr at [sp, #1024] after each step.
        arm = platform.machine() in ('aarch64', 'arm64')
        probes = 'xzr, [sp, #1024]' in disassembly if arm else '0x1000' in disassembly and '(%rsp)' in disassembly
        if not probes:
            raise RuntimeError('stack-clash page probes missing')
        guards = subprocess.check_output(['objdump', '-d', '--disassemble=stack_probe', str(binary)], text=True)
        if '__stack_chk_fail' not in guards:
            raise RuntimeError('stack probe has no emitted canary failure path')
        # The pinned musl canary failure traps with SIGSEGV. fortify-headers
        # uses the compiler's trap: an illegal instruction on x86_64, a
        # breakpoint instruction on aarch64. A generic crash must not pass fortify.
        trap = -signal.SIGTRAP if arm else -signal.SIGILL
        cases = [('stack', '1', ord('x')), ('fortify', 'ok', ord('o')),
                 ('stack', '48', -signal.SIGSEGV), ('fortify', 'x' * 128, trap)]
        results = []
        for mode, value, expected in cases:
            result = subprocess.run([str(binary), mode, value], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            if result.returncode != expected:
                raise RuntimeError('hardening probe outcome mismatch: ' + mode + ' exit=' + str(result.returncode))
            results.append({'mode': mode, 'expected_exit': expected, 'exit': result.returncode})
        # Negative control: the same overflow without fortify reaches an ordinary
        # memory fault, which the qualified fortify case above must reject.
        control = root / 'unfortified'
        subprocess.run(['gcc', '-Os', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        *flags['c'], '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', *flags['link'],
                        str(ROOT / 'tests/native_hardening_probe.c'), '-o', str(control)], check=True)
        unfortified = subprocess.run([str(control), 'fortify', 'x' * 128], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=5)
        # On x86_64 the overflow runs over the return address and faults. On
        # aarch64 the return address is stored below the buffer, so with this
        # pinned toolchain the overflow goes unnoticed and the probe returns
        # normally: either way, not the trap that fortify raises.
        unnoticed = ord('x') if arm else -signal.SIGSEGV
        if unfortified.returncode != unnoticed:
            raise RuntimeError('unfortified negative control did not reach the expected outcome')
        results.append({'mode': 'unfortified negative control', 'expected_exit': unnoticed,
                        'exit': unfortified.returncode})
        source = root / 'assertions.cpp'
        source.write_text('#include <vector>\n#include <cstdlib>\nint main(int argc,char** argv){std::vector<int> v(1);return v[argc>1?std::strtoul(argv[1],nullptr,10):0];}\n')
        subprocess.run(['g++', '-Os', '-std=c++23', '-Wall', '-Wextra', '-Wpedantic', '-Werror', *flags['cxx'], *flags['link'], str(source), '-o', str(root / 'assertions')], check=True)
        result = subprocess.run([str(root / 'assertions'), '9'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        if result.returncode != -signal.SIGABRT:
            raise RuntimeError('libstdc++ assertions did not reject bounds violation')
        results.append({'mode': 'libstdc++ assertions', 'expected_exit': -signal.SIGABRT, 'exit': result.returncode})
    (output / 'hardening-probes.json').write_text(json.dumps({'passed': True, 'cases': results, 'stack_clash_page_probe': True, 'stack_guard_failure_path_present': True}, indent=2) + '\n')


if __name__ == '__main__':
    main()
