#include "jfg/boot/executor.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

// AddressSanitizer must be told about every stack switch (fiber backend).
#if defined(__SANITIZE_ADDRESS__)
#define JFG_EXECUTOR_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define JFG_EXECUTOR_ASAN 1
#endif
#endif
#if defined(JFG_EXECUTOR_ASAN)
// From <sanitizer/common_interface_defs.h>, declared here so the build does
// not depend on the compiler shipping the header.
extern "C" void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom,
                                               std::size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                                std::size_t* size_old);
#endif

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
// Settable (set_baton_spin): on a PC with many idle cores a longer spin can
// avoid most wake-ups, which are costly on Windows.
std::atomic<std::int64_t> g_baton_spin_us{50};

std::atomic<std::uint64_t> g_handoffs{0U};
std::atomic<std::uint64_t> g_sleeps{0U};

std::atomic<ExecutorBackend> g_default_backend{ExecutorBackend::fibers};
std::array<std::atomic<FiberLocalSwap>, kMaxFiberLocals> g_fiber_locals{};
std::atomic<std::size_t> g_fiber_local_count{0U};

// A fiber's stack reserve: the same 16 MB the Windows build gives each OS
// thread (/STACK in CMakeLists.txt), so deep guest call chains still fit.
constexpr std::size_t kFiberStackBytes = std::size_t{16U} << 20U;

} // namespace

// RGO fiber backend: one participant's (or the scheduler's) stack and saved
// context. The scheduler's is its own thread converted to a fiber.
struct BatonExecutor::Fiber {
#if defined(_WIN32)
    void* handle = nullptr;
    bool converted = false;  // scheduler: we called ConvertThreadToFiberEx
#else
    ucontext_t context{};
    void* mapping = nullptr;
    std::size_t mapped = 0U;
#endif
    // AddressSanitizer: the stack this context runs on, and its fake stack
    // while switched out.
    const void* stack_bottom = nullptr;
    std::size_t stack_size = 0U;
    void* fake_stack = nullptr;
    // Participant fibers: who to run at the first instruction.
    BatonExecutor* owner = nullptr;
    int id = kSchedulerParticipant;
};

void BatonExecutor::FiberDeleter::operator()(Fiber* fiber) const noexcept {
    if (fiber == nullptr) return;
#if defined(_WIN32)
    if (fiber->converted)
        (void)ConvertFiberToThread();
    else if (fiber->handle != nullptr)
        DeleteFiber(fiber->handle);
#else
    if (fiber->mapping != nullptr)
        (void)munmap(fiber->mapping, fiber->mapped);
#endif
    delete fiber;
}

namespace {

// Switches from `from` to `to`; returns when something switches back to
// `from`. `dying`: `from` will never run again (a finished participant).
void switch_fiber(BatonExecutor::Fiber& from, BatonExecutor::Fiber& to, const bool dying) {
#if defined(JFG_EXECUTOR_ASAN)
    __sanitizer_start_switch_fiber(dying ? nullptr : &from.fake_stack, to.stack_bottom,
                                   to.stack_size);
#else
    static_cast<void>(dying);
#endif
#if defined(_WIN32)
    static_cast<void>(from);
    SwitchToFiber(to.handle);
#else
    if (swapcontext(&from.context, &to.context) != 0)
        std::terminate();
#endif
#if defined(JFG_EXECUTOR_ASAN)
    __sanitizer_finish_switch_fiber(from.fake_stack, nullptr, nullptr);
#endif
}

#if defined(_WIN32)
void WINAPI windows_fiber_entry(void* parameter) {
    const auto* fiber = static_cast<const BatonExecutor::Fiber*>(parameter);
    BatonExecutor::fiber_entry_trampoline(fiber->owner, fiber->id);
}
#else
// makecontext passes int arguments only: the Fiber pointer in two halves.
void ucontext_entry(unsigned int high, unsigned int low) {
    const auto address = (static_cast<std::uintptr_t>(high) << 32U) | low;
    const auto* fiber = reinterpret_cast<const BatonExecutor::Fiber*>(address);
    BatonExecutor::fiber_entry_trampoline(fiber->owner, fiber->id);
}
#endif

} // namespace

bool register_fiber_local(const FiberLocalSwap swap) noexcept {
    const std::size_t count = g_fiber_local_count.load(std::memory_order_acquire);
    for (std::size_t index = 0U; index < count; ++index)
        if (g_fiber_locals[index].load(std::memory_order_relaxed) == swap) return true;
    if (count >= kMaxFiberLocals) return false;
    g_fiber_locals[count].store(swap, std::memory_order_relaxed);
    g_fiber_local_count.store(count + 1U, std::memory_order_release);
    return true;
}

void set_default_executor_backend(const ExecutorBackend backend) noexcept {
    g_default_backend.store(backend, std::memory_order_relaxed);
}

ExecutorBackend default_executor_backend() noexcept {
    return g_default_backend.load(std::memory_order_relaxed);
}

void set_baton_spin(const std::chrono::microseconds spin) noexcept {
    g_baton_spin_us.store(spin.count() < 0 ? 0 : spin.count(), std::memory_order_relaxed);
}

std::chrono::microseconds baton_spin() noexcept {
    return std::chrono::microseconds(g_baton_spin_us.load(std::memory_order_relaxed));
}

std::uint64_t baton_handoffs() noexcept {
    return g_handoffs.load(std::memory_order_relaxed);
}

std::uint64_t baton_sleeps() noexcept {
    return g_sleeps.load(std::memory_order_relaxed);
}

BatonExecutor::~BatonExecutor() {
    shutdown();
}

int BatonExecutor::spawn(Body body) {
    if (backend_ == ExecutorBackend::fibers) {
        const int id = static_cast<int>(participants_.size());
        std::unique_ptr<Fiber, FiberDeleter> fiber(new Fiber{});
        fiber->owner = this;
        fiber->id = id;
#if defined(_WIN32)
        fiber->handle = CreateFiberEx(0U, kFiberStackBytes, FIBER_FLAG_FLOAT_SWITCH,
                                      &windows_fiber_entry, fiber.get());
        if (fiber->handle == nullptr)
            throw std::runtime_error("CreateFiberEx failed");
#else
        const std::size_t guard = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        fiber->mapped = kFiberStackBytes + guard;
        fiber->mapping = mmap(nullptr, fiber->mapped, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (fiber->mapping == MAP_FAILED) {
            fiber->mapping = nullptr;
            throw std::runtime_error("fiber stack allocation failed");
        }
        // A guard page below the stack turns an overflow into a fault.
        (void)mprotect(fiber->mapping, guard, PROT_NONE);
        if (getcontext(&fiber->context) != 0)
            throw std::runtime_error("getcontext failed");
        fiber->context.uc_stack.ss_sp = static_cast<char*>(fiber->mapping) + guard;
        fiber->context.uc_stack.ss_size = kFiberStackBytes;
        fiber->context.uc_link = nullptr;
        const auto address = reinterpret_cast<std::uintptr_t>(fiber.get());
        makecontext(&fiber->context, reinterpret_cast<void (*)()>(&ucontext_entry), 2,
                    static_cast<unsigned int>(address >> 32U),
                    static_cast<unsigned int>(address & 0xFFFFFFFFU));
        fiber->stack_bottom = fiber->context.uc_stack.ss_sp;
        fiber->stack_size = kFiberStackBytes;
#endif
        participants_.emplace_back();
        Participant& participant = participants_.back();
        participant.started = true;
        participant.body = std::move(body);
        participant.fiber = std::move(fiber);
        return id;
    }
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
    if (backend_ == ExecutorBackend::fibers) {
        fiber_resume(id);
        return participants_[static_cast<std::size_t>(id)].finished;
    }
    hand_off(id);
    wait_until_holder(kSchedulerParticipant);
    std::unique_lock<std::mutex> lock(mutex_);
    return participants_[static_cast<std::size_t>(id)].finished;
}

void BatonExecutor::yield_to_scheduler() {
    if (backend_ == ExecutorBackend::fibers) {
        fiber_yield();
        if (shutting_down_) {
            throw ExecutorShutdown{};
        }
        return;
    }
    const int self = t_self;
    hand_off(kSchedulerParticipant);
    wait_until_holder(self);
    if (shutting_down_) {
        throw ExecutorShutdown{};
    }
}

void BatonExecutor::hand_off(const int to) {
    g_handoffs.fetch_add(1U, std::memory_order_relaxed);
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
    const auto spin_until = std::chrono::steady_clock::now() + baton_spin();
    for (unsigned spin = 1U; holder_.load(std::memory_order_acquire) != self; ++spin) {
        if ((spin & 63U) == 0U && std::chrono::steady_clock::now() >= spin_until)
            break;
        JFG_EXECUTOR_PAUSE();
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto &ready = self == kSchedulerParticipant ? cv_
        : *participants_[static_cast<std::size_t>(self)].ready;
    if (holder_ != self)
        g_sleeps.fetch_add(1U, std::memory_order_relaxed);
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
        participant.fiber.reset();
    }
    scheduler_fiber_.reset();
}

// ---- RGO fiber backend -----------------------------------------------------

void BatonExecutor::fiber_entry_trampoline(BatonExecutor* executor, const int id) noexcept {
    executor->fiber_body(id);
}

void BatonExecutor::fiber_body(const int id) noexcept {
#if defined(JFG_EXECUTOR_ASAN)
    // Coming from the scheduler: learn its stack, needed to switch back.
    const void* bottom = nullptr;
    std::size_t size = 0U;
    __sanitizer_finish_switch_fiber(nullptr, &bottom, &size);
    if (scheduler_fiber_->stack_bottom == nullptr) {
        scheduler_fiber_->stack_bottom = bottom;
        scheduler_fiber_->stack_size = size;
    }
#endif
    {
        // The body moves onto this fiber's stack: participants_ may grow (and
        // move its elements) while this participant is parked. It is
        // destroyed here, before the last switch: this function never returns.
        Body body = std::move(participants_[static_cast<std::size_t>(id)].body);
        try {
            if (!shutting_down_) {
                body(*this, id);
            }
        } catch (const ExecutorShutdown&) {
            // Cooperative cancellation during shutdown; fall through to hand back.
        }
    }
    // Any other exception reaches this noexcept boundary and terminates, as
    // it did at the end of a participant's OS thread.
    participants_[static_cast<std::size_t>(id)].finished = true;
    g_handoffs.fetch_add(1U, std::memory_order_relaxed);
    Fiber* self = participants_[static_cast<std::size_t>(id)].fiber.get();
    switch_fiber(*self, *scheduler_fiber_, true);
    std::terminate();  // a finished participant is never resumed
}

void BatonExecutor::fiber_resume(const int id) {
    if (!scheduler_fiber_) {
        std::unique_ptr<Fiber, FiberDeleter> scheduler(new Fiber{});
#if defined(_WIN32)
        if (IsThreadAFiber()) {
            scheduler->handle = GetCurrentFiber();
        } else {
            scheduler->handle = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
            if (scheduler->handle == nullptr)
                throw std::runtime_error("ConvertThreadToFiberEx failed");
            scheduler->converted = true;
        }
#endif
        scheduler_fiber_ = std::move(scheduler);
        scheduler_thread_ = std::this_thread::get_id();
    } else if (scheduler_thread_ != std::this_thread::get_id()) {
        // Every participant runs on the scheduler's thread; resuming from
        // another thread would run two at once.
        throw std::logic_error("fiber executor resumed from another thread");
    }
    Participant& participant = participants_.at(static_cast<std::size_t>(id));
    if (participant.finished)
        throw std::logic_error("resume of a finished participant");
    Fiber* target = participant.fiber.get();
    // Swap in the participant's values of the registered host thread_locals.
    const std::size_t locals = g_fiber_local_count.load(std::memory_order_acquire);
    std::array<void*, kMaxFiberLocals> scheduler_values{};
    for (std::size_t index = 0U; index < locals; ++index)
        scheduler_values[index] = g_fiber_locals[index].load(std::memory_order_relaxed)(
            participant.locals[index]);
    running_ = id;
    holder_.store(id, std::memory_order_release);
    g_handoffs.fetch_add(1U, std::memory_order_relaxed);
    switch_fiber(*scheduler_fiber_, *target, false);
    running_ = kSchedulerParticipant;
    holder_.store(kSchedulerParticipant, std::memory_order_release);
    Participant& returned = participants_[static_cast<std::size_t>(id)];
    for (std::size_t index = 0U; index < locals; ++index)
        returned.locals[index] = g_fiber_locals[index].load(std::memory_order_relaxed)(
            scheduler_values[index]);
}

void BatonExecutor::fiber_yield() {
    const int self = running_;
    if (self == kSchedulerParticipant)
        throw std::logic_error("yield outside a participant");
    Fiber* from = participants_[static_cast<std::size_t>(self)].fiber.get();
    g_handoffs.fetch_add(1U, std::memory_order_relaxed);
    switch_fiber(*from, *scheduler_fiber_, false);
}

} // namespace jfg::boot
