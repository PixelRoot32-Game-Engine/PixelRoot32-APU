# golden_gen — integer-core golden PCM (S9)

Renders a golden scene with a sound profile and writes raw S16LE PCM, no
header: the same format as the golden fixtures. Two scenes exist, both 22 050
samples at 44 100 Hz:

| `--scene` | Source | What it pins |
|---|---|---|
| `canonical` (default) | `test/golden/apu_mirror_canonical_track.h` | The frozen PULSE track every fixture from E1 and earlier was rendered from |
| `wavetable` | `test/golden/apu_wavetable_track.h` | E2's band-limited SAW: four SAW notes, a level boundary crossed inside one note |

The profile decides **which render body** runs (S3), and that is exactly the
property this harness pins: the integer body must produce the same bytes in any
build that compiles it. That is why the same `main.cpp` is built three times,
changing only the core configuration.

```bash
python tools/golden_gen/run.py          # verify the canonical scene
python tools/golden_gen/run.py --write  # regenerate the integer fixture
```

---

## The two scenes

`canonical` is the default, and the default is load-bearing: every invocation
recorded before the scene option existed has to keep rendering the same bytes,
so `--scene canonical` is byte-for-byte the pre-option behaviour and no
existing fixture may move.

`wavetable` exists because the canonical track is PULSE-only. E2's band-limited
SAW (S4) is `Hybrid`-only and invisible to a pulse track, so `Hybrid`'s
canonical fixture is identical before and after the mipmap landed; the only
thing that pinned it was a unit test whose reference re-derives the read. The
wavetable scene renders four SAW notes through the same public command API:

| Note | Start | Renders | Mipmap level |
|---|---|---|---|
| 55 Hz, 0.09 s | sample 0 | `0..4409` | 0 (below the first band, clamped) |
| 220 Hz → 900 Hz glide, 0.1 s | sample 4 410 | `4410..9701` | 1 → 2 → 3 |
| 880 Hz, 0.1 s | sample 9 702 | `9702..14993` | 3 |
| 1 760 Hz, 0.1 s | sample 14 994 | `14994..22049` | 4 |

The glide is the S6 case: a linear pitch envelope crosses the 344.5 Hz and
689.0 Hz band edges inside one note, which is the seam the crossfade exists to
remove. The instrument is flat (one-sample attack, no decay, sustain 1.0, no
release) so `Nes` and `Hybrid` differ in this scene only by the SAW generator —
same mix weight, same output chain, no envelope-curve difference to confuse the
comparison.

## The three builds

| Target | Defines | Body rendered by `--profile legacy` |
|---|---|---|
| `apu_golden_gen` | defaults (`PR32_APU_HAS_FPU` autodetects to 1 on host) | frozen float |
| `apu_golden_gen_nofpu` | `PR32_APU_HAS_FPU=0` | integer (the path the C3 runs today) |
| `apu_golden_gen_integer` | `PR32_APU_HAS_FPU=1 PR32_APU_ENABLE_LEGACY_FLOAT=0` | integer (the S2.5 lever) |

All three are built from the `PR32_APU_BUILD_TESTS` block of `CMakeLists.txt`, so
a single `cmake --build build` leaves them ready next to the suites.

`Nes` and `Hybrid` always render the integer core, so all three builds must emit
the same bytes for either scene and either profile. `run.py` exercises that for
the canonical scene; for the wavetable scene, compare the three renders by hash
(the regeneration commands below do exactly that by hand).

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

Regenerating the wavetable fixtures (only when E2's SAW definition changes
intentionally, and never to make a red test green):

```bash
cmake --build build --target apu_golden_gen_nofpu
./build/apu_golden_gen_nofpu --profile nes    --scene wavetable \
    --out test/golden/fixtures/apu_nes_wavetable_s16le_44100.pcm
./build/apu_golden_gen_nofpu --profile hybrid --scene wavetable \
    --out test/golden/fixtures/apu_hybrid_wavetable_s16le_44100.pcm
```

`Nes` and `Hybrid` both run the integer core, so `apu_golden_gen` and
`apu_golden_gen_integer` must reproduce those two files exactly; a difference
means a build dependency survived in the integer core.

## Relation to `test_apu_integer_golden`

The fixture this harness writes is the one consumed by
`test/test_apu_integer_golden.cpp`, compiled twice: with
`PR32_APU_ENABLE_LEGACY_FLOAT=0` and without an FPU. Both targets select `Legacy`
and reach the integer core, because in the first build the float body is compiled
out and in the second it does not exist. That is what makes the two targets assert
the same file without either of them depending on a profile name.

E1 also added `test_apu_profile_golden`, which pins one canonical fixture per
audible profile (`apu_nes_canonical_...pcm`, `apu_hybrid_canonical_...pcm`), and
E2 added the wavetable pair (`apu_nes_wavetable_...pcm`,
`apu_hybrid_wavetable_...pcm`) so both targets also cover the band-limited SAW.
The canonical fixtures were rendered from `--scene canonical` (the default) and
the wavetable ones from `--scene wavetable`; all four are regenerated only when a
phase changes that profile's definition, never to make a red test green.
