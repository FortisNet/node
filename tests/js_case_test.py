#!/usr/bin/env python3
"""Check src/compat/js_case.cpp against recorded JavaScript answers.

What is promised: JsToUpperCase / JsToLowerCase return exactly what
String.prototype.toUpperCase / toLowerCase return in the JavaScript engine
the server replaced (full Unicode case mapping, including one-to-many
mappings such as "ß" -> "SS" and the Final_Sigma rule for "Σ").

Where the expected answers come from: Python cannot be the reference (its
Unicode version and its special-casing differ from the engine's), so the
engine's own answers were recorded once, by
tests/reference/record_js_case.mjs, into
tests/corpus/js_case_expectations.json. The header of that file names the
engine and the Unicode version. This test only replays it; no JavaScript
engine is needed.

What is exercised (as the earlier JavaScript version of this test, except that the
mixed strings come from the recorder's corrected generator; the earlier one
produced only 29 different strings):
  * every Unicode scalar value alone (U+0000..U+10FFFF without the surrogates
    U+D800..U+DFFF), upper-cased and lower-cased. The data file lists only the
    scalar values that change; every other one must come back unchanged;
  * hand-written Final_Sigma and special-casing contexts;
  * 20000 seeded mixed strings, stored with their answers.

How the driver is used: build/js_case_test reads lines "U <hex>" / "L <hex>"
(hex of the UTF-8 input) and prints the hex of the UTF-8 result per line, "!"
when it refuses the input, and finally "unicode <version>" for the version of
its compiled-in tables. That version must equal the recorded engine's.

Usage: js_case_test.py <binary> [data file]
The optional data file (with a "<file>.sha256" beside it) replaces
tests/corpus/js_case_expectations.json; it exists so that a deliberately
wrong copy can prove the test still fails.
Exit status: 0 all strings match, 1 a mismatch or a broken run, 2 usage.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

DEFAULT_DATA = Path(__file__).resolve().parent / "corpus" / "js_case_expectations.json"


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
        print("usage: js_case_test.py <binary> [data file]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    fixture = load(Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_DATA)
    source = fixture["source"]

    # Scalar values that change, by code point. Anything absent is an
    # identity case: both operations must return the input.
    changed = {}
    for code, upper, lower in fixture["scalars"]:
        changed[int(code, 16)] = (upper, lower)

    # cases: (input hex, expected upper hex, expected lower hex), in the order
    # the earlier test sent them.
    cases = []
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        own = chr(cp).encode("utf-8").hex()
        answer = changed.get(cp)
        if answer is None:
            cases.append((own, own, own))
        else:
            cases.append((own, utf8_hex(answer[0]), utf8_hex(answer[1])))
    for text, upper, lower in fixture["contexts"] + fixture["mixed"]:
        cases.append((utf8_hex(text), utf8_hex(upper), utf8_hex(lower)))

    stdin = "".join(f"U {item[0]}\nL {item[0]}\n" for item in cases).encode("ascii")
    # The recorder hashed the driver input as JavaScript encodes it (hex of
    # Buffer.from(text, 'utf8') per string). Equal hashes mean this test asks
    # exactly the recorded questions, in the same order, byte for byte, and
    # that the identity cases were derived correctly.
    if len(cases) != source["strings"] or hashlib.sha256(stdin).hexdigest() != source["stdinSha256"]:
        print(f"the {len(cases)} strings built here are not the {source['strings']} strings "
              "that were recorded (driver input hash differs)", file=sys.stderr)
        return 1

    run = subprocess.run([binary], input=stdin, stdout=subprocess.PIPE, check=False)
    if run.returncode != 0:
        print(f"binary exited {run.returncode}", file=sys.stderr)
        return 1
    output = run.stdout.decode("utf-8", "replace").rstrip().split("\n")
    version = output.pop()
    if version != f"unicode {source['unicode']}":
        print(f"tables are {version}, the recorded {source['engine']} has Unicode "
              f"{source['unicode']}", file=sys.stderr)
        return 1

    failures = 0
    for index, (own, upper, lower) in enumerate(cases):
        for offset, method, expected in ((0, "toUpperCase", upper), (1, "toLowerCase", lower)):
            at = index * 2 + offset
            actual = output[at] if at < len(output) else None
            if actual != expected:
                failures += 1
                if failures <= 10:
                    text = bytes.fromhex(own).decode("utf-8")
                    print(f"{method}({json.dumps(text)}): expected {expected}, got {actual}",
                          file=sys.stderr)
    if failures:
        print(f"{failures} mismatches", file=sys.stderr)
        return 1
    print(f"js case: {len(cases)} strings match recorded Node.js {source['node']} "
          f"(Unicode {source['unicode']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
