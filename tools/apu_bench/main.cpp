/// apu_bench — measures what one 512-sample render block costs in each render
/// core that a build can carry, on the host.
///
/// The matrix compares CORES, not builds: `Legacy` renders the float body and
/// `Nes` the integer body inside the same binary (S3), selected through the
/// public `setSoundProfile` API only. A benchmark that called
/// `renderFloatLegacy` / `renderIntegerCore` directly would measure a function
/// it invented rather than the dispatch a game actually goes through, so the
/// profile is the only lever used here.
///
/// Everything printed is a host figure. `tools/apu_bench_firmware` (S11) is the
/// one that says what an ESP32 does; these numbers say what the two bodies cost
/// per sample on an x86_64 desktop, which is the only place both bodies can be
/// run side by side in one process.
///
/// Output split, so `apu_bench > baseline.csv` stays clean:
///   - stdout: CSV, header `profile,rate,voices,p50_us,p99_us,us_per_sample`
///   - stderr: the human table with the real-time factor and the sizeof block

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioCommandQueue.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "pixelroot32/apu/AudioTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using pixelroot32::audio::AudioChannel;
using pixelroot32::audio::AudioCommand;
using pixelroot32::audio::AudioCommandQueue;
using pixelroot32::audio::AudioCommandType;
using pixelroot32::audio::AudioEvent;
using pixelroot32::audio::ApuCore;
using pixelroot32::audio::InstrumentPreset;
using pixelroot32::audio::MusicNote;
using pixelroot32::audio::MusicTrack;
using pixelroot32::audio::Note;
using pixelroot32::audio::SoundProfile;
using pixelroot32::audio::WaveType;

using pixelroot32::audio::INSTR_PULSE_LEAD;
using pixelroot32::audio::INSTR_TRIANGLE_BASS;
using pixelroot32::audio::INSTR_TRIANGLE_LEAD;

/// @brief Samples per rendered block. 512 is the FPU default of S12, so the
/// benchmark measures the block size the firmware in this milestone renders.
constexpr int kBlockSamples = 512;

/// @brief Discarded blocks, so the first measurements do not pay for cache
/// misses, command processing and the attack ramp of the voices.
constexpr int kWarmupBlocks = 100;

/// @brief Measured blocks per cell. S11 fixes this number, and it is what makes
/// a p99 meaningful: with 1 000 samples, nearest-rank p99 is the 990th value.
constexpr int kMeasuredBlocks = 1000;

/// @brief Music-note length, in beats, chosen to outlast the whole run.
///
/// The measured window is (100 + 1000) blocks = 563 200 samples, i.e. 25.5 s at
/// 22 050 Hz and 17.6 s at 32 000 Hz. One note of 128 beats at the sequencer's
/// default 150 BPM lasts 128 * 60 / 150 = 51.2 s at either rate, so the four
/// music voices never reach their gate and never retrigger mid-measurement.
/// A gate that cannot expire is the guarantee; see the README.
constexpr float kMusicNoteBeats = 128.0f;

/**
 * @brief One matrix cell: a profile, a rate and a voice count.
 *
 * The name is the CSV/table label for the profile. Keeping it here instead of
 * formatting the enumerator at print time keeps stdout and stderr in sync.
 */
struct Cell {
    SoundProfile profile;
    const char* name;
    int rate;
    int voices;
};

/**
 * @brief Music tracks used to occupy the four sequencer voice slots.
 *
 * Four tracks are the only public way to keep voices 0–3 sounding: slots 0–3
 * belong to the sequencer and `PLAY_EVENT` can only allocate 4–7
 * (`findVoiceForSfxEvent`), so a matrix that claimed "8 voices" out of SFX
 * events alone would silently cap at 4. Each track holds a single long note, so
 * the sequencer has no note boundary to cross inside the run.
 */
constexpr MusicNote kMusicNotes0[] = {
    {Note::A, 4, kMusicNoteBeats, 0.30f, &INSTR_PULSE_LEAD}};
constexpr MusicNote kMusicNotes1[] = {
    {Note::E, 5, kMusicNoteBeats, 0.28f, &INSTR_TRIANGLE_LEAD}};
constexpr MusicNote kMusicNotes2[] = {
    {Note::C, 5, kMusicNoteBeats, 0.22f, nullptr}};
constexpr MusicNote kMusicNotes3[] = {
    {Note::G, 4, kMusicNoteBeats, 0.22f, nullptr}};

constexpr MusicTrack kMusicTracks[] = {
    {kMusicNotes0, 1, true, WaveType::PULSE, 0.5f, nullptr, nullptr, nullptr},
    {kMusicNotes1, 1, true, WaveType::TRIANGLE, 0.5f, nullptr, nullptr, nullptr},
    {kMusicNotes2, 1, true, WaveType::SINE, 0.5f, nullptr, nullptr, nullptr},
    {kMusicNotes3, 1, true, WaveType::SAW, 0.5f, nullptr, nullptr, nullptr},
};

/**
 * @brief Result of one cell: the two percentiles and the render checksum.
 *
 * The checksum exists so the compiler cannot treat the render as dead code: a
 * loop that only writes a buffer nobody reads can be deleted in theory, and a
 * benchmark that measures a deleted loop would report a beautifully fast zero.
 */
struct CellResult {
    double p50;
    double p99;
    int64_t checksum;
};

/**
 * @brief Builds a looping `PLAY_EVENT` for the SFX voice pool.
 *
 * `loop = true` is what makes the voice outlive the run: with
 * `gate_samples == 0` and `loop` set, `initVoiceFromEvent` sets
 * `remainingSamples = UINT64_MAX` and the per-sample expiry never fires.
 * @param type Waveform for the voice.
 * @param frequency Pitch in Hz (ignored by NOISE, unused here).
 * @param volume Per-voice volume, 0..1.
 * @param preset ADSR/LFO source, or nullptr for the legacy defaults.
 * @return The event, ready to be wrapped in an `AudioCommand`.
 */
AudioEvent makeLoopEvent(WaveType type, float frequency, float volume,
                         const InstrumentPreset* preset)
{
    AudioEvent event{};
    event.type = type;
    event.frequency = frequency;
    event.duration = 0.0f;
    event.volume = volume;
    event.duty = 0.5f;
    event.preset = preset;
    event.loop = true;
    return event;
}

/**
 * @brief Submits one command, stopping the run if the queue rejects it.
 *
 * A dropped command would silently change the voice count under measurement, so
 * the failure has to be loud instead of showing up as a suspiciously fast cell.
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
 * @brief Starts the requested number of voices and keeps them sounding.
 *
 * `1` uses the first SFX slot; `8` fills the four sequencer slots with the
 * music tracks above and the four SFX slots with four looping events of
 * distinct waveforms (so `findVoiceForSfxEvent` hands out 4, 5, 6 and 7 in
 * turn instead of reusing one slot).
 * @param apu Core to configure; must already be initialised at the cell rate.
 * @param voices 1 or 8.
 */
void startVoices(ApuCore& apu, int voices)
{
    AudioCommand event;
    event.type = AudioCommandType::PLAY_EVENT;
    event.event = makeLoopEvent(WaveType::PULSE, 330.0f, 0.30f, &INSTR_PULSE_LEAD);
    submit(apu, event);

    if (voices < 2) {
        return;
    }

    AudioCommand music;
    music.type = AudioCommandType::MUSIC_PLAY;
    music.track = &kMusicTracks[0];
    music.subTracks[0] = &kMusicTracks[1];
    music.subTracks[1] = &kMusicTracks[2];
    music.subTracks[2] = &kMusicTracks[3];
    music.subTrackCount = 3;
    submit(apu, music);

    event.event = makeLoopEvent(WaveType::TRIANGLE, 196.0f, 0.28f,
                                &INSTR_TRIANGLE_BASS);
    submit(apu, event);
    event.event = makeLoopEvent(WaveType::SINE, 523.25f, 0.22f, nullptr);
    submit(apu, event);
    event.event = makeLoopEvent(WaveType::SAW, 392.0f, 0.22f, nullptr);
    submit(apu, event);
}

/**
 * @brief Nearest-rank percentile over an already sorted vector.
 *
 * Nearest rank (not an interpolation) because a block time is an observation,
 * not a distribution: p99 must be a block that actually happened, so the figure
 * can be tied back to a sample. `ceil(p/100 * N) - 1`, clamped for p = 0.
 * @param sorted Block times in microseconds, ascending.
 * @param percent Percentile in (0, 100].
 * @return The observed block time at that rank.
 */
double nearestRank(const std::vector<double>& sorted, double percent)
{
    const size_t count = sorted.size();
    size_t rank = static_cast<size_t>(std::ceil(percent / 100.0 * double(count)));
    if (rank == 0) {
        rank = 1;
    }
    if (rank > count) {
        rank = count;
    }
    return sorted[rank - 1];
}

/**
 * @brief Renders one cell and returns its timing statistics.
 * @param cell Profile, rate and voice count to measure.
 * @return Percentiles plus the output checksum.
 */
CellResult measure(const Cell& cell)
{
    // A fresh core per cell: a reused one would carry the previous cell's
    // voice phases, envelope stages and command history into the timing.
    ApuCore apu;
    apu.init(cell.rate);
    // After init(): the profile is configuration and survives init (S4), and
    // setting it here keeps the cell self-contained.
    apu.setSoundProfile(cell.profile);
    startVoices(apu, cell.voices);

    std::array<int16_t, kBlockSamples> buffer{};
    for (int block = 0; block < kWarmupBlocks; ++block) {
        apu.generateSamples(buffer.data(), kBlockSamples);
    }

    std::vector<double> blockMicros(static_cast<size_t>(kMeasuredBlocks), 0.0);
    int64_t checksum = 0;

    for (int block = 0; block < kMeasuredBlocks; ++block) {
        // The clock brackets the render call and nothing else: the checksum
        // below and the loop bookkeeping are deliberately outside, so the
        // figure describes generateSamples and not the harness around it.
        const auto start = std::chrono::steady_clock::now();
        apu.generateSamples(buffer.data(), kBlockSamples);
        const auto stop = std::chrono::steady_clock::now();

        blockMicros[static_cast<size_t>(block)] =
            std::chrono::duration<double, std::micro>(stop - start).count();
        for (int sample = 0; sample < kBlockSamples; ++sample) {
            checksum += buffer[static_cast<size_t>(sample)];
        }
    }

    std::sort(blockMicros.begin(), blockMicros.end());
    return CellResult{nearestRank(blockMicros, 50.0),
                      nearestRank(blockMicros, 99.0),
                      checksum};
}

/**
 * @brief Prints the host `sizeof` block required by S11.
 *
 * The type is `ApuCore`; S11 and §4 spell it `AudioCore`. The label keeps the
 * requested name and states the real one, so a reader searching for either
 * finds the line and the discrepancy is visible rather than silently corrected.
 */
void printSizes()
{
    const size_t queueBytes =
        AudioCommandQueue::CAPACITY * sizeof(AudioCommand);

    std::fprintf(stderr, "\nsizeof (host ABI, no UNIT_TEST)\n");
    std::fprintf(stderr, "  sizeof(AudioCore)        = %zu B  (type is ApuCore)\n",
                 sizeof(ApuCore));
    std::fprintf(stderr, "  sizeof(AudioCommand)     = %zu B\n",
                 sizeof(AudioCommand));
    std::fprintf(stderr, "  sizeof(AudioChannel)     = %zu B\n",
                 sizeof(AudioChannel));
    std::fprintf(stderr, "  sizeof(InstrumentPreset) = %zu B\n",
                 sizeof(InstrumentPreset));
    std::fprintf(stderr,
                 "  command queue budget     = %zu x %zu B = %zu B  "
                 "(AUDIO_COMMAND_QUEUE_CAPACITY x sizeof(AudioCommand))\n",
                 AudioCommandQueue::CAPACITY, sizeof(AudioCommand), queueBytes);
}

}  // namespace

int main()
{
    const Cell cells[] = {
        {SoundProfile::Legacy, "Legacy", 22050, 1},
        {SoundProfile::Legacy, "Legacy", 22050, 8},
        {SoundProfile::Legacy, "Legacy", 32000, 1},
        {SoundProfile::Legacy, "Legacy", 32000, 8},
        {SoundProfile::Nes, "Nes", 22050, 1},
        {SoundProfile::Nes, "Nes", 22050, 8},
        {SoundProfile::Nes, "Nes", 32000, 1},
        {SoundProfile::Nes, "Nes", 32000, 8},
    };

    std::fprintf(stderr,
                 "apu_bench -- 512-sample blocks, %d warm-up + %d measured per cell\n",
                 kWarmupBlocks, kMeasuredBlocks);

    // Flush before the measurements so the header and the first CSV row cannot
    // interleave when stdout is a file and stderr a terminal.
    std::printf("profile,rate,voices,p50_us,p99_us,us_per_sample\n");
    std::fflush(stdout);

    std::fprintf(stderr,
                 "\n%-8s %6s %6s %10s %10s %12s %12s %14s %16s\n",
                 "profile", "rate", "voices", "p50_us", "p99_us", "us/sample",
                 "budget_us", "headroom", "checksum");

    for (const Cell& cell : cells) {
        const CellResult result = measure(cell);
        const double usPerSample = result.p50 / double(kBlockSamples);
        // The real-time budget of the block: how long the audio it holds would
        // last. headroom = budget / p99 is the factor by which the render is
        // faster than the wall clock it has to chase.
        const double budgetMicros = double(kBlockSamples) * 1e6 / double(cell.rate);
        const double headroom = budgetMicros / result.p99;

        std::printf("%s,%d,%d,%.3f,%.3f,%.3f\n", cell.name, cell.rate, cell.voices,
                    result.p50, result.p99, usPerSample);
        std::fflush(stdout);

        // The checksum is printed, not just computed: it is the only reader of
        // the rendered buffer, so without a use the compiler would be free to
        // delete the render it is supposed to measure.
        std::fprintf(stderr,
                     "%-8s %6d %6d %10.3f %10.3f %12.3f %12.3f %13.1fx %16lld\n",
                     cell.name, cell.rate, cell.voices, result.p50, result.p99,
                     usPerSample, budgetMicros, headroom,
                     static_cast<long long>(result.checksum));
    }

    printSizes();
    return 0;
}
