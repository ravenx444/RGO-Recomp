# RGO roadmap

Upstream gives a faithful port. RGO adds improvements. Keep each item a
separate PR that the owner can test on their PC, and note what to test.
Order within a tier is the owner's priority.

## Tier 1: core improvements
1. **Proper widescreen.** Wider view without stretching, and HUD elements
   anchored to the screen edges.
2. **Unlocked frame rate.** Render faster than the game's logic rate (frame
   interpolation, as other N64Recomp ports do) without speeding up gameplay.
3. **Gyro aiming** for controllers with motion sensors.
4. **Dual input.** Mouse and controller at the same time (e.g. Steam Input
   setups), with no mode switching.
5. **Full camera control.** Free camera on the right stick or mouse.
6. **Gears of War-style aiming.** The crosshair should point where shots land:
   cast from the camera through the crosshair, then aim the character at the
   hit point. Today it's projected forward on a spring arm.
7. **Persistent gore and splatter.** Bodies, splatter and especially collectible
   alien heads shouldn't vanish.

## Tier 2: setup and quality
- **No WSL / Ubuntu.** The WSL stage only rebuilds the decomp to get a map of
  the game (function boundaries, symbols, overlay layout). Generate that once
  and ship it as data (addresses and names only), so users need just Python and
  a C++ compiler. The cloud (Linux) can do the investigation.
- **Real setup progress.** The build already reports named steps
  (`setup_step=...`), so show "step N of M".
- **Use all CPU cores** for the final compile by default (`--jobs`).

## Tier 3: graphics
- Anti-aliasing (check what RT64 already exposes)
- HDR output
- Ray tracing (investigate; RT64's name comes from its ray-tracing origins)
- Texture packs (RT64 supports texture replacement)

## Wishlist
- Full-campaign 4-player co-op
- Online multiplayer and co-op
- New Game+
- Horde mode

## How changes are made
The recompiled game code is generated from the ROM on every build and is never
committed. Changes go into this repo's runtime/renderer code, or replace or hook
specific game functions with hand-written C. Understanding those functions
comes from the decomp (ravenx444/Rocket-Group-Orion). Replacements only need to
behave correctly; they don't have to match byte for byte.
