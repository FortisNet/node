#!/usr/bin/env python3
"""Measure production-source coverage from the three native ingress unit suites.

Requires normal native prerequisites and matching Clang/LLVM tools. Instrumented
objects and profiles live only in build-https-unit-coverage/. The report includes
related pre-existing HTTP files; it is not a project-wide coverage measurement.
Clang branch coverage counts individual short-circuit boolean outcomes.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'build-https-unit-coverage'
SOURCES = ['src/app/https_listener.cpp', 'src/http/http_request.cpp',
           'src/http/http_server.cpp', 'src/app/shutdown_signal.cpp']
SUITES = ['https_listener_unit_test', 'https_tls_unit_test', 'http_transport_unit_test']
FLAGS = ['-O0', '-g', '-fprofile-instr-generate', '-fcoverage-mapping']


def tool(name):
    # Apple's /usr/bin compiler shim supplies the selected SDK's C++ headers.
    if sys.platform == 'darwin' and name != 'clang++':
        return subprocess.check_output(['xcrun', '--find', name], text=True).strip()
    return shutil.which(name + '-18') or shutil.which(name) or name


def recipe(source, target, marker):
    lines = subprocess.check_output(
        ['make', '--no-print-directory', '-n', '-W', source, target], text=True).splitlines()
    return next(shlex.split(line) for line in lines if marker in line)


def main():
    os.chdir(ROOT)
    OUTPUT.mkdir(exist_ok=True)
    for profile in OUTPUT.glob('*.profraw'):
        profile.unlink()
    subprocess.run(['make', '-j4', *['build/' + name for name in SUITES]],
                   check=True, stdout=subprocess.DEVNULL)
    objects = {}
    for source in SOURCES:
        stem = Path(source).stem
        target = 'build/' + stem + '.o'
        command = recipe(source, target, ' -c ' + source + ' ')
        command[0] = tool('clang++')
        command = [str(OUTPUT / Path(arg).name)
                   if arg in (target, 'build/' + stem + '.d') else arg for arg in command]
        subprocess.run(command + FLAGS, check=True, stdout=subprocess.DEVNULL)
        objects[target] = str(OUTPUT / (stem + '.o'))
    for name in SUITES:
        target, source = 'build/' + name, 'tests/' + name + '.cpp'
        command = recipe(source, target, source + ' ')
        command[0] = tool('clang++')
        command = [objects.get(arg, arg) for arg in command]
        command[command.index('-o') + 1] = str(OUTPUT / name)
        subprocess.run(command + FLAGS, check=True, stdout=subprocess.DEVNULL)
        with (OUTPUT / (name + '.log')).open('w') as log:
            subprocess.run([str(OUTPUT / name)], check=True, stdout=log,
                           stderr=subprocess.STDOUT,
                           env={**os.environ, 'LLVM_PROFILE_FILE': str(OUTPUT / (name + '-%p.profraw'))})
    profile = str(OUTPUT / 'units.profdata')
    subprocess.run([tool('llvm-profdata'), 'merge', '-sparse',
                    *[str(path) for path in OUTPUT.glob('*.profraw')], '-o', profile], check=True)
    objects = [str(OUTPUT / name) for name in SUITES]
    common = [objects[0], '-object', objects[1], '-object', objects[2], '-instr-profile=' + profile]
    files = SOURCES + ['src/http/http_transport.h']
    report = subprocess.check_output([tool('llvm-cov'), 'report', *common, *files], text=True)
    print(report)
    (OUTPUT / 'report.txt').write_text(report)
    (OUTPUT / 'summary.json').write_bytes(subprocess.check_output(
        [tool('llvm-cov'), 'export', '-summary-only', *common, *files]))
    (OUTPUT / 'listener.txt').write_bytes(subprocess.check_output(
        [tool('llvm-cov'), 'show', *common, SOURCES[0], '-show-branches=count']))


if __name__ == '__main__':
    main()
