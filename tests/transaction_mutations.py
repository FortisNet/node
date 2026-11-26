#!/usr/bin/env python3
"""Behavior mutations of the R26 transaction-status code against its tests.

Each mutation edits one spot of src/, deletes every object compiled from
that source (build/{stem}.o and its build/{stem}_*.o test variants) and both
checked binaries, rebuilds them (so each mutant is compiled and relinked:
source mtimes have one-second resolution), runs the check that should
notice it — the unit (build/transaction_test) or named replay groups of
tests/transaction_edges_test.py with R26_FAIL_FAST=1 — and restores the
source bytes. Outcomes: KILLED (the check failed), SURVIVED (it passed),
BUILD FAILED (the mutant did not compile; not counted as killed), NOT
APPLIED (the original text is not unique). Every mutated source's SHA-256
is recorded before and after; the sources are rebuilt at the end and the
unit and the whole replay rerun on the restored binaries.

Usage (from the repository root, with no other replay running):
  python3 tests/transaction_mutations.py [log path]
  R26_MUTATION_FILTER='substring of a name' python3 tests/transaction_mutations.py [log path]
"""
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/transaction_edges.json"
BINARY = "build/fortis_node_address_route_test"
UNIT = "build/transaction_test"

# (name, file, old, new, check) — check is "unit" or "replay:<groups>".
MUTATIONS = [
    # Dropped (opaque-params, no flag left): "route without opaque params".
    ("no raw-byte Client Error", "src/wallet/address_network_routes.cpp",
     "void handle_tx(int client, const http::HttpRequest& request, bool head, void*) {\n"
     "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;",
     "void handle_tx(int client, const http::HttpRequest& request, bool head, void*) {\n"
     "  if (reject_long_params(client, request, head)) return;", "replay:parser"),
    ("network bound 65 units", "src/wallet/address_network_routes.cpp",
     'path_param(request, "network", 64, network, sizeof(network));\n'
     '  const bool hash_ok = path_param(request, "hash", 128, hash, sizeof(hash));\n'
     '  if (!network_ok || !hash_ok) {\n'
     '    bad_request(client, head, policy);  // the schema\'s 1..64 / 1..128 units\n'
     '    return;\n  }\n  AddressTask* task = new_task(AddressTask::kTx',
     'path_param(request, "network", 65, network, sizeof(network));\n'
     '  const bool hash_ok = path_param(request, "hash", 128, hash, sizeof(hash));\n'
     '  if (!network_ok || !hash_ok) {\n'
     '    bad_request(client, head, policy);  // the schema\'s 1..64 / 1..128 units\n'
     '    return;\n  }\n  AddressTask* task = new_task(AddressTask::kTx', "replay:main"),
    ("route cost 0", "src/wallet/address_network_routes.cpp",
     "void handle_tx(int client, const http::HttpRequest& request, bool head, void*) {\n"
     "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1,",
     "void handle_tx(int client, const http::HttpRequest& request, bool head, void*) {\n"
     "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 0,", "replay:main"),
    # Dropped (opaque-params, a NUL never reaches it): "NUL network without its detail".
    ("no topic before the facet", "src/wallet/address_service.cpp",
     "      f.RememberTxTopic(arena, device, task->network, task->subject);\n"
     "      result = f.TxDetail(arena, task->network, task->subject);",
     "      result = f.TxDetail(arena, task->network, task->subject);", "replay:main"),
    ("settled record answered at 120,000 ms", "src/wallet/address_facets.cpp",
     "static_cast<int64_t>(fetched->number) > start - kSettledRecheckMs) {",
     "static_cast<int64_t>(fetched->number) >= start - kSettledRecheckMs) {", "replay:main"),
    ("pending record answered from the store", "src/wallet/address_facets.cpp",
     'if (!chains::Equals(status, "pending") && !chains::Equals(status, "unknown") && tj::IsNumber(fetched) &&',
     'if (!chains::Equals(status, "unknown") && tj::IsNumber(fetched) &&', "replay:main"),
    # Dropped (lmdb-msgpack, stored as written): "record stored without the msgpack copy".
    # Dropped (lmdb-msgpack, stored as written): "__proto__ key kept".
    ("hash key buffer 600 bytes", "src/wallet/address_facets.cpp",
     "constexpr size_t kTxEncodedBytes = 1800 + 1;", "constexpr size_t kTxEncodedBytes = 600;", "replay:main"),
    ("poll window 10 minutes", "src/wallet/address_facets.cpp",
     "constexpr int64_t kTxPollWindowMs = 15 * 60000;", "constexpr int64_t kTxPollWindowMs = 10 * 60000;",
     "replay:main"),
    ("record kept 1 day", "src/wallet/address_facets.cpp",
     "constexpr int64_t kTxDetailTtlMs = 7LL * 86400000;", "constexpr int64_t kTxDetailTtlMs = 1LL * 86400000;",
     "replay:main"),
    ("previous pending entry kept on a reread", "src/wallet/address_facets.cpp",
     "    if (tj::IsNumber(old_poll) &&\n        tx_pending_key(",
     "    if (false && tj::IsNumber(old_poll) &&\n        tx_pending_key(", "replay:main"),
    ("poller keeps the settled entry", "src/wallet/address_facets.cpp",
     "      if (tj::IsNumber(poll) &&\n          tx_pending_key(",
     "      if (false && tj::IsNumber(poll) &&\n          tx_pending_key(", "replay:main"),
    ("block number as BigInt(value)", "src/chains/chain_evm.cpp",
     'if (!StringBigIntNumber(a, tj::Get(tx, "blockNumber"), &number, error)) return false;',
     'if (!BigIntNumber(a, tj::Get(tx, "blockNumber"), &number, error)) return false;', "unit"),
    ("fractional tip is a 502", "src/chains/chain_evm.cpp",
     'if (error->kind != ErrorKind::kRange) error->Upstream("eth_blockNumber: not a BigInt");',
     'error->Upstream("eth_blockNumber: not a BigInt");', "unit"),
    ("catalogue confirmations ignored", "src/chains/chain_evm.cpp",
     "const double required = network.has_required_confirmations ? network.required_confirmations : 3;\n"
     "  const bool confirmed = has_receipt",
     "const double required = 3;\n  const bool confirmed = has_receipt", "unit"),
    # The variant above leaves `network` unused (-Werror: a build failure);
    # this one keeps it referenced.
    ("catalogue confirmations ignored (compiling)", "src/chains/chain_evm.cpp",
     "const double required = network.has_required_confirmations ? network.required_confirmations : 3;\n"
     "  const bool confirmed = has_receipt",
     "const double required = network.has_required_confirmations && false ? network.required_confirmations : 3;\n"
     "  const bool confirmed = has_receipt", "unit"),
    ("failed only when confirmed deep", "src/chains/chain_evm.cpp",
     'const bool failed = has_receipt && equals(tj::Get(receipt, "status"), "0x0");',
     'const bool failed = false && equals(tj::Get(receipt, "status"), "0x0");', "unit"),
    ("Solana object value ignored", "src/chains/chain_solana.cpp",
     '    status = tj::Get(list, "0");', "    status = nullptr;", "unit"),
    ("Solana string value whole", "src/chains/chain_solana.cpp",
     "      status = tj::Str(a, list->text, bytes);", "      status = tj::Str(a, list->text, list->length);", "unit"),
    ("Solana confirmed settles", "src/chains/chain_solana.cpp",
     'const bool finalized = Equals(confirmation, "finalized");\n  const Value* slot = tj::Get(status, "slot");\n'
     '  Value* detail = tj::Obj(a);\n  Value* out = tj::Obj(a);\n  const char* state',
     'const bool finalized = Equals(confirmation, "finalized") || Equals(confirmation, "confirmed");\n'
     '  const Value* slot = tj::Get(status, "slot");\n'
     '  Value* detail = tj::Obj(a);\n  Value* out = tj::Obj(a);\n  const char* state', "unit"),
    ("String() of null in an array is 'null'", "src/chains/chain_core.cpp",
     "    case tj::Type::kNull: return true;\n    case tj::Type::kTrue: out.Text(\"true\");",
     "    case tj::Type::kNull: out.Text(\"null\"); return out.ok;\n    case tj::Type::kTrue: out.Text(\"true\");",
     "unit"),
]


def sha(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


def run(command, log, env=None):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    lines = result.stdout.splitlines()
    shown = [line for line in lines if line.startswith("FAIL") or "error:" in line][:4] + lines[-4:]
    log.write("\n".join(shown) + f"\n(exit {result.returncode})\n")
    log.flush()
    return result.returncode


def objects_of(path):
    stem = Path(path).stem
    return [ROOT / f"build/{stem}.o"] + sorted((ROOT / "build").glob(f"{stem}_*.o"))


def build(log, paths):
    """Delete every object built from `paths` and both binaries, rebuild,
    and confirm each deleted object and binary was produced again."""
    removed = []
    for path in paths:
        for obj in objects_of(path):
            if obj.exists():
                obj.unlink()
                removed.append(obj)
    for binary in (BINARY, UNIT):
        (ROOT / binary).unlink(missing_ok=True)
    started = time.time()
    code = run(["make", "-j8", BINARY, UNIT], log)
    if code == 0:
        rebuilt = [p for p in removed + [ROOT / BINARY, ROOT / UNIT] if p.exists() and p.stat().st_mtime >= started - 1]
        log.write(f"rebuilt {len(rebuilt)}/{len(removed) + 2}: {', '.join(p.name for p in rebuilt)}\n")
        if len(rebuilt) != len(removed) + 2:
            return 1
    return code


def main():
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "transaction_mutations.log"
    selected = os.environ.get("R26_MUTATION_FILTER")
    results = []
    sources = sorted({path for _, path, _, _, _ in MUTATIONS})
    before = {path: sha(path) for path in sources}
    with open(log_path, "w") as log:
        log.write(f"R26 transaction mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        for path in sources:
            log.write(f"  before {before[path]}  {path}\n")
        log.write("\n")
        try:
            for name, path, old, new, check in MUTATIONS:
                if selected and selected not in name:
                    continue
                file = ROOT / path
                original = file.read_bytes()
                text = original.decode("utf-8")
                if text.count(old) != 1:
                    results.append((name, "NOT APPLIED"))
                    log.write(f"== {name}: the original text is not unique/present\n\n")
                    continue
                log.write(f"== {name} ({path}; {check})\n")
                try:
                    file.write_bytes(text.replace(old, new).encode("utf-8"))
                    if build(log, [path]) != 0:
                        results.append((name, "BUILD FAILED"))
                        continue
                    if check == "unit":
                        code = run([UNIT, FIXTURE], log)
                    else:
                        env = dict(os.environ, R26_ONLY_GROUPS=check.split(":", 1)[1], R26_FAIL_FAST="1")
                        code = run(["python3", "tests/transaction_edges_test.py", BINARY], log, env)
                    results.append((name, "KILLED" if code != 0 else "SURVIVED"))
                finally:
                    file.write_bytes(original)
                    log.write(f"-> {results[-1][1]}; restored {sha(path)}\n\n")
        finally:
            after = {path: sha(path) for path in sources}
            for path in sources:
                log.write(f"  after  {after[path]}  {path} {'(restored)' if after[path] == before[path] else 'CHANGED'}\n")
            log.write("\nRestored build\n")
            restored_build = build(log, sources)
            unit = run([UNIT, FIXTURE], log) if restored_build == 0 else 1
            replay = run(["python3", "tests/transaction_edges_test.py", BINARY], log) if restored_build == 0 else 1
            log.write(f"restored binaries: build exit {restored_build}, unit exit {unit}, replay exit {replay}\n\n")
            log.write("Summary\n")
            for name, outcome in results:
                log.write(f"  {outcome:12} {name}\n")
            counts = {k: sum(1 for _, o in results if o == k) for k in ("KILLED", "SURVIVED", "BUILD FAILED",
                                                                          "NOT APPLIED")}
            log.write(f"{counts['KILLED']} killed, {counts['SURVIVED']} survived, {counts['BUILD FAILED']} build "
                      f"failed, {counts['NOT APPLIED']} not applied, of {len(results)}\n")
    for name, outcome in results:
        print(f"{outcome:12} {name}")
    print(f"{sum(1 for _, o in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
