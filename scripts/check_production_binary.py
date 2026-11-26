#!/usr/bin/env python3
"""Fail when test-only code is linked into the production server binary.

Run by `make check-production-binary` (part of `make test`):

  python3 scripts/check_production_binary.py build/fortis_node \\
      build/fortis_node_probe_test

The production binary must contain no test probe route, no test hook and no
test-only name. The replay binaries get all of that by linking the units
under tests/server instead of src/app/main.cpp; nothing in src/ may bring it
back. Four checks:

  1. Bytes. No forbidden string in the production binary (FORBIDDEN_BYTES):
     test probe paths, FORTIS_*TEST* environment names, fixture host markers.
  2. Symbols. No demangled symbol of the production binary has a word `test`,
     `tests`, `testing`, `fixture`, `fixtures`, `mock`, `fake` or `stub`
     (identifiers are split at `_`, `::` and camelCase boundaries, so
     `attestation` and `latest` do not match), unless ALLOWED_SYMBOLS names
     it with the reason it is production code. Mbed TLS and blst symbols are
     third-party and are not checked.
  3. Source list. sources.mk compiles nothing from tests/ and passes no
     FORTIS_*TEST* define, and it names exactly the .c/.cpp files under src/
     (src/ holds production code only).
  4. Seam callers. The test-only seams that stay in src/ (StartRedirected,
     RunWithoutStateLock, SetNetworkEndpoints, SetPriceHistoryCoverageClock;
     REDIRECT_SEAMS) are declared, defined and forwarded there (`impl_->`) but
     never called: only a composition under tests/ may switch them on, so no
     production input (environment, config file, request) can reach them.

The second argument is the positive control: a replay binary that must
trip checks 1 and 2, so a missing `nm`, a stripped binary or a broken pattern
cannot make the check pass by finding nothing.
"""
import os
import re
import subprocess
import sys

# (pattern, what it is). Matched against the raw bytes of the binary.
FORBIDDEN_BYTES = [
    (rb"/probe/test-", "a /probe/test-* route of a replay binary"),
    (rb"/probe/async", "the async test probes"),
    (rb"/probe/provider-test", "the provider bridge test probe"),
    (rb"FORTIS_(?:[A-Z0-9]+_)*TEST(?:_[A-Z0-9]+)*", "a test-only environment or macro name"),
    (rb"fortis-test", "a fixture host or lane marker (x-fortis-test-host, rpc:fortis-test-*)"),
    (rb"[Tt]est-only", "a test-only message"),
    (rb"tests/server", "a path under tests/server"),
]

# Production symbols that contain a flagged word: "<regex over the demangled
# symbol>": "why it is production code". Keep this list short; an entry that
# no longer matches any symbol is reported as stale and fails the check.
ALLOWED_SYMBOLS = {
    r"fortis::http::\(anonymous namespace\)::tls_probe_test_endpoint\(":
        "GET /probe/tls (FORTIS_ENABLE_DIAGNOSTIC_PROBES=1): the address policy of the operator's "
        "configured TLS probe endpoint, not a test seam",
}

# Test-only seams compiled into production objects but never called from src/
# (check 4). A src/ line may name one only to declare or define it (a return
# type before the name) or to forward to the owner's implementation (impl_->).
REDIRECT_SEAMS = re.compile(
    r"\b(StartRedirected|RunWithoutStateLock|SetNetworkEndpoints|SetPriceHistoryCoverageClock)\s*\(")
SEAM_DECLARATION = re.compile(r"(?:\b(?:bool|void)\s+(?:[A-Za-z_]\w*::)?|\bimpl_->)$")

THIRD_PARTY = re.compile(r"^_?(mbedtls_|psa_|blst_|blake3_|pb_|fortisnet_intelligence_v1_)")
FLAGGED_WORDS = {"test", "tests", "testing", "fixture", "fixtures", "mock", "fake", "stub"}
WORD = re.compile(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|[0-9]+")


def symbols(binary):
    result = subprocess.run(["nm", "-C", binary], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"check_production_binary: nm failed on {binary}: {result.stderr.strip()}")
    names = set()
    for line in result.stdout.splitlines():
        parts = line.split(None, 2)
        # "<address> <type> <name>" or "<type> <name>" for undefined symbols.
        if len(parts) == 3:
            names.add(parts[2])
        elif len(parts) == 2 and len(parts[0]) == 1:
            names.add(parts[1])
    return names


def flagged(name):
    """The flagged words of a demangled symbol (parameter lists included)."""
    if THIRD_PARTY.match(name):
        return set()
    return {word.lower() for word in WORD.findall(name)} & FLAGGED_WORDS


def byte_hits(data):
    hits = []
    for pattern, what in FORBIDDEN_BYTES:
        found = sorted({match.group(0) for match in re.finditer(pattern, data)})
        if found:
            hits.append((what, found))
    return hits


def symbol_hits(names):
    return sorted(name for name in names if flagged(name))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    production, control = sys.argv[1], sys.argv[2]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    errors = []

    # The positive control must trip both binary checks.
    control_bytes = byte_hits(open(control, "rb").read())
    control_symbols = symbol_hits(symbols(control))
    control_whats = {what for what, _ in control_bytes}
    for pattern, what in FORBIDDEN_BYTES[1:4]:
        if what not in control_whats:
            sys.exit(f"check_production_binary: the control binary {control} lacks {what}; "
                     "the byte check would pass vacuously")
    if not any("TestHooks" in name for name in control_symbols):
        sys.exit(f"check_production_binary: no TestHooks symbol found in the control binary "
                 f"{control}; is it stripped, or did nm change its output?")

    data = open(production, "rb").read()
    for what, found in byte_hits(data):
        shown = ", ".join(item.decode("latin-1") for item in found[:8])
        errors.append(f"{production} contains {what}: {shown}"
                      + (f" (+{len(found) - 8} more)" if len(found) > 8 else ""))
    names = symbols(production)
    if not any("ServerMain" in name for name in names):
        sys.exit(f"check_production_binary: no ServerMain symbol in {production}; it must not be "
                 "stripped before this check")
    used = set()
    for name in symbol_hits(names):
        allowed = [pattern for pattern in ALLOWED_SYMBOLS if re.search(pattern, name)]
        if allowed:
            used.update(allowed)
        else:
            errors.append(f"{production} links a test-named symbol: {name}")
    for pattern in sorted(set(ALLOWED_SYMBOLS) - used):
        errors.append(f"stale ALLOWED_SYMBOLS entry (matches no symbol): {pattern}")

    # The source list: sources.mk names every production source, and nothing
    # from tests/ or carrying a test-only define.
    shared = open(os.path.join(root, "sources.mk")).read()
    for number, line in enumerate(shared.splitlines(), 1):
        code = line.split("#", 1)[0]
        if re.search(r"/tests/|\btests/", code):
            errors.append(f"sources.mk:{number} compiles a file under tests/: {code.strip()}")
        if re.search(r"-DFORTIS_(?:[A-Z0-9]+_)*TEST", code):
            errors.append(f"sources.mk:{number} passes a test-only define: {code.strip()}")
        if re.search(r"\bsrc/[a-z_]+/(?:[a-z0-9]+_)*tests?(?:_[a-z0-9]+)*\.c", code):
            errors.append(f"sources.mk:{number} compiles a test-named source: {code.strip()}")

    entries = re.findall(r"^FORTIS_SOURCES \+= (\S+)$", shared, re.M)
    for entry in sorted(set(entries)):
        if entries.count(entry) > 1:
            errors.append(f"sources.mk lists {entry} more than once")
    listed = {entry[len("src/"):] for entry in entries if entry.startswith("src/")}
    guest_only = re.search(r"^FORTIS_GUEST_ONLY_SOURCES\s*:=(.*)$", shared, re.M)
    for entry in (guest_only.group(1).split() if guest_only else []):
        if entry not in entries:
            errors.append(f"sources.mk FORTIS_GUEST_ONLY_SOURCES names {entry}, which FORTIS_SOURCES does not list")
    present = set()
    source_root = os.path.join(root, "src")
    for module in sorted(os.listdir(source_root)):
        directory = os.path.join(source_root, module)
        if os.path.isdir(directory):
            present.update(f"{module}/{name}" for name in os.listdir(directory)
                           if name.endswith((".cpp", ".c")))
    for path in sorted(present - listed):
        errors.append(f"src/{path} is not listed in sources.mk (test-only code belongs under "
                      "tests/; production code must be in the source list)")
    for path in sorted(listed - present):
        errors.append(f"sources.mk lists src/{path}, which does not exist")

    seam_files = 0
    for directory, _, files in sorted(os.walk(source_root)):
        for name in sorted(files):
            if not name.endswith((".h", ".cpp", ".c")):
                continue
            path = os.path.join(directory, name)
            seam_files += 1
            for number, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                code = line.split("//", 1)[0]
                for match in REDIRECT_SEAMS.finditer(code):
                    if not SEAM_DECLARATION.search(code[:match.start()]):
                        errors.append(f"{os.path.relpath(path, root)}:{number} calls the test-only "
                                      f"seam {match.group(1)} from production code")

    if errors:
        print("production binary check failed:")
        for error in errors:
            print("  " + error)
        return 1
    print(f"production binary: no test probe routes, hook code or test-only names "
          f"({len(names)} symbols, {len(data)} bytes; control binary trips "
          f"{len(control_bytes)} byte rules and {len(control_symbols)} symbols; "
          f"{len(ALLOWED_SYMBOLS)} allowed production symbols); sources.mk lists the "
          f"{len(present)} sources under src/ and nothing from tests/; no src/ caller of a "
          f"test-only seam ({seam_files} files): ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
