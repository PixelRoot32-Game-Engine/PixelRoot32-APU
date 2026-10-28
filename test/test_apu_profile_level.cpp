/**
 * @file test_apu_profile_level.cpp
 * @brief E2 level parity: switching profile must not be a level change.
 *
 * The `Nes`/`Hybrid` output chain removes low-mid energy and spikes on every
 * edge, so on the canonical track it left the two profiles 3.28 dB and 4.31 dB
 * below `Legacy`. The listener's requirement is that a `Legacy` -> `Nes` ->
 * `Hybrid` switch changes the character, not the perceived level, so each
 * profile carries a fixed trim applied after the chain.
 *
 * This suite is the acceptance test for that trim and it works by RENDERING:
 * the canonical track is rendered end to end through the public API under all
 * three profiles and the level is measured on the samples that come out. It
 * deliberately does not compare against the committed fixtures — those pin
 * bytes (a change is expected here), while this pins the relationship between
 * the three renders, which no single fixture can express.
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

/// Accepted mismatch. The trim is derived from the whole-track RMS of these
/// very renders, so the real residual is a hundredth of a decibel; the
/// tolerance is what "the perceived level does not change" means, not the
/// measurement error.
constexpr double kMaxLevelDeltaDb = 0.5;

/// @brief Rendered level of one profile on the canonical track.
struct Level {
    double rmsDbfs;  ///< 20 * log10(rms / 32768)
    int peak;        ///< Largest |sample|, i.e. what hits the rail first
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

/// @brief RMS in dBFS and peak of a rendered buffer.
/// @param samples Render to measure; exactly `kSampleCount` samples.
/// @return The two numbers the parity assertion and the report need.
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
    return Level{20.0 * std::log10(rms / kFullScale), peak};
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
/// the deliverable of the tuning decision and a reader should be able to
/// reproduce them with one command.
void render_all_and_print(Level out[3])
{
    std::printf("  canonical track level, %d samples @ %d Hz\n", kSampleCount,
                apu_golden::kSampleRate);
    for (int i = 0; i < 3; ++i) {
        out[i] = render_profile(i);
        const double delta = out[i].rmsDbfs - out[0].rmsDbfs;
        std::printf("    %-7s rms %+8.3f dBFS  peak %5d  delta %+6.3f dB\n",
                    kNames[i], out[i].rmsDbfs, out[i].peak, delta);
    }
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

/// @brief The contract: `Nes` and `Hybrid` are within +-0.5 dB of `Legacy`.
void test_apu_profile_level_rms_matches_legacy(void)
{
    Level level[3] = {};
    render_all_and_print(level);

    // The anchor must be a real render, not a silent buffer: a parity claim
    // between three zeros would pass while proving nothing.
    TEST_ASSERT_TRUE_MESSAGE(level[0].peak > 0, "Legacy rendered silence");
    TEST_ASSERT_TRUE_MESSAGE(level[0].rmsDbfs > -40.0,
                             "Legacy rendered implausibly low");

    for (int i = 1; i < 3; ++i) {
        const double delta = level[i].rmsDbfs - level[0].rmsDbfs;
        char message[160] = {};
        std::snprintf(message, sizeof(message),
                      "%s is %+.3f dB against Legacy (limit +-%.1f dB)",
                      kNames[i], delta, kMaxLevelDeltaDb);
        TEST_ASSERT_TRUE_MESSAGE(std::fabs(delta) <= kMaxLevelDeltaDb, message);
    }
}

/// @brief Matching RMS raises the peak: it must still stay inside the rail.
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
/// render exactly the level it rendered before the trim existed. The byte
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
    RUN_TEST(test_apu_profile_level_rms_matches_legacy);
    RUN_TEST(test_apu_profile_level_peak_under_rail);
    RUN_TEST(test_apu_profile_level_legacy_frozen);
    return UNITY_END();
}
