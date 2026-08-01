/**
 * @file test_q15_smoke.cpp
 * @brief Smoke coverage for the integer/Q15 render path compiled natively
 *        (PR32_APU_HAS_FPU=0).
 *
 * The extraction analysis found that no firmware or test build compiled the
 * Q15 branch at all. This suite closes that gap: it forces the integer path
 * on the host and checks the behaviours that do not depend on exact gain
 * staging (which intentionally differs from the float path by the documented
 * -6 dB headroom pad).
 */

#include <unity.h>
#include <cmath>
#include <cstring>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

#if PR32_APU_HAS_FPU
#error "test_q15_smoke must be compiled with PR32_APU_HAS_FPU=0"
#endif

using namespace pixelroot32::audio;

static ApuCore apu;

extern "C" void setUp(void) {
    apu.reset();
    apu.init(22050);
}

extern "C" void tearDown(void) {}

static AudioEvent make_event(WaveType type, float freq, float duration) {
    AudioEvent ev{};
    ev.type = type;
    ev.frequency = freq;
    ev.duration = duration;
    ev.volume = 1.0f;
    ev.duty = 0.5f;
    return ev;
}

static int16_t peak_abs(const int16_t* buf, int count) {
    int peak = 0;
    for (int i = 0; i < count; ++i) {
        const int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) peak = v;
    }
    return static_cast<int16_t>(peak);
}

void test_q15_pulse_produces_audio(void) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event = make_event(WaveType::PULSE, 440.0f, 0.5f);
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1024];
    apu.generateSamples(buffer, 1024);
    TEST_ASSERT_GREATER_THAN_INT16(100, peak_abs(buffer, 1024));
}

void test_q15_triangle_produces_audio(void) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event = make_event(WaveType::TRIANGLE, 220.0f, 0.5f);
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1024];
    apu.generateSamples(buffer, 1024);
    TEST_ASSERT_GREATER_THAN_INT16(100, peak_abs(buffer, 1024));
}

void test_q15_noise_produces_audio(void) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event = make_event(WaveType::NOISE, 0.0f, 0.5f);
    cmd.event.noisePeriod = 15;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1024];
    apu.generateSamples(buffer, 1024);
    TEST_ASSERT_GREATER_THAN_INT16(100, peak_abs(buffer, 1024));
}

void test_q15_silence_when_idle(void) {
    int16_t buffer[512];
    apu.generateSamples(buffer, 512);
    // DC-blocker settles to zero with no voices; allow tiny residual.
    TEST_ASSERT_LESS_THAN_INT16(4, peak_abs(buffer, 512));
}

void test_q15_master_volume_scales_before_compressor(void) {
    int16_t loud[1024];
    int16_t quiet[1024];

    AudioCommand play;
    play.type = AudioCommandType::PLAY_EVENT;
    play.event = make_event(WaveType::PULSE, 440.0f, 0.5f);

    TEST_ASSERT_TRUE(apu.submitCommand(play));
    apu.generateSamples(loud, 1024);
    const int16_t peakLoud = peak_abs(loud, 1024);

    apu.reset();
    apu.init(22050);
    AudioCommand vol;
    vol.type = AudioCommandType::SET_MASTER_VOLUME;
    vol.volume = 0.25f;
    TEST_ASSERT_TRUE(apu.submitCommand(vol));
    TEST_ASSERT_TRUE(apu.submitCommand(play));
    apu.generateSamples(quiet, 1024);
    const int16_t peakQuiet = peak_abs(quiet, 1024);

    TEST_ASSERT_GREATER_THAN_INT16(peakQuiet, peakLoud);
    TEST_ASSERT_GREATER_THAN_INT16(0, peakQuiet);
}

void test_q15_envelope_reaches_release_and_voice_ends(void) {
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event = make_event(WaveType::PULSE, 440.0f, 0.05f);
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1024];
    apu.generateSamples(buffer, 1024);
    TEST_ASSERT_EQUAL_size_t(1, apu.countEnabledVoicesForTesting());

    // 0.05 s @ 22050 Hz ≈ 1103 samples; render well past it (+ release tail).
    for (int i = 0; i < 40; ++i) {
        apu.generateSamples(buffer, 1024);
    }
    TEST_ASSERT_EQUAL_size_t(0, apu.countEnabledVoicesForTesting());
}

void test_q15_music_sequencer_ticks(void) {
    static const MusicNote notes[] = {
        {Note::C, 4, 0.25f, 0.8f, nullptr},
        {Note::E, 4, 0.25f, 0.8f, nullptr},
        {Note::G, 4, 0.25f, 0.8f, nullptr},
        {Note::C, 5, 0.25f, 0.8f, nullptr},
    };
    static const MusicTrack track = {notes, 4, true, WaveType::PULSE, 0.5f,
                                     nullptr, nullptr, nullptr};

    AudioCommand cmd;
    cmd.type = AudioCommandType::MUSIC_PLAY;
    cmd.track = &track;
    cmd.subTrackCount = 0;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1024];
    int16_t globalPeak = 0;
    for (int i = 0; i < 32; ++i) {
        apu.generateSamples(buffer, 1024);
        const int16_t p = peak_abs(buffer, 1024);
        if (p > globalPeak) globalPeak = p;
    }
    TEST_ASSERT_TRUE(apu.isMusicPlaying());
    TEST_ASSERT_GREATER_THAN_INT16(100, globalPeak);
    TEST_ASSERT_TRUE(apu.getMusicGlobalTick() > 0);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_q15_pulse_produces_audio);
    RUN_TEST(test_q15_triangle_produces_audio);
    RUN_TEST(test_q15_noise_produces_audio);
    RUN_TEST(test_q15_silence_when_idle);
    RUN_TEST(test_q15_master_volume_scales_before_compressor);
    RUN_TEST(test_q15_envelope_reaches_release_and_voice_ends);
    RUN_TEST(test_q15_music_sequencer_ticks);
    return UNITY_END();
}
