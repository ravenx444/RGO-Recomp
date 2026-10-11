#pragma once

// Deterministic stackful thread executor (Phase 6 native boot).
//
// Recompiled game threads are imperative C that calls libultra (e.g.
// osRecvMesg) mid-function and expects to block there until woken. To run
// that real code we need genuine stacks. This executor gives each guest
// thread a real OS thread but enforces that **exactly one participant runs at
// a time**, handing a single "baton" back and forth. The scheduler decides
// who runs next, so the interleaving is fully determined by scheduler choices,
// not by host thread timing — deterministic, while preserving real stacks.
//
// Why real threads and not fibers: real stacks are natively understood by
// AddressSanitizer (no fiber switch annotations), and the baton guarantees no
// concurrent access, so there are no data races to reason about.
//
// This is the execution substrate; binding it to osCreateThread/osStartThread
// and the message-queue HLE is a later brick.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace jfg::boot {

// Sentinel participant id for the scheduler (baton holder when no guest runs).
inline constexpr int kSchedulerParticipant = -1;

// RGO diagnostics (FPS counter Detailed): baton hand-offs since start, and
// how many of them found the receiver asleep after its spin, so it needed an
// operating-system wake-up. Process-wide, relaxed counts.
[[nodiscard]] std::uint64_t baton_handoffs() noexcept;
[[nodiscard]] std::uint64_t baton_sleeps() noexcept;

class BatonExecutor final {
public:
    // A guest body receives the executor and its own participant id. It runs
    // only while it holds the baton; it calls yield_to_scheduler() to block
    // and returns when the thread is finished.
    using Body = std::function<void(BatonExecutor&, int)>;

    BatonExecutor() = default;
    ~BatonExecutor();

    BatonExecutor(const BatonExecutor&) = delete;
    BatonExecutor& operator=(const BatonExecutor&) = delete;

    // Registers a participant and starts its OS thread parked (waiting for the
    // baton). Returns its id. Must be called from the scheduler side only.
    [[nodiscard]] int spawn(Body body);

    // Scheduler hands the baton to participant `id`, blocks until the baton
    // returns (the participant yielded or finished), and returns whether that
    // participant has finished. Scheduler-side only.
    bool resume(int id);

    // Participant hands the baton back to the scheduler and blocks until it is
    // resumed again. Participant-side only.
    void yield_to_scheduler();

    // Joins all participant threads. Any still-parked participant is released
    // and allowed to run to completion. Scheduler-side only.
    void shutdown();

    [[nodiscard]] std::size_t participant_count() const noexcept {
        return participants_.size();
    }

private:
    struct Participant final {
        std::thread thread;
        std::unique_ptr<std::condition_variable> ready = std::make_unique<std::condition_variable>();
        bool finished = false;
        bool started = false;
    };

    void participant_main(int id, Body body);
    void hand_off(int to);            // set holder and wake the waiter
    void wait_until_holder(int self); // block until `self` holds the baton

    std::mutex mutex_;
    std::condition_variable cv_;
    // Written under mutex_; atomic so a waiter can spin on it briefly before
    // sleeping (wait_until_holder).
    std::atomic<int> holder_{kSchedulerParticipant};
    bool shutting_down_ = false;
    std::vector<Participant> participants_;
};

} // namespace jfg::boot
