# RGO

RGO is an unofficial, fan-made fork of
[TK22-26/JetForceGemini-Recomp](https://github.com/TK22-26/JetForceGemini-Recomp),
the native PC recompilation of the Nintendo 64 game *Jet Force Gemini*.
Upstream aims for a faithful 1:1 port. RGO adds modern improvements on top:
widescreen, real 60 fps game logic, modern controls and more.

**This repository contains no game code, ROMs, or game assets.** You supply
your own legally obtained ROM, and the game is built on your own PC from it.
Nothing generated from the ROM is distributed.

RGO is developed with AI assistance (Anthropic's Claude), directed and
tested by the project owner.

## Credit where it's due

RGO exists because of the work of the people below. Please support their
projects.

- **[TK22-26](https://github.com/TK22-26) and the JFG Recomp contributors**:
  [JetForceGemini-Recomp](https://github.com/TK22-26/JetForceGemini-Recomp),
  which this repository is a fork of. That covers the recompilation, runtime,
  renderer integration, launcher, build pipeline and tests. RGO is a set of
  changes on top of their work. Their git history and authorship are kept
  in this repository.
- **Community contributors to JFG Recomp**, credited in
  [CREDITS.md](CREDITS.md) (including Luke Deardoff).
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** by Wiseguy
  (Mr-Wiseguy) and contributors, the static recompiler that turns N64 code
  into C (MIT).
- **[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)** by the
  N64Recomp contributors, the runtime support for recompiled games
  (GPL-3.0). It is built on your PC from source and is not distributed by
  this project.
- **[RT64](https://github.com/rt64/rt64)** by DarÃƒÂ­o (DarioSamo) and
  contributors, the renderer, and **[Plume](https://github.com/renderbag/plume)**,
  its graphics abstraction (both MIT). RT64 in turn credits its own
  third-party components.
- **[Jet Force Gemini decompilation](https://github.com/Ryan-Myers/Jet-Force-Gemini)**
  by Ryan Myers and contributors. The build uses it on your PC to produce a
  symbol map of the game. RGO's game-function hooks use the names, addresses
  and structure layouts it documents. No decomp source is copied into this
  repository.
- **[ares](https://ares-emu.net/)** by Near and the ares team: the instruction
  and cache timing model and the cartridge DMA timing formula are adapted from
  it (ISC, notice kept in the source).
- **X-Scale**, for the CIC-NUS-6105 challenge-response algorithm (notice kept
  in the source), as found in [BizHawk](https://github.com/TASEmulators/BizHawk).
- **[SDL](https://www.libsdl.org/)** (zlib), **[RmlUi](https://github.com/mikke89/RmlUi)**
  (MIT, with its robin_hood and itlib containers) and
  **[FreeType](https://freetype.org/)** (FreeType License), used by the
  launcher.
- **Fonts:** [Barlow](https://fonts.google.com/specimen/Barlow) and
  [Chakra Petch](https://fonts.google.com/specimen/Chakra+Petch) (SIL Open
  Font License 1.1, from [Google Fonts](https://github.com/google/fonts)),
  and RmlUi's Courier Prime Code (OFL).
- **Build tools:** [splat](https://github.com/ethteck/splat),
  [spimdisasm](https://github.com/Decompollaborate/spimdisasm),
  [rabbitizer](https://github.com/Decompollaborate/rabbitizer),
  [asm-processor](https://github.com/simonlindholm/asm-processor),
  [LLVM/Clang](https://llvm.org/), [ELFIO](https://github.com/serge1/ELFIO),
  [{fmt}](https://github.com/fmtlib/fmt) and
  [toml++](https://github.com/marzer/tomlplusplus), plus the decomp's own
  build tools ([ido-static-recomp](https://github.com/decompals/ido-static-recomp),
  [objdiff](https://github.com/encounter/objdiff)). These are downloaded and
  run on your PC during the build. None of them are part of this repository.

Full licence texts and notices are in [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), `launcher/ui/licenses/`,
`launcher/ui/fonts/` and `patches/*/LICENSE.upstream`. Pinned dependency
versions are in `dependencies.lock.json` and
[docs/legal/dependency-license-inventory.md](docs/legal/dependency-license-inventory.md).

*Jet Force Gemini* and all of its content are the property of their respective
owners (Rare Ltd. and Nintendo). RGO is not affiliated with or endorsed by
Rare, Nintendo, Microsoft, or the JetForceGemini-Recomp project.

## What RGO adds

Compared with upstream:

- **Widescreen**, a wider 3D view without stretching, with the HUD anchored to
  the screen edges ([docs](docs/rgo/widescreen.md)).
- **Real 60 fps game logic.** The game updates 60 times a second at its
  original speed, with no frame interpolation. A PC that can't keep up gets
  fewer frames instead of slow motion. You can also run the original 30 fps
  with the console's slowdown removed ([docs](docs/rgo/framerate.md)).
- **Display settings**: any resolution and aspect ratio (ultrawide
  included), render scale, MSAA anti-aliasing, VSync and an FPS counter
  ([docs](docs/rgo/display-settings.md)).
- **Controllers**: DualShock 4 and DualSense with PlayStation button names and
  rumble, gyro aiming (DualSense, DualShock 4, Switch Pro), and choosing which
  controller is Player 1 ([docs](docs/rgo/input.md)).
- **Mouse and keyboard**: direct mouse aiming (not a fake stick, no
  spring-back) with hold or toggle aim on the right mouse button, and a Modern
  scheme where the mouse turns the camera.
- **Dual input**: mouse and keyboard plus a controller at the same time, with
  no mode switching.
- **Modern twin-stick controls** with the camera on the right stick.
- **Persistent gore**: severed limbs, drone heads and blood splats stay, each
  with its own setting and limit ([docs](docs/rgo/gore.md)).
- Faster rebuilds and audio fixes.

Every feature is an option; the original behaviour is still available. The
roadmap and current status are in [docs/rgo/ROADMAP.md](docs/rgo/ROADMAP.md).

## Install and play (Windows 10/11, 64-bit)

1. Download `RGO-Launcher-<version>-windows-x64.zip` from
   [Releases](https://github.com/ravenx444/RGO-Recomp/releases) and extract
   it to a folder you can write to, such as `C:\Games\RGO`.
2. Open `RGO-Launcher.exe`, pick your ROM (the North American 32 MiB
   big-endian `.z64`; the launcher checks it), and click **Play**.
3. The first time, the launcher installs what it needs (Git, Python, Visual
   Studio Build Tools), downloads the RGO source and builds
   the game from your ROM. **First-time setup can take an hour or more.** Leave the launcher open and the game starts by itself when it's
   done. Later starts take seconds.

Everything RGO creates (saves, settings, the downloaded source and the build)
stays in a `data` folder next to the launcher; delete the folder to remove
it. The [player guide](docs/rgo/PLAYER_GUIDE.md) has the details, settings
and what to do if something goes wrong.

Known issues:
- The heaviest cutscene shots fall back to 30 fps in True 60 mode, and big
  firefights can dip.
- Upstream's own [known issues](docs/known-issues.md) also apply, including
  the open Windows 10 setup problems.

Working on RGO itself? See [docs/rgo/build.md](docs/rgo/build.md).

## Licence

RGO's own code and documentation use the MIT License, like upstream's, and
upstream's copyright notice is kept in [LICENSE](LICENSE). Dependencies keep
their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Builds made on your PC combine components under several licences,
including N64ModernRuntime's GPL-3.0. If you redistribute a built game, you
must meet every one of those licences, and you must never distribute
anything generated from the ROM.
