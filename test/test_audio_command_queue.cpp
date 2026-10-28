#include <unity.h>
#include "pixelroot32/apu/AudioCommandQueue.h"

using namespace pixelroot32::audio;

void setUp(void) {}
void tearDown(void) {}

// S4: the profile selector must ride inside the padding that already sits
// next to `nesLengthHalt`. A silent growth would enlarge every queue slot, so
// the ABI is pinned here instead of being discovered in the RAM budget.
static_assert(sizeof(AudioCommand) == 144,
              "AudioCommand ABI must stay at 144 bytes (host)");

// S7/S8: the queue is the dominant member of ApuCore (144 B per slot), so
// its RAM bill is pinned at compile time instead of being discovered on target.
static_assert(AudioCommandQueue::CAPACITY * sizeof(AudioCommand) <= 9216,
              "command queue must stay within the 9 KB RAM budget");

void test_audio_command_queue_initial_state(void) {
    AudioCommandQueue queue;
    TEST_ASSERT_TRUE(queue.isEmpty());
}

void test_audio_command_queue_enqueue_dequeue(void) {
    AudioCommandQueue queue;
    AudioCommand cmd1;
    cmd1.type = AudioCommandType::PLAY_EVENT;
    cmd1.event.type = WaveType::PULSE;
    cmd1.event.frequency = 440.0f;
    cmd1.event.duration = 0.5f;
    cmd1.event.volume = 1.0f;
    
    TEST_ASSERT_TRUE(queue.enqueue(cmd1));
    TEST_ASSERT_FALSE(queue.isEmpty());
    
    AudioCommand outCmd;
    TEST_ASSERT_TRUE(queue.dequeue(outCmd));
    TEST_ASSERT_TRUE(queue.isEmpty());
    TEST_ASSERT_EQUAL(AudioCommandType::PLAY_EVENT, outCmd.type);
    TEST_ASSERT_EQUAL_FLOAT(440.0f, outCmd.event.frequency);
}

void test_audio_command_queue_full(void) {
    AudioCommandQueue queue;
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.8f;
    
    // Fill the queue. A ring buffer keeps one slot free to tell full from
    // empty (nextTail == head means full), so max CAPACITY-1 items.
    for (size_t i = 0; i < AudioCommandQueue::CAPACITY - 1; i++) {
        TEST_ASSERT_TRUE(queue.enqueue(cmd));
    }
    
    // Next enqueue should fail
    TEST_ASSERT_FALSE(queue.enqueue(cmd));
}

// S7: the default dropped from 128 to 64 because the real command rate is tens
// per second; a silent climb back would double this object's dominant cost.
void test_audio_command_queue_default_capacity_is_64(void) {
    TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(AudioCommandQueue::CAPACITY));
}

// S7: a ring of CAPACITY slots holds exactly CAPACITY-1 commands. The extra
// enqueue is the drop and must be counted once, without evicting an accepted
// command (the policy drops the newest, not the oldest).
void test_audio_command_queue_accepts_capacity_minus_one_and_drops_the_last(void) {
    AudioCommandQueue queue;
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.8f;

    for (size_t i = 0; i < AudioCommandQueue::CAPACITY - 1; i++) {
        TEST_ASSERT_TRUE(queue.enqueue(cmd));
    }
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(queue.getDroppedCommands()));

    TEST_ASSERT_FALSE(queue.enqueue(cmd));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(queue.getDroppedCommands()));

    // The rejected command did not displace anything: the ring still holds
    // every accepted command.
    AudioCommand outCmd;
    size_t dequeued = 0;
    while (queue.dequeue(outCmd)) {
        dequeued++;
    }
    TEST_ASSERT_EQUAL_UINT32(AudioCommandQueue::CAPACITY - 1,
                             static_cast<uint32_t>(dequeued));
}

void test_audio_command_queue_empty_dequeue(void) {
    AudioCommandQueue queue;
    AudioCommand outCmd;
    TEST_ASSERT_FALSE(queue.dequeue(outCmd));
}

// MPSC tests - Multiple Producer Single Consumer
void test_audio_command_queue_mpsc_single_producer_baseline(void) {
    AudioCommandQueue queue;
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 1.0f;
    
    // Single producer should work same as before: a ring holds CAPACITY-1.
    for (size_t i = 0; i < AudioCommandQueue::CAPACITY - 1; i++) {
        TEST_ASSERT_TRUE(queue.enqueue(cmd));
    }
    
    // Dequeue all of them
    AudioCommand outCmd;
    size_t dequeued = 0;
    while (queue.dequeue(outCmd)) {
        dequeued++;
    }
    TEST_ASSERT_EQUAL(AudioCommandQueue::CAPACITY - 1, dequeued);
}

void test_audio_command_queue_mpsc_full_with_drops(void) {
    AudioCommandQueue queue;
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.8f;
    
    // Fill to capacity
    for (size_t i = 0; i < AudioCommandQueue::CAPACITY - 1; i++) {
        queue.enqueue(cmd);
    }
    
    // Get dropped count - should be 0 initially
    TEST_ASSERT_EQUAL(0, queue.getDroppedCommands());
    
    // Try to enqueue more - should fail and increment dropped
    bool enqueueResult = queue.enqueue(cmd);
    TEST_ASSERT_FALSE(enqueueResult); // Queue full, drop policy
    
    // Now dropped count should reflect the drop
    TEST_ASSERT_TRUE(queue.getDroppedCommands() > 0);
}

void test_audio_command_queue_mpsc_drop_counter_accuracy(void) {
    AudioCommandQueue queue;
    AudioCommand cmd;
    cmd.type = AudioCommandType::PLAY_EVENT;
    cmd.event.type = WaveType::PULSE;
    cmd.event.frequency = 440.0f;
    cmd.event.duration = 0.5f;
    cmd.event.volume = 0.8f;

    // Reset dropped counter by creating new queue
    AudioCommandQueue queue2;
    TEST_ASSERT_EQUAL(0, queue2.getDroppedCommands());

    // Fill the queue to capacity (CAPACITY-1 items max)
    for (size_t i = 0; i < AudioCommandQueue::CAPACITY - 1; i++) {
        queue2.enqueue(cmd);
    }

    // Try to overfill - each failed enqueue should increment dropped
    // Now try to add more than capacity
    for (size_t i = 0; i < 10; i++) {
        queue2.enqueue(cmd); // These will fail and increment dropped
    }

    // Dropped should be at least 10 (we tried to add 10 to full queue)
    TEST_ASSERT_TRUE(queue2.getDroppedCommands() >= 10);
}

// S4: the profile travels through the same queue as every other command, and
// arriving intact is the whole contract — a truncated field would silently
// select the wrong core.
void test_audio_command_queue_sound_profile_round_trip(void) {
    AudioCommandQueue queue;
    AudioCommand cmd;
    cmd.type = AudioCommandType::SET_SOUND_PROFILE;
    cmd.soundProfile = static_cast<uint8_t>(SoundProfile::Hybrid);

    TEST_ASSERT_TRUE(queue.enqueue(cmd));
    TEST_ASSERT_FALSE(queue.isEmpty());

    AudioCommand outCmd;
    TEST_ASSERT_TRUE(queue.dequeue(outCmd));
    TEST_ASSERT_TRUE(queue.isEmpty());
    TEST_ASSERT_EQUAL(AudioCommandType::SET_SOUND_PROFILE, outCmd.type);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SoundProfile::Hybrid),
                            outCmd.soundProfile);
    // The neighbours inside the reused padding must survive untouched.
    TEST_ASSERT_FALSE(outCmd.nesLengthHalt);
    TEST_ASSERT_EQUAL_UINT8(0, outCmd.nesLengthIndex);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_audio_command_queue_initial_state);
    RUN_TEST(test_audio_command_queue_enqueue_dequeue);
    RUN_TEST(test_audio_command_queue_full);
    RUN_TEST(test_audio_command_queue_default_capacity_is_64);
    RUN_TEST(test_audio_command_queue_accepts_capacity_minus_one_and_drops_the_last);
    RUN_TEST(test_audio_command_queue_empty_dequeue);
    // MPSC tests
    RUN_TEST(test_audio_command_queue_mpsc_single_producer_baseline);
    RUN_TEST(test_audio_command_queue_mpsc_full_with_drops);
    RUN_TEST(test_audio_command_queue_mpsc_drop_counter_accuracy);
    // S4: sound profile command
    RUN_TEST(test_audio_command_queue_sound_profile_round_trip);
    return UNITY_END();
}
