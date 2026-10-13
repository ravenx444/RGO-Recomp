# Building RGO (developers)

Players never build by hand: the launcher installs the tools, downloads the
RGO source it was packaged from and builds the game (see
[PLAYER_GUIDE.md](PLAYER_GUIDE.md)). This page is for working on RGO itself.

From a checkout, with the prerequisites installed (Visual Studio 2022 C++
tools, Python 3, WSL2 Ubuntu 24.04; or run the launcher's setup once):

```powershell
python scripts/build_from_rom.py --rom C:\path\to\rom.z64 --play         # build, then play through the launcher UI
python scripts/build_from_rom.py --rom C:\path\to\rom.z64 --clean        # reuse nothing (full build, about an hour)
python scripts/build_from_rom.py --rom C:\path\to\rom.z64 --regenerate   # generate the game code from the ROM again
python scripts/build_from_rom.py --rom C:\path\to\rom.z64 --no-frontend  # skip the launcher; with --play, bare game window
```

Or double-click `scripts\dev\rgo-rebuild.cmd`: it asks for a branch, updates
it, builds it with the options above and plays it. It remembers the ROM in
`%LOCALAPPDATA%\RGO\rebuild-rom.txt` (delete that file to choose another, or
set `RGO_ROM`), and runs from a copy in `%TEMP%` because `git checkout` can
rewrite it.

A source build also builds the launcher (`RGO-Launcher.exe`) unless
`--no-frontend` is given; with `--play` it starts the game through it, and the
launcher's *Set up and build* then rebuilds from that checkout. Close the
launcher before a source build that builds it.

## What gets reused

A build has two expensive parts:

1. **Generation** (the `generate` step, in WSL). It rebuilds the decomp and
   recompiles the ROM into C code.
2. **Compilation** (the `compile` step, Visual Studio). It compiles the generated
   code, RT64 and our runtime.

The generated code depends only on:

- the ROM,
- the pinned decomp and N64Recomp revisions in `dependencies.lock.json`,
- `scripts/generate_from_rom.py` and every script it imports or runs (found
  automatically by following imports),
- `patches/n64recomp/`,
- the WSL distribution name.

The build hashes all of these into a key and stores it in the workspace
(`generation-key.json`) when generation finishes. The next build reuses the
newest complete workspace with the same key, and reuses its Visual Studio build
folder, so MSBuild recompiles only what changed. It prints what it decided:

```
Reusing ROM generation from 2026-10-11 14:02 (inputs unchanged)
Regenerating: scripts/generate_from_rom.py changed
Generating from the ROM: no cached generation yet
```

Three files are copied unchanged from the checkout into the generated code
(`src/bootstrap/brokered-rsp.hpp`, `src/bootstrap/production-audio-adapter.cpp`
and `include/jfg/boot/original_timing_fast.h`). They don't trigger a
regeneration. A reused build gets the current copies before compiling
("Updated in the cached build: ..."), so changing them costs only a recompile.

Switching branches is safe. Git gives changed files a new timestamp, CMake
configures again every build (so new, removed or renamed source files and
CMakeLists changes are picked up), and MSBuild rebuilds whatever changed. If two
branches differ in a generation input, each has its own cached generation; the
older one is kept (see below), so switching back and forth doesn't regenerate
each time.

Configure no longer rewrites the patched RT64 sources when their content hasn't
changed (`cmake/WriteIfChanged.cmake`), so an unchanged build doesn't recompile
them.

## Expected times

These are estimates; the cloud can't run the Windows build. Each step prints
`Step N of M`, and steps that take a minute or more print how long they took.
Please send those lines so the estimates can be replaced with real numbers.

| Build | What runs | Estimate |
|---|---|---|
| First build after this change, `--clean`, or a generator change | everything | about as long as before (about an hour); a bit less on machines with more than 8 threads, since it now uses all of them |
| Runtime-only change (e.g. `native_boot.cpp`, `include/jfg/...`) | configure, changed files, link, frontend | about 3-10 minutes |
| Nothing changed | configure, up-to-date checks, generated-object audit | about 1-3 minutes |

The link step and the generated-object audit (which runs on every build) are
probably the biggest parts of an incremental build.

## When it regenerates

- A generation input above changed (a pull or branch switch touched the
  generator scripts, `patches/n64recomp/`, or the decomp/N64Recomp pin).
- You pass `--regenerate` or `--clean`.
- The cached generation is incomplete or a generated file was modified.
- The cache format changed (`CACHE_SCHEMA` in `scripts/rgo_build_cache.py`).

Builds made before this change have no key, so the first build after it
regenerates once.

Not part of the key (they should not change the output): the WSL toolchain
versions (gcc, Python, CMake), and the decomp's Python packages. If you update
WSL and the game behaves strangely, run once with `--regenerate`.

## Options

- `--jobs N`: parallel jobs for generation and compilation. Default: all
  logical cores, but at most one per 1.5 GB of RAM (an estimate of what a
  compiler process on the large generated files needs), up to 64.
- `--regenerate`: generate again even if a cached generation matches. The
  generated code is then compiled from zero too, because the build folder
  belongs to the generation.
- `--clean`: reuse nothing: regenerate, compile from zero and rebuild the
  frontend from zero.
- `--keep-old-builds`: don't delete older build folders.
- `--resume-native <workspace>`: still works. It continues a specific
  workspace without checking the generation key.

## Disk space

Each workspace (`%LOCALAPPDATA%\JFG\b\<checkout>\w\<id>`) is several GB. After a
successful build, the build keeps the workspace it used plus the most recently
used other reusable one (typically your other branch), deletes the rest, and
prints how much space it freed. It only
deletes run folders in this checkout's `w` folder, never anything else, and
skips a folder that another build is using. Each build
holds a lock file (`build.lock`) in its workspace; Windows releases it
automatically if the build crashes.

WSL keeps a temporary copy of each generation under `/tmp/jfg-rom-build-*`
(listed in the workspace's `linux-workspace.txt`). Regenerating is now rare, so
these pile up much more slowly. Ubuntu under WSL normally clears `/tmp` when
WSL restarts (`wsl --shutdown`).
