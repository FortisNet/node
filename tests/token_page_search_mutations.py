#!/usr/bin/env python3
"""Behavior mutations of the R24 token page and R25 token search code against
their tests (tests/token_page_test.cpp + tests/token_page_edges_test.py for
R24; tests/token_search_test.cpp + tests/token_search_edges_test.py for R25).

Each mutation edits one spot of src/ (never a header), deletes every object
compiled from that source (build/<stem>.o and its build/<stem>_*.o variants)
and every binary the checks run (both route test binaries and both unit
tests), rebuilds them, runs the check that should notice it (a unit test, or
named replay groups with R24_FAIL_FAST=1 / R25_FAIL_FAST=1) and restores the
source. Deleting the objects and binaries forces the compile and the relink
of the changed source (with one-second mtimes a same-second mutant would not
be recompiled otherwise). A mutation is KILLED when its check fails,
SURVIVED when it passes, BUILD FAILED when it does not compile or link (never
counted as killed). Every source is restored, compared with its original
bytes and rebuilt at the end (also after an interruption).

Usage (from the repository root, with no other replay running):
  python3 tests/token_page_search_mutations.py [log path]
  R24_R25_MUTATION_FILTER='R25' python3 tests/token_page_search_mutations.py [log path]
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
PAGE_FIXTURE = "tests/corpus/token_page_edges.json"
SEARCH_FIXTURE = "tests/corpus/token_search_edges.json"
TOKEN_BINARY = "build/fortis_node_token_route_test"
DISCOVERY_BINARY = "build/fortis_node_discovery_route_test"
PAGE_UNIT = "build/token_page_test"
SEARCH_UNIT = "build/token_search_test"
BINARIES = [TOKEN_BINARY, DISCOVERY_BINARY, PAGE_UNIT, SEARCH_UNIT]
SERVICE = "src/tokens/token_service.cpp"
PAGE = "src/tokens/token_page.cpp"
SEARCH = "src/discovery/token_search_route.cpp"
OWNER = "src/discovery/discovery_provider_owner.cpp"
ROUTE_SUPPORT = "src/tokens/token_route_support.cpp"
TASK_ROUTES = "src/tokens/token_task_routes.cpp"

# (name, file, old, new, check) — check is "unit24", "unit25", "replay24:<groups>" or "replay25:<groups>".
MUTATIONS = [
    # ---- R24 GET …/token/{tokenId}/page ----
    # Dropped (opaque-params, no flag left): "R24 route without opaque params".
    ("R24 raw non-ASCII byte not refused before the hooks", TASK_ROUTES,
     "  if (reject_raw_non_ascii(client, request, head) || reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, g_route.impl->attestation_required);\n"
     "  if (reply_policy(client, policy, head)) return;\n"
     "  const int cost = policy.rate_limit_cost;\n"
     "  const uint32_t remaining = policy.rate_limit_remaining;\n"
     '  static const char* const kNames[] = {"include", "days", "interval", "coingeckoId"};',
     "  if (reject_long_params(client, request, head)) return;\n"
     "  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost, g_route.impl->attestation_required);\n"
     "  if (reply_policy(client, policy, head)) return;\n"
     "  const int cost = policy.rate_limit_cost;\n"
     "  const uint32_t remaining = policy.rate_limit_remaining;\n"
     '  static const char* const kNames[] = {"include", "days", "interval", "coingeckoId"};', "replay24:main"),
    ("R24 include bound 199 units", PAGE,
     "if (codec::history_batch::Utf16Units(include.text + include_begin, include_end - include_begin) > 200)",
     "if (codec::history_batch::Utf16Units(include.text + include_begin, include_end - include_begin) > 199)", "unit24"),
    ("R24 days bound 364", PAGE,
     "value < 1 || value > 365) return PageQueryStatus::kInvalid;",
     "value < 1 || value > 364) return PageQueryStatus::kInvalid;", "unit24"),
    ("R24 interval accepts upper case", PAGE,
     'if (interval.length == 2 && memcmp(interval.text, "1h", 2) == 0) options->interval = SeriesInterval::k1h;',
     'if (interval.length == 2 && (memcmp(interval.text, "1h", 2) == 0 || memcmp(interval.text, "1H", 2) == 0)) '
     'options->interval = SeriesInterval::k1h;', "unit24"),
    ("R24 coingeckoId bound 99 characters", PAGE,
     "lowered->length < 1 || lowered->length > 100", "lowered->length < 1 || lowered->length > 99", "unit24"),
    ("R24 repeated keys accepted", PAGE,
     "if (include.count > 1 || days.count > 1 || interval.count > 1 || coingecko_id.count > 1)",
     "if (false && (include.count > 1 || days.count > 1 || interval.count > 1 || coingecko_id.count > 1))", "unit24"),
    ("R24 include=all not recognized", PAGE,
     'if (lower && lower->length == 3 && memcmp(lower->text, "all", 3) == 0) {',
     'if (false && lower && lower->length == 3 && memcmp(lower->text, "all", 3) == 0) {', "unit24"),
    ("R24 unknown include names joined by ';'", PAGE,
     'const char* lead = any_unknown ? "," : kPrefix;', 'const char* lead = any_unknown ? ";" : kPrefix;', "unit24"),
    ("R24 quote section without the liquidity facet", PAGE,
     "if (sections.Has(kLiquidity) || sections.Has(kQuote) || sections.Has(kSafety))",
     "if (sections.Has(kLiquidity) || sections.Has(kSafety))", "unit24"),
    ("R24 a zero market price is quoted", PAGE,
     "if (tj::IsNumber(price) && price->number != 0 && !isnan(price->number)) {",
     "if (tj::IsNumber(price) && !isnan(price->number)) {", "unit24"),
    ("R24 a zero DEX price is quoted", PAGE,
     '&dex) && dex > 0) {', '&dex) && dex >= 0) {', "unit24"),
    ("R24 identity provenance without the market row", PAGE,
     'CombineProvenance(identity_parts, market ? 2 : 1)', 'CombineProvenance(identity_parts, 1)', "unit24"),
    ("R24 asOf the newest section", PAGE,
     "    if (at < oldest) oldest = at;\n  }\n  return token_facts::IsoValue(a, oldest);",
     "    if (at > oldest || i == 0) oldest = at;\n  }\n  return token_facts::IsoValue(a, oldest);", "unit24"),
    ("R24 series window one day longer", PAGE,
     "const int64_t from = to - static_cast<int64_t>(options.days * 86400);",
     "const int64_t from = to - static_cast<int64_t>((options.days + 1) * 86400);", "unit24"),
    ("R24 series failure labelled UNAVAILABLE", PAGE,
     'return tj::Set(a, errors, "series", tj::Str(a, "UPSTREAM_UNAVAILABLE"));',
     'return tj::Set(a, errors, "series", tj::Str(a, "UNAVAILABLE"));', "unit24"),
    ("R24 comparison failure not reported", PAGE,
     'tj::Set(a, errors, "comparison", tj::Str(a, "UNAVAILABLE"));',
     'tj::Set(a, errors, "comparison", tj::Str(a, "UPSTREAM_UNAVAILABLE"));', "unit24"),
    ("R24 page cache-control max-age=60", SERVICE,
     'const char* const kPageCacheControl = "private, max-age=15";',
     'const char* const kPageCacheControl = "private, max-age=60";', "replay24:main"),
    ("R24 page not conditional", SERVICE,
     'CompleteStructured(work, *task, "token-page", document, nullptr, kPageCacheControl, true);',
     'CompleteStructured(work, *task, "token-page", document, nullptr, kPageCacheControl, false);', "replay24:main"),
    ("R24 query buffers of 1 KiB", ROUTE_SUPPORT,
     "capacity_ = strlen(target) + 1;", "capacity_ = 1024;", "replay24:main"),
    # ---- R25 GET …/tokens/search ----
    # Dropped (opaque-params, no flag left): "R25 route without opaque params".
    # Pass 1 removed the call, leaving the function unused (-Werror: BUILD
    # FAILED); pass 2 lets every byte below 0xF8 through instead.
    ("R25 raw non-ASCII byte not refused before the hooks", SEARCH,
     "    if (static_cast<unsigned char>(*p) < 0x80) continue;\n    http::WriteRefusal(client, 400,",
     "    if (static_cast<unsigned char>(*p) < 0xF8) continue;\n    http::WriteRefusal(client, 400,", "replay25:main"),
    ("R25 a 101-unit network is not 414", SEARCH,
     "if (!param || param->utf16_units <= 100) return false;",
     "if (!param || param->utf16_units <= 101) return false;", "replay25:main"),
    ("R25 network bound 63 units", SEARCH,
     "if (term != TermStatus::kOk || network_units < 1 || network_units > 64) {",
     "if (term != TermStatus::kOk || network_units < 1 || network_units > 63) {", "replay25:main"),
    ("R25 q bound 99 units", SEARCH,
     "valid = units >= 1 && units <= kMaxQueryUnits &&", "valid = units >= 1 && units < kMaxQueryUnits &&", "unit25"),
    ("R25 q not trimmed", SEARCH,
     "    hb::JsTrim(memory, raw_length, &begin, &end);\n",
     "    begin = 0;\n    end = raw_length;\n", "unit25"),
    # Pass 1 left `capacity` unused (-Werror: BUILD FAILED).
    ("R25 term not lower-cased", SEARCH,
     "fortis::compat::JsToLowerCase(memory + begin, end - begin, term, capacity, term_length);",
     "(capacity > end - begin && (memcpy(term, memory + begin, end - begin), *term_length = end - begin, true));",
     "unit25"),
    ("R25 repeated q accepted", SEARCH,
     "bool valid = occurrences == 1 && hb::ValidUtf8(",
     "bool valid = occurrences >= 1 && hb::ValidUtf8(", "unit25"),
    ("R25 query values over 4 KiB refused (the old fixed buffers)", SEARCH,
     "    if (equal || key_length) {\n      for (size_t i = 0; i < key_length; ++i) key_raw[i]",
     "    if ((equal || key_length) && key_length < 1024 && value_length < 4096) {\n"
     "      for (size_t i = 0; i < key_length; ++i) key_raw[i]", "unit25"),
    ("R25 a NUL ends the cache key", SEARCH,
     "    if (term[i] == '\\0') key.Text(\"%00\");", "    if (term[i] == '\\0') break;", "unit25"),
    ("R25 '%' not escaped in the cache key", SEARCH,
     "    else if (term[i] == '%') key.Text(\"%25\");\n", "", "unit25"),
    ("R25 unknown-network detail cut at a NUL", SEARCH,
     "hb::AppendJsonString(out, detail, sizeof(kPrefix) - 1 + length);",
     "hb::AppendJsonString(out, detail, strnlen(detail, sizeof(kPrefix) - 1 + length));", "replay25:main"),
    ("R25 provider accept header */*", OWNER,
     'snprintf(request.headers[0].value, sizeof(request.headers[0].value), "application/json");',
     'snprintf(request.headers[0].value, sizeof(request.headers[0].value), "*/*");', "replay25:main"),
    ("R25 an unparseable 2xx is not retried", OWNER,
     "      options.retry_unparseable_json = f.retry_unparseable;\n", "      options.retry_unparseable_json = false;\n",
     "replay25:failure"),
    ("R25 no dedupe", SEARCH,
     "      seen = lengths[j] == length && items[j].network_length == items[i].network_length &&",
     "      seen = false && lengths[j] == length && items[j].network_length == items[i].network_length &&",
     "replay25:main"),
    ("R25 Jupiter sort ignores verified", SEARCH,
     "double order = static_cast<double>(moving.verified) - static_cast<double>(a.verified);",
     "double order = 0;", "replay25:main"),
    ("R25 a coin detail 404 is not cached", SEARCH,
     '    return cache->Write(key, kDiscoveryTtlTokenIdentity, cache->Now(), "null", 4, nullptr) || true;',
     "    return false;", "replay25:main"),
    ("R25 a partial answer is stored", SEARCH,
     "if (!job.partial && write_record(state, items, count, &length))",
     "if (write_record(state, items, count, &length))", "replay25:failure"),
    ("R25 protobuf accepts a fractional holder count", SEARCH,
     "floor(item.holders) == item.holders)) return false;", "true)) return false;", "unit25"),
    ("R25 the protobuf 500 without vary", SEARCH,
     "  response.content_type = \"application/json; charset=utf-8\";\n  response.vary_accept = true;\n  return response;",
     "  response.content_type = \"application/json; charset=utf-8\";\n  response.vary_accept = false;\n  return response;",
     "replay25:main"),
    ("R25 answers fresh for 4 minutes", SEARCH,
     "cache->Write(job.cache_key, kDiscoveryTtlSearch, cache->Now(), state->record, length, nullptr);",
     "cache->Write(job.cache_key, DiscoveryCacheTtl{240000, 3600000}, cache->Now(), state->record, length, nullptr);",
     "replay25:swr"),
    ("R25 quota cost 5", SEARCH, "constexpr uint32_t kCost = 10;", "constexpr uint32_t kCost = 5;", "replay25:main"),
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
    if check == "unit24":
        return [PAGE_UNIT, PAGE_FIXTURE], None
    if check == "unit25":
        return [SEARCH_UNIT, SEARCH_FIXTURE], None
    route, groups = check.split(":", 1)
    if route == "replay24":
        env = dict(os.environ, R24_ONLY_GROUPS=groups, R24_FAIL_FAST="1")
        return ["python3", "tests/token_page_edges_test.py", TOKEN_BINARY], env
    env = dict(os.environ, R25_ONLY_GROUPS=groups, R25_FAIL_FAST="1")
    return ["python3", "tests/token_search_edges_test.py", DISCOVERY_BINARY], env


def interrupted(_signum, _frame):
    """SIGTERM unwinds like Ctrl-C, so the sources are restored."""
    raise KeyboardInterrupt


def main():
    signal.signal(signal.SIGTERM, interrupted)
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "token_page_search_mutations.log"
    selected = os.environ.get("R24_R25_MUTATION_FILTER")
    sources = {path: (ROOT / path).read_bytes() for _, path, _, _, _ in MUTATIONS}
    digests = {path: hashlib.sha256(data).hexdigest() for path, data in sources.items()}
    results = []
    with open(log_path, "w") as log:
        log.write(f"R24–R25 token page and search mutations, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
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
        for route in ("R24", "R25"):
            mine = [outcome for name, outcome in results if name.startswith(route)]
            killed = sum(1 for outcome in mine if outcome == "KILLED")
            failed = sum(1 for outcome in mine if outcome == "BUILD FAILED")
            log.write(f"{route}: {killed}/{len(mine)} killed, {failed} build failures (not counted as killed)\n")
    for name, outcome in results:
        print(f"{outcome:12} {name}")
    print(f"{sum(1 for _, o in results if o == 'KILLED')}/{len(results)} killed; log {log_path}")


if __name__ == "__main__":
    main()
