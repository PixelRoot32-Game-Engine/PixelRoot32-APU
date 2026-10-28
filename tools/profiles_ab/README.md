# profiles_ab — blind A/B of the three sound profiles (S9.4)

Renders **one excerpt of the same music** through `Nes`, `Hybrid` and `Legacy`
and writes three 16-bit mono 44 100 Hz WAV files with **the same RMS**, under
neutral names. The mapping lives in a separate `key.txt`, so you can answer
without seeing the labels.

```bash
python tools/profiles_ab/run.py                        # writes to %TEMP%\pr32-apu-ab
python tools/profiles_ab/run.py --out D:\listening     # or wherever you prefer
```

Then listen to `a.wav`, `b.wav` and `c.wav` and answer **one** question:

> Which file is `Nes`, which is `Hybrid`, and which is `Legacy`?

That is the whole task. Answer it before opening `key.txt`; the verdict is the
person's, and a metric cannot stand in for it (S9.4).

---

## What the excerpt contains

6.5 s at 120 BPM: a PULSE lead at 50 % duty, a PULSE harmony at 25 %, a
TRIANGLE bass one note per bar, and NOISE percussion from the shipped kick,
snare and hi-hat presets. Those are exactly the four character changes of E1 —
envelope curve (S2), weighted mix (S4), unipolar noise on the NES period LUT
(S5) and stepped duty (S6) — so a difference you hear belongs to the profile and
not to the arrangement.

## How the RMS is matched

1. `render.cpp` renders the track with **one** profile to raw S16LE: no header,
   no post-processing, no normalisation inside C++.
2. `run.py` measures the RMS of each render and takes the **quietest** as the
   common target. Matching downwards means every gain is ≤ 1, so the matching
   cannot clip a sample and no limiter is involved.
3. Every sample is multiplied by `target / rms` and rounded half away from zero.
   The achieved RMS is measured again from the raw render and printed.

Measured on 2026-10-28 (g++ 16.2.0, MSYS2 `x86_64-w64-mingw32`, Windows 10):

| Profile | RMS before (LSB) | Before (dBFS) | Peak before | Gain | File | RMS after (LSB) | After (dBFS) |
|---|---|---|---|---|---|---|---|
| `Hybrid` | 4 113.09 | −18.03 | 22 648 | 1.000 (+0.00 dB) | `a.wav` | 4 113.09 | −18.03 |
| `Legacy` | 4 308.90 | −17.62 | 27 419 | 0.955 (−0.40 dB) | `b.wav` | 4 113.09 | −18.03 |
| `Nes` | 4 651.26 | −16.96 | 22 648 | 0.884 (−1.07 dB) | `c.wav` | 4 113.09 | −18.03 |

Spread after matching: **0.0005 LSB (0.0000 dB)**, measured independently from
the three WAV files after writing them. The `peak` column is reported because a
file that railed before matching would make the comparison meaningless; none of
them does.

The mapping in that table is here for the record only — `key.txt` is what you
read after answering, and it is written into the output directory next to the
audio.

## Requirements and side effects

- A C++17 compiler on the PATH (`g++`, `clang++` or `c++`). No CMake.
- The tool and the core are compiled into a temporary directory and removed on
  exit. **The only thing left behind is the output directory** with `a.wav`,
  `b.wav`, `c.wav` and `key.txt`, because that is the deliverable.
- Nothing is written inside the repository.

## Expected output

```
rendering 286650 samples (6.500 s @ 44100 Hz) per profile
  hybrid  profile=hybrid sample_rate=44100 samples=286650 bytes=573300 peak=22648
  ...
RMS before matching
  hybrid  rms  4113.09 (-18.03 dBFS)  peak 22648
  legacy  rms  4308.90 (-17.62 dBFS)  peak 27419
  nes     rms  4651.26 (-16.96 dBFS)  peak 22648
matching every file to rms 4113.09 (-18.03 dBFS), the quietest render
  ...
RMS spread after matching: 0.000 LSB (0.0000 dB)
```

The three renders are deterministic: re-running the script reproduces the same
samples and the same RMS figures.
