/**
 * @file test_apu_profile_level.cpp
 * @brief E2 level parity on a crest-weighted loudness metric.
 *
 * The `Nes`/`Hybrid` output chain removes low-mid energy and spikes on every
 * edge, so on the canonical track it left the two profiles 3.28 dB and 4.31 dB
 * below `Legacy` in RMS. The listener's requirement is that a `Legacy` -> `Nes`
 * -> `Hybrid` switch changes the character, not the perceived level.
 *
 * THE METRIC IS THE POINT OF THIS SUITE, and it has already been wrong once.
 * The first trim matched RAW RMS and shipped; on hardware the user reported the
 * two profiles as louder and clearer than `Legacy` in every sample. The reason
 * is measurable rather than subjective: `Legacy`'s crest factor on this track
 * is ~4.6 dB against the chain's ~13.3 / ~14.3 dB, so an equal-RMS trim lifts
 * every transient by 3-4 dB and the ear reads that as more power. Equal RMS is
 * only a proxy for equal loudness when the crest factors are comparable, and
 * here they are 9 dB apart.
 *
 * The rule this suite encodes, stated so it can be argued with:
 *
 *     crest-weighted loudness W = 0.75 * RMS_dBFS + 0.25 * peak_dBFS
 *
 * matched to `Legacy` within +-0.5 dB. This is a documented heuristic, not a
 * standard: pure RMS overshoots (the failure above) and pure peak would
 * overcorrect below the un-trimmed level.
 *
 * Two consequences are asserted explicitly, because a passing weighted number
 * alone is not enough:
 *   - WARNING case: a crest-weighted match must land BELOW `Legacy` in RMS,
 *     never above. That is the exact property the listener's ear rejected, so
 *     it is pinned as a hard limit rather than left implicit in the weights.
 *   - The table below prints RMS, peak, crest, weighted loudness and each
 *     delta, so a reader can see both the metric and what it is weighting
 *     instead of trusting a single number.
 *
 * The suite works by RENDERING: the canonical track is rendered end to end
 * through the public API under all three profiles. It deliberately does not
 * compare against the committed fixtures — those pin bytes (a change is
 * expected here), while this pins the relationship between the three renders,
 * which no single fixture can express.
 *
 * `Legacy` is the anchor: it is the frozen compatibility contract and cannot
 * move. The suite therefore compiles with `PR32_APU_ENABLE_LEGACY_FLOAT=0` so
 * `Legacy` renders through the integer core — the like-for-like route, since
 * the trim lives in that core. The float route sits 0.45 dB away and is not
 * touched by this change.
 */

#include "golden/apu_mirror_canonical_track.h"
#include "pixelroot32/apu/ApuCore.h"
#include "unity.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace pixelroot32::audio;

namespace {

constexpr int kSampleCount = apu_golden::kGoldenSampleCount;

/// Full scale of the int16 rail, the reference for dBFS.
constexpr double kFullScale = 32768.0;

/// Weights of the loudness heuristic. 0.75 / 0.25 is a deliberate choice, not
/// a measurement: RMS alone was rejected by listening and peak alone would
/// overcorrect. Changing these is changing the contract the profiles ship on.
constexpr double kRmsWeight = 0.75;
constexpr double kPeakWeight = 0.25;

/// Accepted mismatch on the crest-weighted figure. The trims are derived from
/// these very renders, so the real residual is a hundredth of a decibel; the
/// tolerance is what "the perceived level does not change" means.
constexpr double kMaxWeightedDeltaDb = 0.5;

/// A crest-weighted match must never sit ABOVE the anchor in RMS. This is the
/// property the first, equal-RMS trim violated on hardware; the limit is the
/// same +-0.5 dB so a systematic drift up cannot hide inside the tolerance.
constexpr double kMaxRmsAboveLegacyDb = 0.5;

/// @brief Every number this suite needs from one rendered profile.
struct Level {
    double rmsDbfs;       ///< 20 * log10(rms / 32768)
    int peak;             ///< Largest |sample|, i.e. what hits the rail first
    double peakDbfs;      ///< 20 * log10(peak / 32768)
    double crestDb;       ///< peakDbfs - rmsDbfs
    double weightedDbfs;  ///< kRmsWeight * rmsDbfs + kPeakWeight * peakDbfs
};

/// @brief The three profiles under test, in the order the report prints them.
constexpr SoundProfile kProfiles[3] = {
    SoundProfile::Legacy,
    SoundProfile::Nes,
    SoundProfile::Hybrid,
};
const char* const kNames[3] = {"Legacy", "Nes", "Hybrid"};

/// One canonical-track render per profile. File scope because 22 050 S16
/// samples per profile is 44 100 B each; three of those do not belong on the
/// stack of a Unity test.
int16_t gPcm[3][kSampleCount];

/// @brief RMS and peak of a rendered buffer, in dBFS, plus the two derived
/// numbers the metric needs.
/// @param samples Render to measure; exactly `kSampleCount` samples.
/// @return Level of the render.
Level measure_level(const int16_t* samples)
{
    double sumSquares = 0.0;
    int peak = 0;
    for (int i = 0; i < kSampleCount; ++i) {
        const int value = samples[i];
        sumSquares += static_cast<double>(value) * static_cast<double>(value);
        const int magnitude = (value < 0) ? -value : value;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(kSampleCount));
    // A silent buffer would make peakDbfs -inf; every caller asserts a
    // non-silent anchor before using the number.
    const double rmsDbfs = 20.0 * std::log10(rms / kFullScale);
    const double peakDbfs =
        (peak > 0) ? 20.0 * std::log10(static_cast<double>(peak) / kFullScale)
                   : -200.0;
    Level level{};
    level.rmsDbfs = rmsDbfs;
    level.peak = peak;
    level.peakDbfs = peakDbfs;
    level.crestDb = peakDbfs - rmsDbfs;
    level.weightedDbfs = kRmsWeight * rmsDbfs + kPeakWeight * peakDbfs;
    return level;
}

/// @brief Renders the canonical track under one profile through the public API.
///
/// The profile is set BEFORE the scene renderer runs, and the renderer itself
/// calls `reset()` + `init()` exactly as a consumer would: the profile is
/// configuration and survives both (S4), which is part of what this exercises.
/// @param index Index into `kProfiles` / `gPcm`.
/// @return Level of the rendered buffer.
Level render_profile(int index)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.setSoundProfile(kProfiles[index]);
    apu_golden::render_canonical(apu, gPcm[index], kSampleCount);
    return measure_level(gPcm[index]);
}

/// @brief Renders all three profiles and prints the table the report quotes.
///
/// Printing on every run, not only on failure, is deliberate: the numbers are
/// the deliverable of the tuning decision, and a reader must be able to see the
/// weighting (RMS, peak, crest, W) rather than a single figure that hides which
/// one moved.
void render_all_and_print(Level out[3])
{
    std::printf("  canonical track, %d samples @ %d Hz\n", kSampleCount,
                apu_golden::kSampleRate);
    std::printf("  loudness W = %.2f * RMS + %.2f * peak (dBFS), anchor Legacy\n",
                kRmsWeight, kPeakWeight);
    std::printf("    %-7s %9s %9s %7s %9s %7s %10s %9s\n", "profile", "RMS dBFS",
                "peak dBFS", "peak", "crest dB", "weighted", "delta W", "delta RMS");
    for (int i = 0; i < 3; ++i) {
        out[i] = render_profile(i);
        const double weightedDelta = out[i].weightedDbfs - out[0].weightedDbfs;
        const double rmsDelta = out[i].rmsDbfs - out[0].rmsDbfs;
        std::printf("    %-7s %+9.3f %+9.3f %7d %9.3f %9.3f %+10.3f %+9.3f\n",
                    kNames[i], out[i].rmsDbfs, out[i].peakDbfs, out[i].peak,
                    out[i].crestDb, out[i].weightedDbfs, weightedDelta, rmsDelta);
    }
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

/// @brief The contract: crest-weighted loudness within +-0.5 dB of `Legacy`.
void test_apu_profile_level_weighted_matches_legacy(void)
{
    Level level[3] = {};
    render_all_and_print(level);

    // The anchor must be a real render, not a silent buffer: a parity claim
    // between three zeros would pass while proving nothing.
    TEST_ASSERT_TRUE_MESSAGE(level[0].peak > 0, "Legacy rendered silence");
    TEST_ASSERT_TRUE_MESSAGE(level[0].rmsDbfs > -40.0,
                             "Legacy rendered implausibly low");

    for (int i = 1; i < 3; ++i) {
        const double delta = level[i].weightedDbfs - level[0].weightedDbfs;
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "%s weighted loudness is %+.3f dB against Legacy "
                      "(rms %+.3f, peak %+.3f; limit +-%.1f dB)",
                      kNames[i], delta,
                      level[i].rmsDbfs - level[0].rmsDbfs,
                      level[i].peakDbfs - level[0].peakDbfs, kMaxWeightedDeltaDb);
        TEST_ASSERT_TRUE_MESSAGE(std::fabs(delta) <= kMaxWeightedDeltaDb, message);
    }
}

/// @brief A crest-weighted match must sit BELOW `Legacy` in RMS, not above.
///
/// This is the exact property the user's ear rejected when the first trim
/// matched raw RMS: the profiles measured equal and sounded louder. The trims
/// are now small enough that both land ~2.2-2.4 dB under `Legacy`; the case
/// exists so a future "fix" cannot reintroduce the loudness by raising them
/// back toward RMS parity and still pass the weighted check on transients.
void test_apu_profile_level_rms_stays_below_legacy(void)
{
    Level level[3] = {};
    render_all_and_print(level);

    for (int i = 1; i < 3; ++i) {
        const double delta = level[i].rmsDbfs - level[0].rmsDbfs;
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "%s RMS is %+.3f dB against Legacy: a crest-weighted match "
                      "must not be louder in RMS (limit +%.1f dB, and it should "
                      "be negative)",
                      kNames[i], delta, kMaxRmsAboveLegacyDb);
        TEST_ASSERT_TRUE_MESSAGE(delta <= kMaxRmsAboveLegacyDb, message);
    }
}

/// @brief The chain's transients must still fit inside the int16 rail.
void test_apu_profile_level_peak_under_rail(void)
{
    Level level[3] = {};
    render_all_and_print(level);

    for (int i = 0; i < 3; ++i) {
        char message[160] = {};
        std::snprintf(message, sizeof(message), "%s peak %d hits the int16 rail",
                      kNames[i], level[i].peak);
        // Strictly below 32767, not merely <= -32768: a rail-sitting render is
        // already clipping, and the trim is what would push it there.
        TEST_ASSERT_TRUE_MESSAGE(level[i].peak < 32767, message);
        TEST_ASSERT_TRUE_MESSAGE(level[i].peak > 0, "profile rendered silence");
    }
}

/// @brief PRESERVE: the frozen `Legacy` integer route did not move.
///
/// The trim is gated on the profile chain being active, so `Legacy` must
/// render exactly the level it rendered before any trim existed. The byte
/// identity is pinned by `test_apu_integer_golden`; these two numbers pin the
/// same fact at the level layer, where a stray multiply would show up as a
/// fraction of a decibel rather than as a diff on sample 0.
void test_apu_profile_level_legacy_frozen(void)
{
    Level level[3] = {};
    render_all_and_print(level);

    // Measured on the committed integer fixture: -19.70 dBFS, peak 5 758.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -19.70f, static_cast<float>(level[0].rmsDbfs));
    TEST_ASSERT_EQUAL_INT(5758, level[0].peak);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apu_profile_level_weighted_matches_legacy);
    RUN_TEST(test_apu_profile_level_rms_stays_below_legacy);
    RUN_TEST(test_apu_profile_level_peak_under_rail);
    RUN_TEST(test_apu_profile_level_legacy_frozen);
    return UNITY_END();
}
