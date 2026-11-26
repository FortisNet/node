#!/usr/bin/env python3
"""Behavior mutations of the R19 Token 360 code against its tests.

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and its build/<stem>_*.o variants)
and both test binaries, rebuilds them, runs the check that should notice it
(the unit test, or named replay groups with R19_FAIL_FAST=1) and restores the
source. Deleting the objects and binaries forces the compile and the relink
of the changed source (R18: with one-second mtimes a same-second mutant was
not recompiled, and a same-second relink was skipped). A mutation is KILLED
when its check fails, SURVIVED when it passes. Every source is restored,
compared with its original bytes and rebuilt at the end (also after an
interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/token360_mutations.py [log path]
  R19_MUTATION_FILTER='opaque' python3 tests/token360_mutations.py [log path]
"""
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/token360_edges.json"
BINARY = "build/fortis_node_token_route_test"
UNIT = "build/token360_test"

# (name, file, old, new, check) — check is "unit" or "replay:<groups>".
MUTATIONS = [
    ("no raw-byte refusal", "src/tokens/token_task_routes.cpp",
     "if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy",
     "if ((false && reject_raw_non_ascii(client, request, head)) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy", "replay:main"),
    # Dropped (fastify-errors): "414 echoes the raw target" (the 414
    # body is the constant {"error":"URI_TOO_LONG"}; nothing is echoed).
    # Dropped (opaque-params, no flag left): "route without opaque params".
    ("network bound 100 units", "src/tokens/token_task_routes.cpp",
     "network_units < 1 || network_units > 64", "network_units < 1 || network_units > 100", "replay:main"),
    # Dropped (opaque-params: a path value never holds a NUL, so the
    # mutants are equivalent): "NUL ref detail cut at the C string", "NUL
    # network detail cut at the C string".
    ("quota cost 1", "src/tokens/token_task_routes.cpp",
     "reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost,",
     "reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1,", "replay:main"),
    ("no Vary on the structured answer", "src/tokens/token_service.cpp",
     "selected.vary_accept = true;", "selected.vary_accept = false;", "replay:main"),
    ("provider requests without accept", "src/providers/token_provider.cpp",
     'snprintf(request.headers[headers].name, sizeof(request.headers[0].name), "accept");',
     'snprintf(request.headers[headers].name, sizeof(request.headers[0].name), "x-accept");', "replay:main"),
    ("legacy wire without chainMeta", "src/tokens/token_page.cpp",
     "token_facts::FacetBit(token_facts::kHolders) |\n                        token_facts::FacetBit(token_facts::kChainMeta);",
     "token_facts::FacetBit(token_facts::kHolders);", "replay:main"),
    ("holders spelled atLeast", "src/tokens/token_page.cpp",
     'tj::Set(a, holders, "floor", tj::Clone(a, at_least))', 'tj::Set(a, holders, "atLeast", tj::Clone(a, at_least))',
     "unit"),
    ("meta for an empty coin id", "src/tokens/token_page.cpp",
     "if (facts.resolution.has_coin && facts.resolution.coin_id[0]) {", "if (facts.resolution.has_coin) {", "unit"),
    ("unparseable 2xx not retried", "src/providers/provider_execution.cpp",
     "const bool unparseable = options.retry_unparseable_json &&",
     "const bool unparseable = false && options.retry_unparseable_json &&", "replay:errors"),
    ("any token is JSON", "src/providers/provider_execution.cpp",
     '} else if (!literal("true") && !literal("false") && !literal("null")) {\n        return false;',
     '} else if (!literal("true") && !literal("false") && !literal("null")) {\n        return true;', "replay:errors"),
    ("fresh through the freshUntil instant", "src/tokens/token_swr.cpp",
     "return now_ms < header.fresh_until_ms ? State::kFresh : State::kStale;",
     "return now_ms <= header.fresh_until_ms ? State::kFresh : State::kStale;", "replay:swr"),
]


def run(command, log, env=None):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    tail = "\n".join(result.stdout.splitlines()[-6:])
    log.write(tail + f"\n(exit {result.returncode})\n")
    log.flush()
    return result.returncode


def build(log, source):
    """Compile and link `source` afresh: its objects and both binaries go."""
    stem = Path(source).stem
    removed = [p for p in (ROOT / "build").glob(f"{stem}*.o") if p.stem == stem or p.stem.startswith(stem + "_")]
    for path in removed + [ROOT / BINARY, ROOT / UNIT]:
        path.unlink(missing_ok=True)
    log.write(f"removed {sorted(p.name for p in removed)} and both binaries\n")
    return run(["make", "-j8", BINARY, UNIT], log)


def main():
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "token360_mutations.log"
    selected = os.environ.get("R19_MUTATION_FILTER")
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    results = []
    with open(log_path, "w") as log:
        log.write(f"R19 token360 mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        try:
            for name, path, old, new, check in MUTATIONS:
                if selected and selected not in name:
                    continue
                file = ROOT / path
                original = sources[path].decode()
                if original.count(old) != 1:
                    results.append((name, "NOT APPLIED"))
                    log.write(f"== {name}: the original text is not unique/present\n\n")
                    continue
                log.write(f"== {name} ({path}; {check})\n")
                try:
                    file.write_text(original.replace(old, new))
                    if build(log, path) != 0:
                        results.append((name, "BUILD FAILED"))
                        continue
                    if check == "unit":
                        code = run([UNIT, FIXTURE], log)
                    else:
                        env = dict(os.environ, R19_ONLY_GROUPS=check.split(":", 1)[1], R19_FAIL_FAST="1")
                        code = run(["python3", "tests/token360_edges_test.py", BINARY], log, env)
                    results.append((name, "KILLED" if code != 0 else "SURVIVED"))
                finally:
                    file.write_bytes(sources[path])
                log.write(f"-> {results[-1][1]}\n\n")
        finally:
            for path, data in sources.items():
                (ROOT / path).write_bytes(data)
            restored = all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == d for p, d in digests.items())
            for path in sources:
                stem = Path(path).stem
                for obj in (ROOT / "build").glob(f"{stem}*.o"):
                    if obj.stem == stem or obj.stem.startswith(stem + "_"):
                        obj.unlink()
            (ROOT / BINARY).unlink(missing_ok=True)
            (ROOT / UNIT).unlink(missing_ok=True)
            code = run(["make", "-j8", BINARY, UNIT, "build/fortis_node"], log)
            log.write(f"sources restored byte for byte: {restored}; restored rebuild exit {code}\n")
        log.write("Summary\n")
        for name, outcome in results:
            log.write(f"  {outcome:9} {name}\n")
        killed = sum(1 for _, outcome in results if outcome == "KILLED")
        log.write(f"{killed}/{len(results)} killed\n")
    for name, outcome in results:
        print(f"{outcome:9} {name}")
    print(f"{sum(1 for _, o in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
