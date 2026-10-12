// RGO callback audio path: the SPSC ring, the stream resampler and the fill
// controller (jfg/audio/audio_ring.hpp).
#include "jfg/audio/audio_ring.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

static void check(bool value, const char *what) {
  if (!value) {
    std::cerr << "FAIL: " << what << '\n';
    std::exit(1);
  }
}

int main() {
  using namespace jfg::audio;
  check(frame_left(stereo_frame(-5, 7)) == -5 && frame_right(stereo_frame(-5, 7)) == 7,
        "stereo frame packing");
  {
    AudioRing ring(3U);  // 8 frames
    std::vector<StereoFrame> in{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    check(ring.push(in) == 8U && ring.fill() == 8U, "push stops at capacity");
    std::vector<StereoFrame> out(5);
    check(ring.pop(out) == 5U && out[0] == 1U && out[4] == 5U, "pop in order");
    check(ring.push(std::span<const StereoFrame>(in).subspan(8)) == 2U, "push wraps");
    out.assign(10, 0U);
    check(ring.pop(out) == 5U && out[0] == 6U && out[4] == 10U, "pop across the wrap");
    ring.push(in);
    ring.drop_all();
    check(ring.fill() == 0U, "drop_all empties");
  }
  {
    // Producer and consumer threads: every frame arrives once, in order.
    AudioRing ring(10U);
    constexpr std::uint32_t total = 200000U;
    std::thread producer([&ring] {
      std::uint32_t next = 0U;
      while (next < total) {
        std::vector<StereoFrame> chunk;
        for (std::uint32_t i = 0; i < 97U && next + i < total; ++i) chunk.push_back(next + i);
        next += static_cast<std::uint32_t>(ring.push(chunk));
      }
    });
    std::uint32_t expected = 0U;
    bool ordered = true;
    std::vector<StereoFrame> out(64);
    while (expected < total) {
      const std::size_t got = ring.pop(out);
      for (std::size_t i = 0; i < got; ++i) ordered = ordered && out[i] == expected++;
    }
    producer.join();
    check(ordered, "SPSC ring keeps order across threads");
  }
  {
    // Resampling 22047 -> 48000 Hz yields the right number of frames over a
    // long stream, and a sine stays a sine (no clicks at buffer edges).
    StreamResampler resampler;
    resampler.configure(22047U, 48000U);
    std::vector<StereoFrame> out;
    constexpr int seconds = 10, chunk = 368;
    double phase = 0.0;
    for (int produced = 0; produced < 22047 * seconds; produced += chunk) {
      std::vector<StereoFrame> in;
      for (int i = 0; i < chunk; ++i) {
        const auto v = static_cast<std::int16_t>(std::lround(10000.0 * std::sin(phase)));
        in.push_back(stereo_frame(v, static_cast<std::int16_t>(-v)));
        phase += 2.0 * 3.14159265358979 * 440.0 / 22047.0;
      }
      resampler.process(in, 0.0, out);
    }
    const double expected = 48000.0 * (22047.0 * seconds - std::fmod(22047.0 * seconds, chunk) +
                                       (22047 * seconds % chunk == 0 ? 0 : chunk)) / 22047.0;
    check(std::abs(static_cast<double>(out.size()) - expected) < 3.0, "output frame count");
    // A 440 Hz sine at 48 kHz moves at most about 577 per sample.
    int worst = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
      worst = std::max(worst, std::abs(frame_left(out[i]) - frame_left(out[i - 1])));
    check(worst < 650, "no discontinuity across buffers");
    // A positive correction consumes faster: fewer output frames.
    StreamResampler fast;
    fast.configure(22047U, 48000U);
    std::vector<StereoFrame> in(22047, stereo_frame(1, 1)), slow_out, fast_out;
    resampler.configure(22047U, 48000U);
    resampler.process(in, 0.0, slow_out);
    fast.process(in, 0.005, fast_out);
    check(fast_out.size() < slow_out.size() && slow_out.size() - fast_out.size() > 200U,
          "correction changes the output rate");
  }
  {
    // The fill controller: nothing inside the deadband, the sign follows the
    // error, never more than 0.5 %.
    FillController controller;
    check(controller.update(1020U, 1000U) == 0.0, "deadband");
    controller.reset();
    check(controller.update(5000U, 1000U) == FillController::kMaxCorrection, "capped high");
    controller.reset();
    const double low = controller.update(500U, 1000U);
    check(low < 0.0 && low > -FillController::kMaxCorrection, "low fill slows consumption");
    // Closed loop: a device 0.3 % slower than nominal; the ring settles at
    // the target instead of growing.
    AudioRing ring(17U);
    StreamResampler resampler;
    resampler.configure(22047U, 48000U);
    controller.reset();
    const std::size_t target = 4800U;  // 100 ms
    std::vector<StereoFrame> in(368, stereo_frame(0, 0)), out, sink(48000);
    double device_debt = 0.0;
    for (int call = 0; call < 6000; ++call) {  // about 100 s
      out.clear();
      resampler.process(in, controller.update(ring.fill(), target), out);
      ring.push(out);
      device_debt += 368.0 * 48000.0 / 22047.0 * 0.997;
      const auto take = static_cast<std::size_t>(device_debt);
      device_debt -= static_cast<double>(take);
      ring.pop(std::span<StereoFrame>(sink).first(std::min(take, sink.size())));
    }
    check(ring.fill() > target / 2U && ring.fill() < target * 3U / 2U,
          "the ring holds near the target with a slow device");
  }
  std::cout << "audio ring tests passed\n";
  return 0;
}
