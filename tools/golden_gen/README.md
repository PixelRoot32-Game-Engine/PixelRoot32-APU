# golden_gen — dorado del núcleo entero (S9)

Renderiza el track canónico de `test/golden/apu_mirror_canonical_track.h`
(44 100 Hz, 22 050 muestras) con un perfil de sonido y escribe PCM S16LE crudo,
sin cabecera: el mismo formato que los fixtures dorados.

El perfil decide **qué cuerpo de render** corre (S3). Y esa es justamente la
propiedad que este harness fija: el cuerpo entero tiene que dar los mismos bytes
en cualquier build que lo compile. Por eso el mismo `main.cpp` se compila tres
veces, cambiando solo la configuración del núcleo.

```bash
python tools/golden_gen/run.py          # verifica
python tools/golden_gen/run.py --write  # regenera el fixture entero
```

---

## Los tres builds

| Objetivo | Definiciones | Qué cuerpo renderiza `--profile legacy` |
|---|---|---|
| `apu_golden_gen` | por defecto (`PR32_APU_HAS_FPU` se autodetecta a 1 en host) | float congelado |
| `apu_golden_gen_nofpu` | `PR32_APU_HAS_FPU=0` | entero (la ruta que hoy corre el C3) |
| `apu_golden_gen_integer` | `PR32_APU_HAS_FPU=1 PR32_APU_ENABLE_LEGACY_FLOAT=0` | entero (la palanca de S2.5) |

Los tres se construyen desde el bloque `PR32_APU_BUILD_TESTS` de
`CMakeLists.txt`, así que un solo `cmake --build build` los deja listos junto
con las suites.

## Las tres comprobaciones

`run.py` construye los tres binarios, renderiza a un directorio temporal del
sistema y compara en este orden:

1. **El tool reproduce el fixture histórico.** `apu_golden_gen --profile legacy`
   tiene que ser **byte a byte** `test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm`.
   Es una puerta, no una tolerancia: si no coincide, el run se detiene y lo
   reporta con el diff. Un generador que no reproduce la referencia congelada no
   sirve para producir otra.
2. **El núcleo entero no depende del build.** `apu_golden_gen_nofpu --profile legacy`
   y `apu_golden_gen_integer --profile legacy` tienen que ser **byte a byte**
   iguales. Si difieren, quedó una ruta que todavía usa float: se arregla el
   núcleo, no el dorado.
3. **El fixture entero describe ese render.** Sin `--write`, el archivo
   `test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm` (44 100 B)
   tiene que coincidir byte a byte con el render. Con `--write` se reescribe
   desde el build `nofpu`.

Imprime el SHA-256 de los tres renders, del fixture float de referencia y del
fixture entero, y termina con una línea `OK` o `FAIL` (exit code 0/1).

## Comandos exactos

```bash
cmake -S . -B build -G "MinGW Makefiles" --fresh -DPR32_APU_BUILD_TESTS=ON
cmake --build build --target apu_golden_gen apu_golden_gen_nofpu apu_golden_gen_integer
./build/apu_golden_gen --profile legacy --out test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm
```

`run.py` es la vía recomendada: hace esos pasos y además las tres comprobaciones.
El render directo no compara nada.

## Relación con `test_apu_integer_golden`

El fixture que este harness escribe es el que consume `test/test_apu_integer_golden.cpp`,
compilado dos veces: con FPU elige el perfil `Nes` (el `Legacy` de ese build es
float) y sin FPU elige `Legacy`. En E0 ambos son alias exactos del mismo cuerpo
(S3), así que los dos objetivos afirman el mismo archivo.
