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

## 4. Loop and Duration

- `loop == true` ⇒ `remainingSamples = UINT64_MAX` (sentinel); only terminates via `STOP_CHANNEL` or stealing.
- `loop == false && duration <= 0` ⇒ the voice is deactivated immediately (anti-stall guarantee).
- Sequencer: `MusicNote.duration == 0` = stacked trigger with no time advance (drums on the same step). Preset fallback duration: `max(duration, 0.05) / tempoFactor`.

## 5. Command Queue

`AudioCommandQueue` is **SPSC**: one producer (UI/game thread) and one consumer (audio thread). Does not support multiple concurrent producers. Full queue ⇒ newest command is dropped and `droppedCommands` is incremented.

## 6. Render Paths

| Path | Guard | Targets |
|---|---|---|
| Float | `PR32_APU_HAS_FPU == 1` | Desktop, classic ESP32, ESP32-S3 |
| Integer Q15 + `audio_mixer_lut` | `PR32_APU_HAS_FPU == 0` | ESP32-C3, RISC-V without FPU (and forced native tests) |

Float chain: `Σ(voice · MIXER_SCALE=0.4) · masterVolume → soft-clip 1/(1+|x|·0.5) → HPF R=0.995 → ±32767 → bitcrush`. The Q15 path replicates the same chain with the pre-adjusted LUT and master volume applied **before** the compressor (order parity since v1.0.0).

**Loudness parity** (unreleased; ships in the next major, 2.0.0): both paths render at the same level. The Q15 accumulator carries the **raw** channel sum and `MIXER_SCALE` is applied exactly once downstream — by `audio_mixer_lut`, whose curve is defined over `x = sum · 1.6 / 131072` and therefore bakes the 0.4 in, or explicitly as `(sum · 13107) >> 15` for the computed soft-clip modes. Do **not** pre-scale channels before summation: that applies 0.4 twice and puts the compressor knee in the wrong place.

Previously the Q15 path was ~0.225× the float level — the double `MIXER_SCALE` plus a fixed −6 dB pad (`>>1`, Q15→Q14) with no float equivalent. Both are gone; consumers on FPU-less targets should re-check their master volume. See the CHANGELOG entry for Hito 5 M12.

## 7. Canonical Percussion Presets

`noisePeriod` is the **LFSR period in samples**: larger period ⇒ slower clock ⇒ deeper sound.

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
