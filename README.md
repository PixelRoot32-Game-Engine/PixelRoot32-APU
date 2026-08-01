# PixelRoot32-APU

Núcleo de audio NES-style compartido (**APU — Audio Processing Unit**) de la familia PixelRoot32. Es la fuente única de verdad de la síntesis que consumen:

- **PixelRoot32 Game Engine** (firmware ESP32 / nativo, vía PlatformIO)
- **PixelRoot32 Tool Suite** (editores de escritorio Music/SFX, vía CMake)

Sustituye al antiguo "mirror FPU" mantenido a mano en el Tool Suite: desde la versión 1.0.0 ambos proyectos enlazan esta librería y la paridad se garantiza por versión, no por copia manual.

## Contenido

| Componente | Descripción |
|---|---|
| `ApuCore` | 8 voces (4 música + 4 SFX), secuenciador musical, mezclador con soft-clip + HPF, ADSR, LFO, sweeps, duty stepped, pitch envelope, bitcrush, transporte (`MUSIC_SEEK`, ticks atómicos) |
| `AudioTypes.h` | `AudioEvent`, `Voice`, `EnvelopeState`, `SfxBreakpoint`, `AudioCommand` |
| `AudioMusicTypes.h` | `MusicNote`, `MusicTrack`, `InstrumentPreset` + presets `INSTR_*` canónicos |
| `AudioCommandQueue.h` | Cola SPSC lock-free sin asignaciones |
| `AudioOscLUT.h` / `AudioMixerLUT.h` | LUTs de seno Q15 y curva de compresión entera |
| `ApuConfig.h` | Superficie de configuración: detección de FPU, hook de logging, profiling |

Dos rutas de render seleccionadas en compilación (`PR32_APU_HAS_FPU`, ver `ApuConfig.h`): **float** (escritorio, ESP32 clásico/S3) y **entera Q15 + LUT** (ESP32-C3 / RISC-V sin FPU). La ruta Q15 tiene cobertura nativa en CI (`test_q15_smoke` con `-DPR32_APU_HAS_FPU=0`).

## Consumo

### PlatformIO (Game Engine)

```ini
lib_deps = <git-url-o-ruta>/PixelRoot32-APU#v1.0.0
```

```cpp
#include <pixelroot32/apu/ApuCore.h>
```

### CMake (Tool Suite / escritorio)

```cmake
add_subdirectory(PixelRoot32-APU)   # o FetchContent con GIT_TAG v1.0.0
target_link_libraries(mi_app PRIVATE PixelRoot32::APU)
```

La librería es C++17 puro, sin dependencias (ni Arduino, ni FreeRTOS, ni SDL, ni PlatformIO): cada empaquetado es inerte para el otro ecosistema.

## Integración con el host

```cpp
using namespace pixelroot32::audio;

// Logging (opcional; por defecto silencio — RT-safe obligatorio):
apuSetLogHandler([](ApuLogLevel lvl, const char* msg) { /* ... */ });

// Flags de build (ver ApuConfig.h):
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

150 casos en 5 targets: las suites migradas del engine (`test_apu_core`, `test_audio_command_queue`, `test_audio_music_types`), la suite de paridad conductual migrada del Tool Suite (`test_apu_mirror`, con golden PCM byte a byte) y el smoke de la ruta Q15 nativa.

## Documentación

- `docs/ABI.md` — contratos del ABI: vida de punteros no-owning, precedencias de automatización, política de robo de voces, límites.
- `docs/CHANGELOG.md` — historial versionado (semver).

## Licencia

MIT — ver `LICENSE`.
