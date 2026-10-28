# ABI and Library Contracts — PixelRoot32-APU

Reference version: **v1.0.0** (derived from Game Engine `a3ac998`, branch `refac/extract-apu`, after Phase 0 convergence with the Tool Suite `fc763b8`).

This document consolidates the contracts that previously lived scattered across `docs/MUSIC_ENGINE_ABI_REFERENCE.md`, `docs/SFX_ENGINE_ABI_REFERENCE.md` (Tool Suite) and the engine's comments. In case of conflict, the code in this library is the authority.

## 1. Memory Contracts (Non-Owning Pointers)

The hot path (`generateSamples` / `generateSampleForVoice`) is pure *pointer-walk*, with no heap allocations or copies. Consequence: **every pointer passed in a command must outlive playback**.

| Pointer | Passed in | Must live until |
|---|---|---|
| `const MusicTrack*` (+ subTracks) | `MUSIC_PLAY`, `MUSIC_UPDATE_TRACKS` | `MUSIC_STOP` or replacement |
| `const MusicNote*` (`MusicTrack::notes`) | via track | same |
| `const InstrumentPreset*` (`MusicNote::preset`, `AudioEvent::preset`) | events/notes | end of the voice (including release tail) |
| `const SfxBreakpoint*` (`dutySteps`, `pitchEnvelope`) | `AudioEvent` | end of the voice |

Consumers with dynamic presets must guarantee address stability (the Tool Suite uses `std::deque<InstrumentPreset>`).

## 2. Voice Partitioning and Stealing

- `MAX_VOICES = 8`: slots **0–3** reserved for the sequencer (track ↔ slot 1:1); slots **4–7** for `PLAY_EVENT`/SFX and sequencer percussion.
- Voice stealing is confined to 4–7; **never** steals music slots.
- Stealing ranking: lowest `remainingSamples` first; voices in loop score **0** (anti-starvation) and are the first candidates.

## 3. Automation and Precedences

- **Duty stepped** (`dutySteps`, count 1–4): *hold* without interpolation; while active, `InstrumentPreset::dutySweep` is ignored.
- **Pitch envelope** (`pitchEnvelope`, count 2–4): multi-segment; with ≥ 2 points it **replaces** the single-segment sweep.
- `SfxBreakpoint.timeSec` must be non-decreasing; `value` = duty [0,1] or Hz > 0 according to the table.
- `SweepCurve::Exponential` degrades to `Linear` if both initial/final Hz are not > 0.
- In NOISE, the sweep interpolates the LFSR clock/period, not melodic pitch.
- Shared clock `automationAgeSamples`: one increment per sample feeds both duty and pitch.

### Three-level precedence, profile < voice < note

| Level | Owner | Owns | Beats |
|---|---|---|---|
| 1. **Profile** | `setSoundProfile` / `AudioCommandType::SET_SOUND_PROFILE` | Character: envelope curve, mix, output filters, default duty, noise LUT, unipolar, pan/echo enablement | — |
| 2. **Voice** | `setVoiceNesOptions`, `setVoicePulseDutyMode`, `setVoiceNesNoiseLutMode`, `setVoiceNesSweepUnitEnabled` | A character override on that slot | Profile |
| 3. **Note** | `InstrumentPreset` / `AudioEvent` | ADSR (times and levels), LFO, `duty`, sweep, pitch envelope, `noisePeriod`, `noiseLfsrShort` | Voice and profile, **only inside its own domain** |

- The profile is **configuration, not state**: `init(int sampleRate)` does not reset it (voices are reset). `setSoundProfile` is an audio-thread call; from the game thread use `submitCommand()` with `SET_SOUND_PROFILE`, the same shape as `SET_NES_OPTIONS`. An unknown value is ignored, never clamped to a neighbouring profile.
- `applyProfileCharacter(int slot)` is the **only** function through which the profile writes character, and it is invoked **last** in `initVoiceFromEvent`, after the `nesAccurate` / `initNesSafe` block. `nesAccurate` still means "start from a clean canonical state"; it does not erase the character the profile chose. In this phase the function writes nothing — `Nes`/`Hybrid` would have to write exactly what the note already left — so it exists as the single write point a later phase changes.
- Ordering is asserted structurally, not visually: `getProfileApplyCountForTesting()` (`UNIT_TEST`) is incremented by `applyProfileCharacter`, so one `PLAY_EVENT` with `nesAccurate = true` must advance it exactly once.

**Hot switch (`setSoundProfile`)** — applies immediately to the core and does **not** cut sounding voices. Executed in this order:

1. The **destination core is computed from the argument**, never from the current `profile_`.
2. The envelope mirror of every voice is synced **only when the core actually changes** (`currentLevelQ15 = currentLevel * 32768` or the inverse, towards the destination core). Each core advances a single mirror, so the idle one still holds its note-on value; copying from it when the core does not change would move a live level backwards and produce the very click this step prevents.
3. The output stage is reset (`hpfPrevIn`, `hpfPrevOut`, `hpfPrevInQ15`, `hpfPrevOutQ15`): one core's HPF recursion carries no meaning for the other.
4. `profile_` is committed. Voices keep playing — no `reset()`, no `enabled = false`.

- The **character** applies to the following notes, not to voices already sounding: `setSoundProfile` never rewrites the character of an active voice.
- The phase is **not** mirrored in both directions: the float body advances `phase` and `phaseQ32`, the integer body only `phaseQ32`. The seamless direction is **float → integer** (`Legacy` → `Nes`/`Hybrid` on an FPU build), which is the one a game performs when it loads content. `integer → float` re-frames phase and duty to their note-on values, so it is documented as a **note-boundary change**, not a mid-note one; the same applies to a continuous `dutySweep` crossing the switch. Switch between notes, or with the voices silent.
- Measurable no-click criterion (float → integer): in a 0.5 s render with the switch at sample 11 025, no consecutive-sample difference in the eight samples after the switch may exceed `0.25 × 32767`, and the render may contain no samples on the rail (`±32767`).

## 4. Loop and Duration

- `loop == true` ⇒ `remainingSamples = UINT64_MAX` (sentinel); only terminates via `STOP_CHANNEL` or stealing.
- `loop == false && duration <= 0` ⇒ the voice is deactivated immediately (anti-stall guarantee).
- Sequencer: `MusicNote.duration == 0` = stacked trigger with no time advance (drums on the same step). Preset fallback duration: `max(duration, 0.05) / tempoFactor`.

## 5. Command Queue

`AudioCommandQueue` is **SPSC**: one producer (UI/game thread) and one consumer (audio thread). Does not support multiple concurrent producers. Full queue ⇒ newest command is dropped and `droppedCommands` is incremented.

| Property | Contract |
|---|---|
| Default capacity | `AUDIO_COMMAND_QUEUE_CAPACITY` = **64** (was 128 before the sound-profile release) |
| Accepted range | **32–64** |
| Slot cost | `sizeof(AudioCommand)` = **144 B** (host ABI) |
| Ring cost at the default | `CAPACITY * 144` = **9 216 B** |
| Effective depth | `CAPACITY - 1` = **63** commands — one slot stays free to distinguish full from empty |
| Drop policy | Unchanged: the **newest** command is discarded and `droppedCommands` is incremented |

The ceiling is a **RAM** decision, not a throughput one: the real command rate is tens per second, so the former 128 slots only doubled the dominant cost of `ApuCore`. Raising the ceiling means raising the 9 KB budget in the same commit, after measuring the new slot size — a capacity the budget cannot pay for is not a capacity.

Two `static_assert`s guard the contract in `AudioCommandQueue.h`, verbatim:

```cpp
static_assert(AUDIO_COMMAND_QUEUE_CAPACITY >= 32 && AUDIO_COMMAND_QUEUE_CAPACITY <= 64,
    "AUDIO_COMMAND_QUEUE_CAPACITY must be between 32 and 64");

static_assert(CAPACITY * sizeof(AudioCommand) <= 9216,
              "command queue must stay within the 9 KB RAM budget");
```

The range guard sits on the user-configured branch (`#ifdef AUDIO_COMMAND_QUEUE_CAPACITY`); the budget guard compiles in **every** build and is what catches an `AudioCommand` that grows without anyone touching the capacity. A `sizeof(AudioCommand)` change is an ABI change: recalculate the budget and the `sizeof(ApuCore)` pin in the same commit, and record it here.

## 6. Render Paths

| Path | Guard | Targets |
|---|---|---|
| Float | `PR32_APU_ENABLE_LEGACY_FLOAT == 1` (defaults to `PR32_APU_HAS_FPU`) | Desktop, classic ESP32, ESP32-S3 |
| Integer Q15 + `audio_mixer_lut` | No guard — always compiled | ESP32-C3, RISC-V without FPU (and forced native tests) |

**Both bodies coexist in the same binary.** The float body of `generateSamples` is compiled under `PR32_APU_ENABLE_LEGACY_FLOAT`, defined in `ApuConfig.h` with the default value `PR32_APU_HAS_FPU`; the integer body carries **no guard**. With the default value every build compiles exactly the body it compiled before, so nothing changes for a consumer that defines nothing. `-DPR32_APU_ENABLE_LEGACY_FLOAT=0` removes the float body from an FPU build (the supported flash lever) and `Legacy` then renders through the integer core; a build without FPU always renders integer. Neither setting can produce a binary that fails to link: the float reference is a compile-time guard, not a dynamic lookup.

**The profile selects the body**, once per render block — never per voice:

| `SoundProfile` | `PR32_APU_HAS_FPU=1` | `PR32_APU_HAS_FPU=0` | `PR32_APU_ENABLE_LEGACY_FLOAT=0` |
|---|---|---|---|
| `Legacy` (default) | float | integer | integer |
| `Nes` | integer | integer | integer |
| `Hybrid` | integer | integer | integer |

**`Legacy` is a frozen contract.** It is pinned by two immutable references, one per route, and neither is regenerated in this phase:

| Route | Reference | SHA-256 |
|---|---|---|
| Float (`PR32_APU_HAS_FPU=1`) | `test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm` | `7FF568A5A163A5E11E7A86DFCC0DAFEBCE9C3EE7088C88C6DE9C82293CC15DE9` |
| Integer (`PR32_APU_HAS_FPU=0`) | `test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm` | `F59501D66B522CC26D7F1D48432FAF0FEF9C8ABDACA1B68A09C67D574EF3902D` |

`Legacy` receives **no new features and no optimisations, only defect fixes**. One landed with the integer golden: the Q15 DC blocker truncated `R·y[n-1]` towards −∞ and parked the output at −199 forever once a voice was silenced; it now truncates towards zero like the float conversion does, so the offset decays. That is the only deliberate deviation of the integer fixture from 2.0.0 and it is announced in the CHANGELOG.

`Nes` and `Hybrid` are **byte-identical aliases** in this phase: same core, same character, same bytes. The scaffolding ships without an audible change so that a later phase can give them their own character without moving `Legacy`.

Float chain: `Σ(voice · MIXER_SCALE=0.4) · masterVolume → soft-clip 1/(1+|x|·0.5) → HPF R=0.995 → ±32767 → bitcrush`. The Q15 path replicates the same chain with the pre-adjusted LUT and master volume applied **before** the compressor (order parity since v1.0.0).

**Loudness parity** (since 2.0.0): both paths render at the same level. The Q15 accumulator carries the **raw** channel sum and `MIXER_SCALE` is applied exactly once downstream — by `audio_mixer_lut`, whose curve is defined over `x = sum · 1.6 / 131072` and therefore bakes the 0.4 in, or explicitly as `(sum · 13107) >> 15` for the computed soft-clip modes. Do **not** pre-scale channels before summation: that applies 0.4 twice and puts the compressor knee in the wrong place.

Previously the Q15 path was ~0.225× the float level — the double `MIXER_SCALE` plus a fixed −6 dB pad (`>>1`, Q15→Q14) with no float equivalent. Both are gone; consumers on FPU-less targets should re-check their master volume. See the CHANGELOG entry for Hito 5 M12.

## 7. Canonical Percussion Presets

`noisePeriod` is the **LFSR period in samples**: larger period ⇒ slower clock ⇒ deeper sound.

These presets are **note-level** (`noisePeriod` travels in `InstrumentPreset`), so under the three-level precedence of §3 they outrank both the profile and any per-voice override inside their own domain: a profile owns the noise **LUT** (`setVoiceNesNoiseLutMode` overrides it per voice) and the envelope curve, but it never rewrites the preset's period.

| Preset | noisePeriod | Character |
|---|---|---|
| `INSTR_KICK` | 60 | deep, with body |
| `INSTR_SNARE` | 15 | bright, snap |
| `INSTR_HIHAT` | 12 | metallic (short mode) |

## 8. Music Transport

- `MUSIC_PLAY` anchors `musicPlayStartTick`; `MUSIC_SEEK` (`seekOffsetTicks` = ticks elapsed since the piece start) repositions by rebuilding per-track indices (`resyncTrackSequencerToTick`), with loop and stacked-note support.
- `MUSIC_UPDATE_TRACKS` replaces tracks on the fly without resetting the transport.
- Thread-safe observation: `getMusicGlobalTick()`, `getMusicPlayStartTick()`, `isMusicPlaying()`, `isMusicPaused()`.
- `TICKS_PER_BEAT = 4`; `MusicNote.duration` in beats (quarter note = 1.0).

## 9. Verification

The suite (`ctest`) includes `test_apu_mirror` with a **golden PCM** (`test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm`): the canonical rendered track must match byte-for-byte. Regenerate it **only** when the output changes intentionally and is documented in the CHANGELOG.

### 9.1 Targets and case counts

Measured 2026-10-27 on the sound-profile branch (`cmake -S . -B build -G "MinGW Makefiles" --fresh -DPR32_APU_BUILD_TESTS=ON`, `cmake --build build`, `cd build && ctest --output-on-failure` → `100% tests passed out of 9`, 1.33 s). The case counts are Unity's summary line, each binary run from `test/`:

| Target | Cases | What it covers |
|---|---|---|
| `test_apu_core` | 182 | Core behaviour on the float core (as selected by the default profile) |
| `test_apu_core_integer` | 182 | The same source against the integer core (`PR32_APU_HAS_FPU=0`) |
| `test_audio_command_queue` | 10 | SPSC queue, capacity and the `AudioCommand` ABI pins |
| `test_audio_music_types` | 35 | Music types, `InstrumentPreset` layout, shipped presets |
| `test_apu_mirror` | 37 | Tool Suite parity, byte-for-byte against the float golden |
| `test_q15_smoke` | 15 | Integer/Q15 path smoke coverage |
| `test_apu_integer_golden` | 1 | Integer core against the integer golden (`Nes`, FPU build) |
| `test_apu_integer_golden_no_fpu` | 1 | Integer core against the integer golden (`Legacy`, no-FPU build) |
| `test_library_link` | 4 | Public symbols exist in the shipped artifact (links, no `UNIT_TEST`) |
| **Total** | **467** | 0 failures, 0 ignored, exit code 0 in all nine |

### 9.2 Golden PCM

Two immutable references, one per render route. Both are raw S16LE, no header, 22 050 samples at 44 100 Hz from `test/golden/apu_mirror_canonical_track.h`.

| Reference | Consumed by | SHA-256 |
|---|---|---|
| `apu_mirror_canonical_s16le_44100.pcm` | `test_apu_mirror` | `7FF568A5A163A5E11E7A86DFCC0DAFEBCE9C3EE7088C88C6DE9C82293CC15DE9` |
| `apu_integer_core_canonical_s16le_44100.pcm` | `test_apu_integer_golden`, `test_apu_integer_golden_no_fpu` | `F59501D66B522CC26D7F1D48432FAF0FEF9C8ABDACA1B68A09C67D574EF3902D` |

The integer fixture is written by `python tools/golden_gen/run.py --write`, which builds three generator binaries and refuses to proceed unless the default build still reproduces the float fixture byte-for-byte and the two integer builds agree with each other. Regenerate a fixture **only** when the output changes intentionally and the change is documented in the CHANGELOG.

### 9.3 CPU baseline, host

8 cells of `{Legacy, Nes} × {22 050, 32 000} × {1, 8 voices}`, 100 discarded blocks of 512 samples followed by 1 000 measured ones per cell. `python tools/apu_bench/run.py`, g++ 16.2.0 (MSYS2, `x86_64-w64-mingw32`), `-std=c++17 -Os -I include`, `Windows-10-10.0.19044-SP0`.

| Profile | Rate (Hz) | Voices | p50 (µs) | p99 (µs) | µs / sample |
|---|---|---|---|---|---|
| Legacy | 22050 | 1 | 16.400 | 29.500 | 0.032 |
| Legacy | 22050 | 8 | 108.100 | 142.500 | 0.211 |
| Legacy | 32000 | 1 | 16.100 | 30.300 | 0.031 |
| Legacy | 32000 | 8 | 116.600 | 168.400 | 0.228 |
| Nes | 22050 | 1 | 23.100 | 37.400 | 0.045 |
| Nes | 22050 | 8 | 74.800 | 103.600 | 0.146 |
| Nes | 32000 | 1 | 23.000 | 37.300 | 0.045 |
| Nes | 32000 | 8 | 74.600 | 103.600 | 0.146 |

**The comparable statistic is the p50. The p99 is host scheduling noise**, not a property of the core: over consecutive runs the p50 stayed within ±8.7 % in all eight cells, while the p99 moved by more than 10 % in three of them (up to +28.9 %) with no correlation to profile or rate — it is the block that lost the CPU to another process, and this host has no prioritised audio thread. Read the table for order of magnitude per core and per voice count; on a board the verdict is the underrun counter, not the p99.

Two consequences worth knowing: at 8 voices the integer core is **cheaper** than the float one (1.445× at 22 050 Hz, 1.563× at 32 000 Hz), and at 1 voice it is **more expensive** (23.100 vs 16.400 µs at 22 050 Hz) because it pays a fixed per-sample cost that only amortises with voices. That is consistent with `Legacy` remaining the default profile: the integer core buys aggregate CPU, not single-voice CPU.

### 9.4 `sizeof` (measured)

```
sizeof(ApuCore)          = 13200 B   host ABI, production build (queue 64)
sizeof(ApuCore)          = 13224 B   host ABI, UNIT_TEST build
sizeof(ApuCore)          =  9800 B   32-bit target ABI
sizeof(AudioCommand)     =   144 B
sizeof(AudioChannel)     =   328 B
sizeof(InstrumentPreset) =    72 B
queue budget             = 64 x 144 B = 9216 B
```

The two host figures are pinned by `static_assert(sizeof(ApuCore) <= 13312, ...)` in `test_apu_core`; the test compiles with `UNIT_TEST=1`, so it asserts 13 224 B and the production figure is recorded next to the pin. The 32-bit figure is the `gCoreStorage` symbol (0x2648) in the firmware maps of `tools/apu_bench_firmware` for `esp32dev`, `esp32-c3-devkitm-1` and `esp32-s3-devkitc-1` — identical on the three, because the ABI is the same width.

### 9.5 CPU and underrun baseline, firmware

Measured on 2026-10-28 with `tools/apu_bench_firmware` over `COM4`. Board: `ESP32-D0WD-V3` rev 3, 240 MHz, `fpu=1 legacy_float=1`, 512-sample block, 100 warm-up + 1000 measured blocks per cell. An unmeasured cell stays `pendiente`; it is never filled with an estimate.

| Board | Profile | 22 050 Hz, 1 / 8 voices (p50 µs) | 32 000 Hz, 1 / 8 voices (p50 µs) | Headroom at 8 voices | Underruns, 10 min |
|---|---|---|---|---|---|
| `esp32dev` | `Legacy` | 1 540 / 6 618 | 1 541 / 6 617 | 3.5x / 2.4x | **0** |
| `esp32dev` | `Nes` | 1 451 / 5 589 | 1 452 / 5 590 | 4.1x / 2.8x | **0** |
| `esp32-c3-devkitm-1` | `Legacy` | 2 329 / 7 546 | 2 331 / 7 545 | 3.1x / 2.1x | `pendiente` |
| `esp32-c3-devkitm-1` | `Nes` | 2 329 / 7 546 | 2 331 / 7 545 | 3.1x / 2.1x | `pendiente` |

What the table says:

- The per-block cost is rate-invariant (6.6 ms for 512 samples at either rate), as it must be: what changes with the rate is the budget (23.2 ms vs 16 ms) and therefore the headroom.
- On silicon the integer core is **1.18x faster** than the float body at 8 voices and **1.06x faster** at 1 voice. The host ratio (1.44-1.56x at 8 voices, and the integer path *slower* at 1 voice) does not transfer: the ESP32's single-precision FPU is not the bottleneck people assume.
- 32 000 Hz with 8 voices leaves **2.4x** (`Legacy`) / **2.8x** (`Nes`): viable, and the tightest configuration in the matrix — which is why 22 050 Hz stayed the default.
- **Underruns: 0 in ten minutes at both rates.** Measured with `tools/apu_underrun_probe` (engine repository), which drives the real I2S backend with eight voices on pins BCLK 26 / LRCK 25 / DOUT 22. Worst whole iteration: 23 221 us at 22 050 Hz (budget 23 220) and 15 995 us at 32 000 (budget 16 000). Worst write wait: 17 298 us and 10 087 us. The write wait is the figure that matters: with a one-block ring a healthy write blocks for most of the period, so a starving producer shows up as a write that barely waits, not as a slow iteration.
- **The C3 rows are identical by construction.** With `fpu=0` both profiles run the same integer body, so `Legacy` and `Nes` must measure the same cell; the tool prints both render macros so the reader can confirm it instead of trusting it. At 160 MHz it costs 51 % more per sample than the ESP32 at one voice (2 329 vs 1 540 us) and 14 % more at eight (7 546 vs 6 618 us).
- **The tightest cell in the matrix is the C3 at 32 000 Hz with eight voices: 2.1x headroom, p99 at 47.4 % of the block** (7 577 of 16 000 us). It passes the rule in S12 (p99 must stay under half the block for a target to keep 32 kHz) with three points of margin, which is exactly why 22 050 Hz stayed the default.
- The I2S gate is a separate harness on purpose: this bench renders into RAM and never touches a peripheral, so it cannot count underruns. `esp32-c3-devkitm-1` and `esp32-s3-devkitc-1` remain `pendiente` for both the CPU table and the gate.

The target `sizeof` printed by that bench is not `pendiente`: `sizeof_apu_core=9800`, `sizeof_audio_command=96`, `sizeof_audio_channel=296`, `queue_bytes=6144` on the 32-bit ABI, all consistent with §9.4. `Legacy` and `Nes` measure the same body on the C3 (no FPU), which the tool prints rather than hides.
