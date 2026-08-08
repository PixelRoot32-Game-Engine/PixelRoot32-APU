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

// Hito 4 M10: the configurable HPF has a second implementation on this path,
// and test_apu_core only ever exercises the float one. Cover the Q15 branch
// here so a divergence between the two cannot pass unnoticed.
// Absolute DC of the first window after a 12.5%-duty note starts. The
// asymmetric duty carries a large DC offset, which is precisely what the
// filter removes, so bypass and any active corner separate by an order of
// magnitude rather than a handful of counts.
static int32_t measure_hpf_dc(float cutoffHz) {
    apu.reset();
    apu.init(22050);
    if (cutoffHz >= 0.0f) {
        apu.setHpfCutoffHz(cutoffHz);
    }

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event = make_event(WaveType::PULSE, 110.0f, 0.0f);
    cmd.event.duty = 0.125f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[512];
    apu.generateSamples(buffer, 512);

    int64_t total = 0;
    for (int i = 0; i < 512; ++i) {
        total += buffer[i];
    }
    const int32_t dc = static_cast<int32_t>(total / 512);
    return dc < 0 ? -dc : dc;
}

void test_q15_hpf_default_is_legacy_coefficient(void) {
    TEST_ASSERT_TRUE(apu.getHpfEnabledForTesting());
    TEST_ASSERT_EQUAL_INT32(32604, apu.getHpfRQ15ForTesting());
}

void test_q15_hpf_cutoff_recomputes_coefficient(void) {
    // setUp() init()s at 22050, so exp(-2*pi*90/22050) = 0.9746804
    apu.setHpfCutoffHz(90.0f);
    TEST_ASSERT_INT32_WITHIN(3,
        (int32_t)(0.9746804f * 32768.0f + 0.5f), apu.getHpfRQ15ForTesting());
}

void test_q15_hpf_cutoff_changes_low_frequency_content(void) {
    const int32_t dcBypass = measure_hpf_dc(0.0f);
    const int32_t dc90 = measure_hpf_dc(90.0f);
    const int32_t dc240 = measure_hpf_dc(240.0f);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(1000, dcBypass,
        "Q15 path: a bypassed HPF must leave the duty-asymmetry DC intact");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(dc90, dcBypass / 2,
        "Q15 path: an active corner must remove most of the DC");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(dc240, dc90,
        "Q15 path: a 240 Hz corner must settle the DC faster than 90 Hz");
}

// Hito 4 M11: the soft-clip curves have a second implementation on this path.
// `Rational` keeps the pre-fitted LUT; the other modes are computed from the
// accumulated sum, so a scaling mistake there would only show up here.
// One voice at full volume: the mix stays well below unity, so no clamp
// engages and each curve's shape reaches the output untouched.
static int32_t q15_curve_peak_below_unity(SoftClipMode mode) {
    apu.reset();
    apu.init(22050);
    apu.setSoftClipMode(mode);
    apu.setHpfCutoffHz(0.0f);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event = make_event(WaveType::PULSE, 220.0f, 0.0f);
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[2048];
    apu.generateSamples(buffer, 2048);
    return peak_abs(buffer, 2048);
}

static int32_t q15_softclip_peak(SoftClipMode mode) {
    apu.reset();
    apu.init(22050);
    apu.setSoftClipMode(mode);
    apu.setHpfCutoffHz(0.0f);   // bypass the HPF so we measure the curve alone

    for (int i = 0; i < 4; ++i) {
        AudioCommand cmd;
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event = make_event(WaveType::PULSE, 220.0f + 55.0f * i, 0.0f);
        cmd.event.loop = true;
        apu.submitCommand(cmd);
    }

    int16_t buffer[1024];
    apu.generateSamples(buffer, 1024);   // dispatch + warm up
    apu.generateSamples(buffer, 1024);
    return peak_abs(buffer, 1024);
}

void test_q15_softclip_default_is_rational(void) {
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::Rational,
                          (int)apu.getSoftClipModeForTesting());
}

void test_q15_softclip_rational_compresses_below_none(void) {
    const int32_t peakNone = q15_softclip_peak(SoftClipMode::None);
    const int32_t peakRational = q15_softclip_peak(SoftClipMode::Rational);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, peakRational,
        "Q15 path: the rational curve must still produce signal");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(peakRational, peakNone,
        "Q15 path: an overdriven mix must be louder unshaped than compressed");
}

// Pins the Q15 scaling of the computed modes against the LUT-driven default.
//
// The integer path feeds the curves `x = sum * 0.4`, matching the convention
// baked into `audio_mixer_lut` (which maps its input through
// `x = sum * 1.6 / 131072`). One voice at full volume gives sum = 0.4*32767,
// so x = 0.16, and the `>>1` output pad halves everything:
//   None      0.16                          -> 2622
//   Rational  LUT(0.16)                     -> 2462
//   Tanh      0.16*(27+x^2)/(27+9x^2)       -> 2602
//   HardClip  clamp(0.16) = no-op           -> 2622
// Absolute values on purpose: a wrong fixed-point factor moves them by
// multiples, which relational assertions between the modes would not catch.
void test_q15_softclip_computed_modes_track_the_lut(void) {
    TEST_ASSERT_INT32_WITHIN_MESSAGE(30, 2622,
        q15_curve_peak_below_unity(SoftClipMode::None),
        "Q15 path: unshaped level must match the LUT's input convention");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(30, 2462,
        q15_curve_peak_below_unity(SoftClipMode::Rational),
        "Q15 path: the rational curve must still come from audio_mixer_lut");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(30, 2602,
        q15_curve_peak_below_unity(SoftClipMode::Tanh),
        "Q15 path: the tanh approximation must be computed at the same scale");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(30, 2622,
        q15_curve_peak_below_unity(SoftClipMode::HardClip),
        "Q15 path: below unity HardClip must be a no-op, identical to None");
}

void test_q15_softclip_all_modes_stay_in_range(void) {
    const SoftClipMode modes[4] = {
        SoftClipMode::None, SoftClipMode::Rational,
        SoftClipMode::Tanh, SoftClipMode::HardClip
    };
    for (int m = 0; m < 4; ++m) {
        apu.reset();
        apu.init(22050);
        apu.setSoftClipMode(modes[m]);

        for (int i = 0; i < 4; ++i) {
            AudioCommand cmd;
            cmd.type = AudioCommandType::PLAY_EVENT;
            cmd.event = make_event(WaveType::PULSE, 220.0f + 55.0f * i, 0.0f);
            cmd.event.loop = true;
            apu.submitCommand(cmd);
        }

        int16_t buffer[1024];
        apu.generateSamples(buffer, 1024);
        apu.generateSamples(buffer, 1024);
        for (int i = 0; i < 1024; ++i) {
            TEST_ASSERT_TRUE(buffer[i] >= -32768 && buffer[i] <= 32767);
        }
    }
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
    RUN_TEST(test_q15_hpf_default_is_legacy_coefficient);
    RUN_TEST(test_q15_hpf_cutoff_recomputes_coefficient);
    RUN_TEST(test_q15_hpf_cutoff_changes_low_frequency_content);
    RUN_TEST(test_q15_softclip_default_is_rational);
    RUN_TEST(test_q15_softclip_rational_compresses_below_none);
    RUN_TEST(test_q15_softclip_computed_modes_track_the_lut);
    RUN_TEST(test_q15_softclip_all_modes_stay_in_range);
    return UNITY_END();
}
