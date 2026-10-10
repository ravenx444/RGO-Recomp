// RGO: dual input (mouse and keyboard plus a controller for Player 1).
#include "jfg/runtime/controller_ports.hpp"
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
} // namespace

int main() {
  using Devices = std::array<int, 4>;
  using Connected = std::array<bool, 4>;
  check(jfg::companion_device(Devices{0, -2, -2, -2}, Connected{true, false, false, false}) == -3,
        "a controller player also gets the free keyboard");
  check(jfg::companion_device(Devices{0, -3, -2, -2}, Connected{true, false, false, false}) == -2,
        "the keyboard of another player is not shared");
  check(jfg::companion_device(Devices{-3, -2, -2, -2}, Connected{false, true, false, false}) == 1,
        "a keyboard player also gets a free connected controller");
  check(jfg::companion_device(Devices{-3, 1, -2, -2}, Connected{false, true, true, false}) == 2,
        "controllers of other players are skipped");
  check(jfg::companion_device(Devices{-3, 1, -2, -2}, Connected{false, true, false, false}) == -2,
        "no free controller, no companion");
  check(jfg::companion_device(Devices{-2, 0, -2, -2}, Connected{true, false, false, false}) == -2,
        "a removed Player 1 gets nothing");
  // ControllerPorts gives Player 1 automatic the controller once connected.
  jfg::ControllerPorts ports;
  auto devices = ports.devices({false, false, false, false});
  check(devices[0] == -3 && jfg::companion_device(devices, {false, false, false, false}) == -2,
        "keyboard only");
  devices = ports.devices({true, false, false, false});
  check(devices[0] == 0 && jfg::companion_device(devices, {true, false, false, false}) == -3,
        "plugging a controller in keeps the keyboard as companion");

  check(!jfg::port_activity({}), "neutral is no activity");
  check(jfg::port_activity({0x8000, 0, 0, true}), "a button is activity");
  check(jfg::port_activity({0, 40, 0, true}), "a stick push is activity");
  check(!jfg::port_activity({0, 10, -10, true}), "stick drift is not activity");

  const jfg::ControllerPortSample pad{0x2000, 10, -70, true}, keys{0x0010, 50, 0, true};
  const auto merged = jfg::merge_ports(pad, keys);
  check(merged.buttons == 0x2010 && merged.connected, "buttons from both devices");
  check(merged.x == 10 && merged.y == -70, "the larger stick push wins");
  check(jfg::merge_ports({}, keys).x == 50, "an idle device does not cancel the other");

  jfg::LastUsedDevice last;
  check(!last.update(false, false), "the primary device starts active");
  check(last.update(false, true), "using the companion switches to it");
  check(last.update(false, false), "idle keeps the last-used device");
  check(last.update(true, true), "using both keeps the current one");
  check(!last.update(true, false), "using the primary switches back");
  last.update(false, true);
  last.reset();
  check(!last.companion(), "reset returns to the primary");
  std::cout << checks << " dual input checks passed\n";
  return 0;
}
