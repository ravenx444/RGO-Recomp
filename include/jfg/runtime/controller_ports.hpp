#pragma once
#include "jfg/runtime/controller_mapping.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
namespace jfg {
// RGO: a physical controller, "vvvv:pppp" (USB vendor and product, lower-case
// hex) plus "/serial" when the controller reports one. Slot numbers depend on
// the order pads were found in, and each process (launcher, controllers
// window, game) finds them on its own, so a player's chosen controller is
// remembered by this identity. Empty when unknown.
inline bool controller_identity_valid(std::string_view id) {
  if (id.size() < 9 || id.size() > 58 || id[4] != ':') return false;
  for (std::size_t i = 0; i < 9; ++i)
    if (i != 4 && !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  if (id.size() == 9) return true;
  if (id[9] != '/' || id.size() == 10) return false;
  for (const char c : id.substr(10))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ':' || c == '.' ||
          c == '_' || c == '-'))
      return false;
  return true;
}
inline std::string controller_identity(std::uint16_t vendor, std::uint16_t product, std::string_view serial) {
  if (vendor == 0 && product == 0) return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string id;
  for (const std::uint16_t value : {vendor, product}) {
    if (!id.empty()) id += ':';
    for (int shift = 12; shift >= 0; shift -= 4) id += hex[(value >> shift) & 0xF];
  }
  std::string clean;
  for (const char c : serial)
    if (clean.size() < 48 && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              c == ':' || c == '.' || c == '_' || c == '-'))
      clean += c;
  if (!clean.empty()) id += '/' + clean;
  return id;
}
// 2: the same controller; 1: the same model (another unit, or no serial); 0: no.
inline int controller_identity_match(std::string_view saved, std::string_view pad) {
  if (saved.size() < 9 || pad.size() < 9 || saved.substr(0, 9) != pad.substr(0, 9)) return 0;
  return saved == pad ? 2 : 1;
}
// controller-ids.ini: "playerN=<slot>,<identity>" for players who chose a
// controller. An entry only counts while that player's controller-N.ini still
// names the same slot, so a mapping saved by a tool that does not know about
// this file wins. Unknown or invalid lines are ignored.
struct ControllerPin {
  int slot = -2;
  std::string id;
};
inline std::array<ControllerPin, 4> parse_controller_pins(std::string_view text) {
  std::array<ControllerPin, 4> pins{};
  while (!text.empty()) {
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1U);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() < 12 || line.substr(0, 6) != "player" || line[6] < '1' || line[6] > '4' || line[7] != '=' ||
        line[8] < '0' || line[8] > '3' || line[9] != ',')
      continue;
    const auto id = line.substr(10);
    if (!controller_identity_valid(id)) continue;
    auto &pin = pins[static_cast<std::size_t>(line[6] - '1')];
    pin.slot = line[8] - '0';
    pin.id = std::string(id);
  }
  return pins;
}
inline std::string serialize_controller_pins(const std::array<ControllerPin, 4> &pins) {
  std::string out = "version=1\n";
  for (std::size_t p = 0; p < pins.size(); ++p)
    if (pins[p].slot >= 0 && pins[p].slot < 4 && controller_identity_valid(pins[p].id))
      out += "player" + std::to_string(p + 1) + "=" + std::to_string(pins[p].slot) + "," + pins[p].id + "\n";
  return out;
}
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
inline bool controller_pinned(const ControllerMapping &mapping, const ControllerPin &pin) {
  return mapping.device >= 0 && pin.slot == mapping.device && controller_identity_valid(pin.id);
}
// Whether the Controllers page can offer a device to a player. Any connected
// controller can be chosen, including one another player has: choosing it
// moves it (see claim_controller).
inline bool controller_device_selectable(int device, const std::array<bool, 4> &connected) {
  if (device == -1 || device == -2 || device == -3) return true;
  return device >= 0 && device < 4 && connected[static_cast<std::size_t>(device)];
}
// Gives `device` (a slot, the keyboard, automatic or nothing) to `player`.
// `resolved` is the current ControllerPorts::devices() result and `identity`
// the chosen controller's identity. Another player who has the device, by
// choice or automatically, loses it; a player who had chosen it becomes
// automatic. Returns which players' saved settings changed.
inline std::array<bool, 4> claim_controller(std::size_t player, int device, std::string_view identity,
                                            std::array<ControllerMapping, 4> &mappings,
                                            std::array<ControllerPin, 4> &pins,
                                            const std::array<int, 4> &resolved) {
  std::array<bool, 4> changed{};
  if (player >= 4 || device < -3 || device > 3) return changed;
  if (device >= 0 || device == -3)
    for (std::size_t q = 0; q < 4; ++q) {
      if (q == player || mappings[q].device == -1 || mappings[q].device == -2) continue;
      const bool chosen = controller_pinned(mappings[q], pins[q]);
      if (resolved[q] == device || (!chosen && mappings[q].device == device)) {
        mappings[q].device = -1;
        pins[q] = {};
        changed[q] = true;
      }
    }
  mappings[player].device = device;
  pins[player] = device >= 0 && controller_identity_valid(identity) ? ControllerPin{device, std::string(identity)}
                                                                    : ControllerPin{};
  changed[player] = true;
  return changed;
}
class ControllerPorts {
public:
  std::array<ControllerMapping, 4> mappings{};
  std::array<ControllerPortSample, 4> samples{};
  std::array<ControllerPin, 4> pins{};
  void configure(std::size_t p, const ControllerMapping &m) {
    if (mappings[p].device != m.device)
      assigned_[p] = -1;
    mappings[p] = m;
  }
  // The player chose a particular physical controller (see ControllerPin).
  [[nodiscard]] bool pinned(std::size_t p) const {
    return p < 4 && controller_pinned(mappings[p], pins[p]);
  }
  // Resolves each player to a slot (0-3), the keyboard (-3) or nothing (-2).
  // `pads` holds the identity of the controller in each slot ("" if unknown).
  // Order: players who chose a controller find it wherever it is now (the
  // same unit first, then the same model); then players with a saved slot;
  // then automatic players take the remaining connected controllers. A chosen
  // controller that is absent reserves nothing.
  std::array<int, 4> devices(const std::array<bool, 4> &connected,
                             const std::array<std::string, 4> &pads = {}) {
    std::array<int, 4> result{-2, -2, -2, -2};
    std::array<bool, 4> used{};
    bool keyboard = false;
    for (int quality = 2; quality >= 1; --quality)
      for (std::size_t p = 0; p < 4; ++p) {
        if (!pinned(p) || result[p] != -2) continue;
        const int saved = mappings[p].device;
        for (int i = 0; i < 4; ++i) {
          const int c = i == 0 ? saved : i <= saved ? i - 1 : i; // saved slot first
          const auto slot = static_cast<std::size_t>(c);
          // Another unit of the same model never takes a slot saved by a
          // player without an identity (an older controller-N.ini).
          bool saved_by_other = false;
          for (std::size_t q = 0; quality == 1 && q < 4; ++q)
            saved_by_other |= q != p && !pinned(q) && mappings[q].device == c;
          if (connected[slot] && !used[slot] && !saved_by_other &&
              controller_identity_match(pins[p].id, pads[slot]) >= quality) {
            result[p] = c;
            used[slot] = true;
            break;
          }
        }
      }
    // Without identities (XInput fallback) a chosen controller is its slot.
    for (std::size_t p = 0; p < 4; ++p) {
      const int d = mappings[p].device;
      if (pinned(p) && result[p] == -2 && connected[static_cast<std::size_t>(d)] &&
          !used[static_cast<std::size_t>(d)] && pads[static_cast<std::size_t>(d)].empty()) {
        result[p] = d;
        used[static_cast<std::size_t>(d)] = true;
      }
    }
    for (std::size_t p = 0; p < 4; ++p) {
      int d = mappings[p].device;
      if (pinned(p)) continue;
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
    // An absent chosen controller shows as its saved slot while that slot is
    // empty, so the player reads "offline" rather than "no device".
    for (std::size_t p = 0; p < 4; ++p) {
      const int d = mappings[p].device;
      if (pinned(p) && result[p] == -2 && !connected[static_cast<std::size_t>(d)] &&
          !used[static_cast<std::size_t>(d)]) {
        result[p] = d;
        used[static_cast<std::size_t>(d)] = true;
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
