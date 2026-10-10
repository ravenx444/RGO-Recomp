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

}  // namespace jfg
