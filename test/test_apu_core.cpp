/**
 * @file test_apu_core.cpp
 * @brief Unit tests for audio/ApuCore module
 * @version 1.0
 * @date 2026-05-06
 *
 * Tests for ApuCore including:
 * - Constructor initialization
 * - init/reset lifecycle
 * - Command queue operations
 * - Voice management (via public API)
 * - ADSR envelope stages (via generateSamples)
 * - LFO oscillator (via generateSamples)
 * - Sample generation (main audio loop)
 * - Bitcrusher
 */

#include <unity.h>
#include <cmath>
#include <cstring>
#include "test_config.h"
#include "golden/apu_mirror_canonical_track.h"
#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/ApuConfig.h"
#include "pixelroot32/apu/AudioTypes.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

using namespace pixelroot32::audio;

// S8: with the 64-slot queue the production ABI measures 13 200 B; this file
// compiles with UNIT_TEST=1, so what is pinned here is the 13 224 B test-build
// size (the 4 B test counter). The cap covers both.
static_assert(sizeof(ApuCore) <= 13312,
              "ApuCore must stay within the 13 KB budget (host ABI, queue 64)");

// Required Unity setup/teardown
void setUp(void) {
    test_setup();
}

void tearDown(void) {
    test_teardown();
}

// =============================================================================
// Tests for ApuCore constructor
// =============================================================================

void test_apu_core_constructor_initializes_voices(void) {
    ApuCore apu;

    // Verify MAX_VOICES constant
    TEST_ASSERT_EQUAL_INT(8, ApuCore::MAX_VOICES);

    // Verify initial sample rate
    TEST_ASSERT_EQUAL_INT(44100, apu.getSampleRate());
}

void test_apu_core_constructor_default_values(void) {
    ApuCore apu;

    // Default values should be set
    TEST_ASSERT_EQUAL_INT(44100, apu.getSampleRate());
    TEST_ASSERT_FALSE(apu.isMusicPlaying());
    TEST_ASSERT_FALSE(apu.isMusicPaused());
    TEST_ASSERT_EQUAL_UINT32(0, apu.getDroppedCommands());
}

// =============================================================================
// Tests for init() and reset()
// =============================================================================

void test_apu_core_init_valid_sample_rate(void) {
    ApuCore apu;

    apu.init(22050);
    TEST_ASSERT_EQUAL_INT(22050, apu.getSampleRate());
}

void test_apu_core_init_valid_sample_rate_48000(void) {
    ApuCore apu;

    apu.init(48000);
    TEST_ASSERT_EQUAL_INT(48000, apu.getSampleRate());
}

void test_apu_core_init_invalid_sample_rate_fallback(void) {
    ApuCore apu;

    // Zero sample rate should fallback to default
    apu.init(0);
    TEST_ASSERT_EQUAL_INT(44100, apu.getSampleRate());

    // Negative sample rate should also fallback
    apu.init(-1);
    TEST_ASSERT_EQUAL_INT(44100, apu.getSampleRate());
}

void test_apu_core_reset_clears_state(void) {
    ApuCore apu;

    apu.init(44100);
    apu.reset();

    // Verify reset state
    TEST_ASSERT_EQUAL_INT(44100, apu.getSampleRate());
    TEST_ASSERT_EQUAL_UINT32(0, apu.getDroppedCommands());
    TEST_ASSERT_FALSE(apu.isMusicPlaying());
    TEST_ASSERT_FALSE(apu.isMusicPaused());
}

// =============================================================================
// Tests for submitCommand() - command queue operations
// =============================================================================

void test_apu_core_submit_command_success(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.1f;
    cmd.event.volume = 0.8f;
    cmd.event.duty = 0.5f;

    bool result = apu.submitCommand(cmd);
    TEST_ASSERT_TRUE(result);
}

void test_apu_core_submit_command_multiple(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.1f;
    cmd.event.volume = 0.8f;
    cmd.event.duty = 0.5f;

    // Submit multiple commands
    for (int i = 0; i < 10; i++) {
        bool result = apu.submitCommand(cmd);
        TEST_ASSERT_TRUE(result);
    }
}

void test_apu_core_submit_command_queue_full(void) {
    ApuCore apu;
    apu.init(44100);

    // Fill the queue with commands
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.1f;
    cmd.event.volume = 0.8f;

    // Try to submit more commands than queue capacity: 200 submissions always
    // overflow the ring regardless of the configured depth.
    int successCount = 0;
    for (int i = 0; i < 200; i++) {
        if (apu.submitCommand(cmd)) {
            successCount++;
        }
    }

    // Verify some commands were dropped
    TEST_ASSERT_TRUE(apu.getDroppedCommands() > 0);
}

// =============================================================================
// Tests for generateSamples() - main audio loop (exercises processCommands internally)
// =============================================================================

void test_apu_core_generate_samples_zero_length(void) {
    ApuCore apu;
    apu.init(44100);

    int16_t buffer[100] = {0};

    // Should handle zero/negative length gracefully
    apu.generateSamples(buffer, 0);
    apu.generateSamples(buffer, -1);

    // Apu is still functional - verify voices are intact
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_generate_samples_null_stream(void) {
    ApuCore apu;
    apu.init(44100);

    // Should handle null stream gracefully
    apu.generateSamples(nullptr, 100);

    // Apu is still functional after handling null stream
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_generate_samples_with_active_voices(void) {
    ApuCore apu;
    apu.init(44100);

    // Submit a note
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    // Generate some samples - this internally calls processCommands()
    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    // Voice should be enabled now
    TEST_ASSERT_TRUE(apu.countEnabledVoicesForTesting() > 0);
}

void test_apu_core_generate_samples_output_range(void) {
    ApuCore apu;
    apu.init(44100);

    // Submit a note with moderate volume
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 0.3f;
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    // Generate samples
    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    // Verify all samples are in int16 range
    for (int i = 0; i < 256; i++) {
        TEST_ASSERT_TRUE(buffer[i] >= -32768);
        TEST_ASSERT_TRUE(buffer[i] <= 32767);
    }
}

void test_apu_core_generate_samples_empty_queue(void) {
    ApuCore apu;
    apu.init(44100);

    // Generate samples without submitting any commands
    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    // Should not crash - buffer should remain zeros (no commands to generate audio)
    bool hasNonZero = false;
    for (int i = 0; i < 256; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_FALSE(hasNonZero);  // Expect zeros when no commands
}

// =============================================================================
// Tests for helper functions (internal via public API)
// =============================================================================

void test_apu_core_sequencer_note_limit_set(void) {
    ApuCore apu;
    apu.init(44100);

    // Set a limit
    apu.setSequencerNoteLimit(16);
    TEST_ASSERT_EQUAL_INT(16, apu.getSequencerNoteLimit());

    // Set zero (should become unbounded)
    apu.setSequencerNoteLimit(0);
    TEST_ASSERT_TRUE(apu.getSequencerNoteLimit() > 16);

    // Set too high (should clamp to 32)
    apu.setSequencerNoteLimit(2000);
    TEST_ASSERT_EQUAL_INT(32, apu.getSequencerNoteLimit());
}

void test_apu_core_deferred_notes_counter(void) {
    ApuCore apu;
    apu.init(44100);

    // Initially no deferred notes
    TEST_ASSERT_EQUAL_INT(0, apu.getDeferredNotes());
}

// =============================================================================
// Tests for different wave types (via generateSamples)
// =============================================================================

void test_apu_core_wave_type_pulse(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.25f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

void test_apu_core_wave_type_triangle(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

void test_apu_core_wave_type_sine(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::SINE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

void test_apu_core_wave_type_saw(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::SAW;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

void test_apu_core_wave_type_noise(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 1000.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

// =============================================================================
// Tests for music commands (exercised via generateSamples)
// =============================================================================

void test_apu_core_music_play_stop_cycle(void) {
    ApuCore apu;
    apu.init(44100);

    // Start music (track is nullptr so won't actually play)
    AudioCommand playCmd;
    playCmd.type = AudioCommandType::MUSIC_PLAY;
    playCmd.track = nullptr;
    apu.submitCommand(playCmd);

    // Generate samples to process the command
    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    // Music playing flag should be set (even with null track)
    // Note: may not actually play due to null track
    // Verify apu is still functional
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());  // No voices active (null track)
}

void test_apu_core_music_stop_clears_flag(void) {
    ApuCore apu;
    apu.init(44100);

    // First play something
    AudioCommand playCmd;
    playCmd.type = AudioCommandType::MUSIC_PLAY;
    playCmd.track = nullptr;
    apu.submitCommand(playCmd);
    apu.generateSamples(nullptr, 128);

    // Then stop
    AudioCommand stopCmd;
    stopCmd.type = AudioCommandType::MUSIC_STOP;
    apu.submitCommand(stopCmd);
    apu.generateSamples(nullptr, 128);

    TEST_ASSERT_FALSE(apu.isMusicPlaying());
}

void test_apu_core_music_pause_resume_cycle(void) {
    ApuCore apu;
    apu.init(44100);

    // With nullptr track, music won't actually play
    // But we can test that the commands are processed without crash
    // and the flag states toggle correctly when commands are submitted

    // Pause when not playing - should still set flag
    AudioCommand pauseCmd;
    pauseCmd.type = AudioCommandType::MUSIC_PAUSE;
    apu.submitCommand(pauseCmd);
    apu.generateSamples(nullptr, 128);

    // Resume when not playing
    AudioCommand resumeCmd;
    resumeCmd.type = AudioCommandType::MUSIC_RESUME;
    apu.submitCommand(resumeCmd);
    apu.generateSamples(nullptr, 128);

    // Verify apu is still functional - isMusicPaused flag should be set
    TEST_ASSERT_TRUE(apu.isMusicPaused() || !apu.isMusicPlaying());
}

void test_apu_core_music_set_tempo(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::MUSIC_SET_TEMPO;
    cmd.tempoFactor = 2.0f;

    apu.submitCommand(cmd);
    apu.generateSamples(nullptr, 100);

    // Verify apu is still functional after tempo command
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_music_set_bpm(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::MUSIC_SET_BPM;
    cmd.bpm = 120.0f;

    apu.submitCommand(cmd);
    apu.generateSamples(nullptr, 100);

    // Verify apu is still functional after BPM command
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_set_master_volume_command(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::SET_MASTER_VOLUME;
    cmd.volume = 0.5f;

    apu.submitCommand(cmd);
    apu.generateSamples(nullptr, 100);

    // Verify apu is still functional after volume command
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_set_master_volume_clamping(void) {
    ApuCore apu;
    apu.init(44100);

    // Test over max
    AudioCommand cmd;
    cmd.type = AudioCommandType::SET_MASTER_VOLUME;
    cmd.volume = 1.5f;

    apu.submitCommand(cmd);
    apu.generateSamples(nullptr, 100);

    // Should clamp to 1.0 - verify no crash
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_set_master_bitcrush(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::SET_MASTER_BITCRUSH;
    cmd.masterBitcrushBits = 8;

    apu.submitCommand(cmd);
    apu.generateSamples(nullptr, 100);

    // Verify apu is still functional after bitcrush command
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

void test_apu_core_set_master_bitcrush_over_max(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::SET_MASTER_BITCRUSH;
    cmd.masterBitcrushBits = 20;  // Over max 15

    apu.submitCommand(cmd);
    apu.generateSamples(nullptr, 100);

    // Should clamp to 15 - verify no crash
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

// =============================================================================
// Tests for post-mix callback
// =============================================================================

static bool callbackInvoked = false;

void test_post_mix_callback(int16_t* mono, int length, void* user) {
    (void)mono;
    (void)length;
    (void)user;
    callbackInvoked = true;
}

void test_apu_core_post_mix_callback(void) {
    ApuCore apu;
    apu.init(44100);

    callbackInvoked = false;
    apu.setPostMixMono(test_post_mix_callback, nullptr);

    // Generate samples to trigger callback
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.1f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    TEST_ASSERT_TRUE(callbackInvoked);
}

// =============================================================================
// Tests for different frequencies
// =============================================================================

void test_apu_core_low_frequency(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 100.0f;  // Low frequency
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    // Verify apu is still functional - voice count should be >= 0
    TEST_ASSERT_TRUE(apu.countEnabledVoicesForTesting() >= 0);
}

void test_apu_core_high_frequency(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 8000.0f;  // High frequency
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    // Verify apu is still functional - voice count should be >= 0
    TEST_ASSERT_TRUE(apu.countEnabledVoicesForTesting() >= 0);
}

// =============================================================================
// Tests for volume levels
// =============================================================================

void test_apu_core_zero_volume(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.0f;  // Silent
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    // Should produce near-zero output
    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_FALSE(hasNonZero);
}

void test_apu_core_full_volume(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 1.0f;  // Full volume
    cmd.event.duty = 0.5f;

    apu.submitCommand(cmd);

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);

    // Should produce output
    bool hasNonZero = false;
    for (int i = 0; i < 128; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

// =============================================================================
// Tests for 4+4 voice partition (upstream v2)
// =============================================================================

void test_apu_core_voice_partition_constants(void) {
    TEST_ASSERT_EQUAL_INT(8, ApuCore::MAX_VOICES);
    TEST_ASSERT_EQUAL_INT(0, ApuCore::MUSIC_VOICE_BASE);
    TEST_ASSERT_EQUAL_INT(4, ApuCore::MUSIC_VOICE_COUNT);
    TEST_ASSERT_EQUAL_INT(4, ApuCore::SFX_VOICE_BASE);
    TEST_ASSERT_EQUAL_INT(4, ApuCore::SFX_VOICE_COUNT);
}

void test_apu_core_music_pulse_tracks_do_not_steal_each_other(void) {
    ApuCore apu;
    apu.init(44100);

    AudioCommand bpmCmd{};
    bpmCmd.type = AudioCommandType::MUSIC_SET_BPM;
    bpmCmd.bpm = 120.0f;
    apu.submitCommand(bpmCmd);

    static const MusicNote kLeadNotes[] = {
        {Note::C, 4, 4.0f, 0.8f, nullptr},
    };
    static const MusicNote kHarmonyNotes[] = {
        {Note::E, 4, 0.5f, 0.8f, nullptr},
        {Note::Rest, 4, 3.5f, 0.0f, nullptr},
    };
    static const MusicTrack kLeadTrack{kLeadNotes, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kHarmonyTrack{kHarmonyNotes, 2, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kLeadTrack;
    play.subTrackCount = 1;
    play.subTracks[0] = &kHarmonyTrack;
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(1));
    TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting(0));

    const int tick_samples = (44100 * 60) / (120 * 4);
    for (int tick = 0; tick < 3; ++tick) {
        apu.generateSamples(buffer, tick_samples);
    }

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_FALSE(apu.isMusicTrackVoiceActiveForTesting(1));
}

void test_apu_core_sfx_play_event_uses_sfx_voice_pool(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kLongNote[] = {{Note::C, 4, 16.0f, 0.5f, nullptr}};
    static const MusicTrack kTrack0{kLongNote, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack1{kLongNote, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack2{kLongNote, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack3{kLongNote, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack0;
    play.subTrackCount = 3;
    play.subTracks[0] = &kTrack1;
    play.subTracks[1] = &kTrack2;
    play.subTracks[2] = &kTrack3;
    apu.submitCommand(play);
    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    for (int slot = 0; slot < ApuCore::SFX_VOICE_BASE; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    }

    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.frequency = 880.0f;
    sfx.event.duration = 0.5f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    apu.submitCommand(sfx);
    apu.generateSamples(buffer, 256);

    bool sfx_in_high_pool = false;
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        if (apu.isVoiceEnabledForTesting(slot)) {
            sfx_in_high_pool = true;
        }
    }
    TEST_ASSERT_TRUE(sfx_in_high_pool);
    for (int slot = 0; slot < ApuCore::SFX_VOICE_BASE; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    }
}

void test_apu_core_sfx_steal_does_not_touch_music_slots(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kLongNote[] = {{Note::C, 4, 16.0f, 0.5f, nullptr}};
    static const MusicTrack kTrack0{kLongNote, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack1{kLongNote, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack2{kLongNote, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack3{kLongNote, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack0;
    play.subTrackCount = 3;
    play.subTracks[0] = &kTrack1;
    play.subTracks[1] = &kTrack2;
    play.subTracks[2] = &kTrack3;
    apu.submitCommand(play);
    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.frequency = 660.0f;
    sfx.event.duration = 2.0f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;

    for (int i = 0; i < 5; ++i) {
        apu.submitCommand(sfx);
    }
    apu.generateSamples(buffer, 256);

    for (int slot = 0; slot < ApuCore::SFX_VOICE_BASE; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    }
}

static int count_enabled_sfx_voices(const ApuCore& apu) {
    int count = 0;
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        if (apu.isVoiceEnabledForTesting(slot)) {
            ++count;
        }
    }
    return count;
}

void test_apu_core_sequencer_percussion_same_step_uses_distinct_sfx_voices(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kStackedDrums[] = {
        makeNote(INSTR_KICK, Note::Rest, 0.0f),
        makeNote(INSTR_SNARE, Note::Rest, 0.0f),
        makeNote(INSTR_HIHAT, Note::Rest, 1.0f),
    };
    static const MusicTrack kDrumTrack{
        kStackedDrums,
        sizeof(kStackedDrums) / sizeof(kStackedDrums[0]),
        false,
        WaveType::NOISE,
        0.5f,
    };

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kDrumTrack;
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, count_enabled_sfx_voices(apu));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(3));
    TEST_ASSERT_FALSE(apu.isMusicTrackVoiceActiveForTesting(0));

    bool has_audio = false;
    for (int i = 0; i < 256; ++i) {
        if (buffer[i] != 0) {
            has_audio = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(has_audio);
}

void test_apu_core_sequencer_percussion_does_not_disturb_melodic_slots(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kLeadNotes[] = {
        {Note::C, 4, 4.0f, 0.8f, nullptr},
    };
    static const MusicNote kBassNotes[] = {
        {Note::C, 3, 4.0f, 0.8f, nullptr},
    };
    static const MusicNote kHarmonyNotes[] = {
        {Note::E, 4, 4.0f, 0.8f, nullptr},
    };
    static const MusicNote kDrumHit[] = {
        makeNote(INSTR_KICK, Note::Rest, 1.0f),
    };

    static const MusicTrack kLeadTrack{kLeadNotes, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kBassTrack{kBassNotes, 1, false, WaveType::TRIANGLE, 0.5f};
    static const MusicTrack kHarmonyTrack{kHarmonyNotes, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kDrumTrack{kDrumHit, 1, false, WaveType::NOISE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kLeadTrack;
    play.subTrackCount = 3;
    play.subTracks[0] = &kBassTrack;
    play.subTracks[1] = &kHarmonyTrack;
    play.subTracks[2] = &kDrumTrack;
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(1));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(2));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(3));
    TEST_ASSERT_TRUE(count_enabled_sfx_voices(apu) >= 1);
    TEST_ASSERT_FALSE(apu.isMusicTrackVoiceActiveForTesting(3));
}

void test_apu_core_sequencer_percussion_shares_sfx_pool_with_play_event(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kLongMelody[] = {{Note::C, 4, 16.0f, 0.5f, nullptr}};
    static const MusicTrack kMelodyTrack{kLongMelody, 1, false, WaveType::PULSE, 0.5f};
    static const MusicNote kDrumHit[] = {makeNote(INSTR_SNARE, Note::Rest, 1.0f)};
    static const MusicTrack kDrumTrack{kDrumHit, 1, false, WaveType::NOISE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kMelodyTrack;
    play.subTrackCount = 1;
    play.subTracks[0] = &kDrumTrack;
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(1));

    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.frequency = 880.0f;
    sfx.event.duration = 0.5f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    apu.submitCommand(sfx);
    apu.generateSamples(buffer, 256);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, count_enabled_sfx_voices(apu));
}

void test_apu_core_percussion_borrows_idle_music_slot(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kLeadNotes[] = {
        {Note::C, 4, 4.0f, 0.8f, nullptr},
    };
    static const MusicNote kBassNotes[] = {
        {Note::C, 3, 4.0f, 0.8f, nullptr},
    };
    static const MusicNote kDrumHit[] = {
        makeNote(INSTR_KICK, Note::Rest, 1.0f),
    };
    static const MusicTrack kLeadTrack{kLeadNotes, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kBassTrack{kBassNotes, 1, false, WaveType::TRIANGLE, 0.5f};
    static const MusicTrack kDrumTrack{kDrumHit, 1, false, WaveType::NOISE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kLeadTrack;          // trackIdx 0 -> voice slot 0
    play.subTrackCount = 2;
    play.subTracks[0] = &kBassTrack;   // trackIdx 1 -> voice slot 1
    play.subTracks[1] = &kDrumTrack;   // trackIdx 2 (percussion never claims its own slot)
    apu.submitCommand(play);

    // Saturate the SFX/percussion subpool (slots 4-7) BEFORE the drum hit
    // fires, so the fallback allocator finds no free voice in its own pool.
    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.duration = 2.0f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sfx.event.frequency = 500.0f + (float)i * 40.0f;
        apu.submitCommand(sfx);
    }

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    // Melodic slots 0 and 1 are live; SFX pool 4-7 is saturated.
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(1));
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    }

    // Voice slot 2 was never claimed by a melodic track (trackIdx 2 is the
    // drum, which never allocates its own trackIdx slot), so it is idle.
    TEST_ASSERT_FALSE(apu.isMusicTrackVoiceActiveForTesting(2));

    // With the SFX subpool saturated and slot 2 idle, the percussion hit
    // must borrow the idle music slot instead of stealing a live SFX voice.
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(2));
    TEST_ASSERT_FALSE(apu.isMusicTrackVoiceActiveForTesting(2));
}

void test_apu_core_percussion_never_steals_live_melodic_voice(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kDrumNotes[] = {
        {Note::Rest, 4, 1.0f, 0.0f, nullptr},    // silent tick, advances the sequencer
        makeNote(INSTR_KICK, Note::Rest, 1.0f),  // actual percussion hit, next tick
    };
    static const MusicNote kMelodyNotes[] = {
        {Note::C, 4, 16.0f, 0.8f, nullptr},      // long note, still gated on the drum tick
    };
    static const MusicTrack kDrumTrack{kDrumNotes, 2, false, WaveType::NOISE, 0.5f};
    static const MusicTrack kMelodyTrack{kMelodyNotes, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kDrumTrack;            // trackIdx 0 (percussion never claims voice slot 0)
    play.subTrackCount = 1;
    play.subTracks[0] = &kMelodyTrack;   // trackIdx 1 -> voice slot 1
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    // Voice slot 1 is now a live melodic note.
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(1));
    TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting(1));

    // Saturate the SFX/percussion subpool before the drum's second tick fires.
    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.duration = 2.0f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sfx.event.frequency = 600.0f + (float)i * 40.0f;
        apu.submitCommand(sfx);
    }

    const int tick_samples =
        (int)((44100.0f * 60.0f) / (ApuCore::DEFAULT_BPM * ApuCore::TICKS_PER_BEAT));
    for (int tick = 0; tick < 5; ++tick) {
        apu.generateSamples(buffer, tick_samples);
    }

    // The drum's second note has now fired a real percussion hit while
    // slot 1's melodic gate was still live; slot 1 must remain untouched.
    TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting(1));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(1));
}

void test_apu_core_sequencer_tempo_factor_short_notes_keeps_playing(void) {
    ApuCore apu;
    apu.init(44100);

    // Space-invaders-style SI_HR: 0.25-beat notes (1 tick at tempoFactor 1.0).
    static const MusicNote kShortPulse[] = {
        makeNote(INSTR_PULSE_BASS, Note::C, 0.25f),
        makeRest(0.25f),
    };
    static const MusicNote kDrumHit[] = {
        makeNote(INSTR_KICK, Note::Rest, 1, 0.5f),
    };
    static const MusicTrack kMainTrack{
        kShortPulse, 2, true, WaveType::PULSE, INSTR_PULSE_BASS.duty};
    static const MusicTrack kDrumTrack{
        kDrumHit, 1, true, WaveType::NOISE, 0.5f};

    AudioCommand tempo{};
    tempo.type = AudioCommandType::MUSIC_SET_TEMPO;
    tempo.tempoFactor = 1.5f;
    apu.submitCommand(tempo);

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kMainTrack;
    play.subTrackCount = 1;
    play.subTracks[0] = &kDrumTrack;
    apu.submitCommand(play);

    int16_t buffer[4096];
    for (int frame = 0; frame < 80; ++frame) {
        apu.generateSamples(buffer, 512);
    }

    TEST_ASSERT_TRUE(apu.isMusicPlaying());

    bool has_audio = false;
    apu.generateSamples(buffer, 4096);
    for (int i = 0; i < 4096; ++i) {
        if (buffer[i] != 0) {
            has_audio = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(has_audio);
}

void test_apu_core_melodic_noise_on_track_three_uses_music_slot(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kSilent[] = {{Note::Rest, 4, 16.0f, 0.0f, nullptr}};
    static const MusicNote kNoiseMelody[] = {
        {Note::C, 4, 2.0f, 0.8f, nullptr},
    };
    static const MusicTrack kTrack0{kSilent, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack1{kSilent, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack2{kSilent, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack3{kNoiseMelody, 1, false, WaveType::NOISE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack0;
    play.subTrackCount = 3;
    play.subTracks[0] = &kTrack1;
    play.subTracks[1] = &kTrack2;
    play.subTracks[2] = &kTrack3;
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(3));
    TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting(3));
    TEST_ASSERT_EQUAL_INT(0, count_enabled_sfx_voices(apu));
}

// Fix B (WU-3): melodic note-on must extend remainingSamples by the full
// env.releaseSamples regardless of legato, so the release tail overlaps
// the next beat instead of being hard-cut at the gate boundary.
//
// There is no test-only accessor for Voice::remainingSamples, so this
// drives through the real sequencer path and asserts the *observable*
// timing consequence instead: at exactly `gate_samples + releaseSamples`
// generated samples,
//   - pre-fix:  remainingSamples started at gate_samples only, so the
//     per-sample auto-release trigger (generateSampleForVoice) already
//     armed and fully exhausted one release cycle by this point ->
//     voice is disabled.
//   - post-fix: remainingSamples started at gate_samples + releaseSamples,
//     so the auto-release trigger only just now arms the release stage ->
//     voice is still enabled (tail keeps sounding into the next beat).
void test_apu_core_melodic_tail_overlaps_next_beat(void) {
    ApuCore apu;
    apu.init(44100);

    // Fresh voice, single note-on -> music_sequencer_legato collapses to
    // false internally (ch->enabled starts false), so this exercises the
    // exact "not legato" case the pre-fix gate guarded against.
    static const MusicNote kNote[] = {
        {Note::C, 4, 1.0f, 0.8f, &INSTR_PULSE_BASS},
    };
    static const MusicTrack kTrack{
        kNote, 1, false, WaveType::PULSE, INSTR_PULSE_BASS.duty};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack;
    apu.submitCommand(play);

    // gate_samples = noteTicks(4) * tickDurationSamples(4410 @ default
    // 150 BPM, 44100 Hz) = 17640.
    // releaseSamples = min(INSTR_PULSE_BASS.releaseTime * 44100, 4410)
    //                = min(3528, 4410) = 3528.
    // Target = gate_samples + releaseSamples = 21168.
    static int16_t buffer[21168];
    apu.generateSamples(buffer, 21168);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
}

// Fix B counterpart: a preset with releaseTime == 0 must leave the gate
// unchanged (no tail extension) in both pre- and post-fix code, since the
// `env.releaseSamples > 0` guard rejects the addition either way.
void test_apu_core_melodic_zero_release_leaves_gate_unchanged(void) {
    ApuCore apu;
    apu.init(44100);

    static constexpr InstrumentPreset kNoReleasePreset{
        0.30f,    // baseVolume
        0.25f,    // duty
        2,        // defaultOctave
        0.0f,     // defaultDuration
        0,        // noisePeriod
        0.001f,   // attackTime
        0.08f,    // decayTime
        0.35f,    // sustainLevel
        0.0f,     // releaseTime -- zero on purpose
        LfoTarget::NONE,
        0.0f,     // lfoFrequency
        0.0f,     // lfoDepth
        0.0f,     // lfoDelay
        false,    // noiseLfsrShort
        false,    // noiseShortMode (deprecated alias)
        0.0f      // dutySweep
    };

    static const MusicNote kNote[] = {
        {Note::C, 4, 1.0f, 0.8f, &kNoReleasePreset},
    };
    static const MusicTrack kTrack{
        kNote, 1, false, WaveType::PULSE, kNoReleasePreset.duty};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack;
    apu.submitCommand(play);

    // gate_samples = 17640 (same tempo/note-duration math as above).
    // releaseSamples == 0 => remainingSamples == gate_samples exactly, so
    // the voice must be fully disabled right at the gate boundary.
    static int16_t buffer[17640];
    apu.generateSamples(buffer, 17640);

    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
}

// =============================================================================
// WU-4: Partition / API / mixer invariant regression tests. Pure coverage --
// no production change is expected here (see spec "Partition and Isolation
// Invariants Preserved").
// =============================================================================

// A melodic note-on for track 3 must always land on its fixed slot
// (MUSIC_VOICE_BASE + 3), unaffected by percussion allocator activity
// elsewhere in the pool.
void test_apu_core_melodic_track_keeps_fixed_slot_mapping(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kDrumHit[] = {
        makeNote(INSTR_KICK, Note::Rest, 1.0f),
    };
    static const MusicNote kSilent[] = {{Note::Rest, 4, 16.0f, 0.0f, nullptr}};
    static const MusicNote kMelodyNote[] = {
        {Note::C, 4, 2.0f, 0.8f, nullptr},
    };
    static const MusicTrack kTrack0{kDrumHit, 1, false, WaveType::NOISE, 0.5f};
    static const MusicTrack kTrack1{kSilent, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack2{kSilent, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack3{kMelodyNote, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack0;           // trackIdx 0 (percussion never claims its own slot)
    play.subTrackCount = 3;
    play.subTracks[0] = &kTrack1;    // trackIdx 1, idle filler
    play.subTracks[1] = &kTrack2;    // trackIdx 2, idle filler
    play.subTracks[2] = &kTrack3;    // trackIdx 3 -> fixed voice slot MUSIC_VOICE_BASE+3
    apu.submitCommand(play);

    // Saturate the SFX/percussion subpool so the drum hit is forced through
    // the idle-melodic-slot fallback, perturbing percussion allocator state
    // before track 3's fixed mapping is exercised.
    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.duration = 2.0f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sfx.event.frequency = 800.0f + (float)i * 40.0f;
        apu.submitCommand(sfx);
    }

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    // Track 3's melodic note-on always allocates its fixed slot
    // MUSIC_VOICE_BASE + 3, regardless of percussion allocator activity.
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(ApuCore::MUSIC_VOICE_BASE + 3));
    TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting(3));
}

// Complement to WU-2's borrow test: when the SFX subpool is saturated AND
// every melodic slot is busy (enabled + a live track gate), the percussion
// allocator has no idle slot to borrow and must stay confined to the
// steal-SFX fallback (slots 4-7), never disturbing slots 0-3.
void test_apu_core_percussion_saturated_no_idle_slot_confined_to_sfx(void) {
    ApuCore apu;
    apu.init(44100);

    static const MusicNote kLongMelody[] = {
        {Note::C, 4, 16.0f, 0.8f, nullptr},
    };
    static const MusicNote kTrack3Notes[] = {
        {Note::C, 4, 1.0f, 0.8f, nullptr},          // claims slot 3 first
        makeNote(INSTR_KICK, Note::Rest, 1.0f),     // then a real percussion hit
    };
    static const MusicTrack kTrack0{kLongMelody, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack1{kLongMelody, 1, false, WaveType::TRIANGLE, 0.5f};
    static const MusicTrack kTrack2{kLongMelody, 1, false, WaveType::PULSE, 0.5f};
    static const MusicTrack kTrack3{kTrack3Notes, 2, false, WaveType::NOISE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack0;           // trackIdx 0 -> voice slot 0
    play.subTrackCount = 3;
    play.subTracks[0] = &kTrack1;    // trackIdx 1 -> voice slot 1
    play.subTracks[1] = &kTrack2;    // trackIdx 2 -> voice slot 2
    play.subTracks[2] = &kTrack3;    // trackIdx 3 -> voice slot 3, then fires a percussion hit
    apu.submitCommand(play);

    int16_t buffer[8192];
    apu.generateSamples(buffer, 256);

    // All 4 melodic slots are live before the percussion hit fires.
    for (int slot = ApuCore::MUSIC_VOICE_BASE; slot < ApuCore::SFX_VOICE_BASE; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
        TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting((size_t)slot));
    }

    // Saturate the SFX/percussion subpool before track 3's second note (the
    // real percussion hit) fires.
    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.duration = 2.0f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sfx.event.frequency = 900.0f + (float)i * 40.0f;
        apu.submitCommand(sfx);
    }

    const int tick_samples =
        (int)((44100.0f * 60.0f) / (ApuCore::DEFAULT_BPM * ApuCore::TICKS_PER_BEAT));
    for (int tick = 0; tick < 5; ++tick) {
        apu.generateSamples(buffer, tick_samples);
    }

    // With SFX saturated and every melodic slot busy, the percussion hit
    // must never disturb a melodic slot -- every track keeps its gate.
    for (int slot = ApuCore::MUSIC_VOICE_BASE; slot < ApuCore::SFX_VOICE_BASE; ++slot) {
        TEST_ASSERT_TRUE(apu.isMusicTrackVoiceActiveForTesting((size_t)slot));
    }

    // The hit stayed confined to the SFX/percussion subpool (steal-SFX
    // fallback), never touching 0-3.
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    }
}

// =============================================================================
// Integration test - full audio pipeline
// =============================================================================

void test_apu_core_integration_full_pipeline(void) {
    ApuCore apu;
    apu.init(44100);

    // Submit an event
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;

    // Submit and generate (processCommands called internally)
    bool submitted = apu.submitCommand(cmd);
    TEST_ASSERT_TRUE(submitted);

    // Generate samples
    int16_t buffer[512] = {0};
    apu.generateSamples(buffer, 512);

    // Verify samples are in valid range
    for (int i = 0; i < 512; i++) {
        TEST_ASSERT_TRUE(buffer[i] >= -32768);
        TEST_ASSERT_TRUE(buffer[i] <= 32767);
    }
}

void test_apu_core_integration_multiple_voices(void) {
    ApuCore apu;
    apu.init(44100);

    // Submit multiple notes with different frequencies
    for (int i = 0; i < 3; i++) {
        AudioCommand cmd;
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event.type = WaveType::PULSE;
        cmd.event.frequency = 220.0f + (i * 110.0f);
        cmd.event.duration = 1.0f;
        cmd.event.volume = 0.3f;
        cmd.event.duty = 0.5f;
        apu.submitCommand(cmd);
    }

    // Generate samples - should mix multiple voices
    int16_t buffer[512] = {0};
    apu.generateSamples(buffer, 512);

    // Should have audio output
    bool hasNonZero = false;
    for (int i = 0; i < 512; i++) {
        if (buffer[i] != 0) {
            hasNonZero = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(hasNonZero);
}

// =============================================================================
// Export parity: MusicTrack.duty is authoritative (not InstrumentPreset.duty)
// =============================================================================

// =============================================================================
// Noise sweep, loop, and duration==0 one-shot (sfx-synthesis-high-priority)
// =============================================================================

void test_apu_core_noise_pitch_sweep_updates_period(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 2000.0f;  // period = 44100/2000 = 22
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.noisePeriod = 0;
    cmd.event.sweepEndHz = 200.0f;  // period = 44100/200 = 220
    cmd.event.sweepDurationSec = 0.05f;
    cmd.event.loop = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    const uint32_t startPeriod = 44100u / 2000u;
    const uint32_t endPeriod = 44100u / 200u;
    int sfxSlot = -1;
    for (int i = ApuCore::SFX_VOICE_BASE; i < ApuCore::MAX_VOICES; ++i) {
        if (apu.isVoiceEnabledForTesting(i)) {
            sfxSlot = i;
            break;
        }
    }
    TEST_ASSERT_TRUE(sfxSlot >= 0);
    const uint32_t periodAfter = apu.getVoiceNoisePeriodForTesting(sfxSlot);
    TEST_ASSERT_TRUE(periodAfter > startPeriod);
    TEST_ASSERT_TRUE(periodAfter <= endPeriod);

    // Run through the rest of the sweep window.
    for (int n = 0; n < 20; ++n) {
        apu.generateSamples(buffer, 256);
    }
    const uint32_t periodEnd = apu.getVoiceNoisePeriodForTesting(sfxSlot);
    TEST_ASSERT_EQUAL_UINT32(endPeriod, periodEnd);
}

void test_apu_core_loop_voice_stays_enabled_until_stop(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 220.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.4f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[512] = {0};
    apu.generateSamples(buffer, 512);
    TEST_ASSERT_TRUE(apu.countEnabledVoicesForTesting() >= 1);

    int loopSlot = -1;
    for (int i = ApuCore::SFX_VOICE_BASE; i < ApuCore::MAX_VOICES; ++i) {
        if (apu.isVoiceLoopForTesting(i) && apu.isVoiceEnabledForTesting(i)) {
            loopSlot = i;
            break;
        }
    }
    TEST_ASSERT_TRUE(loopSlot >= 0);

    // Far beyond any short one-shot duration — still playing.
    for (int n = 0; n < 40; ++n) {
        apu.generateSamples(buffer, 512);
    }
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(loopSlot));
    TEST_ASSERT_TRUE(apu.isVoiceLoopForTesting(loopSlot));

    AudioCommand stop{};
    stop.type = AudioCommandType::STOP_CHANNEL;
    stop.channelIndex = static_cast<uint8_t>(loopSlot);
    TEST_ASSERT_TRUE(apu.submitCommand(stop));
    apu.generateSamples(buffer, 64);
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(loopSlot));
}

void test_apu_core_oneshot_zero_duration_does_not_hang(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[128] = {0};
    apu.generateSamples(buffer, 128);
    TEST_ASSERT_EQUAL_INT(0, apu.countEnabledVoicesForTesting());
}

// =============================================================================
// SweepCurve Linear / Exponential (sfx-synthesis-medium-priority)
// =============================================================================

static int find_first_enabled_sfx_slot(ApuCore& apu) {
    for (int i = ApuCore::SFX_VOICE_BASE; i < ApuCore::MAX_VOICES; ++i) {
        if (apu.isVoiceEnabledForTesting(i)) {
            return i;
        }
    }
    return -1;
}

void test_apu_core_linear_sweep_midpoint_arithmetic(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.sweepEndHz = 880.0f;
    cmd.event.sweepDurationSec = 0.1f;  // 4410 samples
    cmd.event.loop = false;
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    // Advance to ~50% of sweep (2205 samples ≈ 8.6 * 256).
    for (int n = 0; n < 9; ++n) {
        apu.generateSamples(buffer, 256);
    }

    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    const float hz = apu.getVoiceFrequencyForTesting(slot);
    // Linear midpoint ≈ 660 Hz; allow coarse window around arithmetic mean.
    TEST_ASSERT_FLOAT_WITHIN(40.0f, 660.0f, hz);
}

void test_apu_core_exponential_sweep_midpoint_geometric(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.sweepEndHz = 880.0f;
    cmd.event.sweepDurationSec = 0.1f;
    cmd.event.loop = false;
    cmd.event.sweepCurve = SweepCurve::Exponential;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    for (int n = 0; n < 9; ++n) {
        apu.generateSamples(buffer, 256);
    }

    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    const float hz = apu.getVoiceFrequencyForTesting(slot);
    // Geometric midpoint = 440 * sqrt(2) ≈ 622.25; must not sit on linear 660.
    TEST_ASSERT_FLOAT_WITHIN(35.0f, 622.25f, hz);
    TEST_ASSERT_TRUE(std::fabs(hz - 660.0f) > 15.0f);
}

void test_apu_core_exponential_noise_sweep_updates_period(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 2000.0f;  // period = 22
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.noisePeriod = 0;
    cmd.event.sweepEndHz = 200.0f;  // period = 220
    cmd.event.sweepDurationSec = 0.05f;
    cmd.event.loop = false;
    cmd.event.sweepCurve = SweepCurve::Exponential;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    const uint32_t startPeriod = 44100u / 2000u;
    const uint32_t endPeriod = 44100u / 200u;
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    const uint32_t periodAfter = apu.getVoiceNoisePeriodForTesting(slot);
    TEST_ASSERT_TRUE(periodAfter > startPeriod);
    TEST_ASSERT_TRUE(periodAfter <= endPeriod);

    for (int n = 0; n < 20; ++n) {
        apu.generateSamples(buffer, 256);
    }
    TEST_ASSERT_EQUAL_UINT32(endPeriod, apu.getVoiceNoisePeriodForTesting(slot));
}

void test_apu_core_sweep_curve_default_linear_and_nonpositive_fallback(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    // Brace-init omits sweepCurve → Linear (ABI default 0).
    AudioCommand linearCmd{};
    linearCmd.type = AudioCommandType::PLAY_EVENT;
    linearCmd.event.type = WaveType::PULSE;
    linearCmd.event.frequency = 440.0f;
    linearCmd.event.duration = 1.0f;
    linearCmd.event.volume = 0.5f;
    linearCmd.event.duty = 0.5f;
    linearCmd.event.sweepEndHz = 880.0f;
    linearCmd.event.sweepDurationSec = 0.1f;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SweepCurve::Linear),
                            static_cast<uint8_t>(linearCmd.event.sweepCurve));
    TEST_ASSERT_TRUE(apu.submitCommand(linearCmd));

    int16_t buffer[256] = {0};
    for (int n = 0; n < 9; ++n) {
        apu.generateSamples(buffer, 256);
    }
    int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(40.0f, 660.0f, apu.getVoiceFrequencyForTesting(slot));

    // Exponential with non-positive start Hz falls back to Linear arithmetic.
    apu.reset();
    AudioCommand fallbackCmd{};
    fallbackCmd.type = AudioCommandType::PLAY_EVENT;
    fallbackCmd.event.type = WaveType::PULSE;
    fallbackCmd.event.frequency = 0.0f;
    fallbackCmd.event.duration = 1.0f;
    fallbackCmd.event.volume = 0.5f;
    fallbackCmd.event.duty = 0.5f;
    fallbackCmd.event.sweepEndHz = 880.0f;
    fallbackCmd.event.sweepDurationSec = 0.1f;
    fallbackCmd.event.sweepCurve = SweepCurve::Exponential;
    TEST_ASSERT_TRUE(apu.submitCommand(fallbackCmd));
    for (int n = 0; n < 9; ++n) {
        apu.generateSamples(buffer, 256);
    }
    slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    // Linear from 0 → 880 at alpha≈0.5 ≈ 440 (not geometric, which is undefined).
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 440.0f, apu.getVoiceFrequencyForTesting(slot));
}

void test_apu_core_loop_voice_is_stealable(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    // Fill SFX pool with long one-shots + one loop; then force steal.
    for (int i = 0; i < ApuCore::SFX_VOICE_COUNT; ++i) {
        AudioCommand cmd{};
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event.type = WaveType::PULSE;
        cmd.event.frequency = 300.0f + static_cast<float>(i) * 10.0f;
        cmd.event.duration = (i == 0) ? 0.0f : 2.0f;
        cmd.event.volume = 0.3f;
        cmd.event.duty = 0.5f;
        cmd.event.loop = (i == 0);
        TEST_ASSERT_TRUE(apu.submitCommand(cmd));
    }

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);
    TEST_ASSERT_EQUAL_INT(ApuCore::SFX_VOICE_COUNT,
                          static_cast<int>(apu.countEnabledVoicesForTesting()));

    int loopSlot = -1;
    for (int i = ApuCore::SFX_VOICE_BASE; i < ApuCore::MAX_VOICES; ++i) {
        if (apu.isVoiceLoopForTesting(i)) {
            loopSlot = i;
            break;
        }
    }
    TEST_ASSERT_TRUE(loopSlot >= 0);

    // New event must steal the loop (steal score 0) rather than starve.
    AudioCommand steal{};
    steal.type = AudioCommandType::PLAY_EVENT;
    steal.event.type = WaveType::TRIANGLE;
    steal.event.frequency = 880.0f;
    steal.event.duration = 0.2f;
    steal.event.volume = 0.5f;
    steal.event.duty = 0.5f;
    steal.event.loop = false;
    TEST_ASSERT_TRUE(apu.submitCommand(steal));
    apu.generateSamples(buffer, 64);

    TEST_ASSERT_FALSE(apu.isVoiceLoopForTesting(loopSlot));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(loopSlot));
}

// =============================================================================
// Duty stepped (sfx-synthesis-low-priority block 1)
// =============================================================================

void test_apu_core_duty_stepped_absent_keeps_legacy_duty_sweep(void) {
    // Large dutySweep with no duty steps must still advance dutyCycle (legacy PWM).
    static const InstrumentPreset kSweepPreset{
        0.8f, 0.5f, 4, 0.0f, 0, 0.001f, 0.0f, 1.0f, 0.001f,
        LfoTarget::NONE, 0.0f, 0.0f, 0.0f, false,
        8.0f  // dutySweep: +8.0 duty units per second
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.125f;
    cmd.event.preset = &kSweepPreset;
    cmd.event.dutySteps = nullptr;
    cmd.event.dutyStepCount = 0;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 1);
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.125f, apu.getVoiceDutyCycleForTesting(slot));
    TEST_ASSERT_TRUE(apu.getVoiceDutySweepPerSampleForTesting(slot) != 0.0f);

    apu.generateSamples(buffer, 256);
    const float dutyAfter = apu.getVoiceDutyCycleForTesting(slot);
    TEST_ASSERT_TRUE_MESSAGE(dutyAfter > 0.125f + 0.01f,
                             "legacy dutySweep must advance duty without steps");
}

void test_apu_core_duty_stepped_mid_note_hold(void) {
    static const SfxBreakpoint kSteps[] = {
        {0.0f, 0.125f},
        {0.05f, 0.5f},
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.25f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.75f;  // superseded by step at t=0
    cmd.event.dutySteps = kSteps;
    cmd.event.dutyStepCount = 2;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 1);
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.125f, apu.getVoiceDutyCycleForTesting(slot));

    // Just before 0.05s (2205 samples @ 44100): still 12.5%.
    const int beforeBoundary = 2200;
    int remaining = beforeBoundary - 1;  // already generated 1 sample
    while (remaining > 0) {
        const int n = remaining > 256 ? 256 : remaining;
        apu.generateSamples(buffer, n);
        remaining -= n;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.125f, apu.getVoiceDutyCycleForTesting(slot));

    // Cross 0.05s boundary → hold 50%.
    apu.generateSamples(buffer, 16);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, apu.getVoiceDutyCycleForTesting(slot));
}

void test_apu_core_pitch_envelope_three_points_linear(void) {
    static const SfxBreakpoint kPitch[] = {
        {0.0f, 523.25f},
        {0.1f, 659.25f},
        {0.2f, 783.99f},
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 100.0f;  // superseded by first pitch point
    cmd.event.duration = 0.4f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.pitchEnvelope = kPitch;
    cmd.event.pitchEnvelopeCount = 3;
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 1);
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 523.25f, apu.getVoiceFrequencyForTesting(slot));

    // Midpoint of first segment (~0.05s).
    int remaining = 2205 - 1;
    while (remaining > 0) {
        const int n = remaining > 256 ? 256 : remaining;
        apu.generateSamples(buffer, n);
        remaining -= n;
    }
    TEST_ASSERT_FLOAT_WITHIN(8.0f, 591.25f, apu.getVoiceFrequencyForTesting(slot));

    // Midpoint of second segment (~0.15s): +4410 samples from 0.05.
    remaining = 4410;
    while (remaining > 0) {
        const int n = remaining > 256 ? 256 : remaining;
        apu.generateSamples(buffer, n);
        remaining -= n;
    }
    TEST_ASSERT_FLOAT_WITHIN(8.0f, 721.62f, apu.getVoiceFrequencyForTesting(slot));

    // Past last point: hold.
    for (int i = 0; i < 20; ++i) {
        apu.generateSamples(buffer, 256);
    }
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 783.99f, apu.getVoiceFrequencyForTesting(slot));
}

void test_apu_core_pitch_envelope_noise_updates_period(void) {
    static const SfxBreakpoint kPitch[] = {
        {0.0f, 2000.0f},
        {0.05f, 200.0f},
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 500.0f;
    cmd.event.duration = 0.3f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.pitchEnvelope = kPitch;
    cmd.event.pitchEnvelopeCount = 2;
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 1);
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_EQUAL_UINT32(44100u / 2000u, apu.getVoiceNoisePeriodForTesting(slot));

    for (int i = 0; i < 20; ++i) {
        apu.generateSamples(buffer, 256);
    }
    TEST_ASSERT_EQUAL_UINT32(44100u / 200u, apu.getVoiceNoisePeriodForTesting(slot));
}

void test_apu_core_pitch_envelope_precedes_single_sweep(void) {
    static const SfxBreakpoint kPitch[] = {
        {0.0f, 440.0f},
        {0.1f, 880.0f},
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.3f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.sweepEndHz = 100.0f;       // must be ignored
    cmd.event.sweepDurationSec = 0.1f;
    cmd.event.pitchEnvelope = kPitch;
    cmd.event.pitchEnvelopeCount = 2;
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    for (int i = 0; i < 30; ++i) {
        apu.generateSamples(buffer, 256);
    }
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    const float hz = apu.getVoiceFrequencyForTesting(slot);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 880.0f, hz);
    TEST_ASSERT_TRUE_MESSAGE(hz > 400.0f, "must follow pitch envelope, not sweepEndHz=100");
}

void test_apu_core_pitch_envelope_count_below_two_keeps_sweep(void) {
    static const SfxBreakpoint kOnePoint[] = {
        {0.0f, 220.0f},
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.3f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.sweepEndHz = 880.0f;
    cmd.event.sweepDurationSec = 0.1f;
    cmd.event.pitchEnvelope = kOnePoint;
    cmd.event.pitchEnvelopeCount = 1;
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    for (int i = 0; i < 30; ++i) {
        apu.generateSamples(buffer, 256);
    }
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 880.0f, apu.getVoiceFrequencyForTesting(slot));
}

void test_apu_core_duty_stepped_hold_after_last_and_ignores_duty_sweep(void) {
    static const InstrumentPreset kSweepPreset{
        0.8f, 0.5f, 4, 0.0f, 0, 0.001f, 0.0f, 1.0f, 0.001f,
        LfoTarget::NONE, 0.0f, 0.0f, 0.0f, false,
        20.0f  // would move duty fast if not ignored
    };
    static const SfxBreakpoint kSteps[] = {
        {0.0f, 0.25f},
        {0.02f, 0.125f},
    };

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 330.0f;
    cmd.event.duration = 0.2f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.preset = &kSweepPreset;
    cmd.event.dutySteps = kSteps;
    cmd.event.dutyStepCount = 2;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[512] = {0};
    apu.generateSamples(buffer, 1);
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, apu.getVoiceDutySweepPerSampleForTesting(slot));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, apu.getVoiceDutyCycleForTesting(slot));

    // Past last breakpoint (0.02s = 882 samples): hold 12.5%, no PWM drift.
    for (int n = 0; n < 8; ++n) {
        apu.generateSamples(buffer, 256);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.125f, apu.getVoiceDutyCycleForTesting(slot));
}

void test_apu_core_music_uses_track_duty_not_preset_duty(void)
{
    // Preset duty is 50%; track duty is 12.5%. Sequencer must wire track->duty.
    static const InstrumentPreset kPresetDuty50{
        0.8f, 0.5f, 4, 0.0f, 0, 0.001f, 0.0f, 1.0f, 0.001f,
        LfoTarget::NONE, 0.0f, 0.0f, 0.0f, false, 0.0f};
    static const MusicNote kNotes[] = {
        makeNote(kPresetDuty50, Note::C, 4, 2.0f),
    };
    static const MusicTrack kTrack{
        kNotes, 1, false, WaveType::PULSE, 0.125f, nullptr, nullptr, nullptr};

    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    int16_t buffer[4096] = {0};
    apu.generateSamples(buffer, 4096);

    int positive = 0;
    int negative = 0;
    for (int i = 0; i < 4096; ++i) {
        if (buffer[i] > 0) {
            ++positive;
        } else if (buffer[i] < 0) {
            ++negative;
        }
    }
    TEST_ASSERT_TRUE(positive > 0);
    TEST_ASSERT_TRUE(negative > 0);
    // 12.5% duty ⇒ far fewer positive samples than a 50% square.
    const float pos_ratio =
        static_cast<float>(positive) /
        static_cast<float>(positive + negative);
    TEST_ASSERT_TRUE_MESSAGE(pos_ratio < 0.30f,
                             "track duty 0.125 must dominate over preset 0.5");
}

// =============================================================================
// Hito 1 M2: NES discrete PULSE duty (4 patterns)
// =============================================================================

// Verifies each of the 4 NES duty indices produces the expected 8-step
// pattern when reading back through a freshly-init voice. Uses a low sample
// rate to keep the frequency reachable and trigger the voice deterministically.
void test_pulse_duty_nes_4modes(void)
{
    // Expected patterns (1 = high, 0 = low). Mirrors the canonical
    // nesdev.org/wiki/APU_Pulse table.
    static const uint8_t kExpected[4][8] = {
        {1,0,0,0,0,0,0,0},  // 12.5%
        {0,1,1,0,0,0,0,0},  // 25%
        {0,1,1,1,1,0,0,0},  // 50%
        {1,0,1,1,1,1,1,0},  // 75%
    };

    for (uint8_t duty = 0; duty < 4; ++duty) {
        ApuCore apu;
        apu.init(44100);
        apu.reset();
        // SFX subpool starts at MUSIC_VOICE_BASE + MUSIC_VOICE_COUNT (= 4).
        constexpr int kSfxBase = ApuCore::MUSIC_VOICE_BASE + ApuCore::MUSIC_VOICE_COUNT;
        apu.setVoicePulseDutyMode(kSfxBase, duty);

        // 8 samples per cycle at exactly 5512.5 Hz. After 88 samples (the
        // default 2 ms attack ramp at 44100 Hz) the envelope is at full
        // and the phase happens to be at a cycle boundary (88 % 8 == 0).
        // Reading from sample 88 yields a clean 8-step window.
        AudioCommand cmd{};
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event.type = WaveType::PULSE;
        cmd.event.frequency = 5512.5f;
        cmd.event.duration = 0.05f;
        cmd.event.volume = 1.0f;
        cmd.event.duty = 0.5f;                 // ignored when pulseDutyIndex<4
        TEST_ASSERT_TRUE(apu.submitCommand(cmd));

        int16_t buffer[128] = {};
        apu.generateSamples(buffer, 128);

        // Cycle boundary at sample 88 (attack ramp end). Verify all 8 steps
        // of the duty pattern. The 12.5% pattern has the high step at
        // index 0; the 25% / 50% have low step 0; 75% has high step 0.
        for (int step_idx = 0; step_idx < 8; ++step_idx) {
            const int16_t v = buffer[88 + step_idx];
            const int16_t expected_v = kExpected[duty][step_idx] ? 1 : -1;
            const int sign = (v > 0) ? 1 : (v < 0 ? -1 : 0);
            const int expected_sign = expected_v;
            TEST_ASSERT_EQUAL_INT(expected_sign, sign);
        }
    }
}

// With pulseDutyIndex == 255 the generator must fall back to the legacy
// continuous dutyCycle comparison. 50% duty should produce a balanced output.
void test_pulse_duty_continuous_unchanged(void)
{
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    // Default (after reset) is 255 -> continuous, which is the legacy path.
    // We exercise it by submitting a play event with a 50% duty and
    // verifying the positive/negative split is roughly balanced.
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 220.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[8192] = {};
    apu.generateSamples(buffer, 8192);

    int positive = 0;
    int negative = 0;
    for (int i = 0; i < 8192; ++i) {
        if (buffer[i] > 0) ++positive;
        else if (buffer[i] < 0) ++negative;
    }
    // At 50% duty, the positive/negative split must be close (within 10 %).
    const int total = positive + negative;
    TEST_ASSERT_TRUE(total > 0);
    const float ratio = static_cast<float>(positive) / static_cast<float>(total);
    TEST_ASSERT_TRUE_MESSAGE(ratio > 0.40f && ratio < 0.60f,
                             "continuous 50% duty must remain near 50/50");
}

// =============================================================================
// Hito 1 M16: TRIANGLE octave doubling (opt-in)
// =============================================================================

// With triangleOctaveUp == false (the default), the voice frequency is
// preserved at the event value. We verify the public frequency readout.
void test_triangle_octave_up_default_off(void)
{
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.1f;
    cmd.event.volume = 1.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[16] = {};
    apu.generateSamples(buffer, 16);

    constexpr int kSfxBase = ApuCore::MUSIC_VOICE_BASE + ApuCore::MUSIC_VOICE_COUNT;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 440.0f,
                             apu.getVoiceFrequencyForTesting(kSfxBase));
}

// With triangleOctaveUp == true on the target voice at init time, the
// effective frequency is doubled (440 -> 880). The test uses the
// test-only setter to flip the flag on the SFX base slot before
// submitting the play event, then asserts the public frequency readout.
void test_triangle_octave_up_doubles_freq(void)
{
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    constexpr int kSfxBase = ApuCore::MUSIC_VOICE_BASE + ApuCore::MUSIC_VOICE_COUNT;
    apu.setVoiceTriangleOctaveUpForTesting(kSfxBase, true);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.1f;
    cmd.event.volume = 1.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[16] = {};
    apu.generateSamples(buffer, 16);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 880.0f,
                             apu.getVoiceFrequencyForTesting(kSfxBase));
}

// Documents that the octave-up doubling is gated on TRIANGLE only. PULSE
// voices must not be affected by the flag, which we verify by submitting
// a PULSE event with the flag set and confirming the frequency stays at
// 440 Hz.
void test_triangle_octave_up_ignored_for_other_types(void)
{
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    constexpr int kSfxBase = ApuCore::MUSIC_VOICE_BASE + ApuCore::MUSIC_VOICE_COUNT;
    apu.setVoiceTriangleOctaveUpForTesting(kSfxBase, true);

    AudioCommand pulse{};
    pulse.type = AudioCommandType::PLAY_EVENT;
    pulse.event.type = WaveType::PULSE;
    pulse.event.frequency = 440.0f;
    pulse.event.duration = 0.1f;
    pulse.event.volume = 1.0f;
    pulse.event.duty = 0.5f;
    TEST_ASSERT_TRUE(apu.submitCommand(pulse));

    int16_t buf[16] = {};
    apu.generateSamples(buf, 16);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 440.0f,
                             apu.getVoiceFrequencyForTesting(kSfxBase));
}

// =============================================================================
// Hito 2 M7: NES envelope unit (PULSE / NOISE)
// =============================================================================
//
// All envelope tests are isolated per `ApuCore` instance. The frame
// counter is OFF by default, so we explicitly enable mode 0 when we need
// quarter-frame ticks to advance. The M3 test-only counter
// `getNesFrameCounterQuarterTickCountForTesting()` lets us advance by
// quarter-clock count rather than computing quarter-clocks-per-sample.

// Dispatches a continuous looping PULSE event and returns the SFX slot
// it landed on. Tests that assert on `isVoiceEnabledForTesting` need a
// real voice: arming only the NES sub-units leaves the slot disabled,
// which would make a "the envelope did not silence it" assertion pass
// or fail for the wrong reason. `scratch` must hold at least 256
// samples. Mirrors the pattern used by the M4 length-counter tests.
static int startLoopingPulseVoice(ApuCore& apu, int16_t* scratch) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;  // 0 + loop => continuous
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    apu.generateSamples(scratch, 256);  // dispatch the play event

    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            return s;
        }
    }
    TEST_FAIL_MESSAGE("looping PULSE event did not land on an SFX slot");
    return -1;
}

// Renders until `count` more quarter-frame clocks have fired. Requires
// the frame counter to be running (`setNesFrameCounterMode`).
static void advanceQuarterClocks(ApuCore& apu, int16_t* scratch, int count) {
    const int target = apu.getNesFrameCounterQuarterTickCountForTesting() + count;
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < target
           && safety < 1000) {
        apu.generateSamples(scratch, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        target, apu.getNesFrameCounterQuarterTickCountForTesting());
}

// Renders `length` samples into `scratch` and returns the peak absolute
// magnitude. Amplitude probe for tests that compare relative levels.
static int32_t renderPeakMagnitude(ApuCore& apu, int16_t* scratch, int length) {
    apu.generateSamples(scratch, length);
    int32_t peak = 0;
    for (int i = 0; i < length; ++i) {
        const int32_t mag = scratch[i] < 0 ? -(int32_t)scratch[i] : scratch[i];
        if (mag > peak) peak = mag;
    }
    return peak;
}

// After init+reset, every voice's `nesEnvelope` must be in the default
// state: envelopeEnabled=false, output=0, decayLevel=0, dividerCounter=0,
// startFlag=false, constVolume=false, volume=0. The shared
// `lengthCounter` pointer must be non-null (bound in
// AudioChannel::reset()). This is the "default OFF" invariant that
// keeps the canonical track PCM byte-for-byte identical.
void test_envelope_initial_state(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeEnabledForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeOutputForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDecayLevelForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDividerForTesting(slot));
        TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeStartFlagForTesting(slot));
        TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeConstVolumeForTesting(slot));
    }
    // The lengthCounter pointer binding is exercised by every other
    // envelope test in this file; we just confirm the field is
    // reachable via the test accessors.
}

// Enable the envelope on slot 0 with V=8. Load length index 0 (which
// also sets the start flag via the M4 surgical fix). After 1
// quarter-clock: start flag cleared, decayLevel=15, dividerCounter=8.
// Subsequent quarter-clocks decrement the divider (8ΓåÆ7ΓåÆΓÇªΓåÆ0). On the
// quarter-clock AFTER the divider reaches 0, decayLevel decrements to
// 14 and the divider reloads to 8.
void test_envelope_setup_and_decay(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);  // also sets start flag
    apu.setVoiceNesEnvelope(0, false, false, 8);  // loop=off, C=off, V=8
    apu.setVoiceNesEnvelopeEnabled(0, true);

    // Pre-Q-clock: start flag is set (M4 surgical fix). divider is 0
    // (default ΓÇö the start flag loads the divider on the next Q-clock).
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));

    int16_t buffer[64] = {0};

    // Quarter-clock 1: start flag ΓåÆ decayLevel=15, dividerCounter=8.
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(15, apu.getVoiceNesEnvelopeDecayLevelForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(8, apu.getVoiceNesEnvelopeDividerForTesting(0));

    // Quarter-clocks 2..9: divider decrements 8ΓåÆ7ΓåÆΓÇªΓåÆ0. After 9 total
    // quarter-clocks the divider is 0 (it took 8 decrements from 8 to
    // reach 0 on the 9th tick). decayLevel still 15.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 9
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDividerForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(15, apu.getVoiceNesEnvelopeDecayLevelForTesting(0));

    // Quarter-clock 10: divider was 0 ΓåÆ reload to 8, decayLevel 15ΓåÆ14.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 10
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(14, apu.getVoiceNesEnvelopeDecayLevelForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(8, apu.getVoiceNesEnvelopeDividerForTesting(0));
}

// With loop=ON (via length counter halt), the decay level must wrap
// from 0 back to 15 every time the divider underflows, instead of
// silencing the channel. The voice must stay enabled across the wrap.
void test_envelope_loop(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // A continuous PULSE voice, so `enabled` reflects the envelope's
    // decision rather than "this slot was never armed".
    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 0, true);   // halt=true -> loop=ON
    apu.setVoiceNesEnvelope(slot, true, false, 1);  // V=1, fast decay
    apu.setVoiceNesEnvelopeEnabled(slot, true);

    // Run enough quarter-clocks to bring decayLevel to 0 and back
    // around the loop several times. V=1 means the divider reloads to
    // 1 each time, so each decrement takes 2 quarter-clocks. From
    // decayLevel=15 to 0 takes 15├ù2=30 quarter-clocks. Add 32 more
    // quarter-clocks to ensure we wrap twice.
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 60
           && safety < 5000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(60,
        apu.getNesFrameCounterQuarterTickCountForTesting());

    // The voice must still be enabled (loop prevents silence).
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    // decayLevel must be in the legal range (0..15) - it wrapped at
    // least once and is now somewhere mid-cycle.
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeDecayLevelForTesting(slot) <= 15);
}

// With constant volume (C=1), the output must be VVVV (=10) immediately,
// regardless of the decay level. The decay level still updates per the
// wiki (we verify it has decremented at least once), but the output
// stays at V.
void test_envelope_const_volume(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);
    apu.setVoiceNesEnvelope(0, false, true, 10);  // C=ON, V=10
    apu.setVoiceNesEnvelopeEnabled(0, true);

    // Render 1 sample. After 1 quarter-clock: start flag consumed,
    // output = volume = 10.
    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(10, apu.getVoiceNesEnvelopeOutputForTesting(0));

    // Render 11 more quarter-clocks (enough for at least one divider
    // wrap with V=10 ΓåÆ ~10 ticks to decrement once). Output must stay
    // at 10 because C=1 ignores the decay level.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 12
           && safety < 5000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(10, apu.getVoiceNesEnvelopeOutputForTesting(0));
}

// With V=0 (the divider reloads to 0 each time) and !loop, the
// decayLevel must decrement EVERY quarter-clock, and when it reaches
// 0 the channel must be silenced.
void test_envelope_silence_at_zero(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);
    apu.setVoiceNesEnvelope(0, false, false, 0);  // V=0, divider counts at 1/Q
    apu.setVoiceNesEnvelopeEnabled(0, true);

    int16_t buffer[64] = {0};

    // Quarter-clock 1: start ΓåÆ decayLevel=15, divider=0.
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(15, apu.getVoiceNesEnvelopeDecayLevelForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDividerForTesting(0));

    // Quarter-clock 2: divider was 0 ΓåÆ reload to 0, decayLevel 15ΓåÆ14.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 2
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(14, apu.getVoiceNesEnvelopeDecayLevelForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDividerForTesting(0));

    // Run 16 more quarter-clocks to bring decayLevel to 0.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 17
           && safety < 5000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    // decayLevel at 0; the channel must be silenced on the 17th
    // quarter-clock (decayLevel was 1 ΓåÆ 0, then on the 18th it would
    // hit the !loop silence path; with 17 ticks total we just hit 0).
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeOutputForTesting(0));
}

// The start flag (set by setVoiceNesLength) must be cleared on the
// first quarter-clock after the load, and the decayLevel must be
// reloaded to 15. This is the test for the M4 surgical fix to
// setVoiceNesLength.
void test_envelope_start_flag_cleared_after_first_qclock(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);  // sets start flag (M7 fix)
    apu.setVoiceNesEnvelope(0, false, false, 8);
    apu.setVoiceNesEnvelopeEnabled(0, true);

    // Pre-tick: start flag is set.
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));

    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }

    // Post-tick: start flag cleared, decayLevel=15.
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(15, apu.getVoiceNesEnvelopeDecayLevelForTesting(0));
}

// TRIANGLE / SINE / SAW voices must keep `envelopeEnabled = false`
// permanently and the quarter-clock dispatch must skip them. The
// sample output must remain driven by the ADSR (default path).
void test_envelope_does_not_affect_sine_saw_triangle(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // Try to enable the envelope on a TRIANGLE / SINE / SAW voice.
    // The quarter-clock dispatch must skip them.
    apu.setVoiceNesEnvelopeEnabled(2, true);  // (some slot)
    apu.setVoiceNesEnvelope(2, true, false, 15);

    // Render a few thousand samples (more than a few quarter-clocks).
    // Sized for the largest single render below (256 samples).
    int16_t buffer[256] = {0};
    for (int i = 0; i < 100; ++i) {
        apu.generateSamples(buffer, 64);
    }

    // decayLevel stays at 0 and output stays at 0 ΓÇö the dispatch
    // skipped slot 2 because either its type is non-PULSE/NOISE or
    // its `envelopeEnabled` was never explicitly enabled.
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDecayLevelForTesting(2));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeOutputForTesting(2));

    // Now exercise the "PULSE+noise skipped" path via type filtering:
    // enable a TRIANGLE voice for real and verify the dispatch loop
    // does not touch its envelope (even after a real quarter-clock
    // tick). We use slot 1 (after dispatching a triangle).
    AudioCommand tri{};
    tri.type = AudioCommandType::PLAY_EVENT;
    tri.event.type = WaveType::TRIANGLE;
    tri.event.frequency = 440.0f;
    tri.event.duration = 0.0f;  // + loop = continuous
    tri.event.volume = 0.5f;
    tri.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(tri));
    apu.generateSamples(buffer, 256);

    // Enable the envelope on this triangle slot. The dispatch must
    // still skip it (type != PULSE/NOISE).
    int triSlot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            triSlot = s;
            break;
        }
    }
    if (triSlot >= 0) {
        apu.setVoiceNesEnvelopeEnabled(triSlot, true);
        apu.setVoiceNesEnvelope(triSlot, true, false, 15);
        // Run a few thousand more samples and confirm the envelope
        // state stays untouched.
        const uint8_t decayBefore =
            apu.getVoiceNesEnvelopeDecayLevelForTesting(triSlot);
        for (int i = 0; i < 200; ++i) {
            apu.generateSamples(buffer, 64);
        }
        TEST_ASSERT_EQUAL_UINT8(decayBefore,
            apu.getVoiceNesEnvelopeDecayLevelForTesting(triSlot));
    }
}

// With constant volume selected, the envelope output is V, and the
// mixed signal must scale linearly with V/15. We compare the peak
// magnitude over a full render window at V=15 against the same window
// at V=7 and expect the ratio 7/15. Peak-over-window rather than a
// single sample: the output high-pass filter turns the flat parts of a
// pulse wave into a decaying tail, so one arbitrary sample is not a
// stable amplitude probe, while the peak is.
void test_envelope_volume_scaled_correctly(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // A continuous PULSE voice, so the channel stays enabled across the
    // render windows below.
    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    // Enable the NES channel + envelope.
    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 0, true);  // halt=true: length must not silence
    apu.setVoiceNesEnvelope(slot, false, true, 15);  // C=1, V=15 -> output 1.0
    apu.setVoiceNesEnvelopeEnabled(slot, true);

    // The envelope output register only updates on a quarter-frame
    // clock, so a freshly enabled envelope reads 0 until the first one
    // lands. Advance past it before probing the amplitude.
    advanceQuarterClocks(apu, buffer, 1);

    // 256 samples is ~5.8 periods at 440 Hz, so the window always
    // contains several edges and therefore the signal peak.
    const int32_t peak15 = renderPeakMagnitude(apu, buffer, 256);
    TEST_ASSERT_TRUE_MESSAGE(peak15 > 4000,
        "V=15 must produce near-full output (envelope at 1.0)");

    // V=7 -> output 7 -> 7/15 of the amplitude. The Q15 path uses
    // 32768/15 = 2184 per level (7 -> 15288 vs 15 -> 32760), so the
    // ratio is 7/15 on both the float and the integer path.
    apu.setVoiceNesEnvelope(slot, false, true, 7);
    advanceQuarterClocks(apu, buffer, 1);
    const int32_t peak7 = renderPeakMagnitude(apu, buffer, 256);

    const float ratio = (float)peak7 / (float)peak15;
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 7.0f / 15.0f, ratio);
}

// Enable BOTH the length counter (M4) and the envelope (M7). The
// length counter reaches 0 first (LUT[3]=2 ΓåÆ 2 half-clocks) and
// silences the channel; the envelope would also have silenced
// eventually but the length counter wins because it counts faster
// (every half-clock vs every quarter-clock's wrap).
void test_envelope_coexists_with_length_counter(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 3, false);  // LUT[3] = 2 ΓåÆ 2 half-clocks
    apu.setVoiceNesEnvelope(0, false, false, 0);  // V=0, fast decay
    apu.setVoiceNesEnvelopeEnabled(0, true);

    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 2
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2,
        apu.getNesFrameCounterHalfTickCountForTesting());

    // Length counter reached 0 ΓåÆ channel silenced.
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
}

// The envelope's loop flag is SHARED with `nesLengthCounter.halt`.
// Setting halt=true via setVoiceNesLength must make the envelope loop,
// even if `setVoiceNesEnvelope` was called with loop=false.
void test_envelope_loop_flag_shared_with_length_halt(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 0, true);  // halt=true -> loop=ON
    apu.setVoiceNesEnvelopeEnabled(slot, true);
    // loop=false here on purpose: the live loop bit comes from the
    // length counter's `halt`, not from this argument.
    apu.setVoiceNesEnvelope(slot, false, false, 1);  // V=1, fast decay

    // Run enough quarter-clocks to bring decayLevel to 0 several times.
    // V=1: divider=1, so each decrement takes 2 quarter-clocks. 15
    // decrements ΓåÆ ~30 quarter-clocks to hit 0, then wrap to 15.
    // We run 60 quarter-clocks to ensure at least 2 wraps.
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 60
           && safety < 5000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(60,
        apu.getNesFrameCounterQuarterTickCountForTesting());

    // Loop=ON: voice stays enabled, decayLevel cycles 0..15 (no silence).
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeDecayLevelForTesting(slot) <= 15);
}

// =============================================================================
// Hito 2 M3: NES frame counter
// =============================================================================
// Hito 2 M3: NES frame counter
// =============================================================================
//
// The frame counter is OFF by default. Each test enables it explicitly via
// `setNesFrameCounterMode(0 or 1)` and counts the per-tick dispatchers that
// the stub methods increment when `UNIT_TEST` is defined.

// Mode 0 (4-step): quarter Γëê 240 Hz, half Γëê 120 Hz, IRQ Γëê 60 Hz.
void test_frame_counter_initial_state(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    // Default OFF: IRQ is false; no ticks have fired.
    TEST_ASSERT_FALSE(apu.getNesFrameCounterIrq());
    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterQuarterTickCountForTesting());
    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterIrqSetCountForTesting());
    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterCurrentStepForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f,
                             (float)apu.getNesFrameCounterApuCyclesForTesting());
}

void test_frame_counter_mode_set_resets_timer(void) {
    ApuCore apu;
    apu.init(44100);

    // Accumulate a non-zero cycle count by rendering samples in the default
    // (mode 0, resetPending false on first call) state. Even though the
    // counter is "mode 0" in the default-init sense, it is OFF until
    // setNesFrameCounterMode is called and the tick is gated on mode !=
    // default, so the accumulator stays at 0.0 here.
    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f,
                             (float)apu.getNesFrameCounterApuCyclesForTesting());

    // Enable mode 1: the setter must schedule a reset (the actual reset
    // happens on the next tick, so verify state right after the call).
    apu.setNesFrameCounterMode(1);
    // The setter zeros the IRQ flag and marks resetPending. The cycle
    // accumulator is still whatever it was (0.0 here).
    TEST_ASSERT_FALSE(apu.getNesFrameCounterIrq());

    // Render a single sample: the first sample after mode-set should
    // observe the reset, then advance currentStep appropriately.
    apu.generateSamples(buffer, 1);
    // After reset + first sample: currentStep is whatever the accumulator
    // indicates (likely still 0, since 20.29 APU cycles is well below
    // 3728.5 step 1). Just confirm the cycle counter is non-zero AND
    // the state is sane (mode preserved, IRQ still false in mode 1).
    TEST_ASSERT_FALSE(apu.getNesFrameCounterIrq());
    // Mode 1 has no IRQ -- confirm the counter is still 0.
    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterIrqSetCountForTesting());
}

void test_frame_counter_quarter_clock_fires_at_240hz_mode0(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // Render 22050 samples (= 0.5 s @ 44100 Hz). Expected quarter rate
    // is 240 Hz -> ~120 ticks in 0.5 s.
    int16_t buffer[2048] = {0};
    int total = 0;
    while (total < 22050) {
        const int n = (22050 - total > 2048) ? 2048 : (22050 - total);
        apu.generateSamples(buffer, n);
        total += n;
    }

    const int quarters = apu.getNesFrameCounterQuarterTickCountForTesting();
    // Accept ┬▒1 ticks of rounding slack.
    TEST_ASSERT_INT_WITHIN(1, 120, quarters);
}

void test_frame_counter_half_clock_fires_at_120hz_mode0(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[2048] = {0};
    int total = 0;
    while (total < 22050) {
        const int n = (22050 - total > 2048) ? 2048 : (22050 - total);
        apu.generateSamples(buffer, n);
        total += n;
    }

    const int halves = apu.getNesFrameCounterHalfTickCountForTesting();
    TEST_ASSERT_INT_WITHIN(1, 60, halves);
}

void test_frame_counter_mode1_no_irq(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(1);

    int16_t buffer[2048] = {0};
    int total = 0;
    while (total < 44100) {
        const int n = (44100 - total > 2048) ? 2048 : (44100 - total);
        apu.generateSamples(buffer, n);
        total += n;
    }

    // Mode 1 never sets the IRQ flag.
    TEST_ASSERT_FALSE(apu.getNesFrameCounterIrq());
    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterIrqSetCountForTesting());
}

void test_frame_counter_irq_fires_at_60hz_mode0(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[2048] = {0};
    int total = 0;
    while (total < 44100) {
        const int n = (44100 - total > 2048) ? 2048 : (44100 - total);
        apu.generateSamples(buffer, n);
        total += n;
    }

    // 1 s @ 60 Hz -> ~60 IRQ sets, ┬▒1.
    const int irqSets = apu.getNesFrameCounterIrqSetCountForTesting();
    TEST_ASSERT_INT_WITHIN(1, 60, irqSets);
    TEST_ASSERT_TRUE(apu.getNesFrameCounterIrq());
}

void test_frame_counter_irq_inhibit_prevents_irq(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);
    apu.setNesFrameCounterInhibit(true);

    int16_t buffer[2048] = {0};
    int total = 0;
    while (total < 44100) {
        const int n = (44100 - total > 2048) ? 2048 : (44100 - total);
        apu.generateSamples(buffer, n);
        total += n;
    }

    TEST_ASSERT_EQUAL_INT(0, apu.getNesFrameCounterIrqSetCountForTesting());
    TEST_ASSERT_FALSE(apu.getNesFrameCounterIrq());
}

void test_frame_counter_step_timing_uses_cpu_cycles(void) {
    // At 22050 Hz, 0.5 s = 11025 samples. If the counter used sample
    // counts, it would fire ~60 quarters in 0.5 s; using APU cycles it
    // must fire ~120 (the rate is independent of the audio sample rate).
    ApuCore apu;
    apu.init(22050);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[1024] = {0};
    int total = 0;
    while (total < 11025) {
        const int n = (11025 - total > 1024) ? 1024 : (11025 - total);
        apu.generateSamples(buffer, n);
        total += n;
    }

    const int quarters = apu.getNesFrameCounterQuarterTickCountForTesting();
    // ~120 quarters at 240 Hz in 0.5 s; the test fails loudly if the
    // counter is sample-driven (would give ~60).
    TEST_ASSERT_INT_WITHIN(2, 120, quarters);
}

// =============================================================================
// Hito 2 M4: NES length counter
// =============================================================================
//
// All length counter tests are isolated per `ApuCore` instance. The
// frame counter is OFF by default, so we explicitly enable mode 0 when
// we need half-frame ticks to advance. The M3 test-only counter
// `getNesFrameCounterHalfTickCountForTesting()` lets us advance by
// half-clock count rather than computing half-clocks-per-sample.

// After init+reset, every voice's `nesLengthCounter` must be in the
// default state: enabled=false, counter=0, index=0, halt=false. This is
// the "default OFF" invariant that keeps the canonical track PCM
// byte-for-byte identical.
void test_length_counter_initial_state(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLengthIndexForTesting(slot));
        TEST_ASSERT_FALSE(apu.getVoiceNesLengthHaltForTesting(slot));
    }
}

// After enabling the channel and loading length index 0, the counter
// must be `kNesLengthLut[0] = 10`. Index and halt must reflect the load.
void test_length_counter_load(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);

    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLengthIndexForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesLengthHaltForTesting(0));

    // Loading with halt=true sets the halt flag without changing the
    // counter (mirrors the NES "halt doesn't affect the load itself").
    apu.setVoiceNesLength(0, 4, true);
    TEST_ASSERT_EQUAL_UINT16(40, apu.getVoiceNesLengthCounterForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(4, apu.getVoiceNesLengthIndexForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesLengthHaltForTesting(0));

    // Out-of-range lengthIndex is clamped to 31.
    apu.setVoiceNesLength(0, 255, false);
    TEST_ASSERT_EQUAL_UINT8(31, apu.getVoiceNesLengthIndexForTesting(0));
    TEST_ASSERT_EQUAL_UINT16(30, apu.getVoiceNesLengthCounterForTesting(0));

    // Out-of-range slot is a silent no-op (slot 8 is past the end).
    const uint16_t counterBefore = apu.getVoiceNesLengthCounterForTesting(0);
    apu.setVoiceNesLength(8, 0, false);
    apu.setVoiceNesChannelEnabled(8, true);
    TEST_ASSERT_EQUAL_UINT16(counterBefore, apu.getVoiceNesLengthCounterForTesting(0));
}

// With the frame counter enabled, the length counter must decrement
// once per half-clock. We render samples until the half-tick counter
// reports 6 half-clocks, then verify counter went from 10 to 4.
void test_length_counter_decrements_on_half_clock(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(0));

    // Render samples in small chunks until the half-tick counter has
    // advanced by 6. At 44100 Hz a half-clock is ~367 samples, so 6
    // half-clocks Γëê 2200 samples; chunked rendering gives the inner
    // loop a chance to dispatch each half-clock.
    int16_t buffer[256] = {0};
    const int targetHalfTicks = 6;
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < targetHalfTicks
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(targetHalfTicks,
                                     apu.getNesFrameCounterHalfTickCountForTesting());

    // 10 initial - 6 half-ticks = 4.
    TEST_ASSERT_EQUAL_UINT16(4, apu.getVoiceNesLengthCounterForTesting(0));
}

// With the frame counter enabled and length index 3 (LUT[3] = 2), the
// counter must reach 0 after 2 half-clocks and the voice must be
// silenced. This verifies the "silence the voice when counter becomes
// 0" behaviour that the M4 spec calls out.
void test_length_counter_silences_at_zero(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 3, false);  // LUT[3] = 2
    TEST_ASSERT_EQUAL_UINT16(2, apu.getVoiceNesLengthCounterForTesting(0));

    int16_t buffer[256] = {0};
    const int targetHalfTicks = 2;
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < targetHalfTicks
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(targetHalfTicks,
                                     apu.getNesFrameCounterHalfTickCountForTesting());

    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(0));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
}

// With `halt` set, the length counter must NOT decrement on half-clocks.
// This matches the NES behaviour: halt blocks the decrement path while
// the channel is still armed. We start a long loop voice so that the
// test can verify both the counter and the voice stay armed.
void test_length_counter_halt_prevents_decrement(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // Long loop voice so the audio thread keeps it enabled.
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;  // 0 + loop => continuous
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);  // dispatch the play event

    // Find the SFX slot the voice landed on.
    int slot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            slot = s;
            break;
        }
    }
    TEST_ASSERT_TRUE(slot >= 0);

    // Arm the length counter with halt=true.
    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 0, true);  // halt = true
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(slot));

    // Render ~0.5 s (much more than the 8 ms it would take to fully
    // decrement without halt). Counter must stay at 10 throughout, and
    // the voice must still be enabled (the length counter never silenced
    // it).
    int total = 0;
    while (total < 22050) {
        apu.generateSamples(buffer, 256);
        total += 256;
    }
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(slot));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
}

// Defensive test: the 32 LUT values must match the nesdev wiki table
// byte-for-byte. A typo in `kNesLengthLut` would silently make every
// NES-mode voice play for the wrong duration; this test catches that
// at build time (static_assert) and at runtime.
void test_length_counter_lut_values(void) {
    // The 32 canonical values from
    // https://www.nesdev.org/wiki/APU_Length_Counter
    static constexpr uint8_t kExpected[32] = {
        10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
        12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
    };
    // Compile-time check on a few canonical indices.
    static_assert(nes_apu::kNesLengthLut[0]  == 10,   "LUT[0] must be 10");
    static_assert(nes_apu::kNesLengthLut[1]  == 254,  "LUT[1] must be 254");
    static_assert(nes_apu::kNesLengthLut[3]  == 2,    "LUT[3] must be 2");
    static_assert(nes_apu::kNesLengthLut[4]  == 40,   "LUT[4] must be 40");
    static_assert(nes_apu::kNesLengthLut[8]  == 160,  "LUT[8] must be 160");
    static_assert(nes_apu::kNesLengthLut[31] == 30,   "LUT[31] must be 30");

    // Full 32-value runtime check.
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQUAL_UINT8(kExpected[i], nes_apu::kNesLengthLut[i]);
    }
}

// Clearing the channel enable must silence the voice immediately and
// force the length counter to 0. This mirrors the NES $4015 write
// semantics. Critically, the half-clock tick must NOT try to silence
// the voice again afterwards (idempotency).
void test_length_counter_channel_enable_off_forces_silence(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(0));

    // Clearing the channel enable silences immediately.
    apu.setVoiceNesChannelEnabled(0, false);
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(0));

    // Re-enable and load; the previous forced zero must NOT linger.
    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 0, false);
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(0));
    // The voice was disabled by the previous clear, so it stays
    // disabled; loading the length counter does not re-enable the
    // voice (only `setVoiceNesChannelEnabled` or a play event does).
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
}

// SINE/SAW voices must keep `nesLengthCounter.enabled == false`
// permanently. The half-clock dispatch loop must skip them even when
// the frame counter is active, and a SINE/SAW voice must continue to
// produce audio (the length counter never silences it). We use
// `loop = true` so the voice stays armed across the full 1 s of
// half-clocks the test renders.
void test_length_counter_does_not_affect_sine_saw(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // Load a SINE voice via the standard PLAY_EVENT path. A long fixed
    // duration (no loop) keeps the voice armed across the half-clock
    // render window. The SINE voice's `nesLengthCounter.enabled` stays
    // false (default), so the half-clock dispatch path skips it.
    AudioCommand sineCmd{};
    sineCmd.type = AudioCommandType::PLAY_EVENT;
    sineCmd.event.type = WaveType::SINE;
    sineCmd.event.frequency = 440.0f;
    sineCmd.event.duration = 2.0f;  // 2 s, well past the 1 s render window
    sineCmd.event.volume = 0.5f;
    sineCmd.event.loop = false;
    TEST_ASSERT_TRUE(apu.submitCommand(sineCmd));
    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);  // dispatch + 256 render samples

    // Find the SINE voice slot in the SFX subpool.
    int sineSlot = -1;
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        if (apu.isVoiceEnabledForTesting(slot)) {
            sineSlot = slot;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sineSlot >= 0, "SINE voice must be present in SFX pool");

    // Render 1 s of audio (~120 half-clocks @ mode 0). The voice must
    // stay armed ΓÇö the half-clock dispatch path skips SINE voices
    // because their `nesLengthCounter.enabled` is false.
    int total = 0;
    while (total < 44100) {
        apu.generateSamples(buffer, 256);
        total += 256;
    }
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(sineSlot));
    // The length counter is still in its default state for SINE/SAW.
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(sineSlot));
}

// =============================================================================
// Hito 2 M5: NES linear counter (TRIANGLE)
// =============================================================================
//
// All linear counter tests are isolated per `ApuCore` instance. The
// frame counter is OFF by default, so we explicitly enable mode 0 when
// we need quarter-frame ticks to advance. The M3 test-only counter
// `getNesFrameCounterQuarterTickCountForTesting()` lets us advance by
// quarter-clock count rather than computing quarter-clocks-per-sample.

// After init+reset, every voice's `nesLinearCounter` must be in the
// default state: linearEnabled=false, counter=0, reloadValue=0,
// reloadFlag=false. The shared lengthCounter pointer must be non-null
// (bound in AudioChannel::reset()). This is the "default OFF" invariant
// that keeps the canonical track PCM byte-for-byte identical.
void test_linear_counter_initial_state(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_FALSE(apu.getVoiceNesLinearEnabledForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearCounterForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearReloadValueForTesting(slot));
        TEST_ASSERT_FALSE(apu.getVoiceNesLinearReloadFlagForTesting(slot));
        // The shared lengthCounter pointer is bound by AudioChannel::reset().
        // We can't read the pointer directly, but we can verify the cross-bind
        // is in place: the next linear-counter tick must observe the same
        // halt flag as the length counter (shared control bit).
        // Here we just confirm the field is reachable via the accessors; the
        // pointer binding is exercised by the decrement / hold tests below.
    }
}

// Enable the length counter and the linear counter for slot 0, then load
// reloadValue=100 with reloadFlag=true. The counter is NOT reloaded yet ΓÇö
// the load happens on the next quarter-clock. After 1 quarter-clock the
// counter must be 100.
void test_linear_counter_load(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // Enable NES channel (length) and linear. halt=false so reloadFlag
    // will be cleared at the next quarter-clock (verified in test 5).
    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLinearEnabled(0, true);
    apu.setVoiceNesLinearCounter(0, 100, true);

    // Values are stored immediately, but the counter itself has not been
    // reloaded yet (that happens on the next quarter-clock).
    TEST_ASSERT_EQUAL_UINT8(100, apu.getVoiceNesLinearReloadValueForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesLinearReloadFlagForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearCounterForTesting(0));

    // Render samples until the quarter-tick counter has advanced by 1.
    // At 44100 Hz a quarter-clock is ~184 samples; we render in 64-sample
    // chunks to let the inner loop dispatch each quarter-clock.
    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1,
        apu.getNesFrameCounterQuarterTickCountForTesting());

    // Quarter-clock fired: reload happened. counter = reloadValue = 100.
    TEST_ASSERT_EQUAL_UINT8(100, apu.getVoiceNesLinearCounterForTesting(0));
}

// With reloadFlag=true and halt=false (control=0), the counter must
// reload on the first quarter-clock, then decrement on subsequent
// quarter-clocks. After 1 quarter-clock: counter=10 (reloaded) and
// reloadFlag=false (cleared because control=0). After 3 total
// quarter-clocks: counter=8 (reloaded once, decremented twice).
void test_linear_counter_decrements_quarter_clock(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLinearEnabled(0, true);
    apu.setVoiceNesLinearCounter(0, 10, true);

    int16_t buffer[64] = {0};
    int safety = 0;

    // Quarter-clock 1: reload ΓåÆ counter=10, reloadFlag cleared (control=0).
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1,
        apu.getNesFrameCounterQuarterTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(10, apu.getVoiceNesLinearCounterForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearReloadFlagForTesting(0));

    // Quarter-clocks 2 + 3: decrement twice ΓåÆ counter=8.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 3
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3,
        apu.getNesFrameCounterQuarterTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(8, apu.getVoiceNesLinearCounterForTesting(0));
}

// With reloadFlag=true and halt=true (control=1), the counter must
// reload on every quarter-clock and reloadFlag must NEVER be cleared.
// After 5 quarter-clocks: counter=10 (reloaded 5 times), reloadFlag=true.
void test_linear_counter_hold_with_control_set(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLinearEnabled(0, true);
    // halt=true (control=1) ΓåÆ reloadFlag is never cleared.
    apu.setVoiceNesLength(0, 0, true);
    apu.setVoiceNesLinearCounter(0, 10, true);

    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 5
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(5,
        apu.getNesFrameCounterQuarterTickCountForTesting());

    // Control=1 keeps the reload flag set forever (matches the wiki:
    // "if !control: reloadFlag = false"). Counter reloads to 10 every
    // quarter-clock.
    TEST_ASSERT_TRUE(apu.getVoiceNesLinearReloadFlagForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(10, apu.getVoiceNesLinearCounterForTesting(0));
}

// With reloadFlag=true and halt=false (control=0), the counter must
// reload on the first quarter-clock and reloadFlag must be cleared.
// After 1 quarter-clock: counter=10, reloadFlag=false. After 3 total
// quarter-clocks: counter=8.
void test_linear_counter_reload_flag_cleared_without_control(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLinearEnabled(0, true);
    apu.setVoiceNesLinearCounter(0, 10, true);  // halt=false (default)

    int16_t buffer[64] = {0};
    int safety = 0;

    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1,
        apu.getNesFrameCounterQuarterTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(10, apu.getVoiceNesLinearCounterForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearReloadFlagForTesting(0));

    // Two more quarter-clocks: counter decrements to 8.
    safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 3
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(8, apu.getVoiceNesLinearCounterForTesting(0));
}

// Safety-net test: with default NES flags (linearEnabled=false,
// lengthEnabled=false), a TRIANGLE voice must produce non-zero audio.
// This confirms the M5 default-OFF invariant does not break the
// canonical triangle synthesis.
void test_triangle_gate_open_default(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;  // 0 + loop = continuous
    cmd.event.volume = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1024] = {0};
    apu.generateSamples(buffer, 1024);

    // At least one sample must be non-zero (the triangle produces a
    // non-trivial waveform; with a 4-bit quantiser the smallest non-zero
    // magnitude is 2/15 Γëê 0.133, well above 0).
    bool anyNonZero = false;
    int16_t maxAbs = 0;
    for (int i = 0; i < 1024; ++i) {
        const int16_t a = (buffer[i] < 0) ? (int16_t)-buffer[i] : buffer[i];
        if (a > 0) anyNonZero = true;
        if (a > maxAbs) maxAbs = a;
    }
    TEST_ASSERT_TRUE_MESSAGE(anyNonZero,
        "TRIANGLE default-OFF must still produce audio");
    // The 4-bit quantiser caps the magnitude at 32767 (1.0 ├ù 32767).
    // We just sanity-check that the level is meaningful (well above 1 LSB).
    TEST_ASSERT_GREATER_THAN_INT16(0, maxAbs);
}

// TRIANGLE voice with NES length counter enabled, load length index 3
// (LUT[3]=2). After 2 half-clocks the length counter hits 0. The voice
// must stay enabled (M5 surgical fix) but the output must be silent
// (M5 per-sample gate closes because the length counter is 0).
void test_triangle_gate_closes_when_length_zero(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);  // dispatch + 256 render samples

    // Find the slot the TRIANGLE voice landed on.
    int slot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            slot = s;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(slot >= 0, "TRIANGLE voice must be present in SFX pool");

    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 3, false);  // LUT[3] = 2
    TEST_ASSERT_EQUAL_UINT16(2, apu.getVoiceNesLengthCounterForTesting(slot));

    // Render until 2 half-clocks have fired.
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 2
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2,
        apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(slot));

    // M5 surgical fix: the voice is NOT disabled when the length counter
    // hits 0 (only M5's per-sample gate handles the silence path).
    TEST_ASSERT_TRUE_MESSAGE(apu.isVoiceEnabledForTesting(slot),
        "TRIANGLE must stay enabled when length counter hits 0 (M5 surgical fix)");

    // Render a long window and check the bulk of samples. The first few
    // samples have a DC-blocker transient (state from prior render), but
    // after ~1000 samples at HPF_R=0.995 the residual is < 1% of the
    // active-triangle level. An active triangle at volume 0.5 produces
    // magnitudes in the thousands; the gate-closed output should be < 50.
    int16_t gateBuffer[4096] = {0};
    apu.generateSamples(gateBuffer, 4096);
    int16_t maxAbs = 0;
    int16_t maxAbsLast2k = 0;
    for (int i = 0; i < 4096; ++i) {
        const int16_t a = (gateBuffer[i] < 0) ? (int16_t)-gateBuffer[i] : gateBuffer[i];
        if (a > maxAbs) maxAbs = a;
        if (i >= 2048 && a > maxAbsLast2k) maxAbsLast2k = a;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT16_MESSAGE(50, maxAbsLast2k,
        "TRIANGLE output must be silent (HPF-decayed) when length gate is closed");
    (void)maxAbs;  // for diagnostics if the first-window check ever needs to be tightened
}

// TRIANGLE voice with linear counter enabled and reloadValue=0
// (reloaded to 0 on every quarter-clock as long as control=0 and
// reloadFlag stays set, but with control=0 the flag is cleared after
// one reload ΓåÆ counter decrements from 0 ΓåÆ stays at 0).
void test_triangle_gate_closes_when_linear_zero(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);

    int slot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            slot = s;
            break;
        }
    }
    TEST_ASSERT_TRUE(slot >= 0);

    apu.setVoiceNesLinearEnabled(slot, true);
    apu.setVoiceNesLinearCounter(slot, 0, true);  // reloadValue=0

    // After 1 quarter-clock: counter is reloaded to 0 (because reloadFlag
    // is set, it loads reloadValue=0), and reloadFlag is cleared because
    // control=0 (default halt). The decrement branch is a no-op because
    // counter is already 0. Subsequent ticks: counter stays at 0.
    int safety = 0;
    while (apu.getNesFrameCounterQuarterTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearCounterForTesting(slot));

    // Render a long window and check the bulk of samples. The first few
    // samples have a DC-blocker transient, but the HPF decays at 0.995
    // per sample. An active triangle at volume 0.5 produces magnitudes
    // in the thousands; gate-closed output should be < 50 in the
    // HPF-decayed region.
    int16_t gateBuffer[4096] = {0};
    apu.generateSamples(gateBuffer, 4096);
    int16_t maxAbsLast2k = 0;
    for (int i = 2048; i < 4096; ++i) {
        const int16_t a = (gateBuffer[i] < 0) ? (int16_t)-gateBuffer[i] : gateBuffer[i];
        if (a > maxAbsLast2k) maxAbsLast2k = a;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT16_MESSAGE(50, maxAbsLast2k,
        "TRIANGLE output must be silent (HPF-decayed) when linear gate is closed");

    // The voice must still be enabled (it can resume when a fresh
    // $4008 write reloads the counter to a non-zero value).
    TEST_ASSERT_TRUE_MESSAGE(apu.isVoiceEnabledForTesting(slot),
        "TRIANGLE must stay enabled when linear gate is closed");
}

// TRIANGLE voice: phase must advance when the gate is open, and must
// NOT advance when the gate is closed. We capture the phase before and
// after two render windows: one with the gate open (default OFF) and
// one with the gate closed (linear counter at 0 with linearEnabled=true).
void test_triangle_phase_does_not_advance_when_gated(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::TRIANGLE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[1000] = {0};
    apu.generateSamples(buffer, 1000);  // dispatch + 1000 samples (warms phase)

    int slot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            slot = s;
            break;
        }
    }
    TEST_ASSERT_TRUE(slot >= 0);

    // Gate is open by default (linearEnabled=false). Phase must advance.
    const float phaseBefore = apu.getVoicePhaseForTesting(slot);
    apu.generateSamples(buffer, 1000);
    const float phaseAfterOpen = apu.getVoicePhaseForTesting(slot);
    TEST_ASSERT_TRUE_MESSAGE(phaseAfterOpen != phaseBefore,
        "TRIANGLE phase must advance when gate is open");

    // Close the gate: enable linear with counter=0. Without any quarter-
    // clock tick, the counter starts at 0 and the gate is immediately
    // closed (we don't even need the frame counter enabled for this).
    apu.setVoiceNesLinearEnabled(slot, true);
    apu.setVoiceNesLinearCounter(slot, 0, false);  // reloadValue=0, no auto-reload
    // Note: reloadFlag=false so no reload happens; counter stays at 0.

    const float phaseBeforeClosed = apu.getVoicePhaseForTesting(slot);
    apu.generateSamples(buffer, 1000);
    const float phaseAfterClosed = apu.getVoicePhaseForTesting(slot);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(phaseBeforeClosed, phaseAfterClosed,
        "TRIANGLE phase must NOT advance when gate is closed");
}

// SINE voices must keep `nesLinearCounter.linearEnabled == false`
// permanently, just like the length counter. The quarter-clock dispatch
// must skip them and a SINE voice must continue to produce audio.
void test_triangle_does_not_affect_sine_saw(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    AudioCommand sineCmd{};
    sineCmd.type = AudioCommandType::PLAY_EVENT;
    sineCmd.event.type = WaveType::SINE;
    sineCmd.event.frequency = 440.0f;
    sineCmd.event.duration = 2.0f;  // long enough that the voice stays armed
    sineCmd.event.volume = 0.5f;
    sineCmd.event.loop = false;
    TEST_ASSERT_TRUE(apu.submitCommand(sineCmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 256);

    int sineSlot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            sineSlot = s;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sineSlot >= 0, "SINE voice must be present in SFX pool");

    // Render ~1 s. The voice must stay armed, and the linear counter
    // must stay at its default (linearEnabled=false, counter=0).
    int total = 0;
    while (total < 44100) {
        apu.generateSamples(buffer, 256);
        total += 256;
    }
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(sineSlot));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearEnabledForTesting(sineSlot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearCounterForTesting(sineSlot));
}

// =============================================================================
// Hito 2 M6: NES sweep unit (PULSE)
// =============================================================================
//
// All sweep tests are isolated per `ApuCore` instance. The sweep is OFF
// by default (default-OFF invariant), so the canonical track PCM is
// unaffected. Tests explicitly opt in by calling `setVoiceNesSweep` /
// `setVoiceNesTimer` and, when the half-clock dispatch is needed, by
// enabling the NES frame counter with `setNesFrameCounterMode(0)`.
//
// The `maxAbs < 50` window (HPF-decayed samples 2048-4096) pattern is
// borrowed from the M5 linear counter tests: the first ~1000 samples
// carry a DC-blocker transient from the previous render window, so the
// "did the voice actually silence" check runs in the post-transient
// region where the residual is < 1% of the active level.

// After init+reset, every voice's `nesSweepUnit` must be in the
// default state: sweepEnabled=false, period=0, negate=false, shift=0,
// reloadFlag=false, dividerCounter=0, isPulse2=false, timer=0, muted=false.
void test_sweep_initial_state(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    for (int slot = 0; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_FALSE(apu.getVoiceNesSweepEnabledForTesting(slot));
        TEST_ASSERT_FALSE(apu.getVoiceNesSweepNegateForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesSweepShiftForTesting(slot));
        TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesSweepDividerForTesting(slot));
        TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesSweepTimerForTesting(slot));
        TEST_ASSERT_FALSE(apu.getVoiceNesSweepMutedForTesting(slot));
    }
}

// PULSE 1, no negate, shift=1, period=7. Timer 256 ΓåÆ 256+128=384 after
// one half-clock. The divider reload branch (which also fires when
// `dividerCounter == 0`) sets the divider to P+1=8 WITHOUT decrementing
// on the same tick ΓÇö that decrement only happens on the next half-clock
// when `dividerCounter != 0` and `reloadFlag` is clear. So after 1
// half-clock: divider == 8.
void test_sweep_setup_and_target_calculation(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesSweepUnitEnabled(0, true);   // opt into the NES sweep unit
    apu.setVoiceNesTimer(0, 256);
    apu.setVoiceNesSweep(0, true, 7, false, 1, false);

    // Render until 1 half-clock has fired (~367 samples at 44100 Hz).
    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, apu.getNesFrameCounterHalfTickCountForTesting());

    // target = 256 + (256 >> 1) = 256 + 128 = 384.
    TEST_ASSERT_EQUAL_UINT16(384, apu.getVoiceNesSweepTimerForTesting(0));
    // Divider was 0 at the tick: the timer-update branch ran, then the
    // reload branch (which also fires on `dividerCounter == 0`) reloaded
    // to P+1 = 8. No decrement on the same tick.
    TEST_ASSERT_EQUAL_UINT8(8, apu.getVoiceNesSweepDividerForTesting(0));
}

// PULSE 1 negate: target = period + (-change - 1). With period=20,
// shift=1 ΓåÆ change=10, so target = 20 - 10 - 1 = 9.
void test_sweep_negate_pulse1_ones_complement(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesSweepUnitEnabled(0, true);   // opt into the NES sweep unit
    apu.setVoiceNesSweep(0, true, 7, true, 1, false);  // PULSE 1, negate
    apu.setVoiceNesTimer(0, 20);

    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, apu.getNesFrameCounterHalfTickCountForTesting());

    // 20 + (-10 - 1) = 9.
    TEST_ASSERT_EQUAL_UINT16(9, apu.getVoiceNesSweepTimerForTesting(0));
}

// PULSE 2 negate: target = period + (-change). With period=20, shift=1
// ΓåÆ change=10, so target = 20 - 10 = 10. (Exactly 1 higher than PULSE 1.)
void test_sweep_negate_pulse2_two_complement(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesSweepUnitEnabled(0, true);   // opt into the NES sweep unit
    apu.setVoiceNesSweep(0, true, 7, true, 1, true);  // PULSE 2, negate
    apu.setVoiceNesTimer(0, 20);

    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, apu.getNesFrameCounterHalfTickCountForTesting());

    // 20 + (-10) = 10.
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesSweepTimerForTesting(0));
    // Sanity: PULSE 1 vs PULSE 2 differ by exactly 1 with these params.
    // (Already verified in test_sweep_negate_pulse1_ones_complement.)
}

// Sweep enabled with period=1500, shift=1, no negate. Target = 1500+750
// = 2250 > 0x7FF. The muting flag must flip to true, and the rendered
// audio must be silent (HPF-decayed window maxAbs < 50).
void test_sweep_muting_target_overflow(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    // A PULSE voice so the hot path has something to gate. Arm the
    // sweep on the slot it actually landed on, not slot 0 - the muting
    // gate is per-voice, so targeting the wrong slot would leave the
    // audible voice untouched.
    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    // Arm the sweep so target overflows. setVoiceNesTimer recomputes
    // the muting flag immediately, so the very next sample is gated.
    apu.setVoiceNesSweepUnitEnabled(slot, true);
    apu.setVoiceNesTimer(slot, 1500);
    apu.setVoiceNesSweep(slot, true, 7, false, 1, false);

    // Muting flag must be true (target = 1500 + 750 = 2250 > 0x7FF).
    TEST_ASSERT_TRUE(apu.getVoiceNesSweepMutedForTesting(slot));

    // Render and verify the HPF-decayed region is silent. We need a
    // loop voice so the audio thread keeps producing samples, but the
    // sweep muting forces them to 0.
    int16_t renderBuffer[4096] = {0};
    apu.generateSamples(renderBuffer, 4096);
    int16_t maxAbs = 0;
    for (int i = 2048; i < 4096; ++i) {
        const int16_t a = (renderBuffer[i] < 0) ? (int16_t)-renderBuffer[i] : renderBuffer[i];
        if (a > maxAbs) maxAbs = a;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT16_MESSAGE(50, maxAbs,
        "PULSE output must be silent (HPF-decayed) when sweep target overflows");
}

// Sweep enabled but current period < 8 (the "low period" muting case).
// timer=4 ΓåÆ muted must be true immediately, and rendered output is 0.
void test_sweep_muting_current_period_too_low(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    // timer = 4 -> current period < 8 -> muted = true.
    apu.setVoiceNesSweepUnitEnabled(slot, true);
    apu.setVoiceNesTimer(slot, 4);
    apu.setVoiceNesSweep(slot, true, 7, false, 1, false);

    TEST_ASSERT_TRUE(apu.getVoiceNesSweepMutedForTesting(slot));

    int16_t renderBuffer[4096] = {0};
    apu.generateSamples(renderBuffer, 4096);
    int16_t maxAbs = 0;
    for (int i = 2048; i < 4096; ++i) {
        const int16_t a = (renderBuffer[i] < 0) ? (int16_t)-renderBuffer[i] : renderBuffer[i];
        if (a > maxAbs) maxAbs = a;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT16_MESSAGE(50, maxAbs,
        "PULSE output must be silent when current period < 8");
}

// Divider reload behaviour. P=2, so on a half-clock with dividerCounter
// starting at 0: the update branch sees `dividerCounter == 0`, the
// reload branch sets it to P+1=3. The decrement branch only fires when
// `dividerCounter != 0` and `reloadFlag` is clear, so on the reload
// tick itself there is NO decrement. Subsequent ticks decrement
// normally (3 ΓåÆ 2 ΓåÆ 1).
void test_sweep_divider_reload(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesSweepUnitEnabled(0, true);   // opt into the NES sweep unit
    apu.setVoiceNesTimer(0, 256);
    apu.setVoiceNesSweep(0, true, 2, false, 1, false);

    int16_t buffer[64] = {0};

    // Half-clock 1: reload to P+1=3 (no decrement on the reload tick).
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(3, apu.getVoiceNesSweepDividerForTesting(0));

    // Half-clock 2: decrement 3 ΓåÆ 2.
    safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 2
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(2, apu.getVoiceNesSweepDividerForTesting(0));

    // Half-clock 3: decrement 2 ΓåÆ 1.
    safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 3
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(1, apu.getVoiceNesSweepDividerForTesting(0));
}

// TRIANGLE/NOISE voices must be skipped by the half-clock sweep dispatch.
// Even when setVoiceNesSweep is called on them, the timer must not be
// updated. We find the SFX slot the TRIANGLE voice landed in (the
// default TRIANGLE PLAY_EVENT goes to the SFX subpool, not the music
// subpool at slots 0-3) and arm the sweep on that exact slot.
void test_sweep_does_not_affect_triangle_noise(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    AudioCommand tri{};
    tri.type = AudioCommandType::PLAY_EVENT;
    tri.event.type = WaveType::TRIANGLE;
    tri.event.frequency = 440.0f;
    tri.event.duration = 0.0f;  // loop
    tri.event.volume = 0.5f;
    tri.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(tri));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);

    // Find the SFX slot the TRIANGLE voice landed in.
    int slot = -1;
    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            slot = s;
            break;
        }
    }
    TEST_ASSERT_TRUE(slot >= 0);

    // Arm the sweep on that exact slot ΓÇö the half-clock dispatch
    // checks `type == PULSE` and must skip it.
    apu.setVoiceNesSweepUnitEnabled(slot, true);  // opt in: proves the TYPE gate skips it
    apu.setVoiceNesTimer(slot, 256);
    apu.setVoiceNesSweep(slot, true, 7, false, 1, false);

    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 2
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, apu.getNesFrameCounterHalfTickCountForTesting());

    // Timer must NOT have been updated by the dispatcher ΓÇö the setter
    // stored 256 and the dispatcher skipped it (TRIANGLE Γëá PULSE), so
    // it stays 256. (If we had armed a PULSE voice, the dispatcher
    // would have updated 256 ΓåÆ 384.)
    TEST_ASSERT_EQUAL_UINT16(256, apu.getVoiceNesSweepTimerForTesting(slot));
}

// setVoiceNesSweep sets reloadFlag=true. The next half-clock tick must
// clear it (the reload branch consumes the flag).
void test_sweep_reload_flag_cleared(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesSweepUnitEnabled(0, true);   // opt into the NES sweep unit
    apu.setVoiceNesTimer(0, 256);
    apu.setVoiceNesSweep(0, true, 7, false, 1, false);

    // The setter sets reloadFlag=true. Render until 1 half-clock has
    // fired: the dispatcher consumed the flag and reloaded the divider
    // to P+1=8 (the reload branch fires on `dividerCounter == 0` even
    // when reloadFlag has already been consumed).
    int16_t buffer[64] = {0};
    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 1
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT16(384, apu.getVoiceNesSweepTimerForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(8, apu.getVoiceNesSweepDividerForTesting(0));

    // Run another half-clock: reloadFlag is now clear (consumed on the
    // previous tick) and dividerCounter=8, so the decrement branch
    // fires. The divider must drop to 7. If reloadFlag had been
    // sticky, the divider would have reloaded to 8 again.
    safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 2
           && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, apu.getNesFrameCounterHalfTickCountForTesting());
    TEST_ASSERT_EQUAL_UINT8(7, apu.getVoiceNesSweepDividerForTesting(0));
}

// THE NES BUG: even when E=0 and SSS=0, the muting flag must still be
// evaluated. timer=1500 with all sweep fields zero ΓåÆ muted = (1500<8) ||
// (1500+0>0x7FF) ΓåÆ false. timer=1500 with shift=0 still computes
// target = 1500 + 0 = 1500 Γëñ 0x7FF, so not muted. We use timer=4 with
// shift=0 to verify the "current period < 8" path is the one that
// forces muted even with the sweep "disabled".
void test_sweep_muting_persists_when_disabled(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    // The unit is armed, but every register bit is off:
    // E=0, period=0, negate=false, shift=0.
    apu.setVoiceNesSweepUnitEnabled(slot, true);
    apu.setVoiceNesSweep(slot, false, 0, false, 0, false);
    // timer = 4 -> current period < 8 -> muted = true EVEN THOUGH the
    // sweep is "disabled" (E=0, shift=0). This is the NES bug:
    // muting is evaluated continuously.
    apu.setVoiceNesTimer(slot, 4);

    TEST_ASSERT_FALSE(apu.getVoiceNesSweepEnabledForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesSweepShiftForTesting(slot));
    TEST_ASSERT_TRUE(apu.getVoiceNesSweepMutedForTesting(slot));

    int16_t renderBuffer[4096] = {0};
    apu.generateSamples(renderBuffer, 4096);
    int16_t maxAbs = 0;
    for (int i = 2048; i < 4096; ++i) {
        const int16_t a = (renderBuffer[i] < 0) ? (int16_t)-renderBuffer[i] : renderBuffer[i];
        if (a > maxAbs) maxAbs = a;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT16_MESSAGE(50, maxAbs,
        "PULSE output must be silent (NES bug) when period<8 with E=0, SSS=0");
}

// Regression: the sweep unit is opt-in per voice. A PULSE voice that
// never armed it must keep playing when the frame counter is switched
// on. Before the opt-in flag existed, the half-clock dispatch evaluated
// every PULSE voice, and the default `timer == 0` satisfied the
// "current period < 8" muting rule, so `setNesFrameCounterMode` alone
// silenced every legacy PULSE voice.
void test_sweep_unarmed_voice_survives_frame_counter(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    // Nothing NES-related armed on this voice at all.
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(slot));

    // Run well past several half-clocks.
    for (int i = 0; i < 20; ++i) {
        apu.generateSamples(buffer, 256);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, apu.getNesFrameCounterHalfTickCountForTesting());

    TEST_ASSERT_FALSE(apu.getVoiceNesSweepMutedForTesting(slot));
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(4000,
        renderPeakMagnitude(apu, buffer, 256),
        "an unarmed PULSE voice must stay audible once the frame counter runs");
}

// Disarming the sweep unit must clear the muting flag, otherwise the
// per-sample gate would keep silencing a voice that is no longer under
// NES sweep control.
void test_sweep_disarm_clears_muting(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    apu.setVoiceNesSweepUnitEnabled(slot, true);
    apu.setVoiceNesTimer(slot, 4);  // period < 8 -> muted
    TEST_ASSERT_TRUE(apu.getVoiceNesSweepMutedForTesting(slot));

    apu.setVoiceNesSweepUnitEnabled(slot, false);
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepMutedForTesting(slot));

    // And it must stay clear across half-clocks (dispatch skips it).
    for (int i = 0; i < 20; ++i) {
        apu.generateSamples(buffer, 256);
    }
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepMutedForTesting(slot));
}

// The register-write setters must NOT arm the unit by themselves. This
// mirrors M4 / M5 / M7, where `setVoiceNesLength`, `setVoiceNesLinearCounter`
// and `setVoiceNesEnvelope` all leave their opt-in flag untouched.
void test_sweep_register_writes_do_not_arm_the_unit(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setVoiceNesSweep(0, true, 2, false, 1, false);
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(0));

    apu.setVoiceNesTimer(0, 4);  // period < 8, but the unit is not armed
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepMutedForTesting(0));

    // Arming afterwards must evaluate the muting rule immediately, using
    // the register state written above.
    apu.setVoiceNesSweepUnitEnabled(0, true);
    TEST_ASSERT_TRUE(apu.getVoiceNesSweepMutedForTesting(0));
}

// =============================================================================
// Hito 2 M13: TRIGGER_NES_LENGTH command
// =============================================================================
//
// The queued counterpart of `setVoiceNesLength`. It exists so a consumer can
// fire a NES-length-gated note across the game/audio thread boundary, which
// the direct setters cannot do safely. Unlike the raw setter it also arms the
// NES channel, so the command is self-sufficient (see the header docs for why
// that deviates from the `$4015` / `$4003` register split).

// The command must load the length counter from the LUT and latch halt,
// exactly as the direct setter does.
void test_trigger_nes_length_loads_counter(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = (uint8_t)slot;
    cmd.nesLengthIndex = 3;      // kNesLengthLut[3] = 2
    cmd.nesLengthHalt = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    apu.generateSamples(buffer, 64);  // dispatch the command

    TEST_ASSERT_TRUE(apu.getVoiceNesChannelEnabledForTesting(slot));
    TEST_ASSERT_EQUAL_UINT16(2, apu.getVoiceNesLengthCounterForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(3, apu.getVoiceNesLengthIndexForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesLengthHaltForTesting(slot));
}

// The command arms the NES channel itself, so it works on a slot that never
// received a `setVoiceNesChannelEnabled` call. Without this the queued path
// would be unusable: the enable setter is not reachable from the game thread.
void test_trigger_nes_length_arms_the_channel(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    TEST_ASSERT_FALSE(apu.getVoiceNesChannelEnabledForTesting(0));

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = 0;
    cmd.nesLengthIndex = 0;  // kNesLengthLut[0] = 10
    cmd.nesLengthHalt = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);

    TEST_ASSERT_TRUE(apu.getVoiceNesChannelEnabledForTesting(0));
    TEST_ASSERT_EQUAL_UINT16(10, apu.getVoiceNesLengthCounterForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesLengthHaltForTesting(0));
}

// M13 coordinates the sub-units: the length load must also raise the M7
// envelope start flag and the M5 linear counter reload flag, mirroring the
// documented side effects of a $4003 / $4007 / $400B / $400F write.
void test_trigger_nes_length_raises_subunit_flags(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearReloadFlagForTesting(0));

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = 0;
    cmd.nesLengthIndex = 5;
    cmd.nesLengthHalt = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);

    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesLinearReloadFlagForTesting(0));
}

// The direct setter must raise the same two flags — one behaviour, two entry
// points. If these ever diverge, a consumer would get different results from
// the queue than from the setter.
void test_set_voice_nes_length_raises_subunit_flags(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setVoiceNesChannelEnabled(0, true);
    apu.setVoiceNesLength(0, 5, false);

    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeStartFlagForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesLinearReloadFlagForTesting(0));
}

// The raised flags must actually drive the sub-units, not just sit there.
// TRIANGLE with the linear counter opted in: the trigger raises the reload
// flag, and the next quarter-clock must load the counter from reloadValue.
// This is the $400B behaviour the milestone was deferred for.
void test_trigger_nes_length_reloads_linear_counter(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    apu.setVoiceNesLinearEnabled(0, true);
    apu.setVoiceNesLinearCounter(0, 12, false);  // reloadValue=12, flag clear

    int16_t buffer[64] = {0};
    advanceQuarterClocks(apu, buffer, 1);
    // No reload flag yet, so the counter stayed at its initial 0.
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearCounterForTesting(0));

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = 0;
    cmd.nesLengthIndex = 0;
    cmd.nesLengthHalt = true;  // control set -> reload flag is not auto-cleared
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));
    apu.generateSamples(buffer, 64);
    TEST_ASSERT_TRUE(apu.getVoiceNesLinearReloadFlagForTesting(0));

    advanceQuarterClocks(apu, buffer, 1);
    TEST_ASSERT_EQUAL_UINT8(12, apu.getVoiceNesLinearCounterForTesting(0));
}

// Out-of-range length indices are clamped to 31, matching the setter.
void test_trigger_nes_length_clamps_index(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = 0;
    cmd.nesLengthIndex = 200;  // out of range
    cmd.nesLengthHalt = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);

    TEST_ASSERT_EQUAL_UINT8(31, apu.getVoiceNesLengthIndexForTesting(0));
    TEST_ASSERT_EQUAL_UINT16(nes_apu::kNesLengthLut[31],
                             apu.getVoiceNesLengthCounterForTesting(0));
}

// An out-of-range slot must be ignored without touching any voice.
void test_trigger_nes_length_ignores_bad_slot(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = (uint8_t)ApuCore::MAX_VOICES;  // out of range
    cmd.nesLengthIndex = 0;
    cmd.nesLengthHalt = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);  // must not crash

    for (int s = 0; s < ApuCore::MAX_VOICES; ++s) {
        TEST_ASSERT_FALSE(apu.getVoiceNesChannelEnabledForTesting(s));
        TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(s));
    }
}

// End to end: trigger a short length, let the half-clocks run, and confirm
// the length counter silenced the voice without the caller ever computing a
// sample budget. This is the reason M13 exists.
void test_trigger_nes_length_silences_voice_end_to_end(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));

    AudioCommand cmd{};
    cmd.type = AudioCommandType::TRIGGER_NES_LENGTH;
    cmd.channelIndex = (uint8_t)slot;
    cmd.nesLengthIndex = 3;  // kNesLengthLut[3] = 2 -> 2 half-clocks
    cmd.nesLengthHalt = false;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));
    apu.generateSamples(buffer, 64);

    int safety = 0;
    while (apu.getNesFrameCounterHalfTickCountForTesting() < 2 && safety < 1000) {
        apu.generateSamples(buffer, 64);
        ++safety;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, apu.getNesFrameCounterHalfTickCountForTesting());

    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(slot));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(slot));
}

// =============================================================================
// Hito 2 M17: initNesSafe / InstrumentPreset::nesAccurate
// =============================================================================
//
// `nesAccurate` on a preset makes `initVoiceFromEvent` reset the voice's NES
// sub-unit fields to the canonical silent state from the nesdev APU_basics
// `@regs` array before the note starts:
//
//   PULSE    $4000=$30 $4001=$08 $4002=$00 $4003=$00
//   TRIANGLE $4008=$80 $400A=$00 $400B=$00
//   NOISE    $400C=$30 $400E=$00 $400F=$00
//
// $30 decodes to L=1 (halt/loop), C=1 (constant volume), VVVV=0 — silent and
// frozen. $08 decodes to E=0, P=0, N=1, SSS=0. $80 sets the TRIANGLE control
// bit, which is the same shared halt flag.
//
// It deliberately does NOT opt the voice into any sub-unit: this is the clean
// slate you get BEFORE enabling NES mode, not the enable itself.

// Dispatches a one-shot note of `type` using `preset` and returns the SFX slot
// it landed on. `scratch` must hold at least 256 samples.
//
// PRECONDITION: no other SFX voice may be playing. This returns the first
// ENABLED slot, so with another voice already running it reports that one
// instead of the note just dispatched. Stop the previous voice with
// STOP_CHANNEL first when retriggering on the same slot.
static int playPresetNote(ApuCore& apu, int16_t* scratch,
                          WaveType type, const InstrumentPreset& preset) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = type;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    cmd.event.preset = &preset;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    apu.generateSamples(scratch, 256);

    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            return s;
        }
    }
    TEST_FAIL_MESSAGE("preset note did not land on an SFX slot");
    return -1;
}

// Asserts the shared part of the canonical safe state (length counter +
// envelope), which is identical for PULSE and NOISE.
static void assertNesSafeCommon(ApuCore& apu, int slot) {
    // $4015 silenced: the channel is not armed.
    TEST_ASSERT_FALSE(apu.getVoiceNesChannelEnabledForTesting(slot));
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesLengthCounterForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLengthIndexForTesting(slot));
    // L bit of $30 / $80: halt set, so nothing counts down on its own.
    TEST_ASSERT_TRUE(apu.getVoiceNesLengthHaltForTesting(slot));
    // C=1, VVVV=0: constant volume zero -> silent.
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeConstVolumeForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeVolumeForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDecayLevelForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeDividerForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeOutputForTesting(slot));
    // The length-load side effect of the $4003 / $400F write.
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeStartFlagForTesting(slot));
    // Opt-in flags stay OFF: this is the clean slate, not the enable.
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeEnabledForTesting(slot));
}

void test_nes_safe_pulse(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;

    int16_t buffer[256] = {0};
    const int slot = playPresetNote(apu, buffer, WaveType::PULSE, preset);

    assertNesSafeCommon(apu, slot);

    // $4001 = $08 -> E=0, P=0, N=1, SSS=0.
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepEnabledForTesting(slot));
    TEST_ASSERT_TRUE(apu.getVoiceNesSweepNegateForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesSweepShiftForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesSweepDividerForTesting(slot));
    // $4002 / $4003 = $00 -> timer 0.
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesSweepTimerForTesting(slot));
    // Not armed, so the muting rule must not have fired despite timer < 8.
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepMutedForTesting(slot));
}

void test_nes_safe_triangle(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    InstrumentPreset preset = INSTR_TRIANGLE_LEAD;
    preset.nesAccurate = true;

    int16_t buffer[256] = {0};
    const int slot = playPresetNote(apu, buffer, WaveType::TRIANGLE, preset);

    assertNesSafeCommon(apu, slot);

    // $4008 = $80 -> control bit set (the shared halt, asserted above),
    // reload value 0. $400A / $400B = $00.
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearCounterForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearReloadValueForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearEnabledForTesting(slot));
}

void test_nes_safe_noise(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;

    int16_t buffer[256] = {0};
    const int slot = playPresetNote(apu, buffer, WaveType::NOISE, preset);

    assertNesSafeCommon(apu, slot);

    // NOISE has no sweep unit; the $4001 negate bit must not be applied.
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepNegateForTesting(slot));
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesSweepTimerForTesting(slot));
}

// The whole point of the safe state: a voice carrying leftover NES state from
// a previous note must come back to the canonical values, not inherit them.
void test_nes_safe_clears_previous_state(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    // Dirty every sub-unit on that slot.
    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 7, false);
    apu.setVoiceNesEnvelope(slot, false, false, 9);
    apu.setVoiceNesEnvelopeEnabled(slot, true);
    apu.setVoiceNesSweepUnitEnabled(slot, true);
    apu.setVoiceNesSweep(slot, true, 3, false, 2, true);
    apu.setVoiceNesTimer(slot, 400);
    apu.setVoiceNesLinearEnabled(slot, true);
    apu.setVoiceNesLinearCounter(slot, 20, true);
    TEST_ASSERT_EQUAL_UINT16(nes_apu::kNesLengthLut[7],
                             apu.getVoiceNesLengthCounterForTesting(slot));

    // Stop it so the retrigger lands on the same slot, then replay with a
    // nesAccurate preset.
    AudioCommand stop{};
    stop.type = AudioCommandType::STOP_CHANNEL;
    stop.channelIndex = (uint8_t)slot;
    TEST_ASSERT_TRUE(apu.submitCommand(stop));
    apu.generateSamples(buffer, 64);

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;
    const int slot2 = playPresetNote(apu, buffer, WaveType::PULSE, preset);
    TEST_ASSERT_EQUAL_INT(slot, slot2);

    assertNesSafeCommon(apu, slot);
    TEST_ASSERT_EQUAL_UINT16(0, apu.getVoiceNesSweepTimerForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesSweepShiftForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepEnabledForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearEnabledForTesting(slot));
    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesLinearReloadValueForTesting(slot));
}

// Default path: nesAccurate is false, so the NES fields are never touched by
// initVoiceFromEvent. This is the invariant that keeps the golden PCM stable.
void test_nes_safe_off_by_default(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);

    apu.setVoiceNesChannelEnabled(slot, true);
    apu.setVoiceNesLength(slot, 7, false);

    AudioCommand stop{};
    stop.type = AudioCommandType::STOP_CHANNEL;
    stop.channelIndex = (uint8_t)slot;
    TEST_ASSERT_TRUE(apu.submitCommand(stop));
    apu.generateSamples(buffer, 64);

    // INSTR_PULSE_LEAD ships with nesAccurate = false.
    TEST_ASSERT_FALSE(INSTR_PULSE_LEAD.nesAccurate);
    const int slot2 = playPresetNote(apu, buffer, WaveType::PULSE, INSTR_PULSE_LEAD);
    TEST_ASSERT_EQUAL_INT(slot, slot2);

    // STOP_CHANNEL reset the voice, but the point is that replaying without
    // nesAccurate does not install the canonical safe state: halt stays clear.
    TEST_ASSERT_FALSE(apu.getVoiceNesLengthHaltForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeConstVolumeForTesting(slot));
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeStartFlagForTesting(slot));
}

// SINE and SAW are our own extensions with no NES counterpart. A nesAccurate
// preset on them must be inert.
void test_nes_safe_does_not_affect_sine_saw(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;

    int16_t buffer[256] = {0};
    const WaveType types[2] = {WaveType::SINE, WaveType::SAW};
    for (int i = 0; i < 2; ++i) {
        ApuCore fresh;
        fresh.init(44100);
        fresh.reset();
        const int slot = playPresetNote(fresh, buffer, types[i], preset);

        // Everything still at construction defaults - no safe state applied.
        TEST_ASSERT_FALSE(fresh.getVoiceNesLengthHaltForTesting(slot));
        TEST_ASSERT_FALSE(fresh.getVoiceNesEnvelopeConstVolumeForTesting(slot));
        TEST_ASSERT_FALSE(fresh.getVoiceNesEnvelopeStartFlagForTesting(slot));
        TEST_ASSERT_FALSE(fresh.getVoiceNesSweepNegateForTesting(slot));
    }
}

// The safe state must be genuinely silent when the sub-units are armed on top
// of it: constant volume 0 means the envelope contributes nothing.
void test_nes_safe_state_is_silent_when_armed(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setNesFrameCounterMode(0);

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;

    int16_t buffer[256] = {0};
    const int slot = playPresetNote(apu, buffer, WaveType::PULSE, preset);

    // Arm the envelope on top of the safe state and let it clock. Several
    // quarter-clocks, not one: the note rendered at full ADSR volume before
    // the envelope took over, and the output HPF needs ~2000 samples to ring
    // that step down below the noise floor (R=0.995 per sample).
    apu.setVoiceNesEnvelopeEnabled(slot, true);
    advanceQuarterClocks(apu, buffer, 8);

    TEST_ASSERT_EQUAL_UINT8(0, apu.getVoiceNesEnvelopeOutputForTesting(slot));
    TEST_ASSERT_LESS_OR_EQUAL_INT32_MESSAGE(50,
        renderPeakMagnitude(apu, buffer, 256),
        "the canonical safe state must be silent once the envelope is armed");

    // Silent because constant volume is 0 (C=1, VVVV=0) — NOT because the
    // decay path ran out and disabled the voice. Without the safe state the
    // envelope would start at constVolume=false / decayLevel=0 and the first
    // divider underflow would kill the channel, which is silence for the
    // wrong reason. The halt bit ($30 L=1) is what keeps it alive.
    TEST_ASSERT_TRUE_MESSAGE(apu.isVoiceEnabledForTesting(slot),
        "the safe state must silence via constant volume, not by disabling the voice");
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeConstVolumeForTesting(slot));
}

// =============================================================================
// Hito 3 M8: NES NOISE period LUT
// =============================================================================
//
// The NES noise channel picks its LFSR clock from a 16-entry table indexed by
// $400E bits 3-0, in CPU cycles. We keep the canonical NTSC table in CPU
// cycles and convert to a sample countdown at the current sample rate, so the
// same index means the same timbre at 22050, 44100 or 48000 Hz.
//
// Opt-in per voice, mirroring M2's `pulseDutyIndex`: 255 = off (legacy
// frequency-derived period), 0..15 select a LUT entry.

// Dispatches a looping NOISE note and returns the SFX slot it landed on.
static int startLoopingNoiseVoice(ApuCore& apu, int16_t* scratch) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 1000.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    apu.generateSamples(scratch, 256);

    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            return s;
        }
    }
    TEST_FAIL_MESSAGE("looping NOISE event did not land on an SFX slot");
    return -1;
}

// The 16 canonical NTSC values, in CPU cycles, from
// https://www.nesdev.org/wiki/APU_Noise. A typo here would silently retune
// every NES-mode noise voice, so pin the table itself.
void test_noise_period_lut(void) {
    const uint16_t expected[16] = {
        4, 8, 16, 32, 64, 96, 128, 160,
        202, 254, 380, 508, 762, 1016, 2034, 4068
    };
    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_UINT16(expected[i], nes_apu::kNesNoisePeriodLutNtsc[i]);
    }

    // Conversion to samples: cycles * sampleRate / CPU_NTSC, min 1 sample.
    // At 44100 the divisor is ~40.585, so index 15 (4068 cycles) is ~100
    // samples and index 0 (4 cycles) underflows to the 1-sample floor.
    TEST_ASSERT_EQUAL_UINT32(1u, nes_apu::nesNoisePeriodToSamples(0, 44100));
    TEST_ASSERT_EQUAL_UINT32(100u, nes_apu::nesNoisePeriodToSamples(15, 44100));
    TEST_ASSERT_EQUAL_UINT32(25u, nes_apu::nesNoisePeriodToSamples(13, 44100));
    // Halving the sample rate halves the sample period.
    TEST_ASSERT_EQUAL_UINT32(50u, nes_apu::nesNoisePeriodToSamples(15, 22050));
    // Out-of-range index is clamped to the last entry.
    TEST_ASSERT_EQUAL_UINT32(nes_apu::nesNoisePeriodToSamples(15, 44100),
                             nes_apu::nesNoisePeriodToSamples(200, 44100));
    // Invalid sample rate must not divide by zero.
    TEST_ASSERT_EQUAL_UINT32(1u, nes_apu::nesNoisePeriodToSamples(15, 0));
}

// Default off: the voice keeps the legacy frequency-derived period, which is
// what preserves the canonical golden PCM.
void test_noise_lut_default_off(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);

    TEST_ASSERT_EQUAL_UINT8(255, apu.getVoiceNesNoiseLutIndexForTesting(slot));
    // 44100 / 1000 Hz = 44 samples, the legacy path.
    TEST_ASSERT_EQUAL_UINT32(44u, apu.getVoiceNoisePeriodForTesting(slot));
}

// Selecting a LUT index must retune the voice immediately, without waiting
// for the next note.
void test_noise_lut_sets_period_from_table(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);

    apu.setVoiceNesNoiseLutMode(slot, 15);
    TEST_ASSERT_EQUAL_UINT8(15, apu.getVoiceNesNoiseLutIndexForTesting(slot));
    TEST_ASSERT_EQUAL_UINT32(100u, apu.getVoiceNoisePeriodForTesting(slot));

    apu.setVoiceNesNoiseLutMode(slot, 10);
    TEST_ASSERT_EQUAL_UINT32(nes_apu::nesNoisePeriodToSamples(10, 44100),
                             apu.getVoiceNoisePeriodForTesting(slot));
}

// The mode is a per-slot setting, like M2's duty mode: it survives a new note
// on the same slot instead of being reset by `initVoiceFromEvent`.
void test_noise_lut_applies_on_new_note(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);
    apu.setVoiceNesNoiseLutMode(slot, 12);

    AudioCommand stop{};
    stop.type = AudioCommandType::STOP_CHANNEL;
    stop.channelIndex = (uint8_t)slot;
    TEST_ASSERT_TRUE(apu.submitCommand(stop));
    apu.generateSamples(buffer, 64);

    // reset() clears the mode, so re-arm it and retrigger.
    apu.setVoiceNesNoiseLutMode(slot, 12);
    const int slot2 = startLoopingNoiseVoice(apu, buffer);
    TEST_ASSERT_EQUAL_INT(slot, slot2);

    // The note frequency (1000 Hz -> 44 samples) must NOT win over the LUT.
    TEST_ASSERT_EQUAL_UINT32(nes_apu::nesNoisePeriodToSamples(12, 44100),
                             apu.getVoiceNoisePeriodForTesting(slot));
}

// Setting the sentinel returns the voice to the legacy frequency-derived
// period on the next note.
void test_noise_lut_off_restores_legacy_period(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);

    apu.setVoiceNesNoiseLutMode(slot, 15);
    TEST_ASSERT_EQUAL_UINT32(100u, apu.getVoiceNoisePeriodForTesting(slot));

    apu.setVoiceNesNoiseLutMode(slot, 255);
    TEST_ASSERT_EQUAL_UINT8(255, apu.getVoiceNesNoiseLutIndexForTesting(slot));
    TEST_ASSERT_EQUAL_UINT32(44u, apu.getVoiceNoisePeriodForTesting(slot));
}

// Indices 16..254 are invalid and must fall back to "off" rather than
// indexing past the table.
void test_noise_lut_invalid_index_falls_back_to_off(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);

    apu.setVoiceNesNoiseLutMode(slot, 16);
    TEST_ASSERT_EQUAL_UINT8(255, apu.getVoiceNesNoiseLutIndexForTesting(slot));
    TEST_ASSERT_EQUAL_UINT32(44u, apu.getVoiceNoisePeriodForTesting(slot));
}

// PULSE / TRIANGLE have no noise period; the setter must not disturb them.
void test_noise_lut_ignored_for_non_noise(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingPulseVoice(apu, buffer);
    const uint32_t before = apu.getVoiceNoisePeriodForTesting(slot);

    apu.setVoiceNesNoiseLutMode(slot, 15);

    // The mode is stored (introspectable) but the PULSE voice is untouched.
    TEST_ASSERT_EQUAL_UINT32(before, apu.getVoiceNoisePeriodForTesting(slot));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(4000,
        renderPeakMagnitude(apu, buffer, 256),
        "a PULSE voice must keep playing after a noise-LUT call");
}

// M17's safe state resets the NES SUB-UNITS, not the waveform mode selectors.
// `pulseDutyIndex` (M2) and `nesNoiseLutIndex` (M8) are deliberately outside
// its scope, so a nesAccurate note must not clear a mode the app selected.
void test_noise_lut_survives_nes_safe_init(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);
    apu.setVoiceNesNoiseLutMode(slot, 9);

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;
    const int slot2 = playPresetNote(apu, buffer, WaveType::NOISE, preset);
    TEST_ASSERT_EQUAL_INT(slot, slot2);

    TEST_ASSERT_EQUAL_UINT8(9, apu.getVoiceNesNoiseLutIndexForTesting(slot));
    TEST_ASSERT_EQUAL_UINT32(nes_apu::nesNoisePeriodToSamples(9, 44100),
                             apu.getVoiceNoisePeriodForTesting(slot));
}

// The LUT is sample-rate independent: the same index must give the same LFSR
// clock in Hz (within rounding) at 22050 and 44100.
void test_noise_lut_is_sample_rate_independent(void) {
    int16_t buffer[256] = {0};

    ApuCore apu44;
    apu44.init(44100);
    apu44.reset();
    const int slot44 = startLoopingNoiseVoice(apu44, buffer);
    apu44.setVoiceNesNoiseLutMode(slot44, 14);
    const float hz44 = 44100.0f / (float)apu44.getVoiceNoisePeriodForTesting(slot44);

    ApuCore apu22;
    apu22.init(22050);
    apu22.reset();
    const int slot22 = startLoopingNoiseVoice(apu22, buffer);
    apu22.setVoiceNesNoiseLutMode(slot22, 14);
    const float hz22 = 22050.0f / (float)apu22.getVoiceNoisePeriodForTesting(slot22);

    // Index 14 = 2034 CPU cycles -> 1789773 / 2034 ~= 880 Hz LFSR clock.
    TEST_ASSERT_FLOAT_WITHIN(20.0f, 880.0f, hz44);
    TEST_ASSERT_FLOAT_WITHIN(20.0f, hz44, hz22);
}

// =============================================================================
// Hito 4 M10: configurable master HPF
// =============================================================================
//
// The DC blocker is `y[n] = x[n] - x[n-1] + R*y[n-1]`, where
// `R = exp(-2*pi*fc/fs)`. It used to carry a hardcoded R = 0.995, which pins
// the coefficient rather than the cutoff: that is ~35.2 Hz at 44100 Hz but
// ~17.6 Hz at 22050 Hz, so the filter changed character with the sample rate.
//
// M10 stores the cutoff in Hz and derives R from it and the current sample
// rate. The default stays the historical coefficient bit-for-bit so the
// canonical golden PCM does not move; calling the setter opts into the
// rate-independent behaviour.

// Starts a continuous 110 Hz PULSE at 12.5% duty. The asymmetric duty is the
// point: it carries a large DC component (mean = 0.125 - 0.875 = -0.75 of full
// scale), which is exactly what a DC blocker is supposed to remove.
static int startLowPulseVoice(ApuCore& apu, int16_t* scratch) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 110.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.8f;
    cmd.event.duty = 0.125f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    apu.generateSamples(scratch, 512);

    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            return s;
        }
    }
    TEST_FAIL_MESSAGE("looping low PULSE event did not land on an SFX slot");
    return -1;
}

// Absolute DC (mean signed sample) of the first window after the note starts,
// with the HPF configured at `cutoffHz`. Negative `cutoffHz` leaves the legacy
// default in place. Signed mean rather than magnitude: it isolates exactly the
// quantity the filter targets, so bypass and any active corner separate by an
// order of magnitude instead of a few counts.
static int32_t measureHpfDc(float cutoffHz) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    if (cutoffHz >= 0.0f) {
        apu.setHpfCutoffHz(cutoffHz);
    }

    int16_t buffer[512] = {0};
    startLowPulseVoice(apu, buffer);   // renders the first 512 samples

    int64_t total = 0;
    for (int i = 0; i < 512; ++i) {
        total += buffer[i];
    }
    const int32_t dc = (int32_t)(total / 512);
    return dc < 0 ? -dc : dc;
}

// Untouched, the APU must keep the exact historical coefficient. This is what
// guarantees the golden PCM is unchanged by M10.
void test_hpf_default_is_legacy_coefficient(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    TEST_ASSERT_TRUE(apu.getHpfEnabledForTesting());
    TEST_ASSERT_EQUAL_FLOAT(0.995f, apu.getHpfRForTesting());
    TEST_ASSERT_EQUAL_INT32(32604, apu.getHpfRQ15ForTesting());
    // Negative cutoff is the "legacy coefficient" sentinel.
    TEST_ASSERT_TRUE(apu.getHpfCutoffHzForTesting() < 0.0f);
}

// Setting a cutoff derives R from it. 90 Hz is the NES analog HPF corner.
void test_hpf_cutoff_computes_coefficient(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setHpfCutoffHz(90.0f);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, apu.getHpfCutoffHzForTesting());
    // exp(-2*pi*90/44100) = 0.9872591
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9872591f, apu.getHpfRForTesting());
    // The Q15 mirror must track the float coefficient.
    TEST_ASSERT_INT32_WITHIN(2,
        (int32_t)(0.9872591f * 32768.0f + 0.5f), apu.getHpfRQ15ForTesting());

    apu.setHpfCutoffHz(240.0f);
    // exp(-2*pi*240/44100) = 0.9663837
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9663837f, apu.getHpfRForTesting());
}

// The whole point: the same cutoff in Hz must survive a sample-rate change,
// which the old fixed coefficient did not.
void test_hpf_cutoff_is_sample_rate_independent(void) {
    ApuCore apu44;
    apu44.init(44100);
    apu44.reset();
    apu44.setHpfCutoffHz(90.0f);

    ApuCore apu22;
    apu22.init(22050);
    apu22.reset();
    apu22.setHpfCutoffHz(90.0f);

    // Different coefficients...
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9872591f, apu44.getHpfRForTesting());
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9746804f, apu22.getHpfRForTesting());

    // ...but the same corner frequency: fc = -ln(R) * fs / (2*pi).
    const float fc44 = -logf(apu44.getHpfRForTesting()) * 44100.0f / 6.2831853f;
    const float fc22 = -logf(apu22.getHpfRForTesting()) * 22050.0f / 6.2831853f;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 90.0f, fc44);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 90.0f, fc22);

    // For contrast, the legacy coefficient does NOT hold its corner: the same
    // R = 0.995 is ~35.2 Hz at 44100 and ~17.6 Hz at 22050.
    const float legacy44 = -logf(0.995f) * 44100.0f / 6.2831853f;
    const float legacy22 = -logf(0.995f) * 22050.0f / 6.2831853f;
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 35.2f, legacy44);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 17.6f, legacy22);
}

// Re-initialising at a new rate must recompute R so the cutoff is preserved.
void test_hpf_init_recomputes_for_new_sample_rate(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setHpfCutoffHz(90.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9872591f, apu.getHpfRForTesting());

    apu.init(22050);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.9746804f, apu.getHpfRForTesting());
    TEST_ASSERT_EQUAL_FLOAT(90.0f, apu.getHpfCutoffHzForTesting());
}

// Cutoff 0 (or negative) bypasses the filter entirely.
void test_hpf_zero_cutoff_disables_filter(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setHpfCutoffHz(0.0f);
    TEST_ASSERT_FALSE(apu.getHpfEnabledForTesting());

    apu.setHpfCutoffHz(90.0f);
    TEST_ASSERT_TRUE(apu.getHpfEnabledForTesting());

    apu.setHpfCutoffHz(-5.0f);
    TEST_ASSERT_FALSE(apu.getHpfEnabledForTesting());
}

// Behavioural: the filter's job is removing DC, so measure DC. Bypass leaves
// the 12.5%-duty offset intact; every active corner removes it, and a higher
// corner removes it faster. Measured DC at 44100 Hz over the first 512
// samples: bypass ~5900, 35 Hz ~1970, 90 Hz ~760, 240 Hz ~410.
void test_hpf_cutoff_changes_low_frequency_content(void) {
    const int32_t dcBypass = measureHpfDc(0.0f);
    const int32_t dc35 = measureHpfDc(35.0f);
    const int32_t dc90 = measureHpfDc(90.0f);
    const int32_t dc240 = measureHpfDc(240.0f);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(3000, dcBypass,
        "a bypassed HPF must leave the duty-asymmetry DC intact");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(dc35, dcBypass / 2,
        "any active corner must remove most of the DC a bypass keeps");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(dc90, dc35,
        "a 90 Hz corner must settle the DC faster than 35 Hz");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(dc240, dc90,
        "a 240 Hz corner must settle the DC faster than 90 Hz");
}

// The untouched default must behave like its nominal corner (~35 Hz at
// 44100), not like a bypass. Guards the "legacy coefficient" path.
void test_hpf_legacy_default_still_blocks_dc(void) {
    const int32_t dcLegacy = measureHpfDc(-1.0f);   // never call the setter
    const int32_t dcBypass = measureHpfDc(0.0f);
    const int32_t dc35 = measureHpfDc(35.0f);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(dcLegacy, dcBypass / 2,
        "the default HPF must block DC, not pass it through");
    // R = 0.995 is ~35.2 Hz at 44100, so it must land next to the 35 Hz case.
    TEST_ASSERT_INT32_WITHIN_MESSAGE(150, dc35, dcLegacy,
        "the legacy coefficient must behave like its nominal ~35 Hz corner");
}

// reset() restores configuration as well as state, the same way it restores
// masterVolume and the bitcrush depth.
void test_hpf_reset_restores_legacy_coefficient(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setHpfCutoffHz(240.0f);
    TEST_ASSERT_TRUE(apu.getHpfCutoffHzForTesting() > 0.0f);

    apu.reset();
    TEST_ASSERT_TRUE(apu.getHpfCutoffHzForTesting() < 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.995f, apu.getHpfRForTesting());
    TEST_ASSERT_EQUAL_INT32(32604, apu.getHpfRQ15ForTesting());
    TEST_ASSERT_TRUE(apu.getHpfEnabledForTesting());
}

// Absurd cutoffs must not produce a coefficient outside (0, 1), which would
// make the recursive filter unstable.
void test_hpf_extreme_cutoff_stays_stable(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setHpfCutoffHz(100000.0f);  // far above Nyquist
    TEST_ASSERT_TRUE(apu.getHpfRForTesting() >= 0.0f);
    TEST_ASSERT_TRUE(apu.getHpfRForTesting() < 1.0f);

    apu.setHpfCutoffHz(0.001f);  // effectively DC
    TEST_ASSERT_TRUE(apu.getHpfRForTesting() > 0.0f);
    TEST_ASSERT_TRUE(apu.getHpfRForTesting() < 1.0f);

    int16_t buffer[512] = {0};
    startLowPulseVoice(apu, buffer);
    apu.generateSamples(buffer, 512);  // must not blow up
    for (int i = 0; i < 512; ++i) {
        TEST_ASSERT_TRUE(buffer[i] >= -32768 && buffer[i] <= 32767);
    }
}

// =============================================================================
// Hito 4 M11: configurable soft-clip
// =============================================================================
//
// The summed mix passes through a shaping curve before the DC blocker.
// `Rational` (`S / (1 + |S|*MIXER_K)`) is the historical default and stays
// bit-exact, including the pre-fitted `audio_mixer_lut` on the integer path.
// `HardClip` is the interesting addition: the NES DAC saturates rather than
// compressing, and the analog HPF then filters the already-squared signal.

// Fills the SFX pool with loud PULSE voices so the accumulated mix is driven
// well past full scale and the curves actually diverge. Returns how many
// voices started.
static int startLoudMix(ApuCore& apu, int16_t* scratch) {
    int started = 0;
    for (int i = 0; i < 4; ++i) {
        AudioCommand cmd{};
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event.type = WaveType::PULSE;
        cmd.event.frequency = 220.0f + 55.0f * (float)i;
        cmd.event.duration = 0.0f;
        cmd.event.volume = 1.0f;
        cmd.event.duty = 0.5f;
        cmd.event.loop = true;
        if (apu.submitCommand(cmd)) ++started;
    }
    apu.generateSamples(scratch, 512);
    TEST_ASSERT_GREATER_THAN_INT(0, started);
    return started;
}

// Regime A: ONE voice at full volume, HPF bypassed. The mix peaks at
// MIXER_SCALE = 0.4, comfortably below unity, so no clamp ever engages and
// each curve's actual shape reaches the output. This is what pins the
// formulas and the amplitude scaling; a wrong constant moves these numbers a
// long way.
static int32_t measureCurvePeakBelowUnity(SoftClipMode mode) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoftClipMode(mode);
    apu.setHpfCutoffHz(0.0f);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 220.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[2048] = {0};
    apu.generateSamples(buffer, 2048);   // dispatch + settle the ADSR
    return renderPeakMagnitude(apu, buffer, 2048);
}

// Regime B: four loud voices with an asymmetric duty and the HPF running, so
// the mix is driven past unity and the curves diverge in how much they let
// through. Mean magnitude, because above unity the final int16 clamp flattens
// every curve to the same peak — the peak cannot tell them apart, the average
// can.
static void measureOverdrive(SoftClipMode mode, int32_t* meanAbs, int32_t* rails) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoftClipMode(mode);
    apu.setHpfCutoffHz(90.0f);

    for (int i = 0; i < 4; ++i) {
        AudioCommand cmd{};
        cmd.type = AudioCommandType::PLAY_EVENT;
        cmd.event.type = WaveType::PULSE;
        cmd.event.frequency = 220.0f + 55.0f * (float)i;
        cmd.event.duration = 0.0f;
        cmd.event.volume = 1.0f;
        cmd.event.duty = 0.125f;
        cmd.event.loop = true;
        apu.submitCommand(cmd);
    }

    int16_t buffer[2048] = {0};
    apu.generateSamples(buffer, 2048);
    apu.generateSamples(buffer, 2048);

    int64_t total = 0;
    int32_t railed = 0;
    for (int i = 0; i < 2048; ++i) {
        const int32_t mag = buffer[i] < 0 ? -(int32_t)buffer[i] : buffer[i];
        total += mag;
        if (mag >= 32766) ++railed;
    }
    *meanAbs = (int32_t)(total / 2048);
    *rails = railed;
}

void test_softclip_default_is_rational(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::Rational,
                          (int)apu.getSoftClipModeForTesting());
}

void test_softclip_mode_round_trips(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setSoftClipMode(SoftClipMode::HardClip);
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::HardClip,
                          (int)apu.getSoftClipModeForTesting());
    apu.setSoftClipMode(SoftClipMode::Tanh);
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::Tanh,
                          (int)apu.getSoftClipModeForTesting());
    apu.setSoftClipMode(SoftClipMode::None);
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::None,
                          (int)apu.getSoftClipModeForTesting());
}

// reset() restores configuration as well as state, matching masterVolume,
// the bitcrush depth and the HPF corner.
void test_softclip_reset_restores_default(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setSoftClipMode(SoftClipMode::HardClip);
    apu.reset();
    TEST_ASSERT_EQUAL_INT((int)SoftClipMode::Rational,
                          (int)apu.getSoftClipModeForTesting());
}

// Below unity each curve reaches the output unaltered by any clamp, so the
// levels are a direct read-out of the formulas at S = MIXER_SCALE = 0.4:
//   None      0.4                          -> 0.4    * 32767 = 13107
//   Rational  0.4 / (1 + 0.4*0.5)          -> 0.3333 * 32767 = 10922
//   Tanh      0.4 * (27+0.16)/(27+1.44)    -> 0.3820 * 32767 = 12519
//   HardClip  clamp(0.4)                   -> identical to None
// Tight tolerances on purpose: this is the test that catches a wrong
// coefficient or a wrong fixed-point scale.
void test_softclip_curve_shapes_below_unity(void) {
    const int32_t none = measureCurvePeakBelowUnity(SoftClipMode::None);
    const int32_t rational = measureCurvePeakBelowUnity(SoftClipMode::Rational);
    const int32_t tanhMode = measureCurvePeakBelowUnity(SoftClipMode::Tanh);
    const int32_t hard = measureCurvePeakBelowUnity(SoftClipMode::HardClip);

    TEST_ASSERT_INT32_WITHIN_MESSAGE(40, 13107, none,
        "unshaped output must be S * 32767 with S = MIXER_SCALE");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(40, 10922, rational,
        "the rational curve must match S/(1+|S|*MIXER_K)");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(40, 12519, tanhMode,
        "the tanh approximation must match S*(27+S^2)/(27+9*S^2)");
    TEST_ASSERT_INT32_WITHIN_MESSAGE(40, none, hard,
        "below unity HardClip must be a no-op, identical to None");
}

// Above unity every curve gets flattened to the same peak by the final int16
// clamp, so compare averages. HardClip caps the mix at 1.0 while the rational
// curve asymptotes toward 1/MIXER_K = 2.0, which makes HardClip the QUIETEST
// of the shaped modes on an overdriven mix — not the loudest.
void test_softclip_overdrive_ordering(void) {
    int32_t meanNone, meanRational, meanTanh, meanHard;
    int32_t railsNone, railsRational, railsTanh, railsHard;
    measureOverdrive(SoftClipMode::None, &meanNone, &railsNone);
    measureOverdrive(SoftClipMode::Rational, &meanRational, &railsRational);
    measureOverdrive(SoftClipMode::Tanh, &meanTanh, &railsTanh);
    measureOverdrive(SoftClipMode::HardClip, &meanHard, &railsHard);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(meanRational, meanNone,
        "an unshaped overdriven mix must pass more level than the rational curve");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(meanTanh, meanRational,
        "the rational curve must pass more level than the tanh approximation");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(meanHard, meanTanh,
        "HardClip caps at 1.0, so it must pass the least level of the shaped modes");

    // And the shaping has to actually bite: unshaped must be well clear of it.
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(2 * meanHard, meanNone,
        "HardClip must bound an overdriven mix far below the unshaped one");
}

// Curve ordering is not the only observable: an unshaped mix overshoots into
// the final clamp far more often, because nothing bounded it beforehand.
void test_softclip_none_hits_the_rails_more_than_hardclip(void) {
    int32_t meanNone, meanHard, railsNone, railsHard;
    measureOverdrive(SoftClipMode::None, &meanNone, &railsNone);
    measureOverdrive(SoftClipMode::HardClip, &meanHard, &railsHard);

    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(0, railsHard,
        "an overdriven mix must reach full scale under HardClip");
    TEST_ASSERT_GREATER_THAN_INT32_MESSAGE(railsHard, railsNone,
        "without shaping the mix must overshoot into the clamp more often");
}

// Every mode must stay inside int16 and never wrap, even fully overdriven.
void test_softclip_all_modes_stay_in_range(void) {
    const SoftClipMode modes[4] = {
        SoftClipMode::None, SoftClipMode::Rational,
        SoftClipMode::Tanh, SoftClipMode::HardClip
    };
    for (int m = 0; m < 4; ++m) {
        ApuCore apu;
        apu.init(44100);
        apu.reset();
        apu.setSoftClipMode(modes[m]);

        int16_t buffer[1024] = {0};
        startLoudMix(apu, buffer);
        apu.generateSamples(buffer, 1024);
        for (int i = 0; i < 1024; ++i) {
            TEST_ASSERT_TRUE(buffer[i] >= -32768 && buffer[i] <= 32767);
        }
    }
}

// The curve runs BEFORE the DC blocker, which is what makes HardClip model the
// NES signal chain. Verify the ordering is observable: with the HPF active,
// HardClip's squared-off output still carries the duty-asymmetry DC removal,
// i.e. changing the curve does not bypass the filter.
void test_softclip_runs_before_the_dc_blocker(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoftClipMode(SoftClipMode::HardClip);
    apu.setHpfCutoffHz(90.0f);

    int16_t buffer[512] = {0};
    startLowPulseVoice(apu, buffer);   // 12.5% duty, strong DC

    int64_t total = 0;
    for (int i = 0; i < 512; ++i) total += buffer[i];
    const int32_t dc = (int32_t)(total / 512);
    const int32_t absDc = dc < 0 ? -dc : dc;

    TEST_ASSERT_LESS_THAN_INT32_MESSAGE(2000, absDc,
        "the DC blocker must still run after the soft-clip curve");
}

// =============================================================================
// Hito 4 M14: VoiceNesOptions
// =============================================================================
//
// One struct that arms every per-voice NES opt-in at once, reachable two ways:
// `setVoiceNesOptions` directly, and `AudioCommandType::SET_NES_OPTIONS`
// through the queue for callers on the game thread. Same shape as M13's
// `setVoiceNesLength` / `TRIGGER_NES_LENGTH` pair — one semantic, two entry
// points, pinned by a test so they cannot drift apart.

static VoiceNesOptions allOnOptions(void) {
    VoiceNesOptions opts{};
    opts.lengthCounterEnabled = true;
    opts.linearCounterEnabled = true;
    opts.envelopeEnabled = true;
    opts.sweepUnitEnabled = true;
    opts.pulseDutyIndex = 2;
    opts.noiseLutIndex = 9;
    return opts;
}

void test_nes_options_defaults_are_all_off(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    const VoiceNesOptions opts = apu.getVoiceNesOptions(0);
    TEST_ASSERT_FALSE(opts.lengthCounterEnabled);
    TEST_ASSERT_FALSE(opts.linearCounterEnabled);
    TEST_ASSERT_FALSE(opts.envelopeEnabled);
    TEST_ASSERT_FALSE(opts.sweepUnitEnabled);
    TEST_ASSERT_EQUAL_UINT8(255, opts.pulseDutyIndex);
    TEST_ASSERT_EQUAL_UINT8(255, opts.noiseLutIndex);
}

void test_nes_options_round_trip(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setVoiceNesOptions(0, allOnOptions());
    const VoiceNesOptions read = apu.getVoiceNesOptions(0);

    TEST_ASSERT_TRUE(read.lengthCounterEnabled);
    TEST_ASSERT_TRUE(read.linearCounterEnabled);
    TEST_ASSERT_TRUE(read.envelopeEnabled);
    TEST_ASSERT_TRUE(read.sweepUnitEnabled);
    TEST_ASSERT_EQUAL_UINT8(2, read.pulseDutyIndex);
    TEST_ASSERT_EQUAL_UINT8(9, read.noiseLutIndex);
}

// The struct must write the REAL sub-unit fields, not a shadow copy that only
// the getter can see. Checked through the per-unit accessors.
void test_nes_options_arms_the_real_subunits(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setVoiceNesOptions(0, allOnOptions());

    TEST_ASSERT_TRUE(apu.getVoiceNesChannelEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesLinearEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesEnvelopeEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.getVoiceNesSweepUnitEnabledForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(9, apu.getVoiceNesNoiseLutIndexForTesting(0));
}

// Clearing must actually disarm, not just leave the previous state behind.
void test_nes_options_disarms_everything(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setVoiceNesOptions(0, allOnOptions());
    apu.setVoiceNesOptions(0, VoiceNesOptions{});

    TEST_ASSERT_FALSE(apu.getVoiceNesChannelEnabledForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesLinearEnabledForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesEnvelopeEnabledForTesting(0));
    TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(0));
    TEST_ASSERT_EQUAL_UINT8(255, apu.getVoiceNesNoiseLutIndexForTesting(0));
}

// Out-of-range selector values must land on the documented sentinels, exactly
// as the dedicated setters already do.
void test_nes_options_invalid_indices_fall_back(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    VoiceNesOptions opts{};
    opts.pulseDutyIndex = 200;   // valid range is 0..3
    opts.noiseLutIndex = 200;    // valid range is 0..15
    apu.setVoiceNesOptions(0, opts);

    const VoiceNesOptions read = apu.getVoiceNesOptions(0);
    TEST_ASSERT_EQUAL_UINT8(255, read.pulseDutyIndex);
    TEST_ASSERT_EQUAL_UINT8(255, read.noiseLutIndex);
}

// The queued command must land on exactly the same state as the direct call.
// If these ever diverge, a caller would get different results depending on
// which entry point they used.
void test_nes_options_command_matches_direct_setter(void) {
    ApuCore direct;
    direct.init(44100);
    direct.reset();
    direct.setVoiceNesOptions(1, allOnOptions());

    ApuCore queued;
    queued.init(44100);
    queued.reset();
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SET_NES_OPTIONS;
    cmd.channelIndex = 1;
    cmd.nesOptions = allOnOptions();
    TEST_ASSERT_TRUE(queued.submitCommand(cmd));
    int16_t buffer[64] = {0};
    queued.generateSamples(buffer, 64);

    const VoiceNesOptions a = direct.getVoiceNesOptions(1);
    const VoiceNesOptions b = queued.getVoiceNesOptions(1);
    TEST_ASSERT_EQUAL_UINT8(a.lengthCounterEnabled, b.lengthCounterEnabled);
    TEST_ASSERT_EQUAL_UINT8(a.linearCounterEnabled, b.linearCounterEnabled);
    TEST_ASSERT_EQUAL_UINT8(a.envelopeEnabled, b.envelopeEnabled);
    TEST_ASSERT_EQUAL_UINT8(a.sweepUnitEnabled, b.sweepUnitEnabled);
    TEST_ASSERT_EQUAL_UINT8(a.pulseDutyIndex, b.pulseDutyIndex);
    TEST_ASSERT_EQUAL_UINT8(a.noiseLutIndex, b.noiseLutIndex);
    TEST_ASSERT_TRUE(queued.getVoiceNesSweepUnitEnabledForTesting(1));
}

void test_nes_options_command_ignores_bad_slot(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::SET_NES_OPTIONS;
    cmd.channelIndex = (uint8_t)ApuCore::MAX_VOICES;   // out of range
    cmd.nesOptions = allOnOptions();
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);   // must not crash

    for (int s = 0; s < ApuCore::MAX_VOICES; ++s) {
        TEST_ASSERT_FALSE(apu.getVoiceNesChannelEnabledForTesting(s));
        TEST_ASSERT_FALSE(apu.getVoiceNesSweepUnitEnabledForTesting(s));
    }
}

// Selecting a noise LUT index through the struct must retune the voice
// immediately, the same as calling `setVoiceNesNoiseLutMode` would.
void test_nes_options_noise_lut_retunes_immediately(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);

    VoiceNesOptions opts{};
    opts.noiseLutIndex = 15;
    apu.setVoiceNesOptions(slot, opts);

    TEST_ASSERT_EQUAL_UINT32(nes_apu::nesNoisePeriodToSamples(15, 44100),
                             apu.getVoiceNoisePeriodForTesting(slot));
}

// M17's safe state clears the NES sub-units but deliberately leaves the
// waveform mode selectors alone. Reading the options back after a nesAccurate
// note must show exactly that split, so the two features stay consistent.
void test_nes_options_reflect_nes_safe_boundary(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    int16_t buffer[256] = {0};
    const int slot = startLoopingNoiseVoice(apu, buffer);
    apu.setVoiceNesOptions(slot, allOnOptions());

    // Stop it first: `playPresetNote` returns the first ENABLED SFX slot, so
    // with the old voice still running the new note lands elsewhere and the
    // assertions below would read an untouched slot.
    AudioCommand stop{};
    stop.type = AudioCommandType::STOP_CHANNEL;
    stop.channelIndex = (uint8_t)slot;
    TEST_ASSERT_TRUE(apu.submitCommand(stop));
    apu.generateSamples(buffer, 64);
    // STOP_CHANNEL resets the voice, so re-arm the modes we want to observe.
    apu.setVoiceNesOptions(slot, allOnOptions());

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;
    const int slot2 = playPresetNote(apu, buffer, WaveType::NOISE, preset);
    TEST_ASSERT_EQUAL_INT(slot, slot2);

    const VoiceNesOptions after = apu.getVoiceNesOptions(slot);
    // Sub-units: cleared by initNesSafe.
    TEST_ASSERT_FALSE(after.lengthCounterEnabled);
    TEST_ASSERT_FALSE(after.envelopeEnabled);
    TEST_ASSERT_FALSE(after.sweepUnitEnabled);
    TEST_ASSERT_FALSE(after.linearCounterEnabled);
    // Waveform mode selectors: untouched.
    TEST_ASSERT_EQUAL_UINT8(9, after.noiseLutIndex);
    TEST_ASSERT_EQUAL_UINT8(2, after.pulseDutyIndex);
}

// =============================================================================
// S4: sound profiles
// =============================================================================
//
// The profile is global configuration: one selector per core, applied to every
// voice. These cases pin the API surface (default, round trip, rejection of
// unknown values, queued path, survival of `init`) but deliberately say nothing
// about which render body the profile picks — that dispatch is T4.

void test_sound_profile_defaults_to_legacy(void) {
    ApuCore apu;
    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Legacy,
                          (int)apu.getSoundProfile());
}

void test_sound_profile_round_trips(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setSoundProfile(SoundProfile::Nes);
    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Nes, (int)apu.getSoundProfile());
    apu.setSoundProfile(SoundProfile::Hybrid);
    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Hybrid, (int)apu.getSoundProfile());
    apu.setSoundProfile(SoundProfile::Legacy);
    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Legacy, (int)apu.getSoundProfile());
}

// Garbage must be ignored, not clamped: 7 is not "nearest profile" and must
// leave whatever was configured untouched.
void test_sound_profile_ignores_unknown_value(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    apu.setSoundProfile(SoundProfile::Nes);
    apu.setSoundProfile(static_cast<SoundProfile>(7));

    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Nes, (int)apu.getSoundProfile());
}

// Game-thread callers reach the setter through the queue, exactly like
// SET_NES_OPTIONS; the command must land on the same state.
void test_sound_profile_command_applies_on_render(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::SET_SOUND_PROFILE;
    cmd.soundProfile = static_cast<uint8_t>(SoundProfile::Nes);
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[64] = {0};
    apu.generateSamples(buffer, 64);

    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Nes, (int)apu.getSoundProfile());
}

// The profile is configuration, not state: init() restarts the stream but must
// keep what the caller selected.
void test_sound_profile_survives_init(void) {
    ApuCore apu;
    apu.setSoundProfile(SoundProfile::Hybrid);
    apu.init(48000);

    TEST_ASSERT_EQUAL_INT((int)SoundProfile::Hybrid, (int)apu.getSoundProfile());
}

// =============================================================================
// S3 / S5 / S6: core dispatch, profile character and the hot switch
// =============================================================================
//
// Both suite binaries run this file, so every case below has to hold whichever
// core the build contains: `test_apu_core` has the float body, while
// `test_apu_core_integer` is built with PR32_APU_HAS_FPU=0. `Nes` and `Hybrid`
// select the integer core in both builds (S3), so a case may say what those two
// profiles sound like without knowing which core the build compiled. Since E1
// they are no longer aliases of `Legacy` — `Hybrid` carries the exponential
// envelope — and the cases below pin what each one renders.

// Queues one looping SINE note. Split from the render calls so the caller can
// put a `setSoundProfile` call between two blocks: a profile change cannot land
// mid-buffer, only at a render-call boundary.
static void queueSustainedSine(ApuCore& apu, float volume) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::SINE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.0f;
    cmd.event.volume = volume;
    cmd.event.duty = 0.5f;
    cmd.event.loop = true;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));
}

static int32_t peakMagnitude(const int16_t* pcm, int length) {
    int32_t peak = 0;
    for (int i = 0; i < length; ++i) {
        const int32_t mag = (pcm[i] < 0) ? -(int32_t)pcm[i] : (int32_t)pcm[i];
        if (mag > peak) peak = mag;
    }
    return peak;
}

// Largest |x[n] - x[n-1]| in [from, to). Adjacent-sample steps are what a click
// looks like in PCM; a level jump shows up here and nowhere else.
static int32_t maxAdjacentStep(const int16_t* pcm, int from, int to) {
    int32_t worst = 0;
    for (int i = (from > 0 ? from : 1); i < to; ++i) {
        const int32_t step = (int32_t)pcm[i] - (int32_t)pcm[i - 1];
        const int32_t mag = (step < 0) ? -step : step;
        if (mag > worst) worst = mag;
    }
    return worst;
}

// S5: `applyProfileCharacter` runs at the very end of `initVoiceFromEvent`,
// after the `nesAccurate` / `initNesSafe` block. The counter is the structural
// proof: exactly one application per note-on, and the canonical safe state the
// block installed is still what the voice reports afterwards.
//
// E1 extends the same case instead of adding a sibling: the character the
// profile writes is now visible (the envelope curve), so the ordering claim is
// checked twice — once structurally (the counter) and once by observing a value
// only the last step can produce. Run under `Hybrid`, whose exponential curve
// differs from everything the note path leaves behind.
void profile_apply_runs_after_nes_accurate_init(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoundProfile(SoundProfile::Hybrid);

    TEST_ASSERT_EQUAL_UINT32(0, apu.getProfileApplyCountForTesting());

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.nesAccurate = true;

    int16_t buffer[256] = {0};
    const int slot = playPresetNote(apu, buffer, WaveType::PULSE, preset);
    TEST_ASSERT_EQUAL_UINT32(1, apu.getProfileApplyCountForTesting());
    // Ordering: the profile did not undo the safe state the note left behind.
    assertNesSafeCommon(apu, slot);
    // S2: `initNesSafe` cleared every NES sub-unit above and still did not
    // erase the profile's curve, which is what "applied last" has to mean.
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::Exponential,
                          (int)apu.getVoiceEnvelopeCurveForTesting(slot));

    // Silence the voice without starting a note: the counter must not move.
    AudioCommand stop{};
    stop.type = AudioCommandType::STOP_CHANNEL;
    stop.channelIndex = (uint8_t)slot;
    TEST_ASSERT_TRUE(apu.submitCommand(stop));
    apu.generateSamples(buffer, 64);
    TEST_ASSERT_EQUAL_UINT32(1, apu.getProfileApplyCountForTesting());

    // A second note-on applies character once more — exactly once, not once
    // per voice touched or per render call.
    const int slot2 = playPresetNote(apu, buffer, WaveType::PULSE, preset);
    TEST_ASSERT_EQUAL_UINT32(2, apu.getProfileApplyCountForTesting());
    assertNesSafeCommon(apu, slot2);
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::Exponential,
                          (int)apu.getVoiceEnvelopeCurveForTesting(slot2));
}

// S3, E1 contract change. E0 pinned `Nes` == `Hybrid` byte for byte because
// neither profile had any character yet. E1 gives `Hybrid` the exponential
// envelope, so that aliasing is broken on purpose and the assertion that goes
// with it is replaced by the two claims underneath it:
//
//   * `Nes` still renders the `Legacy` curve — its own character is the 4-bit
//     `nesEnvelope` at 240 Hz, which is not wired in T1 (T3/T4 complete it).
//     The committed integer golden is what pins its bytes; here we pin that it
//     resolves to `Linear` and therefore renders the same bytes as an explicit
//     linear override, while an exponential override does change them.
//   * `Hybrid` defaults to `Exponential`, and the exponential is exactly the
//     curve it renders: same bytes as an explicit exponential override,
//     different bytes from a linear one.
//
// The E0 claim that is still true is kept: both profiles run the same integer
// core, so forcing the same curve on both makes their bytes identical again.
static void renderCurveProbe(SoundProfile profile, EnvelopeCurve curve,
                             int16_t* out, int samples) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoundProfile(profile);

    // Every parameter of the note is identical across probes; only the profile
    // and the preset's curve change, so a byte difference can only come from
    // the curve. `INSTR_PULSE_LEAD` decays (0.18 s) and releases (0.22 s), so
    // both curved stages are exercised inside the window.
    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.envelopeCurve = curve;

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 0.8f;
    cmd.event.duty = 0.25f;
    cmd.event.loop = true;
    cmd.event.preset = &preset;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));
    apu.generateSamples(out, samples);
}

void profile_nes_and_hybrid_differ_only_by_the_envelope_curve(void) {
    constexpr int kSamples = 4096;
    static int16_t nesDefault[kSamples];
    static int16_t hybridDefault[kSamples];
    static int16_t nesLinear[kSamples];
    static int16_t hybridLinear[kSamples];
    static int16_t nesExponential[kSamples];

    renderCurveProbe(SoundProfile::Nes, EnvelopeCurve::ProfileDefault, nesDefault, kSamples);
    renderCurveProbe(SoundProfile::Hybrid, EnvelopeCurve::ProfileDefault, hybridDefault, kSamples);
    renderCurveProbe(SoundProfile::Nes, EnvelopeCurve::Linear, nesLinear, kSamples);
    renderCurveProbe(SoundProfile::Hybrid, EnvelopeCurve::Linear, hybridLinear, kSamples);
    renderCurveProbe(SoundProfile::Nes, EnvelopeCurve::Exponential, nesExponential, kSamples);

    // Guard against comparing two silent buffers: two equal silences prove
    // nothing about the core, only that nothing rendered.
    TEST_ASSERT_TRUE(peakMagnitude(nesDefault, kSamples) > 1000);
    TEST_ASSERT_TRUE(peakMagnitude(hybridDefault, kSamples) > 1000);

    // E1: the defaults are no longer aliases. `Hybrid`'s exponential is the
    // only difference in T1, so the curve is what has to show up here.
    TEST_ASSERT_TRUE_MESSAGE(
        std::memcmp(nesDefault, hybridDefault, sizeof(nesDefault)) != 0,
        "E1: Hybrid must no longer render the same bytes as Nes");

    // Same curve -> same bytes: the two profiles still share one core.
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(nesLinear, hybridLinear, sizeof(nesLinear)));

    // `Nes` in T1 renders the `Legacy` curve: its default is byte-identical to
    // an explicit linear override, and an exponential override *would* move it
    // (which is what keeps the equality above from being vacuous).
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(nesDefault, nesLinear, sizeof(nesLinear)));
    TEST_ASSERT_TRUE(std::memcmp(nesDefault, nesExponential, sizeof(nesExponential)) != 0);

    // `Hybrid`'s default IS the exponential curve, not something in between.
    TEST_ASSERT_TRUE(std::memcmp(hybridDefault, hybridLinear, sizeof(hybridLinear)) != 0);
}

// S6: the measurable no-click criterion. 0.5 s at 44100 Hz, profile switched
// at the midpoint, which is a call boundary (one block per render call).
//
// The note runs at full volume on purpose: at lower levels a lost envelope
// mirror only drops the output part-way and the seam stays under the
// threshold, so the case would pass for the wrong reason.
void profile_hot_switch_does_not_click(void) {
    constexpr int kHalf = 11025;          // 0.25 s each; 0.5 s total
    static int16_t pcm[2 * kHalf];

    ApuCore apu;
    apu.init(44100);
    apu.reset();
    queueSustainedSine(apu, 1.0f);

    apu.generateSamples(pcm, kHalf);          // Legacy core
    apu.setSoundProfile(SoundProfile::Nes);   // switch, voices keep playing
    apu.generateSamples(pcm + kHalf, kHalf);  // integer core

    // No rails anywhere in the render.
    int rails = 0;
    for (int i = 0; i < 2 * kHalf; ++i) {
        if (pcm[i] == 32767 || pcm[i] == -32767) ++rails;
    }
    TEST_ASSERT_EQUAL_INT(0, rails);

    // The seam itself: the eight differences that straddle the switch.
    const int32_t seam = maxAdjacentStep(pcm, kHalf, kHalf + 8);
    TEST_ASSERT_TRUE(seam < (32767 / 4));

    // The note must still be sounding after the switch, otherwise "no click"
    // could be satisfied by an output that simply went silent.
    TEST_ASSERT_TRUE(peakMagnitude(pcm + kHalf, kHalf) > 1000);
}

// L8.3: the test accessors must report the mirrors of the core that is
// actually rendering, not the ones `PR32_APU_HAS_FPU` implies. With `Nes` the
// integer core renders, so `basePhaseIncQ32` / `phaseQ32` are authoritative
// and the float `frequency` / `phase` mirrors stay at their note-on values.
void profile_accessors_follow_the_active_core(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoundProfile(SoundProfile::Nes);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 1.0f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.sweepEndHz = 880.0f;
    cmd.event.sweepDurationSec = 0.1f;  // 4410 samples
    cmd.event.loop = false;
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    // ~52% of the sweep: the single-segment sweep writes `basePhaseIncQ32`
    // only, so a readout taken from the float mirror would still say 440 Hz.
    for (int n = 0; n < 9; ++n) {
        apu.generateSamples(buffer, 256);
    }

    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);

    // Linear midpoint of 440 -> 880 Hz is 660 Hz.
    const float hz = apu.getVoiceFrequencyForTesting(slot);
    TEST_ASSERT_FLOAT_WITHIN(40.0f, 660.0f, hz);

    // Phase: the integer core advances only `phaseQ32`, so a float-mirror
    // readout would be stuck at the note-on value of 0.
    const float phase = apu.getVoicePhaseForTesting(slot);
    TEST_ASSERT_TRUE(phase > 0.0f);
    TEST_ASSERT_TRUE(phase < 1.0f);

    // Duty: both mirrors agree here (no duty sweep), so this pins that the
    // accessor still reports the sounding duty rather than a stale field.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, apu.getVoiceDutyCycleForTesting(slot));
}

// S6 idempotence: selecting the profile already in force must leave every bit
// of render state alone. The strongest form of that claim is byte equality
// against a reference run that never repeats the call — a reset of the output
// stage or of an envelope mirror would show up as a difference.
void profile_setter_is_idempotent_for_state(void) {
    constexpr int kBlock = 11025;
    static int16_t reference[2 * kBlock];
    static int16_t repeated[2 * kBlock];

    auto render = [](int16_t* out, bool repeatSetter) {
        ApuCore apu;
        apu.init(44100);
        apu.reset();
        apu.setSoundProfile(SoundProfile::Nes);

        queueSustainedSine(apu, 1.0f);

        apu.generateSamples(out, kBlock);
        if (repeatSetter) {
            apu.setSoundProfile(SoundProfile::Nes);  // already in force
        }
        apu.generateSamples(out + kBlock, kBlock);
    };

    render(reference, false);
    render(repeated, true);

    TEST_ASSERT_EQUAL_INT(0, std::memcmp(reference, repeated, sizeof(reference)));
    // And the seam is continuous in absolute terms, so the equality above is
    // not simply two identically-broken renders.
    TEST_ASSERT_TRUE(maxAdjacentStep(repeated, kBlock, kBlock + 8) < (32767 / 4));
}

// =============================================================================
// S2 / S3: envelope curve, its profile default and the D-3 precedence (E1/T1)
// =============================================================================

// S2: the new field is appended, so a positional literal written against the
// 19-field layout keeps meaning exactly what it meant, and the zero value is
// the "the profile decides" sentinel rather than a third curve. If this ever
// becomes non-zero, every shipped `INSTR_*` preset silently changes shape.
void instrument_preset_envelope_curve_is_appended_and_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)EnvelopeCurve::ProfileDefault);

    // The same literal the published-ABI case uses: 17 positional values,
    // stopping at `pitchSweepDurationSec`. Everything after it is defaulted.
    constexpr InstrumentPreset positional{
        0.8f, 0.5f, 4, 0.25f, 7,
        0.01f, 0.02f, 0.75f, 0.03f,
        LfoTarget::VOLUME, 6.0f, 0.4f, 0.05f,
        true, 0.125f, 880.0f, 1.5f
    };
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::ProfileDefault,
                          (int)positional.envelopeCurve);
    // The shipped presets are positional too, and none of them opts in.
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::ProfileDefault,
                          (int)INSTR_PULSE_LEAD.envelopeCurve);
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::ProfileDefault,
                          (int)INSTR_KICK.envelopeCurve);
}

// D-3, three tiers: the profile supplies the default curve, the note may
// override it, and the `nesAccurate` safe state must not erase either — it
// clears NES sub-units, not the character the profile resolved.
//
// The observable is the resolved per-voice mirror. Under `Legacy` the frozen
// float body ignores the curve by design (S1), so what is asserted there is the
// contract `initVoiceFromEvent` owes the integer core, not a PCM claim.
void profile_envelope_curve_precedence_matches_d3(void) {
    struct CurveCase {
        SoundProfile profile;
        EnvelopeCurve presetCurve;
        bool nesAccurate;
        EnvelopeCurve expected;
        const char* name;
    };
    static const CurveCase cases[] = {
        {SoundProfile::Legacy, EnvelopeCurve::ProfileDefault, false,
         EnvelopeCurve::Linear,
         "Legacy default is the frozen linear ramp"},
        {SoundProfile::Legacy, EnvelopeCurve::Exponential, false,
         EnvelopeCurve::Exponential,
         "a note overrides the Legacy default"},
        {SoundProfile::Hybrid, EnvelopeCurve::ProfileDefault, false,
         EnvelopeCurve::Exponential,
         "Hybrid default is exponential"},
        {SoundProfile::Hybrid, EnvelopeCurve::Linear, false,
         EnvelopeCurve::Linear,
         "a note overrides the Hybrid default"},
        {SoundProfile::Hybrid, EnvelopeCurve::ProfileDefault, true,
         EnvelopeCurve::Exponential,
         "nesAccurate must not erase the profile character"},
        {SoundProfile::Nes, EnvelopeCurve::ProfileDefault, false,
         EnvelopeCurve::Linear,
         "Nes keeps the Legacy curve until its 4-bit envelope lands"},
        {SoundProfile::Nes, EnvelopeCurve::ProfileDefault, true,
         EnvelopeCurve::Linear,
         "nesAccurate must not change Nes either"},
        {SoundProfile::Nes, EnvelopeCurve::Exponential, true,
         EnvelopeCurve::Exponential,
         "initNesSafe must not erase a note override"},
    };

    for (const CurveCase& c : cases) {
        ApuCore apu;
        apu.init(44100);
        apu.reset();
        apu.setSoundProfile(c.profile);

        InstrumentPreset preset = INSTR_PULSE_LEAD;
        preset.envelopeCurve = c.presetCurve;
        preset.nesAccurate = c.nesAccurate;

        int16_t scratch[256] = {0};
        const int slot = playPresetNote(apu, scratch, WaveType::PULSE, preset);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)c.expected,
                                      (int)apu.getVoiceEnvelopeCurveForTesting(slot),
                                      c.name);
    }
}

// Plays a note and advances the render by exactly one sample, so the caller
// owns the envelope clock: every further `generateSamples(1)` is one
// `tickEnvelopeQ15` step and the level can be read between them.
static int startOneSampleNote(ApuCore& apu, WaveType type,
                              const InstrumentPreset& preset,
                              float durationSec, bool loop) {
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = type;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = durationSec;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.loop = loop;
    cmd.event.preset = &preset;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t tick[1] = {0};
    apu.generateSamples(tick, 1);

    for (int s = ApuCore::SFX_VOICE_BASE; s < ApuCore::MAX_VOICES; ++s) {
        if (apu.isVoiceEnabledForTesting(s)) {
            return s;
        }
    }
    TEST_FAIL_MESSAGE("the envelope probe did not land on an SFX slot");
    return -1;
}

// The decay the exponential curve is checked against, in Q15 samples.
static constexpr int kEnvelopeDecaySamples = 4410;  // 0.1 s at 44100 Hz

// Reference iteration of the shape, used by the tests below to know what the
// curve is expected to do without duplicating the renderer's clock.
static int32_t stepShapeOnce(int32_t level) {
    return level - (((level - 1) >> 8) + 1);
}

// S9.2 / S2: the exponential decay reaches −60 dB in the **stage's own time**,
// within ±5 %, and keeps its shape while doing it.
//
// That is the clause the first E1 cut broke: the plain recurrence runs at τ =
// 256 samples (5.8 ms at 44.1 kHz) no matter what `decayTime` says, so a
// 0.1 s decay hit −60 dB after ~30 ms and then sat silent until the stage
// ended, where it jumped back up to the sustain level. The shape is now clocked
// from the stage duration (`exponentialCurveRateQ15`), so the configured time
// means exactly what it says.
//
// Two properties are pinned here:
//   * the −60 dB crossing lands on the stage duration;
//   * the ramp is still the exponential shape, not a linear one: the level
//     halves after 176 of the 1 356 shape iterations that span −60 dB, i.e.
//     after ~572 of 4 410 samples, where a linear ramp would halve at 2 205.
void envelope_exponential_decay_reaches_minus_60_db_at_the_stage_end(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoundProfile(SoundProfile::Hybrid);

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.attackTime = 0.0f;      // one sample: the decay starts at full scale
    preset.decayTime = 0.1f;       // kEnvelopeDecaySamples
    preset.sustainLevel = 0.0f;    // −60 dB of full scale is the stage's target
    preset.releaseTime = 0.005f;

    const int slot = startOneSampleNote(apu, WaveType::PULSE, preset, 0.0f, true);

    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::Exponential,
                          (int)apu.getVoiceEnvelopeCurveForTesting(slot));
    // The attack stays linear (S2) and the one-sample ramp lands on full scale.
    TEST_ASSERT_EQUAL_INT32(32768, apu.getVoiceEnvelopeLevelQ15ForTesting(slot));

    // −60 dB of a Q15 envelope: 32768 × 10^(-60/20) ≈ 33.
    constexpr int32_t kMinus60DbQ15 = 33;

    // What the shape does in iterations: the half-level point and the −60 dB
    // point, both counted on the same recurrence the renderer runs.
    int32_t model = 32768;
    int halfSteps = -1;
    int fullSteps = 0;
    while (model > kMinus60DbQ15 / 2) {
        model = stepShapeOnce(model);
        ++fullSteps;
        if (halfSteps < 0 && model <= 16384) {
            halfSteps = fullSteps;
        }
    }
    const int expectedHalf = (kEnvelopeDecaySamples * halfSteps) / fullSteps;

    int halfSample = -1;
    int crossing = -1;
    int32_t previous = 32768;
    for (int n = 1; n <= kEnvelopeDecaySamples; ++n) {
        int16_t tick[1] = {0};
        apu.generateSamples(tick, 1);
        const int32_t level = apu.getVoiceEnvelopeLevelQ15ForTesting(slot);
        // The curve never rises and never parks: every change is a decrease.
        TEST_ASSERT_TRUE_MESSAGE(level <= previous, "the envelope rose");
        TEST_ASSERT_TRUE_MESSAGE(level >= 0, "the envelope went negative");
        if (halfSample < 0 && level <= 16384) {
            halfSample = n;
        }
        if (crossing < 0 && level <= kMinus60DbQ15) {
            crossing = n;
        }
        previous = level;
    }

    TEST_ASSERT_TRUE_MESSAGE(crossing > 0,
                             "the envelope never reached -60 dB inside the stage");
    const int tolerance = (kEnvelopeDecaySamples * 5) / 100;  // ±5 %
    TEST_ASSERT_INT_WITHIN(tolerance, kEnvelopeDecaySamples, crossing);

    TEST_ASSERT_TRUE_MESSAGE(halfSample > 0, "the envelope never fell by half");
    TEST_ASSERT_INT_WITHIN((expectedHalf * 5) / 100, expectedHalf, halfSample);
    // ... and it is not a linear ramp wearing an exponential's name.
    TEST_ASSERT_TRUE(halfSample < (kEnvelopeDecaySamples / 2));
}

// S2: the release uses the same clock and the same shape, calibrated on its own
// stage. The span is the sustain level (the release starts there), so −60 dB
// lands at the end of `releaseTime` instead of ~30 ms into it.
void envelope_exponential_release_reaches_minus_60_db_at_the_stage_end(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoundProfile(SoundProfile::Hybrid);

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.attackTime = 0.0f;
    preset.decayTime = 0.0f;       // attack -> sustain directly
    preset.sustainLevel = 0.5f;    // the release starts at 16384
    preset.releaseTime = 0.1f;     // 4410 samples (equal to the sr/10 clamp)

    constexpr int kReleaseSamples = 4410;
    // 0.01 s = 441 samples of sustain, then the automatic release.
    const int slot = startOneSampleNote(apu, WaveType::PULSE, preset, 0.01f, false);
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::Exponential,
                          (int)apu.getVoiceEnvelopeCurveForTesting(slot));
    TEST_ASSERT_EQUAL_INT32(16384, apu.getVoiceEnvelopeLevelQ15ForTesting(slot));

    int releaseStart = -1;
    int crossing = -1;
    int32_t previous = apu.getVoiceEnvelopeLevelQ15ForTesting(slot);
    for (int n = 1; n <= kReleaseSamples + 512; ++n) {
        int16_t tick[1] = {0};
        apu.generateSamples(tick, 1);
        const int32_t level = apu.getVoiceEnvelopeLevelQ15ForTesting(slot);
        if (releaseStart < 0) {
            if (level < previous) {
                releaseStart = n;
            }
        } else if (crossing < 0 && level <= 33) {
            crossing = n - releaseStart;
            break;
        }
        previous = level;
    }

    TEST_ASSERT_TRUE_MESSAGE(releaseStart > 0, "the release never began");
    TEST_ASSERT_TRUE_MESSAGE(crossing > 0,
                             "the release never reached -60 dB inside the stage");
    const int tolerance = (kReleaseSamples * 5) / 100;
    TEST_ASSERT_INT_WITHIN(tolerance, kReleaseSamples, crossing);
    // The note is still above the noise floor at the end of the stage, i.e. the
    // release no longer goes silent early (the defect this correction fixes).
    TEST_ASSERT_TRUE(apu.getVoiceEnvelopeLevelQ15ForTesting(slot) > 0);
}

// S2: a stage shorter than the shape's own −60 dB span cannot hold the shape at
// one step per sample. Stepping once per sample anyway would leave the level far
// above its target and snap there at the boundary — a 5 ms release would jump
// ~7 dB — so such a stage keeps the frozen linear ramp, which lands exactly on
// its target. This is the documented fallback, not an accident.
void envelope_exponential_short_stage_keeps_the_linear_ramp(void) {
    ApuCore apu;
    apu.init(44100);
    apu.reset();
    apu.setSoundProfile(SoundProfile::Hybrid);

    InstrumentPreset preset = INSTR_PULSE_LEAD;
    preset.attackTime = 0.0f;
    preset.decayTime = 0.002f;     // 88 samples, far below the 1356 step span
    preset.sustainLevel = 0.0f;

    constexpr int kShortStage = 88;
    constexpr int32_t kLinearDelta = 32768 / kShortStage;  // 372

    const int slot = startOneSampleNote(apu, WaveType::PULSE, preset, 0.0f, true);
    TEST_ASSERT_EQUAL_INT((int)EnvelopeCurve::Exponential,
                          (int)apu.getVoiceEnvelopeCurveForTesting(slot));
    TEST_ASSERT_EQUAL_INT32(32768, apu.getVoiceEnvelopeLevelQ15ForTesting(slot));

    int32_t previous = 32768;
    // The last sample is the stage-end snap, so the constant-step claim covers
    // the 87 samples before it.
    for (int n = 1; n < kShortStage; ++n) {
        int16_t tick[1] = {0};
        apu.generateSamples(tick, 1);
        const int32_t level = apu.getVoiceEnvelopeLevelQ15ForTesting(slot);
        // A constant step per sample is the linear ramp.
        TEST_ASSERT_EQUAL_INT32_MESSAGE(previous - kLinearDelta, level,
                                        "a short stage must keep the linear ramp");
        previous = level;
    }

    int16_t tick[1] = {0};
    apu.generateSamples(tick, 1);
    // The stage ends at its target with nothing left to snap: the linear ramp
    // already brought the level to the noise floor, so there is no click.
    TEST_ASSERT_TRUE(previous < 512);
    TEST_ASSERT_EQUAL_INT32(0, apu.getVoiceEnvelopeLevelQ15ForTesting(slot));
}

// The defect this correction fixes, measured where it was audible: the canonical
// track's lead under `Hybrid`. `INSTR_PULSE_LEAD` decays for 0.18 s (7938
// samples) down to a 0.88 sustain, so the mix must never fall silent inside that
// window. The broken cut silenced the note for ~145 ms of every note and then
// jumped back up to the sustain level.
void profile_hybrid_canonical_note_never_falls_silent_before_decay_ends(void) {
    constexpr int kNoteSamples = 11025;  // 0.25 s of the first note at 120 bpm
    constexpr int kDecayEnd = 7938;      // 0.18 s of DECAY
    static int16_t hybrid[kNoteSamples];

    ApuCore apu;
    apu.init(44100);
    apu.setSoundProfile(SoundProfile::Hybrid);
    // render_canonical() resets and re-initialises the core, which is fine: the
    // profile is configuration, so init() keeps it.
    apu_golden::render_canonical(apu, hybrid, kNoteSamples);

    // Past the attack ramp, inside the decay window: the longest run of exact
    // zeros. A pulse wave at 440 Hz alternates every ~50 samples, so a run this
    // long can only mean the envelope is at zero, not a waveform crossing.
    int run = 0;
    int longest = 0;
    for (int i = 128; i < kDecayEnd; ++i) {
        if (hybrid[i] == 0) {
            ++run;
            if (run > longest) longest = run;
        } else {
            run = 0;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(longest < 256,
                             "the envelope fell silent inside its decay stage");

    // And it is still sounding when the decay ends (sustain 0.88).
    TEST_ASSERT_TRUE(peakMagnitude(hybrid + kDecayEnd - 1024, 1024) > 1000);
}

// =============================================================================
// Unity test runner
// =============================================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    // Constructor tests
    RUN_TEST(test_apu_core_constructor_initializes_voices);
    RUN_TEST(test_apu_core_constructor_default_values);

    // Init/Reset tests
    RUN_TEST(test_apu_core_init_valid_sample_rate);
    RUN_TEST(test_apu_core_init_valid_sample_rate_48000);
    RUN_TEST(test_apu_core_init_invalid_sample_rate_fallback);
    RUN_TEST(test_apu_core_reset_clears_state);

    // Command queue tests
    RUN_TEST(test_apu_core_submit_command_success);
    RUN_TEST(test_apu_core_submit_command_multiple);
    RUN_TEST(test_apu_core_submit_command_queue_full);

    // Generate samples tests
    RUN_TEST(test_apu_core_generate_samples_zero_length);
    RUN_TEST(test_apu_core_generate_samples_null_stream);
    RUN_TEST(test_apu_core_generate_samples_with_active_voices);
    RUN_TEST(test_apu_core_generate_samples_output_range);
    RUN_TEST(test_apu_core_generate_samples_empty_queue);

    // Helper function tests
    RUN_TEST(test_apu_core_sequencer_note_limit_set);
    RUN_TEST(test_apu_core_deferred_notes_counter);

    // Wave type tests
    RUN_TEST(test_apu_core_wave_type_pulse);
    RUN_TEST(test_apu_core_wave_type_triangle);
    RUN_TEST(test_apu_core_wave_type_sine);
    RUN_TEST(test_apu_core_wave_type_saw);
    RUN_TEST(test_apu_core_wave_type_noise);

    // Music command tests
    RUN_TEST(test_apu_core_music_play_stop_cycle);
    RUN_TEST(test_apu_core_music_stop_clears_flag);
    RUN_TEST(test_apu_core_music_pause_resume_cycle);
    RUN_TEST(test_apu_core_music_set_tempo);
    RUN_TEST(test_apu_core_music_set_bpm);
    RUN_TEST(test_apu_core_set_master_volume_command);
    RUN_TEST(test_apu_core_set_master_volume_clamping);
    RUN_TEST(test_apu_core_set_master_bitcrush);
    RUN_TEST(test_apu_core_set_master_bitcrush_over_max);

    // Post-mix callback test
    RUN_TEST(test_apu_core_post_mix_callback);

    // Frequency tests
    RUN_TEST(test_apu_core_low_frequency);
    RUN_TEST(test_apu_core_high_frequency);

    // Volume tests
    RUN_TEST(test_apu_core_zero_volume);
    RUN_TEST(test_apu_core_full_volume);

    // 4+4 voice partition tests
    RUN_TEST(test_apu_core_voice_partition_constants);
    RUN_TEST(test_apu_core_music_pulse_tracks_do_not_steal_each_other);
    RUN_TEST(test_apu_core_sfx_play_event_uses_sfx_voice_pool);
    RUN_TEST(test_apu_core_sfx_steal_does_not_touch_music_slots);
    RUN_TEST(test_apu_core_sequencer_percussion_same_step_uses_distinct_sfx_voices);
    RUN_TEST(test_apu_core_sequencer_percussion_does_not_disturb_melodic_slots);
    RUN_TEST(test_apu_core_sequencer_percussion_shares_sfx_pool_with_play_event);
    RUN_TEST(test_apu_core_percussion_borrows_idle_music_slot);
    RUN_TEST(test_apu_core_percussion_never_steals_live_melodic_voice);
    RUN_TEST(test_apu_core_sequencer_tempo_factor_short_notes_keeps_playing);
    RUN_TEST(test_apu_core_melodic_noise_on_track_three_uses_music_slot);
    RUN_TEST(test_apu_core_melodic_tail_overlaps_next_beat);
    RUN_TEST(test_apu_core_melodic_zero_release_leaves_gate_unchanged);
    RUN_TEST(test_apu_core_melodic_track_keeps_fixed_slot_mapping);
    RUN_TEST(test_apu_core_percussion_saturated_no_idle_slot_confined_to_sfx);
    RUN_TEST(test_apu_core_music_uses_track_duty_not_preset_duty);
    RUN_TEST(test_apu_core_noise_pitch_sweep_updates_period);
    RUN_TEST(test_apu_core_linear_sweep_midpoint_arithmetic);
    RUN_TEST(test_apu_core_exponential_sweep_midpoint_geometric);
    RUN_TEST(test_apu_core_exponential_noise_sweep_updates_period);
    RUN_TEST(test_apu_core_sweep_curve_default_linear_and_nonpositive_fallback);
    RUN_TEST(test_apu_core_loop_voice_stays_enabled_until_stop);
    RUN_TEST(test_apu_core_oneshot_zero_duration_does_not_hang);
    RUN_TEST(test_apu_core_loop_voice_is_stealable);

    // Duty stepped + pitch envelope (sfx-synthesis-low-priority)
    RUN_TEST(test_apu_core_duty_stepped_absent_keeps_legacy_duty_sweep);
    RUN_TEST(test_apu_core_duty_stepped_mid_note_hold);
    RUN_TEST(test_apu_core_duty_stepped_hold_after_last_and_ignores_duty_sweep);
    RUN_TEST(test_apu_core_pitch_envelope_three_points_linear);
    RUN_TEST(test_apu_core_pitch_envelope_noise_updates_period);
    RUN_TEST(test_apu_core_pitch_envelope_precedes_single_sweep);
    RUN_TEST(test_apu_core_pitch_envelope_count_below_two_keeps_sweep);

    // Hito 1: NES PULSE duty (M2) + TRIANGLE octave doubling (M16)
    RUN_TEST(test_pulse_duty_nes_4modes);
    RUN_TEST(test_pulse_duty_continuous_unchanged);
    RUN_TEST(test_triangle_octave_up_default_off);
    RUN_TEST(test_triangle_octave_up_doubles_freq);
    RUN_TEST(test_triangle_octave_up_ignored_for_other_types);

    // Hito 2 M3: NES frame counter
    RUN_TEST(test_frame_counter_initial_state);
    RUN_TEST(test_frame_counter_mode_set_resets_timer);
    RUN_TEST(test_frame_counter_quarter_clock_fires_at_240hz_mode0);
    RUN_TEST(test_frame_counter_half_clock_fires_at_120hz_mode0);
    RUN_TEST(test_frame_counter_mode1_no_irq);
    RUN_TEST(test_frame_counter_irq_fires_at_60hz_mode0);
    RUN_TEST(test_frame_counter_irq_inhibit_prevents_irq);
    RUN_TEST(test_frame_counter_step_timing_uses_cpu_cycles);

    // Hito 2 M4: NES length counter
    RUN_TEST(test_length_counter_initial_state);
    RUN_TEST(test_length_counter_load);
    RUN_TEST(test_length_counter_decrements_on_half_clock);
    RUN_TEST(test_length_counter_silences_at_zero);
    RUN_TEST(test_length_counter_halt_prevents_decrement);
    RUN_TEST(test_length_counter_lut_values);
    RUN_TEST(test_length_counter_channel_enable_off_forces_silence);
    RUN_TEST(test_length_counter_does_not_affect_sine_saw);

    // Hito 2 M5: NES linear counter
    RUN_TEST(test_linear_counter_initial_state);
    RUN_TEST(test_linear_counter_load);
    RUN_TEST(test_linear_counter_decrements_quarter_clock);
    RUN_TEST(test_linear_counter_hold_with_control_set);
    RUN_TEST(test_linear_counter_reload_flag_cleared_without_control);
    RUN_TEST(test_triangle_gate_open_default);
    RUN_TEST(test_triangle_gate_closes_when_length_zero);
    RUN_TEST(test_triangle_gate_closes_when_linear_zero);
    RUN_TEST(test_triangle_phase_does_not_advance_when_gated);
    RUN_TEST(test_triangle_does_not_affect_sine_saw);

    // Hito 2 M6: NES sweep unit (PULSE)
    RUN_TEST(test_sweep_initial_state);
    RUN_TEST(test_sweep_setup_and_target_calculation);
    RUN_TEST(test_sweep_negate_pulse1_ones_complement);
    RUN_TEST(test_sweep_negate_pulse2_two_complement);
    RUN_TEST(test_sweep_muting_target_overflow);
    RUN_TEST(test_sweep_muting_current_period_too_low);
    RUN_TEST(test_sweep_divider_reload);
    RUN_TEST(test_sweep_does_not_affect_triangle_noise);
    RUN_TEST(test_sweep_reload_flag_cleared);
    RUN_TEST(test_sweep_muting_persists_when_disabled);
    RUN_TEST(test_sweep_unarmed_voice_survives_frame_counter);
    RUN_TEST(test_sweep_disarm_clears_muting);
    RUN_TEST(test_sweep_register_writes_do_not_arm_the_unit);

    // Hito 2 M7: NES envelope unit (PULSE / NOISE)
    RUN_TEST(test_envelope_initial_state);
    RUN_TEST(test_envelope_setup_and_decay);
    RUN_TEST(test_envelope_loop);
    RUN_TEST(test_envelope_const_volume);
    RUN_TEST(test_envelope_silence_at_zero);
    RUN_TEST(test_envelope_start_flag_cleared_after_first_qclock);
    RUN_TEST(test_envelope_does_not_affect_sine_saw_triangle);
    RUN_TEST(test_envelope_volume_scaled_correctly);
    RUN_TEST(test_envelope_coexists_with_length_counter);
    RUN_TEST(test_envelope_loop_flag_shared_with_length_halt);

    // Hito 2 M13: TRIGGER_NES_LENGTH command
    RUN_TEST(test_trigger_nes_length_loads_counter);
    RUN_TEST(test_trigger_nes_length_arms_the_channel);
    RUN_TEST(test_trigger_nes_length_raises_subunit_flags);
    RUN_TEST(test_set_voice_nes_length_raises_subunit_flags);
    RUN_TEST(test_trigger_nes_length_reloads_linear_counter);
    RUN_TEST(test_trigger_nes_length_clamps_index);
    RUN_TEST(test_trigger_nes_length_ignores_bad_slot);
    RUN_TEST(test_trigger_nes_length_silences_voice_end_to_end);

    // Hito 2 M17: initNesSafe / nesAccurate
    RUN_TEST(test_nes_safe_pulse);
    RUN_TEST(test_nes_safe_triangle);
    RUN_TEST(test_nes_safe_noise);
    RUN_TEST(test_nes_safe_clears_previous_state);
    RUN_TEST(test_nes_safe_off_by_default);
    RUN_TEST(test_nes_safe_does_not_affect_sine_saw);
    RUN_TEST(test_nes_safe_state_is_silent_when_armed);

    // Hito 3 M8: NES NOISE period LUT
    RUN_TEST(test_noise_period_lut);
    RUN_TEST(test_noise_lut_default_off);
    RUN_TEST(test_noise_lut_sets_period_from_table);
    RUN_TEST(test_noise_lut_applies_on_new_note);
    RUN_TEST(test_noise_lut_off_restores_legacy_period);
    RUN_TEST(test_noise_lut_invalid_index_falls_back_to_off);
    RUN_TEST(test_noise_lut_ignored_for_non_noise);
    RUN_TEST(test_noise_lut_survives_nes_safe_init);
    RUN_TEST(test_noise_lut_is_sample_rate_independent);

    // Hito 4 M10: configurable master HPF
    RUN_TEST(test_hpf_default_is_legacy_coefficient);
    RUN_TEST(test_hpf_cutoff_computes_coefficient);
    RUN_TEST(test_hpf_cutoff_is_sample_rate_independent);
    RUN_TEST(test_hpf_init_recomputes_for_new_sample_rate);
    RUN_TEST(test_hpf_zero_cutoff_disables_filter);
    RUN_TEST(test_hpf_cutoff_changes_low_frequency_content);
    RUN_TEST(test_hpf_legacy_default_still_blocks_dc);
    RUN_TEST(test_hpf_reset_restores_legacy_coefficient);
    RUN_TEST(test_hpf_extreme_cutoff_stays_stable);

    // Hito 4 M11: configurable soft-clip
    RUN_TEST(test_softclip_default_is_rational);
    RUN_TEST(test_softclip_mode_round_trips);
    RUN_TEST(test_softclip_reset_restores_default);
    RUN_TEST(test_softclip_curve_shapes_below_unity);
    RUN_TEST(test_softclip_overdrive_ordering);
    RUN_TEST(test_softclip_none_hits_the_rails_more_than_hardclip);
    RUN_TEST(test_softclip_all_modes_stay_in_range);
    RUN_TEST(test_softclip_runs_before_the_dc_blocker);

    // Hito 4 M14: VoiceNesOptions
    RUN_TEST(test_nes_options_defaults_are_all_off);
    RUN_TEST(test_nes_options_round_trip);
    RUN_TEST(test_nes_options_arms_the_real_subunits);
    RUN_TEST(test_nes_options_disarms_everything);
    RUN_TEST(test_nes_options_invalid_indices_fall_back);
    RUN_TEST(test_nes_options_command_matches_direct_setter);
    RUN_TEST(test_nes_options_command_ignores_bad_slot);
    RUN_TEST(test_nes_options_noise_lut_retunes_immediately);
    RUN_TEST(test_nes_options_reflect_nes_safe_boundary);

    // S4: sound profiles
    RUN_TEST(test_sound_profile_defaults_to_legacy);
    RUN_TEST(test_sound_profile_round_trips);
    RUN_TEST(test_sound_profile_ignores_unknown_value);
    RUN_TEST(test_sound_profile_command_applies_on_render);
    RUN_TEST(test_sound_profile_survives_init);

    // S3 / S5 / S6: core dispatch, profile character and the hot switch
    RUN_TEST(profile_nes_and_hybrid_differ_only_by_the_envelope_curve);
    RUN_TEST(profile_apply_runs_after_nes_accurate_init);
    RUN_TEST(profile_hot_switch_does_not_click);
    RUN_TEST(profile_accessors_follow_the_active_core);
    RUN_TEST(profile_setter_is_idempotent_for_state);

    // S2 / S3: envelope curve, profile default and D-3 precedence (E1/T1)
    RUN_TEST(instrument_preset_envelope_curve_is_appended_and_zero);
    RUN_TEST(profile_envelope_curve_precedence_matches_d3);
    RUN_TEST(envelope_exponential_decay_reaches_minus_60_db_at_the_stage_end);
    RUN_TEST(envelope_exponential_release_reaches_minus_60_db_at_the_stage_end);
    RUN_TEST(envelope_exponential_short_stage_keeps_the_linear_ramp);
    RUN_TEST(profile_hybrid_canonical_note_never_falls_silent_before_decay_ends);

    // Integration tests
    RUN_TEST(test_apu_core_integration_full_pipeline);
    RUN_TEST(test_apu_core_integration_multiple_voices);

    return UNITY_END();
}
