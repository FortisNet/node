#!/usr/bin/env python3
"""Behavior mutations of the R20–R23 token history/liquidity code against its
tests (tests/token_history_test.cpp and tests/token_history_edges_test.py).

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and its build/<stem>_*.o variants)
and both test binaries, rebuilds them, runs the check that should notice it
(the unit test, or named replay groups with R20_FAIL_FAST=1, optionally
limited to one route) and restores the source. Deleting the objects and
binaries forces the compile and the relink of the changed source (the R18
lesson: with one-second mtimes a same-second mutant was not recompiled). A
mutation is KILLED when its check fails, SURVIVED when it passes, BUILD
FAILED when it does not compile or link (never counted as killed). Every
source is restored, compared with its original bytes and rebuilt at the end
(also after an interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/token_history_mutations.py [log path]
  R20_MUTATION_FILTER='R22' python3 tests/token_history_mutations.py [log path]
"""
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/token_history_edges.json"
BINARY = "build/fortis_node_token_route_test"
UNIT = "build/token_history_test"
SERVICE = "src/tokens/token_service.cpp"
HISTORY = "src/tokens/token_history.cpp"
DATA = "src/tokens/token_data.cpp"
HISTORY_ROUTES = "src/tokens/token_history_routes.cpp"
ROUTE_SUPPORT = "src/tokens/token_route_support.cpp"

# (name, file, old, new, check) — check is "unit" or "replay:<groups>[:<routes>]".
MUTATIONS = [
    # ---- R20 holders/history ----
    # Dropped (opaque-params, no flag left): "R20 route without opaque params".
    ("R20 days bound 365", HISTORY_ROUTES,
     "value >= 1 && value <= 400;", "value >= 1 && value <= 365;", "replay:main:holders"),
    ("R20 source bound 79 units", HISTORY_ROUTES,
     "valid = units >= 1 && units <= 80;", "valid = units >= 1 && units <= 79;", "replay:main:holders"),
    ("R20/R21 query buffers of 1 KiB", ROUTE_SUPPORT,
     "capacity_ = strlen(target) + 1;", "capacity_ = 1024;", "replay:main:holders"),
    ("R20 lower-bound counts enter trends", HISTORY,
     "if (at_least && at_least->type == tj::Type::kTrue) continue;",
     "if (false && at_least && at_least->type == tj::Type::kTrue) continue;", "unit"),
    ("R20 count baseline tolerance exclusive", HISTORY,
     "const double distance = fabs(rows[i].observed_at - target);\n"
     "    if (distance > static_cast<double>(tolerance)) continue;",
     "const double distance = fabs(rows[i].observed_at - target);\n"
     "    if (distance >= static_cast<double>(tolerance)) continue;", "unit"),
    ("R20 trends stale after 48 h", HISTORY,
     "> static_cast<double>(36 * kHourMs))", "> static_cast<double>(48 * kHourMs))", "unit"),
    ("R20 distribution not limited to its newest source", HISTORY,
     "    if (tj::IsString(s) && tj::IsString(latest_source) && s->length == latest_source->length &&\n"
     "        memcmp(s->text, latest_source->text, s->length) == 0)\n      out[kept++] = out[i];",
     "    (void)s;\n    (void)latest_source;\n    out[kept++] = out[i];", "unit"),
    ("R20 observations oldest first", HISTORY,
     "while (pos > 0 && picked[pos - 1].at < at)", "while (pos > 0 && picked[pos - 1].at > at)", "unit"),
    ("R20 256 positions per day", HISTORY,
     "i < list->count && n < capacity; ++i) {", "i < list->count && n < capacity && n < 256; ++i) {", "unit"),
    ("R20 trends from a second read", HISTORY,
     "if (!trends_of(a, rows, source, now_ms, &trends)) return false;",
     "if (!Trends(work, network, token_id, source, now_ms, &trends)) return false;", "replay:concurrent"),
    # ---- R21 liquidity/history ----
    # Dropped (opaque-params, no flag left): "R21 route without opaque params".
    ("R21 no 1h/90-day cap", HISTORY_ROUTES,
     "if (!fortis::tokens::token_history::ResolutionAllowed(range, interval)) {",
     "if (false && !fortis::tokens::token_history::ResolutionAllowed(range, interval)) {", "replay:main:history"),
    ("R21 validator keeps latestAgeSeconds", HISTORY,
     "shallow(availability, kAge, 1);", "shallow(availability, kAge, 0);", "replay:main:history"),
    ("R21 rollup lag one hour", HISTORY,
     "constexpr int64_t kRollupLagMs = 2 * kHourMs;", "constexpr int64_t kRollupLagMs = 1 * kHourMs;", "unit"),
    ("R21 no daily-candle baseline for d7", HISTORY,
     "window.ago >= 7 * kDayMs) {", "window.ago >= 30 * kDayMs) {", "unit"),
    ("R21 observation scan stops at one page", DATA,
     "more = blocks && !state.stopped && blocks->count == kObservationPage;", "more = false;", "unit"),
    ("R21 latest from a forward scan of the first page", DATA,
     "kv_->Scan(prefix, after, kObservationPage, now_ms, collect_visit, &state, true)",
     "kv_->Scan(prefix, \"\", kObservationPage, now_ms, collect_visit, &state, false)", "unit"),
    ("R21 candle scan stops at one page", DATA,
     "more = blocks && !state.stopped && blocks->count == kCandlePage;", "more = false;", "unit"),
    # ---- R22 liquidity/pools ----
    # Dropped (opaque-params, no flag left): "R22 route without opaque params".
    # Dropped (opaque-params: a path value never holds a NUL):
    # "R22 NUL ref detail cut at the C string".
    ("R22 page cache-control", SERVICE,
     '"token-liquidity-pools", document, nullptr, kMinuteCacheControl, true);',
     '"token-liquidity-pools", document, nullptr, kPageCacheControl, true);', "replay:pools"),
    ("R22 not conditional", SERVICE,
     '"token-liquidity-pools", document, nullptr, kMinuteCacheControl, true);',
     '"token-liquidity-pools", document, nullptr, kMinuteCacheControl, false);', "replay:pools"),
    ("R22 fused multiply-add allowed", "src/tokens/token_liquidity.cpp",
     "#pragma clang fp contract(off)", "#pragma clang fp contract(on)", "unit"),
    ("R22 unparseable 2xx not retried (R19)", "src/providers/provider_execution.cpp",
     "const bool unparseable = options.retry_unparseable_json &&",
     "const bool unparseable = false && options.retry_unparseable_json &&", "replay:pools"),
    # ---- R23 liquidity/summary ----
    # Dropped (opaque-params, no flag left): "R23 route without opaque params".
    ("R23 price-only rows count as successful", HISTORY,
     "if (rows.items[i].ok && rows.items[i].has_liquidity) rows.items[successful++]",
     "if (rows.items[i].ok && (rows.items[i].has_liquidity || rows.items[i].has_price)) rows.items[successful++]",
     "unit"),
    ("R23 baseline tolerance under 2 h", HISTORY,
     "change_at(a, rows.items, successful, latest, kDayMs, 2 * kHourMs);",
     "change_at(a, rows.items, successful, latest, kDayMs, 2 * kHourMs - 1);", "unit"),
    ("R21/R23 stale after 1801 s", HISTORY,
     "const double limit = 1800 > cadence * 2 ? 1800 : cadence * 2;",
     "const double limit = 1801 > cadence * 2 ? 1801 : cadence * 2;", "replay:main:summary"),
]


def run(command, log, env=None):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                            encoding="utf-8", errors="replace")
    lines = [line for line in result.stdout.splitlines() if not line.startswith("c++ ")]
    log.write("\n".join(lines[-8:]) + f"\n(exit {result.returncode})\n")
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
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "token_history_mutations.log"
    selected = os.environ.get("R20_MUTATION_FILTER")
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    results = []
    with open(log_path, "w") as log:
        log.write(f"R20–R23 token history mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        for path, digest in sorted(digests.items()):
            log.write(f"  original {path} sha256 {digest}\n")
        log.write("\n")
        try:
            for name, path, old, new, check in MUTATIONS:
                if selected and selected not in name:
                    continue
                file = ROOT / path
                original = sources[path].decode()
                if original.count(old) != 1:
                    results.append((name, "NOT APPLIED"))
                    log.write(f"== {name}: the original text is not unique/present ({original.count(old)})\n\n")
                    continue
                log.write(f"== {name} ({path}; {check})\n")
                started = time.time()
                try:
                    file.write_text(original.replace(old, new))
                    if build(log, path) != 0:
                        results.append((name, "BUILD FAILED"))
                        log.write("-> BUILD FAILED (not a kill)\n\n")
                        continue
                    if check == "unit":
                        code = run([UNIT, FIXTURE], log)
                    else:
                        parts = check.split(":")
                        env = dict(os.environ, R20_ONLY_GROUPS=parts[1], R20_FAIL_FAST="1")
                        if len(parts) > 2:
                            env["R20_ONLY_ROUTES"] = parts[2]
                        code = run(["python3", "tests/token_history_edges_test.py", BINARY], log, env)
                    results.append((name, "KILLED" if code != 0 else "SURVIVED"))
                finally:
                    file.write_bytes(sources[path])
                log.write(f"-> {results[-1][1]} ({time.time() - started:.0f} s)\n\n")
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
            for path in sorted(digests):
                log.write(f"  restored {path} sha256 {hashlib.sha256((ROOT / path).read_bytes()).hexdigest()}\n")
            log.write(f"sources restored byte for byte: {restored}; restored rebuild exit {code}\n")
        log.write("Summary\n")
        for name, outcome in results:
            log.write(f"  {outcome:12} {name}\n")
        killed = sum(1 for _, outcome in results if outcome == "KILLED")
        failed = sum(1 for _, outcome in results if outcome == "BUILD FAILED")
        log.write(f"{killed}/{len(results)} killed, {failed} build failures (not counted as killed)\n")
    for name, outcome in results:
        print(f"{outcome:12} {name}")
    print(f"{sum(1 for _, o in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
