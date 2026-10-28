#!/usr/bin/env python3
"""S11 -- host baseline for the two render cores, one CSV and one table.

Compiles tools/apu_bench/main.cpp together with src/ApuCore.cpp straight from
the repo (no CMake, like tools/loudness_probe) and runs the measured matrix:
`{Legacy, Nes} x {22050, 32000} x {1, 8 voices}`. `Legacy` is the float body and
`Nes` the integer body of the SAME binary, selected only through
`setSoundProfile` -- the benchmark measures the dispatch a game goes through, not
an internal function it reached for.

Why `-Os` by default: it is what the firmware builds use, so the host baseline
is comparable with the target one (tools/apu_bench_firmware, T8) instead of
being a differently optimised story. Override with `--opt` when checking whether
a difference is optimisation-driven.

    python tools/apu_bench/run.py
    python tools/apu_bench/run.py --opt -O2

The binary speaks two channels on purpose: CSV on stdout (so
`apu_bench > baseline.csv` stays machine-readable) and the human table plus the
sizeof block on stderr. This script re-prints the CSV verbatim, then a Markdown
table ready to paste into odd/tasks/apu-sound-profiles.md, then the sizeof
lines. It never writes inside the repo: the build goes to the OS temp directory
and is removed on the way out.

These are HOST figures. They say nothing about an ESP32; the target numbers are
measured on the board by T8 and recorded in docs/ABI.md §9.
"""

import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
APU = HERE.parent.parent

# The block size the binary renders. Duplicated here only to rebuild the
# real-time budget when printing the table; main.cpp is the owner of the value.
BLOCK_SAMPLES = 512


def find_compiler():
    """First C++17 compiler on PATH, the same lookup loudness_probe uses."""
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("error: no C++ compiler on PATH (tried g++, clang++, c++)")


def compiler_identity(cxx):
    """One-line version string, so a recorded table can be attributed."""
    proc = subprocess.run([cxx, "--version"], capture_output=True, text=True)
    first = proc.stdout.splitlines()[0] if proc.stdout else cxx
    return first.strip()


def build(cxx, opt, outdir):
    """Compiles the tool and the core into `outdir`, never into the repo."""
    exe = outdir / ("apu_bench.exe" if os.name == "nt" else "apu_bench")
    cmd = [cxx, "-std=c++17", opt,
           f"-I{APU / 'include'}",
           str(HERE / "main.cpp"),
           str(APU / "src" / "ApuCore.cpp"),
           "-o", str(exe)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"build failed:\n  {' '.join(cmd)}\n{proc.stdout}{proc.stderr}")
    return exe


def run(exe):
    """Runs one measurement pass and returns (csv rows, sizeof lines)."""
    proc = subprocess.run([str(exe)], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"run failed (exit {proc.returncode}):\n"
                 f"{proc.stdout}{proc.stderr}")

    lines = proc.stdout.strip().splitlines()
    if not lines:
        sys.exit("error: the binary produced no CSV")

    header = lines[0].split(",")
    rows = []
    for line in lines[1:]:
        values = line.split(",")
        rows.append(dict(zip(header, values)))
    if len(rows) != 8:
        sys.exit(f"error: expected 8 cells, got {len(rows)}")

    # The sizeof block is everything after its heading; re-printing it from the
    # binary keeps one source for the figures (no re-derivation here).
    sizeof_lines = []
    capture = False
    for line in proc.stderr.splitlines():
        if line.startswith("sizeof (host ABI"):
            capture = True
            continue
        if capture and line.strip():
            sizeof_lines.append(line.rstrip())
    return header, rows, sizeof_lines


def markdown_table(rows):
    """Markdown table of the measured cells, ready to paste into the plan."""
    out = [
        "| Perfil | Tasa (Hz) | Voces | p50 (µs) | p99 (µs) | µs/muestra "
        "| Presupuesto (µs) | Headroom |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        rate = int(row["rate"])
        # Same formula the binary uses, so the table and the CSV cannot drift:
        # the audio time the 512 samples of the block would last on the wire.
        budget = BLOCK_SAMPLES * 1e6 / rate
        headroom = budget / float(row["p99_us"])
        out.append(f"| {row['profile']} | {rate} | {row['voices']} "
                   f"| {row['p50_us']} | {row['p99_us']} "
                   f"| {row['us_per_sample']} | {budget:.3f} | {headroom:.1f}x |")
    return out


def normalize_argv(argv):
    """Joins `--opt -O2` into `--opt=-O2`.

    argparse refuses a value that starts with `-` because it looks like a flag,
    and `--opt -O2` is the natural spelling of this option. Folding it into the
    `=` form keeps the documented command working instead of making the user
    discover the workaround.
    """
    out = []
    index = 0
    while index < len(argv):
        if argv[index] == "--opt" and index + 1 < len(argv):
            out.append(f"--opt={argv[index + 1]}")
            index += 2
            continue
        out.append(argv[index])
        index += 1
    return out


def main():
    if hasattr(sys.stdout, "reconfigure"):
        # The table uses µ and ×; do not let a cp1252 console decide this.
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser()
    ap.add_argument("--opt", default="-Os",
                    help="optimisation flag for both translation units "
                         "(default: -Os, the firmware build's level)")
    args = ap.parse_args(normalize_argv(sys.argv[1:]))

    cxx = find_compiler()

    scratch = pathlib.Path(tempfile.mkdtemp(prefix="pr32-apu-bench-"))
    try:
        print(f"compiler : {compiler_identity(cxx)}")
        print(f"host     : {platform.platform()}")
        print(f"flags    : -std=c++17 {args.opt} -I include")
        print(f"build dir: {scratch} (removed on exit)")

        exe = build(cxx, args.opt, scratch)
        header, rows, sizeof_lines = run(exe)

        print("\nCSV (stdout del binario)")
        print(",".join(header))
        for row in rows:
            print(",".join(row[column] for column in header))

        print("\nTabla Markdown (lista para el documento del plan)")
        for line in markdown_table(rows):
            print(line)

        print("\nsizeof (stderr del binario)")
        for line in sizeof_lines:
            print(line)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
