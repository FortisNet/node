"""Offline contract for the opt-in public device and price probe."""

import io
from pathlib import Path
import struct
import sys
import unittest
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import public_price_device_probe as probe


class Reply(io.BytesIO):
    def __init__(self, status: int, body: bytes):
        super().__init__(body)
        self.status = status


class Server:
    challenge = "A" * 43
    token = "fintel_dev_" + "B" * 43

    def __init__(self, *, reject_prices: bool = False):
        self.paths = []
        self.reject_prices = reject_prices

    def open(self, request, *, timeout):
        path = urlsplit(request.full_url)
        self.paths.append(path.path)
        if path.path == "/v1/devices/challenge":
            assert request.get_method() == "POST"
            assert request.data is None
            return Reply(200, probe.pb_text(1, self.challenge))
        if path.path == "/v1/devices":
            assert request.get_method() == "POST"
            assert request.get_header("Content-type") == "application/protobuf"
            assert probe.text_field(request.data, 1) == "ios"
            attestation = next(value for number, wire, value in probe.fields(request.data)
                               if number == 3 and wire == 2)
            assert probe.text_field(attestation, 1) == "development"
            assert probe.text_field(attestation, 2) == self.challenge
            return Reply(201, probe.pb_text(1, "device-id") + probe.pb_text(2, self.token))
        if path.path == "/v1/prices":
            assert request.get_method() == "GET"
            assert request.get_header("Authorization") == "Bearer " + self.token
            assert parse_qs(path.query) == {"ids": ["bitcoin,ethereum"], "fiat": ["usd"]}
            if self.reject_prices:
                raise HTTPError(request.full_url, 401, "Unauthorized", {},
                                io.BytesIO(b'{"error":"UNAUTHORIZED"}'))
            quote = probe.pb_text(1, "bitcoin") + probe.varint(2 << 3 | 1) + struct.pack("<d", 100.0)
            entry = probe.pb_text(1, "bitcoin") + probe.pb_bytes(2, quote)
            return Reply(200, probe.pb_text(1, "usd") + probe.pb_bytes(2, entry)
                         + probe.pb_text(3, "ethereum"))
        raise AssertionError(f"unexpected route {path.path}")


class PublicPriceDeviceProbeTest(unittest.TestCase):
    def test_protobuf_enrollment_bearer_and_price_read(self):
        server = Server()
        quotes, missing, invalid = probe.run("https://example.test", ["bitcoin", "ethereum"],
                                              opener=server)
        self.assertEqual(server.paths, ["/v1/devices/challenge", "/v1/devices", "/v1/prices"])
        self.assertEqual(quotes, {"bitcoin": 100.0})
        self.assertEqual(missing, ["ethereum"])
        self.assertEqual(invalid, [])

    def test_unauthorized_price_error_omits_bearer(self):
        server = Server(reject_prices=True)
        with self.assertRaisesRegex(probe.ProbeError, "GET /v1/prices: HTTP 401 UNAUTHORIZED") as result:
            probe.run("https://example.test", ["bitcoin", "ethereum"], opener=server)
        self.assertNotIn(server.token, str(result.exception))

    def test_rejects_insecure_remote_url(self):
        with self.assertRaises(probe.ProbeError):
            probe.base_url("http://example.test")
        self.assertEqual(probe.base_url("http://127.0.0.1:8080/"), "http://127.0.0.1:8080")


if __name__ == "__main__":
    unittest.main()
