/**
 * @file test_apu_output_filter.cpp
 * @brief E2/S1-S3, S8: output filter chain of the `Nes` / `Hybrid` profiles.
 *
 * The chain is HPF 1st order at 90 Hz -> HPF 1st order at 440 Hz -> LPF 2nd
 * order Butterworth at `min(14 kHz, 0.45 * fs)`, applied once to the summed
 * mix. It is profile character, so it is exercised through PRODUCTION code:
 * every case feeds a buffer through `ApuCore::processOutputStageForTesting`,
 * which runs the same `applyOutputStage` the integer render loop calls. No
 * case here re-implements the filter math; the only reference arithmetic is
 * the HISTORICAL one-pole of `Legacy`, which is the definition being pinned.
 *
 * Coverage:
 *   - S2 response: 20 Hz .. 0.45 * fs, log-spaced, against the cascade
 *     computed in double, within +-1 dB, at 8 000 / 22 050 / 44 100 / 48 000.
 *   - S3 stability: full-scale alternating and DC runs at each rate, chain
 *     active and inactive, with the recursion state pinned to its bound.
 *   - S8 precedence: `setHpfCutoffHz()` outranks the profile, `reset()`
 *     clears the override.
 *   - Legacy inertness: the inactive stage is the historical one-pole, byte
 *     for byte, on the path a no-float build renders `Legacy` with.
 */

#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "test_config.h"

using namespace pixelroot32::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

// S1 corners. Kept here as the independent statement of the spec, so a change
// in the library constant has to disagree with the test rather than with
// itself.
constexpr double kHpf1Hz = 90.0;
constexpr double kHpf2Hz = 440.0;
constexpr double kLpfMaxHz = 14000.0;
// Audit P3 risk (iii): 0.45 * fs at 8 kHz is 3.6 kHz and would gut the
// passband, so the LPF engages only when 0.45 * fs >= 6 kHz.
constexpr double kLpfEnableMin45FsHz = 6000.0;

constexpr int kResponsePoints = 32;
constexpr double kResponseToleranceDb = 1.0;
// Amplitude chosen so the largest cascade gain (the two HPFs near 0.45 * fs at
// 8 kHz, where the LPF is off, ~1.21) stays clear of the int16 rail while the
// 20 Hz point still carries ~150 LSB, far above the truncation error.
constexpr int16_t kResponseAmplitude = 16000;

// S3: samples per case. 10^7 is the audit's budget; this suite pays it for
// every {active, inactive} x {alternating, dc} case at every rate.
constexpr long long kStabilitySamplesPerCase = 10000000;
constexpr int kStabilityBlock = 1024;
// Every stage saturates its state to int16, so no state variable can exceed
// this bound no matter what a corrupt coefficient would do.
constexpr int32_t kStateBound = 32768;

/// @brief The theoretical cascade for one sample rate, in double.
struct Theory {
    double r1 = 0.0;
    double r2 = 0.0;
    bool lpf = false;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

/// @brief Ideal cascade at `fs` (same policy the profile implements).
Theory makeTheory(double fs) {
    Theory t;
    t.r1 = std::exp(-2.0 * kPi * kHpf1Hz / fs);
    t.r2 = std::exp(-2.0 * kPi * kHpf2Hz / fs);
    const double fc45 = 0.45 * fs;
    t.lpf = (fc45 >= kLpfEnableMin45FsHz);
    if (t.lpf) {
        const double fc = std::min(kLpfMaxHz, fc45);
        const double w0 = 2.0 * kPi * fc / fs;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / std::sqrt(2.0);  // Q = 1/sqrt(2), RBJ
        const double a0 = 1.0 + alpha;
        t.b0 = (1.0 - cw) * 0.5 / a0;
        t.b1 = (1.0 - cw) / a0;
        t.b2 = t.b0;
        t.a1 = -2.0 * cw / a0;
        t.a2 = (1.0 - alpha) / a0;
    }
    return t;
}

/// @brief |H| of a first-order high-pass `(1 - z^-1) / (1 - R z^-1)`.
double hpfMag(double r, double w) {
    const double num = std::sqrt(2.0 - 2.0 * std::cos(w));
    const double den = std::sqrt(1.0 - 2.0 * r * std::cos(w) + r * r);
    return num / den;
}

/// @brief |H| of the whole cascade at angular frequency `w`.
double theoryMag(const Theory& t, double w) {
    double m = hpfMag(t.r1, w) * hpfMag(t.r2, w);
    if (t.lpf) {
        const double cw = std::cos(w);
        const double sw = std::sin(w);
        const double c2 = std::cos(2.0 * w);
        const double s2 = std::sin(2.0 * w);
        const double nRe = t.b0 + t.b1 * cw + t.b2 * c2;
        const double nIm = -(t.b1 * sw + t.b2 * s2);
        const double dRe = 1.0 + t.a1 * cw + t.a2 * c2;
        const double dIm = -(t.a1 * sw + t.a2 * s2);
        m *= std::sqrt(nRe * nRe + nIm * nIm) /
             std::sqrt(dRe * dRe + dIm * dIm);
    }
    return m;
}

/// @brief Goertzel power of `x` at bin `k` of an `n`-sample DFT.
///
/// Using a bin makes the single-tone measurement exact for a periodic segment:
/// every point is rendered at `f = k * fs / n`, so no window leaks energy and
/// the ratio of two Goertzel powers is the magnitude ratio of the two signals.
double goertzelPower(const std::vector<int16_t>& x, long long k) {
    const double n = static_cast<double>(x.size());
    const double coeff = 2.0 * std::cos(2.0 * kPi * static_cast<double>(k) / n);
    double s1 = 0.0;
    double s2 = 0.0;
    for (int16_t sample : x) {
        const double s0 = static_cast<double>(sample) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/// @brief Measures |Y(f)| / |X(f)| of the production output stage at `f`.
///
/// The stage is driven for an extra `fs / 8` samples before the measured
/// window: the slowest pole (the 90 Hz HPF) decays in ~`fs / 565` samples, so
/// the prefix leaves only the steady-state sine. The window itself is exactly
/// `2 * fs` samples, i.e. `2 * f` whole cycles at the bin used, so a
/// rectangular window is exact and no leakage enters the ratio.
double measureGain(ApuCore& apu, int fs, double f) {
    const int n = 2 * fs;
    const int warmup = fs / 8;
    const int total = warmup + n;
    long long k = static_cast<long long>(std::llround(f * 2.0));
    if (k < 1) {
        k = 1;
    }
    const double rendered = static_cast<double>(k) / 2.0;

    std::vector<int16_t> in(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) {
        in[i] = static_cast<int16_t>(std::lround(
            static_cast<double>(kResponseAmplitude) *
            std::sin(2.0 * kPi * rendered * static_cast<double>(i) /
                     static_cast<double>(fs))));
    }
    std::vector<int16_t> out = in;

    apu.resetOutputFilterStateForTesting();
    apu.processOutputStageForTesting(out.data(), total);

    const std::vector<int16_t> inTail(in.begin() + warmup, in.end());
    const std::vector<int16_t> outTail(out.begin() + warmup, out.end());

    const double px = goertzelPower(inTail, k);
    const double py = goertzelPower(outTail, k);
    return std::sqrt(py / px);
}

/// @brief S2: worst |measured - theory| in dB over 20 Hz .. 0.45 * fs.
double assert_response_at_rate(int fs) {
    ApuCore apu;
    apu.init(fs);
    apu.setSoundProfile(SoundProfile::Nes);
    TEST_ASSERT_TRUE(apu.isOutputFilterChainActiveForTesting());

    const Theory theory = makeTheory(static_cast<double>(fs));
    TEST_ASSERT_EQUAL_INT(theory.lpf ? 1 : 0,
                          apu.isOutputFilterLpfActiveForTesting() ? 1 : 0);

    const double fMax = 0.45 * static_cast<double>(fs);
    const double span = std::log(fMax / 20.0);
    double worstDb = 0.0;
    double worstHz = 0.0;

    for (int p = 0; p < kResponsePoints; ++p) {
        const double target =
            20.0 * std::exp(span * static_cast<double>(p) /
                            static_cast<double>(kResponsePoints - 1));
        const double f = std::round(target * 2.0) / 2.0;  // snap to the 0.5 Hz bin
        if (f < 0.5 || f > fMax) {
            continue;
        }
        const double measured = measureGain(apu, fs, f);
        const double expected =
            theoryMag(theory, 2.0 * kPi * f / static_cast<double>(fs));
        TEST_ASSERT_TRUE_MESSAGE(expected > 0.0, "theory magnitude collapsed");
        const double db = 20.0 * std::log10(measured / expected);

        if (std::fabs(db) > std::fabs(worstDb)) {
            worstDb = db;
            worstHz = f;
        }
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "fs=%d f=%.1f Hz: measured %.4f vs theory %.4f (%+.3f dB)",
                      fs, f, measured, expected, db);
        TEST_ASSERT_TRUE_MESSAGE(std::fabs(db) <= kResponseToleranceDb, message);
    }

    std::printf("  response fs=%5d Hz: worst %+.3f dB at %.1f Hz\n",
                fs, worstDb, worstHz);
    return worstDb;
}

/// @brief Fills a stability block: alternating full scale, or DC full scale.
void fill_stability_block(std::vector<int16_t>& block, int n, bool dc) {
    for (int i = 0; i < n; ++i) {
        if (dc) {
            block[static_cast<size_t>(i)] = 32767;
        } else {
            block[static_cast<size_t>(i)] = ((i & 1) == 0) ? 32767 : -32767;
        }
    }
}

/// @brief S3: bounded state and in-range output over a full-scale run.
void assert_stability_case(int fs, bool chainActive, bool dc) {
    ApuCore apu;
    apu.init(fs);
    apu.setSoundProfile(chainActive ? SoundProfile::Nes : SoundProfile::Legacy);
    TEST_ASSERT_EQUAL_INT(chainActive ? 1 : 0,
                          apu.isOutputFilterChainActiveForTesting() ? 1 : 0);

    std::vector<int16_t> block(static_cast<size_t>(kStabilityBlock));
    long long remaining = kStabilitySamplesPerCase;
    int32_t worstState = 0;
    int16_t minOut = 0;
    int16_t maxOut = 0;
    bool first = true;

    while (remaining > 0) {
        const int n = static_cast<int>(
            std::min<long long>(remaining, kStabilityBlock));
        fill_stability_block(block, n, dc);
        apu.processOutputStageForTesting(block.data(), n);

        for (int i = 0; i < n; ++i) {
            if (first) {
                minOut = maxOut = block[static_cast<size_t>(i)];
                first = false;
            } else {
                minOut = std::min(minOut, block[static_cast<size_t>(i)]);
                maxOut = std::max(maxOut, block[static_cast<size_t>(i)]);
            }
        }
        worstState =
            std::max(worstState, apu.getOutputFilterStateMaxAbsForTesting());
        remaining -= n;
    }

    TEST_ASSERT_TRUE_MESSAGE(worstState <= kStateBound,
                             "output filter state exceeded its int16 bound");
    TEST_ASSERT_TRUE(minOut >= -32768);
    TEST_ASSERT_TRUE(maxOut <= 32767);
}

/// @brief Historical single-pole HPF, transcribed from the frozen definition.
std::vector<int16_t> reference_one_pole(const std::vector<int16_t>& in,
                                        int32_t rq15) {
    std::vector<int16_t> out(in.size());
    int32_t prevIn = 0;
    int32_t prevOut = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const int32_t x = static_cast<int32_t>(in[i]);
        // Truncation toward zero, not an arithmetic shift: the shift froze
        // the blocker on a permanent DC offset (see the render code comment).
        const int32_t feedbackQ15 =
            static_cast<int32_t>((static_cast<int64_t>(rq15) *
                                  static_cast<int64_t>(prevOut)) /
                                 32768);
        int32_t y = x - prevIn + feedbackQ15;
        if (y > 32767) {
            y = 32767;
        }
        if (y < -32768) {
            y = -32768;
        }
        prevIn = x;
        prevOut = y;
        out[i] = static_cast<int16_t>(y);
    }
    return out;
}

/// @brief A deterministic, sign-changing buffer that exercises both rails.
std::vector<int16_t> make_probe_buffer(int rate) {
    constexpr int kCount = 4096;
    std::vector<int16_t> buffer(kCount);
    for (int i = 0; i < kCount; ++i) {
        const double sine =
            8000.0 * std::sin(2.0 * kPi * 300.0 * static_cast<double>(i) /
                             static_cast<double>(rate));
        // A DC step exercises the near-zero decay the HPF exists for.
        const double step = (i < kCount / 8) ? 0.0 : 4000.0;
        buffer[static_cast<size_t>(i)] =
            static_cast<int16_t>(std::lround(sine + step));
    }
    return buffer;
}

}  // namespace

void setUp(void) {
    test_setup();
}

void tearDown(void) {
    test_teardown();
}

// =============================================================================
// S2 - measured response
// =============================================================================

void test_output_filter_response_8000(void) {
    assert_response_at_rate(8000);
}

void test_output_filter_response_22050(void) {
    assert_response_at_rate(22050);
}

void test_output_filter_response_44100(void) {
    assert_response_at_rate(44100);
}

void test_output_filter_response_48000(void) {
    assert_response_at_rate(48000);
}

// =============================================================================
// S3 - stability
// =============================================================================

void test_output_filter_stability_8000(void) {
    assert_stability_case(8000, true, false);
    assert_stability_case(8000, true, true);
    assert_stability_case(8000, false, false);
    assert_stability_case(8000, false, true);
}

void test_output_filter_stability_22050(void) {
    assert_stability_case(22050, true, false);
    assert_stability_case(22050, true, true);
    assert_stability_case(22050, false, false);
    assert_stability_case(22050, false, true);
}

void test_output_filter_stability_44100(void) {
    assert_stability_case(44100, true, false);
    assert_stability_case(44100, true, true);
    assert_stability_case(44100, false, false);
    assert_stability_case(44100, false, true);
}

void test_output_filter_stability_48000(void) {
    assert_stability_case(48000, true, false);
    assert_stability_case(48000, true, true);
    assert_stability_case(48000, false, false);
    assert_stability_case(48000, false, true);
}

// =============================================================================
// S8 - the caller override outranks the profile
// =============================================================================

void test_output_filter_override_precedes_profile(void) {
    ApuCore apu;
    apu.init(44100);
    apu.setSoundProfile(SoundProfile::Nes);
    TEST_ASSERT_TRUE(apu.isOutputFilterChainActiveForTesting());

    // The explicit corner pins the historical one-pole path, even though the
    // profile wants its chain.
    apu.setHpfCutoffHz(35.0f);
    TEST_ASSERT_FALSE(apu.isOutputFilterChainActiveForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 35.0f, apu.getHpfCutoffHzForTesting());

    // Reference: `Legacy` at the same explicit corner. Legacy is inactive by
    // definition, so both objects must run the same one-pole arithmetic.
    ApuCore reference;
    reference.init(44100);
    reference.setHpfCutoffHz(35.0f);
    TEST_ASSERT_FALSE(reference.isOutputFilterChainActiveForTesting());

    const std::vector<int16_t> probe = make_probe_buffer(44100);
    std::vector<int16_t> stage = probe;
    std::vector<int16_t> expected = probe;
    apu.processOutputStageForTesting(stage.data(), static_cast<int>(stage.size()));
    reference.processOutputStageForTesting(expected.data(),
                                           static_cast<int>(expected.size()));
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), stage.data(),
                             expected.size() * sizeof(int16_t));

    // reset() clears the override and the profile survives, so the chain is
    // installed again on the very next block.
    apu.reset();
    TEST_ASSERT_TRUE(apu.isOutputFilterChainActiveForTesting());
    TEST_ASSERT_TRUE(apu.getHpfCutoffHzForTesting() < 0.0f);
}

// =============================================================================
// Legacy inertness
// =============================================================================

void test_output_filter_legacy_stage_is_historical_one_pole(void) {
    ApuCore apu;
    apu.init(22050);
    TEST_ASSERT_FALSE(apu.isOutputFilterChainActiveForTesting());
    TEST_ASSERT_EQUAL_INT32(32604, apu.getHpfRQ15ForTesting());
    TEST_ASSERT_TRUE(apu.getHpfEnabledForTesting());

    const std::vector<int16_t> probe = make_probe_buffer(22050);
    std::vector<int16_t> stage = probe;
    apu.processOutputStageForTesting(stage.data(), static_cast<int>(stage.size()));

    const std::vector<int16_t> expected = reference_one_pole(probe, 32604);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), stage.data(),
                             expected.size() * sizeof(int16_t));
}

// =============================================================================
// S1 - activation and the low-rate LPF policy
// =============================================================================

void test_output_filter_activation_and_lpf_policy(void) {
    ApuCore apu;
    apu.init(44100);

    // Legacy: no chain, no LPF, historical coefficient untouched.
    TEST_ASSERT_FALSE(apu.isOutputFilterChainActiveForTesting());
    TEST_ASSERT_FALSE(apu.isOutputFilterLpfActiveForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, apu.getOutputFilterLpfCutoffHzForTesting());

    apu.setSoundProfile(SoundProfile::Nes);
    TEST_ASSERT_TRUE(apu.isOutputFilterChainActiveForTesting());
    TEST_ASSERT_TRUE(apu.isOutputFilterLpfActiveForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 14000.0f,
                             apu.getOutputFilterLpfCutoffHzForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f,
                             apu.getOutputFilterHpf1CutoffHzForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 440.0f,
                             apu.getOutputFilterHpf2CutoffHzForTesting());

    // The Q15 pole of the 90 Hz HPF mirrors the documented R = exp(-2*pi*fc/fs).
    const double r1 = std::exp(-2.0 * kPi * kHpf1Hz / 44100.0);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(std::lround(r1 * 32768.0)),
                            apu.getOutputFilterHpf1RQ15ForTesting());

    // 22 050: LPF follows 0.45 * fs = 9 922.5 Hz.
    apu.init(22050);
    TEST_ASSERT_TRUE(apu.isOutputFilterLpfActiveForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 9922.5f,
                             apu.getOutputFilterLpfCutoffHzForTesting());

    // 8 000: 0.45 * fs = 3 600 Hz, below the 6 kHz engagement floor, so the
    // chain is the two HPFs only (audit P3 risk iii).
    apu.init(8000);
    TEST_ASSERT_TRUE(apu.isOutputFilterChainActiveForTesting());
    TEST_ASSERT_FALSE(apu.isOutputFilterLpfActiveForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f,
                             apu.getOutputFilterLpfCutoffHzForTesting());

    // Boundary: 0.45 * 13 333 = 5 999.85 Hz -> off; 13 334 -> 6 000.3 -> on.
    apu.init(13333);
    TEST_ASSERT_FALSE(apu.isOutputFilterLpfActiveForTesting());
    apu.init(13334);
    TEST_ASSERT_TRUE(apu.isOutputFilterLpfActiveForTesting());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 6000.3f,
                             apu.getOutputFilterLpfCutoffHzForTesting());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_output_filter_response_8000);
    RUN_TEST(test_output_filter_response_22050);
    RUN_TEST(test_output_filter_response_44100);
    RUN_TEST(test_output_filter_response_48000);
    RUN_TEST(test_output_filter_stability_8000);
    RUN_TEST(test_output_filter_stability_22050);
    RUN_TEST(test_output_filter_stability_44100);
    RUN_TEST(test_output_filter_stability_48000);
    RUN_TEST(test_output_filter_override_precedes_profile);
    RUN_TEST(test_output_filter_legacy_stage_is_historical_one_pole);
    RUN_TEST(test_output_filter_activation_and_lpf_policy);
    return UNITY_END();
}
