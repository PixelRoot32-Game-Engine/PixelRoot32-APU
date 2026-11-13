/**
 * @file test_apu_voice_meters_placement.cpp
 * @brief Verifier probes for 2.4.0 peak metering: tap placement, output identity,
 *        NES group weight, post-mix ordering, rejected-slot safety, no side effects.
 *
 * These are independent of test_apu_voice_meters.cpp: they do not trust the
 * worker's assertions. Each case pins a property a consumer relies on, and the
 * suite is compiled twice (float `Legacy` body and PR32_APU_ENABLE_LEGACY_FLOAT=0)
 * so the property holds on both render loops.
 */

#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "test_config.h"

using namespace pixelroot32::audio;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kBlock = 512;

InstrumentPreset makePreset(float volume) {
    InstrumentPreset p{};
    p.baseVolume = volume;
    p.duty = 0.5f;
    p.defaultOctave = 4;
    p.attackTime = 0.001f;
    p.decayTime = 0.0f;
    p.sustainLevel = 1.0f;
    p.releaseTime = 0.005f;
    p.lfoTarget = LfoTarget::NONE;
    return p;
}

const InstrumentPreset kFull = makePreset(1.0f);
const InstrumentPreset kQuarter = makePreset(0.25f);
const InstrumentPreset kHalf = makePreset(0.5f);

const MusicNote kFullNote[] = { MusicNote{Note::A, 4, 8.0f, 1.0f, &kFull} };
const MusicNote kQuarterNote[] = { MusicNote{Note::A, 4, 8.0f, 0.25f, &kQuarter} };

MusicTrack makeTrack(const MusicNote* notes, size_t count, WaveType type) {
    MusicTrack t{};
    t.notes = notes;
    t.count = count;
    t.loop = false;
    t.channelType = type;
    t.duty = 0.5f;
    return t;
}

void playEvent(ApuCore& apu, WaveType type, float hz, float volume,
               const InstrumentPreset* preset) {
    AudioCommand c{};
    c.type = AudioCommandType::PLAY_EVENT;
    c.event.type = type;
    c.event.frequency = hz;
    c.event.duration = 10.0f;
    c.event.volume = volume;
    c.event.duty = 0.5f;
    c.event.preset = preset;
    apu.submitCommand(c);
}

void musicPlay(ApuCore& apu, const MusicTrack& a, const MusicTrack* b = nullptr,
               const MusicTrack* c = nullptr, const MusicTrack* d = nullptr) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::MUSIC_PLAY;
    cmd.track = &a;
    cmd.subTracks[0] = b;
    cmd.subTracks[1] = c;
    cmd.subTracks[2] = d;
    cmd.subTrackCount = (b ? 1u : 0u) + (c ? 1u : 0u) + (d ? 1u : 0u);
    apu.submitCommand(cmd);
}

/// Exact normalized peak of a finished buffer, independent of the meter.
float bufferPeak(const int16_t* pcm, int n) {
    int32_t m = 0;
    for (int i = 0; i < n; ++i) {
        const int32_t a = (pcm[i] < 0) ? -(int32_t)pcm[i] : (int32_t)pcm[i];
        if (a > m) m = a;
    }
    return (m >= 32767) ? 1.0f : ((float)m / 32767.0f);
}

int16_t gPreHook[kBlock];
int gPreHookLen = 0;

void recordThenZeroHook(int16_t* mono, int length, void* /*user*/) {
    gPreHookLen = (length > kBlock) ? kBlock : length;
    std::memcpy(gPreHook, mono, sizeof(int16_t) * (size_t)gPreHookLen);
    for (int i = 0; i < length; ++i) mono[i] = 0;
}

}  // namespace

// outputPeak is the sample that lands in the caller's buffer: post volume,
// post shaping, post filter, post bitcrush. Verified against the buffer itself.
void test_meter_output_peak_equals_buffer_peak(void) {
    ApuCore apu;
    apu.init(kSampleRate);
    int16_t pcm[kBlock]{};

    AudioCommand mv{};
    mv.type = AudioCommandType::SET_MASTER_VOLUME;
    mv.volume = 0.6f;
    apu.submitCommand(mv);
    AudioCommand bc{};
    bc.type = AudioCommandType::SET_MASTER_BITCRUSH;
    bc.masterBitcrushBits = 2;
    apu.submitCommand(bc);

    static MusicTrack t = makeTrack(kFullNote, 1, WaveType::PULSE);
    musicPlay(apu, t);
    playEvent(apu, WaveType::SAW, 330.0f, 1.0f, &kFull);
    playEvent(apu, WaveType::NOISE, 500.0f, 1.0f, &kFull);

    for (int blk = 0; blk < 4; ++blk) {
        apu.generateSamples(pcm, kBlock);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(bufferPeak(pcm, kBlock), apu.outputPeak(),
                                        "outputPeak must equal max|buffer|/32767");
    }
}

// The per-voice value is the voice's own contribution, never the sum: four
// voices that each add 0.25 must each read 0.25, not the 1.0 mix.
void test_meter_voice_peak_is_not_the_mix(void) {
    ApuCore apu;
    apu.init(kSampleRate);
    int16_t pcm[kBlock]{};

    static MusicTrack a = makeTrack(kQuarterNote, 1, WaveType::PULSE);
    static MusicTrack b = makeTrack(kQuarterNote, 1, WaveType::PULSE);
    static MusicTrack c = makeTrack(kQuarterNote, 1, WaveType::PULSE);
    static MusicTrack d = makeTrack(kQuarterNote, 1, WaveType::PULSE);
    musicPlay(apu, a, &b, &c, &d);
    apu.generateSamples(pcm, kBlock);

    for (int slot = 0; slot < 4; ++slot) {
        TEST_ASSERT_TRUE_MESSAGE(apu.voicePeak(slot) > 0.2f,
                                 "each 0.25 voice must meter around 0.25");
        TEST_ASSERT_TRUE_MESSAGE(apu.voicePeak(slot) < 0.3f,
                                 "the voice meter must not report the 1.0 sum");
    }
}

// Under Nes/Famicom the published group weight is inside the per-voice value:
// triangle (4635/4096 = 1.1316) against pulse (4096/4096).
void test_meter_nes_group_weight_is_inside_voice_peak(void) {
    ApuCore apu;
    apu.init(kSampleRate);
    apu.setSoundProfile(SoundProfile::Nes);
    int16_t pcm[kBlock]{};

    playEvent(apu, WaveType::PULSE, 440.0f, 0.5f, &kHalf);     // SFX slot 4
    playEvent(apu, WaveType::TRIANGLE, 440.0f, 0.5f, &kHalf);  // SFX slot 5
    apu.generateSamples(pcm, kBlock);

    const float pulse = apu.voicePeak(4);
    const float triangle = apu.voicePeak(5);
    TEST_ASSERT_TRUE_MESSAGE(pulse > 0.49f && pulse < 0.51f,
                             "the pulse voice must read exactly its volume");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.005f, 1.1316f * pulse, triangle,
                                     "the triangle voice must carry kNesMixWeightTriangle");
}

// The output tap sits before setPostMixMono: the meter must equal the buffer
// the hook received, even though the hook then rewrites the buffer to silence.
void test_meter_output_tap_is_before_post_mix_hook(void) {
    ApuCore apu;
    apu.init(kSampleRate);
    int16_t pcm[kBlock]{};

    static MusicTrack t = makeTrack(kFullNote, 1, WaveType::PULSE);
    musicPlay(apu, t);
    apu.setPostMixMono(&recordThenZeroHook, nullptr);
    apu.generateSamples(pcm, kBlock);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, bufferPeak(pcm, kBlock),
                                    "the hook must have zeroed the caller's buffer");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(bufferPeak(gPreHook, gPreHookLen), apu.outputPeak(),
                                    "outputPeak must describe the pre-hook buffer");
}

// Slots outside [0, MAX_VOICES) answer 0.0 for any int, without UB.
void test_meter_extreme_slots_answer_zero(void) {
    ApuCore apu;
    apu.init(kSampleRate);
    int16_t pcm[kBlock]{};
    static MusicTrack t = makeTrack(kFullNote, 1, WaveType::PULSE);
    musicPlay(apu, t);
    apu.generateSamples(pcm, kBlock);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, apu.voicePeak(-1));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, apu.voicePeak(ApuCore::MAX_VOICES));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, apu.voicePeak(ApuCore::MAX_VOICES + 1000));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, apu.voicePeak(INT32_MIN));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, apu.voicePeak(INT32_MAX));
    TEST_ASSERT_TRUE_MESSAGE(apu.voicePeak(0) > 0.9f,
                             "a rejected read must not disturb the valid slots");
}

// Reading the meters is side-effect free: interleaving reads between blocks
// must not change one sample of the rendered PCM.
void test_meter_reads_do_not_change_pcm(void) {
    static MusicTrack a = makeTrack(kFullNote, 1, WaveType::PULSE);
    static MusicTrack b = makeTrack(kQuarterNote, 1, WaveType::TRIANGLE);

    int16_t withReads[kBlock]{};
    int16_t withoutReads[kBlock]{};

    ApuCore probed;
    probed.init(kSampleRate);
    probed.setSoundProfile(SoundProfile::Nes);
    musicPlay(probed, a, &b);
    for (int blk = 0; blk < 6; ++blk) {
        probed.generateSamples(withReads, kBlock);
        for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
            (void)probed.voicePeak(slot);
        }
        (void)probed.voicePeak(-1);
        (void)probed.voicePeak(ApuCore::MAX_VOICES + 1);
        (void)probed.outputPeak();
    }

    ApuCore untouched;
    untouched.init(kSampleRate);
    untouched.setSoundProfile(SoundProfile::Nes);
    musicPlay(untouched, a, &b);
    for (int blk = 0; blk < 6; ++blk) {
        untouched.generateSamples(withoutReads, kBlock);
    }

    TEST_ASSERT_EQUAL_INT8_ARRAY_MESSAGE(
        reinterpret_cast<const int8_t*>(withReads),
        reinterpret_cast<const int8_t*>(withoutReads),
        kBlock * (int)sizeof(int16_t),
        "meter reads must not alter the rendered audio");
}

void setUp(void) { test_setup(); }
void tearDown(void) { test_teardown(); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_meter_output_peak_equals_buffer_peak);
    RUN_TEST(test_meter_voice_peak_is_not_the_mix);
    RUN_TEST(test_meter_nes_group_weight_is_inside_voice_peak);
    RUN_TEST(test_meter_output_tap_is_before_post_mix_hook);
    RUN_TEST(test_meter_extreme_slots_answer_zero);
    RUN_TEST(test_meter_reads_do_not_change_pcm);
    return UNITY_END();
}
