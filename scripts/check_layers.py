#!/usr/bin/env python3
"""Module layering check for src/ (run by `make check-layers`, part of `make test`).

Every directory under src/ is one module. Every src/ directory is on the
include path, so a file names a header by its basename alone
(`#include "kv_store.h"`); this script maps each basename to the directory that
owns it and checks every cross-module include against the layer order below.

Intended layer order, lowest first. A module may include headers from its own
directory and from any module in a LOWER layer. Including from a higher layer,
or from another module in the same layer (peers stay independent), is a
violation:

  0  base                         header-only seams (Clock, Starter<T>, ...)
  1  compat, security             compat: the remaining helpers that reproduce
                                  another runtime's behaviour (js_case,
                                  js_date_parse, js_regex, whatwg_url, idna);
                                  security: the secret scanner for public
                                  error reports and AI text
                                  (diagnostic_redaction, redaction_tables)
  2  codec, crypto, config        byte/JSON codecs (tj, price_history_batch_codec,
                                  bounded_json, json_document_writer, protobuf),
                                  the node's text rules (provider_text,
                                  text_sanitize), crypto, NativeConfig
  3  http, storage                HTTP server/clients, KV store
  4  auth                         device auth, attestation, EVM proofs
  5  providers, networks          provider lanes/requests, price-id parser,
                                  network registry and token refs
  6  chains, discovery            chain drivers; market-data caches, media
                                  registry, coin popularity, search/markets routes
  7  prices                       spot/history/series stores and services
  8  news
  9  exchange                     cross-chain, Robinhood, DashPay
  10 tokens
  11 wallet, ai                   address/sync plane; AI conversations
  12 operations                   ops pages/routes, /v1/networks route
  13 app                          composition root (server.cpp, main.cpp)

Why the less obvious placements are where they are (the order follows the
real includes; it was not loosened to make the check pass):
  - compat sits BELOW codec. What is left in compat/ includes nothing but
    base, and codec's provider_text.cpp still calls its case mapping and date
    parser (js_case.h, js_date_parse.h).
  - discovery sits BELOW prices. discovery/ is the shared market-data layer
    (discovery_cache SWR caches, market_view, media_registry) and the
    coin_price_scheduler in prices/ reads it. coin_popularity (the decayed
    device-demand tree) is fed by the /v1/prices/markets route in discovery/
    and read by the price scheduler and spot owner, so it lives with its
    writer. ohlc_cache_policy is a dependency-free leaf consumed by
    discovery_cache.h, so it must sit at or below discovery.
  - news sits above prices: news_store/news_service read spot prices from
    PriceSeriesStore to enrich clustered stories; prices never reads news.
  - operations sits above every feature module: it is the cross-cutting
    observability/admin surface (statusz, cache explorer, token-price and
    network preview pages). network_route (/v1/networks) lives there because
    it reports which planes started, which needs chain_core and the wallet
    plane's OverviewSupports* predicates; networks/ itself stays a leaf
    registry. Feature modules (ai included) must not include operations.
  - tests/ are not layered: test harnesses may link any module.

Remaining violations are listed in ALLOWLIST as exact
"<including file> -> <included header>" strings. The list may only shrink: an
entry that no longer matches a real include is reported as stale and fails the
check too, so fixing a violation forces its removal here.

Also enforced: includable basenames (.h, .inc) are unique across src/ (the
flat include style depends on it), every src/ directory appears in LAYERS,
and src/<module>/ has no subdirectories (they would not be on the include
path and would escape this check). Both "..." and <...> includes of a src/
header are checked; a directory prefix in the include is ignored, because
resolution is by basename.

Namespaces (docs/namespaces.md): every C++ declaration under src/<module>/
lives in fortis::<module> or a namespace nested in it. Outside such a block
a file may only hold forward declarations of another module's types (in
that module's namespace), `extern "C"` declarations, forward declarations of
C structs, main() in src/app/, and namespace aliases in .cpp files (never in
headers). C sources (.c) and .inc fragments are not checked.

Usage: python3 scripts/check_layers.py [--graph]
  --graph  also print the module dependency counts and mutual pairs.
"""
import collections
import os
import re
import sys

LAYERS = [
    ["base"],
    ["compat", "security"],
    ["codec", "crypto", "config"],
    ["http", "storage"],
    ["auth"],
    ["providers", "networks"],
    ["chains", "discovery"],
    ["prices"],
    ["news"],
    ["exchange"],
    ["tokens"],
    ["wallet", "ai"],
    ["operations"],
    ["app"],
]

# Known layering debt: "<module>/<file> -> <header>". May only shrink.
ALLOWLIST = set()

SOURCE_SUFFIXES = (".h", ".c", ".cpp", ".inc")
INCLUDABLE_SUFFIXES = (".h", ".inc")
INCLUDE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)', re.M)


LITERAL = re.compile(r'(?:u8|u|U|L)?R"([^(\s]{0,16})\((?:.|\n)*?\)\1"|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)+\'')
COMMENT = re.compile(r"//[^\n]*|/\*(?:.|\n)*?\*/")
FORWARD_ONLY = re.compile(r"^(?:\s*(?:class|struct|union|enum(?:\s+class|\s+struct)?)\s+\w+(?:\s*:\s*[\w:]+)?\s*;)*\s*$")


def code_only(text):
    """The text with comments, literals and preprocessor lines blanked."""
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    out, i = [], 0
    pattern = re.compile(COMMENT.pattern + "|" + LITERAL.pattern)
    for match in pattern.finditer(text):
        if match.start() < i:
            continue
        out.append(text[i:match.start()])
        token = match.group(0)
        out.append(blank(match) if token.startswith("/") else token[0] + " " * (len(token) - 2) + token[-1]
                   if token[0] in "\"'" else re.sub(r"[^\n]", " ", token))
        i = match.end()
    out.append(text[i:])
    return re.sub(r"(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", blank, "".join(out))


def statement_end(code, i):
    """End of the top-level statement or braced block starting at i."""
    depth = 0
    while i < len(code):
        char = code[i]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                rest = re.match(r"\s*;", code[i + 1:])
                return i + 1 + (rest.end() if rest else 0)
        elif char == ";" and depth == 0:
            return i + 1
        i += 1
    return i


def namespace_problems(module, name, text, modules=()):
    """modules: every src/ module; a forward-declaring block must name one."""
    if not name.endswith((".h", ".cpp")):
        return []
    code = code_only(text)
    own = f"fortis::{module}"
    problems, i = [], 0
    if name.endswith(".h"):
        # An alias anywhere in a header (also inside fortis::<module>) leaks
        # into every includer.
        for alias in re.finditer(r"\bnamespace\s+\w+\s*=", code):
            line = code.count("\n", 0, alias.start()) + 1
            problems.append(f"src/{module}/{name}:{line}: namespace alias in a header (allowed in .cpp files only)")
    while True:
        match = re.compile(r"\S").search(code, i)
        if not match:
            return problems
        i = match.start()
        line = code.count("\n", 0, i) + 1
        block = re.compile(r"namespace\s+([\w:]+)\s*\{").match(code, i)
        end = statement_end(code, i)
        where = f"src/{module}/{name}:{line}"
        if block:
            space = block.group(1)
            if space != own and not space.startswith(own + "::"):
                body = code[block.end():end].rstrip().rstrip(";").rstrip()[:-1]
                other = space.split("::")[1] if space.startswith("fortis::") else None
                if not (other is not None and (other in modules or not modules) and FORWARD_ONLY.match(body)):
                    problems.append(f"{where}: namespace {space} (declarations belong in {own}; "
                                    "another module's namespace may only hold forward declarations)")
        elif re.compile(r"namespace\s+\w+\s*=").match(code, i):
            pass  # headers: reported above
        elif re.compile(r'extern\s+"').match(code, i):
            # code_only blanks literals, so read the linkage from the text.
            linkage = re.compile(r'extern\s+"([^"]*)"').match(text, i)
            if not linkage or linkage.group(1) != "C" or re.search(r"\b(namespace|template|class)\b", code[i:end]):
                snippet = " ".join(text[i:end].split())[:60]
                problems.append(f'{where}: only extern "C" declarations of C entities may sit outside {own}: {snippet}')
        elif not (re.compile(r"struct\s+\w+\s*;").match(code, i)
                  or (module == "app" and re.compile(r"int\s+main\s*\(").match(code, i))):
            snippet = " ".join(code[i:end].split())[:60]
            problems.append(f"{where}: declaration outside {own}: {snippet}")
        i = end


def main() -> int:
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
    rank = {module: index for index, group in enumerate(LAYERS) for module in group}
    errors = []
    owner = {}
    files = []
    for module in sorted(os.listdir(root)):
        directory = os.path.join(root, module)
        if not os.path.isdir(directory):
            continue
        if module not in rank:
            errors.append(f"src/{module}/ is not assigned a layer in scripts/check_layers.py")
            continue
        for name in sorted(os.listdir(directory)):
            if os.path.isdir(os.path.join(directory, name)):
                errors.append(f"src/{module}/{name}/ is a subdirectory; modules must be flat")
                continue
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            files.append((module, name))
            if name.endswith(INCLUDABLE_SUFFIXES):
                if name in owner:
                    errors.append(f"duplicate header basename {name}: src/{owner[name]}/ and src/{module}/")
                owner[name] = module

    # Both builds take the include path from FORTIS_SOURCE_DIRS in sources.mk
    # (the Makefile includes that file); a directory
    # missing from it breaks every build of a file that names its headers.
    modules = sorted(m for m in os.listdir(root) if os.path.isdir(os.path.join(root, m)))
    with open(os.path.join(root, "..", "sources.mk"), encoding="utf-8") as handle:
        match = re.search(r"^FORTIS_SOURCE_DIRS\s*:=(.*)$", handle.read(), re.M)
    listed = sorted(match.group(1).split()) if match else []
    if listed != modules:
        errors.append("sources.mk FORTIS_SOURCE_DIRS does not list exactly the src/ directories "
                      f"(missing {sorted(set(modules) - set(listed))}, extra {sorted(set(listed) - set(modules))})")
    for makefile in ("Makefile",):
        with open(os.path.join(root, "..", makefile), encoding="utf-8") as handle:
            text = handle.read()
        if not re.search(r"^include \S*sources\.mk$", text, re.M) or "$(FORTIS_SOURCE_DIRS)" not in text:
            errors.append(f"{makefile} does not take its src/ directories from sources.mk")

    edges = collections.defaultdict(list)
    seen = set()
    for module, name in files:
        with open(os.path.join(root, module, name), encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        errors.extend(namespace_problems(module, name, text, rank))
        for quoted, angled in INCLUDE.findall(text):
            included = quoted or angled
            target = owner.get(os.path.basename(included))
            if target is None or target == module:
                continue
            edge = f"{module}/{name} -> {os.path.basename(included)}"
            edges[(module, target)].append(edge)
            if rank[target] < rank[module]:
                continue
            seen.add(edge)
            if edge not in ALLOWLIST:
                where = "a higher layer" if rank[target] > rank[module] else "a peer in the same layer"
                errors.append(f"layer violation: src/{edge} ({module} includes {target}, {where})")
    for edge in sorted(ALLOWLIST - seen):
        errors.append(f"stale allowlist entry (fixed? remove it): {edge}")

    mutual = sorted({tuple(sorted(pair)) for pair in edges if (pair[1], pair[0]) in edges})
    if "--graph" in sys.argv:
        for (source, target), items in sorted(edges.items()):
            print(f"{source} -> {target}: {len(items)}")
        print(f"mutual module pairs: {len(mutual)}")
        for left, right in mutual:
            print(f"  {left} <-> {right}")
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"check-layers: {len(errors)} problem(s); see the layer order in scripts/check_layers.py",
              file=sys.stderr)
        return 1
    print(f"check-layers: ok ({len(files)} files, {len(edges)} module edges, "
          f"{len(mutual)} mutual pairs, {len(ALLOWLIST)} allowlisted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
