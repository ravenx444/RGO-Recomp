#include "jfg/audio/playback_buffer.hpp"
#include <array>
#include <cstdlib>
#include <iostream>

static void check(bool value) { if (!value) std::abort(); }

int main() {
  // RGO: the rebuffer cushion grows by a step per underrun, up to a maximum.
  check(jfg::audio::grown_cushion(100U, 50U, 200U) == 150U);
  check(jfg::audio::grown_cushion(150U, 50U, 200U) == 200U);
  check(jfg::audio::grown_cushion(200U, 50U, 200U) == 200U);
  check(jfg::audio::grown_cushion(180U, 50U, 200U) == 200U);
  using jfg::audio::PlaybackBufferAction;
  using jfg::audio::playback_buffer_action;
  constexpr std::uint32_t frequency = 22018U;
  constexpr auto prebuffer = frequency;
  constexpr auto resume = frequency * 4U * 3U / 20U;
  bool started = false, playing = false;
  unsigned starts = 0, pauses = 0, resumes = 0;
  auto service = [&](std::uint32_t bytes) {
    const auto action = playback_buffer_action(started, playing, bytes,
                                               prebuffer, resume);
    switch (action) {
    case PlaybackBufferAction::start:
      started = playing = true; ++starts; break;
    case PlaybackBufferAction::pause_empty:
      playing = false; ++pauses; break;
    case PlaybackBufferAction::resume:
      playing = true; ++resumes; break;
    case PlaybackBufferAction::none: break;
    }
  };
  service(0U);
  for (std::uint32_t bytes = 2944U; bytes < prebuffer; bytes += 2944U) {
    service(bytes); check(!playing && starts == 0U);
  }
  service(23552U); check(playing && starts == 1U);

  // Observed Vela cutscene: 6272 bytes (71 ms) remain; the next block arrives
  // 26 ms later. Simulate the 4096-byte device reads and continuing producer.
  // The former 75 ms low-watermark rule inserted a 93 ms silent pause here.
  std::uint32_t queued = 6272U;
  service(queued);
  constexpr std::array<int, 8> events{2944, -4096, 2944, -4096,
                                      2944, 2944, -4096, 2944};
  for (int delta : events) {
    check(static_cast<int>(queued) + delta > 0);
    queued = static_cast<std::uint32_t>(static_cast<int>(queued) + delta);
    service(queued); check(playing && pauses == 0U && resumes == 0U);
  }
  // Repeated service calls on a real empty queue pause once. Recovery remains
  // buffered, rather than playing each tiny arriving fragment immediately.
  service(0U); service(0U); check(!playing && pauses == 1U);
  for (std::uint32_t bytes = 2944U; bytes < resume; bytes += 2944U) {
    service(bytes); check(!playing && resumes == 0U);
  }
  service(14720U); check(playing && resumes == 1U && starts == 1U);
  service(14720U); check(resumes == 1U);
  std::cout << "playback buffer continuity and starvation recovery passed\n";
}
