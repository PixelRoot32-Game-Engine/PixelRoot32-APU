/**
 * @file test_apu_hybrid_wavetable.cpp
 * @brief E2/T3 (S4, S5, S6, S9): the `Hybrid` SAW reads the band-limited
 * mipmap.
 *
 * `test_apu_wavetable_spectral.cpp` proves the committed tables and states the
 * read the renderer has to honour; this suite proves the renderer honours it.
 * The definitions under test are:
 *
 *   - `Hybrid` is the ONLY profile whose SAW is band-limited. `Legacy` and
 *     `Nes` must still render the ideal ramp sample for sample, and SINE must
 *     stay on `SINE_LUT_Q15` for every profile (L5).
 *   - the level comes from the voice's own `phaseIncQ32`
 *     (`L = floor(log2(q)) + 9`, clamped), which is what makes pitch
 *     automation track the table selection.
 *   - adjacent levels are CROSSFADED by the position inside the octave, so a
 *     glissando crossing a boundary has no seam (S6).
 *   - S5 end to end: the SAME note rendered through `ApuCore` under `Hybrid`
 *     folds at least 10 dB less energy below the fundamental than under `Nes`.
 *     The spectral suite measures the tables in isolation and the cases below
 *     compare profiles sample for sample; neither of those adds up to the
 *     profile-level number S5 states, so it is measured on the PCM here.
 *     Measured worst case: 18.5 dB at 104.97 Hz / 22 050 Hz (Nes -41.29 dB,
 *     Hybrid -59.81 dB). At 22 050 Hz the baseline climbs from -46.7 dB at
 *     56 Hz to -2.8 dB at 8 kHz as the ramp's aliases start landing below the
 *     fundamental, while `Hybrid` reads between -59.8 dB and -94.8 dB
 *     everywhere. The 64.60 Hz row reproduces the spectral suite's ramp figure
 *     (-46.01 dB) exactly, which is the cross-check that this is the same
 *     baseline measured through a different path.
 *
 * The render is read through the public pipeline with the output stage
 * deliberately bypassed: the profile owns it (S1), but `setHpfCutoffHz(0)`
 * is the documented caller override (S8), and `SoftClipMode::None` leaves the
 * integer core's single 0.4 mix scale as the only arithmetic between the
 * oscillator and the PCM. The flat preset (one-sample attack, sustain 1.0)
 * makes that scale exact, so the reference below is the oscillator itself.
 *
 * The target is built with `PR32_APU_ENABLE_LEGACY_FLOAT=0` on purpose: on an
 * FPU build `Legacy` renders through the frozen float body, and "the ideal
 * ramp exactly" is a statement about the integer core both other profiles
 * use. See CMakeLists.txt.
 */

#include <unity.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioOscMipLUT.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "test_config.h"

using namespace pixelroot32::audio;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kLevels = static_cast<int>(SAW_MIP_LEVEL_COUNT);
constexpr int kTableSize = 256;
// The integer core's one mix scale (0.4 in Q15), applied to the summed voices
// before the output stage. Documented next to the shaping curve.
constexpr int32_t kMixScaleQ15 = 13107;
constexpr int32_t kMixScaleShift = 15;

/// @brief Flat one-note instrument: no envelope shape, so the PCM is the
/// oscillator times the mix scale and nothing else.
InstrumentPreset flatPreset()
{
    InstrumentPreset preset{};
    preset.baseVolume = 1.0f;
    preset.duty = 0.5f;
    preset.defaultOctave = 4;
    preset.attackTime = 0.0f;
    preset.decayTime = 0.0f;
    preset.sustainLevel = 1.0f;
    preset.releaseTime = 0.005f;
    return preset;
}

/// @brief Puts the output stage out of the way: HPF bypassed (S8 override),
/// no shaping curve, so the integer core's mix scale is the only arithmetic.
void prepare(ApuCore& apu, SoundProfile profile, int sampleRate)
{
    apu.init(sampleRate);
    apu.reset();
    apu.setSoundProfile(profile);
    apu.setSoftClipMode(SoftClipMode::None);
    apu.setHpfCutoffHz(0.0f);
}

/// @brief One looping note command, optionally swept.
AudioCommand noteCommand(WaveType type, float frequencyHz, bool loop,
                         float sweepEndHz, float sweepDurationSec,
                         SweepCurve curve, const InstrumentPreset& preset)
{
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = type;
    cmd.event.frequency = frequencyHz;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = loop;
    cmd.event.preset = &preset;
    cmd.event.sweepEndHz = sweepEndHz;
    cmd.event.sweepDurationSec = sweepDurationSec;
    cmd.event.sweepCurve = curve;
    return cmd;
}

/// @brief The Q32 increment `initVoiceFromEvent` derives from a frequency.
uint32_t phaseIncFor(double frequencyHz, int sampleRate)
{
    const double inc = frequencyHz * 4294967296.0 / static_cast<double>(sampleRate);
    if (inc <= 0.0) return 0u;
    if (inc >= 4294967295.0) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(inc);
}

/// @brief `generateSawSampleQ15`, the ideal ramp `Legacy` and `Nes` keep.
int32_t idealRampQ15(uint32_t phaseQ32)
{
    const int64_t value = (static_cast<int64_t>(phaseQ32) << 1) - (1LL << 32);
    return static_cast<int32_t>(value >> 17);
}

/// @brief Oscillator Q15 -> the PCM the integer core emits once the mix scale
/// is applied (flat envelope, unity volume, bypassed output stage).
int16_t toPcm(int32_t sampleQ15)
{
    return static_cast<int16_t>(
        (static_cast<int32_t>(static_cast<int64_t>(sampleQ15) * kMixScaleQ15)
         >> kMixScaleShift));
}

/// @brief Renders one command and returns the PCM.
std::vector<int16_t> render(SoundProfile profile, const AudioCommand& cmd,
                            int count, int sampleRate = kSampleRate)
{
    ApuCore apu;
    prepare(apu, profile, sampleRate);
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));
    std::vector<int16_t> pcm(static_cast<size_t>(count), 0);
    apu.generateSamples(pcm.data(), count);
    return pcm;
}

/// @brief The band rule of `AudioOscMipLUT.h`, written independently of the
/// production selector: level `L` covers `q` in `[2^-(9-L), 2^-(8-L))`, with
/// the two clamps.
int bandLevel(uint32_t phaseIncQ32)
{
    if (phaseIncQ32 == 0u) return 0;
    const double q = static_cast<double>(phaseIncQ32) / 4294967296.0;
    for (int level = 0; level < kLevels - 1; ++level) {
        if (q < std::ldexp(1.0, -(8 - level))) return level;
    }
    return kLevels - 1;
}

/// @brief Test-side leading-zero count, used only to build references.
int leadingZeros(uint32_t value)
{
    int count = 0;
    while ((value & 0x80000000u) == 0u) {
        value <<= 1;
        ++count;
    }
    return count;
}

/// @brief The table read T2 measured: linear blend of the two adjacent points.
int32_t readMipQ15(int level, uint32_t phaseQ32)
{
    const int index = static_cast<int>((phaseQ32 >> 24) & 255u);
    const int next = (index + 1) & (kTableSize - 1);
    const int32_t a = SAW_MIP_Q15[level][index];
    const int32_t b = SAW_MIP_Q15[level][next];
    const int32_t fraction = static_cast<int32_t>((phaseQ32 >> 8) & 0xFFFFu);
    return a + static_cast<int32_t>(((b - a) * (fraction >> 1) + 16384) >> 15);
}

/// @brief The continuous `Hybrid` SAW definition: level crossfade by the Q8
/// octave position, interpolation inside each level.
int32_t hybridSawQ15(uint32_t phaseQ32, uint32_t phaseIncQ32)
{
    if (phaseIncQ32 == 0u) return readMipQ15(0, phaseQ32);
    const int zeros = leadingZeros(phaseIncQ32);
    const int level = bandLevel(phaseIncQ32);

    const int32_t low = readMipQ15(level, phaseQ32);
    if (level >= kLevels - 1) return low;
    const int32_t weightQ8 =
        static_cast<int32_t>(((phaseIncQ32 << zeros) >> 23) & 0xFFu);
    const int32_t high = readMipQ15(level + 1, phaseQ32);
    return low + (((high - low) * weightQ8 + 128) >> 8);
}

/// @brief Largest |T[x + width] - T[x]| for one level, wrap included.
int32_t maxTableWindowStep(int level, int width)
{
    int32_t worst = 0;
    for (int i = 0; i < kTableSize; ++i) {
        const int32_t step = std::abs(
            static_cast<int32_t>(SAW_MIP_Q15[level][(i + width) & (kTableSize - 1)]) -
            static_cast<int32_t>(SAW_MIP_Q15[level][i]));
        worst = std::max(worst, step);
    }
    return worst;
}

/// @brief Largest difference between two adjacent levels at the same phase.
int32_t maxAdjacentLevelDifference()
{
    int32_t worst = 0;
    for (int level = 0; level + 1 < kLevels; ++level) {
        for (int i = 0; i < kTableSize; ++i) {
            const int32_t diff = std::abs(
                static_cast<int32_t>(SAW_MIP_Q15[level][i]) -
                static_cast<int32_t>(SAW_MIP_Q15[level + 1][i]));
            worst = std::max(worst, diff);
        }
    }
    return worst;
}

// =============================================================================
// S5 end to end - fold-down of what `ApuCore` actually emits
// =============================================================================
//
// Same metric as `test_apu_wavetable_spectral.cpp`, deliberately: the fold-down
// is the energy in bins 1 .. fundamentalBin-1 relative to the fundamental, over
// a record that holds an EXACT integer number of periods, so no window and no
// guard band are needed. What changes is where the samples come from: the PCM
// the renderer produces, not a table read composed in the test.

constexpr double kPi = 3.14159265358979323846;
constexpr int kRecordLength = 8192;
// The note is queued on the first render call and sample 0 still has the
// envelope at zero (the S4 wiring case pins that), so the measured record
// starts after the note-on transient. Any whole-sample offset keeps the record
// coherent: `phaseIncQ32 * kRecordLength` is an exact number of cycles, so
// every window of `kRecordLength` samples is one full period.
constexpr int kRecordSkip = 64;
constexpr double kMinImprovementDb = 10.0;
constexpr double kFoldFloorDb = -60.0;
constexpr double kSilentDb = -300.0;
constexpr double kSweepLowHz = 55.0;
constexpr double kSweepHighHz = 8000.0;
// Eleven targets: the S5 floor, 64.60 Hz (the frequency the table-level sweep
// in `test_apu_wavetable_spectral.cpp` found hardest for the interpolated read:
// -59.70 dB, 13.69 dB better than the ramp), then ~1.63x steps to 8 kHz.
// The step is NOT an octave on purpose: the fold-down depends only on
// `q = f / fs` and the Q32 increment, so a shared octave list would print the
// 44 100 Hz row at `2f` identical to the 22 050 Hz row at `f`.
constexpr double kRenderTargetsHz[] = {kSweepLowHz, 64.60, 105.3, 171.6,
                                       279.7,       455.9, 743.1, 1211.4,
                                       1974.6,      3218.6, kSweepHighHz};
constexpr int kRenderPoints =
    static_cast<int>(sizeof(kRenderTargetsHz) / sizeof(kRenderTargetsHz[0]));
constexpr int kRenderRates[] = {22050, 44100};
// A record that rendered silence would pass the "already below the floor"
// clause vacuously; the smallest peak a real SAW note leaves is ~5 500 LSB
// (level 7 at 8 kHz is a sine sampled ~2.8 times per period).
constexpr double kMinPlausiblePeakLsb = 4000.0;

/// @brief One measured point: a rate, the note that was rendered, and the bin
/// its fundamental sits on.
struct RenderPoint {
    int sampleRate = 0;
    double frequencyHz = 0.0;
    int fundamentalBin = 0;
};

/// @brief In-place iterative radix-2 FFT; `data.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& data)
{
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
std::vector<double> spectrumPower(const std::vector<double>& signal)
{
    std::vector<std::complex<double>> bins(signal.size());
    for (std::size_t i = 0; i < signal.size(); ++i) bins[i] = signal[i];
    fft(bins);
    std::vector<double> power(bins.size());
    for (std::size_t i = 0; i < bins.size(); ++i) power[i] = std::norm(bins[i]);
    return power;
}

/// @brief Fold-down in dB relative to the fundamental, exactly as the spectral
/// suite defines it (energy below the fundamental cannot exist in a
/// band-limited oscillator).
double foldDownDb(const std::vector<double>& power, int fundamentalBin)
{
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

/// @brief The nearest DFT-grid point the `float` note frequency reproduces
/// exactly.
///
/// The renderer derives its Q32 increment from a `float`
/// (`frequency_hz_to_phase_inc_q32`), and a `float` cannot hold every grid
/// frequency: near 8 kHz the step between representable values moves the
/// truncated increment by ~95 LSB, which over 8 192 samples is more than a bin
/// of phase drift -- the fundamental then leaks into the bins the metric counts
/// as folded energy. Measured, not assumed: with this check disabled the
/// 4 293 Hz / 22 050 Hz row reads -78.5 dB instead of -90.3 dB for `Hybrid`,
/// because that frequency's bin is one the `float` increment cannot land on.
/// So the snap picks the nearest bin whose increment is exactly `bin << 19`,
/// which makes the record hold a whole number of periods by construction.
///
/// Power-of-two bins are skipped: there the note's period is an exact
/// submultiple of the record (`8192 / bin` samples), so the ramp's aliases also
/// land on multiples of `bin` and both profiles read `-300 dB` because nothing
/// folds below the fundamental at all. The measurement is not wrong there, it
/// is empty, and an empty row is not evidence for S5.
bool nearestCoherentPoint(double targetHz, int rate, RenderPoint& out)
{
    const int lowestBin = std::max(
        2, static_cast<int>(std::ceil(kSweepLowHz * kRecordLength /
                                      static_cast<double>(rate))));
    const int centre = static_cast<int>(
        std::lround(targetHz * kRecordLength / static_cast<double>(rate)));
    for (int offset = 0; offset <= 64; ++offset) {
        for (int direction = 0; direction < 2; ++direction) {
            if (offset == 0 && direction == 1) continue;
            const int bin = centre + ((direction == 0) ? offset : -offset);
            if (bin < lowestBin || bin > kRecordLength / 2 - 1) continue;
            if ((bin & (bin - 1)) == 0) continue;  // see above
            const double exact = bin * static_cast<double>(rate) /
                                 static_cast<double>(kRecordLength);
            const float asFloat = static_cast<float>(exact);
            if (phaseIncFor(static_cast<double>(asFloat), rate) !=
                (static_cast<uint32_t>(bin) << 19)) {
                continue;
            }
            out.sampleRate = rate;
            out.frequencyHz = static_cast<double>(asFloat);
            out.fundamentalBin = bin;
            return true;
        }
    }
    return false;
}

/// @brief One rendered note and its fold-down.
struct RenderFold {
    double foldDownDb = kSilentDb;
    double peakLsb = 0.0;
};

/// @brief Renders the flat SAW note under `profile` and measures its record.
RenderFold measureRender(SoundProfile profile, const RenderPoint& point)
{
    const InstrumentPreset preset = flatPreset();
    const AudioCommand cmd = noteCommand(WaveType::SAW,
                                         static_cast<float>(point.frequencyHz), true,
                                         0.0f, 0.0f, SweepCurve::Linear, preset);
    const std::vector<int16_t> pcm = render(profile, cmd,
                                            kRecordSkip + kRecordLength,
                                            point.sampleRate);

    std::vector<double> record(static_cast<std::size_t>(kRecordLength));
    RenderFold result;
    for (int i = 0; i < kRecordLength; ++i) {
        const double value = static_cast<double>(pcm[static_cast<std::size_t>(kRecordSkip + i)]);
        record[static_cast<std::size_t>(i)] = value;
        result.peakLsb = std::max(result.peakLsb, std::fabs(value));
    }
    result.foldDownDb = foldDownDb(spectrumPower(record), point.fundamentalBin);
    return result;
}

} // namespace

void setUp(void) { test_setup(); }

void tearDown(void) { test_teardown(); }

// =============================================================================
// S4 - the wiring: only Hybrid reads the tables, Nes/Legacy keep the ramp
// =============================================================================

void test_hybrid_saw_reads_the_tables_and_others_keep_the_ramp(void)
{
    static const float frequencies[] = {220.0f, 440.0f, 880.0f, 1760.0f, 3520.0f};
    constexpr int kCount = 1024;
    int32_t worstHybridDiff = 0;
    size_t differingSamples = 0;

    for (float frequency : frequencies) {
        const InstrumentPreset preset = flatPreset();
        const AudioCommand cmd = noteCommand(WaveType::SAW, frequency, true,
                                             0.0f, 0.0f, SweepCurve::Linear,
                                             preset);
        const std::vector<int16_t> legacy = render(SoundProfile::Legacy, cmd, kCount);
        const std::vector<int16_t> nes = render(SoundProfile::Nes, cmd, kCount);
        const std::vector<int16_t> hybrid = render(SoundProfile::Hybrid, cmd, kCount);

        // The reference is the ideal ramp straight from the Q32 accumulator,
        // then the integer core's single documented mix scale. Sample 0 is the
        // note-on sample, still at envelope zero.
        const uint32_t increment = phaseIncFor(frequency, kSampleRate);
        uint32_t phase = 0u;
        for (int i = 0; i < kCount; ++i) {
            const int16_t expected = (i == 0) ? 0 : toPcm(idealRampQ15(phase));
            char message[192];
            std::snprintf(message, sizeof(message),
                          "Legacy SAW at %.1f Hz diverged at sample %d: %d vs ramp %d",
                          static_cast<double>(frequency), i,
                          static_cast<int>(legacy[static_cast<size_t>(i)]),
                          static_cast<int>(expected));
            TEST_ASSERT_EQUAL_INT16_MESSAGE(expected,
                                            legacy[static_cast<size_t>(i)],
                                            message);
            std::snprintf(message, sizeof(message),
                          "Nes SAW at %.1f Hz diverged at sample %d: %d vs ramp %d",
                          static_cast<double>(frequency), i,
                          static_cast<int>(nes[static_cast<size_t>(i)]),
                          static_cast<int>(expected));
            TEST_ASSERT_EQUAL_INT16_MESSAGE(expected,
                                            nes[static_cast<size_t>(i)],
                                            message);

            const int32_t diff = std::abs(
                static_cast<int32_t>(hybrid[static_cast<size_t>(i)]) -
                static_cast<int32_t>(expected));
            if (diff != 0) ++differingSamples;
            worstHybridDiff = std::max(worstHybridDiff, diff);
            phase += increment;
        }

        // Legacy and Nes are the same profile for SAW: nothing to differ on.
        TEST_ASSERT_EQUAL_INT(0, std::memcmp(legacy.data(), nes.data(),
                                             static_cast<size_t>(kCount) *
                                                 sizeof(int16_t)));
    }

    std::printf("\n  S4 wiring: Hybrid differs from the ramp on %zu of %d samples, "
                "worst %d LSB\n", differingSamples, kCount * 5, worstHybridDiff);
    TEST_ASSERT_TRUE_MESSAGE(differingSamples > 0,
                             "Hybrid rendered the ideal ramp: the tables are not wired");
    TEST_ASSERT_TRUE_MESSAGE(worstHybridDiff > 500,
                             "Hybrid and the ideal ramp are too close to be the mipmap read");
}

// =============================================================================
// S4 - level selection is the band rule, on the voice's own increment
// =============================================================================

void test_hybrid_saw_level_selection_matches_the_band_rule(void)
{
    // The clamps and the degenerate increment.
    TEST_ASSERT_EQUAL_INT(0, ApuCore::sawMipmapLevelForTesting(0u));
    TEST_ASSERT_EQUAL_INT(0, ApuCore::sawMipmapLevelForTesting(1u));
    TEST_ASSERT_EQUAL_INT(kLevels - 1,
                          ApuCore::sawMipmapLevelForTesting(0xFFFFFFFFu));
    // q >= 0.5 is above level 7's band and must clamp, not wrap.
    TEST_ASSERT_EQUAL_INT(kLevels - 1,
                          ApuCore::sawMipmapLevelForTesting(0x80000000u));

    // The exact band edges: the bottom of every band selects that level, and
    // one step below its top still selects it (the top belongs to the next).
    for (int level = 0; level < kLevels; ++level) {
        const double bottom = std::ldexp(1.0, -(9 - level));
        const double top = std::ldexp(1.0, -(8 - level));
        const uint32_t bottomInc = static_cast<uint32_t>(
            std::llround(bottom * 4294967296.0));
        const uint32_t topInc = static_cast<uint32_t>(
            std::llround(top * 4294967296.0));
        char message[160];
        std::snprintf(message, sizeof(message),
                      "level %d band bottom %u selected %d", level, bottomInc,
                      ApuCore::sawMipmapLevelForTesting(bottomInc));
        TEST_ASSERT_EQUAL_INT_MESSAGE(level,
                                      ApuCore::sawMipmapLevelForTesting(bottomInc),
                                      message);
        std::snprintf(message, sizeof(message),
                      "level %d band top %u selected %d (top belongs to level %d)",
                      level, topInc,
                      ApuCore::sawMipmapLevelForTesting(topInc),
                      (level + 1 < kLevels) ? level + 1 : kLevels - 1);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            std::min(level + 1, kLevels - 1),
            ApuCore::sawMipmapLevelForTesting(topInc), message);
    }

    // A dense in-band sweep: every power-of-two and several mantissa steps,
    // which is the whole band at the resolution the selector sees.
    size_t checked = 0;
    for (int bit = 0; bit < 32; ++bit) {
        const uint32_t base = 1u << bit;
        for (uint32_t mantissa = 0; mantissa < 8u; ++mantissa) {
            const uint32_t inc = base | (mantissa * (base / 8u));
            if (inc == 0u) continue;
            char message[160];
            std::snprintf(message, sizeof(message),
                          "increment %u: selector %d, band rule %d", inc,
                          ApuCore::sawMipmapLevelForTesting(inc), bandLevel(inc));
            TEST_ASSERT_EQUAL_INT_MESSAGE(bandLevel(inc),
                                          ApuCore::sawMipmapLevelForTesting(inc),
                                          message);
            ++checked;
        }
    }
    std::printf("\n  S4 selection: %zu increments checked against the band rule\n",
                checked);
}

// =============================================================================
// S6 - the glissando has no seam and SINE is inert
// =============================================================================

void test_hybrid_saw_glissando_crosses_levels_without_a_step(void)
{
    // 55 Hz -> 880 Hz exponential, four octaves, crossing the four boundaries
    // that separate levels 0..3. Three seconds keeps the per-sample increment
    // change tiny, so the render is near-steady and any discontinuity would be
    // the table switch, not the sweep.
    constexpr float kStartHz = 55.0f;
    constexpr float kEndHz = 880.0f;
    constexpr float kSeconds = 3.0f;
    const int count = static_cast<int>(kSeconds * kSampleRate);

    const InstrumentPreset preset = flatPreset();
    const AudioCommand cmd = noteCommand(WaveType::SAW, kStartHz, true, kEndHz,
                                         kSeconds, SweepCurve::Exponential,
                                         preset);
    ApuCore apu;
    prepare(apu, SoundProfile::Hybrid, kSampleRate);
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    std::vector<int16_t> pcm(static_cast<size_t>(count), 0);
    std::vector<int> levels(static_cast<size_t>(count), 0);
    std::vector<uint32_t> phaseIncHistory(static_cast<size_t>(count), 0u);
    const int slot = ApuCore::SFX_VOICE_BASE;
    // Sample 0: the queued command is processed INSIDE the render call, so the
    // increment it uses is the note-on one (the event frequency), before any
    // sweep step. Every later sample is generated one at a time so the level
    // the renderer will use can be read first, from the voice's own increment.
    phaseIncHistory[0] = phaseIncFor(static_cast<double>(kStartHz), kSampleRate);
    levels[0] = ApuCore::sawMipmapLevelForTesting(phaseIncHistory[0]);
    apu.generateSamples(&pcm[0], 1);
    for (int i = 1; i < count; ++i) {
        phaseIncHistory[static_cast<size_t>(i)] = apu.getVoicePhaseIncQ32ForTesting(slot);
        levels[static_cast<size_t>(i)] = ApuCore::sawMipmapLevelForTesting(
            phaseIncHistory[static_cast<size_t>(i)]);
        apu.generateSamples(&pcm[static_cast<size_t>(i)], 1);
    }

    // The level moves by at most one per sample.
    for (int i = 1; i < count; ++i) {
        const int delta = levels[static_cast<size_t>(i)] -
                          levels[static_cast<size_t>(i - 1)];
        char message[128];
        std::snprintf(message, sizeof(message),
                      "level jumped by %d at sample %d (%d -> %d)", delta, i,
                      levels[static_cast<size_t>(i - 1)],
                      levels[static_cast<size_t>(i)]);
        TEST_ASSERT_TRUE_MESSAGE(delta >= -1 && delta <= 1, message);
    }
    int distinctLevels = 0;
    int levelSequence[16] = {};
    for (int i = 0; i < count; ++i) {
        const int level = levels[static_cast<size_t>(i)];
        if (distinctLevels == 0 ||
            levelSequence[distinctLevels - 1] != level) {
            TEST_ASSERT_TRUE(distinctLevels < 16);
            levelSequence[distinctLevels++] = level;
        }
    }
    char sequence[64] = {};
    int written = 0;
    for (int i = 0; i < distinctLevels && written < (int)sizeof(sequence) - 8; ++i) {
        written += std::snprintf(sequence + written,
                                 sizeof(sequence) - static_cast<size_t>(written),
                                 "%s%d", (i == 0) ? "" : ",", levelSequence[i]);
    }
    std::printf("\n  S6 level sequence: %s\n", sequence);
    // Crossing three boundaries means at least four levels are visited.
    TEST_ASSERT_TRUE_MESSAGE(distinctLevels >= 4,
                             "the sweep did not cross three level boundaries");

    // Largest adjacent-sample delta of the render, and the bound the tables
    // imply for it: the table is a polyline, so a phase advance of `h` points
    // can move at most `max |T[x + ceil(h)] - T[x]|`, and interpolation only
    // shrinks that. The crossfade's own Q8 residue adds at most one
    // 1/256 step of the adjacent-level difference.
    int32_t maxDelta = 0;
    int32_t maxDeltaAtLevelChange = 0;
    int32_t worstWindowStep = 0;
    for (int i = 1; i < count; ++i) {
        const int32_t delta = std::abs(
            static_cast<int32_t>(pcm[static_cast<size_t>(i)]) -
            static_cast<int32_t>(pcm[static_cast<size_t>(i - 1)]));
        maxDelta = std::max(maxDelta, delta);
        if (levels[static_cast<size_t>(i)] != levels[static_cast<size_t>(i - 1)]) {
            maxDeltaAtLevelChange = std::max(maxDeltaAtLevelChange, delta);
        }
    }
    for (int level = 0; level < kLevels; ++level) {
        // 880 Hz at 44 100 is 5.11 table points per sample; one more point
        // covers the boundary case.
        worstWindowStep = std::max(worstWindowStep, maxTableWindowStep(level, 6));
    }
    const int32_t seamResidue = (maxAdjacentLevelDifference() + 255) / 256;
    const int32_t bound =
        (int32_t)((static_cast<int64_t>(worstWindowStep + seamResidue + 4) *
                   kMixScaleQ15) >> kMixScaleShift);
    // What a hard switch would be allowed to jump by at a boundary: the whole
    // adjacent-level difference. The crossfade keeps the observed delta at a
    // level change orders below this.
    const int32_t hardSwitchJump =
        (maxAdjacentLevelDifference() * kMixScaleQ15) >> kMixScaleShift;
    std::printf("  S6 largest delta %d LSB (at a level change %d), table-derived bound %d LSB; "
                "hard-switch jump would reach %d LSB, crossfade residue %d\n",
                maxDelta, maxDeltaAtLevelChange, bound, hardSwitchJump,
                seamResidue);
    TEST_ASSERT_TRUE_MESSAGE(maxDelta <= bound,
                             "adjacent-sample delta exceeded the table-derived bound");
    TEST_ASSERT_TRUE_MESSAGE(maxDeltaAtLevelChange < hardSwitchJump,
                             "a level change jumped as far as a hard table switch");

    // The continuous definition, sample for sample. This is the S6 pin that a
    // hard table switch cannot pass: at a boundary a switch would jump by the
    // whole adjacent-level difference, while the crossfade stays inside the
    // interpolation rounding. The absolute delta above is dominated by the
    // saw's own discontinuity at the top of the sweep, so it cannot see that.
    uint32_t phase = 0u;
    int32_t worstReferenceDelta = 0;
    int firstMismatch = -1;
    for (int i = 0; i < count; ++i) {
        // The reference uses the increment the voice held for this sample,
        // captured before generating it, so the phase trajectory is the
        // renderer's own and no sweep formula is re-derived here.
        const int32_t expected = hybridSawQ15(phase, phaseIncHistory[static_cast<size_t>(i)]);
        const int32_t actual = static_cast<int32_t>(pcm[static_cast<size_t>(i)]);
        const int32_t mapped = toPcm(expected);
        if (firstMismatch < 0 && std::abs(actual - mapped) > 2) {
            firstMismatch = i;
            std::printf("  first mismatch at %d: actual %d expected %d "
                        "(phaseinc %u phase %u level %d)\n", i, actual, mapped,
                        phaseIncHistory[static_cast<size_t>(i)], phase,
                        ApuCore::sawMipmapLevelForTesting(
                            phaseIncHistory[static_cast<size_t>(i)]));
        }
        worstReferenceDelta = std::max(worstReferenceDelta,
                                       std::abs(actual - mapped));
        TEST_ASSERT_TRUE_MESSAGE(std::abs(actual - mapped) <= 2,
                                 "Hybrid SAW left the continuous crossfade definition");
        phase += phaseIncHistory[static_cast<size_t>(i)];
    }
    std::printf("  S6 worst deviation from the continuous definition: %d LSB\n",
                worstReferenceDelta);
}

void test_hybrid_saw_fast_sweep_stays_bounded(void)
{
    // 55 Hz -> 8 kHz in 20 ms: a deliberately violent jump through every level.
    // The output must stay inside the mix-scale range, which a table-selection
    // bug that summed instead of blending (or wrapped) would break.
    const InstrumentPreset preset = flatPreset();
    const AudioCommand cmd = noteCommand(WaveType::SAW, 55.0f, true, 8000.0f,
                                         0.02f, SweepCurve::Linear, preset);
    const std::vector<int16_t> pcm = render(SoundProfile::Hybrid, cmd,
                                            kSampleRate / 2);
    int peak = 0;
    for (int16_t sample : pcm) {
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    // 32767 * 13107 / 32768 = 13106 is the largest PCM the unshaped path can
    // emit; anything above it means an overflow or a summed crossfade.
    std::printf("\n  S6 fast sweep peak %d LSB (unshaped limit 13106)\n", peak);
    TEST_ASSERT_TRUE_MESSAGE(peak > 0, "the fast sweep rendered silence");
    TEST_ASSERT_TRUE_MESSAGE(peak <= 13106,
                             "the fast sweep left the unshaped output range");
}

void test_hybrid_sine_is_not_band_limited(void)
{
    // L5: SINE stays on SINE_LUT_Q15 on every profile. `Hybrid` must be byte
    // identical to `Nes` for it, which is the same statement.
    static const float frequencies[] = {110.0f, 440.0f, 1760.0f, 7040.0f};
    constexpr int kCount = 1024;
    for (float frequency : frequencies) {
        const InstrumentPreset preset = flatPreset();
        const AudioCommand cmd = noteCommand(WaveType::SINE, frequency, true,
                                             0.0f, 0.0f, SweepCurve::Linear,
                                             preset);
        const std::vector<int16_t> hybrid = render(SoundProfile::Hybrid, cmd, kCount);
        const std::vector<int16_t> nes = render(SoundProfile::Nes, cmd, kCount);
        char message[128];
        std::snprintf(message, sizeof(message),
                      "Hybrid SINE diverged from Nes at %.1f Hz", frequency);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0, std::memcmp(hybrid.data(), nes.data(),
                           static_cast<size_t>(kCount) * sizeof(int16_t)),
            message);
    }
}

// =============================================================================
// S5 end to end - the improvement the profile delivers, on rendered PCM
// =============================================================================

void test_hybrid_saw_alias_improvement_through_the_renderer()
{
    double worstImprovementDb = 1e9;
    RenderPoint worstPoint;
    double worstNesDb = 0.0;
    double worstHybridDb = 0.0;
    bool worstFloorClause = false;
    bool worstImprovementClause = false;
    int measured = 0;
    char message[320];

    std::printf("\n  S5 end to end: SAW through ApuCore, output stage bypassed "
                "(setHpfCutoffHz(0), SoftClipMode::None)\n");
    std::printf("  dB below the fundamental, relative to it; lower is better\n");
    std::printf("  rate    freq      Nes      Hybrid   improvement\n");

    for (int rate : kRenderRates) {
        for (int index = 0; index < kRenderPoints; ++index) {
            const double target = kRenderTargetsHz[index];
            RenderPoint point;
            std::snprintf(message, sizeof(message),
                          "no coherent grid point near %.2f Hz at %d Hz", target, rate);
            TEST_ASSERT_TRUE_MESSAGE(nearestCoherentPoint(target, rate, point), message);

            const RenderFold nes = measureRender(SoundProfile::Nes, point);
            const RenderFold hybrid = measureRender(SoundProfile::Hybrid, point);
            const double improvement = nes.foldDownDb - hybrid.foldDownDb;
            ++measured;

            std::printf("  %5d  %7.2f   %7.2f   %7.2f   %6.2f dB\n", point.sampleRate,
                        point.frequencyHz, nes.foldDownDb, hybrid.foldDownDb,
                        improvement);

            // A record that rendered silence would satisfy the floor clause
            // for free, so the note has to be there before the number counts.
            std::snprintf(message, sizeof(message),
                          "record at %.2f Hz / %d Hz peaked at %.0f LSB: the note did not render",
                          point.frequencyHz, point.sampleRate,
                          std::min(nes.peakLsb, hybrid.peakLsb));
            TEST_ASSERT_TRUE_MESSAGE(
                std::min(nes.peakLsb, hybrid.peakLsb) >= kMinPlausiblePeakLsb, message);

            // S5: at least 10 dB better, or the mipmapped figure is already at
            // or below the floor the S4 case pins (-60 dB), where "better" has
            // nothing left to buy.
            const bool floorClause = hybrid.foldDownDb <= kFoldFloorDb;
            const bool improvementClause = improvement >= kMinImprovementDb;
            std::snprintf(message, sizeof(message),
                          "%.2f Hz / %d Hz: Hybrid %.2f dB vs Nes %.2f dB -> %.2f dB "
                          "(need %.1f dB better, or Hybrid at or below %.0f dB)",
                          point.frequencyHz, point.sampleRate, hybrid.foldDownDb,
                          nes.foldDownDb, improvement, kMinImprovementDb,
                          kFoldFloorDb);
            TEST_ASSERT_TRUE_MESSAGE(floorClause || improvementClause, message);

            if (improvement < worstImprovementDb) {
                worstImprovementDb = improvement;
                worstPoint = point;
                worstNesDb = nes.foldDownDb;
                worstHybridDb = hybrid.foldDownDb;
                worstFloorClause = floorClause;
                worstImprovementClause = improvementClause;
            }
        }
    }

    std::printf("  worst of %d points: %.2f dB better at %.2f Hz / %d Hz "
                "(Nes %.2f dB, Hybrid %.2f dB; cleared by %s)\n",
                measured, worstImprovementDb, worstPoint.frequencyHz,
                worstPoint.sampleRate, worstNesDb, worstHybridDb,
                worstImprovementClause ? "the 10 dB clause"
                                       : "the absolute floor clause");
    TEST_ASSERT_TRUE(measured >= 16);
    std::snprintf(message, sizeof(message),
                  "worst S5 point %.2f Hz / %d Hz: Hybrid %.2f dB vs Nes %.2f dB is "
                  "only %.2f dB better (floor clause %s, 10 dB clause %s)",
                  worstPoint.frequencyHz, worstPoint.sampleRate, worstHybridDb,
                  worstNesDb, worstImprovementDb,
                  worstFloorClause ? "met" : "missed",
                  worstImprovementClause ? "met" : "missed");
    TEST_ASSERT_TRUE_MESSAGE(worstImprovementClause || worstFloorClause, message);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hybrid_saw_reads_the_tables_and_others_keep_the_ramp);
    RUN_TEST(test_hybrid_saw_level_selection_matches_the_band_rule);
    RUN_TEST(test_hybrid_saw_glissando_crosses_levels_without_a_step);
    RUN_TEST(test_hybrid_saw_fast_sweep_stays_bounded);
    RUN_TEST(test_hybrid_sine_is_not_band_limited);
    RUN_TEST(test_hybrid_saw_alias_improvement_through_the_renderer);
    return UNITY_END();
}
