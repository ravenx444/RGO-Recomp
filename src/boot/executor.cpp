#include "jfg/boot/executor.hpp"

#include <chrono>
#include <utility>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define JFG_EXECUTOR_PAUSE() _mm_pause()
#else
#define JFG_EXECUTOR_PAUSE() ((void)0)
#endif

namespace jfg::boot {

namespace {

// Thrown into a parked participant during shutdown to unwind its body so the
// OS thread can be joined cleanly. Never escapes participant_main.
struct ExecutorShutdown final {};

// The running participant's own id, so yield_to_scheduler() needs no argument.
thread_local int t_self = kSchedulerParticipant;

// RGO: how long a participant waiting for the baton spins before it sleeps on
// its condition variable. The game's threads hand the baton on thousands of
// times a second, often after a few microseconds; a waiter that is still
// spinning takes it without the futex wake-up and context switch a sleeping
// one needs (docs/rgo/framerate.md, "Host speed").
constexpr std::chrono::microseconds kBatonSpin{50};

} // namespace

BatonExecutor::~BatonExecutor() {
    shutdown();
}

int BatonExecutor::spawn(Body body) {
    std::unique_lock<std::mutex> lock(mutex_);
    const int id = static_cast<int>(participants_.size());
    participants_.emplace_back();
    participants_[static_cast<std::size_t>(id)].started = true;
    // The new thread immediately parks on the baton (holder_ != id), so it
    // touches nothing until the scheduler resumes it.
    participants_[static_cast<std::size_t>(id)].thread =
        std::thread([this, id, body = std::move(body)]() {
            participant_main(id, body);
        });
    return id;
}

void BatonExecutor::participant_main(const int id, Body body) {
    t_self = id;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        participants_[static_cast<std::size_t>(id)].ready->wait(lock, [&]() { return holder_ == id; });
    }
    try {
        if (shutting_down_) {
            throw ExecutorShutdown{};
        }
        body(*this, id);
    } catch (const ExecutorShutdown&) {
        // Cooperative cancellation during shutdown; fall through to hand back.
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        participants_[static_cast<std::size_t>(id)].finished = true;
        holder_ = kSchedulerParticipant;
    }
    cv_.notify_one();
}

bool BatonExecutor::resume(const int id) {
    hand_off(id);
    wait_until_holder(kSchedulerParticipant);
    std::unique_lock<std::mutex> lock(mutex_);
    return participants_[static_cast<std::size_t>(id)].finished;
}

void BatonExecutor::yield_to_scheduler() {
    const int self = t_self;
    hand_off(kSchedulerParticipant);
    wait_until_holder(self);
    if (shutting_down_) {
        throw ExecutorShutdown{};
    }
}

void BatonExecutor::hand_off(const int to) {
    std::condition_variable *ready = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        holder_ = to;
        ready = to == kSchedulerParticipant ? &cv_
            : participants_[static_cast<std::size_t>(to)].ready.get();
    }
    // Only the selected baton recipient can run. Waking the other parked
    // guest threads just makes them contend for this mutex and sleep again.
    ready->notify_one();
}

void BatonExecutor::wait_until_holder(const int self) {
    const auto spin_until = std::chrono::steady_clock::now() + kBatonSpin;
    for (unsigned spin = 1U; holder_.load(std::memory_order_acquire) != self; ++spin) {
        if ((spin & 63U) == 0U && std::chrono::steady_clock::now() >= spin_until)
            break;
        JFG_EXECUTOR_PAUSE();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto &ready = self == kSchedulerParticipant ? cv_
        : *participants_[static_cast<std::size_t>(self)].ready;
    ready.wait(lock, [&]() { return holder_ == self; });
}

void BatonExecutor::shutdown() {
    // Unwind each unfinished participant one at a time, in id order. Only the
    // baton holder ever runs, so this stays deterministic and race-free.
    for (std::size_t index = 0; index < participants_.size(); ++index) {
        bool finished = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            finished = participants_[index].finished;
            if (!finished) {
                shutting_down_ = true;
            }
        }
        if (!finished) {
            (void)resume(static_cast<int>(index));
        }
    }
    for (Participant& participant : participants_) {
        if (participant.thread.joinable()) {
            participant.thread.join();
        }
    }
}

} // namespace jfg::boot
