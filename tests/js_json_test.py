#!/usr/bin/env python3
"""Check src/compat/js_json.cpp against recorded JavaScript answers.

What is promised: for a JSON document the server
  * re-serializes it as JavaScript's JSON.stringify(JSON.parse(text)) would
    format every number and string (1e21 -> "1e+21", -0 -> "0", shortest
    round-trip digits, the same string escapes), keeping object members in
    source order (a repeated key keeps its first position and its last value);
  * encodes it as a google.protobuf.Value exactly as protobuf-es 2.x does;
  * counts the elements of a top-level array.

Where the expected answers come from: they are the node's own reference,
tests/corpus/js_json_expectations.json. They were first recorded from
Node.js and @bufbuild/protobuf (the header of that file names the versions)
and are now maintained here: a deliberate change of behaviour changes that
file in the same commit. This test only replays it: it never parses or formats a document
itself, and it does not reproduce the JavaScript random generator (the seeded
documents are stored as data).

What is exercised: the hand-written edge cases of the earlier JavaScript
version of this test and 3000 seeded documents (nested objects and arrays up
to the accepted depth of 16, numbers in many spellings and magnitudes, strings
with escapes, non-ASCII and lone surrogate escapes, repeated and integer-like
keys). The seeded documents come from the recorder's generator, which
replaces the earlier test's (that one produced almost only `null`).

How the driver is used: build/js_json_test reads one document per line, as
the hex of its UTF-8 bytes, and prints
"<output JSON hex> <protobuf hex> <array element count or -1>", or "!" when it
refuses the document. No recorded document may be refused.

Usage: js_json_test.py <binary> [data file]
The optional data file (with a "<file>.sha256" beside it) replaces
tests/corpus/js_json_expectations.json; it exists so that a deliberately
wrong copy can prove the test still fails.
Exit status: 0 all documents match, 1 a mismatch or a broken run, 2 usage.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

DEFAULT_DATA = Path(__file__).resolve().parent / "corpus" / "js_json_expectations.json"


def load(path):
    """Read the data file after checking it against its pinned hash."""
    data = path.read_bytes()
    pinned = Path(f"{path}.sha256").read_text().split()[0]
    if hashlib.sha256(data).hexdigest() != pinned:
        raise SystemExit(f"{path.name} does not match its pinned hash")
    return json.loads(data)


def utf8_hex(text):
    """Hex of the UTF-8 bytes, as Buffer.from(text, 'utf8').toString('hex').

    A JavaScript string is UTF-16 and may hold a lone surrogate, which that
    call writes as U+FFFD (ef bf bd). No recorded string has one today; the
    rule is kept so that a future recording is encoded here exactly as the
    recorder encoded it (the driver input hash below would show a difference).
    """
    try:
        return text.encode("utf-8").hex()
    except UnicodeEncodeError:
        units = text.encode("utf-16-le", "surrogatepass")
        return units.decode("utf-16-le", "replace").encode("utf-8").hex()


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: js_json_test.py <binary> [data file]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    fixture = load(Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_DATA)
    source = fixture["source"]
    documents = fixture["documents"]

    stdin = "".join(utf8_hex(item["text"]) + "\n" for item in documents).encode("ascii")
    # The recorder hashed the driver input as JavaScript encodes it (hex of
    # Buffer.from(text, 'utf8') per document). Equal hashes mean this test
    # sends exactly those documents, in the same order, byte for byte.
    if hashlib.sha256(stdin).hexdigest() != source["stdinSha256"]:
        print("the documents built here are not the documents that were recorded "
              "(driver input hash differs)", file=sys.stderr)
        return 1

    run = subprocess.run([binary], input=stdin, stdout=subprocess.PIPE, check=False)
    if run.returncode != 0:
        print(f"binary exited {run.returncode}", file=sys.stderr)
        return 1
    lines = run.stdout.decode("utf-8", "replace").rstrip().split("\n")

    failures = 0
    refused = 0
    for index, item in enumerate(documents):
        doc = item["text"]
        line = lines[index] if index < len(lines) else None
        if line is None:
            failures += 1
            print(f"no output line for: {doc}", file=sys.stderr)
            continue
        if line == "!":
            refused += 1
            failures += 1
            print(f"refused: {doc}", file=sys.stderr)
            continue
        fields = line.split(" ")
        fields += [None] * (3 - len(fields))
        checks = (("stringify", fields[0], utf8_hex(item["json"])),
                  ("protobuf", fields[1], item["proto"]),
                  ("elements", fields[2], str(item["elements"])))
        for name, actual, expected in checks:
            if actual != expected:
                failures += 1
                if failures <= 10:
                    print(f"{name} {doc}\n  expected {expected}\n  actual   {actual}",
                          file=sys.stderr)
    if failures:
        print(f"{failures} mismatches ({refused} refused)", file=sys.stderr)
        return 1
    print(f"js json: {len(documents)} documents match recorded source-order JSON "
          f"(Node.js {source['node']}) and protobuf scalar/wire reference "
          f"{source['protobufLine']} ({source['protobuf']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
