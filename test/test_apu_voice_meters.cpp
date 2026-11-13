/**
 * @file test_apu_voice_meters.cpp
 * @brief 2.4.0: the per-voice and output peak meters.
 *
 * `ApuCore::voicePeak(slot)` publishes one slot's own contribution to the mix
 * and `ApuCore::outputPeak()` publishes the final buffer, both as peak
 * magnitude over the most recently rendered block, normalized to [0.0, 1.0].
 * They are the level tap the Tool Suite needs (per-track bars in the Music
 * mixer, an output meter in the SFX editor) without asking the UI thread to
 * look inside a block.
 *
 * The rules this suite pins, stated so they can be argued with:
 *
 *   - A voice's meter is ITS OWN contribution: waveform x volume x envelope,
 *     taken where the voice enters the mix. It is not the summed mix, so four
 *     tracks at four volumes read four different values, and with one voice at
 *     full volume the meter reads ~1.0 while the mix output sits far below it.
 *   - The output meter is the final output: after master volume, shaping,
 *     the output filter/DC blocker and the master bitcrush - the number that
 *     actually lands in the caller's buffer - so a saturated mix reads exactly
 *     1.0 rather than the pre-clamp overshoot.
 *   - Both describe the LAST BLOCK only. There is no hold and no decay: a
 *     silent block publishes 0.0. Smoothing, if the UI wants a ballistic
 *     meter, is the consumer's job.
 *   - `reset()` zeroes both, out-of-range slots answer 0.0, and neither value
 *     depends on which render body produced the block.
 *
 * Everything here goes through the public API. The suite is registered twice -
 * once on the default float `Legacy` body and once with
 * `PR32_APU_ENABLE_LEGACY_FLOAT=0`, the integer core - for the same reason
 * `test_apu_output_filter` is: "the meters are correct" must be a statement
 * about both render loops, not about the one this host happens to compile.
 *
 * The notes are authored here rather than borrowed from a shipped preset: the
 * preset's own volume and envelope would turn the volume ordering assertion
 * into a statement about `INSTR_*` instead of about the meter.
 */

#include <unity.h>

#include <cmath>
#include <cstdint>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "test_config.h"

using namespace pixelroot32::audio;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kBlock = 1024;   // ~23 ms: past the 2 ms attack this preset uses
constexpr float kLongBeats = 4.0f;  // one bar at 120 BPM: no retrigger inside a block
constexpr int kMaxSilentBlocks = 64;  // DC-blocker tail budget, in blocks

/// A note is 440 Hz (`Note::A`, octave 4) unless a test says otherwise, and every
/// track in this file is a phase-locked 50 % square: two voices on the same note
/// start at phase 0 and share an increment, which is what makes the saturation
/// case deterministic instead of statistical.
constexpr Note kPitch = Note::A;

/// @brief The one preset this suite uses: a plain full-level pulse.
///
/// No decay (sustain 1.0) and no LFO, so a voice's peak IS its `volume`: the
/// voice-meter assertions are then about the tap and not about an envelope
/// shape. It is a namespace-scope object because `AudioEvent::preset` and
/// `MusicNote::preset` must point at storage that outlives the event.
InstrumentPreset makePlainPulsePreset() {
    InstrumentPreset preset{};
    preset.baseVolume = 1.0f;
    preset.duty = 0.5f;
    preset.defaultOctave = 4;
    preset.attackTime = 0.001f;
    preset.decayTime = 0.0f;
    preset.sustainLevel = 1.0f;
    preset.releaseTime = 0.005f;
    preset.lfoTarget = LfoTarget::NONE;
    return preset;
}

const InstrumentPreset kPlainPulse = makePlainPulsePreset();

/// @brief One note at `volume`, on the suite's pitch, held for a whole bar.
MusicNote noteAt(float volume) {
    return MusicNote{kPitch, 4, kLongBeats, volume, &kPlainPulse};
}

/// @brief A PULSE track over `notes` (the array must outlive the track).
MusicTrack makeTrack(const MusicNote* notes, size_t count) {
    MusicTrack track{};
    track.notes = notes;
    track.count = count;
    track.loop = false;
    track.channelType = WaveType::PULSE;
    track.duty = 0.5f;
    return track;
}

/// @brief A core with its own output buffer, ready to render one block.
struct Rig {
    ApuCore apu;
    int16_t pcm[kBlock];

    Rig() : pcm{} { apu.init(kSampleRate); }

    void renderBlock() { apu.generateSamples(pcm, kBlock); }
};

void queuePlay(ApuCore& apu, const MusicTrack& track) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::MUSIC_PLAY;
    cmd.track = &track;
    apu.submitCommand(cmd);
}

void queuePlayWithSubTrack(ApuCore& apu, const MusicTrack& first,
                           const MusicTrack& second) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::MUSIC_PLAY;
    cmd.track = &first;
    cmd.subTracks[0] = &second;
    cmd.subTrackCount = 1;
    apu.submitCommand(cmd);
}

void queuePlayFourTracks(ApuCore& apu, const MusicTrack& a, const MusicTrack& b,
                         const MusicTrack& c, const MusicTrack& d) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::MUSIC_PLAY;
    cmd.track = &a;
    cmd.subTracks[0] = &b;
    cmd.subTracks[1] = &c;
    cmd.subTracks[2] = &d;
    cmd.subTrackCount = 3;
    apu.submitCommand(cmd);
}

/// @brief One full-level PULSE one-shot: lands on the next free SFX slot.
void queueSfxBurst(ApuCore& apu) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    cmd.event.preset = &kPlainPulse;
    apu.submitCommand(cmd);
}

void queueStop(ApuCore& apu, int slot) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::STOP_CHANNEL;
    cmd.channelIndex = static_cast<uint8_t>(slot);
    apu.submitCommand(cmd);
}

/// A track of one full-level note on the suite's pitch (music slot 0).
const MusicTrack& soloTrack() {
    static const MusicNote notes[] = {noteAt(1.0f)};
    static const MusicTrack track = makeTrack(notes, 1);
    return track;
}

/// A track of one quiet note on the suite's pitch (music slot 1 as a sub-track).
const MusicTrack& quietTrack() {
    static const MusicNote notes[] = {noteAt(0.25f)};
    static const MusicTrack track = makeTrack(notes, 1);
    return track;
}

}  // namespace

// =============================================================================
// 1. A single voice: only its own slot meters, and the tap is pre-mix
// =============================================================================

void test_voice_meters_single_voice_on_slot_zero(void) {
    Rig rig;
    queuePlay(rig.apu, soloTrack());
    rig.renderBlock();

    // Slot 0 is the voice the sequencer put the note on. A full-volume square
    // reads full scale: the meter is the voice's own contribution, before the
    // 0.4 per-channel mix scale.
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) > 0.9f,
                             "a full-volume square must read close to 1.0");
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) <= 1.0f,
                             "the meter must stay inside [0, 1]");

    // Every other slot was never enabled, so it contributed nothing.
    for (int slot = 1; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(slot),
                                        "a silent slot must read exactly 0.0");
    }

    // The mix is the same voice AFTER the 0.4 scale and the Rational curve, so
    // the output meter has to sit well below the voice meter. If the tap were
    // taken after the sum, these two numbers would be equal - which is the
    // distinction the feature exists for.
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.outputPeak() > 0.2f,
                             "one audible voice must move the output meter");
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.outputPeak() < 0.6f,
                             "the output of one voice is attenuated by the mix");
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.outputPeak() < rig.apu.voicePeak(0),
                             "an output meter taken after the mix cannot exceed "
                             "a single pre-mix voice meter");
}

// =============================================================================
// 2. Two voices at different volumes: both metered, ordered by their volume
// =============================================================================

void test_voice_meters_two_volumes_keep_their_order(void) {
    Rig rig;
    queuePlayWithSubTrack(rig.apu, soloTrack(), quietTrack());
    rig.renderBlock();

    // Sub-track 0 -> music slot 1; both tracks fire on the same tick.
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) > 0.9f,
                             "the full-volume track must read full scale");
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(1) > 0.2f,
                             "the quarter-volume track must read a quarter scale");
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(1) < 0.3f,
                             "the quarter-volume track must not read full scale");

    // 1.0 against 0.25 is a factor of four; allow the ladder some room and
    // still fail if the two slots report the summed mix (which would be equal).
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) > 3.0f * rig.apu.voicePeak(1),
                             "slot 0 is four times louder than slot 1");

    for (int slot = 2; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(slot),
                                        "an unused slot must read exactly 0.0");
    }
}

// =============================================================================
// 3. reset() clears both meters, and a silent block publishes silence
// =============================================================================

void test_voice_meters_reset_and_silence_read_zero(void) {
    Rig rig;

    // Before any block: nothing has been rendered, so nothing is metered.
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(0),
                                    "a fresh core must read 0.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.outputPeak(),
                                    "a fresh core must read 0.0");

    queuePlay(rig.apu, soloTrack());
    rig.renderBlock();
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) > 0.0f,
                             "the note must register before it can be cleared");
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.outputPeak() > 0.0f,
                             "the note must reach the output before it is cleared");

    // reset() stops every voice and zeroes the tap in the same call: the
    // values describe the last block, and there is no block to describe.
    rig.apu.reset();
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(0),
                                    "reset() must clear the voice meter");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.outputPeak(),
                                    "reset() must clear the output meter");

    // A block rendered with no voice enabled is pure silence, and both meters
    // must say so - the recursion state was cleared by reset() as well.
    rig.renderBlock();
    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(slot),
                                        "a silent block must publish 0.0");
    }
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.outputPeak(),
                                    "a silent block must publish 0.0");
    for (int i = 0; i < kBlock; ++i) {
        TEST_ASSERT_EQUAL_INT16_MESSAGE(0, rig.pcm[i],
                                        "the block itself must be silence");
    }
}

// =============================================================================
// 4. Out-of-range slots answer 0.0 and cannot crash the reader
// =============================================================================

void test_voice_meters_out_of_range_slots_read_zero(void) {
    Rig rig;
    queuePlay(rig.apu, soloTrack());
    rig.renderBlock();

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(-1),
                                    "a negative slot must read 0.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(
        0.0f, rig.apu.voicePeak(ApuCore::MAX_VOICES),
        "one past the last slot must read 0.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(
        0.0f, rig.apu.voicePeak(ApuCore::MAX_VOICES + 100),
        "far past the last slot must read 0.0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(-1000),
                                    "far below the first slot must read 0.0");

    // The valid slots still answer: the guard rejects the index, it does not
    // disable the meter.
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) > 0.0f,
                             "a valid slot must keep working after a bad read");
}

// =============================================================================
// 5. The value is per block: a silent block clears a loud one, and no hold
// =============================================================================

void test_voice_meters_describe_the_last_block_only(void) {
    Rig rig;
    queuePlay(rig.apu, soloTrack());
    rig.renderBlock();
    TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(0) > 0.9f,
                             "the first block must be the loud one");

    // Stop every voice. The NEXT block has no voice in it, so every voice
    // meter is 0.0 immediately - there is no hold and no decay.
    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        queueStop(rig.apu, slot);
    }
    rig.renderBlock();
    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.voicePeak(slot),
                                        "a voice that stopped must stop metering");
    }

    // The output meter is the real output, so it still carries the output
    // stage's decaying tail for a few blocks. What it must NOT do is hold:
    // once a whole block is silent it reads 0.0, and it stays there.
    int silentBlocks = 0;
    while (rig.apu.outputPeak() > 0.0f && silentBlocks < kMaxSilentBlocks) {
        rig.renderBlock();
        ++silentBlocks;
    }
    TEST_ASSERT_TRUE_MESSAGE(silentBlocks < kMaxSilentBlocks,
                             "the output meter never returned to 0.0: it is holding, "
                             "not reporting the last block");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.outputPeak(),
                                    "a fully silent block must publish 0.0");
    rig.renderBlock();
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, rig.apu.outputPeak(),
                                    "a second silent block must stay at 0.0");
}

// =============================================================================
// 6. The output meter is the real output: a saturated mix clamps to 1.0
// =============================================================================

void test_voice_meters_output_peak_saturates_at_full_scale(void) {
    // Default `Rational` curve and default master volume: the configuration a
    // consumer ships with, no mode switch to help the meter reach the rail.
    Rig rig;

    // Eight voices, all 440 Hz, all full volume, all phase-locked: four music
    // tracks plus four SFX one-shots on slots 4-7. The sum is 3.2 in float
    // terms (8 x 0.4) and 262 136 in the integer core's raw sum, far past the
    // curve's fixed point — so what reaches the int16 rail is the mix itself,
    // and the meter must report the clamp rather than the overshoot.
    queuePlayFourTracks(rig.apu, soloTrack(), soloTrack(), soloTrack(), soloTrack());
    for (int i = 0; i < 4; ++i) {
        queueSfxBurst(rig.apu);
    }
    rig.renderBlock();

    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_TRUE_MESSAGE(rig.apu.voicePeak(slot) > 0.9f,
                                 "every one of the eight voices must meter");
    }
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(
        1.0f, rig.apu.outputPeak(),
        "a saturated mix must read exactly full scale, not the pre-clamp "
        "overshoot");
}

// Required Unity setup/teardown
void setUp(void) {
    test_setup();
}

void tearDown(void) {
    test_teardown();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_voice_meters_single_voice_on_slot_zero);
    RUN_TEST(test_voice_meters_two_volumes_keep_their_order);
    RUN_TEST(test_voice_meters_reset_and_silence_read_zero);
    RUN_TEST(test_voice_meters_out_of_range_slots_read_zero);
    RUN_TEST(test_voice_meters_describe_the_last_block_only);
    RUN_TEST(test_voice_meters_output_peak_saturates_at_full_scale);
    return UNITY_END();
}
