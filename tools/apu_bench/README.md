# apu_bench — host CPU baseline for both render cores (S11 / T7)

Measures what **one 512-sample render block** costs in each core, on the desktop,
with both cores living in the **same binary**.

The comparison is between cores, not between builds: `Legacy` runs the float
body and `Nes` the integer body, and the only lever used to choose is
`setSoundProfile`, the public API. A benchmark that called `renderFloatLegacy` /
`renderIntegerCore` directly would measure a function it invented itself, not the
dispatch a game goes through.

```bash
python tools/apu_bench/run.py
python tools/apu_bench/run.py --opt -O2     # to separate optimisation from core
```

Requires a C++17 compiler on the PATH (`g++`, `clang++` or `c++`). There is no
CMake: `run.py` compiles `main.cpp` + `src/ApuCore.cpp` directly, like
`tools/loudness_probe`. The binary is built in a temporary system directory and
removed at the end: **this tool writes nothing inside the repo**.

---

## The matrix

| Profile | Rate | Voices |
|---|---|---|
| `Legacy` (float body) | 22 050 Hz, 32 000 Hz | 1, 8 |
| `Nes` (integer body) | 22 050 Hz, 32 000 Hz | 1, 8 |

Eight cells. Each one: `init(rate)`, `setSoundProfile(profile)`, start the voices,
**100 warm-up blocks discarded** and **1 000 blocks measured**. The two rates are
the ones the plan uses; 512 samples is the `blockSize` S12 sets for FPU platforms,
so the measured block is what the firmware really renders.

## How the eight voices are guaranteed

The pool has 8 slots, but not everything may take any of them: slots 0–3 belong
to the sequencer and `PLAY_EVENT` only allocates 4–7 (`findVoiceForSfxEvent`
walks `SFX_VOICE_BASE..MAX_VOICES`). A benchmark that started eight effect events
would silently stay at 4 voices. Hence:

- **Slots 0–3 — music.** One `MUSIC_PLAY` with the lead track and three sub-tracks
  (`subTracks[3]`), one per slot. Each track has **a single note of 128 beats**:
  at the default 150 BPM that is 51.2 s, while the whole window (1 100 blocks =
  563 200 samples) lasts 25.5 s at 22 050 Hz and 17.6 s at 32 000 Hz. The note
  gate cannot expire inside the run, so the sequencer does not retrigger it
  mid-measurement.
- **Slots 4–7 — effects.** Four `PLAY_EVENT`s with `loop = true` and distinct
  waveforms, so `findVoiceForSfxEvent` hands out 4, 5, 6 and 7 instead of reusing
  one. With `gate_samples == 0` and `loop`, `initVoiceFromEvent` sets
  `remainingSamples = UINT64_MAX` and the per-sample expiry never fires.

There is no retrigger: nothing stops, so nothing needs re-arming every 100 blocks.
The guarantee is structural (a gate and a loop that do not expire) and was checked
in T7 with a `UNIT_TEST` probe outside the tree: **8 voices enabled before and
after the 1 100 blocks, at both rates** (1 voice, 1 enabled). If the note were
shortened or the `loop` dropped, the eight-voice cell would fall to 4 and the
number would look better for the wrong reason.

## What each column measures

| Column | What it is | What it is for |
|---|---|---|
| `p50_us` | Median of the 1 000 block times | **The cost of the core.** It is the reproducible figure. |
| `p99_us` | 99th percentile by **nearest rank** | Real worst case of the run, not an interpolation. |
| `us_per_sample` | `p50 / 512` | Comparable with the board's per-sample budget. |
| `budget_us` | `512 × 1e6 / rate` | Audio time that block represents. |
| `headroom` | `budget / p99` | "x times real time": how much faster than the clock. |

The percentile is nearest-rank (`ceil(p/100 × N) − 1`) on purpose: a block time is
an observation, not a distribution, so `p99` has to be a block that actually
happened. With 1 000 blocks it is the 990th value.

The render cannot be elided: each cell accumulates an `int64_t` checksum of the
512 samples of every measured block and prints it in the last column of the stderr
table. Without a reader, a compiler could reason that nobody uses the buffer and
measure a deleted loop, which is very fast and does not exist.

## Why `-Os`

It is the firmware build level, so the desktop baseline (T7) and the board one
(T8) compare the same thing. `--opt` exists to answer a different question: if a
difference between cores disappears at `-O2`, it belongs to the optimiser, not to
the core.

## How to read the result

- **`p50` is the measurement; `p99` is the operating system's tail.** On a Windows
  desktop there is no prioritised audio thread and no reserved CPU: `p99` catches
  the block that lost the CPU to another process. Measured in T7 over two
  consecutive runs: `p50` moved ≤ 8.7 % across the eight cells (within the ±10 %
  S11 asks for), while `p99` moved up to 28.9 % and with opposite signs between
  cells. The **1-voice** cells are the noisiest in relative terms (their absolute
  cost is 16–24 µs, so a few µs of jitter weigh heavily: a later pass gave 18.7 µs
  for `Legacy 22050 v1`, +14 %), and the **8-voice** cells — the ones that matter
  for the budget — moved ≤ 8.2 %. It is not core instability; it is the host. On
  the board the authority is the underrun counter (S13), not this percentile.
- **`headroom` computed from `p99`** is the conservative number: if the worst block
  fits the budget, the cell cannot fail on CPU.
- **The cost per block does not depend on the rate** (same number of samples), but
  the cost **per second of audio** does: at 32 000 Hz a block lasts 16 ms instead
  of 23.22 ms, so 1.45× more blocks are rendered per second. That is why the
  headroom at 32 000 Hz is ≈ 0.69× the one at 22 050 Hz.

## What these figures are NOT

They are **host x86_64** figures, measured with the desktop toolchain. They say
nothing about an ESP32: no flash, no shared instruction cache, no `i2s_write`, no
underruns. That is `tools/apu_bench_firmware` (T8), recorded per board in
`docs/ABI.md` §9.

The tool **has no threshold**: it measures, it does not pass or fail. A slow cell
is a datum, not a build failure. T7 records the baseline; the performance gate is
made of S15's conditions, not of this binary.

## Output

- **stdout**: CSV, exact header `profile,rate,voices,p50_us,p99_us,us_per_sample`,
  one row per cell, three decimals. `apu_bench > baseline.csv` stays clean.
- **stderr**: the human table with the budget and the headroom, plus the `sizeof`
  block (`AudioCore`, `AudioCommand`, `AudioChannel`, `InstrumentPreset` and the
  queue budget, `CAPACITY × sizeof(AudioCommand)`).
- **`run.py`** reprints the CSV, a Markdown table ready to paste into the plan
  document, and the `sizeof` lines.

`sizeof(AudioCore)` is the name S11 and §4 ask for; the type is called `ApuCore`
(`include/pixelroot32/apu/ApuCore.h`). The line prints the requested name and
declares the real one, so the discrepancy is visible instead of being quietly
corrected.
