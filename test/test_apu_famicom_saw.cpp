/**
 * @file test_apu_famicom_saw.cpp
 * @brief S3 (Famicom profile): no profile reads the band-limited SAW.
 *
 * 2.2.x shipped value 2 of `SoundProfile` as `Hybrid`, whose SAW read the
 * band-limited mipmap of `AudioOscMipLUT.h`. Value 2 is now `Famicom`, which
 * is the `Nes` character with a different output stage, so its SAW is the
 * ideal ramp — aliasing included, as on the hardware. The mipmap read is
 * DORMANT: it still compiles (`generateBandLimitedSawSampleQ15`) and the
 * tables are still proven by `test_apu_wavetable_spectral.cpp`, but nothing
 * selects it until it is either removed or exposed as a per-voice option.
 *
 * The definitions under test are:
 *
 *   - every profile renders the ideal ramp sample for sample: `Legacy`,
 *     `Nes` and `Famicom` (the S3 regression this file used to pin the other
 *     way round);
 *   - SINE stays on `SINE_LUT_Q15` for every profile (L5);
 *   - the dormant level selector still answers the band rule of
 *     `AudioOscMipLUT.h`, so the code kept for a later decision is not rotting
 *     while it waits.
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
 * ramp exactly" is a statement about the integer core the other two profiles
 * use. See CMakeLists.txt.
 */

#include <unity.h>

#include <cmath>
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

/// @brief One looping note command.
AudioCommand noteCommand(WaveType type, float frequencyHz,
                         const InstrumentPreset& preset)
{
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = type;
    cmd.event.frequency = frequencyHz;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    cmd.event.preset = &preset;
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

/// @brief `generateSawSampleQ15`, the ideal ramp every profile renders.
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

} // namespace

void setUp(void) { test_setup(); }

void tearDown(void) { test_teardown(); }

// =============================================================================
// S3 - every profile renders the ideal ramp; the mipmap read is dormant
// =============================================================================

void test_every_profile_renders_the_ideal_saw_ramp(void)
{
    static const float frequencies[] = {220.0f, 440.0f, 880.0f, 1760.0f, 3520.0f};
    static const SoundProfile profiles[] = {
        SoundProfile::Legacy, SoundProfile::Nes, SoundProfile::Famicom};
    static const char* const names[] = {"Legacy", "Nes", "Famicom"};
    constexpr int kCount = 1024;

    for (float frequency : frequencies) {
        const InstrumentPreset preset = flatPreset();
        const AudioCommand cmd = noteCommand(WaveType::SAW, frequency, preset);
        const uint32_t increment = phaseIncFor(frequency, kSampleRate);

        for (int p = 0; p < 3; ++p) {
            const std::vector<int16_t> pcm = render(profiles[p], cmd, kCount);
            // The reference is the ideal ramp straight from the Q32
            // accumulator, then the integer core's single documented mix
            // scale. Sample 0 is the note-on sample, still at envelope zero.
            uint32_t phase = 0u;
            for (int i = 0; i < kCount; ++i) {
                const int16_t expected = (i == 0) ? 0 : toPcm(idealRampQ15(phase));
                char message[192];
                std::snprintf(message, sizeof(message),
                              "%s SAW at %.1f Hz diverged at sample %d: %d vs ramp %d",
                              names[p], static_cast<double>(frequency), i,
                              static_cast<int>(pcm[static_cast<size_t>(i)]),
                              static_cast<int>(expected));
                TEST_ASSERT_EQUAL_INT16_MESSAGE(expected,
                                                pcm[static_cast<size_t>(i)],
                                                message);
                phase += increment;
            }
        }
    }
}

// =============================================================================
// L5 - SINE is never band-limited
// =============================================================================

void test_famicom_sine_matches_nes(void)
{
    static const float frequencies[] = {110.0f, 440.0f, 1760.0f, 7040.0f};
    constexpr int kCount = 1024;
    for (float frequency : frequencies) {
        const InstrumentPreset preset = flatPreset();
        const AudioCommand cmd = noteCommand(WaveType::SINE, frequency, preset);
        const std::vector<int16_t> famicom = render(SoundProfile::Famicom, cmd, kCount);
        const std::vector<int16_t> nes = render(SoundProfile::Nes, cmd, kCount);
        char message[128];
        std::snprintf(message, sizeof(message),
                      "Famicom SINE diverged from Nes at %.1f Hz",
                      static_cast<double>(frequency));
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0, std::memcmp(famicom.data(), nes.data(),
                           static_cast<size_t>(kCount) * sizeof(int16_t)),
            message);
    }
}

// =============================================================================
// Dormant code - the level selector still answers the band rule
// =============================================================================

void test_dormant_saw_level_selection_matches_the_band_rule(void)
{
    // The clamps and the degenerate increment.
    TEST_ASSERT_EQUAL_INT(0, ApuCore::sawMipmapLevelForTesting(0u));
    TEST_ASSERT_EQUAL_INT(0, ApuCore::sawMipmapLevelForTesting(1u));
    TEST_ASSERT_EQUAL_INT(kLevels - 1,
                          ApuCore::sawMipmapLevelForTesting(0xFFFFFFFFu));
    // q >= 0.5 is above level 7's band and must clamp, not wrap.
    TEST_ASSERT_EQUAL_INT(kLevels - 1,
                          ApuCore::sawMipmapLevelForTesting(0x80000000u));

    // A dense sweep: every power-of-two and several mantissa steps, which is
    // the whole band at the resolution the selector sees.
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
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_profile_renders_the_ideal_saw_ramp);
    RUN_TEST(test_famicom_sine_matches_nes);
    RUN_TEST(test_dormant_saw_level_selection_matches_the_band_rule);
    return UNITY_END();
}
