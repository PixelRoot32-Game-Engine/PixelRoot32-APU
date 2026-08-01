# Changelog — PixelRoot32-APU

Formato: [semver](https://semver.org/). Cambios que alteren el PCM renderizado o el ABI de structs requieren bump mayor o minor con nota explícita.

## v1.0.0 — 2026-08-01

Primera versión como librería independiente. Origen: PixelRoot32 Game Engine commit `a3ac998` (rama `refac/extract-apu`), que a su vez incorpora la convergencia de la Fase 0 con el mirror FPU del Tool Suite (`fc763b8`).

### Extraído del engine
- `ApuCore` completo (síntesis, secuenciador, mezclador, transporte) con ambas rutas de render (float y Q15+LUT).
- Tipos: `AudioTypes.h`, `AudioMusicTypes.h`, `AudioCommandQueue.h`, `AudioOscLUT.h`, `AudioMixerLUT.h`.

### Nuevo
- `ApuConfig.h`: detección de FPU unificada (`PR32_APU_HAS_FPU`, sustituye al par desalineado `SOC_CPU_HAS_FPU` / `config::HasFPU`), hook de logging inyectable (`apuSetLogHandler`, sustituye a `core/Log.h`) y profiling configurable (`PR32_APU_ENABLE_PROFILING`, sustituye a `platforms::config::EnableProfiling`).
- Empaquetado dual: `library.json` (PlatformIO) + `CMakeLists.txt` (target `PixelRoot32::APU`).
- Suite de tests propia (150 casos): suites del engine (`test_apu_core` 68, `test_audio_command_queue` 7, `test_audio_music_types` 31), suite de paridad del Tool Suite (`test_apu_mirror` 37, con golden PCM) y `test_q15_smoke` (7) — primera cobertura de la ruta Q15, ejecutada en nativo con `-DPR32_APU_HAS_FPU=0`.

### Sin cambios de comportamiento
- El golden PCM canónico del Tool Suite pasa byte a byte contra esta librería.
- Divergencia documentada que se conserva: pad −6 dB de la ruta Q15 (ver `docs/ABI.md §6`).
