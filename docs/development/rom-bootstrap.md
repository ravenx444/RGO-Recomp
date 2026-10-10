# Build the game from your ROM

For downloads and the first-run path, start with [getting started](https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/docs/getting-started.md).
See [known setup issues](https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/docs/known-issues.md#windows-10-setup) before installation.

This Windows x64 release starts with the supported US big-endian ROM and
creates a local native executable. The launcher can install missing developer
tools and obtain its pinned source checkout automatically. Open the standalone
EXE, choose your ROM, then click **Set up and build**. Administrator prompts and
a Windows restart may be needed; reopen the launcher to continue afterward.
See [the launcher guide](https://github.com/TK22-26/JetForceGemini-Recomp/blob/main/docs/development/launcher.md) for downloads, storage, and requirements.

For manual setup or command-line use, install Git, Python 3.11 or newer, Visual Studio 2022 with Desktop development
with C++ and CMake, and WSL2 with Ubuntu 24.04. Inside Ubuntu, install the
upstream build prerequisites:

```sh
sudo apt update
sudo apt install build-essential gcc-multilib binutils-mips-linux-gnu python3-venv python3-pip pkg-config cmake ninja-build git wget
```

From PowerShell at the repository root:

```powershell
python scripts/build_from_rom.py --rom 'D:\Games\my-copy.z64' --play
```

Alternatively, run `launcher/windows/Build-And-Play.ps1` in PowerShell to choose
the ROM using a file picker. Compilation can take several minutes.

The script also builds the unified frontend (`JFG-Launcher.exe`, via
`scripts/build_launcher.ps1`) into the short build cache (`...\JFG\b\<key>\f`).
With `--play` it points the default profile at the fresh game build and starts
the game through that frontend, so the menu bar, Video settings, controller and
audio settings work exactly as in a release launcher. Inside a frontend started
this way, **Set up and build** rebuilds the game from the same checkout
(`JFG_SOURCE_ROOT`) instead of downloading upstream source. Close any open
frontend before building; the script refuses to overwrite a running one. Pass
`--no-frontend` to skip the frontend; with `--play` that starts the bare game
window without menus, as before.
The script validates the ROM before downloading dependencies. Other regions,
modified ROMs, and byte-swapped dumps are rejected.

The recipe clones the pinned dependencies, creates a fresh matching ELF in a
private WSL temporary directory, derives overlay boundaries from that build's
linker metadata, applies the pinned recompiler patch series, generates CPU and
audio code, and builds the live Windows runtime. It consults the upstream
decompilation locally; no upstream game source is copied into this repository.
Its generated audio configuration comes from the ROM's DMA descriptors and
command table. The host audio adapter under `src/bootstrap` is project code.

ROM copies, generated code, logs and binaries stay in the short local cache
`%LOCALAPPDATA%\JFG\b\<checkout-id>\w\<run-id>` and a private WSL temporary
directory. Do not upload either directory or add it to a release. The command
prints the exact workspace; its `linux-workspace.txt` identifies the WSL folder
retained for diagnosis. A failed run preserves its files. A normal invocation
creates a fresh workspace. If generation finished but compilation failed, pass
that workspace to `--resume-native` with the same `--rom` (and `--build-root`, if
used). The script checks the generation manifest and hashes before resuming.
Legacy workspaces under `tools/private/local-builds` remain intact and can be
resumed when their native output path fits MSBuild's limit.

`--dependency-root` accepts an existing pinned upstream source/tool cache. This
can reuse IDO tools and repository objects; it creates a fresh Python environment.
The recipe always extracts and builds the matching ELF again. It never takes a
maintainer-generated ELF, overlay layout, CPU root, or audio source as input.

`--generation-timeout` bounds the Linux process group (default one hour).
`--jobs` controls build parallelism, and `--distro` selects a different installed
WSL distribution. Existing dependency checkouts at another revision are rejected
without changing them. Save files live in `%LOCALAPPDATA%\JFGRecomp\profiles\default`.

## Build verification

On 2026-10-01, the default dependency-download path produced a fresh matching ELF,
CPU corpus and audio program, then compiled the Windows x64 live runtime.
Development failures were corrected and native compilation resumed from those
locally generated inputs. No maintainer-generated game code was used as input.

A fresh save profile reached the title sequence with 592 rendered frames and
599 decoded audio tasks over 1,200 retraces. A second process accepted menu input
and preserved the profile. The new-game test committed 13 FlashRAM writes;
its save file changed and remained available for the next launch. Relaunching
reached playable Goldwood gameplay with 3,283 presented frames and 3,299 audio
tasks over 6,600 retraces. Further input tests moved the player through Goldwood
and fired three shots. The launcher's production launch path opened a live game
window and exited successfully after closing it.
These runs reported zero unsupported device accesses. Frames, audio,
ROM-derived code and detailed diagnostics remain local.

The launcher and setup tests use ROM-free fixtures. Public CI exercises the
build recipe, native output paths, launcher compilation, and source policy.
Neither CI nor compilation certifies a completed campaign. A pristine Windows
installation with installer elevation and reboot remains an outstanding
playtest of the setup flow.

A successful build establishes compilation. The maintainer has completed the
campaign with the testing scope described in the README; original-console
parity and broader unassisted play remain under validation. The v1.0 runtime
also passes the combined opening/Quarry validation recorded in
[the timing findings](../planning/quarry-audio-sync-findings.md). Keep generated
game code and binaries local.


Windows build paths: use `python scripts/build_windows.py --test` for ROM-free
development builds. It keeps output under `%LOCALAPPDATA%\JFG\b\<checkout-id>`
so deep source checkouts do not lengthen MSBuild tracking paths. The ROM builder
uses the same cache for dependencies, generated inputs and native output. Existing
workspaces are preserved; `--build-root D:/JFG-builds` selects a shorter writable
location when necessary. Different checkouts receive separate cache IDs. Keep
these local generated builds and symbols out of issue attachments.
