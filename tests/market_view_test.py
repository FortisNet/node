#!/usr/bin/env python3
"""Replay tests/corpus/market_view_corpus.json through src/discovery/market_view.cpp.

What is promised: the market view the native server builds from a provider
row is, byte for byte, the one the earlier TypeScript implementation built:
the entry JSON (number formatting, case mapping, sparkline, dates), whether a
date string is one JavaScript's Date.parse accepts, whether an image link is
an http(s) URL, and the media key derived from it.

Where the expected answers come from: the corpus was frozen from the
TypeScript implementation by its own tests (see "source" in the corpus). All
the expected values are in that file: the entry text, the Date.parse verdict
of each date and the URL verdict and media key of each link. Nothing is
computed here by Python's own date or URL handling, and no JavaScript engine
is consulted (the earlier JavaScript version of this test did not consult one
either; it only parsed the rows as JSON to tell objects from other values,
which Python's json module does identically for this purpose).

One known difference is kept from the earlier test: the URL verdicts were
frozen under the Node.js named in "source.node". Production ran Node.js 22
(Ada 2.9.2), which src/compat/whatwg_url.cpp reproduces exactly
(tests/whatwg_url_test.py). Ada 3.x rejects an xn-- label whose punycode
decodes to ASCII only, which Ada 2.9.2 accepts; when the corpus was frozen
under a Node.js newer than 22, such a URL must be accepted by the native code
although the corpus says it is not.

How the driver is used: build/market_view_test reads one request per line
  "E <id hex> <row hex>" -> the entry JSON in hex, "-" for a row that is not a
                            JSON object (the route never builds an entry for
                            one), "!" on a refusal
  "D <hex>"              -> "1" when Date.parse would succeed, else "0"
  "U <hex>"              -> "<0|1> <media key or ->"
where hex is the hex of the UTF-8 bytes.

Usage: market_view_test.py <binary> [corpus file]
The optional corpus file (with a "<file>.sha256" beside it) replaces
tests/corpus/market_view_corpus.json; it exists so that a deliberately wrong
copy can prove the test still fails.
Exit status: 0 everything matches, 1 a mismatch or a broken run, 2 usage.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

DEFAULT_DATA = Path(__file__).resolve().parent / "corpus" / "market_view_corpus.json"

# Accepted by the production parser (Ada 2.9.2), rejected by Ada 3.x.
ADA_2_9_2_ACCEPTS = {"https://xn--abc-/"}


def load(path):
    """Read the corpus after checking it against its pinned hash."""
    data = path.read_bytes()
    pinned = Path(f"{path}.sha256").read_text().split()[0]
    if hashlib.sha256(data).hexdigest() != pinned:
        raise SystemExit(f"{path.name} does not match its pinned hash")
    return json.loads(data)


def utf8_hex(text):
    """Hex of the UTF-8 bytes, as Buffer.from(text, 'utf8').toString('hex').

    The corpus strings were JavaScript (UTF-16) strings. One that holds a lone
    surrogate is written by that call with U+FFFD (ef bf bd) in its place, so
    the same is done here.
    """
    try:
        return text.encode("utf-8").hex()
    except UnicodeEncodeError:
        units = text.encode("utf-16-le", "surrogatepass")
        return units.decode("utf-16-le", "replace").encode("utf-8").hex()


def unhex_text(field):
    """Buffer.from(field, 'hex').toString('utf8'): stops at the first pair
    that is not hex, and decodes invalid UTF-8 with U+FFFD."""
    digits = "0123456789abcdefABCDEF"
    end = 0
    while end + 1 < len(field) and field[end] in digits and field[end + 1] in digits:
        end += 2
    return bytes.fromhex(field[:end]).decode("utf-8", "replace")


def refuse_constant(name):
    raise ValueError(f"{name} is not JSON")


def is_json_object(text):
    """True when the row is a JSON object. Strict JSON, as JSON.parse: Python
    would otherwise accept NaN and Infinity."""
    return isinstance(json.loads(text, parse_constant=refuse_constant), dict)


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: market_view_test.py <binary> [corpus file]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    fixture = load(Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_DATA)
    node = fixture["source"]["node"]

    lines = [f"E {utf8_hex(item['id'])} {utf8_hex(item['rowText'])}" for item in fixture["entries"]]
    lines += [f"D {utf8_hex(item['text'])}" for item in fixture["dates"]]
    lines += [f"U {utf8_hex(item['text'])}" for item in fixture["urls"]]
    run = subprocess.run([binary], input=("\n".join(lines) + "\n").encode("ascii"),
                         stdout=subprocess.PIPE, check=False)
    if run.returncode != 0:
        print(f"binary exited {run.returncode}", file=sys.stderr)
        return 1
    output = run.stdout.decode("utf-8", "replace").rstrip().split("\n")
    output += [None] * (len(lines) - len(output))  # a missing line fails its case

    known_differences = ADA_2_9_2_ACCEPTS if int(node.split(".")[0]) > 22 else set()
    failures = 0
    known = 0

    def fail(message):
        nonlocal failures
        failures += 1
        if failures <= 20:
            print(message, file=sys.stderr)

    index = 0
    for item in fixture["entries"]:
        actual = output[index]
        index += 1
        if not is_json_object(item["rowText"]):
            if actual != "-":
                fail(f"{item['id']}: expected no entry, got {actual}")
            continue
        if actual is None:
            fail(f"{item['id']}: no output line")
            continue
        text = "!" if actual == "!" else unhex_text(actual)
        if text != item["entry"]:
            fail(f"{item['id']}:\n  expected {item['entry']}\n  actual   {text}")
    for item in fixture["dates"]:
        actual = output[index] == "1"
        index += 1
        if actual != item["valid"]:
            fail(f"Date.parse({json.dumps(item['text'])}): expected {json.dumps(item['valid'])}")
    for item in fixture["urls"]:
        fields = (output[index] or "").split(" ")
        index += 1
        http = fields[0]
        key = fields[1] if len(fields) > 1 else None
        if item["text"] in known_differences:
            if http != "1" or item["http"]:
                fail(f"url {item['text']}: expected Ada 2.9.2 to accept it over Node {node}")
            known += 1
            continue
        expected_key = "-" if item.get("key") is None else item["key"]
        if (http == "1") != item["http"] or expected_key != key:
            fail(f"url {json.dumps(item['text'])}: expected {json.dumps(item['http'])} "
                 f"{item.get('key')}, got {http} {key}")
    if failures:
        print(f"{failures} mismatches", file=sys.stderr)
        return 1
    print(f"market view: {len(fixture['entries'])} entries, {len(fixture['dates'])} dates, "
          f"{len(fixture['urls']) - known} URLs match TypeScript ({known} judged by the "
          f"production Ada 2.9.2 instead of Node {node})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
