# Rocket Group Orion (RGO) 0.1.1-preview.3 tester build

This is an early Windows x64 test build. Please keep a separate copy of any
existing RGO installation. Extract the entire package to a short, writable path
such as `C:\RGO-Test`, then open `RGO-Launcher.exe`.

Supply your own supported US Jet Force Gemini `.z64` ROM. The package contains no ROM, built
game, game textures, music, saves, or extracted weapon icons. First setup builds
the game on your PC. It needs internet access for dependencies and any missing
Windows tools, several GB of downloads and additional disk space for compilation.
WSL, Ubuntu and virtualization are not required. Setup can take an hour or more;
actual time and download size depend on your PC and installed tools.

If `source.bundle` is included, keep it beside the launcher. It supplies the
exact reviewed project source without requiring access to the private GitHub
repository. It is not a game executable. Setup verifies the pinned revision.
If no bundle is included, setup downloads the exact pinned source from the public
repository; a GitHub account is not required.

## What to try

- First setup, ROM selection, Play, closing and reopening, and save persistence.
- Keyboard/mouse and any available controller. Check normal and inverted aiming,
  dual input, Vela/Lupus camera movement, surface swimming, and boss strafing.
- Hold Y/Triangle or Tab for the weapon wheel. The opener and navigation stick
  can be rebound in controller settings. Release to equip; centre cancels.
  While open, press your mapped interact/A action to hide or show Floyd.
- D-pad menu navigation and weapon shortcut assignment in the original Weapons menu.
- Widescreen HUD, pickup messages, weapon switching, damage overlay and pause menu.
- Tri-Rocket and Remote Mine crosshairs with interpolation and anti-aliasing enabled.
- Audio during the intro, aiming at enemies, combat, and area transitions.
- Repeated area changes, cutscenes, character changes and the first Juno boss,
  especially Tri-Rockets hitting bats. Keep ordinary gameplay saves as you go.

## Known limits

- Loading and boss fixes have bounded local tests and some owner gameplay testing.
  The latest loading-hang safeguard still needs confirmation at the reported door.
  Crashes or hangs may remain. This is not a full-campaign or stable release.
- Display presentation can exceed 60 Hz. New game frames can still remain at 60
  or drop to 30 under load. Higher display counters do not guarantee new simulation
  frames at that rate. Performance work is ongoing.
- Camera, controller coverage and interpolation need broader hardware testing.
- A fresh setup on the developer's PC previously hit Windows Application Control
  blocking an unsigned locally built generator. That clean-machine setup gate is
  still unqualified. If Windows blocks a tool, preserve the setup log and ask your
  administrator or security provider to review it. Do not change policy for this test.

## Reporting problems

For a freeze, use Help > Support report > Capture freeze before closing if possible.
Create a support ZIP and send it to the person who supplied this tester build.
If a menu action fails, the logs are in `data\reports` and `data\setup.log`.
Inspect reports before sharing. Do not send your ROM, saves, generated game files,
private captures, or the entire data folder.

Include Windows version, CPU/GPU, controller, video settings, character, area,
weapon, and exact steps. For crashes, say whether the game closed, returned to
launcher, or stayed frozen. A screenshot of the full FPS overlay helps with
performance reports. Mention any other games running at the same time.

Your saves/settings are in `data\profile`. Back that folder up before changing
builds. Keep `README.md`, the credits, notices and `licenses` files with the launcher.
