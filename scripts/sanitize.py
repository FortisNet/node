#!/usr/bin/env python3
"""Sanitizer builds and test runs (make asan / ubsan / tsan, make sanitize-test,
make tsan-test, make sanitize-replay).

  scripts/sanitize.py VARIANT build [make targets...]
  scripts/sanitize.py VARIANT unit [REGEX]
  scripts/sanitize.py VARIANT replay REGEX
  scripts/sanitize.py VARIANT run COMMAND...

VARIANT   asan  AddressSanitizer + UndefinedBehaviorSanitizer (+ LeakSanitizer
                on Linux)
          ubsan UndefinedBehaviorSanitizer alone
          tsan  ThreadSanitizer

build     the server and every unit-test binary of `make test` (or the named
          targets).
unit      builds, then runs the `./build/...` commands of the `make test`
          recipe, those matching REGEX if one is given.
replay    builds the server binaries the matching `python3 tests/...` recipe
          commands name, then runs those commands against them. REGEX is
          required: the whole replay suite takes hours without a sanitizer.
run       runs one command in the variant's tree with the sanitizer options
          set (e.g. `run ./build/kv_store_test`).

A variant is built out of tree: build-<variant>/ is a directory of symlinks to
the project's top-level entries with its own build/ and .deps/ (a separately
built Mbed TLS), and the unchanged Makefile runs inside it with the variant's
compilers and flags. build/, .deps/ and the flags stamp of the normal build
are never touched, and the recipes' `build/...` paths mean the variant's
binaries.

Where it runs (FORTIS_SANITIZER_RUNTIME = auto | host | docker; default auto):
on Linux the host toolchain is used (CXX/CC, default clang++/clang). On macOS
the run moves into the Dockerfile.sanitizers container: Apple's ASan runtime
deadlocks while it initializes and its TSan runtime crashes there on this OS
release (see docs/sanitizers.md), and macOS has no LeakSanitizer. The image is
built on first use.

A command fails when it exits non-zero or when any process it started wrote a
sanitizer report (reports go to build-<variant>/sanitizer-logs/). The exit
status is non-zero if any command failed.
"""
import hashlib
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
# Tagged with a checksum of the Dockerfile: editing it builds a new image.
IMAGE = "fortis-node-sanitizers:" + hashlib.sha256(
    (PROJECT / "Dockerfile.sanitizers").read_bytes()).hexdigest()[:12]
SUPPRESSIONS = "tests/sanitizers"

# Every variant keeps the normal build's warnings, -fno-rtti (the guest has no
# RTTI) and -ffp-contract=off. UBSan's vptr check needs RTTI, so it is off.
# -fno-sanitize-recover makes every UBSan finding fatal.
# tests/sanitizers/thread_stack.h enlarges the explicitly sized thread stacks.
COMMON_CXX = ("-std=c++23 -fno-rtti -Wall -Wextra -Werror -pedantic -ffp-contract=off "
              "-include tests/sanitizers/thread_stack.h")
COMMON_C = "-std=c11 -Wall -Wextra -Werror -pedantic"
DEBUG = "-O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls"
UBSAN = "-fsanitize=undefined -fno-sanitize=vptr -fno-sanitize-recover=all"
VARIANTS = {
    "asan": f"{DEBUG} -fsanitize=address {UBSAN}",
    "ubsan": f"{DEBUG} {UBSAN}",
    "tsan": f"{DEBUG} -fsanitize=thread",
}
# Not linked into the variant: local output and the other build trees.
NOT_LINKED = {"build", ".deps", ".unikraft", ".kraftkit", "dist", "data", "__pycache__"}


def die(message: str) -> "NoReturn":
    print(f"sanitize: {message}", file=sys.stderr)
    sys.exit(2)


def in_docker(argv: list[str]) -> int:
    if shutil.which("docker") is None:
        die("docker is needed for the sanitizer builds on this host (or set FORTIS_SANITIZER_RUNTIME=host)")
    if subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True).returncode != 0:
        print(f"sanitize: building {IMAGE} from Dockerfile.sanitizers", flush=True)
        with open(PROJECT / "Dockerfile.sanitizers", "rb") as dockerfile:
            built = subprocess.run(["docker", "build", "-q", "-t", IMAGE, "-"], stdin=dockerfile)
        if built.returncode != 0:
            die("could not build the sanitizer image")
    command = ["docker", "run", "--rm", "--init", "-v", f"{PROJECT}:/work", "-w", "/work",
               # LeakSanitizer stops the world with ptrace; ThreadSanitizer
               # re-executes itself with ASLR disabled (personality(2)).
               "--cap-add", "SYS_PTRACE", "--security-opt", "seccomp=unconfined",
               # /tmp stays on the container's disk: the KV kill-point tests
               # need fsync to take real time. A test that needs it to be
               # cheap instead reads FORTIS_TEST_TMPDIR (see sanitizer_environment).
               "--shm-size", "2g",
               "-e", "FORTIS_SANITIZER_RUNTIME=host", "-e", "CXX=clang++-18", "-e", "CC=clang-18",
               # libc++ as on macOS and in the guest, linked statically: Ubuntu's
               # libc++abi.so binds its own operator delete internally, so ASan
               # sees `free` release what `operator new` allocated whenever the
               # runtime destroys an exception (a false alloc-dealloc-mismatch).
               "-e", "FORTIS_SANITIZER_STDLIB=-stdlib=libc++ -static-libstdc++ -Wno-unused-command-line-argument"]
    for name in ("FORTIS_SANITIZER_JOBS", "FORTIS_SANITIZER_TIMEOUT"):
        if name in os.environ:
            command += ["-e", f"{name}={os.environ[name]}"]
    if platform.system() == "Linux":
        command += ["--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp"]
    if sys.stdin.isatty() and sys.stdout.isatty():
        command.append("-t")
    command += [IMAGE, "python3", "scripts/sanitize.py", *argv]
    return subprocess.run(command).returncode


def prepare_tree(variant: str) -> Path:
    tree = PROJECT / f"build-{variant}"
    tree.mkdir(exist_ok=True)
    # Objects of another platform (a host run after a container run) cannot be
    # reused; the flags stamp only knows the compiler's name.
    marker = tree / ".platform"
    here = f"{platform.system()}-{platform.machine()}"
    if marker.exists() and marker.read_text() != here:
        for stale in ("build", ".deps/mbedtls-3.6.7"):
            shutil.rmtree(tree / stale, ignore_errors=True)
    marker.write_text(here)
    wanted = {entry.name for entry in PROJECT.iterdir()
              if entry.name not in NOT_LINKED and not entry.name.startswith(("build-", ".config."))}
    for entry in tree.iterdir():
        if entry.is_symlink() and entry.name not in wanted:
            entry.unlink()
    for name in wanted:
        link = tree / name
        if not link.is_symlink() and not link.exists():
            link.symlink_to(Path("..") / name)
    (tree / ".deps").mkdir(exist_ok=True)
    archive = PROJECT / ".deps" / "mbedtls-3.6.7.tar.bz2"
    if archive.is_file() and not (tree / ".deps" / archive.name).exists():
        shutil.copyfile(archive, tree / ".deps" / archive.name)
    (tree / "sanitizer-logs").mkdir(exist_ok=True)
    return tree


def make_command(variant: str) -> list[str]:
    stdlib = os.environ.get("FORTIS_SANITIZER_STDLIB", "")
    cxx = os.environ.get("CXX") or "clang++"
    cc = os.environ.get("CC") or "clang"
    flags = VARIANTS[variant]
    jobs = os.environ.get("FORTIS_SANITIZER_JOBS") or str(os.cpu_count() or 4)
    # The generated-catalog check (node) guards the source tree and belongs to
    # the normal build; a variant compiles whatever the tree holds.
    return ["make", f"-j{jobs}", "-o", "check-network-catalog", f"CXX={cxx}", f"CC={cc}",
            f"CXXFLAGS={flags} {COMMON_CXX} {stdlib}".strip(), f"CFLAGS={flags} {COMMON_C}"]


def recipe(tree: Path, variant: str) -> list[str]:
    """The commands of the `make test` recipe, as the Makefile prints them."""
    out = subprocess.run(make_command(variant) + ["-n", "test"], cwd=tree, capture_output=True, text=True)
    lines = [line.strip() for line in out.stdout.splitlines()]
    return [line for line in lines if line.startswith(("./build/", "python3 tests/"))]


def binaries(commands: list[str]) -> list[str]:
    found: list[str] = []
    for command in commands:
        for word in shlex.split(command):
            word = word[2:] if word.startswith("./") else word
            if re.fullmatch(r"build/[A-Za-z0-9_]+", word) and word not in found:
                found.append(word)
    return found


def build(tree: Path, variant: str, targets: list[str]) -> None:
    make = make_command(variant)
    # Mbed TLS first and alone: its recipe runs a nested make.
    for step in (["build/fortis_node"], targets):
        if step and subprocess.run(make + ["-k", *step], cwd=tree).returncode != 0:
            die(f"the {variant} build failed")


def sanitizer_environment(tree: Path, variant: str) -> dict[str, str]:
    logs = tree / "sanitizer-logs"
    supp = tree / SUPPRESSIONS
    linux = platform.system() == "Linux"
    env = dict(os.environ)
    # A memory-backed directory for the tests whose time bounds assume a cheap
    # fsync (macOS's is; a container disk's costs milliseconds).
    if linux and os.path.isdir("/dev/shm"):
        env.setdefault("FORTIS_TEST_TMPDIR", "/dev/shm")
    # One log prefix per variant: the runtimes share their common flags, and
    # under TSan the UBSAN_OPTIONS log_path would otherwise name TSan reports.
    log = f"log_path={logs}/{variant}"
    env["ASAN_OPTIONS"] = ":".join([
        log, "halt_on_error=1", "strict_string_checks=1", "detect_stack_use_after_return=1",
        "check_initialization_order=1", "strict_init_order=1", f"detect_leaks={1 if linux else 0}"])
    env["LSAN_OPTIONS"] = f"suppressions={supp}/lsan.supp:print_suppressions=0"
    env["UBSAN_OPTIONS"] = f"{log}:print_stacktrace=1:halt_on_error=1:suppressions={supp}/ubsan.supp"
    env["TSAN_OPTIONS"] = (f"{log}:suppressions={supp}/tsan.supp:print_suppressions=0:"
                           "second_deadlock_stack=1:history_size=4")
    return env


def skipped(tree: Path, commands: list[str]) -> list[str]:
    """Commands tests/sanitizers/skip.txt excludes (one regex per line)."""
    patterns = [line.strip() for line in (tree / SUPPRESSIONS / "skip.txt").read_text().splitlines()
                if line.strip() and not line.startswith("#")]
    return [command for command in commands if any(re.search(pattern, command) for pattern in patterns)]


def run_commands(tree: Path, variant: str, commands: list[str]) -> int:
    logs = tree / "sanitizer-logs"
    skip = skipped(tree, commands)
    for command in skip:
        print(f"skip (tests/sanitizers/skip.txt): {command[:150]}", flush=True)
    commands = [command for command in commands if command not in skip]
    for old in logs.iterdir():
        old.unlink()
    env = sanitizer_environment(tree, variant)
    timeout = int(os.environ.get("FORTIS_SANITIZER_TIMEOUT", "3600"))
    failed: list[tuple[str, str]] = []
    started = time.time()
    for index, command in enumerate(commands):
        before = set(logs.iterdir())
        begin = time.time()
        try:
            proc = subprocess.run(command, shell=True, cwd=tree, env=env, capture_output=True, text=True,
                                  errors="replace", timeout=timeout)
            status, output = proc.returncode, proc.stdout + proc.stderr
        except subprocess.TimeoutExpired as expired:
            status, output = -1, f"timed out after {timeout} s\n{expired.stdout or ''}{expired.stderr or ''}"
        reports = sorted(set(logs.iterdir()) - before)
        ok = status == 0 and not reports
        print(f"[{index + 1}/{len(commands)}] {'ok  ' if ok else 'FAIL'} {time.time() - begin:6.1f}s  {command[:150]}",
              flush=True)
        if not ok:
            detail = f"exit status {status}; {len(reports)} sanitizer report file(s)\n"
            for report in reports:
                detail += f"--- {report.name}\n{report.read_text(errors='replace')[:6000]}\n"
            detail += f"--- output tail\n{output[-1500:]}\n"
            print(detail, flush=True)
            failed.append((command, detail))
    print(f"\n{variant}: {len(commands)} commands, {len(commands) - len(failed)} passed, {len(failed)} failed, "
          f"{len(skip)} skipped, {round(time.time() - started)} s")
    for command, _ in failed:
        print("FAIL:", command[:160])
    return 1 if failed else 0


def main() -> int:
    argv = sys.argv[1:]
    if len(argv) < 2 or argv[0] not in VARIANTS or argv[1] not in ("build", "unit", "replay", "run"):
        print(__doc__, file=sys.stderr)
        return 2
    runtime = os.environ.get("FORTIS_SANITIZER_RUNTIME", "auto")
    if runtime == "docker" or (runtime == "auto" and platform.system() != "Linux"):
        return in_docker(argv)
    variant, action, rest = argv[0], argv[1], argv[2:]
    tree = prepare_tree(variant)
    if action == "run":
        if not rest:
            die("run needs a command")
        return subprocess.run(rest, cwd=tree, env=sanitizer_environment(tree, variant)).returncode
    if action == "build" and rest:
        build(tree, variant, rest)
        return 0
    build(tree, variant, [])
    commands = recipe(tree, variant)
    if action == "replay":
        if not rest:
            die("replay needs a regex choosing the python3 recipe commands")
        commands = [c for c in commands if c.startswith("python3 ") and re.search(rest[0], c)]
    else:
        commands = [c for c in commands if c.startswith("./build/")]
        if action == "unit" and rest:
            commands = [c for c in commands if re.search(rest[0], c)]
    if not commands:
        die("no recipe command matches")
    build(tree, variant, binaries(commands))
    return run_commands(tree, variant, commands) if action != "build" else 0


if __name__ == "__main__":
    sys.exit(main())
