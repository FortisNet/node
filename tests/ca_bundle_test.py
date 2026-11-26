#!/usr/bin/env python3
"""Offline checks for scripts/ca_bundle.py and scripts/ca_bundle_volume.py.

Covers PEM/X.509 validation, status classification, operation planning, the
volume-side file operations against a temporary directory (including copy and
rename failures), output hygiene, and agreement with the native host build's
startup CA validation, and the whole CLI lifecycle on the --data-dir backend
(the Linux KVM launcher's ./data) in a temporary directory. No Docker and no
network; the Docker volume backend is not exercised here.

  python3 tests/ca_bundle_test.py build/fortis_node
"""

import base64
import contextlib
import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import ca_bundle as tool  # noqa: E402
import ca_bundle_volume as volume  # noqa: E402

UTC = dt.timezone.utc
NOW = dt.datetime.now(UTC).replace(microsecond=0)
SECRET = "fortis-ca-bundle-test-secret-value-0123456789"


def openssl(*args):
    subprocess.run(["openssl", *args], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_ca(root: Path, name: str, days: int, ca: bool = True) -> Path:
    path = root / f"{name}.pem"
    constraint = "critical,CA:TRUE" if ca else "critical,CA:FALSE"
    openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
            "-nodes", "-sha256", "-days", str(days), "-subj", f"/CN={name}",
            "-addext", f"basicConstraints={constraint}",
            "-keyout", str(root / f"{name}.key"), "-out", str(path))
    return path


def rejects(data: bytes, fragment: str, now=None):
    if now is None:
        now = NOW
    try:
        tool.check_times(tool.parse_bundle(data), now)
    except tool.BundleError as error:
        message = str(error)
        assert fragment in message, (fragment, message)
        assert not re.search(r"[A-Za-z0-9+/]{40,}", message), message  # never a PEM body
        return message
    raise AssertionError(f"accepted a bundle that should fail with {fragment!r}")


def test_validation(fixtures: Path):
    global NOW
    long_ca = make_ca(fixtures, "Fortis long CA", 400).read_bytes()
    short_ca = make_ca(fixtures, "Fortis short CA", 2).read_bytes()
    leaf = make_ca(fixtures, "Fortis leaf", 400, ca=False).read_bytes()
    key = (fixtures / "Fortis long CA.key").read_bytes()
    # OpenSSL sets notBefore when it creates each certificate. Taking the
    # validation clock at module import can put it one second before notBefore.
    NOW = dt.datetime.now(UTC).replace(microsecond=0)

    commented = b"# Issuer: CN=Fortis long CA\n# comment line\n" + long_ca + b"\n## next\n" + short_ca
    report = tool.validate_for_install(commented, NOW)
    assert len(report.certificates) == 2 and all(cert.is_ca for cert in report.certificates)
    assert report.sha256 == hashlib.sha256(commented).hexdigest() and report.size == len(commented)
    assert report.earliest.name == "Fortis short CA" and report.earliest.index == 2
    assert report.certificates[0].version == 3
    assert tool.validate_for_install(long_ca.replace(b"\n", b"\r\n"), NOW).certificates

    rejects(b"", "empty")
    rejects(b"# only a comment\n", "no PEM CERTIFICATE block")
    rejects(long_ca + key, "private key")
    rejects(b"-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n",
            "private key")
    rejects(b"-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n", "only CERTIFICATE")
    rejects(long_ca.replace(b"CERTIFICATE", b"TRUSTED CERTIFICATE"), "only CERTIFICATE")
    rejects(long_ca + b"-----END CERTIFICATE-----\n", "no matching BEGIN")
    rejects(long_ca.replace(b"-----END CERTIFICATE-----", b""), "missing END")
    body = long_ca.split(b"\n")
    rejects(b"\n".join(body[:1] + [b"Proc-Type: 4,ENCRYPTED"] + body[1:]), "encapsulated headers")
    rejects(long_ca.replace(body[1], body[1][:10] + b"*" + body[1][11:]), "invalid base64")
    der = base64.b64decode(b"".join(body[1:-2]))
    truncated = base64.encodebytes(der[:-20])
    rejects(b"-----BEGIN CERTIFICATE-----\n" + truncated + b"-----END CERTIFICATE-----\n",
            "malformed X.509")
    trailing = base64.encodebytes(der + b"\x00\x00")
    rejects(b"-----BEGIN CERTIFICATE-----\n" + trailing + b"-----END CERTIFICATE-----\n",
            "malformed X.509")
    rejects(der, "NUL bytes")
    rejects(long_ca + b"\0", "NUL bytes")
    rejects(long_ca + b"#" * tool.MAX_BYTES, "bundle cap")
    rejects(long_ca + leaf, "certificate #2 (Fortis leaf) is not a CA")
    rejects(long_ca + short_ca, "certificate #2 (Fortis short CA) expired",
            now=NOW + dt.timedelta(days=10))
    rejects(long_ca, "is not valid until", now=NOW - dt.timedelta(days=2))

    assert tool.classify(None, NOW, 30) == "missing"
    assert tool.classify(tool.BundleError("x"), NOW, 30) == "invalid"
    assert tool.classify(tool.parse_bundle(long_ca), NOW, 30) == "valid"
    assert tool.classify(tool.parse_bundle(long_ca + short_ca), NOW, 30) == "soon_expiring"
    assert tool.classify(tool.parse_bundle(long_ca), NOW, 401) == "soon_expiring"
    assert tool.classify(tool.parse_bundle(short_ca), NOW + dt.timedelta(days=3), 30) == "expired"
    assert tool.classify(tool.parse_bundle(long_ca), NOW - dt.timedelta(days=2), 30) == "invalid"
    return long_ca, short_ca


def snapshot(active=None, versions=()):
    def entry(data):
        return {"state": "present", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "content": base64.b64encode(data).decode()}
    return {"active": entry(active) if active is not None else {"state": "missing"},
            "versions": [{"name": name, **entry(data)} for name, data in versions],
            "temps": [], "other_entries": 0, "truncated": False}


def expect_operation_error(function, *args, fragment="", **kwargs):
    try:
        function(*args, **kwargs)
    except (tool.OperationError, volume.VolumeError) as error:
        assert fragment in str(error), (fragment, str(error))
        return
    raise AssertionError(f"{function.__name__} did not refuse")


def test_planning(long_ca: bytes, short_ca: bytes):
    sha_long = hashlib.sha256(long_ca).hexdigest()
    sha_short = hashlib.sha256(short_ca).hexdigest()
    stamp = dt.datetime(2026, 10, 22, 19, 30, 5, tzinfo=UTC)
    name_long = tool.version_name(stamp, "certifi-2026.05.20", sha_long)
    assert name_long == f"20261022T193005Z-certifi-2026.05.20-{sha_long[:16]}.pem"
    assert tool.version_name(stamp, None, sha_long) == f"20261022T193005Z-{sha_long[:16]}.pem"
    for label in ("", "../x", "a/b", "x" * 41, "white space"):
        expect_operation_error(tool.version_name, stamp, label, sha_long, fragment="--label")
    for name in ("../ca.pem", "ca-bundle.pem", f"20261022T193005Z-{sha_long[:16]}.pem/..",
                 f"x20261022T193005Z-{sha_long[:16]}.pem", f".incoming-{sha_long[:16]}"):
        expect_operation_error(volume.check_name, name, fragment="version name")

    empty = snapshot()
    plan = tool.plan_install(empty, sha_long, None, stamp)
    assert plan["action"] == "publish" and plan["create"] and plan["previous_sha256"] is None
    first = snapshot(long_ca, [(name_long, long_ca)])
    assert tool.plan_install(first, sha_long, None, stamp)["action"] == "none"
    plan = tool.plan_install(first, sha_short, "next", stamp)
    assert plan["create"] and plan["previous"] == name_long and plan["previous_sha256"] == sha_long
    name_short = plan["version"]
    second = snapshot(short_ca, [(name_long, long_ca), (name_short, short_ca)])
    plan = tool.plan_install(second, sha_long, None, stamp)
    assert plan == {"action": "publish", "version": name_long, "create": False,
                    "previous": name_short, "previous_sha256": sha_short}
    not_regular = dict(second, active={"state": "not_regular"})
    expect_operation_error(tool.plan_install, not_regular, sha_long, None, stamp, fragment="not_regular")

    assert tool.plan_activate(second, name_long)["sha256"] == sha_long
    assert tool.plan_activate(second, name_short)["action"] == "none"
    expect_operation_error(tool.plan_activate, second, f"20200101T000000Z-{sha_long[:16]}.pem",
                           fragment="not on the volume")
    expect_operation_error(tool.plan_remove, second, name_short, fragment="is the active bundle")
    assert tool.plan_remove(second, name_long) == {"action": "remove-version", "version": name_long}
    assert tool.plan_remove(second, active=True)["previous"] == name_short
    expect_operation_error(tool.plan_remove, empty, active=True, fragment="already absent")

    vol = tool.VolumeClient("vol", tool.IMAGE, {})
    document = tool.status_document(second, NOW, 30, vol)
    assert document["backend"] == "docker-volume" and document["volume"] == "vol"
    assert document["state"] == "soon_expiring" and document["active"]["version"] == name_short
    assert [item["active"] for item in document["versions"]] == [False, True]
    assert tool.status_document(empty, NOW, 30, vol)["state"] == "missing"
    broken = dict(second, active={"state": "present", "size": 3, "sha256": hashlib.sha256(b"abc").hexdigest(),
                                  "content": base64.b64encode(b"abc").decode()})
    assert tool.status_document(broken, NOW, 30, vol)["state"] == "invalid"
    text = json.dumps(document)
    assert "content" not in text and base64.b64encode(long_ca[30:90]).decode()[:20] not in text
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        tool.print_status(document)
    assert "BEGIN" not in output.getvalue() and name_short in output.getvalue()

    env = tool.docker_env({"PATH": "/bin", "HOME": "/h", "DOCKER_HOST": "unix:///x",
                           "COINGECKO_API_KEY": SECRET, "METRICS_TOKEN": SECRET,
                           "ATTESTATION_MODE": "development", "FORTIS_GUEST_ATTESTATION_MODE": "x"})
    assert env == {"PATH": "/bin", "HOME": "/h", "DOCKER_HOST": "unix:///x"}
    return sha_long, sha_short, name_long, name_short


def tree(root: Path):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*")) if path.is_file()}


def test_volume_operations(long_ca, short_ca, sha_long, sha_short, name_long, name_short):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "fortis.kv").write_bytes(b"unrelated kv bytes")
        (root / "ca.pem").write_bytes(b"an unrelated probe file")
        (root / "notes").mkdir()
        (root / "notes/keep.txt").write_bytes(b"keep me")
        unrelated = tree(root)
        active = root / volume.ACTIVE

        def leftovers():
            names = [p.name for p in root.iterdir()] + \
                    [p.name for p in (root / volume.VERSIONS).iterdir()]
            return [n for n in names if n.startswith((volume.ACTIVE_TEMP_PREFIX, volume.VERSION_TEMP_PREFIX))]

        assert volume.snapshot(str(root))["active"] == {"state": "missing"}
        result = volume.install(str(root), name_long, sha_long, long_ca)
        assert result == {"version": name_long, "created": True, "previous_sha256": None}
        assert active.read_bytes() == long_ca and (active.stat().st_mode & 0o777) == 0o444
        stored = root / volume.VERSIONS / name_long
        assert (stored.stat().st_mode & 0o777) == 0o444

        result = volume.install(str(root), name_short, sha_short, short_ca)
        assert result["previous_sha256"] == sha_long and active.read_bytes() == short_ca
        assert stored.read_bytes() == long_ca  # install never deletes an old version

        # Re-installing identical content under the same name reuses it.
        assert volume.install(str(root), name_short, sha_short, short_ca)["created"] is False
        expect_operation_error(volume.install, str(root), name_long, sha_short, short_ca,
                               fragment="different content")
        expect_operation_error(volume.install, str(root), name_long, sha_long, short_ca,
                               fragment="do not match")

        # A failed copy or rename keeps the previous active bundle.
        third = f"20261022T200000Z-{sha_long[:16]}.pem"
        real_read = volume.read_regular
        with mock.patch.object(volume, "read_regular",
                               side_effect=lambda p: b"corrupt" if ".incoming-" in p else real_read(p)):
            expect_operation_error(volume.install, str(root), third, sha_long, long_ca,
                                   fragment="did not match")
        assert not (root / volume.VERSIONS / third).exists()
        with mock.patch.object(volume.os, "replace", side_effect=OSError(28, "No space left")):
            try:
                volume.activate(str(root), name_long, sha_long)
            except OSError:
                pass
            else:
                raise AssertionError("rename failure was not raised")
        assert active.read_bytes() == short_ca and not leftovers()
        with mock.patch.object(volume, "read_regular",
                               side_effect=lambda p: b"short" if ".tmp-" in p else real_read(p)):
            expect_operation_error(volume.activate, str(root), name_long, sha_long, fragment="did not match")
        assert active.read_bytes() == short_ca and not leftovers()

        # Rollback, refusal to delete the active version, removal, unpublish.
        assert volume.activate(str(root), name_long, sha_long)["previous_sha256"] == sha_short
        assert active.read_bytes() == long_ca
        expect_operation_error(volume.remove_version, str(root), name_long, fragment="active bundle")
        assert volume.remove_version(str(root), name_short)["removed"]
        expect_operation_error(volume.activate, str(root), name_short, sha_short, fragment="does not exist")
        assert volume.remove_active(str(root))["previous_sha256"] == sha_long
        assert not active.exists() and stored.read_bytes() == long_ca
        expect_operation_error(volume.remove_active, str(root), fragment="does not exist")

        # Non-regular stable paths are refused, not replaced.
        active.mkdir()
        expect_operation_error(volume.activate, str(root), name_long, sha_long, fragment="not a regular")
        active.rmdir()
        active.symlink_to(root / "ca.pem")
        expect_operation_error(volume.activate, str(root), name_long, sha_long, fragment="not a regular")
        active.unlink()

        # A held lock refuses a concurrent mutation.
        with volume.Lock(str(root)):
            expect_operation_error(volume.activate, str(root), name_long, sha_long, fragment="lock")

        # The CLI-facing dispatcher reports JSON-able results and reads stdin.
        reply = volume.run(["install", name_short, sha_short], io.BytesIO(short_ca), root=str(root))
        assert reply["created"] and active.read_bytes() == short_ca
        expect_operation_error(volume.run, ["install", "../x.pem", sha_short], io.BytesIO(b""),
                               root=str(root), fragment="version name")
        snap = volume.snapshot(str(root))
        assert [entry["name"] for entry in snap["versions"]] == [name_long, name_short]
        assert snap["temps"] == [] and snap["other_entries"] == 0
        assert not leftovers()
        after = tree(root)
        assert {name: after[name] for name in unrelated} == unrelated, "unrelated files changed"


def test_native_agreement(binary: Path, fixtures: Path):
    good = fixtures / "Fortis long CA.pem"
    commented = fixtures / "commented.pem"
    commented.write_bytes(b"# a comment\n" + good.read_bytes() + b"\n# trailer\n")
    malformed = fixtures / "malformed.pem"
    malformed.write_bytes(b"-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n")
    tool.native_check(binary, good)
    tool.native_check(binary, commented)
    for path in (malformed, fixtures / "missing.pem"):
        try:
            tool.native_check(binary, path)
        except tool.BundleError as error:
            assert "native mbedtls parser rejects" in str(error)
        else:
            raise AssertionError(f"native startup accepted {path.name}")


def test_cli(fixtures: Path, binary: Path):
    env = {"PATH": os.environ.get("PATH", ""), "COINGECKO_API_KEY": SECRET, "METRICS_TOKEN": SECRET}
    script = str(ROOT / "scripts/ca_bundle.py")
    good = fixtures / "Fortis long CA.pem"
    result = subprocess.run([sys.executable, script, "check", str(good), "--native-check", str(binary)],
                            env=env, capture_output=True, text=True, check=False)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "native mbedtls checks passed" in output
    assert hashlib.sha256(good.read_bytes()).hexdigest() in output
    bad = fixtures / "with-key.pem"
    bad.write_bytes(good.read_bytes() + (fixtures / "Fortis long CA.key").read_bytes())
    result = subprocess.run([sys.executable, script, "check", str(bad)],
                            env=env, capture_output=True, text=True, check=False)
    output += result.stdout + result.stderr
    assert result.returncode == 1 and "private key" in result.stderr
    assert SECRET not in output and "BEGIN" not in output
    body = good.read_text().splitlines()[1]
    assert body not in output


def run_cli(*args, expect, env=None):
    env = env or {"PATH": os.environ.get("PATH", ""), "COINGECKO_API_KEY": SECRET,
                  "METRICS_TOKEN": SECRET}
    result = subprocess.run([sys.executable, str(ROOT / "scripts/ca_bundle.py"), *map(str, args)],
                            env=env, capture_output=True, text=True, timeout=60, check=False)
    output = result.stdout + result.stderr
    assert result.returncode == expect, f"{args}: exit {result.returncode}, expected {expect}\n{output}"
    assert SECRET not in output and "BEGIN CERTIFICATE" not in output, output
    return output


def data_dir_status(data: Path):
    result = subprocess.run([sys.executable, str(ROOT / "scripts/ca_bundle.py"), "--data-dir", str(data),
                             "status", "--json"], capture_output=True, text=True, timeout=60, check=False)
    document = json.loads(result.stdout)
    assert result.returncode == tool.STATUS_EXIT[document["state"]], result
    return document


def test_data_dir_backend(fixtures: Path):
    """The Linux KVM ./data lifecycle through the CLI, on a temporary directory."""
    first = fixtures / "Fortis long CA.pem"
    second = fixtures / "Fortis short CA.pem"  # inside the 30-day window
    sha_one = hashlib.sha256(first.read_bytes()).hexdigest()
    sha_two = hashlib.sha256(second.read_bytes()).hexdigest()
    with_key = fixtures / "data-dir-with-key.pem"
    with_key.write_bytes(first.read_bytes() + (fixtures / "Fortis long CA.key").read_bytes())
    malformed = fixtures / "data-dir-malformed.pem"
    malformed.write_bytes(b"-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n")

    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        data = base / "data"

        # The backend must already exist as a real, accessible directory.
        output = run_cli("--data-dir", data, "status", expect=1)
        assert "does not exist" in output and "refusing to create" in output
        output = run_cli("--data-dir", data, "install", first, "--dry-run", expect=1)
        assert "does not exist" in output and not data.exists(), "a missing data dir was created"
        data.write_bytes(b"not a directory")
        assert "is not a directory" in run_cli("--data-dir", data, "status", expect=1)
        data.unlink()
        data.mkdir()
        link = base / "data-link"
        link.symlink_to(data, target_is_directory=True)
        for spelled in (link, f"{link}/"):
            output = run_cli("--data-dir", spelled, "install", first, expect=1)
            assert "is a symlink" in output, output
        assert not any(data.iterdir()), "a symlinked data dir was written"

        # Backend selection and check stay local-only.
        assert "not allowed with argument" in run_cli("--volume", "v", "--data-dir", data, "status",
                                                      expect=2)
        assert "--image applies" in run_cli("--data-dir", data, "--image", "x", "status", expect=2)
        assert "accepted for install" in run_cli("--data-dir", base / "absent", "check", first, expect=0)

        (data / "fortis.kv.lock").write_bytes(b"")
        (data / "fortis.kv").write_bytes(b"unrelated kv bytes")
        (data / "ca.pem").write_bytes(b"an unrelated probe file")
        (data / "notes").mkdir()
        (data / "notes/keep.txt").write_bytes(b"keep me")
        unrelated = tree(data)

        output = run_cli("--data-dir", data, "status", expect=5)
        assert "CA bundle status: missing" in output and f"data dir:         {data.resolve()}" in output

        before = tree(data)
        output = run_cli("--data-dir", data, "install", first, "--label", "first", "--dry-run", expect=0)
        assert "dry run: nothing changed" in output and f"plan for {data.resolve()}:" in output
        assert tree(data) == before and not (data / volume.VERSIONS).exists(), "dry run wrote"

        output = run_cli("--data-dir", data, "install", first, "--label", "first", expect=0)
        assert sha_one in output and "Restart the guest" in output
        document = data_dir_status(data)
        assert document["backend"] == "data-dir" and document["data_dir"] == str(data.resolve())
        assert "volume" not in document and document["guest_path"] == "/data/ca-bundle.pem"
        assert document["state"] == "valid" and document["active"]["sha256"] == sha_one
        version_one = document["active"]["version"]
        assert version_one.endswith(f"-first-{sha_one[:16]}.pem")
        active = data / volume.ACTIVE
        assert active.read_bytes() == first.read_bytes() and (active.stat().st_mode & 0o777) == 0o444

        assert "nothing to do" in run_cli("--data-dir", data, "install", first, expect=0)
        output = run_cli("--data-dir", data, "install", second, "--label", "second", expect=0)
        assert f"--data-dir {data.resolve()} activate {version_one}" in output, output
        document = data_dir_status(data)
        assert document["state"] == "soon_expiring" and document["active"]["sha256"] == sha_two
        version_two = document["active"]["version"]
        assert [item["name"] for item in document["versions"]] == sorted([version_one, version_two])
        assert (data / volume.VERSIONS / version_one).read_bytes() == first.read_bytes()
        run_cli("--data-dir", data, "status", expect=3)

        # Bad input and a failed publication keep the previous active file.
        before = tree(data)
        for bad, fragment in ((with_key, "private key"), (malformed, "malformed X.509"),
                              (fixtures / "absent.pem", "not a regular file")):
            assert fragment in run_cli("--data-dir", data, "install", bad, expect=1)
        assert "not on the volume" in run_cli("--data-dir", data, "activate",
                                              f"20200101T000000Z-{sha_one[:16]}.pem", expect=1)
        assert "version name" in run_cli("--data-dir", data, "activate", "../ca.pem", expect=1)
        stderr = io.StringIO()
        with mock.patch.object(volume.os, "replace", side_effect=OSError(28, "No space left")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            assert tool.main(["--data-dir", str(data), "activate", version_one]) == 1
        assert "os_error" in stderr.getvalue() and f"--data-dir {data} status" in stderr.getvalue()
        assert tree(data) == before, "a refused or failed operation changed the data dir"

        # Rollback, refusals, removal, unpublish; dry runs never write.
        run_cli("--data-dir", data, "activate", version_one, "--dry-run", expect=0)
        run_cli("--data-dir", data, "remove", version_two, "--dry-run", expect=1)  # active
        run_cli("--data-dir", data, "remove", "--active", "--dry-run", expect=0)
        assert tree(data) == before, "a dry run changed the data dir"
        run_cli("--data-dir", data, "activate", version_one, expect=0)
        assert data_dir_status(data)["active"]["version"] == version_one
        assert "is the active bundle" in run_cli("--data-dir", data, "remove", version_one, expect=1)
        run_cli("--data-dir", data, "remove", version_two, expect=0)
        assert [item["name"] for item in data_dir_status(data)["versions"]] == [version_one]
        assert "stored versions were kept" in run_cli("--data-dir", data, "remove", "--active", expect=0)
        document = data_dir_status(data)
        assert document["state"] == "missing" and [i["name"] for i in document["versions"]] == [version_one]
        assert "already absent" in run_cli("--data-dir", data, "remove", "--active", expect=1)

        # A read-only data dir still answers status but refuses mutation.
        if os.geteuid() != 0:
            data.chmod(0o555)
            try:
                run_cli("--data-dir", data, "status", expect=5)
                run_cli("--data-dir", data, "activate", version_one, "--dry-run", expect=0)
                output = run_cli("--data-dir", data, "activate", version_one, expect=1)
                assert "lacks read, write and search access" in output, output
            finally:
                data.chmod(0o755)

        final = tree(data)
        assert {name: final[name] for name in unrelated} == unrelated, "unrelated files changed"
        ours = set(final) - set(unrelated)
        assert ours == {f"{volume.VERSIONS}/{version_one}"}, ours
        print("data-dir backend lifecycle passed")


def test_docker_check_not_in_make_test():
    makefile = (ROOT / "Makefile").read_text()
    assert "ca_bundle_test.py" in makefile
    assert "ca_bundle_volume_docker_test" not in makefile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: ca_bundle_test.py build/fortis_node")
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        fixtures = Path(temporary)
        long_ca, short_ca = test_validation(fixtures)
        planned = test_planning(long_ca, short_ca)
        test_volume_operations(long_ca, short_ca, *planned)
        test_native_agreement(binary, fixtures)
        test_cli(fixtures, binary)
        test_data_dir_backend(fixtures)
    test_docker_check_not_in_make_test()
    print("CA bundle validation, planning and volume operations passed")


if __name__ == "__main__":
    main()
