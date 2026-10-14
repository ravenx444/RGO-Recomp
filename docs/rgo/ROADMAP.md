# RGO roadmap

Upstream gives a faithful port. RGO adds improvements. Keep each item a
separate PR that the owner can test on their PC, and note what to test.
Order within a tier is the owner's priority. Status as of 2026-10-13.

## Tier 1: core improvements
1. **Proper widescreen.** Done (#2, #3): wider 3D view without stretching,
   edge culling, HUD anchored to the screen edges. See `widescreen.md`.
2. **Real frame rate.** Done (#13, #15, #19): **True 60** runs the game's
   logic 60 times a second at the original game speed (no interpolation),
   and a PC that can't keep up gets fewer frames instead of slow motion.
   **Original** with *Original N64 slowdown* off holds a steady 30.
   Open: the heaviest cutscene shots fall back to 30, and big firefights can
   dip. Rates above 60 would come from interpolation on top. See
   `framerate.md`.
3. **Gyro aiming.** Done (#11, PS5 fixes): DualSense, DualShock 4 and Switch
   Pro. See `input.md`.
4. **Dual input.** Done (#12): mouse and keyboard plus a controller for
   Player 1 at the same time, no mode switching.
5. **Full camera control.** Partly done: the Modern twin-stick scheme (#20)
   and Modern mouse and keyboard scheme (#23) turn the camera with the right
   stick or mouse, with separate Camera Y and Aim Y invert. Not done: a fully
   free camera (pitch, distance, collision) beyond what the game's camera
   allows.
6. **Gears of War-style aiming.** Planned, not started. The crosshair should
   point where shots land: cast from the camera through the crosshair, then
   aim the character at the hit point. Today it's projected forward on a
   spring arm. Plan in `input.md` ("Plan: crosshair points where shots
   land").
7. **Persistent gore and splatter.** Mostly done (#17): severed limbs, drone
   heads (and other pickups that drop as limbs) and blood splats stay, each
   with its own setting and limit. Not done: dead bodies still vanish. See
   `gore.md`.

Also done, not in the original list:
- DualShock 4 / DualSense support with PlayStation button names and rumble
  (#9), choosing which controller is Player 1.
- Direct mouse and keyboard aiming, no fake stick and no spring-back, hold or
  toggle aim on the right mouse button (#10).
- Display settings: any resolution and aspect (ultrawide included), render
  scale, MSAA, VSync, FPS counter (#5). See `display-settings.md`.
- Source builds build the frontend, and `--play` goes through it (#4).
- Audio: no crackle or drift from the queue (#21, #22, audio callback).
- Runtime host speed roughly doubled (#19).

## In progress
- **Guest threads as fibers** (`rgo/guest-fibers`, paused). About 1.8x
  faster headless and lighter cutscenes, which should help the shots that
  fall back to 30. Blocked by an intermittent real-time stall in two opening
  cutscene points (the main game thread waits on a message that never
  arrives). Not merged; see the status note on that branch's `framerate.md`.

## Tier 2: setup, quality and release
- **First public release.** Planned, not started; see `RELEASE_PLAN.md`.
- **No WSL / Ubuntu.** Required before the first public release. Windows-native
  CPU/RSP generation and the player installer are implemented. A pinned,
  strictly validated names-and-addresses bundle replaces the matching ELF
  build. Generation matches the baseline apart from line endings, independent
  audio case ordering and the truthful normalizer hash. Fresh compilation,
  first-run setup and runtime validation are still in progress.
- **Real setup progress.** Done for the command line: the build prints
  "Step N of M" (see `build.md`). The launcher's setup screen should show it
  too (release plan item 2).
- **Use all CPU cores** by default (`--jobs`). Done.
- **Fast rebuilds.** Done (#16): generation and the build folder are reused
  when the generator inputs are unchanged (`build.md`).

## Tier 3: graphics
- Anti-aliasing: done through MSAA in display settings (#5).
- HDR output: not started.
- Ray tracing: not started (investigate; RT64's name comes from its
  ray-tracing origins).
- Texture packs: not started (RT64 supports texture replacement).

## Wishlist
- Full-campaign 4-player co-op
- Online multiplayer and co-op
- New Game+
- Horde mode

## How changes are made
The recompiled game code is generated from the ROM on the owner's PC (and
cached there between builds) and is never committed. Changes go into this
repo's runtime/renderer code, or replace or hook specific game functions with
hand-written C. Understanding those functions comes from the decomp
(Ryan-Myers/Jet-Force-Gemini). Replacements only need to behave correctly;
they don't have to match byte for byte.
