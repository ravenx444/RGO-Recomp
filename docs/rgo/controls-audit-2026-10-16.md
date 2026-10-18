# Controls direction audit, 2026-10-16

## Contract

Normal: pushing the look stick forward, pressing Look Up, moving the mouse
up, or tilting gyro nose up looks up. Inverted reverses only vertical look.
Camera and aim may have independent preferences, but the meaning of each
preference is identical. Mouse preferences are independent of the controller
when both devices are active. Movement and menu directions are not look.

## Findings and changes

- The normal camera guard treated control byte +1 as a ground/medium flag.
  The guest camera dispatcher and character accessor identify it as the
  character byte. Its low two bits select the three characters, with 3
  reserved for the hovership. The guard now accepts all three on-foot
  characters, preserving special movement and scripted camera restrictions.
- Modern mouse-only camera mode applied a second pitch conversion after
  mouse aim inversion, while mouse deltas merged into controller input did
  not. Both paths now consume the same positive-up deltas. Camera inversion
  is applied at mouse sampling, before any device merge, independently of aim.
- Look keys now honor the applicable mouse/keyboard camera or aim inversion.
- Experimental stick camera sampling now selects camera inversion separately
  from aim inversion, matching the modern twin-stick path.
- Boss overlays consume C-left/C-right to sidestep instead of the left stick.
  Modern movement now supplies those native actions from horizontal movement
  for boss states 32 through 41. Frozen state 42 is excluded. The original
  boss movement, animations, damage, jumping and collision code still runs.
- Dual input shows separate Mouse Camera and Mouse Aim inversion buttons
  immediately below its enable toggle. Both persist the existing profile
  settings; no new preference format or automatic preference reset is needed.

## Interface coverage

| Interface | Source of normal upward look | Inversion setting | Verification |
| --- | --- | --- | --- |
| Original one-stick controller | SDL negative Y becomes positive guest stick Y | Original mapping Invert Y | Normalized adapter and guest aim source audited; direction tests pass |
| Experimental separate stick aim | Unused SDL stick, positive-up normalized | Aim Y | Both directions and invert states tested |
| Experimental separate stick camera | Same normalized stick | Camera Y | Setting selection corrected; direction tests pass |
| Modern controller camera | Right/unused stick, angle rate | Camera Y | Rate, geometry and both directions tested |
| Modern controller aim | Same stick, aim angle rate | Aim Y | Rate, pitch result and both directions tested |
| Original keyboard directional input | Keyboard bindings produce guest stick values | Native original input mapping | Source inspected; no movement inversion introduced |
| Legacy mouse stick emulation | Upward mouse motion becomes positive guest Y | Mouse/Aim Y | Both directions tested |
| Modern/experimental mouse aim | Upward mouse displacement becomes positive look delta | Mouse Aim Y | Independent camera/aim combinations tested |
| Modern/experimental mouse camera | Same displacement convention | Mouse Camera Y | Removed mode-specific second inversion; independent combinations tested |
| Keyboard Look Up/Down | Positive-up angle deltas | Applicable mouse/keyboard Camera or Aim Y | Both directions and modes tested |
| Gyro | Adapter documents positive pitch as nose up | Gyro Y | Adapter and downstream pitch convention audited; both directions tested |
| Dual input | Independently converted mouse, stick and gyro deltas merge | Each device's own preference | Same-direction reinforcement and independent mouse/controller preferences tested |
| Xbox, PlayStation, Nintendo and generic pads | Shared SDL GameController standard axes | Same settings above | Shared adapter source audited; every physical device was not tested |
| Menus | Positive guest Y navigates up; D-pad maps at menu boundary | No look inversion | Existing menu mapping retained |

Original mapping inversion applies to the mapped stick itself, including
movement. It is not a second modern right-stick or mouse look inversion.
Original mode retains the game's camera behavior; selecting Original does
not enable the modern free-look orbit.

## Validation and limits

Native Windows game and launcher builds passed. Nine related input, replay,
controller and audio DMA regression tests passed. The expanded tests check
final guest pitch and camera geometry, not only intermediate signs, including
mixed input and independently inverted camera and aim settings.

A hidden 3000-VI replay using a copy of the latest save exited normally with
no PI request rejection. Its normal-camera window was brief, so it does not
establish sustained Vela free-look, Lupus behavior, or a complete boss fight.
A second 3600-VI replay on the older grounded save exited normally with
911 orbit events and both pitch directions exercised. This verifies the
common orbit application without proving every physical input adapter.

Physical device, live inversion, boss movement and Vela/Lupus acceptance
remain required. No desktop interaction or owner save modification was used.

## Loading crashes

Two reports from the previous cutscene candidate were resolved against its
matching symbols. The failing guest operations were 0x8003CAB8 in
func_8003C92C_3D52C (a model-related table access) and 0x80055D08 in
texDPTextureX (a texture descriptor access). These are separate from the
previous RT64 MSAA color-target crash. The loading failures remain unresolved.
This candidate adds a bounded fault diagnostic with the last guest PC,
instruction, access type and RDRAM-relative fault offset. No invalid-pointer
skip or speculative dependency update was applied.

The accepted audio repair remains in place. Weapon wheel and launcher-menu
D-pad work remain separate pending tasks.

## Later work on 2026-10-16

The loading investigation progressed to the repair and evidence in
[transition-crash-2026-10-16.md](transition-crash-2026-10-16.md). The owner then
completed Vela's full first level without a crash. The separate weapon wheel,
Floyd visibility and high-refresh candidate is described in
[weapon-wheel-2026-10-16.md](weapon-wheel-2026-10-16.md). Earlier pending notes
above describe the state before those follow-up investigations.
