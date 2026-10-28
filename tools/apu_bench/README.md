# apu_bench — línea base de CPU de los dos núcleos (S11 / T7)

Mide lo que cuesta **un bloque de render de 512 muestras** en cada núcleo, en
el escritorio, con los dos núcleos viviendo en el **mismo binario**.

La comparación es entre núcleos, no entre builds: `Legacy` ejecuta el cuerpo
float y `Nes` el cuerpo entero, y la única palanca que se usa para elegirlo es
`setSoundProfile`, la API pública. Un banco que llamara a `renderFloatLegacy` /
`renderIntegerCore` directamente mediría una función que él mismo inventó, no
el despacho por el que pasa un juego.

```bash
python tools/apu_bench/run.py
python tools/apu_bench/run.py --opt -O2     # para separar optimización de núcleo
```

Requiere un compilador C++17 en el PATH (`g++`, `clang++` o `c++`). No hay
CMake: `run.py` compila `main.cpp` + `src/ApuCore.cpp` directamente, igual que
`tools/loudness_probe`. El binario se construye en un directorio temporal del
sistema y se borra al terminar: **este tool no escribe nada dentro del repo**.

---

## La matriz

| Perfil | Tasa | Voces |
|---|---|---|
| `Legacy` (cuerpo float) | 22 050 Hz, 32 000 Hz | 1, 8 |
| `Nes` (cuerpo entero) | 22 050 Hz, 32 000 Hz | 1, 8 |

Ocho celdas. Cada una: `init(tasa)`, `setSoundProfile(perfil)`, arrancar las
voces, **100 bloques de calentamiento descartados** y **1 000 bloques
medidos**. Las dos tasas son las del plan; 512 muestras es el `blockSize` que
S12 fija para las plataformas con FPU, así que el bloque medido es el que el
firmware renderiza de verdad.

## Cómo se garantizan las 8 voces

El pool tiene 8 ranuras, pero no las reparte cualquiera: las ranuras 0–3 son del
secuenciador y `PLAY_EVENT` solo asigna 4–7 (`findVoiceForSfxEvent` recorre
`SFX_VOICE_BASE..MAX_VOICES`). Un banco que arrancara ocho eventos de efecto se
quedaría en 4 voces sin avisar. Por eso:

- **Ranuras 0–3 — música.** Un `MUSIC_PLAY` con la pista principal y tres
  subpistas (`subTracks[3]`), una por ranura. Cada pista tiene **una sola nota de
  128 pulsos**: a los 150 BPM por defecto son 51,2 s, y la ventana completa
  (1 100 bloques = 563 200 muestras) dura 25,5 s a 22 050 Hz y 17,6 s a
  32 000 Hz. La compuerta de la nota no puede expirar dentro de la corrida, así
  que el secuenciador no la retriggerea a mitad de medición.
- **Ranuras 4–7 — efectos.** Cuatro `PLAY_EVENT` con `loop = true` y formas de
  onda distintas, para que `findVoiceForSfxEvent` entregue 4, 5, 6 y 7 en vez de
  reutilizar una. Con `gate_samples == 0` y `loop`, `initVoiceFromEvent` pone
  `remainingSamples = UINT64_MAX` y el vencimiento por muestra nunca dispara.

No hay retrigger: nada se apaga, así que no hace falta rearmar cada 100 bloques.
La garantía es estructural (compuerta y loop que no vencen) y se comprobó en T7
con una sonda con `UNIT_TEST` fuera del árbol: **8 voces habilitadas antes y
después de los 1 100 bloques, a las dos tasas** (1 voz, 1 habilitada). Si algún
día se acorta la nota o se quita el `loop`, la celda de 8 voces bajaría a 4 y el
número saldría más bonito por la razón equivocada.

## Qué mide cada columna

| Columna | Qué es | Para qué sirve |
|---|---|---|
| `p50_us` | Mediana de los 1 000 tiempos de bloque | **El coste del núcleo.** Es la cifra reproducible. |
| `p99_us` | Percentil 99 por **rango más cercano** | Peor caso real de la corrida, no una interpolación. |
| `us_per_sample` | `p50 / 512` | Comparable con el presupuesto por muestra de la placa. |
| `budget_us` | `512 × 1e6 / tasa` | Tiempo de audio que ese bloque representa. |
| `headroom` | `budget / p99` | «x veces tiempo real»: cuánto más rápido que el reloj. |

El percentil es de rango más cercano (`ceil(p/100 × N) − 1`) a propósito: un
tiempo de bloque es una observación, no una distribución, así que `p99` tiene
que ser un bloque que ocurrió de verdad. Con 1 000 bloques es el 990.º valor.

El render no se puede elidir: cada celda acumula un checksum `int64_t` de las
512 muestras de cada bloque medido y lo imprime en la última columna de la tabla
de stderr. Sin un lector, un compilador podría razonar que nadie usa el buffer y
medir un bucle borrado, que es rapidísimo y no existe.

## Por qué `-Os`

Es el nivel del build de firmware, así que la línea base del escritorio y la de
la placa (T8) comparan lo mismo. `--opt` existe para responder otra pregunta:
si una diferencia entre núcleos desaparece con `-O2`, es del optimizador y no
del núcleo.

## Cómo se lee el resultado

- **`p50` es la medida; `p99` es la cola del sistema operativo.** En un
  escritorio Windows no hay hilo de audio con prioridad ni CPU reservada: el
  `p99` engancha el bloque que perdió la CPU a manos de otro proceso. Medido en
  T7 sobre dos corridas consecutivas: `p50` se movió ≤ 8,7 % en las ocho celdas
  (dentro del ±10 % que pide S11), mientras `p99` se movió hasta un 28,9 % y con
  signos opuestos entre celdas. Las celdas de **1 voz** son las más ruidosas en
  términos relativos (su coste absoluto es de 16–24 µs, así que unos pocos µs de
  jitter pesan mucho: una pasada posterior dio 18,7 µs en `Legacy 22050 v1`,
  +14 %), y las de **8 voces** —las que importan para el presupuesto— se movieron
  ≤ 8,2 %. No es inestabilidad del núcleo; es el host. En la placa lo que manda
  es el contador de underruns (S13), no este percentil.
- **`headroom` con `p99`** es el número conservador: si el peor bloque entra en
  el presupuesto, la celda no puede fallar por CPU.
- **El coste por bloque no depende de la tasa** (mismo número de muestras), pero
  el coste **por segundo de audio** sí: a 32 000 Hz el bloque dura 16 ms en vez
  de 23,22 ms, así que se renderizan 1,45× más bloques por segundo. Por eso el
  headroom a 32 000 Hz es ≈ 0,69× el de 22 050 Hz.

## Qué NO son estas cifras

Son cifras de **host x86_64**, medidas con el toolchain de escritorio. No dicen
qué hace un ESP32: no hay flash, ni caché de instrucciones compartida, ni
`i2s_write`, ni underruns. Eso es `tools/apu_bench_firmware` (T8) y se registra
por placa en `docs/ABI.md` §9.

El tool **no tiene umbral**: mide, no aprueba ni reprueba. Una celda lenta es un
dato, no un fallo de build. T7 registra la línea base; la puerta de rendimiento
son las condiciones de S15, no este binario.

## Salida

- **stdout**: CSV, cabecera exacta `profile,rate,voices,p50_us,p99_us,us_per_sample`,
  una fila por celda, tres decimales. `apu_bench > baseline.csv` queda limpio.
- **stderr**: la tabla humana con el presupuesto y el headroom, y el bloque
  `sizeof` (`AudioCore`, `AudioCommand`, `AudioChannel`, `InstrumentPreset` y el
  presupuesto de la cola, `CAPACITY × sizeof(AudioCommand)`).
- **`run.py`** reimprime el CSV, una tabla Markdown lista para pegar en el
  documento del plan y las líneas de `sizeof`.

`sizeof(AudioCore)` es el nombre que piden S11 y §4; el tipo se llama `ApuCore`
(`include/pixelroot32/apu/ApuCore.h`). La línea imprime el nombre pedido y
declara el real, para que la discrepancia se vea en vez de corregirse en
silencio.
