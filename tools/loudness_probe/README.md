# loudness_probe — calibración de la ruta Q15 (Hito 5 / M12)

Renderiza un track real del `music-demo` por las **dos** rutas de la APU y
compara los niveles.

La ruta se elige en tiempo de compilación con `PR32_APU_HAS_FPU`
(`ApuConfig.h`), que es una definición común. Así que la aritmética entera
exacta que ejecuta el ESP32-C3 se puede correr acá, en el escritorio: **no hace
falta el hardware para la parte que importa.**

```bash
python tools/loudness_probe/run.py
python tools/loudness_probe/run.py --track blaster_ridge --volume 0.8
python tools/loudness_probe/run.py --wav-dir out/     # para el A/B auditivo
```

Tracks: `moonwell_hymn`, `lightworld_march`, `blaster_ridge`, `coinleaf_grove`.

Requiere un compilador C++17 en el PATH y `PixelRoot32-Game-Engine` como
checkout hermano (de ahí salen los assets).

---

## Cómo leer el resultado

| Métrica | Para qué sirve |
|---|---|
| **rms ratio** | **La medida de loudness.** Es lo que M12 arregló: antes daba ~0.225. |
| peak ratio | Informativo. Es un estadístico de **una sola muestra**. |
| rail samples | Muestras clavadas en ±32767. **Tiene que ser 0.** |
| peak dBFS | Headroom que queda. |

### Por qué el pico no es un criterio

Las dos rutas nunca fueron bit-idénticas. La float evalúa la curva del
compresor de forma continua; la Q15 la busca en `audio_mixer_lut`: 1025
entradas indexadas por `(sum + 131072) >> 8`, **sin interpolación** — unos 102
counts de salida por escalón cerca de cero.

Sobre material real de varias voces eso da unos puntos porcentuales de
diferencia. Medido sobre los cuatro tracks del demo:

| track | rms ratio | peak ratio |
|---|---|---|
| moonwell_hymn | 1.013 | 1.145 |
| lightworld_march | 1.064 | 1.066 |
| blaster_ridge | **0.990** | 1.158 |
| coinleaf_grove | 1.094 | 1.117 |

**Lo que distingue cuantización de un bug de ganancia es la dirección.** El RMS
cae a los dos lados del 1.0 (0.990 a 1.094); un error de ganancia sería
unidireccional y constante. El pico, en cambio, sale siempre alto: es el máximo
sobre cientos de miles de muestras, así que engancha sistemáticamente el lado
donde la cuantización del LUT redondea para arriba.

El escalonado de ganancia exacto está fijado aparte y bit-exacto por
`test_q15_softclip_curve_shapes_match_the_float_path`. Este harness no lo
reemplaza: mide el comportamiento sobre material real, que es otra pregunta.

---

## El procedimiento completo

**Fase 1 — objetiva (esto).** Correr `run.py` sobre los cuatro tracks. Buscás:

- `rms ratio` cerca de 1.0 → la paridad del M12 se sostiene.
- `rail samples` en 0 en las dos rutas → no clippea.

Si clippea, bajá `--volume` y repetí hasta llegar a 0. Ese valor es el master
nuevo, y lo obtuviste midiendo en vez de adivinando.

**Fase 2 — subjetiva.** `--wav-dir out/` deja `<track>_float.wav` y
`<track>_q15.wav`. Escuchalos uno detrás del otro: tienen que sonar **al mismo
nivel**. Es mucho más fácil juzgar paridad entre dos cosas que loudness
absoluto.

**Fase 3 — hardware.** Recién ahora, y sabiendo que el mixer ya está
verificado. En el C3 lo que estás evaluando es el amplificador y el parlante:
distorsión en los picos y si el volumen del sistema necesita ajuste.

El orden importa. Si arrancás por la fase 3 y algo suena mal, no vas a poder
distinguir si es el mixer, el amp o el parlante.
