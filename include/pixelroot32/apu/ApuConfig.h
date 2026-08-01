/*
 * Copyright (c) 2026 PixelRoot32
 * Licensed under the MIT License
 */
#pragma once

#include <cstdint>

/**
 * @file ApuConfig.h
 * @brief Build-time configuration surface of the PixelRoot32 APU library.
 *
 * Every integration point the synthesis core needs from its host lives here,
 * so the core itself stays free of engine- or app-specific includes.
 *
 * Configuration macros (all overridable from the build system):
 *
 *   PR32_APU_HAS_FPU            1 = float render path, 0 = integer/Q15 + LUT
 *                               path. Auto-detected when not defined:
 *                               ESP-IDF targets use SOC_CPU_HAS_FPU from
 *                               <soc/soc_caps.h>; every other platform
 *                               defaults to 1. Define it explicitly to force
 *                               either path (e.g. -DPR32_APU_HAS_FPU=0 to
 *                               exercise the Q15 path in native tests).
 *
 *   PR32_APU_ENABLE_PROFILING   1 = capture peak/clip snapshots into the
 *                               ApuCore profiling ring (default 0).
 *
 *   AUDIO_SEQUENCER_MAX_NOTES   Max sequencer notes per frame [1, 1024]
 *                               (default 32, see ApuCore.h).
 *
 *   AUDIO_COMMAND_QUEUE_CAPACITY  Command queue capacity [128, 1024]
 *                               (default 128, see AudioCommandQueue.h).
 *
 *   PIXELROOT32_DEBUG_MODE      Enables the diagnostic log call sites
 *                               (routed through the log hook below).
 */

// ---------------------------------------------------------------------------
// FPU detection (unified: the single source of truth for the render path)
// ---------------------------------------------------------------------------
#ifndef PR32_APU_HAS_FPU
    #if defined(ESP32) || defined(ESP_PLATFORM)
        #if defined(__has_include)
            #if __has_include(<soc/soc_caps.h>)
                #include <soc/soc_caps.h>
            #endif
        #endif
        #if defined(SOC_CPU_HAS_FPU) && SOC_CPU_HAS_FPU
            #define PR32_APU_HAS_FPU 1
        #else
            #define PR32_APU_HAS_FPU 0
        #endif
    #else
        #define PR32_APU_HAS_FPU 1
    #endif
#endif

// ---------------------------------------------------------------------------
// Profiling
// ---------------------------------------------------------------------------
#ifndef PR32_APU_ENABLE_PROFILING
    #define PR32_APU_ENABLE_PROFILING 0
#endif

namespace pixelroot32::audio {

    /** Compile-time switch consumed by ApuCore's profiling ring. */
    inline constexpr bool kApuProfilingEnabled = (PR32_APU_ENABLE_PROFILING != 0);

    // -----------------------------------------------------------------------
    // Log hook (host-injectable; default is a silent no-op)
    // -----------------------------------------------------------------------
    enum class ApuLogLevel : uint8_t {
        Info = 0,
        Warning = 1,
    };

    /**
     * Host log sink. Receives a fully formatted, null-terminated message.
     * May be called from the audio thread — implementations must be
     * real-time safe or defer the work.
     */
    using ApuLogHandler = void (*)(ApuLogLevel level, const char* message);

    /** Installs the process-wide log handler (nullptr = silence, the default). */
    void apuSetLogHandler(ApuLogHandler handler);

    /** Returns the currently installed handler (nullptr when none). */
    ApuLogHandler apuGetLogHandler();

    /**
     * printf-style helper used by the library's diagnostic call sites.
     * Formats into a small stack buffer and forwards to the installed
     * handler; does nothing when no handler is set.
     */
    void apuLogf(ApuLogLevel level, const char* fmt, ...);

} // namespace pixelroot32::audio
