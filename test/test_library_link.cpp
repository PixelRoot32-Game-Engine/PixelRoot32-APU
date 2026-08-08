/**
 * @file test_library_link.cpp
 * @brief Links the SHIPPED library target and calls the whole public API.
 *
 * Every other suite compiles `src/ApuCore.cpp` itself with `UNIT_TEST=1`. That
 * means a definition accidentally left inside a `#if defined(UNIT_TEST)` block
 * still resolves in all of them, and the gap stays invisible here while every
 * consumer that links `PixelRoot32::APU` gets an undefined reference — which is
 * exactly how the entire NES public API (Hitos 2-4) shipped unlinkable.
 *
 * So this target is deliberately different from the rest: it links the library
 * instead of recompiling the source, and it must NOT define UNIT_TEST.
 *
 * It asserts almost nothing on purpose. Behaviour is covered by the other
 * suites; what this one proves is that every public symbol EXISTS in the
 * artifact consumers actually receive. The calls only have to be reachable at
 * link time, so keep this file exhaustive rather than deep: when a new public
 * method is added to ApuCore, add a call here.
 */

#include <unity.h>
#include <cstdint>

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

#if defined(UNIT_TEST)
#error "test_library_link must be compiled WITHOUT UNIT_TEST: it exists to link the shipped library"
#endif

using namespace pixelroot32::audio;

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

static void post_mix_noop(int16_t*, int, void*) {}

// Transport, mixing and the output stage (Hito 4).
void test_link_core_and_output_stage(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSequencerNoteLimit(16);
    apu.setPostMixMono(post_mix_noop, nullptr);
    apu.setHpfCutoffHz(90.0f);
    apu.setSoftClipMode(SoftClipMode::Tanh);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.05f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256];
    apu.generateSamples(buffer, 256);

    ApuCore::ProfileEntry entries[8];
    uint8_t count = 0;
    apu.getAndResetProfileStats(entries, count);
}

// Waveform mode selectors (Hito 1 M2, Hito 3 M8).
void test_link_waveform_mode_selectors(void) {
    ApuCore apu;
    apu.init(44100);
    apu.setVoicePulseDutyMode(0, 2);
    apu.setVoiceNesNoiseLutMode(0, 7);
}

// The NES sub-units (Hito 2 M3-M7, Hito 2 M13). These are the symbols that
// were missing from the shipped library.
void test_link_nes_sub_units(void) {
    ApuCore apu;
    apu.init(44100);

    apu.setNesFrameCounterMode(0);
    apu.setNesFrameCounterInhibit(true);
    (void)apu.getNesFrameCounterIrq();

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 5, false);
    apu.setVoiceNesLinearCounter(0, 64, true);
    apu.setVoiceNesLinearEnabled(0, true);
    apu.setVoiceNesEnvelope(0, false, true, 12);
    apu.setVoiceNesEnvelopeEnabled(0, true);
    apu.setVoiceNesSweep(0, true, 2, false, 1, false);
    apu.setVoiceNesSweepUnitEnabled(0, true);
    apu.setVoiceNesTimer(0, 512);

    // tickNesFrameCounter is private but reached through the render loop, and
    // it was missing from the library too — so render with the counter armed.
    int16_t buffer[128];
    apu.generateSamples(buffer, 128);
}

// The bundled per-voice options (Hito 4 M14) and their queued counterpart.
void test_link_nes_options(void) {
    ApuCore apu;
    apu.init(44100);

    VoiceNesOptions opts{};
    opts.lengthCounterEnabled = true;
    opts.envelopeEnabled = true;
    apu.setVoiceNesOptions(0, opts);
    const VoiceNesOptions readBack = apu.getVoiceNesOptions(0);
    TEST_ASSERT_TRUE(readBack.lengthCounterEnabled);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = 0;
    cmd.nesLengthIndex = 3;
    cmd.nesLengthHalt = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[128];
    apu.generateSamples(buffer, 128);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_link_core_and_output_stage);
    RUN_TEST(test_link_waveform_mode_selectors);
    RUN_TEST(test_link_nes_sub_units);
    RUN_TEST(test_link_nes_options);
    return UNITY_END();
}
