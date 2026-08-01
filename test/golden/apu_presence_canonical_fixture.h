#pragma once

/// Offline presence/headroom harness for APU preview calibration.
/// Renders in-memory PCM only (no SDL, no git checkout, no branch switch).

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace apu_presence {

using pixelroot32::audio::ApuCore;
using pixelroot32::audio::AudioCommand;
using pixelroot32::audio::AudioCommandType;
using pixelroot32::audio::INSTR_PULSE_HARMONY;
using pixelroot32::audio::INSTR_PULSE_LEAD;
using pixelroot32::audio::INSTR_PULSE_PAD;
using pixelroot32::audio::INSTR_TRIANGLE_BASS;
using pixelroot32::audio::InstrumentPreset;
using pixelroot32::audio::MusicNote;
using pixelroot32::audio::MusicTrack;
using pixelroot32::audio::Note;
using pixelroot32::audio::WaveType;

inline constexpr int kSampleRate = 44100;
inline constexpr float kBpm = 120.0f;
inline constexpr int kRenderSampleCount = 44100;       // 1.0 s
inline constexpr int kSustainWindowStart = 8820;       // ~0.2 s (past attack)
inline constexpr int kSustainWindowCount = 22050;      // 0.5 s
inline constexpr int16_t kMinSoloPeakS16 = 4000;       // ~-18 dBFS
inline constexpr int16_t kMaxHeadroomPeakS16 = 31129;  // 95% of int16 full scale
inline constexpr float kCandidateTrackVolume = 1.0f;
inline constexpr float kLegacyTrackVolume = 0.5f;

struct Metrics {
    int16_t peakAbs = 0;
    double sustainRms = 0.0;
    int activeVoiceCount = 0;
};

inline float effective_note_volume(float preset_base_volume, float track_volume)
{
    return preset_base_volume * track_volume;
}

inline MusicNote make_scaled_note(const InstrumentPreset& preset, Note note,
                                  uint8_t octave, float duration,
                                  float track_volume)
{
    return MusicNote{note, octave, duration,
                     effective_note_volume(preset.baseVolume, track_volume),
                     &preset};
}

inline int16_t peak_abs_s16(const int16_t* buf, int count)
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

inline double rms_s16(const int16_t* buf, int count)
{
    if (count <= 0) {
        return 0.0;
    }
    double sum_sq = 0.0;
    for (int i = 0; i < count; ++i) {
        const double s = static_cast<double>(buf[i]);
        sum_sq += s * s;
    }
    return std::sqrt(sum_sq / static_cast<double>(count));
}

inline int count_active_music_voices(const ApuCore& apu, int expected_voices)
{
    int active = 0;
    for (int i = 0; i < expected_voices; ++i) {
        if (apu.isMusicTrackVoiceActiveForTesting(static_cast<size_t>(i))) {
            ++active;
        }
    }
    return active;
}

inline void queue_play(ApuCore& apu, const MusicTrack& main,
                       const MusicTrack* const* subs, size_t sub_count)
{
    AudioCommand bpm{};
    bpm.type = AudioCommandType::MUSIC_SET_BPM;
    bpm.bpm = kBpm;
    apu.submitCommand(bpm);

    AudioCommand play{};
    play.type = AudioCommandType::MUSIC_PLAY;
    play.track = &main;
    play.subTrackCount = std::min(sub_count, AudioCommand::MAX_SUB_TRACKS);
    for (size_t i = 0; i < play.subTrackCount; ++i) {
        play.subTracks[i] = subs[i];
    }
    apu.submitCommand(play);
}

inline Metrics measure_render(const MusicTrack& main,
                              const MusicTrack* const* subs, size_t sub_count,
                              int expected_voices)
{
    ApuCore apu;
    apu.init(kSampleRate);
    apu.reset();
    queue_play(apu, main, subs, sub_count);

    std::vector<int16_t> pcm(static_cast<size_t>(kRenderSampleCount), 0);
    constexpr int kBlock = 512;
    int generated = 0;
    int max_active = 0;
    while (generated < kRenderSampleCount) {
        const int block = std::min(kBlock, kRenderSampleCount - generated);
        apu.generateSamples(pcm.data() + generated, block);
        generated += block;
        if (generated >= kSustainWindowStart &&
            generated <= kSustainWindowStart + kSustainWindowCount) {
            max_active = std::max(
                max_active, count_active_music_voices(apu, expected_voices));
        }
    }

    Metrics metrics;
    metrics.peakAbs =
        peak_abs_s16(pcm.data() + kSustainWindowStart, kSustainWindowCount);
    metrics.sustainRms =
        rms_s16(pcm.data() + kSustainWindowStart, kSustainWindowCount);
    metrics.activeVoiceCount = max_active;
    return metrics;
}

/// Canonical 120 BPM sustained Pulse/melodic cases for presence/headroom gates.
inline Metrics measure_solo_pulse(float track_volume)
{
    const MusicNote notes[] = {
        make_scaled_note(INSTR_PULSE_LEAD, Note::C, 4, 4.0f, track_volume),
    };
    const MusicTrack track{notes, 1, false, WaveType::PULSE,
                           INSTR_PULSE_LEAD.duty, nullptr, nullptr, nullptr};
    return measure_render(track, nullptr, 0, 1);
}

inline Metrics measure_two_pulse(float track_volume)
{
    const MusicNote lead_notes[] = {
        make_scaled_note(INSTR_PULSE_LEAD, Note::C, 4, 4.0f, track_volume),
    };
    const MusicNote harmony_notes[] = {
        make_scaled_note(INSTR_PULSE_HARMONY, Note::E, 4, 4.0f, track_volume),
    };
    const MusicTrack lead{lead_notes, 1, false, WaveType::PULSE,
                          INSTR_PULSE_LEAD.duty, nullptr, nullptr, nullptr};
    const MusicTrack harmony{harmony_notes, 1, false, WaveType::PULSE,
                             INSTR_PULSE_HARMONY.duty, nullptr, nullptr,
                             nullptr};
    const MusicTrack* subs[] = {&harmony};
    return measure_render(lead, subs, 1, 2);
}

inline Metrics measure_four_melodic(float track_volume)
{
    const MusicNote n0[] = {
        make_scaled_note(INSTR_PULSE_LEAD, Note::C, 4, 4.0f, track_volume),
    };
    const MusicNote n1[] = {
        make_scaled_note(INSTR_PULSE_HARMONY, Note::E, 4, 4.0f, track_volume),
    };
    const MusicNote n2[] = {
        make_scaled_note(INSTR_TRIANGLE_BASS, Note::C, 3, 4.0f, track_volume),
    };
    const MusicNote n3[] = {
        make_scaled_note(INSTR_PULSE_PAD, Note::G, 4, 4.0f, track_volume),
    };
    const MusicTrack t0{n0, 1, false, WaveType::PULSE, INSTR_PULSE_LEAD.duty,
                        nullptr, nullptr, nullptr};
    const MusicTrack t1{n1, 1, false, WaveType::PULSE, INSTR_PULSE_HARMONY.duty,
                        nullptr, nullptr, nullptr};
    const MusicTrack t2{n2, 1, false, WaveType::TRIANGLE,
                        INSTR_TRIANGLE_BASS.duty, nullptr, nullptr, nullptr};
    const MusicTrack t3{n3, 1, false, WaveType::PULSE, INSTR_PULSE_PAD.duty,
                        nullptr, nullptr, nullptr};
    const MusicTrack* subs[] = {&t1, &t2, &t3};
    return measure_render(t0, subs, 3, 4);
}

inline bool meets_presence_headroom_gates(const Metrics& solo,
                                          const Metrics& two,
                                          const Metrics& four)
{
    return solo.peakAbs >= kMinSoloPeakS16 && solo.activeVoiceCount == 1 &&
           two.peakAbs < kMaxHeadroomPeakS16 && two.activeVoiceCount == 2 &&
           four.peakAbs < kMaxHeadroomPeakS16 && four.activeVoiceCount == 4;
}

}  // namespace apu_presence
