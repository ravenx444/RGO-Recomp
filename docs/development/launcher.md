# Windows ROM-to-play launcher

## Unified frontend (v1.0.0)

The RmlUi/C++ launcher owns one window for setup, home, and gameplay. Its fonts and UI resources are embedded in the EXE. Stop returns
to home and flushes the game session; closing a running session waits for it to
stop. The setup/settings helper is embedded in the launcher EXE and extracted
locally. Existing profile saves are reused.

Use the top menu during play. F11 toggles fullscreen; Esc opens or closes the Game menu. Video settings > Aspect ratio chooses Widescreen (the default:
the 3D view widens to the window's shape) or Original 4:3 (pillarboxed); it
applies while playing and is stored in the profile's `video.ini`. See
[RGO widescreen](../rgo/widescreen.md). It is independent of the original
in-game widescreen setting, which is best left off with Widescreen. Video settings also chooses the window mode (windowed, borderless or exclusive fullscreen), output and render resolution, Max aspect ratio, MSAA, VSync and an FPS counter; see [RGO display settings](../rgo/display-settings.md). Audio provides a live volume slider and mute. Settings modals pause the game at a frame boundary and resume it when closed. Menu dropdowns keep gameplay running. Video settings also offers Pause when inactive. In fullscreen, the menu bar is hidden until Esc or F10 opens the controls.

Controllers provides Player 1 through Player 4 tabs. Choose a distinct physical
XInput device for each player. Changes save automatically. Click a binding,
release all controls, and press its replacement; Escape cancels learning.
Automatic assigns each connected controller to a different player; empty ports
remain disconnected. Choose Disconnected to disable a port. One port may use
Keyboard. Automatic
assignment reserves its selected XInput slot across disconnects for the current
session, so another player does not take it over. XInput may renumber hardware
across a complete reconnect/restart; verify assignments in the input display.
Mappings apply while playing; release held inputs when returning from settings.
Native HID/DirectInput devices still require an XInput-compatible adapter.

Tools opens Live Map, Live Inventory, saved profiles, and support reports.
Both live-tool windows use the same RmlUi/Direct3D renderer, embedded fonts,
and per-monitor DPI handling as the launcher. Live Map has a collapsible
inspector, cursor-centered wheel zoom, and bottom-right zoom/Fit controls.
View also retains Fit room and offers All object labels (off by default)
for descriptions on every mapped room object. Map settings groups Display and AI in left-hand tabs; only opening that modal
pauses gameplay. The Tools menu opens the inventory screen or current export folder.

Map and inventory follow the current game process. They show a waiting state
before gameplay and clear current data when the game stops or fresh telemetry
is unavailable. A running game paused by the launcher is labelled PAUSED.
Inventory artwork, including named character plaques, is extracted locally
from the selected verified ROM. No game artwork is included in the launcher.
Autopilot requires a navigation-enabled session; ordinary play exports only
read-only telemetry.

The new frontend requires a game build with the frontend protocol. An older
build receives a rebuild message. Setup remains pinned to the launcher source
revision; local uncommitted development binaries are for testing, not a public
release. See [the implementation plan](../planning/unified-frontend.md) and
[RmlUi design handoff](../planning/rmlui-frontend.md). The home screen asks only for the required ROM; Setup
manages the game build and shows its installation path.



Start with [getting started](https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/docs/getting-started.md)
for downloads, requirements, and known setup issues.

Download and extract the complete Windows x64 release ZIP, then open
`JFG-Launcher.exe` and select
your supported North American ROM. The launcher sets up the tools, builds the
game locally, and remembers the resulting executable for later play.

## Release 1.0.0

This stable release includes the unified launcher and in-game menus, live map
and inventory, configurable controller ports, and opt-in experimental mouse
look and dual-stick controls. Native fixes cover opening asteroid textures,
lens-flare occlusion, cutscene audio continuity, particle textures and saving.

Rebuild an existing game with **Set up and build** to receive native fixes from
this launcher's pinned revision; keep your existing save profile. See the
[v1.0.0 release notes](../releases/1.0.0.md) for verification and remaining limits.

Map and inventory tracking are available during ordinary play. The optional
Navigation mod adds supported walking, jump chains, chests, NPC rewards and
exit actions. Automation remains experimental; see
[its validation and limits](navigation-mod.md).

## First setup

1. Open the launcher and choose your US big-endian `.z64` ROM.
2. Click **Set up and build**. Setup explains the downloads before starting.
   Allow the official Windows installers when prompted.
3. If setup requests a Windows restart, restart, reopen the launcher and click
   **Set up and build** again. Your selections and saves are preserved.
4. When compilation finishes, click **Play**. Future launches reuse the
   game build and do not require setup or recompilation.

The first setup needs internet access and can download several GB. Allow ample
disk space for Visual Studio, Ubuntu, source dependencies, and generated build
files. Windows 10/11 x64 must support WSL2 with virtualization enabled. Microsoft
App Installer (`winget`) must be installed; setup gives instructions if it is
missing. Administrators can also install the tools manually using
[ROM build setup](https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/docs/development/rom-bootstrap.md).

Setup detects and reuses existing tools. Missing components are Git for Windows,
Python 3.12, Visual Studio 2022 C++ Build Tools with CMake, WSL/Ubuntu 24.04, and
the Ubuntu compiler/build packages. It downloads this launcher's exact source
commit and the revisions in `dependencies.lock.json`. A modified source cache
is rejected while its files are preserved. Package installers retain their own
licenses. See Microsoft's [WinGet installation reference](https://learn.microsoft.com/en-us/windows/package-manager/winget/install)
and [WSL installation guide](https://learn.microsoft.com/en-us/windows/wsl/install).

Source stays in `%LOCALAPPDATA%\JFGRecomp\source`; setup diagnostics stay in
`%LOCALAPPDATA%\JFGRecomp\setup.log`. Preview 0.4.0-preview.2 stores generated
inputs, dependencies, game builds and build logs under the short per-checkout
cache `%LOCALAPPDATA%\JFG\b\<checkout-id>`. Earlier previews retain their
original `tools/private/local-builds` directories; no existing files are moved.
The selected ROM stays at its original path and is required when playing.
If you move it, select its new location in the launcher. An existing playable
build can be selected directly as `jfg-native-boot.exe` with its runtime DLLs.

The launcher checks the ROM size, byte order and full SHA-1 locally. It supports
the 32 MiB US revision with SHA-1 `493ced9008dbe932d6e91179b68e8630cf23a023`.
It does not download a ROM or upload diagnostics. The setup-guide button opens
the public instructions in your browser.

## Saves and controls

Saves and remembered file selections live under
`%LOCALAPPDATA%\JFGRecomp\profiles\default`. **Open saves** opens that folder.
Existing saves are preserved. Closing the frontend stops the game and waits for saves to flush.
Only one launcher instance runs in a Windows session.

### Quick-launch shortcuts

The Game menu's saved shortcut starts `JFG-Launcher.exe --play --profile "<folder>"`.
It starts the game after profile initialization and keeps the same in-game menu,
settings, live tools, and save handling as clicking Play. Starting the launcher
without `--play` still opens Home. Recreate older shortcuts that target the setup
helper directly. Close an existing launcher before opening a different profile.

For a local cutscene test, use a separate profile folder. Explicit diagnostic
arguments `--input-replay "<file>"` and `--progress-output "<file>"` require
`--play`; they forward the recording and progress destination to the hosted
game. Ordinary play continues to discard inherited diagnostic environment
variables. A blank test profile leaves the normal campaign save untouched.

| Keys | Action |
| --- | --- |
| W, A, S, D | Analog movement |
| Shift | Full stick magnitude |
| Space or Z | A / jump |
| X / C | B / Z |
| Enter | Start |
| Q / E | L / R |
| I, J, K, L | C buttons |
| Arrow keys | D-pad |
| Escape | Open frontend settings (standalone diagnostic game: exit) |
| F11 | Toggle frontend fullscreen |

**Controllers** detects Xbox/XInput devices, lets you assign each player,
remap all N64 buttons (including Start/pause), select the movement stick, invert
its axes, and adjust dead zones and trigger/stick thresholds. Click **Learn**,
release the controls, then press a button or move a stick. Apply the mapping to
update the running game. **Restore defaults** restores the standard layout.
Mappings are stored as controller.ini, controller-2.ini, controller-3.ini,
and controller-4.ini alongside your default save profile. Player 1 starts with
keyboard fallback when no controller has been assigned automatically. A later
disconnection keeps the assignment reserved; select Keyboard explicitly to use
it while that controller is unplugged. Other controller types need
an XInput-compatible driver or adapter. Native DirectInput/HID mapping and
rumble configuration are not implemented in this preview. Hardware coverage
still needs tester feedback; automated tests use synthetic controller samples.

Existing game builds must be rebuilt with this launcher version to consume the
mapping and write native support diagnostics. An older selected executable can
still produce an exit-code report but does not gain new runtime features.

## Reporting a problem

Launch through `JFG-Launcher.exe` to keep a local support session. If the game
crashes, click **Create support report**, inspect the ZIP if desired, and attach
it to a [playtest issue](https://github.com/TK22-26/JetForceGemini-Recomp/issues/new?template=playtest.yml).
Include the level/menu, what you pressed, what you expected and whether the
problem repeats. If Windows or the launcher closes unexpectedly, reopen the
launcher and select the affected session when creating the report.

[Preview 0.4.0-preview.2](https://github.com/TK22-26/JetForceGemini-Recomp/releases/tag/v0.4.0-preview.2) expands reports with session selection,
recognized setup/compiler errors, system/configuration details, game build and
symbol identity, automatic crash stacks and an on-demand **Capture freeze** action.
The older **0.4.0-preview.1** retains its original two-log exporter.

See [playtesting and reporting](../playtesting.md) for the file inventory, retention,
older-build behavior and GitHub attachment instructions, and
[support diagnostics](support-diagnostics.md) for maintainer validation.

## Build the launcher

```powershell
powershell -NoProfile -File scripts/build_launcher.ps1 -Test
python scripts/package_launcher.py
```

Use `-OutputDirectory` to build into a separate folder while another launcher
copy is running.

The C# helper uses the Windows .NET Framework compiler and has no NuGet or
game dependency. Outputs remain under ignored `build/launcher`. Tests cover ROM
rejection, executable validation, argument forwarding, save preservation,
source discovery, setup argument forwarding, embedded-script restoration,
missing-tool installation plans, retries, ROM rejection before downloads, controller profile validation,
button capture, crash-process reporting, log bounds/redaction, archive contents, and UI rendering.

The ZIP uses a fixed allowlist: launcher executable, this guide, build setup
guide, and project license. It excludes the generated game executable, game
assets, runtime DLLs, ROM, saves, private diagnostics and compiler debug symbols.
Keep locally generated game output outside release archives. Original project
contributions use MIT; third-party notices retain their separate scope.
Packaging requires a clean committed source tree. The build receipt binds the
launcher to its source revision and installer.

A pristine Windows installation with UAC/reboot has not yet been tested end to
end. The local ROM-to-game build and launch were demonstrated separately; the
new setup flow has automated tests and a real pinned-source download check.
Please report installer failures with the stage, reproduction steps, and the
exported support ZIP.


## Live map Mods

Open **Live map > Settings > Map settings > Mods** for three independent toggles:

- **Warp to exits:** double-click a loaded exit marker or an exit in the inspector
  to move your current character to it. The game's normal exit requirements apply.
- **Infinite health:** keep your current character at full health and prevent
  damage during gameplay.
- **Instant kill enemies:** automatically defeat loaded ordinary squad enemies.
  Tribals and friendly NPCs are spared. Switching off does not revive enemies.

These options use the existing gameplay helpers, work in ordinary live-tool
sessions with the updated native runtime, and remain off by default. You can
choose Mods before starting a game; the choices are saved with your profile and
applied on subsequent launches, even if the map is closed. Pausing or waiting for
a game does not disable the controls. An actual loading/applying failure shows an
**Error loading mods** popup and disables Mods for that connection; reopening the
map or starting a new game retries. **Restore defaults** switches all three off.
Warping
stops autopilot and rejects stale room requests and scripted scenes.

## Navigation mod preview

The optional **Navigation mod** checkbox enables full health, automatic clearing
of ordinary squad enemies during gameplay, and local map/exit exports.
It creates a separate campaign profile on first use. **Live map** opens the
exports, and **Controllers** continues to control the shared input mappings.
When a complete native build is beside the launcher, it is selected automatically.

Use the matching native build from the mod source revision.
See [navigation mod setup and limits](navigation-mod.md), including scripted-scene
exclusions and the remaining route-following work.

## Master volume

Audio > Audio settings has a 0-100% **Volume** slider. **Mute / Unmute** is directly in the Audio menu, with **Ctrl+M** available while the launcher or hosted game is foreground.
They remain usable while the game runs and are remembered for subsequent
launches. Muting preserves the chosen volume so unmuting restores it.
Normal and Navigation mod launches share these audio preferences.

The native game reads the launcher's audio.ini preference before its first audio
buffer, then polls for changes every 100 ms. Updates affect final host PCM only;
the original game's music/SFX levels still apply. Already queued audio can take
a few tenths of a second to drain after a change. Brief gain ramps avoid clicks.
This requires the updated native game executable paired with this launcher.


Live Map displays living enemies as red diamonds, independent of the collision
and unknown-origin overlays. The inspector's Enemies filter shows their current
position and health. Classification uses qualified normal squad-member records;
tribals, dead members and unknown actors are not labelled as enemies. Unsupported
enemy behaviors (including unqualified boss actors) retain ordinary entity
markers rather than guessed hostility. Older runtimes without hostility metadata
must be rebuilt to provide enemy markers. Telemetry does not alter game memory.


## Keyboard, mouse and separate-stick aim

Open **Controllers > Controller mapping…** to open the separate, resizable
RmlUi mapping window. Select a player, then **Add device** or **Change device**.
Choose **Gamepad**, **Mouse and keyboard**, or **Automatic**, and confirm with
**Add** / **Use device**. Cancel leaves the saved assignment unchanged. **Remove**
disconnects this player while preserving their bindings.

The controller list shows only connected devices available to the selected
player, including their own connected controller. Devices assigned to other
players are excluded, including automatic assignments. Unplug/reconnect refreshes
the list; adding a device checks availability again before saving. A disconnected
saved controller remains identified above the bindings, without appearing as an
available device. Its configuration is retained for reconnection.

The setup follows Cemu's player pages, add/remove device workflow, connection
status, settings and clickable binding fields ([input settings source](https://github.com/cemu-project/Cemu/blob/main/src/gui/wxgui/input/InputSettings2.cpp),
[device discovery source](https://github.com/cemu-project/Cemu/blob/main/src/gui/wxgui/input/InputAPIAddWindow.cpp)).
Dolphin's mapping window also informs refreshing discovery without changing
saved selections ([source](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/DolphinQt/Config/Mapping/MappingWindow.cpp)).
These are independently implemented interaction patterns in RmlUi. JFG currently
uses one input device per player and discovers gamepads through XInput.

**Mouse and keyboard** shows key and mouse-button bindings with experimental
mouse options. A gamepad shows controller bindings and experimental dual-stick
options. **Settings** reveals mouse sensitivity and look options for keyboard,
or gamepad deadzone, thresholds, inversion and dual-stick options for gamepads.
Click a binding, release the controls, then press a key, mouse button, controller
button, move a stick or scroll the wheel as appropriate to that device.
Escape cancels binding capture; keyboard capture also cancels on focus loss.
Automatic follows its assigned device, with a Player 1 keyboard fallback before
a physical controller has been assigned. A removed player shows only device setup.

The experimental toggle is integrated into the selected device's binding page
and remains off by default. It applies to the whole profile. Keyboard bindings
remain editable and active while experiments are disabled.
Choose a Normal or Expert preset matching JFG's in-game scheme. Device changes
retain saved bindings. Presets do not change the guest game's control scheme.
Closing the mapping window releases the settings pause and input capture.

Keyboard presets use WASD for the original analog movement, left mouse to fire,
right mouse to aim, Space to jump and Tab for Start. Wheel up/down issue the
original L/B weapon controls; these remain context-dependent guest actions.
Every keyboard/mouse binding is editable, including five mouse buttons and
both wheel directions. Menus also accept Enter to confirm, Backspace to go
back and arrow keys to navigate. Escape and F11 retain their host shortcuts.

Controller presets use RT to fire, LT to aim, A to jump and the other stick
for aiming and camera control. Ordinary gamepad bindings remain editable with experiments disabled.
Separate camera and aim sensitivities, vertical scaling, stick response curves,
aim-stick deadzone and vertical inversion are
saved separately for each player in `pc-input.ini` and `pc-input-2.ini`
through `pc-input-4.ini` beside the controller profiles. Settings reload live.
Malformed profiles are rejected without partially applying values.

The profile-wide switch is stored in `experimental-controls.ini`; missing or
malformed switch files disable the new input paths. Turning it off restores the
ordinary movement behavior and disables mouse capture and separate-stick aiming,
while retaining saved bindings and experimental settings. Ordinary controller remapping
remains available. Changes made to those normal mappings by a controller preset
remain saved; the switch does not erase controller bindings.

Keyboard and controller presets enable **Modern camera + movement**. In this
mode, the mouse or other stick rotates the ordinary third-person camera.
Hold aim for direct mouse aiming or steady-rate stick aiming; WASD / the
movement stick can move forward, backward and sideways in ground aim states.
Diagonal aim movement is normalized. JFG still applies movement collision and
camera scenery constraints, with a small clearance from resolved wall hits.
Special character actions retain their guest movement logic. Camera and aim
sensitivity can be adjusted independently for each device type.

Mouse input retains fractional movement at low sensitivity and does not queue
delayed turns after a fast flick. Capture releases on focus loss, opening
settings, pause and scripted cameras. Returning from settings requires neutral
input, including the aim stick. The separate aim stick suppresses its original
button mappings only while the experimental look path is active; menus, scripted
scenes and unsupported modes retain ordinary mappings. Turning **Modern camera + movement** off keeps
the earlier separate-input aiming behavior and the original camera.

PC profiles now use version 3. Existing version-1 and version-2 profiles retain
their saved bindings and behavior, with their previous sensitivity applied to
both camera and aim, linear stick response and 100% vertical scaling.
Recordings started with the experimental switch enabled use input format v3,
which records the first player's movement, look mode and look axes separately.
V1/v2 recordings continue to use the original controls; v3 extensions also
require the experimental switch when replayed. A recording started with the
switch off stays v2 and suppresses extensions for that recording.

The implementation targets single-player in the supported US build and checks the guest
routines, camera stack and active actor before applying changes. Native replay
checks cover direct aiming, ground movement and third-person orbit in the
retained Juno scene. The experimental-off replay matches the stock player and
camera state. Tests also cover pause/resume, firing, and a v3 recording that
reproduces the actor, aim and camera event timelines exactly. Those replay results
come from earlier builds. The current controller changes passed 89 input checks,
564 launcher checks, 12 setup checks and actual RmlUi persistence checks, and the
launcher/runtime builds completed. The final native replay sweep remains incomplete.
These checks do not establish gameplay feel across all characters, weapons,
rooms, scripted transitions or split-screen modes; those still need playtesting.
A steep-angle camera test near a tree also exposed scenery occlusion; complete
camera placement and feel are not yet qualified.

See [PC controls status and remaining work](../planning/pc-controls-status.md).
