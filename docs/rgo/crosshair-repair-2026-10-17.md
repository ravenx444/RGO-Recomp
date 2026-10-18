# CPU crosshairs on interpolated frames

The Tri-Rocket and Remote Mine sights use JFG's CPU line queue. The lines and
small sight digits are drawn into the completed VI framebuffer after rendering.
The original game continues to generate them correctly in the affected build.

RT64 applies CPU framebuffer changes to its normal render target. Its present
queue then switches to separate interpolated color targets for extra frames.
With MSAA, every presentation in a multi-frame group uses these separate targets.
The CPU sight layer was absent there, explaining intermittent sights without
MSAA and missing sights with MSAA and interpolation enabled.

`cmake/rt64_cpu_overlays.cmake` creates a patched present-queue translation unit
from the pinned, hash-checked upstream source. After waiting for an interpolated
target, it copies the existing CPU change mask onto that target before scanout.
It admits only color changes matching the current VI address, width and pixel
size, under the existing change-pool lock. The original target retains its
normal path. Changes are not blended repeatedly onto the original target.
Tracking the actual target selection also covers workload skipping that reduces
a group to one presentation after the interpolation wait.

The dependency checkout, game weapon selection, line artwork, targeting logic,
weapon damage and guest timing are unchanged. This also preserves CPU sight
digits and other CPU framebuffer effects carried by the same change mask.

## Local evidence

Hidden Windows tests used copied profiles and virtual input, without desktop
input, focus changes or cursor capture. Private ROM-derived captures and test
fixtures stay outside the repository.

- Before the repair, both weapon sights were absent on a proven middle frame
  (index 1 of 3), with Display interpolation and 4x MSAA. CPU traces still showed
  queued lines and changed framebuffer pixels.
- After the repair, the same middle-frame captures show the native Tri-Rocket
  sight and its digits, and the native Remote Mine sight. A bounded central
  image comparison counted 0 to 345 green sight pixels for Tri-Rockets and
  0 to 108 for Remote Mines.
- Additional captures confirm interpolation without MSAA, Original frame rate,
  4:3, and the unchanged pistol sight. Both 4:3 and pistol captures also
  prove an interior interpolated frame (index 1 of 3).
- Windows renderer and game builds, renderer memory tests, frame interpolation
  tests and widescreen HUD tests passed.

These are bounded rendering checks. Sustained combat, rapid weapon switching,
level transitions and all weapon/character combinations still need owner play.
This is not full-game or public-release acceptance.
