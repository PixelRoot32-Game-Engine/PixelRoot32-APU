cmake_minimum_required(VERSION 3.16)
project(pixelroot32-apu VERSION 2.0.0 LANGUAGES C CXX)

# The test files have pre-existing narrowing conversions in brace-init
# (committed before M7). Suppress this warning so the test build doesn't
# fail on those lines.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wno-narrowing)
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(pixelroot32-apu STATIC src/ApuCore.cpp)
add_library(PixelRoot32::APU ALIAS pixelroot32-apu)

target_compile_features(pixelroot32-apu PUBLIC cxx_std_17)
target_include_directories(pixelroot32-apu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Configuration knobs (see include/pixelroot32/apu/ApuConfig.h):
#   -DPR32_APU_HAS_FPU=0|1, -DPR32_APU_ENABLE_PROFILING=0|1,
#   -DAUDIO_SEQUENCER_MAX_NOTES=n, -DAUDIO_COMMAND_QUEUE_CAPACITY=n
# are plain compile definitions consumers may set on this target or globally.

# ---------------------------------------------------------------------------
# Tests (top-level builds only, unless forced)
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_pr32_apu_tests_default ON)
else()
    set(_pr32_apu_tests_default OFF)
endif()
option(PR32_APU_BUILD_TESTS "Build PixelRoot32-APU unit tests" ${_pr32_apu_tests_default})

if(PR32_APU_BUILD_TESTS)
    enable_testing()

    add_library(pr32_apu_unity STATIC test/vendor/unity/unity.c)
    target_include_directories(pr32_apu_unity PUBLIC test/vendor/unity)

    # Each test compiles src/ApuCore.cpp itself so UNIT_TEST-only diagnostics
    # (test hooks) are available; the shipped library never contains them.
    function(pr32_apu_add_test name)
        cmake_parse_arguments(ARG "" "" "EXTRA_DEFS" ${ARGN})
        add_executable(${name} test/${name}.cpp src/ApuCore.cpp)
        target_compile_definitions(${name} PRIVATE UNIT_TEST=1 ${ARG_EXTRA_DEFS})
        target_include_directories(${name} PRIVATE
            include test test/support)
        target_link_libraries(${name} PRIVATE pr32_apu_unity)
        add_test(NAME ${name} COMMAND ${name}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)
    endfunction()

    # Float-path suites (migrated from the Game Engine's native_test set)
    pr32_apu_add_test(test_apu_core)
    pr32_apu_add_test(test_audio_command_queue)
    pr32_apu_add_test(test_audio_music_types)

    # Behavioural parity suite + golden PCM (migrated from the Tool Suite
    # mirror; the fixture lives in test/golden/fixtures/)
    pr32_apu_add_test(test_apu_mirror)

    # E2/S1-S3, S8: the `Nes`/`Hybrid` output filter chain — measured response
    # against the theoretical cascade, full-scale stability, the caller's
    # `setHpfCutoffHz` override, and the historical one-pole of `Legacy`.
    # Built twice: the default (float `Legacy`) and the build where `Legacy`
    # renders through the integer core, which is the path the one-pole pin
    # exists for.
    pr32_apu_add_test(test_apu_output_filter)
    add_executable(test_apu_output_filter_integer test/test_apu_output_filter.cpp src/ApuCore.cpp)
    target_compile_definitions(test_apu_output_filter_integer PRIVATE
        UNIT_TEST=1 PR32_APU_ENABLE_LEGACY_FLOAT=0)
    target_include_directories(test_apu_output_filter_integer PRIVATE
        include test test/support)
    target_link_libraries(test_apu_output_filter_integer PRIVATE pr32_apu_unity)
    add_test(NAME test_apu_output_filter_integer COMMAND test_apu_output_filter_integer
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # E2/S4, S5 (and the S6 groundwork): the generated SAW mipmap tables are
    # band-limited per level, the table has no seam of its own, and the
    # mipmapped synthesis folds down at least 10 dB less energy below the
    # fundamental than the ideal ramp it will replace. Nothing is wired into
    # the renderer here, so no golden moves.
    pr32_apu_add_test(test_apu_wavetable_spectral)

    # Q15 / integer path exercised natively — the coverage gap identified in
    # the extraction analysis (no firmware build compiled this path).
    add_executable(test_q15_smoke test/test_q15_smoke.cpp src/ApuCore.cpp)
    target_compile_definitions(test_q15_smoke PRIVATE UNIT_TEST=1 PR32_APU_HAS_FPU=0)
    target_include_directories(test_q15_smoke PRIVATE include test test/support)
    target_link_libraries(test_q15_smoke PRIVATE pr32_apu_unity)
    add_test(NAME test_q15_smoke COMMAND test_q15_smoke
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # The float suite compiled against the integer core: proves the two render
    # paths agree on behaviour, not just on level.
    add_executable(test_apu_core_integer test/test_apu_core.cpp src/ApuCore.cpp)
    target_compile_definitions(test_apu_core_integer PRIVATE UNIT_TEST=1 PR32_APU_HAS_FPU=0)
    target_include_directories(test_apu_core_integer PRIVATE include test test/support)
    target_link_libraries(test_apu_core_integer PRIVATE pr32_apu_unity)
    add_test(NAME test_apu_core_integer COMMAND test_apu_core_integer
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # S9: the integer core's golden PCM. One source, two builds into the same
    # core — an FPU build with the float body compiled out, and a no-FPU build
    # that has no float body at all — so a byte difference between the targets
    # means the integer core still depends on the build, which is the defect the
    # fixture exists to catch. Both reach it through `Legacy`: routing an FPU
    # build through `Nes` was only equivalent while E0 kept the profiles
    # byte-identical, and E1 makes `Nes` audibly different by design.
    add_executable(test_apu_integer_golden test/test_apu_integer_golden.cpp src/ApuCore.cpp)
    target_compile_definitions(test_apu_integer_golden PRIVATE
        UNIT_TEST=1 PR32_APU_ENABLE_LEGACY_FLOAT=0)
    target_include_directories(test_apu_integer_golden PRIVATE include test test/support)
    target_link_libraries(test_apu_integer_golden PRIVATE pr32_apu_unity)
    add_test(NAME test_apu_integer_golden COMMAND test_apu_integer_golden
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    add_executable(test_apu_integer_golden_no_fpu test/test_apu_integer_golden.cpp src/ApuCore.cpp)
    target_compile_definitions(test_apu_integer_golden_no_fpu PRIVATE
        UNIT_TEST=1 PR32_APU_HAS_FPU=0)
    target_include_directories(test_apu_integer_golden_no_fpu PRIVATE
        include test test/support)
    target_link_libraries(test_apu_integer_golden_no_fpu PRIVATE pr32_apu_unity)
    add_test(NAME test_apu_integer_golden_no_fpu COMMAND test_apu_integer_golden_no_fpu
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # S7: one golden per audible profile, each compiled twice for the same
    # reason as the integer fixture above. `Nes` and `Hybrid` always render the
    # integer core, so the two targets differ only in the build around it (FPU
    # with the float body switched off, and no FPU at all) and must agree byte
    # for byte; `Legacy` keeps its two committed references untouched.
    add_executable(test_apu_profile_golden test/test_apu_profile_golden.cpp src/ApuCore.cpp)
    target_compile_definitions(test_apu_profile_golden PRIVATE
        UNIT_TEST=1 PR32_APU_ENABLE_LEGACY_FLOAT=0)
    target_include_directories(test_apu_profile_golden PRIVATE include test test/support)
    target_link_libraries(test_apu_profile_golden PRIVATE pr32_apu_unity)
    add_test(NAME test_apu_profile_golden COMMAND test_apu_profile_golden
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    add_executable(test_apu_profile_golden_no_fpu test/test_apu_profile_golden.cpp src/ApuCore.cpp)
    target_compile_definitions(test_apu_profile_golden_no_fpu PRIVATE
        UNIT_TEST=1 PR32_APU_HAS_FPU=0)
    target_include_directories(test_apu_profile_golden_no_fpu PRIVATE
        include test test/support)
    target_link_libraries(test_apu_profile_golden_no_fpu PRIVATE pr32_apu_unity)
    add_test(NAME test_apu_profile_golden_no_fpu COMMAND test_apu_profile_golden_no_fpu
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # S9: host fixture generator, built three ways from the same source. The
    # only difference is which core the build compiles in, because that is the
    # property tools/golden_gen/run.py has to prove byte-stable.
    function(pr32_apu_add_golden_gen name)
        cmake_parse_arguments(ARG "" "" "EXTRA_DEFS" ${ARGN})
        add_executable(${name} tools/golden_gen/main.cpp src/ApuCore.cpp)
        target_compile_definitions(${name} PRIVATE ${ARG_EXTRA_DEFS})
        target_include_directories(${name} PRIVATE include test)
        target_compile_features(${name} PRIVATE cxx_std_17)
    endfunction()

    # Default host defines: PR32_APU_HAS_FPU autodetects to 1, so `legacy`
    # renders the frozen float body — the build that must reproduce the
    # historical mirror fixture byte for byte.
    pr32_apu_add_golden_gen(apu_golden_gen)
    # No FPU: `legacy` falls to the integer core, the path an ESP32-C3 runs.
    pr32_apu_add_golden_gen(apu_golden_gen_nofpu EXTRA_DEFS PR32_APU_HAS_FPU=0)
    # FPU target with the float body switched off (S2 clause 5): the same
    # integer core reached by the other supported route.
    pr32_apu_add_golden_gen(apu_golden_gen_integer EXTRA_DEFS
        PR32_APU_HAS_FPU=1 PR32_APU_ENABLE_LEGACY_FLOAT=0)

    # Consumer link surface. Every suite above recompiles src/ApuCore.cpp with
    # UNIT_TEST=1, so a definition left inside a `#if defined(UNIT_TEST)` block
    # still resolves for them — and the shipped library silently loses the
    # symbol. This target is the only one that links `pixelroot32-apu` the way
    # a consumer does, so it MUST NOT define UNIT_TEST and MUST NOT compile the
    # source itself.
    add_executable(test_library_link test/test_library_link.cpp)
    target_include_directories(test_library_link PRIVATE test test/support)
    target_link_libraries(test_library_link PRIVATE pixelroot32-apu pr32_apu_unity)
    add_test(NAME test_library_link COMMAND test_library_link
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

    # S2 clause 5: the integer core must compile on an FPU target with the
    # float path switched off. Compile-only on purpose — there is no behaviour
    # to assert, the build itself is the check, and it catches any integer-path
    # symbol left inside a `#if !PR32_APU_HAS_FPU` guard.
    add_library(pixelroot32-apu-integer-only-check STATIC src/ApuCore.cpp)
    target_compile_definitions(pixelroot32-apu-integer-only-check PRIVATE
        PR32_APU_HAS_FPU=1 PR32_APU_ENABLE_LEGACY_FLOAT=0)
    target_include_directories(pixelroot32-apu-integer-only-check PRIVATE include)
    target_compile_features(pixelroot32-apu-integer-only-check PRIVATE cxx_std_17)
endif()
