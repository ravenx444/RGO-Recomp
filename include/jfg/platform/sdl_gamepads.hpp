#pragma once
// RGO: SDL2 GameController input for the runtime and the launcher. One host
// thread owns SDL's joystick subsystem: Windows device notifications are tied
// to the thread that initialised it, and polling it quickly keeps motion
// sensor data fresh. Other threads read snapshots and queue output.
#include "jfg/runtime/gamepad.hpp"
#include <SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jfg {
struct GamepadInfo {
  bool connected = false;
  GamepadFamily family = GamepadFamily::generic;
  int type = 0; // SDL_GameControllerType
  std::string name;
  bool rumble = false, light = false;
  bool gyro = false, gyro_calibrated = false;
};
struct GamepadSnapshot {
  std::array<GamepadInfo, 4> info{};
  std::array<StandardControllerSample, 4> pads{};
  std::uint64_t generation = 0; // changes when a pad connects or disconnects
};

class SdlGamepads {
public:
  SdlGamepads() = default;
  SdlGamepads(const SdlGamepads &) = delete;
  SdlGamepads &operator=(const SdlGamepads &) = delete;
  ~SdlGamepads() { stop(); }

  // Starts the input thread. False when SDL's controller support is unavailable.
  bool start() {
    if (thread_.joinable()) return ready_;
    std::unique_lock lock(mutex_);
    thread_ = std::thread([this] { run(); });
    started_.wait(lock, [this] { return initialized_; });
    return ready_;
  }
  void stop() {
    if (!thread_.joinable()) return;
    quit_ = true;
    thread_.join();
  }
  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] GamepadSnapshot snapshot() const {
    const std::lock_guard lock(mutex_);
    return published_;
  }
  // Amplitudes 0..1. A zero duration or amplitude stops the motors.
  void rumble(int slot, float low, float high, std::uint32_t milliseconds) {
    if (slot < 0 || slot >= GamepadSlots::kCount) return;
    const std::lock_guard lock(mutex_);
    auto &out = output_[static_cast<std::size_t>(slot)];
    out.low = std::clamp(low, 0.0F, 1.0F);
    out.high = std::clamp(high, 0.0F, 1.0F);
    out.milliseconds = milliseconds;
    ++out.rumble_sequence;
  }
  void light(int slot, LightColor color) {
    if (slot < 0 || slot >= GamepadSlots::kCount) return;
    const std::lock_guard lock(mutex_);
    auto &out = output_[static_cast<std::size_t>(slot)];
    if (out.has_light && out.color.r == color.r && out.color.g == color.g && out.color.b == color.b) return;
    out.color = color;
    out.has_light = true;
    ++out.light_sequence;
  }
  // Gyro rotation since the last call (radians; yaw positive to the left,
  // pitch positive nose up), bias-corrected. Zero for pads without a gyro.
  GyroDelta take_gyro(int slot) {
    if (slot < 0 || slot >= GamepadSlots::kCount) return {};
    const std::lock_guard lock(mutex_);
    const GyroDelta out = gyro_[static_cast<std::size_t>(slot)];
    gyro_[static_cast<std::size_t>(slot)] = {};
    return out;
  }
  void stop_rumble() {
    for (int slot = 0; slot < GamepadSlots::kCount; ++slot) rumble(slot, 0.0F, 0.0F, 0U);
  }

private:
  struct Output {
    float low = 0.0F, high = 0.0F;
    std::uint32_t milliseconds = 0U;
    std::uint64_t rumble_sequence = 0U, light_sequence = 0U;
    LightColor color{};
    bool has_light = false;
  };
  struct Pad {
    SDL_GameController *controller = nullptr;
    SDL_JoystickID id = -1;
    std::uint64_t rumble_applied = 0U, light_applied = 0U;
    bool accel = false;
    Uint64 gyro_time = 0U;
    std::chrono::steady_clock::time_point gyro_host{};
    GyroIntegrator gyro;
  };

  static bool xinput(SDL_JoystickGUID guid) noexcept {
    return guid.data[14] == 'x'; // SDL marks XInput devices this way.
  }
  void run() {
    // Xbox pads stay on SDL's XInput backend (stable XInput user indices);
    // PlayStation and Nintendo pads use HIDAPI. Positional face buttons keep
    // one default mapping for every pad. The PS rumble hints also enable the
    // full Bluetooth reports that carry rumble, lightbar and motion data.
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetMainReady();
    const bool ok = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    if (ok) {
      // Nothing reads SDL's event queue; state is polled below.
      SDL_JoystickEventState(SDL_IGNORE);
      SDL_GameControllerEventState(SDL_IGNORE);
    }
    {
      const std::lock_guard lock(mutex_);
      ready_ = ok;
      initialized_ = true;
    }
    started_.notify_all();
    if (!ok) return;
#if defined(_WIN32)
    // Sleep() granularity is 15.6 ms by default; poll at about 500 Hz.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002U /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
#endif
    while (!quit_) {
      poll();
#if defined(_WIN32)
      LARGE_INTEGER due{};
      due.QuadPart = -20000; // 2 ms, relative, in 100 ns units
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, 100);
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
    }
#if defined(_WIN32)
    if (timer) CloseHandle(timer);
#endif
    for (auto &pad : pads_)
      if (pad.controller) {
        SDL_GameControllerRumble(pad.controller, 0, 0, 0);
        SDL_GameControllerClose(pad.controller);
        pad = {};
      }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
  }
  void poll() {
    SDL_GameControllerUpdate(); // also detects added and removed devices
    bool changed = false;
    for (std::size_t slot = 0; slot < pads_.size(); ++slot) {
      auto &pad = pads_[slot];
      if (pad.controller && !SDL_GameControllerGetAttached(pad.controller)) {
        SDL_GameControllerClose(pad.controller);
        slots_.detach(pad.id);
        pad = {};
        working_.info[slot] = {};
        working_.pads[slot] = {};
        changed = true;
      }
    }
    std::array<Output, 4> output{};
    {
      const std::lock_guard lock(mutex_);
      output = output_;
    }
    // XInput pads first, so they keep their XInput user index as their slot.
    const int count = SDL_NumJoysticks();
    for (int pass = 0; pass < 2; ++pass)
      for (int index = 0; index < count; ++index) {
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
        if (id < 0 || slots_.find(id) >= 0 || !SDL_IsGameController(index)) continue;
        const bool is_xinput = xinput(SDL_JoystickGetDeviceGUID(index));
        if ((pass == 0) != is_xinput) continue;
        const int slot = slots_.attach(id, is_xinput ? SDL_JoystickGetDevicePlayerIndex(index) : -1);
        if (slot < 0) continue;
        SDL_GameController *controller = SDL_GameControllerOpen(index);
        if (!controller) {
          slots_.detach(id);
          continue;
        }
        if (!is_xinput) SDL_GameControllerSetPlayerIndex(controller, slot);
        auto &pad = pads_[static_cast<std::size_t>(slot)];
        // A new pad does not replay an old rumble; it does take the slot's colour.
        pad = {};
        pad.controller = controller;
        pad.id = id;
        pad.rumble_applied = output[static_cast<std::size_t>(slot)].rumble_sequence;
        auto &info = working_.info[static_cast<std::size_t>(slot)];
        info.connected = true;
        info.type = static_cast<int>(SDL_GameControllerGetType(controller));
        info.family = gamepad_family(info.type);
        const char *name = SDL_GameControllerName(controller);
        info.name = name ? name : std::string(gamepad_family_name(info.family));
        info.rumble = SDL_GameControllerHasRumble(controller) == SDL_TRUE;
        info.light = SDL_GameControllerHasLED(controller) == SDL_TRUE;
        // Motion sensors (DualShock 4, DualSense, Switch Pro, Joy-Con). The
        // accelerometer only helps tell when the controller lies still.
        info.gyro = SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE &&
                    SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, SDL_TRUE) == 0;
        pad.accel = info.gyro && SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE &&
                    SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, SDL_TRUE) == 0;
        info.gyro_calibrated = false;
        changed = true;
      }
    for (std::size_t slot = 0; slot < pads_.size(); ++slot) {
      auto &pad = pads_[slot];
      if (!pad.controller) continue;
      auto &sample = working_.pads[slot];
      for (int button = 0; button < 15; ++button)
        sample.buttons[static_cast<std::size_t>(button)] =
            SDL_GameControllerGetButton(pad.controller, static_cast<SDL_GameControllerButton>(button)) != 0;
      sample.buttons[15] = SDL_GameControllerGetButton(pad.controller, SDL_CONTROLLER_BUTTON_TOUCHPAD) != 0;
      sample.buttons[16] = SDL_GameControllerGetButton(pad.controller, SDL_CONTROLLER_BUTTON_MISC1) != 0;
      for (int axis = 0; axis < 6; ++axis)
        sample.axes[static_cast<std::size_t>(axis)] =
            SDL_GameControllerGetAxis(pad.controller, static_cast<SDL_GameControllerAxis>(axis));
      if (working_.info[slot].gyro) read_gyro(pad, slot);
      const auto &out = output[slot];
      if (out.rumble_sequence != pad.rumble_applied) {
        pad.rumble_applied = out.rumble_sequence;
        const bool on = out.milliseconds > 0U && (out.low > 0.0F || out.high > 0.0F);
        SDL_GameControllerRumble(pad.controller, static_cast<Uint16>(on ? out.low * 65535.0F : 0.0F),
                                 static_cast<Uint16>(on ? out.high * 65535.0F : 0.0F), on ? out.milliseconds : 0U);
      }
      if (out.has_light && out.light_sequence != pad.light_applied) {
        pad.light_applied = out.light_sequence;
        SDL_GameControllerSetLED(pad.controller, out.color.r, out.color.g, out.color.b);
      }
    }
    if (changed) ++working_.generation;
    const std::lock_guard lock(mutex_);
    published_ = working_;
    for (std::size_t slot = 0; slot < pads_.size(); ++slot) {
      // Whatever nobody takes is bounded, so a late reader cannot get a jump.
      const GyroDelta add = pads_[slot].gyro.take();
      auto &total = gyro_[slot];
      if (!pads_[slot].controller) { total = {}; continue; }
      total.yaw = std::clamp(total.yaw + add.yaw, -0.5, 0.5);
      total.pitch = std::clamp(total.pitch + add.pitch, -0.5, 0.5);
    }
  }
  void read_gyro(Pad &pad, std::size_t slot) {
    std::array<float, 3> rate{}, accel{};
    Uint64 time = 0U;
    if (SDL_GameControllerGetSensorDataWithTimestamp(pad.controller, SDL_SENSOR_GYRO, &time, rate.data(), 3) != 0)
      return;
    double seconds = 0.0;
    if (time != 0U) { // sensor timestamps in microseconds, when the driver has them
      if (time == pad.gyro_time) return; // no new report since the last poll
      seconds = pad.gyro_time != 0U && time > pad.gyro_time ? static_cast<double>(time - pad.gyro_time) * 1e-6 : 0.0;
      pad.gyro_time = time;
    } else {
      const auto now = std::chrono::steady_clock::now();
      seconds = pad.gyro_host == std::chrono::steady_clock::time_point{} ? 0.0
          : std::chrono::duration<double>(now - pad.gyro_host).count();
      pad.gyro_host = now;
    }
    if (pad.accel) SDL_GameControllerGetSensorData(pad.controller, SDL_SENSOR_ACCEL, accel.data(), 3);
    pad.gyro.add(rate, accel, seconds);
    working_.info[slot].gyro_calibrated = pad.gyro.calibrated();
  }

  mutable std::mutex mutex_;
  std::condition_variable started_;
  bool initialized_ = false;
  std::atomic<bool> ready_{false}, quit_{false};
  std::thread thread_;
  GamepadSlots slots_;
  std::array<Pad, 4> pads_{};
  std::array<Output, 4> output_{};
  GamepadSnapshot working_{}, published_{};
  std::array<GyroDelta, 4> gyro_{};
};
} // namespace jfg
