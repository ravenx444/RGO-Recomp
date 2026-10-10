#pragma once
// RGO: gamepad support shared by the runtime and the launcher. Pure logic only;
// the SDL glue lives in jfg/platform/sdl_gamepads.hpp.
#include "jfg/runtime/controller_mapping.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace jfg {
enum class GamepadFamily : std::uint8_t { generic, xbox, playstation, nintendo };

// SDL_GameControllerType values, stable across SDL2 releases.
inline GamepadFamily gamepad_family(int sdl_type) noexcept {
  switch (sdl_type) {
  case 1: case 2: return GamepadFamily::xbox;              // Xbox 360, Xbox One/Series
  case 3: case 4: case 7: return GamepadFamily::playstation; // PS3, PS4, PS5
  case 5: case 11: case 12: case 13: return GamepadFamily::nintendo; // Switch Pro, Joy-Con
  default: return GamepadFamily::generic;
  }
}

inline std::string_view gamepad_family_name(GamepadFamily family) noexcept {
  switch (family) {
  case GamepadFamily::xbox: return "Xbox controller";
  case GamepadFamily::playstation: return "PlayStation controller";
  case GamepadFamily::nintendo: return "Nintendo controller";
  default: return "Controller";
  }
}

// Display name of a binding (see ControllerMapping) on the given controller.
// Nintendo names assume SDL's positional layout (SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0):
// SDL "A" is the bottom face button, which Nintendo labels B.
inline std::string_view gamepad_binding_name(int binding, GamepadFamily family) noexcept {
  static constexpr std::array<std::string_view, kBindingCount> generic{
      "A / Cross", "B / Circle", "X / Square", "Y / Triangle", "Back / Share", "Guide",
      "Start / Menu", "Left stick click", "Right stick click", "LB / L1", "RB / R1",
      "D-pad up", "D-pad down", "D-pad left", "D-pad right",
      "Left stick right", "Left stick left", "Left stick down", "Left stick up",
      "Right stick right", "Right stick left", "Right stick down", "Right stick up",
      "LT / L2", "LT negative", "RT / R2", "RT negative", "Touchpad click", "Share / Mic"};
  static constexpr std::array<std::string_view, kBindingCount> xbox{
      "A", "B", "X", "Y", "View", "Xbox button", "Menu", "Left stick click (LS)",
      "Right stick click (RS)", "LB", "RB", "D-pad up", "D-pad down", "D-pad left",
      "D-pad right", "Left stick right", "Left stick left", "Left stick down",
      "Left stick up", "Right stick right", "Right stick left", "Right stick down",
      "Right stick up", "LT", "LT negative", "RT", "RT negative", "Touchpad click", "Share"};
  static constexpr std::array<std::string_view, kBindingCount> playstation{
      "Cross", "Circle", "Square", "Triangle", "Create / Share", "PS button", "Options",
      "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
      "Left stick right", "Left stick left", "Left stick down", "Left stick up",
      "Right stick right", "Right stick left", "Right stick down", "Right stick up",
      "L2", "L2 negative", "R2", "R2 negative", "Touchpad click", "Mic"};
  static constexpr std::array<std::string_view, kBindingCount> nintendo{
      "B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick click", "Right stick click",
      "L", "R", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
      "Left stick right", "Left stick left", "Left stick down", "Left stick up",
      "Right stick right", "Right stick left", "Right stick down", "Right stick up",
      "ZL", "ZL negative", "ZR", "ZR negative", "Touchpad click", "Capture"};
  if (binding < 0 || binding >= kBindingCount) return "Unbound";
  const auto index = static_cast<std::size_t>(binding);
  switch (family) {
  case GamepadFamily::xbox: return xbox[index];
  case GamepadFamily::playstation: return playstation[index];
  case GamepadFamily::nintendo: return nintendo[index];
  default: return generic[index];
  }
}

// "Controller 1-4" slots. An XInput pad asks for its XInput user index, so
// assignments saved by the XInput-only builds still match; other pads take
// the lowest free slot. A pad keeps its slot until it disconnects.
class GamepadSlots {
public:
  static constexpr int kCount = 4;
  int attach(std::int32_t id, int preferred) noexcept {
    if (const int existing = find(id); existing >= 0) return existing;
    int slot = -1;
    if (preferred >= 0 && preferred < kCount && ids_[static_cast<std::size_t>(preferred)] < 0)
      slot = preferred;
    for (int i = 0; slot < 0 && i < kCount; ++i)
      if (ids_[static_cast<std::size_t>(i)] < 0) slot = i;
    if (slot >= 0) ids_[static_cast<std::size_t>(slot)] = id;
    return slot;
  }
  void detach(std::int32_t id) noexcept {
    for (auto &slot : ids_)
      if (slot == id) slot = -1;
  }
  [[nodiscard]] int find(std::int32_t id) const noexcept {
    for (int i = 0; i < kCount; ++i)
      if (id >= 0 && ids_[static_cast<std::size_t>(i)] == id) return i;
    return -1;
  }
  [[nodiscard]] std::int32_t id(int slot) const noexcept {
    return slot >= 0 && slot < kCount ? ids_[static_cast<std::size_t>(slot)] : -1;
  }

private:
  std::array<std::int32_t, kCount> ids_{-1, -1, -1, -1};
};

// Lightbar colour for a player (DualShock 4 / DualSense).
struct LightColor { std::uint8_t r = 0, g = 0, b = 0; };
inline LightColor player_light(int player) noexcept {
  constexpr std::array<LightColor, 4> colors{
      LightColor{0, 64, 255}, LightColor{255, 24, 24}, LightColor{0, 200, 40}, LightColor{255, 160, 0}};
  return colors[static_cast<std::size_t>(std::clamp(player, 0, 3))];
}

// The game's rumble requests, as seen at its rumble API (saves.c). The game
// stores strength^2 / 10 and drives the Rumble Pak motor fully above 490
// (strength 70); the presets use 40-75 for 0.2-0.5 s.
enum class GameRumbleCall : std::uint8_t { start, alter, max, stop, kill };
struct GameRumbleRequest {
  GameRumbleCall call = GameRumbleCall::stop;
  int channel = 0;     // N64 controller channel (joyGetController(player))
  int strength = 0;
  float seconds = 0.0F;
};
struct HostRumble {
  float amplitude = 0.0F;
  std::uint32_t milliseconds = 0U;
  bool operator==(const HostRumble &) const = default;
};
inline float rumble_amplitude(int strength) noexcept {
  return strength < 10 ? 0.0F : std::min(1.0F, static_cast<float>(strength) / 70.0F);
}
// Replays the requests the way the game's rumble state would, without a pak.
// Times are host seconds. A continuous rumble stops after 4 s, as the game
// rests the motor after 240 ticks.
class RumbleModel {
public:
  static constexpr double kMaxContinuous = 4.0;
  void apply(const GameRumbleRequest &request, double now) noexcept {
    if (request.call == GameRumbleCall::kill) {
      for (auto &channel : channels_) channel = {};
      return;
    }
    if (request.channel < 0 || request.channel >= 4) return;
    auto &c = channels_[static_cast<std::size_t>(request.channel)];
    const bool active = c.amplitude > 0.0F && now < c.end;
    const double seconds = std::isfinite(request.seconds) ? std::clamp(double(request.seconds), 0.0, kMaxContinuous) : 0.0;
    switch (request.call) {
    case GameRumbleCall::start:
      c.amplitude = rumble_amplitude(request.strength);
      c.begin = now;
      c.end = now + seconds;
      break;
    case GameRumbleCall::max:
      if (!active) { c.amplitude = 0.0F; c.begin = now; c.end = now; }
      if (request.strength != 0) c.amplitude = std::max(c.amplitude, rumble_amplitude(request.strength));
      c.end = std::max(c.end, now + seconds);
      break;
    case GameRumbleCall::alter:
      if (request.strength != 0) c.amplitude = rumble_amplitude(request.strength);
      if (seconds > 0.0) {
        if (!active) c.begin = now;
        c.end = now + seconds;
      }
      break;
    default:
      c = {};
      return;
    }
    c.end = std::min(c.end, c.begin + kMaxContinuous);
  }
  [[nodiscard]] HostRumble output(int channel, double now, int scale_percent) const noexcept {
    if (channel < 0 || channel >= 4) return {};
    const auto &c = channels_[static_cast<std::size_t>(channel)];
    const float amplitude = std::min(1.0F, c.amplitude * static_cast<float>(std::clamp(scale_percent, 0, 200)) / 100.0F);
    if (amplitude <= 0.0F || now >= c.end) return {};
    return {amplitude, static_cast<std::uint32_t>(std::max(1L, std::lround((c.end - now) * 1000.0)))};
  }
  void stop_all() noexcept { for (auto &channel : channels_) channel = {}; }

private:
  struct Channel { float amplitude = 0.0F; double begin = 0.0, end = 0.0; };
  std::array<Channel, 4> channels_{};
};

// Gyro integration with automatic bias calibration. SDL reports angular speed
// in rad/s (x pitch, positive nose up; y yaw, positive turning left; z roll)
// and acceleration in m/s^2. Controllers drift slightly at rest, so whenever
// the controller lies still for a second the average rate becomes the bias.
// Very small corrected rates are tightened towards zero to hide noise.
struct GyroDelta {
  double yaw = 0.0, pitch = 0.0; // radians since the last take()
};
class GyroIntegrator {
public:
  static constexpr double kStillSeconds = 1.0;
  static constexpr double kStillRate = 0.15;      // rad/s on any axis
  static constexpr double kStillSpread = 0.04;    // rad/s range within the window
  static constexpr double kStillAccelSpread = 0.6; // m/s^2 range of |accel|
  static constexpr double kTightening = 0.005;    // rad/s, about 0.3 deg/s
  void add(const std::array<float, 3> &gyro, const std::array<float, 3> &accel, double seconds) noexcept {
    if (!(seconds > 0.0) || seconds > 0.05) seconds = 0.0; // stalls and timestamp jumps
    for (const float value : gyro)
      if (!std::isfinite(value)) return;
    calibrate(gyro, accel, seconds);
    std::array<double, 3> rate{};
    for (std::size_t i = 0; i < 3; ++i) {
      rate[i] = static_cast<double>(gyro[i]) - bias_[i];
      const double magnitude = std::abs(rate[i]);
      if (magnitude < kTightening) rate[i] *= magnitude / kTightening;
    }
    pitch_ += rate[0] * seconds;
    yaw_ += rate[1] * seconds;
  }
  GyroDelta take() noexcept {
    const GyroDelta out{yaw_, pitch_};
    yaw_ = pitch_ = 0.0;
    return out;
  }
  void reset() noexcept { *this = GyroIntegrator{}; }
  [[nodiscard]] bool calibrated() const noexcept { return calibrated_; }
  [[nodiscard]] const std::array<double, 3> &bias() const noexcept { return bias_; }

private:
  void restart(const std::array<float, 3> &gyro, double accel) noexcept {
    window_ = 0.0;
    count_ = 0;
    sum_ = {};
    for (std::size_t i = 0; i < 3; ++i) low_[i] = high_[i] = gyro[i];
    accel_low_ = accel_high_ = accel;
  }
  void calibrate(const std::array<float, 3> &gyro, const std::array<float, 3> &accel, double seconds) noexcept {
    const double a = std::sqrt(double(accel[0]) * accel[0] + double(accel[1]) * accel[1] + double(accel[2]) * accel[2]);
    bool still = count_ > 0;
    for (std::size_t i = 0; i < 3; ++i) {
      if (std::abs(gyro[i]) > kStillRate) still = false;
      low_[i] = std::min(low_[i], double(gyro[i]));
      high_[i] = std::max(high_[i], double(gyro[i]));
      if (high_[i] - low_[i] > kStillSpread) still = false;
    }
    accel_low_ = std::min(accel_low_, a);
    accel_high_ = std::max(accel_high_, a);
    if (accel_high_ - accel_low_ > kStillAccelSpread) still = false;
    if (!still) {
      restart(gyro, a);
      count_ = 1;
      for (std::size_t i = 0; i < 3; ++i) sum_[i] = gyro[i];
      return;
    }
    ++count_;
    window_ += seconds;
    for (std::size_t i = 0; i < 3; ++i) sum_[i] += gyro[i];
    if (window_ < kStillSeconds) return;
    for (std::size_t i = 0; i < 3; ++i) {
      const double mean = sum_[i] / static_cast<double>(count_);
      bias_[i] = calibrated_ ? bias_[i] + 0.5 * (mean - bias_[i]) : mean;
    }
    calibrated_ = true;
    restart(gyro, a);
    count_ = 1;
    for (std::size_t i = 0; i < 3; ++i) sum_[i] = gyro[i];
  }

  double yaw_ = 0.0, pitch_ = 0.0;
  std::array<double, 3> bias_{};
  bool calibrated_ = false;
  double window_ = 0.0;
  std::size_t count_ = 0;
  std::array<double, 3> sum_{}, low_{}, high_{};
  double accel_low_ = 0.0, accel_high_ = 0.0;
};
} // namespace jfg
