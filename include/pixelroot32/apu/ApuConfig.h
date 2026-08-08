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

    // -----------------------------------------------------------------------
    // NES APU frame counter timing (Hito 2 M3)
    // -----------------------------------------------------------------------
    //
    // CPU cycle rate NTSC = 1.789773 MHz. 1 APU cycle = 2 CPU cycles, so the
    // frame sequencer ticks at 894886.5 Hz. Tables below are the APU-cycle
    // thresholds for each step of the 4-step (mode 0) and 5-step (mode 1)
    // sequences; see https://www.nesdev.org/wiki/APU_Frame_Counter.
    namespace nes_apu {
        /** CPU clock NTSC in Hz. */
        inline constexpr double kNesCpuHzNtsc = 1789773.0;
        /** APU cycle rate NTSC = CPU/2 in Hz. */
        inline constexpr double kNesApuHzNtsc = kNesCpuHzNtsc / 2.0;

        /**
         * APU cycles threshold for entry to step N (1-indexed) of the
         * 4-step (mode 0) sequence. Index 0 is the wrap point at 0.
         */
        inline constexpr double kMode0ApuCyclesPerStep[5] = {
            0.0,        // 0: wrap
            3728.5,     // 1: quarter
            7456.5,     // 2: quarter + half
            11185.5,    // 3: quarter
            14914.5,    // 4: half (+ irq on wrap if not inhibited)
        };

        /**
         * APU cycles threshold for entry to step N (1-indexed) of the
         * 5-step (mode 1) sequence. Index 0 is the wrap point at 0.
         */
        inline constexpr double kMode1ApuCyclesPerStep[6] = {
            0.0,        // 0: wrap
            3728.5,     // 1: quarter
            7456.5,     // 2: quarter + half
            11185.5,    // 3: quarter
            14914.5,    // 4: half
            18640.5,    // 5: quarter + half
        };

        /**
         * NES length counter lookup table (32 values). Mirrors the
         * canonical table on https://www.nesdev.org/wiki/APU_Length_Counter.
         *
         * Per the wiki, the values are "the actual values the length
         * counter gets loaded with plus one, to allow a model where the
         * channel is silenced when the length counter becomes zero."
         * We use the model where the counter is initialised to
         * `kNesLengthLut[index]` and silenced when it BECOMES 0, i.e.
         * after `kNesLengthLut[index]` half-clock decrements.
         *
         * Hito 2 M4.
         */
        inline constexpr uint8_t kNesLengthLut[32] = {
            10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
            12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
        };

        // -----------------------------------------------------------------------
        // NES PULSE timer <-> Hz conversion (Hito 2 M6)
        // -----------------------------------------------------------------------
        //
        // The NES APU drives the PULSE timer from the CPU clock divided by 16:
        //   f = CPU_NTSC / 16 / (timer + 1)
        //     = 1789773 / 16 / (timer + 1)
        //     = 111860.8125 / (timer + 1)
        //
        // Source: https://www.nesdev.org/wiki/APU_Pulse (timer = 11-bit, range
        // 0..2047). The value 111860.8 is the canonical rounded constant used
        // by the APU community; the last 0.0125 Hz per step is below the
        // resolution of any consumer oscillator.
        //
        // These helpers let composers port NES music to PixelRoot32 without
        // having to redo the period-to-Hz math by hand. They are pure / no
        // side effects, so they can be called from any thread.

        /** Convert an 11-bit NES PULSE timer period to Hz (NTSC). */
        inline float nesTimerToHz(uint16_t timer) {
            if (timer > 2047) timer = 2047;
            return 111860.8f / static_cast<float>(timer + 1u);
        }

        /**
         * Convert Hz to an 11-bit NES PULSE timer period (NTSC). Clamped to
         * [0, 2047]. Non-positive Hz returns 2047 (highest timer = lowest Hz).
         */
        inline uint16_t hzToNesTimer(float hz) {
            if (!(hz > 0.0f)) return 2047u;
            float t = 111860.8f / hz - 1.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 2047.0f) t = 2047.0f;
            return static_cast<uint16_t>(t);
        }

        // -----------------------------------------------------------------------
        // NES NOISE period LUT (Hito 3 M8)
        // -----------------------------------------------------------------------

        /**
         * The 16 NTSC noise timer periods, in CPU cycles, indexed by
         * `$400E` bits 3-0. Source:
         * https://www.nesdev.org/wiki/APU_Noise
         *
         * "The period determines how many CPU cycles happen between shift
         * register clocks. These periods are all even numbers because there
         * are 2 CPU cycles in an APU cycle."
         *
         * NOTE: the internal `docs/nes-apu-comparison.md` sketch lists this
         * table as `... 508, 1014, 2034, 4068, 8136`, which is wrong — it
         * drops 762, mistypes 1016 as 1014, and shifts the tail up by one
         * entry. The values below are the canonical wiki table.
         *
         * PAL uses a different table; this library models NTSC only, which
         * matches `kNesCpuHzNtsc` used everywhere else.
         *
         * Hito 3 M8.
         */
        inline constexpr uint16_t kNesNoisePeriodLutNtsc[16] = {
            4, 8, 16, 32, 64, 96, 128, 160,
            202, 254, 380, 508, 762, 1016, 2034, 4068
        };

        /**
         * Convert a NOISE LUT index to an LFSR shift period expressed in
         * output samples at `sampleRate`.
         *
         * The table is stored in CPU cycles so the same index yields the
         * same timbre at any sample rate:
         *   samples = cycles * sampleRate / kNesCpuHzNtsc
         *
         * Indices above 15 are clamped to 15. The result is floored at 1
         * sample: the NES clocks noise up to ~447 kHz, so the first few
         * entries land below one output sample at any rate we support and
         * collapse onto the fastest representable period. That is an
         * inherent limit of a sample-based LFSR, not a rounding bug.
         *
         * Hito 3 M8.
         */
        inline uint32_t nesNoisePeriodToSamples(uint8_t index, int sampleRate) {
            if (sampleRate <= 0) return 1u;
            if (index > 15u) index = 15u;
            const double samples =
                static_cast<double>(kNesNoisePeriodLutNtsc[index])
                * static_cast<double>(sampleRate) / kNesCpuHzNtsc;
            if (samples < 1.0) return 1u;
            return static_cast<uint32_t>(samples + 0.5);
        }
    } // namespace nes_apu

} // namespace pixelroot32::audio
