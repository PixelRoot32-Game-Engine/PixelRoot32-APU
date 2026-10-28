# apu_listen — judging the three sound profiles by ear

Plays three fixed pieces of increasing complexity through the **real I2S
backend** and prints one line per block on the serial console. There is no
display and no input: **the speaker is the judge, the serial output is the
record**.

This is the listening sibling of `tools/apu_underrun_probe` (which answers "did
the producer keep up") and of `tools/profiles_ab` (which answers "what changed"
with raw PCM). Here the question is "which profile sounds better", and only a
human can answer it.

## What plays, in what order

For each piece, in this order, under each profile in this order:

| Block | Piece | Profile | Length |
|---|---|---|---|
| 1 | `simple` | `Legacy` | 12 s |
| 2 | `simple` | `Nes` | 12 s |
| 3 | `simple` | `Hybrid` | 12 s |
| 4 | `medium` | `Legacy` | 12 s |
| 5 | `medium` | `Nes` | 12 s |
| 6 | `medium` | `Hybrid` | 12 s |
| 7 | `complex` | `Legacy` | 12 s |
| 8 | `complex` | `Nes` | 12 s |
| 9 | `complex` | `Hybrid` | 12 s |

After block 9 the programme restarts at block 1, forever. A full cycle is
1 min 48 s. Every piece is in A minor / C major at 120 BPM, so the comparison is
musical rather than a test tone.

- **`simple`** — one 50 % `PULSE` melody, sixteen eighth notes, looping. One voice.
- **`medium`** — the `PULSE` melody plus a `TRIANGLE` bass on the root of each bar
  (i–VI–III–VII) plus a `NOISE` hi-hat on every beat. Three voices.
- **`complex`** — four music tracks (melody, a diatonic third above on a 25 %
  duty pulse, `TRIANGLE` bass, `NOISE` kick/hat percussion) plus a four-voice
  `PLAY_EVENT` layer on the SFX slots: a sustained `SAW` pad, a `SINE`
  counter-melody on the half notes, a `NOISE` snare on beats 2 and 4, and a
  `PULSE` arpeggio on the eighth notes. Eight voices at the peak, the same
  music/SFX partition the gate harness exercises.

A looping `PLAY_EVENT` is a sustained tone and cannot retrigger, so only the pad
is `loop = true`; the snare, the counter-melody and the arpeggio are one-shot
`PLAY_EVENT`s re-submitted on the eighth-note grid. That is also what makes the
envelope differences audible: `Hybrid`'s exponential decay is most obvious on
the percussion, not on a held note.

## Wiring (fixed)

Classic ESP32 (`board = esp32dev`) and a MAX98357A, wired as in the `pong`
demo's `PLATFORM_ESP32DEV_ILI9341` platform:

| Signal | GPIO |
|---|---|
| BCLK | 5 |
| LRCK | 21 |
| DOUT | 19 |

Sample rate: **22 050 Hz**, mono. No display, no buttons.

## Build and flash

```powershell
Set-Location tools\apu_listen
pio run -e esp32dev                                  # build
pio run -e esp32dev -t upload --upload-port COMx      # flash
pio device monitor --port COMx --baud 115200          # listen + capture
```

The project is standalone and is not part of the root CMake build. It compiles
the APU from this working tree (`symlink://../..`) and the engine from its
worktree, never from the registry.

## Serial output

Once at start-up:

```
# profiles: Legacy=flat reference, Nes=stepped duty+unipolar noise, Hybrid=exponential envelope
```

One banner per block, before it starts, one report when it ends:

```
# LISTEN piece=simple profile=Legacy block=1 of 9
REPORT piece=simple profile=Legacy underruns=0 worst_render_us=0 peak=18432
```

- `underruns` — `ESP32_I2S_AudioBackend::getUnderrunCount()` **delta for this
  block** (the backend counter is monotonic).
- `worst_render_us` — delta of `getWorstRenderUs()` for this block. That counter
  is a running maximum, so this is the increase: `0` means the block set no new
  record, not that it rendered instantly.
- `peak` — largest absolute mixed sample in the block, read through the APU's
  post-mix hook. `peak=0` means the block was digital silence.

The frame also prints the firmware's block size and `sizeof` of
`MusicNote` / `MusicTrack` / `AudioEvent` / `AudioCommand`, so a report can be
tied to the exact build that produced it.

## Limits

Nothing here has been verified on hardware yet: the project builds, but nobody
has listened to it. If the I2S peripheral fails to start, the firmware prints
`# WARN backend never iterated: i2s init failed? pins=5/21/19` three seconds in
instead of pretending the run is healthy.
