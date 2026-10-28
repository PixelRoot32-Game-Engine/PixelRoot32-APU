# mipmap_gen — band-limited SAW tables (S4)

Generates `include/pixelroot32/apu/AudioOscMipLUT.h`: eight 256-point Q15 tables
of one period of a saw, each carrying only the harmonics that fit under Nyquist
for its own octave of `q = f / fs`.

```bash
python tools/mipmap_gen/generate.py --write   # regenerate the header
python tools/mipmap_gen/generate.py --check   # verify the committed header
```

Python 3 standard library only. `--check` is the default mode; it regenerates
the header in memory and fails with the first differing line if the committed
file drifted, and `--check` never writes.

---

## Why the levels are keyed on `q = f / fs`

A 256-point table read by a Q32 phase accumulator is alias-free only while the
note's harmonic series stays below Nyquist. How much of the series fits is
decided by `f / fs`, not by absolute Hz: a 1 000 Hz saw at 8 000 Hz output has
one usable octave, the same note at 48 000 Hz has five. Keying the levels on
`q` is what lets **one** committed table set serve 8 000, 22 050, 44 100 and
48 000 Hz; keying them on Hz would need a copy per rate, which the 8.5 KB E2
flash budget does not hold.

Level `L` covers `q` in `[2^-(9-L), 2^-(8-L))` — one octave each, tiling without
a gap:

| L | `q` band | H(L) | peak before normalisation | peak in Q15 |
|---|---|---|---|---|
| 0 | [0.001953125, 0.003906250) | 128 | 1.839649 | 32767 |
| 1 | [0.003906250, 0.007812500) | 64 | 1.827329 | 32767 |
| 2 | [0.007812500, 0.015625000) | 32 | 1.802594 | 32767 |
| 3 | [0.015625000, 0.031250000) | 16 | 1.752739 | 32767 |
| 4 | [0.031250000, 0.062500000) | 8 | 1.673787 | 32767 |
| 5 | [0.062500000, 0.125000000) | 4 | 1.526910 | 32767 |
| 6 | [0.125000000, 0.250000000) | 2 | 1.298951 | 32767 |
| 7 | [0.250000000, 0.500000000) | 1 | 1.000000 | 32767 |

The count comes from the **top** of each band — the worst case, not the centre —
because a table that is alias-free at its band's top is alias-free everywhere in
it: `H(L) = floor(0.5 / 2^-(8-L)) = 2^(7-L)`, capped at 128 because a 256-point
table cannot carry more. The cap binds exactly at level 0 (its band top also
asks for 128; one octave lower would ask for 256), which is why the octaves
stop there. Harmonic 128 lands on Nyquist and samples to zero, so level 0 is a
127-harmonic table in practice.

## How each table is built

    saw(t) = sum_{n=1..H} (-1)^(n+1) * sin(2*pi*n*t) / n

sampled at `t = i / 256`, then normalised by the **actual maximum of the
sampled table** — the truncated series overshoots by 5–84 % (see the peak column
above; that is Gibbs), so normalising by the ideal amplitude would clip. Values
are rounded half-up, cast to Q15 and clamped to `[-32767, 32767]`; the run
fails if the clamp ever has to move a sample, because that means the scale is
wrong.

The output is deterministic: no timestamps, no locale, no hash/dict ordering,
explicit rounding, fixed-width integer formatting, LF endings, ASCII bytes. The
same generator run twice produces the same bytes on any machine.

## What it costs

| Object | Bytes |
|---|---|
| `SAW_MIP_Q15[8][256]` int16 | **4 096** (pinned by a `static_assert` in the test) |
| `SAW_MIP_HARMONICS[8]` uint8 | 8 |
| `SAW_MIP_LEVEL_COUNT` uint8 | 1 (usually folded; it is only a bound) |

4 096 B of table data against the 8.5 KB E2 flash budget, which also has to
carry T1's filter chain (~1.5 KB).

## Why SAW only, and not SINE (L5)

The budget does not hold two mipmap sets (8 KB for SINE + SAW before T1's
1.5 KB), and SINE has nothing to gain: it is a single harmonic, so it cannot
alias. `test/test_apu_wavetable_spectral.cpp` measures SINE's fold-down with the
same metric and reports it rather than asserting a spec on it; its worst figure
across the sweep is the 8-bit phase-index floor, and at every swept frequency it
is already well below the ideal ramp's. Mipmapping SINE would spend 4 KB to fix
an artefact that is not aliasing.

## Reading the table (this matters for T3)

`SAW_MIP_Q15[level][(phaseQ32 >> 24) & 255]`, selected by
`level = levelOf(q)` from the band above, is the same read the existing
`SINE_LUT_Q15` uses. It is measured to be **worse than it looks**: truncating
the phase to 8 bits puts a ~-43 dBFS fold-down floor under a table this steep,
and the ideal ramp it replaces is already at or below that below a few hundred
Hz (the boundary moves with the rate), so the S5 "10 dB better" target cannot be
met there by any 256-point read that truncates. Linear interpolation between the
two adjacent points
(`a + ((b - a) * fracQ16 + 32768) >> 16`) keeps the same 4 KB of flash, costs a
multiply and a shift per sample, and clears S5 at every swept frequency with
13.7 dB to spare at the worst point. The spectral test measures both reads so
the trade is recorded where it was made.

## Relation to the tests

`test/test_apu_wavetable_spectral.cpp` (5 cases) is the proof:

1. the level layout, the harmonic counts and their derivation from the band top;
2. a 256-point DFT per level: every bin above `H(L)` at or below -60 dBFS
   relative to the fundamental, the fundamental the strongest bin, harmonics
   following 1/n;
3. continuity: zero crossing at phase 0 and a largest adjacent step inside the
   bound `2*pi*H/256 * A1`, wrap included;
4. the S5 sweep: fold-down of the mipmapped synthesis against the ideal ramp of
   `generateSawSampleQ15`, 16 log-spaced frequencies across 55 Hz–8 kHz at
   22 050 and 44 100 Hz;
5. the SINE control.

Nothing is wired into the renderer by this tool; the tables become audible only
when T3 selects a level and reads them.
