# apu_bench_firmware — línea base de CPU en placa (S11 / T8)

Mide lo que cuesta **un bloque de render de 512 muestras** en un ESP32, con
`esp_timer_get_time()`, y lo imprime por el puerto serie como CSV.

Es el hermano de placa de `tools/apu_bench` (host): **la misma matriz, el mismo
calentamiento, el mismo percentil y el mismo arranque de voces**, porque dos
bancos que midieran cosas distintas darían dos tablas que nadie puede comparar.

```bash
cd tools/apu_bench_firmware
pio run                                       # solo esp32dev (default_envs)
pio run -e esp32dev -e esp32-c3-devkitm-1 -e esp32-s3-devkitc-1
pio run -e esp32dev -t upload                 # flashear
pio device monitor -b 115200 > bench.csv      # capturar
```

El proyecto es independiente: no entra en el CMake de la raíz.

---

## Qué mide

| Perfil | Tasa | Voces |
|---|---|---|
| `Legacy` (cuerpo float si el objetivo tiene FPU; entero si no) | 22 050 Hz, 32 000 Hz | 1, 8 |
| `Nes` (cuerpo entero) | 22 050 Hz, 32 000 Hz | 1, 8 |

Ocho celdas. Cada una: `init(tasa)`, `setSoundProfile(perfil)`, arrancar las
voces, **100 bloques descartados** y **1 000 medidos**. El reloj envuelve
`generateSamples(buffer, 512)` y nada más; el checksum de las 512 muestras se
acumula **fuera** del intervalo cronometrado, para que el compilador no pueda
elidir el render y para que el checksum no se mida a sí mismo.

Los percentiles son de **rango más cercano** (`ceil(p/100 × N) − 1`): un tiempo
de bloque es una observación, así que el p99 tiene que ser un bloque que ocurrió.
`us_per_sample = p50 / 512`.

En un objetivo sin FPU (`esp32-c3-devkitm-1`) `Legacy` y `Nes` ejecutan **el
mismo cuerpo entero**, así que sus dos filas miden lo mismo. La herramienta no
lo esconde: imprime los dos macros de render en la línea de identidad.

## Qué NO mide

- **No mide latencia de I2S ni del DAC.** No abre ningún periférico: renderiza a
  un buffer y lo tira.
- **No cuenta underruns.** Eso lo da el contador del backend en una corrida real
  (0 en 10 minutos por placa y por tasa, S13); aquí no hay backend, ni DMA, ni
  reloj de audio que perseguir.
- **No tiene umbral ni veredicto.** Mide; la puerta de rendimiento son las
  condiciones de S15.

## Cómo se garantizan las 8 voces

Igual que en el banco de host, y por la misma razón: las ranuras 0–3 son del
secuenciador y `PLAY_EVENT` solo asigna 4–7 (`findVoiceForSfxEvent`). Ocho
eventos de efecto se quedarían en 4 voces sin avisar. Por eso:

- **Ranuras 0–3 — música.** Un `MUSIC_PLAY` con pista principal y tres subpistas,
  cada una con **una sola nota de 128 pulsos** (51,2 s a los 150 BPM por
  defecto), frente a los 25,5 s / 17,6 s que dura la corrida: la compuerta no
  puede expirar dentro de la medición.
- **Ranuras 4–7 — efectos.** Cuatro `PLAY_EVENT` con `loop = true` y formas de
  onda distintas, para que el asignador entregue 4, 5, 6 y 7 en vez de reutilizar
  una.

La garantía es estructural (compuerta y `loop` que no vencen), la misma que en
host, donde T7 la comprobó con una sonda: 8 voces habilitadas antes y después de
los 1 100 bloques. En placa no se cuenta nada: si una celda de 8 voces sale
anómalamente barata, ese es el primer sitio donde mirar.

## Salida

Todo por serie a 115 200 baudios, en este orden:

1. **Una línea de identidad** que empieza por `# `: modelo y revisión del chip,
   MHz de CPU, `PR32_APU_HAS_FPU`, `PR32_APU_ENABLE_LEGACY_FLOAT`, `sizeof` de
   `ApuCore` / `AudioCommand` / `AudioChannel` / `InstrumentPreset` y el
   presupuesto de la cola (`CAPACITY × sizeof(AudioCommand)`). El `sizeof` de
   objetivo es el que S8 manda registrar por placa en vez de fijarlo con una
   constante.
2. **La cabecera CSV exacta**: `profile,rate,voices,p50_us,p99_us,us_per_sample`.
3. **Una fila por celda**, y detrás de cada fila **su línea `# `** con el
   presupuesto del bloque, el headroom (`budget / p99`) y el checksum.

Todo lo que no es una fila CSV empieza por `#`, así que:

```bash
grep -v '^#' bench.csv     # CSV limpio
grep '^#' bench.csv        # metadatos: identidad y por celda
```

El arranque del chip imprime basura del ROM antes de la primera línea; se
descarta quedándose con las líneas que empiezan por `#` o por `profile,` y con
las ocho filas de datos.

Los números se imprimen con aritmética entera (`x.yyy`), no con `%f`: el reloj de
la placa da microsegundos enteros y así la herramienta no depende del soporte de
coma flotante de `newlib`, que aquí no se puede verificar sin placa. Por eso
`p50_us` sale siempre con `.000`: la resolución de `esp_timer_get_time()` es 1 µs.

## Resultados

**Medición en placa: pendiente.** El 2026-10-27 no había placa conectada
(`pio device list` solo ve `COM1`), así que no se ha medido ni una celda de este
banco. No hay cifras estimadas en este archivo ni en el informe de T8.

Cuando haya placa, las cifras se transcriben a **`docs/ABI.md` §9** en T12, por
placa y por tasa. Una celda sin medir se deja vacía y marcada `pendiente`; no se
rellena con una estimación. Lo mismo vale para el contador de underruns de S13.

## Desviaciones respecto de la ficha de T8

1. **`lib_deps = symlink://../..` en lugar de `file://../..`.** Este proyecto
   vive *dentro* de la biblioteca que mide, y PlatformIO instala un `file://`
   con `shutil.copytree` de la carpeta entera: copiaría el repositorio (`.git`
   incluido) a `.pio/libdeps/<env>/` en cada env y otra vez en cada rebuild,
   llevándose el propio `.pio` del proyecto dentro. El primer intento murió con
   `Permission denied` sobre `tools/apu_bench_firmware/.pio/libdeps/esp32dev.lock`.
   `symlink://` en PlatformIO **no es un symlink del sistema**: escribe la ruta
   en un `.pio-link` y compila la biblioteca en su sitio, así que la medición
   solo puede describir este árbol de trabajo.
2. **`-std=gnu++17` + `build_unflags = -std=gnu++11`.** El framework Arduino de
   `espressif32` compila con `gnu++11` y la biblioteca declara `-std=gnu++17` en
   su `library.json` (las reglas de agregado relajadas de C++14 son lo que hace
   compilar `MusicNote{...}`). El último `-std` de la línea de comandos gana, y
   el del framework llega después de `build_flags`, así que hay que quitarlo.
3. **El `ApuCore` vive en almacenamiento estático** (`gCoreStorage` +
   placement new por celda), no en una local: `sizeof(ApuCore)` es de **9 800 B**
   en el ABI de 32 bits de las tres placas (13 200 B en host, L16) y la tarea
   `loop` que ejecuta `setup()` tiene 8 KB de pila. La garantía del banco de
   host —cada celda empieza con un núcleo recién construido— se conserva; lo que
   cambia es dónde está la memoria. Lo mismo con el array de 1 000 tiempos de
   bloque.
4. **`submit()` no puede llamar a `std::exit`.** Si la cola rechaza un comando,
   imprime `error: command queue rejected a command` y se queda en `delay()` en
   vez de medir con un número de voces que no es el del CSV.
