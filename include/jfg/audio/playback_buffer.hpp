#pragma once
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
} // namespace jfg::audio
