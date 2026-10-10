# Modern PC input

Goal: modern PC input on top of the faithful port: native PlayStation
controllers, direct mouse/keyboard aiming, gyro aiming, and mouse plus
controller at the same time. This page records what the investigation found,
the plan, and what each PR delivers. The cloud cannot run the game, so every
gameplay behaviour below needs a test on the owner's PC.

## How input reaches the game today

### Host side (runtime, `src/boot/native_boot.cpp`)

- **Window thread.** `LiveRt64Window` owns the game window. It registers raw
  mouse input (`RegisterRawInputDevices`, usage page 1 / usage 2) and pushes
  relative `WM_INPUT` motion, mouse buttons, the wheel and keys into a
  `WindowInputMailbox` (`include/jfg/runtime/window_input_mailbox.hpp`).
- **Guest thread.** The game polls its controllers with
  `osContStartReadData` / `osContGetReadData`, which the runtime handles
  itself. Each poll calls `sample_live_controller` → `sample_controller_ports`:
  - every 250 ms it re-reads `controller.ini` / `controller-N.ini` (mapping and
    device per player), `pc-input.ini` / `pc-input-N.ini` (keyboard keys, mouse
    and aim options) and `experimental-controls.ini` (the profile-wide opt-in);
  - it reads gamepads with **`XInputGetState` only** (`read_xinput_device`).
    Device slot N is XInput user index N, so DualShock 4, DualSense and Switch
    Pro controllers are invisible unless Steam or DS4Windows wraps them as an
    Xbox pad;
  - `ControllerPorts::devices` (`controller_ports.hpp`) resolves each player to
    exactly **one** device: a slot 0-3, the keyboard (-3), or nothing (-2).
    Automatic picks the next free controller and falls back to the keyboard for
    Player 1 only when no controller is connected;
  - `map_controller` turns SDL's standard layout (buttons 0-14, axis directions
    15-26) into N64 buttons and the analog stick; `map_pc_keyboard` does the
    same for keys.
- The result is written into the `OSContPad` the game reads. A separate,
  per-port **look channel** (`ControllerReplaySample::pc_mode`, `look_x/y`)
  carries mouse or second-stick input to the hooks below. Modes: 0 stock,
  1 legacy mouse as a fake stick, 2 legacy second stick as a fake stick,
  3 modern mouse (angle deltas), 4 modern second stick (rates).
- There is **no rumble**. The runtime emulates a Controller Pak on port 0 and
  never calls `XInputSetState`.
- The launcher's Controllers page (`launcher/ui/native_ui.cpp`, RmlUi, opened
  as a separate "controllers" window by `JFG-Launcher.exe`) also discovers and
  learns bindings through XInput, and names buttons "A / Cross" etc.

### Game side (decomp, ravenx444/Rocket-Group-Orion)

| Address | Name | Role |
| --- | --- | --- |
| `0x800F6DA0` | `controlKeys` | held N64 buttons; R (`0x10`) is manual aim |
| `0x800F6DB4` / `0x800F6DB8` | `controlAbsXjoy` / `controlAbsYjoy` | stick as seen by aiming |
| `0x800F6DC0` | `controlcam` | camera; `s16` yaw at +0 |
| `0x80032A48` | `controlPlayer` | per-player control update |
| `0x8003AABC` | `controlGetManualAim` | manual aim (decompiled, `src/charControl.c`) |
| `0x8002CF6C` | `func_8002CF6C` | player camera update |
| `0x800446E4` | `levelGetCamera` | call site where the camera's wanted point is known |
| `0x8002CC70` | `func_8002CC70` | camera scenery collision |
| `0x8004B3D8` | `rumbleMax` | the game's rumble request (also `rumbleStart` / `rumbleAlter` / `rumbleStop` / `rumbleKill`) |
| `0x800FDF58` | `rumbleStructArray` | 4 x 10-byte rumble state, one per N64 channel |

**Manual aim.** `controlGetManualAim(object, player, hRange, vRange, minPitch,
maxPitch, *h, *v, elapsed)` reads the stick from `controlAbsXjoy/Yjoy`.
Inside +/-40 the stick only moves the crosshair *relative to* the heading
(`*h = hRange * x / 40`), so releasing the stick snaps the crosshair back to
the centre: this is the spring-back. Past +/-45 the heading turns with an
eased velocity (`+0x1E4`, `+0x1E8`). The aim state lives in the player
control block: heading `s16` at `+0x11C`, pitch `s16` at `+0x1E2`, clamped to
`minPitch..maxPitch` (the caller passes them on the stack at `+0x10/+0x14`,
`elapsed` at `+0x20`).

**Upstream's experimental controls** (`docs/planning/pc-controls-status.md`)
already hook this properly. With "Modern camera + movement" on, the hook at
`0x8003AABC` runs the original with the stick zeroed (crosshair centred), then
writes heading and pitch directly (`pc_direct_aim`) and clears the edge
velocities. So in mode 3 the mouse turns the aim directly and nothing springs
back. The hook at `0x8002CF6C` feeds an orbit yaw/pitch into the camera's
wanted point before the game's own scenery solver runs. Gaps:

- without "Modern" the mouse is still a fake stick (mode 1), with spring-back;
- the keyboard has no aim keys; WASD is movement;
- aim is hold-only;
- one device per player, and only XInput gamepads;
- no gyro, no rumble.

**Rumble.** Disassembly of `controlSetRumble` shows it picks a preset and
calls `rumbleMax(player, strength, seconds)` with strength 40-75 and
0.2-0.5 s. Overlays call `rumbleMax` too. `rumbleMax/Start/Alter` only check
that rumble is enabled and the game is not paused, not that a Rumble Pak is
present. They store `strength^2 / 10` and `seconds * 60` in
`rumbleStructArray[channel]`. Only `rumbleTick` needs a pak (it pulses the
motor). So hooking the request API gives real rumble without emulating a
Rumble Pak and without touching the Controller Pak.

## Plan

Four PRs, stacked in this order. Each builds on the one before and is tested
by the owner before merging. Merge them in order.

1. **DualShock 4 / DualSense (and other SDL controllers).** Replace XInput
   discovery in the runtime and in the Controllers page with SDL2's
   GameController API: the same SDL 2.26.3 the game already ships for audio
   (RT64's copy), built statically into the launcher. Xbox pads stay on SDL's
   XInput backend and keep their old slot numbers. PlayStation and Nintendo
   pads take the next free slot. The Controllers page shows the controller's
   name and PlayStation (or Xbox, or Nintendo) button names, and can bind the
   touchpad click and the Mic/Share/Capture button. Rumble: hook the game's
   rumble requests and play them on the player's controller (any SDL
   controller with rumble, Xbox included), with a per-player strength
   setting and a test button. Lightbar: player colour on DS4/DualSense
   (cheap). Adaptive triggers: not done; a later PR can use
   `SDL_GameControllerSendEffect`.
2. **Direct aiming for mouse and keyboard.** The mouse always turns the aim
   directly (no fake stick), with or without "Modern camera". Keyboard aim
   keys turn the aim at a set speed, and the aim stays put when they are
   released. Aim can be hold or toggle (right mouse button by default).
   Separate look and aim sensitivity (both exist upstream) and invert Y.
3. **Gyro aiming** for DualSense, DS4 and Switch Pro through SDL sensors,
   added as angle deltas into the same direct-aim path. Settings: on/off,
   sensitivity, active only while aiming or always, invert Y, and a ratchet
   button that pauses gyro while held. Gyro bias is calibrated automatically
   while the controller is still.
4. **Dual input.** Player 1 also accepts the mouse and keyboard while
   playing on a controller (and a free controller while on the keyboard), for
   Steam Input-style setups. On: both at once, with no mode switching (buttons
   combined, the larger stick wins, mouse/gyro/stick look add up). Off: the
   last-used device wins.

The input maths (slot policy, binding names, rumble mapping, direct-aim
deltas, gyro calibration, device merging) lives in header-only code under
`include/jfg/runtime/` with unit tests in `tests/`. The live runtime is not
compiled by CI (it needs ROM-generated code), so each PR also gets a local
MinGW syntax check of `native_boot.cpp` with stub generated headers.

## Direct aiming (PR 2)

Requires the profile-wide **Experimental** switch in Controllers (upstream's
opt-in for the hooks above). For the player on mouse and keyboard:

- **Mouse.** Raw mouse motion becomes angle deltas (about 0.1 degree per
  count at 100%) written straight into the aim heading/pitch while aiming.
  The crosshair stays centred on the aim and nothing springs back. With
  **Modern camera + movement** on, the same deltas also turn the camera when
  not aiming (upstream's look mode 3). With it off, the mouse aims only while
  aiming (new look mode 5; upstream's mode 1, the mouse as a fake stick, is
  kept only for old input recordings).
- **Aim keys** (new bindings, default Numpad 8/2/4/6 with Num Lock on) turn
  the aim at a set speed (default 120 degrees/s) through the same path. When
  they are released the aim stays where it is.
- **Aim button: Hold or Toggle.** R / aim (right mouse button in the Normal
  and Expert presets). Toggle only applies during gameplay; menus, pauses,
  cutscenes, the settings window and focus loss release a toggled aim, so
  the controls never wait on a stuck button.
- Settings (Controllers > keyboard player > Settings): mouse look
  sensitivity (camera) and mouse aim sensitivity are separate, plus vertical
  scale, Mouse Y invert, aim key speed and the aim button mode.
- Settings live in `pc-input.ini` (version 4 keys `aim_toggle`,
  `key_aim_speed`, `key20`-`key23`). Older files load with the defaults.

## Gyro aiming (PR 3)

For DualShock 4, DualSense, Switch Pro and Joy-Con (any SDL controller with a
gyro). Since the PS5 fixes it is off by default, does not need the
Experimental switch, and its settings are on the player's device card (see
below):

- The SDL input thread enables the gyro and accelerometer, integrates the
  gyro at about 500 Hz using the sensor timestamps, and removes the drift
  bias: whenever the controller lies still for a second, its average rate
  becomes the new bias. Tiny remaining rates are tightened towards zero.
- Each poll, the rotation since the last poll becomes direct angle deltas
  (`PcGyroAim`): yaw turns the aim left/right, pitch up/down. 100% turns the
  aim exactly as far as the controller turned. The deltas go through the same
  direct-aim path as the mouse: added to the heading and pitch after the game
  aims (stock aiming), or added to the mouse/stick deltas in the direct modes.
- **Active: while aiming** (default) or **always**. Outside aiming the gyro
  turns the camera only when the modern camera is active for that player
  (Experimental on, Modern camera + Dual-stick aim on); otherwise it does
  nothing there.
- **Ratchet**: any controller binding (default none). While it is held the
  gyro is ignored, so you can re-centre your hands.
- Settings (Controllers > player > device card, shown when the player's
  controller has a gyro): on/off, sensitivity 10-1000%, while aiming/always,
  Gyro Y invert, ratchet.
- Without the Experimental switch the aim hook (`0x8003AABC`) also runs
  while some player has gyro on. With the switch off every player is in look
  mode 0, so the hook only lets the game aim as usual and then adds the gyro
  turn; it touches nothing else.
  `pc-input.ini` keys `gyro`, `gyro_sensitivity`, `gyro_mode`,
  `gyro_ratchet`, `gyro_invert_y`.
- Gyro is not written to input recordings, and is off while recording, so
  recordings replay exactly.

## Dual input (PR 4)

Player 1 gets a second device whenever one is free (`companion_device`):
the mouse and keyboard while Player 1 plays on a controller, or the first
free connected controller while Player 1 plays on the keyboard. Other
players' devices are never shared, so multiplayer assignments are unchanged.

- **Dual input on:** both devices at once, with no mode switching. Buttons
  combine, the larger movement-stick push wins, and the look sources add up:
  the second stick stays the look channel (mode 4) while mouse, aim keys and
  gyro add direct angle deltas. Built for Steam Input setups (gyro-as-mouse,
  trackpads, back buttons mapped to keys).
- **Dual input off (default):** the device used last controls Player 1. A
  button press or a clear stick push switches; gyro, stick drift and a
  toggled aim never do. Switching away from the keyboard releases a toggled
  aim and the mouse capture. Moving the mouse alone does not switch back to
  the keyboard (the mouse is only read while captured); press a key or a
  mouse button.
- Rumble, the lightbar and gyro follow Player 1's controller whichever
  device is primary.
- Setting: Controllers > Player 1 > Dual input (`dual_input` in
  `pc-input.ini`). Player 1's gyro and stick settings are on the gamepad
  page; switch the device to the controller to edit them.

## PS5 fixes: choosing a controller, gyro settings

**Choosing a controller.** "Controller 1-4" are slots numbered in the order
SDL finds pads, and every process (launcher, Controllers window, game) numbers
them on its own. Saved assignments were slot numbers, so a saved choice could
point at another pad, an absent Xbox-style pad's slot kept its player, and a
controller another player had (by a saved slot or automatically) was hidden
from the picker. Getting a DualSense onto Player 1 then took removing every
player and restarting. Now:

- Choosing a controller also saves which physical controller it is (USB
  vendor:product plus the serial number when the pad reports one) in
  `controller-ids.ini` (`playerN=<slot>,<identity>`). `controller-N.ini` is
  unchanged, so older builds still read it; an entry only counts while that
  player's `controller-N.ini` names the same slot.
- Resolution (`ControllerPorts::devices`): players who chose a controller get
  it wherever it is now (same unit first, then the same model if free); then
  players with only a saved slot (older files); then Automatic players. An
  absent chosen controller reserves nothing.
- The picker lists every connected controller, marked with the player who has
  it. Choosing one moves it (`claim_controller`): a player who had chosen it
  becomes Automatic. The same goes for the mouse and keyboard.
- Hot-plug: a chosen controller is found again in whatever slot it returns to.

**Gyro settings** are on the device card for any player whose controller has a
gyro, with no Experimental switch needed (see Gyro aiming above).

## Status

- PR 1 (DualShock 4 / DualSense, rumble): merged (#9).
- PR 2 (direct mouse/keyboard aim): merged (#10).
- PR 3 (gyro): merged (#11).
- PR 4 (dual input): merged (#12).
- PS5 fixes (choosing a controller, gyro settings): in review.
