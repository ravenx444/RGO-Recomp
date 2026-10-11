#pragma once
// RGO: what each N64 button does on foot, for the Controllers page labels
// ("Jump (C-up)"). From the game's control tables (controlModeKeys points at
// one of them; slots: +0 fire, +4 next weapon (controlSelectNextWeapon),
// +8 previous weapon, +0xC jump (after a controlCeiling headroom check),
// +0x10 crouch, +0x14/+0x18 sidestep left/right (strafe while aiming),
// +0x1C/+0x20 zoom in/out with the sniper scope, and walk forward/back while
// aiming). R is aim (controlGetManualAim), Start pauses.
//
// The game's Normal controls are the table at 0x800A18D8 (jump on C-up,
// A/B change weapon), Expert the one at 0x800A18B4 (jump on A, crouch on B).
// The decomp names these two tables the other way round; a new game uses
// 0x800A18D8, and the in-game-tested presets call C-up jump "Normal".
// L and the D-pad have no on-foot action in the control code.
#include <array>
#include <string>
#include <string_view>

namespace jfg {
// N64 button names in ControllerMapping::bindings / PcInputConfig::keys order.
inline constexpr std::array<std::string_view, 14> kN64ButtonNames{
    "A", "B", "Z", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "L", "R", "C-up", "C-down", "C-left", "C-right"};

// The on-foot action of N64 button `index` (0..13) for the game's Normal or
// Expert controls, or an empty view when it has none.
inline std::string_view game_action(int index, bool expert) {
  switch (index) {
  case 0: return expert ? "Jump / zoom in" : "Previous weapon";
  case 1: return expert ? "Crouch / zoom out" : "Next weapon";
  case 2: return "Fire";
  case 3: return "Pause";
  case 9: return "Aim";
  case 10: return expert ? "Next weapon" : "Jump / zoom in";
  case 11: return expert ? "Previous weapon" : "Crouch / zoom out";
  case 12: return "Sidestep left";
  case 13: return "Sidestep right";
  default: return {};
  }
}

// "Jump / zoom in (C-up)", or just "D-pad up" when the button has no action.
inline std::string game_action_label(int index, bool expert) {
  if (index < 0 || index >= static_cast<int>(kN64ButtonNames.size())) return {};
  const auto name = kN64ButtonNames[static_cast<std::size_t>(index)];
  const auto action = game_action(index, expert);
  return action.empty() ? std::string(name) : std::string(action) + " (" + std::string(name) + ")";
}
} // namespace jfg
