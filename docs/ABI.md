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
- **Duty mirror at full scale**: `AudioEvent::duty` is clamped to [0, 1] and `AudioChannel::dutyCycleQ32` mirrors it for the integer core as `duty * 2^32`, **saturating at `0xFFFFFFFF` for `duty == 1.0`**. The natural product is exactly 2^32 there, which does not fit in `uint32_t`; the wrapped 0 made `phaseQ32 < dutyCycleQ32` false for every phase, so a full-scale pulse rendered flat while the float body rendered a constant high. No other value changes, so the integer golden is untouched.

### Three-level precedence, profile < voice < note

| Level | Owner | Owns | Beats |
|---|---|---|---|
| 1. **Profile** | `setSoundProfile` / `AudioCommandType::SET_SOUND_PROFILE` | Character: default envelope curve, mix curve, output filters, default duty, noise LUT, unipolar noise, pan/echo enablement | — |
| 2. **Voice** | `setVoiceNesOptions`, `setVoicePulseDutyMode`, `setVoiceNesNoiseLutMode`, `setVoiceNesSweepUnitEnabled` | A character override on that slot (including `VoiceNesOptions::noiseUnipolar`) | Profile |
| 3. **Note** | `InstrumentPreset` / `AudioEvent` | ADSR (times and levels), envelope curve, LFO, `duty`, sweep, pitch envelope, `noisePeriod`, `noiseLfsrShort` | Voice and profile, **only inside its own domain** |

- The profile is **configuration, not state**: `init(int sampleRate)` does not reset it (voices are reset). `setSoundProfile` is an audio-thread call; from the game thread use `submitCommand()` with `SET_SOUND_PROFILE`, the same shape as `SET_NES_OPTIONS`. An unknown value is ignored, never clamped to a neighbouring profile.
- `applyProfileCharacter(int slot)` is the **only** function through which the profile writes character, and it is invoked **last** in `initVoiceFromEvent`, after the `nesAccurate` / `initNesSafe` block. `nesAccurate` still means "start from a clean canonical state"; it does not erase the character the profile chose. Since E1 it resolves the default envelope curve and its exponential clock, the unipolar NOISE gate, the NES period LUT selector and the stepped PULSE duty. The mix curve is not voice state: it is resolved once per render block from the same profile (§6).
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

**`sizeof(AudioCommand)` is still 144 B after E1.** S5 needed a seventh `VoiceNesOptions` byte (`noiseUnipolar`); `nesLengthIndex`, `nesLengthHalt` and the options used to fill exactly eight bytes before `subTracks`, so the seventh option byte would have spilled into the next 8-byte boundary and grown every slot to **152 B** — 9 728 B for the 64-slot queue, over the 9 KB assert. `AudioCommand::nesLengthHalt` therefore moved into the padding before `seekOffsetTicks`: one byte out of the group puts it back at eight, the budget stays `64 × 144 B = 9 216 B`, and nothing that names the member observes the move — the name and the type are unchanged, only its offset inside bytes that were already padding. The move is documented at the field, in `AudioTypes.h`.

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

**`Nes` and `Hybrid` own a character since E1**; they are no longer aliases of `Legacy` or of each other. Both render the integer core and share the published NES balance and the hardware-shaped noise; `Hybrid` adds the exponential envelope.

| Character | `Legacy` | `Nes` | `Hybrid` | Who sets the default |
|---|---|---|---|---|
| Envelope curve | linear (frozen) | linear, plus the 4-bit `nesEnvelope` at 240 Hz | exponential DECAY and RELEASE, linear ATTACK | profile; a note may name `InstrumentPreset::envelopeCurve` |
| Mix curve | `Rational` (equal weight) | `NesWeighted` | `NesWeighted` | profile only, resolved once per block |
| NOISE output | bipolar | unipolar `(lfsr & 1) ? level : 0` | unipolar | profile; `VoiceNesOptions::noiseUnipolar` pins it per voice |
| NOISE period | derived from the note's Hz | NES period LUT index 0 (`$400E = $00`) | index 0 | profile; `setVoiceNesNoiseLutMode` pins it per voice |
| PULSE duty | continuous (`255`) | NES step 0–3 | NES step 0–3 | profile; `setVoicePulseDutyMode` pins it, an animated duty keeps the continuous generator |

The four are contracts, not implementation notes:

- **Envelope curve.** `EnvelopeCurve` is the last member of `InstrumentPreset` and defaults to `ProfileDefault`, which means "the profile decides": `Legacy` and `Nes` answer `Linear`, `Hybrid` `Exponential`. DECAY and RELEASE follow `level -= ((level - 1) >> 8) + 1` (floor at 0) and reach −60 dB in the stage's own time ±5 %; ATTACK stays linear because a predictable attack is worth more than a matching curve. A stage **shorter than the shape's own −60 dB span keeps the linear ramp**: stepping once per sample there would leave the level far above its target and snap at the stage boundary (~7 dB on the default 5 ms release), and those are exactly the stages where the two ramps are indistinguishable.
- **Mix curve.** `MixerCurve::NesWeighted`, orthogonal to `SoftClipMode` and applied **before** the HPF, is the published linear approximation of the NES mixer by group: pulses `0.00752 × (p1 + p2)`, triangle `0.00851`, noise `0.00494`, DMC `0.00335`. Every weight is divided by the pulse weight and rounded to Q12 — **4096 / 4635 / 2691 / 1825**, with the pulse group pinned at unity — so a full-scale pulse keeps exactly the amplitude `Legacy` gives it and the change is balance, not loudness. `SINE` and `SAW` join the pulse group: they are the only other melodic voices and the published approximation has no counterpart for them. The DMC term is **reserved** (`kNesMixWeightDmc`): E1 has no DMC voice model, so it is documented rather than driven. The `Legacy` curve stays the historical rational one.
- **Noise.** The unipolar gate switches between `0` and the envelope level instead of `±level`. Unipolarisation halves the AC amplitude, so one exact ×2 shift follows the volume × envelope multiplies — measured −0.001 dB against bipolar **at the same weighting**. The remaining −3.649 dB against `Legacy` is deliberate: it *is* the published noise weight (`0.00494 / 0.00752 = −3.650 dB`). Making it as loud as `Legacy` would need ×3.044 and would double the noise group against the pulse anchor. `Legacy` keeps its bipolar noise and its Hz-derived period.
- **Duty.** `Nes` and `Hybrid` snap the note's `duty` to the nearest of the four NES patterns, round-half-up: `[0, 0.1875) → 12.5 %`, `[0.1875, 0.375) → 25 %`, `[0.375, 0.625) → 50 %`, `[0.625, 1] → 75 %`. PULSE only, and only when the slot is not pinned. A note that **animates** duty (`dutySteps` or `dutySweep`) keeps the continuous generator, because one static 2-bit index cannot express animation — that is the note winning inside its own domain (§3), not an exception. `setVoicePulseDutyMode(slot, 255)` is the documented way to keep continuous duty under a NES profile.

What a **later phase** completes, not this one: DMC (the term is reserved), pan/echo enablement, and the S10 collapse — if the blind A/B of `tools/profiles_ab` shows that `Nes` and `Hybrid` cannot be told apart, the two profiles merge into `Hybrid` and this table loses a column.

Float chain: `Σ(voice · MIXER_SCALE=0.4) · masterVolume → soft-clip 1/(1+|x|·0.5) → HPF R=0.995 → ±32767 → bitcrush`. The Q15 path replicates the same chain with the pre-adjusted LUT and master volume applied **before** the compressor (order parity since v1.0.0).

**Loudness parity** (since 2.0.0): both paths render at the same level. The Q15 accumulator carries the **raw** channel sum and `MIXER_SCALE` is applied exactly once downstream — by `audio_mixer_lut`, whose curve is defined over `x = sum · 1.6 / 131072` and therefore bakes the 0.4 in, or explicitly as `(sum · 13107) >> 15` for the computed soft-clip modes. Do **not** pre-scale channels before summation: that applies 0.4 twice and puts the compressor knee in the wrong place.

Previously the Q15 path was ~0.225× the float level — the double `MIXER_SCALE` plus a fixed −6 dB pad (`>>1`, Q15→Q14) with no float equivalent. Both are gone; consumers on FPU-less targets should re-check their master volume. See the CHANGELOG entry for Hito 5 M12.

## 7. Canonical Percussion Presets

`noisePeriod` is the **LFSR period in samples**: larger period ⇒ slower clock ⇒ deeper sound.

These presets are **note-level** (`noisePeriod` travels in `InstrumentPreset`), so inside their own domain they beat a profile default and a per-voice override. The NOISE **period** is the documented exception, because the profile owns it through the LUT: `Nes` and `Hybrid` arm LUT index 0 by default, arming the LUT **retunes** the period, and a preset's own `noisePeriod` (60 for `INSTR_KICK`, 15 for `INSTR_SNARE`, 12 for `INSTR_HIHAT`) is therefore replaced under those profiles. `Legacy` keeps the preset's period, because it never arms the LUT. `setVoiceNesNoiseLutMode(slot, 255)` opts out of the LUT, but it opts out to the **frequency-derived** clock, not back to the preset's `noisePeriod`: the note's period is gone once a LUT index was applied.

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

The suite (`ctest`) includes `test_apu_mirror` with a **golden PCM** (`test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm`), the two `Legacy` integer references, and one golden per audible profile (§9.2): the canonical rendered track must match byte-for-byte. Regenerate a fixture **only** when the output changes intentionally and is documented in the CHANGELOG.

### 9.1 Targets and case counts

Measured 2026-10-28 on the sound-profile branch (`cmake -S . -B build -G "MinGW Makefiles" -DPR32_APU_BUILD_TESTS=ON`, `cmake --build build`, `cd build && ctest --output-on-failure` → `100% tests passed out of 15`). The case counts are Unity's summary line, each binary run from `test/`:

| Target | Cases | What it covers |
|---|---|---|
| `test_apu_core` | 210 | Core behaviour on the float core (as selected by the default profile) |
| `test_apu_core_integer` | 210 | The same source against the integer core (`PR32_APU_HAS_FPU=0`) |
| `test_audio_command_queue` | 10 | SPSC queue, capacity and the `AudioCommand` ABI pins |
| `test_audio_music_types` | 35 | Music types, `InstrumentPreset` layout, shipped presets |
| `test_apu_mirror` | 37 | Tool Suite parity, byte-for-byte against the float golden |
| `test_apu_output_filter` | 12 | E2 output chain: measured response, full-scale stability, `setHpfCutoffHz` precedence, `Legacy` one-pole, Q15 product bound |
| `test_apu_output_filter_integer` | 12 | The same suite where `Legacy` has no float body (`PR32_APU_ENABLE_LEGACY_FLOAT=0`) |
| `test_apu_wavetable_spectral` | 5 | E2/S4: the generated SAW mipmap — band limit per level, seam-free tables, the S5 alias sweep |
| `test_apu_hybrid_wavetable` | 5 | E2/T3: `Hybrid` reads the tables (level from the voice's own increment, crossfade), `Nes`/`Legacy` keep the ramp, SINE inert |
| `test_q15_smoke` | 15 | Integer/Q15 path smoke coverage |
| `test_apu_integer_golden` | 1 | The integer core against the `Legacy` integer golden (FPU build with the float body off) |
| `test_apu_integer_golden_no_fpu` | 1 | The same golden from a build with no FPU at all |
| `test_apu_profile_golden` | 4 | `Nes` and `Hybrid` against their canonical and wavetable goldens (FPU build with the float body off) |
| `test_apu_profile_golden_no_fpu` | 4 | The same four goldens from a build with no FPU at all |
| `test_library_link` | 4 | Public symbols exist in the shipped artifact (links, no `UNIT_TEST`) |
| **Total** | **565** | 0 failures, 0 ignored, exit code 0 in all fifteen |

The five continuous-duty cases added by the full-scale fix (`test_pulse_duty_full_scale_*`, `test_pulse_duty_zero_never_goes_positive`, `test_pulse_duty_interior_value_unchanged`) live in `test/test_apu_core.cpp`, so both core targets run them; that is the 205 → 210 step on the two rows above.

### 9.2 Golden PCM

Six immutable references: two for `Legacy` — one per render route, never regenerated — and two per audible profile, one for each committed scene. All are raw S16LE, no header, 22 050 samples at 44 100 Hz, rendered from `test/golden/apu_mirror_canonical_track.h` (canonical scene) or `test/golden/apu_wavetable_track.h` (wavetable scene).

| Reference | Consumed by | SHA-256 |
|---|---|---|
| `apu_mirror_canonical_s16le_44100.pcm` | `test_apu_mirror` | `7FF568A5A163A5E11E7A86DFCC0DAFEBCE9C3EE7088C88C6DE9C82293CC15DE9` |
| `apu_integer_core_canonical_s16le_44100.pcm` | `test_apu_integer_golden`, `test_apu_integer_golden_no_fpu` | `F59501D66B522CC26D7F1D48432FAF0FEF9C8ABDACA1B68A09C67D574EF3902D` |
| `apu_nes_canonical_s16le_44100.pcm` | `test_apu_profile_golden`, `test_apu_profile_golden_no_fpu` | `645DE5C756B74A52F4CDBCE0D186F4B2DE79EAF94255D1A3C54B45B77E42AA76` |
| `apu_hybrid_canonical_s16le_44100.pcm` | `test_apu_profile_golden`, `test_apu_profile_golden_no_fpu` | `163F65E2B511AD81F7489D9937309532472A91573398D1805EC300573C8B7708` |
| `apu_nes_wavetable_s16le_44100.pcm` | `test_apu_profile_golden`, `test_apu_profile_golden_no_fpu` | `A4012158A45AA076C5C8951E79334F223A18C9F7A5CF557D99B4ED5AE23890CC` |
| `apu_hybrid_wavetable_s16le_44100.pcm` | `test_apu_profile_golden`, `test_apu_profile_golden_no_fpu` | `C3FDEDCCD33940A1CDD02566FBD7C217514B1E90B3163B44E28F9F6AA0B8A419` |

One golden per affected profile, and no more (S7): E1 is the change that makes `Nes` and `Hybrid` audible, so each needs its own reference and neither may keep sharing `Legacy`'s. The two canonical profile files were rendered by `apu_golden_gen_nofpu --profile <nes|hybrid>` and independently reproduced byte for byte by `apu_golden_gen` (FPU build) and `apu_golden_gen_integer` (FPU build with the float body off): both profiles run the integer core, so the build cannot be allowed to matter.

The wavetable pair exists because the canonical track is PULSE-only and cannot observe E2's band-limited SAW (S4): `apu_hybrid_canonical_s16le_44100.pcm` is byte-identical before and after the mipmap landed. `apu_golden_gen_nofpu --scene wavetable --profile <nes|hybrid>` renders four SAW notes — 55 Hz, 220 Hz gliding through a mipmap level boundary, 880 Hz and 1 760 Hz — and the same three builds reproduce both files byte for byte. `Nes` keeps the ideal ramp, so its wavetable file is the reference the `Hybrid` one is read against; the two differ on 18 139 of 22 050 samples (max 13 181 LSB, −17.81 dBFS RMS difference), which is the evidence that the scene reaches the table read instead of describing a ramp twice.

The integer fixture is written by `python tools/golden_gen/run.py --write`, which builds three generator binaries and refuses to proceed unless the default build still reproduces the float fixture byte-for-byte and the two integer builds agree with each other. Regenerate a fixture **only** when the output changes intentionally and the change is documented in the CHANGELOG.

The E2 cost pass that followed the flash gate is bit-exact, so none of the six rows above moved. It replaced the output chain's int64 products and `/32768` divisions with int32 products and truncating shifts, and moved the coefficient derivation to `float`; `apu_golden_gen` reproduces all four profile fixtures byte for byte (0 differing samples, max |Δ| 0, RMS unchanged) and `apu_mirror_canonical` / `apu_integer_core_canonical` were not touched. The hashes above are therefore still current, verified on 2026-10-28.

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
sizeof(ApuCore)          = 13344 B   host ABI, production build (queue 64)
sizeof(ApuCore)          = 13368 B   host ABI, UNIT_TEST build
sizeof(AudioCommand)     =   144 B
sizeof(AudioChannel)     =   336 B
sizeof(InstrumentPreset) =    72 B
queue budget             = 64 x 144 B = 9216 B
```

The host figures were re-measured on 2026-10-28 after E2. E1's per-voice character state took `AudioChannel` from 328 to 336 B (+64 B on `ApuCore` over the pre-E1 13 200 / 13 224 B), and E2/S1 added the `Nes`/`Hybrid` output filter chain state: **+80 B** on both builds (two high-pass poles, five Butterworth coefficients in Q15 `int32_t` because the LPF's `b1` does not fit `int16`, eight recursion registers, the resolved corners and three flags), landing at 13 344 / 13 368 B. `AudioCommand` stayed at 144 B (see §5). The E2 cost pass (int32 products, truncating shifts, `float` coefficient derivation) changed no member, so these figures and the pin below are unchanged. The two host figures are pinned by `static_assert(sizeof(ApuCore) <= 13376, ...)` in `test_apu_core`: the test compiles with `UNIT_TEST=1`, so it asserts the 13 368 B test-build size rounded up to the next multiple of 16, and the production figure is recorded in the message next to it. The 32-bit target figure recorded before E1 was **9 800 B** (`gCoreStorage`, 0x2648, in the firmware maps of `tools/apu_bench_firmware` for `esp32dev`, `esp32-c3-devkitm-1` and `esp32-s3-devkitc-1`, identical on the three because the ABI is the same width); E1's channel growth changes it by the same 8 B per channel and E2 adds the same 80 B, so it is **not restated as current here** — the firmware bench run prints it and it is recorded with §9.5's numbers.

### 9.5 CPU and underrun baseline, firmware

Measured on 2026-10-28 with `tools/apu_bench_firmware`: `ESP32-D0WD-V3` rev 3 over `COM4` (240 MHz, `fpu=1 legacy_float=1`, 512-sample block) and `ESP32-C3` rev 4 over `COM5` (160 MHz, `fpu=0 legacy_float=0`, 512-sample block); 100 warm-up + 1000 measured blocks per cell. An unmeasured cell stays `pendiente`; it is never filled with an estimate.

| Board | Profile | 22 050 Hz, 1 / 8 voices (p50 µs) | 32 000 Hz, 1 / 8 voices (p50 µs) | Headroom at 8 voices | Underruns, 10 min |
|---|---|---|---|---|---|
| `esp32dev` | `Legacy` | 1 540 / 6 618 | 1 541 / 6 617 | 3.5x / 2.4x | **0** |
| `esp32dev` | `Nes` | 1 451 / 5 589 | 1 452 / 5 590 | 4.1x / 2.8x | **0** |
| `esp32-c3-devkitm-1` | `Legacy` | 2 329 / 7 546 | 2 331 / 7 545 | 3.1x / 2.1x | **0** |
| `esp32-c3-devkitm-1` | `Nes` | 2 329 / 7 546 | 2 331 / 7 545 | 3.1x / 2.1x | **0** |

What the table says:

- The per-block cost is rate-invariant (6.6 ms for 512 samples at either rate), as it must be: what changes with the rate is the budget (23.2 ms vs 16 ms) and therefore the headroom.
- On silicon the integer core is **1.18x faster** than the float body at 8 voices and **1.06x faster** at 1 voice. The host ratio (1.44-1.56x at 8 voices, and the integer path *slower* at 1 voice) does not transfer: the ESP32's single-precision FPU is not the bottleneck people assume.
- 32 000 Hz with 8 voices leaves **2.4x** (`Legacy`) / **2.8x** (`Nes`): viable, and the tightest configuration in the matrix — which is why 22 050 Hz stayed the default.
- **Underruns: 0 in ten minutes at both rates, on both boards.** Measured with `tools/apu_underrun_probe` (engine repository), which drives the real I2S backend with the same eight-voice recipe and each board's own block size: `esp32dev` on BCLK 26 / LRCK 25 / DOUT 22 with 512-sample blocks, `esp32-c3-devkitm-1` on BCLK 4 / LRCK 5 / DOUT 6 with 1024 (the no-FPU default). Worst whole iteration: 23 221 us at 22 050 Hz (budget 23 220) and 15 995 us at 32 000 (budget 16 000) on the ESP32; 46 438 and 31 996 us on the C3 (budgets 46 440 and 32 000). The write wait is the figure that decides an underrun: with a one-block ring a healthy write blocks for most of the period, so a starving producer shows up as a write that barely waits, not as a slow iteration. Measured write waits: 17 269 / 13 586 us on the ESP32 and 44 013 / 29 643 us on the C3.
- **The render cost is measured, not inferred.** `getWorstRenderUs()` times `generateSamples()` per iteration and keeps its own maximum. `worstBlockUs - worstWriteUs` is *not* a render cost: those are two independent maxima and need not come from the same block, so their difference can be off by multiples. Measured inside the gate: 7 195 us per 512 samples at 22 050 Hz and 6 813 at 32 000 on the ESP32 (14.1 / 13.3 us per sample); 13 969 per 1024 and 13 972 on the C3 (13.6 us per sample). Render alone leaves 3.2x / 2.3x headroom on the ESP32 and 3.3x / 2.3x on the C3, inside the same run that counted the underruns.
- **A gate over silence is not a gate.** The harness reports `peak=` (the largest absolute sample the mix produced), a `# core` line with the transport state and the dropped-command count, and aborts when the backend never iterated. All four runs reported `music=1`, `dropped=0` and a non-zero `peak`: 17 655 / 17 997 (ESP32) and 17 614 / 17 849 (C3). The counters alone cannot tell a healthy gate from a dead backend, and an earlier version of this harness reported `underruns=0` over digital silence because of exactly that.
- **The C3 rows share one body but are no longer bit-identical cells.** With `fpu=0` both profiles run the same integer body, so the two rows must agree to within the profile-dependent branches the E1 character adds (0.14 % measured); the tool prints both render macros so the reader can confirm the body rather than trust it. At 160 MHz the integer body costs 51 % more per sample than the ESP32 at one voice (2 325 vs 1 542 us) and 16 % more at eight (7 682 vs 6 620 us).
- **The tightest cell in the matrix is the C3 at 32 000 Hz with eight voices: 2.1x headroom, p99 at 48.3 % of the block** (7 724 of 16 000 us). It passes the rule in S12 (p99 must stay under half the block for a target to keep 32 kHz), which is exactly why 22 050 Hz stayed the default.
- The I2S gate is a separate harness on purpose: this bench renders into RAM and never touches a peripheral, so it cannot count underruns. It now runs on both boards at their own block size and both rates; `esp32-s3-devkitc-1` remains `pendiente` for both the CPU table and the gate. An S3 board was attached on 2026-10-28 and could not be read: its UART bridge does not enumerate on this machine (`Device Descriptor Request Failed`) and the application prints nothing on the native USB port, so the measurements were not taken. `tools/apu_bench_firmware/platformio.ini` records the attempts.

The target `sizeof` printed by that bench is not `pendiente`: `sizeof_apu_core=9864`, `sizeof_audio_command=96`, `sizeof_audio_channel=304`, `sizeof_instrument_preset=72`, `queue_bytes=6144` on the 32-bit ABI, all consistent with §9.4. The C3 prints both render macros so the body it runs is visible rather than assumed.

### 9.6 CPU cost of the E1 character

E1 gives `Nes` and `Hybrid` their audible character (exponential envelope curve, weighted NES mix, unipolar noise with the NES period LUT, stepped duty) inside the integer core. Measured on both boards with `tools/apu_bench_firmware`, 8 voices, p50 per 512-sample block; the figure is rate-invariant because it is per block, not per second:

| Board | Profile | E0 | E1 | Delta | Budget |
|---|---|---|---|---|---|
| `esp32dev` (240 MHz) | `Legacy` | 6 618 | 6 620 | +0.03 % | — (float body, untouched) |
| `esp32dev` | `Nes` | 5 589 | 5 631 | **+0.75 %** | <= +4 % |
| `esp32-c3-devkitm-1` (160 MHz) | `Legacy` | 7 546 | 7 682 | +1.80 % | — |
| `esp32-c3-devkitm-1` | `Nes` | 7 546 | 7 693 | **+1.95 %** | <= +6 % |

The first version of this table **failed the budget**: +12.5 % on the ESP32 and +9.5 % on the C3, with `Legacy` itself +5.6 % on the C3 although its waveform had not changed. The cause was the weighted mixer's three per-sample group buckets, which ran for every profile; folding the per-block-constant weight into the per-voice Q15 gain removed them. The failed figure is kept here because it is the one that shows the budget is a gate and not a formality.

The tightest cell is still the C3 at 32 000 Hz with eight voices: p99 7 723 us = **48.3 %** of the block, inside the half-block rule of S12, which the unoptimised build had broken at 51.8 %.
