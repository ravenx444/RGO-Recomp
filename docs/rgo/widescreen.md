# Widescreen (roadmap Tier 1, item 1)

Goal: at 16:9 (and ultrawide) the 3D view is wider rather than stretched,
nothing pops in or out at the screen edges, and the HUD sits at the screen
edges instead of in a 4:3 box. A launcher setting chooses **Original 4:3** or
**Widescreen**; Widescreen is the default.

This page records what the investigation found, the plan, and what each PR
delivers. The cloud cannot run the game, so every behaviour below that
depends on gameplay still needs a test on the owner's PC.

## How things work today

### Renderer shell (`src/renderer`)

- `Rt64Shell::create` builds an `RT64::Application` with
  `useConfigurationFile = false` and only touches `developerMode`, the
  presentation mode and `copyWithGPU`. RT64's `userConfig.aspectRatio`
  therefore stays at its default, **Original**: the 320x240 picture is
  pillarboxed in a wider window.
- JFG uses Rare's F3DDKR microcode, which RT64 does not know. The shell
  installs its own GBI table (`rt64_f3ddkr.cpp`). F3DDKR loads a matrix that
  the CPU has already multiplied by the camera's projection, so the shell puts
  that **combined MVP** in RT64's model slot and an **identity projection** in
  the projection slot (`select_matrix`).
- The shell also does F3DDKR's trivial rejection itself: a triangle whose three
  vertices are outside the same clip plane is dropped, and `G_CULLDL` (0xBE)
  skips a whole display list when its bounding vertices are. Both tests used
  the 4:3 frustum (`|x| <= w`).
- An owned, hash-checked patch of RT64's VI renderer
  (`cmake/rt64_vi_presentation.cmake`) presents the active VI scanout shape, so
  the game's own widescreen mode (below) is shown as 16:9.
  `clear_vi_guard_texel` hides RT64's undefined right/bottom guard texel and
  assumed the 4:3/16:9 scanout shape.

### What RT64 offers (pinned 5473732)

- `UserConfiguration::aspectRatio`: Original, Expand (window shape) or Manual
  (`aspectTarget`). RT64 renders `target / source` times wider, where source is
  the VI framebuffer shape (320x240 = 4:3).
- `ProjectionProcessor` widens a projection by scaling its X column by
  `1 / scale` when its draws cover the whole framebuffer width and no extended
  viewport origin is set (`G_EX_ASPECT_AUTO`). This is decided by screen
  coverage, not by matrix type, so it also applies to JFG's identity
  "projection": the whole scene keeps its proportions and RT64 reveals more to
  the sides.
- 2D rectangles keep their 4:3 shape and stay centred, except rectangles that
  cover the full scissor width (backgrounds, fades), which stretch to the full
  width.
- Extended GBI (`include/rt64_extended_gbi.h`): `G_EX_SETRECTALIGN_V1`,
  `G_EX_SETVIEWPORTALIGN_V1`, `G_EX_SETSCISSORALIGN_V1` and friends anchor
  later rectangles, viewports and scissors to `G_EX_ORIGIN_LEFT`, `CENTER` or
  `RIGHT`. `extAspectRatio`/`extAspectTarget` decide how far LEFT/RIGHT reach
  (Zelda clamps the HUD to 16:9 on ultrawide). RT64 only routes extended
  commands after `State::enableExtendedGBI(opcode)`, and disables them again at
  every present. Because the shell owns the GBI table, it can enable them
  itself; the game's display lists only need the commands.

### Other N64Recomp ports

- **Zelda 64: Recompiled**, Starfox and Goemon map "Aspect ratio:
  Original/Expand" to `userConfig.aspectRatio` and "HUD placement:
  Original/16:9/Expand" to `extAspectRatio` (Zelda's defaults: Expand, HUD
  clamped to 16:9). RT64 widens the 3D view; the game patches only add
  extended-GBI alignment to HUD code (`gEXSetRectAlign`,
  `gEXSetViewportAlign`, reset to `G_EX_ORIGIN_NONE` afterwards) and redraw
  full-screen effects (letterbox bars, transitions) wide.
- **Culling:** Zelda and Banjo do not widen their frustum tests by the aspect
  factor; they turn the horizontal test off and keep the depth test. Banjo keeps
  the original culling during demo playback so recorded inputs stay
  deterministic.
- **Banjo** (also Rare) draws HUD counters as 3D models and anchors them with
  viewport alignment. JFG's HUD is built the same way (below).

### The game (decomp, ravenx444/Rocket-Group-Orion)

- **Projection:** `camSetFOV` (0x8003F7F4) calls a modified `guPerspectiveF`
  with a constant 4:3 aspect (`CAMERA_ASPECT`, camera.h), near 10, far 15000.
  The game's own **Widescreen option** keeps the 320x240 framebuffer and
  viewport, multiplies `proj[0][0]` by 0.75 (16:9, more to the sides) and has
  the VI squeeze 240 lines into 180 (`viSetTiming`, `resolutionSettings[1]`).
  The HUD is drawn into the same 320x240 buffer, so it is squeezed too.
  `viGetVideoMode() & 1` is the wide bit.
- **Culling:** the track renderer builds three planes per camera
  (`func_800158BC`, track.c) from fixed camera-space triangles at
  0x800A0CF4 (a near plane and two side planes), transformed by the inverse
  view matrix, and tests level blocks against them (`func_80015B3C`). The
  side planes are fixed: they do not follow FOV or aspect. They leave some
  margin outside the 4:3 view but not enough for wide FOVs or ultrawide.
  Objects are not culled on screen position on the CPU; display lists use
  F3DDKR's `G_CULLDL`, which the shell implements. Lens flares do a CPU
  Z-buffer read at the sun's projected 320x240 position
  (`camlightVisibilityCheck`).
- **HUD:** `frontUpdate` (0x80058A6C) collects 2D work into the global
  `frontgfx` (0x800FF398) display list. Single-player instruments are overlay
  14's `frontSingleInstruments`; the health display is overlay 6's
  `instDrawHealth`. They draw:
  - textured 3D quads through `frontDrawObj` (0x8005A0D0) under an ortho
    camera (`camStandardOrtho`), so most HUD art is triangles, not rectangles;
  - text through `fontPrintXY` (0x8006FCA8), drawn as texture rectangles;
  - bars through `frontDrawRectangles` (0x800595F4), drawn as fill
    rectangles;
  - scope lines and small digits **on the CPU, straight into the framebuffer**
    (`fxDrawLine`, `fxTinyPrint`, `fxDrawDigitalNumber`).

  Overlay functions dispatch at `overlay << 20 | offset` (for example
  overlay 14 + 0xB30 is 0x00E00B30), the same convention the existing hooks
  use.

## How game-side changes are made here

The recompiled game is regenerated from the ROM on every build, so changes are
hooks in `dispatch` (`src/boot/native_boot.cpp`): qualify the target by its
address and prologue words, adjust guest memory or registers, call the
generated original, and restore. Pure logic lives in headers under
`include/jfg/` with ROM-free unit tests, because `native_boot.cpp` only builds
in the private Windows game build.

## Plan

1. **3D widening, setting and culling (PR 1, `rgo/widescreen`)**
   - Setting **Video settings > Aspect ratio: Original 4:3 / Widescreen**,
     stored as `video.ini` in the launcher profile (`version=1`,
     `aspect=original|widescreen`). A missing file means Widescreen. The
     launcher passes the path as `JFG_VIDEO_CONFIG`; the game re-reads it
     every 250 ms, so the setting applies while playing.
   - The shell sets RT64 to **Manual** with
     `target = window_aspect * source / display` (capped at 32:9, never below
     the source). With the normal 320x240 mode this equals RT64's Expand. With
     the game's own Widescreen option the scanout is already 16:9, so a 16:9
     window gets no extra widening instead of being widened twice, and
     ultrawide only widens the remainder.
   - The shell's triangle and `G_CULLDL` rejection accept `|x| <= w * extent`
     (`extent = target / source`), so geometry that RT64 now shows beside the
     4:3 frustum is not dropped. The guard-texel clip uses the widened shape.
   - Hook `func_800158BC`: scale the X of the frustum triangles by the extent
     while the game builds its planes, then restore the table. The near plane
     is unchanged; the side planes open by exactly the view's widening.
   - A standalone game window (no launcher) opens out to at least 16:9 and no
     longer snaps back to 4:3.
   - Original 4:3 changes nothing: RT64 stays on Original, the extent is 1 and
     the hook does not run.
2. **HUD anchoring (PR 2, `rgo/widescreen-hud`)**
   - While the picture is widened, the shell enables RT64's extended GBI
     (opcode 0x64, unused by F3DDKR) for each task and sets
     `extAspectRatio` to Manual at 16:9, so anchored HUD elements stop at a
     16:9 area on ultrawide screens.
   - A hook on `frontUpdate` starts each HUD frame. Hooks on the overlay
     functions that draw the single-player HUD (overlay 14's
     `frontSingleInstruments` and the functions it calls, overlay 6's
     `instDrawHealth`, overlay 13's crosshair) mark element **groups**.
   - Hooks on the three primitives record where each group draws and move it:
     - `fontPrintXY` and `frontDrawRectangles`: the hook writes
       `G_EX_SETRECTALIGN_V1` (LEFT or RIGHT) into the primitive's own display
       list before the call and a reset after it.
     - `frontDrawObj`: the hook offsets the object's orthographic X by
       `width / 2 * (extent - 1)` for the call and restores it afterwards.
   - A group is anchored left when last frame's bounds stay left of the middle
     and centre in the left third, right likewise, and centred otherwise. All
     parts of an element (icon, number, bar) therefore move together, and
     anything that spans the middle (crosshair, messages) stays put. Elements
     are only moved under a full-width scissor; RT64 keeps a narrower one in
     the centred 4:3 area, where a moved element would be cut off.
   - The logic is in `include/jfg/mod/widescreen_hud.hpp` with ROM-free tests.
3. **Follow-ups (later, after PC testing)**
   - Full-screen effects that are not full-width rectangles (CPU-drawn scope
     lines and digits, CPU rain/refraction, transitions).
   - Split-screen multiplayer: RT64 only widens projections that cover the
     whole width, so 2-4 player views will stay 4:3 per view.
   - Lens flare visibility at the wide edges (CPU Z-buffer read at 4:3
     coordinates).
   - Optional "HUD placement" setting (Original / 16:9 / Full) if wanted.

## Risks and open questions

- **Untested in game.** Everything above is reasoned from the code; the owner
  must check it on the PC.
- **HUD coverage.** Only HUD parts drawn through the three hooked primitives
  move. Parts an overlay draws with its own matrices or texture rectangles,
  and the CPU-drawn digits and lines, stay in the centred 4:3 area; if one of
  them belongs to an anchored element, that element will look split. The HUD
  hooks add 32 bytes per anchored primitive to the HUD display list.
- **CPU framebuffer drawing.** JFG writes some HUD pixels and effects directly
  into the 320x240 framebuffer in RAM. How RT64 composites those changes into
  a widened framebuffer has not been verified; they may appear stretched or
  offset.
- **Gameplay determinism.** The culling hook changes which level blocks the
  game considers visible. That is presentation in practice, but it changes
  guest memory, so input replays recorded with Original may diverge with
  Widescreen. Direct runs without a launcher profile keep Original for replays
  and captures.
- **Cutscenes and level edges.** A wider view can reveal geometry or actors
  that were staged just off screen.
- **The game's own Widescreen option** still works. With RGO Widescreen it
  gives a vertically squeezed HUD and lower vertical detail, so the launcher
  advises leaving it off.
