/*
 * Copyright (c) 2026 PixelRoot32
 * Licensed under the MIT License
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace pixelroot32::audio {

    // --- Channel Types ---

    enum class WaveType : uint8_t {
        PULSE,
        TRIANGLE,
        NOISE,
        /** Band-limited sine via LUT. */
        SINE,
        /** Polyphonic saw from linear phase ramp. */
        SAW
    };

    // Voice abstraction for SNES-like dynamic pool (internal APU use).
    enum class VoiceType : uint8_t {
        PULSE,
        TRIANGLE,
        NOISE,
        SINE,
        SAW
    };

    constexpr VoiceType toVoiceType(WaveType waveType) {
        switch (waveType) {
            case WaveType::PULSE: return VoiceType::PULSE;
            case WaveType::TRIANGLE: return VoiceType::TRIANGLE;
            case WaveType::NOISE: return VoiceType::NOISE;
            case WaveType::SINE: return VoiceType::SINE;
            case WaveType::SAW: return VoiceType::SAW;
            default: return VoiceType::PULSE;
        }
    }

    constexpr WaveType toWaveType(VoiceType voiceType) {
        switch (voiceType) {
            case VoiceType::PULSE: return WaveType::PULSE;
            case VoiceType::TRIANGLE: return WaveType::TRIANGLE;
            case VoiceType::NOISE: return WaveType::NOISE;
            case VoiceType::SINE: return WaveType::SINE;
            case VoiceType::SAW: return WaveType::SAW;
            default: return WaveType::PULSE;
        }
    }

    /**
     * @struct EnvelopeState
     * @brief Holds ADSR envelope state for a single voice.
     * 
     * Tracks the attack/decay/sustain/release stages and provides both
     * floating-point and Q15 fixed-point variants to support platforms
     * without an FPU (e.g., ESP32-C3 RISC-V).
     */
    struct EnvelopeState {
        /** ADSR stage: ATTACK, DECAY, SUSTAIN, RELEASE, or OFF. */
        enum class Stage : uint8_t { ATTACK, DECAY, SUSTAIN, RELEASE, OFF };
        /** Current ADSR stage. */
        Stage stage = Stage::OFF;
        
        // Timing (in samples, pre-calculated from seconds * sampleRate)
        uint32_t attackSamples = 0;   ///< Samples for attack phase (0 = instantaneous).
        uint32_t decaySamples = 0;    ///< Samples for decay phase.
        float    sustainLevel = 1.0f; ///< Target volume at sustain phase [0.0 - 1.0].
        uint32_t releaseSamples = 0;  ///< Samples for release phase.
        
        // Runtime state
        uint32_t sampleCounter = 0;    ///< Counter within current stage.
        float    currentLevel = 0.0f;  ///< Current envelope amplitude [0.0 - 1.0].

        float attackDelta = 0.0f;    ///< Volume increment per sample during attack: 1.0 / attackSamples.
        float decayDelta = 0.0f;     ///< Volume decrement per sample during decay: (1.0 - sustainLevel) / decaySamples.
        float releaseDelta = 0.0f;   ///< Volume decrement per sample during release: sustainLevel / releaseSamples.

        // --- Fixed-point additions for no-FPU path ---
        int32_t currentLevelQ15 = 0;    ///< Q15 mirror of currentLevel (-32768 to +32768).
        int32_t attackDeltaQ15 = 0;     ///< Q15 attack delta: 32768 / attackSamples.
        int32_t decayDeltaQ15 = 0;      ///< Q15 decay delta: (32768 - sustainLevelQ15) / decaySamples.
        int32_t sustainLevelQ15 = 32768;///< Q15 sustain level.
        int32_t releaseDeltaQ15 = 0;    ///< Q15 release delta: sustainLevelQ15 / releaseSamples.

        /**
         * @brief Resets all envelope state to OFF.
         */
        void reset() {
            stage = Stage::OFF;
            attackSamples = 0;
            decaySamples = 0;
            sustainLevel = 1.0f;
            releaseSamples = 0;
            sampleCounter = 0;
            currentLevel = 0.0f;
            currentLevelQ15 = 0;
            attackDeltaQ15 = 0;
            decayDeltaQ15 = 0;
            sustainLevelQ15 = 32768;
            releaseDeltaQ15 = 0;
        }
    };

    // --- LFO Types ---
    enum class LfoTarget : uint8_t { NONE, PITCH, VOLUME };

    /** Pitch/period sweep interpolation curve (default Linear keeps legacy behavior). */
    enum class SweepCurve : uint8_t { Linear = 0, Exponential = 1 };

    /**
     * @brief Master output shaping curve applied to the summed mix.
     *
     * Runs on the accumulated mix `S` BEFORE the DC blocker, so the filter
     * sees whatever the curve produced — which is what makes `HardClip`
     * sound like the NES: its DAC saturates and the analog high-pass then
     * filters the already-squared-off signal.
     *
     * `Rational` is the default and reproduces the historical behaviour
     * bit-for-bit, including the pre-fitted `audio_mixer_lut` on the
     * integer path. Hito 4 M11.
     */
    enum class SoftClipMode : uint8_t {
        /** No shaping. The mix runs into the final int16 clamp unaltered. */
        None = 0,
        /** `y = S / (1 + |S| * MIXER_K)`. Historical default. */
        Rational = 1,
        /** `y = S * (27 + S^2) / (27 + 9*S^2)`, a rational tanh approximation. */
        Tanh = 2,
        /** `y = clamp(S, -1, 1)`. Models the NES DAC saturating. */
        HardClip = 3
    };

    /**
     * @struct SfxBreakpoint
     * @brief Timed automation point for SFX duty steps or pitch envelope.
     *
     * `value` is duty in [0,1] for duty steps, or frequency/clock Hz for pitch.
     * Tables are static/constexpr in exported banks; AudioEvent holds pointer+count.
     */
    struct SfxBreakpoint {
        float timeSec = 0.0f; ///< Offset from voice start (seconds); non-decreasing in a table.
        float value = 0.0f;   ///< Duty [0,1] or Hz > 0 depending on table context.
    };

    /** Max duty-step breakpoints per AudioEvent (hold semantics). */
    static constexpr uint8_t kMaxSfxDutySteps = 4;
    /** Max pitch-envelope breakpoints per AudioEvent (multi-segment). */
    static constexpr uint8_t kMaxSfxPitchPoints = 4;

    /**
     * @struct LfoState
     * @brief Holds LFO (Low-Frequency Oscillator) state for pitch or volume modulation.
     * 
     * Supports both floating-point and Q15 fixed-point variants for no-FPU platforms.
     * The LFO produces a sine-like waveform that can modulate pitch (vibrato) or
     * volume (tremolo) of a voice.
     */
    struct LfoState {
        bool enabled = false;            ///< Whether the LFO is active.
        LfoTarget target = LfoTarget::NONE;///< What the LFO modulates: PITCH or VOLUME.
        
        float depth = 0.0f;             ///< Modulation depth (pitch: ratio e.g. 0.05; volume: 0.0-1.0).
        uint32_t periodSamples = 0;     ///< LFO period in samples (derived from lfoFrequency).
        
        uint32_t sampleCounter = 0;      ///< Current sample position within the period.
        float currentValue = 0.0f;       ///< Current LFO output value [-1.0 to +1.0].

        // Q15 fixed-point fields for no-FPU path
        int32_t depthQ15 = 0;            ///< Q15 depth: 0-32768 maps to 0.0-1.0.
        int32_t currentValueQ15 = 0;      ///< Q15 output: -32768 to +32768 maps to -1.0 to +1.0.

        uint16_t delaySamples = 0;      ///< Samples to wait before LFO starts (in seconds * sampleRate).
        uint16_t delayCounter = 0;       ///< Countdown for delay phase.
        
        /**
         * @brief Resets all LFO state to disabled.
         */
        void reset() {
            enabled = false;
            target = LfoTarget::NONE;
            depth = 0.0f;
            periodSamples = 0;
            sampleCounter = 0;
            currentValue = 0.0f;
            // Reset Q15 fields for no-FPU path
            depthQ15 = 0;
            currentValueQ15 = 0;
            delaySamples = 0;
            delayCounter = 0;
        }
    };

    /**
     * @struct NesFrameCounter
     * @brief NES APU frame counter (sequencer) state.
     *
     * Counts APU cycles (CPU/2) and dispatches quarter/half clocks at the
     * step boundaries defined by nes_apu::kMode0ApuCyclesPerStep (4-step,
     * mode 0) and nes_apu::kMode1ApuCyclesPerStep (5-step, mode 1).
     *
     * Default state: enabled=false (counter OFF), mode=0, all clocks
     * disabled. Consumers opt in with `ApuCore::setNesFrameCounterMode(0 or 1)`.
     * Hito 2 M3.
     */
    struct NesFrameCounter {
        /** 0 = 4-step (mode 0), 1 = 5-step (mode 1). Default is 0. */
        int mode = 0;

        /**
         * When false, the frame counter does not advance and dispatches no
         * clocks. Set to true by `ApuCore::setNesFrameCounterMode` and reset
         * to false by `ApuCore::reset` (or unit-test reset). Default OFF
         * keeps the canonical track PCM byte-for-byte identical.
         */
        bool enabled = false;

        /** Current step within the cycle (0 = wrap point, 1..maxStep = active). */
        int currentStep = 0;

        /** Continuous APU-cycle accumulator. Doubled per render sample. */
        double apuCycles = 0.0;

        /** Frame interrupt flag. Set on mode-0 cycle wrap (if not inhibited). */
        bool irqFlag = false;

        /** $4017 bit 6 (inhibit). When true, the IRQ flag is cleared and not set. */
        bool irqInhibit = false;

        /**
         * Reset pending: when true, the next "step entry" zeroes the cycle
         * accumulator and fires quarter+half (mirrors $4017 M=1 side effect).
         */
        bool resetPending = false;
    };

    /**
     * @struct NesLengthCounter
     * @brief NES APU length counter state for a single voice.
     *
     * The length counter is a half-frame clocked countdown. Once loaded
     * (via `ApuCore::setVoiceNesLength`) it decrements at ~120 Hz (mode 0)
     * or ~96 Hz (mode 1). When the counter reaches 0, the voice is silenced
     * directly. While `halt` is set, the counter is frozen (does not
     * decrement).
     *
     * The `enabled` field mirrors the NES channel enable bit ($4015):
     * when clear, the counter is forced to 0 and the voice is silenced.
     * When set, the counter can be loaded and decremented.
     *
     * Default state: enabled=false, counter=0, halt=false. SINE/SAW voices
     * keep `enabled=false` permanently and are unaffected by the dispatch
     * loop. Hito 2 M4.
     *
     * Source: https://www.nesdev.org/wiki/APU_Length_Counter
     */
    struct NesLengthCounter {
        /** Mirrors $4015 channel-enable bit. False = forced silence, true = armed. */
        bool enabled = false;
        /** Halt flag (shared with envelope loop / linear counter). True = freeze decrement. */
        bool halt = false;
        /** 0..31; the loaded NES LUT index. */
        uint8_t index = 0;
        /** Remaining ticks; 0 = silenced on next half-clock. */
        uint16_t counter = 0;
    };

    /**
     * @struct NesLinearCounter
     * @brief NES APU linear counter state for the TRIANGLE channel.
     *
     * The linear counter is a quarter-frame clocked 7-bit countdown shared
     * with the TRIANGLE channel. A write to $4008 sets the reload value
     * (lower 7 bits) and the reload flag. On the next quarter-clock:
     *   - if `reloadFlag` is set, the counter is loaded from `reloadValue`;
     *   - else if the counter is > 0, it is decremented;
     *   - if the shared `control` flag (i.e. `NesLengthCounter::halt`) is
     *     clear, the reload flag is also cleared.
     *
     * The shared `halt` flag is read through the `lengthCounter` pointer,
     * which is bound in `AudioChannel::reset()`. This avoids duplicating
     * the flag and keeps `$4008`'s control bit and `$400B`'s halt bit
     * behaviour in lockstep with the canonical NES semantics.
     *
     * When the linear counter is enabled (`linearEnabled == true`) and the
     * counter is 0, the TRIANGLE sequencer is gated per-sample (output 0,
     * phase frozen) but the voice is NOT disabled. Re-loading via a
     * $4008 write resumes the voice without needing a fresh play event.
     *
     * Default OFF: `linearEnabled == false` on construction, so the
     * counter does not tick and the canonical track PCM is unchanged.
     * Hito 2 M5.
     *
     * Source: https://www.nesdev.org/wiki/APU_Linear_Counter
     */
    struct NesLinearCounter {
        /** Per-voice opt-in. False = counter does not tick; bypasses the gate. */
        bool linearEnabled = false;
        /** Set by $4008 write; cleared on quarter-clock if the shared halt/control flag is 0. */
        bool reloadFlag = false;
        /** 7-bit reload value (high bits ignored). */
        uint8_t reloadValue = 0;
        /** Current value, decremented at quarter-clock rate (240 Hz NTSC mode 0). */
        uint8_t counter = 0;
        /** Bound in AudioChannel::reset() to share the halt/control flag with the length counter. */
        NesLengthCounter* lengthCounter = nullptr;
    };

    /**
     * @struct NesSweepUnit
     * @brief NES APU sweep unit state for the PULSE channels.
     *
     * Mirrors the $4001 / $4005 sweep register pair. Operates on the
     * 11-bit timer period (NES PULSE NTSC = 111860.8 / (timer+1) Hz).
     * Clocks at half-frame rate (120 Hz NTSC mode 0); on each tick the
     * unit computes a target period, evaluates the muting conditions,
     * and optionally updates `timer` from the target.
     *
     * The muting flag is evaluated CONTINUOUSLY (the "NES bug" from
     * https://www.nesdev.org/wiki/APU_Sweep) — even when `sweepEnabled`
     * is false or `shift` is 0, the dispatcher still computes the
     * target and updates `muted`. The hot path (PULSE generation)
     * checks `muted` per-sample and gates the output to 0.
     *
     * That continuous evaluation is scoped to voices that opted in via
     * `unitEnabled`. Without that gate the rule `period < 8` would fire
     * on the default `timer == 0`, so merely starting the frame counter
     * would silence every PULSE voice driven by the legacy float
     * frequency path. See `unitEnabled` below.
     *
     * Default state: all fields zero / false. The canonical track never
     * enables NES mode, so the rendered PCM is byte-for-byte identical.
     * Hito 2 M6.
     *
     * Source: https://www.nesdev.org/wiki/APU_Sweep
     */
    struct NesSweepUnit {
        /**
         * Per-voice opt-in for the whole sweep unit. NOT a hardware
         * register bit — it is the same "does this voice participate in
         * the NES model at all" gate as `NesLengthCounter::enabled`,
         * `NesLinearCounter::linearEnabled` and
         * `NesEnvelope::envelopeEnabled`. Set it with
         * `ApuCore::setVoiceNesSweepUnitEnabled`.
         *
         * INVARIANT: `muted` is only ever true while this is true.
         * The per-sample hot path relies on it and tests `muted` alone.
         */
        bool unitEnabled = false;
        /** E bit ($4001 bit 7 / $4005 bit 7). Enables the timer update path. */
        bool sweepEnabled = false;
        /** P (3 bits) — divider period (NTSC mode 0: half-frame rate). */
        uint8_t period = 0;
        /** N bit — negate (true = subtract, false = add). */
        bool negate = false;
        /** SSS (3 bits) — shift amount. 0 disables the timer-update branch. */
        uint8_t shift = 0;
        /** Set by $4001 / $4005 write; cleared on the next half-clock tick. */
        bool reloadFlag = false;
        /** Current divider counter. */
        uint8_t dividerCounter = 0;
        /** true = PULSE 2 (two's complement negate); false = PULSE 1 (ones' complement negate). */
        bool isPulse2 = false;
        /** 11-bit NES timer period (0..2047). Source of truth for the oscillator freq. */
        uint16_t timer = 0;
        /**
         * Continuously-evaluated muting flag. True when the channel is
         * silenced (current period < 8 OR target > 0x7FF). Test-only
         * field exposed by the `getVoiceNesSweepMutedForTesting` accessor.
         */
        bool muted = false;
    };

    /**
     * @struct NesEnvelope
     * @brief NES APU envelope unit state for PULSE 1, PULSE 2, and NOISE.
     *
     * Mirrors the $4000 / $4004 / $400C write side effects (loop flag,
     * constant volume flag, VVVV) and the $4003 / $4007 / $400F length
     * load side effect (start flag). Clocks at quarter-frame rate
     * (240 Hz NTSC mode 0) via the M3 `tickAllNesEnvelopes()` dispatcher.
     *
     * The loop flag is SHARED with the length counter (`halt` bit):
     * `tickAllNesEnvelopes()` reads `lengthCounter->halt` to decide whether
     * the decay level should wrap to 15 on underflow (loop) or silence
     * the channel (!loop). The `lengthCounter` pointer is bound in
     * `AudioChannel::reset()` so the two sub-units see the same bit.
     *
     * When `envelopeEnabled == true` on a PULSE or NOISE voice, the
     * envelope's `output` (0..15) REPLACES the ADSR `envelope.currentLevel`
     * as the per-sample volume source. When the envelope reaches `0` and
     * loop is clear, the voice is silenced directly (matches the NES
     * hardware, which gates the channel on the envelope output).
     *
     * Default state: `envelopeEnabled == false` on construction, so the
     * quarter-frame dispatch is a no-op and the canonical track PCM
     * remains byte-for-byte identical. Hito 2 M7.
     *
     * Source: https://www.nesdev.org/wiki/APU_Envelope
     */
    struct NesEnvelope {
        /** Per-voice opt-in. False = quarter-clock dispatch is a no-op for this voice. */
        bool envelopeEnabled = false;
        /** Set by $4003 / $4007 / $400F write; cleared on the next quarter-clock. */
        bool startFlag = false;
        /** C bit (constant volume flag). True = use VVVV directly as output. */
        bool constVolume = false;
        /** VVVV (4 bits) — constant volume (C=1) or divider reload value (C=0). */
        uint8_t volume = 0;
        /** Current divider counter, decremented each quarter-clock while > 0. */
        uint8_t dividerCounter = 0;
        /** 0..15. Decremented on each divider wrap; wraps to 15 on loop. */
        uint8_t decayLevel = 0;
        /** 0..15, computed output = constVolume ? volume : decayLevel. */
        uint8_t output = 0;
        /** Bound in AudioChannel::reset() to share the halt/loop flag with the length counter. */
        NesLengthCounter* lengthCounter = nullptr;
    };

    /**
     * @struct AudioChannel
     * @brief Represents the internal state of a single audio channel.
     * 
     * Designed to be static and memory-efficient.
     */
    struct AudioChannel {
        bool enabled = false;
        WaveType type;
        
        // Oscillator state (float — used by FPU and native paths)
        float frequency = 0.0f;
        float phase = 0.0f;
        float phaseIncrement = 0.0f; // Pre-calculated (freq / sampleRate)

        // Fixed-point mirror of the oscillator state — used by the no-FPU
        // (ESP32-C3 RISC-V) hot path so we avoid soft-float emulation for
        // the phase accumulator, which runs NUM_CHANNELS × sampleRate
        // times per second.
        // phaseQ32 wraps naturally at 2^32, encoding one period.
        uint32_t phaseQ32 = 0;
        uint32_t phaseIncQ32 = 0;
        uint32_t dutyCycleQ32 = 0x80000000u; // 50% default
        uint32_t basePhaseIncQ32 = 0;

        // Envelope / Volume
        EnvelopeState envelope;
        float volume = 0.0f;       // Current volume [0.0 - 1.0]
        float targetVolume = 0.0f; // Target volume for interpolation
        float volumeDelta = 0.0f;  // Volume change per sample
        
        // LFO
        LfoState lfo;

        // Wave specific parameters
        float dutyCycle = 0.5f;      // For Pulse wave [0.0 - 1.0]
        float dutySweep = 0.0f;      // Duty cycle change per sample
        int32_t dutySweepQ32 = 0;    // Fixed-point duty sweep
        /** Duty stepped table (PULSE); nullptr/0 = use duty + dutySweep. */
        const SfxBreakpoint* dutySteps = nullptr;
        uint8_t dutyStepCount = 0;
        /** Next duty step index to apply; == count when finished. */
        uint8_t dutyStepIndex = 0;
        /** Sample age at which the next duty step applies (UINT32_MAX = none). */
        uint32_t dutyNextBoundarySamples = 0xFFFFFFFFu;
        /** Samples since voice start (duty/pitch automation clock). */
        uint32_t automationAgeSamples = 0;
        /** Pitch envelope table; runtime multi-segment when count >= 2. */
        const SfxBreakpoint* pitchEnvelope = nullptr;
        uint8_t pitchEnvelopeCount = 0;
        /** Start-point index of the active pitch segment (0 .. count-2). */
        uint8_t pitchSegIndex = 0;
        /** Absolute sample age at the start of the active pitch segment. */
        uint32_t pitchSegStartAge = 0;
        /** Length of the active pitch segment in samples (0 = hold final value). */
        uint32_t pitchSegLenSamples = 0;
        uint16_t lfsrState = 0x7FFF; // NES-style 15-bit LFSR for deterministic noise.
                                   // Seeded to all-1s (0x7FFF) to match the NES APU state
                                   // after a write to register $400F (length counter load
                                   // resets the LFSR to 0x7FFF). Hito 1 M9.
        // NES register $400E bit 4 (T) maps INVERTED to this flag:
        //   noiseLfsrShort == true  <->  NES T = 0 (noisy/metallic, 93-step, tap bit 6)
        //   noiseLfsrShort == false <->  NES T = 1 (tone mode, 32767-step, tap bit 1)
        // Short alias retained for ABI stability; will be removed in a future major.
        bool noiseLfsrShort = false;
        /** @deprecated Use noiseLfsrShort. Will be removed in a future major version. */
        bool noiseShortMode = false;

        /**
         * @brief When type == TRIANGLE: quantize output to 4-bit NES levels (32 steps).
         *
         * The NES APU drives the triangle wave through a 5-bit linear counter and
         * produces 16 unique positive + 16 negative levels (effectively 4-bit
         * resolution: -1, -13/15, ..., 13/15, +1). When this flag is true, the
         * synthesised triangle is snapped to those 32 levels instead of remaining
         * a smooth ramp.
         *
         * Default is true to match NES-accurate canonical output. Hito 1 M1.
         */
        bool triangleQuantize4Bit = true;

        /**
         * @brief When type == TRIANGLE: double the effective frequency at
         *  initVoiceFromEvent time, matching the NES convention where TRIANGLE
         *  is exactly one octave below PULSE for the same period value.
         *
         * Composers porting a NES piece receive Hz; if the NES period-to-Hz
         * mapping produced 110 Hz on PULSE for the same input, the TRIANGLE
         * voice would be 55 Hz. With this flag, `event.frequency` is treated
         * as the PULSE-equivalent and the TRIANGLE voice is doubled to 110 Hz.
         *
         * Default false: callers that already account for the octave offset
         * should leave it off. Hito 1 M16.
         */
        bool triangleOctaveUp = false;

        /**
         * @brief When type == PULSE: 0..3 selects the NES discrete duty pattern
         *  (12.5% / 25% / 50% / 75%); 255 = use continuous `dutyCycle` (default).
         *
         * 255 acts as a sentinel so a legitimate `0` index (12.5% duty) is
         * distinguishable from the unset / continuous default. Out-of-range
         * values (1..254, >3 except 255) are treated as continuous by the
         * generator; set this per voice via `ApuCore::setVoicePulseDutyMode`.
         *
         * Hito 1 M2.
         */
        uint8_t pulseDutyIndex = 255;

        /**
         * NOISE LUT index: 0..15 select a NES noise period from
         * `nes_apu::kNesNoisePeriodLutNtsc`; 255 = derive the period from
         * `frequency` (default, legacy behaviour).
         *
         * Same sentinel scheme as `pulseDutyIndex`, and the same lifetime:
         * it is a per-slot mode set by the app via
         * `ApuCore::setVoiceNesNoiseLutMode`, not a per-note value, so it
         * survives new notes and is cleared only by `reset()`. Out-of-range
         * values (16..254) are stored as 255 by the setter.
         *
         * Hito 3 M8.
         */
        uint8_t nesNoiseLutIndex = 255;

        /** Samples until next LFSR step on NOISE; `frequency` sets noise clock rate (not pitch). */
        uint32_t noisePeriodSamples = 1;
        /** Counts down each output sample; at 0 the LFSR advances and reloads to noisePeriodSamples. */
        uint32_t noiseCountdown = 0;

        // Duration control (sample-accurate timing)
        uint64_t remainingSamples = 0;
        /** Continuous voice: no auto-disable; cleared only by STOP_CHANNEL / steal. */
        bool loop = false;

        // Optional frequency/period sweep (melodic waves + NOISE clock)
        uint32_t sweepSamplesTotal = 0;   ///< Total samples for the sweep.
        uint32_t sweepSamplesRemaining = 0;///< Samples remaining in the sweep.
        float sweepStartHz = 0.0f;        ///< Starting frequency in Hz (NOISE: LFSR clock).
        float sweepEndHz = 0.0f;          ///< Ending frequency in Hz (NOISE: LFSR clock).
        uint32_t sweepStartIncQ32 = 0;    ///< Melodic: Q32 phase inc start; NOISE: start period.
        uint32_t sweepEndIncQ32 = 0;      ///< Melodic: Q32 phase inc end; NOISE: end period.
        SweepCurve sweepCurve = SweepCurve::Linear; ///< Active sweep curve (may fallback to Linear).
        float sweepLogRatio = 0.0f;       ///< FPU Exponential: logf(endHz/startHz).
        int32_t sweepLogStartQ16 = 0;     ///< Q15 path Exponential: log2(start) in Q16.
        int32_t sweepLogDeltaQ16 = 0;     ///< Q15 path Exponential: log2(end/start) in Q16.

        /**
         * @brief NES APU length counter state for this voice.
         *
         * The NES length counter is a per-voice half-frame clocked countdown
         * that, when it reaches 0, silences the voice directly. SINE/SAW
         * slots keep `enabled = false` permanently and are unaffected by the
         * dispatch loop; only NES voices (PULSE/TRIANGLE/NOISE) opt in.
         *
         * Default OFF: the canonical track never touches this field, so the
         * rendered PCM is byte-for-byte identical. Hito 2 M4.
         *
         * Note: appended at the END of AudioChannel to keep aggregate
         * initializers in test literals positionally stable (see Hito 0/1
         * lessons on aggregate-initializer hazards).
         */
        NesLengthCounter nesLengthCounter;

        /**
         * @brief NES APU linear counter state for this voice (TRIANGLE).
         *
         * SINE/SAW/PULSE/NOISE voices keep `linearEnabled = false`
         * permanently and are unaffected by the quarter-clock dispatch.
         * Only TRIANGLE voices opt in via `setVoiceNesLinearEnabled`.
         *
         * `lengthCounter` is bound in `reset()` to point at this voice's
         * `nesLengthCounter`, so the shared halt/control flag is read in
         * one place (the length counter) and the linear counter picks it
         * up via pointer.
         *
         * Default OFF: `linearEnabled == false` on construction, so the
         * counter does not tick and the canonical track PCM is unchanged.
         * Hito 2 M5.
         *
         * Note: appended at the END of AudioChannel to keep aggregate
         * initializers in test literals positionally stable (see Hito 0/1
         * lessons on aggregate-initializer hazards).
         */
        NesLinearCounter nesLinearCounter;

        /**
         * @brief NES APU sweep unit state for this voice (PULSE).
         *
         * SINE/SAW/TRIANGLE/NOISE voices keep `sweepEnabled = false` and
         * the half-clock dispatch skips them. Only PULSE voices opt in
         * via `setVoiceNesSweep`.
         *
         * Default OFF: all fields zero / false on construction, so the
         * canonical track PCM remains byte-for-byte identical. Hito 2 M6.
         *
         * Note: appended at the END of AudioChannel to keep aggregate
         * initializers in test literals positionally stable (see Hito 0/1
         * lessons on aggregate-initializer hazards).
         */
        NesSweepUnit nesSweepUnit;

        /**
         * @brief NES APU envelope unit state for this voice (PULSE / NOISE).
         *
         * SINE/SAW/TRIANGLE voices keep `envelopeEnabled = false` and the
         * quarter-clock dispatch skips them. Only PULSE and NOISE voices
         * opt in via `setVoiceNesEnvelopeEnabled`.
         *
         * `lengthCounter` is bound in `reset()` to point at this voice's
         * `nesLengthCounter`, so the shared loop/halt flag is read in one
         * place (the length counter) and the envelope picks it up via
         * pointer.
         *
         * Default OFF: `envelopeEnabled == false` on construction, so the
         * quarter-frame dispatch is a no-op and the canonical track PCM
         * remains byte-for-byte identical. Hito 2 M7.
         *
         * Note: appended at the END of AudioChannel to keep aggregate
         * initializers in test literals positionally stable (see Hito 0/1
         * lessons on aggregate-initializer hazards).
         */
        NesEnvelope nesEnvelope;

        /**
         * @brief Resets the channel to a clean disabled state.
         */
        void reset() {
            nesLengthCounter = NesLengthCounter{};
            // Hito 2 M5: bind the shared halt/control flag pointer for the
            // linear counter. The pointer survives subsequent assignments
            // to `nesLengthCounter` because the binding is re-asserted on
            // every reset() call.
            nesLinearCounter = NesLinearCounter{};
            nesLinearCounter.lengthCounter = &nesLengthCounter;
            // Hito 2 M6: zero the NES sweep unit. Default OFF keeps the
            // canonical track PCM byte-for-byte identical.
            nesSweepUnit = NesSweepUnit{};
            // Hito 2 M7: zero the NES envelope unit and bind the shared
            // loop/halt pointer to the length counter. Default OFF keeps
            // the canonical track PCM byte-for-byte identical.
            nesEnvelope = NesEnvelope{};
            nesEnvelope.lengthCounter = &nesLengthCounter;
            enabled = false;
            type = WaveType::PULSE;
            phase = 0.0f;
            phaseQ32 = 0;
            phaseIncQ32 = 0;
            dutyCycleQ32 = 0x80000000u;
            dutySweep = 0.0f;
            dutySweepQ32 = 0;
            dutySteps = nullptr;
            dutyStepCount = 0;
            dutyStepIndex = 0;
            dutyNextBoundarySamples = 0xFFFFFFFFu;
            automationAgeSamples = 0;
            pitchEnvelope = nullptr;
            pitchEnvelopeCount = 0;
            pitchSegIndex = 0;
            pitchSegStartAge = 0;
            pitchSegLenSamples = 0;
            envelope.reset();
            lfo.reset();
            volume = 0.0f;
            remainingSamples = 0;
            loop = false;
            lfsrState = 0x7FFF; // Initialize LFSR to NES-equivalent all-1s seed (Hito 1 M9)
            noiseLfsrShort = false;
            noiseShortMode = false;
            triangleQuantize4Bit = true;  // Hito 1 M1: 4-bit NES levels default ON
            triangleOctaveUp = false;     // Hito 1 M16: octave doubling opt-in
            pulseDutyIndex = 255;         // Hito 1 M2: 255 = continuous (legacy default)
            nesNoiseLutIndex = 255;       // Hito 3 M8: 255 = derive period from frequency
            noisePeriodSamples = 1;
            noiseCountdown = 0;
            sweepSamplesTotal = 0;
            sweepSamplesRemaining = 0;
            sweepStartHz = 0.0f;
            sweepEndHz = 0.0f;
            sweepStartIncQ32 = 0;
            sweepEndIncQ32 = 0;
            sweepCurve = SweepCurve::Linear;
            sweepLogRatio = 0.0f;
            sweepLogStartQ16 = 0;
            sweepLogDeltaQ16 = 0;
        }
    };

    using Voice = AudioChannel;

    // --- Event Types ---
    
    /**
     * @struct AudioEvent
     * @brief A fire-and-forget sound event triggered by the game.
     */
    struct AudioEvent {
        WaveType type;
        float frequency;
        float duration; // seconds
        float volume;   // 0.0 - 1.0
        float duty;     // For pulse only
        uint8_t noisePeriod = 0;  // For NOISE channel: 0 = calc from frequency, >0 = direct LFSR period
        
        /**
         * Optional preset driving ADSR and LFO parameters. 
         * MUST be a pointer to a static, constexpr, or global instance.
         * If nullptr, falls back to legacy default behavior.
         */
        const struct InstrumentPreset* preset = nullptr;

        /**
         * Optional frequency/period sweep (PULSE / TRIANGLE / SINE / SAW / NOISE).
         * Active iff sweepDurationSec > 0 and sweepEndHz > 0.
         * Melodic: starts at `frequency`, ends at `sweepEndHz`.
         * NOISE: interpolates LFSR clock Hz (and thus noisePeriodSamples).
         * Curve: Linear (default) or Exponential (geometric in Hz); falls back to Linear
         * if start/end Hz are not both > 0.
         * Duration clamped to note length for one-shots; full sweepDurationSec when loop.
         * API decision: ADR-A1 in docs/architecture/AUDIO_ROADMAP_SHORT_MEDIUM.md §A.7.
         */
        float sweepEndHz = 0.0f;
        float sweepDurationSec = 0.0f;

        /**
         * Continuous playback: voice stays enabled until STOP_CHANNEL (or steal).
         * When false, duration <= 0 MUST NOT leave a hanging voice (disabled immediately).
         */
        bool loop = false;

        /** Sweep interpolation; additive at end of struct for brace-init safety (0 = Linear). */
        SweepCurve sweepCurve = SweepCurve::Linear;

        /**
         * Optional duty stepped table (PULSE). Active when dutySteps != nullptr and
         * dutyStepCount > 0 (clamped to kMaxSfxDutySteps). Hold between points;
         * ignores InstrumentPreset::dutySweep while active.
         */
        const SfxBreakpoint* dutySteps = nullptr;
        uint8_t dutyStepCount = 0;

        /**
         * Optional multi-breakpoint pitch envelope. Active when count >= 2
         * (clamped to kMaxSfxPitchPoints); then replaces single-segment sweep.
         */
        const SfxBreakpoint* pitchEnvelope = nullptr;
        uint8_t pitchEnvelopeCount = 0;
    };

    // --- Command Types ---

    /**
     * @struct VoiceNesOptions
     * @brief Every per-voice NES opt-in in one place (Hito 4 M14).
     *
     * Each field already has a dedicated setter; this bundles them so a
     * caller can put a voice into (or out of) NES mode in a single atomic
     * write, and so the whole configuration can cross the game/audio thread
     * boundary through `AudioCommandType::SET_NES_OPTIONS`. The individual
     * setters remain the right tool for changing one thing.
     *
     * All defaults are "off", so a default-constructed instance disarms a
     * voice completely.
     *
     * NOTE: `noiseLfsrShort` is deliberately NOT here even though the
     * original M14 sketch listed it. That flag is owned per NOTE by
     * `InstrumentPreset` and rewritten by `initVoiceFromEvent` on every
     * trigger, unlike the per-SLOT modes below. Putting it in this struct
     * would give one field two owners, and the preset would silently win on
     * the next note.
     */
    struct VoiceNesOptions {
        /** Arms the M4 length counter (`$4015` channel enable). */
        bool lengthCounterEnabled = false;
        /** Arms the M5 linear counter (TRIANGLE only). */
        bool linearCounterEnabled = false;
        /** Arms the M7 envelope (PULSE / NOISE only). */
        bool envelopeEnabled = false;
        /** Arms the M6 sweep unit (PULSE only). NOT the `$4001` E bit. */
        bool sweepUnitEnabled = false;
        /** M2 duty mode: 0..3 select NES patterns, 255 = continuous. */
        uint8_t pulseDutyIndex = 255;
        /** M8 noise period: 0..15 select a LUT entry, 255 = from frequency. */
        uint8_t noiseLutIndex = 255;
    };

    enum class AudioCommandType : uint8_t {
        PLAY_EVENT,
        STOP_CHANNEL,
        SET_MASTER_VOLUME,
        SET_MASTER_BITCRUSH,
        MUSIC_PLAY,
        /// Hot-swap sequencer tracks while playback continues (no transport reset).
        MUSIC_UPDATE_TRACKS,
        MUSIC_STOP,
        MUSIC_PAUSE,
        MUSIC_RESUME,
        MUSIC_SET_TEMPO,
        MUSIC_SET_BPM,
        /// Reposition music sequencer to an elapsed-tick offset from piece start.
        MUSIC_SEEK,
        /**
         * Hito 2 M13: arm a voice's NES length counter from the audio
         * command queue. Reads `channelIndex`, `nesLengthIndex` and
         * `nesLengthHalt`. See `ApuCore::setVoiceNesLength`.
         */
        TRIGGER_NES_LENGTH,
        /**
         * Hito 4 M14: apply a whole `VoiceNesOptions` to a voice from the
         * audio command queue. Reads `channelIndex` and `nesOptions`.
         */
        SET_NES_OPTIONS,
    };

    // Forward declaration for MusicTrack
    struct MusicTrack;

    /**
     * @struct AudioCommand
     * @brief Internal command to communicate between game and audio threads.
     */
    struct AudioCommand {
        AudioCommandType type;
        union {
            AudioEvent event;
            uint8_t channelIndex;
            float volume;
            const MusicTrack* track;
            float tempoFactor;
            float bpm;
        };

        // Multi-track support
        static constexpr size_t MAX_SUB_TRACKS = 3;
        /** Used when type == SET_MASTER_BITCRUSH (clamped 0–15; 0 = off). */
        uint8_t masterBitcrushBits = 0;
        /** Used when type == MUSIC_SEEK: elapsed ticks from music play start. */
        uint64_t seekOffsetTicks = 0;
        /** Used when type == TRIGGER_NES_LENGTH: LUT index (clamped 0–31). */
        uint8_t nesLengthIndex = 0;
        /** Used when type == TRIGGER_NES_LENGTH: halt flag latched on the voice. */
        bool nesLengthHalt = false;
        /** Used when type == SET_NES_OPTIONS: the options to apply. */
        VoiceNesOptions nesOptions{};
        const MusicTrack* subTracks[MAX_SUB_TRACKS];
        size_t subTrackCount;

        // Default constructor to allow use in arrays/buffers.
        // Only initializing channelIndex(0) leaves the AudioEvent branch mostly
        // uninitialized; PLAY_EVENT paths must not read a garbage preset pointer.
        AudioCommand()
            : type(AudioCommandType::STOP_CHANNEL),
              masterBitcrushBits(0),
              seekOffsetTicks(0),
              nesLengthIndex(0),
              nesLengthHalt(false),
              nesOptions(),
              subTracks{nullptr, nullptr, nullptr},
              subTrackCount(0) {
            std::memset(static_cast<void*>(&event), 0, sizeof(event));
            channelIndex = 0;
        }
    };

}
