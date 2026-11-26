#!/usr/bin/env python3
"""clang-tidy and clang-format front end (make lint, lint-all, format-diffstat).

  scripts/lint.py tidy [--base REF] [--whole-files]
                                         files changed against REF (default:
                                         the merge-base with the default/main/master branch),
                                         uncommitted and untracked ones
                                         included; only findings on lines
                                         changed against REF are reported
                                         (--whole-files: every finding in
                                         those files)
  scripts/lint.py tidy --all             every file under src/ and tests/
  scripts/lint.py tidy FILE...           the named files
  scripts/lint.py format-diffstat        how many lines .clang-format would
                                         change; nothing is rewritten

tidy prints each finding once (a header's finding repeats in every including
file) and then the count per check. Against a base ref ("zero new warnings")
a finding counts only when its line is an added or modified line of
`git diff -U0 REF` (every line of an untracked file); findings elsewhere in
the analysed files are pre-existing debt, counted but not shown (make
lint-all lists them). It exits 1 when there are findings and 2
when a file could not be analysed. FORTIS_LINT_REPORT_ONLY=1 makes findings
exit 0 for an explicitly requested advisory run; make lint-all is strict.

A changed header is linted through the source of the same name where there is
one, else through a source that includes it, and on its own otherwise. Each
source is analysed with the flags of its own compile line, read from a dry run
of the test build (`make -n -B test` plus every tests/ program with a rule,
Mbed TLS held back with -o), so the
per-object include paths and -DFORTIS_*_TEST_ONLY switches apply; a source the
dry run does not compile gets the default flags (`make print-compile-flags`);
test programs require a build rule so their test switches are known.
Where one source compiles several ways, its production object's line is used.
Generated sources (proto/generated, *_generated.cpp, *_tables.cpp) and the
vendored third_party/ code are never linted.

The tools are clang-tidy / clang-format 18 from the Dockerfile.sanitizers
image unless FORTIS_LINT_RUNTIME=host, which uses CLANG_TIDY / CLANG_FORMAT
(default clang-tidy / clang-format) from PATH.
"""
import collections
import concurrent.futures
import difflib
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
# Tagged with a checksum of the Dockerfile: editing it builds a new image.
IMAGE = "fortis-node-sanitizers:" + hashlib.sha256(
    (PROJECT / "Dockerfile.sanitizers").read_bytes()).hexdigest()[:12]
FINDING = re.compile(r"^(?P<where>[^\s:][^:]*:\d+:\d+): (?P<kind>warning|error): (?P<text>.*) \[(?P<check>[^\]\s]+)\]$")


def die(message: str) -> "NoReturn":
    print(f"lint: {message}", file=sys.stderr)
    sys.exit(2)


def lintable(path: str) -> bool:
    if not path.startswith(("src/", "tests/")) or not path.endswith((".cpp", ".h")):
        return False
    name = Path(path).name
    return not (name.endswith(("_generated.cpp", "_generated.h", "_tables.cpp")) or "/blst/" in path)


def all_files() -> list[str]:
    found = []
    for top in ("src", "tests"):
        for path in sorted((PROJECT / top).rglob("*")):
            relative = path.relative_to(PROJECT).as_posix()
            if path.is_file() and lintable(relative):
                found.append(relative)
    return found


def default_base() -> str:
    """Support standalone main branches, historical master, and initial clones."""
    for reference in ('refs/remotes/origin/HEAD', 'refs/heads/main', 'refs/heads/master'):
        result = subprocess.run(['git', 'merge-base', 'HEAD', reference], cwd=PROJECT,
                                capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.strip()
    return 'HEAD'


def changed_files(base: str | None) -> list[str]:
    def git(*args: str) -> str:
        out = subprocess.run(["git", *args], cwd=PROJECT, capture_output=True, text=True)
        if out.returncode != 0:
            die(f"git {' '.join(args)}: {out.stderr.strip()}")
        return out.stdout
    if base is None:
        base = default_base()
    names = git("diff", "--name-only", "--relative", "--diff-filter=ACMR", base, "--", "src", "tests").split("\n")
    names += git("ls-files", "--others", "--exclude-standard", "--", "src", "tests").split("\n")
    return sorted({name for name in names if name and lintable(name) and (PROJECT / name).is_file()})


def changed_lines(base: str | None) -> dict[str, list[int] | None]:
    """File -> the line numbers added or modified against BASE, from
    `git diff -U0` (working tree included); None for an untracked file (all
    of it is new)."""
    if base is None:
        base = default_base()
    out = subprocess.run(["git", "diff", "-U0", "--no-color", "--no-ext-diff", "--relative", "--diff-filter=ACMR",
                          base, "--", "src", "tests"], cwd=PROJECT, capture_output=True, text=True)
    if out.returncode != 0:
        die(f"git diff -U0 {base}: {out.stderr.strip()}")
    lines: dict[str, list[int] | None] = {}
    current: list[int] | None = None
    for line in out.stdout.splitlines():
        if line.startswith("+++ "):
            name = line[4:]
            current = lines.setdefault(name[2:], []) if name.startswith("b/") else None
            continue
        hunk = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", line)
        if hunk and current is not None:
            start, count = int(hunk[1]), int(hunk[2] if hunk[2] is not None else 1)
            current.extend(range(start, start + count))
    untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "--", "src", "tests"],
                               cwd=PROJECT, capture_output=True, text=True, check=True).stdout.split("\n")
    for name in untracked:
        if name:
            lines[name] = None
    return lines


def including_source(header: str, sources: list[str]) -> str | None:
    """The first source that includes HEADER by its file name."""
    pattern = re.compile(r'^\s*#\s*include\s+"(?:[^"]*/)?' + re.escape(Path(header).name) + '"', re.M)
    for source in sources:
        if pattern.search((PROJECT / source).read_text(errors="replace")):
            return source
    return None


def translation_units(files: list[str]) -> list[str]:
    """Sources as they are; a header through its same-named source, else
    through a source that includes it, else alone."""
    sources = [f for f in all_files() if f.endswith(".cpp")]
    by_stem = {Path(f).stem: f for f in sources}
    units = []
    for name in files:
        if name.endswith(".h"):
            name = by_stem.get(Path(name).stem) or including_source(name, sources) or name
        if name not in units:
            units.append(name)
    return units


def compile_lines() -> dict[str, list[str]]:
    """Source -> its compile flags, from a dry run of the whole test build."""
    def make(*args: str) -> str:
        out = subprocess.run(["make", *args], cwd=PROJECT, capture_output=True, text=True)
        if out.returncode != 0:
            die(f"make {' '.join(args)} failed: {out.stderr.strip()[-400:]}")
        return out.stdout
    stamp = make("-s", "print-mbedtls-stamp").strip()
    # The test recipe, plus the test programs it does not run that have a rule.
    makefile = (PROJECT / "Makefile").read_text()
    extra = [f"build/{p.stem}" for p in sorted((PROJECT / "tests").glob("*.cpp"))
             if re.search(rf"^build/{re.escape(p.stem)}:", makefile, re.M)]
    dry = make("-n", "-B", "-o", stamp, "-o", ".deps/mbedtls-3.6.7/library/Makefile", "test", *extra)
    found: dict[str, tuple[bool, list[str]]] = {}
    for line in dry.splitlines():
        try:
            words = shlex.split(line)
        except ValueError:
            continue
        if len(words) < 3 or not words[0].endswith(("++", "c++", "g++")) and "clang++" not in words[0]:
            continue
        sources = [w for w in words if w.endswith(".cpp") and lintable(w)]
        if len(sources) != 1:
            continue
        source = sources[0]
        flags: list[str] = []
        output = ""
        skip = False
        for index, word in enumerate(words[1:], 1):
            if skip:
                skip = False
                continue
            if word in ("-o", "-MF", "-MT"):
                skip = True
                output = words[index + 1] if word == "-o" else output
                continue
            if word in ("-c", "-MMD", "-MP", "-Werror", source) or word.endswith((".o", ".a")):
                continue
            flags.append(word)
        production = output == f"build/{Path(source).stem}.o" or not output.endswith(".o")
        if source not in found or (production and not found[source][0]):
            found[source] = (production, flags)
    return {source: flags for source, (_, flags) in found.items()}


def in_docker(argv: list[str]) -> int:
    if shutil.which("docker") is None:
        die("docker is needed for clang-tidy/clang-format (or set FORTIS_LINT_RUNTIME=host)")
    if subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True).returncode != 0:
        print(f"lint: building {IMAGE} from Dockerfile.sanitizers", flush=True)
        with open(PROJECT / "Dockerfile.sanitizers", "rb") as dockerfile:
            if subprocess.run(["docker", "build", "-q", "-t", IMAGE, "-"], stdin=dockerfile).returncode != 0:
                die("could not build the lint image")
    command = ["docker", "run", "--rm", "--init", "-v", f"{PROJECT}:/work", "-w", "/work",
               "-e", "FORTIS_LINT_RUNTIME=host", "-e", "CLANG_TIDY=clang-tidy-18", "-e", "CLANG_FORMAT=clang-format-18",
               "-e", "FORTIS_LINT_EXTRA_FLAGS=-stdlib=libc++"]
    for name in ("FORTIS_LINT_REPORT_ONLY", "FORTIS_LINT_JOBS"):
        if name in os.environ:
            command += ["-e", f"{name}={os.environ[name]}"]
    if platform.system() == "Linux":
        command += ["--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp"]
    return subprocess.run(command + [IMAGE, "python3", "scripts/lint.py", *argv]).returncode


def jobs() -> int:
    return int(os.environ.get("FORTIS_LINT_JOBS") or os.cpu_count() or 4)


def on_changed_line(where: str, lines: dict[str, list[int] | None]) -> bool:
    name, line, _ = where.rsplit(":", 2)
    if name not in lines:
        return False
    changed = lines[name]
    return changed is None or int(line) in changed


def tidy(files: list[str], lines_file: str | None = None) -> int:
    if not files:
        print("lint: no source files to check")
        return 0
    if not (PROJECT / ".deps/mbedtls-3.6.7/include").is_dir():
        subprocess.run(["./scripts/fetch-mbedtls.sh"], cwd=PROJECT, check=True)
    flags = subprocess.run(["make", "-s", "print-compile-flags"], cwd=PROJECT, capture_output=True, text=True)
    if flags.returncode != 0:
        die(f"make print-compile-flags failed: {flags.stderr.strip()}")
    extra = shlex.split(os.environ.get("FORTIS_LINT_EXTRA_FLAGS", ""))
    default_flags = [f for f in shlex.split(flags.stdout) if f != "-Werror"] + ["-Itests", "-Itests/server"]
    per_source = compile_lines()
    # Test switches are part of the source's contract. A missing build rule
    # must fail the gate rather than silently dropping that source from lint.
    unbuilt = [name for name in files if name.startswith("tests/") and name.endswith(".cpp")
               and name not in per_source]
    if unbuilt:
        for name in unbuilt:
            print(f"lint: cannot analyse {name}: register its build rule in Makefile")
        return 2
    binary = os.environ.get("CLANG_TIDY", "clang-tidy")

    def one(name: str) -> tuple[str, int, str]:
        language = ["-xc++"] if name.endswith(".h") else []
        compile_flags = per_source.get(name, default_flags) + extra
        proc = subprocess.run([binary, "--quiet", name, "--", *language, *compile_flags], cwd=PROJECT,
                              capture_output=True, text=True, errors="replace")
        return name, proc.returncode, proc.stdout + proc.stderr

    findings: dict[tuple[str, str], str] = {}
    broken: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(jobs()) as pool:
        for name, status, output in pool.map(one, files):
            lines = output.splitlines()
            diagnostic_error = False
            for index, line in enumerate(lines):
                match = FINDING.match(line)
                if not match:
                    continue
                where = os.path.normpath(match["where"].replace(str(PROJECT) + "/", "").replace("/work/", ""))
                if match["kind"] == "error" and match["check"] == "clang-diagnostic-error":
                    diagnostic_error = True
                    broken.append(f"{where}: {match['text']}")
                    continue
                context = "\n".join(lines[index + 1:index + 3])
                findings.setdefault((where, match["check"]), f"{where}: {match['text']} [{match['check']}]\n{context}")
            if status != 0 and not diagnostic_error:
                broken.append(f"{name}: clang-tidy exited {status}: {output[-300:]}")
    old = 0
    if lines_file is not None:
        changed = {name: (None if value is None else set(value))
                   for name, value in json.loads((PROJECT / lines_file).read_text()).items()}
        kept = {key: text for key, text in findings.items() if on_changed_line(key[0], changed)}
        old = len(findings) - len(kept)
        findings = kept
    for key in sorted(findings):
        print(findings[key])
    counts = collections.Counter(check for _, check in findings)
    print(f"\nlint: {len(files)} files, {len(findings)} findings"
          + (f" on changed lines ({old} pre-existing elsewhere in these files not shown; make lint-all lists them)"
             if lines_file is not None else ""))
    for check, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        print(f"{count:6d}  {check}")
    if broken:
        print(f"\nlint: {len(broken)} analysis error(s):")
        for line in broken[:40]:
            print("  ", line)
        return 2
    return 1 if findings and os.environ.get("FORTIS_LINT_REPORT_ONLY") != "1" else 0


def format_diffstat() -> int:
    binary = os.environ.get("CLANG_FORMAT", "clang-format")
    files = all_files()

    def one(name: str) -> tuple[int, int]:
        before = (PROJECT / name).read_text(errors="replace").splitlines()
        after = subprocess.run([binary, "--style=file", name], cwd=PROJECT, capture_output=True, text=True,
                               errors="replace", check=True).stdout.splitlines()
        changed = sum(i2 - i1 for tag, i1, i2, _, _ in
                      difflib.SequenceMatcher(None, before, after, autojunk=False).get_opcodes() if tag != "equal")
        return len(before), changed

    with concurrent.futures.ThreadPoolExecutor(jobs()) as pool:
        results = list(pool.map(one, files))
    total = sum(lines for lines, _ in results)
    changed = sum(count for _, count in results)
    touched = sum(1 for _, count in results if count)
    print(f"format-diffstat: .clang-format would change {changed} of {total} lines ({100 * changed / total:.1f}%) "
          f"in {touched} of {len(files)} files")
    return 0


def main() -> int:
    argv = sys.argv[1:]
    if not argv or argv[0] not in ("tidy", "format-diffstat"):
        print(__doc__, file=sys.stderr)
        return 2
    in_container = os.environ.get("FORTIS_LINT_RUNTIME") == "host"
    if argv[0] == "format-diffstat":
        return format_diffstat() if in_container else in_docker(argv)
    rest = argv[1:]
    if in_container and rest and rest[0] == "--lines":
        return tidy(rest[2:], rest[1])
    if in_container and rest and rest[0] not in ("--all", "--base", "--whole-files"):
        return tidy(rest)
    # git runs here, outside the container, which mounts only this directory.
    whole_files = "--whole-files" in rest
    rest = [word for word in rest if word != "--whole-files"]
    lines_args: list[str] = []
    if rest and rest[0] == "--all":
        files = all_files()
    elif not rest or rest[0] == "--base":
        base = rest[1] if len(rest) > 1 else None
        files = changed_files(base)
        if not whole_files:
            # Handed to the analysing process (possibly in the container) as
            # a file under build/, which the container mounts with the rest.
            lines_file = "build/.lint-changed-lines.json"
            (PROJECT / "build").mkdir(exist_ok=True)
            (PROJECT / lines_file).write_text(json.dumps(changed_lines(base)))
            lines_args = ["--lines", lines_file]
    else:
        files = [name for name in rest if lintable(name)]
    files = translation_units(files)
    if not files:
        print("lint: no changed source files")
        return 0
    if in_container:
        return tidy(files, lines_args[1] if lines_args else None)
    return in_docker(["tidy", *lines_args, *files])


if __name__ == "__main__":
    sys.exit(main())
