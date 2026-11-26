#!/usr/bin/env python3
"""Exercise locked, validated offline backup and restore against real KV logs."""

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/kv_backup_fixture"
CLI = ROOT / "scripts/kv_backup.py"


def fixture(mode: str, directory: Path, *args: str,
            durability: str = "rollback-marker") -> subprocess.CompletedProcess:
    return subprocess.run([str(FIXTURE), mode, str(directory), *args],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env={**os.environ, "FORTIS_KV_DURABILITY": durability},
                          text=True, timeout=10)


def cli(operation: str, directory: Path, path: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(CLI), operation, str(directory), str(path)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=10)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="fortis-kv-backup-") as temp:
        root = Path(temp)
        live_dir = root / "data"
        live_dir.mkdir()
        live_path = live_dir / "fortis.kv"
        backup_path = root / "backup.kv"
        assert fixture("write", live_dir, "v1").returncode == 0

        compact_path = live_dir / "fortis.kv.compact"
        compact_path.write_bytes(b"stale-compaction-file")
        result = cli("backup", live_dir, compact_path)
        assert result.returncode != 0
        assert compact_path.read_bytes() == b"stale-compaction-file"
        assert cli("restore", live_dir, compact_path).returncode != 0
        assert compact_path.read_bytes() == b"stale-compaction-file"
        compact_path.unlink()

        result = cli("backup", live_dir, backup_path)
        assert result.returncode == 0, result.stderr
        assert "recoverable_tail_bytes=0" in result.stdout
        version_one = backup_path.read_bytes()
        assert fixture("write", live_dir, "v2").returncode == 0
        version_two = live_path.read_bytes()
        result = cli("restore", live_dir, backup_path)
        assert result.returncode == 0, result.stderr
        assert fixture("check", live_dir, "v1").returncode == 0
        assert (live_dir / "fortis.kv.rollback").read_bytes() == version_two

        # A complete malformed header and a complete bad-CRC frame are refused
        # without changing the current live file.
        before_invalid = live_path.read_bytes()
        malformed = root / "malformed.kv"
        malformed.write_bytes(b"NOPE" + struct.pack("<II", 4, 0))
        result = cli("restore", live_dir, malformed)
        assert result.returncode != 0 and live_path.read_bytes() == before_invalid
        malformed_record = root / "malformed-record.kv"
        body = struct.pack("<I", 1) + b"x"
        malformed_record.write_bytes(b"FKV1" + struct.pack("<II", len(body),
                                                             zlib.crc32(body)) + body)
        result = cli("restore", live_dir, malformed_record)
        assert result.returncode != 0 and live_path.read_bytes() == before_invalid
        malformed_tail = root / "malformed-tail.kv"
        malformed_tail.write_bytes(version_one + b"BAD")
        result = cli("restore", live_dir, malformed_tail)
        assert result.returncode != 0 and live_path.read_bytes() == before_invalid
        bad_crc = root / "bad-crc.kv"
        damaged = bytearray(version_one)
        damaged[-1] ^= 0x01
        bad_crc.write_bytes(damaged)
        result = cli("restore", live_dir, bad_crc)
        assert result.returncode != 0 and "checksum" in result.stderr
        assert live_path.read_bytes() == before_invalid

        # Replay accepts and truncates an incomplete final header. Backup keeps
        # the exact recoverable tail; restore succeeds and the next open trims it.
        with live_path.open("ab") as stream:
            stream.write(b"FKV1\x20\x00")
        tailed = live_path.read_bytes()
        result = cli("backup", live_dir, backup_path)
        assert result.returncode == 0, result.stderr
        assert "recoverable_tail_bytes=6" in result.stdout
        assert backup_path.read_bytes() == tailed
        result = cli("restore", live_dir, backup_path)
        assert result.returncode == 0, result.stderr
        assert fixture("check", live_dir, "v1").returncode == 0
        assert live_path.read_bytes() == version_one

        with live_path.open("ab") as stream:
            stream.write(b"FKV1" + struct.pack("<II", 64, 0) + b"\x01\x00")
        result = cli("backup", live_dir, backup_path)
        assert result.returncode == 0, result.stderr
        assert "recoverable_tail_bytes=14" in result.stdout
        result = cli("restore", live_dir, backup_path)
        assert result.returncode == 0, result.stderr
        assert fixture("check", live_dir, "v1").returncode == 0
        assert live_path.read_bytes() == version_one

        # A corrupted live log cannot overwrite a previous backup. The known
        # good backup can then restore it, retaining the corrupt bytes as rollback.
        pristine_backup = root / "pristine.kv"
        shutil.copyfile(backup_path, pristine_backup)
        current = bytearray(live_path.read_bytes())
        current[-1] ^= 0x80
        live_path.write_bytes(current)
        backup_path.write_bytes(b"keep-this-existing-backup")
        result = cli("backup", live_dir, backup_path)
        assert result.returncode != 0
        assert backup_path.read_bytes() == b"keep-this-existing-backup"
        corrupt_live = live_path.read_bytes()
        result = cli("restore", live_dir, pristine_backup)
        assert result.returncode == 0, result.stderr
        assert fixture("check", live_dir, "v1").returncode == 0
        assert (live_dir / "fortis.kv.rollback").read_bytes() == corrupt_live

        # A live KvStore owner excludes both maintenance operations without
        # altering the log or output backup.
        owner = subprocess.Popen([str(FIXTURE), "hold", str(live_dir)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True)
        assert owner.stdout is not None and owner.stdout.readline().strip() == "ready"
        locked_live = live_path.read_bytes()
        locked_backup = backup_path.read_bytes()
        for operation, path in (("backup", backup_path), ("restore", pristine_backup)):
            result = cli(operation, live_dir, path)
            assert result.returncode != 0 and "lock" in result.stderr.lower()
            assert live_path.read_bytes() == locked_live
        assert backup_path.read_bytes() == locked_backup
        owner.terminate()
        owner.wait(timeout=5)

        commit_slots(root)

    print("native KV offline backup/restore validation test passed")


def commit_slots(root: Path) -> None:
    """Backup, validation and export of a commit-slot log."""
    slots = "commit-slots"
    data = root / "slots"
    data.mkdir()
    live = data / "fortis.kv"
    backup_path = root / "slots-backup.kv"
    assert fixture("write", data, "s1", durability=slots).returncode == 0
    assert fixture("write", data, "s2", durability=slots).returncode == 0
    published = live.read_bytes()
    assert published[:4] == b"FKVS"
    # The default mode refuses the migrated log and leaves it untouched.
    assert fixture("check", data, "s2").returncode == 3
    assert live.read_bytes() == published

    result = cli("backup", data, backup_path)
    assert result.returncode == 0, result.stderr
    assert "format=commit-slots" in result.stdout and "recoverable_tail_bytes=0" in result.stdout
    assert backup_path.read_bytes() == published

    # Bytes past the published end are reported, not validated.
    live.write_bytes(published + b"unpublished tail")
    result = cli("backup", data, backup_path)
    assert result.returncode == 0, result.stderr
    assert "recoverable_tail_bytes=16" in result.stdout
    live.write_bytes(published)

    # A damaged published frame or two damaged slots are refused.
    keep = b"keep-this-existing-backup"
    refused_path = root / "slots-refused.kv"
    for offset in (len(published) - 1, None):
        damaged = bytearray(published)
        if offset is None:
            damaged[2000] ^= 0x01
            damaged[4096 + 2000] ^= 0x01
        else:
            damaged[offset] ^= 0x01
        live.write_bytes(damaged)
        refused_path.write_bytes(keep)
        result = cli("backup", data, refused_path)
        assert result.returncode != 0 and refused_path.read_bytes() == keep
    live.write_bytes(published)

    # export-legacy writes the published frames as a default-mode log, and
    # restore installs it, keeping the commit-slot log as the rollback copy.
    legacy_path = root / "slots-legacy.kv"
    live.write_bytes(published + b"unpublished tail")
    result = cli("export-legacy", data, legacy_path)
    assert result.returncode == 0, result.stderr
    assert "format=rollback-marker" in result.stdout
    assert legacy_path.read_bytes() == published[8192:]
    with_tail = live.read_bytes()
    result = cli("restore", data, legacy_path)
    assert result.returncode == 0, result.stderr
    assert (data / "fortis.kv.rollback").read_bytes() == with_tail
    assert fixture("check", data, "s2").returncode == 0
    assert cli("export-legacy", data, legacy_path).returncode != 0

    # A commit-slot backup restores into a commit-slot store.
    result = cli("restore", data, backup_path)
    assert result.returncode == 0, result.stderr
    assert fixture("check", data, "s2", durability=slots).returncode == 0


if __name__ == "__main__":
    main()
