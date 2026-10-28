/*
 * Copyright (c) 2026 PixelRoot32
 * Licensed under the MIT License
 */

/**
 * @file main.cpp
 * @brief Listening harness: three fixed pieces, each under the three sound profiles.
 *
 * The gate harness (`tools/apu_underrun_probe`) answers "did the producer keep
 * up"; this one answers "which profile sounds better", which only a human ear
 * can answer. It therefore does the two things a listener needs and nothing
 * else:
 *
 * - it plays a fixed programme, in a fixed order, for a fixed time, so two
 *   profiles are always compared on the same notes; and
 * - it prints one banner before and one report after each block, so the serial
 *   console is a transcript of what the speaker just played (`peak` also proves
 *   the block was not silent, which `underruns=0` alone cannot).
 *
 * The audio path is the real one: `AudioEngine` + `ESP32_I2S_AudioBackend` at
 * 22 050 Hz. Music and events go through the APU command queue, and the profile
 * is switched with `SET_SOUND_PROFILE` because the profile is audio-thread state
 * and the command is the documented crossing from the main thread.
 *
 * The pieces are deliberately cumulative, so a difference heard at one level can
 * be attributed to the number of voices:
 *   simple  - one PULSE melody (one voice).
 *   medium  - melody + TRIANGLE bass + NOISE hi-hat (three voices).
 *   complex - four music tracks (melody, harmony a third above, bass, percussion)
 *             plus a four-voice PLAY_EVENT layer on the SFX slots (eight voices
 *             at the peak), the same partition the gate harness exercises.
 *
 * No display, no buttons, no dynamic allocation in the audio path: every note
 * table is `constexpr` and lives in flash.
 */

#include <Arduino.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <audio/AudioConfig.h>
#include <audio/AudioEngine.h>
#include <drivers/esp32/ESP32_I2S_AudioBackend.h>
#include <pixelroot32/apu/ApuCore.h>
#include <pixelroot32/apu/AudioMusicTypes.h>
#include <pixelroot32/apu/AudioTypes.h>

namespace {

using pixelroot32::audio::ApuCore;
using pixelroot32::audio::AudioCommand;
using pixelroot32::audio::AudioCommandType;
using pixelroot32::audio::AudioConfig;
using pixelroot32::audio::AudioEngine;
using pixelroot32::audio::AudioEvent;
using pixelroot32::audio::AudioScheduler;
using pixelroot32::audio::InstrumentPreset;
using pixelroot32::audio::MusicNote;
using pixelroot32::audio::MusicTrack;
using pixelroot32::audio::Note;
using pixelroot32::audio::SoundProfile;
using pixelroot32::audio::WaveType;

using pixelroot32::audio::INSTR_HIHAT;
using pixelroot32::audio::INSTR_KICK;
using pixelroot32::audio::INSTR_PULSE_HARMONY;
using pixelroot32::audio::INSTR_PULSE_LEAD;
using pixelroot32::audio::INSTR_SNARE;
using pixelroot32::audio::INSTR_TRIANGLE_BASS;
using pixelroot32::audio::noteToFrequency;

using pixelroot32::drivers::esp32::ESP32_I2S_AudioBackend;

// ---------------------------------------------------------------------------
// Fixed hardware: the `pong` demo's PLATFORM_ESP32DEV_ILI9341 MAX98357A wiring.
// These are compile-time constants on purpose: a listening comparison is only
// meaningful on the board the profiles ship on, so there is nothing to configure.
// ---------------------------------------------------------------------------

/** @brief I2S bit clock. */
constexpr int kI2sBclk = 5;
/** @brief I2S word clock / left-right clock. */
constexpr int kI2sLrck = 21;
/** @brief I2S serial data out to the MAX98357A DIN. */
constexpr int kI2sDout = 19;
/** @brief Sample rate the comparison runs at. */
constexpr int kSampleRate = 22050;

/** @brief One block per piece x profile combination. */
constexpr uint32_t kBlockMs = 12000;

/** @brief Tempo of every piece: one beat is 500 ms, one eighth note 250 ms. */
constexpr float kBpm = 120.0f;
/** @brief Eighth-note grid used by the complex piece's SFX layer. */
constexpr uint32_t kEighthNoteMs = 250;

/** @brief Pieces and profiles, and therefore blocks, in the programme. */
constexpr uint8_t kPieceCount = 3;
constexpr uint8_t kProfileCount = 3;
constexpr uint8_t kBlockCount = kPieceCount * kProfileCount;
static_assert(kBlockCount == 9, "the programme is three pieces x three profiles");

/** @brief Defensive bound for a track table; a typo must not walk off the array. */
constexpr size_t kMaxTrackNotes = 64;

// ---------------------------------------------------------------------------
// Music tables.
//
// `MusicNote::duration` is in beats (quarter note = 1.0), not seconds: the
// sequencer multiplies it by the tick duration in `updateMusicSequencer`. The
// tables are `constexpr` so they live in flash, and the helpers below keep the
// positional initializers readable.
// ---------------------------------------------------------------------------

/**
 * @brief Builds one melodic note for a constexpr table.
 * @param preset Instrument preset supplying ADSR, LFO and duty defaults.
 * @param pitch Note name.
 * @param octave Octave index (C4 = middle C, A4 = 440 Hz).
 * @param beats Note length in beats.
 * @param volume Linear event volume [0, 1].
 * @return The note, pointing at the static preset.
 */
constexpr MusicNote note(const InstrumentPreset& preset, Note pitch, uint8_t octave,
                         float beats, float volume)
{
    return MusicNote{pitch, octave, beats, volume, &preset};
}

/**
 * @brief Builds one rest for a constexpr table.
 * @param beats Rest length in beats.
 * @return The rest note.
 */
constexpr MusicNote rest(float beats)
{
    return MusicNote{Note::Rest, 0, beats, 0.0f, nullptr};
}

/**
 * @brief Builds one percussion hit for a NOISE track.
 *
 * A percussion hit is a `Note::Rest` carrying a preset whose duty is 0: the
 * sequencer's `isPercussionHit` rule. Its duration is 0 beats on purpose, so the
 * hit fires without advancing the grid (the following rest does that).
 * @param preset Percussion preset (kick, snare, hi-hat).
 * @param volume Linear event volume [0, 1].
 * @return The hit note.
 */
constexpr MusicNote hit(const InstrumentPreset& preset, float volume)
{
    return MusicNote{Note::Rest, preset.defaultOctave, 0.0f, volume, &preset};
}

/** @brief Compile-time element count, so a table edit cannot desync its track. */
template <typename T, size_t N>
constexpr size_t countOf(const T (&)[N])
{
    return N;
}

// -- simple: one PULSE melody, sixteen eighth notes in A minor / C major ------

constexpr MusicNote kSimpleNotes[] = {
    note(INSTR_PULSE_LEAD, Note::A, 4, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::B, 4, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::A, 4, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::G, 4, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::A, 4, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::A, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::G, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 0.5f, 0.32f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.32f),
};

constexpr MusicTrack kSimpleTrack{kSimpleNotes, countOf(kSimpleNotes), true,
                                  WaveType::PULSE, 0.5f, nullptr, nullptr, nullptr};

// -- medium: melody + TRIANGLE root per bar + NOISE hi-hat on the beat --------

constexpr MusicNote kMediumMelodyNotes[] = {
    note(INSTR_PULSE_LEAD, Note::A, 4, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::G, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::A, 4, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::G, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 0.5f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 0.5f, 0.30f),
};

// i - VI - III - VII in A minor, one whole-bar root each.
constexpr MusicNote kMediumBassNotes[] = {
    note(INSTR_TRIANGLE_BASS, Note::A, 2, 4.0f, 0.28f),
    note(INSTR_TRIANGLE_BASS, Note::F, 2, 4.0f, 0.28f),
    note(INSTR_TRIANGLE_BASS, Note::C, 3, 4.0f, 0.28f),
    note(INSTR_TRIANGLE_BASS, Note::G, 2, 4.0f, 0.28f),
};

constexpr MusicNote kMediumHatNotes[] = {
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_HIHAT, 0.12f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
};

constexpr MusicTrack kMediumMelodyTrack{kMediumMelodyNotes, countOf(kMediumMelodyNotes),
                                        true, WaveType::PULSE, 0.5f, nullptr, nullptr,
                                        nullptr};
constexpr MusicTrack kMediumBassTrack{kMediumBassNotes, countOf(kMediumBassNotes), true,
                                      WaveType::TRIANGLE, 0.5f, nullptr, nullptr, nullptr};
constexpr MusicTrack kMediumHatTrack{kMediumHatNotes, countOf(kMediumHatNotes), true,
                                     WaveType::NOISE, 0.5f, nullptr, nullptr, nullptr};

// -- complex: four music tracks ---------------------------------------------

constexpr MusicNote kComplexMelodyNotes[] = {
    note(INSTR_PULSE_LEAD, Note::A, 4, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::A, 4, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::C, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::A, 4, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::E, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::G, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::A, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::G, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::B, 4, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::D, 5, 1.0f, 0.30f),
    note(INSTR_PULSE_LEAD, Note::G, 4, 1.0f, 0.30f),
};

// A diatonic third above the melody on the shipped 25 % duty harmony preset, so
// the lead's stepped 50 % duty and this voice's continuous 25 % duty are heard
// side by side under `Nes`/`Hybrid`.
constexpr MusicNote kComplexHarmonyNotes[] = {
    note(INSTR_PULSE_HARMONY, Note::C, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::E, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::G, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::F, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::E, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::C, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::E, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::C, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::G, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::B, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::C, 6, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::B, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::F, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::D, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::F, 5, 1.0f, 0.18f),
    note(INSTR_PULSE_HARMONY, Note::B, 4, 1.0f, 0.18f),
};

constexpr MusicNote kComplexBassNotes[] = {
    note(INSTR_TRIANGLE_BASS, Note::A, 2, 4.0f, 0.28f),
    note(INSTR_TRIANGLE_BASS, Note::F, 2, 4.0f, 0.28f),
    note(INSTR_TRIANGLE_BASS, Note::C, 3, 4.0f, 0.28f),
    note(INSTR_TRIANGLE_BASS, Note::G, 2, 4.0f, 0.28f),
};

// One percussion hit per beat (kick on 1 and 3, hat on 2 and 4). One hit at a
// time matters: percussion hits borrow a voice from the SFX pool, and stacking
// them on one tick would make the allocator reach into a sustained layer.
constexpr MusicNote kComplexPercussionNotes[] = {
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
    hit(INSTR_KICK, 0.30f), rest(1.0f), hit(INSTR_HIHAT, 0.12f), rest(1.0f),
};

constexpr MusicTrack kComplexMelodyTrack{kComplexMelodyNotes, countOf(kComplexMelodyNotes),
                                         true, WaveType::PULSE, 0.5f, nullptr, nullptr,
                                         nullptr};
constexpr MusicTrack kComplexHarmonyTrack{
    kComplexHarmonyNotes, countOf(kComplexHarmonyNotes), true, WaveType::PULSE, 0.25f,
    nullptr, nullptr, nullptr};
constexpr MusicTrack kComplexBassTrack{kComplexBassNotes, countOf(kComplexBassNotes), true,
                                       WaveType::TRIANGLE, 0.5f, nullptr, nullptr, nullptr};
constexpr MusicTrack kComplexPercussionTrack{
    kComplexPercussionNotes, countOf(kComplexPercussionNotes), true, WaveType::NOISE, 0.5f,
    nullptr, nullptr, nullptr};

// -- complex SFX layer, played on the four PLAY_EVENT slots -------------------

/**
 * @brief Counter-melody for the SINE voice, one note per half note.
 *
 * Kept as a pitch list instead of a `MusicTrack` because this layer must own the
 * SFX slots: the four music slots are already taken by the tracks above.
 */
constexpr Note kCounterMelodyNotes[] = {
    Note::E, Note::G, Note::A, Note::G, Note::E, Note::D, Note::C, Note::E};

/** @brief Arpeggio for the PULSE voice, one note per eighth note. */
constexpr Note kArpeggioNotes[] = {Note::A, Note::C, Note::E, Note::A};
constexpr uint8_t kArpeggioOctaves[] = {4, 5, 5, 5};

// ---------------------------------------------------------------------------
// Programme description.
// ---------------------------------------------------------------------------

/** @brief One piece of the programme and the tracks it needs. */
struct Piece {
    const char* name;                                  ///< Name used in the serial lines.
    const MusicTrack* mainTrack;                       ///< MUSIC_PLAY's primary track.
    const MusicTrack* subTracks[AudioCommand::MAX_SUB_TRACKS];  ///< MUSIC_PLAY's sub-tracks.
    uint8_t subTrackCount;                             ///< Number of valid sub-track entries.
    bool withSfxLayer;                                 ///< True for the complex piece.
};

constexpr Piece kPieces[] = {
    {"simple", &kSimpleTrack, {nullptr, nullptr, nullptr}, 0, false},
    {"medium", &kMediumMelodyTrack, {&kMediumBassTrack, &kMediumHatTrack, nullptr}, 2,
     false},
    {"complex", &kComplexMelodyTrack,
     {&kComplexHarmonyTrack, &kComplexBassTrack, &kComplexPercussionTrack}, 3, true},
};

constexpr SoundProfile kProfiles[] = {SoundProfile::Legacy, SoundProfile::Nes,
                                      SoundProfile::Hybrid};
constexpr const char* kProfileNames[] = {"Legacy", "Nes", "Hybrid"};

// ---------------------------------------------------------------------------
// Engine wiring.
// ---------------------------------------------------------------------------

/// File scope on purpose: the backend carries a 1 KB render buffer and the
/// Arduino loop task stack is 8 KB (see the engine's audio guide).
ESP32_I2S_AudioBackend gBackend(kI2sBclk, kI2sLrck, kI2sDout, kSampleRate);
AudioConfig gConfig(&gBackend, kSampleRate);

/**
 * @brief Largest absolute mixed sample of the block in progress.
 *
 * Written by the audio task through the post-mix hook, reset by the main loop at
 * every block boundary. Atomic because those are two different tasks and a torn
 * read would report a peak that never happened.
 */
std::atomic<int32_t> gBlockPeak{0};

/**
 * @brief FNV-1a hash of everything the mix produced during the current block.
 *
 * `peak` cannot tell a block that replayed the same audio from one that changed
 * profile: two different waveforms can share a peak. The hash is what makes the
 * listening record defensible - if two profiles report the same hash for the
 * same piece, the profile did not reach the renderer and the ear is being asked
 * about nothing. Audio thread only; the main thread reads it between blocks.
 */
std::atomic<uint32_t> gBlockHash{2166136261u};

/**
 * @brief Post-mix hook: tracks the peak of the buffer the audio task just mixed.
 *
 * `AudioEngine` copies its `AudioConfig` in the constructor, so setting
 * `gConfig.postMixMono` after construction never reaches the copy `init()`
 * reads. Attaching it to the scheduler's own core after `init()` is the only
 * path guaranteed to land on the instance the audio task calls.
 * @param mono Mixed mono buffer; owned by the audio task.
 * @param length Number of samples in the buffer.
 * @param user Unused.
 */
void postMixMono(int16_t* mono, int length, void* user)
{
    (void)user;
    if (mono == nullptr || length <= 0) {
        return;
    }
    int32_t peak = 0;
    uint32_t hash = gBlockHash.load(std::memory_order_relaxed);
    for (int i = 0; i < length; ++i) {
        const int32_t value = mono[i] < 0 ? -static_cast<int32_t>(mono[i]) : mono[i];
        if (value > peak) {
            peak = value;
        }
        // FNV-1a over the mixed samples: cheap, order sensitive and good enough
        // to tell two renders apart.
        hash ^= static_cast<uint32_t>(static_cast<uint16_t>(mono[i]));
        hash *= 16777619u;
    }
    gBlockHash.store(hash, std::memory_order_relaxed);
    int32_t current = gBlockPeak.load(std::memory_order_relaxed);
    while (peak > current &&
           !gBlockPeak.compare_exchange_weak(current, peak, std::memory_order_relaxed)) {
    }
}

AudioEngine gEngine(gConfig);

// ---------------------------------------------------------------------------
// Block state.
// ---------------------------------------------------------------------------

uint8_t gBlockIndex = 0;            ///< 0..8, printed as 1..9.
uint8_t gPieceIndex = 0;            ///< Index into `kPieces` for the current block.
uint32_t gBlockStartMs = 0;         ///< `millis()` at the current block's start.
int32_t gGridLastServed = -1;       ///< Last eighth-note step serviced in this block.
uint32_t gUnderrunsAtBlockStart = 0;    ///< Backend counter snapshot, block start.
uint32_t gWorstRenderAtBlockStart = 0;  ///< Backend counter snapshot, block start.
bool gLivenessChecked = false;      ///< One-shot I2S failure warning.

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Validates a track before it is handed to the sequencer.
 *
 * The tables are static, so this only ever fires on a typo in this file, but the
 * sequencer dereferences `notes` and walks `count` entries: a bad pointer is a
 * crash, not a wrong note, and a listening harness must not crash mid-programme.
 * @param track Track to check.
 * @return true when the track is safe to play.
 */
bool isValidTrack(const MusicTrack* track)
{
    if (track == nullptr || track->notes == nullptr) {
        return false;
    }
    if (track->count == 0 || track->count > kMaxTrackNotes) {
        return false;
    }
    if (static_cast<uint8_t>(track->channelType) > static_cast<uint8_t>(WaveType::SAW)) {
        return false;
    }
    return true;
}

/**
 * @brief Validates a piece's whole track set before it is submitted.
 * @param piece Piece to check.
 * @return true when the main track and every declared sub-track are playable.
 */
bool isValidPiece(const Piece& piece)
{
    if (!isValidTrack(piece.mainTrack)) {
        return false;
    }
    if (piece.subTrackCount > AudioCommand::MAX_SUB_TRACKS) {
        return false;
    }
    for (uint8_t i = 0; i < piece.subTrackCount; ++i) {
        if (!isValidTrack(piece.subTracks[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Hands one command to the engine.
 * @param cmd Command to enqueue.
 */
void submit(const AudioCommand& cmd)
{
    gEngine.submitCommand(cmd);
}

/**
 * @brief Submits one one-shot PLAY_EVENT.
 * @param type Waveform of the voice.
 * @param frequency Voice frequency (Hz); NOISE treats it as the LFSR clock.
 * @param duration Audible length in seconds.
 * @param volume Linear event volume [0, 1].
 * @param duty Pulse duty for PULSE voices; ignored otherwise.
 * @param preset Optional instrument preset (ADSR, LFO, noise mode).
 * @param noisePeriod NOISE period override; 0 derives it from `frequency`.
 */
void submitEvent(WaveType type, float frequency, float duration, float volume, float duty,
                 const InstrumentPreset* preset, uint8_t noisePeriod = 0)
{
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = type;
    cmd.event.frequency = frequency;
    cmd.event.duration = duration;
    cmd.event.volume = volume;
    cmd.event.duty = duty;
    cmd.event.noisePeriod = noisePeriod;
    cmd.event.preset = preset;
    cmd.event.loop = false;
    submit(cmd);
}

/**
 * @brief Starts the sustained SAW pad of the complex piece.
 *
 * The pad is the one SFX voice that is a genuine `loop = true` event: it holds a
 * single pitch for the whole block and gives the eight-voice mix its bed.
 */
void submitPad()
{
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::SAW;
    cmd.event.frequency = noteToFrequency(Note::A, 3);
    cmd.event.duration = 0.0f;
    cmd.event.volume = 0.10f;
    cmd.event.duty = 0.5f;
    cmd.event.preset = nullptr;
    cmd.event.loop = true;
    submit(cmd);
}

/**
 * @brief Fires the SFX voices due at one eighth-note step of the complex piece.
 *
 * A looping `PLAY_EVENT` is a sustained tone: it cannot retrigger, so a snare
 * pattern and an arpeggio have to be *submitted* on the grid. The pad above is
 * the only sustained member of this layer. Everything here is one shot, shorter
 * than the grid step, which keeps at least one SFX slot free for the next event
 * and stops the allocator from stealing the pad.
 * @param step Zero-based eighth-note index since the block started.
 */
void fireComplexStep(uint32_t step)
{
    const uint8_t arpIndex = static_cast<uint8_t>(step % countOf(kArpeggioNotes));
    submitEvent(WaveType::PULSE,
                noteToFrequency(kArpeggioNotes[arpIndex], kArpeggioOctaves[arpIndex]),
                0.18f, 0.12f, 0.25f, nullptr);

    // 4/4 at 120 BPM: eighth steps 2 and 6 are beats 2 and 4.
    const uint32_t stepInBar = step % 8u;
    if (stepInBar == 2u || stepInBar == 6u) {
        submitEvent(WaveType::NOISE, 150.0f, 0.12f, 0.26f, 0.5f, &INSTR_SNARE,
                    INSTR_SNARE.noisePeriod);
    }

    // One counter-melody note per half note, so the SINE line moves against the
    // PULSE melody instead of doubling it.
    if ((step % 4u) == 0u) {
        const uint8_t counterIndex =
            static_cast<uint8_t>((step / 4u) % countOf(kCounterMelodyNotes));
        submitEvent(WaveType::SINE, noteToFrequency(kCounterMelodyNotes[counterIndex], 4),
                    0.60f, 0.14f, 0.5f, nullptr);
    }
}

/**
 * @brief Services every eighth-note step that has elapsed in this block.
 *
 * Catch-up is capped: if the main loop stalls, replaying every missed step would
 * burst dozens of commands into a 64-slot queue. A skipped step is inaudible on
 * a 12-second listening block, a dropped command is not.
 */
void serveComplexGrid()
{
    const uint32_t elapsedMs = static_cast<uint32_t>(millis() - gBlockStartMs);
    const uint32_t grid = elapsedMs / kEighthNoteMs;
    if (static_cast<int32_t>(grid) > gGridLastServed + 8) {
        gGridLastServed = static_cast<int32_t>(grid) - 1;
    }
    while (gGridLastServed < static_cast<int32_t>(grid)) {
        ++gGridLastServed;
        fireComplexStep(static_cast<uint32_t>(gGridLastServed));
    }
}

/**
 * @brief Prints the block banner and starts everything the block needs.
 * @param blockIndex 0-based programme position (piece x profile).
 */
void startBlock(uint8_t blockIndex)
{
    if (blockIndex >= kBlockCount) {
        blockIndex = 0;
    }
    gBlockIndex = blockIndex;
    gPieceIndex = static_cast<uint8_t>(blockIndex / kProfileCount);
    const uint8_t profileIndex = static_cast<uint8_t>(blockIndex % kProfileCount);
    const Piece& piece = kPieces[gPieceIndex];

    // Banner first: the human must be able to associate what he is about to
    // hear with the line already on screen.
    Serial.printf("# LISTEN piece=%s profile=%s block=%u of %u\n", piece.name,
                  kProfileNames[profileIndex], static_cast<unsigned>(blockIndex) + 1u,
                  static_cast<unsigned>(kBlockCount));
    Serial.flush();

    gBlockPeak.store(0, std::memory_order_relaxed);
    gBlockHash.store(2166136261u, std::memory_order_relaxed);
    gUnderrunsAtBlockStart = gBackend.getUnderrunCount();
    gWorstRenderAtBlockStart = gBackend.getWorstRenderUs();

    // Hard cut at the boundary: MUSIC_STOP clears the sequencer, but the voices
    // it owns keep their remaining gate, so they are reset explicitly. The click
    // that makes is also the audible marker of a block change.
    AudioCommand stopMusic{};
    stopMusic.type = AudioCommandType::MUSIC_STOP;
    submit(stopMusic);
    for (uint8_t channel = 0; channel < static_cast<uint8_t>(ApuCore::MAX_VOICES);
         ++channel) {
        AudioCommand stopVoice{};
        stopVoice.type = AudioCommandType::STOP_CHANNEL;
        stopVoice.channelIndex = channel;
        submit(stopVoice);
    }

    AudioCommand tempo{};
    tempo.type = AudioCommandType::MUSIC_SET_BPM;
    tempo.bpm = kBpm;
    submit(tempo);

    // The profile is audio-thread state; the command is the documented path from
    // here, and it is queued before MUSIC_PLAY so the first note already sounds
    // under the new profile.
    AudioCommand profile{};
    profile.type = AudioCommandType::SET_SOUND_PROFILE;
    profile.soundProfile = static_cast<uint8_t>(kProfiles[profileIndex]);
    submit(profile);

    if (isValidPiece(piece)) {
        AudioCommand music{};
        music.type = AudioCommandType::MUSIC_PLAY;
        music.track = piece.mainTrack;
        music.subTrackCount = piece.subTrackCount;
        for (uint8_t i = 0; i < piece.subTrackCount; ++i) {
            music.subTracks[i] = piece.subTracks[i];
        }
        submit(music);

        if (piece.withSfxLayer) {
            submitPad();
        }
    } else {
        Serial.printf("# ERROR piece=%s has an invalid track table; playing silence\n",
                      piece.name);
        Serial.flush();
    }

    gBlockStartMs = millis();
    gGridLastServed = -1;
}

/**
 * @brief Prints the report for the block that just ended.
 *
 * The backend counters are monotonic, so the block value is the delta from the
 * snapshot taken at the block start. `worst_render_us` is a running maximum, so
 * its delta is the increase (0 means no new maximum was recorded in the block).
 */
void reportBlock()
{
    const uint32_t underrunsNow = gBackend.getUnderrunCount();
    const uint32_t worstRenderNow = gBackend.getWorstRenderUs();
    const uint32_t underruns = underrunsNow - gUnderrunsAtBlockStart;
    const uint32_t worstRender = (worstRenderNow > gWorstRenderAtBlockStart)
                                     ? (worstRenderNow - gWorstRenderAtBlockStart)
                                     : 0u;
    const long peak = static_cast<long>(gBlockPeak.load(std::memory_order_relaxed));
    const unsigned long hash = static_cast<unsigned long>(
        gBlockHash.load(std::memory_order_relaxed));

    Serial.printf("REPORT piece=%s profile=%s underruns=%lu worst_render_us=%lu peak=%ld hash=%08lX\n",
                  kPieces[gPieceIndex].name,
                  kProfileNames[gBlockIndex % kProfileCount],
                  static_cast<unsigned long>(underruns),
                  static_cast<unsigned long>(worstRender), peak, hash);
    Serial.flush();
}

/**
 * @brief Warns once if the I2S backend never rendered a block.
 *
 * A failed `i2s_driver_install` / `i2s_set_pin` makes the backend return before
 * creating its task; every counter then reads zero forever and the report would
 * look like a healthy run. One warning is enough to send the listener to the
 * wiring instead of to the profiles.
 */
void checkBackendLiveness()
{
    if (gLivenessChecked || millis() < 3000u) {
        return;
    }
    gLivenessChecked = true;
    if (gBackend.getWorstBlockUs() == 0u) {
        Serial.printf("# WARN backend never iterated: i2s init failed? pins=%d/%d/%d\n",
                      kI2sBclk, kI2sLrck, kI2sDout);
        Serial.flush();
    }
}

/**
 * @brief Refuses to start on a render block the backend cannot honour.
 *
 * The backend derives its DMA depth and its fixed render buffer from the block
 * size, so a bad value is not degraded audio: it is an init failure that would
 * render the whole harness meaningless.
 */
void validateBlockSize()
{
    const int size = gConfig.blockSize;
    if (size > 0 && size <= pixelroot32::audio::PR32_AUDIO_MAX_BLOCK_SAMPLES &&
        (size % 128) == 0) {
        return;
    }
    Serial.printf("# ERROR invalid block size %d (expected a positive multiple of 128 up to %d)\n",
                  size, pixelroot32::audio::PR32_AUDIO_MAX_BLOCK_SAMPLES);
    Serial.flush();
    while (true) {
        delay(1000);
    }
}

/**
 * @brief Connects the peak meter to the APU instance the audio task renders with.
 */
void attachPeakMeter()
{
    AudioScheduler* scheduler = gEngine.getScheduler();
    if (scheduler == nullptr) {
        Serial.println("# ERROR no scheduler: peak will read 0");
        Serial.flush();
        return;
    }
    scheduler->getApuCore().setPostMixMono(&postMixMono, nullptr);
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.printf("# apu_listen rate=%d pins=%d/%d/%d block_ms=%u\n", kSampleRate, kI2sBclk,
                  kI2sLrck, kI2sDout, static_cast<unsigned>(kBlockMs));
    Serial.printf("# order piece=simple,medium,complex x profile=Legacy,Nes,Hybrid; "
                  "repeats forever\n");
    Serial.println(
        "# profiles: Legacy=flat reference, Nes=stepped duty+unipolar noise, "
        "Hybrid=exponential envelope");
    Serial.printf("# sizes MusicNote=%u MusicTrack=%u AudioEvent=%u AudioCommand=%u\n",
                  static_cast<unsigned>(sizeof(MusicNote)),
                  static_cast<unsigned>(sizeof(MusicTrack)),
                  static_cast<unsigned>(sizeof(AudioEvent)),
                  static_cast<unsigned>(sizeof(AudioCommand)));
    Serial.flush();

    validateBlockSize();

    gEngine.init();
    attachPeakMeter();
    startBlock(0);
}

void loop()
{
    if (kPieces[gPieceIndex].withSfxLayer) {
        serveComplexGrid();
    }

    if (static_cast<uint32_t>(millis() - gBlockStartMs) >= kBlockMs) {
        reportBlock();
        startBlock(static_cast<uint8_t>((gBlockIndex + 1u) % kBlockCount));
        return;
    }

    checkBackendLiveness();
    delay(2);
}
