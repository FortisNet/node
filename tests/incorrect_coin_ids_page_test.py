#!/usr/bin/env python3
"""Static check of the "Incorrect coin IDs" panel of the errors page.

The page is the HTML in kErrorsPageHtml (src/operations/operations_pages.cpp).
Its script fetches /errorsz and sorts the operational records into four
panels. The rule is:
  * a record whose code is provider_rejected_coin_id or invalid_identifier
    goes to "Incorrect coin IDs" and to no other panel;
  * any other record goes to "Node errors" (origin node), "Third-party
    errors" (origin third_party) or "Unclassified outcomes" (anything else).
So a provider rate limit (http_rate_limited, 429) or a missing quote
(quote_unavailable) never appears as an incorrect coin ID.

THIS TEST DOES NOT EXECUTE THE PAGE'S SCRIPT. Python's standard library
cannot run JavaScript, so what the script displays for given records is not
checked here; try the page in a browser after changing it. The test checks
the source text instead:
  1. the page names the panel, says it is read-only and has no form or
     control that could submit anything;
  2. the script cannot write over the network: its only request is one
     fetch('/errorsz') without options (a GET), and it uses no other network
     interface;
  3. the statements that implement the rule above are present, written
     exactly as reviewed (compared after removing white space), and each
     panel element exists and is filled from the matching selection.
Point 3 pins the reviewed text: a harmless rewording of the script fails
here and the fragments below must then be reviewed and updated by hand. A
change elsewhere in the script that alters what is displayed (for example in
the row template, or a later statement that overwrites a panel) is NOT
detected. `transcription_check` restates the rule in Python on the four
records of the JavaScript test; it documents what the pinned statements
mean and proves nothing about the page beyond them.

Usage: incorrect_coin_ids_page_test.py [operations_pages.cpp]
Exit status: 0 passed, 1 failed.
"""
import re
import sys
from pathlib import Path

DEFAULT_SOURCE = Path(__file__).resolve().parent.parent / "src" / "operations" / "operations_pages.cpp"

# The reviewed statements of the classification, without white space.
PINNED = [
    # Which records count as incorrect coin IDs.
    "function incorrect(e) { return e.code === 'provider_rejected_coin_id' || "
    "e.code === 'invalid_identifier'; }",
    # The records come from the fetched reply.
    "var incidents = operational.records || [];",
    # One selection per panel: incorrect IDs first, everything else by origin.
    "var selected = incidents.filter(function(e) {"
    " var known = e.origin === 'node' || e.origin === 'third_party';"
    " return origin === 'incorrect' ? incorrect(e) :"
    " !incorrect(e) && (known ? e.origin : 'unclassified') === origin;"
    " });",
    # Each panel is filled from its own selection.
    "$('e-incorrect-ids').innerHTML = table('incorrect', 'No incorrect coin IDs reported.');",
    "$('e-node').innerHTML = table('node', 'No node errors recorded.');",
    "$('e-third-party').innerHTML = table('third_party', 'No third-party errors recorded.');",
    "$('e-unclassified').innerHTML = table('unclassified', 'No unclassified outcomes recorded.');",
    # The code and the identifier of a selected record are shown, escaped.
    "'</td><td>'+esc(e.code)+'</td><td>'+esc(e.identifier || '—')+'</td>",
    # The only request.
    "fetch('/errorsz').then(function(r) { return r.json(); }).then(render)",
]
PANELS = ["e-incorrect-ids", "e-node", "e-third-party", "e-unclassified"]
# Interfaces that could send data, and request options that make a write.
FORBIDDEN_IN_SCRIPT = [
    r"XMLHttpRequest", r"sendBeacon", r"WebSocket", r"EventSource", r"\bimport\s*\(",
    r"\.submit\s*\(", r"\bmethod\s*:", r"\bbody\s*:", r"\b(POST|PUT|PATCH|DELETE)\b",
    r"location\s*(\.\w+\s*)?=", r"\.src\s*=", r"\.href\s*=", r"window\.open", r"\beval\s*\(",
    r"document\.write", r"createElement",
]


def squeeze(text):
    return re.sub(r"\s+", "", text)


def transcription_check():
    """The rule, restated in Python, on the records of the JavaScript test."""
    records = [
        {"code": "provider_rejected_coin_id", "origin": "third_party", "identifier": "old-coin"},
        {"code": "invalid_identifier", "origin": "unclassified", "identifier": ""},
        {"code": "http_rate_limited", "origin": "third_party"},
        {"code": "quote_unavailable", "origin": "unclassified", "identifier": "bitcoin"},
    ]

    def incorrect(record):
        return record["code"] in ("provider_rejected_coin_id", "invalid_identifier")

    def panel(origin):
        chosen = []
        for record in records:
            known = record["origin"] in ("node", "third_party")
            if origin == "incorrect":
                keep = incorrect(record)
            else:
                keep = not incorrect(record) and (record["origin"] if known else "unclassified") == origin
            if keep:
                chosen.append(record["code"])
        return chosen

    return (panel("incorrect") == ["provider_rejected_coin_id", "invalid_identifier"]
            and panel("third_party") == ["http_rate_limited"]
            and panel("unclassified") == ["quote_unavailable"]
            and panel("node") == [])


def main():
    source = (Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SOURCE).read_text(encoding="utf-8")
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    # The page, extracted as the JavaScript test extracts it.
    opened = source.split('R"FERROR(')
    if len(opened) != 2 or ')FERROR"' not in opened[1]:
        print("FAIL: the errors page literal R\"FERROR(...)FERROR\" was not found exactly once",
              file=sys.stderr)
        return 1
    html = opened[1].split(')FERROR"')[0]
    scripts = re.findall(r"<script\b[^>]*>(.*?)</script>", html, re.S)
    check(len(scripts) == 1, f"expected one <script> in the page, found {len(scripts)}")
    check(not re.search(r"<script\b[^>]*\bsrc\s*=", html, re.I), "the page loads an external script")
    script = scripts[0] if scripts else ""

    # 1. Read-only page (the first three are the assertions of the JavaScript test).
    check("Incorrect coin IDs" in html, "the page does not name the panel 'Incorrect coin IDs'")
    check("read-only" in html, "the page does not say that the view is read-only")
    check(not re.search(r'<form|method="post"', html, re.I), "the page has a form or a POST method")
    check(not re.search(r"<(input|button|textarea|select)\b", html, re.I),
          "the page has an input control")
    check(not re.search(r"\son\w+\s*=", html.replace(script, ""), re.I),
          "the page has an inline event handler outside the script")

    # 2. No network write.
    fetches = re.findall(r"\bfetch\s*\(([^)]*)\)", script)
    check(fetches == ["'/errorsz'"],
          f"the script must make exactly one request, fetch('/errorsz'), found {fetches}")
    for pattern in FORBIDDEN_IN_SCRIPT:
        check(not re.search(pattern, script), f"the script contains /{pattern}/")

    # 3. The classification statements, and the panels they fill.
    squeezed = squeeze(script)
    for fragment in PINNED:
        check(squeeze(fragment) in squeezed, f"reviewed statement not found in the script: {fragment}")
    for panel in PANELS:
        check(len(re.findall(rf'\bid="{panel}"', html)) == 1, f"no single element with id {panel}")
        check(len(re.findall(rf"\$\('{panel}'\)\.innerHTML\s*=", script)) == 1,
              f"{panel} is not assigned exactly once")
    for code in ("provider_rejected_coin_id", "invalid_identifier"):
        check(script.count(code) == 1, f"{code} is tested in more than one place")
    check(transcription_check(), "the Python restatement of the rule is inconsistent")

    if failures:
        for message in failures:
            print(f"FAIL: {message}", file=sys.stderr)
        return 1
    print("Incorrect coin IDs page: static read-only and classification-source checks passed "
          "(the script is not executed; weaker than the JavaScript test).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
