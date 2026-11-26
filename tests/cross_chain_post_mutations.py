#!/usr/bin/env python3
"""Behaviour mutations of the R41 cross-chain quotes and R42 cross-chain
status handlers against their tests (tests/cross_chain_post_test.cpp, the
unit oracle check, and tests/cross_chain_post_edges_test.py, the native
socket replay of the pinned TypeScript fixtures).

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and its build/<stem>_*.o variants)
and every binary the checks run (the cross-chain route test binary and the
unit test), rebuilds them, runs the check that should notice it (the unit
test, or named replay groups of one fixture with R41_R42_FAIL_FAST=1) and
restores the source. Deleting the objects and binaries forces the compile
and the relink of the changed source (with one-second mtimes a same-second
mutant would not be recompiled otherwise). A mutation is KILLED when its
check fails, SURVIVED when it passes, BUILD FAILED when it does not compile
or link (never counted as killed). Every source is restored, compared with
its original bytes and rebuilt at the end (also after an interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/cross_chain_post_mutations.py [log path]
  R41_R42_MUTATION_FILTER='R41' python3 tests/cross_chain_post_mutations.py [log path]
  R41_R42_MUTATION_DRY=1 … only checks that every mutation applies (no build)
"""
import hashlib
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
QUOTES_FIXTURE = "tests/corpus/cross_chain_quotes_edges.json"
STATUS_FIXTURE = "tests/corpus/cross_chain_status_edges.json"
ROUTE_BINARY = "build/fortis_node_cross_chain_route_test"
UNIT = "build/cross_chain_post_test"
BINARIES = [ROUTE_BINARY, UNIT]
SERVICE = "src/exchange/cross_chain_service.cpp"
SUPPORT = "src/discovery/discovery_route_support.cpp"
QUOTE_AUTH = """AuthorizeDeviceRequest(request, 10, config.attestation_required);
    if (reply_policy(client, authorized, head)) { tj::FreeArena(arena); return; }
    const Policy policy = policy_of(authorized);
    CrossChainQuotePlan plan;"""
STATUS_AUTH = """AuthorizeDeviceRequest(request, 10, config.attestation_required);
    if (reply_policy(client, authorized, head)) { tj::FreeArena(arena); return; }
    const Policy policy = policy_of(authorized);
    CrossChainStatusPlan plan;"""

# (name, file, old, new, check) — check is "unit" or "replay:<quotes|status>:<groups>".
MUTATIONS = [
    # ---- R41 POST /v1/exchange/cross-chain/quotes ----
    ("R41 prepare counted after de-duplication", SERVICE,
     "if (q.prepare && !(out->has_providers && out->provider_count == 1)) return false;",
     "if (q.prepare && !(out->has_providers && out->provider_count >= 1)) return false;", "unit"),
    ("R41 providers not de-duplicated", SERVICE,
     "seen = cc::SameText(unique[k], parsed.providers[i]);",
     "seen = false && cc::SameText(unique[k], parsed.providers[i]);", "replay:quotes:main"),
    ("R41 only the first unknown provider listed", SERVICE,
     "if (index < 0) plan->unknown[plan->unknown_count++] = unique[i];",
     "if (index < 0 && !plan->unknown_count) plan->unknown[plan->unknown_count++] = unique[i];", "unit"),
    ("R41 slippage default 0", SERVICE, "  q.slippage_bps = 50;\n", "  q.slippage_bps = 0;\n", "replay:quotes:main"),
    ("R41 protobuf slippage 0 read as absent", SERVICE,
     "      if (!reader.Uint32(&slippage)) return nullptr;\n      has_slippage = true;",
     "      if (!reader.Uint32(&slippage)) return nullptr;\n      has_slippage = slippage != 0;", "unit"),
    ("R41 protobuf endpoint bounded to its own length", SERVICE,
     "  }\n  return true;\n}\n\nValue* endpoint_object",
     "  }\n  return reader.pos() == end;\n}\n\nValue* endpoint_object", "replay:quotes:main"),
    ("R41 shared protobuf reader keeps a leading BOM", SUPPORT,
     "  if (size >= 3 && data_[start] == 0xef && data_[start + 1] == 0xbb && data_[start + 2] == 0xbf) {",
     "  if (size >= 3 && data_[start] == 0xef && data_[start + 1] == 0xbb && data_[start + 2] == 0xbe) {", "unit"),
    ("R41 unknown fee ranks first", SERVICE,
     "const double fee_a = tj::IsNumber(fa) ? fa->number : INFINITY;",
     "const double fee_a = tj::IsNumber(fa) ? fa->number : 0;", "replay:quotes:main"),
    ("R41 unstable sort on ties", SERVICE,
     "while (k > 0 && compare_routes(routes->items[k - 1], item) > 0) {",
     "while (k > 0 && compare_routes(routes->items[k - 1], item) >= 0) {", "replay:quotes:main"),
    ("R41 empty bestRouteId is a string", SERVICE,
     "ranked->count ? tj::Get(ranked->items[0], \"id\") : tj::Null(arena)",
     "ranked->count ? tj::Get(ranked->items[0], \"id\") : tj::Str(arena, \"\")", "replay:quotes:main"),
    ("R41 unconfigured providers run", SERVICE,
     "job->configured[i] = cc::Describe(settings, plan.providers[i]).configured;",
     "job->configured[i] = true;", "replay:quotes:unconfigured"),
    ("R41 quota cost 1", SERVICE, "AuthorizeDeviceRequest(request, 10, config.attestation_required);\n"
     "    if (reply_policy(client, authorized, head)) { tj::FreeArena(arena); return; }\n"
     "    const Policy policy = policy_of(authorized);\n    CrossChainQuotePlan plan;",
     "AuthorizeDeviceRequest(request, 1, config.attestation_required);\n"
     "    if (reply_policy(client, authorized, head)) { tj::FreeArena(arena); return; }\n"
     "    const Policy policy = policy_of(authorized);\n    CrossChainQuotePlan plan;", "replay:quotes:quota"),
    ("R41 200 without vary", SERVICE,
     "        response = http::HttpResponse{200, \"OK\", job->protobuf ? kProtobufType : kJsonType, buffer, out.used};\n"
     "        response.vary_accept = true;",
     "        response = http::HttpResponse{200, \"OK\", job->protobuf ? kProtobufType : kJsonType, buffer, out.used};\n"
     "        response.vary_accept = false;", "replay:quotes:main"),
    ("R41 provider deadline 11 s", SERVICE, "constexpr uint32_t kProviderDeadlineMs = 25000;",
     "constexpr uint32_t kProviderDeadlineMs = 11000;", "replay:quotes:failure"),
    # ---- R42 POST /v1/exchange/cross-chain/status ----
    # Dropped: "R42 form bodies refused" and "R42 form __proto__ key
    # fails .strict()": form bodies are 415 for every route and `__proto__`
    # is an ordinary key (compat-decisions.md#form-bodies, #js-prototype-keys),
    # so the mutated text no longer exists.
    ("R42 bridge label reaches every provider", SERVICE,
     'if (!out->bridge_id && cc::Is(provider, "squid")) out->bridge_id = bridge;',
     "if (!out->bridge_id) out->bridge_id = bridge;", "unit"),
    ("R42 toSymbol refuses '+'", SERVICE, "c == '.' || c == '_' ||\n            c == '+' || c == '-'))",
     "c == '.' || c == '_' ||\n            c == '-'))", "replay:status:main"),
    ("R42 toDecimals bound 37", SERVICE, 'if (!integer_in(decimals, 0, 36, &out->to_decimals)) return false;',
     'if (!integer_in(decimals, 0, 37, &out->to_decimals)) return false;', "unit"),
    ("R42 empty optional protobuf string absent", SERVICE,
     "      values[field] = read_string(reader, arena);\n      if (!values[field]) return nullptr;",
     "      values[field] = read_string(reader, arena);\n      if (!values[field]) return nullptr;\n"
     "      if (field >= 5 && !values[field]->length) values[field] = nullptr;", "replay:status:main"),
    ("R42 unknown provider lists ids", SERVICE,
     'send(client, head, 400, "Bad Request", "{\\"error\\":\\"UNKNOWN_CROSS_CHAIN_PROVIDER\\"}", policy);',
     'send(client, head, 400, "Bad Request", "{\\"error\\":\\"UNKNOWN_CROSS_CHAIN_PROVIDER\\",\\"providers\\":[]}", policy);',
     "replay:status:main"),
    ("R42 unconfigured provider asked", SERVICE, "    if (!descriptor.configured) {\n      char text[128];",
     "    if (false && !descriptor.configured) {\n      char text[128];", "replay:status:unconfigured"),
    ("R42 lane back-pressure answered 502", SERVICE, "if (outcome.failure == cc::Failure::kLaneBusy) {",
     "if (false && outcome.failure == cc::Failure::kLaneBusy) {", "replay:status:failure"),
    ("R42 quota cost 1", SERVICE, "AuthorizeDeviceRequest(request, 10, config.attestation_required);\n"
     "    if (reply_policy(client, authorized, head)) { tj::FreeArena(arena); return; }\n"
     "    const Policy policy = policy_of(authorized);\n    CrossChainStatusPlan plan;",
     "AuthorizeDeviceRequest(request, 1, config.attestation_required);\n"
     "    if (reply_policy(client, authorized, head)) { tj::FreeArena(arena); return; }\n"
     "    const Policy policy = policy_of(authorized);\n    CrossChainStatusPlan plan;", "replay:status:quota"),
]


def run(command, log, env=None, timeout=1800):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    try:
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                                encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        log.write(f"(timeout after {timeout} s)\n")
        return 124
    lines = [line for line in result.stdout.splitlines() if not line.startswith("c++ ")]
    log.write("\n".join(lines[-12:]) + f"\n(exit {result.returncode})\n")
    log.flush()
    return result.returncode


def remove_objects(source):
    stem = Path(source).stem
    removed = [p for p in (ROOT / "build").glob(f"{stem}*.o") if p.stem == stem or p.stem.startswith(stem + "_")]
    for path in removed:
        path.unlink(missing_ok=True)
    return removed


def build(log, source):
    """Compile and link `source` afresh: its objects and every checked binary go."""
    removed = remove_objects(source)
    for binary in BINARIES:
        (ROOT / binary).unlink(missing_ok=True)
    log.write(f"removed {sorted(p.name for p in removed)} and {len(BINARIES)} binaries\n")
    return run(["make", "-j8"] + BINARIES, log)


def check_command(check):
    if check == "unit":
        return [UNIT, QUOTES_FIXTURE, STATUS_FIXTURE], None
    _, kind, groups = check.split(":", 2)
    env = dict(os.environ, R41_R42_FAIL_FAST="1", R41_R42_ONLY=kind, R41_R42_GROUPS=groups,
               R41_R42_NATIVE_ONLY="none")
    return ["python3", "tests/cross_chain_post_edges_test.py", ROUTE_BINARY], env


def interrupted(_signum, _frame):
    """SIGTERM unwinds like Ctrl-C, so the sources are restored."""
    raise KeyboardInterrupt


def main():
    signal.signal(signal.SIGTERM, interrupted)
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "cross_chain_post_mutations.log"
    selected = os.environ.get("R41_R42_MUTATION_FILTER")
    dry = os.environ.get("R41_R42_MUTATION_DRY") == "1"
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    if dry:
        bad = [(name, sources[path].decode().count(old)) for name, path, old, new, _ in MUTATIONS
               if sources[path].decode().count(old) != 1 or old == new]
        print(f"{len(MUTATIONS)} mutations; not uniquely applicable: {bad}")
        return
    results = []
    with open(log_path, "w") as log:
        log.write(f"R41–R42 cross-chain POST mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        for path, digest in sorted(digests.items()):
            log.write(f"  original {path} sha256 {digest}\n")
        log.write("\n")
        try:
            for name, path, old, new, check in MUTATIONS:
                if selected and not any(part in name for part in selected.split("|")):
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
                    command, env = check_command(check)
                    code = run(command, log, env)
                    results.append((name, "KILLED" if code != 0 else "SURVIVED"))
                finally:
                    file.write_bytes(sources[path])
                log.write(f"-> {results[-1][1]} ({time.time() - started:.0f} s)\n\n")
        finally:
            for path, data in sources.items():
                (ROOT / path).write_bytes(data)
            restored = all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == d for p, d in digests.items())
            for path in sources:
                remove_objects(path)
            for binary in BINARIES:
                (ROOT / binary).unlink(missing_ok=True)
            code = run(["make", "-j8"] + BINARIES + ["build/fortis_node"], log)
            for path in sorted(digests):
                log.write(f"  restored {path} sha256 {hashlib.sha256((ROOT / path).read_bytes()).hexdigest()}\n")
            log.write(f"sources restored byte for byte: {restored}; restored rebuild exit {code}\n")
        log.write("Summary\n")
        for name, outcome in results:
            log.write(f"  {outcome:12} {name}\n")
        for route in ("R41", "R42"):
            mine = [outcome for name, outcome in results if name.startswith(route)]
            killed = sum(1 for outcome in mine if outcome == "KILLED")
            failed = sum(1 for outcome in mine if outcome == "BUILD FAILED")
            survived = sum(1 for outcome in mine if outcome == "SURVIVED")
            log.write(f"{route}: {killed}/{len(mine)} killed, {survived} survived, {failed} build failures "
                      f"(not counted as killed)\n")
    for name, outcome in results:
        print(f"{outcome:12} {name}")
    print(f"{sum(1 for _, o in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
