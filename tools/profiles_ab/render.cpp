/// profiles_ab render — raw S16LE render of the blind A/B track under one
/// sound profile.
///
/// S9.4 asks for the SAME track rendered through `Nes`, `Hybrid` and `Legacy`
/// so a listener can say which is which. This binary renders one profile and
/// writes raw S16LE; `run.py` owns everything a blind comparison needs and that
/// C++ has no business doing here: the RIFF/WAV header, the RMS measurement and
/// the RMS matching. Keeping the audio path byte-exact and header-free means the
/// matching is one multiplication applied by the caller, not a second renderer.
///
/// The track exercises exactly the four character changes of E1, so a difference
/// between the files is audible and attributable:
///   - PULSE lead at 50 % duty (S6 steps it, `Legacy` keeps it continuous);
///   - PULSE harmony at 25 % duty (a different step, same rule);
///   - TRIANGLE bass (S4 weights the triangle 13 % above a pulse);
///   - NOISE percussion from the shipped kick/snare/hat presets (S5 makes the
///     noise unipolar and puts it on the NES period LUT).
/// The envelope difference (S2) is audible on every one of those voices:
/// `Hybrid` decays and releases exponentially, `Nes` and `Legacy` linearly.
///
/// It writes only the file given by `--out`, so `run.py` can keep every
/// intermediate outside the repository.

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "pixelroot32/apu/AudioTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

using pixelroot32::audio::ApuCore;
using pixelroot32::audio::AudioCommand;
using pixelroot32::audio::AudioCommandType;
using pixelroot32::audio::MusicNote;
using pixelroot32::audio::MusicTrack;
using pixelroot32::audio::Note;
using pixelroot32::audio::SoundProfile;
using pixelroot32::audio::WaveType;

using pixelroot32::audio::INSTR_HIHAT;
using pixelroot32::audio::INSTR_KICK;
using pixelroot32::audio::INSTR_PULSE_HARMONY;
using pixelroot32::audio::INSTR_PULSE_LEAD;
using pixelroot32::audio::INSTR_SNARE;
using pixelroot32::audio::INSTR_TRIANGLE_BASS;
using pixelroot32::audio::makeNote;
using pixelroot32::audio::makeRest;

/// @brief Sample rate the A/B files must carry (S9.4). Not a flag: a comparison
/// is only fair when all three files share one rate.
constexpr int kSampleRate = 44100;

/// @brief Tempo of the excerpt. 120 BPM makes one beat exactly 22 050 samples.
constexpr float kBpm = 120.0f;

/// @brief Length of the excerpt in beats: three bars of 4/4.
constexpr int kBeats = 12;

/// @brief Release tail rendered past the last note so a release is not cut
/// mid-decay and mistaken for a character difference.
constexpr int kTailSamples = kSampleRate / 2;

/// @brief Render block. Only the WAV caller sees sample boundaries, so the
/// block size is internal; 512 matches the firmware default and keeps the loop
/// cheap.
constexpr int kBlockSamples = 512;

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
                 "usage: apu_profiles_ab --profile <legacy|nes|hybrid> "
                 "--out <path>\n"
                 "  --profile  sound profile to render with (required)\n"
                 "  --out      output file, raw S16LE mono PCM (required)\n");
}

/**
 * @brief Submits one command, refusing to render a track that never started.
 *
 * A dropped `MUSIC_PLAY` would produce a silent file that still measures a
 * valid RMS, i.e. a clean-looking A/B entry with nothing in it. Failing here is
 * the only way the listener finds out.
 * @param apu Core whose queue receives the command.
 * @param cmd Command to enqueue.
 */
void submit(ApuCore& apu, const AudioCommand& cmd)
{
    if (!apu.submitCommand(cmd)) {
        std::fprintf(stderr, "error: command queue rejected a command\n");
        std::exit(1);
    }
}

/**
 * @brief Builds the four tracks of the excerpt.
 *
 * The note arrays outlive the render: `MusicTrack` stores raw pointers (S1),
 * so they live in the caller's scope and are passed back by pointer.
 */
struct Tracks {
    std::vector<MusicNote> lead;
    std::vector<MusicNote> bass;
    std::vector<MusicNote> harmony;
    std::vector<MusicNote> percussion;
    MusicTrack leadTrack{};
    MusicTrack bassTrack{};
    MusicTrack harmonyTrack{};
    MusicTrack percussionTrack{};

    void build()
    {
        // Lead: a 12-beat melody in half notes, with a gap in the second bar so
        // the release and the noise tail are heard without a melody over them.
        lead = {
            makeNote(INSTR_PULSE_LEAD, Note::E, 4, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::G, 4, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::B, 4, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::E, 5, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::D, 5, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::B, 4, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::G, 4, 0.5f),
            makeRest(0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::A, 4, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::C, 5, 0.5f),
            makeNote(INSTR_PULSE_LEAD, Note::E, 5, 1.0f),
            makeNote(INSTR_PULSE_LEAD, Note::D, 5, 1.0f),
        };

        // Bass: one note per bar, so the triangle's weight is measurable by ear
        // without the melody masking it.
        bass = {
            makeNote(INSTR_TRIANGLE_BASS, Note::E, 2, 4.0f),
            makeNote(INSTR_TRIANGLE_BASS, Note::G, 2, 4.0f),
            makeNote(INSTR_TRIANGLE_BASS, Note::A, 2, 4.0f),
        };

        // Harmony: quarter notes at 25 % duty, a different NES step from the
        // lead's 50 %, so the stepped-vs-continuous rule is audible on its own.
        harmony = {
            makeNote(INSTR_PULSE_HARMONY, Note::B, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::B, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::D, 4, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::G, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::G, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::B, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::D, 4, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::G, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::C, 4, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::E, 4, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::A, 3, 1.0f),
            makeNote(INSTR_PULSE_HARMONY, Note::A, 3, 1.0f),
        };

        // Percussion: one bar is kick-hat-rest / snare-hat-rest / kick-hat-rest /
        // snare-hat-rest. A percussion hit is `Note::Rest` plus a preset whose
        // `duty` is 0; the trailing rest is what advances the sequencer, so the
        // stacked hits do not need a zero-length track.
        percussion.clear();
        percussion.reserve(static_cast<size_t>(kBeats) * 3u);
        for (int beat = 0; beat < kBeats; ++beat) {
            const bool backbeat = (beat % 4) == 1 || (beat % 4) == 3;
            MusicNote hit = makeRest(0.0f);
            hit.preset = backbeat ? &INSTR_SNARE : &INSTR_KICK;
            hit.volume = 1.0f;
            percussion.push_back(hit);

            MusicNote hat = makeRest(0.0f);
            hat.preset = &INSTR_HIHAT;
            hat.volume = 1.0f;
            percussion.push_back(hat);

            percussion.push_back(makeRest(1.0f));
        }

        leadTrack = MusicTrack{lead.data(), lead.size(), false, WaveType::PULSE,
                               0.5f, nullptr, nullptr, nullptr};
        bassTrack = MusicTrack{bass.data(), bass.size(), false, WaveType::TRIANGLE,
                               0.5f, nullptr, nullptr, nullptr};
        harmonyTrack =
            MusicTrack{harmony.data(), harmony.size(), false, WaveType::PULSE,
                       0.25f, nullptr, nullptr, nullptr};
        percussionTrack =
            MusicTrack{percussion.data(), percussion.size(), false, WaveType::NOISE,
                       0.5f, nullptr, nullptr, nullptr};
    }
};

/**
 * @brief Renders the excerpt under one profile into raw S16LE samples.
 * @param profile Profile to render with.
 * @param samples Output buffer, already sized to the render length.
 * @return Peak absolute sample of the render, so the caller can report clipping
 * before matching instead of discovering it as a broken comparison.
 */
int renderTrack(SoundProfile profile, std::vector<int16_t>& samples)
{
    const int samplesPerBeat = static_cast<int>(kSampleRate * 60.0f / kBpm);
    const int bodySamples = samplesPerBeat * kBeats;
    samples.assign(static_cast<size_t>(bodySamples + kTailSamples), 0);

    Tracks tracks;
    tracks.build();

    ApuCore apu;
    apu.init(kSampleRate);
    // After init() and before the first block: the profile is configuration and
    // survives the reset (S4).
    apu.setSoundProfile(profile);

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = kBpm;
    submit(apu, bpm);

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &tracks.leadTrack;
    play.subTracks[0] = &tracks.bassTrack;
    play.subTracks[1] = &tracks.harmonyTrack;
    play.subTracks[2] = &tracks.percussionTrack;
    play.subTrackCount = 3;
    submit(apu, play);

    int peak = 0;
    for (size_t offset = 0; offset < samples.size(); offset += kBlockSamples) {
        const int block = static_cast<int>(
            std::min<size_t>(kBlockSamples, samples.size() - offset));
        apu.generateSamples(samples.data() + offset, block);
        for (int i = 0; i < block; ++i) {
            const int value = samples[offset + static_cast<size_t>(i)];
            const int magnitude = value < 0 ? -value : value;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
    }
    return peak;
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

    std::vector<int16_t> samples;
    const int peak = renderTrack(profile, samples);

    // Serialised as little-endian explicitly, not by dumping host memory: the
    // fixture format is defined by the contract, not by the build machine.
    std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t), 0);
    for (size_t i = 0; i < samples.size(); ++i) {
        const uint16_t value = static_cast<uint16_t>(samples[i]);
        bytes[i * 2u] = static_cast<uint8_t>(value & 0xFFu);
        bytes[i * 2u + 1u] = static_cast<uint8_t>(value >> 8);
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

    std::printf("profile=%s sample_rate=%d samples=%zu bytes=%zu peak=%d\n",
                profileName, kSampleRate, samples.size(), bytes.size(), peak);
    return 0;
}
