#!/usr/bin/env python3
"""Builds tests/corpus/dashpay_proof_corpus.json.

Inputs are live captures from tests/dashpay_capture.py; every verdict comes
from the official Dash Platform v4.1.1 verifier (tests/dashpay_oracle), not
from the native code under test.

Usage:
  python3 tests/dashpay_corpus_build.py ORACLE_BINARY OUT.json CAPTURE.json [CAPTURE.json ...]

Case families:
  real       every captured reply, as received
  mutation   one field or byte changed in a real reply
  forgery    a real signed reply restructured so that the official verifier
             accepts a false answer;
             the native verifier must reject these ("hardening")
  transport  gRPC-web framing/status cases the oracle does not judge (its
             decoder reads only the data frame); expected per gRPC semantics
"""
import base64
import json
import subprocess
import sys

DPNS = bytes.fromhex("e668c659af66aee1e72c186dde7b5b7e0a1d712a09c40d5721f622bf53c53155")
INDEX_PATH = [b"@", DPNS, b"\x01", b"domain", b"normalizedParentDomainName", b"dash", b"normalizedLabel"]


# ---- protobuf -----------------------------------------------------------------------------

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read_varint(b, i):
    shift = value = 0
    while True:
        c = b[i]
        i += 1
        value |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return value, i


def fields(b):
    """[(number, wire, value)] for a protobuf message (bytes for LEN, int for VARINT)."""
    out, i = [], 0
    while i < len(b):
        key, i = read_varint(b, i)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, i = read_varint(b, i)
        elif wire == 2:
            length, i = read_varint(b, i)
            value, i = b[i:i + length], i + length
        else:
            raise ValueError(f"wire {wire}")
        out.append((number, wire, value))
    return out


def encode(items):
    out = b""
    for number, wire, value in items:
        out += varint(number << 3 | wire)
        out += varint(value) if wire == 0 else varint(len(value)) + value
    return out


def frames(body):
    out, i = [], 0
    while i < len(body):
        flag, length = body[i], int.from_bytes(body[i + 1:i + 5], "big")
        out.append((flag, body[i + 5:i + 5 + length]))
        i += 5 + length
    return out


def frame(flag, payload):
    return bytes([flag]) + len(payload).to_bytes(4, "big") + payload


class Reply:
    """A captured gRPC-web reply split into editable parts."""

    def __init__(self, body):
        parts = frames(body)
        self.trailers = [p for f, p in parts if f & 0x80]
        message = [p for f, p in parts if not f & 0x80][0]
        (number, _, version), = fields(message)
        self.version = number
        inner = fields(version)
        self.proof = {n: v for n, _, v in fields([v for n, _, v in inner if n == 2][0])}
        self.metadata = {n: v for n, _, v in fields([v for n, _, v in inner if n == 3][0])}

    def body(self, proof=None, metadata=None, trailers=None, version=None):
        proof = {**self.proof, **(proof or {})}
        metadata = {**self.metadata, **(metadata or {})}
        proof_items = [(n, 0 if n in (4, 6) else 2, proof[n]) for n in sorted(proof)]
        meta_items = [(n, 2 if n == 6 else 0, metadata[n]) for n in sorted(metadata)]
        inner = encode([(2, 2, encode(proof_items)), (3, 2, encode(meta_items))])
        message = encode([(version or self.version, 2, inner)])
        out = frame(0, message)
        for t in (self.trailers if trailers is None else trailers):
            out += frame(0x80, t)
        return out


# ---- GroveDB envelope (bincode, big endian varint) -----------------------------------------

class Cursor:
    def __init__(self, b):
        self.b, self.i = b, 0

    def byte(self):
        self.i += 1
        return self.b[self.i - 1]

    def take(self, n):
        self.i += n
        return self.b[self.i - n:self.i]

    def varint(self):
        f = self.byte()
        return f if f < 251 else int.from_bytes(self.take({251: 2, 252: 4, 253: 8}[f]), "big")


def bvar(n):
    if n < 251:
        return bytes([n])
    return b"\xfb" + n.to_bytes(2, "big") if n < 1 << 16 else b"\xfc" + n.to_bytes(4, "big")


def parse_layer(c):
    variant = c.varint()
    merk = c.take(c.varint())
    children = []
    for _ in range(c.varint()):
        key = c.take(c.varint())
        children.append((key, parse_layer(c)))
    return {"variant": variant, "merk": merk, "children": children}


def encode_layer(layer):
    out = bvar(layer["variant"]) + bvar(len(layer["merk"])) + layer["merk"] + bvar(len(layer["children"]))
    for key, child in layer["children"]:
        out += bvar(len(key)) + key + encode_layer(child)
    return out


def parse_envelope(proof):
    c = Cursor(proof)
    assert c.varint() == 1
    root = parse_layer(c)
    assert c.i == len(proof)
    return root


def replace_at(root, path, replacement):
    if not path:
        return replacement(root)
    return {"variant": root["variant"], "merk": root["merk"],
            "children": [(k, replace_at(c, path[1:], replacement) if k == path[0] else c)
                         for k, c in root["children"]]}


def layer_at(root, path):
    for key in path:
        root = dict(root["children"])[key]
    return root


def merk_ops(merk):
    """[(tag, start, end, key, value, hash)] for the left-to-right op subset."""
    out, c = [], Cursor(merk)
    while c.i < len(merk):
        start, tag = c.i, c.byte()
        key = value = digest = None
        if tag in (0x10, 0x11):
            pass
        elif tag in (0x01, 0x02):
            digest = c.take(32)
        elif tag in (0x03, 0x04, 0x06):
            key = c.take(c.byte())
            value = c.take(int.from_bytes(c.take(2), "big"))
            if tag != 0x03:
                digest = c.take(32)
        elif tag == 0x05:
            key = c.take(c.byte())
            digest = c.take(32)
        else:
            raise ValueError(f"op {tag:#x}")
        out.append((tag, start, c.i, key, value, digest))
    return out


# ---- forgeries ------------------------------------------------------------------------------

class Blake3:
    def __init__(self, oracle):
        self.proc = subprocess.Popen([oracle, "blake3"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def __call__(self, data):
        self.proc.stdin.write(data.hex() + "\n")
        self.proc.stdin.flush()
        return bytes.fromhex(self.proc.stdout.readline().strip())


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def forge_hash_child(found, label, b3):
    """Hangs a fake `label` subtree (a document owned by 0x41*32) under the
    first eligible Hash node of the label layer of a real found proof."""
    root = parse_envelope(found.proof[1])
    layer = layer_at(root, INDEX_PATH)
    (found_key, leaf), = layer["children"]
    (tag, _, _, _, item, _), = merk_ops(leaf["merk"])
    assert tag == 0x06
    doc = bytearray(item[2:2 + item[1]])
    doc[33:65] = b"\x41" * 32
    fake_item = bytes([0, len(doc)]) + bytes(doc) + b"\x00"
    nvh = bytes(32)
    leaf_merk = b"\x06\x01\x00" + len(fake_item).to_bytes(2, "big") + fake_item + nvh
    combined = b3(nvh + b3(leb(len(fake_item)) + fake_item))
    leaf_root = b3(b3(leb(1) + b"\x00" + combined) + bytes(64))
    tree_value = bytes.fromhex("0201010000")
    fake_vh = b3(b3(leb(len(tree_value)) + tree_value) + leaf_root)
    target = label.encode()
    fake_push = b"\x04" + bytes([len(target)]) + target + len(tree_value).to_bytes(2, "big") + tree_value + fake_vh
    ops = merk_ops(layer["merk"])
    keyed = [(i, op[3]) for i, op in enumerate(ops) if op[3] is not None]
    for i, (tag, start, end, *_rest) in enumerate(ops):
        before = [k for j, k in keyed if j < i]
        after = [k for j, k in keyed if j > i]
        if tag != 0x01 or (before and not before[-1] < target) or (after and not target < after[0]):
            continue
        merk = layer["merk"][:end] + fake_push + b"\x11" + layer["merk"][end:]
        forged = replace_at(root, INDEX_PATH, lambda _: {
            "variant": 0, "merk": merk, "children": [(target, {"variant": 0, "merk": leaf_merk, "children": []})]})
        return found.body(proof={1: bvar(1) + encode_layer(forged)})
    raise ValueError("no eligible Hash node")


INVERT = {0x01: 0x08, 0x02: 0x09, 0x03: 0x0A, 0x04: 0x0B, 0x05: 0x0C, 0x06: 0x0D}


def tree_of(merk):
    stack = []
    for tag, start, end, *_ in merk_ops(merk):
        if tag == 0x10:
            parent, child = stack.pop(), stack.pop()
            parent["left"] = child
            stack.append(parent)
        elif tag == 0x11:
            child, parent = stack.pop(), stack.pop()
            parent["right"] = child
            stack.append(parent)
        else:
            stack.append({"tag": tag, "body": merk[start + 1:end], "left": None, "right": None})
    (top,) = stack
    return top


def right_to_left(node):
    out = b""
    if node["right"]:
        out += right_to_left(node["right"])
    out += bytes([INVERT[node["tag"]]]) + node["body"]
    if node["right"]:
        out += b"\x12"
    if node["left"]:
        out += right_to_left(node["left"]) + b"\x13"
    return out


def forge_inverted(reply):
    """Re-encodes the label layer with inverted (right-to-left) ops."""
    root = parse_envelope(reply.proof[1])
    forged = replace_at(root, INDEX_PATH, lambda layer: {
        "variant": layer["variant"], "merk": right_to_left(tree_of(layer["merk"])), "children": layer["children"]})
    return reply.body(proof={1: bvar(1) + encode_layer(forged)})


def forge_empty_tree(found, label):
    """Presents the found label's subtree as an empty Tree(None)."""
    root = parse_envelope(found.proof[1])
    target = label.encode()

    def rewrite(layer):
        merk, out = layer["merk"], b""
        for tag, start, end, key, value, digest in merk_ops(merk):
            if tag == 0x04 and key == target:
                value = bytes.fromhex("020000")
                out += b"\x04" + bytes([len(key)]) + key + len(value).to_bytes(2, "big") + value + digest
            else:
                out += merk[start:end]
        return {"variant": 0, "merk": out, "children": []}
    forged = replace_at(root, INDEX_PATH, rewrite)
    return found.body(proof={1: bvar(1) + encode_layer(forged)})


# ---- corpus -----------------------------------------------------------------------------------

def b64(data):
    return base64.b64encode(data).decode()


def main():
    oracle, out_path, captures = sys.argv[1], sys.argv[2], sys.argv[3:]
    b3 = Blake3(oracle)
    cases = []
    keys = {}
    capture_notes = []
    for capture_path in captures:
        capture = json.load(open(capture_path))
        capture_notes.append({"captured_at_ms": capture["captured_at_ms"], "nodes": capture["nodes"]})
        for path in ("/quorums", "/previous"):
            doc = json.loads(base64.b64decode(capture["trusted"][path]))
            rows = doc["data"] if isinstance(doc["data"], list) else doc["data"]["quorums"]
            for q in rows:
                keys[q["quorum_hash"]] = q["key"]
        for index, reply in enumerate(capture["replies"]):
            cases.append({"id": f"real/{reply['node']}/{reply['label']}", "family": "real",
                          "label": reply["label"], "body_b64": reply["body_b64"]})

    def quorum_of(case):
        reply = Reply(base64.b64decode(case["body_b64"]))
        return reply.proof.get(2, b"").hex()

    reals = [c for c in cases if c["family"] == "real"]
    by_label = {}
    for c in reals:
        by_label.setdefault(c["label"], c)
    found = Reply(base64.b64decode(by_label["dash"]["body_b64"]))
    miss = Reply(base64.b64decode(by_label["zzzznotarealname1234"]["body_b64"]))
    other_key = next(k for h, k in keys.items() if h != found.proof[2].hex())

    def add(case_id, family, label, body, **extra):
        case = {"id": case_id, "family": family, "label": label, "body_b64": b64(body)}
        case.update(extra)
        cases.append(case)

    def flip(data, at, bit=1):
        data = bytearray(data)
        data[at] ^= bit
        return bytes(data)

    for name, reply, label in (("found", found, "dash"), ("miss", miss, "zzzznotarealname1234")):
        p = reply.proof
        for at in (0, 1, 47, 48, 95):
            add(f"mutation/{name}/signature-byte-{at}", "mutation", label, reply.body(proof={3: flip(p[3], at)}))
        add(f"mutation/{name}/signature-zero", "mutation", label, reply.body(proof={3: bytes(96)}))
        add(f"mutation/{name}/signature-short", "mutation", label, reply.body(proof={3: p[3][:95]}))
        add(f"mutation/{name}/block-id", "mutation", label, reply.body(proof={5: flip(p[5], 7)}))
        add(f"mutation/{name}/block-id-short", "mutation", label, reply.body(proof={5: p[5][:31]}))
        add(f"mutation/{name}/round", "mutation", label, reply.body(proof={4: p.get(4, 0) + 1}))
        for quorum_type in (5, 6, 256):
            add(f"mutation/{name}/quorum-type-{quorum_type}", "mutation", label,
                reply.body(proof={6: quorum_type}))
        add(f"mutation/{name}/quorum-hash", "mutation", label, reply.body(proof={2: flip(p[2], 31)}))
        add(f"mutation/{name}/wrong-quorum-key", "mutation", label, reply.body(), quorum_key_hex=other_key)
        add(f"mutation/{name}/unknown-quorum", "mutation", label, reply.body(), quorum_key_hex=None)
        m = reply.metadata
        for field, delta in ((1, 1), (2, 1), (4, 1), (5, 1)):
            add(f"mutation/{name}/metadata-{field}", "mutation", label, reply.body(metadata={field: m.get(field, 0) + delta}))
        add(f"mutation/{name}/metadata-epoch", "mutation", label, reply.body(metadata={3: m.get(3, 0) + 1}))
        add(f"mutation/{name}/chain-id", "mutation", label, reply.body(metadata={6: b"evo2"}))
        g = p[1]
        for k in range(24):
            at = (len(g) - 1) * k // 23
            add(f"mutation/{name}/proof-byte-{at}", "mutation", label, reply.body(proof={1: flip(g, at)}))
        add(f"mutation/{name}/proof-truncated", "mutation", label, reply.body(proof={1: g[:-1]}))
        add(f"mutation/{name}/proof-trailing", "mutation", label, reply.body(proof={1: g + b"\x00"}))
        add(f"mutation/{name}/proof-empty", "mutation", label, reply.body(proof={1: b""}))
        add(f"mutation/{name}/v0-envelope", "mutation", label, reply.body(version=1))
        other = "alice" if label == "dash" else "dash"
        add(f"mutation/{name}/other-label-{other}", "mutation", other, reply.body())
    # The response without a proof result (documents rows instead).
    rows = encode([(1, 2, encode([(1, 2, b"\x00")])), (3, 2, encode([(n, 2 if n == 6 else 0, v)
                                                                     for n, v in sorted(found.metadata.items())]))])
    add("mutation/no-proof", "mutation", "dash", frame(0, encode([(2, 2, rows)])))
    add("forgery/hash-child/alice", "forgery", "alice", forge_hash_child(found, "alice", b3), hardening="hash-child")
    add("forgery/hash-child/aaa", "forgery", "aaa", forge_hash_child(found, "aaa", b3), hardening="hash-child")
    add("forgery/inverted/dash-absent", "forgery", "dash", forge_inverted(miss), hardening="inverted-ops")
    add("forgery/inverted/dash-found", "forgery", "dash", forge_inverted(found), hardening="inverted-ops")
    add("forgery/empty-tree/dash-absent", "forgery", "dash", forge_empty_tree(found, "dash"), hardening="empty-tree")
    # Transport (not oracle-judged): gRPC semantics.
    add("transport/grpc-status-14", "transport", "dash", found.body(trailers=[b"grpc-status:14\r\ngrpc-message:unavailable\r\n"]),
        transport_expect=False)
    add("transport/no-trailers", "transport", "dash", found.body(trailers=[]), transport_expect=True)
    add("transport/two-data-frames", "transport", "dash", found.body() + frame(0, b""), transport_expect=False)
    add("transport/compressed-flag", "transport", "dash",
        bytes([1]) + found.body()[1:], transport_expect=False)
    add("transport/truncated-frame", "transport", "dash", found.body()[:-3], transport_expect=False)

    for case in cases:
        if "quorum_key_hex" not in case:
            qh = quorum_of(case) if case["family"] != "mutation" or "no-proof" not in case["id"] else ""
            case["quorum_hash_hex"] = qh
            case["quorum_key_hex"] = keys.get(qh)
        else:
            case["quorum_hash_hex"] = found.proof[2].hex() if case["label"] == "dash" else miss.proof[2].hex()
    batch = "/tmp/dashpay_corpus_oracle_input.json"
    with open(batch, "w") as handle:
        json.dump({"cases": cases}, handle)
    verdicts = json.loads(subprocess.run([oracle, "batch", batch], check=True, capture_output=True,
                                         text=True).stdout)
    by_id = {v["id"]: v for v in verdicts}
    accepted = 0
    for case in cases:
        verdict = by_id[case["id"]]
        case["oracle"] = verdict
        if case["family"] == "forgery":
            if not verdict["ok"]:
                raise SystemExit(f"{case['id']}: the official verifier rejected the forgery: {verdict}")
            case["expect"] = {"ok": False}
        elif case["family"] == "transport":
            case["expect"] = {"ok": case.pop("transport_expect")}
            if case["expect"]["ok"]:
                case["expect"].update({k: verdict.get(k) for k in ("identity_id", "root_hash")})
        else:
            case["expect"] = {"ok": verdict["ok"]}
            if verdict["ok"]:
                case["expect"].update({k: verdict.get(k) for k in ("identity_id", "root_hash")})
        accepted += case["expect"]["ok"]
    corpus = {
        "description": "DashPay DPNS proof corpus: real mainnet getDocuments replies and derived cases; "
                       "verdicts from the official Dash Platform v4.1.1 verifier (tests/dashpay_oracle).",
        "generator": "tests/dashpay_corpus_build.py",
        "captures": capture_notes,
        "cases": cases,
    }
    with open(out_path, "w") as handle:
        json.dump(corpus, handle, indent=1)
        handle.write("\n")
    print(f"{out_path}: {len(cases)} cases, {accepted} expected to verify")


if __name__ == "__main__":
    main()
