/*
 * Copyright (c) 2026 PixelRoot32
 * Licensed under the MIT License
 */
#pragma once

#include "AudioTypes.h"
#include "AudioCommandQueue.h"
#include "AudioMusicTypes.h"
#include "ApuConfig.h"

#include <atomic>
#include <cstdint>

namespace pixelroot32::audio {

/**
      * @class ApuCore
      * @brief Shared NES-style APU core used by every AudioScheduler.
      *
      * Owns an eight-voice pool (MAX_VOICES = 8), the SPSC command queue and the
      * music sequencer. Voice slots 0–3 are reserved for sequencer tracks
      * (trackIdx maps 1:1 to slot); slots 4–7 are reserved for PLAY_EVENT / SFX
      * and sequencer percussion hits (shared subpool, steal policy confined to 4–7). Platform-specific schedulers
      * (DefaultAudioScheduler, ESP32AudioScheduler, NativeAudioScheduler) are
      * thin orchestrators that decide *when* generateSamples() runs; all
      * synthesis, mixing and sequencing lives here to eliminate the three-way
      * duplication that existed before.
      *
      * Mixing curve:
      *   per channel: s_c = wave_c(phase) * volume_c * MIXER_SCALE
      *   sum:         S   = Σ s_c                         (bounded to [-1.6, 1.6])
      *   compressor:  y   = S / (1 + |S| * MIXER_K)
      *   output:      y * masterVolume * 32767, passed through a single-pole
      *                DC-blocker to remove offset + transient clicks.
      *
      * `Nes`/`Hybrid` replace the uniform per-channel weighting with
      * `MixerCurve::NesWeighted` (S4): the voices are summed per group and the
      * published NES ratios decide how much each group contributes, with the
      * PULSE group anchored at exactly the level above so the change is
      * balance, not loudness. The weighted sum then enters the same compressor,
      * master volume and DC blocker chain. `Legacy` keeps the formula above
      * unchanged, byte for byte.
      *
      * On cores without an FPU (ESP32-C3) the integer-optimised path uses
      * `audio_mixer_lut`, which is pre-fitted to the same curve and renders
      * at the same level (Hito 5 M12). It applies MIXER_SCALE once, after
      * summation rather than per channel, because the table's mapping already
      * contains the 0.4 — see AudioMixerLUT.h.
      *
      * Since the profiles arrived (S3) the selector, not the target, decides
      * which body runs: `Nes`/`Hybrid` use the integer core on every build, and
      * `Legacy` falls back to it where the float body is not compiled
      * (`PR32_APU_ENABLE_LEGACY_FLOAT == 0`).
      */
     class ApuCore {
    public:
        /** @brief Maximum number of voices (dynamic pool size). */
        static constexpr int MAX_VOICES = 8;
        // Backward-compatible alias used in some tests/diagnostics.
        /** @brief Alias for MAX_VOICES for backward compatibility. */
        static constexpr int NUM_CHANNELS = MAX_VOICES;
        /** @brief Maximum simultaneous music tracks. */
        static constexpr size_t MAX_MUSIC_TRACKS = 4;
        /** @brief First voice slot reserved for music tracks (inclusive). */
        static constexpr int MUSIC_VOICE_BASE = 0;
        /** @brief Number of voice slots reserved for music tracks. */
        static constexpr int MUSIC_VOICE_COUNT = static_cast<int>(MAX_MUSIC_TRACKS);
        /** @brief First voice slot reserved for SFX / PLAY_EVENT (inclusive). */
        static constexpr int SFX_VOICE_BASE = MUSIC_VOICE_BASE + MUSIC_VOICE_COUNT;
        /** @brief Number of voice slots reserved for SFX. */
        static constexpr int SFX_VOICE_COUNT = MAX_VOICES - SFX_VOICE_BASE;
        static_assert(SFX_VOICE_BASE + SFX_VOICE_COUNT == MAX_VOICES,
                      "Music + SFX voice partitions must cover MAX_VOICES");
        /** @brief Default ticks (subdivisions) per beat. */
        static constexpr int TICKS_PER_BEAT = 4;
        /** @brief Default tempo in BPM. */
        static constexpr float DEFAULT_BPM = 150.0f;

        // Sequencer note limit: max notes processed per frame to prevent unbounded catch-up
        /** @brief Default max notes per frame for the music sequencer. */
        static constexpr size_t DEFAULT_MAX_NOTES_PER_FRAME = 32;
        #ifndef AUDIO_SEQUENCER_MAX_NOTES
        /** @brief User-configurable max notes per frame. */
        static constexpr size_t MAX_NOTES_PER_FRAME = DEFAULT_MAX_NOTES_PER_FRAME;
        #else
        static_assert(AUDIO_SEQUENCER_MAX_NOTES >= 1 && AUDIO_SEQUENCER_MAX_NOTES <= 1024,
            "AUDIO_SEQUENCER_MAX_NOTES must be between 1 and 1024");
        static constexpr size_t MAX_NOTES_PER_FRAME = (AUDIO_SEQUENCER_MAX_NOTES > 1000) ? 32 : AUDIO_SEQUENCER_MAX_NOTES;
        #endif

        // Mixing constants exposed for diagnostics / tests.
        /** @brief Per-channel scaling factor before summation. */
        static constexpr float MIXER_SCALE = 0.4f;
        /** @brief Compressor nonlinearity constant. */
        static constexpr float MIXER_K     = 0.5f;

        /** @brief Default constructor. Initializes state but does not set sample rate. */
        ApuCore();

        /**
         * @brief Configures the output sample rate.
         * @param sampleRate Sample rate in Hz (e.g., 22050, 44100).
         * 
         * Safe to call before start(). Pre-calculates internal timing values.
         */
        void init(int sampleRate);

        /**
         * @brief Enqueues a command for processing.
         * @param cmd The audio command to enqueue.
         * @return true if the command was enqueued, false if the queue was full.
         */
        bool submitCommand(const AudioCommand& cmd);

        /**
         * @brief Generates audio samples.
         * @param stream Output buffer (mono, int16 samples).
         * @param length Number of samples to generate.
         * 
         * Processes all pending commands, updates music sequencer, and synthesizes
         * voices into the output buffer. Applies master volume, bitcrush, and DC blocking.
         */
        void generateSamples(int16_t* stream, int length);

        /** @brief Returns the total number of commands dropped since construction. @return Monotonic count. */
        uint32_t getDroppedCommands() const {
            return droppedCommands.load(std::memory_order_relaxed);
        }

        // Sequencer note limit API
        /**
         * @brief Sets the maximum notes processed per audio frame.
         * @param limit Max notes [1-1000], clamped to safe bounds internally.
         * 
         * Bounded processing prevents audio starvation when many notes queue up.
         */
        void setSequencerNoteLimit(size_t limit);

        /** @brief Gets current max notes per frame setting. @return Current limit. */
        size_t getSequencerNoteLimit() const { return sequencerNoteLimit.load(std::memory_order_acquire); }

        /** @brief Returns notes deferred to next frame due to note limit. @return Deferred count. */
        size_t getDeferredNotes() const { return deferredNotes.load(std::memory_order_relaxed); }

        /** @brief Reports if music is currently playing. @return true if playing. */
        bool isMusicPlaying() const { return musicPlayingFlag.load(std::memory_order_acquire); }
        /** @brief Reports if music is paused. @return true if paused. */
        bool isMusicPaused()  const { return musicPausedFlag.load(std::memory_order_acquire); }

        /** @brief Current absolute sequencer tick (transport clock). Thread-safe. */
        uint64_t getMusicGlobalTick() const {
            return musicGlobalTick_.load(std::memory_order_acquire);
        }

        /** @brief Absolute tick at which the current piece started (MUSIC_PLAY / MUSIC_SEEK anchor). Thread-safe. */
        uint64_t getMusicPlayStartTick() const {
            return musicPlayStartTick_.load(std::memory_order_acquire);
        }

        /** @brief Gets the configured sample rate. @return Sample rate in Hz. */
        int getSampleRate() const { return sampleRate; }

        /**
         * @brief Resets all state to initial values.
         * 
         * Intended for unit tests. Stops all voices, clears sequencer, resets counters.
         */
        void reset();

        /**
         * @brief Sets an optional post-mix callback.
         * @param fn Function pointer: void(int16_t* mono, int length, void* user).
         * @param user User data passed to the callback.
         * 
         * Called after bitcrush on the final mono buffer. Runs in audio thread context.
         */
        void setPostMixMono(void (*fn)(int16_t* mono, int length, void* user), void* user);

        /**
         * @brief Set the PULSE duty mode for a voice slot.
         *
         * Hito 1 M2. Opt-in NES-accurate duty (12.5/25/50/75 %) for PULSE voices.
         * The default `pulseDutyIndex == 255` keeps the legacy continuous
         * `dutyCycle` behaviour, so existing consumers are unaffected.
         *
         * S6: since E1 the sound profile also owns this mode as its DEFAULT —
         * `Nes` and `Hybrid` arm the step nearest the note's own `duty`, and
         * `Legacy` arms continuous. A call here is a tier-2 override (D-3): it
         * pins the mode for the slot, so the profile does NOT re-arm its own
         * choice on the next note and the note's `duty` no longer selects the
         * step. Passing 255 therefore means "this slot stays continuous" even
         * under a NES-derived profile. The pin is per-slot state: it survives
         * new notes and is cleared only by `Voice::reset()`.
         *
         * Like the other selectors, this is not safe to call concurrently with
         * rendering.
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param nesDutyIndex 0..3 for NES discrete duty, 4..254 treated as
         *                     continuous (defensive), 255 for continuous.
         */
        void setVoicePulseDutyMode(int slot, uint8_t nesDutyIndex);

        /**
         * @brief Set the NES NOISE period LUT mode for a voice slot.
         *
         * Hito 3 M8. Opt-in NES-accurate noise colours: index 0..15 selects
         * a period from `nes_apu::kNesNoisePeriodLutNtsc` (the canonical
         * `$400E` table, in CPU cycles) instead of deriving the LFSR clock
         * from the note frequency. The table is converted to a sample
         * countdown at the current sample rate, so the same index gives the
         * same timbre at 22050, 44100 or 48000 Hz.
         *
         * The default `nesNoiseLutIndex == 255` keeps the legacy
         * frequency-derived behaviour, so existing consumers and the
         * canonical golden PCM are unaffected.
         *
         * Like `setVoicePulseDutyMode`, this is a per-slot mode rather than
         * a per-note value: it survives new notes on the same slot and is
         * cleared only by `Voice::reset()`. When the slot currently holds a
         * NOISE voice the period is retuned immediately, so the change is
         * audible without waiting for the next note. On non-NOISE voices the
         * index is stored but nothing else is touched.
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param lutIndex 0..15 to select a LUT entry; 16..254 are invalid
         *                 and stored as 255; 255 = derive from frequency.
         */
        void setVoiceNesNoiseLutMode(int slot, uint8_t lutIndex);

        /**
         * @brief Set the master high-pass (DC blocker) corner frequency.
         *
         * Hito 4 M10. The output stage runs a single-pole high-pass
         * `y[n] = x[n] - x[n-1] + R*y[n-1]` with `R = exp(-2*pi*fc/fs)`.
         *
         * Until this is called the APU uses the historical fixed
         * coefficient `R = 0.995`, which pins the COEFFICIENT rather than
         * the corner: that is ~35.2 Hz at 44100 Hz but ~17.6 Hz at
         * 22050 Hz, so the filter changed character with the sample rate.
         * Calling this stores the corner in Hz and derives `R` from the
         * current rate instead, so the same value means the same filter
         * everywhere. `init()` recomputes it when the rate changes.
         *
         * The default is left bit-identical on purpose: it keeps the
         * canonical golden PCM unchanged for existing consumers.
         *
         * Reference points: ~35 Hz is this library's historical corner and
         * ~90 Hz approximates the NES's analog output HPF. `reset()`
         * restores the legacy default, the same way it restores the master
         * volume and bitcrush depth.
         *
         * S8 (caller precedence): this setter is an explicit statement about
         * the output corner, so it OUTRANKS the sound profile. Once it has
         * been called, `Nes` and `Hybrid` do NOT install their output filter
         * chain; the historical single-pole HPF runs at the caller's corner
         * instead. `reset()` clears that override, so the profile owns the
         * stage again on the following block. This mirrors the
         * `pulseDutyOverridden` / `noiseUnipolarOverridden` /
         * `nesNoiseLutOverridden` tier-2 convention of E1. Calling it with
         * <= 0 still counts as an explicit choice (an overridden bypass).
         *
         * Not safe to call concurrently with rendering; treat it like the
         * other direct configuration setters.
         *
         * @param cutoffHz Corner in Hz. Values <= 0 bypass the filter
         *                 entirely. Clamped internally so the recursive
         *                 coefficient always stays inside (0, 1).
         */
        void setHpfCutoffHz(float cutoffHz);

        /**
         * @brief Set the master output shaping curve.
         *
         * Hito 4 M11. The curve is applied to the summed mix BEFORE the DC
         * blocker, so the filter sees its output. That ordering is what
         * makes `SoftClipMode::HardClip` model the NES signal chain: the
         * DAC saturates, and the analog high-pass then filters the
         * already-squared-off signal.
         *
         * `SoftClipMode::Rational` is the default and reproduces the
         * historical behaviour bit-for-bit — on the integer path it keeps
         * using the pre-fitted `audio_mixer_lut` rather than recomputing
         * the same curve, so the canonical golden PCM does not move. The
         * other modes are computed on both paths.
         *
         * `reset()` restores the default, the same way it restores the
         * master volume, the bitcrush depth and the HPF corner.
         *
         * Not safe to call concurrently with rendering; treat it like the
         * other direct configuration setters.
         *
         * @param mode Curve to apply. Unknown values are ignored.
         */
        void setSoftClipMode(SoftClipMode mode);

        /**
         * @brief Apply a whole NES option set to a voice slot.
         *
         * Hito 4 M14. Bundles every per-voice NES opt-in so a caller can
         * put a voice into (or out of) NES mode in one atomic write instead
         * of five separate calls that could be observed half-applied. The
         * dedicated setters remain the right tool for changing one thing.
         *
         * Delegates to those setters rather than writing the fields itself,
         * so their validation and side effects apply unchanged: invalid
         * selector indices land on the documented sentinels, and selecting
         * a noise LUT entry retunes a live NOISE voice immediately.
         *
         * S5: two of the bundled fields — `noiseUnipolar` and `noiseLutIndex` —
         * are also the per-slot override of a sound-profile default (D-3 tier
         * 2), so applying this struct pins them: a profile will not re-arm its
         * own value on the next note. `noiseUnipolar` has no standalone setter,
         * which makes this method and `AudioCommandType::SET_NES_OPTIONS` its
         * only per-voice entry points.
         *
         * S6 adds `pulseDutyIndex` to that set: it is delegated to
         * `setVoicePulseDutyMode`, which pins the duty mode against the
         * profile's default, so applying a default-constructed struct returns a
         * NES-profile PULSE voice to the continuous generator.
         *
         * Not safe to call concurrently with rendering. Use
         * `AudioCommandType::SET_NES_OPTIONS` from the game thread — it
         * carries the same struct through the queue and applies it via this
         * method, so both entry points produce identical state.
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param opts Options to apply. A default-constructed value disarms
         *             every NES sub-unit on the voice.
         */
        void setVoiceNesOptions(int slot, const VoiceNesOptions& opts);

        /**
         * @brief Read back a voice's current NES options.
         *
         * Hito 4 M14. Reflects the live per-voice fields, so it also shows
         * changes made through the individual setters or by M17's
         * `initNesSafe` (which clears the sub-unit flags but deliberately
         * leaves the waveform mode selectors alone).
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns a
         *             default-constructed value.
         */
        VoiceNesOptions getVoiceNesOptions(int slot) const;

        /**
         * @brief Select the character set applied to every voice.
         *
         * S4. The profile is global configuration, not per-voice state: one
         * selector, one render body for the whole mix. Unknown values are
         * ignored rather than clamped, so a corrupt command cannot silently
         * change the sound to a neighbouring profile.
         *
         * The profile also OWNS the output stage (`Nes` and `Hybrid`, S1):
         * one shared chain applied to the summed mix, never per voice, in
         * this order — HPF 1st order at 90 Hz, HPF 1st order at 440 Hz, LPF
         * 2nd order Butterworth (Q = 1/sqrt(2)) at `min(14000 Hz,
         * 0.45 * fs)`. Coefficients are derived on a configuration or rate
         * change, never per sample. `Legacy` keeps the historical single-pole
         * HPF and is byte-identical on every build.
         *
         * The LPF is deliberately NOT engaged below `0.45 * fs >= 6000 Hz`
         * (i.e. `fs >= 13334 Hz`): at 8 000 Hz `0.45 * fs` is 3 600 Hz, which
         * would cut most of the passband and gut the sound instead of
         * approximating the NES's 14 kHz roof. Below that floor the chain is
         * the two high-passes only, which is a documented property of the
         * profile, not a fallback. An explicit `setHpfCutoffHz()` call wins
         * over all of this (S8).
         *
         * SAW is the one waveform whose TIMBRE the profile changes (S4): only
         * `Hybrid` reads a band-limited table, a mipmap of eight 256-point Q15
         * octaves selected from the voice's own `f / fs`, crossfaded between
         * adjacent levels so a glissando has no seam (S6). `Legacy` and `Nes`
         * both keep the ideal ramp and its aliasing; `Nes` does so on purpose
         * (P4) — its PULSE character deliberately reproduces the NES's own
         * artefacts, and the untuned SAW is simply left alone rather than
         * given a half-NES treatment. SINE is unchanged on every profile:
         * it is a single harmonic, so band-limiting it would spend flash to
         * remove an artefact it does not have (L5).
         *
         * Thread safety: like `setVoiceNesOptions`, this mutates configuration
         * the renderer reads and is NOT safe to call concurrently with
         * rendering — write it from the audio thread. From the game thread use
         * `AudioCommandType::SET_SOUND_PROFILE`, which `processCommands`
         * dispatches here, so both entry points produce identical state.
         *
         * `init()` deliberately leaves it alone: a stream restart must not
         * discard the caller's choice.
         *
         * @param profile Profile to apply. `Legacy`, `Nes` or `Hybrid`;
         *                anything else is ignored.
         */
        void setSoundProfile(SoundProfile profile);

        /**
         * @brief Read back the active sound profile.
         *
         * S4. A plain read of the stored selector, so it is safe to call from
         * any thread. Returns `SoundProfile::Legacy` until one is selected.
         *
         * @return Profile currently in force.
         */
        SoundProfile getSoundProfile() const;

        // -- NES frame counter (Hito 2 M3) -----------------------------
        /**
         * @brief Set the NES frame counter mode.
         * @param mode 0 = 4-step (240/120/60 Hz, IRQ at 60 Hz),
         *             1 = 5-step (~192/~96 Hz, no IRQ).
         *
         * Mirrors the $4017 write side effects: schedules a timer reset on
         * the next tick, and clears the IRQ flag. Values other than 0 or 1
         * are silently ignored. Calling with the current mode is a no-op
         * (idempotent). Default is OFF — the counter does not tick and
         * the canonical track PCM is unaffected.
         */
        void setNesFrameCounterMode(int mode);

        /**
         * @brief Inhibit the frame interrupt flag.
         * @param inhibit true = clear the IRQ flag and prevent it from being
         *                      set on quarter/half clocks.
         *
         * Mirrors the I bit ($4017 bit 6).
         */
        void setNesFrameCounterInhibit(bool inhibit);

        /**
         * @brief Returns the current frame interrupt flag.
         * @return true if the IRQ flag is set (only ever true in mode 0).
         *
         * Mirrors a read of $4015 (but does NOT clear the flag).
         */
        bool getNesFrameCounterIrq() const;

        // -- NES length counter (Hito 2 M4) -----------------------------
        /**
         * @brief Load the NES length counter for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param lengthIndex 0..31 (NES LUT index). Values > 31 are clamped to 31.
         * @param halt Halt flag: true = freeze the counter (do not decrement).
         *
         * Mirrors the length load side effect of a write to
         * $4003/$4007/$400B/$400F. The counter is immediately usable: the
         * next half-clock tick from the NES frame counter will decrement
         * from `nes_apu::kNesLengthLut[lengthIndex]`. If the voice's NES
         * channel enable is clear (`nesLengthCounter.enabled == false`),
         * this call is a no-op (writes to a disabled NES channel are
         * silently lost).
         *
         * Two side effects come with the load, matching the hardware:
         * the M7 envelope start flag is raised (`$4003`/`$4007`/`$400F`)
         * and the M5 linear counter reload flag is raised (`$400B`).
         * Both are set unconditionally; their dispatchers gate on voice
         * type and on their own opt-in flag, so a voice that did not opt
         * into those sub-units never observes them.
         *
         * Thread safety: this is a direct mutation of the voice pool and
         * is NOT safe to call from a thread other than the audio thread.
         * Use `AudioCommandType::TRIGGER_NES_LENGTH` for the queued path.
         *
         * Hito 2 M4; side effects completed in M7 and M13.
         */
        void setVoiceNesLength(int slot, uint8_t lengthIndex, bool halt);

        /**
         * @brief Set the NES channel enable for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param enabled true = channel is on, false = forced silence.
         *
         * Mirrors the effect of writing to $4015. When cleared, the
         * length counter is forced to 0 and the voice is silenced
         * (enabled=false, envelope OFF, currentLevel=0). When set, the
         * counter can be loaded via `setVoiceNesLength` but no immediate
         * effect on the counter value is taken.
         *
         * `remainingSamples` is left untouched (M4 is orthogonal to the
         * pre-Hito-2 duration mechanism). Hito 2 M4.
         */
        void setVoiceNesChannelEnabled(int slot, bool enabled);

        // -- NES linear counter (Hito 2 M5) -------------------------------
        /**
         * @brief Load the NES linear counter for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param reloadValue 7-bit reload value (high bits are masked off).
         * @param reloadFlag true = load `reloadValue` into `counter` on the
         *                   next quarter-clock; cleared automatically if
         *                   the shared halt/control flag is clear.
         *
         * Mirrors the linear counter load side effect of a write to
         * $4008. The values are always stored; the counter only acts on
         * them when `linearEnabled` is true (i.e. the consumer has opted
         * in via `setVoiceNesLinearEnabled`). The TRIANGLE voice is the
         * only consumer in this library.
         *
         * Hito 2 M5.
         */
        void setVoiceNesLinearCounter(int slot, uint8_t reloadValue, bool reloadFlag);

        /**
         * @brief Set the NES linear counter enable for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param enabled true = the linear counter ticks at quarter-frame
         *                      rate and gates the TRIANGLE sequencer per-sample.
         *
         * Mirrors the linear counter enable bit ($4008 bit 7, "length
         * counter / linear counter enable"). When false, the counter is
         * bypassed entirely (the gate is treated as always open and the
         * counter does not tick). The TRIANGLE voice is the only consumer
         * in this library; SINE/SAW/PULSE/NOISE keep it false.
         *
         * Hito 2 M5.
         */
        void setVoiceNesLinearEnabled(int slot, bool enabled);

        // -- NES envelope unit (Hito 2 M7) ----------------------------
        /**
         * @brief Configure the NES envelope unit for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param loop L bit (shared with length counter `halt`). When true,
         *             the decay level wraps from 0 back to 15 instead of
         *             silencing the channel. The value is stored in the
         *             envelope struct, but the live read at quarter-clock
         *             time comes from `nesLengthCounter.halt` (same bit).
         * @param constVolume C bit. When true, the output is the
         *                    constant `volume` (VVVV); when false, the
         *                    output is the decremented `decayLevel`.
         * @param volume VVVV (4 bits, clamped). Constant volume (C=1) or
         *               divider reload value (C=0).
         *
         * Mirrors the $4000 / $4004 / $400C write side effects. Does NOT
         * set the start flag (that comes from `setVoiceNesLength`).
         * Non-PULSE / non-NOISE voices still have the field written so
         * callers can introspect the configuration, but the
         * quarter-clock dispatch skips them.
         *
         * Hito 2 M7.
         */
        void setVoiceNesEnvelope(int slot, bool loop, bool constVolume, uint8_t volume);

        /**
         * @brief Set the NES envelope enable for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param enabled true = the quarter-frame dispatch ticks the
         *                      envelope and its `output` replaces the ADSR
         *                      level as the per-sample volume source
         *                      (PULSE / NOISE only).
         *
         * When false, the quarter-frame dispatch is a no-op for this
         * voice and the ADSR envelope continues to drive the volume.
         * TRIANGLE / SINE / SAW voices keep this false permanently.
         *
         * Hito 2 M7.
         */
        void setVoiceNesEnvelopeEnabled(int slot, bool enabled);

        // -- NES sweep unit (Hito 2 M6) -------------------------------
        /**
         * @brief Configure the NES sweep unit for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param enabled E bit. When false, the timer-update branch is
         *                     disabled (but the muting flag is still evaluated —
         *                     the "NES bug").
         * @param period P (3 bits, clamped to 0..7). Divider period in
         *               half-clocks. P+1 half-clocks elapse between
         *               timer updates.
         * @param negate N bit. When true, the target is computed by
         *               subtracting `change` from the current timer
         *               (with the PULSE 1/2 formula difference).
         * @param shift SSS (3 bits, clamped to 0..7). Right-shift amount
         *              for the change calculation. 0 disables the
         *              timer-update branch.
         * @param isPulse2 true = PULSE 2 (two's complement negate:
         *                      `target = period - change`); false =
         *                      PULSE 1 (ones' complement negate:
         *                      `target = period - change - 1`).
         *
         * Mirrors the $4001 / $4005 write side effect. Sets
         * `reloadFlag = true` so the next half-clock reloads the
         * divider counter from `period + 1`. Non-PULSE voices are
         * silently ignored by the half-clock dispatch (the field is
         * still updated so callers can introspect the configuration).
         *
         * Hito 2 M6.
         */
        void setVoiceNesSweep(int slot, bool enabled, uint8_t period, bool negate,
                              uint8_t shift, bool isPulse2);

        /**
         * @brief Opt a voice slot into (or out of) the NES sweep unit.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param enabled true to place the voice under NES sweep control.
         *
         * This is the per-voice participation gate, NOT the `$4001`/`$4005`
         * E bit — that one is the `enabled` argument of
         * `setVoiceNesSweep`. Same role as `setVoiceNesChannelEnabled`
         * (M4), `setVoiceNesLinearEnabled` (M5) and
         * `setVoiceNesEnvelopeEnabled` (M7).
         *
         * Default OFF. While it is off the half-clock dispatch skips the
         * voice entirely and `muted` is held false, so a PULSE voice
         * driven by the legacy float frequency path keeps playing when
         * the frame counter is switched on. Toggling this re-evaluates
         * `muted` immediately in both directions.
         *
         * Hito 2 M6.
         */
        void setVoiceNesSweepUnitEnabled(int slot, bool enabled);

        /**
         * @brief Set the 11-bit NES timer period for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param timer11 11-bit NES timer period (clamped to 0..2047).
         *
         * Mirrors the $4002 / $4006 / low-3-bits-of-$4003/$4007 timer
         * load. Updates the per-voice sweep unit's `timer` field, then
         * re-syncs the float / Q32 phase increments so the oscillator
         * frequency matches `111860.8 / (timer + 1)` Hz immediately
         * (the hot path does not need to wait for the next half-clock).
         *
         * Also recomputes the `muted` flag right away, so the PULSE
         * output gate sees the correct value before the next
         * half-clock tick.
         *
         * Hito 2 M6.
         */
        void setVoiceNesTimer(int slot, uint16_t timer11);

        // -- Profiling API (public for Engine access) -------------
        /** @brief Ring buffer size for profile entries. */
        static constexpr int PROFILE_RING_SIZE = 64;
        /**
         * @brief A single profile snapshot.
         */
        struct ProfileEntry {
            uint64_t audioTimeSamples; ///< Global sample counter at capture.
            float peak;                ///< Peak sample magnitude [0.0 - 1.0].
            bool clipped;              ///< Whether any sample exceeded ±32767.
        };
        /**
         * @brief Reads and clears all profile entries.
         * @param out Array of ProfileEntry to fill.
         * @param count On input: max entries. On output: actual count written.
         */
        void getAndResetProfileStats(ProfileEntry* out, uint8_t& count);

#if defined(UNIT_TEST)
        // -- Test diagnostics (native_test only; stripped in firmware builds) --
        /**
         * @brief Test-only: counts voices with enabled==true.
         * @note Available only when UNIT_TEST is defined (native_test). Not for game code.
         * @return Active voice count in [0, MAX_VOICES].
         */
        size_t countEnabledVoicesForTesting() const;
        /**
         * @brief Test-only: main music track note index after the last sequencer run.
         * @note Available only when UNIT_TEST is defined (native_test). Not for game code.
         * @return Index into track 0's note array.
         */
        size_t getSequencerMainNoteIndexForTesting() const;
        /**
         * @brief Test-only: reports whether a voice slot is currently synthesizing.
         * @note Available only when UNIT_TEST is defined (native_test). Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return true if voices[slot].enabled.
         */
        bool isVoiceEnabledForTesting(int slot) const;
        /**
         * @brief Test-only: envelope stage of the first enabled voice of a wave type.
         * @param type Wave type to search for.
         * @return Envelope stage, or EnvelopeState::Stage::OFF if no match.
         */
        EnvelopeState::Stage getActiveVoiceStageForTesting(WaveType type) const;
        /**
         * @brief Test-only: fixed music voice slot when the melodic gate is active, else -1.
         * @param track_index Sequencer track [0, MAX_MUSIC_TRACKS).
         */
        int getTrackVoiceSlotForTesting(size_t track_index) const;
        /**
         * @brief Test-only: envelope stage of a music track's fixed voice slot.
         * @param track_index Sequencer track [0, MAX_MUSIC_TRACKS).
         * @return Envelope stage, or EnvelopeState::Stage::OFF if disabled/out-of-range.
         */
        EnvelopeState::Stage getTrackVoiceStageForTesting(size_t track_index) const;
        /**
         * @brief Test-only: reports whether a music track has an active melodic gate.
         * @note Available only when UNIT_TEST is defined (native_test). Not for game code.
         * @param track_index Sequencer track [0, MAX_MUSIC_TRACKS); out-of-range returns false.
         * @return true after a melodic note until Rest note-off or lifecycle reset.
         */
        bool isMusicTrackVoiceActiveForTesting(size_t track_index) const;
        /**
         * @brief Test-only: NOISE LFSR period in samples for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return NOISE LFSR period in samples if slot is valid, 0 otherwise.
         */
        uint32_t getVoiceNoisePeriodForTesting(int slot) const;
        /**
         * @brief Test-only: remaining sample gate for a voice slot.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return remaining samples if slot is valid, 0 otherwise.
         */
        uint64_t getVoiceRemainingSamplesForTesting(int slot) const;
        /**
         * @brief Test-only: whether a voice slot is in continuous loop mode.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return true if voice is in loop mode.
         */
        bool isVoiceLoopForTesting(int slot) const;
        /**
         * @brief Test-only: current voice frequency in Hz (melodic / noise clock).
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Frequency in Hz if slot is valid, 0 otherwise.
         */
        float getVoiceFrequencyForTesting(int slot) const;
        /**
         * @brief Test-only: pre-set the M16 triangleOctaveUp flag on a voice slot.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range ignored.
         * @param enabled true to double the effective frequency on the next
         *                initVoiceFromEvent that targets a TRIANGLE voice.
         */
        void setVoiceTriangleOctaveUpForTesting(int slot, bool enabled);
        /**
         * @brief Test-only: current PULSE duty cycle [0,1].
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         */
        float getVoiceDutyCycleForTesting(int slot) const;
        /**
         * @brief Test-only: current oscillator phase [0,1) for a voice slot.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Float phase in [0,1), or 0 if slot is invalid.
         */
        float getVoicePhaseForTesting(int slot) const;
        /**
         * @brief Test-only: continuous dutySweep delta per sample (0 when duty stepped).
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         */
        float getVoiceDutySweepPerSampleForTesting(int slot) const;
        /**
         * @brief Test-only: current Q32 phase increment of a voice slot.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return The increment the integer core renders the slot with, or 0.
         */
        uint32_t getVoicePhaseIncQ32ForTesting(int slot) const;
        /**
         * @brief Test-only: SAW mipmap level the `Hybrid` read selects.
         *
         * Exposes the production selector (`level = floor(log2(q)) + 9`,
         * clamped to [0, 7], `q = phaseIncQ32 / 2^32`) so a test can assert it
         * against the band layout of `AudioOscMipLUT.h` without reimplementing
         * the renderer. Pure: it reads no voice state.
         *
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param phaseIncQ32 Phase increment in Q32 cycles per sample.
         * @return Mipmap level in [0, SAW_MIP_LEVEL_COUNT - 1].
         */
        static int sawMipmapLevelForTesting(uint32_t phaseIncQ32);

        // -- NES frame counter test diagnostics (Hito 2 M3) ------------
        /**
         * @brief Test-only: reset the NES frame counter to APU cycles = 0.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         *
         * Mirrors the $4017 write side effect directly: zeros the cycle
         * accumulator and current step, clears the IRQ flag and the reset
         * pending flag. Does NOT change mode or irqInhibit.
         */
        void resetNesFrameCounterForTesting();
        /**
         * @brief Test-only: cumulative count of quarter-frame ticks dispatched
         *        by the frame counter since the last reset.
         */
        int getNesFrameCounterQuarterTickCountForTesting() const;
        /**
         * @brief Test-only: cumulative count of half-frame ticks dispatched
         *        by the frame counter since the last reset.
         */
        int getNesFrameCounterHalfTickCountForTesting() const;
        /**
         * @brief Test-only: cumulative count of linear-counter ticks dispatched
         *        by the frame counter since the last reset.
         */
        int getNesFrameCounterLinearTickCountForTesting() const;
        /**
         * @brief Test-only: cumulative count of sweep-unit ticks dispatched
         *        by the frame counter since the last reset.
         */
        int getNesFrameCounterSweepTickCountForTesting() const;
        /**
         * @brief Test-only: cumulative count of IRQ-flag false→true transitions
         *        since the last reset. Does NOT count repeated ticks where the
         *        flag was already set.
         */
        int getNesFrameCounterIrqSetCountForTesting() const;
        /**
         * @brief Test-only: current APU-cycle accumulator of the frame counter.
         */
        double getNesFrameCounterApuCyclesForTesting() const;
        /**
         * @brief Test-only: current step of the frame counter (0 = wrap point).
         */
        int getNesFrameCounterCurrentStepForTesting() const;

        // -- NES length counter test diagnostics (Hito 2 M4) ---------
        /**
         * @brief Test-only: current `counter` value of a voice's NES length counter.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Counter value in [0, 254], or 0 if slot is invalid.
         */
        uint16_t getVoiceNesLengthCounterForTesting(int slot) const;
        /**
         * @brief Test-only: 0..31 NES LUT index currently loaded on a voice.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return NES length index in [0, 31], or 0 if slot is invalid.
         */
        uint8_t getVoiceNesLengthIndexForTesting(int slot) const;
        /**
         * @brief Test-only: current `halt` flag of a voice's NES length counter.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Halt flag, or false if slot is invalid.
         */
        bool getVoiceNesLengthHaltForTesting(int slot) const;
        /**
         * @brief Test-only: whether a voice's NES channel is enabled ($4015).
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Channel enable flag, or false if slot is invalid.
         */
        bool getVoiceNesChannelEnabledForTesting(int slot) const;

        // -- NES linear counter test diagnostics (Hito 2 M5) -----------
        /**
         * @brief Test-only: current `counter` value of a voice's NES linear counter.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Counter value in [0, 127], or 0 if slot is invalid.
         */
        uint8_t getVoiceNesLinearCounterForTesting(int slot) const;
        /**
         * @brief Test-only: 7-bit reload value currently loaded on a voice's linear counter.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Reload value in [0, 127], or 0 if slot is invalid.
         */
        uint8_t getVoiceNesLinearReloadValueForTesting(int slot) const;
        /**
         * @brief Test-only: current `reloadFlag` of a voice's NES linear counter.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Reload flag, or false if slot is invalid.
         */
        bool getVoiceNesLinearReloadFlagForTesting(int slot) const;
        /**
         * @brief Test-only: current `linearEnabled` flag of a voice's NES linear counter.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Linear enabled flag, or false if slot is invalid.
         */
        bool getVoiceNesLinearEnabledForTesting(int slot) const;

        // -- NES sweep unit test diagnostics (Hito 2 M6) -----------
        /**
         * @brief Test-only: current 11-bit NES timer period of a voice's sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Timer period in [0, 2047], or 0 if slot is invalid.
         */
        uint16_t getVoiceNesSweepTimerForTesting(int slot) const;
        /**
         * @brief Test-only: current divider counter of a voice's sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Divider counter, or 0 if slot is invalid.
         */
        uint8_t getVoiceNesSweepDividerForTesting(int slot) const;
        /**
         * @brief Test-only: E bit of a voice's sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return sweepEnabled flag, or false if slot is invalid.
         */
        bool getVoiceNesSweepEnabledForTesting(int slot) const;
        /**
         * @brief Test-only: N bit (negate) of a voice's sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Negate flag, or false if slot is invalid.
         */
        bool getVoiceNesSweepNegateForTesting(int slot) const;
        /**
         * @brief Test-only: SSS (shift) of a voice's sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Shift amount in [0, 7], or 0 if slot is invalid.
         */
        uint8_t getVoiceNesSweepShiftForTesting(int slot) const;
        /**
         * @brief Test-only: continuously-evaluated `muted` flag of a voice's sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return true if the voice is currently silenced by the sweep muting
         *         conditions (current period < 8 OR target > 0x7FF), even
         *         when the sweep is "disabled" (E=0 or SSS=0 — the NES bug).
         */
        bool getVoiceNesSweepMutedForTesting(int slot) const;
        /**
         * @brief Test-only: per-voice opt-in flag of the NES sweep unit.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return true if the voice is under NES sweep control. Distinct
         *         from `getVoiceNesSweepEnabledForTesting`, which reports
         *         the `$4001`/`$4005` E bit.
         */
        bool getVoiceNesSweepUnitEnabledForTesting(int slot) const;

        // -- NES envelope unit test diagnostics (Hito 2 M7) ---------
        /**
         * @brief Test-only: current `output` of a voice's NES envelope.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return 0..15 (constVolume ? volume : decayLevel), or 0 if slot invalid.
         */
        uint8_t getVoiceNesEnvelopeOutputForTesting(int slot) const;
        /**
         * @brief Test-only: current `decayLevel` of a voice's NES envelope.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Decay level in [0, 15], or 0 if slot invalid.
         */
        uint8_t getVoiceNesEnvelopeDecayLevelForTesting(int slot) const;
        /**
         * @brief Test-only: current `dividerCounter` of a voice's NES envelope.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Divider counter, or 0 if slot invalid.
         */
        uint8_t getVoiceNesEnvelopeDividerForTesting(int slot) const;
        /**
         * @brief Test-only: `startFlag` of a voice's NES envelope.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Start flag, or false if slot invalid.
         */
        bool getVoiceNesEnvelopeStartFlagForTesting(int slot) const;
        /**
         * @brief Test-only: `envelopeEnabled` of a voice's NES envelope.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Envelope enable flag, or false if slot invalid.
         */
        bool getVoiceNesEnvelopeEnabledForTesting(int slot) const;
        /**
         * @brief Test-only: `constVolume` of a voice's NES envelope.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return Const volume flag, or false if slot invalid.
         */
        bool getVoiceNesEnvelopeConstVolumeForTesting(int slot) const;
        /**
         * @brief Test-only: VVVV of a voice's NES envelope (constant volume
         *        level, and the divider reload value in decay mode).
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Volume in [0, 15], or 0 if slot is invalid.
         */
        uint8_t getVoiceNesEnvelopeVolumeForTesting(int slot) const;

        // -- NES NOISE period LUT test diagnostics (Hito 3 M8) ---------
        /**
         * @brief Test-only: current NOISE LUT index of a voice.
         * @note Available only when UNIT_TEST is defined. Not for game code.
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 255.
         * @return 0..15 when a LUT entry is armed, 255 when off.
         */
        uint8_t getVoiceNesNoiseLutIndexForTesting(int slot) const;

        // -- Unipolar NOISE test diagnostics (E1/S5) -------------------
        /**
         * @brief Test-only: resolved unipolar gate of a voice slot.
         *
         * Reports the per-voice mirror the integer core renders with, i.e. the
         * value after `applyProfileCharacter` resolved the profile default and
         * after any voice-level override. Never "undecided": the mirror is a
         * plain flag, not a tri-state.
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns false.
         * @return true when a NOISE voice gates between 0 and the level.
         */
        bool isVoiceNoiseUnipolarForTesting(int slot) const;

        // -- Master HPF test diagnostics (Hito 4 M10) ------------------
        /** @brief Test-only: current HPF feedback coefficient R. */
        float getHpfRForTesting() const;
        /** @brief Test-only: Q15 mirror of the HPF coefficient. */
        int32_t getHpfRQ15ForTesting() const;
        /** @brief Test-only: false when the HPF is bypassed. */
        bool getHpfEnabledForTesting() const;
        /** @brief Test-only: requested corner in Hz; negative = legacy coefficient. */
        float getHpfCutoffHzForTesting() const;

        // -- Soft-clip test diagnostics (Hito 4 M11) -------------------
        /** @brief Test-only: current master output shaping curve. */
        SoftClipMode getSoftClipModeForTesting() const;

        // -- Mixer curve test diagnostics (E1/S4) -----------------------
        /**
         * @brief Test-only: mixer curve the active profile selects.
         *
         * Reports the value the renderer will use, derived from the profile
         * selector. There is no matching setter on purpose: the curve is
         * profile character (D-3 tier 1), so `setSoundProfile` is the only way
         * to change it and this accessor is the way to observe that.
         *
         * @return `Rational` under `Legacy`, `NesWeighted` under `Nes` and
         *         `Hybrid`.
         */
        MixerCurve getMixerCurveForTesting() const;

        // -- Sound profile test diagnostics (S4/S5) --------------------
        /**
         * @brief Test-only: number of `applyProfileCharacter` applications so far.
         *
         * Structural proof of the S5 ordering: one application per note-on,
         * and after the `nesAccurate` / `initNesSafe` block.
         *
         * @return Monotonic count since construction.
         */
        uint32_t getProfileApplyCountForTesting() const;

        // -- Envelope curve test diagnostics (E1/S2) -------------------
        /**
         * @brief Test-only: resolved ADSR curve of a voice slot.
         *
         * Reports the per-voice mirror after `initVoiceFromEvent` has run, i.e.
         * the value the renderer will use: never `ProfileDefault`.
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns
         *             `EnvelopeCurve::ProfileDefault`.
         * @return Resolved curve of the slot.
         */
        EnvelopeCurve getVoiceEnvelopeCurveForTesting(int slot) const;

        /**
         * @brief Test-only: current Q15 ADSR level of a voice slot.
         *
         * Exposes the mirror the integer core actually renders with, so the
         * envelope curve can be compared against its analytic model sample by
         * sample instead of being inferred from PCM.
         *
         * @param slot Voice index [0, MAX_VOICES); out-of-range returns 0.
         * @return Envelope level in Q15 (0..32768), or 0 if invalid.
         */
        int32_t getVoiceEnvelopeLevelQ15ForTesting(int slot) const;

        // -- Output filter chain test diagnostics (E2/S1-S3, S8) -----------
        /**
         * @brief Test-only: true when the profile chain is installed.
         *
         * Reports the resolved state the render loop reads: `Nes`/`Hybrid`
         * with no caller `setHpfCutoffHz` override.
         * @return true when the render loop runs the three-stage chain.
         */
        bool isOutputFilterChainActiveForTesting() const;

        /**
         * @brief Test-only: true when the LPF is part of the installed chain.
         *
         * False when the chain itself is inactive, and false below the
         * `0.45 * fs >= 6000 Hz` floor, where the chain is the two
         * high-passes only.
         * @return true when the Butterworth stage runs.
         */
        bool isOutputFilterLpfActiveForTesting() const;

        /** @brief Test-only: Q15 pole of the 90 Hz high-pass. */
        int32_t getOutputFilterHpf1RQ15ForTesting() const;
        /** @brief Test-only: Q15 pole of the 440 Hz high-pass. */
        int32_t getOutputFilterHpf2RQ15ForTesting() const;
        /** @brief Test-only: Q15 `b0` of the Butterworth LPF. */
        int32_t getOutputFilterLpfB0Q15ForTesting() const;
        /** @brief Test-only: Q15 `b1` of the Butterworth LPF. */
        int32_t getOutputFilterLpfB1Q15ForTesting() const;
        /** @brief Test-only: Q15 `b2` of the Butterworth LPF. */
        int32_t getOutputFilterLpfB2Q15ForTesting() const;
        /** @brief Test-only: Q15 `a1` of the Butterworth LPF. */
        int32_t getOutputFilterLpfA1Q15ForTesting() const;
        /** @brief Test-only: Q15 `a2` of the Butterworth LPF. */
        int32_t getOutputFilterLpfA2Q15ForTesting() const;
        /** @brief Test-only: resolved 90 Hz corner. */
        float getOutputFilterHpf1CutoffHzForTesting() const;
        /** @brief Test-only: resolved 440 Hz corner. */
        float getOutputFilterHpf2CutoffHzForTesting() const;
        /** @brief Test-only: resolved LPF corner; 0 when the LPF is off. */
        float getOutputFilterLpfCutoffHzForTesting() const;

        /**
         * @brief Test-only: largest |state| across both output stages.
         *
         * Covers the chain's four recursions and the historical one-pole, so a
         * stability run can assert the recursion stays inside the int16 bound
         * instead of inferring it from the output.
         * @return Maximum absolute state value.
         */
        int32_t getOutputFilterStateMaxAbsForTesting() const;

        /** @brief Test-only: clears both output stages' recursion state. */
        void resetOutputFilterStateForTesting();

        /**
         * @brief Test-only: runs `applyOutputStage` over an injected buffer.
         *
         * Same function `renderIntegerCore` calls per sample, including the
         * final int16 clamp and the master bitcrush, so a test observes the
         * production stage and not a copy of its math.
         * @param buffer Mono int16 samples, rewritten in place.
         * @param length Number of samples.
         */
        void processOutputStageForTesting(int16_t* buffer, int length);
#endif

    private:
        static constexpr int kInvalidVoiceSlot = -1;

        void processCommands();
        void updateMusicSequencer();
        /** Rebuild a track's note index / next-tick state for an absolute target tick. */
        void resyncTrackSequencerToTick(size_t track_index, uint64_t target_tick);
        void executePlayEvent(const AudioEvent& event, uint64_t gate_samples = 0);
        void playSequencerPercussionHit(const AudioEvent& event);
        void initVoiceFromEvent(Voice* voice, const AudioEvent& event, uint64_t gate_samples,
                                bool music_sequencer_legato = false);
        void beginReleaseOnMusicTrack(size_t track_index);
        void clearMusicTrackVoiceState();
        Voice* musicVoiceForTrack(size_t track_index);
        Voice* findVoiceForSfxEvent(WaveType type);
        Voice* findVoiceForPercussionHit();
        float generateSampleForVoice(Voice& voice);

        // -- Render core selection (S3) ------------------------------------
        /**
         * @brief Reports whether the active profile renders through the integer core.
         *
         * Derived from the profile selector and the build's float-path switch,
         * never from `PR32_APU_HAS_FPU`: on an FPU target `Nes`/`Hybrid` still
         * pick the integer body, and with `PR32_APU_ENABLE_LEGACY_FLOAT == 0`
         * `Legacy` falls back to it as well.
         *
         * Evaluated once per rendered block, never per sample.
         *
         * @return true if `generateSamples` must use the integer core.
         */
        bool profileUsesIntegerCore() const {
            return (profile_ != SoundProfile::Legacy) || (PR32_APU_ENABLE_LEGACY_FLOAT == 0);
        }

        /**
         * @brief Renders one block through the portable integer/Q15 core.
         *
         * The single core every target can run (S2/S3), so this definition is
         * never compiled out. `generateSamples` owns the shared prologue
         * (command drain, sequencer) and the post-mix hook; this method owns
         * only the per-sample loop.
         *
         * @param stream Output buffer (mono, int16 samples).
         * @param length Number of samples; the caller guarantees length > 0.
         */
        void renderIntegerCore(int16_t* stream, int length);

#if PR32_APU_ENABLE_LEGACY_FLOAT
        /**
         * @brief Renders one block through the frozen `Legacy` float core.
         *
         * S1: this body is a contract. Only the mechanical extraction that
         * created it may touch it — no sound fix or optimization lands here.
         * Compiled only when the float path is enabled for this build.
         *
         * @param stream Output buffer (mono, int16 samples).
         * @param length Number of samples; the caller guarantees length > 0.
         */
        void renderFloatLegacy(int16_t* stream, int length);
#endif

        /**
         * @brief Applies the active profile's character to one voice.
         *
         * S5: the only place a profile writes character — envelope curve, mix
         * weighting, output filters, default duty, noise LUT, unipolar output
         * and pan/echo enablement. Called at the very end of
         * `initVoiceFromEvent`, after the `nesAccurate` / `initNesSafe` block,
         * so the note's own values are already in place when it runs.
         *
         * The mix weighting is profile character too, but it has no per-voice
         * field: `renderIntegerCore` resolves the `MixerCurve` from the
         * profile once per block (S4), so there is nothing for this function
         * to write for it and nothing that could be half-applied per voice.
         *
         * E0: `Nes`/`Hybrid` are exact aliases and therefore write nothing —
         * the note path already leaves those values (S3). E1 fills the body:
         * the envelope curve (S2), the noise character (S5) and the stepped
         * PULSE duty (S6), each as a DEFAULT the note or the voice may pin.
         *
         * @param slot Voice index [0, MAX_VOICES); the caller guarantees it is
         *             a valid voice of the pool.
         */
        void applyProfileCharacter(int slot);

        // -- Channels and I/O ---------------------------------------------
        Voice voices[MAX_VOICES];
        AudioCommandQueue commandQueue;
        std::atomic<uint32_t> droppedCommands{0};

        int sampleRate = 44100;
        float masterVolume = 1.0f;
        int32_t masterVolumeScale = 65536; // Q16 for integer/LUT path
        /** 0 = off; 1–15 = re-quantize final int16 (post-mixer). */
        uint8_t masterBitcrushBits_ = 0;

        // -- Anti-click + DC removal --------------------------------------
        // Single-pole high-pass filter `y[n] = x[n] - x[n-1] + R*y[n-1]`,
        // approximating the NES APU's output HPF and removing the DC that
        // asymmetric pulse duties introduce. Hito 4 M10 made the corner
        // configurable; see `setHpfCutoffHz`.
        float hpfPrevIn = 0.0f;
        float hpfPrevOut = 0.0f;

        // Q15 HPF state for no-FPU path (ESP32-C3, RISC-V cores)
        int32_t hpfPrevInQ15 = 0;
        int32_t hpfPrevOutQ15 = 0;

        // -- HPF configuration (Hito 4 M10) --------------------------------
        /** Historical fixed coefficient: ~35.2 Hz at 44100, ~17.6 Hz at 22050. */
        static constexpr float kHpfLegacyR = 0.995f;
        /** Q15 mirror of `kHpfLegacyR` (0.995 * 32768 = 32604.16). */
        static constexpr int32_t kHpfLegacyRQ15 = 32604;

        /**
         * Requested corner in Hz. Negative is the sentinel for "use the
         * legacy fixed coefficient", which is the default so the canonical
         * golden PCM is bit-identical. 0 means the filter is bypassed.
         */
        float hpfCutoffHz_ = -1.0f;
        bool hpfEnabled_ = true;
        float hpfR_ = kHpfLegacyR;
        int32_t hpfRQ15_ = kHpfLegacyRQ15;
        /**
         * S8: true once the caller has called `setHpfCutoffHz`. A set corner
         * outranks the profile's output chain; `reset()` clears it.
         */
        bool hpfCutoffOverridden_ = false;

        // -- Output filter chain, `Nes` / `Hybrid` (E2/S1) ------------------
        //
        // One shared stage on the summed mix: HPF 90 Hz -> HPF 440 Hz ->
        // Butterworth LPF. The one-pole above is the `Legacy` stage and keeps
        // running unchanged when the chain is inactive; the render loop picks
        // one or the other per sample through `applyOutputStage`.
        //
        // The two first-order high-passes are DELIBERATELY kept as two
        // sections instead of being merged into one second-order section with
        // `b = [1, -2, 1]`. The merge is exact in real arithmetic, but the
        // chain input is already quantised to int16: the second difference of
        // a 20 Hz tone at half scale is ~0.5 LSB, below the ~2 LSB of
        // quantisation noise it differentiates, so the merged form loses the
        // bottom of the band (measured -45 dB instead of -0.35 dB at 20 Hz).
        // The cascade's intermediate state is what keeps that resolution, and
        // the cost is recovered below by replacing the 64-bit divisions with
        // truncating shifts and the 64-bit products with int32 ones.
        //
        // All coefficients are Q15 in `int32_t`: the LPF's `b1` can exceed 1
        // (up to ~2 near 0.45 * fs), so it does not fit the 16-bit range the
        // one-pole uses. Products are int32_t because |coefficient| < 2^16 and
        // |state| < 2^15; only the SUM of the LPF's five products needs
        // `int64_t`.
        static constexpr float kOutputHpf1Hz = 90.0f;
        static constexpr float kOutputHpf2Hz = 440.0f;
        static constexpr float kOutputLpfMaxHz = 14000.0f;
        /** 0.45 * fs floor below which the LPF would gut the passband. */
        static constexpr float kOutputLpfMin45FsHz = 6000.0f;

        /**
         * Bounds that make the fixed-point arithmetic provably safe.
         *
         * `kOutputCoeffQ15Max` is below 2^16 and every coefficient is clamped
         * to it, and `kOutputStateQ15Max` is 2^15 and every recursion state is
         * saturated to the int16 rail before it is stored. A product is
         * therefore below 2^31 and fits `int32_t`; a five-term sum does not,
         * which is why the LPF accumulator is `int64_t`.
         */
        static constexpr int32_t kOutputCoeffQ15Max = 65535;
        static constexpr int32_t kOutputStateQ15Max = 32768;
        static_assert(kOutputCoeffQ15Max < 65536,
                      "output chain coefficient must stay below 2^16");
        static_assert(kOutputStateQ15Max <= 32768,
                      "output chain state must stay inside the int16 rail");
        static_assert(
            static_cast<int64_t>(kOutputCoeffQ15Max) * kOutputStateQ15Max <=
                2147483647LL,
            "one coefficient*state product must fit int32_t");
        static_assert(
            5 * static_cast<int64_t>(kOutputCoeffQ15Max) * kOutputStateQ15Max <=
                9223372036854775807LL,
            "the five-term accumulator must fit int64_t");

        /** True when the profile owns the stage and no caller override exists. */
        bool outputFilterActive_ = false;
        /** False below the 6 kHz floor; the chain is then the two HPFs only. */
        bool outputLpfActive_ = false;
        int32_t outHpf1RQ15_ = 0;
        int32_t outHpf2RQ15_ = 0;
        int32_t outLpfB0Q15_ = 0;
        int32_t outLpfB1Q15_ = 0;
        int32_t outLpfB2Q15_ = 0;
        int32_t outLpfA1Q15_ = 0;
        int32_t outLpfA2Q15_ = 0;
        // Recursion state, all clamped to the int16 range by the stage itself.
        int32_t outHpf1PrevInQ15_ = 0;
        int32_t outHpf1PrevOutQ15_ = 0;
        int32_t outHpf2PrevInQ15_ = 0;
        int32_t outHpf2PrevOutQ15_ = 0;
        int32_t outLpfX1Q15_ = 0;
        int32_t outLpfX2Q15_ = 0;
        int32_t outLpfY1Q15_ = 0;
        int32_t outLpfY2Q15_ = 0;
        // Resolved corners in Hz; `outLpfCutoffHz_` is 0 when the LPF is off.
        float outHpf1CutoffHz_ = kOutputHpf1Hz;
        float outHpf2CutoffHz_ = kOutputHpf2Hz;
        float outLpfCutoffHz_ = 0.0f;

        // -- Soft-clip configuration (Hito 4 M11) --------------------------
        /**
         * Default `Rational` keeps the historical curve. On the integer
         * path that specifically means keeping `audio_mixer_lut`, which is
         * pre-fitted to it, so the canonical golden PCM is unchanged.
         */
        SoftClipMode softClipMode_ = SoftClipMode::Rational;

        // -- Sound profile (S4) --------------------------------------------
        /**
         * Global character set for every voice. Configuration, not state:
         * `init()` leaves it alone, so a stream restart keeps the caller's
         * choice. T4 adds the core dispatch and the hot-switch
         * synchronisation here.
         */
        SoundProfile profile_ = SoundProfile::Legacy;

        /**
         * Derive `hpfR_` / `hpfRQ15_` from `hpfCutoffHz_` and the current
         * sample rate. Called by `init` (so a configured corner follows a
         * rate change) and by `setHpfCutoffHz`. Never called per sample.
         */
        void recomputeHpfCoefficients();

        /**
         * @brief Derive the `Nes` / `Hybrid` output chain from profile and rate.
         *
         * Called by `init` (a configured corner is in Hz, so it follows the
         * rate), by `setSoundProfile` (the profile decides whether the chain
         * is installed) and by `setHpfCutoffHz` (the caller override flips it
         * off). Never called per sample or per block: the render loop copies
         * `outputFilterActive_` and the pre-computed Q15 coefficients into its
         * block-local stage snapshot once, and the sample loop only reads that.
         *
         * The LPF is computed with the RBJ low-pass at Q = 1/sqrt(2) and its
         * corner is `min(14000 Hz, 0.45 * fs)`, engaged only when
         * `0.45 * fs >= 6000 Hz`. The transcendentals are confined here, on a
         * configuration change, so no `exp`/`sin`/`cos` reaches the render
         * path even on a soft-float target. They are evaluated in `float`,
         * not `double`: the coefficients are quantised to Q15 immediately, so
         * double precision buys nothing and pulls the soft-float libm
         * transcendants into the image (the E2 flash regression).
         */
        void recomputeOutputFilterCoefficients();

        /**
         * @brief Clears the recursion state of the output chain.
         *
         * A new rate, a new profile or a reset must not leave a stale
         * coefficient/state pair ringing; called from all three paths.
         */
        void resetOutputFilterState();

        /**
         * @struct OutputStageStateQ15
         * @brief Block-local snapshot of the integer output stage.
         *
         * The stage is the only reader and writer of its own state, and it runs
         * on every sample, so the render loop copies it out of the class once
         * per block and writes it back once. Read through `this`, the ten
         * recursion words plus the coefficients and the three flags cost twenty
         * member loads and stores per sample for six multiplies of arithmetic;
         * held in a local, the compiler is free to keep the words that stay
         * live across iterations in registers.
         *
         * It holds no new state and changes no arithmetic: every field is a
         * verbatim copy of a member below, and `captureOutputStageState` /
         * `restoreOutputStageState` are the only two places that move between
         * them. That mechanical copy is what makes the hoist bit-exact — a
         * member added to the stage must be added to this struct and to both
         * helpers, never handled inline at a call site.
         */
        struct OutputStageStateQ15 {
            /** Stage selection; block-constant, resolved on a config change. */
            bool chainActive;          ///< `outputFilterActive_`
            bool lpfActive;            ///< `outputLpfActive_`
            bool legacyHpfEnabled;     ///< `hpfEnabled_`
            /** Coefficients, all Q15 in `int32_t` (see `kOutputCoeffQ15Max`). */
            int32_t legacyRQ15;        ///< `hpfRQ15_`, the historical pole
            int32_t hpf1RQ15;          ///< `outHpf1RQ15_`
            int32_t hpf2RQ15;          ///< `outHpf2RQ15_`
            int32_t lpfB0Q15;          ///< `outLpfB0Q15_`
            int32_t lpfB1Q15;          ///< `outLpfB1Q15_`
            int32_t lpfB2Q15;          ///< `outLpfB2Q15_`
            int32_t lpfA1Q15;          ///< `outLpfA1Q15_`
            int32_t lpfA2Q15;          ///< `outLpfA2Q15_`
            /** Recursion state; every word is saturated to the int16 rail. */
            int32_t legacyPrevInQ15;   ///< `hpfPrevInQ15`
            int32_t legacyPrevOutQ15;  ///< `hpfPrevOutQ15`
            int32_t hpf1PrevInQ15;     ///< `outHpf1PrevInQ15_`
            int32_t hpf1PrevOutQ15;    ///< `outHpf1PrevOutQ15_`
            int32_t hpf2PrevInQ15;     ///< `outHpf2PrevInQ15_`
            int32_t hpf2PrevOutQ15;    ///< `outHpf2PrevOutQ15_`
            int32_t lpfX1Q15;          ///< `outLpfX1Q15_`
            int32_t lpfX2Q15;          ///< `outLpfX2Q15_`
            int32_t lpfY1Q15;          ///< `outLpfY1Q15_`
            int32_t lpfY2Q15;          ///< `outLpfY2Q15_`
        };

        /**
         * @brief Copies the live output stage into a block-local snapshot.
         *
         * Called once per rendered block, never per sample.
         *
         * @return Snapshot of every field `OutputStageStateQ15` names.
         */
        OutputStageStateQ15 captureOutputStageState() const;

        /**
         * @brief Writes a block-local snapshot's recursion state back.
         *
         * The counterpart of `captureOutputStageState`; called once per block,
         * after the sample loop, so the next block (and every test accessor)
         * sees the state the loop ended on. Only the recursion words travel
         * back: the stage's configuration is read-only for the duration of a
         * block, because its writers (`recomputeOutputFilterCoefficients`,
         * `setSoundProfile`, `setHpfCutoffHz`, `resetOutputFilterState`) never
         * run inside the loop.
         *
         * @param state Snapshot produced by `captureOutputStageState`.
         */
        void restoreOutputStageState(const OutputStageStateQ15& state);

        /**
         * @brief The complete integer output stage for one Q15 sample.
         *
         * The single entry point `renderIntegerCore` uses: the profile chain
         * (HPF 90 Hz -> HPF 440 Hz -> Butterworth LPF) when `chainActive`,
         * otherwise the historical single-pole HPF, which is the frozen
         * `Legacy` arithmetic. Extracted verbatim so the same code the renderer
         * runs can be driven over a test buffer.
         *
         * Every stage saturates its own state to [-32768, 32767], so a corrupt
         * coefficient can never wrap the recursion. Every Q15 feedback product
         * is `int32_t` (see the bounds above) and is rescaled by a truncating
         * shift, not a division and not an arithmetic shift: a floor shift
         * freezes the DC decay on a permanent offset, while division truncates
         * toward zero (see the historical one-pole comment).
         *
         * `static` and `inline` because it is a pure function of its arguments:
         * it reads nothing from `this`, so the per-sample path can carry the
         * state in a local and the call disappears into the loop body.
         *
         * @param inputQ15 Post-shaping mix sample in Q15.
         * @param state Stage configuration and recursion state, updated in place.
         * @return Stage output, clamped to the int16 range.
         */
        static inline int32_t applyOutputStage(int32_t inputQ15,
                                               OutputStageStateQ15& state);

        // -- Profiling ring buffer (thread-safe offload) --------------------
        ProfileEntry profileRing[PROFILE_RING_SIZE];
        uint8_t profileHead = 0;
        uint8_t profileCount = 0;
        std::atomic<uint8_t> profileWriteIdx{0};

        // -- Legacy diagnostics (keep for non-profiling builds) ----------
        float currentPeak = 0.0f;
        uint64_t samplesSinceLog = 0;

        // -- Timing -------------------------------------------------------
        uint64_t audioTimeSamples = 0;

        // -- Music sequencer ---------------------------------------------
        const MusicTrack* tracks[MAX_MUSIC_TRACKS] = {nullptr, nullptr, nullptr, nullptr};
        size_t currentNoteIndices[MAX_MUSIC_TRACKS] = {0, 0, 0, 0};
        uint64_t nextNoteTicks[MAX_MUSIC_TRACKS] = {0, 0, 0, 0};
        uint64_t globalTickCounter = 0;
        uint64_t tickDurationSamples = 0;
        float tempoBPM = DEFAULT_BPM;
        size_t activeTrackCount = 0;
        float tempoFactor = 1.0f;

        // Sequencer note limit per frame (bounded processing to prevent audio starvation)
        std::atomic<size_t> sequencerNoteLimit{MAX_NOTES_PER_FRAME};
        std::atomic<size_t> deferredNotes{0};

        // Expose transport flags atomically so MusicPlayer can observe
        // natural end-of-track without polling private state.
        std::atomic<bool> musicPlayingFlag{false};
        std::atomic<bool> musicPausedFlag{false};

        /** Transport clock mirrors (absolute ticks) for playhead observers. */
        std::atomic<uint64_t> musicGlobalTick_{0};
        std::atomic<uint64_t> musicPlayStartTick_{0};

        /** After MUSIC_PLAY, allow one sequencer pass even when currentTick == startTick. */
        bool firstSequencerCallAfterPlay_ = false;

        /** True while a track's melodic gate is active (for scoped Rest note-off). */
        bool track_voice_active_[MAX_MUSIC_TRACKS] = {};

        // -- Post-mix hook -------------------------------------------------
        void (*postMixMono_)(int16_t* mono, int length, void* user) = nullptr;
        void* postMixUser_ = nullptr;

        // -- NES frame counter (Hito 2 M3) -----------------------------
        // Default-constructed: mode 0, currentStep 0, all clocks off.
        NesFrameCounter frameCounter_;

        /** Tick the NES envelopes (one of the four quarter-clock consumers). */
        void tickAllNesEnvelopes();
        /** Tick the NES linear counters (quarter-clock consumer). */
        void tickAllNesLinearCounters();
        /** Tick the NES length counters (half-clock consumer). */
        void tickAllNesLengthCounters();
        /** Tick the NES sweep units (half-clock consumer). */
        void tickAllNesSweepUnits();

        /**
         * Hito 2 M3: per-sample tick of the NES frame counter.
         * `apuCyclesPerSample` is computed once per generateSamples() call
         * and passed in to avoid a per-sample division. No-op when the mode
         * is not 0 or 1.
         */
        void tickNesFrameCounter(double apuCyclesPerSample);

#if defined(UNIT_TEST)
        // Test-only instrumentation: counters incremented in the stubs above
        // so tests can verify the frame-counter dispatch rate.
        int nesQuarterTickCountForTesting_ = 0;
        int nesHalfTickCountForTesting_ = 0;
        int nesLinearCounterTickCountForTesting_ = 0;
        int nesSweepTickCountForTesting_ = 0;
        int nesIrqSetCountForTesting_ = 0;
        /** Applications of `applyProfileCharacter` (S5 ordering proof). */
        uint32_t profileApplyCount_ = 0;
#endif
    };

} // namespace pixelroot32::audio
