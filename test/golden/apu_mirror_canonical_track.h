#pragma once

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

#include <cstdint>

namespace apu_golden {

using pixelroot32::audio::ApuCore;
using pixelroot32::audio::AudioCommand;
using pixelroot32::audio::AudioCommandType;
using pixelroot32::audio::AudioEvent;
using pixelroot32::audio::InstrumentPreset;
using pixelroot32::audio::LfoTarget;
using pixelroot32::audio::MusicNote;
using pixelroot32::audio::MusicTrack;
using pixelroot32::audio::Note;
using pixelroot32::audio::WaveType;
using pixelroot32::audio::INSTR_KICK;
using pixelroot32::audio::INSTR_PULSE_LEAD;
using pixelroot32::audio::makeNote;
using pixelroot32::audio::makeRest;

inline constexpr int kSampleRate = 44100;
inline constexpr float kBpm = 120.0f;
inline constexpr int kGoldenSampleCount = 22050;  // 0.5 s @ 44100 Hz

/// Canonical parity track: pulse melody + rest spacing (Engine commit 8ccc31a).
inline const MusicTrack& canonical_track()
{
    static const MusicNote notes[] = {
        makeNote(INSTR_PULSE_LEAD, Note::E, 4, 0.25f),
        makeRest(0.25f),
        makeNote(INSTR_PULSE_LEAD, Note::G, 4, 0.25f),
        makeRest(0.75f),
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
    return track;
}

inline void queue_canonical_play(ApuCore& apu)
{
    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = kBpm;
    apu.submitCommand(bpm);

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &canonical_track();
    apu.submitCommand(play);
}

inline void render_canonical(ApuCore& apu, int16_t* out, int count)
{
    apu.reset();
    apu.init(kSampleRate);
    queue_canonical_play(apu);
    apu.generateSamples(out, count);
}

inline uint64_t tick_duration_samples(int sample_rate, float bpm)
{
    return static_cast<uint64_t>(
        static_cast<float>(sample_rate) * 60.0f / (bpm * static_cast<float>(ApuCore::TICKS_PER_BEAT)));
}

}  // namespace apu_golden
