#pragma once

// RGO frame pacing (docs/rgo/framerate.md): wait until a deadline without the
// operating system's sleep overshoot. Sleeping alone can wake a millisecond
// or more late (more without a raised timer resolution), which makes VIs and
// so presented frames uneven. Sleep until shortly before the deadline, then
// yield until it passes.

#include <chrono>
#include <thread>

namespace jfg {

inline constexpr auto kPreciseWaitSpin = std::chrono::microseconds(2000);

template <typename Clock, typename Duration>
void precise_sleep_until(const std::chrono::time_point<Clock, Duration> deadline) {
    const auto coarse = deadline - kPreciseWaitSpin;
    if (Clock::now() < coarse)
        std::this_thread::sleep_until(coarse);
    while (Clock::now() < deadline)
        std::this_thread::yield();
}

// The same with the coarse sleep done by `sleep_for` (a callable taking a
// std::chrono::nanoseconds). On Windows the runtime passes a high-resolution
// waitable timer: a plain sleep there is only as precise as the process's
// timer resolution, which Windows 11 does not always raise (it can refuse a
// process it considers occluded or inaudible), and then it can wake up to
// 15.6 ms late, past the 2 ms margin.
template <typename Clock, typename Duration, typename SleepFor>
void precise_sleep_until(const std::chrono::time_point<Clock, Duration> deadline,
                         SleepFor&& sleep_for) {
    const auto coarse = deadline - kPreciseWaitSpin;
    const auto now = Clock::now();
    if (now < coarse)
        sleep_for(std::chrono::duration_cast<std::chrono::nanoseconds>(coarse - now));
    while (Clock::now() < deadline)
        std::this_thread::yield();
}

}  // namespace jfg
