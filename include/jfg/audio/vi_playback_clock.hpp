#pragma once
#include <chrono>
#include <cstdint>

namespace jfg::audio {
// The original NTSC Count clock runs at 46.875 MHz. Host stalls must not
// change guest Count or device event order. Once audio has actually starved,
// however, its old wall-clock deadline is no longer a valid catch-up target.
class ViPlaybackClock {
public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;

  void anchor(std::uint64_t count, Time now, std::uint64_t underruns) noexcept {
    count_ = count;
    wall_ = now;
    underruns_ = underruns;
  }

  [[nodiscard]] Time deadline(std::uint64_t count, Time now,
                              std::uint64_t underruns) noexcept {
    auto target = wall_ + std::chrono::nanoseconds((count - count_) * 64U / 3U);
    const bool starved = underruns > underruns_;
    underruns_ = underruns;
    // Ordinary late frames still catch up while audio keeps playing. Only
    // an observed loss of audio continuity invalidates the previous origin.
    if (starved && now > target) {
      wall_ += now - target;
      target = now;
    }
    return target;
  }

  // RGO: move the origin, and so every later deadline, earlier (positive)
  // or later (negative); the audio queue-level trim paces this way.
  void trim(std::chrono::nanoseconds earlier) noexcept { wall_ -= earlier; }

private:
  std::uint64_t count_ = 0;
  std::uint64_t underruns_ = 0;
  Time wall_{};
};
} // namespace jfg::audio
