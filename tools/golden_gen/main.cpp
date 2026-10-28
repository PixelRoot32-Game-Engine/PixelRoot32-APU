/// golden_gen — renders the canonical track through a chosen sound profile and
/// writes the result as raw S16LE, the format the golden fixtures use.
///
/// The profile selects the render core (S3), and which core a build compiles is
/// exactly what S9 fixes: the same bytes must come out of an FPU build routing
/// `legacy` to the integer core through `PR32_APU_ENABLE_LEGACY_FLOAT=0` and out
/// of a no-FPU build. So this source is compiled three times (see
/// CMakeLists.txt) and tools/golden_gen/run.py compares what each build emitted
/// instead of trusting a single run.

#include "golden/apu_mirror_canonical_track.h"
#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioTypes.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

using pixelroot32::audio::ApuCore;
using pixelroot32::audio::SoundProfile;

/**
 * @brief Translates the CLI profile name into the library enumerator.
 * @param name Lower-case CLI value.
 * @param out Set to the matching profile when the name is known.
 * @return true when the name matched a profile.
 */
bool resolveProfile(const char* name, SoundProfile& out)
{
    if (std::strcmp(name, "legacy") == 0) {
        out = SoundProfile::Legacy;
        return true;
    }
    if (std::strcmp(name, "nes") == 0) {
        out = SoundProfile::Nes;
        return true;
    }
    if (std::strcmp(name, "hybrid") == 0) {
        out = SoundProfile::Hybrid;
        return true;
    }
    return false;
}

void usage()
{
    std::fprintf(stderr,
                 "usage: apu_golden_gen --profile <legacy|nes|hybrid> --out <path>\n"
                 "  --profile  sound profile to render with (required)\n"
                 "  --out      output file, raw S16LE PCM (required)\n");
}

}  // namespace

int main(int argc, char** argv)
{
    const char* profileName = nullptr;
    const char* outPath = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage();
            return 1;
        }
        if (std::strcmp(arg, "--profile") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --profile needs a value\n");
                usage();
                return 1;
            }
            profileName = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--out") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --out needs a value\n");
                usage();
                return 1;
            }
            outPath = argv[++i];
            continue;
        }
        std::fprintf(stderr, "error: unknown argument '%s'\n", arg);
        usage();
        return 1;
    }

    if (profileName == nullptr || outPath == nullptr) {
        std::fprintf(stderr, "error: --profile and --out are both required\n");
        usage();
        return 1;
    }

    SoundProfile profile{};
    if (!resolveProfile(profileName, profile)) {
        std::fprintf(stderr,
                     "error: unknown profile '%s' (expected legacy, nes or hybrid)\n",
                     profileName);
        usage();
        return 1;
    }

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    // After init() and before any rendering: the profile is configuration, so
    // it survives the reset()/init() that render_canonical performs below.
    apu.setSoundProfile(profile);

    const int count = apu_golden::kGoldenSampleCount;
    std::vector<int16_t> samples(static_cast<size_t>(count), 0);
    apu_golden::render_canonical(apu, samples.data(), count);

    // Serialise explicitly as little-endian instead of dumping host memory: the
    // fixture format is defined by S9, not by the build machine's byte order.
    std::vector<uint8_t> bytes(static_cast<size_t>(count) * sizeof(int16_t), 0);
    for (int i = 0; i < count; ++i) {
        const uint16_t value = static_cast<uint16_t>(samples[static_cast<size_t>(i)]);
        bytes[static_cast<size_t>(i) * 2u] = static_cast<uint8_t>(value & 0xFFu);
        bytes[static_cast<size_t>(i) * 2u + 1u] = static_cast<uint8_t>(value >> 8);
    }

    std::ofstream file(outPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "error: cannot open '%s' for writing\n", outPath);
        return 1;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        std::fprintf(stderr, "error: failed writing %zu bytes to '%s'\n",
                     bytes.size(), outPath);
        return 1;
    }

    std::printf("profile=%s samples=%d bytes=%zu\n", profileName, count, bytes.size());
    return 0;
}
