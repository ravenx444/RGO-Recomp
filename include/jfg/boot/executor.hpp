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
// RGO: handing the baton between OS threads costs a kernel wake-up and a
// context switch each time, thousands of times a second, and that was more
// than half of the host's work (docs/rgo/framerate.md, "Host speed"). The
// default backend is now user-mode switching on the scheduler's own thread:
// Windows fibers, or ucontext on the headless Linux build (with
// AddressSanitizer fiber annotations). The baton discipline, the order of
// execution and the API are the same; OS threads remain as a fallback
// (Backend::threads, JFG_RGO_GUEST_THREADS=os in the live build). With fibers
// every participant runs on the thread that calls resume(), so a host
// thread_local that must follow the guest thread is registered with
// register_fiber_local().
//
// This is the execution substrate; binding it to osCreateThread/osStartThread
// and the message-queue HLE is a later brick.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
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
// RGO: how long a guest thread waiting for the baton spins before it
// sleeps (50 us by default; JFG_RGO_BATON_SPIN_US in the live build).
void set_baton_spin(std::chrono::microseconds spin) noexcept;
[[nodiscard]] std::chrono::microseconds baton_spin() noexcept;

// RGO: a host thread_local that must stay per guest thread under fibers:
// `swap` installs the given value and returns the previous one. The fiber
// backend swaps each registered value in and out around every resume().
using FiberLocalSwap = void* (*)(void* value) noexcept;
inline constexpr std::size_t kMaxFiberLocals = 4U;
// Returns false when kMaxFiberLocals are already registered.
bool register_fiber_local(FiberLocalSwap swap) noexcept;

enum class ExecutorBackend { threads, fibers };
// The backend new executors use (fibers unless set otherwise).
void set_default_executor_backend(ExecutorBackend backend) noexcept;
[[nodiscard]] ExecutorBackend default_executor_backend() noexcept;

class BatonExecutor final {
public:
    // A guest body receives the executor and its own participant id. It runs
    // only while it holds the baton; it calls yield_to_scheduler() to block
    // and returns when the thread is finished.
    using Body = std::function<void(BatonExecutor&, int)>;

    explicit BatonExecutor(ExecutorBackend backend = default_executor_backend())
        : backend_(backend) {}
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
    [[nodiscard]] ExecutorBackend backend() const noexcept { return backend_; }

    // RGO fiber backend internals (executor.cpp).
    struct Fiber;
    struct FiberDeleter {
        void operator()(Fiber* fiber) const noexcept;
    };

private:
    struct Participant final {
        std::thread thread;
        std::unique_ptr<std::condition_variable> ready = std::make_unique<std::condition_variable>();
        bool finished = false;
        bool started = false;
        // Fiber backend: the participant's stack and saved context, its body
        // and its values of the registered fiber locals.
        std::unique_ptr<Fiber, FiberDeleter> fiber;
        Body body;
        std::array<void*, kMaxFiberLocals> locals{};
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

    // Fiber backend.
    void fiber_resume(int id);
    void fiber_yield();
    void fiber_body(int id) noexcept;
    ExecutorBackend backend_;
    std::unique_ptr<Fiber, FiberDeleter> scheduler_fiber_;
    std::thread::id scheduler_thread_{};
    int running_ = kSchedulerParticipant;

public:
    // Fiber entry point (executor.cpp); not for callers.
    static void fiber_entry_trampoline(BatonExecutor* executor, int id) noexcept;
};

} // namespace jfg::boot
