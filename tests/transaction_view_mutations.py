#!/usr/bin/env python3
"""Behavior mutations of the R27 transaction-view code against its tests.

Each mutation edits one source of src/ (one or more exact, unique
replacements), deletes every object compiled from that source
(build/{stem}.o and its build/{stem}_*.o test variants) and both checked
binaries, rebuilds them (so each mutant is compiled and relinked: source
mtimes have one-second resolution), runs the check that should notice it --
the unit (build/transaction_view_test) or named replay groups of
tests/transaction_view_edges_test.py with R27_FAIL_FAST=1 -- and restores
the source bytes. Outcomes: KILLED (the check failed), SURVIVED (it passed),
BUILD FAILED (the mutant did not compile or link; never counted as killed),
NOT APPLIED (an original text is not unique). Every mutated source's
SHA-256 is recorded before and after; the sources are rebuilt at the end
and the unit and the whole replay rerun on the restored binaries.

Usage (from the repository root, with no other replay running):
  python3 tests/transaction_view_mutations.py [log path]
  R27_MUTATION_FILTER='substring of a name' python3 tests/transaction_view_mutations.py [log path]
"""
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/transaction_view_edges.json"
BINARY = "build/fortis_node_address_route_test"
UNIT = "build/transaction_view_test"

VIEW_HEAD = "void handle_view(int client, const http::HttpRequest& request, bool head, void*) {\n"

# (name, file, [(old, new), ...], check) -- check is "unit" or "replay:<groups>".
MUTATIONS = [
    # Dropped (opaque-params, no flag left): "route without opaque params".
    ("no raw-byte Client Error", "src/wallet/address_network_routes.cpp",
     [(VIEW_HEAD + "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) "
       "return;", VIEW_HEAD + "  if (reject_long_params(client, request, head)) return;")], "replay:parser"),
    ("route cost 0", "src/wallet/address_network_routes.cpp",
     [(VIEW_HEAD + "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) "
       "return;\n  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1,",
       VIEW_HEAD + "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) "
       "return;\n  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 0,")], "replay:main"),
    ("network bound 65 units", "src/wallet/address_network_routes.cpp",
     [('path_param(request, "network", 64, network, sizeof(network));\n'
       '  const bool hash_ok = path_param(request, "hash", 128, hash, sizeof(hash));\n'
       '  if (!network_ok || !hash_ok) {\n'
       '    bad_request(client, head, policy);  // the schema\'s 1..64 / 1..128 units\n'
       '    return;\n  }\n  // The registry and family checks',
       'path_param(request, "network", 65, network, sizeof(network));\n'
       '  const bool hash_ok = path_param(request, "hash", 128, hash, sizeof(hash));\n'
       '  if (!network_ok || !hash_ok) {\n'
       '    bad_request(client, head, policy);  // the schema\'s 1..64 / 1..128 units\n'
       '    return;\n  }\n  // The registry and family checks')], "replay:main"),
    ("unknown network echoes the network", "src/wallet/address_network_routes.cpp",
     [('  if (!g_route.impl->catalog.Resolve(network, strlen(network), &resolved)) {\n'
       '    bad_request(client, head, policy, "unknown network");',
       '  if (!g_route.impl->catalog.Resolve(network, strlen(network), &resolved)) {\n'
       '    bad_request(client, head, policy, network);')], "replay:main"),
    ("unsupported family as BAD_REQUEST", "src/wallet/address_network_routes.cpp",
     [('    send_error(client, 400, "Bad Request", "UNSUPPORTED", detail, head,',
       '    send_error(client, 400, "Bad Request", "BAD_REQUEST", detail, head,')], "replay:main"),
    ("family check after the topic", "src/wallet/address_network_routes.cpp",
     [('  if (!fortis::chains::ViewFor(resolved.family)) {\n    char detail[128];',
       '  if (false && !fortis::chains::ViewFor(resolved.family)) {\n    char detail[128];')], "replay:main"),
    ("no topic before the view", "src/wallet/address_service.cpp",
     [("      f.RememberTxTopic(arena, device, task->network, task->subject);\n"
       "      result = f.TxView(arena, task->network, task->subject);",
       "      result = f.TxView(arena, task->network, task->subject);")], "replay:main"),
    # Dropped (opaque-params, a NUL never reaches it): "NUL hash read with the truncated hash".
    ("pending view stored", "src/wallet/address_facets.cpp",
     [("  fetcher.context = &context;\n  fetcher.should_store = view_should_store;\n  fetcher.revalidation = "
       "view_revalidation;", "  fetcher.context = &context;\n  fetcher.revalidation = view_revalidation;")],
     "replay:main"),
    ("revalidated pending view stored", "src/wallet/address_facets.cpp",
     [("      fetcher.should_store = view_should_store;\n      impl_->Revalidate(job->key, kTtlTxDetail, fetcher, "
       "arena);", "      impl_->Revalidate(job->key, kTtlTxDetail, fetcher, arena);")], "replay:main"),
    ("view fresh for 2 hours", "src/wallet/address_facets.cpp",
     [("  if (!impl_->SwrRead(key, kTtlTxDetail, fetcher, arena, &swr)) {",
       "  if (!impl_->SwrRead(key, token_swr::Ttl{7200000, 86400000}, fetcher, arena, &swr)) {")], "replay:main"),
    ("unknown hash held as a 502", "src/wallet/address_facets.cpp",
     [("  if (unknown) return 404;", "  if (unknown) return 502;")], "replay:main"),
    ("pending leader not shared with followers", "src/wallet/address_facets.cpp",
     [("    if (waited && TakeShared(generation, arena, &value)) {",
       "    if (false && waited && TakeShared(generation, arena, &value)) {")], "replay:main"),
    ("envelope key txd:{network}/{hash}", "src/wallet/address_facets.cpp",
     [('  return key_text(out, capacity, "txd:%s:%s", network, hash);',
       '  return key_text(out, capacity, "txd:%s/%s", network, hash);')], "unit"),
    ("renewal memo direct-mapped on FNV low bits", "src/wallet/address_store.cpp",
     [("&recent_topics_[(mixed % (kRecentTopics / kRecentTopicWays)) * kRecentTopicWays]",
       "&recent_topics_[(hash % (kRecentTopics / kRecentTopicWays)) * kRecentTopicWays + 0 * mixed]"),
      ("  for (size_t way = 0; set && way < kRecentTopicWays && !slot; ++way)",
       "  for (size_t way = 0; set && way < 1 && !slot; ++way)"),
      ("      slot = &set[0];\n      for (size_t way = 0; way < kRecentTopicWays; ++way) {",
       "      slot = &set[0];\n      for (size_t way = 0; way < 1; ++way) {")], "replay:main"),
    ("Blockscout txhash sent raw", "src/chains/chain_evm_view.cpp",
     [("  const char* path = raw_path ? FetchWirePath(a, raw_path->text) : nullptr;",
       "  const char* path = raw_path ? raw_path->text : nullptr;")], "replay:main"),
    ("EVM view always final", "src/chains/chain_evm_view.cpp",
     [('bool evm_view_final(const Value* detail, const Network&) { return !nullish(tj::Get(detail, "blockNumber")); }',
       'bool evm_view_final(const Value* detail, const Network&) { return detail != nullptr; }')], "unit"),
    ("UTXO view final at 2 confirmations", "src/chains/chain_utxo_view.cpp",
     [("  const double required = network.has_required_confirmations ? network.required_confirmations : 1;",
       "  const double required = network.has_required_confirmations ? network.required_confirmations : 2;")],
     "unit"),
    ("inclusion needs a fee above the floor", "src/chains/chain_utxo_view.cpp",
     [("    if (!*has_blocks && fee_rate >= min) {", "    if (!*has_blocks && fee_rate > min) {")], "unit"),
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
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "transaction_view_mutations.log"
    selected = os.environ.get("R27_MUTATION_FILTER")
    results = []
    sources = sorted({path for _, path, _, _ in MUTATIONS})
    before = {path: sha(path) for path in sources}
    with open(log_path, "a" if selected else "w") as log:
        log.write(f"R27 transaction view mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}"
                  f"{f' (filter {selected!r})' if selected else ''}\n")
        for path in sources:
            log.write(f"  before {before[path]}  {path}\n")
        log.write("\n")
        try:
            for name, path, edits, check in MUTATIONS:
                if selected and selected not in name:
                    continue
                file = ROOT / path
                original = file.read_bytes()
                text = original.decode("utf-8")
                if any(text.count(old) != 1 for old, _ in edits):
                    results.append((name, "NOT APPLIED"))
                    log.write(f"== {name}: an original text is not unique/present\n\n")
                    continue
                log.write(f"== {name} ({path}; {len(edits)} edit(s); {check})\n")
                try:
                    mutated = text
                    for old, new in edits:
                        mutated = mutated.replace(old, new)
                    file.write_bytes(mutated.encode("utf-8"))
                    if build(log, [path]) != 0:
                        results.append((name, "BUILD FAILED"))
                        continue
                    if check == "unit":
                        code = run([UNIT, FIXTURE], log)
                    else:
                        env = dict(os.environ, R27_ONLY_GROUPS=check.split(":", 1)[1], R27_FAIL_FAST="1")
                        code = run(["python3", "tests/transaction_view_edges_test.py", BINARY], log, env)
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
            replay = run(["python3", "tests/transaction_view_edges_test.py", BINARY], log) if restored_build == 0 else 1
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
