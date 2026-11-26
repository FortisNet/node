#!/usr/bin/env python3
"""Behavior mutations of the R18 overview code against its tests.

Each mutation edits one line of src/, rebuilds the test binaries, runs the
check that should notice it (the unit test or named replay groups with
R18_FAIL_FAST=1) and restores the source. A mutation is KILLED when that
check fails, SURVIVED when it passes. The sources are restored and rebuilt
at the end (also after an interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/network_overview_mutations.py [log path]
  R18_MUTATION_FILTER='first included token' python3 tests/network_overview_mutations.py [log path]
"""
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/network_overview_edges.json"
BINARY = "build/fortis_node_address_route_test"
UNIT = "build/network_overview_test"

# (name, file, old, new, check) — check is "unit" or "replay:<groups>".
MUTATIONS = [
    ("protobuf accepts any holderCount", "src/wallet/address_overview.cpp",
     "if (!is_null(holders) && !valid_uint64(holders)) {", "if (false && !valid_uint64(holders)) {", "unit"),
    ("refused protobuf 500 without cache-control", "src/wallet/address_service.cpp",
     "if (refused) response.cache_control = kOverviewCacheControl;", "(void)refused;", "replay:shapes"),
    ("fixed 8 KiB token sub-message", "src/wallet/address_overview.cpp",
     "Sub item(encoded_bound(token));", "Sub item(8192);", "unit"),
    ("phrase without a leading boundary", "src/wallet/address_overview.cpp",
     "if (i > 0 && ascii_alnum(static_cast<uint8_t>(text[i - 1]))) continue;", "", "unit"),
    ("first included token wins", "src/wallet/address_overview.cpp",
     "same_text(id, base_id)) token = resource;", "same_text(id, base_id) && !token) token = resource;",
     "replay:shapes"),
    ("no token-id dedupe", "src/wallet/address_overview.cpp",
     "seen = same_text(selected[s].token_id, token_id);", "seen = false;", "replay:shapes"),
    ("Ethplorer null body cached as null", "src/wallet/address_overview.cpp",
     "if (!call.value || call.value->type == tj::Type::kNull) {", "if (false) {", "replay:shapes"),
    ("no Etherscan free-api memo", "src/wallet/address_overview.cpp",
     "if (const auto id = chains::ChainId::FromNumber(chain_id)) etherscan_unsupported.Insert(*id);", "(void)chain_id;", "replay:etherscan"),
    ("memo re-checked per token", "src/wallet/address_overview.cpp",
     "HolderContext context{&network, token_id, ref.has_address ? ref.address : address, false};",
     "HolderContext context{&network, token_id, ref.has_address ? ref.address : address, true};", "replay:etherscan"),
    ("Number(null) is NaN", "src/wallet/address_overview.cpp",
     "case tj::Type::kNull: return 0;", "case tj::Type::kNull: return NAN;", "replay:etherscan"),
    # Dropped (opaque-params, a NUL never reaches it): "NUL network without its detail".
    # Dropped (opaque-params, no flag left): "overview route without opaque params".
    ("five news items", "src/wallet/address_overview.cpp",
     "news_items->count < 4;", "news_items->count < 5;", "replay:news"),
    ("outage card not stale", "src/wallet/address_overview.cpp",
     "popular = popular_section(arena, true, nullptr, false, 0, true, true);",
     "popular = popular_section(arena, true, nullptr, false, 0, false, true);", "replay:errors"),
]


def run(command, log, env=None):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    tail = "\n".join(result.stdout.splitlines()[-6:])
    log.write(tail + f"\n(exit {result.returncode})\n")
    log.flush()
    return result.returncode


def build(log):
    # Source mtimes have one-second resolution: a mutant written in the same
    # second its predecessor's objects were built would not be recompiled.
    # A relink can also be skipped on a same-second timestamp tie, leaving
    # the previous mutation in the executable despite fresh object files.
    for path in ("build/address_overview.o", "build/address_service.o", "build/address_network_routes.o", BINARY,
                 UNIT):
        (ROOT / path).unlink(missing_ok=True)
    return run(["make", "-j8", BINARY, UNIT], log)


def main():
    import os
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "network_overview_mutations.log"
    selected = os.environ.get("R18_MUTATION_FILTER")
    results = []
    with open(log_path, "w") as log:
        log.write(f"R18 overview mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        for name, path, old, new, check in MUTATIONS:
            if selected and selected not in name:
                continue
            file = ROOT / path
            original = file.read_text()
            if original.count(old) != 1:
                results.append((name, "NOT APPLIED"))
                log.write(f"== {name}: the original line is not unique/present\n")
                continue
            log.write(f"== {name} ({path}; {check})\n")
            try:
                file.write_text(original.replace(old, new))
                if build(log) != 0:
                    results.append((name, "BUILD FAILED"))
                    continue
                if check == "unit":
                    code = run([UNIT, FIXTURE], log)
                else:
                    env = dict(os.environ, R18_ONLY_GROUPS=check.split(":", 1)[1], R18_FAIL_FAST="1")
                    code = run(["python3", "tests/network_overview_edges_test.py", BINARY], log, env)
                results.append((name, "KILLED" if code != 0 else "SURVIVED"))
            finally:
                file.write_text(original)
            log.write(f"-> {results[-1][1]}\n\n")
        build(log)
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
