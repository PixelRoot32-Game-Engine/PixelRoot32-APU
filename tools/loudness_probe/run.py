#!/usr/bin/env python3
"""Phase 1 of the Hito 5 loudness calibration: float vs Q15, side by side.

Builds loudness_probe.cpp twice from the same source -- once normally (float
path) and once with -DPR32_APU_HAS_FPU=0 (the integer path an ESP32-C3 runs) --
renders the same music-demo track through both, and compares the levels.

After Hito 5 (M12) the two paths should render at the SAME level. That parity
is the milestone's contract, so the ratios below are the actual test: peak and
RMS ratios near 1.00, and no samples pinned at the rails.

    python tools/loudness_probe/run.py
    python tools/loudness_probe/run.py --track blaster_ridge --volume 0.8
    python tools/loudness_probe/run.py --wav-dir out/   # for the listening A/B

Needs a C++17 compiler on PATH (g++ or clang++) and a checkout of
PixelRoot32-Game-Engine as a sibling of this repo, for the demo assets.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
APU = HERE.parent.parent
WORKSPACE = APU.parent
ENGINE = (WORKSPACE / "PixelRoot32-Game-Samples" / "lib" / "PixelRoot32-Game-Engine")
ASSETS = ENGINE / "examples" / "music-demo" / "src"

VARIANTS = [("float", []), ("q15", ["-DPR32_APU_HAS_FPU=0"])]


def find_compiler():
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("error: no C++ compiler on PATH (tried g++, clang++, c++)")


def build(cxx, outdir, label, extra_defs):
    exe = outdir / f"loudness_probe_{label}.exe"
    cmd = [cxx, "-std=c++17", "-O2", "-DNOMINMAX",
           f"-I{APU / 'include'}",
           f"-I{ENGINE / 'include'}",   # the <audio/...> re-export shims
           f"-I{ASSETS}",               # assets/<track>.h
           *extra_defs,
           str(HERE / "loudness_probe.cpp"),
           str(APU / "src" / "ApuCore.cpp"),
           "-o", str(exe)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"build failed ({label}):\n{proc.stdout}\n{proc.stderr}")
    return exe


def run(exe, args):
    proc = subprocess.run([str(exe), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"run failed ({exe.name}):\n{proc.stdout}\n{proc.stderr}")
    return dict(re.findall(r"(\w+)=(\S+)", proc.stdout.strip()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--track", default="moonwell_hymn")
    ap.add_argument("--rate", default="44100")
    ap.add_argument("--seconds", default="20")
    ap.add_argument("--volume", default="1.0",
                    help="master volume 0..1 -- the knob being calibrated")
    ap.add_argument("--wav-dir", default=None,
                    help="also write <track>_float.wav / <track>_q15.wav here")
    ap.add_argument("--outdir", default=None, help="where to put the binaries")
    args = ap.parse_args()

    if not ASSETS.is_dir():
        sys.exit(f"error: music-demo assets not found at {ASSETS}\n"
                 f"       expected PixelRoot32-Game-Engine as a sibling checkout")

    outdir = pathlib.Path(args.outdir) if args.outdir else (HERE / "build")
    outdir.mkdir(parents=True, exist_ok=True)
    wavdir = pathlib.Path(args.wav_dir) if args.wav_dir else None
    if wavdir:
        wavdir.mkdir(parents=True, exist_ok=True)

    cxx = find_compiler()
    probe_args = ["--track", args.track, "--rate", args.rate,
                  "--seconds", args.seconds, "--volume", args.volume]

    results = {}
    for label, defs in VARIANTS:
        exe = build(cxx, outdir, label, defs)
        extra = ["--wav", str(wavdir / f"{args.track}_{label}.wav")] if wavdir else []
        results[label] = run(exe, probe_args + extra)

    f, q = results["float"], results["q15"]
    print(f"\ntrack={args.track}  rate={args.rate}Hz  "
          f"seconds={args.seconds}  volume={args.volume}\n")
    print(f"{'':<14}{'float (ESP32/S3)':>20}{'q15 (ESP32-C3)':>20}{'ratio':>10}")
    print("-" * 64)

    peak_ratio = int(q["peak"]) / int(f["peak"]) if int(f["peak"]) else float("nan")
    rms_ratio = float(q["rms"]) / float(f["rms"]) if float(f["rms"]) else float("nan")

    print(f"{'peak':<14}{f['peak']:>20}{q['peak']:>20}{peak_ratio:>10.3f}")
    print(f"{'peak dBFS':<14}{f['peak_dbfs']:>20}{q['peak_dbfs']:>20}{'':>10}")
    print(f"{'rms':<14}{f['rms']:>20}{q['rms']:>20}{rms_ratio:>10.3f}")
    print(f"{'rms dBFS':<14}{f['rms_dbfs']:>20}{q['rms_dbfs']:>20}{'':>10}")
    print(f"{'rail samples':<14}{f['rails']:>20}{q['rails']:>20}{'':>10}")
    print(f"{'rails ppm':<14}{f['rails_ppm']:>20}{q['rails_ppm']:>20}{'':>10}")

    print("\nverdict")
    ok = True

    # RMS is the loudness metric, and it is what M12 had to fix: before the
    # milestone this ratio was ~0.225.
    #
    # The tolerance is 15% rather than something tight because the two paths
    # are NOT bit-identical and never were. The float path evaluates the
    # compressor curve continuously; the Q15 path looks it up in
    # audio_mixer_lut, 1025 entries indexed by (sum + 131072) >> 8 with no
    # interpolation -- about 102 output counts per step near zero. On real
    # multi-voice material that quantisation shows up as a few percent either
    # way, and the recursive DC blocker carries the difference forward.
    #
    # What distinguishes quantisation from a gain bug is DIRECTION: measured
    # across the four demo tracks the ratio lands both above and below 1.0
    # (0.990 to 1.094). A gain error would be one-directional and constant.
    # The exact gain staging is pinned separately and bit-exactly by
    # test_q15_softclip_curve_shapes_match_the_float_path.
    if abs(rms_ratio - 1.0) <= 0.15:
        print(f"  [ok]   loudness parity holds -- rms x{rms_ratio:.3f}")
        print(f"         (peak x{peak_ratio:.3f}; peak is a single-sample statistic and")
        print(f"         runs a few percent high on q15 from LUT quantisation --")
        print(f"         informational, not a level check)")
    else:
        ok = False
        print(f"  [FAIL] loudness mismatch: rms x{rms_ratio:.3f} (peak x{peak_ratio:.3f}).")
        print("         Beyond what LUT quantisation explains. If this is near 0.225,")
        print("         the build predates M12. Investigate before touching volume.")

    for label, r in (("float", f), ("q15", q)):
        rails = int(r["rails"])
        if rails:
            ok = False
            print(f"  [FAIL] {label}: {rails} samples pinned at the rails "
                  f"({r['rails_ppm']} ppm) -- clipping. Lower --volume and re-run")
            print(f"         until this reaches 0, then use that as the new master.")
    if not any(int(r["rails"]) for r in (f, q)):
        headroom = -float(q["peak_dbfs"])
        print(f"  [ok]   no clipping; {headroom:.1f} dB of headroom left on q15")

    if wavdir:
        print(f"\nwavs in {wavdir} -- phase 2 is listening to these back to back.")
        print("they should sound identical in level.")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
