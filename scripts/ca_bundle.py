#!/usr/bin/env python3
"""Install, rotate, inspect and remove the guest's public CA bundle.

The native TLS client trusts the PEM named by FORTIS_CA_BUNDLE_PATH. In
production that is one stable guest path on the durable guest /data:

  FORTIS_CA_BUNDLE_PATH=/data/ca-bundle.pem

  python3 scripts/ca_bundle.py check PEM [--native-check BINARY]
  python3 scripts/ca_bundle.py [BACKEND] status [--json] [--warn-days N]
  python3 scripts/ca_bundle.py [BACKEND] install PEM [--label LABEL] [--native-check BINARY] [--dry-run]
  python3 scripts/ca_bundle.py [BACKEND] activate VERSION [--dry-run]
  python3 scripts/ca_bundle.py [BACKEND] remove VERSION [--dry-run]
  python3 scripts/ca_bundle.py [BACKEND] remove --active [--dry-run]

BACKEND is where guest /data lives on the host, one of:

  --volume NAME   a named Docker volume (the default, fortis-node-data,
                  when no backend is given)
  --data-dir PATH an existing host directory, e.g. ./data for a Linux KVM
                  launcher; never created, never a symlink

`install` validates an operator-supplied local PEM on the host (certificates
only, no private key, every certificate a parseable CA certificate that is
already valid and not expired, at most 1 MiB), stores it once as an immutable
version under /data/ca-bundles/ and atomically renames a verified copy over
/data/ca-bundle.pem. Older versions are never deleted by `install`; `activate`
re-publishes one (rollback), and `remove` deletes an inactive version or
unpublishes the stable path. Nothing is downloaded and no server is started:
volume operations run scripts/ca_bundle_volume.py in the existing build image
with only the volume mounted and no network, or the same functions in process
against the --data-dir directory. A running guest keeps the bundle it loaded
at boot, so every change takes effect only after a guest restart.

Output carries fingerprints, counts and dates only, never certificate bodies
or environment values. The Docker CLI receives an allowlisted environment.

status exit codes: 0 valid, 3 soon-expiring, 4 expired, 5 missing,
6 invalid. Every command exits 1 on a refused or failed operation and
2 on usage errors.
"""

import argparse
import base64
import binascii
from dataclasses import dataclass
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import ssl
import stat
import subprocess
import sys
import tempfile

import ca_bundle_volume as volume

IMAGE = "fortis-node-build:0.12.16"
VOLUME = "fortis-node-data"
GUEST_PATH = f"/data/{volume.ACTIVE}"
GUEST_VERSIONS = f"/data/{volume.VERSIONS}"
MAX_BYTES = volume.MAX_BYTES
WARN_DAYS = 30
DOCKER_TIMEOUT_S = 180.0  # an emulated linux/amd64 helper can start slowly
NATIVE_TIMEOUT_S = 15.0
DOCKER_CLIENT_ENV = ("PATH", "HOME", "TMPDIR", "DOCKER_HOST", "DOCKER_CONTEXT",
                     "DOCKER_CONFIG", "DOCKER_CERT_PATH", "DOCKER_TLS_VERIFY")
LABEL = re.compile(r"\A[A-Za-z0-9._-]{1,40}\Z")
STATUS_EXIT = {"valid": 0, "soon_expiring": 3, "expired": 4, "missing": 5, "invalid": 6}
VOLUME_SOURCE = Path(volume.__file__).read_text(encoding="utf-8")


class BundleError(Exception):
    """A supplied or installed bundle is not an acceptable CA bundle."""


class OperationError(Exception):
    """Backend, volume or planning failure; the active bundle is unchanged unless stated."""


# --- PEM and X.509 validation (pure) -----------------------------------------

BEGIN = re.compile(rb"-----BEGIN ([^-\r\n]{1,64})-----")
END_CERTIFICATE = b"-----END CERTIFICATE-----"
PRIVATE_KEY = re.compile(rb"PRIVATE KEY")
WHITESPACE = re.compile(rb"[ \t\r\n]+")
OID_BASIC_CONSTRAINTS = bytes.fromhex("551d13")
OID_COMMON_NAME = bytes.fromhex("550403")
OID_ORGANIZATION = bytes.fromhex("55040a")


@dataclass(frozen=True)
class Certificate:
    index: int
    version: int          # 1, 2 or 3
    is_ca: bool
    not_before: dt.datetime
    not_after: dt.datetime
    name: str             # subject CN or O, printable ASCII, for messages only


@dataclass(frozen=True)
class BundleReport:
    sha256: str
    size: int
    certificates: tuple

    @property
    def earliest(self) -> Certificate:
        return min(self.certificates, key=lambda cert: (cert.not_after, cert.index))

    @property
    def latest_not_before(self) -> Certificate:
        return max(self.certificates, key=lambda cert: (cert.not_before, -cert.index))


def _tlv(data: bytes, pos: int, end: int):
    """(tag, content start, content end) of one DER element; definite lengths only."""
    if pos + 2 > end:
        raise ValueError("truncated element")
    tag = data[pos]
    if tag & 0x1F == 0x1F:
        raise ValueError("unsupported high tag number")
    length = data[pos + 1]
    pos += 2
    if length & 0x80:
        count = length & 0x7F
        if count == 0 or count > 4 or pos + count > end:
            raise ValueError("bad length")
        length = int.from_bytes(data[pos:pos + count], "big")
        pos += count
    if pos + length > end:
        raise ValueError("truncated element")
    return tag, pos, pos + length


def _children(data: bytes, start: int, end: int):
    items = []
    while start < end:
        tag, content, stop = _tlv(data, start, end)
        items.append((tag, content, stop, start))
        start = stop
    return items


def _time(tag: int, raw: bytes) -> dt.datetime:
    text = raw.decode("ascii")
    if tag == 0x17 and len(text) == 13 and text.endswith("Z") and text[:12].isdigit():
        year = int(text[:2])
        text = ("19" if year >= 50 else "20") + text
    elif not (tag == 0x18 and len(text) == 15 and text.endswith("Z") and text[:14].isdigit()):
        raise ValueError("unsupported validity time")
    return dt.datetime.strptime(text, "%Y%m%d%H%M%SZ").replace(tzinfo=dt.timezone.utc)


def _printable(raw: bytes, tag: int) -> str:
    try:
        text = raw.decode("utf-16-be") if tag == 0x1E else raw.decode("utf-8")
    except UnicodeDecodeError:
        return ""
    text = "".join(c if 0x20 <= ord(c) < 0x7F else "?" for c in text)
    return text[:64]


def _subject_name(data: bytes, start: int, end: int) -> str:
    found = {}
    for set_tag, set_start, set_end, _ in _children(data, start, end):
        if set_tag != 0x31:
            raise ValueError("bad name")
        for seq_tag, seq_start, seq_end, _ in _children(data, set_start, set_end):
            parts = _children(data, seq_start, seq_end) if seq_tag == 0x30 else []
            if len(parts) != 2 or parts[0][0] != 0x06:
                raise ValueError("bad name attribute")
            oid = data[parts[0][1]:parts[0][2]]
            if oid in (OID_COMMON_NAME, OID_ORGANIZATION) and oid not in found:
                found[oid] = _printable(data[parts[1][1]:parts[1][2]], parts[1][0])
    return found.get(OID_COMMON_NAME) or found.get(OID_ORGANIZATION) or "unnamed"


def _basic_constraints_ca(data: bytes, start: int, end: int):
    """cA flag from the v3 extensions, None when basicConstraints is absent."""
    outer = _children(data, start, end)
    if len(outer) != 1 or outer[0][0] != 0x30:
        raise ValueError("bad extensions")
    result = None
    for tag, ext_start, ext_end, _ in _children(data, outer[0][1], outer[0][2]):
        parts = _children(data, ext_start, ext_end) if tag == 0x30 else []
        if len(parts) not in (2, 3) or parts[0][0] != 0x06 or parts[-1][0] != 0x04 or \
           (len(parts) == 3 and parts[1][0] != 0x01):
            raise ValueError("bad extension")
        if data[parts[0][1]:parts[0][2]] != OID_BASIC_CONSTRAINTS:
            continue
        if result is not None:
            raise ValueError("duplicate basicConstraints")
        value = _children(data, parts[-1][1], parts[-1][2])
        if len(value) != 1 or value[0][0] != 0x30:
            raise ValueError("bad basicConstraints")
        fields = _children(data, value[0][1], value[0][2])
        result = bool(fields and fields[0][0] == 0x01 and
                      data[fields[0][1]:fields[0][2]] not in (b"", b"\x00"))
    return result


def parse_certificate(der: bytes, index: int) -> Certificate:
    """Structure of one DER X.509 certificate; ValueError when malformed."""
    tag, start, end = _tlv(der, 0, len(der))
    if tag != 0x30 or end != len(der):
        raise ValueError("not a single DER certificate")
    top = _children(der, start, end)
    if len(top) != 3 or [item[0] for item in top] != [0x30, 0x30, 0x03]:
        raise ValueError("not a certificate")
    fields = _children(der, top[0][1], top[0][2])
    position = 0
    version = 1
    if fields and fields[0][0] == 0xA0:
        inner = _children(der, fields[0][1], fields[0][2])
        if len(inner) != 1 or inner[0][0] != 0x02 or inner[0][2] - inner[0][1] != 1:
            raise ValueError("bad version")
        version = der[inner[0][1]] + 1
        if version not in (1, 2, 3):
            raise ValueError("bad version")
        position = 1
    expected = [0x02, 0x30, 0x30, 0x30, 0x30, 0x30]  # serial, sigAlg, issuer, validity, subject, spki
    core = fields[position:position + 6]
    if [item[0] for item in core] != expected:
        raise ValueError("bad tbsCertificate")
    issuer = der[core[2][3]:core[2][2]]
    subject = der[core[4][3]:core[4][2]]
    validity = _children(der, core[3][1], core[3][2])
    if len(validity) != 2:
        raise ValueError("bad validity")
    not_before = _time(validity[0][0], der[validity[0][1]:validity[0][2]])
    not_after = _time(validity[1][0], der[validity[1][1]:validity[1][2]])
    is_ca = None
    for item in fields[position + 6:]:
        if item[0] in (0x81, 0x82) and version >= 2:
            continue
        if item[0] == 0xA3 and version == 3 and is_ca is None:
            is_ca = _basic_constraints_ca(der, item[1], item[2])
            continue
        raise ValueError("unexpected tbsCertificate field")
    if version < 3:
        # mbedtls trusts locally configured v1/v2 roots without basicConstraints.
        is_ca = issuer == subject
    return Certificate(index, version, bool(is_ca), not_before, not_after,
                       _subject_name(der, core[4][1], core[4][2]))


def split_pem(data: bytes):
    """DER bodies of the CERTIFICATE blocks; text between blocks is allowed."""
    blocks = []
    position = 0
    while True:
        match = BEGIN.search(data, position)
        if not match:
            if b"-----END" in data[position:]:
                raise BundleError("an END marker has no matching BEGIN marker")
            break
        number = len(blocks) + 1
        label = match.group(1).decode("ascii", "replace")
        if label != "CERTIFICATE":
            raise BundleError(f"PEM block #{number} is '{label}'; only CERTIFICATE blocks are allowed")
        if b"-----END" in data[position:match.start()]:
            raise BundleError("an END marker has no matching BEGIN marker")
        if not data.startswith((b"\n", b"\r\n"), match.end()):
            raise BundleError(f"certificate #{number}: BEGIN marker is not followed by a line break")
        stop = data.find(END_CERTIFICATE, match.end())
        if stop < 0:
            raise BundleError(f"certificate #{number}: missing END CERTIFICATE marker")
        body = data[match.end():stop]
        if b"-----" in body or b":" in body:
            raise BundleError(f"certificate #{number}: malformed PEM body or encapsulated headers")
        try:
            der = base64.b64decode(WHITESPACE.sub(b"", body), validate=True)
        except (binascii.Error, ValueError):
            raise BundleError(f"certificate #{number}: invalid base64") from None
        if not der:
            raise BundleError(f"certificate #{number}: empty body")
        blocks.append(der)
        position = stop + len(END_CERTIFICATE)
    return blocks


def parse_bundle(data: bytes) -> BundleReport:
    """Structure of a whole bundle without time checks; BundleError when unusable."""
    if not data:
        raise BundleError("the file is empty")
    if len(data) > MAX_BYTES:
        raise BundleError(f"{len(data)} bytes exceeds the native {MAX_BYTES}-byte bundle cap")
    if b"\0" in data:
        raise BundleError("the file contains NUL bytes; supply a PEM text bundle")
    if PRIVATE_KEY.search(data):
        raise BundleError("the file contains a private key; supply public CA certificates only")
    blocks = split_pem(data)
    if not blocks:
        raise BundleError("no PEM CERTIFICATE block found")
    certificates = []
    for index, der in enumerate(blocks, 1):
        try:
            certificate = parse_certificate(der, index)
        except (ValueError, UnicodeDecodeError):
            raise BundleError(f"certificate #{index}: malformed X.509 DER") from None
        if not certificate.is_ca:
            raise BundleError(f"certificate #{index} ({certificate.name}) is not a CA certificate")
        if certificate.not_after <= certificate.not_before:
            raise BundleError(f"certificate #{index} ({certificate.name}) has an empty validity window")
        certificates.append(certificate)
    return BundleReport(hashlib.sha256(data).hexdigest(), len(data), tuple(certificates))


def check_times(report: BundleReport, now: dt.datetime) -> None:
    """Refuses expired and not-yet-valid certificates."""
    earliest = report.earliest
    if earliest.not_after <= now:
        raise BundleError(f"certificate #{earliest.index} ({earliest.name}) expired "
                          f"{iso(earliest.not_after)}")
    future = report.latest_not_before
    if future.not_before > now:
        raise BundleError(f"certificate #{future.index} ({future.name}) is not valid until "
                          f"{iso(future.not_before)}")


def openssl_ca_count(data: bytes) -> int:
    """CA certificates the host OpenSSL loads from the bundle (a second parser)."""
    with tempfile.NamedTemporaryFile(suffix=".pem") as handle:
        handle.write(data)
        handle.flush()
        try:
            context = ssl.create_default_context(cafile=handle.name)
        except (ssl.SSLError, OSError):
            raise BundleError("the host OpenSSL cannot load the bundle") from None
        return context.cert_store_stats().get("x509_ca", 0)


def validate_for_install(data: bytes, now: dt.datetime) -> BundleReport:
    report = parse_bundle(data)
    check_times(report, now)
    if openssl_ca_count(data) < 1:
        raise BundleError("the host OpenSSL loads no CA certificate from the bundle")
    return report


def classify(report, now: dt.datetime, warn_days: int):
    """Status state of an installed bundle: valid, soon_expiring, expired or invalid."""
    if report is None:
        return "missing"
    if isinstance(report, BundleError):
        return "invalid"
    if report.earliest.not_after <= now:
        return "expired"
    if report.latest_not_before.not_before > now:
        return "invalid"
    if report.earliest.not_after <= now + dt.timedelta(days=warn_days):
        return "soon_expiring"
    return "valid"


def native_check(binary: Path, path: Path) -> None:
    """Loads the bundle with the native host build's startup validation.

    The binary receives only FORTIS_CA_BUNDLE_PATH and an invalid port
    argument. main() validates the configured bundle before it parses the
    port, so it exits 2 with `Invalid port` when the bundle is accepted and
    with `Invalid CA bundle` when it is not; it can never listen.
    """
    try:
        result = subprocess.run([str(binary), "not-a-port"],
                                env={"FORTIS_CA_BUNDLE_PATH": str(path)},
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, timeout=NATIVE_TIMEOUT_S, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise OperationError(f"native check could not run ({type(error).__name__})") from None
    stderr = result.stderr.decode("utf-8", "replace")
    if result.returncode == 2 and "Invalid CA bundle" in stderr:
        raise BundleError("the native mbedtls parser rejects the bundle")
    if result.returncode != 2 or "Invalid port" not in stderr:
        raise OperationError(f"native check gave an unexpected result (exit {result.returncode})")


# --- planning (pure) ---------------------------------------------------------

def iso(moment: dt.datetime) -> str:
    return moment.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def version_name(stamp: dt.datetime, label, sha: str) -> str:
    if label is not None and not LABEL.match(label):
        raise OperationError("--label must be 1-40 characters of A-Z a-z 0-9 . _ -")
    middle = f"-{label}" if label else ""
    name = f"{stamp.astimezone(dt.timezone.utc):%Y%m%dT%H%M%SZ}{middle}-{sha[:16]}.pem"
    volume.check_name(name)
    return name


def present_versions(snapshot):
    return [entry for entry in snapshot["versions"] if entry.get("state") == "present"]


def active_sha(snapshot):
    active = snapshot["active"]
    return active.get("sha256") if active.get("state") == "present" else None


def active_version(snapshot):
    sha = active_sha(snapshot)
    if sha is None:
        return None
    matches = [entry["name"] for entry in present_versions(snapshot) if entry["sha256"] == sha]
    return matches[-1] if matches else None


def plan_install(snapshot, sha: str, label, stamp: dt.datetime):
    """What `install` will do. Content already active is a no-op; content
    already stored as a version is re-published without a second copy."""
    if snapshot["active"].get("state") not in ("present", "missing"):
        raise OperationError(f"{GUEST_PATH} is {snapshot['active'].get('state')}; not replacing it")
    if active_sha(snapshot) == sha:
        return {"action": "none", "version": active_version(snapshot),
                "reason": "this bundle is already active"}
    for entry in present_versions(snapshot):
        if entry["sha256"] == sha:
            return {"action": "publish", "version": entry["name"], "create": False,
                    "previous": active_version(snapshot), "previous_sha256": active_sha(snapshot)}
    name = version_name(stamp, label, sha)
    if any(entry["name"] == name for entry in snapshot["versions"]):
        raise OperationError(f"version {name} already exists with different content")
    return {"action": "publish", "version": name, "create": True,
            "previous": active_version(snapshot), "previous_sha256": active_sha(snapshot)}


def plan_activate(snapshot, name: str):
    volume.check_name(name)
    entry = next((item for item in snapshot["versions"] if item["name"] == name), None)
    if entry is None or entry.get("state") != "present":
        raise OperationError(f"version {name} is not on the volume")
    if snapshot["active"].get("state") not in ("present", "missing"):
        raise OperationError(f"{GUEST_PATH} is {snapshot['active'].get('state')}; not replacing it")
    if active_sha(snapshot) == entry["sha256"]:
        return {"action": "none", "version": name, "reason": "this version is already active"}
    return {"action": "publish", "version": name, "sha256": entry["sha256"],
            "previous": active_version(snapshot), "previous_sha256": active_sha(snapshot)}


def plan_remove(snapshot, name=None, active=False):
    if active:
        if snapshot["active"].get("state") == "missing":
            raise OperationError(f"{GUEST_PATH} is already absent")
        return {"action": "remove-active", "previous": active_version(snapshot),
                "previous_sha256": active_sha(snapshot)}
    volume.check_name(name)
    entry = next((item for item in snapshot["versions"] if item["name"] == name), None)
    if entry is None:
        raise OperationError(f"version {name} is not on the volume")
    if entry.get("state") == "present" and entry["sha256"] == active_sha(snapshot):
        raise OperationError(f"version {name} is the active bundle; activate another version "
                             "or use `remove --active` first")
    return {"action": "remove-version", "version": name}


# --- backends: Docker named volume or host data directory -------------------
#
# Both clients expose the same `call(operation, *args, data=None)` over the
# same ca_bundle_volume operations and replies, so planning, output and exit
# codes do not depend on where /data lives.

def docker_env(inherited) -> dict:
    """Only what the Docker CLI needs; nothing reaches the helper container."""
    return {name: inherited[name] for name in DOCKER_CLIENT_ENV if name in inherited}


def bounded(text: str, limit: int = 240) -> str:
    text = " | ".join(line.strip() for line in text.strip().splitlines() if line.strip())
    return text if len(text) <= limit else text[:limit - 3] + "..."


def checked_reply(reply: dict, operation: str) -> dict:
    if not reply.get("ok"):
        raise OperationError(f"volume refused {operation}: {reply.get('error')}: "
                             f"{reply.get('message')}")
    return reply


def command_line(backend_args, *rest) -> str:
    return shlex.join(["python3", "scripts/ca_bundle.py", *backend_args, *rest])


class VolumeClient:
    kind = "docker-volume"

    def __init__(self, volume_name: str, image: str, env: dict, timeout: float = DOCKER_TIMEOUT_S):
        self.volume = volume_name
        self.image = image
        self.env = env
        self.timeout = timeout
        self.target = volume_name
        self.backend_args = [] if volume_name == VOLUME else ["--volume", volume_name]

    def location(self) -> dict:
        return {"volume": self.volume}

    def docker(self, args, data=None):
        try:
            return subprocess.run(["docker", *args], env=self.env, input=data,
                                  stdin=None if data is not None else subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=self.timeout, check=False)
        except subprocess.TimeoutExpired:
            raise OperationError(f"docker {args[0]} exceeded {self.timeout:.0f} s; the active "
                                 "bundle is replaced only by rename, so check `status`") from None
        except OSError as error:
            raise OperationError(f"docker CLI unavailable ({type(error).__name__})") from None

    def preflight(self) -> None:
        result = self.docker(["version", "--format", "{{.Server.Version}}"])
        if result.returncode != 0:
            raise OperationError("Docker daemon unavailable: "
                                 + bounded(result.stderr.decode("utf-8", "replace")))
        if self.docker(["image", "inspect", self.image]).returncode != 0:
            raise OperationError(f"image {self.image} is missing; run ./scripts/build-image.sh")
        # `docker run -v` would silently create a missing volume.
        if self.docker(["volume", "inspect", self.volume]).returncode != 0:
            raise OperationError(f"named volume {self.volume} does not exist; refusing to create it")

    def call(self, *args, data=None) -> dict:
        command = ["run", "--rm", "--network", "none", "--platform", "linux/amd64",
                   "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
                   "-v", f"{self.volume}:/data"]
        if data is not None:
            command.append("-i")
        command += [self.image, "python3", "-I", "-c", VOLUME_SOURCE, *args]
        result = self.docker(command, data)
        lines = result.stdout.decode("utf-8", "replace").strip().splitlines()
        try:
            reply = json.loads(lines[-1]) if lines else None
        except json.JSONDecodeError:
            reply = None
        if not isinstance(reply, dict):
            raise OperationError(f"volume helper failed (exit {result.returncode}): "
                                 + bounded(result.stderr.decode("utf-8", "replace")))
        return checked_reply(reply, args[0])


def _identity(info) -> tuple:
    return (info.st_dev, info.st_ino)


def resolve_data_dir(raw: Path, writable: bool) -> Path:
    """The absolute data directory after refusing anything but an existing,
    accessible real directory. Nothing is created; `raw` itself may not be a
    symlink (parents may be, e.g. /tmp on macOS)."""
    try:
        info = os.lstat(raw)  # pathlib drops a trailing slash, so a symlink is not followed
    except FileNotFoundError:
        raise OperationError(f"data directory {raw} does not exist; create it first "
                             "(refusing to create it)") from None
    except OSError as error:
        raise OperationError(f"cannot inspect data directory {raw} ({type(error).__name__})") from None
    if stat.S_ISLNK(info.st_mode):
        raise OperationError(f"data directory {raw} is a symlink; pass the real directory")
    if not stat.S_ISDIR(info.st_mode):
        raise OperationError(f"data directory {raw} is not a directory")
    try:
        resolved = raw.resolve(strict=True)
        after = os.lstat(resolved)
    except OSError as error:
        raise OperationError(f"cannot resolve data directory {raw} ({type(error).__name__})") from None
    if not stat.S_ISDIR(after.st_mode) or _identity(after) != _identity(info):
        raise OperationError(f"data directory {raw} changed while it was resolved")
    needed = os.R_OK | os.X_OK | (os.W_OK if writable else 0)
    if not os.access(resolved, needed):
        access = "read, write and search" if writable else "read and search"
        raise OperationError(f"data directory {resolved} lacks {access} access for this user")
    return resolved


class DirectoryClient:
    """ca_bundle_volume operations in process on a host directory mounted as
    guest /data (e.g. a launcher's ./data)."""

    kind = "data-dir"

    def __init__(self, raw: Path, writable: bool):
        self.root = resolve_data_dir(raw, writable)
        self.identity = _identity(os.lstat(self.root))
        self.target = str(self.root)
        self.backend_args = ["--data-dir", str(self.root)]

    def location(self) -> dict:
        return {"data_dir": str(self.root)}

    def call(self, *args, data=None) -> dict:
        try:
            info = os.lstat(self.root)
        except OSError:
            info = None
        if info is None or not stat.S_ISDIR(info.st_mode) or _identity(info) != self.identity:
            raise OperationError(f"data directory {self.root} changed after it was validated")
        reply = volume.execute(list(args), io.BytesIO(data if data is not None else b""),
                               str(self.root))
        return checked_reply(reply, args[0])


# --- inspection --------------------------------------------------------------

def inspect_entry(entry):
    """(BundleReport | BundleError | None, raw bytes) for one snapshot entry."""
    state = entry.get("state")
    if state == "missing":
        return None
    if state != "present":
        return BundleError(f"{state.replace('_', ' ')}")
    data = base64.b64decode(entry["content"])
    if hashlib.sha256(data).hexdigest() != entry["sha256"]:
        return BundleError("helper digest mismatch")
    try:
        return parse_bundle(data)
    except BundleError as error:
        return error


def describe_report(report, now, warn_days):
    state = classify(report, now, warn_days)
    item = {"state": state}
    if isinstance(report, BundleReport):
        earliest = report.earliest
        item.update({"sha256": report.sha256, "size": report.size,
                     "certificates": len(report.certificates),
                     "earliest_expiry": iso(earliest.not_after),
                     "earliest_expiry_certificate": f"#{earliest.index} ({earliest.name})",
                     "days_left": (earliest.not_after - now).days})
    elif isinstance(report, BundleError):
        item["reason"] = str(report)
    return item


def status_document(snapshot, now, warn_days, client):
    active = describe_report(inspect_entry(snapshot["active"]), now, warn_days)
    active["version"] = active_version(snapshot)
    versions = []
    for entry in snapshot["versions"]:
        item = describe_report(inspect_entry(entry), now, warn_days)
        item["name"] = entry["name"]
        item["active"] = entry.get("sha256") is not None and entry.get("sha256") == active_sha(snapshot)
        versions.append(item)
    return {"backend": client.kind, **client.location(), "guest_path": GUEST_PATH,
            "checked_at": iso(now), "warn_days": warn_days, "state": active["state"], "active": active,
            "versions": versions, "leftover_temps": snapshot["temps"],
            "other_entries": snapshot["other_entries"], "truncated": snapshot["truncated"]}


def print_report(report: BundleReport, now: dt.datetime) -> None:
    earliest = report.earliest
    print(f"  sha256:           {report.sha256}")
    print(f"  size:             {report.size} bytes")
    print(f"  certificates:     {len(report.certificates)} CA")
    print(f"  earliest expiry:  {iso(earliest.not_after)} "
          f"({(earliest.not_after - now).days} days, certificate #{earliest.index} {earliest.name})")


def print_status(document) -> None:
    active = document["active"]
    print(f"CA bundle status: {document['state']}")
    if "data_dir" in document:
        print(f"  data dir:         {document['data_dir']}")
    else:
        print(f"  volume:           {document['volume']}")
    print(f"  guest path:       {document['guest_path']}")
    print(f"  checked at:       {document['checked_at']} (soon-expiring window "
          f"{document['warn_days']} days)")
    if active["state"] != "missing":
        print(f"  active version:   {active.get('version') or 'not a stored version'}")
    if "sha256" in active:
        print(f"  sha256:           {active['sha256']}")
        print(f"  certificates:     {active['certificates']} CA, {active['size']} bytes")
        print(f"  earliest expiry:  {active['earliest_expiry']} ({active['days_left']} days, "
              f"certificate {active['earliest_expiry_certificate']})")
    if "reason" in active:
        print(f"  problem:          {active['reason']}")
    if document["state"] == "missing":
        print("  A guest started with FORTIS_CA_BUNDLE_PATH set to this path exits at startup "
              "(Invalid CA bundle configured by FORTIS_CA_BUNDLE_PATH).")
    print(f"versions in {GUEST_VERSIONS}: {len(document['versions'])}"
          + (" (listing truncated)" if document["truncated"] else ""))
    for item in document["versions"]:
        marker = "*" if item["active"] else " "
        detail = (f"expires {item['earliest_expiry']} ({item['days_left']} days)"
                  if "earliest_expiry" in item else item.get("reason", ""))
        print(f"  {marker} {item['name']}  {item['state']}  {detail}")
    if document["leftover_temps"]:
        print(f"leftover temporary files from interrupted runs: {len(document['leftover_temps'])} "
              "(ignored; never published)")
    if document["other_entries"]:
        print(f"unrecognized entries in {GUEST_VERSIONS}: {document['other_entries']} (left alone)")


# --- commands ----------------------------------------------------------------

def read_local(path: Path) -> bytes:
    try:
        if not path.is_file():
            raise OperationError(f"{path} is not a regular file")
        if path.stat().st_size > MAX_BYTES:
            raise BundleError(f"{path.stat().st_size} bytes exceeds the native {MAX_BYTES}-byte bundle cap")
        return path.read_bytes()
    except OSError as error:
        raise OperationError(f"cannot read {path} ({type(error).__name__})") from None


def validated_local(path: Path, native, now) -> BundleReport:
    data = read_local(path)
    report = validate_for_install(data, now)
    if native is not None:
        native_check(native, path.resolve())
    return report


def command_check(args, now) -> int:
    report = validated_local(args.pem, args.native_check, now)
    print("CA bundle accepted for install"
          + (" (host OpenSSL and native mbedtls checks passed)" if args.native_check
             else " (host OpenSSL check passed; native check not requested)"))
    print_report(report, now)
    return 0


def backend_args(args) -> list:
    """Backend flags as given, for the commands printed in hints."""
    if args.data_dir is not None:
        return ["--data-dir", str(args.data_dir)]
    return ["--volume", args.volume] if args.volume not in (None, VOLUME) else []


def client_for(args, writable: bool):
    """The selected backend after its preflight; `writable` is False for
    status and --dry-run, which only read."""
    if args.data_dir is not None:
        return DirectoryClient(args.data_dir, writable)
    client = VolumeClient(args.volume or VOLUME, args.image or IMAGE, docker_env(os.environ))
    client.preflight()
    return client


def print_plan(plan) -> None:
    for key in ("action", "version", "create", "previous", "previous_sha256", "reason"):
        if key in plan:
            print(f"  {key}: {plan[key]}")


def print_restart() -> None:
    print(f"Restart the guest to load it; a running guest keeps the bundle it loaded at boot. "
          f"Launch with FORTIS_CA_BUNDLE_PATH={GUEST_PATH}.")


def command_install(args, now) -> int:
    report = validated_local(args.pem, args.native_check, now)
    data = args.pem.read_bytes()
    if hashlib.sha256(data).hexdigest() != report.sha256:
        raise OperationError(f"{args.pem} changed while it was validated")
    print("CA bundle validated" + (" (host OpenSSL and native mbedtls)" if args.native_check
                                   else " (host OpenSSL; native check not requested)"))
    print_report(report, now)
    client = client_for(args, writable=not args.dry_run)
    plan = plan_install(client.call("snapshot"), report.sha256, args.label, now)
    print(f"plan for {client.target}:")
    print_plan(plan)
    if plan["action"] == "none" or args.dry_run:
        print("dry run: nothing changed" if args.dry_run and plan["action"] != "none"
              else "nothing to do")
        return 0
    if plan["create"]:
        result = client.call("install", plan["version"], report.sha256, data=data)
    else:
        result = client.call("activate", plan["version"], report.sha256)
    print(f"installed {GUEST_VERSIONS}/{plan['version']} and published {GUEST_PATH}"
          + ("" if result.get("created", plan["create"]) else " (reused the stored version)"))
    if plan.get("previous") or plan.get("previous_sha256"):
        print(f"previous active bundle kept as {plan.get('previous') or 'an unstored file'}; "
              f"roll back with: {command_line(client.backend_args, 'activate', plan.get('previous') or '<version>')}")
    print_restart()
    return 0


def checked_version(snapshot, name, now) -> dict:
    plan = plan_activate(snapshot, name)
    if plan["action"] == "none":
        return plan
    entry = next(item for item in snapshot["versions"] if item["name"] == name)
    report = inspect_entry(entry)
    if isinstance(report, BundleError):
        raise OperationError(f"version {name} is not a valid bundle: {report}")
    try:
        check_times(report, now)
    except BundleError as error:
        raise OperationError(f"version {name} cannot be activated: {error}") from None
    return plan


def command_activate(args, now) -> int:
    client = client_for(args, writable=not args.dry_run)
    plan = checked_version(client.call("snapshot"), args.version, now)
    print(f"plan for {client.target}:")
    print_plan(plan)
    if plan["action"] == "none" or args.dry_run:
        print("dry run: nothing changed" if args.dry_run and plan["action"] != "none"
              else "nothing to do")
        return 0
    client.call("activate", plan["version"], plan["sha256"])
    print(f"published {GUEST_VERSIONS}/{plan['version']} at {GUEST_PATH}")
    print_restart()
    return 0


def command_remove(args, now) -> int:
    if bool(args.version) == bool(args.active):
        raise OperationError("give exactly one of VERSION or --active")
    client = client_for(args, writable=not args.dry_run)
    plan = plan_remove(client.call("snapshot"), args.version, args.active)
    print(f"plan for {client.target}:")
    print_plan(plan)
    if args.dry_run:
        print("dry run: nothing changed")
        return 0
    if args.active:
        client.call("remove-active")
        print(f"removed {GUEST_PATH}; stored versions were kept. A guest started with "
              "FORTIS_CA_BUNDLE_PATH set to it now exits at startup until a version is activated.")
    else:
        client.call("remove-version", args.version)
        print(f"removed {GUEST_VERSIONS}/{args.version}")
    return 0


def command_status(args, now) -> int:
    client = client_for(args, writable=False)
    document = status_document(client.call("snapshot"), now, args.warn_days, client)
    if args.json:
        print(json.dumps(document, indent=2, sort_keys=True))
    else:
        print_status(document)
    return STATUS_EXIT[document["state"]]


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    backend = top.add_mutually_exclusive_group()
    backend.add_argument("--volume", help=f"named Docker volume (default {VOLUME})")
    backend.add_argument("--data-dir", type=Path, metavar="PATH",
                         help="existing host directory mounted as guest /data (Linux KVM: ./data)")
    top.add_argument("--image", help=f"Docker helper image (default {IMAGE})")
    commands = top.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="validate a local PEM only (no backend)")
    check.add_argument("pem", type=Path)
    install = commands.add_parser("install", help="validate, store and publish a local PEM")
    install.add_argument("pem", type=Path)
    install.add_argument("--label", help="version label, e.g. the upstream bundle release")
    for sub in (check, install):
        sub.add_argument("--native-check", type=Path, metavar="BINARY",
                         help="also load the bundle with a host build/fortis_node")
    activate = commands.add_parser("activate", help="publish a stored version (rollback)")
    activate.add_argument("version")
    remove = commands.add_parser("remove", help="delete an inactive version or unpublish")
    remove.add_argument("version", nargs="?")
    remove.add_argument("--active", action="store_true", help=f"remove {GUEST_PATH}")
    for sub in (install, activate, remove):
        sub.add_argument("--dry-run", action="store_true", help="print the plan only")
    status = commands.add_parser("status", help="report the active bundle and versions")
    status.add_argument("--json", action="store_true")
    status.add_argument("--warn-days", type=int, default=WARN_DAYS,
                        help=f"soon-expiring window in days (default {WARN_DAYS})")
    return top


def main(argv=None) -> int:
    top = parser()
    args = top.parse_args(argv)
    if args.data_dir is not None and args.image is not None:
        top.error("--image applies to the Docker volume backend only, not --data-dir")
    if getattr(args, "warn_days", 0) < 0:
        print("error: --warn-days must not be negative", file=sys.stderr)
        return 2
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    handlers = {"check": command_check, "install": command_install, "activate": command_activate,
                "remove": command_remove, "status": command_status}
    try:
        return handlers[args.command](args, now)
    except BundleError as error:
        print(f"error: CA bundle rejected: {error}", file=sys.stderr)
        print("nothing was written to the volume", file=sys.stderr)
    except (OperationError, volume.VolumeError) as error:
        print(f"error: {error}", file=sys.stderr)
        if args.command in ("install", "activate", "remove"):
            print("the active bundle changes only by a final rename or unlink; run "
                  f"`{command_line(backend_args(args), 'status')}` to confirm it", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
