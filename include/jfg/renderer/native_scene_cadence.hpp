#pragma once
#include <cstdint>
#include <cmath>
#include "jfg/renderer/video_settings.hpp"

namespace jfg {
// Native scene delivery must not also ask RT64 to generate intervening
// frames. Keep the player's stored settings and device/simulation clocks.
inline VideoSettings native_scene_video(VideoSettings settings, bool native_present) {
  if (native_present) {
    settings.interpolate_frames = false;
    settings.interpolate_objects = false;
  }
  return settings;
}
// Experimental scene cadence only. Console VI, Count and audio deadlines stay
// in their original clock domain. A scene with no elapsed VI has no new whole
// simulation tick; it must not invent one just to increase the frame counter.
class NativeSceneCadence {
 public:
  void reset() { extra_pending_ = 0U; }
  void completed_vi_frame(unsigned extra=1U) { extra_pending_ = extra>3U?3U:extra; }
  bool take_extra_frame() {
    if(extra_pending_==0U)return false;
    --extra_pending_;return true;
  }
 private:
  unsigned extra_pending_ = 0U;
};

// Account for device ticks consumed by the ordinary sync, independently of
// scene count. Zero-tick scenes are diagnostic until all per-call updates are
// separated from rendering. This is deliberately not a player-facing option.
class SceneTickBudget {
 public:
  void anchor_if_needed(std::uint64_t vi) {
    if (!ready_) { ready_ = true; previous_ = vi; }
  }
  std::uint32_t consume(std::uint64_t vi) {
    if (!ready_ || vi < previous_) {
      ready_ = true; previous_ = vi; return 0;
    }
    const auto elapsed = vi - previous_;
    previous_ = vi;
    // Match the engine's six-tick stall bound; never replay a long load.
    return static_cast<std::uint32_t>(elapsed > 6 ? 6 : elapsed);
  }
  void reset() { ready_ = false; previous_ = 0; }
 private:
  bool ready_ = false;
  std::uint64_t previous_ = 0;
};
// Reviewed ground locomotion damping calls. Other powers retain their guest
// integer exponent, including powers used for trajectories or fixed constants.
inline constexpr bool native_player_damping_site(unsigned section,unsigned offset) {
  return section==15U?(offset==0x3030U || offset==0x3070U || offset==0x310CU || offset==0x3148U):
         section==16U?(offset==0x2C70U || offset==0x2CB0U || offset==0x2D4CU || offset==0x2D88U):
         section==17U?(offset==0x2EF8U || offset==0x2F38U || offset==0x2FD4U || offset==0x3010U):false;
}
inline float native_player_damping(float retention,float ticks) {
  return std::pow(retention,ticks);
}
// Continuous player time is independent of the integer device-tick budget.
// A reset anchors the next scene; loads cannot become a large movement step.
class NativePlayerStepClock {
 public:
  void reset() { ready_=false;previous_=0;period_=0.0; }
  float step(std::uint64_t microseconds) { return advance(microseconds,1000000.0/60.0); }
  float step_device(std::uint64_t count,std::uint64_t period) {
    if(period==0U){reset();return 0.0F;}
    return advance(count,static_cast<double>(period));
  }
 private:
  float advance(std::uint64_t value,double period) {
    if(!ready_ || value<previous_ || period!=period_) {
      ready_=true;previous_=value;period_=period;return 0.0F;
    }
    const double ticks=static_cast<double>(value-previous_)/period;previous_=value;
    return static_cast<float>(ticks>6.0?6.0:ticks);
  }
  bool ready_=false;
  std::uint64_t previous_=0;
  double period_=0.0;
};
}  // namespace jfg
