/*
 * Copyright (c) 2026 PixelRoot32
 * Licensed under the MIT License
 */
#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/ApuConfig.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "pixelroot32/apu/AudioMixerLUT.h"
#include "pixelroot32/apu/AudioOscLUT.h"
#include "pixelroot32/apu/AudioOscMipLUT.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pixelroot32::audio {

    // ========================================================================================================
    // Wave generator lambdas (moved to file scope for branch-free dispatch)
    // ========================================================================================================
    // NES PULSE duty patterns (8-step, 1=high, 0=low). Mirrors the canonical
    // table on https://www.nesdev.org/wiki/APU_Pulse. Index 0..3 maps to
    // 12.5% / 25% / 50% / 75% high time respectively. Hito 1 M2.
    static const uint8_t kNesPulseDuty[4][8] = {
        {1,0,0,0,0,0,0,0},  // 12.5%  (1/8 high)
        {0,1,1,0,0,0,0,0},  // 25%    (2/8 high)
        {0,1,1,1,1,0,0,0},  // 50%    (4/8 high)
        {1,0,1,1,1,1,1,0},  // 75%    (6/8 high)
    };

    static auto generatePulseSampleQ15 = [](AudioChannel& ch) -> int32_t {
        if (ch.pulseDutyIndex < 4) {
            // phaseQ32 ranges [0, 2^32); the high 3 bits form the 8-step index
            // (top bit cleared at 2^32 so values map to 0..7).
            const uint32_t step = (ch.phaseQ32 >> 29) & 7u;
            return kNesPulseDuty[ch.pulseDutyIndex][step] ? 32767 : -32767;
        }
        return (ch.phaseQ32 < ch.dutyCycleQ32) ? 32767 : -32767;
    };
    static auto generateTriangleSampleQ15 = [](AudioChannel& ch) -> int32_t {
        const uint32_t p16 = ch.phaseQ32 >> 16;
        int32_t tri_q15 = (p16 < 32768u)
            ? ((int32_t)(p16 << 1) - 32768)
            : (32768 - (int32_t)((p16 - 32768u) << 1));
        if (ch.triangleQuantize4Bit) {
            // NES TRIANGLE: 16 levels on each side. Q15 step = 2 * 32768 / 15.
            // Rounding pattern matches the float path (banker's-style +0.5*step).
            constexpr int32_t kStep = (2 * 32768) / 15;   // = 4369
            constexpr int32_t kHalf = kStep / 2;          // = 2184
            int32_t shifted = tri_q15 + 32768 + kHalf;    // [0..65536+kHalf]
            int32_t idx = shifted / kStep;
            if (idx < 0) idx = 0;
            if (idx > 15) idx = 15;
            tri_q15 = idx * kStep - 32768;
        }
        return tri_q15;
    };
    static auto generateSawSampleQ15 = [](AudioChannel& ch) -> int32_t {
        const int64_t v = ((int64_t)ch.phaseQ32 << 1) - (1LL << 32);
        return (int32_t)(v >> 17);
    };

    // ========================================================================
    // Hybrid SAW: the band-limited mipmap read (E2/T3, S4 + S6)
    // ========================================================================
    // `Legacy` and `Nes` keep `generateSawSampleQ15` above, the ideal ramp with
    // its audible aliasing. Only `Hybrid` gets the tables. The level and the
    // crossfade window are derived from the voice's OWN `phaseIncQ32`, never
    // from `frequency`, so a sweep, LFO or pitch envelope is tracked -- and they
    // are derived once per voice per block, then re-derived only when that
    // increment leaves the band it described. No per-voice state is added
    // (`AudioChannel` is size-pinned); the cache lives on the render stack.

    /**
     * @brief Leading-zero count of a non-zero 32-bit value.
     *
     * The level depends only on which octave `phaseIncQ32` falls in, which is
     * exactly what a leading-zero count answers. GCC/Clang targets (Xtensa
     * `NSAU`, RISC-V `CLZ`, host) compile this to one instruction; the portable
     * branch chain below is for anything else and is never reached on the
     * boards this library ships to. Never called with 0.
     *
     * @param value Non-zero value to normalise.
     * @return Number of leading zero bits, 0..31.
     */
    static inline int leadingZeros32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_clz(value);
#else
        int count = 0;
        if (value <= 0x0000FFFFu) { count += 16; value <<= 16; }
        if (value <= 0x00FFFFFFu) { count += 8;  value <<= 8;  }
        if (value <= 0x0FFFFFFFu) { count += 4;  value <<= 4;  }
        if (value <= 0x3FFFFFFFu) { count += 2;  value <<= 2;  }
        if (value <= 0x7FFFFFFFu) { count += 1; }
        return count;
#endif
    }

    /**
     * @brief Mipmap level for an already-normalised leading-zero count.
     *
     * Level `L` covers `q` in `[2^-(9-L), 2^-(8-L))` (see
     * `AudioOscMipLUT.h`), i.e. `L = floor(log2(q)) + 9`, clamped at both
     * ends. With `inc = q * 2^32`, `8 - clz(inc)` is exactly that expression,
     * so the whole selection is integer and needs no float conversion. This is
     * the single clamp; both the per-sample path and the test hook go through
     * it.
     *
     * @param zeros Leading-zero count of the increment.
     * @return Level in [0, SAW_MIP_LEVEL_COUNT - 1].
     */
    static inline int sawMipLevelFromLeadingZeros(int zeros) {
        const int level = 8 - zeros;
        if (level < 0) return 0;
        if (level > (int)SAW_MIP_LEVEL_COUNT - 1) {
            return (int)SAW_MIP_LEVEL_COUNT - 1;
        }
        return level;
    }

    /**
     * @brief Mipmap level whose octave of `q = f / fs` holds the increment.
     *
     * `inc == 0` means the oscillator does not advance; it answers level 0
     * rather than calling `clz(0)`, which is undefined.
     *
     * @param phaseIncQ32 Phase increment in Q32 cycles per sample.
     * @return Level in [0, SAW_MIP_LEVEL_COUNT - 1].
     */
    static inline int sawMipLevelForPhaseIncQ32(uint32_t phaseIncQ32) {
        if (phaseIncQ32 == 0u) return 0;
        return sawMipLevelFromLeadingZeros(leadingZeros32(phaseIncQ32));
    }

    /**
     * @brief Forces one of the two hot helpers below into its caller.
     *
     * `-Os` (what the ESP32 presets build with) treats `inline` as a hint and
     * leaves a short helper out of line: the table read became a call, so every
     * `Hybrid` SAW sample paid two call/return pairs on top of the loads it
     * cannot avoid. This attribute is what removes them; `inline` alone is not
     * enough at `-Os`.
     */
#if defined(__GNUC__) || defined(__clang__)
#define PR32_APU_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define PR32_APU_ALWAYS_INLINE inline
#endif

    /**
     * @brief One Q15 point of a mipmap level, linearly interpolated.
     *
     * The bare `phase >> 24` read puts a ~-43 dBFS truncation floor under a
     * table this steep; the linear blend of the two adjacent points is the
     * read T2 measured to clear S5 (see tools/mipmap_gen/README.md). The
     * fraction is halved before the multiply so the product of two full-range
     * int16 neighbours cannot overflow `int32_t` on a 32-bit target; the cost
     * is at most 1 LSB of rounding, inaudible against a Q15 sample.
     *
     * @param level Mipmap level to read.
     * @param phaseQ32 Oscillator phase in Q32.
     * @return Interpolated Q15 sample in [-32767, 32767].
     */
    static PR32_APU_ALWAYS_INLINE int32_t readSawMipSampleQ15(int level, uint32_t phaseQ32) {
        const int index = (int)((phaseQ32 >> 24) & 255u);
        const int next = (index + 1) & 255;
        const int32_t a = SAW_MIP_Q15[level][index];
        const int32_t b = SAW_MIP_Q15[level][next];
        const int32_t fraction = (int32_t)((phaseQ32 >> 8) & 0xFFFFu);
        return a + (int32_t)(((b - a) * (fraction >> 1) + 16384) >> 15);
    }

    /**
     * @brief First Q8 position inside a band that blends toward the neighbour.
     *
     * Table `L` is built for the TOP of its band, so it is alias-free
     * everywhere below it: the neighbour table exists only to remove the step
     * where the level increments. Blending across the whole octave -- what
     * E2/T3 did -- therefore made every sample pay for two table reads to fix a
     * seam that only exists in the last fraction of the band. `224` puts the
     * window on the top 1/8, i.e. `[2^(24+L) - 2^(20+L), 2^(24+L))` for level
     * `L`, which is the shift in `resolveSawMipBandState` below. S5 is
     * unaffected by the narrowing: a table that is alias-free over its whole
     * band stays alias-free when the neighbour read is dropped.
     */
    static constexpr int kSawMipCrossfadeStartQ8 = 224;
    /** @brief Width of the crossfade window, in Q8 position units. */
    static constexpr int kSawMipCrossfadeWidthQ8 = 256 - kSawMipCrossfadeStartQ8;

    /**
     * @brief Everything the `Hybrid` SAW read needs that is constant in a band.
     *
     * The level, the window edges and the shift all come from `phaseIncQ32`,
     * which only moves while pitch automation is active. Resolving them once
     * per voice per block -- and again when the increment leaves the band --
     * takes a `clz`, the mantissa shifts and three clamps out of the per-sample
     * path. What stays there is one table read, one compare, and near a
     * boundary the neighbour read and the blend.
     *
     * Deliberately NOT a member of `AudioChannel`: the pool is size-pinned
     * (`sizeof(AudioChannel)` is 336 B with no spare padding), so this lives on
     * the render call's stack, indexed by voice, for one block at a time.
     */
    struct SawMipBandState {
        uint32_t bandLo;     ///< First increment this state describes.
        uint32_t bandSpan;   ///< Width of that range; one compare guards it.
        uint32_t windowLo;   ///< First increment of the crossfade window.
        uint32_t windowSpan; ///< Width of the window; 0 disables the blend entirely.
        uint8_t level;       ///< Table row the band selects.
        uint8_t windowShift; ///< `(inc - windowLo) >> windowShift` is the Q8 weight.
    };

    /**
     * @brief Fills @p state for one increment; the only place a band is derived.
     *
     * The level comes from `sawMipLevelForPhaseIncQ32` unchanged -- the same
     * selector the S4 case asserts against the band layout -- so the cache
     * cannot drift from the documented rule. The rest is algebra on the bands:
     * level `L` covers `[2^(23+L), 2^(24+L))`, level 0 also owns the low clamp
     * below 2^23 and level 7 the high clamp above 2^31, the window is the top
     * 1/8 of the unclamped band, and scaling that span by 256 is a shift.
     *
     * @param phaseIncQ32 Increment to describe.
     * @param state Destination.
     */
    static inline void resolveSawMipBandState(uint32_t phaseIncQ32, SawMipBandState& state) {
        const int level = sawMipLevelForPhaseIncQ32(phaseIncQ32);
        state.level = (uint8_t)level;

        if (level <= 0) {
            state.bandLo = 0u;
            state.bandSpan = 1u << 24;
        } else if (level >= (int)SAW_MIP_LEVEL_COUNT - 1) {
            state.bandLo = 0x40000000u;
            state.bandSpan = 0xC0000000u;
        } else {
            state.bandLo = 1u << (23 + level);
            state.bandSpan = state.bandLo;
        }

        if (level < (int)SAW_MIP_LEVEL_COUNT - 1) {
            // The window is the top `kSawMipCrossfadeWidthQ8 / 256` = 1/8 of
            // the band, and the band is a power of two, so both its start and
            // its width are shifts: level `L` covers `[2^(23+L), 2^(24+L))`,
            // the window is its last `2^(20+L)` increments, and scaling that
            // span to 256 weight units is the remaining 8 bits (12 + level).
            static_assert(kSawMipCrossfadeWidthQ8 == 32,
                          "the crossfade window must be a power-of-two fraction of a band");
            state.windowLo = (1u << (24 + level)) - (1u << (20 + level));
            state.windowSpan = 1u << (20 + level);
            state.windowShift = (uint8_t)(12 + level);
        } else {
            // Level 7 has no upper neighbour: the span of zero makes the
            // per-sample window test always fail, so nothing ever blends.
            state.windowLo = 0xFFFFFFFFu;
            state.windowSpan = 0u;
            state.windowShift = 0u;
        }
    }

    /**
     * @brief One `Hybrid` SAW sample: the band's table, blended only near the edge.
     *
     * Continuity at a boundary is exact in both directions, which is why this
     * is a crossfade and not a switch: the last increment inside band `L`
     * blends almost entirely to table `L + 1`, and the first increment of band
     * `L + 1` reads table `L + 1` outright, so the same table is on both sides
     * of the seam. The bucket the weight does not spend is the 8/256 the shift
     * mapping leaves below `bandTop`; that residue is the seam step the tests
     * budget for, and it is 1/32 of what a hard switch would jump by.
     *
     * @param ch Voice whose phase and increment are read.
     * @param state Band state for this voice, updated when the increment leaves
     *              the band it describes (sweep, vibrato, pitch envelope).
     * @return Q15 sample in [-32767, 32767].
     */
    static PR32_APU_ALWAYS_INLINE int32_t generateBandLimitedSawSampleQ15(
        const AudioChannel& ch, SawMipBandState& state) {
        const uint32_t increment = ch.phaseIncQ32;
        if (increment - state.bandLo >= state.bandSpan) {
            // One unsigned compare covers both edges and the wrap. A steady
            // note never takes it; pitch automation takes it once per band
            // crossing instead of once per sample, and that is what keeps the
            // level exact for any block size.
            resolveSawMipBandState(increment, state);
        }

        const int32_t low = readSawMipSampleQ15(state.level, ch.phaseQ32);
        if (increment - state.windowLo >= state.windowSpan) {
            // Outside the window: one table, which is the common case and the
            // branch that is predicted that way.
            return low;
        }
        const int32_t weightQ8 =
            (int32_t)((increment - state.windowLo) >> state.windowShift);
        const int32_t high = readSawMipSampleQ15(state.level + 1, ch.phaseQ32);
        return low + (int32_t)(((high - low) * weightQ8 + 128) >> 8);
    }

    static auto generateNoiseSampleQ15 = [](AudioChannel& ch) -> int32_t {
        return (ch.lfsrState & 1u) ? 32767 : -32767;
    };

    // Function pointer array for branch-free dispatch
    typedef int32_t (*WaveGeneratorQ15)(AudioChannel&);
    static constexpr WaveGeneratorQ15 WAVE_GENERATORS_Q15[] = {
        generatePulseSampleQ15,    // WaveType::PULSE = 0
        generateTriangleSampleQ15, // WaveType::TRIANGLE = 1
        nullptr,                    // WaveType::NOISE = 2 (special case - requires state update)
        nullptr,                    // WaveType::SINE = 3 (uses LUT)
        generateSawSampleQ15       // WaveType::SAW = 4
    };

    static_assert(
        sizeof(WAVE_GENERATORS_Q15) / sizeof(WAVE_GENERATORS_Q15[0]) == 5,
        "WAVE_GENERATORS_Q15 must have 5 entries for WaveType enum"
    );

    // ============================================================================
    // Internal helper functions
    // ============================================================================

    /**
     * @brief The envelope curve a sound profile supplies as its default (S2/D-2).
     *
     * The profile is the first tier of the D-3 precedence table: it owns the
     * default, and a note that names a curve of its own wins over it. This is
     * the single place that mapping lives, so `applyProfileCharacter` cannot
     * drift from what the profiles are documented to sound like.
     *
     * `Nes` deliberately answers `Linear`: its character is the 4-bit
     * `nesEnvelope` clocked at 240 Hz, which is a different mechanism and not
     * the exponential ramp. `Legacy` answers `Linear` because its curve is
     * frozen (S1), and any unknown value is treated as `Legacy` rather than
     * guessed.
     *
     * @param profile Profile in force.
     * @return Curve the profile supplies when the note asks for none.
     */
    static inline EnvelopeCurve profileDefaultEnvelopeCurve(SoundProfile profile) {
        return (profile == SoundProfile::Hybrid) ? EnvelopeCurve::Exponential
                                                 : EnvelopeCurve::Linear;
    }

    /**
     * @brief The unipolar NOISE gate a sound profile arms by default (S5/D-3).
     *
     * Tier 1 of the D-3 table owns the default and the voice may pin it. `Nes`
     * and `Hybrid` gate the noise between 0 and the envelope level, which is
     * what the hardware does and what makes the noise sit "on top of" the mix
     * instead of subtracting as much energy as it adds; `Legacy` keeps the
     * bipolar output frozen (S1), and any unknown value answers `Legacy`
     * rather than being guessed.
     *
     * @param profile Profile in force.
     * @return true when a NOISE voice should render the unipolar gate.
     */
    static inline bool profileDefaultNoiseUnipolar(SoundProfile profile) {
        return profile != SoundProfile::Legacy;
    }

    /**
     * @brief NOISE period LUT index the `Nes`/`Hybrid` profiles arm (S5).
     *
     * `$400E = $00`: the write the canonical NES init performs (see
     * `initNesSafe`), i.e. the fastest of the sixteen published periods. The
     * `Legacy` profile answers the 255 sentinel instead — "derive the period
     * from the note" — so its noise keeps the frequency-derived clock.
     */
    static constexpr uint8_t kProfileNesNoiseLutIndex = 0;

    /**
     * @brief NOISE period LUT index a sound profile arms by default (S5).
     *
     * @param profile Profile in force.
     * @return 0..15 for a LUT entry, 255 to derive the period from the note.
     */
    static inline uint8_t profileDefaultNoiseLutIndex(SoundProfile profile) {
        return (profile == SoundProfile::Legacy) ? 255u : kProfileNesNoiseLutIndex;
    }

    /**
     * @brief PULSE duty step a NES-derived profile arms for a note (S6).
     *
     * `Nes` and `Hybrid` render the four discrete NES duty patterns instead of
     * the continuous `phaseQ32 < dutyCycleQ32` comparison, because that is what
     * the hardware does: the pulse channel has no duty register, only a 2-bit
     * selector. The selector is chosen by NEAREST STEP from the note's own
     * `duty`, so a composer who asks for 40 % gets the closest NES timbre (50 %)
     * rather than silence or a made-up fifth pattern.
     *
     * The rule is round-half-up over the three midpoints:
     *
     *   | `duty`                | index       |
     *   |-----------------------|-------------|
     *   | `[0, 0.1875)`         | 0 (12.5 %)  |
     *   | `[0.1875, 0.375)`     | 1 (25 %)    |
     *   | `[0.375, 0.625)`      | 2 (50 %)    |
     *   | `[0.625, 1]`          | 3 (75 %)    |
     *
     * An exact TIE (0.1875, 0.375, 0.625) rounds UP to the higher duty, because
     * the comparison is `<` against each midpoint. All three midpoints are
     * exactly representable in float, so the documented behaviour is
     * reproducible, not a rounding accident.
     *
     * The input is the note's clamped `dutyCycle` (float) and not
     * `dutyCycleQ32`. The float is the value the caller declared, and this
     * selector is about the declared duty, not about the integer generator's
     * mirror of it: the mirror saturates at `duty == 1.0` (it used to wrap to 0,
     * which the full-scale fix removed), so reading it here would fold "100 %"
     * into "full scale" and turn a declared 100 % note into the 75 % step by a
     * route nobody asked for. Snapping from the declared float keeps the two
     * from disagreeing, whatever the mirror does.
     *
     * `Legacy` answers the 255 sentinel — "keep the continuous generator" — for
     * every duty. Every other value answers the nearest step, i.e. the
     * NES-derived reading, which is the same treatment the profile defaults
     * above give an unknown profile; `setSoundProfile` rejects unknown values,
     * so this is a fallback and never a guess.
     *
     * Called once per note-on, never per sample: no float, division or
     * transcendental is on the render path.
     *
     * @param profile Profile in force.
     * @param duty The note's duty in [0, 1] (`AudioChannel::dutyCycle`).
     * @return 0..3 to render a NES step, 255 to keep the continuous comparison.
     */
    static inline uint8_t profileDefaultPulseDutyIndex(SoundProfile profile,
                                                       float duty) {
        if (profile == SoundProfile::Legacy) {
            return 255u;
        }
        // Midpoints of 12.5/25, 25/50 and 50/75 %, exactly representable.
        constexpr float kMidStep0Step1 = 0.1875f;
        constexpr float kMidStep1Step2 = 0.375f;
        constexpr float kMidStep2Step3 = 0.625f;
        if (duty < kMidStep0Step1) return 0u;
        if (duty < kMidStep1Step2) return 1u;
        if (duty < kMidStep2Step3) return 2u;
        return 3u;
    }

    /**
     * @brief The mixer curve a sound profile selects (S4/D-3).
     *
     * Tier 1 of the D-3 precedence table owns the mix shape, so the profile is
     * its only input and this is the single place that mapping lives: `Legacy`
     * keeps the historical equal-weight sum, and the two NES-derived profiles
     * share the published NES weighting. Any other value answers `Rational`,
     * i.e. the frozen behaviour, rather than a guess.
     *
     * @param profile Profile in force.
     * @return Curve the mix stage must apply.
     */
    static inline MixerCurve profileMixerCurve(SoundProfile profile) {
        return (profile == SoundProfile::Legacy) ? MixerCurve::Rational
                                                 : MixerCurve::NesWeighted;
    }

    // --- NES-weighted mixer (S4 / P2a) -------------------------------------
    //
    // Published linear approximation of the NES mixer (nesdev "APU Mixer"),
    // per group:
    //
    //   pulses   : 0.00752 * (p1 + p2)
    //   triangle : 0.00851 * tri
    //   noise    : 0.00494 * noise
    //   DMC      : 0.00335 * dmc          <- reserved, see kNesMixWeightDmc
    //
    // Those are ABSOLUTE DAC weights, and the renderer needs two things the
    // published numbers do not give directly: the RATIO between the groups and
    // one normalisation, with no division in the per-sample loop.
    //
    // Both are folded into the weight table. Every published weight is
    // multiplied by 1 / 0.00752 = 132.979... and rounded to the nearest Q12
    // integer, with the PULSE group pinned at Q12 unity:
    //
    //   pulse    : 1 / 0.00752       = 1.0000  -> 4096   (exact)
    //   triangle : 0.00851 / 0.00752 = 1.1316  -> 4635  (-0.005 %)
    //   noise    : 0.00494 / 0.00752 = 0.6569  -> 2691  (+0.010 %)
    //   DMC      : 0.00335 / 0.00752 = 0.4456  -> 1825  (+0.016 %)
    //
    // Pinning the pulse group is the whole normalisation story: a full-scale
    // pulse contributes exactly the amplitude `Legacy` gives it
    // (MIXER_SCALE * level), so two full-scale pulses keep the historical
    // 2 * 0.4 * 32767 = 26213.6 and switching profile rebalances the groups
    // instead of changing the overall level. The `>> 12` in the per-block fold
    // IS the 1/0.00752 factor applied to the pulse group, so the residual there
    // is zero and every other group carries only its own rounding.
    //
    // WHERE the weight is applied is a cost question, not a mix question. It is
    // a constant per voice for a whole block, so `renderIntegerCore` applies it
    // ONCE per voice per block — inside the same Q15 gain the sample loop
    // already multiplies by — instead of summing into per-group buckets and
    // collapsing them once per sample. Distributing one constant over a sum is
    // exact in the reals; only the truncation moves, and that is worth < 1 LSB
    // per voice (see the fold in `renderIntegerCore`).

    /** Q12 weight of one full-scale PULSE voice: the anchor, exactly 1.0. */
    static constexpr int32_t kNesMixWeightPulse = 4096;
    /** Q12 weight of one full-scale TRIANGLE voice. */
    static constexpr int32_t kNesMixWeightTriangle = 4635;
    /** Q12 weight of one full-scale NOISE voice. */
    static constexpr int32_t kNesMixWeightNoise = 2691;
    /**
     * Q12 weight of a full-scale DMC voice.
     *
     * RESERVED: E1 has no DMC voice model — `WaveType` has no DMC member and
     * nothing in this library synthesises one — so no group feeds this term
     * yet. It stays next to the table so the published approximation is
     * complete in one place; a future DMC voice only has to add its
     * `NesMixGroup` and its weight here, and the fold already applies whatever
     * this table answers.
     */
    [[maybe_unused]] static constexpr int32_t kNesMixWeightDmc = 1825;
    /** Fixed-point divisor of the weight table: 2^12 = one full-scale pulse. */
    static constexpr int kNesMixWeightShift = 12;

    /** Weight groups of the NES mixer (S4). */
    enum class NesMixGroup : uint8_t {
        Pulse = 0,     ///< PULSE, plus SINE and SAW (see nesMixGroupOf).
        Triangle = 1,
        Noise = 2
    };

    /**
     * @brief NES mixer group a voice feeds (S4).
     *
     * `SINE` and `SAW` have no NES counterpart, so which group they join is a
     * modelling choice, and this is it: they join PULSE. All three are band-
     * limited melodic voices, and PULSE is the only melodic group the published
     * approximation has. Weighing them as triangle would boost every sine SFX
     * by 13 %, and as noise would cut it by 34 %; neither is a property of the
     * waveform, both would be accidents of the grouping. Any unknown type falls
     * back to the same group, so a future waveform inherits the safest weight
     * instead of a silent zero.
     *
     * @param type Voice waveform.
     * @return Group the voice accumulates into.
     */
    static inline NesMixGroup nesMixGroupOf(WaveType type) {
        switch (type) {
            case WaveType::TRIANGLE: return NesMixGroup::Triangle;
            case WaveType::NOISE:    return NesMixGroup::Noise;
            default:                 return NesMixGroup::Pulse;
        }
    }

    /**
     * @brief Q12 weight the NES mixer applies to one accumulation group (S4).
     *
     * The table is consulted ONCE per voice per block, from the fold in
     * `renderIntegerCore`; the per-sample loop only sees the resulting Q15 gain
     * and never a weight. Any unknown group answers the PULSE weight, the same
     * fallback `nesMixGroupOf` gives an unknown waveform, so a future group
     * cannot silently render as a zero.
     *
     * @param group Group the voice belongs to.
     * @return Q12 weight, where one full-scale PULSE voice is 2^12.
     */
    static inline int32_t nesMixWeightQ12(NesMixGroup group) {
        switch (group) {
            case NesMixGroup::Triangle: return kNesMixWeightTriangle;
            case NesMixGroup::Noise:    return kNesMixWeightNoise;
            default:                    return kNesMixWeightPulse;
        }
    }

    /** Q15 envelope scale: 1.0. */
    static constexpr int32_t kEnvelopeFullScaleQ15 = 32768;
    /** Q15 clock phase that means "one shape step per sample". */
    static constexpr uint32_t kCurveFullRateQ15 = 32768u;
    /**
     * Upper bound for one stage's shape-step count.
     *
     * The count is computed from a level in Q15, so the tight bound is ~1 500
     * steps (a full-scale span to −60 dB takes 1 356); the constant only exists
     * so a corrupt preset can never turn the note-on helper into an unbounded
     * loop.
     */
    static constexpr uint16_t kMaxCurveSteps = 2048;

    /**
     * @brief Shape iterations the exponential curve needs to fall 60 dB.
     *
     * Runs the very recurrence the renderer runs
     * (`level -= ((level - 1) >> 8) + 1`) until the level is 60 dB below
     * `spanQ15`. A closed form is not available: the recurrence's `+1` term is
     * a fixed-size step that dominates once the level is small, so the exact
     * count (1 356 for a full-scale span) is far below the continuous estimate
     * (1 766 for τ = 256 samples). Calibrating with the continuous estimate
     * would make every −60 dB time 30 % short.
     *
     * Called once per note-on per stage, never per sample.
     *
     * @param spanQ15 Distance the stage has to travel, in Q15.
     * @return Shape iterations to −60 dB of that span (at least 1).
     */
    static uint16_t curveStepsForSpanQ15(int32_t spanQ15) {
        if (spanQ15 <= 1) {
            return 1;
        }
        const int32_t target = spanQ15 / 1000;  // −60 dB
        int32_t level = spanQ15;
        uint16_t steps = 0;
        while (level > target && steps < kMaxCurveSteps) {
            level -= ((level - 1) >> 8) + 1;
            ++steps;
        }
        return (steps == 0) ? 1 : steps;
    }

    /**
     * @brief Clock rate that spreads the exponential's −60 dB span over a stage.
     *
     * The S-DSP recurrence is the SHAPE; this is its RATE. The shape's natural
     * rate is one step per sample (τ = 256 samples = 5.8 ms at 44.1 kHz), which
     * ignores `decayTime`/`releaseTime` and therefore cannot satisfy S2's
     * "−60 dB in the theoretical time": a note would fall silent in ~30 ms and
     * then jump back to its sustain level. Deriving the rate from the stage
     * duration makes the configured time mean what it says — the time to fall
     * 60 dB — and the stage ends ON its target instead of sailing past it.
     *
     * The clock never steps more than once per sample (rate ≤ 32768). A stage
     * shorter than the shape's own span cannot hold the shape at that limit, so
     * it returns 0 and the caller keeps the frozen linear ramp: stepping once
     * per sample instead would leave the level far above its target and snap
     * there at the stage boundary, which is exactly the audible discontinuity
     * this calibration exists to remove (a 5 ms release would snap ~7 dB).
     *
     * @param spanQ15 Distance the stage has to travel, in Q15.
     * @param stageSamples Duration of the stage in samples (0 = no stage).
     * @return Shape steps per sample in Q15, or 0 to keep the linear ramp.
     */
    static uint16_t exponentialCurveRateQ15(int32_t spanQ15, uint32_t stageSamples) {
        if (stageSamples == 0 || spanQ15 <= 0) {
            return 0;  // no distance to travel, or no stage: nothing to clock
        }
        const uint16_t steps = curveStepsForSpanQ15(spanQ15);
        if (stageSamples < (uint32_t)steps) {
            return 0;  // shape does not fit: the linear ramp is the continuous one
        }
        // Rounded UP so that a stage of exactly `steps` samples performs the
        // whole span instead of stopping one step short of the target.
        const uint64_t rate =
            (((uint64_t)steps << 15) + (uint64_t)stageSamples - 1u) / (uint64_t)stageSamples;
        if (rate < 1u) {
            return 1;  // slowest representable clock; a stall is impossible
        }
        if (rate > kCurveFullRateQ15) {
            return (uint16_t)kCurveFullRateQ15;
        }
        return (uint16_t)rate;
    }

    static inline uint32_t frequency_hz_to_phase_inc_q32(float hz, int sr) {
        if (sr <= 0) return 0u;
        if (hz < 0.0f) hz = 0.0f;
        const double inc = (double)hz * 4294967296.0 / (double)sr;
        if (inc < 0.0) return 0u;
        if (inc >= 4294967295.0) return 0xFFFFFFFFu;
        return (uint32_t)inc;
    }

    /**
     * Hito 2 M6: re-sync a voice's float / Q32 phase increments to match
     * the current NES PULSE timer period. Used by `setVoiceNesTimer`
     * (immediate) and the half-clock dispatch (when the timer is updated).
     * No-op when the timer is 0 (no sweep active) or the sample rate is
     * invalid.
     */
    static inline void syncNesTimerToPhaseIncQ32(Voice& v, int sr) {
        if (sr <= 0) return;
        if (v.nesSweepUnit.timer == 0) return;
        const float hz = nes_apu::nesTimerToHz(v.nesSweepUnit.timer);
        v.frequency = hz;
        v.phaseIncrement = hz / static_cast<float>(sr);
        v.phaseIncQ32 = frequency_hz_to_phase_inc_q32(hz, sr);
        v.basePhaseIncQ32 = v.phaseIncQ32;
    }

    /**
     * Hito 2 M6: recompute the continuously-evaluated `muted` flag for a
     * voice's sweep unit. Called by both `setVoiceNesSweep` (after
     * updating E/P/N/SSS) and `setVoiceNesTimer` (after updating the
     * timer), so the per-sample output gate sees the correct value
     * before the next half-clock tick.
     *
     * The target is checked for > 0x7FF BEFORE clamping (matches the NES
     * algorithm — the overflow is the signal for muting).
     *
     * Voices that have not opted into the sweep unit are forced to
     * `muted = false`: the default `timer == 0` would otherwise satisfy
     * the "current period < 8" rule and silence a voice that never asked
     * for NES sweep behaviour.
     */
    static inline void recomputeSweepMuted(Voice& v) {
        const NesSweepUnit& sw = v.nesSweepUnit;
        if (!sw.unitEnabled) {
            v.nesSweepUnit.muted = false;
            return;
        }
        const uint16_t period = sw.timer;
        const int32_t change = (sw.shift > 0) ? (period >> sw.shift) : 0;
        int32_t target = sw.negate
            ? (sw.isPulse2
                ? (int32_t)period - change
                : (int32_t)period - change - 1)
            : (int32_t)period + change;
        if (target < 0) target = 0;
        v.nesSweepUnit.muted = (period < 8) || (target > 0x7FF);
    }

    /**
     * Hito 2 M17: reset a voice's NES sub-unit fields to the canonical
     * silent state, replicating the `@regs` array from
     * https://www.nesdev.org/wiki/APU_basics:
     *
     *   PULSE     $4000=$30  $4001=$08  $4002=$00  $4003=$00
     *   TRIANGLE  $4008=$80  $400A=$00  $400B=$00
     *   NOISE     $400C=$30  $400E=$00  $400F=$00
     *
     * Decoding the shared byte $30 (DDLC VVVV) gives L=1, C=1, VVVV=0 —
     * length counter halted, constant volume selected, volume zero. So the
     * voice is silent and nothing counts down on its own. $08 (EPPP NSSS)
     * gives E=0, P=0, N=1, SSS=0. $80 sets the TRIANGLE control bit, which
     * is the same shared halt flag this codebase stores once on the length
     * counter.
     *
     * On NES the $4003/$400B/$400F write also raises the envelope start
     * flag and the linear counter reload flag; `setVoiceNesLength` models
     * the same pair. Here the start flag is raised but the linear reload
     * flag is explicitly cleared, because $400B is written with a zero
     * reload value: reloading from 0 is what the safe state wants, and
     * leaving the flag set would make the counter reload every quarter
     * clock instead of once.
     *
     * This is a clean slate, NOT an enable. The per-voice opt-in flags
     * (`nesLengthCounter.enabled`, `nesEnvelope.envelopeEnabled`,
     * `nesSweepUnit.unitEnabled`, `nesLinearCounter.linearEnabled`) are all
     * cleared; callers arm what they need afterwards.
     *
     * SINE and SAW have no NES counterpart and are left untouched.
     */
    static void initNesSafe(Voice& ch, WaveType type) {
        const bool isPulse = (type == WaveType::PULSE);
        const bool isTriangle = (type == WaveType::TRIANGLE);
        const bool isNoise = (type == WaveType::NOISE);
        if (!isPulse && !isTriangle && !isNoise) {
            return;
        }

        // $4015 = $00: the channel starts silenced/unarmed.
        NesLengthCounter& lc = ch.nesLengthCounter;
        lc.enabled = false;
        lc.counter = 0;
        lc.index = 0;
        lc.halt = true;  // L bit of $30 / control bit of $80

        // $4000 / $400C = $30, and $4008 = $80 for the triangle: constant
        // volume selected at level 0. The triangle has no envelope, but the
        // fields are zeroed anyway so introspection is deterministic.
        NesEnvelope& env = ch.nesEnvelope;
        env.envelopeEnabled = false;
        env.constVolume = true;
        env.volume = 0;
        env.decayLevel = 0;
        env.dividerCounter = 0;
        env.output = 0;
        env.startFlag = true;  // side effect of the $4003 / $400B / $400F write

        NesSweepUnit& sw = ch.nesSweepUnit;
        sw.unitEnabled = false;
        sw.sweepEnabled = false;
        sw.period = 0;
        sw.shift = 0;
        sw.dividerCounter = 0;
        sw.reloadFlag = false;
        sw.timer = 0;
        sw.muted = false;
        // $4001 = $08 sets N. Only the pulse channels have a sweep unit, so
        // the bit is meaningless (and misleading) on the others.
        sw.negate = isPulse;

        NesLinearCounter& linc = ch.nesLinearCounter;
        linc.linearEnabled = false;
        linc.counter = 0;
        linc.reloadValue = 0;
        linc.reloadFlag = false;
    }

    /** NOISE LFSR period from clock Hz (minimum 1 sample). */
    static inline uint32_t noise_clock_hz_to_period(float hz, int sr) {
        if (sr <= 0) return 1u;
        if (hz < 1.0f) hz = 1.0f;
        uint32_t period = (uint32_t)((float)sr / hz);
        if (period < 1u) period = 1u;
        return period;
    }

    /**
     * @brief Retune a NOISE voice from its period selector (S5 / Hito 3 M8).
     *
     * The single place that turns `nesNoiseLutIndex` into
     * `noisePeriodSamples`, shared by the selector setter and by
     * `applyProfileCharacter` so a profile default and an explicit write can
     * never disagree about what an index means. 0..15 read the published NES
     * table at the current rate; 255 (and any other sentinel) derives the
     * period from the voice's own clock in Hz.
     *
     * No-op for every waveform other than NOISE, whose period the LFSR does
     * not use. The countdown is clamped so a shorter period takes effect
     * immediately instead of after the old, longer one elapses.
     *
     * @param v Voice to retune.
     * @param sampleRate Output rate in Hz.
     */
    static inline void retuneNoisePeriodFromSelector(Voice& v, int sampleRate) {
        if (v.type != WaveType::NOISE) return;
        if (v.nesNoiseLutIndex < 16u) {
            v.noisePeriodSamples =
                nes_apu::nesNoisePeriodToSamples(v.nesNoiseLutIndex, sampleRate);
        } else {
            v.noisePeriodSamples = noise_clock_hz_to_period(v.frequency, sampleRate);
        }
        if (v.noiseCountdown > v.noisePeriodSamples) {
            v.noiseCountdown = v.noisePeriodSamples;
        }
    }

    /**
     * @brief Level compensation of the unipolar NOISE gate, as a shift (S5/P9).
     *
     * A bipolar noise voice swings between -A and +A and carries AC RMS `A`. A
     * unipolar one swings between 0 and A, so its mean is `A/2` — the DC the
     * output blocker removes — and what is left is AC RMS `A/2`: the gate half
     * the amplitude, i.e. -6.02 dB of energy.
     *
     * The compensation is therefore exactly x2 (`1 << 1`), applied once per
     * voice per BLOCK: it is a constant of the whole block, so the renderer
     * folds it into the same Q15 gain that already carries the voice's volume
     * and its mix weight (see the fold in `renderIntegerCore`) instead of
     * shifting each sample. Multiplying by 2 and shifting by 1 are the same
     * operation here — `sv << 1` IS `sv * 2` — so the factor is unchanged; only
     * its position in the multiply chain moved, and it is still never a repeat
     * of the volume or envelope multiplies. It is not a taste knob: after it,
     * the unipolar voice's AC RMS is `A` again, so the profile's MIXER weight
     * (`kNesMixWeightNoise`, 0.00494/0.00752 of the pulse group) is the only
     * thing that decides the noise group's level — which is what keeps S4's
     * published ratio intact. Measured: 0.00 dB between the unipolar and the
     * bipolar render of the same voice under the same weighting, and -3.65 dB
     * from `Legacy`'s bipolar noise, that residual being exactly that weight.
     */
    static constexpr int kNoiseUnipolarCompensationShift = 1;

    static inline uint32_t time_sec_to_samples(float timeSec, int sr) {
        if (sr <= 0 || timeSec <= 0.0f) {
            return 0u;
        }
        const double samples = (double)timeSec * (double)sr;
        if (samples >= 4294967295.0) {
            return 0xFFFFFFFFu;
        }
        return (uint32_t)samples;
    }

    static inline void set_pulse_duty(AudioChannel& ch, float duty) {
        if (duty < 0.0f) {
            duty = 0.0f;
        }
        if (duty > 1.0f) {
            duty = 1.0f;
        }
        ch.dutyCycle = duty;
        // `duty * 2^32` is exactly 2^32 at full scale, which does not fit in
        // `uint32_t`: the truncating conversion wrapped it to 0, so the integer
        // generator's `phaseQ32 < dutyCycleQ32` was never true and a 100 %
        // pulse rendered flat instead of a constant high level. Saturate at the
        // boundary and keep the original product for every other value, so the
        // rest of the integer PCM cannot move. Rescaling the product to
        // 4294967295.0 would fix this one input and shift every voice by 1 LSB.
        ch.dutyCycleQ32 = (duty >= 1.0f)
            ? 0xFFFFFFFFu
            : (uint32_t)((double)duty * 4294967296.0);
    }

    /** Apply t<=0 duty steps at init; schedule next boundary in samples (no float/sample). */
    static void init_duty_stepped(AudioChannel& ch, const AudioEvent& event, int sr) {
        ch.dutySteps = nullptr;
        ch.dutyStepCount = 0;
        ch.dutyStepIndex = 0;
        ch.dutyNextBoundarySamples = 0xFFFFFFFFu;
        ch.automationAgeSamples = 0;

        if (event.type != WaveType::PULSE || event.dutySteps == nullptr || event.dutyStepCount == 0) {
            return;
        }

        uint8_t count = event.dutyStepCount;
        if (count > kMaxSfxDutySteps) {
            count = kMaxSfxDutySteps;
        }
        ch.dutySteps = event.dutySteps;
        ch.dutyStepCount = count;

        uint8_t applied = 0;
        while (applied < count && event.dutySteps[applied].timeSec <= 0.0f) {
            set_pulse_duty(ch, event.dutySteps[applied].value);
            ++applied;
        }
        ch.dutyStepIndex = applied;
        if (applied < count) {
            ch.dutyNextBoundarySamples =
                time_sec_to_samples(event.dutySteps[applied].timeSec, sr);
        }
    }

    static inline bool pitch_envelope_active(const AudioChannel& ch) {
        return ch.pitchEnvelope != nullptr && ch.pitchEnvelopeCount >= 2;
    }

    static inline void set_voice_hz(AudioChannel& ch, float hz, int sr) {
        if (sr <= 0) {
            return;
        }
        if (ch.type == WaveType::NOISE) {
            if (hz < 1.0f) {
                hz = 1.0f;
            }
            ch.frequency = hz;
            ch.noisePeriodSamples = noise_clock_hz_to_period(hz, sr);
        } else {
            if (hz < 0.0f) {
                hz = 0.0f;
            }
            ch.frequency = hz;
            ch.phaseIncrement = hz / (float)sr;
            ch.phaseIncQ32 = frequency_hz_to_phase_inc_q32(hz, sr);
            ch.basePhaseIncQ32 = ch.phaseIncQ32;
        }
    }

    /**
     * Advance duty stepped hold table using sample-age boundaries.
     * Does not increment automationAgeSamples (shared clock owned by tick_voice_automation).
     */
    static void tick_duty_stepped(AudioChannel& ch, int sr) {
        if (ch.dutyStepCount == 0 || ch.dutySteps == nullptr) {
            return;
        }
        while (ch.dutyStepIndex < ch.dutyStepCount
               && ch.automationAgeSamples >= ch.dutyNextBoundarySamples) {
            set_pulse_duty(ch, ch.dutySteps[ch.dutyStepIndex].value);
            ++ch.dutyStepIndex;
            if (ch.dutyStepIndex < ch.dutyStepCount) {
                ch.dutyNextBoundarySamples =
                    time_sec_to_samples(ch.dutySteps[ch.dutyStepIndex].timeSec, sr);
            } else {
                ch.dutyNextBoundarySamples = 0xFFFFFFFFu;
                break;
            }
        }
    }

    /** Steal ranking: looping voices score 0 so they can be replaced (anti-starvation). */
    static inline uint64_t voice_steal_score(const AudioChannel& voice) {
        if (voice.loop) return 0;
        return voice.remainingSamples;
    }

    /** Fractional part of 2^x: LUT[i] ≈ 2^(i/256) in Q15 (range [1, 2)). */
    static constexpr uint16_t kExp2FracQ15[256] = {
        32768,32857,32946,33035,33125,33215,33305,33395,33486,33576,33667,33759,33850,33942,34034,34126,
        34219,34312,34405,34498,34591,34685,34779,34874,34968,35063,35158,35253,35349,35445,35541,35637,
        35734,35831,35928,36025,36123,36221,36319,36417,36516,36615,36715,36814,36914,37014,37114,37215,
        37316,37417,37518,37620,37722,37824,37927,38030,38133,38236,38340,38444,38548,38653,38757,38863,
        38968,39074,39180,39286,39392,39499,39606,39714,39821,39929,40037,40146,40255,40364,40473,40583,
        40693,40804,40914,41025,41136,41248,41360,41472,41584,41697,41810,41923,42037,42151,42265,42380,
        42495,42610,42726,42841,42958,43074,43191,43308,43425,43543,43661,43780,43898,44017,44137,44256,
        44376,44497,44617,44738,44859,44981,45103,45225,45348,45471,45594,45718,45842,45966,46091,46216,
        46341,46467,46593,46719,46846,46973,47100,47228,47356,47484,47613,47742,47871,48001,48131,48262,
        48393,48524,48655,48787,48920,49052,49185,49319,49452,49586,49721,49856,49991,50126,50262,50399,
        50535,50672,50810,50947,51085,51224,51363,51502,51642,51782,51922,52063,52204,52346,52488,52630,
        52773,52916,53059,53203,53347,53492,53637,53782,53928,54074,54221,54368,54515,54663,54811,54960,
        55109,55258,55408,55558,55709,55860,56012,56163,56316,56468,56622,56775,56929,57083,57238,57393,
        57549,57705,57861,58018,58176,58333,58491,58650,58809,58968,59128,59289,59449,59611,59772,59934,
        60097,60260,60423,60587,60751,60916,61081,61247,61413,61579,61746,61914,62081,62250,62419,62588,
        62757,62928,63098,63269,63441,63613,63785,63958,64132,64306,64480,64655,64830,65006,65182,65359
    };

    static inline int32_t log2_q16_from_float(float x) {
        if (!(x > 0.0f)) return -32 * 65536;
        return (int32_t)(std::log2(x) * 65536.0f);
    }

    static inline int32_t log2_q16_from_u32(uint32_t x) {
        if (x == 0u) return -32 * 65536;
        return log2_q16_from_float((float)x);
    }

    /** Integer 2^(logQ16/65536) for Q15 sweep path (no soft-float per sample). */
    static inline uint32_t exp2_q16_to_u32(int32_t logQ16) {
        if (logQ16 <= -32 * 65536) return 0u;
        if (logQ16 >= 32 * 65536) return 0xFFFFFFFFu;
        const int32_t ip = logQ16 >> 16;
        const uint32_t frac = (uint32_t)(logQ16 & 0xFFFF);
        const uint32_t mantQ15 = kExp2FracQ15[frac >> 8];
        if (ip >= 0) {
            if (ip > 31) return 0xFFFFFFFFu;
            const uint64_t v = ((uint64_t)mantQ15 << (uint32_t)ip) >> 15;
            if (v > 0xFFFFFFFFu) return 0xFFFFFFFFu;
            return (uint32_t)v;
        }
        const int sh = 15 - ip;
        if (sh >= 32) return 0u;
        return mantQ15 >> sh;
    }

    /** Frequency/period sweep (Linear or Exponential); decrements sweep counter. */
    static inline void apply_frequency_sweep_float(AudioChannel& ch, int sr) {
        if (sr <= 0) return;
        if (ch.sweepSamplesTotal == 0) return;
        if (ch.sweepSamplesRemaining == 0) return;
        const float denom = (float)ch.sweepSamplesTotal;
        const float alpha = (float)(ch.sweepSamplesTotal - ch.sweepSamplesRemaining) / denom;
        float hz;
        if (ch.sweepCurve == SweepCurve::Exponential
            && ch.sweepStartHz > 0.0f && ch.sweepEndHz > 0.0f) {
            hz = ch.sweepStartHz * std::exp(alpha * ch.sweepLogRatio);
        } else {
            hz = ch.sweepStartHz + (ch.sweepEndHz - ch.sweepStartHz) * alpha;
        }
        if (ch.type == WaveType::NOISE) {
            ch.frequency = hz;
            ch.noisePeriodSamples = noise_clock_hz_to_period(hz, sr);
        } else {
            ch.frequency = hz;
            ch.phaseIncrement = hz / (float)sr;
        }
        ch.sweepSamplesRemaining--;
        if (ch.sweepSamplesRemaining == 0) {
            ch.frequency = ch.sweepEndHz;
            if (ch.type == WaveType::NOISE) {
                ch.noisePeriodSamples = noise_clock_hz_to_period(ch.sweepEndHz, sr);
            } else {
                ch.phaseIncrement = ch.sweepEndHz / (float)sr;
            }
            ch.sweepSamplesTotal = 0;
        }
    }

    /** Configure current pitch segment i → i+1; reuses sweep* fields as segment state. */
    static void begin_pitch_segment(AudioChannel& ch, uint8_t segIndex, int sr) {
        if (!pitch_envelope_active(ch) || segIndex + 1u >= ch.pitchEnvelopeCount || sr <= 0) {
            ch.pitchSegLenSamples = 0;
            return;
        }
        const SfxBreakpoint& a = ch.pitchEnvelope[segIndex];
        const SfxBreakpoint& b = ch.pitchEnvelope[segIndex + 1u];
        ch.pitchSegIndex = segIndex;
        ch.pitchSegStartAge = time_sec_to_samples(a.timeSec, sr);
        const uint32_t endAge = time_sec_to_samples(b.timeSec, sr);
        ch.pitchSegLenSamples = (endAge > ch.pitchSegStartAge)
            ? (endAge - ch.pitchSegStartAge)
            : 1u;
        ch.sweepStartHz = a.value;
        ch.sweepEndHz = b.value;
        if (ch.type == WaveType::NOISE) {
            ch.sweepStartIncQ32 = noise_clock_hz_to_period(a.value, sr);
            ch.sweepEndIncQ32 = noise_clock_hz_to_period(b.value, sr);
        } else {
            ch.sweepStartIncQ32 = frequency_hz_to_phase_inc_q32(a.value, sr);
            ch.sweepEndIncQ32 = frequency_hz_to_phase_inc_q32(b.value, sr);
        }
        if (ch.sweepCurve == SweepCurve::Exponential
            && ch.sweepStartHz > 0.0f && ch.sweepEndHz > 0.0f) {
            ch.sweepLogRatio = std::log(ch.sweepEndHz / ch.sweepStartHz);
            if (ch.type == WaveType::NOISE) {
                ch.sweepLogStartQ16 = log2_q16_from_float(ch.sweepStartHz);
                ch.sweepLogDeltaQ16 =
                    log2_q16_from_float(ch.sweepEndHz) - ch.sweepLogStartQ16;
            } else {
                ch.sweepLogStartQ16 = log2_q16_from_u32(ch.sweepStartIncQ32);
                ch.sweepLogDeltaQ16 =
                    log2_q16_from_u32(ch.sweepEndIncQ32) - ch.sweepLogStartQ16;
            }
        } else {
            ch.sweepLogRatio = 0.0f;
            ch.sweepLogStartQ16 = 0;
            ch.sweepLogDeltaQ16 = 0;
        }
    }

    static void init_pitch_envelope(AudioChannel& ch, const AudioEvent& event, int sr) {
        ch.pitchEnvelope = nullptr;
        ch.pitchEnvelopeCount = 0;
        ch.pitchSegIndex = 0;
        ch.pitchSegStartAge = 0;
        ch.pitchSegLenSamples = 0;

        if (event.pitchEnvelope == nullptr || event.pitchEnvelopeCount == 0) {
            return;
        }
        uint8_t count = event.pitchEnvelopeCount;
        if (count > kMaxSfxPitchPoints) {
            count = kMaxSfxPitchPoints;
        }
        ch.pitchEnvelope = event.pitchEnvelope;
        ch.pitchEnvelopeCount = count;
        ch.sweepCurve = event.sweepCurve;

        if (count < 2) {
            return; // stored only; single-segment sweep remains authoritative
        }

        set_voice_hz(ch, event.pitchEnvelope[0].value, sr);
        begin_pitch_segment(ch, 0, sr);
    }

    /** Advance completed pitch segments; returns false if holding final value. */
    static bool advance_pitch_segments(AudioChannel& ch, int sr) {
        while (ch.pitchSegLenSamples > 0) {
            const uint32_t segEnd = ch.pitchSegStartAge + ch.pitchSegLenSamples;
            if (ch.automationAgeSamples < segEnd) {
                return true;
            }
            const uint8_t next = static_cast<uint8_t>(ch.pitchSegIndex + 1u);
            if (next + 1u >= ch.pitchEnvelopeCount) {
                ch.pitchSegLenSamples = 0;
                set_voice_hz(ch, ch.pitchEnvelope[ch.pitchEnvelopeCount - 1u].value, sr);
                return false;
            }
            begin_pitch_segment(ch, next, sr);
            set_voice_hz(ch, ch.sweepStartHz, sr);
        }
        return false;
    }

    /** Interpolate multi-breakpoint pitch envelope (FPU path). */
    static void tick_pitch_envelope_float(AudioChannel& ch, int sr) {
        if (!pitch_envelope_active(ch) || sr <= 0) {
            return;
        }
        if (!advance_pitch_segments(ch, sr)) {
            return;
        }
        if (ch.automationAgeSamples < ch.pitchSegStartAge) {
            set_voice_hz(ch, ch.sweepStartHz, sr);
            return;
        }

        uint32_t numer = ch.automationAgeSamples - ch.pitchSegStartAge;
        if (numer > ch.pitchSegLenSamples) {
            numer = ch.pitchSegLenSamples;
        }
        const float alpha = (float)numer / (float)ch.pitchSegLenSamples;
        float hz;
        if (ch.sweepCurve == SweepCurve::Exponential
            && ch.sweepStartHz > 0.0f && ch.sweepEndHz > 0.0f) {
            hz = ch.sweepStartHz * std::exp(alpha * ch.sweepLogRatio);
        } else {
            hz = ch.sweepStartHz + (ch.sweepEndHz - ch.sweepStartHz) * alpha;
        }
        set_voice_hz(ch, hz, sr);
    }

    /**
     * Pitch envelope for Q15/no-FPU path: integer lerp / log-domain on precomputed
     * segment endpoints (no soft-float per sample).
     */
    static void tick_pitch_envelope_q15(AudioChannel& ch, int sr) {
        if (!pitch_envelope_active(ch) || sr <= 0) {
            return;
        }
        if (!advance_pitch_segments(ch, sr)) {
            return;
        }
        if (ch.automationAgeSamples < ch.pitchSegStartAge) {
            if (ch.type == WaveType::NOISE) {
                ch.noisePeriodSamples = ch.sweepStartIncQ32 < 1u ? 1u : ch.sweepStartIncQ32;
            } else {
                ch.basePhaseIncQ32 = ch.sweepStartIncQ32;
                ch.phaseIncQ32 = ch.sweepStartIncQ32;
            }
            ch.frequency = ch.sweepStartHz;
            return;
        }

        uint32_t numer = ch.automationAgeSamples - ch.pitchSegStartAge;
        if (numer > ch.pitchSegLenSamples) {
            numer = ch.pitchSegLenSamples;
        }

        if (ch.sweepCurve == SweepCurve::Exponential
            && ch.sweepStartHz > 0.0f && ch.sweepEndHz > 0.0f) {
            const int32_t logV = ch.sweepLogStartQ16
                + (int32_t)(((int64_t)ch.sweepLogDeltaQ16 * (int64_t)numer)
                            / (int64_t)ch.pitchSegLenSamples);
            if (ch.type == WaveType::NOISE) {
                uint32_t hz = exp2_q16_to_u32(logV);
                if (hz < 1u) {
                    hz = 1u;
                }
                uint32_t period = (uint32_t)sr / hz;
                if (period < 1u) {
                    period = 1u;
                }
                ch.noisePeriodSamples = period;
                ch.frequency = (float)hz;
            } else {
                ch.basePhaseIncQ32 = exp2_q16_to_u32(logV);
                ch.phaseIncQ32 = ch.basePhaseIncQ32;
            }
        } else {
            const int64_t delta =
                (int64_t)ch.sweepEndIncQ32 - (int64_t)ch.sweepStartIncQ32;
            int64_t value = (int64_t)ch.sweepStartIncQ32
                + (delta * (int64_t)numer) / (int64_t)ch.pitchSegLenSamples;
            if (value < 0) {
                value = 0;
            }
            if (value > (int64_t)0xFFFFFFFFLL) {
                value = 0xFFFFFFFFLL;
            }
            if (ch.type == WaveType::NOISE) {
                uint32_t period = (uint32_t)value;
                if (period < 1u) {
                    period = 1u;
                }
                ch.noisePeriodSamples = period;
            } else {
                ch.basePhaseIncQ32 = (uint32_t)value;
                ch.phaseIncQ32 = ch.basePhaseIncQ32;
            }
            // Approximate Hz for diagnostics (not used by oscillator on Q15 path).
            ch.frequency = ch.sweepStartHz
                + (ch.sweepEndHz - ch.sweepStartHz)
                    * ((float)numer / (float)ch.pitchSegLenSamples);
        }
    }

    /**
     * Shared automation clock: one age++ per sample, then duty + pitch.
     * When pitch envelope is active, single-segment sweep must not run.
     */
    static void tick_voice_automation(AudioChannel& ch, int sr, bool q15Path) {
        const bool dutyActive = ch.dutyStepCount > 0 && ch.dutySteps != nullptr;
        const bool pitchActive = pitch_envelope_active(ch);
        if (!dutyActive && !pitchActive) {
            return;
        }
        if (ch.automationAgeSamples < 0xFFFFFFFFu) {
            ++ch.automationAgeSamples;
        }
        if (dutyActive) {
            tick_duty_stepped(ch, sr);
        }
        if (pitchActive) {
            if (q15Path) {
                tick_pitch_envelope_q15(ch, sr);
            } else {
                tick_pitch_envelope_float(ch, sr);
            }
        }
    }

    static inline int16_t apply_master_bitcrush(int16_t sample, uint8_t bits) {
        if (bits == 0 || bits >= 16) return sample;
        const int shift = 16 - bits;
        int32_t v = (int32_t)sample;
        v = (v >> shift) << shift;
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return (int16_t)v;
    }

    // ------------------------------------------------------------------
    // Log hook (see ApuConfig.h)
    // ------------------------------------------------------------------
    namespace {
        ApuLogHandler g_apuLogHandler = nullptr;
    }

    void apuSetLogHandler(ApuLogHandler handler) { g_apuLogHandler = handler; }

    ApuLogHandler apuGetLogHandler() { return g_apuLogHandler; }

    void apuLogf(ApuLogLevel level, const char* fmt, ...) {
        const ApuLogHandler handler = g_apuLogHandler;
        if (handler == nullptr || fmt == nullptr) {
            return;
        }
        char buffer[128];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        handler(level, buffer);
    }

    // ------------------------------------------------------------------
    // Construction / lifecycle
    // ------------------------------------------------------------------
    ApuCore::ApuCore() {
        for (int i = 0; i < MAX_VOICES; ++i) {
            voices[i].reset();
            voices[i].type = WaveType::PULSE;
        }
        clearMusicTrackVoiceState();
    }

    void ApuCore::init(int sr) {
        // Graceful degradation: use safe default if invalid sample rate
        sampleRate = (sr > 0) ? sr : 44100;
        tickDurationSamples =
            (uint64_t)((float)sampleRate * 60.0f / (tempoBPM * (float)TICKS_PER_BEAT));
        // Hito 4 M10: a configured corner is expressed in Hz, so it has to
        // follow the new rate. No-op while the legacy coefficient is in use.
        recomputeHpfCoefficients();
        // E2/S1: the profile chain's corners are in Hz too, so a rate change
        // re-derives them. The recursion state is cleared because a pole
        // computed for the old rate has no meaning for the new one.
        resetOutputFilterState();
        recomputeOutputFilterCoefficients();
        // Voices are reset by the constructor and by `reset()`; the sound
        // profile is configuration and deliberately survives a stream restart.
    }

    // Hito 4 M10: master HPF configuration.
    void ApuCore::recomputeHpfCoefficients() {
        if (hpfCutoffHz_ < 0.0f) {
            // Legacy: pin the historical coefficient exactly, so the
            // canonical golden PCM stays byte-for-byte identical.
            hpfR_ = kHpfLegacyR;
            hpfRQ15_ = kHpfLegacyRQ15;
            return;
        }
        if (hpfCutoffHz_ == 0.0f || sampleRate <= 0) {
            // Bypassed; the coefficient is unused but kept sane.
            hpfR_ = kHpfLegacyR;
            hpfRQ15_ = kHpfLegacyRQ15;
            return;
        }
        // R = exp(-2*pi*fc/fs). Computed here rather than per sample: this
        // runs on configuration changes only, so the transcendental is fine
        // even on soft-float targets. `float`, not `double`: the pole is
        // quantised to Q15 on the next line, so the extra precision of the
        // double libm only costs flash (the E2 regression).
        const float r = std::exp(-6.283185307179586f * hpfCutoffHz_ /
                                 (float)sampleRate);
        // Clamp strictly inside (0, 1): R >= 1 makes the recursion unstable,
        // and R <= 0 is not a meaningful single-pole coefficient. A corner
        // above Nyquist lands near 0 and is effectively "block everything".
        float clamped = r;
        if (!(clamped > 0.0f)) clamped = 0.0f;        // also catches NaN
        if (clamped > 0.99999f) clamped = 0.99999f;
        hpfR_ = clamped;
        hpfRQ15_ = (int32_t)(clamped * 32768.0f + 0.5f);
        if (hpfRQ15_ > 32767) hpfRQ15_ = 32767;
        if (hpfRQ15_ < 0) hpfRQ15_ = 0;
    }

    // E2/S1: the output stage owned by the `Nes` / `Hybrid` profiles.
    void ApuCore::recomputeOutputFilterCoefficients() {
        // The profile owns the stage (D-3 tier 1), but an explicit
        // `setHpfCutoffHz` is a tier-2 caller override and wins (S8). Resolve
        // that here, once per configuration change, so the sample loop only
        // tests a bool.
        const bool profileWantsChain = (profile_ == SoundProfile::Nes) ||
                                       (profile_ == SoundProfile::Hybrid);
        outputFilterActive_ = profileWantsChain && !hpfCutoffOverridden_ &&
                              sampleRate > 0;

        // Level trim of the active profile (see `kProfileTrimNesQ15`). Resolved
        // HERE, next to the activation it belongs to, so it is configuration
        // and never a per-sample or per-block computation. The amount follows
        // the PROFILE, but it is applied only while the chain is active: an
        // explicit `setHpfCutoffHz` asks for the historical one-pole and that
        // includes its level, so the trim goes quiet with the chain (S8).
        switch (profile_) {
            case SoundProfile::Nes:
                outTrimQ15_ = kProfileTrimNesQ15;
                break;
            case SoundProfile::Hybrid:
                outTrimQ15_ = kProfileTrimHybridQ15;
                break;
            case SoundProfile::Legacy:
            default:
                // `Legacy` is the anchor: exactly unity, and the render loop's
                // chain branch never runs for it, so no multiply is paid.
                outTrimQ15_ = kProfileTrimUnityQ15;
                break;
        }

        // LPF policy (audit P3 risk iii): at 8 000 Hz `0.45 * fs` is 3 600 Hz,
        // which would cut most of the passband and "switch off" the sound
        // instead of approximating the NES's 14 kHz roof. Below the 6 kHz
        // floor the LPF is skipped by definition; the chain is then the two
        // high-passes only.
        //
        // Everything below is `float`. The coefficients are quantised to Q15
        // immediately, so double precision changes no bit of them; it only
        // pulls the double libm transcendants (exp/cos/sin/round) into a
        // soft-float image, which is exactly the E2 flash regression.
        const float nyquist45 = 0.45f * (float)sampleRate;
        outputLpfActive_ = outputFilterActive_ && (sampleRate > 0) &&
                           (nyquist45 >= kOutputLpfMin45FsHz);

        outHpf1CutoffHz_ = kOutputHpf1Hz;
        outHpf2CutoffHz_ = kOutputHpf2Hz;
        outLpfCutoffHz_ = outputLpfActive_
            ? std::min(kOutputLpfMaxHz, nyquist45)
            : 0.0f;

        // First-order high-pass pole, the same definition as the historical
        // one-pole: R = exp(-2*pi*fc/fs), clamped strictly inside (0, 1) so a
        // bad corner can never make the recursion unstable. The transcendental
        // lives here, on configuration only, so it never reaches a sample loop
        // even on a soft-float target.
        auto highPassPoleQ15 = [this](float fcHz) -> int32_t {
            const float r =
                std::exp(-6.283185307179586f * fcHz / (float)sampleRate);
            float clamped = r;
            if (!(clamped > 0.0f)) clamped = 0.0f;  // also catches NaN
            if (clamped > 0.99999f) clamped = 0.99999f;
            int32_t q = (int32_t)(clamped * 32768.0f + 0.5f);
            if (q > kOutputCoeffQ15Max) q = kOutputCoeffQ15Max;
            if (q < 0) q = 0;
            return q;
        };
        outHpf1RQ15_ = highPassPoleQ15(kOutputHpf1Hz);
        outHpf2RQ15_ = highPassPoleQ15(kOutputHpf2Hz);

        outLpfB0Q15_ = 0;
        outLpfB1Q15_ = 0;
        outLpfB2Q15_ = 0;
        outLpfA1Q15_ = 0;
        outLpfA2Q15_ = 0;
        if (!outputLpfActive_ || sampleRate <= 0) {
            return;
        }

        // RBJ low-pass at Q = 1/sqrt(2) (Butterworth): monotonic, -3 dB at the
        // corner, no peaking. The corner is `min(14 kHz, 0.45 * fs)`, i.e. it
        // tracks the rate until it hits the NES roof.
        const float fc = std::min(kOutputLpfMaxHz, nyquist45);
        const float w0 = 6.283185307179586f * fc / (float)sampleRate;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) / 1.4142135623730951f;  // 2Q with Q=1/sqrt(2)
        const float a0 = 1.0f + alpha;
        const float b0 = (1.0f - cw) * 0.5f / a0;
        const float b1 = (1.0f - cw) / a0;
        const float b2 = b0;
        const float a1 = -2.0f * cw / a0;
        const float a2 = (1.0f - alpha) / a0;

        // Q15 in int32_t, not int16: `b1` can reach ~2 near 0.45 * fs (that is
        // ~52 000 in Q15) and must keep its resolution. The clamp to
        // `kOutputCoeffQ15Max` is what keeps every product inside int32_t.
        auto toQ15 = [](float v) -> int32_t {
            const float scaled = v * 32768.0f;
            if (!(scaled == scaled)) return 0;  // NaN guard
            const float rounded = scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f;
            if (rounded > (float)kOutputCoeffQ15Max) {
                return kOutputCoeffQ15Max;
            }
            if (rounded < -(float)kOutputCoeffQ15Max) {
                return -kOutputCoeffQ15Max;
            }
            return (int32_t)rounded;  // truncation == round after the +-0.5
        };
        outLpfB0Q15_ = toQ15(b0);
        outLpfB1Q15_ = toQ15(b1);
        outLpfB2Q15_ = toQ15(b2);
        outLpfA1Q15_ = toQ15(a1);
        outLpfA2Q15_ = toQ15(a2);

        // Stability guard. |a2| >= 1 places a pole on or outside the unit
        // circle; a saturated recurrence would then ring forever. Over the
        // supported 8 000-48 000 Hz range |a2| stays <= 0.65, so this never
        // fires in practice; if a future rate ever needed it, dropping the LPF
        // is the documented degradation (a missing slope, never a blow-up).
        if (outLpfA2Q15_ >= 32768 || outLpfA2Q15_ <= -32768) {
            outLpfB0Q15_ = 0;
            outLpfB1Q15_ = 0;
            outLpfB2Q15_ = 0;
            outLpfA1Q15_ = 0;
            outLpfA2Q15_ = 0;
            outputLpfActive_ = false;
            outLpfCutoffHz_ = 0.0f;
        }
    }

    /**
     * Q15 rescale of an int32 feedback product, truncated toward zero.
     *
     * C++ integer division truncates toward zero; an arithmetic `>> 15` floors
     * instead, which has a fixed point for negative values and freezes the DC
     * decay on a permanent offset (the defect documented on the historical
     * one-pole). Negating, shifting and negating back is exact because
     * |v| < 2^31 here, and it costs no division: on a soft-div target the
     * int64 divide this replaces is a library call of dozens of cycles.
     */
    static inline int32_t q15_truncate_from_int32(int32_t v) {
        return v >= 0 ? (v >> 15) : -((-v) >> 15);
    }

    /** @brief Q15 rescale of an int64 accumulator, truncated toward zero. */
    static inline int32_t q15_truncate_from_int64(int64_t v) {
        return (int32_t)(v >= 0 ? (v >> 15) : -((-v) >> 15));
    }

    ApuCore::OutputStageStateQ15 ApuCore::captureOutputStageState() const {
        // The zero-init is not load-bearing — every field is assigned below.
        // It is here so a field added to the struct but forgotten here degrades
        // to a zeroed stage instead of reading indeterminate memory.
        OutputStageStateQ15 state{};
        state.chainActive = outputFilterActive_;
        state.lpfActive = outputLpfActive_;
        state.legacyHpfEnabled = hpfEnabled_;
        state.legacyRQ15 = hpfRQ15_;
        state.hpf1RQ15 = outHpf1RQ15_;
        state.hpf2RQ15 = outHpf2RQ15_;
        state.lpfB0Q15 = outLpfB0Q15_;
        state.lpfB1Q15 = outLpfB1Q15_;
        state.lpfB2Q15 = outLpfB2Q15_;
        state.lpfA1Q15 = outLpfA1Q15_;
        state.lpfA2Q15 = outLpfA2Q15_;
        state.trimQ15 = outTrimQ15_;
        state.legacyPrevInQ15 = hpfPrevInQ15;
        state.legacyPrevOutQ15 = hpfPrevOutQ15;
        state.hpf1PrevInQ15 = outHpf1PrevInQ15_;
        state.hpf1PrevOutQ15 = outHpf1PrevOutQ15_;
        state.hpf2PrevInQ15 = outHpf2PrevInQ15_;
        state.hpf2PrevOutQ15 = outHpf2PrevOutQ15_;
        state.lpfX1Q15 = outLpfX1Q15_;
        state.lpfX2Q15 = outLpfX2Q15_;
        state.lpfY1Q15 = outLpfY1Q15_;
        state.lpfY2Q15 = outLpfY2Q15_;
        return state;
    }

    void ApuCore::restoreOutputStageState(const OutputStageStateQ15& state) {
        // Only the recursion state travels back. The stage's configuration —
        // the three selection flags, the seven coefficients and the profile
        // trim — is read-only for the duration of a block: its only writers are
        // `recomputeOutputFilterCoefficients`, `setSoundProfile`,
        // `setHpfCutoffHz` and `resetOutputFilterState`, and none of them can
        // run inside the sample loop. Writing those twelve words back would be
        // unchanged values stored into unchanged fields, paid in flash on a
        // budget where that is the scarce resource.
        //
        // Anything that makes the stage mutate its own configuration must add
        // the corresponding line here in the same change.
        hpfPrevInQ15 = state.legacyPrevInQ15;
        hpfPrevOutQ15 = state.legacyPrevOutQ15;
        outHpf1PrevInQ15_ = state.hpf1PrevInQ15;
        outHpf1PrevOutQ15_ = state.hpf1PrevOutQ15;
        outHpf2PrevInQ15_ = state.hpf2PrevInQ15;
        outHpf2PrevOutQ15_ = state.hpf2PrevOutQ15;
        outLpfX1Q15_ = state.lpfX1Q15;
        outLpfX2Q15_ = state.lpfX2Q15;
        outLpfY1Q15_ = state.lpfY1Q15;
        outLpfY2Q15_ = state.lpfY2Q15;
    }

    void ApuCore::resetOutputFilterState() {
        outHpf1PrevInQ15_ = 0;
        outHpf1PrevOutQ15_ = 0;
        outHpf2PrevInQ15_ = 0;
        outHpf2PrevOutQ15_ = 0;
        outLpfX1Q15_ = 0;
        outLpfX2Q15_ = 0;
        outLpfY1Q15_ = 0;
        outLpfY2Q15_ = 0;
    }

    // `always_inline` is the point of this shape, not a hint. At `-Os` (the
    // ESP32 preset) GCC leaves a body this size out of line, and the render
    // loop then pays two call/return pairs per sample — the exact overhead the
    // block hoist around it exists to remove. The symbol must not survive in
    // the object; the bench disassembly is where that is checked.
    PR32_APU_ALWAYS_INLINE int32_t ApuCore::applyOutputStage(
        int32_t inputQ15, OutputStageStateQ15& state) {
        if (state.chainActive) {
            // HPF 90 Hz, then HPF 440 Hz: `y[n] = x[n] - x[n-1] + R*y[n-1]`.
            //
            // Two first-order sections rather than one merged second-order
            // section on purpose. The merge (`b = [1, -2, 1]`) is exact in real
            // arithmetic but differentiates the int16 input twice, and at 20 Hz
            // that second difference (~0.5 LSB at half scale) is smaller than
            // the quantisation noise it differentiates; measured, the merged
            // form turned a -0.35 dB point into -45 dB. The cascade keeps the
            // intermediate at full scale, which is where the low-band
            // resolution comes from.
            //
            // The cost is paid back by the arithmetic: every product is int32_t
            // (|coefficient| < 2^16, |state| < 2^15, see the header's
            // static_asserts) and the /32768 is a truncating shift instead of an
            // int64 division, so |y| still drops by at least one LSB per silent
            // sample.
            int32_t hpf1 = inputQ15 - state.hpf1PrevInQ15 +
                           q15_truncate_from_int32(state.hpf1RQ15 * state.hpf1PrevOutQ15);
            if (hpf1 > 32767) hpf1 = 32767;
            if (hpf1 < -32768) hpf1 = -32768;
            state.hpf1PrevInQ15 = inputQ15;
            state.hpf1PrevOutQ15 = hpf1;

            int32_t hpf2 = hpf1 - state.hpf2PrevInQ15 +
                           q15_truncate_from_int32(state.hpf2RQ15 * state.hpf2PrevOutQ15);
            if (hpf2 > 32767) hpf2 = 32767;
            if (hpf2 < -32768) hpf2 = -32768;
            state.hpf2PrevInQ15 = hpf1;
            state.hpf2PrevOutQ15 = hpf2;

            if (!state.lpfActive) {
                return hpf2;
            }

            // Butterworth biquad:
            //   y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2, all Q15, then /32768.
            // Each product fits int32_t (same bound as above); the five-term sum
            // needs int64_t. The state is saturated so a corrupt coefficient can
            // never wrap the recursion.
            const int32_t b0x = state.lpfB0Q15 * hpf2;
            const int32_t b1x1 = state.lpfB1Q15 * state.lpfX1Q15;
            const int32_t b2x2 = state.lpfB2Q15 * state.lpfX2Q15;
            const int32_t a1y1 = state.lpfA1Q15 * state.lpfY1Q15;
            const int32_t a2y2 = state.lpfA2Q15 * state.lpfY2Q15;
            const int64_t acc = (int64_t)b0x + (int64_t)b1x1 + (int64_t)b2x2
                                - (int64_t)a1y1 - (int64_t)a2y2;
            int32_t y = q15_truncate_from_int64(acc);
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;

            state.lpfX2Q15 = state.lpfX1Q15;
            state.lpfX1Q15 = hpf2;
            state.lpfY2Q15 = state.lpfY1Q15;
            state.lpfY1Q15 = y;
            return y;
        }

        // Historical single-pole HPF — the frozen `Legacy` arithmetic. It also
        // carries `Legacy` on builds with no float core, where the integer
        // body is the only one compiled, so every line below must stay
        // byte-identical to what it replaced.
        int32_t hpfOutQ15;
        if (state.legacyHpfEnabled) {
            // Compute R * y[n-1] in Q30, then shift to Q15.
            // The shift truncates TOWARD ZERO, not arithmetically: an
            // arithmetic >>15 has a fixed point for every negative y with
            // |y| < 32768/(1-R) (~200 at the default R), so a voice that
            // went silent froze the blocker on a permanent -199 DC offset
            // instead of decaying. Truncation toward zero mirrors the
            // float path's int conversion and guarantees |y| drops by at
            // least one LSB per silent sample, whatever the coefficient.
            int64_t feedback = (int64_t)state.legacyRQ15 * (int64_t)state.legacyPrevOutQ15;
            int32_t feedbackQ15 = (int32_t)(feedback / 32768);

            // HPF difference equation
            hpfOutQ15 = inputQ15 - state.legacyPrevInQ15 + feedbackQ15;

            // Saturating clamp to prevent overflow artifacts
            if (hpfOutQ15 > 32767) hpfOutQ15 = 32767;
            if (hpfOutQ15 < -32768) hpfOutQ15 = -32768;

            // Update state for next sample
            state.legacyPrevInQ15 = inputQ15;
            state.legacyPrevOutQ15 = hpfOutQ15;
        } else {
            hpfOutQ15 = inputQ15;
            state.legacyPrevInQ15 = inputQ15;
            state.legacyPrevOutQ15 = 0;
        }
        return hpfOutQ15;
    }

    // Hito 4 M14: bundled per-voice NES options.
    void ApuCore::setVoiceNesOptions(int slot, const VoiceNesOptions& opts) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        // Delegate to the dedicated setters so their validation and side
        // effects apply unchanged — notably the index clamping and the
        // immediate retune a live NOISE voice gets from the LUT setter.
        setVoiceNesChannelEnabled(slot, opts.lengthCounterEnabled);
        setVoiceNesLinearEnabled(slot, opts.linearCounterEnabled);
        setVoiceNesEnvelopeEnabled(slot, opts.envelopeEnabled);
        setVoiceNesSweepUnitEnabled(slot, opts.sweepUnitEnabled);
        setVoicePulseDutyMode(slot, opts.pulseDutyIndex);
        // S5: the selector setter also pins the selector against the profile's
        // default, so the delegated call already records the tier-2 override.
        setVoiceNesNoiseLutMode(slot, opts.noiseLutIndex);
        // S5: `noiseUnipolar` has no standalone setter on purpose (it is
        // profile character with a per-voice override), so this write is the
        // per-voice owner: resolve the value AND pin it, otherwise the profile
        // would re-arm its own default on the very next note (D-3 tier 2).
        voices[slot].noiseUnipolar = opts.noiseUnipolar;
        voices[slot].noiseUnipolarOverridden = true;
    }

    VoiceNesOptions ApuCore::getVoiceNesOptions(int slot) const {
        VoiceNesOptions opts{};
        if (slot < 0 || slot >= MAX_VOICES) {
            return opts;
        }
        const Voice& v = voices[slot];
        opts.lengthCounterEnabled = v.nesLengthCounter.enabled;
        opts.linearCounterEnabled = v.nesLinearCounter.linearEnabled;
        opts.envelopeEnabled = v.nesEnvelope.envelopeEnabled;
        opts.sweepUnitEnabled = v.nesSweepUnit.unitEnabled;
        opts.pulseDutyIndex = v.pulseDutyIndex;
        opts.noiseLutIndex = v.nesNoiseLutIndex;
        opts.noiseUnipolar = v.noiseUnipolar;
        return opts;
    }

    // S4/S6: sound profile selector.
    void ApuCore::setSoundProfile(SoundProfile profile) {
        // Reject unknown values instead of clamping: a clamped value would
        // silently pick a neighbouring profile, and the caller's intent is
        // exactly what must not be guessed.
        switch (profile) {
            case SoundProfile::Legacy:
            case SoundProfile::Nes:
            case SoundProfile::Hybrid:
                break;
            default:
                return;  // unknown value: leave the current profile alone
        }
        if (profile == profile_) {
            // Selecting the profile already in force is a no-op. Treating it
            // as a switch would reset the output stage for nothing.
            return;
        }

        // (1) Destination core from the ARGUMENT, never from `profile_`: the
        // two are different values at this point by construction, and reading
        // the old one would sync the mirrors in the wrong direction.
        const bool destinationInteger =
            (profile != SoundProfile::Legacy) || (PR32_APU_ENABLE_LEGACY_FLOAT == 0);
        const bool coreChanges = (destinationInteger != profileUsesIntegerCore());

        // (2) Sync the envelope mirror so the block after the switch resumes at
        // the level the block before it left.
        //
        // Only when the core actually changes: the mirror each core does NOT
        // run stays at its note-on value (the integer core ticks `currentLevelQ15`
        // only, the float core ticks `currentLevel` only), so copying from the
        // idle mirror would jump the level — exactly the click this step exists
        // to prevent. With the core unchanged, the active mirror is already
        // authoritative and must be left alone.
        if (coreChanges) {
            for (int i = 0; i < MAX_VOICES; ++i) {
                Voice& voice = voices[i];
                if (destinationInteger) {
                    voice.envelope.currentLevelQ15 =
                        (int32_t)(voice.envelope.currentLevel * 32768.0f);
                } else {
                    voice.envelope.currentLevel =
                        (float)voice.envelope.currentLevelQ15 / 32768.0f;
                }
            }
        }

        // (3) Reset the output stage: the HPF recursion of one core holds no
        // meaning for the other, so both pairs start clean. E2/S1 adds the
        // profile chain's four recursions to that reset: a stale state paired
        // with the incoming profile's coefficients would ring.
        hpfPrevIn = hpfPrevOut = 0.0f;
        hpfPrevInQ15 = hpfPrevOutQ15 = 0;
        resetOutputFilterState();

        // (4) Commit. Voices keep playing: no reset(), no `enabled = false`.
        // Character is NOT re-applied to sounding voices — only notes started
        // after this point run `applyProfileCharacter` with the new profile
        // (S5/S6).
        profile_ = profile;

        // (5) The output chain belongs to the profile (S1), so its activation
        // follows the commit. Coefficients are already resolved for this rate;
        // only the active flag and the corners can change here. The caller's
        // `setHpfCutoffHz` override, if any, still wins (S8).
        recomputeOutputFilterCoefficients();
    }

    SoundProfile ApuCore::getSoundProfile() const { return profile_; }

    // Hito 4 M11: master output shaping curve.
    void ApuCore::setSoftClipMode(SoftClipMode mode) {
        switch (mode) {
            case SoftClipMode::None:
            case SoftClipMode::Rational:
            case SoftClipMode::Tanh:
            case SoftClipMode::HardClip:
                softClipMode_ = mode;
                break;
            default:
                break;  // unknown value: leave the current curve alone
        }
    }

    void ApuCore::setHpfCutoffHz(float cutoffHz) {
        // S8: any explicit corner is the caller's decision about the output
        // stage, so it outranks the profile chain from here on — the profile
        // does not install its chain over it. `reset()` clears the override.
        hpfCutoffOverridden_ = true;
        if (!(cutoffHz > 0.0f)) {   // <= 0 or NaN: bypass
            hpfCutoffHz_ = 0.0f;
            hpfEnabled_ = false;
        } else {
            hpfCutoffHz_ = cutoffHz;
            hpfEnabled_ = true;
        }
        recomputeHpfCoefficients();
        recomputeOutputFilterCoefficients();
    }

    void ApuCore::reset() {
        for (int i = 0; i < MAX_VOICES; ++i) {
            voices[i].reset();
            voices[i].type = WaveType::PULSE;
        }
        masterVolume = 1.0f;
        masterVolumeScale = 65536;
        masterBitcrushBits_ = 0;
        hpfPrevIn = hpfPrevOut = 0.0f;
        hpfPrevInQ15 = hpfPrevOutQ15 = 0;  // Q15 state for no-FPU path
        // Hito 4 M10: reset() restores configuration as well as state, the
        // same way it restores masterVolume and the bitcrush depth.
        hpfCutoffHz_ = -1.0f;
        hpfEnabled_ = true;
        recomputeHpfCoefficients();
        // E2/S1: `reset()` clears the caller's corner override (S8), so the
        // profile owns the stage again; the recursion starts clean. The
        // profile selector itself is configuration and survives (S4).
        hpfCutoffOverridden_ = false;
        resetOutputFilterState();
        recomputeOutputFilterCoefficients();
        softClipMode_ = SoftClipMode::Rational;  // Hito 4 M11
        currentPeak = 0.0f;
        samplesSinceLog = 0;
        audioTimeSamples = 0;
        for (size_t i = 0; i < MAX_MUSIC_TRACKS; ++i) {
            tracks[i] = nullptr;
            currentNoteIndices[i] = 0;
            nextNoteTicks[i] = 0;
        }
        globalTickCounter = 0;
        activeTrackCount = 0;
        tempoFactor = 1.0f;
        sequencerNoteLimit.store(MAX_NOTES_PER_FRAME, std::memory_order_release);
        deferredNotes.store(0, std::memory_order_release);
        musicPlayingFlag.store(false, std::memory_order_release);
        musicPausedFlag.store(false, std::memory_order_release);
        musicGlobalTick_.store(0, std::memory_order_release);
        musicPlayStartTick_.store(0, std::memory_order_release);
        droppedCommands.store(0, std::memory_order_release);
        firstSequencerCallAfterPlay_ = false;
        clearMusicTrackVoiceState();
        // Hito 2 M3: reset the NES frame counter to its default state.
        // mode/irqInhibit are deliberately kept (consumer-owned); the rest
        // is zeroed and the counter is disabled.
        frameCounter_.enabled = false;
        frameCounter_.currentStep = 0;
        frameCounter_.apuCycles = 0.0;
        frameCounter_.irqFlag = false;
        frameCounter_.resetPending = false;
#if defined(UNIT_TEST)
        nesQuarterTickCountForTesting_ = 0;
        nesHalfTickCountForTesting_ = 0;
        nesLinearCounterTickCountForTesting_ = 0;
        nesSweepTickCountForTesting_ = 0;
        nesIrqSetCountForTesting_ = 0;
#endif
    }

#if defined(UNIT_TEST)
    size_t ApuCore::countEnabledVoicesForTesting() const {
        size_t n = 0;
        for (int i = 0; i < MAX_VOICES; ++i) {
            if (voices[i].enabled) {
                ++n;
            }
        }
        return n;
    }

    size_t ApuCore::getSequencerMainNoteIndexForTesting() const {
        return currentNoteIndices[0];
    }

    bool ApuCore::isVoiceEnabledForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].enabled;
    }

    EnvelopeState::Stage ApuCore::getActiveVoiceStageForTesting(WaveType type) const {
        for (int i = 0; i < MAX_VOICES; ++i) {
            if (voices[i].enabled && voices[i].type == type) {
                return voices[i].envelope.stage;
            }
        }
        return EnvelopeState::Stage::OFF;
    }

    int ApuCore::getTrackVoiceSlotForTesting(size_t track_index) const {
        if (track_index >= MAX_MUSIC_TRACKS) {
            return kInvalidVoiceSlot;
        }
        if (!track_voice_active_[track_index]) {
            return kInvalidVoiceSlot;
        }
        return static_cast<int>(track_index);
    }

    EnvelopeState::Stage ApuCore::getTrackVoiceStageForTesting(
        size_t track_index) const
    {
        if (track_index >= MAX_MUSIC_TRACKS) {
            return EnvelopeState::Stage::OFF;
        }
        const Voice& voice = voices[static_cast<int>(track_index)];
        if (!voice.enabled) {
            return EnvelopeState::Stage::OFF;
        }
        return voice.envelope.stage;
    }

    bool ApuCore::isMusicTrackVoiceActiveForTesting(size_t track_index) const {
        if (track_index >= MAX_MUSIC_TRACKS) {
            return false;
        }
        return track_voice_active_[track_index];
    }

    uint32_t ApuCore::getVoiceNoisePeriodForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].noisePeriodSamples;
    }

    uint64_t ApuCore::getVoiceRemainingSamplesForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].remainingSamples;
    }

    bool ApuCore::isVoiceLoopForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].loop;
    }

    float ApuCore::getVoiceFrequencyForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0.0f;
        }
        if (!profileUsesIntegerCore()) {
            return voices[slot].frequency;
        }
        // The integer core runs the oscillator from `basePhaseIncQ32`; the
        // float `frequency` field is only refreshed by the helpers shared
        // with the float build (note-on, pitch-envelope segments), so a
        // single-segment sweep would leave it at the note-on value and this
        // accessor would report a pitch the core is not playing. Deriving Hz
        // from the increment the core actually uses keeps the readout honest.
        // The branch is on the profile, not on `PR32_APU_HAS_FPU`: on an FPU
        // target `Nes`/`Hybrid` render the integer core too (S3, L8.3).
        // NOISE has no phase increment (its pitch is the LFSR period, which
        // `getVoiceNoisePeriodForTesting` reports), so it keeps the float.
        if (voices[slot].type == WaveType::NOISE || sampleRate <= 0) {
            return voices[slot].frequency;
        }
        return (float)((double)voices[slot].basePhaseIncQ32
                       * (double)sampleRate / 4294967296.0);
    }

    float ApuCore::getVoiceDutyCycleForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0.0f;
        }
        if (!profileUsesIntegerCore()) {
            return voices[slot].dutyCycle;
        }
        // Same reasoning as getVoiceFrequencyForTesting: the continuous duty
        // sweep advances `dutyCycleQ32` only, so the Q32 mirror is the duty
        // the integer core actually compares against the phase.
        return (float)((double)voices[slot].dutyCycleQ32 / 4294967296.0);
    }

    float ApuCore::getVoiceDutySweepPerSampleForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0.0f;
        }
        return voices[slot].dutySweep;
    }

    float ApuCore::getVoicePhaseForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0.0f;
        }
        if (!profileUsesIntegerCore()) {
            return voices[slot].phase;
        }
        // The integer core never touches the float phase accumulator, so the
        // Q32 mirror is the only place the rendered phase exists.
        return (float)((double)voices[slot].phaseQ32 / 4294967296.0);
    }

    uint32_t ApuCore::getProfileApplyCountForTesting() const {
        return profileApplyCount_;
    }

    int32_t ApuCore::getProfileTrimQ15ForTesting() const {
        // `outTrimQ15_` is resolved with the chain's activation, so this reports
        // unity when `setHpfCutoffHz` suppressed the chain (S8) — that is what
        // the renderer applies, not what the profile would have applied.
        return outTrimQ15_;
    }

    EnvelopeCurve ApuCore::getVoiceEnvelopeCurveForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return EnvelopeCurve::ProfileDefault;
        }
        return voices[slot].envelopeCurve;
    }

    int32_t ApuCore::getVoiceEnvelopeLevelQ15ForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0;
        }
        // The integer core is the only body that ticks this mirror, and every
        // profile except `Legacy` runs it; on `Legacy` an FPU build leaves it
        // at its note-on value. That is the same contract as `phaseQ32`.
        return voices[slot].envelope.currentLevelQ15;
    }

    uint32_t ApuCore::getVoicePhaseIncQ32ForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].phaseIncQ32;
    }

    int ApuCore::sawMipmapLevelForTesting(uint32_t phaseIncQ32) {
        return sawMipLevelForPhaseIncQ32(phaseIncQ32);
    }
#endif  // UNIT_TEST — the test-only accessors end here.

    // =====================================================================
    // NES sub-units (Hito 2 M3-M7). PRODUCTION CODE — everything from here
    // to the end of tickNesFrameCounter is public API and MUST stay outside
    // the UNIT_TEST guard. It once sat inside it, which compiled fine in
    // every suite (they all recompile this file with UNIT_TEST=1) while the
    // shipped library lost the symbols and no consumer could link. See
    // test_library_link, which exists to catch exactly that.
    // =====================================================================

    // -- NES frame counter (Hito 2 M3) -----------------------------------
    void ApuCore::setNesFrameCounterMode(int mode) {
        if (mode != 0 && mode != 1) {
            // Out-of-range values are silently ignored (no state change).
            return;
        }
        // Idempotent: same mode does not schedule another reset.
        if (frameCounter_.mode == mode && frameCounter_.enabled) {
            return;
        }
        frameCounter_.mode = mode;
        frameCounter_.enabled = true;
        // $4017 side effect: timer reset on the next step entry.
        frameCounter_.resetPending = true;
        // Mode change also clears the IRQ flag (mirrors hardware).
        frameCounter_.irqFlag = false;
    }

    void ApuCore::setNesFrameCounterInhibit(bool inhibit) {
        frameCounter_.irqInhibit = inhibit;
        if (inhibit) {
            // $4017 I=1 clears the IRQ flag immediately.
            frameCounter_.irqFlag = false;
        }
    }

    bool ApuCore::getNesFrameCounterIrq() const {
        return frameCounter_.irqFlag;
    }

    void ApuCore::tickAllNesEnvelopes() {
        // Hito 2 M7: NES envelope unit dispatch (quarter-frame consumer).
        //
        // Iterates the voice pool and ticks the envelope of every voice
        // that has `envelopeEnabled == true` and is a PULSE or NOISE
        // voice (TRIANGLE / SINE / SAW are skipped; the NES hardware
        // has no envelope for them). The algorithm mirrors
        // https://www.nesdev.org/wiki/APU_Envelope exactly:
        //
        //   if (startFlag) {
        //       startFlag = false;
        //       decayLevel = 15;
        //       dividerCounter = volume;   // immediate reload
        //   } else if (dividerCounter == 0) {
        //       dividerCounter = volume;
        //       if (decayLevel > 0) {
        //           --decayLevel;
        //       } else if (loop) {         // loop = lengthCounter->halt
        //           decayLevel = 15;
        //       } else {
        //           silence the channel    // v.enabled = false, ADSR off
        //       }
        //   } else {
        //       --dividerCounter;
        //   }
        //   output = constVolume ? volume : decayLevel;
        //
        // The `loop` flag is read from `nesLengthCounter.halt` (the
        // shared bit; matches M5's pattern for the linear counter).
        for (int slot = 0; slot < MAX_VOICES; ++slot) {
            Voice& v = voices[slot];
            if (v.type != WaveType::PULSE && v.type != WaveType::NOISE) continue;
            NesEnvelope& env = v.nesEnvelope;
            if (!env.envelopeEnabled) continue;
            if (env.lengthCounter == nullptr) continue;  // safety: pointer must be bound in reset()
            const bool loop = env.lengthCounter->halt;    // shared with length counter

            if (env.startFlag) {
                env.startFlag = false;
                env.decayLevel = 15;
                env.dividerCounter = env.volume;
            } else {
                if (env.dividerCounter == 0) {
                    env.dividerCounter = env.volume;
                    if (env.decayLevel > 0) {
                        --env.decayLevel;
                    } else {
                        if (loop) {
                            env.decayLevel = 15;
                        } else {
                            // Silence the channel. Matches the M4 / M5
                            // pattern: disable the voice, zero the
                            // ADSR envelope so the per-sample gate sees
                            // 0 immediately. The triangle voice is
                            // untouched (it has no envelope).
                            v.enabled = false;
                            v.envelope.stage = EnvelopeState::Stage::OFF;
                            v.envelope.currentLevel = 0.0f;
                            v.envelope.currentLevelQ15 = 0;
                        }
                    }
                } else {
                    --env.dividerCounter;
                }
            }

            env.output = env.constVolume ? env.volume : env.decayLevel;
        }
#if defined(UNIT_TEST)
#if defined(UNIT_TEST)
        ++nesQuarterTickCountForTesting_;
#endif
#endif
    }

    void ApuCore::tickAllNesLinearCounters() {
        // Hito 2 M5: NES linear counter dispatch.
        //
        // Iterates the voice pool. Voices with `linearEnabled == false`
        // (the default for everything except TRIANGLE-in-NES-mode) are
        // skipped silently. For the opted-in voices:
        //   - if `reloadFlag` is set, the counter is loaded from `reloadValue`;
        //   - else if the counter is > 0, it is decremented;
        //   - if the shared `halt`/control flag is clear, `reloadFlag` is cleared.
        //
        // The order matters: reload/decrement FIRST, then clear reloadFlag.
        // This matches the canonical nesdev wiki algorithm exactly.
        //
        // This method is called by the existing `tickNesFrameCounter`
        // from M3, on the quarter-frame step. It is NOT called per sample.
        for (int slot = 0; slot < MAX_VOICES; ++slot) {
            Voice& v = voices[slot];
            NesLinearCounter& lc = v.nesLinearCounter;
            if (!lc.linearEnabled) continue;
            if (lc.lengthCounter == nullptr) continue;  // safety: pointer must be bound in reset()
            const bool control = lc.lengthCounter->halt;  // shared with length counter

            if (lc.reloadFlag) {
                lc.counter = lc.reloadValue;
            } else if (lc.counter > 0) {
                --lc.counter;
            }
            if (!control) {
                lc.reloadFlag = false;
            }
        }
#if defined(UNIT_TEST)
        ++nesLinearCounterTickCountForTesting_;
#endif
    }

    void ApuCore::tickAllNesLengthCounters() {
        // Hito 2 M4: NES length counter dispatch.
        //
        // Iterates the voice pool. SINE/SAW voices keep
        // `nesLengthCounter.enabled == false` and are silently skipped.
        // NES voices (PULSE/TRIANGLE/NOISE) decrement their counter on
        // each half-clock (skipping when `halt` is set or the counter is
        // already 0), and silence the voice when the counter hits 0.
        //
        // Hito 2 M5 surgical fix: TRIANGLE voices are NOT disabled when
        // the length counter reaches 0. The M5 per-sample gate
        // (lengthOk && linearOk) handles the silence path, and the voice
        // stays enabled so it can be re-armed by a fresh $400B write.
        // PULSE and NOISE keep the legacy M4 behaviour.
        //
        // This method is called by the existing `tickNesFrameCounter`
        // from M3, on the half-frame step. It is NOT called per sample.
        for (int slot = 0; slot < MAX_VOICES; ++slot) {
            Voice& v = voices[slot];
            NesLengthCounter& lc = v.nesLengthCounter;
            if (!lc.enabled) continue;
            if (lc.halt) continue;
            if (lc.counter == 0) continue;
            --lc.counter;
            if (lc.counter == 0) {
                if (v.type != WaveType::TRIANGLE) {
                    // Silence the voice. Do NOT touch remainingSamples
                    // (M4 is orthogonal to the pre-Hito-2 duration mechanism;
                    // the caller's plan stays intact, but the voice is
                    // disabled so it will not produce audio).
                    v.enabled = false;
                    v.envelope.stage = EnvelopeState::Stage::OFF;
                    v.envelope.currentLevel = 0.0f;
                    v.envelope.currentLevelQ15 = 0;
                }
                // TRIANGLE: stay enabled; M5 per-sample gate handles silence.
            }
        }
#if defined(UNIT_TEST)
        ++nesHalfTickCountForTesting_;
#endif
    }

    // Hito 2 M4: NES length counter public API.
    void ApuCore::setVoiceNesLength(int slot, uint8_t lengthIndex, bool halt) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        NesLengthCounter& lc = voices[slot].nesLengthCounter;
        // Writes to a disabled NES channel are silently lost (mirrors NES
        // hardware: the channel is muted, so length loads are ignored).
        if (!lc.enabled) {
            return;
        }
        if (lengthIndex > 31) {
            lengthIndex = 31;
        }
        lc.index = lengthIndex;
        lc.counter = nes_apu::kNesLengthLut[lengthIndex];
        lc.halt = halt;
        // Hito 2 M7 surgical fix: NES $4003 / $4007 / $400F writes set the
        // envelope start flag as a side effect. The flag is consumed by
        // the next quarter-clock tick (the M7 `tickAllNesEnvelopes()`
        // dispatcher reloads `decayLevel = 15` and the divider from VVVV).
        // It is set even if the envelope is not opted in (matches NES
        // hardware: the bit is set independently of the envelope enable).
        voices[slot].nesEnvelope.startFlag = true;
        // Hito 2 M13: the other documented side effect of the same write.
        // On $400B (TRIANGLE) the length load raises the linear counter's
        // reload flag, consumed by the next quarter-clock in
        // `tickAllNesLinearCounters()`. Set unconditionally for the same
        // reason as the envelope start flag above: the dispatchers already
        // gate on type and on their own opt-in flag, so a voice that is
        // not a NES-mode TRIANGLE never observes it.
        voices[slot].nesLinearCounter.reloadFlag = true;
    }

    void ApuCore::setVoiceNesChannelEnabled(int slot, bool enabled) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        NesLengthCounter& lc = voices[slot].nesLengthCounter;
        Voice& v = voices[slot];
        lc.enabled = enabled;
        if (enabled) {
            // NES: "There is no immediate effect when enabled is set."
            return;
        }
        // Cleared: force the counter to 0 and silence the voice.
        lc.counter = 0;
        v.enabled = false;
        v.envelope.stage = EnvelopeState::Stage::OFF;
        v.envelope.currentLevel = 0.0f;
        v.envelope.currentLevelQ15 = 0;
        // Note: remainingSamples is intentionally NOT touched here.
        // M4 is orthogonal to the pre-Hito-2 duration mechanism (§2.4 of
        // the M4 design doc).
    }

    // Hito 2 M5: NES linear counter public API.
    void ApuCore::setVoiceNesLinearCounter(int slot, uint8_t reloadValue, bool reloadFlag) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        NesLinearCounter& lc = voices[slot].nesLinearCounter;
        // Mirror the M4 pattern: always store, effect only at the next
        // quarter-clock tick. This means writes to a voice that has not
        // yet enabled NES mode are kept and acted on later when the voice
        // is opted in.
        lc.reloadValue = (reloadValue & 0x7F);
        lc.reloadFlag = reloadFlag;
    }

    void ApuCore::setVoiceNesLinearEnabled(int slot, bool enabled) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        voices[slot].nesLinearCounter.linearEnabled = enabled;
    }

    // Hito 2 M7: NES envelope unit public API.
    void ApuCore::setVoiceNesEnvelope(int slot, bool loop, bool constVolume, uint8_t volume) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        NesEnvelope& env = voices[slot].nesEnvelope;
        env.volume = (volume & 0x0F);
        env.constVolume = constVolume;
        // The `loop` arg is stored for introspection, but the live read
        // at quarter-clock time comes from `nesLengthCounter.halt`
        // (shared bit, bound via pointer in AudioChannel::reset()).
        // This matches the M5 linear counter pattern.
        // We do NOT change `nesLengthCounter.halt` here — that is owned
        // by `setVoiceNesLength`. A test that wants the envelope loop
        // set must call `setVoiceNesLength(slot, idx, true)` first.
        // The "loop" arg on this method is therefore a stored hint
        // mirrored into the envelope struct for symmetry with the
        // wiki register layout, but the dispatch loop reads `halt`.
        (void)loop;
        // Mirrors the NES hardware: the envelope write does NOT set the
        // start flag. The start flag is set exclusively by the length
        // load (`setVoiceNesLength`).
    }

    void ApuCore::setVoiceNesEnvelopeEnabled(int slot, bool enabled) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        voices[slot].nesEnvelope.envelopeEnabled = enabled;
    }

    void ApuCore::tickAllNesSweepUnits() {
        // Hito 2 M6: NES sweep unit dispatch (half-clock consumer).
        //
        // Iterates the voice pool and ticks the sweep unit of every PULSE
        // voice. Non-PULSE voices are skipped (their nesSweepUnit is
        // configured by callers but never updated by the dispatcher).
        //
        // Per https://www.nesdev.org/wiki/APU_Sweep, the half-clock logic is:
        //   1. Compute the target period using the CURRENT timer, shift, negate,
        //      and isPulse2. The target is checked for > 0x7FF BEFORE clamping
        //      — overflow is the signal for muting, so it must be observed.
        //   2. Update the `muted` flag from (current period < 8) OR (target > 0x7FF).
        //      This is evaluated CONTINUOUSLY, even when the sweep is "disabled"
        //      (E=0 or SSS=0). That's the "NES bug": the muting logic always runs,
        //      the timer-update path doesn't.
        //   3. If `dividerCounter == 0` and the sweep is enabled, shift > 0, and
        //      the channel isn't muted, update `timer` from the (clamped) target.
        //   4. Reload the divider: if `dividerCounter == 0` OR `reloadFlag` is
        //      set, `dividerCounter = period + 1`; else decrement.
        for (int slot = 0; slot < MAX_VOICES; ++slot) {
            Voice& v = voices[slot];
            if (v.type != WaveType::PULSE) continue;
            NesSweepUnit& sw = v.nesSweepUnit;
            // Opt-in gate. Without it the muting rule below would fire on
            // the default `timer == 0` and silence every PULSE voice that
            // is still driven by the legacy float frequency path.
            if (!sw.unitEnabled) continue;

            // Step 1: compute the raw target period.
            const uint16_t period = sw.timer;
            const int32_t change = (sw.shift > 0) ? (period >> sw.shift) : 0;
            int32_t target;
            if (sw.negate) {
                target = sw.isPulse2
                    ? ((int32_t)period - change)
                    : ((int32_t)period - change - 1);
            } else {
                target = (int32_t)period + change;
            }
            if (target < 0) target = 0;
            // NOTE: do NOT clamp target to 0x7FF here — the muting check
            // needs the raw value. Clamp only happens when the timer is
            // actually updated in step 3.

            // Step 2: update the muting flag (always).
            sw.muted = (period < 8) || (target > 0x7FF);

            // Step 3: timer update (only on the divider==0 boundary, with
            // the three "sweep is active" guards: E=1, SSS>0, !muted).
            if (sw.dividerCounter == 0) {
                if (sw.sweepEnabled && sw.shift > 0 && !sw.muted) {
                    int32_t clamped = (target > 0x7FF) ? 0x7FF : target;
                    sw.timer = (uint16_t)clamped;
                    // Re-sync the per-voice phase increments so the hot
                    // path sees the new frequency immediately (no need
                    // to wait for the next setVoiceNesTimer call).
                    syncNesTimerToPhaseIncQ32(v, sampleRate);
                }
            }

            // Step 4: reload or decrement the divider.
            if (sw.dividerCounter == 0 || sw.reloadFlag) {
                sw.dividerCounter = sw.period + 1;
                sw.reloadFlag = false;
            } else {
                --sw.dividerCounter;
            }
        }
#if defined(UNIT_TEST)
        ++nesSweepTickCountForTesting_;
#endif
    }

    // Hito 2 M6: NES sweep unit public API.
    void ApuCore::setVoiceNesSweep(int slot, bool enabled, uint8_t period, bool negate,
                                   uint8_t shift, bool isPulse2) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        NesSweepUnit& sw = voices[slot].nesSweepUnit;
        sw.sweepEnabled = enabled;
        sw.period = (period & 0x07);
        sw.negate = negate;
        sw.shift = (shift & 0x07);
        sw.isPulse2 = isPulse2;
        // $4001 / $4005 write side effect: schedule a divider reload.
        sw.reloadFlag = true;
        // The muting flag is continuously evaluated (the "NES bug"),
        // so any change to E/P/N/SSS must re-trigger the check, even
        // before the next half-clock. This matches the spec test
        // `test_sweep_muting_target_overflow`: the caller sets the
        // timer, then the sweep fields, and expects `muted` to be
        // observable immediately.
        recomputeSweepMuted(voices[slot]);
    }

    void ApuCore::setVoiceNesSweepUnitEnabled(int slot, bool enabled) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        voices[slot].nesSweepUnit.unitEnabled = enabled;
        // Re-evaluate immediately in both directions: arming must apply
        // the muting rule to the register state already written, and
        // disarming must clear `muted` so the per-sample gate stops
        // silencing a voice that left NES sweep control.
        recomputeSweepMuted(voices[slot]);
    }

    void ApuCore::setVoiceNesTimer(int slot, uint16_t timer11) {
        if (slot < 0 || slot >= MAX_VOICES) {
            return;
        }
        timer11 &= 0x07FF;
        voices[slot].nesSweepUnit.timer = timer11;
        // Re-sync the per-voice phase increments so the oscillator
        // frequency matches the new NES timer period immediately
        // (the hot path does NOT need to wait for the next half-clock).
        syncNesTimerToPhaseIncQ32(voices[slot], sampleRate);
        // Recompute the muting flag right now so the per-sample
        // gate sees the correct value before the next half-clock.
        recomputeSweepMuted(voices[slot]);
    }

    // Hito 2 M3: per-sample tick of the NES frame counter.
    // `apuCyclesPerSample` is computed once per generateSamples() call and
    // passed in to keep the inner loop free of division by sampleRate.
    void ApuCore::tickNesFrameCounter(double apuCyclesPerSample) {
        // Default OFF: the canonical track never enables the frame counter,
        // so its PCM must stay byte-for-byte identical (no accumulator
        // update, no clock dispatches).
        if (!frameCounter_.enabled) {
            return;
        }
        // Apply $4017 write reset side-effect: zero the cycle counter on
        // the first sample after the mode was set.
        if (frameCounter_.resetPending) {
            frameCounter_.apuCycles = 0.0;
            frameCounter_.currentStep = 0;
            frameCounter_.resetPending = false;
            // Per nesdev wiki, $4017 M=1 also fires quarter+half on the reset
            // sample. Mode 0 does not (it just resets the timer).
            if (frameCounter_.mode == 1) {
                tickAllNesEnvelopes();
                tickAllNesLinearCounters();
                tickAllNesLengthCounters();
                tickAllNesSweepUnits();
            }
        }

        frameCounter_.apuCycles += apuCyclesPerSample;

        const double* steps = (frameCounter_.mode == 0)
            ? nes_apu::kMode0ApuCyclesPerStep
            : nes_apu::kMode1ApuCyclesPerStep;
        const int maxStep = (frameCounter_.mode == 0) ? 4 : 5;

        // Fire every step the accumulator has crossed (could be > 1 at low
        // sample rates). The step fired is the new `currentStep` (1-indexed).
        while (frameCounter_.apuCycles >= steps[frameCounter_.currentStep + 1]) {
            ++frameCounter_.currentStep;

            const bool isQuarter = (frameCounter_.mode == 0)
                ? (frameCounter_.currentStep == 1
                    || frameCounter_.currentStep == 2
                    || frameCounter_.currentStep == 3)
                : (frameCounter_.currentStep == 1
                    || frameCounter_.currentStep == 2
                    || frameCounter_.currentStep == 3
                    || frameCounter_.currentStep == 5);
            const bool isHalf = (frameCounter_.mode == 0)
                ? (frameCounter_.currentStep == 2
                    || frameCounter_.currentStep == 4)
                : (frameCounter_.currentStep == 2
                    || frameCounter_.currentStep == 4);

            if (isQuarter) {
                tickAllNesEnvelopes();
                tickAllNesLinearCounters();
            }
            if (isHalf) {
                tickAllNesLengthCounters();
                tickAllNesSweepUnits();
            }

            if (frameCounter_.currentStep >= maxStep) {
                // Wrap. Subtract the cycle count of the last step so
                // `apuCycles` represents time since wrap. The wrap is
                // itself a "step 0 entry" of the next cycle:
                //   - Mode 0: fires one extra quarter + IRQ set. Half was
                //     already fired at step 4 entry (designed doc, table
                //     "→ 0" row). This matches the canonical 240/120/60 Hz
                //     rate: 4 quarters, 2 halves, 1 IRQ per cycle.
                //   - Mode 1: no clocks on wrap (designed doc, table "→ 0"
                //     row is empty). Matches 192/~96 Hz rate.
                if (frameCounter_.mode == 0) {
                    tickAllNesEnvelopes();
                    tickAllNesLinearCounters();
                    if (!frameCounter_.irqInhibit) {
                        // The visible flag is sticky (no $4015 read
                        // modeled), but we still flip false→true to
                        // reflect the actual edge. The test-only counter
                        // also tracks the *generation* rate (60 Hz) so
                        // the test can observe the configured cadence
                        // without the flag oscillating.
                        if (!frameCounter_.irqFlag) {
                            frameCounter_.irqFlag = true;
                        }
#if defined(UNIT_TEST)
                        ++nesIrqSetCountForTesting_;
#endif
                    }
                }
                frameCounter_.apuCycles -= steps[maxStep];
                frameCounter_.currentStep = 0;
            }
        }
    }

#if defined(UNIT_TEST)
    // Test-only accessors resume here.
    void ApuCore::resetNesFrameCounterForTesting() {
        frameCounter_.apuCycles = 0.0;
        frameCounter_.currentStep = 0;
        frameCounter_.irqFlag = false;
        frameCounter_.resetPending = false;
        // mode and irqInhibit are deliberately left untouched.
    }

    int ApuCore::getNesFrameCounterQuarterTickCountForTesting() const {
        return nesQuarterTickCountForTesting_;
    }

    int ApuCore::getNesFrameCounterHalfTickCountForTesting() const {
        return nesHalfTickCountForTesting_;
    }

    int ApuCore::getNesFrameCounterLinearTickCountForTesting() const {
        return nesLinearCounterTickCountForTesting_;
    }

    int ApuCore::getNesFrameCounterSweepTickCountForTesting() const {
        return nesSweepTickCountForTesting_;
    }

    int ApuCore::getNesFrameCounterIrqSetCountForTesting() const {
        return nesIrqSetCountForTesting_;
    }

    double ApuCore::getNesFrameCounterApuCyclesForTesting() const {
        return frameCounter_.apuCycles;
    }

    int ApuCore::getNesFrameCounterCurrentStepForTesting() const {
        return frameCounter_.currentStep;
    }

    // Hito 2 M4: NES length counter test-only accessors.
    uint16_t ApuCore::getVoiceNesLengthCounterForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesLengthCounter.counter;
    }

    uint8_t ApuCore::getVoiceNesLengthIndexForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesLengthCounter.index;
    }

    bool ApuCore::getVoiceNesLengthHaltForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesLengthCounter.halt;
    }

    bool ApuCore::getVoiceNesChannelEnabledForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesLengthCounter.enabled;
    }

    // Hito 2 M5: NES linear counter test-only accessors.
    uint8_t ApuCore::getVoiceNesLinearCounterForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesLinearCounter.counter;
    }

    uint8_t ApuCore::getVoiceNesLinearReloadValueForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesLinearCounter.reloadValue;
    }

    bool ApuCore::getVoiceNesLinearReloadFlagForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesLinearCounter.reloadFlag;
    }

    bool ApuCore::getVoiceNesLinearEnabledForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesLinearCounter.linearEnabled;
    }

    // Hito 2 M6: NES sweep unit test-only accessors.
    uint16_t ApuCore::getVoiceNesSweepTimerForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesSweepUnit.timer;
    }

    uint8_t ApuCore::getVoiceNesSweepDividerForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesSweepUnit.dividerCounter;
    }

    bool ApuCore::getVoiceNesSweepEnabledForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesSweepUnit.sweepEnabled;
    }

    bool ApuCore::getVoiceNesSweepNegateForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesSweepUnit.negate;
    }

    uint8_t ApuCore::getVoiceNesSweepShiftForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesSweepUnit.shift;
    }

    bool ApuCore::getVoiceNesSweepUnitEnabledForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesSweepUnit.unitEnabled;
    }

    bool ApuCore::getVoiceNesSweepMutedForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesSweepUnit.muted;
    }

    // Hito 2 M7: NES envelope unit test-only accessors.
    uint8_t ApuCore::getVoiceNesEnvelopeOutputForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesEnvelope.output;
    }

    uint8_t ApuCore::getVoiceNesEnvelopeDecayLevelForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesEnvelope.decayLevel;
    }

    uint8_t ApuCore::getVoiceNesEnvelopeDividerForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesEnvelope.dividerCounter;
    }

    bool ApuCore::getVoiceNesEnvelopeStartFlagForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesEnvelope.startFlag;
    }

    bool ApuCore::getVoiceNesEnvelopeEnabledForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesEnvelope.envelopeEnabled;
    }

    bool ApuCore::getVoiceNesEnvelopeConstVolumeForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].nesEnvelope.constVolume;
    }

    uint8_t ApuCore::getVoiceNesEnvelopeVolumeForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 0u;
        }
        return voices[slot].nesEnvelope.volume;
    }

    uint8_t ApuCore::getVoiceNesNoiseLutIndexForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return 255u;
        }
        return voices[slot].nesNoiseLutIndex;
    }

    // S5/E1: resolved unipolar gate of a voice slot.
    bool ApuCore::isVoiceNoiseUnipolarForTesting(int slot) const {
        if (slot < 0 || slot >= MAX_VOICES) {
            return false;
        }
        return voices[slot].noiseUnipolar;
    }

    // Hito 4 M10: master HPF test-only accessors.
    float ApuCore::getHpfRForTesting() const { return hpfR_; }
    int32_t ApuCore::getHpfRQ15ForTesting() const { return hpfRQ15_; }
    bool ApuCore::getHpfEnabledForTesting() const { return hpfEnabled_; }
    float ApuCore::getHpfCutoffHzForTesting() const { return hpfCutoffHz_; }

    // Hito 4 M11.
    SoftClipMode ApuCore::getSoftClipModeForTesting() const { return softClipMode_; }

    // E1/S4: the mixer curve follows the profile, so this accessor is the
    // observable half of D-3 tier 1 (there is no setter to test against).
    MixerCurve ApuCore::getMixerCurveForTesting() const {
        return profileMixerCurve(profile_);
    }

    // E2/S1: the output chain is profile character with no public setter, so
    // these accessors are the observable half of the API.
    bool ApuCore::isOutputFilterChainActiveForTesting() const {
        return outputFilterActive_;
    }

    bool ApuCore::isOutputFilterLpfActiveForTesting() const {
        return outputLpfActive_;
    }

    int32_t ApuCore::getOutputFilterHpf1RQ15ForTesting() const {
        return outHpf1RQ15_;
    }

    int32_t ApuCore::getOutputFilterHpf2RQ15ForTesting() const {
        return outHpf2RQ15_;
    }

    int32_t ApuCore::getOutputFilterLpfB0Q15ForTesting() const {
        return outLpfB0Q15_;
    }

    int32_t ApuCore::getOutputFilterLpfB1Q15ForTesting() const {
        return outLpfB1Q15_;
    }

    int32_t ApuCore::getOutputFilterLpfB2Q15ForTesting() const {
        return outLpfB2Q15_;
    }

    int32_t ApuCore::getOutputFilterLpfA1Q15ForTesting() const {
        return outLpfA1Q15_;
    }

    int32_t ApuCore::getOutputFilterLpfA2Q15ForTesting() const {
        return outLpfA2Q15_;
    }

    float ApuCore::getOutputFilterHpf1CutoffHzForTesting() const {
        return outHpf1CutoffHz_;
    }

    float ApuCore::getOutputFilterHpf2CutoffHzForTesting() const {
        return outHpf2CutoffHz_;
    }

    float ApuCore::getOutputFilterLpfCutoffHzForTesting() const {
        return outLpfCutoffHz_;
    }

    int32_t ApuCore::getOutputFilterStateMaxAbsForTesting() const {
        const int32_t values[] = {
            hpfPrevInQ15,      hpfPrevOutQ15,
            outHpf1PrevInQ15_, outHpf1PrevOutQ15_,
            outHpf2PrevInQ15_, outHpf2PrevOutQ15_,
            outLpfX1Q15_,      outLpfX2Q15_,
            outLpfY1Q15_,      outLpfY2Q15_,
        };
        int32_t worst = 0;
        for (int32_t v : values) {
            const int32_t magnitude = (v < 0) ? -v : v;
            if (magnitude > worst) {
                worst = magnitude;
            }
        }
        return worst;
    }

    void ApuCore::resetOutputFilterStateForTesting() {
        // The historical one-pole is part of the stage too, so a test that
        // compares whole buffers starts it from zero as well.
        hpfPrevIn = 0.0f;
        hpfPrevOut = 0.0f;
        hpfPrevInQ15 = 0;
        hpfPrevOutQ15 = 0;
        resetOutputFilterState();
    }

    void ApuCore::processOutputStageForTesting(int16_t* buffer, int length) {
        if (buffer == nullptr || length <= 0) {
            return;
        }
        // The same block-local snapshot the render loop keeps, so the hook still
        // drives PRODUCTION code — `applyOutputStage` on hoisted state — and not
        // a per-sample re-read of `this` that the renderer no longer performs.
        // State still persists across calls: the snapshot is written back below.
        OutputStageStateQ15 stage = captureOutputStageState();
        for (int i = 0; i < length; ++i) {
            int32_t sample = applyOutputStage(static_cast<int32_t>(buffer[i]), stage);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            buffer[i] = apply_master_bitcrush(static_cast<int16_t>(sample),
                                              masterBitcrushBits_);
        }
        restoreOutputStageState(stage);
    }
#endif

    void ApuCore::clearMusicTrackVoiceState() {
        for (size_t i = 0; i < MAX_MUSIC_TRACKS; ++i) {
            track_voice_active_[i] = false;
        }
    }

    Voice* ApuCore::musicVoiceForTrack(size_t track_index) {
        if (track_index >= MAX_MUSIC_TRACKS) {
            return nullptr;
        }
        return &voices[MUSIC_VOICE_BASE + static_cast<int>(track_index)];
    }

    void ApuCore::setSequencerNoteLimit(size_t limit) {
        if (limit == 0) {
            // Zero means unbounded (fallback to effectively unlimited)
            limit = MAX_NOTES_PER_FRAME * 32; // ~1024, effectively unbounded
        } else if (limit > 1000) {
            limit = 32; // Clamp per spec: > 1000 → 32
        }
        sequencerNoteLimit.store(limit, std::memory_order_release);
    }

    bool ApuCore::submitCommand(const AudioCommand& cmd) {
        const bool ok = commandQueue.enqueue(cmd);
        if (!ok) {
            droppedCommands.fetch_add(1u, std::memory_order_relaxed);
#if defined(PIXELROOT32_DEBUG_MODE)
            static uint32_t lastLogged = 0;
            const uint32_t total = droppedCommands.load(std::memory_order_relaxed);
            if (total - lastLogged >= 16u) {
                lastLogged = total;
                apuLogf(ApuLogLevel::Warning, "[AUDIO] command queue full; dropped %u total", (unsigned)total);
            }
#endif
        }
        return ok;
    }

    // ------------------------------------------------------------------
    // Command processing
    // ------------------------------------------------------------------
    void ApuCore::processCommands() {
        AudioCommand cmd;
        while (commandQueue.dequeue(cmd)) {
            switch (cmd.type) {
                case AudioCommandType::PLAY_EVENT:
                    executePlayEvent(cmd.event);
                    break;

                case AudioCommandType::SET_MASTER_VOLUME: {
                    float v = cmd.volume;
                    if (v > 1.0f) v = 1.0f;
                    if (v < 0.0f) v = 0.0f;
                    masterVolume = v;
                    masterVolumeScale = (int32_t)(masterVolume * 65536.0f);
                    break;
                }

                case AudioCommandType::SET_MASTER_BITCRUSH: {
                    uint8_t b = cmd.masterBitcrushBits;
                    if (b > 15u) b = 15u;
                    masterBitcrushBits_ = b;
                    break;
                }

                case AudioCommandType::STOP_CHANNEL:
                    if (cmd.channelIndex < MAX_VOICES) {
                        voices[cmd.channelIndex].reset();
                    }
                    break;

                case AudioCommandType::SET_NES_OPTIONS:
                    // Bounds-checked inside the setter, but keep the guard
                    // here too so the intent is visible next to the others.
                    if (cmd.channelIndex < MAX_VOICES) {
                        setVoiceNesOptions(cmd.channelIndex, cmd.nesOptions);
                    }
                    break;

                case AudioCommandType::SET_SOUND_PROFILE:
                    // No channel: the profile is global. The setter ignores
                    // unknown values, so no clamp is needed here either.
                    setSoundProfile(static_cast<SoundProfile>(cmd.soundProfile));
                    break;

                case AudioCommandType::TRIGGER_NES_LENGTH:
                    if (cmd.channelIndex < MAX_VOICES) {
                        // Arm the channel first: `setVoiceNesLength` drops
                        // writes to a disabled NES channel, and the enable
                        // setter is not reachable from the game thread, so
                        // without this the queued path could never fire.
                        setVoiceNesChannelEnabled(cmd.channelIndex, true);
                        setVoiceNesLength(cmd.channelIndex, cmd.nesLengthIndex,
                                          cmd.nesLengthHalt);
                    }
                    break;

                case AudioCommandType::MUSIC_PLAY: {
                    activeTrackCount = 1;
                    tracks[0] = cmd.track;
                    currentNoteIndices[0] = 0;

                    tickDurationSamples =
                        (uint64_t)((float)sampleRate * 60.0f / (tempoBPM * (float)TICKS_PER_BEAT));

                    // Anchor sequencer to audio-thread "now" so we do not treat
                    // elapsed time since boot as a backlog of ticks (would fire
                    // MAX_NOTES_PER_FRAME notes in one block and voice-steal).
                    const uint64_t startTick =
                        (tickDurationSamples > 0) ? (audioTimeSamples / tickDurationSamples) : 0;
                    globalTickCounter = startTick;
                    nextNoteTicks[0] = startTick;
                    musicPlayStartTick_.store(startTick, std::memory_order_release);
                    musicGlobalTick_.store(startTick, std::memory_order_release);

                    for (size_t i = 0;
                         i < cmd.subTrackCount && activeTrackCount < MAX_MUSIC_TRACKS;
                         ++i) {
                        if (cmd.subTracks[i]) {
                            tracks[activeTrackCount] = cmd.subTracks[i];
                            currentNoteIndices[activeTrackCount] = 0;
                            nextNoteTicks[activeTrackCount] = startTick;
                            activeTrackCount++;
                        }
                    }

                    firstSequencerCallAfterPlay_ = true;
                    clearMusicTrackVoiceState();
                    musicPlayingFlag.store(true, std::memory_order_release);
                    musicPausedFlag.store(false, std::memory_order_release);
                    break;
                }

                case AudioCommandType::MUSIC_UPDATE_TRACKS: {
                    if (!musicPlayingFlag.load(std::memory_order_acquire)) {
                        break;
                    }

                    activeTrackCount = 1;
                    tracks[0] = cmd.track;

                    for (size_t i = 0;
                         i < cmd.subTrackCount && activeTrackCount < MAX_MUSIC_TRACKS;
                         ++i) {
                        if (cmd.subTracks[i]) {
                            tracks[activeTrackCount] = cmd.subTracks[i];
                            activeTrackCount++;
                        }
                    }

                    const uint64_t target_tick =
                        (tickDurationSamples > 0)
                            ? (audioTimeSamples / tickDurationSamples)
                            : globalTickCounter;
                    for (size_t track_idx = 0; track_idx < activeTrackCount;
                         ++track_idx) {
                        resyncTrackSequencerToTick(track_idx, target_tick);
                    }

                    clearMusicTrackVoiceState();
                    firstSequencerCallAfterPlay_ = false;
                    break;
                }

                case AudioCommandType::MUSIC_STOP:
                    for (size_t i = 0; i < MAX_MUSIC_TRACKS; ++i) {
                        tracks[i] = nullptr;
                        currentNoteIndices[i] = 0;
                        nextNoteTicks[i] = 0;
                    }
                    activeTrackCount = 0;
                    clearMusicTrackVoiceState();
                    musicPlayingFlag.store(false, std::memory_order_release);
                    musicGlobalTick_.store(0, std::memory_order_release);
                    musicPlayStartTick_.store(0, std::memory_order_release);
                    break;

                case AudioCommandType::MUSIC_PAUSE:
                    musicPausedFlag.store(true, std::memory_order_release);
                    break;

                case AudioCommandType::MUSIC_RESUME:
                    musicPausedFlag.store(false, std::memory_order_release);
                    break;

                case AudioCommandType::MUSIC_SET_TEMPO:
                    tempoFactor = std::max(0.1f, cmd.tempoFactor);
                    break;

                case AudioCommandType::MUSIC_SET_BPM:
                    tempoBPM = std::max(30.0f, std::min(300.0f, cmd.bpm));
                    tickDurationSamples =
                        (uint64_t)((float)sampleRate * 60.0f / (tempoBPM * (float)TICKS_PER_BEAT));
                    break;

                case AudioCommandType::MUSIC_SEEK: {
                    if (!musicPlayingFlag.load(std::memory_order_acquire)) {
                        break;
                    }

                    uint64_t target_tick =
                        (tickDurationSamples > 0)
                            ? (audioTimeSamples / tickDurationSamples)
                            : globalTickCounter;
                    const uint64_t seek_ticks = cmd.seekOffsetTicks;

                    // Want elapsed = seek_ticks, i.e. target - start == seek_ticks.
                    // When "now" is still before seek_ticks (just after play), advance
                    // the clock so start=0 and current=seek_ticks.
                    if (target_tick < seek_ticks) {
                        musicPlayStartTick_.store(0, std::memory_order_release);
                        target_tick = seek_ticks;
                        if (tickDurationSamples > 0) {
                            audioTimeSamples = seek_ticks * tickDurationSamples;
                        }
                    } else {
                        musicPlayStartTick_.store(target_tick - seek_ticks,
                                                  std::memory_order_release);
                    }
                    musicGlobalTick_.store(target_tick, std::memory_order_release);
                    globalTickCounter = target_tick;

                    for (size_t track_idx = 0; track_idx < activeTrackCount;
                         ++track_idx) {
                        resyncTrackSequencerToTick(track_idx, target_tick);
                        beginReleaseOnMusicTrack(track_idx);
                    }
                    clearMusicTrackVoiceState();
                    firstSequencerCallAfterPlay_ = false;
                    break;
                }

                default:
                    break;
            }
        }
    }

    // ------------------------------------------------------------------
    // Music sequencer (NES-style tick sync)
    // ------------------------------------------------------------------
    void ApuCore::resyncTrackSequencerToTick(size_t track_index, uint64_t target_tick)
    {
        if (track_index >= MAX_MUSIC_TRACKS) {
            return;
        }

        const MusicTrack* track = tracks[track_index];
        if (track == nullptr || track->count == 0) {
            currentNoteIndices[track_index] = 0;
            nextNoteTicks[track_index] = target_tick;
            return;
        }

        auto note_ticks_for = [this](const MusicNote& note) -> uint64_t {
            // duration == 0 means "fire now, do not advance" (stacked drum hits).
            return (uint64_t)(note.duration * (float)TICKS_PER_BEAT / tempoFactor);
        };

        uint64_t loop_ticks = 0;
        for (size_t i = 0; i < track->count; ++i) {
            loop_ticks += note_ticks_for(track->notes[i]);
        }
        if (loop_ticks == 0) {
            loop_ticks = 1;
        }

        const uint64_t start_tick =
            musicPlayStartTick_.load(std::memory_order_acquire);
        uint64_t loop_index = 0;
        uint64_t pos_in_loop = 0;
        if (target_tick >= start_tick) {
            const uint64_t elapsed = target_tick - start_tick;
            if (track->loop) {
                loop_index = elapsed / loop_ticks;
                pos_in_loop = elapsed % loop_ticks;
            } else {
                pos_in_loop = elapsed;
                if (pos_in_loop >= loop_ticks) {
                    tracks[track_index] = nullptr;
                    currentNoteIndices[track_index] = track->count;
                    nextNoteTicks[track_index] = start_tick + loop_ticks;
                    return;
                }
            }
        }

        const uint64_t abs_cycle_base = start_tick + loop_index * loop_ticks;
        uint64_t rel_tick = 0;
        size_t note_idx = 0;

        for (int guard = 0; guard < 100000; ++guard) {
            if (note_idx >= track->count) {
                if (!track->loop) {
                    tracks[track_index] = nullptr;
                    currentNoteIndices[track_index] = note_idx;
                    nextNoteTicks[track_index] = abs_cycle_base + rel_tick;
                    return;
                }
                note_idx = 0;
                rel_tick = 0;
            }

            const uint64_t note_ticks = note_ticks_for(track->notes[note_idx]);
            // Zero-duration stacked hits: land on the first one at this tick.
            if (note_ticks == 0) {
                if (rel_tick == pos_in_loop) {
                    currentNoteIndices[track_index] = note_idx;
                    nextNoteTicks[track_index] = abs_cycle_base + rel_tick;
                    return;
                }
                ++note_idx;
                continue;
            }

            if (rel_tick + note_ticks > pos_in_loop) {
                if (pos_in_loop > rel_tick) {
                    size_t next_idx = note_idx + 1;
                    if (next_idx >= track->count) {
                        if (track->loop) {
                            next_idx = 0;
                        } else {
                            tracks[track_index] = nullptr;
                            currentNoteIndices[track_index] = next_idx;
                            nextNoteTicks[track_index] =
                                abs_cycle_base + rel_tick + note_ticks;
                            return;
                        }
                    }
                    currentNoteIndices[track_index] = next_idx;
                    nextNoteTicks[track_index] =
                        abs_cycle_base + rel_tick + note_ticks;
                } else {
                    currentNoteIndices[track_index] = note_idx;
                    nextNoteTicks[track_index] = abs_cycle_base + rel_tick;
                }
                return;
            }

            rel_tick += note_ticks;
            ++note_idx;
        }

        currentNoteIndices[track_index] = std::min(note_idx, track->count);
        nextNoteTicks[track_index] = abs_cycle_base + rel_tick;
    }

    void ApuCore::updateMusicSequencer() {
        if (!musicPlayingFlag.load(std::memory_order_acquire)
            || musicPausedFlag.load(std::memory_order_acquire)
            || activeTrackCount == 0
            || tickDurationSamples == 0) {
            return;
        }

        uint64_t currentTick = audioTimeSamples / tickDurationSamples;
        if (currentTick <= globalTickCounter && globalTickCounter > 0
            && !firstSequencerCallAfterPlay_) {
            return;
        }
        firstSequencerCallAfterPlay_ = false;
        globalTickCounter = currentTick;
        musicGlobalTick_.store(globalTickCounter, std::memory_order_release);

        const size_t limit = sequencerNoteLimit.load(std::memory_order_acquire);
        size_t notesProcessedThisFrame = 0;
        size_t frameDeferredNotes = 0;

        for (size_t trackIdx = 0; trackIdx < activeTrackCount; ++trackIdx) {
            const MusicTrack* track = tracks[trackIdx];
            if (!track) continue;

            size_t& noteIdx = currentNoteIndices[trackIdx];
            uint64_t& nextTick = nextNoteTicks[trackIdx];

            while (musicPlayingFlag.load(std::memory_order_acquire)
                   && globalTickCounter >= nextTick) {
                if (noteIdx >= track->count) {
                    break;
                }
                const MusicNote& note = track->notes[noteIdx];
                // duration == 0 => fire without advancing (stacked drum hits).
                uint64_t noteTicks =
                    (uint64_t)(note.duration * (float)TICKS_PER_BEAT / tempoFactor);
                // tempoFactor > 1 must not truncate short notes to 0 ticks: that stalls
                // nextTick and can spin the per-track while-loop indefinitely.
                if (note.duration > 0.0f && noteTicks == 0) {
                    noteTicks = 1;
                }

                // Check note limit per frame - bounded processing.
                // Zero-tick stacked hits always process so same-step drums stay together.
                if (notesProcessedThisFrame >= limit && noteTicks > 0) {
                    frameDeferredNotes++;
                    nextTick += noteTicks;
                    noteIdx++;
                    if (noteIdx >= track->count) {
                        if (track->loop) {
                            noteIdx = 0;
                        } else {
                            tracks[trackIdx] = nullptr;
                            break;
                        }
                    }
                    continue;
                }

                // Percussion hits use Note::Rest + noise preset. Plain rests are silence.
                const bool isPercussionHit =
                    track->channelType == WaveType::NOISE && note.preset &&
                    note.preset->duty == 0.0f;
                const bool shouldPlay =
                    (note.note != Note::Rest) || isPercussionHit;

                if (shouldPlay) {
                    AudioEvent event{};
                    event.type = track->channelType;

                    if (isPercussionHit) {
                        event.frequency = instrumentToFrequency(*note.preset, note.note, note.octave);
                        event.duration = (note.preset->defaultDuration > 0.0f)
                            ? note.preset->defaultDuration / tempoFactor
                            : std::max(note.duration, 0.05f) / tempoFactor;
                        event.noisePeriod = note.preset->noisePeriod;
                        event.preset = note.preset;
                    } else {
                        event.frequency = noteToFrequency(note.note, note.octave);
                        event.duration = note.duration / tempoFactor;
                        event.noisePeriod = 0;
                        event.preset = note.preset;
                    }

                    event.volume = note.volume;
                    event.duty = (event.type == WaveType::PULSE) ? track->duty : 0.5f;
                    if (note.preset != nullptr &&
                        note.preset->pitchSweepEndHz > 0.0f &&
                        note.preset->pitchSweepDurationSec > 0.0f &&
                        (event.type == WaveType::PULSE ||
                         event.type == WaveType::TRIANGLE)) {
                        event.sweepEndHz = note.preset->pitchSweepEndHz;
                        event.sweepDurationSec = note.preset->pitchSweepDurationSec;
                    }
                    if (isPercussionHit) {
                        playSequencerPercussionHit(event);
                    } else {
                        Voice* voice = musicVoiceForTrack(trackIdx);
                        if (voice != nullptr) {
                            const uint64_t gate_samples = noteTicks * tickDurationSamples;
                            initVoiceFromEvent(
                                voice, event,
                                gate_samples > 0 ? gate_samples : 1,
                                true);
                            track_voice_active_[trackIdx] = true;
                        }
                    }
                } else if (note.note == Note::Rest && !isPercussionHit) {
                    if (track_voice_active_[trackIdx]) {
                        beginReleaseOnMusicTrack(trackIdx);
                    }
                }

                nextTick += noteTicks;

                noteIdx++;
                if (noteIdx >= track->count) {
                    if (track->loop) {
                        noteIdx = 0;
                    } else {
                        tracks[trackIdx] = nullptr;
                        break;
                    }
                }
                // Count only tempo-advancing notes toward the per-frame budget.
                if (noteTicks > 0) {
                    notesProcessedThisFrame++;
                }
            }
        }

// Update deferred notes counter for diagnostics
        if (frameDeferredNotes > 0) {
            deferredNotes.store(frameDeferredNotes, std::memory_order_relaxed);
#if defined(PIXELROOT32_DEBUG_MODE)
            apuLogf(ApuLogLevel::Info, "[APU] deferred %u notes (limit=%u)", (unsigned)frameDeferredNotes, (unsigned)limit);
#endif
        } else {
            deferredNotes.store(0, std::memory_order_relaxed);
        }

        // If every track has finished and none loops, stop music so that
        // MusicPlayer::isPlaying() reports the real state.
        bool anyActive = false;
        for (size_t trackIdx = 0; trackIdx < activeTrackCount; ++trackIdx) {
            const MusicTrack* track = tracks[trackIdx];
            if (track && (track->loop || currentNoteIndices[trackIdx] < track->count)) {
                anyActive = true;
                break;
            }
        }
        if (!anyActive) {
            musicPlayingFlag.store(false, std::memory_order_release);
        }
    }

    // ------------------------------------------------------------------
    // Play event (retrigger a voice)
    // ------------------------------------------------------------------
    void ApuCore::beginReleaseOnMusicTrack(size_t track_index) {
        if (track_index >= MAX_MUSIC_TRACKS) {
            return;
        }

        track_voice_active_[track_index] = false;
        Voice& ch = voices[MUSIC_VOICE_BASE + static_cast<int>(track_index)];
        if (!ch.enabled) {
            return;
        }
        if (ch.envelope.stage == EnvelopeState::Stage::RELEASE
            || ch.envelope.stage == EnvelopeState::Stage::OFF) {
            return;
        }
        if (ch.envelope.releaseSamples > 0) {
            ch.envelope.stage = EnvelopeState::Stage::RELEASE;
            ch.envelope.sampleCounter = 0;
            // S2: the exponential clock is shared by both stages, so rewind it
            // when the release starts; otherwise the decay's leftover phase
            // would pull the first release step up to one step interval early.
            ch.envelopeCurvePhaseQ15 = 0;
            ch.remainingSamples = ch.envelope.releaseSamples;
        } else {
            ch.enabled = false;
            ch.envelope.stage = EnvelopeState::Stage::OFF;
            ch.remainingSamples = 0;
        }
    }

    void ApuCore::initVoiceFromEvent(Voice* ch,
                                     const AudioEvent& event,
                                     uint64_t gate_samples,
                                     bool music_sequencer_legato) {
        if (ch == nullptr) {
            return;
        }

        const bool legato = music_sequencer_legato && ch->enabled
            && ch->envelope.stage != EnvelopeState::Stage::OFF
            && ch->envelope.currentLevel > 0.05f;
        const float legato_level = legato ? ch->envelope.currentLevel : 0.0f;

        const VoiceType voiceType = toVoiceType(event.type);
        // Compatibility fallback required by migration plan.
        ch->type = toWaveType(voiceType);
        ch->enabled = true;
        ch->frequency = event.frequency;
        ch->phaseIncrement = event.frequency / (float)sampleRate;

        if (!legato) {
            ch->phase = 0.0f;
            ch->phaseQ32 = 0u;
        }
        if (sampleRate > 0) {
            const double inc = (double)event.frequency * 4294967296.0 / (double)sampleRate;
            ch->phaseIncQ32 = (inc < 0.0) ? 0u
                            : (inc >= 4294967295.0) ? 0xFFFFFFFFu
                            : (uint32_t)inc;
        } else {
            ch->phaseIncQ32 = 0u;
        }
        ch->basePhaseIncQ32 = ch->phaseIncQ32;

        // ADSR envelope initialization from preset (or legacy defaults).
        // Default values (attack=2ms, decay=0, sustain=1.0, release=5ms)
        // produce identical output to the old anti-click ramp.
        float attackTime   = 0.002f;
        float decayTime    = 0.0f;
        float sustainLevel = 1.0f;
        float releaseTime  = 0.005f;
        if (event.preset) {
            attackTime   = event.preset->attackTime;
            decayTime    = event.preset->decayTime;
            sustainLevel = event.preset->sustainLevel;
            releaseTime  = event.preset->releaseTime;
        }

        auto& env = ch->envelope;
        env.attackSamples  = (uint32_t)std::max(1.0f, attackTime  * (float)sampleRate);
        env.decaySamples   = (uint32_t)(decayTime   * (float)sampleRate);
        env.sustainLevel   = sustainLevel;
        // Clamp release to <= sampleRate/10 (100 ms max) per Decision D2
        env.releaseSamples = std::min((uint32_t)(releaseTime * (float)sampleRate),
                                     (uint32_t)(sampleRate / 10));
        env.sampleCounter  = 0;
        if (legato) {
            env.currentLevel = std::max(legato_level, sustainLevel);
            env.stage        = EnvelopeState::Stage::SUSTAIN;
        } else {
            env.currentLevel = 0.0f;
            env.stage        = EnvelopeState::Stage::ATTACK;
        }

        ch->volume = event.volume;  // base volume (preset level)
        ch->targetVolume = event.volume;
        ch->volumeDelta = 0.0f;     // no longer used for anti-click

        env.attackDelta  = 1.0f / (float)env.attackSamples;
        env.decayDelta   = (env.decaySamples > 0) ? (1.0f - sustainLevel) / (float)env.decaySamples : 0.0f;
        env.releaseDelta = (env.releaseSamples > 0) ? sustainLevel / (float)env.releaseSamples : 0.0f;

        // Initialize Q15 envelope fields. Unconditional: the integer core is
        // compiled in every build (S2), so its envelope mirrors must always be
        // in sync with the float fields above. On the float path they are
        // simply unused.
        env.currentLevelQ15 = (int32_t)(env.currentLevel * 32768.0f);
        env.sustainLevelQ15 = (int32_t)(sustainLevel * 32768.0f);
        env.attackDeltaQ15 = (env.attackSamples > 0) ? (32768 / (int32_t)env.attackSamples) : 32768;
        env.decayDeltaQ15 = (env.decaySamples > 0) ? ((32768 - env.sustainLevelQ15) / (int32_t)env.decaySamples) : 0;
        env.releaseDeltaQ15 = (env.releaseSamples > 0) ? (env.sustainLevelQ15 / (int32_t)env.releaseSamples) : 0;

        // S2 / D-3 tier 3: the note's own request, unresolved. `ProfileDefault`
        // means "the profile decides" and is resolved by `applyProfileCharacter`,
        // which runs after the `nesAccurate` block below; storing the request
        // (not a resolved value) is what keeps the profile a default instead of
        // an override.
        ch->envelopeCurve = event.preset ? event.preset->envelopeCurve
                                         : EnvelopeCurve::ProfileDefault;

        // LFO initialization
        ch->lfo.enabled = false;
        if (event.preset && event.preset->lfoTarget != LfoTarget::NONE && event.preset->lfoFrequency > 0.0f) {
            ch->lfo.enabled = true;
            ch->lfo.target = event.preset->lfoTarget;
            ch->lfo.depth = event.preset->lfoDepth;
            ch->lfo.periodSamples = (uint32_t)((float)sampleRate / event.preset->lfoFrequency);
            if (ch->lfo.periodSamples < 1u) ch->lfo.periodSamples = 1u;
            ch->lfo.sampleCounter = 0;
            ch->lfo.currentValue = 0.0f;
            ch->lfo.delaySamples = (uint16_t)(event.preset->lfoDelay * (float)sampleRate);
            ch->lfo.delayCounter = 0;
            // Convert float depth to Q15 for no-FPU path
            ch->lfo.depthQ15 = (int32_t)(ch->lfo.depth * 32768.0f);
            ch->lfo.currentValueQ15 = 0;
        }

        ch->loop = false;
        if (gate_samples > 0) {
            ch->remainingSamples = gate_samples;
            // Extend every melodic gate by the release tail so the note overlaps the
            // next beat/rest instead of hard-cutting at the beat boundary. The
            // per-sample RELEASE auto-arm still runs afterwards, so an un-retriggered
            // note holds one release length at sustain, then decays.
            if (env.releaseSamples > 0) {
                ch->remainingSamples += env.releaseSamples;
            }
        } else if (event.loop) {
            ch->loop = true;
            ch->remainingSamples = std::numeric_limits<uint64_t>::max();
        } else {
            ch->remainingSamples =
                (uint64_t)(event.duration * (float)sampleRate);
            if (ch->remainingSamples == 0) {
                // One-shot with duration==0 must not hang the voice.
                ch->enabled = false;
                ch->loop = false;
                ch->sweepSamplesTotal = 0;
                ch->sweepSamplesRemaining = 0;
                return;
            }
        }

        if (event.type == WaveType::PULSE) {
            set_pulse_duty(*ch, event.duty);

            // Duty stepped (count > 0) supersedes continuous dutySweep.
            const bool useDutySteps =
                event.dutySteps != nullptr && event.dutyStepCount > 0;
            if (event.preset && !useDutySteps) {
                ch->dutySweep = event.preset->dutySweep / (float)sampleRate;
                ch->dutySweepQ32 = (int32_t)((double)event.preset->dutySweep
                                             * 4294967296.0 / (double)sampleRate);
            } else {
                ch->dutySweep = 0.0f;
                ch->dutySweepQ32 = 0;
            }
            init_duty_stepped(*ch, event, sampleRate);
        } else if (event.type == WaveType::NOISE) {
            uint32_t period;
            float noiseHz = event.frequency;
            if (event.noisePeriod > 0) {
                period = event.noisePeriod;
                if (sampleRate > 0) {
                    noiseHz = (float)sampleRate / (float)period;
                }
            } else {
                if (noiseHz < 1.0f) noiseHz = 1000.0f;
                period = noise_clock_hz_to_period(noiseHz, sampleRate);
            }
            // Hito 3 M8: an armed NOISE LUT index wins over both the direct
            // `noisePeriod` and the frequency-derived period. Like M2's duty
            // mode this is a per-slot setting, so it survives new notes.
            if (ch->nesNoiseLutIndex < 16u) {
                period = nes_apu::nesNoisePeriodToSamples(ch->nesNoiseLutIndex,
                                                          sampleRate);
            }
            ch->frequency = noiseHz;
            ch->noisePeriodSamples = period;
            ch->noiseCountdown = 1u;
            // Hito 1 M9: NES-equivalent seed (all 1s in the 15-bit LFSR) —
            // matches the state after a write to register $400F.
            ch->lfsrState = 0x7FFF;
            const bool lfsrShort = (event.preset) ? event.preset->noiseLfsrShort : false;
            ch->noiseLfsrShort = lfsrShort;
            ch->noiseShortMode = lfsrShort; // keep deprecated alias in sync
            ch->phase = 0.0f;
            ch->phaseIncrement = 0.0f;
            ch->dutySweep = 0.0f;
            ch->dutySweepQ32 = 0;
            ch->dutySteps = nullptr;
            ch->dutyStepCount = 0;
            ch->dutyStepIndex = 0;
            ch->dutyNextBoundarySamples = 0xFFFFFFFFu;
            ch->automationAgeSamples = 0;
        } else if (event.type == WaveType::SINE || event.type == WaveType::SAW) {
            ch->dutyCycle = 0.5f;
            ch->dutySweep = 0.0f;
            ch->dutySweepQ32 = 0;
            ch->dutySteps = nullptr;
            ch->dutyStepCount = 0;
            ch->dutyStepIndex = 0;
            ch->dutyNextBoundarySamples = 0xFFFFFFFFu;
            ch->automationAgeSamples = 0;
        } else {
            // TRIANGLE (and any future non-pulse): no duty automation.
            ch->dutySweep = 0.0f;
            ch->dutySweepQ32 = 0;
            ch->dutySteps = nullptr;
            ch->dutyStepCount = 0;
            ch->dutyStepIndex = 0;
            ch->dutyNextBoundarySamples = 0xFFFFFFFFu;
            ch->automationAgeSamples = 0;
        }

        // Hito 1 M16: NES TRIANGLE is one octave below PULSE for the same
        // period. When the application opts in, double the effective
        // frequency and rebuild both the float and Q32 phase increments so
        // the inner-loop arithmetic sees the doubled rate immediately.
        if (event.type == WaveType::TRIANGLE && ch->triangleOctaveUp) {
            ch->frequency *= 2.0f;
            ch->phaseIncrement = ch->frequency / (float)sampleRate;
            if (sampleRate > 0) {
                const double inc = (double)ch->frequency * 4294967296.0 / (double)sampleRate;
                ch->phaseIncQ32 = (inc < 0.0) ? 0u
                                : (inc >= 4294967295.0) ? 0xFFFFFFFFu
                                : (uint32_t)inc;
            } else {
                ch->phaseIncQ32 = 0u;
            }
            ch->basePhaseIncQ32 = ch->phaseIncQ32;
        }

        init_pitch_envelope(*ch, event, sampleRate);

        ch->sweepSamplesTotal = 0;
        ch->sweepSamplesRemaining = 0;
        // Pitch envelope (>=2 points) supersedes single-segment sweep.
        // Do not clear sweepLog*/sweepStartHz — init_pitch_envelope owns them as segment state.
        if (!pitch_envelope_active(*ch)) {
            ch->sweepCurve = SweepCurve::Linear;
            ch->sweepLogRatio = 0.0f;
            ch->sweepLogStartQ16 = 0;
            ch->sweepLogDeltaQ16 = 0;
        }
        if (!pitch_envelope_active(*ch)
            && (event.type == WaveType::PULSE || event.type == WaveType::TRIANGLE
                || event.type == WaveType::SINE || event.type == WaveType::SAW
                || event.type == WaveType::NOISE)
            && event.sweepDurationSec > 0.0f && event.sweepEndHz > 0.0f && sampleRate > 0) {
            uint64_t sweepLen = (uint64_t)(event.sweepDurationSec * (float)sampleRate);
            if (sweepLen == 0) sweepLen = 1;
            if (!ch->loop) {
                const uint64_t noteLen = ch->remainingSamples;
                if (noteLen == 0) {
                    sweepLen = 0;
                } else if (sweepLen > noteLen) {
                    sweepLen = noteLen;
                }
            }
            if (sweepLen > 0xFFFFFFFFu) {
                sweepLen = 0xFFFFFFFFu;
            }
            if (sweepLen >= 1) {
                ch->sweepSamplesTotal = (uint32_t)sweepLen;
                ch->sweepSamplesRemaining = ch->sweepSamplesTotal;
                ch->sweepStartHz = ch->frequency;
                ch->sweepEndHz = event.sweepEndHz;
                if (event.type == WaveType::NOISE) {
                    ch->sweepStartIncQ32 = ch->noisePeriodSamples;
                    ch->sweepEndIncQ32 =
                        noise_clock_hz_to_period(event.sweepEndHz, sampleRate);
                } else {
                    ch->sweepStartIncQ32 =
                        frequency_hz_to_phase_inc_q32(ch->sweepStartHz, sampleRate);
                    ch->sweepEndIncQ32 =
                        frequency_hz_to_phase_inc_q32(event.sweepEndHz, sampleRate);
                }
                const bool use_exp = (event.sweepCurve == SweepCurve::Exponential
                    && ch->sweepStartHz > 0.0f && event.sweepEndHz > 0.0f);
                if (use_exp) {
                    ch->sweepCurve = SweepCurve::Exponential;
                    ch->sweepLogRatio = std::log(event.sweepEndHz / ch->sweepStartHz);
                    if (event.type == WaveType::NOISE) {
                        ch->sweepLogStartQ16 = log2_q16_from_float(ch->sweepStartHz);
                        ch->sweepLogDeltaQ16 =
                            log2_q16_from_float(event.sweepEndHz) - ch->sweepLogStartQ16;
                    } else {
                        ch->sweepLogStartQ16 = log2_q16_from_u32(ch->sweepStartIncQ32);
                        ch->sweepLogDeltaQ16 =
                            log2_q16_from_u32(ch->sweepEndIncQ32) - ch->sweepLogStartQ16;
                    }
                } else {
                    ch->sweepCurve = SweepCurve::Linear;
                }
            }
        }

        // Hito 2 M17: last, so no earlier branch can leave stale NES state
        // behind. Off unless the preset opts in, which keeps the default
        // render path and the canonical golden PCM untouched.
        if (event.preset && event.preset->nesAccurate) {
            initNesSafe(*ch, event.type);
        }

        // S5: the profile writes character last of all, after `initNesSafe`,
        // so `nesAccurate` keeps meaning "start from a canonical clean state"
        // without erasing the profile's choice. This is the only call site —
        // one application per note-on.
        applyProfileCharacter(static_cast<int>(ch - voices));
    }

    // S5: the ONLY place a profile writes voice character.
    //
    // Character is the envelope curve, the mix weighting, the output filters,
    // the default duty, the noise LUT, unipolar output and pan/echo enablement;
    // it belongs to the profile and wins over the per-voice opt-ins while the
    // note's own ADSR/LFO/sweep/pitch-envelope values keep winning inside their
    // own domain.
    //
    // The mix weighting is character too, but it is not voice state: the mix
    // stage resolves `MixerCurve` from `profile_` once per block (S4), so it
    // has no per-voice field to write here and cannot be half-applied.
    //
    // E0 left this body empty because `Nes` and `Hybrid` were exact aliases.
    // E1 gives it its real assignments: the envelope curve (S2), the noise
    // character (S5) and the stepped PULSE duty (S6).
    void ApuCore::applyProfileCharacter(int slot) {
        Voice& ch = voices[slot];

        // S2 / D-3: the profile supplies the DEFAULT curve only. A note that
        // named one keeps it — the note owns the field inside its own domain
        // and the profile is the fallback. This is also why `nesAccurate`
        // cannot erase the character: `initNesSafe` ran above and does not
        // touch this field, so resolving it here still wins.
        if (ch.envelopeCurve == EnvelopeCurve::ProfileDefault) {
            ch.envelopeCurve = profileDefaultEnvelopeCurve(profile_);
        }

        // S2: derive the exponential clock from the stage durations. This is the
        // first point where the curve is resolved, and it runs once per note-on,
        // never per sample. `Legacy` answers `Linear` and leaves the clock at 0,
        // so its render arithmetic is untouched.
        if (ch.envelopeCurve == EnvelopeCurve::Exponential) {
            // DECAY: the attack always lands on full scale, so the distance to
            // travel is the excursion above the sustain level.
            const int32_t decaySpan =
                (ch.envelope.sustainLevelQ15 < kEnvelopeFullScaleQ15)
                    ? (kEnvelopeFullScaleQ15 - ch.envelope.sustainLevelQ15)
                    : 0;
            ch.decayCurveRateQ15 = exponentialCurveRateQ15(
                decaySpan, ch.envelope.decaySamples);
            // RELEASE: the stage targets silence. It normally starts at the
            // sustain level, but a note shorter than its own decay releases from
            // wherever the decay had reached — and a sustain of 0 would then
            // describe a span of 0, i.e. a release that cannot move and snaps.
            // Full scale is the conservative span for that case: it can only
            // make the release end at or before its duration, never after.
            const int32_t releaseSpan = (ch.envelope.sustainLevelQ15 > 0)
                ? ch.envelope.sustainLevelQ15
                : kEnvelopeFullScaleQ15;
            ch.releaseCurveRateQ15 = exponentialCurveRateQ15(
                releaseSpan, ch.envelope.releaseSamples);
        } else {
            ch.decayCurveRateQ15 = 0;
            ch.releaseCurveRateQ15 = 0;
        }
        ch.envelopeCurvePhaseQ15 = 0;

        // S5 / D-3 tiers 1-2: the profile arms the unipolar NOISE gate and the
        // NES period LUT. Both are resolved here — once per note-on, after the
        // note and after `initNesSafe` — because the profile owns the DEFAULT
        // only: a slot whose caller pinned either value keeps it across notes
        // instead of being re-armed. `Legacy` answers "off, derive the period
        // from the note", which is exactly what the note path already left
        // behind, so its render arithmetic is untouched.
        //
        // Arming the LUT is a tier-2 owner of the NOISE period, so a note that
        // asked for an explicit `noisePeriod` (or a frequency-derived clock)
        // loses to it — the M8 precedence documented on `nesNoiseLutIndex`.
        // `setVoiceNesNoiseLutMode(slot, 255)` is the documented way out.
        if (!ch.noiseUnipolarOverridden) {
            ch.noiseUnipolar = profileDefaultNoiseUnipolar(profile_);
        }
        if (!ch.nesNoiseLutOverridden) {
            const uint8_t lutIndex = profileDefaultNoiseLutIndex(profile_);
            if (ch.nesNoiseLutIndex != lutIndex) {
                ch.nesNoiseLutIndex = lutIndex;
                // The note computed its own period before this point, so the
                // selector has to be applied now: otherwise the first note
                // after a profile change would render the previous period.
                retuneNoisePeriodFromSelector(ch, sampleRate);
            }
        }

        // S6 / D-3 tiers 1-3: PULSE duty. `Nes`/`Hybrid` arm the NES step
        // nearest the note's own `duty`; `Legacy` keeps the continuous
        // comparison. The writer is PULSE-only by construction — the index is
        // read by the pulse generator alone, but resolving it for the other
        // waveforms would still be wrong twice over: it would hand a NOISE or
        // TRIANGLE voice a mode it can only inherit into a later PULSE note on
        // the same slot, and it would make "the profile never touches the other
        // waveforms" untrue to read.
        if (ch.type == WaveType::PULSE && !ch.pulseDutyOverridden) {
            // Tier 3, inside the duty domain: a note that ANIMATES its duty
            // (a `dutySteps` table or a `dutySweep`) cannot be expressed by one
            // static NES pattern — the renderer ignores `dutyCycleQ32` while an
            // index is armed — so those notes keep the continuous generator.
            // That is the note winning in its own domain (D-3), not an
            // exception to it.
            const bool animatedDuty = (ch.dutyStepCount > 0) ||
                                      (ch.dutySweep != 0.0f) ||
                                      (ch.dutySweepQ32 != 0);
            ch.pulseDutyIndex = animatedDuty
                ? 255u
                : profileDefaultPulseDutyIndex(profile_, ch.dutyCycle);
        }

#if defined(UNIT_TEST)
        // Structural proof of the ordering above: a test observes exactly one
        // application per note-on, and only after the nesAccurate block ran.
        ++profileApplyCount_;
#endif
    }

    void ApuCore::playSequencerPercussionHit(const AudioEvent& event) {
        Voice* ch = findVoiceForPercussionHit();
        if (ch == nullptr) {
            return;
        }
        initVoiceFromEvent(ch, event, 0);
    }

    void ApuCore::executePlayEvent(const AudioEvent& event, uint64_t gate_samples) {
        Voice* ch = findVoiceForSfxEvent(event.type);
        if (!ch) return;
        initVoiceFromEvent(ch, event, gate_samples);
    }

    Voice* ApuCore::findVoiceForSfxEvent(WaveType type) {
        Voice* bestInactive = nullptr;
        Voice* bestSteal = nullptr;
        uint64_t minScore = std::numeric_limits<uint64_t>::max();

        for (int i = SFX_VOICE_BASE; i < MAX_VOICES; ++i) {
            Voice& voice = voices[i];
            if (!voice.enabled) {
                if (voice.type == type) return &voice;
                if (!bestInactive) bestInactive = &voice;
                continue;
            }
            const uint64_t score = voice_steal_score(voice);
            if (score < minScore) {
                minScore = score;
                bestSteal = &voice;
            }
        }

        if (bestInactive) return bestInactive;
        return bestSteal;
    }

    Voice* ApuCore::findVoiceForPercussionHit() {
        Voice* bestInactive = nullptr;
        Voice* bestSteal = nullptr;
        uint64_t minScore = std::numeric_limits<uint64_t>::max();

        for (int i = SFX_VOICE_BASE; i < MAX_VOICES; ++i) {
            Voice& voice = voices[i];
            if (!voice.enabled) {
                if (!bestInactive) bestInactive = &voice;
                continue;
            }
            const uint64_t score = voice_steal_score(voice);
            if (score < minScore) {
                minScore = score;
                bestSteal = &voice;
            }
        }

        if (bestInactive) return bestInactive;

        // SFX/percussion subpool is saturated: borrow an idle music slot
        // before stealing a live SFX/percussion voice. A slot only counts as
        // idle when it is both disabled and not holding a live melodic gate,
        // so a live melodic note is never stolen; music always reclaims its
        // fixed slot on the next note-on via musicVoiceForTrack().
        for (int i = MUSIC_VOICE_BASE; i < SFX_VOICE_BASE; ++i) {
            if (!voices[i].enabled && !track_voice_active_[i]) return &voices[i];
        }

        return bestSteal;
    }

    // ------------------------------------------------------------------
    // ADSR Envelope state machine (per-sample tick)
    // ------------------------------------------------------------------
    static inline void tickEnvelope(AudioChannel& ch) {
        auto& env = ch.envelope;
        switch (env.stage) {
            case EnvelopeState::Stage::ATTACK:
                env.sampleCounter++;
                // env.currentLevel = (float)env.sampleCounter / (float)env.attackSamples;
                env.currentLevel += env.attackDelta;
                if (env.sampleCounter >= env.attackSamples) {
                    env.currentLevel = 1.0f;
                    env.sampleCounter = 0;
                    // Skip decay if decaySamples == 0
                    if (env.decaySamples > 0) {
                        env.stage = EnvelopeState::Stage::DECAY;
                    } else {
                        env.currentLevel = env.sustainLevel;
                        env.stage = EnvelopeState::Stage::SUSTAIN;
                    }
                }
                break;

            case EnvelopeState::Stage::DECAY:
                env.sampleCounter++;
                // Linear ramp from 1.0 down to sustainLevel
                // env.currentLevel = 1.0f - (1.0f - env.sustainLevel)
                //     * ((float)env.sampleCounter / (float)env.decaySamples);
                env.currentLevel -= env.decayDelta;
                if (env.sampleCounter >= env.decaySamples) {
                    env.currentLevel = env.sustainLevel;
                    env.sampleCounter = 0;
                    env.stage = EnvelopeState::Stage::SUSTAIN;
                }
                break;

            case EnvelopeState::Stage::SUSTAIN:
                // Hold at sustainLevel — nothing to tick
                break;

            case EnvelopeState::Stage::RELEASE:
                env.sampleCounter++;
                // env.currentLevel = env.sustainLevel
                //     * (1.0f - (float)env.sampleCounter / (float)env.releaseSamples);
                env.currentLevel -= env.releaseDelta;
                if (env.sampleCounter >= env.releaseSamples || env.currentLevel <= 0.0f) {
                    env.currentLevel = 0.0f;
                    env.stage = EnvelopeState::Stage::OFF;
                    ch.enabled = false;
                }
                break;

            case EnvelopeState::Stage::OFF:
                break;
        }
    }

    /**
     * @brief One step of the S-DSP-style exponential envelope decrement (S2/P1).
     *
     * `level -= ((level - 1) >> 8) + 1`, the shape the audit's P1 specifies,
     * applied to the distance between the current level and the stage's end
     * target. Acting on the excursion rather than on the raw level is what lets
     * a decay finish AT its sustain level (a release, whose target is 0, is
     * therefore bit-for-bit the plain recurrence): the stage boundary is then a
     * −60 dB step instead of the jump back to sustain.
     *
     * Above the last 8 bits the recurrence is a first-order decay with a time
     * constant of 256 samples; the `+1` term is what keeps it moving below that,
     * where a plain `level -= level >> 8` would park at 1 forever.
     *
     * The expression needs no explicit clamp: at offset 0 it evaluates to
     * `0 - ((-1 >> 8) + 1) == 0`, so the offset cannot go negative — the
     * "floored at 0" clause of the spec is intrinsic.
     *
     * @param levelQ15 Level in Q15; decremented in place.
     * @param floorQ15 Value the stage ends on (sustain for DECAY, 0 for RELEASE).
     */
    static inline void applyExponentialEnvelopeStepQ15(int32_t& levelQ15, int32_t floorQ15) {
        int32_t offset = levelQ15 - floorQ15;
        if (offset <= 0) {
            levelQ15 = floorQ15;
            return;
        }
        offset -= ((offset - 1) >> 8) + 1;
        if (offset < 0) {
            offset = 0;
        }
        levelQ15 = floorQ15 + offset;
    }

    /**
     * @brief Runs the exponential clock for one sample and applies a step when due.
     *
     * The clock is a Q15 phase accumulator: it advances by `rateQ15` per sample
     * and performs one shape step each time it crosses 1.0, so the shape keeps
     * its exact arithmetic while its RATE follows the stage duration. Chosen
     * over a stride counter because the fractional part makes the rate exact for
     * any duration (a stride of `stageSamples / steps` would quantise long
     * stages by up to one full step) and because `phase + rate < 2 * 32768`
     * cannot overflow a `uint16_t` while the rate is capped at one step per
     * sample.
     *
     * @param ch Voice whose clock and level are advanced.
     * @param floorQ15 Value the stage ends on.
     * @param rateQ15 Shape steps per sample in Q15 (1..32768).
     */
    static inline void tickExponentialEnvelopeQ15(AudioChannel& ch, int32_t floorQ15,
                                                  uint16_t rateQ15) {
        const uint32_t phase = (uint32_t)ch.envelopeCurvePhaseQ15 + (uint32_t)rateQ15;
        if (phase < kCurveFullRateQ15) {
            ch.envelopeCurvePhaseQ15 = (uint16_t)phase;
            return;
        }
        ch.envelopeCurvePhaseQ15 = (uint16_t)(phase - kCurveFullRateQ15);
        applyExponentialEnvelopeStepQ15(ch.envelope.currentLevelQ15, floorQ15);
    }

    // ------------------------------------------------------------------
    // ADSR Envelope state machine (fixed-point Q15, portable integer core)
    // ------------------------------------------------------------------
    // Compiled unconditionally: the integer core exists in every build (S2),
    // so this tick must never be hidden behind an FPU guard again.
    //
    // S2: the curve applies to DECAY and RELEASE only. ATTACK stays linear on
    // purpose — a linear ramp is predictable and curving it buys nothing — and
    // `Linear` keeps the frozen `Legacy` arithmetic untouched, branch included.
    //
    // The exponential branches on the stage's clock rate: a rate of 0 means the
    // stage is shorter than the shape's span and deliberately falls back to the
    // same linear delta (see `exponentialCurveRateQ15`).
    static inline void tickEnvelopeQ15(AudioChannel& ch) {
        auto& env = ch.envelope;
        const bool exponential = (ch.envelopeCurve == EnvelopeCurve::Exponential);
        switch (env.stage) {
            case EnvelopeState::Stage::ATTACK:
                env.sampleCounter++;
                env.currentLevelQ15 += env.attackDeltaQ15;
                if (env.sampleCounter >= env.attackSamples) {
                    env.currentLevelQ15 = 32768; // 1.0 in Q15
                    env.sampleCounter = 0;
                    if (env.decaySamples > 0) {
                        env.stage = EnvelopeState::Stage::DECAY;
                    } else {
                        env.currentLevelQ15 = env.sustainLevelQ15;
                        env.stage = EnvelopeState::Stage::SUSTAIN;
                    }
                }
                break;
            case EnvelopeState::Stage::DECAY:
                env.sampleCounter++;
                if (exponential && ch.decayCurveRateQ15 != 0) {
                    tickExponentialEnvelopeQ15(ch, env.sustainLevelQ15,
                                               ch.decayCurveRateQ15);
                } else {
                    env.currentLevelQ15 -= env.decayDeltaQ15;
                }
                if (env.sampleCounter >= env.decaySamples) {
                    env.currentLevelQ15 = env.sustainLevelQ15;
                    env.sampleCounter = 0;
                    env.stage = EnvelopeState::Stage::SUSTAIN;
                }
                break;
            case EnvelopeState::Stage::SUSTAIN:
                break;
            case EnvelopeState::Stage::RELEASE:
                env.sampleCounter++;
                if (exponential && ch.releaseCurveRateQ15 != 0) {
                    tickExponentialEnvelopeQ15(ch, 0, ch.releaseCurveRateQ15);
                } else {
                    env.currentLevelQ15 -= env.releaseDeltaQ15;
                }
                if (env.sampleCounter >= env.releaseSamples || env.currentLevelQ15 <= 0) {
                    env.currentLevelQ15 = 0;
                    env.stage = EnvelopeState::Stage::OFF;
                    ch.enabled = false;
                }
                break;
            case EnvelopeState::Stage::OFF:
                env.currentLevelQ15 = 0;
                break;
        }
    }

    // ------------------------------------------------------------------
    // LFO oscillator tick (triangle wave, per-sample)
    // ------------------------------------------------------------------
    static inline void tickLfo(AudioChannel& ch) {
        auto& lfo = ch.lfo;
        if (!lfo.enabled || lfo.periodSamples == 0) return;

        if (lfo.delayCounter < lfo.delaySamples) {
            lfo.delayCounter++;
            return;
        }

        lfo.sampleCounter++;
        if (lfo.sampleCounter >= lfo.periodSamples) lfo.sampleCounter = 0;

        // Triangle wave in [-1.0, +1.0]
        const float t = (float)lfo.sampleCounter / (float)lfo.periodSamples;
        lfo.currentValue = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
    }

    // ------------------------------------------------------------------
    // LFO oscillator tick (Q15 integer-only, triangle wave)
    // ------------------------------------------------------------------
    static inline void tickLfoQ15(LfoState& lfo) {
        if (!lfo.enabled || lfo.periodSamples == 0) return;

        if (lfo.delayCounter < lfo.delaySamples) {
            lfo.delayCounter++;
            return;
        }

        lfo.sampleCounter++;
        if (lfo.sampleCounter >= lfo.periodSamples) lfo.sampleCounter = 0;

        // Integer triangle wave in Q15
        // tQ15 = (sampleCounter * 32768) / periodSamples (range: 0 to 32768)
        int32_t tQ15 = (int32_t)((uint64_t)lfo.sampleCounter * 32768u / lfo.periodSamples);

        // Triangle: if t < 0.5: 4*t - 1, else: 3 - 4*t
        // In Q15: if tQ15 < 16384: 4*tQ15 - 32768, else: 98304 - 4*tQ15
        if (tQ15 < 16384) {
            lfo.currentValueQ15 = (tQ15 << 2) - 32768;  // 4*tQ15 - 32768
        } else {
            lfo.currentValueQ15 = 98304 - (tQ15 << 2);  // 98304 - 4*tQ15
        }
    }

    // ------------------------------------------------------------------
    // Per-channel sample generation
    // ------------------------------------------------------------------
    float ApuCore::generateSampleForVoice(Voice& ch) {
        if (!ch.enabled) return 0.0f;

        tick_voice_automation(ch, sampleRate, /*q15Path=*/false);
        if (!pitch_envelope_active(ch)) {
            apply_frequency_sweep_float(ch, sampleRate);
        }

        float sample = 0.0f;
        switch (ch.type) {
            case WaveType::PULSE: {
                if (ch.pulseDutyIndex < 4) {
                    // Hito 1 M2: NES discrete duty (12.5/25/50/75%).
                    // phaseQ32 top 3 bits = current 8-step index (matches Q15 path).
                    const uint32_t step = (ch.phaseQ32 >> 29) & 7u;
                    sample = kNesPulseDuty[ch.pulseDutyIndex][step] ? 1.0f : -1.0f;
                } else {
                    sample = (ch.phase < ch.dutyCycle) ? 1.0f : -1.0f;
                }
                // Hito 2 M6: NES sweep muting. The `muted` flag is
                // continuously evaluated by the half-clock dispatch
                // (and by `setVoiceNesTimer`), even when the sweep is
                // "disabled" (E=0 or SSS=0). When set, the PULSE output
                // is forced to 0 (the phase still advances below).
                if (ch.nesSweepUnit.muted) {
                    sample = 0.0f;
                }
                break;
            }
            case WaveType::TRIANGLE: {
                // Hito 2 M5: per-sample NES gate. The TRIANGLE channel is
                // ANDed by (length counter open) AND (linear counter open).
                // Either sub-unit, when its respective `enabled` flag is
                // false, is treated as always-open. When the gate is
                // closed the output is 0 and the phase does NOT advance —
                // re-loading via a $4008 / $400B write resumes the voice.
                const bool lengthOk =
                    (!ch.nesLengthCounter.enabled) || (ch.nesLengthCounter.counter > 0);
                const bool linearOk =
                    (!ch.nesLinearCounter.linearEnabled) || (ch.nesLinearCounter.counter > 0);
                const bool gate = lengthOk && linearOk;

                if (gate) {
                    float tri = (ch.phase < 0.5f)
                              ? (4.0f * ch.phase - 1.0f)
                              : (3.0f - 4.0f * ch.phase);
                    if (ch.triangleQuantize4Bit) {
                        // Hito 1 M1: NES 4-bit quantisation.
                        // 32 levels = 16 positive + 16 negative, step = 2/15, range
                        // [-1, +1]. The duty counter's 5-bit value 0..15 maps to
                        // 2*counter/15 with the counter not reaching "0" (apex
                        // duplicates at phase 0 and 0.5). We snap to the nearest
                        // of {-1, -13/15, -11/15, ..., 11/15, 13/15, 1}.
                        const float step = 2.0f / 15.0f;
                        const int idx = (int)std::floor((tri + 1.0f) / step + 0.5f);
                        const int clamped = (idx < 0) ? 0 : (idx > 15 ? 15 : idx);
                        tri = ((float)clamped * step) - 1.0f;
                    }
                    sample = tri;
                    ch.phase += ch.phaseIncrement;
                    if (ch.phase >= 1.0f) ch.phase -= 1.0f;
                    ch.phaseQ32 += ch.phaseIncQ32;
                } else {
                    // Gate closed: silent, no phase advance. Voice stays
                    // enabled so it can resume when a counter reloads.
                    sample = 0.0f;
                }
                break;
            }
            case WaveType::SINE: {
                const unsigned i = (unsigned)(ch.phase * 256.0f) & 255u;
                sample = (float)SINE_LUT_Q15[i] / 32768.0f;
                break;
            }
            case WaveType::SAW:
                sample = 2.0f * ch.phase - 1.0f;
                break;
            case WaveType::NOISE: {
                // Unified 15-bit NES LFSR shared by all platforms so the
                // timbre is identical in simulator and hardware.
                if (ch.noiseCountdown > 0u) ch.noiseCountdown--;
                if (ch.noiseCountdown == 0u) {
                    const uint16_t bitToXor = ch.noiseLfsrShort ? ((ch.lfsrState >> 6) & 1u) : ((ch.lfsrState >> 1) & 1u);
                    const uint16_t fb = (uint16_t)(((ch.lfsrState & 1u) ^ bitToXor) & 1u);
                    ch.lfsrState = (uint16_t)((ch.lfsrState >> 1) | (fb << 14));
                    ch.noiseCountdown = ch.noisePeriodSamples;
                }
                sample = (ch.lfsrState & 1u) ? 1.0f : -1.0f;
                break;
            }
            default:
                sample = 0.0f;
                break;
        }

        if (ch.type != WaveType::NOISE && ch.type != WaveType::TRIANGLE) {
            ch.phase += ch.phaseIncrement;
            if (ch.phase >= 1.0f) ch.phase -= 1.0f;
            // Hito 1 M2: keep phaseQ32 in sync with the float phase on the
            // FPU path so the NES discrete duty lookup (which uses the
            // top 3 bits of phaseQ32) tracks the same step as the Q15 path.
            ch.phaseQ32 += ch.phaseIncQ32;
            if (ch.type == WaveType::PULSE && ch.dutySweep != 0.0f) {
                ch.dutyCycle += ch.dutySweep;
                if (ch.dutyCycle > 1.0f) ch.dutyCycle -= 1.0f;
                else if (ch.dutyCycle < 0.0f) ch.dutyCycle += 1.0f;
            }
        }

        // Tick the ADSR envelope
        tickEnvelope(ch);

        // Tick the LFO and apply modulation
        tickLfo(ch);
        float lfoVolMod = 1.0f;
        if (ch.lfo.enabled) {
            if (ch.lfo.target == LfoTarget::PITCH) {
                // Vibrato: modulate phase increment (frequency)
                ch.phaseIncrement = ch.frequency / (float)sampleRate
                    * (1.0f + ch.lfo.currentValue * ch.lfo.depth);
            } else if (ch.lfo.target == LfoTarget::VOLUME) {
                // Tremolo: modulate output amplitude
                lfoVolMod = 1.0f - ch.lfo.depth * 0.5f * (1.0f - ch.lfo.currentValue);
            }
        }

        // Trigger RELEASE when remaining duration expires (Decision D2:
        // release samples are added to remainingSamples at transition).
        // Looping voices never auto-expire.
        if (!ch.loop && ch.remainingSamples > 0) {
            ch.remainingSamples--;
            if (ch.remainingSamples == 0) {
                if (ch.envelope.stage != EnvelopeState::Stage::RELEASE
                    && ch.envelope.stage != EnvelopeState::Stage::OFF
                    && ch.envelope.releaseSamples > 0) {
                    // Enter release and extend channel lifetime
                    ch.envelope.stage = EnvelopeState::Stage::RELEASE;
                    ch.envelope.sampleCounter = 0;
                    ch.remainingSamples = ch.envelope.releaseSamples;
                } else {
                    ch.enabled = false;
                }
            }
        }

        // Hito 2 M7: when the NES envelope is enabled for this voice
        // (PULSE / NOISE only), the envelope's `output` (0..15) replaces
        // the ADSR `envelope.currentLevel` as the per-sample volume
        // source. The NES envelope's `output` is the const-volume level
        // (VVVV) or the current decay level, both 0..15. Scaling by
        // 1/15.0 maps output=15 to 1.0 (output=0 → 0.0). Matches NES.
        float envLevel;
        if ((ch.type == WaveType::PULSE || ch.type == WaveType::NOISE)
            && ch.nesEnvelope.envelopeEnabled) {
            envLevel = (float)ch.nesEnvelope.output / 15.0f;
        } else {
            envLevel = ch.envelope.currentLevel;
        }
        return sample * ch.volume * envLevel * lfoVolMod;
    }

    // ------------------------------------------------------------------
    // Main mixing path
    // ------------------------------------------------------------------
    void ApuCore::generateSamples(int16_t* stream, int length) {
        if (!stream || length <= 0) return;

        if (!commandQueue.isEmpty()) processCommands();
        updateMusicSequencer();

        // S3: one core per block, chosen once from the profile. The branch is
        // deliberately outside the sample loop — a per-sample test would cost
        // more than the body it selects, and the profile cannot change inside
        // a block anyway (`setSoundProfile` runs on the audio thread).
#if PR32_APU_ENABLE_LEGACY_FLOAT
        if (profileUsesIntegerCore()) {
            renderIntegerCore(stream, length);
        } else {
            renderFloatLegacy(stream, length);
        }
#else
        // No float body in this build, so `Legacy` falls back to the integer
        // core (S3) and every profile lands on the same destination.
        renderIntegerCore(stream, length);
#endif

        if (postMixMono_) {
            postMixMono_(stream, length, postMixUser_);
        }

        audioTimeSamples += (uint64_t)length;
        samplesSinceLog += (uint64_t)length;

        if constexpr (kApuProfilingEnabled) {
            if (samplesSinceLog >= (uint64_t)sampleRate) {
                uint8_t idx = profileWriteIdx.fetch_add(1, std::memory_order_relaxed);
                idx %= PROFILE_RING_SIZE;
                profileRing[idx].audioTimeSamples = audioTimeSamples;
                profileRing[idx].peak = currentPeak;
                profileRing[idx].clipped = (currentPeak >= 32767.0f);
                if (profileCount < PROFILE_RING_SIZE) {
                    profileCount++;
                }
                currentPeak = 0.0f;
                samplesSinceLog = 0;
            }
        } else if (samplesSinceLog >= (uint64_t)sampleRate) {
            currentPeak = 0.0f;
            samplesSinceLog = 0;
        }
    }

#if PR32_APU_ENABLE_LEGACY_FLOAT
    // S1: the frozen `Legacy` float body. Only the mechanical extraction that
    // gave it a name is allowed to touch it — no sound fix, no optimization.
    // `generateSamples` owns prologue, dispatch and the post-mix hook.
    void ApuCore::renderFloatLegacy(int16_t* stream, int length) {
        constexpr float FINAL_SCALE = 32767.0f;
        // HPF is `y[n] = x[n] - x[n-1] + R*y[n-1]`; R now lives in `hpfR_`
        // (Hito 4 M10) so the corner can be configured in Hz.

        // ---- Float path (native, ESP32 classic, ESP32-S3) ---------------
        // Compiled when PR32_APU_ENABLE_LEGACY_FLOAT is set (defaults to
        // PR32_APU_HAS_FPU). This is the frozen `Legacy` render body: no new
        // functions, optimizations or sound fixes may land here. The
        // integer/LUT branch below is the portable core and is compiled
        // unconditionally, so an FPU build with the macro at 0 still links.

        // Hito 2 M3: APU cycles per render sample. Computed once per render
        // block. When the frame counter is OFF (default) the tick is a no-op,
        // so this is just a cheap set-up cost.
        const double apuCyclesPerSample =
            (sampleRate > 0) ? (nes_apu::kNesApuHzNtsc / (double)sampleRate) : 0.0;

        for (int i = 0; i < length; ++i) {
            // Hito 2 M3: tick the NES frame counter before voice mixing so
            // any future sub-units (M4-M7) see a consistent clock state.
            tickNesFrameCounter(apuCyclesPerSample);
            float acc = 0.0f;
            for (int c = 0; c < MAX_VOICES; ++c) {
                if (voices[c].enabled) {
                    acc += generateSampleForVoice(voices[c]) * MIXER_SCALE;
                }
            }
            acc *= masterVolume;
            // Hito 4 M11: master shaping curve, applied before the DC
            // blocker so the filter sees whatever the curve produced.
            float mixed;
            switch (softClipMode_) {
                case SoftClipMode::None:
                    mixed = acc;
                    break;
                case SoftClipMode::Tanh: {
                    // Rational tanh approximation: x*(27+x^2)/(27+9x^2).
                    const float x2 = acc * acc;
                    mixed = acc * (27.0f + x2) / (27.0f + 9.0f * x2);
                    break;
                }
                case SoftClipMode::HardClip:
                    // The NES DAC saturates rather than compressing.
                    mixed = (acc > 1.0f) ? 1.0f : ((acc < -1.0f) ? -1.0f : acc);
                    break;
                case SoftClipMode::Rational:
                default:
                    mixed = acc / (1.0f + std::fabs(acc) * MIXER_K);
                    break;
            }

            // DC-blocker: removes duty-asymmetry DC and softens retrigger pops.
            // Hito 4 M10: coefficient comes from the configured corner.
            float hpfOut;
            if (hpfEnabled_) {
                hpfOut = mixed - hpfPrevIn + hpfR_ * hpfPrevOut;
                hpfPrevIn = mixed;
                hpfPrevOut = hpfOut;
            } else {
                hpfOut = mixed;
                // Keep the state coherent so re-enabling starts the
                // recursion cleanly instead of stepping from a stale value.
                hpfPrevIn = mixed;
                hpfPrevOut = 0.0f;
            }

            float finalSample = hpfOut * FINAL_SCALE;
            const float absSample = std::fabs(finalSample);
            if (absSample > currentPeak) currentPeak = absSample;
            if (finalSample > 32767.0f) finalSample = 32767.0f;
            if (finalSample < -32768.0f) finalSample = -32768.0f;
            stream[i] = apply_master_bitcrush((int16_t)finalSample, masterBitcrushBits_);
        }
    }
#endif

    // ---- Integer / LUT path (portable core: no-FPU targets, and FPU
    // targets built with -DPR32_APU_ENABLE_LEGACY_FLOAT=0) --------------
    // Unconditional body: it is the single core every platform can run
    // (S2/S3). Uses the Q32 phase mirror + Q15 output per channel so the inner
    // loop contains zero soft-float operations. The LUT is pre-fitted
    // to the same compression curve as the FPU path, so both paths
    // produce numerically equivalent output.
    //
    // OPTIMIZATION: Hoisted switch from inner loop. Instead of switch(ch.type) per
    // sample (causes branch misprediction at 22kHz), we process by wave type
    // using separate accumulators. This eliminates ~4 branches per sample.
    //
    // Volume is still stored in float; we convert to Q15 once per
    // block (not per sample) so volume ramps still work but the 22kHz
    // inner loop stays integer-only.
    void ApuCore::renderIntegerCore(int16_t* stream, int length) {
        // envQ15 is now tracked continuously per-channel via tickEnvelopeQ15
        // Hito 2 M3: APU cycles per render sample (same as FPU path).
        const double apuCyclesPerSample =
            (sampleRate > 0) ? (nes_apu::kNesApuHzNtsc / (double)sampleRate) : 0.0;

        // Hoisted wave type dispatch - compute sample once before inner loop
        // using inline branch-free operations instead of switch
        auto generatePulseSampleQ15 = [](AudioChannel& ch) -> int32_t {
            if (ch.pulseDutyIndex < 4) {
                const uint32_t step = (ch.phaseQ32 >> 29) & 7u;
                return kNesPulseDuty[ch.pulseDutyIndex][step] ? 32767 : -32767;
            }
            return (ch.phaseQ32 < ch.dutyCycleQ32) ? 32767 : -32767;
        };
        auto generateTriangleSampleQ15 = [](AudioChannel& ch) -> int32_t {
            const uint32_t p16 = ch.phaseQ32 >> 16;
            int32_t tri_q15 = (p16 < 32768u)
                ? ((int32_t)(p16 << 1) - 32768)
                : (32768 - (int32_t)((p16 - 32768u) << 1));
            if (ch.triangleQuantize4Bit) {
                // NES 4-bit quantisation (same as the file-scope lambda).
                constexpr int32_t kStep = (2 * 32768) / 15;
                constexpr int32_t kHalf = kStep / 2;
                int32_t shifted = tri_q15 + 32768 + kHalf;
                int32_t idx = shifted / kStep;
                if (idx < 0) idx = 0;
                if (idx > 15) idx = 15;
                tri_q15 = idx * kStep - 32768;
            }
            return tri_q15;
        };
        auto generateNoiseSampleQ15 = [](AudioChannel& ch) -> int32_t {
            if (ch.noiseUnipolar) {
                // S5/P9: the NES noise channel switches between silence and the
                // envelope level, never to -level. Only the SOURCE changes: the
                // level itself is still the volume x envelope multiply that runs
                // below for every voice, and the AC-level compensation is the
                // x2 folded into this voice's per-block gain (`mixGainQ15`).
                return (ch.lfsrState & 1u) ? 32767 : 0;
            }
            return (ch.lfsrState & 1u) ? 32767 : -32767;
        };

        // Process channels by type - hoisted from inner loop
        // This eliminates the switch(ch.type) branch misprediction
        //
        // S4: the mixer curve is a property of the mix, so it is resolved from
        // the profile ONCE per block and never inside the sample loop. The
        // group a voice feeds depends only on its waveform, and `type` can only
        // change in `initVoiceFromEvent` — which runs from `processCommands`
        // and `updateMusicSequencer`, both before this loop — so the group is
        // resolved per block too.
        const bool nesWeighted =
            (profileMixerCurve(profile_) == MixerCurve::NesWeighted);

        // E2/S4: the mipmap read is `Hybrid` character, resolved once per block
        // like the mixer curve rather than tested against `profile_` inside the
        // sample loop. `Legacy` and `Nes` keep `generateSawSampleQ15`, the ideal
        // ramp; `Nes` keeps its aliasing on purpose (P4).
        const bool sawWavetable = (profile_ == SoundProfile::Hybrid);

        // S4/S5: everything the mix does to a voice that is CONSTANT for the
        // whole block is folded into one Q15 gain here, once per voice, instead
        // of being applied once per voice per sample:
        //
        //   - the NES group weight (`>> kNesMixWeightShift`), which used to be
        //     applied to the summed groups after the voice loop; and
        //   - the unipolar x2 compensation, which used to be a `sv << 1` on
        //     every sample of a gated NOISE voice.
        //
        // The mix is the same because both factors are scalars distributed over
        // a sum: sum_c (w * v_c) == w * sum_c v_c. Only the truncation moves —
        // from after the sum to before each voice's multiply — so the weighted
        // sum can differ by < 1 LSB per voice from what the old path produced.
        // That is below the resolution of the mix, but not always below the
        // resolution of the OUTPUT: the `Rational` curve indexes its LUT in
        // 256-LSB steps of `sum`, so a sample sitting on a step boundary can
        // move by that LUT's own step (~100 LSB down where its slope is
        // steepest) instead of by 1. Measured against the previous tree:
        // `Legacy` byte-identical, and `Nes`/`Hybrid` peak and RMS within
        // 0.002 % with a worst single-sample delta of 101 LSB on the two scenes
        // that mix a TRIANGLE voice (the pulse weight is Q12 unity, so the
        // committed goldens, a pulse-only track, are byte-identical).
        //
        // No new overflow: `volume` is documented 0..1 and the fold keeps every
        // intermediate inside int32 over that range — the worst case is a gated
        // NOISE voice at full volume, 32767 * (2691 / 4096) * 2 = 43046, i.e.
        // 1.41e9 of the 2.1e9 an int32 holds.
        //
        // What it buys is the whole group-bucket machinery: three zeroed
        // accumulators per SAMPLE, a bucket index per voice and three int64
        // multiplies per sample become one multiply per voice that the loop was
        // already doing. Those zeroed buckets were paid by every profile,
        // `Legacy` included, even though its weights are the identity.
        //
        // `volQ15` stays the plain Q15 volume: the sync-back at the end of the
        // block writes it into `voices[c].volume`, so the volume ramp and the
        // FPU fallback keep seeing a plain volume. `mixGainQ15` is the folded
        // one and never leaves this function.
        int32_t volQ15[MAX_VOICES];
        int32_t mixGainQ15[MAX_VOICES];
        // E2/T4: the `Hybrid` SAW read state, resolved here once per voice
        // instead of once per sample. Only the SAW branch below reads an entry,
        // and `type` cannot change between this fold and that branch (see the
        // note above about `initVoiceFromEvent`), so the other slots are left
        // alone: writing them would be per-block work for a value nothing reads.
        SawMipBandState sawReadBand[MAX_VOICES];
        for (int c = 0; c < MAX_VOICES; ++c) {
            volQ15[c] = (int32_t)(voices[c].volume * 32768.0f);
            int32_t gain = volQ15[c];
            if (nesWeighted) {
                gain = (gain * nesMixWeightQ12(nesMixGroupOf(voices[c].type)))
                     >> kNesMixWeightShift;
            }
            if (voices[c].noiseUnipolar && voices[c].type == WaveType::NOISE) {
                gain <<= kNoiseUnipolarCompensationShift;
            }
            mixGainQ15[c] = gain;

            if (sawWavetable && voices[c].type == WaveType::SAW) {
                resolveSawMipBandState(voices[c].phaseIncQ32, sawReadBand[c]);
            }
        }

        // E2/S7: the output stage is the only reader and writer of its own
        // state and it runs on every sample, so the whole stage is snapshotted
        // into a local before the loop and written back after it. That is what
        // lets the ten recursion words live where the compiler wants them
        // (registers, or stack slots it chose) instead of being reloaded from
        // `this` twice per sample through a call boundary. Bit-exact by
        // construction: the snapshot is a copy, never a re-derivation, and no
        // line of arithmetic changes — only where the words live.
        OutputStageStateQ15 outStage = captureOutputStageState();

        for (int i = 0; i < length; ++i) {
            // Hito 2 M3: tick the NES frame counter before voice mixing.
            tickNesFrameCounter(apuCyclesPerSample);
            // `Rational` keeps the historical accumulator untouched: one raw
            // sum, with `MIXER_SCALE` applied once by the output stage.
            // `NesWeighted` now accumulates into the SAME sum: the published
            // weights are already inside each voice's `mixGainQ15`, so "weighted
            // by group" no longer needs a bucket per group nor a second pass —
            // and `sum` has one meaning for both curves, which is what the
            // output stage downstream expects.
            int32_t sum = 0;

            for (int c = 0; c < MAX_VOICES; ++c) {
                Voice& ch = voices[c];
                if (!ch.enabled) continue;

                int32_t s = 0; // Q15 sample in [-32767, +32767]

                // Branch-free type dispatch using function pointer lookup
                if (ch.type == WaveType::NOISE) {
                    // NOISE: inline state update (required for LFSR mutation)
                    if (ch.noiseCountdown > 0u) ch.noiseCountdown--;
                    if (ch.noiseCountdown == 0u) {
                        const uint16_t bitToXor = ch.noiseLfsrShort ? ((ch.lfsrState >> 6) & 1u) : ((ch.lfsrState >> 1) & 1u);
                        const uint16_t fb = (uint16_t)(((ch.lfsrState & 1u) ^ bitToXor) & 1u);
                        ch.lfsrState = (uint16_t)((ch.lfsrState >> 1) | (fb << 14));
                        ch.noiseCountdown = ch.noisePeriodSamples;
                    }
                    s = generateNoiseSampleQ15(ch);
                } else if (ch.type == WaveType::SINE) {
                    // SINE: direct LUT lookup
                    s = (int32_t)SINE_LUT_Q15[(ch.phaseQ32 >> 24) & 255u];
                } else if (ch.type == WaveType::TRIANGLE) {
                    // Hito 2 M5: per-sample NES gate. AND of (length open)
                    // and (linear open); each sub-unit, when its `enabled`
                    // flag is false, is treated as always-open. When the
                    // gate is closed the output is 0 and the phase does
                    // NOT advance — re-loading via a $4008 / $400B write
                    // resumes the voice.
                    const bool lengthOk =
                        (!ch.nesLengthCounter.enabled) || (ch.nesLengthCounter.counter > 0);
                    const bool linearOk =
                        (!ch.nesLinearCounter.linearEnabled) || (ch.nesLinearCounter.counter > 0);
                    if (lengthOk && linearOk) {
                        s = generateTriangleSampleQ15(ch);
                        ch.phaseQ32 += ch.phaseIncQ32;  // advance only if gate open
                    } else {
                        s = 0;
                    }
                } else if (ch.type == WaveType::PULSE) {
                    // Hito 2 M6: PULSE special case to apply the sweep
                    // muting flag (always-evaluated by the half-clock
                    // dispatch). SAW + SINE go through the function
                    // pointer dispatch below.
                    s = generatePulseSampleQ15(ch);
                    if (ch.nesSweepUnit.muted) {
                        s = 0;
                    }
                } else if (auto gen = WAVE_GENERATORS_Q15[static_cast<int>(ch.type)]) {
                    // SAW: function pointer lookup. (TRIANGLE handled above,
                    // PULSE handled above, NOISE / SINE handled above.)
                    // E2/S4: `Hybrid` reads the band-limited mipmap instead; the
                    // other profiles reach the same ideal ramp as before.
                    s = sawWavetable ? generateBandLimitedSawSampleQ15(ch, sawReadBand[c])
                                     : gen(ch);
                } else {
                    // Fallback (should never reach)
                    s = 0;
                }

                // Apply per-channel Q15 gain × envelope: (s * gainQ15 * envQ15) >> 30
                // The gain already carries the voice's Q15 volume plus the two
                // block-constant mix factors (NES group weight, unipolar x2), so
                // this is the only per-sample scale left.
                // Hito 2 M7: when the NES envelope is enabled (PULSE / NOISE),
                // use the envelope's `output` (0..15) scaled to Q15 as the
                // multiplier. 32768/15 = 2184, so output=15 → 32760 (≈1.0).
                int32_t envQ15;
                if ((ch.type == WaveType::PULSE || ch.type == WaveType::NOISE)
                    && ch.nesEnvelope.envelopeEnabled) {
                    envQ15 = (int32_t)ch.nesEnvelope.output * 2184;  // 32768/15
                } else {
                    envQ15 = ch.envelope.currentLevelQ15;
                }
                int32_t sv = (s * mixGainQ15[c]) >> 15;
                sv = (sv * envQ15) >> 15;

                // Volume LFO (tremolo) — Q15 only, no float conversion.
                if (ch.lfo.enabled && ch.lfo.target == LfoTarget::VOLUME) {
                    // Tremolo: vol = 1.0 - depth * 0.5 * (1.0 - lfo)
                    // In Q15: volQ15 = 32768 - ((depthQ15 * (32768 - currentValueQ15)) >> 16)
                    int32_t volModQ15 = 32768 - ((ch.lfo.depthQ15 * (32768 - ch.lfo.currentValueQ15)) >> 16);
                    sv = (sv * volModQ15) >> 15;
                }

                // Hito 5 M12: NO per-channel MIXER_SCALE here. The
                // accumulator carries the RAW channel sum (+-131072 for four
                // full-scale voices) because that is the convention every
                // downstream stage was built for: `audio_mixer_lut` bakes the
                // 0.4 into its own mapping, and the computed curves apply it
                // once explicitly. Scaling here as well applied 0.4 twice and
                // left the outer 60% of the table unreachable.
                sum += sv;

                // Duty stepped + pitch envelope (shared age; integer pitch path).
                tick_voice_automation(ch, sampleRate, /*q15Path=*/true);

                // Phase accumulator (integer, wraps automatically).
                // Hito 2 M5: TRIANGLE phase is advanced inside the gate
                // branch above (only when the gate is open), so we must
                // skip it here to avoid double-advancing.
                if (ch.type != WaveType::NOISE && ch.type != WaveType::TRIANGLE) {
                    ch.phaseQ32 += ch.phaseIncQ32;
                    if (ch.type == WaveType::PULSE && ch.dutySweepQ32 != 0) {
                        ch.dutyCycleQ32 += ch.dutySweepQ32; // naturally wraps
                    }
                }

                // ADSR envelope tick (fixed-point Q15, once per sample).
                tickEnvelopeQ15(ch);

                // Single-segment sweep only when pitch envelope is inactive.
                if (!pitch_envelope_active(ch)
                    && ch.sweepSamplesTotal > 0 && ch.sweepSamplesRemaining > 0) {
                    const uint32_t numer = ch.sweepSamplesTotal - ch.sweepSamplesRemaining;
                    if (ch.sweepCurve == SweepCurve::Exponential) {
                        const int32_t logV = ch.sweepLogStartQ16
                            + (int32_t)(((int64_t)ch.sweepLogDeltaQ16 * (int64_t)numer)
                                        / (int64_t)ch.sweepSamplesTotal);
                        if (ch.type == WaveType::NOISE) {
                            uint32_t hz = exp2_q16_to_u32(logV);
                            if (hz < 1u) hz = 1u;
                            uint32_t period = (sampleRate > 0)
                                ? ((uint32_t)sampleRate / hz) : 1u;
                            if (period < 1u) period = 1u;
                            ch.noisePeriodSamples = period;
                        } else {
                            ch.basePhaseIncQ32 = exp2_q16_to_u32(logV);
                        }
                    } else {
                        const int64_t delta =
                            (int64_t)ch.sweepEndIncQ32 - (int64_t)ch.sweepStartIncQ32;
                        int64_t value = (int64_t)ch.sweepStartIncQ32
                            + (delta * (int64_t)numer) / (int64_t)ch.sweepSamplesTotal;
                        if (value < 0) value = 0;
                        if (value > (int64_t)0xFFFFFFFFLL) value = 0xFFFFFFFFLL;
                        if (ch.type == WaveType::NOISE) {
                            uint32_t period = (uint32_t)value;
                            if (period < 1u) period = 1u;
                            ch.noisePeriodSamples = period;
                        } else {
                            ch.basePhaseIncQ32 = (uint32_t)value;
                        }
                    }
                    ch.sweepSamplesRemaining--;
                    if (ch.sweepSamplesRemaining == 0) {
                        if (ch.type == WaveType::NOISE) {
                            uint32_t period = ch.sweepEndIncQ32;
                            if (period < 1u) period = 1u;
                            ch.noisePeriodSamples = period;
                        } else {
                            ch.basePhaseIncQ32 = ch.sweepEndIncQ32;
                        }
                        ch.sweepSamplesTotal = 0;
                    }
                }

                // LFO tick + pitch modulation (Q15-only, no float).
                tickLfoQ15(ch.lfo);
                if (ch.lfo.enabled && ch.lfo.target == LfoTarget::PITCH
                    && ch.type != WaveType::NOISE) {
                    // Vibrato: inc = base * (1 + lfo * depth)
                    // modQ15 = currentValueQ15 * depthQ15 >> 15
                    int32_t modQ15 = (int32_t)((int64_t)ch.lfo.currentValueQ15 * ch.lfo.depthQ15 >> 15);
                    int64_t inc = (int64_t)ch.basePhaseIncQ32 + ((int64_t)ch.basePhaseIncQ32 * modQ15 >> 15);
                    ch.phaseIncQ32 = (inc < 0) ? 0u : (uint32_t)inc;
                } else if (ch.type != WaveType::NOISE) {
                    ch.phaseIncQ32 = ch.basePhaseIncQ32;
                }

                // Duration countdown + release trigger (same as float path).
                if (!ch.loop && ch.remainingSamples > 0) {
                    ch.remainingSamples--;
                    if (ch.remainingSamples == 0) {
                        if (ch.envelope.stage != EnvelopeState::Stage::RELEASE
                            && ch.envelope.stage != EnvelopeState::Stage::OFF
                            && ch.envelope.releaseSamples > 0) {
                            ch.envelope.stage = EnvelopeState::Stage::RELEASE;
                            ch.envelope.sampleCounter = 0;
                            // S2: rewind the shared exponential clock at the
                            // stage change (see beginReleaseOnMusicTrack).
                            ch.envelopeCurvePhaseQ15 = 0;
                            ch.remainingSamples = ch.envelope.releaseSamples;
                        } else {
                            ch.enabled = false;
                        }
                    }
                }
            }

            // S4: the voice loop has already accumulated the WEIGHTED mix, so
            // `sum` reaches the output chain in exactly the same domain and at
            // exactly the same point as the `Rational` curve: before master
            // volume, before the shaping curve, before the DC blocker. That is
            // the "weighted before the HPF" contract — the filter and the
            // compressor see the weighted signal, not a weighted version of
            // their own output — and it is why no collapse pass is needed here.

            // Apply master volume before the compressor curve so the nonlinear
            // stage sees the attenuated mix — same order as the FPU path
            // (acc *= masterVolume happens before the soft-clip there).
            if (masterVolumeScale != 65536) {
                sum = (int32_t)(((int64_t)sum * (int64_t)masterVolumeScale) >> 16);
            }

            // Hito 4 M11: master shaping curve. `Rational` keeps the
            // pre-fitted LUT so the canonical golden PCM is untouched; the
            // other modes are computed from the same accumulated mix.
            //
            // The LUT maps the RAW sum through `32767 * S / (1 + |S|*0.5)`
            // with `S = sum * 1.6 / 131072`, i.e. it applies MIXER_SCALE = 0.4
            // itself (four full-scale channels land at S = 1.6, the float
            // path's post-MIXER_SCALE bound). The computed modes therefore
            // need the same single 0.4 applied by hand: `(sum * 13107) >> 15`.
            int32_t inputQ15;
            if (softClipMode_ == SoftClipMode::Rational) {
                int32_t index = (sum + 131072) >> 8;
                if (index < 0) index = 0;
                if (index > 1024) index = 1024;
                inputQ15 = audio_mixer_lut[index];
            } else {
                const int32_t sQ15 = (int32_t)(((int64_t)sum * 13107) >> 15);
                switch (softClipMode_) {
                    case SoftClipMode::Tanh: {
                        // x*(27+x^2)/(27+9x^2) evaluated in Q15. int64 is
                        // required: |sQ15| reaches ~52428 when four voices
                        // sum at full scale, so x^2 overflows int32.
                        const int64_t x2 = ((int64_t)sQ15 * sQ15) >> 15;
                        const int64_t num = (int64_t)27 * 32768 + x2;
                        const int64_t den = (int64_t)27 * 32768 + 9 * x2;
                        inputQ15 = (int32_t)(((int64_t)sQ15 * num) / den);
                        break;
                    }
                    case SoftClipMode::HardClip:
                        inputQ15 = sQ15;
                        if (inputQ15 > 32767) inputQ15 = 32767;
                        if (inputQ15 < -32768) inputQ15 = -32768;
                        break;
                    case SoftClipMode::None:
                    default:
                        // Unshaped: the final int16 clamp is the only bound,
                        // so the HPF still sees the overshoot (as in float).
                        inputQ15 = sQ15;
                        break;
                }
            }

            // E2/S1: the output stage is profile character. `Nes`/`Hybrid`
            // run the shared three-stage chain (HPF 90 -> HPF 440 -> LPF) on
            // the summed mix; `Legacy` keeps the historical single-pole HPF
            // byte for byte, on every build. Which one runs was resolved on
            // the configuration change, so this is a flag test here — no
            // coefficient is computed per sample. `applyOutputStage` owns the
            // arithmetic, including the truncate-toward-zero reasoning the
            // one-pole depends on; it is forced inline and reads only
            // `outStage`, so the per-sample path carries no call and no
            // `this`-relative state access.
            int32_t hpfOutQ15 = applyOutputStage(inputQ15, outStage);

            // E2 level parity: the profile's fixed trim, after the chain and
            // before the master bitcrush. `chainActive` is block-constant and
            // is false for `Legacy`, so the frozen path never executes this
            // multiply, not even as a no-op. It sits here rather than inside
            // `applyOutputStage` on purpose: the stage's measured response is
            // the cascade itself (S2 pins the transfer function, not the
            // profile's loudness), and the trim is a render-path gain.
            // `trimQ15 * sample` fits int32_t (32767 * 53854 < 2^31, see the
            // header's static_assert), and the saturation below is the clamp
            // this gain needs: it is the only stage that can push a legal
            // chain output back over the int16 rail.
            if (outStage.chainActive) {
                hpfOutQ15 = q15_truncate_from_int32(outStage.trimQ15 * hpfOutQ15);
            }

            // Hito 5 M12: the fixed -6 dB headroom pad (`>>1`, Q15 -> Q14) is
            // gone; this path now lands at the same level as the float one.
            // The pad also happened to keep the value below half scale, so the
            // int16 cast could never overflow. It can now: an unshaped mix
            // reaches S = 1.6 and a bypassed HPF skips the clamp above, so
            // saturate here exactly as the float branch does before its cast.
            int32_t finalSample = hpfOutQ15;
            if (finalSample > 32767) finalSample = 32767;
            if (finalSample < -32768) finalSample = -32768;

            stream[i] = apply_master_bitcrush((int16_t)finalSample, masterBitcrushBits_);

            const int32_t absSample = (stream[i] < 0) ? -(int32_t)stream[i] : (int32_t)stream[i];
            if ((float)absSample > currentPeak) currentPeak = (float)absSample;
        }

        // Persist the stage the loop ended on. Every return path out of the
        // loop is the loop's own exit, so this cannot be skipped.
        restoreOutputStageState(outStage);

        // Sync per-channel float volume back from Q15 so the next block
        // (and the FPU fallback if ever invoked) sees up-to-date state.
        for (int c = 0; c < MAX_VOICES; ++c) {
            voices[c].volume = (float)volQ15[c] / 32768.0f;
        }
    }

    void ApuCore::setPostMixMono(void (*fn)(int16_t* mono, int length, void* user), void* user) {
        postMixMono_ = fn;
        postMixUser_ = user;
    }

    void ApuCore::setVoicePulseDutyMode(int slot, uint8_t nesDutyIndex) {
        if (slot < 0 || slot >= MAX_VOICES) return;
        // 0..3 -> NES discrete patterns. 4..254 are reserved / invalid and
        // are clamped to continuous. 255 is the documented "continuous"
        // sentinel and is stored verbatim.
        if (nesDutyIndex < 4 || nesDutyIndex == 255) {
            voices[slot].pulseDutyIndex = nesDutyIndex;
        } else {
            voices[slot].pulseDutyIndex = 255;
        }
        // S6 / D-3 tier 2: an explicit mode pins the slot against the sound
        // profile's default, so `255` here means "this voice keeps the
        // continuous generator" for good, not only until the next note-on.
        voices[slot].pulseDutyOverridden = true;
    }

    // Hito 3 M8: NES NOISE period LUT.
    void ApuCore::setVoiceNesNoiseLutMode(int slot, uint8_t lutIndex) {
        if (slot < 0 || slot >= MAX_VOICES) return;
        Voice& v = voices[slot];
        // 0..15 -> LUT entry. 16..254 are invalid and fall back to the
        // documented 255 "derive from frequency" sentinel.
        v.nesNoiseLutIndex = (lutIndex < 16u) ? lutIndex : 255u;
        // S5 / D-3 tier 2: an explicit selector pins the slot against the sound
        // profile's default, so `255` here means "this voice derives its period
        // from the note" for good, not only until the next note-on.
        v.nesNoiseLutOverridden = true;

        // Retune now so the change is audible without waiting for a new
        // note, matching how setVoiceNesTimer re-syncs the pulse period.
        retuneNoisePeriodFromSelector(v, sampleRate);
    }

#if defined(UNIT_TEST)
    void ApuCore::setVoiceTriangleOctaveUpForTesting(int slot, bool enabled) {
        if (slot < 0 || slot >= MAX_VOICES) return;
        voices[slot].triangleOctaveUp = enabled;
    }
#endif

    void ApuCore::getAndResetProfileStats(ProfileEntry* out, uint8_t& count) {
        count = profileCount;
        uint8_t startIdx = profileCount > 0 ? (profileWriteIdx.load(std::memory_order_relaxed) - profileCount + PROFILE_RING_SIZE) % PROFILE_RING_SIZE : 0;
        for (uint8_t i = 0; i < count; ++i) {
            out[i] = profileRing[(startIdx + i) % PROFILE_RING_SIZE];
        }
        profileCount = 0;
    }

} // namespace pixelroot32::audio
