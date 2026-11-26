#!/usr/bin/env python3
"""Behaviour mutations of the R43 prepare, R44 submit and R45 diagnose
handlers (POST /v1/networks/robinhood/sponsored-trades/{prepare,submit},
POST /v1/networks/robinhood/transactions/diagnose) against their tests
(tests/robinhood_writes_test.cpp, the unit oracle check, and
tests/robinhood_writes_edges_test.py, the native socket replay of the pinned
TypeScript fixtures).

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and its build/<stem>_*.o variants)
and every binary the checks run (the Robinhood route test binary and the
unit test), rebuilds them, runs the check that should notice it (the unit
test, or named replay groups of one fixture with R43_R45_FAIL_FAST=1) and
restores the source. Deleting the objects and binaries forces the compile
and the relink of the changed source (with one-second mtimes a same-second
mutant would not be recompiled otherwise). A mutation is KILLED when its
check fails, SURVIVED when it passes, BUILD FAILED when it does not compile
or link (never counted as killed). Every source is restored, compared with
its original bytes and rebuilt at the end (also after an interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/robinhood_writes_mutations.py [log path]
  R43_R45_MUTATION_FILTER='R44' python3 tests/robinhood_writes_mutations.py [log path]
  R43_R45_MUTATION_DRY=1 … only checks that every mutation applies (no build)
"""
import hashlib
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREPARE_FIXTURE = "tests/corpus/robinhood_prepare_edges.json"
SUBMIT_FIXTURE = "tests/corpus/robinhood_submit_edges.json"
DIAGNOSE_FIXTURE = "tests/corpus/robinhood_diagnose_edges.json"
ROUTE_BINARY = "build/fortis_node_robinhood_route_test"
UNIT = "build/robinhood_writes_test"
BINARIES = [ROUTE_BINARY, UNIT]
SERVICE = "src/exchange/robinhood_service.cpp"
TICKETS = "src/exchange/robinhood_tickets.cpp"
POLICY = "src/exchange/robinhood_policy.cpp"
ROUTES = "src/exchange/robinhood_routes.cpp"
REQUESTS = "src/exchange/robinhood_write_requests.cpp"
AUTH = "AuthorizeDeviceRequest(request, 20, config.attestation_required);"

# (name, file, old, new, check) — check is "unit" or "replay:<prepare|submit|diagnose>:<groups>".
MUTATIONS = [
    # ---- R43 POST …/sponsored-trades/prepare ----
    ("R43 quota cost 1 for prepare", SERVICE, AUTH,
     "AuthorizeDeviceRequest(request, kind == Kind::kPrepare ? 1 : 20, config.attestation_required);",
     "replay:prepare:main"),
    ("R43 prepare needs only the Alchemy key", ROUTES,
     "if (kind == Kind::kPrepare && !sponsored_enabled) return answer(sponsored_unavailable(policy));",
     "if (kind == Kind::kPrepare && !diagnostics_enabled) return answer(sponsored_unavailable(policy));",
     "replay:prepare:no-policy"),
    ("R43 sender not lowercased", SERVICE,
     "  Value* from_lower = cc::Lower(arena, from);",
     "  Value* from_lower = const_cast<Value*>(from);", "replay:prepare:main"),
    ("R43 ticket lifetime + 1 s", SERVICE,
     "const int64_t expires = now + rh::TicketStore::kTtlMs;",
     "const int64_t expires = now + rh::TicketStore::kTtlMs + 1000;", "replay:prepare:main"),
    ("R43 protobuf call bounded to its own length (old decoder)", REQUESTS,
     "  Value* call = tj::Obj(arena);\n  if (!call) return nullptr;\n  const int last = fees ? 6 : 3;",
     "  if (reader.pos() != end) return nullptr;\n  Value* call = tj::Obj(arena);\n  if (!call) return nullptr;\n"
     "  const int last = fees ? 6 : 3;", "replay:prepare:main"),
    ("R43 prepare keeps protobuf gas fields", REQUESTS,
     "  const int last = fees ? 6 : 3;", "  const int last = fees ? 6 : 6;", "unit"),
    ("R43 prepared sender not checked", POLICY,
     "      if (!is_record(data) || !tj::IsString(sender) || !lower_equals(sender->text, sender->length, from_lower))",
     "      if (!is_record(data) || !tj::IsString(sender) ||\n"
     "          (from_lower == nullptr && !lower_equals(sender->text, sender->length, from_lower)))",
     "replay:prepare:main"),
    ("R43 gas policy id missing from wallet_prepareCalls", SERVICE,
     "RobinhoodPrepareParams(arena, from_lower, executable, config.alchemy_gas_policy_id);",
     "RobinhoodPrepareParams(arena, from_lower, executable, \"\");", "replay:prepare:main"),
    ("R43 call value default omitted", POLICY,
     ': arena.CopyText("0x0", 3);', ': arena.CopyText("0x", 2);', "unit"),
    ("R43 cap answered before Alchemy", SERVICE,
     "  outcome = Rpc(job, kWalletBase, wallet_path, \"alchemy-wallet\", \"wallet_prepareCalls\", params, &body);",
     "  if (tickets.Pending(Now()) >= rh::TicketStore::kCapacity) {\n"
     "    Complete(job, sponsored_unavailable(job->policy));\n"
     "    return;\n"
     "  }\n"
     "  outcome = Rpc(job, kWalletBase, wallet_path, \"alchemy-wallet\", \"wallet_prepareCalls\", params, &body);",
     "replay:prepare:cap"),
    # ---- R44 POST …/sponsored-trades/submit ----
    ("R44 quota cost 1 for submit", SERVICE, AUTH,
     "AuthorizeDeviceRequest(request, kind == Kind::kSubmit ? 1 : 20, config.attestation_required);",
     "replay:submit:main"),
    ("R44 ticket not bound to its device", TICKETS,
     "if (!slot || strcmp(slot->device_id, device_id) != 0 || slot->expires_at_ms <= now_ms || slot->in_flight) {",
     "if (!slot || (device_id == nullptr && strcmp(slot->device_id, device_id) != 0) || "
     "slot->expires_at_ms <= now_ms || slot->in_flight) {", "replay:submit:main"),
    ("R44 expiry one millisecond late", ROUTES,
     "switch (tickets.Begin(ticket->text, authorized.device.id, Now(), signatures->count, &claim)) {",
     "switch (tickets.Begin(ticket->text, authorized.device.id, Now() - 1, signatures->count, &claim)) {",
     "replay:submit:main"),
    ("R44 delivered ticket kept (not single use)", SERVICE,
     "  tickets.Finish(job->claim, true);", "  tickets.Finish(job->claim, false);", "replay:submit:main"),
    ("R44 in-flight ticket claimable twice", TICKETS,
     "if (!slot || strcmp(slot->device_id, device_id) != 0 || slot->expires_at_ms <= now_ms || slot->in_flight) {",
     "if (!slot || strcmp(slot->device_id, device_id) != 0 || slot->expires_at_ms <= now_ms) {",
     "replay:submit:concurrent"),
    ("R44 fewer signatures than nodes accepted", TICKETS,
     "  if (signature_count != slot->node_count) {", "  if (signature_count > slot->node_count) {",
     "replay:submit:main"),
    ("R44 failed send keeps the claim", SERVICE,
     "    // A job that never reached Alchemy leaves its ticket claimable.\n"
     "    tickets.Finish(job->claim, false);",
     "    // A job that never reached Alchemy leaves its ticket claimable.", "replay:submit:main"),
    ("R44 submit needs the gas policy", ROUTES,
     "    if (!diagnostics_enabled) return answer(sponsored_unavailable(policy));",
     "    if (!sponsored_enabled) return answer(sponsored_unavailable(policy));", "replay:submit:no-policy"),
    ("R44 repeated protobuf ticket keeps the first", REQUESTS,
     "    if (field == 1) {\n      ticket = read_string(reader, arena);",
     "    if (field == 1 && !ticket->length) {\n      ticket = read_string(reader, arena);", "replay:submit:main"),
    ("R44 signature type", POLICY,
     'set(arena, value, "type", str(arena, "secp256k1"))', 'set(arena, value, "type", str(arena, "secp256k1r"))',
     "unit"),
    # ---- R45 POST …/transactions/diagnose ----
    ("R45 quota cost 1 for diagnose", SERVICE, AUTH,
     "AuthorizeDeviceRequest(request, kind == Kind::kDiagnose ? 1 : 20, config.attestation_required);",
     "replay:diagnose:main"),
    ("R45 diagnose needs the gas policy", ROUTES,
     "  if (kind == Kind::kDiagnose && !diagnostics_enabled)",
     "  if (kind == Kind::kDiagnose && !sponsored_enabled)", "replay:diagnose:no-policy"),
    ("R45 validation flag not inverted", REQUESTS,
     'tj::Set(arena, options, "validation", tj::Bool(arena, !sponsored))',
     'tj::Set(arena, options, "validation", tj::Bool(arena, sponsored))', "unit"),
    ("R45 protobuf sponsored ignored", REQUESTS,
     "    } else if (field == 3 && diagnose) {", "    } else if (field == 3 && !diagnose) {", "unit"),
    ("R45 sender lowercased before the policy", SERVICE,
     "  const Value* executable = rh::ValidateTradeCalls(arena, calls, stocks, from->text, from->length, &exhausted);",
     "  Value* lowered = cc::Lower(arena, from);\n"
     "  const Value* executable = rh::ValidateTradeCalls(arena, calls, stocks, lowered->text, lowered->length, &exhausted);",
     "replay:diagnose:main"),
    ("R45 null trace counted as available", SERVICE,
     "          result->type != tj::Type::kNull)", "          true)", "replay:diagnose:main"),
    ("R45 blank RPC message kept as the reason", SERVICE,
     "    if (!reason && !cc::Blank(message->text, message->length)) reason = message;",
     "    if (!reason) reason = message;", "replay:diagnose:main"),
    ("R45 fee summed instead of multiplied", POLICY,
     '!fee.Mul(gas_used[i]) || !gas_total.Add(gas_used[i])', '!fee.Add(gas_used[i]) || !gas_total.Add(gas_used[i])',
     "unit"),
    ("R45 41 internal calls", POLICY,
     "  if (!state.ok || !is_record(value) || state.out->count >= 40) return;",
     "  if (!state.ok || !is_record(value) || state.out->count >= 41) return;", "unit"),
    ("R45 simulatedAt off the route clock", SERVICE,
     "  request.sponsored = job->sponsored;\n"
     "  request.stocks = &stocks;\n"
     "  request.now_ms = Now();",
     "  request.sponsored = job->sponsored;\n"
     "  request.stocks = &stocks;\n"
     "  request.now_ms = Now() + 1;",
     "replay:diagnose:main"),
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
        return [UNIT, PREPARE_FIXTURE, SUBMIT_FIXTURE, DIAGNOSE_FIXTURE], None
    _, kind, groups = check.split(":", 2)
    env = dict(os.environ, R43_R45_FAIL_FAST="1", R43_R45_FIXTURES=kind, R43_R45_GROUPS=groups,
               R43_R45_SKIP_NATIVE_ONLY="1")
    return ["python3", "tests/robinhood_writes_edges_test.py", ROUTE_BINARY], env


def interrupted(_signum, _frame):
    """SIGTERM unwinds like Ctrl-C, so the sources are restored."""
    raise KeyboardInterrupt


def main():
    signal.signal(signal.SIGTERM, interrupted)
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "robinhood_writes_mutations.log"
    selected = os.environ.get("R43_R45_MUTATION_FILTER")
    dry = os.environ.get("R43_R45_MUTATION_DRY") == "1"
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    if dry:
        bad = [(name, sources[path].decode().count(old)) for name, path, old, new, _ in MUTATIONS
               if sources[path].decode().count(old) != 1 or old == new]
        print(f"{len(MUTATIONS)} mutations; not uniquely applicable: {bad}")
        return
    results = []
    with open(log_path, "w") as log:
        log.write(f"R43–R45 Robinhood transaction POST mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
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
        for route in ("R43", "R44", "R45"):
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
