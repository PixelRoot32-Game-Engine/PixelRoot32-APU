/**
 * @file test_q15_smoke.cpp
 * @brief Smoke coverage for the integer/Q15 render path compiled natively
 *        (PR32_APU_HAS_FPU=0).
 *
 * The extraction analysis found that no firmware or test build compiled the
 * Q15 branch at all. This suite closes that gap: it forces the integer path
 * on the host and checks the behaviours the float path cannot cover.
 *
 * Since Hito 5 (M12) the gain staging is unified, so the absolute levels
 * asserted here are the same numbers `test_apu_core` pins on the float path.
 * That is deliberate: cross-path parity is the property under test, and the
 * only way to check it from two separate binaries is to pin both to the
 * closed-form value of the mixing chain.
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

// Overdrive regime: four loud voices push the mix past unity. Mean magnitude,
// not peak — above unity every curve is flattened to the same rail by the
// final clamp, so the peak cannot tell them apart but the average can. Same
// reasoning as `measureOverdrive` on the float side.
static int32_t q15_softclip_mean(SoftClipMode mode) {
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

    int64_t total = 0;
    for (int i = 0; i < 1024; ++i) {
        total += (buffer[i] < 0) ? -(int32_t)buffer[i] : (int32_t)buffer[i];
    }
    return static_cast<int32_t>(total / 1024);
}

void test_q15_softclip_default_is_rational(void) {
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::Rational,
                          (int)apu.getSoftClipModeForTesting());
}

void test_q15_softclip_rational_compresses_below_none(void) {
    const int32_t meanNone = q15_softclip_mean(SoftClipMode::None);
    const int32_t meanRational = q15_softclip_mean(SoftClipMode::Rational);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, meanRational,
        "Q15 path: the rational curve must still produce signal");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(meanRational, meanNone,
        "Q15 path: an overdriven mix must be louder unshaped than compressed");
}

// Hito 5 M12: pins the unified gain staging. MIXER_SCALE is applied exactly
// once — the summation feeds `audio_mixer_lut` the RAW channel sum, which is
// the convention the table was fitted to (`x = sum * 1.6 / 131072`, so its
// endpoints are four full-scale channels at S = 1.6, LUT[0] = -29126).
//
// One voice at full volume gives S = MIXER_SCALE = 0.4, below unity, so no
// clamp engages and each curve's shape reaches the output untouched. These
// are the SAME numbers `test_softclip_curve_shapes_below_unity` pins on the
// float path — that identity is the point of the milestone:
//   None      0.4                          -> 0.4    * 32767 = 13107
//   Rational  LUT(0.4) = 0.4/(1 + 0.4*0.5) -> 0.3333 * 32767 = 10922
//   Tanh      0.4*(27+0.16)/(27+1.44)      -> 0.3820 * 32767 = 12519
//   HardClip  clamp(0.4) = no-op           -> identical to None
// Absolute values on purpose: a reintroduced pad or a second MIXER_SCALE
// moves them by multiples, which relational assertions would not catch.
void test_q15_softclip_curve_shapes_match_the_float_path(void) {
    TEST_ASSERT_INT32_WITHIN_MESSAGE(60, 13107,
        q15_curve_peak_below_unity(SoftClipMode::None),
        "Q15 path: unshaped level must be S * 32767 with S = MIXER_SCALE");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(60, 10922,
        q15_curve_peak_below_unity(SoftClipMode::Rational),
        "Q15 path: the rational curve must match S/(1+|S|*MIXER_K)");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(60, 12519,
        q15_curve_peak_below_unity(SoftClipMode::Tanh),
        "Q15 path: the tanh approximation must match S*(27+S^2)/(27+9*S^2)");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(60, 13107,
        q15_curve_peak_below_unity(SoftClipMode::HardClip),
        "Q15 path: below unity HardClip must be a no-op, identical to None");
}

// Hito 5 M12: the `>>1` pad used to keep the output below half scale, so the
// final int16 cast could never overflow. Without it an unshaped four-voice
// mix reaches S = 1.6 (52428 in Q15) and the cast wraps unless the path
// clamps explicitly, exactly as the float branch does before its own cast.
//
// A wrapped output is unmistakable: 52428 becomes -13108, so the extremes
// collapse to ~+-13107 instead of pinning both rails. Checked as min/max
// rather than through `peak_abs`, whose int16_t return cannot represent the
// magnitude of the negative rail.
void test_q15_full_scale_mix_saturates_without_wrapping(void) {
    apu.reset();
    apu.init(22050);
    apu.setSoftClipMode(SoftClipMode::None);
    apu.setHpfCutoffHz(0.0f);   // bypass: the HPF has its own clamp

    for (int i = 0; i < 4; ++i) {
        AudioCommand cmd;
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event = make_event(WaveType::PULSE, 220.0f, 0.0f);
        cmd.event.loop = true;
        TEST_ASSERT_TRUE(apu.submitCommand(cmd));
    }

    int16_t buffer[1024];
    apu.generateSamples(buffer, 1024);   // dispatch + settle the ADSR
    apu.generateSamples(buffer, 1024);

    int32_t lowest = 32767;
    int32_t highest = -32768;
    for (int i = 0; i < 1024; ++i) {
        if (buffer[i] < lowest) lowest = buffer[i];
        if (buffer[i] > highest) highest = buffer[i];
    }

    TEST_ASSERT_EQUAL_INT32_MESSAGE(32767, highest,
        "Q15 path: an over-unity mix must saturate at the top rail, not wrap");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(-32768, lowest,
        "Q15 path: an over-unity mix must saturate at the bottom rail too");
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
    RUN_TEST(test_q15_softclip_curve_shapes_match_the_float_path);
    RUN_TEST(test_q15_full_scale_mix_saturates_without_wrapping);
    RUN_TEST(test_q15_softclip_all_modes_stay_in_range);
    return UNITY_END();
}
