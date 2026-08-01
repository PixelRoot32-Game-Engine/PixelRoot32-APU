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

Two render paths selected at compile time (`PR32_APU_HAS_FPU`, see `ApuConfig.h`): **float** (desktop, classic ESP32/S3) and **integer Q15 + LUT** (ESP32-C3 / RISC-V without FPU). The Q15 path has native CI coverage (`test_q15_smoke` with `-DPR32_APU_HAS_FPU=0`).

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
//   PR32_APU_HAS_FPU=0|1, PR32_APU_ENABLE_PROFILING=0|1,
//   AUDIO_SEQUENCER_MAX_NOTES, AUDIO_COMMAND_QUEUE_CAPACITY,
//   PIXELROOT32_DEBUG_MODE
```

## Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

150 cases across 5 targets: suites migrated from the engine (`test_apu_core`, `test_audio_command_queue`, `test_audio_music_types`), the behavioral parity suite migrated from the Tool Suite (`test_apu_mirror`, with byte-for-byte golden PCM) and the native Q15 path smoke test.

## Documentation

- `docs/ABI.md` — ABI contracts: non-owning pointer lifetimes, automation precedences, voice stealing policy, limits.
- `docs/CHANGELOG.md` — Versioned history (semver).

## License

MIT — see `LICENSE`.
