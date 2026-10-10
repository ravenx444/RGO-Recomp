# PC controls status and follow-up

Updated 2026-10-09. The current priority is dual-stick controller support.

## Mouse and keyboard

Mouse and keyboard is suitable for an opt-in **single-player experimental
trial**. This is not full campaign or gameplay-feel acceptance.

Implemented behavior:

- Editable keyboard keys, five mouse buttons, and wheel directions; Normal and
  Expert presets matching the corresponding in-game control scheme.
- Raw mouse look and direct aiming, WASD movement, and forward/backward/sideways
  movement while in supported ground aim states.
- Independent camera and aim sensitivity, vertical scaling, and inversion.
- Mouse capture releases for focus loss, settings, pause, and scripted cameras;
  returning from settings waits for neutral controls.
- Keyboard and mouse-button rebinding is independent of the experimental
  switch. Turning experiments off retains saved bindings and disables the
  modern look/movement extensions. This corrects the previous gating bug.

The rebinding and tuning changes are implemented and have passed input and
RmlUi checks. The launcher and runtime builds completed; the final native replay
sweep and installation were not completed before work stopped. An already-running
older build does not acquire these code changes.

## Deferred KBM work

1. **Camera obstruction.** Steep angles near trees can hide part of the player.
   The game scenery solver and existing wall clearance remain in use. A lower
   body visibility probe was tried but did not resolve the captured obstruction,
   so that additional probe was removed rather than shipped as a completed fix.
2. **Pointer-driven game menus.** Guest menus accept keyboard navigation,
   confirmation, and back shortcuts with experimental controls enabled. Pointer
   position does not select guest menu items; full hover/click interaction is
   unfinished. The launcher/settings UI uses RmlUi and supports mouse input.
3. **Broader gameplay coverage.** Continue testing all weapons, character
   actions, swimming, jumping, transitions, rooms, and extended play sessions.
   Native replay cases are narrow checks, not campaign-wide acceptance. Special
   movement states currently keep the original game's movement logic.
4. **Multiplayer.** Modern camera and movement hooks currently require
   single-player gameplay. Split-screen support needs separate implementation
   and qualification; ordinary mapped controller inputs remain available.
5. **Additional controller backends.** RGO: controller discovery now uses
   SDL2 (DualShock 4, DualSense, Switch Pro, Xbox); see
   [docs/rgo/input.md](../rgo/input.md).

These items are documented for later work. Do not resume the camera or KBM
feature investigation merely because this backlog exists.

## Current controller priority

- Reliable dual-stick camera/aim and simultaneous movement/strafe behavior.
- Normal/Expert presets, independent camera and aiming sensitivity, vertical
  scaling, deadzone, inversion, and selectable stick response.
- Preserve ordinary right-stick button mappings in menus, scripted scenes, and
  modes where the modern look path is inactive.
- Keep experimental controls visible for controllers, with only the relevant
  binding content shown for the selected device in the RmlUi mapping window.
- Rebuild and qualify the launcher/runtime pair before installing it for the
  normal launcher. Retain the supported-US and single-player qualification limits.

See [launcher controls](../development/launcher.md#keyboard-mouse-and-separate-stick-aim) and the
[reference/qualification notes](../upstream/perfect-dark-pc-reference.md).
Further execution must use the existing guarded investigation and its remaining
budget under [AGENTS.md](../../AGENTS.md); this backlog grants no new allowance.
