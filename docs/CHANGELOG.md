# Changelog — PixelRoot32-APU

Format: [semver](https://semver.org/). Changes that alter rendered PCM or struct ABI require a major or minor bump with an explicit note.

## [Unreleased]

Sound profiles, one integer render core on every platform, a smaller command
queue, one audible fix on no-FPU builds, and the audible character of the `Nes`
and `Hybrid` profiles. **On `PR32_APU_HAS_FPU=1` builds with the default profile
the rendered PCM is unchanged** (float golden `7FF568A5…5DE9`); every sound
change lives on the integer path, and the two a consumer can hear are listed
first.

### Breaking changes

1. **The integer/Q15 DC blocker now decays instead of parking a −199 offset**
   once a voice is silenced. The recursion truncated `R·y[n-1]` towards −∞, so
   every negative `y` with `|y| < 32768/(1−R)` — about 200 with the default
   `R = 32604` — was a fixed point: after silencing a PULSE voice the output
   stayed frozen at −199 for good. It now truncates towards zero, the same
   operation the float conversion performs, so `|y|` drops at least one LSB per
   silent sample whatever the coefficient. Measured on the canonical track with
   `PR32_APU_HAS_FPU=0`: **21 982 of 22 050 samples differ** from 2.0.0,
   `max|Δ|` = **127 LSB**, and the mean of the tail (last 2 050 samples) goes
   from **−68 to 0**. The difference is the DC offset that used to stay glued,
   not a different waveform. **Affects builds with `PR32_APU_HAS_FPU=0` only**
   (ESP32-C3 and any `-DPR32_APU_HAS_FPU=0` build); desktop, ESP32 and
   ESP32-S3 render the float path and are untouched. **Any reference or
   measurement taken from the integer path before this change must be
   regenerated, not inherited.** The new immutable reference is
   `test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm`
   (`F59501D6…3902D`). This alters rendered PCM, so the policy at the top of
   this file requires a major or minor bump; **which of the two is the release
   owner's decision**, and this entry claims no version number.

### Added

- **Audio character**: `Nes` and `Hybrid` are no longer byte-identical aliases.
  Four changes, all inside the integer core they already run, while `Legacy`
  stays byte-identical on both of its routes (`7FF568A5…5DE9` float,
  `F59501D6…3902D` integer) — neither of those fixtures is regenerated.
  - **Envelope curve (P1).** `EnvelopeCurve { ProfileDefault, Linear,
    Exponential }` is the **last** member of `InstrumentPreset`, so every
    positional literal keeps its meaning and `ProfileDefault` means "the profile
    decides". `Hybrid` decays and releases exponentially
    (`level -= ((level - 1) >> 8) + 1`, floor at 0) and reaches −60 dB in the
    stage's own time ±5 %; ATTACK stays linear because a predictable attack is
    worth more than a matching curve, and a stage shorter than the shape's own
    −60 dB span keeps the linear ramp so it still ends on its target instead of
    snapping (~7 dB on the default 5 ms release). `Nes` stays linear and lets its
    existing 4-bit `nesEnvelope` at 240 Hz be the character.
  - **Weighted mix (P2a).** `MixerCurve::NesWeighted` applies the published NES
    balance before the HPF — pulses `0.00752 × (p1 + p2)`, triangle `0.00851`,
    noise `0.00494`, DMC `0.00335` — normalised to the pulse group (Q12 4096 /
    4635 / 2691 / 1825), so a full-scale pulse keeps exactly the amplitude
    `Legacy` gives it: triangle lands +1.07 dB against a pulse and noise
    −3.65 dB, and the DMC term is reserved until a DMC voice exists.
  - **Unipolar noise (P9).** `Nes`/`Hybrid` switch NOISE between 0 and the
    envelope level instead of ±level, with one exact ×2 compensation after the
    volume and envelope multiplies: −0.001 dB against bipolar at the same
    weighting, and −3.649 dB against `Legacy` — which is exactly the published
    noise weight, not a lost level. They also arm the NES period LUT (index 0,
    `$400E = $00`), and arming it retunes the period, so a preset's own
    `noisePeriod` is replaced under those profiles;
    `setVoiceNesNoiseLutMode(slot, 255)` opts out.
  - **Stepped duty (S6).** PULSE renders the four NES patterns (12.5 / 25 / 50 /
    75 %) snapped round-half-up from the note's `duty`, instead of the
    continuous comparison. A note that **animates** duty (`dutySteps`,
    `dutySweep`) keeps the continuous generator, because one static 2-bit index
    cannot express animation; `setVoicePulseDutyMode(slot, 255)` pins continuous
    duty under a NES profile.

  The profile supplies the **default** of each and the note keeps winning inside
  its own domain (ADSR, LFO, `duty`, sweep, pitch envelope, `noisePeriod`) — the
  three-level precedence of `docs/ABI.md` §3. Two new immutable goldens pin the
  result: `apu_nes_canonical_s16le_44100.pcm` (`225CE203…B18B8`) and
  `apu_hybrid_canonical_s16le_44100.pcm` (`7E77507E…72343`), each reproduced
  byte for byte by an FPU build with the float body switched off and by a
  no-FPU build, because both profiles run the same integer core. Blind A/B
  material is `tools/profiles_ab`: the same track rendered through the three
  profiles as three RMS-matched WAVs plus a separate key, so the profiles can be
  judged by ear rather than by a metric.
- **API**: `SoundProfile { Legacy = 0, Nes = 1, Hybrid = 2 }` in `AudioTypes.h`, with `ApuCore::setSoundProfile(SoundProfile)` (audio thread), `ApuCore::getSoundProfile() const` (plain read, any thread) and `AudioCommandType::SET_SOUND_PROFILE`, so the game thread selects a profile through `submitCommand()` the same way it already sends `SET_NES_OPTIONS`. Default is `Legacy`. The profile is **configuration, not state**: `init(int sampleRate)` does not reset it, while voices are still reset. An unknown value is ignored rather than clamped to a neighbouring profile. The profile selects the **render core**, once per block and not per voice: `Legacy` renders the float core on FPU builds, `Nes`/`Hybrid` render the integer core everywhere, and on a build without FPU every profile renders the integer core. The profile is the first of three precedence levels — profile < voice < note — and its character hook (`applyProfileCharacter`) runs **last** in `initVoiceFromEvent`, after `initNesSafe`, while the note keeps winning inside its own domain (ADSR times and levels, LFO, `duty`, sweep, pitch envelope, `noisePeriod`). `Legacy` remains the default; the audible character of `Nes` and `Hybrid` is the entry above.
- **Behavior**: a profile change in the middle of playback does not cut the sounding voices. `setSoundProfile` computes the destination core from its argument, syncs the envelope mirror **only when the core actually changes**, resets the output stage, and applies character to the following notes. **Float → integer is the seamless, tested direction** (`Legacy` → `Nes`/`Hybrid` on an FPU build): no consecutive-sample step above `0.25 × 32767` and no rails. `integer → float` re-frames phase and duty to their note-on values, so it is a **note-boundary change**: switch between notes, or with the voices silent.
- **Tools**: `tools/golden_gen` renders the canonical track with a selected profile, verifies the (never regenerated) float golden and writes/verifies the integer one (`python tools/golden_gen/run.py --write`); the two profile goldens were produced with `--profile nes` and `--profile hybrid`. `tools/apu_bench` measures the host CPU baseline (p50/p99 per block, µs per sample, the `{Legacy, Nes} × {22 050, 32 000} × {1, 8}` matrix) and `tools/apu_bench_firmware` does the same on a board with `esp_timer_get_time()`. `tools/profiles_ab` renders one excerpt through the three profiles and writes three 16-bit mono 44 100 Hz WAVs matched to the same RMS, under neutral names with the key in a separate file, so the A/B is blind. The integer golden added here, `apu_integer_core_canonical_s16le_44100.pcm` (44 100 B, `F59501D6…3902D`), is produced by two independent integer builds that must agree byte-for-byte, and the tool refuses to write it unless the default build still reproduces the float golden.
- **Audio character, E2 (P3 + P4)**: `Nes` and `Hybrid` gain the NES output
  stage and `Hybrid` gains a band-limited SAW. `Legacy` is untouched on both of
  its routes, and neither `Legacy` fixture is regenerated.
  - **Output chain.** HPF 90 Hz, HPF 440 Hz and a second-order Butterworth
    low-pass at `min(14 kHz, 0.45 · fs)`, applied once to the summed mix where
    the historical one-pole high-pass used to sit. Coefficients are derived on
    `init(rate)`, `setSoundProfile` and `setHpfCutoffHz` — never per sample and
    never per block. The low-pass is skipped when `0.45 · fs < 6 kHz`, because
    at 8 kHz that corner is 3.6 kHz and would gut the passband instead of
    approximating the 14 kHz roof.
  - **Precedence.** `setHpfCutoffHz` is the caller's explicit knob and outranks
    the profile: once called, the profile does not install its chain and the
    historical one-pole keeps running at that corner. `reset()` clears the
    override. This mirrors the `pulseDutyOverridden` / `noiseUnipolarOverridden`
    convention of the character work above.
  - **Level trim: switching profile is no longer a loudness change — and the
    first attempt at it was wrong.** The chain left `Nes` at **−22.98 dBFS** and
    `Hybrid` at **−24.01 dBFS** RMS on the canonical track, against the integer
    `Legacy` anchor's **−19.70 dBFS**. The trim that ships is matched on
    **crest-weighted loudness, `0.75 × RMS dBFS + 0.25 × peak dBFS`**, a
    documented heuristic rather than a standard: **+1.10 dB** (`Nes`,
    37192/32768) and **+1.90 dB** (`Hybrid`, 40780/32768), applied after the
    chain and before the master bitcrush, resolved once per configuration
    change with the chain's activation.
    **The failed first attempt is part of the record.** It matched raw RMS
    instead: +3.28 dB and +4.31 dB, landing both profiles at −19.70 dBFS, within
    0.01 dB of `Legacy`. It measured perfect and it was wrong — on hardware the
    user reported `Nes` and `Hybrid` as **louder and clearer than `Legacy` in
    every sample**. The crest factors are why: 13.3 / 14.3 dB for the chain
    against 4.6 dB for `Legacy`, so an RMS match lifts every transient by 3–4 dB
    and the ear reads that as more power. Equal RMS is only a proxy for equal
    loudness when the crest factors are comparable. A metric can pass while the
    sound is wrong; that is why the test now asserts the weighted figure, prints
    RMS/peak/crest beside it, and separately forbids RMS from rising above
    `Legacy`'s.
    **Where it landed.** Measured on the canonical track, `Nes` reads
    −18.554 weighted against `Legacy`'s −18.549 (**−0.005 dB**) and `Hybrid`
    −18.528 (**+0.021 dB**), with RMS **2.18 / 2.41 dB below** `Legacy` — between
    the un-trimmed state and the rejected RMS-matched one, which is the intended
    zone. Canonical peaks are now **12 210** (`Nes`, 37 % of full scale) and
    **13 370** (`Hybrid`, 41 %) against `Legacy`'s 5 758 (18 %); on the listening
    demo's denser `complex` piece they land near **16 960** and **16 620**
    (about half scale), against the rejected attempt's ≈21 800 / ≈21 950. Still
    less room than `Legacy` leaves, but nothing clips: check the master level on
    a dense mix instead of assuming it. `Legacy` is the frozen anchor and stays
    at exact unity — it does not take the chain branch, so it never runs the
    multiply — and both of its fixtures are byte-identical through both
    attempts. The trim is part of the chain: `setHpfCutoffHz()` suppresses chain
    and trim together, because that caller asked for the historical one-pole,
    level included. The per-sample cost is one multiply and the clamp the loop
    already performs, on the two profiles that run the chain; `Legacy`'s work is
    unchanged.
  - **Band-limited SAW (`Hybrid` only).** Eight 256-point Q15 tables keyed on
    `q = f / fs` (never on absolute Hz, because the band limit is Nyquist), one
    per octave, with harmonic counts 128 down to 1 taken from the band's worst
    case. The read interpolates the two adjacent points and blends the level in
    force with its upper neighbour by the note's position inside the octave, so
    a glide crossing a band edge has no step. `Legacy` and `Nes` keep the ideal
    ramp; `Nes` keeps its aliasing on purpose, and its PULSE character is the
    deliberate reproduction of it. **`SINE` keeps `SINE_LUT_Q15` on every
    profile**: a sine is one harmonic and cannot alias, which was measured
    rather than assumed, so four kilobytes of SINE tables would buy nothing.
  - **Goldens.** The two canonical profile fixtures were regenerated, because
    the chain changes both profiles. Two new fixtures
    (`apu_{nes,hybrid}_wavetable_s16le_44100.pcm`) pin the `Hybrid` SAW end to
    end, because the canonical track is PULSE-only and would not have seen it:
    a 220 → 900 Hz glide crosses two band edges inside a single note.
  - **Measured cost, classic ESP32, p99 per 512-sample block at 22 050 Hz.**
    `Legacy` flat (−0.1 %), `Nes` 8 voices **+5.10 %**, `Hybrid` 8 voices
    **+6.98 %** — the profile that carries both changes misses the +6 % budget by
    one point at 26 % of the block, and the level trim is what pushed it there. Flash **+8 692 B** of the 8 704 B allowance: 12 B of margin, which is a warning rather than an achievement.
    The C3 was not measurable: its native USB port delivers no application
    output with either core USB mode, and a five-line probe sketch is equally
    silent, so this is a tooling limit and not this change. `docs/ABI.md` §9.7
    carries the full table, the two optimisations that were measured and
    rejected, and what the last one cost in alias margin (18.52 → 13.71 dB).
- **Tools**: `tools/mipmap_gen` generates and verifies the SAW tables
  (`python tools/mipmap_gen/generate.py --write|--check`); `tools/golden_gen`
  gained `--scene <canonical|wavetable>`, defaulting to `canonical`, so every
  pre-existing invocation renders byte-identically.

### Changed

- **ABI (size)**: `AUDIO_COMMAND_QUEUE_CAPACITY` default drops from **128 to 64**, range **32–64** (the ceiling is the 9 KB RAM budget, not throughput: 64 × 144 B = 9 216 B, effective depth 63). Drop policy is unchanged — a full queue still discards the newest command and increments `droppedCommands`. Consequence for the largest consumer type: `sizeof(ApuCore)` goes from **22 416 to 13 264 B** on the host ABI (13 288 B with `UNIT_TEST`). The per-voice NES character adds 8 B to `AudioChannel` (**336 B**), which is the +64 B over the pre-character 13 200 / 13 224 B and is still inside the 13 312 B pin. The 32-bit target figure was 9 800 B before the character work and is re-measured with the firmware bench rather than restated. `sizeof(AudioCommand)` stays at **144 B**: the new `soundProfile` byte fitted in the existing padding before `seekOffsetTicks`, and the seventh `VoiceNesOptions` byte made `nesLengthHalt` move into the same padding group instead of pushing every slot to 152 B. E2's output-filter state raises `sizeof(ApuCore)` again to **13 344 B** (13 368 B with `UNIT_TEST`), **+80 B**, and its pin moves from 13 312 to **13 376 B**. `sizeof(AudioChannel)` and `sizeof(AudioCommand)` do not move: the per-sample mipmap level and crossfade weight are derived from `phaseIncQ32` instead of stored.
- **Build**: `PR32_APU_ENABLE_LEGACY_FLOAT` (default `PR32_APU_HAS_FPU`) controls whether the float body of `generateSamples` is compiled; the integer body is always compiled. With the default value nothing changes for a consumer that defines nothing. `-DPR32_APU_ENABLE_LEGACY_FLOAT=0` is the supported flash lever on an FPU target: the float body disappears and `Legacy` falls back to the integer core. A build that keeps both bodies pays the second body in flash (budget for `esp32dev`: ≤ 8 KB of `.text` over the base).
- **Engine-side (consumer contract, PixelRoot32 Game Engine)**: the audio render block moves from 256/128 samples to **512 (FPU) / 1024 (no FPU)**, with `PR32_AUDIO_MAX_BLOCK_SAMPLES = 1024`. The I2S and DAC backends derive `dma_buf_count = clamp(blockSize / 128, 2, 8)` over a fixed `dma_buf_len = 128`, which puts the objective latency at **23.2 ms** (dual-core) / **46.4 ms** (single-core) at 22 050 Hz and **16 / 32 ms** at 32 000 Hz. Because the block is the render granularity, **a queued command can now be applied up to one render block later**; that is the price of the lower wake-up rate, not a queue regression.

### Fixed

- **Integer path only: a 100 % PULSE duty now renders a constant high level
  instead of a flat line.** `set_pulse_duty` mirrored the clamped float duty
  into Q32 as `(uint32_t)(duty * 2^32)`. At exactly `duty == 1.0` the product
  is 2^32, which does not fit in `uint32_t`, so the conversion yielded 0 and the
  integer generator's `phaseQ32 < dutyCycleQ32` was never true — while the
  float body compares against the float `dutyCycle` (1.0) and was always
  correct. The mirror now saturates to `0xFFFFFFFF` at that boundary; **every
  other `duty` keeps the identical expression**, so the rest of the PCM is
  byte-identical and no fixture was regenerated: the float golden
  (`7FF568A5…5DE9`) and the integer golden (`F59501D6…3902D`) still reproduce
  byte for byte, and `Nes`/`Hybrid` still render the integer reference. The
  input that changes is exactly `duty == 1.0` on the integer path — a no-FPU
  build, or `Nes`/`Hybrid` on any build. No version is claimed here: the bump
  is the release owner's decision, as with the DC-blocker fix above.

### Migration

- Consumers on `PR32_APU_HAS_FPU=0` targets that calibrated levels or wrote PCM references against the integer path must regenerate them: the path no longer leaves a −199 DC offset behind a silenced voice.
- Consumers that sized a buffer from `AUDIO_COMMAND_QUEUE_CAPACITY` should read it rather than hard-coding 128; the default is now 64, and more than 63 outstanding commands between consumer passes will be dropped.
- Consumers that selected `Nes` or `Hybrid` while this release sat unreleased should re-check their levels: the mix balance changed on purpose (a triangle is +1.07 dB against a pulse, noise −3.65 dB), `Hybrid` envelopes now decay exponentially, PULSE duty snaps to the nearest NES step, and noise percussion follows the NES period LUT instead of the preset's own `noisePeriod`. No released version ever exposed those profiles as aliases, so there is nothing to migrate from.
- Consumers already using `Nes`/`Hybrid` must re-check their levels **again** after E2: the output chain takes the canonical track from -19.7/-20.7 dBFS to -23.0/-24.0 dBFS RMS while nearly doubling the peak, because the 440 Hz high-pass removes low-mid energy and the two cascaded high-passes spike on every edge. E2 ships a fixed per-profile trim matched on crest-weighted loudness (0.75 x RMS + 0.25 x peak) to the `Legacy` anchor, so the profiles end up 2.2/2.4 dB BELOW `Legacy` in RMS while level with it on the weighted figure - deliberately, because an earlier RMS-matched trim (+3.28/+4.31 dB) measured equal and sounded louder on hardware. Peak growth is reduced but not gone: `Nes`/`Hybrid` peak at ~37 %/41 % of full scale on the canonical track and ~52 %/51 % on a dense mix, against `Legacy`'s 18 %/36 %. Anything calibrated against a `Nes`/`Hybrid` render before this release - including recorded references and loudness budgets - must be re-measured, not inherited.
- Consumers on FPU builds that set `setHpfCutoffHz()` to a custom corner keep exactly that one-pole filter under every profile: the profile chain does not install over an explicit corner, and `reset()` clears the override. That is the escape hatch if the NES chain is not wanted on a specific build.
- Nothing else changes: the default profile is `Legacy`, and on FPU builds it still renders the same bytes as 2.0.0.

## [2.0.0] - 2026-08-08

NES-accurate APU emulation (Hitos 0-5), a configurable output stage, and the
unification of the two render paths.

### Breaking changes

Three, and only the first is audible to existing consumers on FPU-capable
targets:

1. **TRIANGLE is quantised to 4-bit NES levels by default** (Hito 1 M1). Opt out
   per voice with `voice.triangleQuantize4Bit = false`. The golden PCM was
   regenerated for this.
2. **The integer/Q15 render path renders ~4.4x louder** (Hito 5 M12), matching
   the float path. Affects FPU-less targets only — ESP32-C3 and any build with
   `-DPR32_APU_HAS_FPU=0`. Desktop, ESP32 and ESP32-S3 are unchanged. Consumers
   on those targets should re-check their master volume.
3. **The NOISE LFSR seeds to `0x7FFF`** instead of `0x4000` (Hito 1 M9), which
   is the NES-equivalent state after a `$400F` write. Changes the first samples
   of every NOISE event.

Everything else added in this release is opt-in and default-off, so a consumer
that upgrades and touches nothing keeps its previous behaviour apart from the
three items above.

`InstrumentPreset` gained fields but its first 17 positions still mean exactly
what they meant in 1.0.1, so positional aggregate initializers keep working
untouched — see the `noiseShortMode` entry under Fixed for why that is load
bearing.

### Added
- **API**: NES APU frame counter (Hito 2 M3). Adds the `pixelroot32::audio::NesFrameCounter` struct (mode, currentStep, APU-cycle accumulator, IRQ flag, inhibit flag, reset-pending) and the `nes_apu` namespace in `ApuConfig.h` with the canonical 4-step / 5-step cycle tables (`kMode0ApuCyclesPerStep`, `kMode1ApuCyclesPerStep`, `kNesApuHzNtsc`). New public methods on `ApuCore`: `setNesFrameCounterMode(int)`, `setNesFrameCounterInhibit(bool)`, `getNesFrameCounterIrq()`. Default OFF — the counter does not tick and the canonical track PCM is unchanged until the consumer calls `setNesFrameCounterMode(0 or 1)`. Four private stub methods (`tickAllNesEnvelopes`, `tickAllNesLinearCounters`, `tickAllNesLengthCounters`, `tickAllNesSweepUnits`) plus a private per-sample `tickNesFrameCounter(apuCyclesPerSample)` wired into both the float and Q15 hot paths. Future Hitos (M4 length counter, M5 linear counter, M6 sweep, M7 envelope) fill in the stubs.
- **Tests (UNIT_TEST)**: 8 new Hito 2 M3 tests in `test_apu_core.cpp` covering initial state, mode-set reset, quarter/half clock rates (240/120 Hz mode 0), mode-1 no-IRQ, IRQ rate (60 Hz), IRQ inhibit, and APU-cycle timing at 22050 Hz. 7 new test-only accessors expose the internal counters under `#if defined(UNIT_TEST)`.
- **API**: NES APU length counter (Hito 2 M4). Adds the `pixelroot32::audio::NesLengthCounter` struct (enabled, halt, index, counter) — appended as the LAST member of `AudioChannel` to keep aggregate initializers in test literals positionally stable. Adds `nes_apu::kNesLengthLut[32]` (the canonical nesdev wiki table) in `ApuConfig.h`. New public methods on `ApuCore`: `setVoiceNesLength(int slot, uint8_t lengthIndex, bool halt)` (mirrors the `$4003/$4007/$400B/$400F` length load, counter is initialised to `kNesLengthLut[index]` and silenced on reaching 0) and `setVoiceNesChannelEnabled(int slot, bool enabled)` (mirrors `$4015`: clear forces counter to 0 and silences the voice; set has no immediate effect). The existing M3 `tickAllNesLengthCounters()` stub is filled with the per-half-clock decrement + silence logic; SINE/SAW voices keep `nesLengthCounter.enabled = false` permanently and are skipped. Default OFF — the canonical track PCM remains byte-for-byte identical. `remainingSamples` is intentionally NOT touched (M4 is orthogonal to the pre-Hito-2 duration mechanism).
- **Tests (UNIT_TEST)**: 8 new Hito 2 M4 tests in `test_apu_core.cpp` covering initial state, load, decrement on half-clock, silence at zero, halt prevents decrement, LUT values (compile-time `static_assert` + runtime check), channel-enable-off forces silence, and SINE/SAW voices are not affected. 3 new test-only accessors (`getVoiceNesLengthCounterForTesting`, `getVoiceNesLengthIndexForTesting`, `getVoiceNesLengthHaltForTesting`) gated on `#if defined(UNIT_TEST)`.
- **API**: NES APU linear counter (Hito 2 M5). Adds the `pixelroot32::audio::NesLinearCounter` struct (linearEnabled, reloadFlag, reloadValue, counter, lengthCounter pointer) — appended as the LAST member of `AudioChannel` to keep aggregate initializers in test literals positionally stable. The `lengthCounter` pointer is bound in `AudioChannel::reset()` to share the halt/control flag with `NesLengthCounter` (no duplication). Fills the existing M3 `tickAllNesLinearCounters()` stub: at quarter-frame rate (240 Hz NTSC mode 0), if `reloadFlag` is set the counter is loaded from `reloadValue`, else if `counter > 0` it is decremented; if the shared `halt` flag is clear, `reloadFlag` is then cleared. The TRIANGLE channel is gated per-sample in both the float and Q15 hot paths: output is 0 and the phase does NOT advance when the AND of (length counter open) and (linear counter open) is closed; either sub-unit, when its `enabled` flag is false, is treated as always-open. Surgical fix to the M4 `tickAllNesLengthCounters()`: TRIANGLE voices are no longer disabled when their length counter hits 0 — the M5 per-sample gate handles the silence path, and the voice stays enabled so it can be re-armed by a fresh $400B write. New public methods on `ApuCore`: `setVoiceNesLinearCounter(int slot, uint8_t reloadValue, bool reloadFlag)` (mirrors the $4008 write side effect, masks the high bit, stores regardless of `linearEnabled`) and `setVoiceNesLinearEnabled(int slot, bool enabled)` (mirrors the $4008 bit 7 enable). Default OFF — `nesLinearCounter.linearEnabled == false` on construction, so the counter does not tick and the canonical track PCM is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 10 new Hito 2 M5 tests in `test_apu_core.cpp` covering initial state (all voices default to OFF), load on quarter-clock, decrement at quarter-clock rate, hold with control set, reloadFlag cleared without control, TRIANGLE gate open by default (safety net), TRIANGLE gate closes when length counter hits 0 (verifies the M5 surgical fix keeps the voice enabled), TRIANGLE gate closes when linear counter is 0, TRIANGLE phase does NOT advance when the gate is closed, and SINE voices are unaffected by the linear counter dispatch. 4 new test-only accessors (`getVoiceNesLinearCounterForTesting`, `getVoiceNesLinearReloadValueForTesting`, `getVoiceNesLinearReloadFlagForTesting`, `getVoiceNesLinearEnabledForTesting`) gated on `#if defined(UNIT_TEST)`, plus a new `getVoicePhaseForTesting` for the phase-freeze test.
- **API**: NES APU sweep unit (Hito 2 M6). Adds the `pixelroot32::audio::NesSweepUnit` struct (sweepEnabled E bit, period P, negate N bit, shift SSS, reloadFlag, dividerCounter, isPulse2, 11-bit timer, continuously-evaluated `muted` flag) — appended as the LAST member of `AudioChannel` to keep aggregate initializers in test literals positionally stable. Adds `nes_apu::nesTimerToHz(uint16_t)` and `nes_apu::hzToNesTimer(float)` helpers in `ApuConfig.h` (PULSE NTSC formula `f = 111860.8 / (timer + 1) Hz`). New public methods on `ApuCore`: `setVoiceNesSweep(int slot, bool enabled, uint8_t period, bool negate, uint8_t shift, bool isPulse2)` (mirrors the `$4001`/`$4005` write side effect: clamps P/SSS to 3 bits, sets `reloadFlag = true`, immediately recomputes `muted` so the per-sample gate sees the correct value before the next half-clock) and `setVoiceNesTimer(int slot, uint16_t timer11)` (mirrors the `$4002`/`$4006`/low-3-bits-of-`$4003`/`$4007` timer load: clamps to 11 bits, re-syncs the float / Q32 phase increments so the oscillator frequency matches `111860.8 / (timer + 1) Hz` immediately, recomputes `muted` immediately). The existing M3 `tickAllNesSweepUnits()` stub is filled with the canonical NES half-clock algorithm: per PULSE voice, compute the target period (PULSE 1 uses ones' complement `target = period - change - 1`, PULSE 2 uses two's complement `target = period - change`, add mode is `target = period + change`), evaluate muting CONTINUOUSLY (the "NES bug": `muted = (period < 8) || (target > 0x7FF)`, checked on the raw target BEFORE clamping), update `timer` from the clamped target on the `dividerCounter == 0` boundary when E=1, SSS>0, and `!muted`, then reload the divider to P+1 (with `reloadFlag` consumed). The PULSE output gate is added to both the float and Q15 hot paths: when `nesSweepUnit.muted` is set, the per-sample output is forced to 0 (the phase still advances, matching the NES bug semantics). Non-PULSE voices are skipped by the half-clock dispatch but their `nesSweepUnit` is still readable for introspection. Default OFF — `sweepEnabled = false` and `timer = 0` on construction, the canonical track never enables NES mode, so the rendered PCM is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 10 new Hito 2 M6 tests in `test_apu_core.cpp` covering initial state (all 8 voices default to OFF), PULSE 1 setup + target calculation (timer 256 → 384 after 1 half-clock), PULSE 1 ones'-complement negate (timer 20 → 9, the `target = period - change - 1` path), PULSE 2 two's-complement negate (timer 20 → 10, the `target = period - change` path — exactly 1 higher than PULSE 1), target-overflow muting (timer 1500 + shift 1 → target 2250 > 0x7FF, output silent in the HPF-decayed region), current-period-too-low muting (timer 4 < 8 → muted immediately, output silent), divider reload sequence (P=2: tick 1 → 3, tick 2 → 2, tick 3 → 1, exercising the canonical "reload without decrement on the boundary tick" behaviour), TRIANGLE/NOISE immunity (half-clock dispatch skips non-PULSE voices), reloadFlag cleared after one half-clock (verified by observing the divider decrement on the next tick instead of reloading), and THE NES BUG: muting persists when E=0 and SSS=0 (timer 4 with everything off still gates the output). 6 new test-only accessors (`getVoiceNesSweepTimerForTesting`, `getVoiceNesSweepDividerForTesting`, `getVoiceNesSweepEnabledForTesting`, `getVoiceNesSweepNegateForTesting`, `getVoiceNesSweepShiftForTesting`, `getVoiceNesSweepMutedForTesting`) gated on `#if defined(UNIT_TEST)`.
- **API**: NES APU envelope unit (Hito 2 M7). Adds the `pixelroot32::audio::NesEnvelope` struct (envelopeEnabled, startFlag, constVolume, volume, dividerCounter, decayLevel, output, lengthCounter pointer) — appended as the LAST member of `AudioChannel` to keep aggregate initializers in test literals positionally stable. The `lengthCounter` pointer is bound in `AudioChannel::reset()` to share the loop/halt flag with `NesLengthCounter` (same pattern as M5's linear counter, no duplication). Fills the existing M3 `tickAllNesEnvelopes()` stub: at quarter-frame rate (240 Hz NTSC mode 0), the canonical nesdev wiki algorithm runs (`if startFlag → decayLevel=15, divider=volume; else if divider==0 → divider=volume, if decayLevel>0 then --decayLevel, else if loop then decayLevel=15 else silence; else --divider`; output = `constVolume ? volume : decayLevel`). PULSE and NOISE voices opt in via `setVoiceNesEnvelopeEnabled`; the dispatch loop skips other types. When opted in, the envelope's `output` (0..15) REPLACES the ADSR `envelope.currentLevel` as the per-sample volume source (float path: `output / 15.0`; Q15 path: `output * 2184 = output * 32768/15`). Surgical fix to the M4 `setVoiceNesLength()`: after loading the length counter (and only if the NES channel is enabled), `nesEnvelope.startFlag = true` — mirrors the NES `$4003/$4007/$400F` length-load side effect. New public methods on `ApuCore`: `setVoiceNesEnvelope(int slot, bool loop, bool constVolume, uint8_t volume)` (clamps VVVV to 4 bits, mirrors `$4000/$4004/$400C`, does NOT set the start flag) and `setVoiceNesEnvelopeEnabled(int slot, bool enabled)` (mirrors the opt-in bit). Default OFF — `envelopeEnabled == false` on construction, so the quarter-clock dispatch is a no-op and the canonical track PCM remains byte-for-byte identical. `remainingSamples` is intentionally NOT touched (M7 is orthogonal to the pre-Hito-2 duration mechanism).
- **Tests (UNIT_TEST)**: 10 new Hito 2 M7 tests in `test_apu_core.cpp` covering initial state (all 8 voices default to OFF, all NesEnvelope fields zero, lengthCounter pointer bound), setup + decay over quarter-clocks (start flag → decayLevel=15, divider=8, decrements 8→0, then wrap-and-decrement), loop (decay wraps to 15 instead of silencing, voice stays enabled), constant volume (C=1, output=VVVV regardless of decayLevel), silence at zero (V=0, decayLevel decrements every Q-clock, voice silenced when it hits 0), start flag cleared after first Q-clock (verifies the M4 surgical fix), TRIANGLE/SINE/SAW immunity (dispatch skips non-PULSE/NOISE), volume scaling (V=15 → ~1.0, V=7 → ~7/15 on the int16 output), coexistence with M4 length counter (length counter wins the race to silence), and shared loop flag (length counter `halt` controls the envelope's loop, even when `setVoiceNesEnvelope` was called with loop=false). 6 new test-only accessors (`getVoiceNesEnvelopeOutputForTesting`, `getVoiceNesEnvelopeDecayLevelForTesting`, `getVoiceNesEnvelopeDividerForTesting`, `getVoiceNesEnvelopeStartFlagForTesting`, `getVoiceNesEnvelopeEnabledForTesting`, `getVoiceNesEnvelopeConstVolumeForTesting`) gated on `#if defined(UNIT_TEST)`.

- **API**: `VoiceNesOptions` + `ApuCore::setVoiceNesOptions(int slot, const VoiceNesOptions&)` / `getVoiceNesOptions(int slot)` and `AudioCommandType::SET_NES_OPTIONS` (Hito 4 M14). Bundles every per-voice NES opt-in — `lengthCounterEnabled` (M4), `linearCounterEnabled` (M5), `envelopeEnabled` (M7), `sweepUnitEnabled` (M6), `pulseDutyIndex` (M2) and `noiseLutIndex` (M8) — so a caller can put a voice into or out of NES mode in one atomic write instead of five calls that could be observed half-applied. All defaults are off, so a default-constructed instance disarms a voice completely. Two entry points with one semantic, the same shape as M13: the setter mutates the voice pool directly and is audio-thread only, while `SET_NES_OPTIONS` carries the struct through the queue and applies it via that same setter, which is what makes the configuration reachable from the game thread at all. The setter delegates to the individual setters rather than writing fields itself, so their validation and side effects apply unchanged — invalid selector indices land on the documented 255 sentinels, and selecting a noise LUT entry retunes a live NOISE voice immediately. `getVoiceNesOptions` reads the live fields, so it also reflects changes made through the individual setters or by M17's `initNesSafe`. Rendered PCM is unchanged and the golden PCM fixture is byte-for-byte identical.
- **Note**: the struct deliberately omits `noiseLfsrShort`, which the original M14 sketch listed. That flag is owned per NOTE by `InstrumentPreset` and rewritten by `initVoiceFromEvent` on every trigger, unlike the per-SLOT modes in the struct; including it would give one field two owners and the preset would silently win on the next note. The sketch also predated M6's `sweepUnitEnabled` (which it lacked) and the Hito 0 rename of `noiseShortMode` to `noiseLfsrShort`.
- **Tests (UNIT_TEST)**: 9 new Hito 4 M14 tests in `test_apu_core.cpp` covering defaults, round-trip, the struct writing the real sub-unit fields rather than a shadow copy, full disarm, invalid indices falling back to the sentinels, the queued command landing on identical state to the direct call, an out-of-range slot being ignored without touching any voice, the noise LUT retuning immediately, and the options reflecting M17's boundary (sub-units cleared by `initNesSafe`, waveform mode selectors preserved).
- **API**: configurable master soft-clip (Hito 4 M11). Adds `SoftClipMode { None, Rational, Tanh, HardClip }` in `AudioTypes.h` and `ApuCore::setSoftClipMode(SoftClipMode)`. The curve shapes the summed mix BEFORE the DC blocker, so the filter sees its output — that ordering is what makes `HardClip` model the NES signal chain, where the DAC saturates and the analog high-pass then filters the already-squared-off signal. `Rational` (`S / (1 + |S| * MIXER_K)`) is the default and reproduces the historical behaviour bit-for-bit: on the integer path it specifically keeps using the pre-fitted `audio_mixer_lut` rather than recomputing the same curve, so the canonical golden PCM does not move. The other three modes are computed on both paths; the integer versions feed the curves `x = sum * 0.4` to match the input convention baked into the LUT, with the tanh approximation evaluated in int64 because `x^2` overflows int32 once several voices sum at full scale. `reset()` restores the default, as it already does for the master volume, the bitcrush depth and the HPF corner. Unknown enum values leave the current curve untouched. Rendered PCM is unchanged for existing consumers and the golden PCM fixture is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 8 new Hito 4 M11 tests in `test_apu_core.cpp` plus 4 in `test_q15_smoke.cpp`. Two measurement regimes, because they answer different questions: below unity (one voice, HPF bypassed) no clamp engages, so each curve's level is a direct read-out of its formula and pins both the coefficients and the fixed-point scale — 13107 / 10922 / 12519 for None / Rational / Tanh at S = 0.4, with HardClip a no-op there; above unity (four loud voices, asymmetric duty, HPF running) the final int16 clamp flattens every curve to the same peak, so the tests compare mean magnitude instead and assert the ordering None > Rational > Tanh > HardClip. Note that ordering: `HardClip` caps the mix at 1.0 while the rational curve asymptotes toward `1/MIXER_K = 2.0`, which makes HardClip the *quietest* shaped mode on an overdriven mix, not the loudest.
- **API**: configurable master HPF (Hito 4 M10). Adds `ApuCore::setHpfCutoffHz(float cutoffHz)`. The output stage runs a single-pole DC blocker `y[n] = x[n] - x[n-1] + R*y[n-1]`; `R` used to be the hardcoded literal `0.995f` (and `32604` on the Q15 path), which pins the COEFFICIENT rather than the corner frequency — that is ~35.2 Hz at 44100 Hz but ~17.6 Hz at 22050 Hz, so the filter silently changed character with the sample rate. The cutoff is now stored in Hz and `R = exp(-2*pi*fc/fs)` is derived from it and the current rate, recomputed by `init()` when the rate changes and clamped strictly inside (0, 1) so the recursion cannot go unstable. `cutoffHz <= 0` bypasses the filter entirely, with the filter state kept coherent so re-enabling does not step from a stale value. Reference points: ~35 Hz is this library's historical corner, ~90 Hz approximates the NES's analog output HPF. Until the setter is called the APU keeps the historical coefficient bit-for-bit — deliberately NOT a computed 35 Hz, which would differ in the last bits and move the canonical golden PCM. `reset()` restores that legacy default, the same way it already restores the master volume and bitcrush depth. Applied to both the float and Q15 hot paths. Rendered PCM is unchanged for existing consumers and the golden PCM fixture is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 9 new Hito 4 M10 tests in `test_apu_core.cpp` plus 3 in `test_q15_smoke.cpp`. Coverage: the untouched default holding the exact legacy coefficient, coefficient derivation at 90/240 Hz, the same corner surviving a sample-rate change (with the legacy coefficient shown NOT to, as contrast), `init()` recomputing on a rate change, bypass on zero/negative cutoff, `reset()` restoring the legacy default, extreme cutoffs staying stable, and two behavioural tests measuring DC directly on a 12.5%-duty pulse — bypass keeps the offset (~5900 at 44100) while every active corner removes it and a higher corner settles it faster (~1970 / ~760 / ~410 at 35 / 90 / 240 Hz). The Q15 path gets its own DC test because `test_apu_core` compiles with the FPU path and would never exercise the integer branch.
- **API**: NES NOISE period LUT (Hito 3 M8). Adds `nes_apu::kNesNoisePeriodLutNtsc[16]` (the canonical `$400E` NTSC table, in CPU cycles) and `nes_apu::nesNoisePeriodToSamples(uint8_t index, int sampleRate)` in `ApuConfig.h`, plus `AudioChannel::nesNoiseLutIndex` (default 255) and `ApuCore::setVoiceNesNoiseLutMode(int slot, uint8_t lutIndex)`. Index 0..15 selects a LUT entry as the LFSR shift period instead of deriving it from the note frequency; 255 keeps the legacy behaviour and 16..254 are stored as 255. The table is kept in CPU cycles and converted at the current sample rate (`samples = cycles * sampleRate / 1789773`, floored at 1 sample), so the same index yields the same timbre at 22050, 44100 and 48000 Hz rather than being baked for one rate. The first few entries fall below one output sample at every supported rate and collapse onto the 1-sample floor — an inherent limit of a sample-based LFSR against the NES's ~447 kHz maximum noise clock, documented at the helper. Mirrors M2's `pulseDutyIndex` in both sentinel scheme and lifetime: a per-slot mode that survives new notes and is cleared only by `Voice::reset()`, applied on top of the per-note `noisePeriod`/frequency paths in `initVoiceFromEvent` and re-applied immediately by the setter so a live change is audible without waiting for the next note. Deliberately NOT reset by M17's `initNesSafe`, which resets the NES sub-units (length, envelope, sweep, linear counters) and not the waveform mode selectors — same treatment `pulseDutyIndex` already gets. Default OFF, so the canonical golden PCM is byte-for-byte identical.
- **Docs**: corrected the NOISE LUT in the `§M8` table of the NES comparison document (that document is no longer versioned in this repository). It listed `..., 508, 1014, 2034, 4068, 8136`, which drops 762, mistypes 1016 as 1014 and shifts the tail up one entry. Verified against https://www.nesdev.org/wiki/APU_Noise; the canonical NTSC table is `4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068`.
- **Tests (UNIT_TEST)**: 9 new Hito 3 M8 tests in `test_apu_core.cpp` — the LUT values pinned against the wiki table plus the sample conversion at two rates and its clamping, default-off keeping the legacy period, the setter retuning immediately, the mode surviving a new note, the sentinel restoring legacy behaviour, invalid indices falling back to off, non-NOISE voices left playing untouched, the mode surviving a `nesAccurate` note, and the LFSR clock landing on the same Hz at 22050 and 44100. New test-only accessor `getVoiceNesNoiseLutIndexForTesting`.
- **API**: `InstrumentPreset::nesAccurate` (Hito 2 M17, default false, appended as the LAST member so the shipped `INSTR_*` presets and the positional test literals stay valid). When true, `initVoiceFromEvent` runs a new internal `initNesSafe(Voice&, WaveType)` helper that resets the voice's NES sub-unit fields to the canonical silent state from the nesdev `APU_basics` `@regs` array — PULSE `$4000=$30 $4001=$08 $4002=$00 $4003=$00`, TRIANGLE `$4008=$80 $400A=$00 $400B=$00`, NOISE `$400C=$30 $400E=$00 $400F=$00`. Decoding the shared `$30` (DDLC VVVV) gives L=1, C=1, VVVV=0, so the length counter is halted and the envelope is constant-volume zero: silent, and nothing counts down on its own. `$08` (EPPP NSSS) gives E=0, P=0, N=1, SSS=0 and is applied to PULSE only (the other channels have no sweep unit). The envelope start flag is raised as the `$4003`/`$400B`/`$400F` side effect, while the linear counter reload flag is explicitly cleared, since `$400B` carries a zero reload value and a sticky flag would reload every quarter-clock instead of once. The helper is a clean slate, NOT an enable: `nesLengthCounter.enabled`, `nesEnvelope.envelopeEnabled`, `nesSweepUnit.unitEnabled` and `nesLinearCounter.linearEnabled` are all cleared, so callers arm what they need afterwards with the `setVoiceNes*` setters or `TRIGGER_NES_LENGTH`. SINE and SAW have no NES counterpart and are left untouched. The call sits at the very end of `initVoiceFromEvent` so no earlier branch can leave stale NES state behind. Default OFF, so the rendered PCM is unchanged for every existing preset and the golden PCM fixture is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 7 new Hito 2 M17 tests in `test_apu_core.cpp` — canonical state for PULSE, TRIANGLE and NOISE; a dirty voice returning to the canonical values on retrigger; the default path leaving the NES fields alone; SINE/SAW inert; and the safe state being genuinely silent once the envelope is armed on top of it, silenced by constant-volume-zero rather than by the decay path disabling the voice. New test-only accessor `getVoiceNesEnvelopeVolumeForTesting`.
- **API**: `AudioCommandType::TRIGGER_NES_LENGTH` (Hito 2 M13) — the queued counterpart of `setVoiceNesLength`, so a consumer can fire a NES-length-gated note from the game thread instead of computing a sample budget. Reads `AudioCommand::channelIndex` (voice slot), plus two new plain members `AudioCommand::nesLengthIndex` (LUT index, clamped 0–31) and `AudioCommand::nesLengthHalt`. Out-of-range slots are ignored. Unlike the raw setter, the command arms the voice's NES channel first (`setVoiceNesChannelEnabled(slot, true)`) before the length load: `setVoiceNesLength` silently drops writes to a disabled NES channel, and the enable setter is a direct voice-pool mutation that is not safe to call off the audio thread, so without this the queued path could never fire. This deliberately merges the `$4015` enable with the `$4003`-style length load — `TRIGGER_NES_LENGTH` is an app-level trigger, not a register write.
- **Behavior**: `setVoiceNesLength` now also raises the M5 linear counter reload flag (`nesLinearCounter.reloadFlag = true`), completing the documented side effects of a `$4003`/`$4007`/`$400B`/`$400F` write. Previously only the M7 envelope start flag was raised, so the `$400B` TRIANGLE behavior (length load reloads the linear counter on the next quarter-frame clock) was never wired up — M4 deferred it to M5, and M5 did not pick it up. Like the envelope start flag it is set unconditionally; the dispatchers gate on voice type and on their own opt-in flag, so a voice that did not opt into the linear counter never observes it. No PCM change (the frame counter is OFF by default) and the golden PCM fixture is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 8 new Hito 2 M13 tests in `test_apu_core.cpp` — length load from the LUT via the queue, the command arming the NES channel on its own, both sub-unit flags raised by the command, the same two flags raised by the direct setter (one behavior, two entry points), the linear counter actually reloading from `reloadValue` on the next quarter-clock (proves the flag drives the sub-unit rather than just being set), index clamping to 31, out-of-range slot ignored without touching any voice, and an end-to-end trigger that lets the length counter silence the voice with no sample budget computed by the caller. New test-only accessor `getVoiceNesChannelEnabledForTesting`.

### Fixed
- **Build**: the entire NES public API was missing from the shipped library. A `#if defined(UNIT_TEST)` block that legitimately wraps the `*ForTesting` accessors had grown to enclose 17 production methods as well — `setNesFrameCounterMode`/`Inhibit`, `getNesFrameCounterIrq`, all four `tickAllNes*` dispatchers, every `setVoiceNes*` setter and the private per-sample `tickNesFrameCounter`. Their call sites sit outside the guard, so `libpixelroot32-apu.a` compiled cleanly and then failed to link with 13 undefined references the moment any consumer used it (reported from PixelRoot32-Tool-Suite, which links `PixelRoot32::APU`). Nothing in this repo caught it: every test target recompiles `src/ApuCore.cpp` with `UNIT_TEST=1`, so the definitions always resolved there. The guard now closes before the NES section and reopens after it, and two test-only counters that production functions were incrementing unguarded (`nesQuarterTickCountForTesting_`, `nesIrqSetCountForTesting_`) got their own guards. No behaviour change under `UNIT_TEST`; rendered PCM unchanged and the golden PCM fixture is byte-for-byte identical.
- **Tests (UNIT_TEST)**: new `test_library_link` target and suite. It is the only one that links `pixelroot32-apu` the way a consumer does instead of recompiling the source, and it is compiled deliberately WITHOUT `UNIT_TEST` (with an `#error` guard so it cannot drift). It calls the whole public surface — transport, output stage, waveform mode selectors, every NES sub-unit setter and `VoiceNesOptions` — asserting almost nothing, because what it proves is that the symbols EXIST in the artifact consumers receive. Add a call here when adding a public method. Mutation-checked: restoring the old guard boundary makes it fail to link.
- **ABI**: `InstrumentPreset::noiseShortMode` (the deprecated alias introduced by the Hito 0 rename) moved to the END of the struct, after `nesAccurate`. It had been inserted immediately after `noiseLfsrShort` at position 15, which pushed `dutySweep`, `pitchSweepEndHz` and `pitchSweepDurationSec` down one slot. Both consumers build presets with POSITIONAL aggregate initializers written against the 1.0.1 field order, so every such literal either failed to compile — a `float` landing on the new `bool` is a narrowing conversion, ill-formed in aggregate initialization — or silently rebound its trailing values. Measured blast radius before this fix: 45 literals across 7 files in PixelRoot32-Game-Engine and 6 sites in PixelRoot32-Tool-Suite, including the two factory functions behind ~65 catalog entries. With the alias last, positions 1..17 mean exactly what they meant in 1.0.1 and all of those compile and map correctly untouched (verified by compiling Engine and Tool-Suite literals verbatim against the header, without `-Wno-narrowing`). The alias is inert on `InstrumentPreset` — only `noiseLfsrShort` drives synthesis (`ApuCore.cpp`) — so moving it changes no behaviour. The shipped `INSTR_SNARE` / `INSTR_HIHAT` now set it explicitly as the trailing member to keep it in sync with `noiseLfsrShort`; the other eight presets let it default. Rendered PCM is unchanged and the golden PCM fixture is byte-for-byte identical.
- **Tests (UNIT_TEST)**: 2 new tests in `test_audio_music_types.cpp`. `test_instrument_preset_positional_layout_matches_published_abi` builds a 17-element positional literal in the exact 1.0.1 field order, with a distinctive value per trailing field so a one-slot shift is unmissable rather than hidden behind a matching default, and asserts each lands on its intended member — this is the consumer contract, and it fails loudly if anything is ever inserted mid-struct again. `test_shipped_presets_keep_deprecated_alias_in_sync` walks all ten `INSTR_*` constants asserting the alias equals `noiseLfsrShort`, plus a direct assertion that `INSTR_SNARE` / `INSTR_HIHAT` are true so an all-false regression cannot satisfy the loop; it exists because the alias no longer sits next to the field it mirrors and is now easy to forget. Three positional literals in `test_apu_core.cpp` (`kSweepPreset` ×2, `kPresetDuty50`) lost the extra element Hito 0 had forced on them — the same patch the consumers were about to be asked to apply.
- **Behavior**: the NES sweep unit (Hito 2 M6) is now opt-in per voice. `tickAllNesSweepUnits()` used to evaluate every PULSE voice regardless of whether it had entered NES mode; since `NesSweepUnit::timer` defaults to 0, the continuous muting rule `period < 8` was always satisfied, so a single call to `setNesFrameCounterMode(0 or 1)` silenced every PULSE voice still driven by the legacy float frequency path. Adds `NesSweepUnit::unitEnabled` (default false) and the public setter `ApuCore::setVoiceNesSweepUnitEnabled(int slot, bool enabled)`, matching the existing per-voice opt-in flags of M4 (`NesLengthCounter::enabled`), M5 (`NesLinearCounter::linearEnabled`) and M7 (`NesEnvelope::envelopeEnabled`). Note this is NOT the `$4001`/`$4005` E bit — that remains the `enabled` argument of `setVoiceNesSweep`. The half-clock dispatch skips unarmed voices, and `recomputeSweepMuted` forces `muted = false` for them, so the invariant "`muted` is only ever true while `unitEnabled` is true" holds and the per-sample hot path still tests `muted` alone (no extra branch). The register-write setters `setVoiceNesSweep` and `setVoiceNesTimer` deliberately do NOT arm the unit, mirroring how `setVoiceNesLength` / `setVoiceNesLinearCounter` / `setVoiceNesEnvelope` leave their own opt-in flags untouched; toggling `setVoiceNesSweepUnitEnabled` re-evaluates `muted` immediately in both directions. Rendered PCM is unchanged for existing consumers (the frame counter is OFF by default, so the defect was never reachable through the canonical track), and the golden PCM fixture is byte-for-byte identical. Consumers that already call `setVoiceNesSweep`/`setVoiceNesTimer` and expect sweep behavior MUST now also call `setVoiceNesSweepUnitEnabled(slot, true)`.
- **Tests (UNIT_TEST)**: 3 new M6 tests in `test_apu_core.cpp` — an unarmed PULSE voice stays audible once the frame counter runs (the regression itself), disarming clears the muting flag, and the register-write setters do not arm the unit. New test-only accessor `getVoiceNesSweepUnitEnabledForTesting` (distinct from `getVoiceNesSweepEnabledForTesting`, which reports the E bit). The 9 pre-existing M6 tests now arm the unit explicitly; three of them (`test_sweep_muting_target_overflow`, `test_sweep_muting_current_period_too_low`, `test_sweep_muting_persists_when_disabled`) were additionally fixed to target the SFX slot the looping PULSE event actually lands on instead of slot 0 — they had been asserting silence on a slot that was never audible, and only passed because the defect muted every PULSE voice at once.

### Changed
- **BREAKING (Behavior, no-FPU targets only)**: the integer/Q15 render path now produces the same loudness as the float path (Hito 5 M12). Two independent attenuations were stacked on that path and both are gone. First, `MIXER_SCALE = 0.4` was applied twice: once per channel before summation (`sv = (sv * 13107) >> 15`) and again inside `audio_mixer_lut`, whose curve is `32767 * S / (1 + |S| * 0.5)` with `S = sum * 1.6 / 131072` — i.e. the table maps a RAW four-channel sum (`±131072`, `S = ±1.6`, hence `LUT[0] = -29126`) and bakes the 0.4 in itself. Pre-scaling meant the accumulator only ever reached `±52428`, so the outer ~60 % of the table was unreachable and the compressor's knee sat in the wrong place. Second, the documented fixed −6 dB headroom pad (`>>1`, Q15 → Q14) had no float equivalent. Combined, ESP32-C3 and other FPU-less targets rendered at ~0.225× the level of ESP32/ESP32-S3 for identical input — the `ABI.md §6` note only ever accounted for the 0.5 of that. The accumulator now carries the raw channel sum and `MIXER_SCALE` is applied exactly once downstream (by the LUT for `Rational`, explicitly as `(sum * 13107) >> 15` for the computed curves), and the output is no longer padded. **Consumers running on no-FPU targets will hear this path get ~4.4× louder and should re-check their master volume.** Also removes a per-channel truncation to Q15 that was quantizing every voice before summation. The **canonical golden PCM fixture is byte-for-byte identical and was NOT regenerated** — contrary to the milestone sketch, `test_apu_mirror` compiles with `PR32_APU_HAS_FPU=1`, so the fixture is rendered through the float path, which this change does not touch. Nothing changes for desktop, ESP32 or ESP32-S3 builds.
- **Fixed (Behavior, no-FPU targets only)**: the Q15 path now saturates before the final `int16_t` cast, as the float path always did. The `>>1` pad had kept the value below half scale, so the overflow was unreachable; without it an unshaped over-unity mix reaches `S = 1.6` (52428 in Q15) and, with the HPF bypassed (its own clamp skipped), the cast wrapped — a full-scale positive peak came out as a large negative sample. Only reachable via `setSoftClipMode(None)` or `HardClip` combined with `setHpfCutoffHz(0)`; the default `Rational` + active HPF was never affected.
- **Tests (UNIT_TEST)**: 2 new Hito 5 M12 tests in `test_q15_smoke.cpp`, and `test_q15_softclip_computed_modes_track_the_lut` replaced by `test_q15_softclip_curve_shapes_match_the_float_path`, which pins 13107 / 10922 / 12519 for None / Rational / Tanh at `S = MIXER_SCALE` — the exact numbers `test_softclip_curve_shapes_below_unity` already pins on the float path. That identity is the milestone's actual contract, and pinning both binaries to the closed-form value of the mixing chain is the only way to assert it across two separate executables. The second test drives four voices past unity and asserts both rails pin at `+32767` / `-32768`; a wrapped output collapses to ~`±13107` instead. `test_q15_softclip_rational_compresses_below_none` now compares mean magnitude rather than peak, because above unity the final clamp flattens every curve to the same peak — the same reasoning `measureOverdrive` uses on the float side.
- **API**: renamed `InstrumentPreset::noiseShortMode` → `noiseLfsrShort` (and the same rename on `AudioChannel`) to better describe the actual LFSR tap it selects. The old field is kept as a docstring-deprecated alias and is written by `ApuCore::initVoiceFromEvent` whenever the new one is read, so existing consumer code that still touches `noiseShortMode` continues to read the correct value through the normal APU lifecycle. The two fields are independent `bool` storage in C++17 (no automatic sync), so direct writes to one without going through `initVoiceFromEvent` will NOT propagate to the other — use `noiseLfsrShort` in new code. The deprecated field will be removed in a future major version. Behavior and PCM output are unchanged; the golden PCM fixture is byte-for-byte identical.
- **Behavior**: TRIANGLE wave output is now quantized to 4-bit NES levels (32 steps) by default. Opt out per voice with `voice.triangleQuantize4Bit = false`. Golden PCM regenerated.
- **API**: added `AudioChannel::triangleQuantize4Bit` (default true).
- **API**: added `AudioChannel::pulseDutyIndex` (default 255 = continuous); 0..3 selects NES discrete duty patterns. Companion setter `ApuCore::setVoicePulseDutyMode(int slot, uint8_t index)`.
- **API**: added `AudioChannel::triangleOctaveUp` (default false). When true on a TRIANGLE voice, the effective frequency at init is doubled to match NES convention.
- **Behavior**: NOISE LFSR is now seeded to `0x7FFF` (all 1s, NES-equivalent) instead of `0x4000`. Matches NES after a write to `$400F`.

### Migration
- Consumers using the NES sweep unit must add `setVoiceNesSweepUnitEnabled(slot, true)` alongside their existing `setVoiceNesSweep` / `setVoiceNesTimer` calls; without it the voice is no longer under sweep control and is never muted. Nothing else changes, and consumers that never enabled the NES frame counter are unaffected.
- Consumers relying on smooth triangular waves should set `voice.triangleQuantize4Bit = false` on the affected voices.
- Consumers loading legacy project files with `noiseShortMode` should continue to work (alias retained from Hito 0).

## v1.0.1 — 2026-08-01

Packaging/metadata release for the first PlatformIO Registry publish. No source, ABI or PCM changes.

- `library.json`: author/maintainer info, `repository` and `homepage` fields.
- Documentation translated to English.
- CI: `publish.yml` workflow (pack validation + PlatformIO Registry publish, gated by a `release` PR label or manual dispatch with dry-run).
- `requirements.txt` for reproducible PlatformIO installs in CI.

## v1.0.0 — 2026-08-01

First version as a standalone library. Origin: PixelRoot32 Game Engine commit `a3ac998` (branch `refac/extract-apu`), which in turn incorporates Phase 0 convergence with the Tool Suite FPU mirror (`fc763b8`).

### Extracted from the engine
- Complete `ApuCore` (synthesis, sequencer, mixer, transport) with both render paths (float and Q15+LUT).
- Types: `AudioTypes.h`, `AudioMusicTypes.h`, `AudioCommandQueue.h`, `AudioOscLUT.h`, `AudioMixerLUT.h`.

### New
- `ApuConfig.h`: unified FPU detection (`PR32_APU_HAS_FPU`, replaces the misaligned pair `SOC_CPU_HAS_FPU` / `config::HasFPU`), injectable logging hook (`apuSetLogHandler`, replaces `core/Log.h`) and configurable profiling (`PR32_APU_ENABLE_PROFILING`, replaces `platforms::config::EnableProfiling`).
- Dual packaging: `library.json` (PlatformIO) + `CMakeLists.txt` (target `PixelRoot32::APU`).
- Own test suite (150 cases): engine suites (`test_apu_core` 68, `test_audio_command_queue` 7, `test_audio_music_types` 31), Tool Suite parity suite (`test_apu_mirror` 37, with golden PCM) and `test_q15_smoke` (7) — first Q15 path coverage, run natively with `-DPR32_APU_HAS_FPU=0`.

### No Behavioral Changes
- The Tool Suite canonical golden PCM passes byte-for-byte against this library.
- Documented divergence preserved: −6 dB pad in the Q15 path (see `docs/ABI.md §6`).
