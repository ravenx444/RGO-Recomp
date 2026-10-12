#pragma once
// RGO: the host audio path for an SDL callback device (docs/rgo/framerate.md,
// "Audio through a callback device"). The game's audio (16-bit stereo at the
// emulated AI rate, about 22 kHz) is resampled once, here, to the device's
// own rate and written into a lock-free single-producer single-consumer ring.
// The device's callback reads the ring at exactly the device clock. A small
// continuous correction of the resampling ratio, driven by how full the ring
// is, keeps it at the cushion without the audible skips a hard bound needs.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jfg::audio {

// A stereo frame: left in the low 16 bits, right in the high 16 bits.
using StereoFrame = std::uint32_t;

[[nodiscard]] constexpr StereoFrame stereo_frame(const std::int16_t left,
                                                 const std::int16_t right) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint16_t>(left)) |
         (static_cast<std::uint32_t>(static_cast<std::uint16_t>(right)) << 16U);
}
[[nodiscard]] constexpr std::int16_t frame_left(const StereoFrame frame) noexcept {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(frame & 0xFFFFU));
}
[[nodiscard]] constexpr std::int16_t frame_right(const StereoFrame frame) noexcept {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(frame >> 16U));
}

// Single producer (the emulation thread), single consumer (the device
// callback). Capacity is a power of two.
class AudioRing {
public:
  explicit AudioRing(const std::size_t capacity_log2 = 17U)
      : frames_(std::size_t{1} << capacity_log2), mask_(frames_.size() - 1U) {}

  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }
  // Frames waiting to be played (either side may call it).
  [[nodiscard]] std::size_t fill() const noexcept {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
  }

  // Producer: appends up to `frames.size()` frames; returns how many fit.
  std::size_t push(const std::span<const StereoFrame> frames) noexcept {
    const std::size_t write = write_.load(std::memory_order_relaxed);
    const std::size_t read = read_.load(std::memory_order_acquire);
    const std::size_t room = frames_.size() - (write - read);
    const std::size_t count = (std::min)(room, frames.size());
    for (std::size_t index = 0U; index < count; ++index)
      frames_[(write + index) & mask_] = frames[index];
    write_.store(write + count, std::memory_order_release);
    return count;
  }

  // Consumer: takes up to `out.size()` frames; returns how many it took.
  std::size_t pop(const std::span<StereoFrame> out) noexcept {
    const std::size_t read = read_.load(std::memory_order_relaxed);
    const std::size_t write = write_.load(std::memory_order_acquire);
    const std::size_t count = (std::min)(write - read, out.size());
    for (std::size_t index = 0U; index < count; ++index)
      out[index] = frames_[(read + index) & mask_];
    read_.store(read + count, std::memory_order_release);
    return count;
  }

  // Consumer: drops everything queued so far.
  void drop_all() noexcept {
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
  }

private:
  std::vector<StereoFrame> frames_;
  std::size_t mask_;
  std::atomic<std::size_t> write_{0U};
  std::atomic<std::size_t> read_{0U};
};

// Linear-interpolation resampler, continuous across calls (keeps the last
// input frame and the fractional position).
class StreamResampler {
public:
  void configure(const std::uint32_t input_rate, const std::uint32_t output_rate) noexcept {
    base_step_ = output_rate == 0U ? 1.0
                                   : static_cast<double>(input_rate) / static_cast<double>(output_rate);
    position_ = 0.0;
    previous_ = 0U;
  }
  // `correction` > 0 consumes the input faster (fewer output frames).
  // Appends the output frames to `out`.
  void process(const std::span<const StereoFrame> input, const double correction,
               std::vector<StereoFrame>& out) {
    const double step = base_step_ * (1.0 + correction);
    const std::size_t count = input.size();
    // Index 0 is the previous call's last frame, index k is input[k - 1].
    while (true) {
      const auto index = static_cast<std::size_t>(position_);
      if (index + 1U > count) break;
      const StereoFrame a = index == 0U ? previous_ : input[index - 1U];
      const StereoFrame b = input[index];
      const double t = position_ - static_cast<double>(index);
      const auto mix = [t](const std::int16_t x, const std::int16_t y) {
        const double value = static_cast<double>(x) + (static_cast<double>(y) - x) * t;
        return static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0));
      };
      out.push_back(stereo_frame(mix(frame_left(a), frame_left(b)),
                                 mix(frame_right(a), frame_right(b))));
      position_ += step;
    }
    if (count != 0U) {
      position_ -= static_cast<double>(count);
      previous_ = input[count - 1U];
    }
  }
  [[nodiscard]] double base_step() const noexcept { return base_step_; }

private:
  double base_step_ = 1.0;
  double position_ = 0.0;
  StereoFrame previous_ = 0U;
};

// The resampling correction that holds the ring at its target fill: the fill
// is smoothed over about half a second of calls; beyond a deadband the
// correction is proportional to the relative error, at most kMaxCorrection
// (0.5 %: a pitch change nobody hears).
class FillController {
public:
  static constexpr double kMaxCorrection = 0.005;
  static constexpr double kDeadband = 0.05;      // of the target
  static constexpr double kGain = 0.01;          // correction per 100 % error
  static constexpr double kSmoothing = 1.0 / 32.0;

  [[nodiscard]] double update(const std::size_t fill, const std::size_t target) noexcept {
    if (target == 0U) return 0.0;
    smoothed_ = primed_ ? smoothed_ + (static_cast<double>(fill) - smoothed_) * kSmoothing
                        : static_cast<double>(fill);
    primed_ = true;
    const double error = (smoothed_ - static_cast<double>(target)) / static_cast<double>(target);
    if (error > -kDeadband && error < kDeadband) return correction_ = 0.0;
    const double beyond = error < 0.0 ? error + kDeadband : error - kDeadband;
    return correction_ = std::clamp(beyond * kGain, -kMaxCorrection, kMaxCorrection);
  }
  void reset() noexcept { primed_ = false; correction_ = 0.0; }
  [[nodiscard]] double correction() const noexcept { return correction_; }
  [[nodiscard]] double smoothed() const noexcept { return smoothed_; }

private:
  double smoothed_ = 0.0;
  double correction_ = 0.0;
  bool primed_ = false;
};

} // namespace jfg::audio
