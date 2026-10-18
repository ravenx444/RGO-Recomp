# Rocket Group Orion (RGO) player guide

RGO is an unofficial PC recompilation with widescreen, real
60 fps game logic, modern mouse, keyboard and controller support, gyro aiming
and more. It is a fork of
[TK22-26/JetForceGemini-Recomp](https://github.com/TK22-26/JetForceGemini-Recomp).

RGO contains no ROM-derived game code or assets. You use your own ROM, and the launcher
builds the game from it on your PC.

For a tester package, read `TESTING.md` first. If `source.bundle` is included,
keep it beside the launcher; setup uses that exact source without GitHub access.

## What you need

- Windows 10 or 11, 64-bit. Setup runs directly on Windows; WSL, Ubuntu
  and BIOS virtualization are not required.
- An internet connection and several GB of free disk space for the first
  setup.
- Your Jet Force Gemini ROM: the North American (US) version as a 32 MiB big-endian `.z64`
  file. Other regions, modified ROMs and byte-swapped `.v64`/`.n64` files are
  not supported. The launcher checks the file and tells you if it's the
  wrong one.

## Install and play

1. Download `RGO-Launcher-<version>-windows-x64.zip` from the
   [releases page](https://github.com/ravenx444/RGO-Recomp/releases).
2. Extract the whole ZIP into a new folder with a **short path** that you
   can write to, for example `C:\Games\RGO`. Don't use `Program Files`, and
   don't run it straight from Downloads (that path is usually too long for
   the build; the launcher tells you if it is).
3. Open `RGO-Launcher.exe`.
4. Click **Change** next to *Game ROM* and pick your ROM.
5. Click **Play**. The first time, this opens *Set up and build*: click
   **Set up game** and confirm.

**First-time setup can take an hour or more.** Please don't
close the launcher: leave it open, and the game will start by itself when
it's done. Later starts take seconds; rebuilds after an update take minutes.

During setup the launcher:
- installs whatever is missing: Git, Python, Visual Studio 2022 Build Tools
  (C++). Windows asks you to approve each installer.
- may ask you to restart Windows after a build-tool installer. Restart,
  open the launcher again and click **Set up game**; it continues where it
  stopped.
- downloads the RGO source this launcher was made for, and builds the game
  from your ROM.

After that, just open the launcher and click **Play**.

## Settings

Use the launcher's menu bar (or press Esc or F10 in game):
- **Video > Video settings**: window mode, resolution and aspect,
  widescreen, frame rate (True 60 or Original), render scale, anti-aliasing,
  VSync, FPS counter.
- **Controllers > Controller mapping**: controllers, keyboard, mouse and aim,
  gyro, dual input.
- **Game > Gameplay settings**: persistent gore options.
- **Audio > Audio settings**: volume.

Keys: F11 fullscreen, Esc or F10 menu, Ctrl+M mute.

## Where RGO keeps its files

Everything RGO creates stays inside its own folder, in `data` next to
`RGO-Launcher.exe`:

| Folder | What it holds |
|---|---|
| `data\profile` | your saves and settings (**Game > Open save folder**) |
| `data\source` | the RGO source the launcher downloaded |
| `data\b` | the game build made from your ROM (several GB) |
| `data\reports`, `data\setup.log` | logs, freeze captures and support reports (**Help > Open reports folder**) |

To move RGO, move the whole folder. To uninstall, back up `data\profile` if
you want your saves, then delete the folder.

The build tools the launcher installs are normal Windows programs and stay
installed: Git, Python and Visual Studio 2022 Build Tools. Other programs may use them too; remove them from Windows Settings >
Apps if you don't need them.

## If something goes wrong

- Read `TESTING.md` and `KNOWN_ISSUES.md` in the ZIP first.
- If the game crashes or freezes, use **Capture freeze** (in **Help > Support
  report**) before closing the game. **Help > Open reports folder** shows the
  logs.
- If Windows Application Control blocks a locally built setup tool, setup
  stops. Have your administrator or security provider review the blocked tool
  and signing policy before retrying. Keep the setup logs for that review.
- If setup stops, use **Help > Support report**, create the ZIP, and
  attach it to an issue on
  [GitHub](https://github.com/ravenx444/RGO-Recomp/issues). The ZIP holds
  logs and settings only, not your ROM or saves. Check it before you share it.

## Credits

RGO is built on the work of:
- TK22-26 and the JetForceGemini-Recomp contributors (the project RGO is a
  fork of), and the community contributors in `CREDITS.md`.
- N64Recomp, N64ModernRuntime, RT64 and Plume.
- The Jet Force Gemini decompilation by Ryan Myers and contributors.
- ares, X-Scale, SDL, RmlUi, FreeType, and the Barlow, Chakra Petch and
  Courier Prime Code fonts.

The full credits are in the
[README](https://github.com/ravenx444/RGO-Recomp#credit-where-its-due). The
licences are in `LICENSE`, `THIRD_PARTY_NOTICES.md`, `THIRD-PARTY.txt` and
the `licenses` folder next to the launcher.

RGO is developed with AI assistance. Original game content and trademarks are
the property of their respective owners (Rare Ltd. and Nintendo). RGO is not
affiliated with or endorsed by them.
