# Getting started

Build and play the Windows x64 v1.0.0 release using your own supported North American
Jet Force Gemini ROM. Start with the launcher for graphical setup, or use the
source build instructions when you want the latest changes on `main`.

## Requirements

- Windows 10 or Windows 11, x64, with WSL2 support and virtualization enabled.
- The supported 32 MiB US big-endian `.z64` ROM. Other regions, modified ROMs,
  and byte-swapped files are not supported.
- Internet access and disk space for Visual Studio, Ubuntu, dependencies,
  and local build output. First setup can download several GB.
- Microsoft App Installer (`winget`) for automatic prerequisite installation.

The launcher checks your ROM locally. It does not download one.
Windows 10 users should read the [open setup issues](known-issues.md#windows-10-setup)
before starting.

## Use the launcher

1. Download `JFG-Launcher-1.0.0-windows-x64.zip` from
   [v1.0.0](https://github.com/TK22-26/JetForceGemini-Recomp/releases/tag/v1.0.0)
   and extract the complete ZIP, including its license files.
2. Open the extracted `JFG-Launcher.exe` and select your ROM.
3. Click **Set up and build**. Review the setup information and approve the
   required Windows installers. If Windows requires a restart, reopen the
   launcher afterward and continue setup.
4. Once the build completes, click **Play**. Future launches reuse it.

The launcher downloads a specific source revision recorded in its release.
Installing an older launcher does not include later `main` fixes. See the
[launcher guide](development/launcher.md) for controls, saves, controller
configuration, and file locations.

If setup repeatedly requests a restart or stops before building, consult
[known issues](known-issues.md) and [report the failed stage](playtesting.md).

## Build the latest source

First install the prerequisites in [ROM build setup](development/rom-bootstrap.md).
Then run these commands in PowerShell:

```powershell
git clone https://github.com/TK22-26/JetForceGemini-Recomp.git
cd JetForceGemini-Recomp
python scripts/build_from_rom.py --rom 'D:\Games\my-copy.z64' --play
```

Replace the example ROM path with your own. This command downloads pinned
source dependencies and builds the game; install the developer tools first.
Record `git rev-parse HEAD` if you report a source-build problem.

The command also builds the launcher frontend and, with `--play`, starts the
game through it, with the same menu bar and settings as a release launcher and
its support-report workflow. Add `--no-frontend` for the bare game window.

## Start playing

Try movement, jumping, aiming, shooting, and opening and closing pause.
At a normal save opportunity, check that quitting and relaunching restores
your progress. Continue playing normally; these checks do not require a report
unless something goes wrong.

For a problem, follow [playtesting and reporting](playtesting.md).
For code changes, see [contributing](../CONTRIBUTING.md).
