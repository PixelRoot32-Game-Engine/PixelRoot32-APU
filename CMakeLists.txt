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

    # Q15 / integer path exercised natively — the coverage gap identified in
    # the extraction analysis (no firmware build compiled this path).
    add_executable(test_q15_smoke test/test_q15_smoke.cpp src/ApuCore.cpp)
    target_compile_definitions(test_q15_smoke PRIVATE UNIT_TEST=1 PR32_APU_HAS_FPU=0)
    target_include_directories(test_q15_smoke PRIVATE include test test/support)
    target_link_libraries(test_q15_smoke PRIVATE pr32_apu_unity)
    add_test(NAME test_q15_smoke COMMAND test_q15_smoke
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test)

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
endif()
