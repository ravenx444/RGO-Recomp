# RGO first public release: plan

Status: planned, not started. Agreed with the owner on 2026-10-12, to start
after the freeze/fibers work lands. Each section is a separate PR.

## Goal

A GitHub Release with one ZIP. The player:

1. Extracts it anywhere they can write to (not `Program Files`).
2. Runs `RGO-Launcher.exe`.
3. Is asked for their ROM. The launcher checks it is the supported US ROM
   (32 MiB, SHA-1 `493ced9008dbe932d6e91179b68e8630cf23a023`, already in
   `launcher/windows/Launcher.cs` `LocalSetup`) and explains clearly if not.
4. The launcher installs/downloads everything needed and builds the game
   **inside the folder it was extracted to**, then starts the game.

Before the first build starts, the launcher shows (and keeps visible while it
runs) a plain message:

> First-time setup takes about an hour. It has not crashed. Please don't
> close this window: leave it open, and the game will start by itself when
> it's done. Later starts take seconds; rebuilds after an update take minutes.

## What upstream's launcher already does (reuse, don't rewrite)

- ROM selection and SHA-1 check (`LocalSetup`).
- Prerequisites through `winget` (`launcher/windows/Setup.ps1`): Git, Python,
  Visual Studio 2022 Build Tools (VCTools), WSL + Ubuntu 24.04 and its
  packages. Windows shows its own approval prompts; WSL can require a restart
  (the launcher resumes afterwards).
- Downloads a pinned source revision recorded in the release, then builds.
- Support reports, crash/freeze capture.

## Work items

### 1. Portable install: everything under the launcher's folder
- Today: profile/saves `%LOCALAPPDATA%\JFGRecomp`, build cache
  `%LOCALAPPDATA%\JFG\b\...` (`scripts/build_paths.py` `windows_cache`,
  `Setup.ps1` `$SetupRoot`, `FirstRun.Root`).
- Change: a portable root next to the launcher exe (e.g. `<folder>\RGO\`)
  for source, dependencies, build cache, profile, saves and reports. Pass
  `--build-root` to `build_from_rom.py` (already supported).
- Keep the long-path guard (`check_binary_path`): if the extract folder is
  too deep, say so before starting and suggest e.g. `C:\Games\RGO`.
- Refuse read-only locations (`Program Files`) with a clear message.
- One-time migration offer for existing `%LOCALAPPDATA%` saves (owner's own).
- System-wide prerequisites (VS Build Tools, WSL/Ubuntu, Git, Python) cannot
  live in the folder; the launcher says what it will install and why.

### 2. First-run expectations and progress
- The message above, shown before setup starts and pinned during the build.
- "Step N of M" (the build already reports named `setup_step`s; Tier 2
  roadmap item) plus elapsed time, so it visibly moves.
- When done: start the game automatically.

### 3. Rebrand and point at RGO
- Name `RGO-Launcher.exe`, window titles, About page.
- The release pins an **RGO-Recomp** revision (the release tag), not
  upstream's. `dependencies.lock.json` stays the dependency source of truth.
- `rgo-rebuild.cmd` keeps working for developers; remove the owner's
  personal default ROM path from it.

### 4. Credit and licences (must be complete before going public)
- Keep upstream `LICENSE` (MIT, upstream copyright line) intact; add the RGO
  copyright line alongside, not instead.
- Keep `CREDITS.md` and `THIRD_PARTY_NOTICES.md`; add RGO's contributors and
  any new third-party pieces (SDL usage for gamepads/audio etc.).
- README: what RGO is, that it is a fork of
  [TK22-26/JetForceGemini-Recomp](https://github.com/TK22-26/JetForceGemini-Recomp)
  with a prominent credit to its authors; credit N64Recomp,
  N64ModernRuntime, RT64, the JFG decompilation authors, SDL, RmlUi, and
  anything else in `THIRD_PARTY_NOTICES.md`; state that RGO is AI-assisted;
  known issues; build-from-source instructions.
- An About/Credits page in the launcher with the same credits and the
  licence texts.
- Licence audit of the release ZIP: list every file in it, and for each
  binary, which components it contains and their licences. N64ModernRuntime
  is GPL-3.0: if any GPL code is inside a shipped binary, the release must
  meet GPL-3.0 (corresponding source for that exact release, licence text
  included). Decide per binary; prefer shipping the launcher only, with GPL
  components built on the player's machine from source.

### 5. No game code or assets, anywhere
- Never in the repo or release: ROMs, recompiled/generated C, ELFs, overlay
  layouts, extracted textures/audio, built game executables, symbols built
  from them.
- Run `python scripts/check_repository_hygiene.py --history` (whole history)
  and inspect the release ZIP's file list before publishing.
- The release contains only the launcher and its licence files; the game is
  built locally from the player's ROM.

### 6. Repo cleanup before making it public
- Write the README (removed in #18).
- Decide on `CLAUDE.md`, `AGENTS.md` (upstream's), and mentions of the
  owner's private repos in docs (`CLAUDE.md` only, now).
- Commit messages contain Claude session links (harmless; leave or note).
- `docs/getting-started.md`, `docs/known-issues.md`: RGO versions.

### 7. Packaging
- GitHub Actions minutes are currently used up, so package on the owner's PC
  with the existing tooling (`scripts/package_launcher.py`) or re-enable CI
  later. Tag `rgo-v0.1`, release notes with features, known issues
  (cutscene freeze on fast PCs if still open, mouse fallback), and the
  first-run expectation.
- The owner flips the repo to public (Settings, Change visibility) and
  publishes the release; that is their call.

## Open questions for the owner
- Saves: portable (in the folder) only, or keep a copy in `%LOCALAPPDATA%`?
- Version name for the first release (`rgo-v0.1`?).
- Whether to tell upstream about the fork.
