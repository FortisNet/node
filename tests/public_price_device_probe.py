#!/usr/bin/env python3
"""Enroll a development device and fetch spot prices over the public API.

Run explicitly; this creates one server-side device and makes live price reads:

  python3 tests/public_price_device_probe.py --base-url https://node.fortisnet.io
  python3 tests/public_price_device_probe.py --base-url https://node.fortisnet.io --id bitcoin

The default IDs reproduce a reported wallet request. This
uses the Flutter client's protobuf challenge, enrollment, and price formats.
Only a development-attestation server accepts this probe. The device token
stays in memory and is never printed, stored, or placed in a URL.
"""

import argparse
from http.client import HTTPSConnection
import socket
import ipaddress
import json
import math
import re
import ssl
import struct
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, HTTPSHandler, ProxyHandler, Request, build_opener

DEFAULT_IDS = (
    "eip155:42161:erc20:0x912CE59144191C1204E64559FE8253a0e49E6548",
    "eip155:1:erc20:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
    "asa:1200094857",
    "eip155:56:erc20:0x6Ec90334d89dBdc89E08A133271be3d104128Edb",
    "eip155:1:erc20:0x514910771AF9Ca656af840dff83E8264EcF986CA",
    "eip155:4663:erc20:0xd0601CE157Db5bdC3162BbaC2a2C8aF5320D9EEC",
)
MAX_REPLY = 1024 * 1024


class ProbeError(Exception):
    pass


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


class EnrollmentTunnelHandler(HTTPSHandler):
    """TLS-verified enrollment through an administrator's local SSH forward.

    Only the TCP dial changes. The original HTTPS hostname, Host and certificate
    verification remain intact; public price requests never use this handler.
    """
    def __init__(self, context, port):
        super().__init__(context=context)
        self.context, self.port = context, port

    def https_open(self, request):
        context, port = self.context, self.port
        class Connection(HTTPSConnection):
            def connect(self):
                plain = socket.create_connection(('127.0.0.1', port), timeout=self.timeout)
                try:
                    self.sock = context.wrap_socket(plain, server_hostname=self.host)
                except BaseException:
                    plain.close()
                    raise
        return self.do_open(Connection, request, context=context)


def enrollment_tunnel_opener(target, context, port):
    if urlsplit(target).scheme != 'https' or not 1 <= port <= 65535:
        raise ProbeError('enrollment tunnel requires HTTPS and a local port 1..65535')
    return build_opener(ProxyHandler({}), EnrollmentTunnelHandler(context, port), NoRedirect())


def base_url(raw: str) -> str:
    parts = urlsplit(raw)
    try:
        loopback = ipaddress.ip_address(parts.hostname or "").is_loopback
    except ValueError:
        loopback = parts.hostname == "localhost"
    if (parts.scheme != "https" and not (parts.scheme == "http" and loopback)) or \
            not parts.netloc or parts.username or parts.password or \
            parts.path not in ("", "/") or parts.query or parts.fragment:
        raise ProbeError("base URL must be HTTPS (or loopback HTTP), without credentials, path, or query")
    return raw.rstrip("/")


def varint(value: int) -> bytes:
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def pb_bytes(number: int, value: bytes) -> bytes:
    return varint(number << 3 | 2) + varint(len(value)) + value


def pb_text(number: int, value: str) -> bytes:
    return pb_bytes(number, value.encode("utf-8"))


def read_varint(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 70, 7):
        if pos >= len(data):
            raise ProbeError("truncated protobuf varint")
        byte = data[pos]
        pos += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, pos
    raise ProbeError("oversized protobuf varint")


def fields(data: bytes) -> list[tuple[int, int, bytes | int]]:
    if len(data) > MAX_REPLY:
        raise ProbeError("protobuf reply exceeds the size limit")
    result = []
    pos = 0
    while pos < len(data):
        tag, pos = read_varint(data, pos)
        number, wire = tag >> 3, tag & 7
        if number == 0:
            raise ProbeError("invalid protobuf field number")
        if wire == 0:
            value, pos = read_varint(data, pos)
        elif wire in (1, 5):
            size = 8 if wire == 1 else 4
            if size > len(data) - pos:
                raise ProbeError("truncated protobuf scalar")
            value, pos = data[pos:pos + size], pos + size
        elif wire == 2:
            size, pos = read_varint(data, pos)
            if size > len(data) - pos:
                raise ProbeError("truncated protobuf field")
            value, pos = data[pos:pos + size], pos + size
        else:
            raise ProbeError("unsupported protobuf wire type")
        result.append((number, wire, value))
    return result


def text_field(data: bytes, number: int) -> str:
    values = [value for field, wire, value in fields(data) if field == number and wire == 2]
    if len(values) != 1:
        raise ProbeError(f"protobuf field {number} is missing or repeated")
    try:
        return values[0].decode("utf-8")
    except UnicodeDecodeError as error:
        raise ProbeError("invalid protobuf text") from error


def price_result(data: bytes, requested: list[str]) -> tuple[dict[str, float], list[str], list[str]]:
    quotes = {}
    missing = []
    invalid = []
    for number, wire, value in fields(data):
        if wire != 2:
            continue
        if number == 2:
            key = text_field(value, 1)
            quote = next((item for field, kind, item in fields(value)
                          if field == 2 and kind == 2), None)
            if quote is None:
                raise ProbeError("price map entry has no quote")
            raw_price = next((item for field, kind, item in fields(quote)
                              if field == 2 and kind == 1), None)
            if raw_price is None:
                raise ProbeError("quote has no price")
            price = struct.unpack("<d", raw_price)[0]
            if not math.isfinite(price) or price <= 0:
                raise ProbeError("quote price is not positive and finite")
            quotes[key] = price
        elif number == 3:
            missing.append(value.decode("utf-8"))
        elif number == 4:
            invalid.append(value.decode("utf-8"))
    if set(quotes) | set(missing) | set(invalid) != set(requested):
        raise ProbeError("price response does not account for the requested IDs")
    return quotes, missing, invalid


def error_code(body: bytes) -> str:
    try:
        value = json.loads(body[:256])
        code = value.get("error") if isinstance(value, dict) else None
    except (ValueError, UnicodeDecodeError):
        code = None
    return code if isinstance(code, str) and re.fullmatch(r"[A-Z_]{1,40}", code) else "unknown"


def call(opener, method: str, url: str, *, body: bytes | None = None,
         token: str | None = None, timeout: float = 40) -> tuple[int, bytes]:
    headers = {"Accept": "application/protobuf"}
    if body is not None:
        headers["Content-Type"] = "application/protobuf"
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    request = Request(url, data=body, method=method, headers=headers)
    try:
        with opener.open(request, timeout=timeout) as response:
            payload = response.read(MAX_REPLY + 1)
            if len(payload) > MAX_REPLY:
                raise ProbeError("response exceeds the size limit")
            return response.status, payload
    except HTTPError as error:
        with error:
            payload = error.read(256)
        raise ProbeError(f"{method} {urlsplit(url).path}: HTTP {error.code} {error_code(payload)}") from None
    except URLError as error:
        raise ProbeError(f"{method} {urlsplit(url).path}: connection failed") from error


def enroll(opener, base: str, *, platform: str = "ios", app_version: str = "debug-probe",
           timeout: float = 15) -> str:
    status, body = call(opener, "POST", base + "/v1/devices/challenge", timeout=timeout)
    if status != 200:
        raise ProbeError(f"device challenge returned HTTP {status}")
    challenge = text_field(body, 1)
    if not re.fullmatch(r"[A-Za-z0-9_-]{43}", challenge):
        raise ProbeError("server challenge is malformed")

    attestation = pb_text(1, "development") + pb_text(2, challenge)
    enrollment = pb_text(1, platform) + pb_text(2, app_version) + pb_bytes(3, attestation)
    status, body = call(opener, "POST", base + "/v1/devices", body=enrollment, timeout=timeout)
    if status != 201:
        raise ProbeError(f"device enrollment returned HTTP {status}")
    token = text_field(body, 2)
    if not token.startswith("fintel_dev_") or len(token) > 256:
        raise ProbeError("device enrollment returned a malformed token")
    return token


def run(base: str, ids: list[str], *, platform: str = "ios", app_version: str = "debug-probe",
        opener=None, enrollment_opener=None) -> tuple[dict[str, float], list[str], list[str]]:
    if not ids or len(ids) > 100 or any(not item or len(item) > 512 for item in ids):
        raise ProbeError("provide 1–100 nonempty IDs of at most 512 characters")
    base = base_url(base)
    if opener is None:
        opener = build_opener(ProxyHandler({}), HTTPSHandler(context=ssl.create_default_context()),
                              NoRedirect())
    token = enroll(enrollment_opener or opener, base, platform=platform, app_version=app_version)

    query = urlencode({"ids": ",".join(ids), "fiat": "usd"})
    status, body = call(opener, "GET", base + "/v1/prices?" + query, token=token)
    if status != 200:
        raise ProbeError(f"price request returned HTTP {status}")
    return price_result(body, ids)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True, help="Development-attestation server URL")
    parser.add_argument("--id", action="append", dest="ids", help="Price ID (repeatable); defaults to the six wallet IDs")
    parser.add_argument('--ca-file', help='trusted CA for a private development node')
    parser.add_argument('--enrollment-tunnel-port', type=int,
                        help='enroll only through a local SSH TCP forward; prices remain public HTTPS')
    args = parser.parse_args()
    ids = args.ids or list(DEFAULT_IDS)
    try:
        context = ssl.create_default_context(cafile=args.ca_file)
        opener = build_opener(ProxyHandler({}), HTTPSHandler(context=context), NoRedirect())
        enrollment_opener = enrollment_tunnel_opener(args.base_url, context, args.enrollment_tunnel_port) if args.enrollment_tunnel_port is not None else None
        quotes, missing, invalid = run(args.base_url, ids, opener=opener, enrollment_opener=enrollment_opener)
    except ProbeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"Device enrollment succeeded; authenticated protobuf prices: {len(quotes)}/{len(ids)}")
    for item in ids:
        status = f"{quotes[item]:.12g} USD" if item in quotes else \
            "missing" if item in missing else "invalid"
        print(f"  {item}: {status}")
    return 0 if not missing and not invalid else 1


if __name__ == "__main__":
    raise SystemExit(main())
