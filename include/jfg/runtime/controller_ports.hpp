#pragma once
#include "jfg/runtime/controller_mapping.hpp"
#include <array>
#include <cstdint>
namespace jfg {
// -1 automatic (sticky device), -2 disconnected, -3 keyboard; 0..3 physical
// slots.
// Availability for an explicit UI selection. Resolved automatic assignments
// reserve their device too; refreshing discovery never changes saved mappings.
inline bool controller_device_available(
    std::size_t player, int device,
    const std::array<ControllerMapping, 4> &mappings,
    const std::array<int, 4> &assigned,
    const std::array<bool, 4> &connected) {
  if (player >= mappings.size() || device < -3 || device >= 4)
    return false;
  if (device == -1 || device == -2)
    return true;
  if (device >= 0 && !connected[static_cast<std::size_t>(device)])
    return false;
  for (std::size_t p = 0; p < mappings.size(); ++p)
    if (p != player && (mappings[p].device == device || assigned[p] == device))
      return false;
  return true;
}
struct ControllerPortSample {
  std::uint16_t buttons = 0;
  std::int8_t x = 0, y = 0;
  bool connected = false;
};
// Physical input remains neutral until all players release controls after a
// menu or focus change. Connectivity is independent of input capture.
class ControllerInputGate {
public:
  bool blocked(bool captured, const std::array<ControllerPortSample, 4> &raw) {
    if (captured)
      waiting_ = true;
    else if (waiting_) {
      bool neutral = true;
      for (const auto &pad : raw)
        if (pad.buttons || pad.x || pad.y)
          neutral = false;
      if (neutral)
        waiting_ = false;
    }
    return captured || waiting_;
  }

private:
  bool waiting_ = false;
};
class ControllerPorts {
public:
  std::array<ControllerMapping, 4> mappings{};
  std::array<ControllerPortSample, 4> samples{};
  void configure(std::size_t p, const ControllerMapping &m) {
    if (mappings[p].device != m.device)
      assigned_[p] = -1;
    mappings[p] = m;
  }
  std::array<int, 4> devices(const std::array<bool, 4> &connected) {
    std::array<int, 4> result{-2, -2, -2, -2};
    std::array<bool, 4> used{};
    bool keyboard = false;
    for (std::size_t p = 0; p < 4; ++p) {
      int d = mappings[p].device;
      if (d >= 0 && d < 4 && !used[static_cast<std::size_t>(d)]) {
        result[p] = d;
        used[static_cast<std::size_t>(d)] = true;
      } else if (d == -3 && !keyboard) {
        result[p] = -3;
        keyboard = true;
      }
    }
    for (std::size_t p = 0; p < 4; ++p)
      if (mappings[p].device == -1) {
        int &d = assigned_[p];
        if (d >= 0 && used[static_cast<std::size_t>(d)])
          d = -1;
        if (d < 0)
          for (int c = 0; c < 4; ++c)
            if (connected[static_cast<std::size_t>(c)] &&
                !used[static_cast<std::size_t>(c)]) {
              d = c;
              break;
            }
        if (d >= 0) {
          result[p] = d;
          used[static_cast<std::size_t>(d)] = true;
        } else if (p == 0 && !keyboard) {
          result[p] = -3;
          keyboard = true;
        }
      }
    return result;
  }
  std::uint8_t mask() const {
    std::uint8_t m = 0;
    for (std::size_t p = 0; p < 4; ++p)
      if (samples[p].connected)
        m |= static_cast<std::uint8_t>(1U << p);
    return m;
  }

private:
  std::array<int, 4> assigned_{-1, -1, -1, -1};
};
// RGO dual input: Player 1's second device. While Player 1 plays on a
// controller it is the mouse and keyboard; while Player 1 plays on the
// keyboard it is the first connected controller. Only devices that no player
// has are used. -2 when there is none.
inline int companion_device(const std::array<int, 4> &devices,
                            const std::array<bool, 4> &connected) {
  const auto taken = [&](int device) {
    for (const int d : devices)
      if (d == device) return true;
    return false;
  };
  if (devices[0] >= 0) return taken(-3) ? -2 : -3;
  if (devices[0] == -3)
    for (int c = 0; c < 4; ++c)
      if (connected[static_cast<std::size_t>(c)] && !taken(c)) return c;
  return -2;
}
// Deliberate input on a port: a button or a clear stick push. Motion sensor
// noise and stick drift never count.
inline bool port_activity(const ControllerPortSample &sample) {
  return sample.buttons != 0 || sample.x > 24 || sample.x < -24 || sample.y > 24 || sample.y < -24;
}
// Both devices at once: buttons combine and the larger stick push wins.
inline ControllerPortSample merge_ports(const ControllerPortSample &a, const ControllerPortSample &b) {
  ControllerPortSample out;
  out.connected = a.connected || b.connected;
  out.buttons = static_cast<std::uint16_t>(a.buttons | b.buttons);
  const int ma = a.x * a.x + a.y * a.y, mb = b.x * b.x + b.y * b.y;
  const auto &stick = mb > ma ? b : a;
  out.x = stick.x;
  out.y = stick.y;
  return out;
}
// Without dual input, the device used last wins. A tie keeps the current one.
class LastUsedDevice {
public:
  // Returns true while the companion device is the active one.
  bool update(bool primary_activity, bool companion_activity) {
    if (companion_activity && !primary_activity) companion_ = true;
    else if (primary_activity && !companion_activity) companion_ = false;
    return companion_;
  }
  void reset() { companion_ = false; }
  [[nodiscard]] bool companion() const { return companion_; }

private:
  bool companion_ = false;
};
} // namespace jfg
