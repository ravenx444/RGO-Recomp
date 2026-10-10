// RGO: gamepad families, binding names, slot policy and the rumble model.
#include "jfg/runtime/gamepad.hpp"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void check(bool result, const char *what) {
  ++checks;
  if (!result) {
    std::cerr << what << '\n';
    std::exit(1);
  }
}
bool near(float a, float b) { return a > b - 0.001F && a < b + 0.001F; }
} // namespace

int main() {
  using jfg::GamepadFamily;
  // SDL_GameControllerType values.
  check(jfg::gamepad_family(4) == GamepadFamily::playstation, "PS4 is PlayStation");
  check(jfg::gamepad_family(7) == GamepadFamily::playstation, "PS5 is PlayStation");
  check(jfg::gamepad_family(1) == GamepadFamily::xbox && jfg::gamepad_family(2) == GamepadFamily::xbox, "Xbox types");
  check(jfg::gamepad_family(5) == GamepadFamily::nintendo, "Switch Pro is Nintendo");
  check(jfg::gamepad_family(0) == GamepadFamily::generic && jfg::gamepad_family(99) == GamepadFamily::generic, "unknown is generic");

  check(jfg::gamepad_binding_name(0, GamepadFamily::playstation) == "Cross", "PS bottom face button");
  check(jfg::gamepad_binding_name(3, GamepadFamily::playstation) == "Triangle", "PS top face button");
  check(jfg::gamepad_binding_name(9, GamepadFamily::playstation) == "L1", "PS left shoulder");
  check(jfg::gamepad_binding_name(25, GamepadFamily::playstation) == "R2", "PS right trigger");
  check(jfg::gamepad_binding_name(jfg::kBindingTouchpad, GamepadFamily::playstation) == "Touchpad click", "PS touchpad");
  check(jfg::gamepad_binding_name(jfg::kBindingMisc, GamepadFamily::playstation) == "Mic", "PS mic button");
  check(jfg::gamepad_binding_name(6, GamepadFamily::playstation) == "Options", "PS options");
  check(jfg::gamepad_binding_name(0, GamepadFamily::xbox) == "A" && jfg::gamepad_binding_name(23, GamepadFamily::xbox) == "LT", "Xbox names");
  check(jfg::gamepad_binding_name(0, GamepadFamily::nintendo) == "B", "Nintendo positional bottom button is B");
  check(jfg::gamepad_binding_name(1, GamepadFamily::nintendo) == "A", "Nintendo positional right button is A");
  check(jfg::gamepad_binding_name(0, GamepadFamily::generic) == "A / Cross", "generic names keep both labels");
  check(jfg::gamepad_binding_name(-1, GamepadFamily::playstation) == "Unbound", "unbound");
  check(jfg::gamepad_binding_name(jfg::kBindingCount, GamepadFamily::xbox) == "Unbound", "out of range");
  for (int b = 0; b < jfg::kBindingCount; ++b)
    for (auto family : {GamepadFamily::generic, GamepadFamily::xbox, GamepadFamily::playstation, GamepadFamily::nintendo})
      check(!jfg::gamepad_binding_name(b, family).empty(), "every binding has a name");

  jfg::GamepadSlots slots;
  check(slots.attach(100, 2) == 2, "XInput pad keeps its user index");
  check(slots.attach(200, -1) == 0, "other pads take the lowest free slot");
  check(slots.attach(300, 0) == 1, "occupied preference falls back to a free slot");
  check(slots.attach(200, -1) == 0, "attach is idempotent");
  check(slots.attach(400, -1) == 3 && slots.attach(500, -1) == -1, "only four slots");
  slots.detach(200);
  check(slots.find(200) == -1 && slots.id(0) == -1 && slots.id(2) == 100, "detach frees only that slot");
  check(slots.attach(600, -1) == 0, "freed slot is reused");
  check(slots.find(-1) == -1, "invalid id has no slot");

  check(jfg::player_light(0).b == 255 && jfg::player_light(1).r == 255, "player colours");

  using jfg::GameRumbleCall;
  jfg::RumbleModel rumble;
  check(rumble.output(0, 0.0, 100) == jfg::HostRumble{}, "idle model is silent");
  rumble.apply({GameRumbleCall::max, 0, 40, 0.2F}, 10.0);
  auto out = rumble.output(0, 10.0, 100);
  check(near(out.amplitude, 40.0F / 70.0F) && out.milliseconds == 200U, "rumbleMax preset");
  check(rumble.output(1, 10.0, 100) == jfg::HostRumble{}, "other channels stay still");
  check(near(rumble.output(0, 10.0, 50).amplitude, 20.0F / 70.0F), "strength setting scales");
  check(rumble.output(0, 10.0, 0) == jfg::HostRumble{}, "zero strength disables");
  check(rumble.output(0, 10.0, 200).amplitude <= 1.0F, "boost is clamped");
  check(rumble.output(0, 10.21, 100) == jfg::HostRumble{}, "rumble expires");
  // The game re-requests the same preset while the old one has expired: a
  // repeat must rumble again even though the game's own state did not change.
  rumble.apply({GameRumbleCall::max, 0, 40, 0.2F}, 11.0);
  check(rumble.output(0, 11.0, 100).milliseconds == 200U, "repeat request rumbles again");
  rumble.apply({GameRumbleCall::max, 0, 75, 0.5F}, 11.1);
  out = rumble.output(0, 11.1, 100);
  check(near(out.amplitude, 1.0F) && out.milliseconds == 500U, "max raises strength and time");
  rumble.apply({GameRumbleCall::max, 0, 40, 0.1F}, 11.2);
  out = rumble.output(0, 11.2, 100);
  check(near(out.amplitude, 1.0F) && out.milliseconds == 400U, "max never lowers an active rumble");
  rumble.apply({GameRumbleCall::alter, 0, 20, 0.0F}, 11.3);
  out = rumble.output(0, 11.3, 100);
  check(near(out.amplitude, 20.0F / 70.0F) && out.milliseconds == 300U, "alter strength keeps time");
  rumble.apply({GameRumbleCall::stop, 0, 0, 0.0F}, 11.4);
  check(rumble.output(0, 11.4, 100) == jfg::HostRumble{}, "stop");
  rumble.apply({GameRumbleCall::start, 2, 5, 1.0F}, 12.0);
  check(rumble.output(2, 12.0, 100) == jfg::HostRumble{}, "below the motor threshold is silent");
  rumble.apply({GameRumbleCall::start, 2, 70, 1.0F}, 12.0);
  rumble.apply({GameRumbleCall::start, 3, 70, 1.0F}, 12.0);
  rumble.apply({GameRumbleCall::kill, 0, 0, 0.0F}, 12.1);
  check(rumble.output(2, 12.1, 100) == jfg::HostRumble{} && rumble.output(3, 12.1, 100) == jfg::HostRumble{}, "kill stops every channel");
  for (int i = 0; i < 40; ++i) rumble.apply({GameRumbleCall::max, 1, 70, 0.5F}, 20.0 + i * 0.1);
  check(rumble.output(1, 23.95, 100).amplitude > 0.0F && rumble.output(1, 24.0, 100) == jfg::HostRumble{}, "continuous rumble rests after 4 s");
  rumble.apply({GameRumbleCall::max, 1, 70, 0.5F}, 24.1);
  check(rumble.output(1, 24.1, 100).milliseconds == 500U, "rumble resumes after the rest");
  rumble.apply({GameRumbleCall::start, 9, 70, 1.0F}, 30.0);
  rumble.apply({GameRumbleCall::start, 0, 70, -1.0F}, 30.0);
  check(rumble.output(0, 30.0, 100) == jfg::HostRumble{}, "negative time is ignored");
  rumble.apply({GameRumbleCall::start, 0, 70, 1.0F}, 30.0);
  rumble.stop_all();
  check(rumble.output(0, 30.0, 100) == jfg::HostRumble{}, "stop_all on pause or focus loss");
  std::cout << checks << " gamepad checks passed\n";
  return 0;
}
