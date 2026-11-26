#!/usr/bin/env python3
"""Behaviour mutations of the R40 Bittensor analytics and R47 price markets
code against their tests (tests/bittensor_analytics_test.cpp +
tests/bittensor_analytics_edges_test.py for R40; tests/price_markets_test.cpp
+ tests/price_markets_edges_test.py for R47).

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and its build/<stem>_*.o variants)
and every binary the checks run (the discovery route test binary and both
unit tests), rebuilds them, runs the check that should notice it (a unit
test, named replay groups with R40_R47_FAIL_FAST=1, or one native-only edge)
and restores the source. Deleting the objects and binaries forces the compile
and the relink of the changed source (with one-second mtimes a same-second
mutant would not be recompiled otherwise). A mutation is KILLED when its
check fails, SURVIVED when it passes, BUILD FAILED when it does not compile
or link (never counted as killed). Every source is restored, compared with
its original bytes and rebuilt at the end (also after an interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/discovery_post_mutations.py [log path]
  R40_R47_MUTATION_FILTER='R40' python3 tests/discovery_post_mutations.py [log path]
  R40_R47_MUTATION_DRY=1 … only checks that every mutation applies (no build)
"""
import hashlib
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
R40_FIXTURE = "tests/corpus/bittensor_analytics_edges.json"
R47_FIXTURE = "tests/corpus/price_markets_edges.json"
DISCOVERY_BINARY = "build/fortis_node_discovery_route_test"
R40_UNIT = "build/bittensor_analytics_test"
R47_UNIT = "build/price_markets_test"
BINARIES = [DISCOVERY_BINARY, R40_UNIT, R47_UNIT]
BITTENSOR = "src/discovery/bittensor_route.cpp"
MARKETS = "src/discovery/price_markets_route.cpp"
SUPPORT = "src/discovery/discovery_route_support.cpp"
OWNER = "src/discovery/discovery_provider_owner.cpp"

# (name, file, old, new, check) — check is "unit40", "unit47", "replay40:<groups>", "replay47:<groups>",
# "native40:<edge>" or "native47:<edge>".
MUTATIONS = [
    # ---- R40 POST /v1/bittensor/analytics ----
    # Dropped: "R40 form bodies stay 415" and "R40 a form __proto__
    # key fails .strict()": form bodies are 415 in the shared body parser and
    # `__proto__` is an ordinary key (compat-decisions.md#form-bodies,
    # #js-prototype-keys); the mutated text no longer exists.
    ("R40 .strict() not enforced", BITTENSOR,
     "else return false;  // .strict()",
     "else (void)0;  // .strict()", "unit40"),
    ("R40 path bound 99 units", BITTENSOR,
     "  const size_t units = DiscoveryUtf16Length(query->path, query->path_length);\n"
     "  if (units < 1 || units > 100) return false;\n  if (values == codec::kBoundedJsonNoToken)",
     "  const size_t units = DiscoveryUtf16Length(query->path, query->path_length);\n"
     "  if (units < 1 || units > 99) return false;\n  if (values == codec::kBoundedJsonNoToken)", "unit40"),
    ("R40 map entry skips unknown fields", BITTENSOR,
     "        if (!r.Uint32(&value_length) || !es_value(r, value_length, depth, &member)) return false;\n      }\n",
     "        if (!r.Uint32(&value_length) || !es_value(r, value_length, depth, &member)) return false;\n"
     "      } else if (!r.Skip(entry_wire, entry_field, kRecursionLimit - depth)) {\n        return false;\n      }\n",
     "unit40"),
    # Dropped: "R40 protobuf __proto__ entry kept": a `__proto__` map
    # entry is now an ordinary key (compat-decisions.md#js-prototype-keys).
    ("R40 Infinity passes zod", BITTENSOR,
     "if (value != value || value - value != 0) return false;  // NaN or ±Infinity",
     "if (value != value) return false;  // NaN or ±Infinity", "unit40"),
    ("R40 unconfigured key not refused", BITTENSOR,
     'if (!configured) { plan->detail = "bittensor analytics not configured"; return; }',
     "(void)configured;", "unit40"),
    # Dropped: "R40 prototype paths answer 400": an Object.prototype
    # member name is now an ordinary unsupported path (400) by construction,
    # so there is no 502 branch left to mutate (compat-decisions.md#js-prototype-keys).
    ("R40 value bound 159 units", BITTENSOR,
     "constexpr size_t kMaxQueryValueUnits = 160;", "constexpr size_t kMaxQueryValueUnits = 159;", "unit40"),
    ("R40 limit bound 199", BITTENSOR,
     '"limit") && !digits_within(text, length, 200)) {', '"limit") && !digits_within(text, length, 199)) {',
     "unit40"),
    ("R40 page bound 21", BITTENSOR,
     '"page") && !digits_within(text, length, 20)) {', '"page") && !digits_within(text, length, 21)) {', "unit40"),
    ("R40 netuid bound 65534", BITTENSOR,
     "!digits_within(text, length, 65535)) {", "!digits_within(text, length, 65534)) {", "unit40"),
    ("R40 network not forced to finney", BITTENSOR,
     'if (allows(*allowed, reinterpret_cast<const uint8_t*>("network"), 7)) {',
     'if (false && allows(*allowed, reinterpret_cast<const uint8_t*>("network"), 7)) {', "unit40"),
    ("R40 space encoded %20", BITTENSOR,
     "    } else if (c == ' ') {\n      out.Byte('+');",
     "    } else if (c == ' ') {\n      out.Text(\"%20\");", "unit40"),
    ("R40 cache key not sorted", BITTENSOR,
     "      if (compared < 0 || (compared == 0 && a.key_length <= b.key_length)) break;",
     "      if (true || compared < 0 || (compared == 0 && a.key_length <= b.key_length)) break;", "unit40"),
    ("R40 revalidation at foreground priority", BITTENSOR,
     "                                  slot->background, provider_complete, slot, &request_id,",
     "                                  false, provider_complete, slot, &request_id,", "replay40:concurrent"),
    ("R40 no cache-key single flight", BITTENSOR,
     "    if (!slot.cancelled && slot.path[0]) { leader = &slot; break; }",
     "    if (false && !slot.cancelled && slot.path[0]) { leader = &slot; break; }", "replay40:concurrent"),
    ("R40 unparseable 2xx not retried", BITTENSOR,
     "                                  slot->background, provider_complete, slot, &request_id,\n"
     "                                  /*retry_unparseable=*/true)) {",
     "                                  slot->background, provider_complete, slot, &request_id,\n"
     "                                  /*retry_unparseable=*/false)) {", "replay40:failure"),
    ("R40 failed miss not held", BITTENSOR,
     "      cache->RecordMissFailure(slot->cache_key, now, result.lane_rejected,",
     "      if (false) cache->RecordMissFailure(slot->cache_key, now, result.lane_rejected,", "replay40:failure"),
    ("R40 quota cost 5", BITTENSOR, "constexpr uint32_t kCost = 10;", "constexpr uint32_t kCost = 5;",
     "replay40:unconfigured"),
    ("R40 Vary missing on JSON 200", BITTENSOR,
     "                             workspace->json_out, out.used};\n    response->vary_accept = true;",
     "                             workspace->json_out, out.used};\n    response->vary_accept = false;",
     "replay40:main"),
    ("R40 slots unbounded refusal answers 502", BITTENSOR,
     "  if (!slot) {\n    SendDiscoveryJson(client, 503, \"Service Unavailable\",",
     "  if (!slot) {\n    SendDiscoveryJson(client, 502, \"Bad Gateway\",", "native40:saturation"),
    ("R40 restart loses the KV entry (no write)", BITTENSOR,
     "      cache->Write(slot->cache_key, kTtl, now, workspace->payload, payload_length, nullptr);",
     "      (void)kTtl;", "native40:restart"),
    # Shared reader (both routes read protobuf with it).
    ("R40 protobuf strings keep a leading BOM", SUPPORT,
     "  if (size >= 3 && data_[start] == 0xef && data_[start + 1] == 0xbb && data_[start + 2] == 0xbf) {",
     "  if (false) {", "unit40"),
    ("R40 unknown varints capped at ten bytes", SUPPORT,
     "    case 0:\n      while (at(pos_++) & 0x80) {}\n      break;",
     "    case 0: {\n      int n = 0;\n      while ((at(pos_++) & 0x80) && ++n < 10) {}\n      if (n >= 10) return false;\n"
     "      break;\n    }", "unit47"),
    # ---- R47 POST /v1/prices/markets ----
    # Dropped: "R47 form bodies stay 415" (compat-decisions.md#form-bodies).
    ("R47 cost floor(n / 10)", MARKETS,
     "const uint32_t cost = static_cast<uint32_t>((n + 9) / 10);",
     "const uint32_t cost = static_cast<uint32_t>(n / 10);", "unit47"),
    ("R47 protobuf costs its id count", MARKETS,
     "  if (body.kind != DiscoveryBodyKind::kJson) return 1;",
     "  if (body.kind == DiscoveryBodyKind::kProtobuf) return body.length ? 2 : 1;\n"
     "  if (body.kind != DiscoveryBodyKind::kJson) return 1;", "unit47"),
    ("R47 at most 99 ids", MARKETS,
     "constexpr size_t kMaxIds = kPriceMarketsMaxIds;", "constexpr size_t kMaxIds = kPriceMarketsMaxIds - 1;",
     "unit47"),
    ("R47 id bound 99 units", MARKETS,
     "constexpr size_t kMaxIdUnits = 100;      // zod string max(100)",
     "constexpr size_t kMaxIdUnits = 99;       // zod string max(100)", "unit47"),
    ("R47 ids not trimmed", MARKETS,
     "    hb::JsTrim(raw[i].text, raw[i].length, &begin, &end);",
     "    end = raw[i].length;", "unit47"),
    ("R47 ids not de-duplicated", MARKETS,
     "    if (duplicate) continue;\n    out[length] = '\\0';",
     "    (void)duplicate;\n    out[length] = '\\0';", "unit47"),
    ("R47 ids sorted by bytes", MARKETS,
     "    while (j > 0 && utf16_compare(arena + ids[j - 1].offset, ids[j - 1].length,\n"
     "                                  arena + moving.offset, moving.length) > 0) {",
     "    while (j > 0 && strcmp(arena + ids[j - 1].offset, arena + moving.offset) > 0) {", "unit47"),
    ("R47 fiat 'USD' accepted", MARKETS,
     '!json.StringEquals(fiat, "usd"))) return false;',
     '!(json.StringEquals(fiat, "usd") || json.StringEquals(fiat, "USD")))) return false;', "unit47"),
    ("R47 a protobuf repeated fiat keeps the first", MARKETS,
     "if (field == 2) { has_fiat = true; fiat = text; fiat_length = text_length; continue; }",
     "if (field == 2) { if (!has_fiat) { fiat = text; fiat_length = text_length; } has_fiat = true; continue; }",
     "unit47"),
    ("R47 '~' percent-encoded", MARKETS,
     "c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\\'' ||",
     "c == '-' || c == '_' || c == '.' || c == '!' || c == '*' || c == '\\'' ||", "unit47"),
    ("R47 pending keeps hit true", MARKETS,
     "  if (pending) { input.hit = false; input.partial = true; }",
     "  if (pending) { input.partial = true; }", "replay47:failure"),
    ("R47 timestamp from live coins", MARKETS,
     "    if (coin.state == CoinState::kHit && coin.as_of > newest) newest = coin.as_of;",
     "    if (coin.as_of > newest) newest = coin.as_of;", "replay47:main"),
    ("R47 JSON pending without escaping", MARKETS,
     "      hb::AppendJsonString(out, input.pending[i], input.pending_lengths[i]);",
     "      out.Byte('\"'); out.Append(input.pending[i], input.pending_lengths[i]); out.Byte('\"');", "unit47"),
    ("R47 batches of 49", MARKETS,
     "constexpr size_t kPriceMarketsBatchIds = 50;", "constexpr size_t kPriceMarketsBatchIds = 50;", "replay47:main"),
    ("R47 revalidation at foreground priority", MARKETS,
     "                              DiscoveryMonotonicMs() + kDiscoveryOwnerDeadlineMs, background,",
     "                              DiscoveryMonotonicMs() + kDiscoveryOwnerDeadlineMs, false,",
     "replay47:concurrent"),
    ("R47 revalidation after the response", MARKETS,
     "  // Stale coins revalidate now, after (and so behind) the cold batches.\n  schedule_refresh(route, index);\n"
     "  if (!request.waiting) {\n    finalize(route, request, head);",
     "  if (!request.waiting) {\n    finalize(route, request, head);\n    schedule_refresh(route, index);",
     "replay47:swr"),
    ("R47 unparseable 2xx not retried", MARKETS,
     "                              batch_complete, batch, &request_id,\n"
     "                              /*retry_unparseable=*/true)) {",
     "                              batch_complete, batch, &request_id,\n"
     "                              /*retry_unparseable=*/false)) {", "replay47:failure"),
    ("R47 a null row does not fail the batch", MARKETS,
     "  if (row->type == codec::BoundedJsonType::kNull) {\n    scan->failed = true;\n    return false;\n  }",
     "  if (row->type == codec::BoundedJsonType::kNull) return true;", "replay47:failure"),
    ("R47 failed coins not held", MARKETS,
     "        else cache->RecordMissFailure(key, now, result.lane_rejected, 502);",
     "        else (void)0;", "replay47:failure"),
    ("R47 the first duplicate row wins", MARKETS,
     "      scan->state->spans[i] = Span{true, static_cast<size_t>(element - scan->base), length};",
     "      if (!scan->state->spans[i].found)\n"
     "        scan->state->spans[i] = Span{true, static_cast<size_t>(element - scan->base), length};",
     "replay47:main"),
    ("R47 unlisted coins answered pending", MARKETS,
     "      out.Text(kNullEntry);\n    }",
     "      continue;\n    }", "replay47:main"),
    ("R47 Vary missing on JSON 200", MARKETS,
     "    *response = http::HttpResponse{200, \"OK\", \"application/json; charset=utf-8\", workspace->json_out,\n"
     "                             out.used};\n    response->vary_accept = true;",
     "    *response = http::HttpResponse{200, \"OK\", \"application/json; charset=utf-8\", workspace->json_out,\n"
     "                             out.used};\n    response->vary_accept = false;", "replay47:main"),
    ("R47 no synchronous slot (the ninth request waits)", MARKETS,
     "    if (request_index < kRequests) {\n      for (size_t i = 0; i < kBatches; ++i)",
     "    if (request_index <= kRequests) {\n      for (size_t i = 0; i < kBatches; ++i)", "native47:deferred-bound"),
    # Shared owner: foreground flights dispatched first.
    ("R47 owner schedules flights by slot only", OWNER,
     "      if (f.background != (pass >= kFlights)) continue;",
     "      if (pass >= kFlights) continue;", "replay47:concurrent"),
]
# Pass 2: pass 1's form mutants left a variable or parameter unused, which
# -Werror refuses (BUILD FAILED, not a kill); these keep it used.
MUTATIONS += [
    # Dropped: the three form-body pass-2 mutants with their pass-1
    # originals (compat-decisions.md#form-bodies, #js-prototype-keys).
    ("R47 unlisted coins answered pending (pass 2)", MARKETS,
     "      out.Text(kNullEntry);\n    }",
     "      (void)kNullEntry;\n      continue;\n    }", "replay47:main"),
    # Pass 1's "timestamp from live coins" is equivalent (a live coin carries
    # no as_of, so it can never be the newest); this one changes the choice.
    ("R47 timestamp the oldest cached asOf (pass 2)", MARKETS,
     "    if (coin.state == CoinState::kHit && coin.as_of > newest) newest = coin.as_of;",
     "    if (coin.state == CoinState::kHit && (newest < 0 || coin.as_of < newest)) newest = coin.as_of;",
     "replay47:main"),
    ("R47 ids sorted by bytes (pass 2)", MARKETS,
     "    while (j > 0 && utf16_compare(arena + ids[j - 1].offset, ids[j - 1].length,\n"
     "                                  arena + moving.offset, moving.length) > 0) {",
     "    while (j > 0 && (static_cast<void>(&utf16_compare),\n"
     "                     strcmp(arena + ids[j - 1].offset, arena + moving.offset) > 0)) {", "unit47"),
]
# The batch-size mutant edits a header constant through the source's own
# copy: kBatchIds is defined from it in the source.
MUTATIONS = [m if m[0] != "R47 batches of 49" else
             (m[0], MARKETS, "constexpr size_t kBatchIds = kPriceMarketsBatchIds;",
              "constexpr size_t kBatchIds = kPriceMarketsBatchIds - 1;", m[4]) for m in MUTATIONS]


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
    if check == "unit40":
        return [R40_UNIT, R40_FIXTURE], None
    if check == "unit47":
        return [R47_UNIT, R47_FIXTURE], None
    kind, what = check.split(":", 1)
    script = "tests/bittensor_analytics_edges_test.py" if kind.endswith("40") else "tests/price_markets_edges_test.py"
    prefix = "R40" if kind.endswith("40") else "R47"
    env = dict(os.environ, R40_R47_FAIL_FAST="1")
    env[f"{prefix}_NATIVE_ONLY" if kind.startswith("native") else f"{prefix}_ONLY_GROUPS"] = what
    return ["python3", script, DISCOVERY_BINARY], env


def interrupted(_signum, _frame):
    """SIGTERM unwinds like Ctrl-C, so the sources are restored."""
    raise KeyboardInterrupt


def main():
    signal.signal(signal.SIGTERM, interrupted)
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "discovery_post_mutations.log"
    selected = os.environ.get("R40_R47_MUTATION_FILTER")
    dry = os.environ.get("R40_R47_MUTATION_DRY") == "1"
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    if dry:
        bad = [(name, sources[path].decode().count(old)) for name, path, old, new, _ in MUTATIONS
               if sources[path].decode().count(old) != 1 or old == new]
        print(f"{len(MUTATIONS)} mutations; not uniquely applicable: {bad}")
        return
    results = []
    with open(log_path, "w") as log:
        log.write(f"R40–R47 discovery POST mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
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
        for route in ("R40", "R47"):
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
