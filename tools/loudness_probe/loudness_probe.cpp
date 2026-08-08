/**
 * @file loudness_probe.cpp
 * @brief Renders a real music-demo track through the APU and reports the
 *        output level, so the float and Q15 paths can be compared objectively.
 *
 * Why this exists
 * ---------------
 * Hito 5 (M12) unified the loudness of the integer/Q15 render path with the
 * float one. Q15 is what FPU-less targets run — notably the ESP32-C3, which is
 * the board `examples/music-demo -e esp32c3` builds for. The change makes that
 * path roughly 4.4x louder, so it has to be checked for clipping and for
 * parity with the float path.
 *
 * The render path is selected at COMPILE time by `PR32_APU_HAS_FPU`
 * (ApuConfig.h), which is a plain compile definition. So the exact integer
 * arithmetic the C3 executes can be run right here on the host — no hardware
 * needed for the part that actually matters. Build this file twice, once
 * normally and once with `-DPR32_APU_HAS_FPU=0`, and compare.
 *
 * What "correct" looks like after M12: peak and RMS should MATCH between the
 * two builds (that parity is the point of the milestone), and neither should
 * be pinning samples at the rails.
 *
 * `run.py` next to this file builds both variants and prints the comparison.
 *
 * The numbers here describe the mixer, not the hardware. A real C3 still has
 * its own amplifier and speaker; this tells you whether the signal you are
 * feeding them is right before you go listen to it.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

// The music-demo assets. They are generated headers that depend only on the
// APU types (through the engine's re-export shims), so they can be replayed
// outside the engine and outside SDL.
#include "assets/blaster_ridge.h"
#include "assets/coinleaf_grove.h"
#include "assets/lightworld_march.h"
#include "assets/moonwell_hymn.h"

using namespace pixelroot32::audio;

namespace {

struct Track {
    const char* name;
    const MusicTrack* track;
    float bpm;
};

const Track kTracks[] = {
    {"moonwell_hymn",    &musicdemo::moonwell_hymn::kTrack,    musicdemo::moonwell_hymn::kBpm},
    {"lightworld_march", &musicdemo::lightworld_march::kTrack, musicdemo::lightworld_march::kBpm},
    {"blaster_ridge",    &musicdemo::blaster_ridge::kTrack,    musicdemo::blaster_ridge::kBpm},
    {"coinleaf_grove",   &musicdemo::coinleaf_grove::kTrack,   musicdemo::coinleaf_grove::kBpm},
};
const int kTrackCount = int(sizeof(kTracks) / sizeof(kTracks[0]));

struct Stats {
    int32_t peak = 0;
    int64_t sumSq = 0;
    int64_t rails = 0;     // samples sitting on +32767 / -32768
    int64_t count = 0;
    std::vector<int16_t>* wav = nullptr;
};

// Runs on the exact stream the hardware would receive: after mixing, the
// soft-clip curve, the DC blocker and the bitcrush.
void capture(int16_t* mono, int length, void* user) {
    Stats* s = static_cast<Stats*>(user);
    for (int i = 0; i < length; ++i) {
        const int32_t v = mono[i];
        const int32_t mag = (v < 0) ? -v : v;
        if (mag > s->peak) s->peak = mag;
        s->sumSq += int64_t(v) * int64_t(v);
        if (v >= 32767 || v <= -32768) ++s->rails;
        ++s->count;
    }
    if (s->wav) s->wav->insert(s->wav->end(), mono, mono + length);
}

double dbfs(double linear) {
    if (linear <= 0.0) return -INFINITY;
    return 20.0 * std::log10(linear / 32768.0);
}

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm, int rate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t dataBytes = uint32_t(pcm.size() * sizeof(int16_t));
    const uint32_t byteRate = uint32_t(rate) * 2u;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36u + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16u); u16(1); u16(1);
    u32(uint32_t(rate)); u32(byteRate); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    std::fwrite(pcm.data(), 1, dataBytes, f);
    std::fclose(f);
    return true;
}

[[noreturn]] void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--track NAME] [--rate HZ] [--seconds N] [--volume F] [--wav PATH]\n"
        "  --track    one of: moonwell_hymn, lightworld_march, blaster_ridge,\n"
        "             coinleaf_grove (default moonwell_hymn)\n"
        "  --rate     sample rate, default 44100 (what DefaultAudioScheduler uses)\n"
        "  --seconds  render duration, default 20\n"
        "  --volume   master volume 0..1, default 1.0 — this is the knob you are\n"
        "             calibrating\n"
        "  --wav      also write the rendered audio here, for the listening A/B\n", argv0);
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    std::string trackName = "moonwell_hymn";
    std::string wavPath;
    int rate = 44100;
    double seconds = 20.0;
    float volume = 1.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1 < argc);
        if (a == "--track" && hasNext)        trackName = argv[++i];
        else if (a == "--rate" && hasNext)    rate = std::atoi(argv[++i]);
        else if (a == "--seconds" && hasNext) seconds = std::atof(argv[++i]);
        else if (a == "--volume" && hasNext)  volume = float(std::atof(argv[++i]));
        else if (a == "--wav" && hasNext)     wavPath = argv[++i];
        else usage(argv[0]);
    }

    const Track* chosen = nullptr;
    for (int i = 0; i < kTrackCount; ++i) {
        if (trackName == kTracks[i].name) { chosen = &kTracks[i]; break; }
    }
    if (!chosen || rate <= 0 || seconds <= 0.0) usage(argv[0]);

    std::vector<int16_t> wav;
    Stats stats;
    if (!wavPath.empty()) {
        wav.reserve(size_t(seconds * rate));
        stats.wav = &wav;
    }

    ApuCore apu;
    apu.init(rate);
    apu.reset();
    apu.setPostMixMono(&capture, &stats);

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = chosen->bpm;
    apu.submitCommand(bpm);

    AudioCommand vol{};
    vol.type = AudioCommandType::SET_MASTER_VOLUME;
    vol.volume = volume;
    apu.submitCommand(vol);

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = chosen->track;
    play.subTrackCount = 0;   // sub-voices are wired inside MusicTrack itself
    apu.submitCommand(play);

    // Block size is irrelevant to the result — the mixer is sample-by-sample —
    // but a realistic one keeps the sequencer's per-block work representative.
    const int kBlock = 512;
    const long long total = (long long)(seconds * rate);
    std::vector<int16_t> buffer(kBlock);
    for (long long done = 0; done < total; done += kBlock) {
        const int n = int((total - done < kBlock) ? (total - done) : kBlock);
        apu.generateSamples(buffer.data(), n);
    }

    const double rms = (stats.count > 0)
        ? std::sqrt(double(stats.sumSq) / double(stats.count)) : 0.0;
    const double railsPpm = (stats.count > 0)
        ? (double(stats.rails) * 1e6 / double(stats.count)) : 0.0;

#if PR32_APU_HAS_FPU
    const char* pathName = "float";
#else
    const char* pathName = "q15";
#endif

    // Machine-readable on purpose: run.py parses this.
    std::printf("path=%s track=%s rate=%d seconds=%.1f volume=%.3f "
                "samples=%lld peak=%d peak_dbfs=%.2f rms=%.1f rms_dbfs=%.2f "
                "rails=%lld rails_ppm=%.1f\n",
                pathName, chosen->name, rate, seconds, double(volume),
                (long long)stats.count, stats.peak, dbfs(stats.peak),
                rms, dbfs(rms), (long long)stats.rails, railsPpm);

    if (!wavPath.empty()) {
        if (!write_wav(wavPath, wav, rate)) {
            std::fprintf(stderr, "error: could not write %s\n", wavPath.c_str());
            return 1;
        }
        std::fprintf(stderr, "wrote %s (%zu samples)\n", wavPath.c_str(), wav.size());
    }
    return 0;
}
