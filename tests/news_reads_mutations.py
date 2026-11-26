#!/usr/bin/env python3
"""Behavior mutations of the R28–R30 news read code against its tests.

Each mutation edits one source of src/ (one or more exact, unique
replacements), deletes every object compiled from that source
(build/{stem}.o and its build/{stem}_*.o variants) and both checked
binaries, rebuilds them (so each mutant is compiled and relinked; source
mtimes have one-second resolution), runs the check that should notice it --
the unit (build/news_reads_test) or tests/news_reads_edges_test.py with
R28_FAIL_FAST=1 and R28_STOP_AFTER=<group> -- and restores the source bytes.
Outcomes: KILLED (the check failed), SURVIVED (it passed), BUILD FAILED (the
mutant did not compile or link; never counted as killed), NOT APPLIED (an
original text is not unique). Every mutated source's SHA-256 is recorded
before and after; the sources are rebuilt at the end and the unit and the
whole replay rerun on the restored binaries.

Usage (from the repository root, with no other news replay running):
  python3 tests/news_reads_mutations.py [log path]
  R28_MUTATION_FILTER='substring of a name' python3 tests/news_reads_mutations.py [log path]
"""
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "tests/corpus/news_reads_edges.json"
BINARY = "build/fortis_node_news_route_test"
UNIT = "build/news_reads_test"
ROUTES = "src/news/news_routes.cpp"
STORE = "src/news/news_store.cpp"
PRICES = "src/news/news_reaction_prices.cpp"
REACTIONS = "src/news/news_reactions.cpp"

# (name, row, file, [(old, new), ...], check) -- check is "unit" or "replay:<stop-after group>".
MUTATIONS = [
    # ---- R28 GET /v1/news
    ("R28 limit default 41", "R28", ROUTES, [("  size_t limit = 40;", "  size_t limit = 41;")], "replay:feed"),
    ("R28 topics bound 201 units", "R28", ROUTES,
     [("    if (trimmed.length > 200) {", "    if (trimmed.length > 201) {")], "replay:feed"),
    ("R28 stale after 46 minutes", "R28", ROUTES,
     [("constexpr int64_t kStaleAfterMs = 45LL * 60 * 1000;", "constexpr int64_t kStaleAfterMs = 46LL * 60 * 1000;")],
     "replay:feed"),
    ("R28 feed fresh 120 s", "R28", ROUTES,
     [("constexpr discovery::DiscoveryCacheTtl kTtlNews{60000, 3600000};", "constexpr discovery::DiscoveryCacheTtl kTtlNews{120000, 3600000};")],
     "replay:feed"),
    ("R28 corrupt record skipped", "R28", STORE,
     [("      if (status != StoreStatus::kOk) return status;\n      if (!found) continue;",
       "      if (status != StoreStatus::kOk && status != StoreStatus::kCorrupt) return status;\n"
       "      if (!found) continue;")], "replay:feed"),
    ("R28 snapshot without the story lock", "R28", STORE,
     [("    StoryLock lock;\n    const StoreStatus status = read_time_index(store, topics, topic_count, limit, false, 0, now_ms,",
       "    const StoreStatus status = read_time_index(store, topics, topic_count, limit, false, 0, now_ms,")],
     "unit"),
    ("R28 parse scratch shared between requests", "R28", ROUTES,
     [("    block = static_cast<uint8_t*>(malloc(text + units));",
       "    static uint8_t* shared = static_cast<uint8_t*>(malloc(1 << 20));\n    block = shared;"),
      ("  ~FeedScratch() { free(block); }", "  ~FeedScratch() {}")], "unit"),
    # ---- R29 GET /v1/news/{id}
    ("R29 item fresh 600 s", "R29", ROUTES,
     [("constexpr discovery::DiscoveryCacheTtl kTtlNewsItem{300000, 86400000};",
       "constexpr discovery::DiscoveryCacheTtl kTtlNewsItem{600000, 86400000};")], "replay:item"),
    ("R29 malformed detail spelled differently", "R29", ROUTES,
     [('"{\\"error\\":\\"BAD_REQUEST\\",\\"detail\\":\\"malformed news id\\"}"',
       '"{\\"error\\":\\"BAD_REQUEST\\",\\"detail\\":\\"malformed id\\"}"')], "replay:grammar"),
    # Dropped (opaque-params, no flag left): "R29 opaque params off".
    ("R29 id not trimmed", "R29", ROUTES,
     [("  hb::JsTrim(param, length, &begin, &end);", "  begin = 0;\n  end = length;")], "unit"),
    ("R29 abstract kept when empty", "R29", ROUTES,
     [('    if (a.summary.length) AppendJsonString(out, a.summary); else out.Text("null");',
       "    AppendJsonString(out, a.summary);")], "replay:item"),
    ("R29 414 from 102 units", "R29", ROUTES,
     [("  if (!param || param->utf16_units <= 100) return false;",
       "  if (!param || param->utf16_units <= 101) return false;")], "replay:grammar"),
    # ---- R30 GET /v1/news/{id}/reactions
    ("R30 malformed answer carries the item detail", "R30", REACTIONS,
     [('    send_json(client, 400, "Bad Request", "{\\"error\\":\\"BAD_REQUEST\\"}", head, cost, remaining);',
       '    send_json(client, 400, "Bad Request", "{\\"error\\":\\"BAD_REQUEST\\",\\"detail\\":\\"malformed news id\\"}", '
       'head, cost, remaining);')], "replay:grammar"),
    ("R30 seven tagged coins", "R30", REACTIONS,
     [("  for (size_t t = 0; t < job.ticker_count && distinct < 6; ++t) {",
       "  for (size_t t = 0; t < job.ticker_count && distinct < 7; ++t) {")], "replay:reactions"),
    ("R30 lag tolerance 6 minutes", "R30", REACTIONS,
     [("  const double lag = 300000.0 < seconds * 1000.0 / 5 ? 300000.0 : seconds * 1000.0 / 5;",
       "  const double lag = 360000.0 < seconds * 1000.0 / 5 ? 360000.0 : seconds * 1000.0 / 5;")], "unit"),
    ("R30 unparseable 2xx not retried", "R30", REACTIONS,
     [("                                   /*retry_unparseable=*/true)) {",
       "                                   /*retry_unparseable=*/false)) {")], "replay:reactions"),
    ("R30 prices null not defaulted", "R30", PRICES,
     [("          if (*prices.p == 'n') out.Text(\"[]\");  // prices: null -> ?? []",
       "          if (*prices.p == 'n') ok = false;  // prices: null -> ?? []")], "unit"),
    # ---- shared story storage (all three handlers read it)
    ("store keeps one U+FFFD in short strings", "store", STORE,
     [("  const size_t per = text.length < 64 ? 3 : 1;", "  const size_t per = 1;")], "replay:feed"),
]


def sha(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


def run(command, log, env=None):
    log.write(f"$ {' '.join(command)}\n")
    log.flush()
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    lines = result.stdout.splitlines()
    shown = [line for line in lines if line.startswith("FAIL") or "error:" in line][:4] + lines[-3:]
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
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "news_reads_mutations.log"
    selected = os.environ.get("R28_MUTATION_FILTER")
    results = []
    sources = sorted({path for _, _, path, _, _ in MUTATIONS})
    before = {path: sha(path) for path in sources}
    with open(log_path, "a" if selected else "w") as log:
        log.write(f"R28–R30 news read mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}"
                  f"{f' (filter {selected!r})' if selected else ''}\n")
        for path in sources:
            log.write(f"  before {before[path]}  {path}\n")
        log.write("\n")
        try:
            for name, row, path, edits, check in MUTATIONS:
                if selected and selected not in name:
                    continue
                file = ROOT / path
                original = file.read_bytes()
                text = original.decode("utf-8")
                if any(text.count(old) != 1 for old, _ in edits):
                    results.append((name, row, "NOT APPLIED"))
                    log.write(f"== {name}: an original text is not unique/present\n\n")
                    continue
                log.write(f"== {name} ({path}; {len(edits)} edit(s); {check})\n")
                try:
                    mutated = text
                    for old, new in edits:
                        mutated = mutated.replace(old, new)
                    file.write_bytes(mutated.encode("utf-8"))
                    if build(log, [path]) != 0:
                        results.append((name, row, "BUILD FAILED"))
                        continue
                    if check == "unit":
                        code = run([UNIT, FIXTURE], log)
                    else:
                        env = dict(os.environ, R28_STOP_AFTER=check.split(":", 1)[1], R28_FAIL_FAST="1")
                        code = run(["python3", "tests/news_reads_edges_test.py", BINARY], log, env)
                    results.append((name, row, "KILLED" if code != 0 else "SURVIVED"))
                finally:
                    file.write_bytes(original)
                    log.write(f"-> {results[-1][2]}; restored {sha(path)}\n\n")
        finally:
            after = {path: sha(path) for path in sources}
            for path in sources:
                log.write(f"  after  {after[path]}  {path} {'(restored)' if after[path] == before[path] else 'CHANGED'}\n")
            log.write("\nRestored build\n")
            restored_build = build(log, sources)
            unit = run([UNIT, FIXTURE], log) if restored_build == 0 else 1
            replay = run(["python3", "tests/news_reads_edges_test.py", BINARY, "build/fortis_node"], log) \
                if restored_build == 0 else 1
            log.write(f"restored binaries: build exit {restored_build}, unit exit {unit}, replay exit {replay}\n\n")
            log.write("Summary\n")
            for name, row, outcome in results:
                log.write(f"  {outcome:12} {row:5} {name}\n")
            for row in ("R28", "R29", "R30", "store"):
                rows = [o for _, r, o in results if r == row]
                log.write(f"  {row}: {sum(o == 'KILLED' for o in rows)} killed of {len(rows)}\n")
            counts = {k: sum(1 for _, _, o in results if o == k) for k in ("KILLED", "SURVIVED", "BUILD FAILED",
                                                                             "NOT APPLIED")}
            log.write(f"{counts['KILLED']} killed, {counts['SURVIVED']} survived, {counts['BUILD FAILED']} build "
                      f"failed, {counts['NOT APPLIED']} not applied, of {len(results)}\n")
    for name, row, outcome in results:
        print(f"{outcome:12} {row:5} {name}")
    print(f"{sum(1 for _, _, o in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
