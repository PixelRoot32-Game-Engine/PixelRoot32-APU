#pragma once

/// The wavetable scene: four SAW notes rendered through the public command API,
/// in the same fixture format as the canonical track (raw S16LE, 22 050 samples
/// at 44 100 Hz).
///
/// Why a second scene exists. The canonical track is PULSE-only, so it cannot
/// observe the E2 band-limited SAW (S4): `Hybrid`'s canonical fixture is
/// byte-identical before and after the mipmap landed, and the only thing that
/// pinned the mipmap was a unit test whose reference re-derived the ramp. This
/// scene is the end-to-end pin instead — a committed render that only stays
/// stable while the level selection, the crossfade and the table read do.
///
/// It shares the fixture CONTRACT with `apu_mirror_canonical_track.h` — the
/// sample rate and the sample count, so both scenes fit the same file size —
/// and nothing else. The canonical track, `render_canonical` and therefore the
/// `Legacy` mirror fixture rendered from it are untouched.

#include "apu_mirror_canonical_track.h"

#include "pixelroot32/apu/ApuCore.h"
#include "pixelroot32/apu/AudioMusicTypes.h"

#include <cstdint>

namespace apu_golden {

using pixelroot32::audio::ApuCore;
using pixelroot32::audio::AudioCommand;
using pixelroot32::audio::AudioCommandType;
using pixelroot32::audio::InstrumentPreset;
using pixelroot32::audio::SfxBreakpoint;
using pixelroot32::audio::WaveType;

/**
 * @brief Instrument shared by every note of the wavetable scene.
 *
 * The envelope is deliberately flat: one-sample attack (the core clamps a 0 s
 * attack to one sample), no decay, sustain 1.0 and no release. The missing
 * release is what keeps the two profile fixtures comparable sample for sample:
 * `Nes` and `Hybrid` resolve different ADSR curves, so a release tail would
 * write a second, unrelated difference into the same file. With a constant
 * envelope and silence between notes, the only thing that can differ between
 * the two renders of this scene is the SAW generator itself — both profiles
 * weigh SAW as the PULSE group (Q12 unity) and share the output filter chain,
 * so neither the mix nor the output stage introduces one.
 *
 * @return The static preset; the returned reference outlives every command.
 */
inline const InstrumentPreset& wavetable_scene_preset()
{
    static const InstrumentPreset preset = [] {
        InstrumentPreset p{};
        p.baseVolume = 0.5f;
        p.duty = 0.5f;
        p.defaultOctave = 4;
        p.attackTime = 0.0f;
        p.decayTime = 0.0f;
        p.sustainLevel = 1.0f;
        p.releaseTime = 0.0f;
        return p;
    }();
    return preset;
}

/**
 * @brief One scheduled SAW note of the wavetable scene.
 *
 * The start is a sample offset rather than a musical time so the fixture does
 * not move when a tempo constant changes: this scene pins the oscillator, not
 * the sequencer.
 */
struct WavetableSceneNote {
    int startSample;                    ///< Sample index the note-on renders into.
    float frequencyHz;                  ///< Note-on frequency; equals the glide's first point.
    float durationSec;                  ///< Gate length (release is zero, see the preset).
    const SfxBreakpoint* pitchEnvelope; ///< nullptr = no pitch automation on this note.
    uint8_t pitchEnvelopeCount;         ///< Breakpoints in `pitchEnvelope`; 0 with a null table.
};

/**
 * @brief The pitch envelope that crosses a mipmap level boundary inside one note.
 *
 * 220 Hz sits in level 1 (`q = f / fs` in `[2^-8, 2^-7)` for a 44 100 Hz
 * rate), and a linear glide to 900 Hz passes the 344.5 Hz and 689.0 Hz band
 * edges on the way. So one note renders levels 1, 2 and 3, and the two seams it
 * crosses are exactly the transition the S6 crossfade exists for: a hard table
 * switch would be visible there, a blended read is not. The glide is slow
 * (0.1 s) on purpose; a fast one would move the increment far enough per sample
 * that the saw's own slope dominates the sample-to-sample delta.
 */
inline const SfxBreakpoint kWavetableGlide[] = {
    {0.0f, 220.0f},
    {0.1f, 900.0f},
};

/**
 * @brief The scene: one note per octave band, spaced through the fixture.
 *
 * The four notes span the table set rather than one band:
 *   55 Hz   -> level 0 (below the first band, clamped to the alias-free table)
 *   220 Hz  -> level 1, gliding through levels 2 and 3
 *   880 Hz  -> level 3
 *   1760 Hz -> level 4
 *
 * Every note ends (gate + zero release) before the next one starts, so the
 * render is a sequence of isolated notes with silent gaps; no voice stealing
 * or overlap can move a sample without changing the scene.
 */
inline constexpr WavetableSceneNote kWavetableScene[] = {
    {    0,   55.0f, 0.09f, nullptr,        0},
    { 4410,  220.0f, 0.10f, kWavetableGlide, 2},
    { 9702,  880.0f, 0.10f, nullptr,        0},
    {14994, 1760.0f, 0.10f, nullptr,        0},
};

/// @brief Number of entries in `kWavetableScene`.
inline constexpr int kWavetableSceneNoteCount =
    static_cast<int>(sizeof(kWavetableScene) / sizeof(kWavetableScene[0]));

// The fixture format is the canonical one; a scene that did not fit it would
// silently truncate (or pad) the render and the committed file would describe
// something the tool did not produce.
static_assert(kWavetableScene[kWavetableSceneNoteCount - 1].startSample
                  < kGoldenSampleCount,
              "the wavetable scene must fit inside the canonical fixture length");

/**
 * @brief Renders the wavetable scene: a reset, an init and the scheduled notes.
 *
 * Commands are submitted right before the block that must start them, because
 * `generateSamples` drains the queue at the top of every call: that is how a
 * sample-accurate offset reaches the public API without a sequencer. The
 * profile is configuration (S4) and survives the `reset()`/`init()` here, so
 * the caller sets it first, exactly like `render_canonical`.
 *
 * @param apu Core to render with; its profile must already be selected.
 * @param out Destination buffer, exactly `count` S16 samples are written.
 * @param count Samples to render; `kGoldenSampleCount` produces the fixture.
 */
inline void render_wavetable(ApuCore& apu, int16_t* out, int count)
{
    apu.reset();
    apu.init(kSampleRate);

    int rendered = 0;
    for (int i = 0; i < kWavetableSceneNoteCount; ++i) {
        const WavetableSceneNote& note = kWavetableScene[i];
        if (note.startSample >= count) {
            break;
        }
        if (note.startSample > rendered) {
            apu.generateSamples(out + rendered, note.startSample - rendered);
            rendered = note.startSample;
        }

        AudioCommand play{};
        play.type = AudioCommandType::PLAY_EVENT;
        play.event.type = WaveType::SAW;
        play.event.frequency = note.frequencyHz;
        play.event.duration = note.durationSec;
        // Half scale: the profile output stage spikes on the saw's edge, and a
        // clipped fixture would flatten exactly the harmonics whose level the
        // mipmap changes.
        play.event.volume = 0.5f;
        play.event.duty = 0.5f;
        play.event.preset = &wavetable_scene_preset();
        play.event.pitchEnvelope = note.pitchEnvelope;
        play.event.pitchEnvelopeCount = note.pitchEnvelopeCount;
        apu.submitCommand(play);
    }

    if (rendered < count) {
        apu.generateSamples(out + rendered, count - rendered);
    }
}

}  // namespace apu_golden
