#!/usr/bin/env python3
"""Test-only EIP-191 personal_sign in pure Python (Keccak-256, secp256k1,
EIP-55), for the AI conversation native and guest tests, which must sign
the proof message for a device id the server assigns at run time.

Not constant-time and not for real keys: tests use fixed throwaway keys.
"""
import hashlib
import hmac

_RC = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
    0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
    0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
_ROT = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61], [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]
_MASK = (1 << 64) - 1


def _rol(value, shift):
    return ((value << shift) | (value >> (64 - shift))) & _MASK if shift else value


def _permute(a):
    for rc in _RC:
        c = [a[x][0] ^ a[x][1] ^ a[x][2] ^ a[x][3] ^ a[x][4] for x in range(5)]
        d = [c[(x - 1) % 5] ^ _rol(c[(x + 1) % 5], 1) for x in range(5)]
        a = [[a[x][y] ^ d[x] for y in range(5)] for x in range(5)]
        b = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                b[y][(2 * x + 3 * y) % 5] = _rol(a[x][y], _ROT[x][y])
        a = [[b[x][y] ^ ((~b[(x + 1) % 5][y]) & b[(x + 2) % 5][y]) for y in range(5)] for x in range(5)]
        a[0][0] ^= rc
    return a


def keccak256(data: bytes) -> bytes:
    rate = 136
    padded = bytearray(data) + b"\x01" + b"\x00" * ((rate - (len(data) + 1) % rate) % rate)
    padded[-1] |= 0x80
    state = [[0] * 5 for _ in range(5)]
    for offset in range(0, len(padded), rate):
        block = padded[offset:offset + rate]
        for i in range(rate // 8):
            x, y = i % 5, i // 5
            state[x][y] ^= int.from_bytes(block[8 * i:8 * i + 8], "little")
        state = _permute(state)
    out = b"".join(state[i % 5][i // 5].to_bytes(8, "little") for i in range(4))
    return out


P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def _add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    if p[0] == q[0] and (p[1] + q[1]) % P == 0:
        return None
    if p == q:
        slope = 3 * p[0] * p[0] * pow(2 * p[1], P - 2, P) % P
    else:
        slope = (q[1] - p[1]) * pow(q[0] - p[0], P - 2, P) % P
    x = (slope * slope - p[0] - q[0]) % P
    return x, (slope * (p[0] - x) - p[1]) % P


def _multiply(k, point=G):
    result = None
    while k:
        if k & 1:
            result = _add(result, point)
        point = _add(point, point)
        k >>= 1
    return result


def checksum(address_hex: str) -> str:
    lower = address_hex.lower().removeprefix("0x")
    digest = keccak256(lower.encode()).hex()
    return "0x" + "".join(c.upper() if c in "abcdef" and int(digest[i], 16) >= 8 else c for i, c in enumerate(lower))


class Signer:
    def __init__(self, private_key: int):
        self.key = private_key
        public = _multiply(private_key)
        raw = public[0].to_bytes(32, "big") + public[1].to_bytes(32, "big")
        self.address = checksum(keccak256(raw)[12:].hex())

    def sign_message(self, message: str) -> str:
        data = message.encode()
        z = int.from_bytes(keccak256(b"\x19Ethereum Signed Message:\n" + str(len(data)).encode() + data), "big")
        # RFC 6979-style deterministic nonce (HMAC-SHA256 over key and digest).
        k = int.from_bytes(hmac.new(self.key.to_bytes(32, "big"), z.to_bytes(32, "big"), hashlib.sha256).digest(),
                           "big") % (N - 1) + 1
        point = _multiply(k)
        r = point[0] % N
        s = pow(k, N - 2, N) * (z + r * self.key) % N
        parity = point[1] & 1
        if s > N // 2:
            s = N - s
            parity ^= 1
        return "0x" + r.to_bytes(32, "big").hex() + s.to_bytes(32, "big").hex() + format(27 + parity, "02x")


if __name__ == "__main__":
    # The ethers fixture signer (0x12 * 32) and its known address.
    signer = Signer(int("12" * 32, 16))
    assert signer.address == "0x1C5A77d9FA7eF466951B2F01F724BCa3A5820b63", signer.address
    assert keccak256(b"").hex() == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"
    print("evm_sign self-test ok")
