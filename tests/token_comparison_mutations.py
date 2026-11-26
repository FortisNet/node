#!/usr/bin/env python3
"""Behavior mutations of POST /v1/tokens/compare (R53) against its tests:
tests/token_comparison_edges_test.cpp (in-process oracle, concurrency,
restart) and tests/token_comparison_edges_test.py (the socket replay).

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and every build/<stem>_*.o) and
both test binaries, rebuilds them with make, runs the unit and then the
replay (COMPARISON_EDGES_FAIL_FAST=1), and restores the source. Deleting the
objects and binaries forces the compile and the relink of the changed source
(with one-second mtimes a same-second mutant would not be recompiled
otherwise). A mutation is KILLED when either check fails, SURVIVED when both
pass, BUILD FAILED when it does not compile or link (never counted as
killed). Every source is restored, compared with its original bytes and
rebuilt at the end (also after an interruption), with build/fortis_node
and build/token_comparison_test.

Usage (from the repository root, with no other replay running):
  python3 tests/token_comparison_mutations.py [log path]
  R53_MUTATION_FILTER='route|store' python3 tests/token_comparison_mutations.py [log path]
  (the filter keeps mutations whose name contains any '|'-separated part)
"""
import hashlib
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/token_comparison_edges.json"
UNIT = "build/token_comparison_edges_test"
DISCOVERY = "build/fortis_node_discovery_route_test"
BINARIES = [UNIT, DISCOVERY]
ROUTE = "src/tokens/token_comparison_route.cpp"
DOMAIN = "src/tokens/token_comparison.cpp"
STORE = "src/tokens/token_comparison_store.cpp"

# (name, file, old, new)
MUTATIONS = [
    # ---- the parsers and hooks ----
    # Dropped: "route: form body 415" and "route: prototype poisoning
    # not refused": both behaviours were removed (compat-decisions.md#form-bodies,
    # #js-prototype-keys), so the mutated text no longer exists.
    # Retargeted (fastify-errors): the framing refusal is decided
    # before and answered after the device policy.
    ("route: empty JSON body parsed", ROUTE,
     "    if (!request.body_length) {\n      framing_status = 400;",
     "    if (false && !request.body_length) {\n      framing_status = 400;"),
    ("route: attestation mode ignored", ROUTE,
     "      request, framing_status ? 1 : request_cost(kind, json), route->attestation_required);",
     "      request, framing_status ? 1 : request_cost(kind, json), false);"),
    ("route: quota cost capped at 20", ROUTE,
     "  return count ? count : 1;", "  return count > 20 ? 20 : (count ? count : 1);"),
    # (A first pass made protobuf bodies count their tokens: equivalent, as a
    # protobuf body never fills the JSON document, so the cost stayed 1.)
    ("route: non-JSON bodies cost 2", ROUTE,
     "  if (kind != BodyKind::kJson) return 1;", "  if (kind != BodyKind::kJson) return 2;"),
    # ---- zod ----
    ("route: zero tokens accepted", ROUTE,
     "  *count = n;\n  return n >= 1;\n}", "  *count = n;\n  return true;\n}"),
    ("route: network bound 65 units", ROUTE,
     "constexpr size_t kMaxNetworkUnits = 64;", "constexpr size_t kMaxNetworkUnits = 65;"),
    ("route: tokenId bound 199 units", ROUTE,
     "constexpr size_t kMaxTokenIdUnits = 200;", "constexpr size_t kMaxTokenIdUnits = 199;"),
    ("route: strings not trimmed", ROUTE,
     "  fortis::codec::history_batch::JsTrim(text, length, &begin, &end);\n",
     "  end = length;\n"),
    ("route: unknown keys accepted", ROUTE,
     "    if (!json.StringEquals(key, a) && !json.StringEquals(key, b)) return false;",
     "    if (false && !json.StringEquals(key, a) && !json.StringEquals(key, b)) return false;"),
    ("route: JSON fiat not checked", ROUTE,
     "  if (fiat != codec::kBoundedJsonNoToken && !json.StringEquals(fiat, \"usd\")) return false;",
     "  if (false && fiat != codec::kBoundedJsonNoToken && !json.StringEquals(fiat, \"usd\")) return false;"),
    ("route: protobuf fiat sticky (first refusal wins)", ROUTE,
     "      fiat_usd = fiat_length == 3 && memcmp(fiat, \"usd\", 3) == 0;",
     "      fiat_usd = fiat_usd && fiat_length == 3 && memcmp(fiat, \"usd\", 3) == 0;"),
    ("route: protobuf string BOM kept", ROUTE,
     "    if (n >= 3 && start[0] == 0xef && start[1] == 0xbb && start[2] == 0xbf) { start += 3; n -= 3; }",
     "    if (false && n >= 3 && start[0] == 0xef && start[1] == 0xbb && start[2] == 0xbf) { start += 3; n -= 3; }"),
    ("route: protobuf groups not skipped", ROUTE,
     "      case 3: {\n        if (limit <= 0) return false;",
     "      case 3: {\n        if (limit <= 1000) return false;"),
    ("route: protobuf invalid UTF-8 accepted", ROUTE,
     "    if (!fortis::codec::history_batch::ValidUtf8(start, length)) return false;",
     "    if (false && !fortis::codec::history_batch::ValidUtf8(start, length)) return false;"),
    # ---- the service ----
    ("route: duplicates not detected", ROUTE,
     "        duplicate = strcmp(work->keys[k].network, network.id) == 0 &&",
     "        duplicate = false && strcmp(work->keys[k].network, network.id) == 0 &&"),
    ("route: INVALID/DUPLICATE codes swapped", ROUTE,
     "      error.duplicate = canonical;", "      error.duplicate = !canonical;"),
    ("route: store corruption read as data", ROUTE,
     "    if (store.Read(work->keys, key_count, \"usd\", now, work->records) !=\n"
     "        fortis::tokens::token_store::ReadStatus::kOk) {",
     "    if (store.Read(work->keys, key_count, \"usd\", now, work->records) ==\n"
     "        fortis::token_store::ReadStatus::kIoError) {"),
    ("route: native coin name not borrowed", ROUTE,
     "    copy_native(&work->records[k].name, work->networks[k].native_symbol);\n", ""),
    # ---- the answer ----
    ("route: cache-control max-age=60", ROUTE,
     "const char kCacheControl[] = \"private, max-age=30\";", "const char kCacheControl[] = \"private, max-age=60\";"),
    ("route: computedAt in the JSON validator", ROUTE,
     "    fortis::tokens::comparison::AppendResponseJson(validator, result, false);",
     "    fortis::tokens::comparison::AppendResponseJson(validator, result, true);"),
    ("route: computedAt in the protobuf validator", ROUTE,
     "        !fortis::tokens::comparison::AppendResponseProto(validator, result, false)) {",
     "        !fortis::tokens::comparison::AppendResponseProto(validator, result, true)) {"),
    # ---- the projection ----
    ("domain: freshness edge exclusive", DOMAIN,
     "  return age >= 0 && age <= max_age;", "  return age >= 0 && age < max_age;"),
    ("domain: future observation fresh", DOMAIN,
     "  return age >= 0 && age <= max_age;", "  return age <= max_age;"),
    ("domain: stale market never yields to fresh DEX", DOMAIN,
     "  return market.status == MetricStatus::kStale && dex.status == MetricStatus::kAvailable\n      ? dex : market;",
     "  return market;"),
    ("domain: remembered liquidity kept available after a failed check", DOMAIN,
     "    if (r.liquidity_status != LiquidityStatus::kOk) base.status = MetricStatus::kStale;", ""),
    ("domain: liquidity error read as unavailable", DOMAIN,
     "      : r.liquidity_status == LiquidityStatus::kError ? MetricStatus::kError\n",
     "      : r.liquidity_status == LiquidityStatus::kError ? MetricStatus::kUnavailable\n"),
    ("domain: rank ties not shared", DOMAIN,
     "    if (!have_previous || amount != previous) rank = static_cast<uint32_t>(i + 1);",
     "    if (!have_previous || amount != previous || true) rank = static_cast<uint32_t>(i + 1);"),
    # ---- the read model ----
    ("store: another fiat's metrics used", STORE,
     "    const bool use_metrics = m[kFiat].type == kString && m[kFiat].length == strlen(fiat) &&\n"
     "        memcmp(m[kFiat].text, fiat, m[kFiat].length) == 0;",
     "    const bool use_metrics = m[kFiat].type == kString;"),
    ("store: no coingeckoId fallback for the market id", STORE,
     "    else\n      copy_string(id[kCoingeckoId], &r.market_source_id);\n", "\n"),
]


def run(command, log, env=None):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                            encoding="utf-8", errors="replace")
    lines = [line for line in result.stdout.splitlines() if not line.startswith("c++ ")]
    shown = [line for line in lines if line.startswith("FAIL")][:6] + lines[-6:]
    log.write("\n".join(shown) + f"\n(exit {result.returncode})\n")
    log.flush()
    return result.returncode


def remove_objects(source):
    stem = Path(source).stem
    removed = [p for p in (ROOT / "build").glob(f"{stem}*.o") if p.stem == stem or p.stem.startswith(stem + "_")]
    for path in removed:
        path.unlink(missing_ok=True)
    return removed


def build(log, source):
    """Compile and link `source` afresh: its objects and both test binaries go."""
    removed = remove_objects(source)
    for binary in BINARIES:
        (ROOT / binary).unlink(missing_ok=True)
    log.write(f"removed {sorted(p.name for p in removed)} and {BINARIES}\n")
    return run(["make", "-j8"] + BINARIES, log)


def interrupted(_signum, _frame):
    """SIGTERM unwinds like Ctrl-C, so the sources are restored."""
    raise KeyboardInterrupt


def main():
    signal.signal(signal.SIGTERM, interrupted)
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "token_comparison_mutations.log"
    selected = os.environ.get("R53_MUTATION_FILTER")
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    results = []
    replay_env = dict(os.environ, COMPARISON_EDGES_FAIL_FAST="1")
    with open(log_path, "w") as log:
        log.write(f"R53 POST /v1/tokens/compare mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        for path, digest in sorted(digests.items()):
            log.write(f"  original {path} sha256 {digest}\n")
        log.write("\n")
        try:
            for index, (name, path, old, new) in enumerate(MUTATIONS, 1):
                if selected and not any(part in name for part in selected.split("|")):
                    continue
                file = ROOT / path
                original = sources[path].decode()
                if original.count(old) != 1:
                    results.append((name, "NOT APPLIED", ""))
                    log.write(f"[{index}] {name}: the original text is not unique/present ({original.count(old)})\n\n")
                    continue
                log.write(f"[{index}] {name} ({path})\n  - {old!r}\n  + {new!r}\n")
                started = time.time()
                try:
                    file.write_text(original.replace(old, new))
                    if build(log, path) != 0:
                        results.append((name, "BUILD FAILED", ""))
                        log.write("-> BUILD FAILED (not a kill)\n\n")
                        continue
                    unit = run([UNIT, FIXTURE], log)
                    replay = run(["python3", "tests/token_comparison_edges_test.py", DISCOVERY], log, replay_env)
                    by = [check for check, code in (("unit", unit), ("replay", replay)) if code != 0]
                    results.append((name, "KILLED" if by else "SURVIVED", ", ".join(by)))
                finally:
                    file.write_bytes(sources[path])
                log.write(f"-> {results[-1][1]}{' by ' + results[-1][2] if results[-1][2] else ''} "
                          f"({time.time() - started:.0f} s)\n\n")
        finally:
            for path, data in sources.items():
                (ROOT / path).write_bytes(data)
            restored = all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == d for p, d in digests.items())
            for path in sources:
                remove_objects(path)
            for binary in BINARIES + ["build/token_comparison_test", "build/fortis_node"]:
                (ROOT / binary).unlink(missing_ok=True)
            code = run(["make", "-j8"] + BINARIES + ["build/token_comparison_test", "build/fortis_node"], log)
            for path in sorted(digests):
                log.write(f"  restored {path} sha256 {hashlib.sha256((ROOT / path).read_bytes()).hexdigest()}\n")
            log.write(f"sources restored byte for byte: {restored}; restored rebuild exit {code}\n")
        log.write("\nSummary\n")
        for name, outcome, by in results:
            log.write(f"  {outcome:12} {name}{' (' + by + ')' if by else ''}\n")
        killed = sum(1 for _, outcome, _ in results if outcome == "KILLED")
        failed = sum(1 for _, outcome, _ in results if outcome == "BUILD FAILED")
        log.write(f"{killed}/{len(results)} killed, {failed} build failures (not counted as killed)\n")
    for name, outcome, by in results:
        print(f"{outcome:12} {name}{' (' + by + ')' if by else ''}")
    print(f"{sum(1 for _, o, _ in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
