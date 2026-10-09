# Project notes for Claude

RGO-Recomp is the owner's **private** fork of
[TK22-26/JetForceGemini-Recomp](https://github.com/TK22-26/JetForceGemini-Recomp),
a static recompilation (N64Recomp + N64ModernRuntime + RT64) of an N64 game into
a native Windows executable. Upstream aims for a faithful 1:1 port; this fork
adds improvements on top. The roadmap is in `docs/rgo/ROADMAP.md`.

## Hard rules
- The repo is private. Never commit ROMs, anything generated from the ROM
  (recompiled C, ELFs, overlay layouts, generated audio, extracted textures), or
  build output. Upstream's `scripts/check_repository_hygiene.py --history`
  enforces much of this, so run it before every PR.
- Keep upstream's `LICENSE` (MIT), `CREDITS.md` and `THIRD_PARTY_NOTICES.md`
  intact. N64ModernRuntime is GPL-3.0, so think about licensing before anything
  is ever distributed.
- Don't open PRs or issues upstream unless the owner asks.
- Use "RGO" in branch names, commit messages and PR titles.
- `AGENTS.md` describes the upstream maintainer's own Codex tooling (a "guarded
  supervisor" and ledger). It does not apply here.

## Remotes
- `origin` is ravenx444/RGO-Recomp.
- `upstream` is TK22-26/JetForceGemini-Recomp. To take upstream fixes:
  `git fetch upstream`, merge `upstream/main` into a branch, then open a PR.

## Building and testing
- **On Windows (the owner's PC):** use the launcher, or
  `python scripts/build_from_rom.py --rom <path-to-rom> --play`. Today that
  needs Visual Studio 2022 and WSL2 Ubuntu 24.04. Build output goes to
  `%LOCALAPPDATA%\JFG\b\...`, saves to `%LOCALAPPDATA%\JFGRecomp\profiles\default`.
- **In the cloud (Linux):** you can run the same checks upstream's CI Linux job runs:
  - `python scripts/check_repository_hygiene.py --history`
  - `python scripts/validate_project.py` (needs `pip install -r requirements-dev.lock.txt`)
  - `python -m unittest discover -s tests -p "test_*.py"`
  - `cmake --preset linux && cmake --build --preset linux && ctest --preset linux`
  The cloud **cannot build the Windows executable or run the game**. Anything
  gameplay-related has to be tested by the owner on their PC, so say clearly
  what to test and how.
- The ROM is in the private repo ravenx444/RGO-source (attach it with
  `add_repo`). Use it only locally, and never copy it or its derivatives into
  this repo.

## Related repos
- ravenx444/Rocket-Group-Orion: the owner's decomp working copy. Use it for
  function names, struct layouts and notes on what functions do
  (`docs/RGO_NEAR_MISSES.md` holds detailed notes on ~60 functions).
