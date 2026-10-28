/**
 * @file test_apu_wavetable_spectral.cpp
 * @brief E2/S4, S5 (and the S6 groundwork): the band-limited SAW mipmap tables.
 *
 * Nothing in this suite touches the renderer. It proves the committed tables
 * (include/pixelroot32/apu/AudioOscMipLUT.h) and, at the same time, states the
 * contract the renderer has to honour when T3 wires them in.
 *
 * Coverage:
 *   - S4 level layout: the levels are keyed on `q = f / fs` (one octave each)
 *     and a level's harmonic count is the count its band's TOP allows, so one
 *     table set serves 8 000 / 22 050 / 44 100 / 48 000 Hz. Including the
 *     128-harmonic cap and the two clamps at the ends of the range.
 *   - S4 band limit, per level and not on average: a 256-point DFT of every
 *     table puts every bin above the level's harmonic count at or below
 *     -60 dBFS relative to the fundamental, the fundamental is the strongest
 *     bin, and the harmonics follow the saw's 1/n law (so a table that is
 *     "quiet up there" because it is the wrong waveform fails too).
 *   - S6 groundwork: every table crosses zero at phase 0 and its largest
 *     adjacent-sample step stays inside the bound its harmonic count implies,
 *     WRAP INCLUDED -- the wrap is exactly where an unbounded saw puts its
 *     discontinuity (a 65 535 step), so that is the step this case pins.
 *   - S5 alias energy: fold-down (energy below the fundamental, which cannot
 *     exist in a band-limited oscillator) of the mipmapped synthesis against
 *     the ideal ramp of `generateSawSampleQ15`, swept over 55 Hz - 8 kHz at
 *     22 050 and 44 100 Hz. The measurement is a 8 192-point DFT over a record
 *     that holds an EXACT integer number of periods, so every harmonic and
 *     every alias falls exactly on a bin and no window or guard band is needed:
 *     the bins below the fundamental are folded energy by construction.
 *   - The SINE control (L5): the same measurement on `SINE_LUT_Q15`, reported
 *     and not specified. It is the evidence that mipmapping SINE would buy
 *     nothing, so the 8.5 KB E2 flash budget goes to SAW alone.
 *
 * Deviation worth knowing, measured and pinned below rather than assumed: with
 * the bare 8-bit table index (`phase >> 24`) the mipmap's own below-fundamental
 * residue is ~-43 dBFS, created by phase truncation, not by the table. The
 * ideal ramp's fold-down is already at or below that figure below a few
 * hundred Hz (the boundary moves with the rate: ~200 Hz at 22 050 and ~350 Hz
 * at 44 100), so the S5 threshold is NOT reachable there by any 256-point table
 * read that truncates the phase. The same tables read with linear interpolation
 * between the two adjacent points land at -55 dBFS or better and clear S5 at
 * every swept frequency (worst measured: 13.7 dB). Both reads are therefore
 * measured: S5 is asserted on the interpolated one, and the truncated one is
 * pinned as "never worse than the ramp it replaces", so the trade is recorded
 * where it was made.
 */

#include <unity.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "pixelroot32/apu/AudioOscLUT.h"
#include "pixelroot32/apu/AudioOscMipLUT.h"
#include "test_config.h"

using namespace pixelroot32::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The level layout, kept here as the independent statement of S4 so that a
// change in the generated header has to disagree with the test, not with
// itself.
constexpr int kLevels = 8;
constexpr int kTableSize = 256;
constexpr int kMaxHarmonics = 128;
constexpr double kBandLimitFloorDb = -60.0;

// S5.
constexpr double kMinImprovementDb = 10.0;
constexpr double kSweepLowHz = 55.0;
constexpr double kSweepHighHz = 8000.0;
// 16 log-spaced points per rate. Each point costs four records of 8 192
// samples and four DFTs; 16 keeps the whole sweep under a second in a debug
// build while still covering the low, middle and top of the range.
constexpr int kSweepPoints = 16;
// Power of two, and long enough that the lowest swept frequency sits on a
// usable bin (55 Hz at 22 050 Hz is bin 21).
constexpr int kRecordLength = 8192;
constexpr int kRates[2] = {22050, 44100};
// A record that holds an exact number of periods leaves the ramp with zero
// fold-down at some frequencies. Zero is not a ratio, so it is floored.
constexpr double kSilentDb = -300.0;

// S6 groundwork: the sampled table may drift by one rounding step from the
// analytic slope bound.
constexpr int kStepRoundingSlack = 2;

/// @brief Which oscillator a record is synthesised from.
enum class Source {
    IdealRamp,          ///< `generateSawSampleQ15`'s arithmetic, today's SAW.
    MipmapTruncated,    ///< `SAW_MIP_Q15[L][phase >> 24]`.
    MipmapInterpolated, ///< linear blend of the two adjacent table points.
    Sine                ///< `SINE_LUT_Q15[phase >> 24]`, today's SINE.
};

/// @brief One point of the S5 sweep: a frequency, a rate and the level it picks.
struct SpectrumPoint {
    double frequencyHz = 0.0;
    int sampleRate = 0;
    int level = 0;
};

/// @brief The level whose band contains `q = f / fs`, clamped at both ends.
int levelForQ(double q) {
    // Level L covers [2^-(9-L), 2^-(8-L)). Comparing against the open top edge
    // of each level is the same statement and keeps the two ends implicit.
    for (int level = 0; level < kLevels - 1; ++level) {
        if (q < std::ldexp(1.0, -(8 - level))) return level;
    }
    return kLevels - 1;
}

/// @brief The top of a level's band: `q` where its harmonic count was chosen.
double bandTopForLevel(int level) { return std::ldexp(1.0, -(8 - level)); }

/// @brief The bottom of a level's band, `2^-(9-L)`.
double bandBottomForLevel(int level) { return std::ldexp(1.0, -(9 - level)); }

/// @brief In-place iterative radix-2 FFT; `data.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& data) {
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t size = 2; size <= n; size <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(size);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += size) {
            std::complex<double> twiddle(1.0, 0.0);
            for (std::size_t k = start; k < start + size / 2; ++k) {
                const std::complex<double> even = data[k];
                const std::complex<double> odd = data[k + size / 2] * twiddle;
                data[k] = even + odd;
                data[k + size / 2] = even - odd;
                twiddle *= step;
            }
        }
    }
}

/// @brief Power per bin of a real record, full spectrum (mirror included).
std::vector<double> spectrumPower(const std::vector<double>& signal) {
    std::vector<std::complex<double>> bins(signal.size());
    for (std::size_t i = 0; i < signal.size(); ++i) bins[i] = signal[i];
    fft(bins);
    std::vector<double> power(bins.size());
    for (std::size_t i = 0; i < bins.size(); ++i) power[i] = std::norm(bins[i]);
    return power;
}

/// @brief Current `phase >> 24` read of a mipmap level, as the renderer does it.
int32_t readTableTruncated(int level, uint32_t phase) {
    return SAW_MIP_Q15[level][(phase >> 24) & 0xFFu];
}

/// @brief Linear blend between the two adjacent table points (Q16 fraction).
int32_t readTableInterpolated(int level, uint32_t phase) {
    const int index = static_cast<int>((phase >> 24) & 0xFFu);
    const int next = (index + 1) & (kTableSize - 1);
    const int32_t a = SAW_MIP_Q15[level][index];
    const int32_t b = SAW_MIP_Q15[level][next];
    const int64_t fraction = static_cast<int64_t>((phase >> 8) & 0xFFFFu);
    return a + static_cast<int32_t>(((static_cast<int64_t>(b) - a) * fraction + 32768) >> 16);
}

/// @brief One Q32-accumulator record of `source` at `frequencyHz`.
std::vector<double> synthesise(Source source, double frequencyHz, int sampleRate,
                               int length, int level) {
    const uint32_t increment = static_cast<uint32_t>(
        std::llround(frequencyHz / static_cast<double>(sampleRate) * 4294967296.0));
    std::vector<double> record(static_cast<std::size_t>(length));
    uint32_t phase = 0;
    for (int i = 0; i < length; ++i) {
        int32_t sample = 0;
        switch (source) {
            case Source::IdealRamp: {
                // Exactly generateSawSampleQ15, the baseline being replaced.
                const int64_t value = (static_cast<int64_t>(phase) << 1) - (1LL << 32);
                sample = static_cast<int32_t>(value >> 17);
                break;
            }
            case Source::MipmapTruncated:
                sample = readTableTruncated(level, phase);
                break;
            case Source::MipmapInterpolated:
                sample = readTableInterpolated(level, phase);
                break;
            case Source::Sine:
                sample = SINE_LUT_Q15[(phase >> 24) & 0xFFu];
                break;
        }
        record[static_cast<std::size_t>(i)] = static_cast<double>(sample);
        phase += increment;
    }
    return record;
}

/**
 * @brief Fold-down: energy below the fundamental, in dB relative to it.
 *
 * `fundamentalBin` is the exact bin the fundamental sits on. The record is an
 * exact number of periods, so anything in bins 1 .. fundamentalBin-1 is folded
 * energy: no harmonic of the note can live there.
 */
double foldDownDb(const std::vector<double>& signal, int fundamentalBin) {
    const std::vector<double> power = spectrumPower(signal);
    double fundamental = 0.0;
    for (int k = fundamentalBin - 1; k <= fundamentalBin + 1; ++k) {
        fundamental = std::max(fundamental, power[static_cast<std::size_t>(k)]);
    }
    double below = 0.0;
    for (int k = 1; k < fundamentalBin; ++k) {
        below += power[static_cast<std::size_t>(k)];
    }
    if (below <= 0.0 || fundamental <= 0.0) return kSilentDb;
    return 10.0 * std::log10(below / fundamental);
}

/// @brief The S5 sweep, snapped to the DFT grid so every record is exact.
std::vector<SpectrumPoint> sweepPoints() {
    std::vector<SpectrumPoint> points;
    for (int rate : kRates) {
        const int lowestBin = static_cast<int>(
            std::ceil(kSweepLowHz * kRecordLength / static_cast<double>(rate)));
        const int highestBin = static_cast<int>(
            std::floor(kSweepHighHz * kRecordLength / static_cast<double>(rate)));
        for (int i = 0; i < kSweepPoints; ++i) {
            const double target = kSweepLowHz *
                std::pow(kSweepHighHz / kSweepLowHz,
                         (static_cast<double>(i) + 0.5) / kSweepPoints);
            int bin = static_cast<int>(
                std::lround(target * kRecordLength / static_cast<double>(rate)));
            bin = std::max(lowestBin, std::min(highestBin, bin));
            SpectrumPoint point;
            point.sampleRate = rate;
            point.frequencyHz = bin * static_cast<double>(rate) / kRecordLength;
            point.level = levelForQ(point.frequencyHz / static_cast<double>(rate));
            points.push_back(point);
        }
    }
    return points;
}

/// @brief Fold-down of one source at one sweep point.
double foldDownDb(Source source, const SpectrumPoint& point) {
    const int fundamentalBin = static_cast<int>(std::lround(
        point.frequencyHz * kRecordLength / static_cast<double>(point.sampleRate)));
    const std::vector<double> record = synthesise(
        source, point.frequencyHz, point.sampleRate, kRecordLength, point.level);
    return foldDownDb(record, fundamentalBin);
}

} // namespace

void setUp(void) { test_setup(); }

void tearDown(void) { test_teardown(); }

// =============================================================================
// S4 - the levels are keyed on q, and the counts come from the band top
// =============================================================================

void test_wavetable_layout_is_keyed_on_q_not_on_hz(void) {
    static_assert(sizeof(int16_t) == 2, "Q15 samples are 2 bytes");
    static_assert(sizeof(SAW_MIP_Q15) == 8u * 256u * 2u,
                  "the table data must stay at 8 x 256 x 2 B = 4 096 B");
    TEST_ASSERT_EQUAL_UINT8(kLevels, SAW_MIP_LEVEL_COUNT);

    for (int level = 0; level < kLevels; ++level) {
        const double bottom = bandBottomForLevel(level);
        const double top = bandTopForLevel(level);
        // One octave of q per level, and the levels tile without a gap.
        TEST_ASSERT_TRUE(std::fabs(top - bottom * 2.0) < 1e-12);
        if (level > 0) {
            TEST_ASSERT_TRUE(std::fabs(bandTopForLevel(level - 1) - bottom) < 1e-12);
        }

        // The count is the band's worst case: the series still fits under
        // Nyquist at the top edge, and one more harmonic would not.
        const int expected = std::min(kMaxHarmonics, 1 << (7 - level));
        TEST_ASSERT_EQUAL_INT(expected, SAW_MIP_HARMONICS[level]);
        const double topHarmonicQ = SAW_MIP_HARMONICS[level] * top;
        TEST_ASSERT_TRUE(topHarmonicQ <= 0.5);
        if (SAW_MIP_HARMONICS[level] < kMaxHarmonics) {
            TEST_ASSERT_TRUE((SAW_MIP_HARMONICS[level] + 1) * top > 0.5);
        } else {
            // Level 0 sits exactly on the cap: its own band top also asks for
            // 128, and one octave below (a hypothetical level -1) would ask
            // for 256, which a 256-point table cannot carry. That is why the
            // octaves stop here rather than continuing downwards.
            TEST_ASSERT_EQUAL_INT(0, level);
            TEST_ASSERT_TRUE(std::fabs(0.5 / top - kMaxHarmonics) < 1e-9);
            TEST_ASSERT_TRUE(0.5 / bandBottomForLevel(level) > kMaxHarmonics);
        }

        // Selection: the bottom, the middle and the last representable q of
        // the band all land on this level.
        TEST_ASSERT_EQUAL_INT(level, levelForQ(bottom));
        TEST_ASSERT_EQUAL_INT(level, levelForQ(std::sqrt(bottom * top)));
        TEST_ASSERT_EQUAL_INT(level, levelForQ(std::nextafter(top, 0.0)));
    }

    // Clamps: below the lowest band and above the highest one.
    TEST_ASSERT_EQUAL_INT(0, levelForQ(1e-9));
    TEST_ASSERT_EQUAL_INT(kLevels - 1, levelForQ(0.75));

    // Cross-check against a table this generator did not produce: level 7
    // carries one harmonic, so it must be the project's existing sine table
    // value for value. That pins the phase origin, the rounding and the Q15
    // scale to the convention `SINE_LUT_Q15` has used all along.
    for (int i = 0; i < kTableSize; ++i) {
        TEST_ASSERT_EQUAL_INT16(SINE_LUT_Q15[i], SAW_MIP_Q15[kLevels - 1][i]);
    }
}

// =============================================================================
// S4 - the band limit itself, per level, from the table's own spectrum
// =============================================================================

void test_wavetable_band_limit_per_level(void) {
    std::printf("\n  S4 band limit (dB relative to the fundamental; the spec floor is %.0f)\n",
                kBandLimitFloorDb);
    std::printf("  level  H     worst bin above H            Nyquist\n");
    for (int level = 0; level < kLevels; ++level) {
        std::vector<double> record(kTableSize);
        for (int i = 0; i < kTableSize; ++i) record[static_cast<std::size_t>(i)] = SAW_MIP_Q15[level][i];
        const std::vector<double> power = spectrumPower(record);
        const double fundamental = power[1];
        TEST_ASSERT_TRUE(fundamental > 0.0);

        // The fundamental is the strongest bin of the positive half-spectrum.
        for (int k = 2; k <= kTableSize / 2; ++k) {
            TEST_ASSERT_TRUE(power[static_cast<std::size_t>(k)] < fundamental);
        }

        // Everything above the level's harmonic count sits at or below
        // -60 dBFS relative to the fundamental. Level 0 carries the cap (128),
        // so its positive half-spectrum has nothing above it; its Nyquist bin
        // is checked instead, and must be a null (harmonic 128 samples to 0).
        const int harmonics = SAW_MIP_HARMONICS[level];
        double worstRelative = kSilentDb;
        int worstBin = 0;
        for (int k = harmonics + 1; k <= kTableSize / 2; ++k) {
            const double relative = 10.0 * std::log10(power[static_cast<std::size_t>(k)] / fundamental);
            if (relative > worstRelative) {
                worstRelative = relative;
                worstBin = k;
            }
            char message[128];
            std::snprintf(message, sizeof(message),
                          "level %d: bin %d (H = %d) at %.1f dB", level, k, harmonics, relative);
            TEST_ASSERT_TRUE_MESSAGE(relative <= kBandLimitFloorDb, message);
        }
        const double nyquistPower = power[kTableSize / 2];
        if (harmonics == kMaxHarmonics) {
            // Harmonic 128 lands exactly on Nyquist and samples to zero, which
            // is why the printed floor for level 0 is a null and not a number.
            TEST_ASSERT_TRUE(nyquistPower < 1e-9);
        }
        if (worstBin == 0) {
            std::printf("  %5d  %3d   none (H = N/2)                -inf (null)\n",
                        level, harmonics);
        } else {
            std::printf("  %5d  %3d   %8.1f dB at bin %3d\n",
                        level, harmonics, worstRelative, worstBin);
        }

        // The surviving harmonics follow the saw's 1/n law, which is what makes
        // this a SAW table and not merely a quiet one.
        for (int n = 2; n <= harmonics; ++n) {
            if (power[static_cast<std::size_t>(n)] <= 0.0) {
                // Only harmonic 128 can be exactly null (Nyquist).
                TEST_ASSERT_EQUAL_INT(kMaxHarmonics, n);
                continue;
            }
            const double deviation = std::fabs(
                10.0 * std::log10(power[static_cast<std::size_t>(n)] / fundamental) +
                20.0 * std::log10(static_cast<double>(n)));
            char message[128];
            std::snprintf(message, sizeof(message),
                          "level %d: harmonic %d deviates %.3f dB from 1/n", level, n, deviation);
            TEST_ASSERT_TRUE_MESSAGE(deviation <= 0.1, message);
        }
    }
}

// =============================================================================
// S6 groundwork - the table has no seam of its own
// =============================================================================

void test_wavetable_continuity_across_the_period(void) {
    std::printf("\n  S6 groundwork: step continuity (wrap included)\n");
    std::printf("  level  H     max step   bound    wrap step\n");
    for (int level = 0; level < kLevels; ++level) {
        std::vector<double> record(kTableSize);
        for (int i = 0; i < kTableSize; ++i) record[static_cast<std::size_t>(i)] = SAW_MIP_Q15[level][i];
        const std::vector<double> power = spectrumPower(record);
        const int harmonics = SAW_MIP_HARMONICS[level];

        // Phase 0 is the zero crossing: the series has no DC term, so a table
        // that starts anywhere else would click once per period at every note.
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, std::abs(static_cast<int>(SAW_MIP_Q15[level][0])));

        // The largest step one sample can take, bounded by the analytic slope
        // of a harmonic series truncated at `harmonics`: |f'| <= 2*pi*H on a
        // period of 1, one sample is 1/256 of that, and the table is in units
        // of its own fundamental amplitude.
        const double fundamentalAmplitude = 2.0 * std::sqrt(power[1]) / kTableSize;
        const double bound = 2.0 * kPi * harmonics / kTableSize * fundamentalAmplitude;
        int maxStep = 0;
        int maxInteriorStep = 0;
        for (int i = 0; i < kTableSize; ++i) {
            const int step = std::abs(static_cast<int>(SAW_MIP_Q15[level][(i + 1) & (kTableSize - 1)]) -
                                      static_cast<int>(SAW_MIP_Q15[level][i]));
            maxStep = std::max(maxStep, step);
            if (i + 1 < kTableSize) maxInteriorStep = std::max(maxInteriorStep, step);
        }
        char message[160];
        std::snprintf(message, sizeof(message),
                      "level %d: largest step %d exceeds the H = %d bound %.0f",
                      level, maxStep, harmonics, bound);
        TEST_ASSERT_TRUE_MESSAGE(static_cast<double>(maxStep) <= bound + kStepRoundingSlack,
                                 message);

        // The wrap is where an unbounded saw puts its discontinuity (65 535
        // LSB). The periodic table must not be special there: the step from
        // the last sample back to the first is an ordinary table step.
        const int wrapStep = std::abs(static_cast<int>(SAW_MIP_Q15[level][0]) -
                                      static_cast<int>(SAW_MIP_Q15[level][kTableSize - 1]));
        std::snprintf(message, sizeof(message),
                      "level %d: wrap step %d vs largest interior step %d",
                      level, wrapStep, maxInteriorStep);
        TEST_ASSERT_TRUE_MESSAGE(wrapStep <= maxInteriorStep, message);

        std::printf("  %5d  %3d   %8d   %6.0f   %9d\n",
                    level, harmonics, maxStep, bound, wrapStep);
    }
}

// =============================================================================
// S5 - alias energy, measured below the fundamental against the ideal ramp
// =============================================================================

void test_wavetable_alias_sweep_beats_the_ideal_ramp(void) {
    const std::vector<SpectrumPoint> points = sweepPoints();
    double worstInterpolatedDb = kSilentDb;
    double worstInterpolatedBaseline = kSilentDb;
    SpectrumPoint worstInterpolated;
    double worstTruncatedDb = kSilentDb;
    double worstTruncatedBaseline = kSilentDb;
    SpectrumPoint worstTruncated;

    std::printf("\n  S5 fold-down (dB relative to the fundamental; lower is better)\n");
    std::printf("  rate    freq      level  ramp     mip>>24  dB    mip+lerp  dB\n");
    for (const SpectrumPoint& point : points) {
        const double ramp = foldDownDb(Source::IdealRamp, point);
        const double truncated = foldDownDb(Source::MipmapTruncated, point);
        const double interpolated = foldDownDb(Source::MipmapInterpolated, point);
        std::printf("  %5d  %7.2f      %d  %7.2f   %7.2f  %5.1f   %7.2f  %5.1f\n",
                    point.sampleRate, point.frequencyHz, point.level,
                    ramp, truncated, ramp - truncated, interpolated, ramp - interpolated);

        // S5: at least 10 dB, or already at the band-limit floor. There is
        // nothing to improve on when the baseline is already at (or below) the
        // floor the S4 case pins, and the two records are then both silent.
        char message[200];
        const bool clearsThreshold =
            interpolated <= ramp - kMinImprovementDb || interpolated <= kBandLimitFloorDb;
        std::snprintf(message, sizeof(message),
                      "interpolated mipmap at %.2f Hz / %d Hz: %.2f dB vs ramp %.2f dB (%.2f dB)",
                      point.frequencyHz, point.sampleRate, interpolated, ramp,
                      ramp - interpolated);
        TEST_ASSERT_TRUE_MESSAGE(clearsThreshold, message);

        // The truncated read is the conservative fallback: no 256-point table
        // can beat the ramp there (see the file comment), but it must not make
        // the fold-down worse than the oscillator it replaces.
        std::snprintf(message, sizeof(message),
                      "truncated mipmap at %.2f Hz / %d Hz: %.2f dB vs ramp %.2f dB",
                      point.frequencyHz, point.sampleRate, truncated, ramp);
        TEST_ASSERT_TRUE_MESSAGE(truncated <= ramp, message);

        if (interpolated > kBandLimitFloorDb &&
            (worstInterpolatedDb <= kBandLimitFloorDb ||
             ramp - interpolated < worstInterpolatedBaseline - worstInterpolatedDb)) {
            worstInterpolatedDb = interpolated;
            worstInterpolatedBaseline = ramp;
            worstInterpolated = point;
        }
        if (truncated > kBandLimitFloorDb &&
            (worstTruncatedDb <= kBandLimitFloorDb ||
             ramp - truncated < worstTruncatedBaseline - worstTruncatedDb)) {
            worstTruncatedDb = truncated;
            worstTruncatedBaseline = ramp;
            worstTruncated = point;
        }
    }

    std::printf("  worst above the floor, interpolated: %.2f dB vs ramp %.2f dB "
                "(%.2f dB better) at %.2f Hz / %d Hz\n",
                worstInterpolatedDb, worstInterpolatedBaseline,
                worstInterpolatedBaseline - worstInterpolatedDb,
                worstInterpolated.frequencyHz, worstInterpolated.sampleRate);
    std::printf("  worst above the floor, phase>>24:    %.2f dB vs ramp %.2f dB "
                "(%.2f dB better) at %.2f Hz / %d Hz\n",
                worstTruncatedDb, worstTruncatedBaseline,
                worstTruncatedBaseline - worstTruncatedDb,
                worstTruncated.frequencyHz, worstTruncated.sampleRate);
}

// =============================================================================
// L5 - SINE is reported, not specified: it has nothing left to remove
// =============================================================================

void test_wavetable_sine_control_reports_its_floor(void) {
    const std::vector<SpectrumPoint> points = sweepPoints();
    double worstDb = kSilentDb;
    SpectrumPoint worst;
    std::printf("\n  L5 SINE control (SINE_LUT_Q15, phase>>24)\n");
    for (const SpectrumPoint& point : points) {
        const double ramp = foldDownDb(Source::IdealRamp, point);
        const double sine = foldDownDb(Source::Sine, point);
        std::printf("  %5d  %7.2f      ramp %7.2f   sine %8.2f\n",
                    point.sampleRate, point.frequencyHz, ramp, sine);
        // The claim L5 rests on: the single-harmonic table is already at or
        // below the saw's fold-down everywhere, so a mipmap would spend flash
        // to fix something SINE does not have. No threshold is asserted on the
        // absolute figure -- it is reported for the docs.
        char message[160];
        std::snprintf(message, sizeof(message),
                      "SINE at %.2f Hz / %d Hz: %.2f dB is worse than the ramp's %.2f dB",
                      point.frequencyHz, point.sampleRate, sine, ramp);
        TEST_ASSERT_TRUE_MESSAGE(sine <= ramp, message);
        if (sine > worstDb) {
            worstDb = sine;
            worst = point;
        }
    }
    std::printf("  worst SINE fold-down: %.2f dB at %.2f Hz / %d Hz\n",
                worstDb, worst.frequencyHz, worst.sampleRate);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wavetable_layout_is_keyed_on_q_not_on_hz);
    RUN_TEST(test_wavetable_band_limit_per_level);
    RUN_TEST(test_wavetable_continuity_across_the_period);
    RUN_TEST(test_wavetable_alias_sweep_beats_the_ideal_ramp);
    RUN_TEST(test_wavetable_sine_control_reports_its_floor);
    return UNITY_END();
}
