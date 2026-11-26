#!/usr/bin/env python3
"""Replay tests/corpus/whatwg_url_corpus.json through src/compat/whatwg_url.cpp.

What is promised: the native URL parser gives, for every input, what
`new URL(input, base)` gave in the production TypeScript runtime
(node:22-alpine, Ada 2.9.2): the same ten components, or the same refusal.
The address filters that decide what the server may fetch depend on it.

Where the expected answers come from: the corpus was frozen from that runtime
by tests/reference/generate_whatwg_url_corpus.mjs ("source" in the corpus names the
image, Node.js, Ada and ICU versions). This test never consults Python's
urllib or any other URL implementation; it only compares the driver's output
with the corpus.

Three kinds of request, as in the earlier JavaScript version of this test:
  cases      "P <input> <base | ->": the ten components (href protocol
             username password host hostname port pathname search hash), tab
             separated, or "null" when parsing throws, or "basefail" when the
             base does not parse. An input longer than the native bound
             (kMaxInputBytes in src/compat/whatwg_url.h) must be refused
             whatever the runtime answered.
  canonical  "C <input>": the canonical article URL, or "null".
  exhaustive "X <prefix> <suffix>" and "Y <prefix> <suffix> <before> <after>":
             the driver itself parses prefix + code point + suffix for EVERY
             Unicode scalar value from U+0080 to U+10FFFF (for Y the code
             point is first put in a punycode label between `before` and
             `after`), writes "<code point hex> <href or null>\\n" for each and
             prints the number of probes, the SHA-256 of all those lines and
             one SHA-256 per block of 4096 code points. The corpus holds the
             hashes of the same lines produced by the production runtime, so
             equal hashes mean every single probe agrees; the block hashes
             say where a difference lies. That is 1111936 probes per entry
             without storing them.
Every field travels as the hex of its UTF-8 bytes, "-" for an empty one.

Usage: whatwg_url_test.py <binary> [corpus file]
The optional corpus file (with a "<file>.sha256" beside it) replaces
tests/corpus/whatwg_url_corpus.json; it exists so that a deliberately wrong
copy can prove the test still fails.
Exit status: 0 everything matches, 1 a mismatch or a broken run, 2 usage.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

DEFAULT_DATA = Path(__file__).resolve().parent / "corpus" / "whatwg_url_corpus.json"

KEYS = ["href", "protocol", "username", "password", "host", "hostname", "port", "pathname",
        "search", "hash"]
# Native bound (src/compat/whatwg_url.h kMaxInputBytes): longer inputs are refused.
MAX_INPUT_BYTES = 8192


def load(path):
    """Read the corpus after checking it against its pinned hash."""
    data = path.read_bytes()
    pinned = Path(f"{path}.sha256").read_text().split()[0]
    if hashlib.sha256(data).hexdigest() != pinned:
        raise SystemExit(f"{path.name} does not match its pinned hash")
    return json.loads(data)


def utf8_bytes(text):
    """The UTF-8 bytes, as Buffer.from(text, 'utf8').

    The corpus strings were JavaScript (UTF-16) strings and some hold a lone
    surrogate, which has no UTF-8 form: that call writes U+FFFD (ef bf bd) in
    its place, so the same is done here.
    """
    try:
        return text.encode("utf-8")
    except UnicodeEncodeError:
        units = text.encode("utf-16-le", "surrogatepass")
        return units.decode("utf-16-le", "replace").encode("utf-8")


def hex_field(text):
    return "-" if text == "" else utf8_bytes(text).hex()


def unhex_field(field):
    """Inverse of hex_field, as Buffer.from(field, 'hex').toString('utf8'):
    stops at the first pair that is not hex, decodes invalid UTF-8 with U+FFFD."""
    if field == "-":
        return ""
    digits = "0123456789abcdefABCDEF"
    end = 0
    while end + 1 < len(field) and field[end] in digits and field[end + 1] in digits:
        end += 2
    return bytes.fromhex(field[:end]).decode("utf-8", "replace")


def short(text):
    return json.dumps(text[:117] + "..." if len(text) > 120 else text)


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: whatwg_url_test.py <binary> [corpus file]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    fixture = load(Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_DATA)

    lines = []
    for item in fixture["cases"]:
        lines.append(f"P {hex_field(item['input'])} "
                     f"{hex_field(item['base']) if 'base' in item else '-'}")
    for item in fixture["canonical"]:
        lines.append(f"C {hex_field(item['input'])}")
    for item in fixture["exhaustive"]:
        if "before" not in item:
            lines.append(f"X {hex_field(item['prefix'])} {hex_field(item['suffix'])}")
        else:
            lines.append(f"Y {hex_field(item['prefix'])} {hex_field(item['suffix'])} "
                         f"{hex_field(item['before'])} {hex_field(item['after'])}")
    run = subprocess.run([binary], input=("\n".join(lines) + "\n").encode("ascii"),
                         stdout=subprocess.PIPE, check=False)
    if run.returncode != 0:
        print(f"binary exited {run.returncode}", file=sys.stderr)
        return 1
    output = run.stdout.decode("utf-8", "replace").rstrip().split("\n")
    if len(output) < len(lines):
        print(f"binary answered {len(output)} of {len(lines)} requests", file=sys.stderr)
        return 1

    failures = 0
    bounded = 0

    def fail(message):
        nonlocal failures
        failures += 1
        if failures <= 40:
            print(message, file=sys.stderr)

    index = 0
    for item in fixture["cases"]:
        actual = output[index]
        index += 1
        label = short(item["input"]) + (f" against {short(item['base'])}" if "base" in item else "")
        if len(utf8_bytes(item["input"])) > MAX_INPUT_BYTES:
            if actual != "null":
                fail(f"{label}: an input over {MAX_INPUT_BYTES} bytes must be refused")
            bounded += 1
            continue
        if item["result"] is None:
            if actual != "null":
                fail(f"{label}: expected a throw, got {unhex_field(actual.split(chr(9))[0])}")
            continue
        if actual in ("null", "basefail"):
            fail(f"{label}: expected {item['result']['href']}, got {actual}")
            continue
        fields = [unhex_field(field) for field in actual.split("\t")]
        fields += [None] * (len(KEYS) - len(fields))
        for position, key in enumerate(KEYS):
            if fields[position] != item["result"][key]:
                fail(f"{label}: {key} expected {json.dumps(item['result'][key])}, "
                     f"got {json.dumps(fields[position])}")
    for item in fixture["canonical"]:
        actual = output[index]
        index += 1
        value = None if actual == "null" else unhex_field(actual)
        if value != item["result"]:
            fail(f"canonicalUrl({json.dumps(item['input'])}): expected "
                 f"{json.dumps(item['result'])}, got {json.dumps(value)}")
    probes = 0
    for item in fixture["exhaustive"]:
        fields = output[index].split(" ")
        index += 1
        try:
            count = int(fields[0])
        except ValueError:
            count = None
        total = fields[1] if len(fields) > 1 else None
        blocks = fields[2:]
        probes += count or 0
        if count != item["count"]:
            fail(f"exhaustive {json.dumps(item['prefix'])}…: {fields[0]} probes, "
                 f"expected {item['count']}")
        if total != item["sha256"]:
            expected_blocks = item["blocks"]
            bad = [str(block) for block, digest in enumerate(blocks)
                   if block >= len(expected_blocks) or digest != expected_blocks[block]]
            fail(f"exhaustive {json.dumps(item['prefix'])}<cp>{json.dumps(item['suffix'])}: "
                 f"hash differs in blocks {','.join(bad)}")
    if failures:
        print(f"{failures} mismatches", file=sys.stderr)
        return 1
    source = fixture["source"]
    print(f"whatwg url: {len(fixture['cases'])} cases, {len(fixture['canonical'])} canonical URLs, "
          f"{probes} exhaustive code-point probes match {source['image'].split('@')[0]} "
          f"(Ada {source['ada']}); {bounded} over-bound input refused by design")
    return 0


if __name__ == "__main__":
    sys.exit(main())
