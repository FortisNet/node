#!/usr/bin/env python3
"""Offline backup and restore for the native Fortis KV append log.

Both log formats are accepted. A rollback-marker log is a sequence of FKV1
frames. A commit-slot log (FORTIS_KV_DURABILITY=commit-slots) starts with two
4 KiB commit slots and holds frames from byte 8192; only frames up to the
newest valid slot's committed end are published. export-legacy writes those
published frames as a rollback-marker log, so restore can return a store to
the default mode.
"""

import argparse
import errno
import fcntl
import os
from pathlib import Path
import struct
import sys
import tempfile
from typing import Optional
import zlib

HEADER_SIZE = 12
RECORD_SIZE = 15
MAX_FRAME = 8 * 1024 * 1024
MAX_VALUE = 1024 * 1024
MAX_KEY = 1024
MAX_EXPIRY = (1 << 63) - 1
LIVE_NAME = "fortis.kv"
LOCK_NAME = "fortis.kv.lock"
RECOVERY_NAME = "fortis.kv.recover"
ROLLBACK_NAME = "fortis.kv.rollback"
SLOT_BYTES = 4096
LOG_BASE = 2 * SLOT_BYTES
ROLLBACK_MARKER = "rollback-marker"
COMMIT_SLOTS = "commit-slots"


class BackupError(Exception):
    pass


def acquire_lock(directory: Path) -> int:
    lock_path = directory / LOCK_NAME
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in (errno.EACCES, errno.EAGAIN):
                raise BackupError("KV directory lock is held by an active writer") from error
            raise
        probe = os.open(lock_path, os.O_RDWR)
        try:
            try:
                fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                if error.errno not in (errno.EACCES, errno.EAGAIN):
                    raise
            else:
                raise BackupError("host filesystem does not enforce flock")
        finally:
            os.close(probe)
        fsync_directory(directory)
        return fd
    except BaseException:
        os.close(fd)
        raise


def fsync_directory(directory: Path) -> None:
    fd = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def pread_exact(fd: int, length: int, offset: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = os.pread(fd, length - len(chunks), offset + len(chunks))
        if not chunk:
            break
        chunks.extend(chunk)
    return bytes(chunks)


def decode_slot(block: bytes, index: int):
    """Return (generation, sequence, committed end) for a valid slot, else None."""
    if len(block) != SLOT_BYTES or block[:4] != b"FKVS":
        return None
    version, generation, sequence, committed, base = struct.unpack_from("<IQQQQ", block, 4)
    checksum = struct.unpack_from("<I", block, SLOT_BYTES - 4)[0]
    if (version != 1 or zlib.crc32(block[:SLOT_BYTES - 4]) & 0xFFFFFFFF != checksum or
            not generation or not sequence or sequence & 1 != index or
            base != LOG_BASE or committed < LOG_BASE):
        return None
    return generation, sequence, committed


def select_slot(header: bytes) -> tuple[int, int, int]:
    """Apply KvStore's rule: the newest valid slot, if both agree on the file."""
    valid = [slot for slot in (decode_slot(header[:SLOT_BYTES], 0),
                               decode_slot(header[SLOT_BYTES:LOG_BASE], 1)) if slot]
    if not valid:
        raise BackupError("no valid commit slot")
    newer = max(valid, key=lambda slot: slot[1])
    older = min(valid, key=lambda slot: slot[1])
    if newer[0] != older[0] or newer[2] < older[2]:
        raise BackupError("inconsistent commit slots")
    return newer


def log_format(fd: int, size: int) -> str:
    # A rollback-marker log starts with a frame header; a commit-slot log never does.
    if size < 4 or pread_exact(fd, 4, 0) == b"FKV1":
        return ROLLBACK_MARKER
    if size >= LOG_BASE:
        header = pread_exact(fd, LOG_BASE, 0)
        if decode_slot(header[:SLOT_BYTES], 0) or decode_slot(header[SLOT_BYTES:], 1):
            return COMMIT_SLOTS
    raise BackupError("unrecognized KV log format")


def validate_frames(fd: int, offset: int, limit: int, exact: bool) -> tuple[int, int]:
    """Validate frames in [offset, limit). With `exact`, they must end at limit."""
    while offset < limit:
        remaining = limit - offset
        if remaining < HEADER_SIZE:
            if exact:
                raise BackupError(f"committed end inside a frame at byte {offset}")
            tail = pread_exact(fd, remaining, offset)
            if len(tail) != remaining or tail[:min(4, remaining)] != b"FKV1"[:min(4, remaining)]:
                raise BackupError(f"invalid partial frame header at byte {offset}")
            return offset, remaining
        header = pread_exact(fd, HEADER_SIZE, offset)
        if len(header) != HEADER_SIZE or header[:4] != b"FKV1":
            raise BackupError(f"invalid frame header at byte {offset}")
        body_size, expected_crc = struct.unpack_from("<II", header, 4)
        if body_size < 4 or body_size > MAX_FRAME:
            raise BackupError(f"invalid frame length at byte {offset}")
        if body_size > remaining - HEADER_SIZE:
            if exact:
                raise BackupError(f"committed end inside a frame at byte {offset}")
            return offset, remaining
        body = pread_exact(fd, body_size, offset + HEADER_SIZE)
        if len(body) != body_size:
            if exact:
                raise BackupError(f"short frame at byte {offset}")
            return offset, remaining
        if zlib.crc32(body) & 0xFFFFFFFF != expected_crc:
            raise BackupError(f"frame checksum mismatch at byte {offset}")
        validate_body(body, offset)
        offset += HEADER_SIZE + body_size
    return offset, 0


def validate_log(path: Path) -> tuple[str, int, int]:
    """Return (format, valid end, tail bytes the next open drops), or reject corruption.

    For a commit-slot log the tail is every byte past the committed end; it
    was never published and is not validated.
    """
    fd = os.open(path, os.O_RDONLY)
    try:
        size = os.fstat(fd).st_size
        log = log_format(fd, size)
        if log == ROLLBACK_MARKER:
            return (log, *validate_frames(fd, 0, size, exact=False))
        committed = select_slot(pread_exact(fd, LOG_BASE, 0))[2]
        if committed > size:
            raise BackupError("commit slot names bytes past the end of the log")
        validate_frames(fd, LOG_BASE, committed, exact=True)
        return log, committed, size - committed
    finally:
        os.close(fd)


def validate_body(body: bytes, frame_offset: int) -> None:
    count = struct.unpack_from("<I", body, 0)[0]
    cursor = 4
    for _ in range(count):
        if len(body) - cursor < RECORD_SIZE:
            raise BackupError(f"invalid record header at byte {frame_offset + HEADER_SIZE + cursor}")
        operation = body[cursor]
        key_length = body[cursor + 1] | (body[cursor + 2] << 8)
        value_length = struct.unpack_from("<I", body, cursor + 3)[0]
        expiry = struct.unpack_from("<Q", body, cursor + 7)[0]
        cursor += RECORD_SIZE
        if (operation > 1 or not key_length or key_length > MAX_KEY or
                value_length > MAX_VALUE or expiry > MAX_EXPIRY or
                key_length + value_length > len(body) - cursor):
            raise BackupError(f"invalid record at byte {frame_offset + HEADER_SIZE + cursor}")
        key = body[cursor:cursor + key_length]
        if b"\0" in key:
            raise BackupError(f"NUL byte in key at byte {frame_offset + HEADER_SIZE + cursor}")
        if operation == 1 and (value_length or expiry):
            raise BackupError(f"invalid erase record at byte {frame_offset + HEADER_SIZE + cursor}")
        cursor += key_length + value_length
    if cursor != len(body):
        raise BackupError(f"frame record count mismatch at byte {frame_offset}")


def new_temp(directory: Path, prefix: str) -> tuple[int, Path]:
    fd, name = tempfile.mkstemp(prefix=prefix, dir=directory)
    os.fchmod(fd, 0o600)
    return fd, Path(name)


def copy_fd(source_fd: int, destination_fd: int, start: int = 0,
            length: Optional[int] = None) -> None:
    """Append source bytes from `start` to the destination: `length` bytes, or to EOF."""
    offset = start
    end = None if length is None else start + length
    while True:
        chunk = 1024 * 1024 if end is None else min(1024 * 1024, end - offset)
        data = os.pread(source_fd, chunk, offset) if chunk else b""
        if not data:
            if end is not None and offset != end:
                raise BackupError("short read while copying KV log")
            return
        written = 0
        while written < len(data):
            count = os.write(destination_fd, data[written:])
            if count <= 0:
                raise OSError("short write while copying KV log")
            written += count
        offset += len(data)


def backup_paths(directory: Path, destination: Path) -> tuple[Path, Path]:
    if (directory / RECOVERY_NAME).exists():
        raise BackupError("KV writeback recovery is pending. Recover the log before backing it up.")
    live = directory / LIVE_NAME
    if not live.is_file():
        raise BackupError(f"live KV log not found: {live}")
    destination = destination.absolute()
    if destination.parent.resolve() == directory.resolve():
        raise BackupError("backup destination must be outside the live KV directory")
    if not destination.parent.is_dir():
        raise BackupError(f"backup parent directory does not exist: {destination.parent}")
    return live, destination


def publish_copy(source_fd: int, destination: Path, start: int, length: Optional[int],
                 expected: Optional[tuple[str, int, int]] = None) -> tuple[str, int, int]:
    """Copy source bytes from `start` (to EOF when length is None), validate, then publish."""
    parent = destination.parent
    temp_fd, temp_path = new_temp(parent, f".{destination.name}.tmp.")
    try:
        try:
            copy_fd(source_fd, temp_fd, start, length)
            os.fsync(temp_fd)
        finally:
            os.close(temp_fd)
        result = validate_log(temp_path)
        if expected is not None and result != expected:
            raise BackupError("exported log does not match the published frames")
        os.replace(temp_path, destination)
        fsync_directory(parent)
        return result
    except BaseException:
        temp_path.unlink(missing_ok=True)
        raise


def backup(directory: Path, destination: Path) -> tuple[str, int, int]:
    live, destination = backup_paths(directory, destination)
    source_fd = os.open(live, os.O_RDONLY)
    try:
        return publish_copy(source_fd, destination, 0, None)
    finally:
        os.close(source_fd)


def export_legacy(directory: Path, destination: Path) -> tuple[str, int, int]:
    """Write a commit-slot log's published frames as a rollback-marker log."""
    live, destination = backup_paths(directory, destination)
    log, committed, _ = validate_log(live)
    if log != COMMIT_SLOTS:
        raise BackupError("the live log already uses the rollback-marker format; use backup")
    source_fd = os.open(live, os.O_RDONLY)
    length = committed - LOG_BASE
    try:
        return publish_copy(source_fd, destination, LOG_BASE, length,
                            (ROLLBACK_MARKER, length, 0))
    finally:
        os.close(source_fd)


def create_durable_copy(source: Path, destination: Path) -> None:
    source_fd = os.open(source, os.O_RDONLY)
    temp_fd, temp_path = new_temp(destination.parent, f".{destination.name}.tmp.")
    try:
        copy_fd(source_fd, temp_fd)
        os.fsync(temp_fd)
    except BaseException:
        os.close(source_fd)
        os.close(temp_fd)
        temp_path.unlink(missing_ok=True)
        raise
    os.close(source_fd)
    os.close(temp_fd)
    try:
        os.replace(temp_path, destination)
        fsync_directory(destination.parent)
    except BaseException:
        temp_path.unlink(missing_ok=True)
        raise


def restore(directory: Path, candidate: Path) -> tuple[str, int, int]:
    if (directory / RECOVERY_NAME).exists():
        raise BackupError("KV writeback recovery is pending. Recover the log before restoring over it.")
    candidate = candidate.absolute()
    live = directory / LIVE_NAME
    rollback = directory / ROLLBACK_NAME
    if candidate.parent.resolve() == directory.resolve():
        raise BackupError("restore candidate must be outside the live KV directory")
    if not candidate.is_file():
        raise BackupError(f"backup candidate not found: {candidate}")

    # Copy the external candidate to a private staged file, then validate the
    # exact bytes that would replace the live log.
    candidate_fd = os.open(candidate, os.O_RDONLY)
    staged_fd, staged_path = new_temp(directory, ".fortis.kv.restore.")
    try:
        try:
            copy_fd(candidate_fd, staged_fd)
            os.fsync(staged_fd)
        finally:
            os.close(candidate_fd)
            os.close(staged_fd)
        result = validate_log(staged_path)
    except BaseException:
        staged_path.unlink(missing_ok=True)
        raise

    had_live = live.exists()
    replaced = False
    try:
        if had_live:
            create_durable_copy(live, rollback)
        os.replace(staged_path, live)
        replaced = True
        fsync_directory(directory)
        return result
    except BaseException as error:
        staged_path.unlink(missing_ok=True)
        if replaced:
            try:
                if had_live:
                    create_durable_copy(rollback, staged_path)
                    os.replace(staged_path, live)
                else:
                    live.unlink(missing_ok=True)
                fsync_directory(directory)
            except BaseException as rollback_error:
                raise BackupError(
                    f"restore failed ({error}); rollback from {rollback} also failed ({rollback_error})"
                ) from rollback_error
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    backup_parser = subparsers.add_parser("backup", help="copy and validate the live log")
    backup_parser.add_argument("directory", type=Path, help="KV data directory")
    backup_parser.add_argument("destination", type=Path, help="backup output path")
    restore_parser = subparsers.add_parser("restore", help="validate and install a backup")
    restore_parser.add_argument("directory", type=Path, help="KV data directory")
    restore_parser.add_argument("candidate", type=Path, help="backup input path")
    export_parser = subparsers.add_parser(
        "export-legacy", help="write a commit-slot log's published frames as a rollback-marker log")
    export_parser.add_argument("directory", type=Path, help="KV data directory")
    export_parser.add_argument("destination", type=Path, help="exported log output path")
    args = parser.parse_args()
    directory = args.directory.absolute()
    if not directory.is_dir():
        parser.error(f"KV data directory does not exist: {directory}")
    lock_fd = -1
    try:
        lock_fd = acquire_lock(directory)
        if args.operation == "backup":
            log, valid_end, tail_size = backup(directory, args.destination)
        elif args.operation == "export-legacy":
            log, valid_end, tail_size = export_legacy(directory, args.destination)
        else:
            log, valid_end, tail_size = restore(directory, args.candidate)
        print(f"{args.operation} complete: format={log} valid_bytes={valid_end} "
              f"recoverable_tail_bytes={tail_size}")
        return 0
    except (BackupError, OSError) as error:
        print(f"KV {args.operation} refused: {error}", file=sys.stderr)
        return 1
    finally:
        if lock_fd >= 0:
            os.close(lock_fd)


if __name__ == "__main__":
    raise SystemExit(main())
