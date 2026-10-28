/// apu_bench_firmware -- T8/S11: what one 512-sample render block costs on an
/// ESP32, printed over serial as CSV.
///
/// Firmware port of `tools/apu_bench` (host). The matrix, the warm-up, the
/// percentile method and the voice setup are the host tool's, deliberately: two
/// benchmarks that measured different things would produce two tables nobody
/// could compare, and the point of T8 is to put the board's figures next to the
/// desktop's.
///
/// What it measures: the CPU cost of `ApuCore::generateSamples` for one block,
/// per profile, rate and voice count, on the target.
///
/// What it does NOT measure: I2S/DAC latency, DMA behaviour or underruns. Those
/// come from the backend counters of S13 on a real run; this binary renders
/// into a buffer and never talks to a peripheral.
///
/// Serial output, one stream, 115200 baud:
///   - one `#` identity line first: chip, revision, CPU clock, the two render
///     macros, the target `sizeof` block and the command-queue budget;
///   - the CSV header `profile,rate,voices,p50_us,p99_us,us_per_sample`;
///   - one CSV row per cell, each followed by its own `#` line carrying that
///     cell's budget, headroom and checksum.
/// Everything that is not a CSV row starts with `#`, so `grep -v '^#'` leaves
/// the machine-readable part and `grep '^#'` leaves the metadata.

#include <Arduino.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <new>

#include "pixelroot32/apu/ApuConfig.h"
#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioCommandQueue.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "pixelroot32/apu/AudioTypes.h"

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

/// @brief Samples per rendered block. 512 is the FPU default of S12, the same
/// block the host tool measures, so the two tables are comparable.
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
constexpr float kMusicNoteBeats = 128.0f;

/**
 * @brief One matrix cell: a profile, a rate and a voice count.
 *
 * The name is the CSV/table label for the profile. Keeping it here instead of
 * formatting the enumerator at print time keeps the row and the `#` line in
 * sync.
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
 * Four tracks are the only public way to keep voices 0-3 sounding: slots 0-3
 * belong to the sequencer and `PLAY_EVENT` can only allocate 4-7
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
 * Whole microseconds, because `esp_timer_get_time()` is a microsecond clock.
 * The checksum exists so the compiler cannot treat the render as dead code: a
 * loop that only writes a buffer nobody reads can be deleted in theory, and a
 * benchmark that measures a deleted loop would report a beautifully fast zero.
 */
struct CellResult {
    uint32_t p50;
    uint32_t p99;
    int64_t checksum;
};

/// @brief Storage for the core of the cell being measured.
///
/// Static, not a local: `sizeof(ApuCore)` is 9 800 B on the 32-bit ABI (13 200 B
/// on the 64-bit host ABI) and the Arduino loop task that runs `setup()` has 8 KB
/// of stack, so a local would overflow it. Unsigned char storage plus placement
/// new keeps the host tool's guarantee -- every cell starts from a freshly
/// constructed core -- without 8 x 9 800 B of statics.
alignas(ApuCore) unsigned char gCoreStorage[sizeof(ApuCore)];

/// @brief Render buffer for the cell being measured.
std::array<int16_t, kBlockSamples> gBuffer{};

/// @brief Per-block durations of the current cell, in microseconds.
uint32_t gBlockMicros[kMeasuredBlocks];

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
 * @brief Submits one command, parking the board if the queue rejects it.
 *
 * The host tool calls `std::exit(1)` here; a microcontroller cannot exit, so
 * the run stops loudly (`error:` line, then idle in `delay()`) instead of
 * continuing with a voice count that is not the one printed in the CSV.
 * @param apu Core whose queue receives the command.
 * @param cmd Command to enqueue.
 */
void submit(ApuCore& apu, const AudioCommand& cmd)
{
    if (!apu.submitCommand(cmd)) {
        Serial.println("error: command queue rejected a command");
        Serial.flush();
        for (;;) {
            delay(1000);
        }
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
 * @brief Nearest-rank percentile over an already sorted array.
 *
 * Nearest rank (not an interpolation) because a block time is an observation,
 * not a distribution: p99 must be a block that actually happened, so the figure
 * can be tied back to a sample. `ceil(p/100 * N) - 1`, clamped for p = 0.
 * @param sorted Block times in microseconds, ascending.
 * @param count Number of entries in `sorted`.
 * @param percent Percentile in (0, 100].
 * @return The observed block time at that rank.
 */
uint32_t nearestRank(const uint32_t* sorted, size_t count, double percent)
{
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
 *
 * The clock brackets `generateSamples` and nothing else; the checksum is
 * accumulated outside the timed region so the figure describes the render and
 * not the harness around it.
 * @param cell Profile, rate and voice count to measure.
 * @return Percentiles plus the output checksum.
 */
CellResult measure(const Cell& cell)
{
    // A fresh core per cell, exactly as on the host: a reused one would carry
    // the previous cell's voice phases, envelope stages and command history
    // into the timing.
    ApuCore& apu = *::new (static_cast<void*>(gCoreStorage)) ApuCore();
    apu.init(cell.rate);
    // After init(): the profile is configuration and survives init (S4), and
    // setting it here keeps the cell self-contained.
    apu.setSoundProfile(cell.profile);
    startVoices(apu, cell.voices);

    gBuffer.fill(0);
    for (int block = 0; block < kWarmupBlocks; ++block) {
        apu.generateSamples(gBuffer.data(), kBlockSamples);
    }

    int64_t checksum = 0;
    for (int block = 0; block < kMeasuredBlocks; ++block) {
        const int64_t start = esp_timer_get_time();
        apu.generateSamples(gBuffer.data(), kBlockSamples);
        const int64_t stop = esp_timer_get_time();

        gBlockMicros[block] = static_cast<uint32_t>(stop - start);
        for (int sample = 0; sample < kBlockSamples; ++sample) {
            checksum += gBuffer[static_cast<size_t>(sample)];
        }
    }

    std::sort(gBlockMicros, gBlockMicros + kMeasuredBlocks);
    return CellResult{nearestRank(gBlockMicros, kMeasuredBlocks, 50.0),
                      nearestRank(gBlockMicros, kMeasuredBlocks, 99.0),
                      checksum};
}

/**
 * @brief Prints `numerator / denominator` with three fractional digits.
 *
 * Integer arithmetic on purpose: the board clock ticks in whole microseconds,
 * so every figure here is exact in integers, and this avoids depending on
 * newlib's `%f` support -- a dependency this project cannot verify without a
 * board attached. Rounding matches the host tool's `%.3f`.
 * @param numerator Value to scale.
 * @param denominator Divisor.
 */
void printFixed3(uint64_t numerator, uint64_t denominator)
{
    const uint64_t milli =
        (numerator * 1000ULL + denominator / 2ULL) / denominator;
    Serial.printf("%llu.%03llu", milli / 1000ULL, milli % 1000ULL);
}

/**
 * @brief Prints `numerator / denominator` with one fractional digit.
 * @param numerator Value to scale.
 * @param denominator Divisor.
 */
void printFixed1(uint64_t numerator, uint64_t denominator)
{
    const uint64_t tenths = (numerator * 10ULL + denominator / 2ULL) / denominator;
    Serial.printf("%llu.%llu", tenths / 10ULL, tenths % 10ULL);
}

/**
 * @brief Prints the identity line: who is running, and with which render path.
 *
 * The two macros are printed because they decide which body `Legacy` runs
 * (S3): on a target without FPU both profile rows measure the integer core,
 * and a reader has to be able to see that from the capture instead of
 * inferring it from the numbers. `sizeof(ApuCore)` is the target figure S8 says
 * is recorded per board rather than pinned by a constant.
 */
void printIdentity()
{
    Serial.printf(
        "# apu_bench_firmware chip=%s rev=%u cpu_mhz=%lu fpu=%d legacy_float=%d "
        "sizeof_apu_core=%u sizeof_audio_command=%u sizeof_audio_channel=%u "
        "sizeof_instrument_preset=%u queue_capacity=%u queue_bytes=%u "
        "block_samples=%d warmup_blocks=%d measured_blocks=%d\n",
        ESP.getChipModel(),
        static_cast<unsigned>(ESP.getChipRevision()),
        static_cast<unsigned long>(ESP.getCpuFreqMHz()),
        PR32_APU_HAS_FPU,
        PR32_APU_ENABLE_LEGACY_FLOAT,
        static_cast<unsigned>(sizeof(ApuCore)),
        static_cast<unsigned>(sizeof(AudioCommand)),
        static_cast<unsigned>(sizeof(AudioChannel)),
        static_cast<unsigned>(sizeof(InstrumentPreset)),
        static_cast<unsigned>(AudioCommandQueue::CAPACITY),
        static_cast<unsigned>(AudioCommandQueue::CAPACITY * sizeof(AudioCommand)),
        kBlockSamples, kWarmupBlocks, kMeasuredBlocks);
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    // Let the UART settle so the identity line is not the first thing lost.
    delay(200);

    printIdentity();

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

    Serial.println("profile,rate,voices,p50_us,p99_us,us_per_sample");

    for (const Cell& cell : cells) {
        const CellResult result = measure(cell);

        Serial.printf("%s,%d,%d,", cell.name, cell.rate, cell.voices);
        printFixed3(result.p50, 1);
        Serial.print(',');
        printFixed3(result.p99, 1);
        Serial.print(',');
        printFixed3(result.p50, kBlockSamples);
        Serial.println();

        // The real-time budget of the block is how long the audio it holds
        // would last; headroom = budget / p99 is the factor by which the render
        // is faster than the wall clock it has to chase.
        const uint64_t budgetMicros =
            static_cast<uint64_t>(kBlockSamples) * 1000000ULL
            / static_cast<uint64_t>(cell.rate);

        Serial.printf("# profile=%s rate=%d voices=%d budget_us=", cell.name,
                      cell.rate, cell.voices);
        printFixed3(static_cast<uint64_t>(kBlockSamples) * 1000000ULL,
                    static_cast<uint64_t>(cell.rate));
        Serial.print(" headroom=");
        // p99 is never 0 for a 512-sample block on these targets; the guard is
        // here so a truncated-to-0 clock reading cannot divide by zero.
        if (result.p99 > 0) {
            printFixed1(budgetMicros, result.p99);
        } else {
            Serial.print("inf");
        }
        Serial.printf("x checksum=%lld\n", static_cast<long long>(result.checksum));
        Serial.flush();
    }
}

void loop()
{
    // The measurement runs once, from setup(). Idle so the board does not
    // watchdog-reset while the capture is being read.
    delay(1000);
}
