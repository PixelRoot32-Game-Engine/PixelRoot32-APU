# ABI y contratos de la librería PixelRoot32-APU

Versión de referencia: **v1.0.0** (derivada del Game Engine `a3ac998`, rama `refac/extract-apu`, tras la convergencia de la Fase 0 con el Tool Suite `fc763b8`).

Este documento consolida los contratos que antes vivían repartidos entre `docs/MUSIC_ENGINE_ABI_REFERENCE.md`, `docs/SFX_ENGINE_ABI_REFERENCE.md` (Tool Suite) y los comentarios del engine. Ante conflicto, el código de esta librería es la autoridad.

## 1. Contratos de memoria (punteros no-owning)

El hot path (`generateSamples` / `generateSampleForVoice`) es *pointer-walk* puro, sin asignaciones ni copias al heap. Consecuencia: **todo puntero entregado en un comando debe sobrevivir a la reproducción**.

| Puntero | Entregado en | Debe vivir hasta |
|---|---|---|
| `const MusicTrack*` (+ subTracks) | `MUSIC_PLAY`, `MUSIC_UPDATE_TRACKS` | `MUSIC_STOP` o sustitución |
| `const MusicNote*` (`MusicTrack::notes`) | vía track | ídem |
| `const InstrumentPreset*` (`MusicNote::preset`, `AudioEvent::preset`) | eventos/notas | fin de la voz (incluida cola de release) |
| `const SfxBreakpoint*` (`dutySteps`, `pitchEnvelope`) | `AudioEvent` | fin de la voz |

Los consumidores con presets dinámicos deben garantizar estabilidad de direcciones (el Tool Suite usa `std::deque<InstrumentPreset>`).

## 2. Particionado de voces y robo

- `MAX_VOICES = 8`: slots **0–3** reservados al secuenciador (track ↔ slot 1:1); slots **4–7** para `PLAY_EVENT`/SFX y percusión del secuenciador.
- El robo de voces está confinado a 4–7; **nunca** roba slots de música.
- Ranking de robo: `remainingSamples` menor primero; las voces en loop puntúan **0** (anti-inanición) y son las primeras candidatas.

## 3. Automatización y precedencias

- **Duty stepped** (`dutySteps`, count 1–4): *hold* sin interpolación; mientras está activo se ignora `InstrumentPreset::dutySweep`.
- **Pitch envelope** (`pitchEnvelope`, count 2–4): multi-segmento; con ≥ 2 puntos **sustituye** al sweep de un solo tramo.
- `SfxBreakpoint.timeSec` no decreciente; `value` = duty [0,1] o Hz > 0 según tabla.
- `SweepCurve::Exponential` degrada a `Linear` si los Hz inicial/final no son ambos > 0.
- En NOISE el sweep interpola el reloj/periodo del LFSR, no el pitch melódico.
- Reloj compartido `automationAgeSamples`: un incremento por muestra alimenta duty y pitch.

## 4. Loop y duración

- `loop == true` ⇒ `remainingSamples = UINT64_MAX` (centinela); solo termina por `STOP_CHANNEL` o robo.
- `loop == false && duration <= 0` ⇒ la voz se desactiva de inmediato (garantía anti-cuelgue).
- Secuenciador: `MusicNote.duration == 0` = disparo apilado sin avance temporal (drums en el mismo step). Duración de fallback de presets: `max(duration, 0.05) / tempoFactor`.

## 5. Cola de comandos

`AudioCommandQueue` es **SPSC**: un productor (hilo de UI/juego) y un consumidor (hilo de audio). No soporta múltiples productores concurrentes. Cola llena ⇒ el comando más nuevo se descarta y se incrementa `droppedCommands`.

## 6. Rutas de render

| Ruta | Guard | Targets |
|---|---|---|
| Float | `PR32_APU_HAS_FPU == 1` | Escritorio, ESP32 clásico, ESP32-S3 |
| Entera Q15 + `audio_mixer_lut` | `PR32_APU_HAS_FPU == 0` | ESP32-C3, RISC-V sin FPU (y tests nativos forzados) |

Cadena float: `Σ(voz · MIXER_SCALE=0.4) · masterVolume → soft-clip 1/(1+|x|·0.5) → HPF R=0.995 → ±32767 → bitcrush`. La ruta Q15 replica la misma cadena con la LUT pre-ajustada y el volumen maestro aplicado **antes** del compresor (paridad de orden desde v1.0.0).

**Divergencia conocida y deliberada**: la ruta Q15 aplica un pad fijo de −6 dB (`>>1`, Q15→Q14) sin equivalente float, heredado del engine; se conserva para no alterar el volumen de los targets sin FPU. Pendiente de unificación en una versión mayor.

## 7. Presets canónicos de percusión

`noisePeriod` es el **periodo del LFSR en samples**: mayor periodo ⇒ reloj más lento ⇒ sonido más grave.

| Preset | noisePeriod | Carácter |
|---|---|---|
| `INSTR_KICK` | 60 | grave, con cuerpo |
| `INSTR_SNARE` | 15 | brillante, chasquido |
| `INSTR_HIHAT` | 12 | metálico (short mode) |

## 8. Transporte musical

- `MUSIC_PLAY` ancla `musicPlayStartTick`; `MUSIC_SEEK` (`seekOffsetTicks` = ticks transcurridos desde el inicio de la pieza) reposiciona reconstruyendo los índices por pista (`resyncTrackSequencerToTick`), con soporte de loop y notas apiladas.
- `MUSIC_UPDATE_TRACKS` sustituye las pistas en caliente sin resetear el transporte.
- Observación thread-safe: `getMusicGlobalTick()`, `getMusicPlayStartTick()`, `isMusicPlaying()`, `isMusicPaused()`.
- `TICKS_PER_BEAT = 4`; `MusicNote.duration` en beats (negra = 1.0).

## 9. Verificación

La suite (`ctest`) incluye `test_apu_mirror` con un **golden PCM** (`test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm`): la pista canónica renderizada debe coincidir byte a byte. Regenerarlo **solo** cuando la salida cambie de forma intencional y documentada en el CHANGELOG.
