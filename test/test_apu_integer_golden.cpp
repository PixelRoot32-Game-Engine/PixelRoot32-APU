/// test_apu_integer_golden.cpp — S9 golden PCM of the integer render core.
///
/// The integer core must produce exactly the bytes in the committed fixture no
/// matter which build reaches it. An FPU build gets there through the `Nes`
/// profile (the frozen `Legacy` body stays float); a no-FPU build gets there
/// through `Legacy`. That is why this one source is compiled twice — see the
/// `test_apu_integer_golden` / `test_apu_integer_golden_no_fpu` targets in
/// CMakeLists.txt — and why the profile is selected per build instead of being
/// hard-coded: the assertion is about the core, not about the profile name.

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

static constexpr const char* kIntegerFixture =
    "apu_integer_core_canonical_s16le_44100.pcm";

// S9 fixes an S16LE fixture of the canonical track; if the sample type or the
// sample count ever changes, the committed file cannot describe the render.
static_assert(sizeof(int16_t) == 2,
              "the golden fixture is raw 16-bit little-endian PCM");
static_assert(sizeof(int16_t) * apu_golden::kGoldenSampleCount == 44100,
              "the S9 fixture is 22 050 S16 samples = 44 100 bytes");

static std::string fixture_dir;

extern "C" void setUp(void)
{
    fixture_dir = std::filesystem::current_path().string();
    for (const auto& candidate : {
             fixture_dir + "/golden/fixtures",
             fixture_dir + "/../test/golden/fixtures",
             fixture_dir + "/../golden/fixtures",
         }) {
        if (std::filesystem::exists(candidate + "/" + kIntegerFixture)) {
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

void test_apu_integer_golden_pcm_parity(void)
{
    const auto bytes = read_fixture_bytes(kIntegerFixture);
    TEST_ASSERT_TRUE_MESSAGE(!bytes.empty(), "missing integer core golden PCM fixture");

    const size_t expected_bytes =
        static_cast<size_t>(apu_golden::kGoldenSampleCount) * sizeof(int16_t);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected_bytes),
                          static_cast<int>(bytes.size()));

    // Profile selection follows the build, not the test: on an FPU target the
    // float body is what `Legacy` means, so only `Nes` routes to the integer
    // core there. Without an FPU both profiles already render integer, and
    // `Legacy` is the reference the fixture was generated from (S9).
#if PR32_APU_HAS_FPU
    const SoundProfile profile = SoundProfile::Nes;
#else
    const SoundProfile profile = SoundProfile::Legacy;
#endif

    int16_t actual[apu_golden::kGoldenSampleCount] = {};
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.setSoundProfile(profile);
    // render_canonical() resets and re-initialises the core, which is fine
    // here: S4 makes the profile configuration, so init() keeps it.
    apu_golden::render_canonical(apu, actual, apu_golden::kGoldenSampleCount);

    const int cmp = std::memcmp(actual, bytes.data(), expected_bytes);
    if (cmp != 0) {
        const auto* expected = reinterpret_cast<const int16_t*>(bytes.data());
        int first = 0;
        while (first < apu_golden::kGoldenSampleCount &&
               actual[first] == expected[first]) {
            ++first;
        }
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "integer core bytes diverged from the canonical fixture "
                      "at sample %d: actual %d, expected %d",
                      first, static_cast<int>(actual[first]),
                      static_cast<int>(expected[first]));
        TEST_FAIL_MESSAGE(message);
    }
    TEST_ASSERT_EQUAL_INT(0, cmp);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apu_integer_golden_pcm_parity);
    return UNITY_END();
}
