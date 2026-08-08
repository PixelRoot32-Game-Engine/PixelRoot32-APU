/// test_apu_mirror.cpp — FPU mirror unit tests (envelope, LFSR, timing, golden PCM)
///
/// Validates pixelroot32::audio::ApuCore against deterministic behavior
/// and a native-reference PCM fixture (Engine FPU path @ commit e24d41e).

#include "golden/apu_mirror_canonical_track.h"
#include "golden/apu_presence_canonical_fixture.h"
#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"
#include "unity.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pixelroot32::audio;

static std::string fixture_dir;

extern "C" void setUp(void)
{
    fixture_dir = std::filesystem::current_path().string();
    for (const auto& candidate : {
             fixture_dir + "/golden/fixtures",
             fixture_dir + "/tests/golden/fixtures",
             fixture_dir + "/toolsuite_app/tests/golden/fixtures",
             fixture_dir + "/../tests/golden/fixtures",
             fixture_dir + "/../../tests/golden/fixtures",
             fixture_dir + "/../../../tests/golden/fixtures",
         }) {
        if (std::filesystem::exists(candidate + "/apu_mirror_canonical_s16le_44100.pcm")) {
            fixture_dir = candidate;
            break;
        }
    }
}

extern "C" void tearDown(void) {}

static int16_t peak_abs(const int16_t* buf, int count)
{
    int peak = 0;
    for (int i = 0; i < count; ++i) {
        const int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) {
            peak = v;
        }
    }
    return static_cast<int16_t>(peak);
}

static uint16_t advance_lfsr(uint16_t state, bool short_mode)
{
    const uint16_t bit_to_xor = short_mode ? static_cast<uint16_t>((state >> 6) & 1u)
                                           : static_cast<uint16_t>((state >> 1) & 1u);
    const uint16_t fb = static_cast<uint16_t>(((state & 1u) ^ bit_to_xor) & 1u);
    return static_cast<uint16_t>((state >> 1) | (fb << 14));
}

static std::vector<uint8_t> read_fixture_bytes(const std::string& name)
{
    const std::string path = fixture_dir + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

static void compute_diff_stats(const int16_t* actual,
                               const int16_t* expected,
                               int count,
                               double& rmse_out,
                               int& max_abs_out)
{
    double sum_sq = 0.0;
    max_abs_out = 0;
    for (int i = 0; i < count; ++i) {
        const int diff = static_cast<int>(actual[i]) - static_cast<int>(expected[i]);
        const int abs_diff = diff < 0 ? -diff : diff;
        if (abs_diff > max_abs_out) {
            max_abs_out = abs_diff;
        }
        sum_sq += static_cast<double>(diff) * static_cast<double>(diff);
    }
    rmse_out = std::sqrt(sum_sq / static_cast<double>(count));
}

static void generate_samples(ApuCore& apu, uint64_t count)
{
    int16_t scratch[512] = {};
    uint64_t generated = 0;
    while (generated < count) {
        const int block =
            static_cast<int>(std::min<uint64_t>(512, count - generated));
        apu.generateSamples(scratch, block);
        generated += static_cast<uint64_t>(block);
    }
}

static bool stage_is_audible_gate(EnvelopeState::Stage stage)
{
    return stage == EnvelopeState::Stage::ATTACK ||
           stage == EnvelopeState::Stage::DECAY ||
           stage == EnvelopeState::Stage::SUSTAIN;
}

void test_apu_mirror_music_dual_pulse_tracks_use_distinct_voices(void)
{
    static const MusicNote lead_notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 4.0f),
    };
    static const MusicNote harmony_notes[] = {
        makeNote(INSTR_PULSE_HARMONY, Note::E, 5, 0.5f),
        makeRest(3.5f),
    };
    static const MusicTrack lead_track{
        lead_notes,
        sizeof(lead_notes) / sizeof(lead_notes[0]),
        true,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };
    static const MusicTrack harmony_track{
        harmony_notes,
        sizeof(harmony_notes) / sizeof(harmony_notes[0]),
        true,
        WaveType::PULSE,
        0.125f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &lead_track;
    play.subTrackCount = 1;
    play.subTracks[0] = &harmony_track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    generate_samples(apu, 512);

    TEST_ASSERT_EQUAL_INT(0, apu.getTrackVoiceSlotForTesting(0));
    TEST_ASSERT_EQUAL_INT(1, apu.getTrackVoiceSlotForTesting(1));

    TEST_ASSERT_TRUE_MESSAGE(
        stage_is_audible_gate(apu.getTrackVoiceStageForTesting(0)),
        "lead sustain gate must remain active while harmony attacks");
    TEST_ASSERT_TRUE_MESSAGE(
        stage_is_audible_gate(apu.getTrackVoiceStageForTesting(1)),
        "harmony note should be audible on its own voice");
}

void test_apu_mirror_music_harmony_rest_does_not_cut_lead_pulse(void)
{
    static const MusicNote lead_notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 4.0f),
    };
    static const MusicNote harmony_notes[] = {
        makeNote(INSTR_PULSE_HARMONY, Note::E, 5, 0.5f),
        makeRest(3.5f),
    };
    static const MusicTrack lead_track{
        lead_notes,
        sizeof(lead_notes) / sizeof(lead_notes[0]),
        true,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };
    static const MusicTrack harmony_track{
        harmony_notes,
        sizeof(harmony_notes) / sizeof(harmony_notes[0]),
        true,
        WaveType::PULSE,
        0.125f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &lead_track;
    play.subTrackCount = 1;
    play.subTracks[0] = &harmony_track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, 120.0f);
    const uint64_t half_beat_samples = 2 * tick_samples;
    generate_samples(apu, half_beat_samples + tick_samples);

    TEST_ASSERT_TRUE_MESSAGE(
        stage_is_audible_gate(apu.getTrackVoiceStageForTesting(0)),
        "lead long note must survive harmony rest");
    const EnvelopeState::Stage harmony_stage =
        apu.getTrackVoiceStageForTesting(1);
    TEST_ASSERT_TRUE_MESSAGE(
        harmony_stage == EnvelopeState::Stage::RELEASE ||
            harmony_stage == EnvelopeState::Stage::OFF,
        "harmony voice should release after its short note");
}

void test_apu_mirror_music_percussion_does_not_claim_track_voice(void)
{
    static const MusicNote lead_notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 1.0f),
    };
    static const MusicNote drum_notes[] = {
        makeNote(INSTR_KICK, Note::Rest, 1, 0.25f),
    };
    static const MusicTrack lead_track{
        lead_notes,
        sizeof(lead_notes) / sizeof(lead_notes[0]),
        true,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };
    static const MusicTrack drum_track{
        drum_notes,
        sizeof(drum_notes) / sizeof(drum_notes[0]),
        true,
        WaveType::NOISE,
        0.0f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &lead_track;
    play.subTrackCount = 1;
    play.subTracks[0] = &drum_track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    generate_samples(apu, 512);

    TEST_ASSERT_TRUE_MESSAGE(apu.getTrackVoiceSlotForTesting(0) >= 0,
                             "melodic lead should own a track voice slot");
    TEST_ASSERT_EQUAL_INT(-1, apu.getTrackVoiceSlotForTesting(1));
    TEST_ASSERT_TRUE_MESSAGE(
        stage_is_audible_gate(apu.getTrackVoiceStageForTesting(0)),
        "lead should remain audible when percussion fires");
}

void test_apu_mirror_music_update_tracks_clears_voice_ownership(void)
{
    static const MusicNote lead_notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 4.0f),
    };
    static const MusicNote harmony_notes[] = {
        makeNote(INSTR_PULSE_HARMONY, Note::E, 5, 0.5f),
        makeRest(3.5f),
    };
    static const MusicTrack lead_track{
        lead_notes,
        sizeof(lead_notes) / sizeof(lead_notes[0]),
        true,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };
    static const MusicTrack harmony_track{
        harmony_notes,
        sizeof(harmony_notes) / sizeof(harmony_notes[0]),
        true,
        WaveType::PULSE,
        0.125f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &lead_track;
    play.subTrackCount = 1;
    play.subTracks[0] = &harmony_track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    generate_samples(apu, 512);
    TEST_ASSERT_TRUE(apu.getTrackVoiceSlotForTesting(0) >= 0);

    AudioCommand update{};
    update.type = AudioCommandType::MUSIC_UPDATE_TRACKS;
    update.track = &lead_track;
    update.subTrackCount = 1;
    update.subTracks[0] = &harmony_track;
    TEST_ASSERT_TRUE(apu.submitCommand(update));

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, 120.0f);
    generate_samples(apu, tick_samples + 512);

    TEST_ASSERT_NOT_EQUAL(apu.getTrackVoiceSlotForTesting(0),
                          apu.getTrackVoiceSlotForTesting(1));
    TEST_ASSERT_TRUE_MESSAGE(
        stage_is_audible_gate(apu.getTrackVoiceStageForTesting(0)),
        "lead must stay audible after arrangement refresh");
}

void test_apu_mirror_envelope_attack_rise(void)
{
    static constexpr InstrumentPreset kPreset{
        1.0f,
        0.5f,
        4,
        0.0f,
        0,
        0.05f,
        0.0f,
        1.0f,
        0.01f,
        LfoTarget::NONE,
        0.0f,
        0.0f,
        0.0f,
        false,
        false,
        0.0f,
    };

    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.2f;
    cmd.event.volume = 1.0f;
    cmd.event.duty = 0.5f;
    cmd.event.preset = &kPreset;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t early[256] = {};
    apu.generateSamples(early, 256);
    const int16_t early_peak = peak_abs(early, 256);

    int16_t late[256] = {};
    for (int block = 0; block < 8; ++block) {
        apu.generateSamples(late, 256);
    }
    const int16_t late_peak = peak_abs(late, 256);

    TEST_ASSERT_TRUE_MESSAGE(early_peak > 0, "pulse should produce audio during attack");
    TEST_ASSERT_TRUE_MESSAGE(late_peak > early_peak,
                             "envelope attack should increase output amplitude over time");
}

void test_apu_mirror_lfsr_15bit_sequence(void)
{
    // Hito 1 M9: the APU seeds the 15-bit LFSR to 0x7FFF (all-1s) on every
    // voice trigger, matching the NES APU state after a write to $400F.
    uint16_t state = 0x7FFFu;
    uint16_t expected[32] = {};
    for (int i = 0; i < 32; ++i) {
        expected[i] = static_cast<uint16_t>(state & 1u);
        state = advance_lfsr(state, false);
    }

    ApuCore apu;
    apu.init(44100);

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 1000.0f;
    cmd.event.duration = 0.05f;
    cmd.event.volume = 1.0f;
    cmd.event.noisePeriod = 1;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[512] = {};
    apu.generateSamples(buffer, 512);

    int matches = 0;
    for (int i = 0; i < 32 && i < 512; ++i) {
        const uint16_t bit = buffer[i] >= 0 ? 1u : 0u;
        if (bit == expected[i]) {
            ++matches;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(matches >= 28,
                             "noise output should follow 15-bit LFSR tap (normal mode)");
}

void test_apu_mirror_lfsr_short_mode_differs(void)
{
    uint16_t normal = 0x4000u;
    uint16_t short_mode = 0x4000u;
    bool differs = false;
    for (int i = 0; i < 64; ++i) {
        const uint16_t normal_bit = static_cast<uint16_t>(normal & 1u);
        const uint16_t short_bit = static_cast<uint16_t>(short_mode & 1u);
        normal = advance_lfsr(normal, false);
        short_mode = advance_lfsr(short_mode, true);
        if (normal_bit != short_bit) {
            differs = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(differs, "noiseLfsrShort tap must diverge from normal LFSR");

    // Hito 0 (M15) rename coverage: an AudioChannel must expose both the new
    // canonical field `noiseLfsrShort` and the deprecated alias
    // `noiseShortMode`. After `reset()` both default to false (symmetric
    // reset). Writing the new field reads back true, and the alias is read
    // independently at the struct level (sync happens in
    // ApuCore::initVoiceFromEvent, not in the struct).
    AudioChannel ch{};
    ch.reset();
    TEST_ASSERT_FALSE(ch.noiseLfsrShort);
    TEST_ASSERT_FALSE(ch.noiseShortMode);

    ch.noiseLfsrShort = true;
    TEST_ASSERT_TRUE(ch.noiseLfsrShort);
    // After a full init via ApuCore, both fields would be in sync; the
    // struct itself does not auto-propagate, by design.
    TEST_ASSERT_FALSE(ch.noiseShortMode);
}

void test_apu_mirror_music_gate_four_beats_at_120_bpm(void)
{
    static const MusicNote notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 4.0f),
    };
    static const MusicTrack track{
        notes,
        sizeof(notes) / sizeof(notes[0]),
        false,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, 120.0f);
    const uint64_t gate_samples = 16 * tick_samples;
    // Fix B (WU-B): every melodic note-on now extends remainingSamples by the
    // full release tail (not just legato retriggers), so the gate no longer
    // expires at `gate_samples` — it expires at `gate_samples + release_samples`.
    const uint32_t release_samples = std::min<uint32_t>(
        static_cast<uint32_t>(INSTR_PULSE_LEAD.releaseTime
                              * static_cast<float>(apu_golden::kSampleRate)),
        static_cast<uint32_t>(apu_golden::kSampleRate / 10));

    int16_t scratch[512] = {};
    auto generate_samples = [&](ApuCore& core, uint64_t count) {
        uint64_t generated = 0;
        while (generated < count) {
            const int block =
                static_cast<int>(std::min<uint64_t>(512, count - generated));
            core.generateSamples(scratch, block);
            generated += static_cast<uint64_t>(block);
        }
    };

    generate_samples(apu, gate_samples / 2);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(EnvelopeState::Stage::SUSTAIN),
                          static_cast<int>(apu.getActiveVoiceStageForTesting(
                              WaveType::PULSE)));

    generate_samples(apu, (gate_samples - gate_samples / 2) + release_samples);
    const EnvelopeState::Stage stage =
        apu.getActiveVoiceStageForTesting(WaveType::PULSE);
    TEST_ASSERT_TRUE_MESSAGE(
        stage == EnvelopeState::Stage::RELEASE ||
            stage == EnvelopeState::Stage::OFF,
        "four-beat gate should expire near 2s plus the release tail at 120 BPM");
}

void test_apu_mirror_music_silent_subtrack_rest_does_not_cut_main_pulse(void)
{
    static const MusicNote lead_notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 1.0f),
        makeRest(15.0f),
    };
    static const MusicNote silent_pulse2[] = {
        makeRest(16.0f),
    };
    static const MusicTrack lead_track{
        lead_notes,
        sizeof(lead_notes) / sizeof(lead_notes[0]),
        true,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };
    static const MusicTrack pulse2_track{
        silent_pulse2,
        sizeof(silent_pulse2) / sizeof(silent_pulse2[0]),
        true,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &lead_track;
    play.subTrackCount = 1;
    play.subTracks[0] = &pulse2_track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    int16_t scratch[512] = {};
    apu.generateSamples(scratch, 512);

    const EnvelopeState::Stage stage =
        apu.getActiveVoiceStageForTesting(WaveType::PULSE);
    TEST_ASSERT_TRUE_MESSAGE(
        stage == EnvelopeState::Stage::ATTACK ||
            stage == EnvelopeState::Stage::DECAY ||
            stage == EnvelopeState::Stage::SUSTAIN,
        "lead note at step 0 must not be cut by silent pulse2 rest");
    TEST_ASSERT_TRUE_MESSAGE(peak_abs(scratch, 512) > 0,
                             "lead note at step 0 should produce audio");
}

void test_apu_mirror_music_rest_cuts_sustain(void)
{
    static const MusicNote notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 1.0f),
        makeRest(3.0f),
    };
    static const MusicTrack track{
        notes,
        sizeof(notes) / sizeof(notes[0]),
        false,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, 120.0f);
    const uint64_t one_beat_samples = 4 * tick_samples;

    int16_t scratch[512] = {};
    uint64_t generated = 0;
    while (generated < one_beat_samples) {
        const int block =
            static_cast<int>(std::min<uint64_t>(512, one_beat_samples - generated));
        apu.generateSamples(scratch, block);
        generated += static_cast<uint64_t>(block);
    }
    apu.generateSamples(scratch, 512);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(EnvelopeState::Stage::RELEASE),
                          static_cast<int>(
                              apu.getActiveVoiceStageForTesting(WaveType::PULSE)));
}

void test_apu_mirror_sequencer_tick_timing(void)
{
    static const MusicNote notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 0.25f),
        makeNote(INSTR_PULSE_LEAD, Note::D, 4, 0.25f),
    };
    static const MusicTrack track{
        notes,
        sizeof(notes) / sizeof(notes[0]),
        false,
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = apu_golden::kBpm;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &track;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    TEST_ASSERT_EQUAL_size_t(0, apu.getSequencerMainNoteIndexForTesting());

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, apu_golden::kBpm);
    TEST_ASSERT_TRUE(tick_samples > 0);

    int16_t scratch[512] = {};
    uint64_t generated = 0;
    while (generated < tick_samples) {
        const int block = static_cast<int>(std::min<uint64_t>(512, tick_samples - generated));
        apu.generateSamples(scratch, block);
        generated += static_cast<uint64_t>(block);
    }

    TEST_ASSERT_EQUAL_size_t(1, apu.getSequencerMainNoteIndexForTesting());
}

void test_apu_mirror_voice_partition_constants(void)
{
    TEST_ASSERT_EQUAL_INT(8, ApuCore::MAX_VOICES);
    TEST_ASSERT_EQUAL_INT(0, ApuCore::MUSIC_VOICE_BASE);
    TEST_ASSERT_EQUAL_INT(4, ApuCore::MUSIC_VOICE_COUNT);
    TEST_ASSERT_EQUAL_INT(4, ApuCore::SFX_VOICE_BASE);
    TEST_ASSERT_EQUAL_INT(4, ApuCore::SFX_VOICE_COUNT);
}

static int count_enabled_sfx_voices(const ApuCore& apu)
{
    int count = 0;
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        if (apu.isVoiceEnabledForTesting(slot)) {
            ++count;
        }
    }
    return count;
}

void test_apu_mirror_sequencer_percussion_same_step_uses_distinct_sfx_voices(void)
{
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

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kDrumTrack;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    int16_t scratch[512] = {};
    apu.generateSamples(scratch, 256);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(2, count_enabled_sfx_voices(apu));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_TRUE(peak_abs(scratch, 256) > 0);
}

void test_apu_mirror_sequencer_percussion_does_not_disturb_melodic_slots(void)
{
    static const MusicNote kLeadNotes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 4.0f),
    };
    static const MusicNote kBassNotes[] = {
        makeNote(INSTR_TRIANGLE_BASS, Note::C, 3, 4.0f),
    };
    static const MusicNote kHarmonyNotes[] = {
        makeNote(INSTR_PULSE_HARMONY, Note::E, 5, 4.0f),
    };
    static const MusicNote kDrumHit[] = {
        makeNote(INSTR_KICK, Note::Rest, 1.0f),
    };

    static const MusicTrack kLeadTrack{
        kLeadNotes, 1, false, WaveType::PULSE, 0.5f, nullptr, nullptr, nullptr};
    static const MusicTrack kBassTrack{
        kBassNotes, 1, false, WaveType::TRIANGLE, 0.5f, nullptr, nullptr, nullptr};
    static const MusicTrack kHarmonyTrack{
        kHarmonyNotes, 1, false, WaveType::PULSE, 0.125f, nullptr, nullptr, nullptr};
    static const MusicTrack kDrumTrack{
        kDrumHit, 1, false, WaveType::NOISE, 0.0f, nullptr, nullptr, nullptr};

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kLeadTrack;
    play.subTrackCount = 3;
    play.subTracks[0] = &kBassTrack;
    play.subTracks[1] = &kHarmonyTrack;
    play.subTracks[2] = &kDrumTrack;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    int16_t scratch[512] = {};
    apu.generateSamples(scratch, 256);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(1));
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(2));
    TEST_ASSERT_FALSE(apu.isVoiceEnabledForTesting(3));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, count_enabled_sfx_voices(apu));
    TEST_ASSERT_FALSE(apu.isMusicTrackVoiceActiveForTesting(3));
}

void test_apu_mirror_instrument_preview_uses_sfx_subpool(void)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    static const MusicNote kLongNote[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 16.0f),
    };
    static const MusicTrack kTrack{kLongNote, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    int16_t scratch[512] = {};
    apu.generateSamples(scratch, 256);
    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));

    AudioCommand preview{};
    preview.type = AudioCommandType::PLAY_EVENT;
    preview.event.type = WaveType::PULSE;
    preview.event.frequency = 880.0f;
    preview.event.duration = 0.5f;
    preview.event.volume = 0.5f;
    preview.event.duty = 0.5f;
    TEST_ASSERT_TRUE(apu.submitCommand(preview));
    apu.generateSamples(scratch, 256);

    TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(0));
    bool preview_in_sfx_pool = false;
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        if (apu.isVoiceEnabledForTesting(slot)) {
            preview_in_sfx_pool = true;
        }
    }
    TEST_ASSERT_TRUE(preview_in_sfx_pool);
}

void test_apu_mirror_partition_confinement_guard(void)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    // Oversubscribe the SFX pool (6 events into 4 slots) while all music
    // tracks stay idle, so a steal within 4-7 is guaranteed to occur.
    AudioCommand preview{};
    preview.type = AudioCommandType::PLAY_EVENT;
    preview.event.type = WaveType::PULSE;
    preview.event.duration = 2.0f;
    preview.event.volume = 0.5f;
    preview.event.duty = 0.5f;
    for (int i = 0; i < 6; ++i) {
        preview.event.frequency = 300.0f + static_cast<float>(i) * 25.0f;
        TEST_ASSERT_TRUE(apu.submitCommand(preview));
    }

    int16_t buffer[256] = {};
    apu.generateSamples(buffer, 256);

    for (int slot = ApuCore::MUSIC_VOICE_BASE; slot < ApuCore::SFX_VOICE_BASE; ++slot) {
        TEST_ASSERT_FALSE_MESSAGE(
            apu.isVoiceEnabledForTesting(slot),
            "executePlayEvent/findVoiceForSfxEvent must stay confined to slots 4-7, "
            "even with an oversubscribed SFX pool and fully idle music slots");
    }
    for (int slot = ApuCore::SFX_VOICE_BASE; slot < ApuCore::MAX_VOICES; ++slot) {
        TEST_ASSERT_TRUE(apu.isVoiceEnabledForTesting(slot));
    }
}

void test_apu_mirror_percussion_borrows_idle_music_slot(void)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    static const MusicNote kLeadNotes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 4.0f),
    };
    static const MusicNote kBassNotes[] = {
        makeNote(INSTR_TRIANGLE_BASS, Note::C, 3, 4.0f),
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
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    // Saturate the SFX/percussion subpool (slots 4-7) BEFORE the drum hit
    // fires, so the fallback allocator finds no free voice in its own pool.
    AudioCommand sfx{};
    sfx.type = AudioCommandType::PLAY_EVENT;
    sfx.event.type = WaveType::PULSE;
    sfx.event.duration = 2.0f;
    sfx.event.volume = 0.5f;
    sfx.event.duty = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sfx.event.frequency = 500.0f + static_cast<float>(i) * 40.0f;
        TEST_ASSERT_TRUE(apu.submitCommand(sfx));
    }

    int16_t buffer[256] = {};
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
    TEST_ASSERT_TRUE_MESSAGE(
        apu.isVoiceEnabledForTesting(2),
        "percussion must borrow the idle music slot when the SFX pool is saturated");
    TEST_ASSERT_FALSE_MESSAGE(
        apu.isMusicTrackVoiceActiveForTesting(2),
        "borrowed slot must not be marked as a live melodic gate");
}

void test_apu_mirror_percussion_never_steals_live_melodic_voice(void)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    static const MusicNote kDrumNotes[] = {
        makeRest(1.0f),                          // silent tick, advances the sequencer
        makeNote(INSTR_KICK, Note::Rest, 1.0f),  // actual percussion hit, next tick
    };
    static const MusicNote kMelodyNotes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 16.0f),  // long note, still gated on the drum tick
    };
    static const MusicTrack kDrumTrack{kDrumNotes, 2, false, WaveType::NOISE, 0.5f};
    static const MusicTrack kMelodyTrack{kMelodyNotes, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kDrumTrack;            // trackIdx 0 (percussion never claims voice slot 0)
    play.subTrackCount = 1;
    play.subTracks[0] = &kMelodyTrack;   // trackIdx 1 -> voice slot 1
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    int16_t buffer[256] = {};
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
        sfx.event.frequency = 600.0f + static_cast<float>(i) * 40.0f;
        TEST_ASSERT_TRUE(apu.submitCommand(sfx));
    }

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, ApuCore::DEFAULT_BPM);
    generate_samples(apu, tick_samples * 5);

    // The drum's second note has now fired a real percussion hit while
    // slot 1's melodic gate was still live; slot 1 must remain untouched.
    TEST_ASSERT_TRUE_MESSAGE(
        apu.isMusicTrackVoiceActiveForTesting(1),
        "percussion steal fallback must never take over a live melodic track voice");
    TEST_ASSERT_TRUE_MESSAGE(
        apu.isVoiceEnabledForTesting(1),
        "live melodic voice must remain enabled after a percussion steal fallback");
}

// Fix B (WU-B): melodic note-on must extend remainingSamples by the full
// env.releaseSamples regardless of legato, so the release tail overlaps
// the next beat instead of being hard-cut at the gate boundary.
//
// There is no test-only accessor for Voice::remainingSamples, so this
// drives through the real sequencer path and asserts the *observable*
// timing consequence instead: at exactly `gate_samples + release_samples`
// generated samples,
//   - pre-fix:  remainingSamples started at gate_samples only, so the
//     per-sample auto-release trigger (generateSampleForVoice) already
//     armed and fully exhausted one release cycle by this point -> voice
//     is disabled.
//   - post-fix: remainingSamples started at gate_samples + release_samples,
//     so the auto-release trigger only just now arms the release stage ->
//     voice is still enabled (tail keeps sounding into the next beat).
void test_apu_mirror_melodic_tail_overlaps_next_beat(void)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    // Fresh voice, single note-on -> ch->enabled starts false, so the
    // internal legato check collapses to false: this exercises the exact
    // "not legato" case the pre-fix gate guarded against.
    static const MusicNote kNote[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 1.0f),
    };
    static const MusicTrack kTrack{kNote, 1, false, WaveType::PULSE, 0.5f};

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, 120.0f);
    const uint64_t gate_samples =
        static_cast<uint64_t>(ApuCore::TICKS_PER_BEAT) * tick_samples;
    const uint32_t release_samples = std::min<uint32_t>(
        static_cast<uint32_t>(INSTR_PULSE_LEAD.releaseTime
                              * static_cast<float>(apu_golden::kSampleRate)),
        static_cast<uint32_t>(apu_golden::kSampleRate / 10));

    generate_samples(apu, gate_samples + release_samples);

    TEST_ASSERT_TRUE_MESSAGE(
        apu.isVoiceEnabledForTesting(0),
        "melodic note-on must extend the gate by the full release tail, "
        "independent of legato, so the note overlaps the next beat");
}

// Fix B counterpart: a preset with releaseTime == 0 must leave the gate
// unchanged (no tail extension) in both pre- and post-fix code, since the
// `env.releaseSamples > 0` guard rejects the addition either way.
void test_apu_mirror_melodic_zero_release_unchanged(void)
{
    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    static constexpr InstrumentPreset kNoReleasePreset{
        0.30f,    // baseVolume
        0.25f,    // duty
        4,        // defaultOctave
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
        0.0f,     // dutySweep
    };
    static const MusicNote kNote[] = {
        makeNote(kNoReleasePreset, Note::C, 4, 1.0f),
    };
    static const MusicTrack kTrack{kNote, 1, false, WaveType::PULSE, 0.25f};

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &kTrack;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    const uint64_t tick_samples =
        apu_golden::tick_duration_samples(apu_golden::kSampleRate, 120.0f);
    const uint64_t gate_samples =
        static_cast<uint64_t>(ApuCore::TICKS_PER_BEAT) * tick_samples;

    generate_samples(apu, gate_samples);

    TEST_ASSERT_FALSE_MESSAGE(
        apu.isVoiceEnabledForTesting(0),
        "zero release time must leave the gate boundary unchanged");
}

void test_apu_mirror_golden_pcm_parity(void)
{
    const auto bytes = read_fixture_bytes("apu_mirror_canonical_s16le_44100.pcm");
    TEST_ASSERT_TRUE_MESSAGE(!bytes.empty(), "missing golden PCM fixture");
    TEST_ASSERT_EQUAL_INT(apu_golden::kGoldenSampleCount * static_cast<int>(sizeof(int16_t)),
                          static_cast<int>(bytes.size()));

    const auto* expected = reinterpret_cast<const int16_t*>(bytes.data());

    int16_t actual[apu_golden::kGoldenSampleCount] = {};
    ApuCore apu;
    apu_golden::render_canonical(apu, actual, apu_golden::kGoldenSampleCount);

    double rmse = 0.0;
    int max_abs = 0;
    compute_diff_stats(actual, expected, apu_golden::kGoldenSampleCount, rmse, max_abs);

    TEST_ASSERT_TRUE_MESSAGE(max_abs <= 2,
                             "golden PCM max abs diff too high vs native reference");
    TEST_ASSERT_TRUE_MESSAGE(rmse < 1.0,
                             "golden PCM RMSE too high vs native reference");
}

void test_apu_mirror_ticks_per_beat_constant(void)
{
    TEST_ASSERT_EQUAL_INT(4, ApuCore::TICKS_PER_BEAT);
}

void test_apu_mirror_preview_note_reference_frequency(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 261.63f, noteToFrequency(Note::C, 4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 130.815f, noteToFrequency(Note::C, 3));
}

void test_apu_presence_helpers_measure_solo_peak_and_rms(void)
{
    const apu_presence::Metrics metrics =
        apu_presence::measure_solo_pulse(apu_presence::kCandidateTrackVolume);
    TEST_ASSERT_TRUE_MESSAGE(metrics.peakAbs > 0, "solo peak must be non-zero");
    TEST_ASSERT_TRUE_MESSAGE(metrics.sustainRms > 0.0,
                             "solo sustain RMS must be non-zero");
    TEST_ASSERT_EQUAL_INT(1, metrics.activeVoiceCount);
}

void test_apu_presence_legacy_volume_baseline_is_recorded(void)
{
    const apu_presence::Metrics solo =
        apu_presence::measure_solo_pulse(apu_presence::kLegacyTrackVolume);
    const apu_presence::Metrics two =
        apu_presence::measure_two_pulse(apu_presence::kLegacyTrackVolume);
    const apu_presence::Metrics four =
        apu_presence::measure_four_melodic(apu_presence::kLegacyTrackVolume);

    std::printf(
        "[apu-presence baseline vol=0.5] solo peak=%d rms=%.1f voices=%d | "
        "two peak=%d voices=%d | four peak=%d voices=%d\n",
        static_cast<int>(solo.peakAbs), solo.sustainRms, solo.activeVoiceCount,
        static_cast<int>(two.peakAbs), two.activeVoiceCount,
        static_cast<int>(four.peakAbs), four.activeVoiceCount);

    TEST_ASSERT_EQUAL_INT(1, solo.activeVoiceCount);
    TEST_ASSERT_EQUAL_INT(2, two.activeVoiceCount);
    TEST_ASSERT_EQUAL_INT(4, four.activeVoiceCount);
    TEST_ASSERT_TRUE(two.peakAbs < apu_presence::kMaxHeadroomPeakS16);
    TEST_ASSERT_TRUE(four.peakAbs < apu_presence::kMaxHeadroomPeakS16);
}

void test_apu_presence_candidate_volume_evaluation_and_adoption_gate(void)
{
    const apu_presence::Metrics baseline_solo =
        apu_presence::measure_solo_pulse(apu_presence::kLegacyTrackVolume);
    const apu_presence::Metrics solo =
        apu_presence::measure_solo_pulse(apu_presence::kCandidateTrackVolume);
    const apu_presence::Metrics two =
        apu_presence::measure_two_pulse(apu_presence::kCandidateTrackVolume);
    const apu_presence::Metrics four =
        apu_presence::measure_four_melodic(apu_presence::kCandidateTrackVolume);

    std::printf(
        "[apu-presence candidate vol=1.0] solo peak=%d rms=%.1f voices=%d | "
        "two peak=%d voices=%d | four peak=%d voices=%d | adopt=%d\n",
        static_cast<int>(solo.peakAbs), solo.sustainRms, solo.activeVoiceCount,
        static_cast<int>(two.peakAbs), two.activeVoiceCount,
        static_cast<int>(four.peakAbs), four.activeVoiceCount,
        apu_presence::meets_presence_headroom_gates(solo, two, four) ? 1 : 0);

    // Candidate improves presence and preserves headroom/voice activity.
    TEST_ASSERT_TRUE(solo.peakAbs > baseline_solo.peakAbs);
    TEST_ASSERT_EQUAL_INT(1, solo.activeVoiceCount);
    TEST_ASSERT_EQUAL_INT(2, two.activeVoiceCount);
    TEST_ASSERT_EQUAL_INT(4, four.activeVoiceCount);
    TEST_ASSERT_TRUE(two.peakAbs < apu_presence::kMaxHeadroomPeakS16);
    TEST_ASSERT_TRUE(four.peakAbs < apu_presence::kMaxHeadroomPeakS16);

    // design.md: adopt when solo peak >= 4000 AND 2/4 headroom holds.
    // After ADSR Core fullness (Engine → mirror), candidate clears the gate
    // (~5243 solo / headroom OK) → new ProjectTrack.volume default is 1.0.
    TEST_ASSERT_TRUE_MESSAGE(
        apu_presence::meets_presence_headroom_gates(solo, two, four),
        "candidate vol=1.0 must be adopted when presence+headroom gates pass");
    TEST_ASSERT_TRUE(solo.peakAbs >= apu_presence::kMinSoloPeakS16);
}

void test_apu_mirror_noise_pitch_sweep_updates_period(void)
{
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

    for (int n = 0; n < 20; ++n) {
        apu.generateSamples(buffer, 256);
    }
    const uint32_t periodEnd = apu.getVoiceNoisePeriodForTesting(sfxSlot);
    TEST_ASSERT_EQUAL_UINT32(endPeriod, periodEnd);
}

static int find_first_enabled_sfx_slot(ApuCore& apu)
{
    for (int i = ApuCore::SFX_VOICE_BASE; i < ApuCore::MAX_VOICES; ++i) {
        if (apu.isVoiceEnabledForTesting(i)) {
            return i;
        }
    }
    return -1;
}

void test_apu_mirror_linear_sweep_midpoint_arithmetic(void)
{
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
    cmd.event.sweepCurve = SweepCurve::Linear;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    for (int n = 0; n < 9; ++n) {
        apu.generateSamples(buffer, 256);
    }
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(40.0f, 660.0f, apu.getVoiceFrequencyForTesting(slot));
}

void test_apu_mirror_exponential_sweep_midpoint_geometric(void)
{
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
    TEST_ASSERT_FLOAT_WITHIN(35.0f, 622.25f, hz);
    TEST_ASSERT_TRUE(std::fabs(hz - 660.0f) > 15.0f);
}

void test_apu_mirror_exponential_noise_sweep_updates_period(void)
{
    ApuCore apu;
    apu.init(44100);
    apu.reset();

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::NOISE;
    cmd.event.frequency = 2000.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.5f;
    cmd.event.duty = 0.5f;
    cmd.event.noisePeriod = 0;
    cmd.event.sweepEndHz = 200.0f;
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

void test_apu_mirror_loop_voice_stays_enabled_until_stop(void)
{
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

void test_apu_mirror_oneshot_zero_duration_does_not_hang(void)
{
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

void test_apu_mirror_duty_stepped_mid_note_hold(void)
{
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
    cmd.event.duty = 0.75f;
    cmd.event.dutySteps = kSteps;
    cmd.event.dutyStepCount = 2;
    TEST_ASSERT_TRUE(apu.submitCommand(cmd));

    int16_t buffer[256] = {0};
    apu.generateSamples(buffer, 1);
    const int slot = find_first_enabled_sfx_slot(apu);
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.125f, apu.getVoiceDutyCycleForTesting(slot));

    int remaining = 2200 - 1;
    while (remaining > 0) {
        const int n = remaining > 256 ? 256 : remaining;
        apu.generateSamples(buffer, n);
        remaining -= n;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.125f, apu.getVoiceDutyCycleForTesting(slot));

    apu.generateSamples(buffer, 16);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, apu.getVoiceDutyCycleForTesting(slot));
}

void test_apu_mirror_pitch_envelope_three_points_linear(void)
{
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
    cmd.event.frequency = 100.0f;
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

    int remaining = 2205 - 1;
    while (remaining > 0) {
        const int n = remaining > 256 ? 256 : remaining;
        apu.generateSamples(buffer, n);
        remaining -= n;
    }
    TEST_ASSERT_FLOAT_WITHIN(8.0f, 591.25f, apu.getVoiceFrequencyForTesting(slot));

    // Reach past last breakpoint (0.2s = 8820 samples): +6615 from age≈2205.
    remaining = 6615;
    while (remaining > 0) {
        const int n = remaining > 256 ? 256 : remaining;
        apu.generateSamples(buffer, n);
        remaining -= n;
    }
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 783.99f, apu.getVoiceFrequencyForTesting(slot));
}

void test_apu_mirror_pitch_envelope_precedes_single_sweep(void)
{
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
    cmd.event.sweepEndHz = 100.0f;
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
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 880.0f, apu.getVoiceFrequencyForTesting(slot));
}

void test_apu_mirror_loop_voice_is_stealable(void)
{
    ApuCore apu;
    apu.init(44100);
    apu.reset();

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

void test_apu_mirror_music_seek_repositions_elapsed_ticks(void)
{
    // Long note so the sequencer stays mid-piece; seek jumps elapsed without backlog.
    static const MusicNote notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::C, 4, 16.0f),
    };
    static const MusicTrack track{
        notes,
        sizeof(notes) / sizeof(notes[0]),
        true,  // loop so the piece stays "playing" for seek assertions
        WaveType::PULSE,
        0.5f,
        nullptr,
        nullptr,
        nullptr,
    };

    ApuCore apu;
    apu.init(apu_golden::kSampleRate);
    apu.reset();

    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = 120.0f;
    TEST_ASSERT_TRUE(apu.submitCommand(bpm));

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &track;
    play.subTrackCount = 0;
    TEST_ASSERT_TRUE(apu.submitCommand(play));

    generate_samples(apu, 2048);
    TEST_ASSERT_TRUE(apu.isMusicPlaying());

    // Seek to 8 elapsed ticks from piece start.
    constexpr uint64_t kSeekTicks = 8;
    AudioCommand seek{};
    seek.type = AudioCommandType::MUSIC_SEEK;
    seek.seekOffsetTicks = kSeekTicks;
    TEST_ASSERT_TRUE(apu.submitCommand(seek));
    generate_samples(apu, 16);

    const uint64_t start_after = apu.getMusicPlayStartTick();
    const uint64_t global_after = apu.getMusicGlobalTick();
    TEST_ASSERT_TRUE(global_after >= start_after);
    const uint64_t elapsed = global_after - start_after;
    // Allow a small tick window for the generateSamples after seek.
    TEST_ASSERT_TRUE(elapsed >= kSeekTicks);
    TEST_ASSERT_TRUE(elapsed < kSeekTicks + 32);
    TEST_ASSERT_TRUE(apu.isMusicPlaying());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apu_mirror_ticks_per_beat_constant);
    RUN_TEST(test_apu_mirror_preview_note_reference_frequency);
    RUN_TEST(test_apu_mirror_envelope_attack_rise);
    RUN_TEST(test_apu_mirror_lfsr_15bit_sequence);
    RUN_TEST(test_apu_mirror_lfsr_short_mode_differs);
    RUN_TEST(test_apu_mirror_music_gate_four_beats_at_120_bpm);
    RUN_TEST(test_apu_mirror_music_silent_subtrack_rest_does_not_cut_main_pulse);
    RUN_TEST(test_apu_mirror_music_dual_pulse_tracks_use_distinct_voices);
    RUN_TEST(test_apu_mirror_music_harmony_rest_does_not_cut_lead_pulse);
    RUN_TEST(test_apu_mirror_music_percussion_does_not_claim_track_voice);
    RUN_TEST(test_apu_mirror_music_update_tracks_clears_voice_ownership);
    RUN_TEST(test_apu_mirror_music_rest_cuts_sustain);
    RUN_TEST(test_apu_mirror_sequencer_tick_timing);
    RUN_TEST(test_apu_mirror_voice_partition_constants);
    RUN_TEST(test_apu_mirror_sequencer_percussion_same_step_uses_distinct_sfx_voices);
    RUN_TEST(test_apu_mirror_sequencer_percussion_does_not_disturb_melodic_slots);
    RUN_TEST(test_apu_mirror_instrument_preview_uses_sfx_subpool);
    RUN_TEST(test_apu_mirror_partition_confinement_guard);
    RUN_TEST(test_apu_mirror_percussion_borrows_idle_music_slot);
    RUN_TEST(test_apu_mirror_percussion_never_steals_live_melodic_voice);
    RUN_TEST(test_apu_mirror_melodic_tail_overlaps_next_beat);
    RUN_TEST(test_apu_mirror_melodic_zero_release_unchanged);
    RUN_TEST(test_apu_mirror_golden_pcm_parity);
    RUN_TEST(test_apu_presence_helpers_measure_solo_peak_and_rms);
    RUN_TEST(test_apu_presence_legacy_volume_baseline_is_recorded);
    RUN_TEST(test_apu_presence_candidate_volume_evaluation_and_adoption_gate);
    RUN_TEST(test_apu_mirror_noise_pitch_sweep_updates_period);
    RUN_TEST(test_apu_mirror_linear_sweep_midpoint_arithmetic);
    RUN_TEST(test_apu_mirror_exponential_sweep_midpoint_geometric);
    RUN_TEST(test_apu_mirror_exponential_noise_sweep_updates_period);
    RUN_TEST(test_apu_mirror_loop_voice_stays_enabled_until_stop);
    RUN_TEST(test_apu_mirror_oneshot_zero_duration_does_not_hang);
    RUN_TEST(test_apu_mirror_loop_voice_is_stealable);
    RUN_TEST(test_apu_mirror_duty_stepped_mid_note_hold);
    RUN_TEST(test_apu_mirror_pitch_envelope_three_points_linear);
    RUN_TEST(test_apu_mirror_pitch_envelope_precedes_single_sweep);
    RUN_TEST(test_apu_mirror_music_seek_repositions_elapsed_ticks);
    return UNITY_END();
}
