/*
 * Copyright (c) 2026 PixelRoot32
 * Licensed under the MIT License
 */
#pragma once

#include "AudioTypes.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pixelroot32::audio {

    /**
     * @class AudioCommandQueue
     * @brief Single-Producer Single-Consumer (SPSC) lock-free ring buffer for AudioCommands.
     * 
     * Fixed-size, zero-allocation queue designed for real-time audio thread communication.
     * Supports one producer (game/logic thread) and a single consumer (the audio thread).
     * Concurrent multi-producer use is not supported by this algorithm.
     * 
     * Memory: one slot holds a whole AudioCommand (sizeof(AudioCommand) bytes,
     * 144 B on the host ABI). The ring keeps one slot free to distinguish full
     * from empty, so a CAPACITY of 64 accepts 63 commands. The accepted range is
     * 32-64, and its ceiling is set by RAM, not by throughput: the real command
     * rate is tens per second, so the former 128 doubled the dominant cost of
     * ApuCore without adding headroom anyone used. Raising the ceiling means
     * raising the 9 KB budget below in the same commit, after measuring the new
     * slot size: a capacity the budget cannot pay for is not a capacity.
     * 
     * Drop policy: When the queue is full, the newest command is silently dropped and
     * the droppedCommands counter is incremented. Callers can monitor this via
     * getDroppedCommands() for diagnostics.
     * 
     * Thread-safety: Atomic head/tail loads and stores for SPSC handoff.
     * Safe for one producer and one consumer; not wait-free under contention from
     * multiple producers.
     */
    class AudioCommandQueue {
    public:
        // Configurable capacity: 32-64. Default 64 (see the class comment).
        // The ceiling is the RAM budget below, not a throughput choice.
        #ifndef AUDIO_COMMAND_QUEUE_CAPACITY
        /** @brief Default queue capacity in commands. */
        static constexpr size_t CAPACITY = 64;
        #else
        static_assert(AUDIO_COMMAND_QUEUE_CAPACITY >= 32 && AUDIO_COMMAND_QUEUE_CAPACITY <= 64,
            "AUDIO_COMMAND_QUEUE_CAPACITY must be between 32 and 64");
        /** @brief User-configured queue capacity in commands. */
        static constexpr size_t CAPACITY = AUDIO_COMMAND_QUEUE_CAPACITY;
        #endif

        // S7/S8 RAM budget: 64 slots x 144 B = 9 216 B is the whole allowance.
        // Today the range above already implies this, but the assert stays as the
        // guard that catches a growing AudioCommand: that is how the budget would
        // be blown without anyone touching the capacity. Affirmed here rather than
        // in a test so every consumer build, not just the suite, refuses it.
        static_assert(CAPACITY * sizeof(AudioCommand) <= 9216,
                      "command queue must stay within the 9 KB RAM budget");

        /** @brief Default constructor. Initializes head, tail, and dropped counters to zero. */
        AudioCommandQueue() : head(0), tail(0), droppedCommands(0) {}

        /**
         * @brief Enqueues a command. Safe for a single producer thread.
         * @param cmd The command to enqueue.
         * @return true if successful, false if the queue is full (dropped).
         */
        bool enqueue(const AudioCommand& cmd) {
            size_t currentTail = tail.load(std::memory_order_relaxed);
            size_t nextTail = (currentTail + 1) % CAPACITY;

            if (nextTail == head.load(std::memory_order_acquire)) {
                // Queue full - drop newest command and increment dropped counter
                droppedCommands.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            buffer[currentTail] = cmd;
            tail.store(nextTail, std::memory_order_release);
            return true;
        }

        /**
         * @brief Dequeues a command. Called from the consumer (Audio Thread).
         * @param outCmd Reference to store the dequeued command.
         * @return true if a command was dequeued, false if the queue is empty.
         */
        bool dequeue(AudioCommand& outCmd) {
            size_t currentHead = head.load(std::memory_order_relaxed);

            if (currentHead == tail.load(std::memory_order_acquire)) {
                // Queue empty
                return false;
            }

            outCmd = buffer[currentHead];
            head.store((currentHead + 1) % CAPACITY, std::memory_order_release);
            return true;
        }

        /**
         * @brief Checks if the queue is empty.
         */
        bool isEmpty() const {
            return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
        }

        /**
         * @brief Returns the count of dropped commands due to queue full.
         * Safe to read from the producer or consumer thread.
         */
        size_t getDroppedCommands() const {
            return droppedCommands.load(std::memory_order_relaxed);
        }

    private:
        AudioCommand buffer[CAPACITY];
        std::atomic<size_t> head;
        std::atomic<size_t> tail;
        std::atomic<size_t> droppedCommands;
    };

} // namespace pixelroot32::audio
