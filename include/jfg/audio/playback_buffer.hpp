#pragma once
#include <algorithm>
#include <cstdint>

namespace jfg::audio {
enum class PlaybackBufferAction { none, start, pause_empty, resume };

// Initial priming and recovery after a real underrun need a cushion. A low
// queue that still contains audio must keep playing: pausing early creates a
// silent gap even when the producer replenishes it before it would run dry.
[[nodiscard]] constexpr PlaybackBufferAction playback_buffer_action(
    bool started, bool playing, std::uint32_t queued_bytes,
    std::uint32_t prebuffer_bytes, std::uint32_t resume_bytes) noexcept {
  if (!started)
    return queued_bytes >= prebuffer_bytes ? PlaybackBufferAction::start
                                          : PlaybackBufferAction::none;
  if (playing)
    return queued_bytes == 0U ? PlaybackBufferAction::pause_empty
                             : PlaybackBufferAction::none;
  return queued_bytes >= resume_bytes ? PlaybackBufferAction::resume
                                      : PlaybackBufferAction::none;
}
// RGO: the cushion to rebuffer after an underrun. A PC whose emulation
// thread stalls longer than the audio queue (a slow frame, a shader compile)
// runs it dry; each time that happens the queue is refilled to one step
// (50 ms) more before playing again, up to a maximum (200 ms), so the next
// stall of the same length no longer cuts out. The cost is latency: sound
// then plays that much later than the picture, for the rest of the session.
[[nodiscard]] constexpr std::uint32_t grown_cushion(const std::uint32_t current,
                                                    const std::uint32_t step,
                                                    const std::uint32_t maximum) noexcept {
  return current >= maximum || maximum - current <= step ? maximum : current + step;
}

// RGO: keeps the host audio queue at its cushion over a long session.
// The queue's only inflow is the game's audio, which the game makes at the
// emulated AI rate, and VI pacing ties emulated time to the wall clock. Time
// the emulation loses for good (a host stall long enough for the emulated AI
// to run dry, a real-time floor reset) leaves the queue that much lower, and
// nothing refills it, so it steps down until it underruns. A sound card
// whose clock runs slightly fast or slow does the same steadily (or makes
// the queue, and the latency, grow). This trims each VI's wall deadline: a
// little earlier while the queue is below the cushion, a little later while
// it is above, by at most kMaxTrim of a VI. The game then runs at most 2 %
// fast or slow until the queue is back; pitch is unaffected.
class QueueLevelTrim {
public:
  static constexpr double kMaxTrim = 0.02;
  static constexpr double kDeadbandMs = 8.0;
  static constexpr double kMsPerPercent = 10.0;  // 1 % per 10 ms beyond the deadband
  static constexpr double kSmoothing = 1.0 / 32.0;

  // Called once per VI while audio plays. Returns how many nanoseconds
  // earlier (positive) or later (negative) the VI's deadline should be.
  [[nodiscard]] std::int64_t update(const std::uint32_t queued_bytes,
                                    const std::uint32_t target_bytes,
                                    const std::uint32_t bytes_per_second,
                                    const std::int64_t vi_nanoseconds) noexcept {
    if (bytes_per_second == 0U) return 0;
    smoothed_ = primed_ ? smoothed_ + (static_cast<double>(queued_bytes) - smoothed_) * kSmoothing
                        : static_cast<double>(queued_bytes);
    primed_ = true;
    const double error_ms = (smoothed_ - static_cast<double>(target_bytes)) * 1000.0 /
                            static_cast<double>(bytes_per_second);
    if (error_ms > -kDeadbandMs && error_ms < kDeadbandMs) return 0;
    const double beyond = error_ms < 0.0 ? error_ms + kDeadbandMs : error_ms - kDeadbandMs;
    const double fraction = std::clamp(-beyond / (kMsPerPercent * 100.0), -kMaxTrim, kMaxTrim);
    return static_cast<std::int64_t>(fraction * static_cast<double>(vi_nanoseconds));
  }
  // The queue jumped (an underrun refill, a pause): start smoothing afresh.
  void reset() noexcept { primed_ = false; }
  [[nodiscard]] double smoothed_bytes() const noexcept { return smoothed_; }

private:
  double smoothed_ = 0.0;
  bool primed_ = false;
};

// RGO: a hard upper bound on the host audio queue. Everything queued plays
// that much later than the picture; the trim above corrects small drifts
// only (2 %). If the device plays slower than the game produces by more than
// that, or stops while the game keeps running, the queue would grow without
// limit. Above the cushion plus kDropAboveMs the incoming game audio is
// dropped until the queue is back at the cushion (a short skip, the delay
// returns to normal); above the cushion plus kClearAboveMs the queue is
// cleared and refilled to the cushion (a short silence instead of seconds of
// stale sound).
enum class QueueBoundAction { keep, drop, clear };
inline constexpr std::uint32_t kDropAboveMs = 100U;
inline constexpr std::uint32_t kClearAboveMs = 1000U;
[[nodiscard]] constexpr QueueBoundAction queue_bound_action(
    const bool playing, const bool dropping, const std::uint32_t queued_bytes,
    const std::uint32_t cushion_bytes, const std::uint32_t bytes_per_second) noexcept {
  if (!playing) return QueueBoundAction::keep;
  const std::uint64_t queued = queued_bytes, cushion = cushion_bytes;
  if (queued > cushion + std::uint64_t{bytes_per_second} * kClearAboveMs / 1000U)
    return QueueBoundAction::clear;
  if (queued > cushion + std::uint64_t{bytes_per_second} * kDropAboveMs / 1000U ||
      (dropping && queued > cushion))
    return QueueBoundAction::drop;
  return QueueBoundAction::keep;
}

// RGO: how fast the sound device really plays, relative to its nominal
// rate, over windows of at least 10 s of uninterrupted playback, in tenths
// of a percent (1000: exact; 0: not measured yet). Shown by the Detailed
// counter: a device that plays a few percent slow explains a growing queue.
class PlaybackRateMeter {
public:
  static constexpr std::uint64_t kWindowUs = 10'000'000U;
  void restart() noexcept { started_ = false; }
  void sample(const std::uint64_t consumed_bytes, const std::uint64_t now_us,
              const std::uint32_t bytes_per_second) noexcept {
    if (!started_ || consumed_bytes < start_bytes_ || now_us < start_us_) {
      started_ = true;
      start_bytes_ = consumed_bytes;
      start_us_ = now_us;
      return;
    }
    const std::uint64_t elapsed = now_us - start_us_;
    if (elapsed < kWindowUs || bytes_per_second == 0U) return;
    const double rate = static_cast<double>(consumed_bytes - start_bytes_) * 1e6 /
                        (static_cast<double>(elapsed) * static_cast<double>(bytes_per_second));
    rate_tenths_ = static_cast<std::uint32_t>(std::clamp(rate * 1000.0 + 0.5, 1.0, 65535.0));
    start_bytes_ = consumed_bytes;
    start_us_ = now_us;
  }
  [[nodiscard]] std::uint32_t rate_tenths() const noexcept { return rate_tenths_; }

private:
  std::uint64_t start_bytes_ = 0U;
  std::uint64_t start_us_ = 0U;
  std::uint32_t rate_tenths_ = 0U;
  bool started_ = false;
};
} // namespace jfg::audio
