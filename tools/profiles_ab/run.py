#!/usr/bin/env python3
"""S9.4 -- blind A/B material: the same track through the three profiles.

Renders one excerpt of the same music under `Nes`, `Hybrid` and `Legacy`, then
writes three 16-bit mono 44 100 Hz WAV files whose RMS is identical. The files
are named `a.wav`, `b.wav`, `c.wav`; the mapping lives in a separate `key.txt`,
so a listener can play the three files without knowing which profile each one
is. That is the whole point of the exercise: the verdict is the person's, and it
is only worth something if the labels are out of sight while listening.

How the matching works:

  1. `render.cpp` renders the track with ONE profile to raw S16LE (byte-exact,
     no header, no post-processing).
  2. This script measures the RMS of each render and picks the QUIETEST as the
     common target. Matching downwards means no file is ever amplified, so the
     matching itself cannot introduce a clipped sample or a limiter.
  3. Every sample is scaled by `target / rms` and rounded half away from zero.
     Rounding leaves a residual of a few hundredths of a dB; the achieved RMS is
     measured again and printed, because "matched" has to be a figure.

    python tools/profiles_ab/run.py
    python tools/profiles_ab/run.py --out D:\\listening

It needs a C++17 compiler on the PATH (`g++`, `clang++` or `c++`). There is no
CMake, like `tools/apu_bench`: the tool and the core are compiled straight from
the repo into a temporary directory that is removed on exit. The only thing this
script leaves behind is the output directory with the three WAVs and the key,
because those are the deliverable and not scratch space.
"""

import argparse
import math
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
APU = HERE.parent.parent

SAMPLE_RATE = 44100
# The excerpt render.cpp produces: 12 beats at 120 BPM plus a 0.5 s tail.
BODY_SAMPLES = 12 * SAMPLE_RATE * 60 // 120
TAIL_SAMPLES = SAMPLE_RATE // 2
EXPECTED_SAMPLES = BODY_SAMPLES + TAIL_SAMPLES

# Blinded order. Deliberately not the enumerator order, and never printed next
# to the audio: the listener reads key.txt only after answering.
FILE_FOR_PROFILE = {"hybrid": "a.wav", "legacy": "b.wav", "nes": "c.wav"}
PROFILES = ("hybrid", "legacy", "nes")


def find_compiler():
    """First C++17 compiler on PATH, the same lookup apu_bench uses."""
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("error: no C++ compiler on PATH (tried g++, clang++, c++)")


def compiler_identity(cxx):
    proc = subprocess.run([cxx, "--version"], capture_output=True, text=True)
    first = proc.stdout.splitlines()[0] if proc.stdout else cxx
    return first.strip()


def build(cxx, outdir):
    """Compiles the renderer and the core into `outdir`, never into the repo."""
    exe = outdir / ("apu_profiles_ab.exe" if os.name == "nt" else "apu_profiles_ab")
    cmd = [cxx, "-std=c++17", "-O2",
           f"-I{APU / 'include'}",
           str(HERE / "render.cpp"),
           str(APU / "src" / "ApuCore.cpp"),
           "-o", str(exe)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"build failed:\n  {' '.join(cmd)}\n{proc.stdout}{proc.stderr}")
    return exe


def render(exe, profile, path):
    proc = subprocess.run([str(exe), "--profile", profile, "--out", str(path)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"render failed for {profile} (exit {proc.returncode}):\n"
                 f"{proc.stdout}{proc.stderr}")
    line = proc.stdout.strip()
    print(f"  {profile:<7} {line}")
    samples = read_s16le(path)
    if len(samples) != EXPECTED_SAMPLES:
        sys.exit(f"error: {profile} rendered {len(samples)} samples, "
                 f"expected {EXPECTED_SAMPLES}")
    return samples


def read_s16le(path):
    data = path.read_bytes()
    if len(data) % 2:
        sys.exit(f"error: {path.name} is not a whole number of S16 samples")
    return [int.from_bytes(data[i:i + 2], "little", signed=True)
            for i in range(0, len(data), 2)]


def rms(samples):
    if not samples:
        return 0.0
    total = sum(float(s) * float(s) for s in samples)
    return math.sqrt(total / len(samples))


def peak(samples):
    return max((abs(int(s)) for s in samples), default=0)


def dbfs(value):
    """Full scale is 32768 for a signed 16-bit sample."""
    if value <= 0.0:
        return float("-inf")
    return 20.0 * math.log10(value / 32768.0)


def match(samples, gain):
    """Applies the gain, rounding half away from zero and clamping to S16.

    The clamp is unreachable by construction (the target is the quietest file,
    so every gain is <= 1), and it is kept anyway: a silent truncation would be
    a clipped A/B file nobody could attribute to the profile.
    """
    out = []
    for value in samples:
        scaled = float(value) * gain
        rounded = int(math.floor(scaled + 0.5)) if scaled >= 0.0 \
            else int(math.ceil(scaled - 0.5))
        if rounded > 32767:
            rounded = 32767
        elif rounded < -32768:
            rounded = -32768
        out.append(rounded)
    return out


def write_wav(path, samples, sample_rate=SAMPLE_RATE):
    """Writes a canonical 44-byte-header PCM WAV: mono, 16-bit, little-endian."""
    data = bytearray()
    for value in samples:
        data += int(value).to_bytes(2, "little", signed=True)

    block_align = 2
    byte_rate = sample_rate * block_align
    header = bytearray()
    header += b"RIFF"
    header += (36 + len(data)).to_bytes(4, "little")
    header += b"WAVE"
    header += b"fmt "
    header += (16).to_bytes(4, "little")
    header += (1).to_bytes(2, "little")          # PCM
    header += (1).to_bytes(2, "little")          # mono
    header += sample_rate.to_bytes(4, "little")
    header += byte_rate.to_bytes(4, "little")
    header += block_align.to_bytes(2, "little")
    header += (16).to_bytes(2, "little")         # bits per sample
    header += b"data"
    header += len(data).to_bytes(4, "little")
    path.write_bytes(bytes(header + data))


def main():
    if hasattr(sys.stdout, "reconfigure"):
        # The table uses dBFS and a middle dot; do not let a cp1252 console
        # decide this.
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None,
                    help="directory for a.wav/b.wav/c.wav plus key.txt "
                         "(default: pr32-apu-ab inside the OS temp directory)")
    args = ap.parse_args()

    outdir = pathlib.Path(args.out) if args.out else \
        pathlib.Path(tempfile.gettempdir()) / "pr32-apu-ab"
    outdir.mkdir(parents=True, exist_ok=True)

    cxx = find_compiler()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="pr32-apu-ab-build-"))
    try:
        print(f"compiler : {compiler_identity(cxx)}")
        print(f"host     : {platform.platform()}")
        print(f"build dir: {scratch} (removed on exit)")
        print(f"out dir  : {outdir}")

        exe = build(cxx, scratch)

        print(f"\nrendering {EXPECTED_SAMPLES} samples "
              f"({EXPECTED_SAMPLES / SAMPLE_RATE:.3f} s @ {SAMPLE_RATE} Hz)"
              " per profile")
        renders = {profile: render(exe, profile, scratch / f"{profile}.pcm")
                   for profile in PROFILES}
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    before = {profile: rms(samples) for profile, samples in renders.items()}
    peaks_before = {profile: peak(samples) for profile, samples in renders.items()}

    # Match downwards to the quietest render: no gain above 1, so the matching
    # cannot clip and the peak of every file is at or below its own render.
    target = min(before.values())
    if target <= 0.0:
        sys.exit("error: a profile rendered digital silence, nothing to match")

    print("\nRMS before matching")
    for profile in PROFILES:
        print(f"  {profile:<7} rms {before[profile]:8.2f} "
              f"({dbfs(before[profile]):+6.2f} dBFS)  peak {peaks_before[profile]:5d}")

    print(f"\nmatching every file to rms {target:.2f} "
          f"({dbfs(target):+.2f} dBFS), the quietest render")

    after = {}
    for profile in PROFILES:
        gain = target / before[profile]
        matched = match(renders[profile], gain)
        after[profile] = rms(matched)
        path = outdir / FILE_FOR_PROFILE[profile]
        write_wav(path, matched)
        print(f"  {profile:<7} gain {gain:6.3f} "
              f"({20.0 * math.log10(gain):+5.2f} dB) -> {path.name} "
              f"rms {after[profile]:8.2f} ({dbfs(after[profile]):+6.2f} dBFS) "
              f"peak {peak(matched):5d}")

    spread = max(after.values()) - min(after.values())
    print(f"\nRMS spread after matching: {spread:.3f} LSB "
          f"({abs(dbfs(max(after.values())) - dbfs(min(after.values()))):.4f} dB)")

    # The key is written last and printed only here: a listener who runs the
    # script sees the audio files before the answer.
    key_path = outdir / "key.txt"
    lines = ["# S9.4 blind A/B key — read this only after you have answered.",
             "#"]
    for profile in PROFILES:
        lines.append(f"{FILE_FOR_PROFILE[profile]} {profile}")
    lines.append("")
    lines.append(f"sample_rate_hz {SAMPLE_RATE}")
    lines.append(f"samples {EXPECTED_SAMPLES}")
    lines.append(f"rms_target {target:.2f}")
    key_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"\nwrote {len(PROFILES)} WAV files and {key_path}")
    print("listen to a.wav, b.wav and c.wav, then open key.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
