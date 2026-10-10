// RGO: choosing a physical controller for a player (controller_ports.hpp).
// Covers the owner's report: a DualSense could only be given to Player 1
// after removing every player and restarting, when an Xbox-style arcade stick
// had been configured before and was now unplugged.
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
using Devices = std::array<int, 4>;
using Connected = std::array<bool, 4>;
using Ids = std::array<std::string, 4>;
const std::string kDualSense = "054c:0ce6/a0-ab-51-00-00-01";
const std::string kDualSense2 = "054c:0ce6/a0-ab-51-00-00-02";
const std::string kStick = "045e:028e";
jfg::ControllerMapping device(int d) {
  jfg::ControllerMapping m;
  m.device = d;
  return m;
}
// What the Controllers page does when a device is chosen for `player`.
void choose(jfg::ControllerPorts &ports, std::size_t player, int slot, const Connected &connected, const Ids &ids,
            std::array<bool, 4> *changed = nullptr) {
  auto mappings = ports.mappings;
  auto pins = ports.pins;
  const auto now = ports.devices(connected, ids);
  const auto result = jfg::claim_controller(player, slot, slot >= 0 ? ids[static_cast<std::size_t>(slot)] : "",
                                            mappings, pins, now);
  if (changed) *changed = result;
  ports.pins = pins;
  for (std::size_t p = 0; p < 4; ++p) ports.configure(p, mappings[p]);
}
} // namespace

int main() {
  // Identities.
  check(jfg::controller_identity(0x054C, 0x0CE6, "a0:ab:51:00:00:01") == "054c:0ce6/a0:ab:51:00:00:01",
        "identity is vendor:product/serial");
  check(jfg::controller_identity(0x045E, 0x028E, "") == "045e:028e", "no serial, model only");
  check(jfg::controller_identity(0, 0, "") .empty(), "unknown controllers have no identity");
  check(jfg::controller_identity(0x054C, 0x0CE6, "a b\n=c") == "054c:0ce6/abc", "serial is cleaned for the file");
  check(jfg::controller_identity_valid(kDualSense) && jfg::controller_identity_valid(kStick), "valid identities");
  for (const char *bad : {"", "054c:0ce", "054C:0ce6", "054c-0ce6", "054c:0ce6/", "054c:0ce6x1", "054c:0ce6/a b"})
    check(!jfg::controller_identity_valid(bad), "invalid identity accepted");
  check(jfg::controller_identity_match(kDualSense, kDualSense) == 2, "same unit");
  check(jfg::controller_identity_match(kDualSense, kDualSense2) == 1, "same model");
  check(jfg::controller_identity_match(kDualSense, kStick) == 0, "different model");
  check(jfg::controller_identity_match(kDualSense, "") == 0, "unknown pad never matches");

  // controller-ids.ini.
  {
    std::array<jfg::ControllerPin, 4> pins{};
    pins[0] = {2, kDualSense};
    pins[3] = {0, kStick};
    const auto text = jfg::serialize_controller_pins(pins);
    check(text == "version=1\nplayer1=2," + kDualSense + "\nplayer4=0," + kStick + "\n", "pins file format");
    const auto back = jfg::parse_controller_pins(text);
    check(back[0].slot == 2 && back[0].id == kDualSense && back[1].slot == -2 && back[3].id == kStick, "pins roundtrip");
    const auto messy = jfg::parse_controller_pins("player2=1,054c:0ce6\r\nplayer5=0,054c:0ce6\nplayer3=4,054c:0ce6\n"
                                                  "player1=0,BAD\njunk\n");
    check(messy[1].slot == 1 && messy[1].id == "054c:0ce6" && messy[0].slot == -2 && messy[2].slot == -2 &&
              messy[3].slot == -2,
          "bad pin lines are ignored");
    check(jfg::parse_controller_pins("")[0].slot == -2, "no file, no pins");
  }

  // The owner's scenario. The arcade stick (XInput) was set up as Player 1's
  // "Controller 2" and is now unplugged; Players 2-4 are Automatic. Only the
  // DualSense is connected, in slot 0.
  {
    jfg::ControllerPorts ports;
    ports.configure(0, device(1));
    const Connected connected{true, false, false, false};
    const Ids ids{kDualSense, "", "", ""};
    auto d = ports.devices(connected, ids);
    check(d[0] == 1 && d[1] == 0, "before: the stale slot keeps Player 1 and Player 2 gets the DualSense");
    // Choosing the DualSense for Player 1 works straight away.
    check(jfg::controller_device_selectable(0, connected) && !jfg::controller_device_selectable(1, connected),
          "every connected controller can be chosen, absent ones cannot");
    std::array<bool, 4> changed{};
    choose(ports, 0, 0, connected, ids, &changed);
    check(changed[0] && !changed[1], "only Player 1's settings change (Player 2 is Automatic)");
    d = ports.devices(connected, ids);
    check(d[0] == 0 && d[1] == -2, "after: Player 1 has the DualSense, Player 2 has nothing");
    check(ports.pinned(0) && ports.pins[0].id == kDualSense, "the chosen controller is remembered");
    // Next start the DualSense is found in another slot (another process, or
    // another order); Player 1 still gets it, and Automatic players do not.
    d = ports.devices({false, false, true, false}, {"", "", kDualSense, ""});
    check(d[0] == 2 && d[1] == -2, "the DualSense follows Player 1 to any slot");
    // The arcade stick comes back in slot 0: it goes to an Automatic player.
    d = ports.devices({true, false, true, false}, {kStick, "", kDualSense, ""});
    check(d[0] == 2 && d[1] == 0, "the old controller does not take Player 1 back");
  }

  // Another player explicitly had the slot the DualSense is in (an older
  // controller-2.ini), and Player 1 was on the keyboard.
  {
    jfg::ControllerPorts ports;
    ports.configure(0, device(-3));
    ports.configure(1, device(0));
    const Connected connected{true, false, false, false};
    const Ids ids{kDualSense, "", "", ""};
    auto d = ports.devices(connected, ids);
    check(d[0] == -3 && d[1] == 0, "before: Player 2 has the DualSense");
    std::array<bool, 4> changed{};
    choose(ports, 0, 0, connected, ids, &changed);
    check(changed[0] && changed[1] && ports.mappings[1].device == -1, "Player 2 gives it up and becomes Automatic");
    d = ports.devices(connected, ids);
    check(d[0] == 0 && d[1] == -2, "after: Player 1 has the DualSense");
    // The keyboard can be moved the same way.
    choose(ports, 1, -3, connected, ids, &changed);
    d = ports.devices(connected, ids);
    check(d[1] == -3 && d[0] == 0, "the keyboard moves to Player 2");
  }

  // Hot-plug: Player 1 chose the DualSense; it is unplugged and plugged back.
  {
    jfg::ControllerPorts ports;
    const Ids ids{kDualSense, "", "", ""};
    choose(ports, 0, 0, {true, false, false, false}, ids);
    auto d = ports.devices({false, false, false, false}, {"", "", "", ""});
    check(d[0] == 0 && d[1] == -2, "unplugged: Player 1 shows its controller offline");
    d = ports.devices({true, false, false, false}, {kStick, "", "", ""});
    check(d[0] == -2 && d[1] == 0, "another controller in that slot is not taken by Player 1");
    d = ports.devices({true, true, false, false}, {kStick, kDualSense, "", ""});
    check(d[0] == 1 && d[1] == 0, "plugged back in another slot: Player 1 has it again");
    d = ports.devices({false, true, false, false}, {"", kDualSense, "", ""});
    check(d[0] == 1, "still there when the other controller leaves");
  }

  // Two DualSenses: each player gets their own unit; a missing unit falls
  // back to the same model only when nobody else wants it.
  {
    jfg::ControllerPorts ports;
    const Connected both{true, true, false, false};
    const Ids ids{kDualSense, kDualSense2, "", ""};
    choose(ports, 0, 0, both, ids);
    choose(ports, 1, 1, both, ids);
    auto d = ports.devices(both, {kDualSense2, kDualSense, "", ""});
    check(d[0] == 1 && d[1] == 0, "units are told apart by serial");
    d = ports.devices({false, true, false, false}, {"", kDualSense2, "", ""});
    check(d[1] == 1 && d[0] == 0, "Player 2's unit stays with Player 2");
    d = ports.devices({true, false, false, false}, {kDualSense2, "", "", ""});
    check(d[1] == 0 && d[0] == -2, "a missing unit is replaced by the same model when free");
    // A same-model fallback never takes a slot an older file saved.
    jfg::ControllerPorts legacy;
    choose(legacy, 0, 0, {true, false, false, false}, {kDualSense, "", "", ""});
    legacy.configure(1, device(1));
    d = legacy.devices({false, true, false, false}, {"", kDualSense2, "", ""});
    check(d[1] == 1 && d[0] == 0, "the saved slot wins over a same-model fallback");
  }

  // A pin only counts while controller-N.ini still names its slot (another
  // tool changed the device), and without identities (XInput fallback) the
  // slot is used.
  {
    jfg::ControllerPorts ports;
    choose(ports, 0, 0, {true, false, false, false}, {kDualSense, "", "", ""});
    ports.configure(0, device(1));
    check(!ports.pinned(0), "a changed device drops the pin");
    auto d = ports.devices({true, true, false, false}, {kDualSense, kStick, "", ""});
    check(d[0] == 1, "the saved slot is used");
    ports.configure(0, device(0));
    d = ports.devices({true, false, false, false}, {"", "", "", ""});
    check(d[0] == 0, "no identities: the chosen slot");
    // Removing a device or choosing Automatic forgets the controller.
    choose(ports, 0, -2, {true, false, false, false}, {kDualSense, "", "", ""});
    check(!ports.pinned(0) && ports.pins[0].id.empty(), "removing forgets the controller");
    d = ports.devices({true, false, false, false}, {kDualSense, "", "", ""});
    check(d[0] == -2 && d[1] == 0, "a removed Player 1 has no device");
  }

  // Without any identities everything behaves as before.
  {
    jfg::ControllerPorts ports;
    auto d = ports.devices({true, true, false, false});
    check(d[0] == 0 && d[1] == 1, "automatic players unchanged");
  }
  std::cout << "controller assignment: " << checks << " checks passed\n";
  return 0;
}
