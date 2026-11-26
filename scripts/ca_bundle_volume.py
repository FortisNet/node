#!/usr/bin/env python3
"""Volume-side file operations for scripts/ca_bundle.py.

scripts/ca_bundle.py runs this source with `python3 -I -c` inside the existing
build image, with only the named guest volume mounted at /data and no network.
With `--data-dir` (the Linux KVM launcher's ./data) it calls `execute` in
process against that host directory instead. Offline tests run the same
functions against a temporary directory. Nothing
here interprets certificates: the host validates a bundle first and passes
its SHA-256, which is re-checked after every write so a short or corrupted
copy is never published. Only these names under the root are touched:

  ca-bundle.pem                   active bundle (the stable guest path)
  .ca-bundle.pem.tmp-<nonce>      publication temp, renamed over the above
  ca-bundles/<version>.pem        immutable versions (mode 0444)
  ca-bundles/.incoming-<nonce>    version temp, hard-linked into place

The active file is replaced only by rename(2), so a failed validation, copy
or rename leaves the previous active bundle in place. Mutations hold an
exclusive flock on the versions directory so two operators cannot interleave.
Every call prints one JSON object on stdout and never echoes file contents
except as base64 in `snapshot`, which only the host tool reads.
"""

import base64
import errno
import fcntl
import hashlib
import json
import os
import re
import secrets
import stat
import sys

ROOT = "/data"
ACTIVE = "ca-bundle.pem"
VERSIONS = "ca-bundles"
ACTIVE_TEMP_PREFIX = ".ca-bundle.pem.tmp-"
VERSION_TEMP_PREFIX = ".incoming-"
VERSION_NAME = re.compile(r"\A[0-9]{8}T[0-9]{6}Z(?:-[A-Za-z0-9._-]{1,40})?-[0-9a-f]{16}\.pem\Z")
SHA256 = re.compile(r"\A[0-9a-f]{64}\Z")
MAX_BYTES = 1024 * 1024  # native OutboundHttpsValidateCaBundle cap
MAX_VERSIONS = 64


class VolumeError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fsync_directory(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def read_regular(path):
    """Bytes of a regular file, None when missing. Symlinks are refused."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    except FileNotFoundError:
        return None
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise VolumeError("not_regular", f"{os.path.basename(path)} is a symlink") from None
        raise
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise VolumeError("not_regular", f"{os.path.basename(path)} is not a regular file")
        if info.st_size > MAX_BYTES:
            raise VolumeError("too_large", f"{os.path.basename(path)} exceeds {MAX_BYTES} bytes")
        chunks = []
        remaining = MAX_BYTES + 1
        while remaining > 0:
            chunk = os.read(fd, min(65536, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        if len(data) > MAX_BYTES:
            raise VolumeError("too_large", f"{os.path.basename(path)} exceeds {MAX_BYTES} bytes")
        return data
    finally:
        os.close(fd)


def active_digest(path):
    """SHA-256 of the regular file at the stable path (any size), None when missing."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(info.st_mode):
        raise VolumeError("not_regular", f"{os.path.basename(path)} is not a regular file")
    hasher = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                return hasher.hexdigest()
            hasher.update(chunk)
    finally:
        os.close(fd)


def write_new(path, data, mode=0o444):
    """Creates `path` exclusively, writes, fsyncs and sets `mode`."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            view = view[written:]
        os.fsync(fd)
        os.fchmod(fd, mode)
    finally:
        os.close(fd)


def unlink_quietly(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def check_name(name):
    if not isinstance(name, str) or not VERSION_NAME.match(name):
        raise VolumeError("bad_version", "version name is not a ca_bundle.py version")


def check_sha(sha):
    if not isinstance(sha, str) or not SHA256.match(sha):
        raise VolumeError("bad_digest", "expected SHA-256 must be 64 lowercase hex digits")


def versions_dir(root, create):
    path = os.path.join(root, VERSIONS)
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        if not create:
            return None
        os.mkdir(path, 0o755)
        fsync_directory(root)
        return path
    if not stat.S_ISDIR(info.st_mode):
        raise VolumeError("not_regular", f"{VERSIONS} is not a directory")
    return path


class Lock:
    """Exclusive non-blocking flock on the versions directory."""

    def __init__(self, root):
        self.path = versions_dir(root, create=True)
        self.fd = None

    def __enter__(self):
        self.fd = os.open(self.path, os.O_RDONLY)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            os.close(self.fd)
            if error.errno in (errno.EAGAIN, errno.EACCES):
                raise VolumeError("busy", "another CA bundle operation holds the lock") from None
            raise
        return self

    def __exit__(self, *exc):
        os.close(self.fd)


def describe(path):
    try:
        data = read_regular(path)
    except VolumeError as error:
        return {"state": error.code}
    if data is None:
        return {"state": "missing"}
    return {"state": "present", "size": len(data), "sha256": digest(data),
            "content": base64.b64encode(data).decode("ascii")}


def snapshot(root=ROOT):
    """Active file, versions and leftover temps; content only as base64."""
    result = {"active": describe(os.path.join(root, ACTIVE)), "versions": [],
              "temps": [], "other_entries": 0, "truncated": False}
    result["temps"] = sorted(name for name in os.listdir(root)
                             if name.startswith(ACTIVE_TEMP_PREFIX))
    path = versions_dir(root, create=False)
    if path is None:
        return result
    names = sorted(os.listdir(path))
    for name in names:
        if name.startswith(VERSION_TEMP_PREFIX):
            result["temps"].append(f"{VERSIONS}/{name}")
        elif not VERSION_NAME.match(name):
            result["other_entries"] += 1
        elif len(result["versions"]) >= MAX_VERSIONS:
            result["truncated"] = True
        else:
            result["versions"].append({"name": name, **describe(os.path.join(path, name))})
    return result


def publish(root, name, sha):
    """Atomically replaces the active file with version `name`.

    Returns the previous active SHA-256 (None when there was none).
    """
    check_name(name)
    check_sha(sha)
    source = read_regular(os.path.join(root, VERSIONS, name))
    if source is None:
        raise VolumeError("version_missing", f"version {name} does not exist")
    if digest(source) != sha:
        raise VolumeError("digest_mismatch", f"version {name} does not match the validated digest")
    active = os.path.join(root, ACTIVE)
    previous = active_digest(active)  # refuses a symlink or directory at the stable path
    temp = os.path.join(root, ACTIVE_TEMP_PREFIX + secrets.token_hex(8))
    try:
        write_new(temp, source)
        if digest(read_regular(temp) or b"") != sha:
            raise VolumeError("copy_failed", "the published copy did not match the validated digest")
        os.replace(temp, active)
    finally:
        unlink_quietly(temp)
    fsync_directory(root)
    return previous


def install(root, name, sha, data):
    """Creates immutable version `name` (or reuses an identical one), then publishes it."""
    check_name(name)
    check_sha(sha)
    if len(data) > MAX_BYTES:
        raise VolumeError("too_large", f"bundle exceeds {MAX_BYTES} bytes")
    if digest(data) != sha:
        raise VolumeError("digest_mismatch", "received bytes do not match the validated digest")
    with Lock(root) as lock:
        final = os.path.join(lock.path, name)
        existing = read_regular(final)
        created = False
        if existing is not None:
            if digest(existing) != sha:
                raise VolumeError("version_conflict", f"version {name} exists with different content")
        else:
            temp = os.path.join(lock.path, VERSION_TEMP_PREFIX + secrets.token_hex(8))
            try:
                write_new(temp, data)
                if digest(read_regular(temp) or b"") != sha:
                    raise VolumeError("copy_failed", "the version copy did not match the validated digest")
                try:
                    os.link(temp, final)  # never overwrites an existing version
                except FileExistsError:
                    raise VolumeError("version_conflict", f"version {name} appeared concurrently") from None
                created = True
            finally:
                unlink_quietly(temp)
            fsync_directory(lock.path)
        previous = publish(root, name, sha)
    return {"version": name, "created": created, "previous_sha256": previous}


def activate(root, name, sha):
    with Lock(root):
        previous = publish(root, name, sha)
    return {"version": name, "previous_sha256": previous}


def remove_version(root, name):
    """Deletes one inactive version; the active bundle's version is refused."""
    check_name(name)
    with Lock(root) as lock:
        path = os.path.join(lock.path, name)
        data = read_regular(path)
        if data is None:
            raise VolumeError("version_missing", f"version {name} does not exist")
        if active_digest(os.path.join(root, ACTIVE)) == digest(data):
            raise VolumeError("version_active", f"version {name} is the active bundle")
        os.unlink(path)
        fsync_directory(lock.path)
    return {"version": name, "removed": True}


def remove_active(root):
    """Unpublishes the stable path; versions stay on the volume."""
    with Lock(root):
        active = os.path.join(root, ACTIVE)
        previous = active_digest(active)
        if previous is None:
            raise VolumeError("active_missing", f"{ACTIVE} does not exist")
        os.unlink(active)
        fsync_directory(root)
    return {"removed": True, "previous_sha256": previous}


def run(argv, stdin, root=ROOT):
    operation = argv[0] if argv else ""
    if operation == "snapshot" and len(argv) == 1:
        return snapshot(root)
    if operation == "install" and len(argv) == 3:
        return install(root, argv[1], argv[2], stdin.read(MAX_BYTES + 1))
    if operation == "activate" and len(argv) == 3:
        return activate(root, argv[1], argv[2])
    if operation == "remove-version" and len(argv) == 2:
        return remove_version(root, argv[1])
    if operation == "remove-active" and len(argv) == 1:
        return remove_active(root)
    raise VolumeError("usage", "unknown volume operation")


def execute(argv, stdin, root=ROOT):
    """The reply object for one operation; refusals and OS errors become ok=False."""
    try:
        return {"ok": True, **run(argv, stdin, root)}
    except VolumeError as error:
        return {"ok": False, "error": error.code, "message": str(error)}
    except OSError as error:
        return {"ok": False, "error": "os_error",
                "message": f"{type(error).__name__}: {os.strerror(error.errno or 0)}"}


def main():
    result = execute(sys.argv[1:], sys.stdin.buffer)
    sys.stdout.write(json.dumps(result) + "\n")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
