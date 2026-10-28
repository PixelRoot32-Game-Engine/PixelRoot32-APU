# apu_bench_firmware — on-board CPU baseline (S11 / T8)

Measures what **one 512-sample render block** costs on an ESP32, with
`esp_timer_get_time()`, and prints it over the serial port as CSV.

It is the board-side sibling of `tools/apu_bench` (host): **the same matrix, the
same warm-up, the same percentile and the same voice setup**, because two benches
measuring different things would produce two tables nobody can compare.

```bash
cd tools/apu_bench_firmware
pio run                                       # esp32dev only (default_envs)
pio run -e esp32dev -e esp32-c3-devkitm-1 -e esp32-s3-devkitc-1
pio run -e esp32dev -t upload                 # flash
pio device monitor -b 115200 > bench.csv      # capture
```

The project is standalone: it is not part of the root CMake build.

---

## What it measures

| Profile | Rate | Voices |
|---|---|---|
| `Legacy` (float body if the target has an FPU; integer if not) | 22 050 Hz, 32 000 Hz | 1, 8 |
| `Nes` (integer body) | 22 050 Hz, 32 000 Hz | 1, 8 |

Eight cells. Each one: `init(rate)`, `setSoundProfile(profile)`, start the voices,
**100 blocks discarded** and **1 000 measured**. The clock wraps
`generateSamples(buffer, 512)` and nothing else; the checksum of the 512 samples is
accumulated **outside** the timed interval, so the compiler cannot elide the render
and the checksum does not time itself.

The percentiles are **nearest-rank** (`ceil(p/100 × N) − 1`): a block time is an
observation, so p99 has to be a block that happened. `us_per_sample = p50 / 512`.

On a target without an FPU (`esp32-c3-devkitm-1`) `Legacy` and `Nes` run **the same
integer body**, so their two rows measure the same thing. The tool does not hide
it: it prints both render macros in the identity line.

## What it does NOT measure

- **It does not measure I2S or DAC latency.** It opens no peripheral: it renders
  into a buffer and throws it away.
- **It does not count underruns.** That comes from the backend counter on a real
  run (0 in 10 minutes per board and rate, S13); here there is no backend, no DMA
  and no audio clock to chase.
- **It has no threshold and no verdict.** It measures; the performance gate is made
  of S15's conditions.

## How the eight voices are guaranteed

Same as in the host bench, for the same reason: slots 0–3 belong to the sequencer
and `PLAY_EVENT` only allocates 4–7 (`findVoiceForSfxEvent`). Eight effect events
would silently stay at 4 voices. Hence:

- **Slots 0–3 — music.** One `MUSIC_PLAY` with the lead track and three sub-tracks,
  each with **a single note of 128 beats** (51.2 s at the default 150 BPM), against
  the 25.5 s / 17.6 s the run lasts: the gate cannot expire inside the measurement.
- **Slots 4–7 — effects.** Four `PLAY_EVENT`s with `loop = true` and distinct
  waveforms, so the allocator hands out 4, 5, 6 and 7 instead of reusing one.

The guarantee is structural (a gate and a `loop` that do not expire), the same one
verified on host in T7 with a probe: 8 voices enabled before and after the
1 100 blocks. On the board nothing is counted: if an eight-voice cell comes out
suspiciously cheap, that is the first place to look.

## Output

Everything over serial at 115 200 baud, in this order:

1. **An identity line** starting with `# `: chip model and revision, CPU MHz,
   `PR32_APU_HAS_FPU`, `PR32_APU_ENABLE_LEGACY_FLOAT`, `sizeof` of `ApuCore` /
   `AudioCommand` / `AudioChannel` / `InstrumentPreset`, and the queue budget
   (`CAPACITY × sizeof(AudioCommand)`). The target `sizeof` is the figure S8
   requires recording per board instead of pinning with a constant.
2. **The exact CSV header**: `profile,rate,voices,p50_us,p99_us,us_per_sample`.
3. **One row per cell**, each followed by **its `# ` line** with the block budget,
   the headroom (`budget / p99`) and the checksum.

Everything that is not a CSV row starts with `#`, so:

```bash
grep -v '^#' bench.csv     # clean CSV
grep '^#' bench.csv        # metadata: identity and per cell
```

The chip's boot ROM prints garbage before the first line; keep only the lines that
start with `#` or with `profile,`, plus the eight data rows.

Numbers are printed with integer arithmetic (`x.yyy`), not `%f`: the board clock
returns whole microseconds, and this way the tool does not depend on `newlib`'s
float support, which cannot be verified here without a board. That is why `p50_us`
always ends in `.000`: `esp_timer_get_time()` resolves 1 µs.

## Results

Measured on 2026-10-28 on an **ESP32-D0WD-V3 rev 3, 240 MHz**, `COM4`
(`fpu=1 legacy_float=1`), p50 / p99 in µs per 512-sample block:

| Profile | Rate | 1 voice | 8 voices | Headroom at 8 voices |
|---|---|---|---|---|
| `Legacy` | 22 050 | 1 540 / 1 555 | 6 618 / 6 656 | 3.5x |
| `Legacy` | 32 000 | 1 541 / 1 556 | 6 617 / 6 655 | 2.4x |
| `Nes` | 22 050 | 1 451 / 1 472 | 5 589 / 5 641 | 4.1x |
| `Nes` | 32 000 | 1 452 / 1 472 | 5 590 / 5 640 | 2.8x |

`esp32-c3-devkitm-1` and `esp32-s3-devkitc-1` remain **pending**: no such board has
been attached yet. An unmeasured cell stays empty and marked `pending`; it is never
filled with an estimate. The same holds for the S13 underrun counter.

The figures are transcribed per board and rate into **`docs/ABI.md` §9**.

## Deviations from the T8 brief

1. **`lib_deps = symlink://../..` instead of `file://../..`.** This project lives
   *inside* the library it measures, and PlatformIO installs a `file://` dependency
   with `shutil.copytree` of the whole folder: it would copy the repository
   (including `.git`) into `.pio/libdeps/<env>/` on every env and again on every
   rebuild, taking the project's own `.pio` with it. The first attempt died with
   `Permission denied` on `tools/apu_bench_firmware/.pio/libdeps/esp32dev.lock`.
   `symlink://` in PlatformIO is **not** a system symlink: it writes the path into a
   `.pio-link` and compiles the library in place, so the measurement can only
   describe this working tree.
2. **`-std=gnu++17` + `build_unflags = -std=gnu++11`.** The `espressif32` Arduino
   framework compiles with `gnu++11` and the library declares `-std=gnu++17` in its
   `library.json` (C++14's relaxed aggregate rules are what makes
   `MusicNote{...}` compile). The last `-std` on the command line wins, and the
   framework's arrives after `build_flags`, so it has to be removed.
3. **`ApuCore` lives in static storage** (`gCoreStorage` + placement new per cell),
   not in a local: `sizeof(ApuCore)` is **9 800 B** on the 32-bit ABI of all three
   boards (13 200 B on host, L16) and the `loop` task that runs `setup()` has 8 KB
   of stack. The host bench's guarantee — every cell starts from a freshly
   constructed core — is preserved; what changes is where the memory lives. Same
   for the array of 1 000 block times.
4. **`submit()` cannot call `std::exit`.** If the queue rejects a command, it prints
   `error: command queue rejected a command` and stays in `delay()` instead of
   measuring with a voice count that is not the one in the CSV.
