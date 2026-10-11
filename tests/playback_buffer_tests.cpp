#include "jfg/audio/playback_buffer.hpp"
#include <algorithm>
#include <array>
#include <utility>
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
  {
    // RGO queue-level trim. Simulated session: the game queues one 32 ms
    // buffer every second VI of emulated time; the sound card consumes on the
    // wall clock; VI deadlines are 1/59.826 s apart in emulated time plus the
    // accumulated trim. Without the trim, a stall that loses 60 ms of
    // emulated time and a sound card 0.05 % fast both stay in the queue for
    // good; with it the queue returns to the cushion.
    constexpr double kBytesPerSecond = 22047.0 * 4.0;
    constexpr double kViSeconds = 783520.0 / 46875000.0;
    constexpr std::uint32_t kTarget = static_cast<std::uint32_t>(kBytesPerSecond / 10.0);
    const auto simulate = [&](bool trimmed, double card_rate, int vis, double lost_at_vi_100) {
      jfg::audio::QueueLevelTrim trim;
      double level = kTarget, lowest = level;
      for (int vi = 0; vi < vis; ++vi) {
        double step = kViSeconds;
        if (trimmed)
          step -= static_cast<double>(trim.update(static_cast<std::uint32_t>(level), kTarget,
              static_cast<std::uint32_t>(kBytesPerSecond),
              static_cast<std::int64_t>(kViSeconds * 1e9))) / 1e9;
        if (vi == 100) step += lost_at_vi_100;  // wall time with no emulated audio
        level -= step * kBytesPerSecond * card_rate;
        if (vi % 2 == 1) level += 2.0 * kViSeconds * kBytesPerSecond;
        lowest = std::min(lowest, level);
      }
      return std::pair{level, lowest};
    };
    const double ms = kBytesPerSecond / 1000.0;
    // A 60 ms loss: untrimmed it stays 60 ms low; trimmed it is back near
    // the cushion 20 s later.
    check(simulate(false, 1.0, 1200, 0.060).first < kTarget - 50.0 * ms);
    const double recovered = simulate(true, 1.0, 1200, 0.060).first;
    check(recovered > kTarget - 30.0 * ms && recovered < kTarget + 30.0 * ms);
    // A sound card 0.05 % fast over 30 minutes: untrimmed the queue runs dry
    // (0.9 s of drift), trimmed it stays near the cushion.
    const int half_hour = static_cast<int>(1800.0 / kViSeconds);
    check(simulate(false, 1.0005, half_hour, 0.0).second < 0.0);
    const auto held = simulate(true, 1.0005, half_hour, 0.0);
    check(held.second > kTarget - 60.0 * ms && held.first < kTarget + 30.0 * ms);
    // A slow card: untrimmed the queue (and latency) grows; trimmed it does not.
    check(simulate(false, 0.9995, half_hour, 0.0).first > kTarget + 500.0 * ms);
    check(simulate(true, 0.9995, half_hour, 0.0).first < kTarget + 30.0 * ms);
    // Inside the deadband nothing is trimmed; the trim never exceeds 2 %.
    jfg::audio::QueueLevelTrim quiet;
    check(quiet.update(kTarget + static_cast<std::uint32_t>(5.0 * ms), kTarget,
                       static_cast<std::uint32_t>(kBytesPerSecond), 16'715'000) == 0);
    jfg::audio::QueueLevelTrim starved;
    check(starved.update(0U, kTarget, static_cast<std::uint32_t>(kBytesPerSecond), 16'715'000) ==
          static_cast<std::int64_t>(0.02 * 16'715'000));
    jfg::audio::QueueLevelTrim flooded;
    check(flooded.update(kTarget * 10U, kTarget, static_cast<std::uint32_t>(kBytesPerSecond),
                         16'715'000) == -static_cast<std::int64_t>(0.02 * 16'715'000));
  }
  {
    // RGO queue bound: drop above cushion + 100 ms until back at the
    // cushion, clear above cushion + 1 s; nothing while paused.
    using jfg::audio::QueueBoundAction;
    using jfg::audio::queue_bound_action;
    constexpr std::uint32_t bps = 22047U * 4U, cushion = bps * 150U / 1000U;
    constexpr std::uint32_t ms = bps / 1000U;
    check(queue_bound_action(true, false, cushion + 50U * ms, cushion, bps) == QueueBoundAction::keep);
    check(queue_bound_action(true, false, cushion + 120U * ms, cushion, bps) == QueueBoundAction::drop);
    check(queue_bound_action(true, true, cushion + 20U * ms, cushion, bps) == QueueBoundAction::drop);
    check(queue_bound_action(true, true, cushion - 1U, cushion, bps) == QueueBoundAction::keep);
    check(queue_bound_action(true, false, cushion + 1100U * ms, cushion, bps) == QueueBoundAction::clear);
    check(queue_bound_action(false, true, cushion * 20U, cushion, bps) == QueueBoundAction::keep);
    // A device playing 5 % slow, more than the trim can absorb: with the
    // bound the queue never gets more than about a buffer past cushion + 100 ms.
    jfg::audio::QueueLevelTrim trim;
    double level = cushion, highest = level;
    bool dropping = false;
    constexpr double kVi = 783520.0 / 46875000.0;
    for (int vi = 0; vi < 60 * 600; ++vi) {
      const double step = kVi - static_cast<double>(trim.update(static_cast<std::uint32_t>(level),
          cushion, bps, static_cast<std::int64_t>(kVi * 1e9))) / 1e9;
      level = std::max(0.0, level - step * bps * 0.95);
      if (vi % 2 == 1) {
        const auto action = queue_bound_action(true, dropping, static_cast<std::uint32_t>(level),
                                               cushion, bps);
        dropping = action == QueueBoundAction::drop;
        if (action == QueueBoundAction::keep) level += 2.0 * kVi * bps;
      }
      highest = std::max(highest, level);
    }
    check(highest < cushion + 140.0 * ms);
    // The rate meter: 10 s windows of uninterrupted playback.
    jfg::audio::PlaybackRateMeter meter;
    meter.sample(0U, 1'000'000U, bps);
    meter.sample(bps * 5U, 6'000'000U, bps);
    check(meter.rate_tenths() == 0U);  // not 10 s yet
    meter.sample(static_cast<std::uint64_t>(bps) * 97U / 10U, 11'000'000U, bps);
    check(meter.rate_tenths() == 970U);  // 97.0 %: the device plays 3 % slow
    meter.restart();
    meter.sample(0U, 20'000'000U, bps);  // a cleared queue starts a new window
    meter.sample(static_cast<std::uint64_t>(bps) * 10U, 30'000'000U, bps);
    check(meter.rate_tenths() == 1000U);
  }
  std::cout << "playback buffer continuity and starvation recovery passed\n";
}
