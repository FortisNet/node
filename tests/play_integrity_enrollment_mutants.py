#!/usr/bin/env python3
"""Mutation check for Android enrollment (run from the repository root).

Usage: play_integrity_enrollment_mutants.py [NAME ...]

Each mutant edits one or more trust-sensitive checks in scratch copies of
src/auth/play_integrity_enrollment.cpp, src/auth/device_enrollment.cpp or
src/auth/device_auth.cpp, compiles the affected objects (release and test-seam
variants) into a temporary directory with the Makefile's own compile lines,
relinks the unit test, the seam server and the release server with the
Makefile's own link lines, and runs tests/play_integrity_enrollment_socket_test.py
on them (device_auth mutants also run build/device_attestation_foundation_test
relinked the same way). The tree and build/ are never written, so no stale
object can hide a mutant. A mutant must make the suite fail ("killed").
Requires the objects `make test-play-integrity` and
`make build/device_attestation_foundation_test` build.
"""

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E = "src/auth/play_integrity_enrollment.cpp"
D = "src/auth/device_enrollment.cpp"
A = "src/auth/device_auth.cpp"

# The objects each source becomes, per binary that the suite runs.
OBJECTS = {E: ["build/play_integrity_enrollment.o", "build/play_integrity_enrollment_test_seam.o"],
           D: ["build/device_enrollment.o"],
           A: ["build/device_auth.o"]}
BINARIES = {"unit": ("build/play_integrity_enrollment_test", "tests/play_integrity_enrollment_test.cpp"),
            "seam": ("build/fortis_node_play_integrity_test", "build/main.o"),
            "release": ("build/fortis_node", "build/main.o"),
            "foundation": ("build/device_attestation_foundation_test",
                           "tests/device_attestation_foundation_test.cpp")}

# name: [(file, original, mutated), ...]
MUTANTS = {
    # The read-only liveness precheck before any Google I/O.
    "precheck": [(E, "  switch (auth.PeekChallenge(request.challenge, request.challenge_length, "
                     "request.now_ms)) {", "  switch (KvReadStatus::Found) {")],
    # Issue a proof for any decode result (all three layers removed).
    "accept_unverified": [
        (E, "  if (decoded.status != PlayDecodeStatus::Verified) {", "  if (false) {"),
        (E, "  if (decoded.verdict != PlayIntegrityStatus::Verified || "
            "decoded.evidence.version_code < 1 ||\n      decoded.evidence.device_label == "
            "PlayDeviceLabel::None) {", "  if (false) {"),
        (A, "      (proof.play_version_code() < 1 ||\n       (proof.play_device_label() != 1 && "
            "proof.play_device_label() != 2)))", "      false)")],
    # EnrollAttested's own evidence check (killed by the attestation foundation test).
    "evidence_check": [(A, "      (proof.play_version_code() < 1 ||\n       (proof.play_device_label() "
                           "!= 1 && proof.play_device_label() != 2)))", "      false)")],
    "route_branch": [(D, 'if (payload.has_attestation && strcmp(payload.provider, "play-integrity") == 0)',
                      "if (false)")],
    "android_platform": [(D, "if (platform != fortis::auth::DevicePlatform::Android || !payload.has_token) {",
                          "if (!payload.has_token) {")],
    "not_configured_answer": [(D, "  if (!play) {\n    send_device_error(client, 401, \"Unauthorized\", "
                                  "\"{\\\"error\\\":\\\"ATTESTATION_REQUIRED\\\"}\");",
                               "  if (!play) {\n    send_device_error(client, 401, \"Unauthorized\", "
                               "\"{\\\"error\\\":\\\"ATTESTATION_INVALID\\\"}\");")],
    "unavailable_answer": [(D, '"{\\"error\\":\\"ATTESTATION_UNAVAILABLE\\"}", 5)',
                            '"{\\"error\\":\\"ATTESTATION_BUSY\\"}", 1)')],
    "attested_tier": [(E, "DeviceQuotaTier::Attested, decided,", "DeviceQuotaTier::Standard, decided,")],
    "post_decode_clock": [(E, "  int64_t decided = request.clock ? request.clock(request.clock_context) "
                              ": realtime_ms();", "  int64_t decided = request.now_ms;")],
    "clock_clamp": [(E, "  if (decided < request.now_ms) decided = request.now_ms;\n", "")],
    "ttl": [(E, "request.ttl_ms, output)) {", "86400000, output)) {")],
    "busy_mapping": [(E, "    case PlayDecodeStatus::Busy:\n    case PlayDecodeStatus::RateLimited: "
                         "return PlayEnrollStatus::Busy;",
                      "    case PlayDecodeStatus::Busy: return PlayEnrollStatus::Busy;")],
    "token_rejected_mapping": [(E, "    case PlayDecodeStatus::TokenRejected:\n", "")],
    "empty_is_off": [(E, "    if (!*value) {\n      set_error(error, capacity, \"Invalid environment: %s "
                         "is empty\", name);\n      return false;\n    }", "    if (!*value) return true;")],
    "presence": [(E, "    if (!value) continue;\n    any = true;",
                  "    if (!value || strcmp(name, kCredentialVariable) != 0) continue;\n    any = true;")],
    "digest_duplicates": [(E, "      if (memcmp(output[j], output[parsed], 32) == 0) return false;",
                           "      if (false) return false;")],
    "absolute_path": [(E, "if (!length || length > kPlayConfigPathMaxBytes || value[0] != '/') return false;",
                       "if (!length || length > kPlayConfigPathMaxBytes) return false;")],
    "version_minimum": [(E, "!decimal(version, 1, kPlayMaxVersionCode,", "!decimal(version, 0, kPlayMaxVersionCode,")],
    "ca_validation": [(E, "  if (!http::OutboundHttpsInitialize() || !http::OutboundHttpsValidateCaBundle(config.ca_file)) {",
                       "  if (!http::OutboundHttpsInitialize()) {")],
    "seam_pairing": [(E, "  if ((test_host == nullptr) != (test_port == nullptr)) {", "  if (false) {")],
}


def make_line(target: str, touch: str) -> list[str]:
    """The Makefile's command that writes `target`, as make -n prints it."""
    output = subprocess.run(["make", "-n", "-W", touch, target], cwd=ROOT, check=True,
                            capture_output=True, text=True).stdout
    for line in output.splitlines():
        words = shlex.split(line)
        if len(words) > 2 and words[-2:] == ["-o", target]:
            return words
    raise RuntimeError(f"no command for {target}")


def compile_object(obj: str, source_copy: Path, out: Path) -> None:
    source = next(src for src, objs in OBJECTS.items() if obj in objs)
    words = make_line(obj, source)
    words = [str(source_copy) if word == source else word for word in words if word != "-Werror"]
    words[-1] = str(out)
    subprocess.run(words + [f"-I{ROOT / 'src'}"], cwd=ROOT, check=True)


def link(binary: str, touch: str, replaced: dict[str, Path], out: Path) -> None:
    words = make_line(binary, touch)
    words = [str(replaced.get(word, word)) for word in words if word != "-Werror"]
    words[-1] = str(out)
    subprocess.run(words, cwd=ROOT, check=True)


def run_mutant(name: str, edits: list[tuple[str, str, str]]) -> tuple[bool, str]:
    texts: dict[str, str] = {}
    for path, original, mutated in edits:
        text = texts.get(path, (ROOT / path).read_text())
        assert text.count(original) == 1, (name, path, text.count(original))
        texts[path] = text.replace(original, mutated)
    with tempfile.TemporaryDirectory(prefix=f"play-c3-{name}-") as scratch_name:
        scratch = Path(scratch_name)
        replaced: dict[str, Path] = {}
        for path, text in texts.items():
            copy = scratch / Path(path).name
            copy.write_text(text)
            for obj in OBJECTS[path]:
                out = scratch / Path(obj).name
                compile_object(obj, copy, out)
                replaced[obj] = out
        built = {}
        wanted = ["unit", "seam", "release"] + (["foundation"] if A in texts else [])
        for key in wanted:
            binary, touch = BINARIES[key]
            built[key] = scratch / Path(binary).name
            link(binary, touch, replaced, built[key])
        env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
        result = subprocess.run([sys.executable, "tests/play_integrity_enrollment_socket_test.py",
                                 str(built["unit"]), str(built["seam"]), str(built["release"])],
                                cwd=ROOT, capture_output=True, text=True, timeout=900, env=env)
        output = result.stdout + result.stderr
        failed = result.returncode != 0
        if not failed and "foundation" in built:
            foundation = subprocess.run([str(built["foundation"])], cwd=ROOT, capture_output=True,
                                        text=True, timeout=900)
            failed = foundation.returncode != 0
            output += foundation.stdout + foundation.stderr
    lines = output.strip().splitlines()
    # The unit binary's own diagnostic is the line before the driver's
    # traceback when the unit groups failed.
    if "Traceback (most recent call last):" in lines:
        before = lines[:lines.index("Traceback (most recent call last):")]
        lines = before[-1:] + lines
    evidence = next((line for line in lines if "check failed" in line or "got " in line
                     or "AssertionError" in line or "accepted" in line or "error \"" in line
                     or ": " in line), lines[-1] if lines else "")
    return failed, evidence.strip()


def main() -> None:
    names = sys.argv[1:] or list(MUTANTS)
    # Control: the same relinking pipeline with no edit must pass, so a kill
    # below is the mutant's doing, not the harness's.
    passed_control = not run_mutant("control", [])[0]
    if not passed_control:
        sys.exit("control (unmutated) run failed: the harness is broken")
    print("control  unmutated relink passes")
    survivors = []
    for name in names:
        killed, evidence = run_mutant(name, MUTANTS[name])
        if killed:
            print(f"killed   {name:<24} {evidence[:110]}")
        else:
            survivors.append(name)
            print(f"SURVIVED {name}")
    if survivors:
        sys.exit(f"surviving mutants: {survivors}")
    print(f"all {len(names)} mutants killed")


if __name__ == "__main__":
    main()
