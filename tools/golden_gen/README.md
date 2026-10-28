# golden_gen — integer-core golden PCM (S9)

Renders the canonical track from `test/golden/apu_mirror_canonical_track.h`
(44 100 Hz, 22 050 samples) with a sound profile and writes raw S16LE PCM, no
header: the same format as the golden fixtures.

The profile decides **which render body** runs (S3), and that is exactly the
property this harness pins: the integer body must produce the same bytes in any
build that compiles it. That is why the same `main.cpp` is built three times,
changing only the core configuration.

```bash
python tools/golden_gen/run.py          # verify
python tools/golden_gen/run.py --write  # regenerate the integer fixture
```

---

## The three builds

| Target | Defines | Body rendered by `--profile legacy` |
|---|---|---|
| `apu_golden_gen` | defaults (`PR32_APU_HAS_FPU` autodetects to 1 on host) | frozen float |
| `apu_golden_gen_nofpu` | `PR32_APU_HAS_FPU=0` | integer (the path the C3 runs today) |
| `apu_golden_gen_integer` | `PR32_APU_HAS_FPU=1 PR32_APU_ENABLE_LEGACY_FLOAT=0` | integer (the S2.5 lever) |

All three are built from the `PR32_APU_BUILD_TESTS` block of `CMakeLists.txt`, so
a single `cmake --build build` leaves them ready next to the suites.

## The three checks

`run.py` builds the three binaries, renders into a temporary system directory and
compares in this order:

1. **The tool reproduces the historical fixture.** `apu_golden_gen --profile legacy`
   must be **byte for byte** `test/golden/fixtures/apu_mirror_canonical_s16le_44100.pcm`.
   It is a gate, not a tolerance: on a mismatch the run stops and prints the diff.
   A generator that cannot reproduce the frozen reference is not fit to produce
   another one.
2. **The integer core does not depend on the build.** `apu_golden_gen_nofpu --profile legacy`
   and `apu_golden_gen_integer --profile legacy` must be **byte for byte** equal.
   If they differ, some path still uses float: fix the core, not the golden.
3. **The integer fixture describes that render.** Without `--write`, the file
   `test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm` (44 100 B)
   must match the render byte for byte. With `--write` it is rewritten from the
   `nofpu` build.

It prints the SHA-256 of the three renders, of the reference float fixture and of
the integer fixture, and ends with an `OK` or `FAIL` line (exit code 0/1).

## Exact commands

```bash
cmake -S . -B build -G "MinGW Makefiles" --fresh -DPR32_APU_BUILD_TESTS=ON
cmake --build build --target apu_golden_gen apu_golden_gen_nofpu apu_golden_gen_integer
./build/apu_golden_gen --profile legacy --out test/golden/fixtures/apu_integer_core_canonical_s16le_44100.pcm
```

`run.py` is the recommended path: it performs those steps plus the three checks.
A direct render compares nothing.

## Relation to `test_apu_integer_golden`

The fixture this harness writes is the one consumed by
`test/test_apu_integer_golden.cpp`, compiled twice: with an FPU it selects the
`Nes` profile (that build's `Legacy` is float) and without an FPU it selects
`Legacy`. In E0 both are exact aliases of the same body (S3), so the two targets
assert the same file.
