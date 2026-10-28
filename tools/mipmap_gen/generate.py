#!/usr/bin/env python3
"""S4 -- generate and verify the band-limited SAW mipmap tables.

A 256-point table read by a Q32 phase accumulator can only be alias-free while
the harmonic series of the note still fits under Nyquist. The cutoff therefore
depends on `q = f / fs` and NOT on absolute Hz: one table set serves 8 000,
22 050, 44 100 and 48 000 Hz because `f / fs` -- not `f` -- is what decides how
much of the series is representable. Levels are one octave of `q` each:

    L = 0  q in [2^-9, 2^-8)   L = 7  q in [2^-2, 2^-1)

and each level carries the harmonic count its band's TOP (the worst case)
allows, `H(L) = floor(0.5 / 2^-(8-L))`, capped at 128 because a 256-point table
cannot carry more:

    H = 128, 64, 32, 16, 8, 4, 2, 1

Each table is the additive saw `sum (-1)^(n+1) sin(2*pi*n*t) / n` sampled at 256
points, normalised by the ACTUAL peak of the sampled table -- the truncated
series overshoots (Gibbs), so normalising by the ideal amplitude would clip --
rounded to Q15 and clamped to [-32767, 32767].

    python tools/mipmap_gen/generate.py --write   # regenerate the header
    python tools/mipmap_gen/generate.py --check   # verify the committed one

`--check` regenerates in memory and fails with a diff summary if the committed
header drifted. Both modes print the level table (band, harmonics, peak before
and after normalisation) so the numbers in the docs can be traced to a run.

The output is deterministic by construction: no timestamps, no locale, no
dict/hash ordering, explicit rounding and fixed-width integer formatting, LF
line endings and pure-ASCII bytes. Python 3 standard library only.
"""

import argparse
import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
APU = HERE.parent.parent
HEADER = APU / "include" / "pixelroot32" / "apu" / "AudioOscMipLUT.h"

LEVEL_COUNT = 8
TABLE_SIZE = 256
MAX_HARMONICS = 128
Q15_MAX = 32767
Q15_MIN = -32767
VALUES_PER_ROW = 16
REGENERATE = "python tools/mipmap_gen/generate.py --write"


def harmonics_for_level(level):
    """Harmonics the band's top edge allows, capped by the table size.

    The top of level L is `2^-(8-L)` cycles per sample, so the series stays
    below Nyquist while `H * q_top <= 0.5`.
    """
    return min(MAX_HARMONICS, 1 << (7 - level))


def band_for_level(level):
    """[q_min, q_max) covered by one level: one octave of `f / fs`."""
    return 2.0 ** -(9 - level), 2.0 ** -(8 - level)


def round_half_up(value):
    """Deterministic rounding (banker's rounding in `round()` is not stated)."""
    return int(math.floor(value + 0.5))


def band_limited_saw(t, harmonics):
    """The additive saw at phase `t` in [0, 1), truncated to `harmonics`."""
    total = 0.0
    for n in range(1, harmonics + 1):
        sign = 1.0 if (n & 1) else -1.0
        total += sign * math.sin(2.0 * math.pi * n * t) / n
    return total


def build_table(level):
    """Return (samples, raw_peak, clipped) for one level, in Q15.

    `raw_peak` is the largest magnitude of the sampled sum before scaling: the
    Gibbs overshoot the normalisation divides out. `clipped` counts samples the
    final clamp had to move, which must stay 0: the normalisation targets
    32 767 exactly, so a non-zero count means the scale itself is wrong, not
    that the rail was needed.
    """
    harmonics = harmonics_for_level(level)
    raw = [band_limited_saw(i / TABLE_SIZE, harmonics) for i in range(TABLE_SIZE)]
    raw_peak = max(abs(v) for v in raw)
    scale = Q15_MAX / raw_peak
    samples = []
    clipped = 0
    for value in raw:
        q15 = round_half_up(value * scale)
        if q15 > Q15_MAX or q15 < Q15_MIN:
            clipped += 1
        samples.append(max(Q15_MIN, min(Q15_MAX, q15)))
    return samples, raw_peak, clipped


def build_tables():
    """All levels, in level order: [level][sample]."""
    tables = []
    peaks = []
    clipped = []
    for level in range(LEVEL_COUNT):
        samples, raw_peak, rail_hits = build_table(level)
        tables.append(samples)
        peaks.append(raw_peak)
        clipped.append(rail_hits)
    return tables, peaks, clipped


def header_text(tables):
    """The committed header, byte for byte."""
    lines = []
    lines.append("/*")
    lines.append(" * Copyright (c) 2026 PixelRoot32")
    lines.append(" * Licensed under the MIT License")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace pixelroot32::audio {")
    lines.append("")
    lines.append("/**")
    lines.append(" * @file AudioOscMipLUT.h")
    lines.append(" * @brief Band-limited SAW mipmap: one 256-point Q15 table per octave of")
    lines.append(" * `q = f / fs`.")
    lines.append(" *")
    lines.append(" * A table read by a Q32 phase accumulator is alias-free only while the")
    lines.append(" * note's harmonic series still fits under Nyquist, and \"how much fits\" is")
    lines.append(" * decided by `f / fs`, never by absolute Hz. Keying the levels on `q` is")
    lines.append(" * what lets one committed set serve 8 000, 22 050, 44 100 and 48 000 Hz")
    lines.append(" * without a second copy.")
    lines.append(" *")
    lines.append(" * Level `L` covers `q` in `[2^-(9-L), 2^-(8-L))`: level 0 is")
    lines.append(" * `[2^-9, 2^-8)`, level 7 is `[2^-2, 2^-1)`. A level's harmonic count comes")
    lines.append(" * from the TOP of its band, which is the worst case:")
    lines.append(" *")
    lines.append(" *     L        0    1    2   3   4   5   6   7")
    lines.append(" *     H(L)   128   64   32  16   8   4   2   1")
    lines.append(" *")
    lines.append(" * 128 is the cap: a 256-point table cannot carry more (harmonic 128 lands")
    lines.append(" * exactly on Nyquist and samples to zero, so level 0 is a 127-harmonic")
    lines.append(" * table in practice).")
    lines.append(" *")
    lines.append(" * Each table is the additive saw normalised by the ACTUAL peak of the")
    lines.append(" * sampled series, because a truncated series overshoots (Gibbs) and")
    lines.append(" * normalising by the ideal amplitude would clip.")
    lines.append(" *")
    lines.append(" * GENERATED FILE -- do not edit by hand. Regenerate with:")
    lines.append(" *")
    lines.append(" *     " + REGENERATE)
    lines.append(" *")
    lines.append(" * The tool prints the band, harmonic count and peak of every level, and")
    lines.append(" * `--check` fails if this file and the generator disagree.")
    lines.append(" */")
    lines.append("")
    lines.append("/** @brief Number of mipmap levels (one octave of `q = f / fs` each). */")
    lines.append("inline constexpr uint8_t SAW_MIP_LEVEL_COUNT = %d;" % LEVEL_COUNT)
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Harmonic count per level, set by the top of the level's band.")
    lines.append(" *")
    lines.append(" * `SAW_MIP_HARMONICS[L]` is the number of harmonics the level's table")
    lines.append(" * carries: the band's worst case, not its centre.")
    lines.append(" */")
    lines.append(
        "inline constexpr uint8_t SAW_MIP_HARMONICS[SAW_MIP_LEVEL_COUNT] = {"
    )
    lines.append("    " + ", ".join(str(harmonics_for_level(l))
                                    for l in range(LEVEL_COUNT)))
    lines.append("};")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief One period of the band-limited saw in Q15, one row per level.")
    lines.append(" *")
    lines.append(" * Read as `SAW_MIP_Q15[level][(phaseQ32 >> 24) & 255]`. Level `L` is only")
    lines.append(" * alias-free while `f / fs` is inside that level's band; picking the level")
    lines.append(" * from `f` alone would break at any sample rate but 44 100 Hz.")
    lines.append(" */")
    lines.append("inline constexpr int16_t SAW_MIP_Q15[SAW_MIP_LEVEL_COUNT][256] = {")
    for level, samples in enumerate(tables):
        lines.append("    {")
        for start in range(0, TABLE_SIZE, VALUES_PER_ROW):
            chunk = samples[start:start + VALUES_PER_ROW]
            lines.append("        " + ", ".join("%7d" % v for v in chunk) + ",")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace pixelroot32::audio")
    lines.append("")
    return "\n".join(lines)


def level_report(tables, peaks, clipped):
    """The per-level numbers, printed by every run."""
    print("level table (q = f / fs; H from the band top, capped at %d)"
          % MAX_HARMONICS)
    print("  L  q_min       q_max       H     peak raw   peak Q15   clipped")
    for level in range(LEVEL_COUNT):
        q_min, q_max = band_for_level(level)
        after = max(abs(v) for v in tables[level])
        print("  %d  %.9f   %.9f   %4d   %.6f   %8d   %d"
              % (level, q_min, q_max, harmonics_for_level(level),
                 peaks[level], after, clipped[level]))


def table_bytes():
    """Flash held by the table data itself (the counts are separate)."""
    return LEVEL_COUNT * TABLE_SIZE * 2


def first_difference(actual, expected):
    """(line number, actual line, expected line) of the first drift, 1-based."""
    actual_lines = actual.splitlines()
    expected_lines = expected.splitlines()
    for index in range(min(len(actual_lines), len(expected_lines))):
        if actual_lines[index] != expected_lines[index]:
            return index + 1, actual_lines[index], expected_lines[index]
    if len(actual_lines) != len(expected_lines):
        return (min(len(actual_lines), len(expected_lines)) + 1,
                "<%d lines>" % len(actual_lines),
                "<%d lines>" % len(expected_lines))
    return None


def check(label, condition, detail=""):
    print("  [%s] %s%s" % ("ok" if condition else "FAIL", label, detail))
    return condition


def main():
    parser = argparse.ArgumentParser(
        description="generate or verify include/pixelroot32/apu/AudioOscMipLUT.h")
    parser.add_argument("--write", action="store_true",
                        help="rewrite the committed header")
    parser.add_argument("--check", action="store_true",
                        help="verify the committed header (default)")
    args = parser.parse_args()
    if args.write and args.check:
        sys.exit("error: --write and --check are mutually exclusive")

    tables, peaks, clipped = build_tables()
    level_report(tables, peaks, clipped)
    generated = header_text(tables)

    print("\ntable data: %d levels x %d points x 2 B = %d B"
          % (LEVEL_COUNT, TABLE_SIZE, table_bytes()))

    print("\ncheck -- the normalisation did not need the final clamp")
    for level in range(LEVEL_COUNT):
        if clipped[level] != 0:
            check("level %d needed no clamp" % level, False,
                  " -- %d samples moved; the scale is wrong" % clipped[level])
            print("\nFAIL")
            return 1
    check("%d levels normalised inside [-%d, %d] without clipping"
          % (LEVEL_COUNT, Q15_MAX, Q15_MAX), True)
    committed = (HEADER.read_bytes().decode("ascii")
                 if HEADER.is_file() else None)

    if args.write:
        HEADER.parent.mkdir(parents=True, exist_ok=True)
        HEADER.write_bytes(generated.encode("ascii"))
        print("\ncheck -- the committed header describes the generated tables")
        check("wrote %s (%d B)" % (HEADER.relative_to(APU).as_posix(),
                                   len(generated)), True)
        print("\nOK")
        return 0

    print("\ncheck -- the committed header describes the generated tables")
    if committed is None:
        check("%s exists" % HEADER.relative_to(APU).as_posix(), False,
              " -- run --write to generate it")
        print("\nFAIL")
        return 1
    if committed == generated:
        check("byte-identical to the generated header "
              "(%d B, sha-free: compared as text)" % len(generated), True)
        print("\nOK")
        return 0
    difference = first_difference(committed, generated)
    line, actual_line, expected_line = difference
    check("byte-identical to the generated header", False,
          " -- %d committed bytes vs %d generated"
          % (len(committed), len(generated)))
    print("  first difference at line %d" % line)
    print("    committed: %s" % actual_line)
    print("    generated: %s" % expected_line)
    print("  the header drifted; run `%s`, never edit it by hand." % REGENERATE)
    print("\nFAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
