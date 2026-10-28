/// test_apu_profile_golden.cpp - S7 goldens of the two audible profiles.
///
/// E1 gives `Nes` and `Hybrid` their own character — envelope curve, weighted
/// mix, unipolar noise on the NES period LUT and stepped PULSE duty — so
/// neither can share `Legacy`'s fixture any more: each profile is pinned by one
/// committed render of the canonical track, and `Legacy` keeps its own two
/// references untouched.
///
/// Both profiles render the integer core by definition (`profileUsesIntegerCore`),
/// so the two targets that compile this source differ only in the core
/// configuration: an FPU build with the float body compiled out, and a build
/// with no float body at all. The core is the same code on both, therefore the
/// bytes must be the same on both — a difference between the two targets means
/// the integer core still depends on the build, which is the defect the fixture
/// exists to catch. See `test_apu_profile_golden` /
/// `test_apu_profile_golden_no_fpu` in CMakeLists.txt.

#include "golden/apu_mirror_canonical_track.h"
#include "pixelroot32/apu/ApuCore.h"
#include "unity.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pixelroot32::audio;

/// @brief One committed reference: the file, the profile that must reproduce
/// it, and the label used in the failure message.
struct ProfileFixture {
    const char* file;
    SoundProfile profile;
    const char* label;
};

/// @brief The two profile goldens of E1, one per audible profile (S7).
static const ProfileFixture kFixtures[] = {
    {"apu_nes_canonical_s16le_44100.pcm", SoundProfile::Nes, "Nes"},
    {"apu_hybrid_canonical_s16le_44100.pcm", SoundProfile::Hybrid, "Hybrid"},
};

/// @brief S16 samples per fixture; both profiles render the same canonical
/// track, so the sample count is shared with the `Legacy` fixtures.
static constexpr int kSampleCount = apu_golden::kGoldenSampleCount;

// The fixture format is fixed by S9 (raw S16LE); if the sample type or the
// sample count ever changed, the committed files could not describe the render.
static_assert(sizeof(int16_t) == 2,
              "the golden fixtures are raw 16-bit little-endian PCM");
static_assert(sizeof(int16_t) * apu_golden::kGoldenSampleCount == 44100,
              "each fixture is 22 050 S16 samples = 44 100 bytes");

static std::string fixture_dir;

extern "C" void setUp(void)
{
    fixture_dir = std::filesystem::current_path().string();
    for (const auto& candidate : {
             fixture_dir + "/golden/fixtures",
             fixture_dir + "/../test/golden/fixtures",
             fixture_dir + "/../golden/fixtures",
         }) {
        if (std::filesystem::exists(candidate + "/" + kFixtures[0].file)) {
            fixture_dir = candidate;
            break;
        }
    }
}

extern "C" void tearDown(void) {}

static std::vector<uint8_t> read_fixture_bytes(const std::string& name)
{
    const std::string path = fixture_dir + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

/// @brief Renders the canonical track under one profile and compares it with
/// that profile's committed fixture.
///
/// A divergence is reported at the first differing sample with both values,
/// because "the golden moved" is only actionable when the reader can see the
/// sample where the render left the reference.
/// @param fixture Reference, profile and label to assert.
static void assert_profile_golden(const ProfileFixture& fixture)
{
    const auto bytes = read_fixture_bytes(fixture.file);
    if (bytes.empty()) {
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "missing %s golden PCM fixture (expected %s)",
                      fixture.label, fixture.file);
        TEST_FAIL_MESSAGE(message);
        return;
    }

    const size_t expected_bytes = static_cast<size_t>(kSampleCount) * sizeof(int16_t);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected_bytes),
                          static_cast<int>(bytes.size()));

    int16_t actual[kSampleCount] = {};
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    // After init() and before rendering: the profile is configuration, so it
    // survives the reset()/init() that render_canonical performs (S4).
    apu.setSoundProfile(fixture.profile);
    apu_golden::render_canonical(apu, actual, kSampleCount);

    const int cmp = std::memcmp(actual, bytes.data(), expected_bytes);
    if (cmp != 0) {
        const auto* expected = reinterpret_cast<const int16_t*>(bytes.data());
        int first = 0;
        while (first < kSampleCount && actual[first] == expected[first]) {
            ++first;
        }
        char message[224] = {};
        std::snprintf(message, sizeof(message),
                      "%s bytes diverged from its canonical fixture at sample "
                      "%d: actual %d, expected %d",
                      fixture.label, first, static_cast<int>(actual[first]),
                      static_cast<int>(expected[first]));
        TEST_FAIL_MESSAGE(message);
    }
    TEST_ASSERT_EQUAL_INT(0, cmp);
}

/// @brief S7: `Nes` renders the bytes of its own committed reference.
void test_apu_profile_golden_nes(void)
{
    assert_profile_golden(kFixtures[0]);
}

/// @brief S7: `Hybrid` renders the bytes of its own committed reference.
void test_apu_profile_golden_hybrid(void)
{
    assert_profile_golden(kFixtures[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apu_profile_golden_nes);
    RUN_TEST(test_apu_profile_golden_hybrid);
    return UNITY_END();
}
