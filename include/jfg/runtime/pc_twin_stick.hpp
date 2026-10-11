#pragma once
// RGO modern twin-stick controller scheme: the left stick only moves, the
// right stick turns the camera and, while aiming, the aim. The maths here
// turns the right stick into angle rates the camera and aim hooks apply per
// game tick, so the turn speed is the same at every frame rate.
#include "jfg/runtime/pc_input.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jfg {
// Controller scheme (pc-input.ini "scheme"): 0 original (one stick moves and
// aims), 1 modern twin-stick. Only applies to a player on a controller.
inline constexpr int kPcSchemeOriginal = 0, kPcSchemeTwinStick = 1;
inline bool pc_twin_stick(const PcInputConfig &c) { return c.scheme == kPcSchemeTwinStick; }

// The settings a controller player actually uses. The twin-stick scheme turns
// on the modern camera and the separate look stick without the Experimental
// switch; the original scheme leaves them to that switch, as before.
inline PcInputConfig effective_pad_input(bool experimental, const PcInputConfig &saved) {
  auto active = effective_pc_input(experimental, saved);
  if (pc_twin_stick(saved)) active.modern = active.dual_stick = 1;
  return active;
}

// The same for the mouse and keyboard player: the Modern scheme turns on
// mouse look and aim and the modern camera, with no Experimental switch.
inline PcInputConfig effective_keyboard_input(bool experimental, const PcInputConfig &saved) {
  auto active = effective_pc_input(experimental, saved);
  if (pc_twin_stick(saved)) active.modern = active.mouse_aim = 1;
  return active;
}

// Turn speed at full right-stick deflection and 100% sensitivity, in degrees
// per second. Typical of modern console shooters: a quick camera, and a
// slower, precise aim.
inline constexpr double kPcCameraDegreesPerSecond = 200.0;
inline constexpr double kPcAimDegreesPerSecond = 100.0;
// Game ticks per second (the game's elapsed time counts 1/60 s ticks at 30,
// 60 and 20 frames per second alike).
inline constexpr double kPcTicksPerSecond = 60.0;
// Look mode 6 carries the rate in 1/16 angle units (65536 = 360 degrees) per
// tick, so it fits the recorded int16 look channel up to ~1100 degrees/s.
inline constexpr int kPcTwinStickMode = 6;
inline constexpr double kPcTwinRateScale = 16.0;

// Look mode 7: the mouse (and aim keys) in the Modern scheme. Angle deltas
// like mode 3, with the twin-stick rules (camera on foot only, turning in
// place, a new aim facing the camera, the game's own aim walking).
inline constexpr int kPcTwinMouseMode = 7;
inline bool pc_twin_mode(int mode) { return mode == kPcTwinStickMode || mode == kPcTwinMouseMode; }

struct PcStickLook {
  double x = 0, y = 0; // -1..1, right and up positive
  bool operator==(const PcStickLook &) const = default;
};

// The look stick (whichever stick does not move) after a radial deadzone
// rescaled to full travel, and the response curve (0 linear, 1 precise:
// squared, 2 extra precise: cubed) applied to the deflection, never to the
// direction, so diagonals stay diagonal.
inline PcStickLook pc_look_stick(const PcInputConfig &c, const StandardControllerSample &pad, int movement_stick) {
  const std::size_t axis = movement_stick == 0 ? 2U : 0U;
  const double x = std::clamp(pad.axes[axis], -32767, 32767) / 32767.0;
  const double y = -std::clamp(pad.axes[axis + 1U], -32767, 32767) / 32767.0;
  const double magnitude = std::hypot(x, y);
  const double deadzone = std::clamp(c.aim_deadzone, 0, 30000) / 32767.0;
  if (!(magnitude > deadzone) || magnitude <= 0.0) return {};
  const double travel = std::clamp((magnitude - deadzone) / (1.0 - deadzone), 0.0, 1.0);
  const double response = c.aim_curve == 2 ? travel * travel * travel : c.aim_curve == 1 ? travel * travel : travel;
  return {x / magnitude * response, y / magnitude * response};
}

// Rate for look mode 6 (1/16 angle units per tick): camera or aim
// sensitivity, the vertical scale and the matching invert Y.
inline PcLookDelta pc_twin_rate(const PcInputConfig &c, PcStickLook look, bool aiming) {
  const double degrees = aiming ? kPcAimDegreesPerSecond * std::clamp(c.aim_sensitivity, 10, 300) / 100.0
                                : kPcCameraDegreesPerSecond * std::clamp(c.camera_sensitivity, 10, 300) / 100.0;
  const double units = degrees * 65536.0 / 360.0 / kPcTicksPerSecond * kPcTwinRateScale;
  const bool invert = aiming ? c.aim_invert_y != 0 : c.camera_invert_y != 0;
  const double vertical = std::clamp(c.stick_vertical_sensitivity, 10, 300) / 100.0 * (invert ? -1.0 : 1.0);
  const auto clamp16 = [](double v) { return static_cast<int>(std::clamp(std::lround(v), -32767L, 32767L)); };
  return {clamp16(look.x * units), clamp16(look.y * units * vertical)};
}

// Angle delta for one camera or aim update that advanced `ticks` game ticks.
// Fractions of an angle unit carry over to the next update.
class PcTwinTurn {
  double x_ = 0, y_ = 0;
public:
  PcLookDelta apply(PcLookDelta rate, float ticks) {
    if (!std::isfinite(ticks) || ticks <= 0.0F) return {};
    const double t = std::min(static_cast<double>(ticks), 5.0) / kPcTwinRateScale;
    if (rate.x == 0) x_ = 0;
    if (rate.y == 0) y_ = 0;
    x_ += rate.x * t;
    y_ += rate.y * t;
    const int x = static_cast<int>(std::trunc(x_)), y = static_cast<int>(std::trunc(y_));
    x_ -= x;
    y_ -= y;
    return {x, y};
  }
  void clear() { x_ = y_ = 0; }
};

// Where the right stick turns the camera: on foot on the ground, movement
// state 0 (standing, running; aiming has its own camera) with the medium
// bits of control block +1 at 1 (ground, as the run-speed code checks).
// Other states and media (swimming, jetpack, vehicles, hanging, ...) keep
// the game's own camera.
inline bool pc_twin_camera_state(std::uint8_t state, std::uint8_t medium) {
  return state == 0 && (medium & 3U) == 1U;
}

// The body heading (control block +0x11C) that faces where the camera looks.
// The game runs at heading atan2(stick) - camera yaw, and a stick pushed
// straight up is 0x8000 there (mathAtan2 0x80049C84), so straight ahead of
// the camera is 0x8000 - camera yaw (controlcam +0). Checked headless: a
// forward run settles at this heading.
inline constexpr int kPcStickAtRest = 8; // of 80
inline std::int16_t pc_camera_heading(int camera_yaw) { return pc_wrap_angle(0x8000 - camera_yaw); }
// Standing on foot (movement state 0 with the left stick at rest), turning
// the right stick turns the character in place with the camera.
inline bool pc_twin_turn_in_place(std::uint8_t state, int move_x, int move_y) {
  return state == 0 && move_x > -kPcStickAtRest && move_x < kPcStickAtRest &&
         move_y > -kPcStickAtRest && move_y < kPcStickAtRest;
}
// A new aim (no aim update for this many VIs, a quarter of a second) starts
// facing the camera's direction instead of the body's.
inline constexpr std::uint64_t kPcNewAimGap = 15U;

// Aim states (control block +0x568): 5 is the standing aim stance, 11 aim
// walking. The game walks forward/back in aim while its zoom-in/zoom-out keys
// are held and strafes with C-left/C-right (control tables at 0x800A18B4/D8, masks at
// +0x1C/+0x20 and +0x14/+0x18). In the twin-stick scheme the left stick holds
// those keys for the game, so it enters aim walking with its own animations;
// the aim hook then sets the walking speed from the stick (pc_aim_movement).
inline bool pc_twin_aim_state(std::uint8_t state) { return state == 5 || state == 11; }
struct PcAimWalkKeys {
  std::uint32_t forward = 0, back = 0, left = 0, right = 0;
};
inline constexpr int kPcAimWalkThreshold = 16; // of 80, past the movement deadzone
inline std::uint16_t pc_twin_aim_walk(N64StickSample move, const PcAimWalkKeys &keys) {
  const int x = move.x, y = move.y;
  const bool walk_x = x <= -kPcAimWalkThreshold || x >= kPcAimWalkThreshold;
  const bool walk_y = y <= -kPcAimWalkThreshold || y >= kPcAimWalkThreshold;
  if (!walk_x && !walk_y) return 0;
  // Forward or back starts aim walking; a pure sideways push walks "forward"
  // at zero forward speed (the aim hook sets the speed) so the game strafes.
  std::uint32_t buttons = y <= -kPcAimWalkThreshold ? keys.back : keys.forward;
  if (x <= -kPcAimWalkThreshold) buttons |= keys.left;
  if (x >= kPcAimWalkThreshold) buttons |= keys.right;
  return static_cast<std::uint16_t>(buttons & 0xFFFFU);
}

// Aim walking speed from the move stick, within the game's own aim-walk
// limits (overlay 15: 2.5 sideways for C-left/C-right; forward clamps at
// 2.0 at 30 fps). Headless, a 3.0 forward speed (pc_aim_movement) walked
// 25% faster at True 60 than at 30, so forward and back use 2.0.
inline PcMoveVelocity pc_twin_aim_movement(int x, int y) {
  const auto v = pc_aim_movement(x, y);
  return {v.side, v.forward / 3.0F * 2.0F};
}

// Controller bindings for the twin-stick scheme, by the game's own control
// option (game_actions.hpp): Cross jumps in both (C-up in Normal, A in
// Expert), Square and Circle are the weapon buttons (Normal: A previous,
// B next) or Square next weapon and Circle crouch (Expert), L3 is C-down
// (crouch in Normal, previous weapon in Expert), L2 aims (the game's R), R2
// fires (Z), L1/R1 sidestep (C-left/C-right, which were on the right stick,
// now the look stick), and Back (Create/Share, View, Minus) is the game's L.
// SDL bindings: buttons 0..14, 23 left trigger, 25 right trigger.
inline ControllerMapping pc_twin_stick_mapping(bool expert, int device) {
  ControllerMapping m;
  m.device = device;
  m.stick = 0;
  // N64: A, B, Z, Start, D-up, D-down, D-left, D-right, L, R, C-up, C-down, C-left, C-right
  m.bindings = {expert ? 0 : 2, 1, 25, 6, 11, 12, 13, 14, 4, 23, expert ? 2 : 0, 7, 9, 10};
  return m;
}
} // namespace jfg
