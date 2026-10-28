# PixelRoot32-APU

Shared NES-style audio core (**APU — Audio Processing Unit**) from the PixelRoot32 family. It is the single source of truth for synthesis consumed by:

- **PixelRoot32 Game Engine** (ESP32 / native firmware, via PlatformIO)
- **PixelRoot32 Tool Suite** (desktop Music/SFX editors, via CMake)

Replaces the old manually-maintained "FPU mirror" in the Tool Suite: as of version 1.0.0 both projects link against this library and parity is guaranteed by version, not by manual copy.

## Contents

| Component | Description |
|---|---|
| `ApuCore` | 8 voices (4 music + 4 SFX), music sequencer, mixer with soft-clip + HPF, ADSR, LFO, sweeps, duty stepped, pitch envelope, bitcrush, transport (`MUSIC_SEEK`, atomic ticks) |
| `AudioTypes.h` | `AudioEvent`, `Voice`, `EnvelopeState`, `SfxBreakpoint`, `AudioCommand` |
| `AudioMusicTypes.h` | `MusicNote`, `MusicTrack`, `InstrumentPreset` + canonical `INSTR_*` presets |
| `AudioCommandQueue.h` | Lock-free SPSC queue with no allocations |
| `AudioOscLUT.h` / `AudioMixerLUT.h` | Q15 sine LUTs and integer compression curve |
| `ApuConfig.h` | Configuration surface: FPU detection, logging hook, profiling |

Two render paths: **float** (desktop, classic ESP32/S3) and **integer Q15 + LUT** (ESP32-C3 / RISC-V without FPU). The Q15 path has native CI coverage (`test_q15_smoke` with `-DPR32_APU_HAS_FPU=0`).

Both bodies now live in the same binary and a runtime **`SoundProfile`** (`Legacy`, `Nes`, `Hybrid`) selects which one renders. `Legacy` is the default and a **frozen contract** (byte-identical to its golden PCM); `Nes`/`Hybrid` currently render the integer core and are byte-identical aliases of it. `PR32_APU_ENABLE_LEGACY_FLOAT` (default `PR32_APU_HAS_FPU`) controls whether the float body is compiled at all. See `docs/ABI.md` §6 for the full selection matrix.

## Usage

### PlatformIO (Game Engine)

```ini
lib_deps = <git-url-or-path>/PixelRoot32-APU#v1.0.0
```

```cpp
#include <pixelroot32/apu/ApuCore.h>
```

### CMake (Tool Suite / desktop)

```cmake
add_subdirectory(PixelRoot32-APU)   # or FetchContent with GIT_TAG v1.0.0
target_link_libraries(my_app PRIVATE PixelRoot32::APU)
```

The library is pure C++17, dependency-free (no Arduino, no FreeRTOS, no SDL, no PlatformIO): each packaging is inert to the other ecosystem.

## Host Integration

```cpp
using namespace pixelroot32::audio;

// Logging (optional; silence by default — RT-safe mandatory):
apuSetLogHandler([](ApuLogLevel lvl, const char* msg) { /* ... */ });

// Build flags (see ApuConfig.h):
//   PR32_APU_HAS_FPU=0|1, PR32_APU_ENABLE_LEGACY_FLOAT=0|1,
//   PR32_APU_ENABLE_PROFILING=0|1,
//   AUDIO_SEQUENCER_MAX_NOTES, AUDIO_COMMAND_QUEUE_CAPACITY,
//   PIXELROOT32_DEBUG_MODE
```

## Tests

```bash
cmake -S . -B build -DPR32_APU_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure    # run ctest from build/, not the repo root
```

On a Windows host with MSYS2 MinGW and no `nmake`, CMake picks a generator it cannot run; there the configure line is `cmake -S . -B build -G "MinGW Makefiles" --fresh -DPR32_APU_BUILD_TESTS=ON`.

**Measured on 2026-10-27: 467 cases across 9 targets**, `0 Failures 0 Ignored` in every one, `100% tests passed out of 9`. These are Unity's own summary lines, taken by running each binary from `test/`:

| Target | Cases | Suite |
|---|---|---|
| `test_apu_core` | 182 | Engine core behaviour — float core |
| `test_apu_core_integer` | 182 | The same source against the integer core (`PR32_APU_HAS_FPU=0`) |
| `test_audio_command_queue` | 10 | SPSC queue: capacity, drop policy, `AudioCommand` ABI pins |
| `test_audio_music_types` | 35 | Music types, `InstrumentPreset` layout, shipped presets |
| `test_apu_mirror` | 37 | Tool Suite parity, byte-for-byte against the float golden |
| `test_q15_smoke` | 15 | Integer/Q15 path smoke coverage |
| `test_apu_integer_golden` | 1 | Integer core against the integer golden (`Nes`, FPU build) |
| `test_apu_integer_golden_no_fpu` | 1 | Integer core against the integer golden (`Legacy`, no-FPU build) |
| `test_library_link` | 4 | The public surface links from the shipped artifact (no `UNIT_TEST`) |
| **Total** | **467** | |

Two targets run the same test source against a different render core, which is why the case count is larger than the number of distinct behaviours. `test_apu_mirror` and both `test_apu_integer_golden*` targets compare rendered PCM against the two immutable fixtures in `test/golden/fixtures/`.

## Tools

| Tool | What it does |
|---|---|
| `tools/golden_gen` | Renders the canonical track (44 100 Hz, 22 050 samples) with a chosen profile and writes raw S16LE PCM. `python tools/golden_gen/run.py` verifies the float golden and the integer one; `--write` regenerates the integer fixture. |
| `tools/apu_bench` | Host CPU baseline for both cores: p50/p99 µs per 512-sample block and µs per sample over `{Legacy, Nes} × {22 050, 32 000} × {1, 8 voices}`. No CMake; `python tools/apu_bench/run.py`. |
| `tools/apu_bench_firmware` | The same matrix on real boards (PlatformIO, `esp_timer_get_time()`), plus the target `sizeof` of the core. |
| `tools/loudness_probe` | Renders a real track through both routes and compares their levels. |

The two goldens are `test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm` (float route, `7FF568A5…5DE9`) and `test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm` (integer core, `F59501D6…3902D`).

## Documentation

- `docs/ABI.md` — ABI contracts: non-owning pointer lifetimes, automation precedences and the profile/voice/note order, render-path selection, voice stealing policy, queue budget, measured baselines.
- `docs/CHANGELOG.md` — Versioned history (semver).

## License

MIT — see `LICENSE`.
