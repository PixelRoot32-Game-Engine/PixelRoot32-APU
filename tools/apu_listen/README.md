# apu_listen - judging the three sound profiles by ear

Plays three fixed pieces of increasing complexity through the **real I2S
backend**, prints one line per block on the serial console, and repaints an
ILI9341 panel once per block with the piece and profile that are playing.
**The speaker is the judge; the serial output is the record; the panel is the
sign on the door.**

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

- **`simple`** - one 50 % `PULSE` melody, sixteen eighth notes, looping. One voice.
- **`medium`** - the `PULSE` melody plus a `TRIANGLE` bass on the root of each bar
  (i-VI-III-VII) plus a `NOISE` hi-hat on every beat. Three voices.
- **`complex`** - four music tracks (melody, a diatonic third above on a 25 %
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

Classic ESP32 (`board = esp32dev`), the `pong` demo's
`PLATFORM_ESP32DEV_ILI9341` board: a MAX98357A DAC and an ILI9341 240x320 panel
mounted in portrait and used in landscape.

| Signal | GPIO |
|---|---|
| I2S BCLK | 5 |
| I2S LRCK | 21 |
| I2S DOUT | 19 |
| TFT MISO | 22 |
| TFT MOSI | 23 |
| TFT SCLK | 14 |
| TFT CS | 15 |
| TFT DC | 2 |
| TFT RST | 4 |
| TFT backlight | 16 |

Sample rate: **22 050 Hz**, mono. No buttons, no touch.

## Status panel

TFT_eSPI is used directly (no `Renderer`, no `Scene`, no engine input config):
this is one static screen, not a game. The panel is built for a 320x240 logical
canvas (the physical 240x320 panel after `DISPLAY_ROTATION=3`) and is cleared
and repainted once per block, in `loop()`, before the block's timer starts.

```
 y=4    APU LISTEN  22050 Hz  I2S 5/21/19          dark grey, 8 px
 y=18   PIECE                                      light grey, 8 px
 y=28   SIMPLE / MEDIUM / COMPLEX                  white, 26 px letters x2
 y=86   PROFILE                                    light grey, 8 px
 y=96   LEGACY / NES / HYBRID                      26 px letters x3, profile colour
 y=178  full-width colour bar (320x6)              profile colour
 y=190  BLOCK 3/9                                  white, 16 px
 y=210  PREV UNDERRUNS <n>   PEAK <n>              light grey, 8 px
 y=221  PREV HASH <8 hex digits>                   light grey, 8 px
 y=230  prev = the block that just ended; ...      dark grey, 8 px
```

Profile colours, chosen to be read from across the room: `LEGACY` white, `NES`
green, `HYBRID` cyan. The bar repeats the colour so the profile is identifiable
even without reading the word. The `PREV` numbers are the ones the serial
`REPORT` line carried for the block that just ended, so screen and log always
agree; on block 1 they read `PREV none yet - first block`.

The top line (rate and I2S pins) is drawn on every block, so the screen says the
harness is alive before any report exists: a black panel is a display problem,
not a dead board.

The panel is optional. The display is initialised only after the first block is
already playing, and every draw returns immediately until that init completed;
`TFT_eSPI::init()` returns `void` and shifts its commands out without waiting for
the panel, so an absent screen cannot block the audio. A missing panel costs the
screen and nothing else.

## Build and flash

```powershell
Set-Location tools\apu_listen
pio run -e esp32dev                                   # build
pio run -e esp32dev -t upload --upload-port COM4       # flash
pio device monitor --port COM4 --baud 115200           # listen + capture
```

The project is standalone and is not part of the root CMake build. It compiles
the APU from this working tree (`symlink://../..`) and the engine from its
worktree, never from the registry; TFT_eSPI is declared explicitly with the same
source the engine pins.

## Serial output

Once at start-up:

```
# profiles: Legacy=flat reference, Nes=stepped duty+unipolar noise, Hybrid=exponential envelope
```

One banner per block, before it starts, one report when it ends:

```
# LISTEN piece=simple profile=Legacy block=1 of 9
REPORT piece=simple profile=Legacy underruns=0 worst_render_us=0 peak=18432 hash=1A2B3C4D
```

- `underruns` - `ESP32_I2S_AudioBackend::getUnderrunCount()` **delta for this
  block** (the backend counter is monotonic).
- `worst_render_us` - delta of `getWorstRenderUs()` for this block. That counter
  is a running maximum, so this is the increase: `0` means the block set no new
  record, not that it rendered instantly.
- `peak` - largest absolute mixed sample in the block, read through the APU's
  post-mix hook. `peak=0` means the block was digital silence.
- `hash` - FNV-1a of the block's mixed samples. Two profiles that report the
  same hash for the same piece did not reach the renderer, so a difference the
  ear claims to hear cannot be confirmed and a difference it misses cannot be
  dismissed.

The frame also prints the firmware's block size, the panel line once it is
configured, and `sizeof` of `MusicNote` / `MusicTrack` / `AudioEvent` /
`AudioCommand`, so a report can be tied to the exact build that produced it.

## Limits

Nothing here has been verified on hardware yet: the project builds, but nobody
has listened to it or looked at the panel. If the I2S peripheral fails to start,
the firmware prints `# WARN backend never iterated: i2s init failed? pins=5/21/19`
three seconds in instead of pretending the run is healthy.
