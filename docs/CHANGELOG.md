# Changelog — PixelRoot32-APU

Format: [semver](https://semver.org/). Changes that alter rendered PCM or struct ABI require a major or minor bump with an explicit note.

## v1.0.0 — 2026-08-01

First version as a standalone library. Origin: PixelRoot32 Game Engine commit `a3ac998` (branch `refac/extract-apu`), which in turn incorporates Phase 0 convergence with the Tool Suite FPU mirror (`fc763b8`).

### Extracted from the engine
- Complete `ApuCore` (synthesis, sequencer, mixer, transport) with both render paths (float and Q15+LUT).
- Types: `AudioTypes.h`, `AudioMusicTypes.h`, `AudioCommandQueue.h`, `AudioOscLUT.h`, `AudioMixerLUT.h`.

### New
- `ApuConfig.h`: unified FPU detection (`PR32_APU_HAS_FPU`, replaces the misaligned pair `SOC_CPU_HAS_FPU` / `config::HasFPU`), injectable logging hook (`apuSetLogHandler`, replaces `core/Log.h`) and configurable profiling (`PR32_APU_ENABLE_PROFILING`, replaces `platforms::config::EnableProfiling`).
- Dual packaging: `library.json` (PlatformIO) + `CMakeLists.txt` (target `PixelRoot32::APU`).
- Own test suite (150 cases): engine suites (`test_apu_core` 68, `test_audio_command_queue` 7, `test_audio_music_types` 31), Tool Suite parity suite (`test_apu_mirror` 37, with golden PCM) and `test_q15_smoke` (7) — first Q15 path coverage, run natively with `-DPR32_APU_HAS_FPU=0`.

### No Behavioral Changes
- The Tool Suite canonical golden PCM passes byte-for-byte against this library.
- Documented divergence preserved: −6 dB pad in the Q15 path (see `docs/ABI.md §6`).
