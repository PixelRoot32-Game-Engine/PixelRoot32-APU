#!/usr/bin/env python3
"""S9 -- generate and verify the integer core's golden PCM.

The integer render core has to produce the same bytes on every build that
compiles it: an FPU build reaches it through the `Nes` profile or through
`PR32_APU_ENABLE_LEGACY_FLOAT=0`, a no-FPU build through `Legacy`. So the same
canonical track is rendered by three builds of tools/golden_gen and the results
are compared byte for byte.

Checks, in this order (the first one is a gate, not a tolerance):

  1. The default build (`PR32_APU_HAS_FPU` autodetected to 1) with
     `--profile legacy` must reproduce the historical float fixture
     test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm exactly. A tool
     that cannot reproduce the frozen reference is not trustworthy, so a
     mismatch stops the run.
  2. `apu_golden_gen_nofpu --profile legacy` and
     `apu_golden_gen_integer --profile legacy` must be byte-identical to each
     other: the integer core cannot depend on the build.
  3. The committed integer fixture must be byte-identical to that render. With
     `--write` it is regenerated from the nofpu build instead of verified.

    python tools/golden_gen/run.py
    python tools/golden_gen/run.py --write

Needs `cmake` and the MinGW toolchain the repo's test runner already uses.
Scratch renders live under the OS temp directory and are removed afterwards.
"""

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
APU = HERE.parent.parent
BUILD = APU / "build"
FIXTURES = APU / "test" / "golden" / "fixtures"
FLOAT_FIXTURE = FIXTURES / "apu_mirror_canonical_s16le_44100.pcm"
INTEGER_FIXTURE = FIXTURES / "apu_integer_core_canonical_s16le_44100.pcm"

TARGETS = ("apu_golden_gen", "apu_golden_gen_nofpu", "apu_golden_gen_integer")
EXPECTED_BYTES = 44100  # 22 050 S16 samples @ 44 100 Hz


def run(cmd, cwd=None):
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"error: command failed (exit {proc.returncode}):\n"
                 f"  {' '.join(str(c) for c in cmd)}\n"
                 f"{proc.stdout}{proc.stderr}")
    return proc


def configure_and_build():
    # Same configuration as the repo's documented test runner. --fresh because
    # a build/ directory configured with another generator cannot be reused.
    run(["cmake", "-S", ".", "-B", "build", "-G", "MinGW Makefiles", "--fresh",
         "-DPR32_APU_BUILD_TESTS=ON"], cwd=APU)
    run(["cmake", "--build", "build", "--target", *TARGETS], cwd=APU)


def executable(target):
    for name in (target, target + ".exe"):
        path = BUILD / name
        if path.exists():
            return path
    sys.exit(f"error: {target} was not built (looked in {BUILD})")


def sha256(data):
    return hashlib.sha256(data).hexdigest().upper()


def diff_stats(actual, expected):
    """Byte-level description of a mismatch, for the failure report."""
    limit = min(len(actual), len(expected))
    first = next((i for i in range(limit) if actual[i] != expected[i]), None)
    differing = sum(1 for i in range(limit) if actual[i] != expected[i])
    differing += abs(len(actual) - len(expected))
    first_sample = None if first is None else first // 2
    sample_delta = 0
    for i in range(0, limit - 1, 2):
        a = int.from_bytes(actual[i:i + 2], "little", signed=True)
        e = int.from_bytes(expected[i:i + 2], "little", signed=True)
        sample_delta = max(sample_delta, abs(a - e))
    return differing, first, first_sample, sample_delta


def render(binary, out_path):
    proc = run([str(binary), "--profile", "legacy", "--out", str(out_path)])
    line = proc.stdout.strip()
    print(f"  {binary.name}: {line}")
    data = out_path.read_bytes()
    if len(data) != EXPECTED_BYTES:
        sys.exit(f"error: {binary.name} wrote {len(data)} bytes, "
                 f"expected {EXPECTED_BYTES}")
    return data


def check(label, condition, detail=""):
    print(f"  [{'ok' if condition else 'FAIL'}] {label}{detail}")
    return condition


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="rewrite the committed integer fixture from the nofpu build")
    args = ap.parse_args()

    print("building the three golden generators")
    configure_and_build()
    default_gen = executable("apu_golden_gen")
    nofpu_gen = executable("apu_golden_gen_nofpu")
    integer_gen = executable("apu_golden_gen_integer")

    if not FLOAT_FIXTURE.is_file():
        sys.exit(f"error: missing historical fixture {FLOAT_FIXTURE}")

    scratch = pathlib.Path(tempfile.mkdtemp(prefix="pr32-golden-gen-"))
    try:
        print(f"\nrendering canonical track (22 050 samples @ 44 100 Hz) in {scratch}")
        default_pcm = render(default_gen, scratch / "default_legacy.pcm")
        nofpu_pcm = render(nofpu_gen, scratch / "nofpu_legacy.pcm")
        integer_pcm = render(integer_gen, scratch / "integer_legacy.pcm")

        float_reference = FLOAT_FIXTURE.read_bytes()
        integer_reference = (nofpu_pcm if args.write
                             else (INTEGER_FIXTURE.read_bytes()
                                   if INTEGER_FIXTURE.is_file() else None))

        print("\nsha256")
        for name, data in (
            ("default_legacy (legacy, host FPU)", default_pcm),
            ("nofpu_legacy (legacy, PR32_APU_HAS_FPU=0)", nofpu_pcm),
            ("integer_legacy (legacy, FPU + LEGACY_FLOAT=0)", integer_pcm),
            ("float fixture (historical reference)", float_reference),
            (f"integer fixture ({'written' if args.write else 'committed'})",
             integer_reference),
        ):
            value = "missing" if data is None else sha256(data)
            print(f"  {value:<64}  {name}")

        ok = True

        # 1. Gate: the tool must reproduce the frozen float reference. If it
        # cannot, no integer comparison below means anything.
        print("\ncheck 1 -- the default build reproduces the historical float fixture")
        if default_pcm == float_reference:
            check("byte-identical to apu_mirror_canonical_s16le_44100.pcm", True)
        else:
            differing, first, first_sample, delta = diff_stats(default_pcm,
                                                               float_reference)
            check("byte-identical to apu_mirror_canonical_s16le_44100.pcm", False,
                  f" -- {differing} bytes differ, first at byte {first} "
                  f"(sample {first_sample}), max |delta| {delta} LSB")
            print("  the tool cannot reproduce the frozen reference; stopping.")
            return 1

        # 2. The integer core is build-independent (S2/S9).
        print("\ncheck 2 -- the two integer routes agree byte for byte")
        if nofpu_pcm == integer_pcm:
            check("PR32_APU_HAS_FPU=0 == PR32_APU_HAS_FPU=1 + LEGACY_FLOAT=0", True)
        else:
            differing, first, first_sample, delta = diff_stats(nofpu_pcm, integer_pcm)
            ok = check("PR32_APU_HAS_FPU=0 == PR32_APU_HAS_FPU=1 + LEGACY_FLOAT=0",
                       False,
                       f" -- {differing} bytes differ, first at byte {first} "
                       f"(sample {first_sample}), max |delta| {delta} LSB")
            print("  a build dependency survived; fix the core, not the fixture.")

        # 3. The committed fixture describes that render.
        print("\ncheck 3 -- the integer fixture matches the integer render")
        if args.write:
            INTEGER_FIXTURE.parent.mkdir(parents=True, exist_ok=True)
            INTEGER_FIXTURE.write_bytes(nofpu_pcm)
            check(f"wrote {INTEGER_FIXTURE.relative_to(APU)} "
                  f"({len(nofpu_pcm)} bytes)", True)
        elif integer_reference is None:
            ok = check(f"{INTEGER_FIXTURE.relative_to(APU)} exists", False,
                       " -- run with --write to generate it")
        elif integer_pcm == integer_reference:
            check("byte-identical to apu_integer_core_canonical_s16le_44100.pcm", True)
        else:
            differing, first, first_sample, delta = diff_stats(integer_pcm,
                                                               integer_reference)
            ok = check("byte-identical to apu_integer_core_canonical_s16le_44100.pcm",
                       False,
                       f" -- {differing} bytes differ, first at byte {first} "
                       f"(sample {first_sample}), max |delta| {delta} LSB")

        print("\nOK" if ok else "\nFAIL")
        return 0 if ok else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
